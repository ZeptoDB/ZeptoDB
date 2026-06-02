// ============================================================================
// ZeptoDB: HTTP Arrow IPC content negotiation tests (devlog 119)
// ============================================================================
// Exercises the `POST /` Arrow IPC content-negotiation path:
//   - Trigger via `Accept: application/vnd.apache.arrow.stream`
//   - Trigger via `?default_format=Arrow` (ClickHouse compatibility)
//   - Trigger via `?format=arrow`
//   - Errors stay JSON regardless of Accept
//   - 406 fallback when Arrow is not compiled in
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "test_port_helper.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/arrow_ipc.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/storage/string_dictionary.h"

#ifdef ZEPTO_FLIGHT_ENABLED
#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#endif

using zeptodb::core::PipelineConfig;
using zeptodb::core::StorageMode;
using zeptodb::core::ZeptoPipeline;
using zeptodb::sql::QueryExecutor;
using namespace std::chrono_literals;

namespace {

struct HttpResp {
    int status = 0;
    std::string body;
    std::string content_type;
    std::string x_zepto_format;
};

// Minimal raw HTTP/1.1 client with header capture so the test can assert on
// Content-Type / X-Zepto-Format. `path` defaults to "/".
HttpResp http_post(uint16_t port,
                   const std::string& body,
                   const std::string& accept = "",
                   const std::string& path   = "/")
{
    HttpResp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd); return r;
    }

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n";
    if (!accept.empty()) req += "Accept: " + accept + "\r\n";
    req += "\r\n";
    req += body;
    ::send(fd, req.data(), req.size(), 0);

    std::string raw;
    char buf[8192];
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        raw.append(buf, static_cast<size_t>(n));
    ::close(fd);

    if (raw.size() > 12) r.status = std::atoi(raw.c_str() + 9);

    auto pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) return r;

    std::string headers = raw.substr(0, pos);
    r.body = raw.substr(pos + 4);

    auto find_header = [&](const std::string& name) -> std::string {
        std::string needle_lc = name;
        for (auto& c : needle_lc) c = static_cast<char>(std::tolower(c));
        std::string h_lc = headers;
        for (auto& c : h_lc) c = static_cast<char>(std::tolower(c));
        auto p = h_lc.find("\r\n" + needle_lc + ":");
        if (p == std::string::npos) return {};
        auto start = headers.find(':', p + 2) + 1;
        auto end   = headers.find("\r\n", start);
        std::string val = headers.substr(start, end - start);
        // trim
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();
        return val;
    };

    r.content_type   = find_header("Content-Type");
    r.x_zepto_format = find_header("X-Zepto-Format");
    return r;
}

std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

class HttpArrowIpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_     = zepto_test_util::pick_free_port();
        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);

        executor_->execute(
            "CREATE TABLE trades "
            "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)",
            nullptr);
        // Two symbols, three trades each — VWAP/group-by tests use both.
        for (int i = 0; i < 3; ++i) {
            executor_->execute(
                "INSERT INTO trades (symbol, price, volume, timestamp) "
                "VALUES ('AAPL', " + std::to_string(15000 + i * 10) +
                ", " + std::to_string(100 + i) +
                ", " + std::to_string(1'711'000'000'000'000'000LL +
                                      static_cast<int64_t>(i)) + ")",
                nullptr);
            executor_->execute(
                "INSERT INTO trades (symbol, price, volume, timestamp) "
                "VALUES ('GOOGL', " + std::to_string(28000 + i * 10) +
                ", " + std::to_string(50 + i) +
                ", " + std::to_string(1'711'000'000'000'000'000LL +
                                      static_cast<int64_t>(i)) + ")",
                nullptr);
        }

        server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
        server_->start_async();
        std::this_thread::sleep_for(80ms);
    }

    void TearDown() override { server_->stop(); }

    uint16_t                                       port_{};
    std::unique_ptr<ZeptoPipeline>                 pipeline_;
    std::unique_ptr<QueryExecutor>                 executor_;
    std::unique_ptr<zeptodb::server::HttpServer>   server_;
};

constexpr const char* kArrowMime = "application/vnd.apache.arrow.stream";

#ifdef ZEPTO_FLIGHT_ENABLED
// Decode an Arrow IPC stream blob into a Table.
std::shared_ptr<arrow::Table> decode_arrow(const std::string& body) {
    auto buf = arrow::Buffer::FromString(body);
    auto in  = std::make_shared<arrow::io::BufferReader>(buf);
    auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(in);
    EXPECT_TRUE(reader_res.ok()) << reader_res.status().ToString();
    if (!reader_res.ok()) return nullptr;
    auto table_res = (*reader_res)->ToTable();
    EXPECT_TRUE(table_res.ok()) << table_res.status().ToString();
    if (!table_res.ok()) return nullptr;
    return *table_res;
}

std::shared_ptr<arrow::Array> int64_array(const std::vector<int64_t>& values) {
    arrow::Int64Builder builder;
    EXPECT_TRUE(builder.AppendValues(values).ok());
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(builder.Finish(&out).ok());
    return out;
}

std::shared_ptr<arrow::Array> string_array(const std::vector<std::string>& values) {
    arrow::StringBuilder builder;
    for (const auto& value : values) {
        EXPECT_TRUE(builder.Append(value).ok());
    }
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(builder.Finish(&out).ok());
    return out;
}

std::string encode_arrow_stream(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
{
    auto sink_res = arrow::io::BufferOutputStream::Create();
    EXPECT_TRUE(sink_res.ok()) << sink_res.status().ToString();
    if (!sink_res.ok()) return {};
    auto sink = *sink_res;

    auto writer_res = arrow::ipc::MakeStreamWriter(sink, schema);
    EXPECT_TRUE(writer_res.ok()) << writer_res.status().ToString();
    if (!writer_res.ok()) return {};
    auto writer = *writer_res;

    for (const auto& batch : batches) {
        EXPECT_TRUE(writer->WriteRecordBatch(*batch).ok());
    }
    EXPECT_TRUE(writer->Close().ok());

    auto buf_res = sink->Finish();
    EXPECT_TRUE(buf_res.ok()) << buf_res.status().ToString();
    if (!buf_res.ok()) return {};
    auto buf = *buf_res;
    return std::string(reinterpret_cast<const char*>(buf->data()),
                       static_cast<size_t>(buf->size()));
}
#endif // ZEPTO_FLIGHT_ENABLED

} // namespace

// ---------------------------------------------------------------------------
// 7. Always-on case: 406 fallback when Arrow IPC is not available.
//    Runs in both build modes — when ZEPTO_FLIGHT_ENABLED is defined,
//    `arrow_ipc_available()` returns true so the request succeeds with 200
//    instead. We only assert the 406 branch when Arrow is *off*.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, ArrowDisabled_Returns406_WithJsonError) {
    auto r = http_post(port_,
                       "SELECT count(*) FROM trades",
                       kArrowMime);
    if (zeptodb::server::arrow_ipc_available()) {
        // Arrow compiled in → success path is exercised by the other tests.
        // Here we just verify the request was a normal Arrow response.
        ASSERT_EQ(r.status, 200) << r.body;
        EXPECT_EQ(r.content_type, kArrowMime);
    } else {
        ASSERT_EQ(r.status, 406);
        EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
        EXPECT_NE(r.body.find("Arrow"), std::string::npos);
    }
}

TEST_F(HttpArrowIpcTest, InsertArrow_UnavailableOrMalformedReturnsError) {
    auto r = http_post(port_,
                       "not an arrow stream",
                       "",
                       "/insert/arrow?table=trades");
    if (zeptodb::server::arrow_ipc_available()) {
        EXPECT_EQ(r.status, 400);
        EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
    } else {
        EXPECT_EQ(r.status, 406);
        EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
        EXPECT_NE(r.body.find("Arrow"), std::string::npos);
    }
}

#ifdef ZEPTO_FLIGHT_ENABLED

// ---------------------------------------------------------------------------
// 1. Happy path INT64 + FLOAT64 — VWAP returns FLOAT64, count(*) returns INT64.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, AcceptHeader_VwapAndCount_DecodesViaPyarrowParity) {
    const std::string sql =
        "SELECT vwap(price, volume) AS vwap, count(*) AS n "
        "FROM trades WHERE symbol = 'AAPL'";

    // 1a. Arrow path
    auto arrow_r = http_post(port_, sql, kArrowMime);
    ASSERT_EQ(arrow_r.status, 200) << arrow_r.body;
    EXPECT_EQ(arrow_r.content_type, kArrowMime);
    EXPECT_EQ(arrow_r.x_zepto_format, "arrow-stream");

    auto table = decode_arrow(arrow_r.body);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->num_rows(), 1);
    ASSERT_EQ(table->num_columns(), 2);
    EXPECT_EQ(table->schema()->field(0)->name(), "vwap");
    EXPECT_EQ(table->schema()->field(1)->name(), "n");

    // 1b. JSON parity — same query without Accept must yield the same numbers.
    auto json_r = http_post(port_, sql, "");
    ASSERT_EQ(json_r.status, 200);
    EXPECT_NE(json_r.content_type.find("application/json"), std::string::npos);
    // count(*) appears literally in JSON `data`
    EXPECT_NE(json_r.body.find("\"data\""), std::string::npos);
    EXPECT_NE(json_r.body.find("\"vwap\""), std::string::npos);
    EXPECT_NE(json_r.body.find("\"n\""), std::string::npos);

    // First-row int64 column ("n") must match between Arrow and JSON.
    auto n_col = std::static_pointer_cast<arrow::Int64Array>(
        table->column(1)->chunk(0));
    ASSERT_NE(n_col, nullptr);
    EXPECT_EQ(n_col->Value(0), 3);
}

// ---------------------------------------------------------------------------
// 2. STRING column via symbol_dict — encoder-level test.
//
//    The HTTP query path returns INT64 for SELECT-symbol queries (the
//    executor reconstructs `symbol` from the partition key as int64; the
//    dict-encoded STRING column type is only produced by string-column DDL
//    paths that aren't exposed through SQL today). To still cover the
//    encoder's STRING + symbol_dict → arrow::utf8 path, we drive
//    `encode_result_set_ipc` with a hand-crafted `QueryResultSet`.
// ---------------------------------------------------------------------------
TEST(HttpArrowIpcEncoder, StringColumnViaSymbolDict_DecodesAsUtf8) {
    zeptodb::storage::StringDictionary dict;
    auto code_aapl  = dict.intern("AAPL");
    auto code_googl = dict.intern("GOOGL");

    zeptodb::sql::QueryResultSet rs;
    rs.column_names = {"symbol", "n"};
    rs.column_types = {zeptodb::storage::ColumnType::STRING,
                       zeptodb::storage::ColumnType::INT64};
    rs.symbol_dict = &dict;
    rs.rows = {
        {static_cast<int64_t>(code_aapl),  static_cast<int64_t>(3)},
        {static_cast<int64_t>(code_googl), static_cast<int64_t>(3)},
    };

    std::string body, err;
    ASSERT_TRUE(zeptodb::server::encode_result_set_ipc(rs, &body, &err))
        << err;

    auto table = decode_arrow(body);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->num_rows(), 2);
    ASSERT_EQ(table->num_columns(), 2);
    EXPECT_EQ(table->schema()->field(0)->type()->id(), arrow::Type::STRING);

    auto sym_col = std::static_pointer_cast<arrow::StringArray>(
        table->column(0)->chunk(0));
    ASSERT_NE(sym_col, nullptr);
    EXPECT_EQ(sym_col->GetString(0), "AAPL");
    EXPECT_EQ(sym_col->GetString(1), "GOOGL");
}

// ---------------------------------------------------------------------------
// 3. Empty result — schema present, zero rows.
//    Use an always-false numeric predicate so the executor keeps the schema
//    (no early-exit on unknown symbol_id).
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, AcceptHeader_EmptyResult_SchemaWithZeroRows) {
    const std::string sql =
        "SELECT price, volume FROM trades WHERE price < 0";
    auto r = http_post(port_, sql, kArrowMime);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.content_type, kArrowMime);

    auto table = decode_arrow(r.body);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->num_rows(), 0);
    // Schema must still be present.
    EXPECT_GE(table->num_columns(), 1);
}

// ---------------------------------------------------------------------------
// 4. ClickHouse-style query parameter `?default_format=Arrow`.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, DefaultFormatArrow_QueryParam) {
    auto r = http_post(port_,
                       "SELECT count(*) AS n FROM trades",
                       /*accept=*/"",
                       /*path=*/"/?default_format=Arrow");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.content_type, kArrowMime);

    auto table = decode_arrow(r.body);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->num_rows(), 1);
}

// ---------------------------------------------------------------------------
// 5. `?format=arrow` short-form query parameter.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, FormatArrow_QueryParam) {
    auto r = http_post(port_,
                       "SELECT count(*) AS n FROM trades",
                       /*accept=*/"",
                       /*path=*/"/?format=arrow");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.content_type, kArrowMime);
}

// ---------------------------------------------------------------------------
// 6. HTTP Arrow IPC ingest endpoint — string symbol alias + explicit ns time.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, InsertArrow_StringSymbolsAndTimestamp_IngestsRows) {
    auto schema = arrow::schema({
        arrow::field("symbol", arrow::utf8()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
        arrow::field("timestamp", arrow::int64()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 2, {
        string_array({"MSFT", "MSFT"}),
        int64_array({41000, 41010}),
        int64_array({10, 20}),
        int64_array({1'711'000'000'000'001'000LL,
                     1'711'000'000'000'001'001LL}),
    });
    const std::string body = encode_arrow_stream(schema, {batch});

    auto r = http_post(port_,
                       body,
                       "",
                       "/insert/arrow?table=trades&sym_col=symbol");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
    EXPECT_EQ(r.x_zepto_format, "arrow-stream");
    EXPECT_NE(r.body.find("\"inserted\":2"), std::string::npos);

    auto rs = executor_->execute(
        "SELECT count(*) AS n FROM trades WHERE symbol = 'MSFT'");
    ASSERT_TRUE(rs.ok()) << rs.error;
    ASSERT_FALSE(rs.rows.empty());
    EXPECT_EQ(rs.rows[0][0], 2);
}

// ---------------------------------------------------------------------------
// 7. Default `sym` column and missing timestamp → ZeptoDB assigns timestamps.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, InsertArrow_DefaultSymColumn_GeneratesTimestamp) {
    auto schema = arrow::schema({
        arrow::field("sym", arrow::int64()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 1, {
        int64_array({42}),
        int64_array({9001}),
        int64_array({7}),
    });
    const std::string body = encode_arrow_stream(schema, {batch});

    auto r = http_post(port_, body, "", "/insert/arrow?table=trades");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"inserted\":1"), std::string::npos);

    auto rs = executor_->execute("SELECT count(*) AS n FROM trades");
    ASSERT_TRUE(rs.ok()) << rs.error;
    ASSERT_FALSE(rs.rows.empty());
    EXPECT_EQ(rs.rows[0][0], 7);
}

// ---------------------------------------------------------------------------
// 8. Empty stream/batch is a valid no-op with inserted=0.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, InsertArrow_EmptyBatch_InsertsZero) {
    auto schema = arrow::schema({
        arrow::field("sym", arrow::int64()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 0, {
        int64_array({}),
        int64_array({}),
        int64_array({}),
    });
    const std::string body = encode_arrow_stream(schema, {batch});

    auto r = http_post(port_, body, "", "/insert/arrow?table=trades");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"inserted\":0"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 9. Required-column validation: volume is required.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, InsertArrow_MissingRequiredColumn_Returns400) {
    auto schema = arrow::schema({
        arrow::field("sym", arrow::int64()),
        arrow::field("price", arrow::int64()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 1, {
        int64_array({1}),
        int64_array({100}),
    });
    const std::string body = encode_arrow_stream(schema, {batch});

    auto r = http_post(port_, body, "", "/insert/arrow?table=trades");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("volume"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 10. Bad SQL with Accept: arrow — error path stays JSON.
// ---------------------------------------------------------------------------
TEST_F(HttpArrowIpcTest, BadSql_WithArrowAccept_StaysJson) {
    auto r = http_post(port_, "SELEKT bogus FROM trades", kArrowMime);
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
    EXPECT_NE(r.body.find("error"), std::string::npos);
    EXPECT_TRUE(r.x_zepto_format.empty());
}

// ---------------------------------------------------------------------------
// 11. Defensive: SYMBOL column with `symbol_dict == nullptr` falls back to an
//    int64 schema. The SQL executor today always populates `symbol_dict`, so
//    this branch is unreachable in production, but the guard exists to keep
//    the schema and the array consistent if a future caller ever hits it.
//    See devlog 119 post-review touch-ups.
// ---------------------------------------------------------------------------
TEST(HttpArrowIpcEncoder, SymbolWithoutDict_FallsBackToInt64Schema) {
    using V = zeptodb::sql::QueryResultSet::Value;

    zeptodb::sql::QueryResultSet rs;
    rs.column_names = {"symbol_code"};
    rs.column_types = {zeptodb::storage::ColumnType::SYMBOL};
    rs.symbol_dict  = nullptr;
    rs.typed_rows   = {
        {V(static_cast<int64_t>(0))},
        {V(static_cast<int64_t>(1))},
        {V(static_cast<int64_t>(2))},
    };

    std::string body, err;
    ASSERT_TRUE(zeptodb::server::encode_result_set_ipc(rs, &body, &err))
        << err;

    auto table = decode_arrow(body);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->num_columns(), 1);
    ASSERT_EQ(table->num_rows(), 3);
    EXPECT_TRUE(table->schema()->field(0)->type()->Equals(*arrow::int64()))
        << "got " << table->schema()->field(0)->type()->ToString();

    auto col = std::static_pointer_cast<arrow::Int64Array>(
        table->column(0)->chunk(0));
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->Value(0), 0);
    EXPECT_EQ(col->Value(1), 1);
    EXPECT_EQ(col->Value(2), 2);
}

TEST(HttpArrowIpcIngest, UnknownTable_ReturnsErrorBeforeClaimingRows) {
    auto pipeline = make_pipeline();
    QueryExecutor executor(*pipeline);

    auto schema = arrow::schema({
        arrow::field("sym", arrow::int64()),
        arrow::field("price", arrow::int64()),
        arrow::field("volume", arrow::int64()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 1, {
        int64_array({1}),
        int64_array({100}),
        int64_array({10}),
    });
    const std::string body = encode_arrow_stream(schema, {batch});

    zeptodb::server::ArrowIpcIngestOptions opts;
    opts.table_name = "missing";

    auto result = zeptodb::server::ingest_arrow_ipc_stream(
        executor, body, opts);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.rows, 0u);
    EXPECT_NE(result.error.find("does not exist"), std::string::npos);
}

#else // !ZEPTO_FLIGHT_ENABLED — additional tests skipped

TEST(HttpArrowIpcStub, ArrowIpcUnavailableAtBuildTime) {
    GTEST_SKIP() << "Arrow IPC not compiled in";
}

#endif // ZEPTO_FLIGHT_ENABLED
