// ============================================================================
// ZeptoDB: HTTP MessagePack columnar ingest tests
// ============================================================================
// Exercises POST /insert/msgpack with the dependency-free MessagePack subset
// used by the server: top-level map, string keys, column arrays, and scalar
// string/integer/float values.
// ============================================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_port_helper.h"
#include "zeptodb/auth/tenant_manager.h"
#include "zeptodb/core/pipeline.h"
#include "zeptodb/server/http_server.h"
#include "zeptodb/sql/executor.h"

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

HttpResp http_post(uint16_t port,
                   const std::string& body,
                   const std::string& path,
                   const std::string& content_type = "application/msgpack",
                   const std::string& extra_headers = "")
{
    HttpResp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        ::close(fd);
        return r;
    }

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Type: " + content_type + "\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n";
    req += extra_headers;
    req += "\r\n";
    req += body;
    ::send(fd, req.data(), req.size(), 0);

    std::string raw;
    char buf[8192];
    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, static_cast<size_t>(n));
    }
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
        auto end = headers.find("\r\n", start);
        std::string val = headers.substr(start, end - start);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) {
            val.pop_back();
        }
        return val;
    };

    r.content_type = find_header("Content-Type");
    r.x_zepto_format = find_header("X-Zepto-Format");
    return r;
}

void append_u16(std::string* out, uint16_t v) {
    out->push_back(static_cast<char>((v >> 8) & 0xff));
    out->push_back(static_cast<char>(v & 0xff));
}

void append_u32(std::string* out, uint32_t v) {
    out->push_back(static_cast<char>((v >> 24) & 0xff));
    out->push_back(static_cast<char>((v >> 16) & 0xff));
    out->push_back(static_cast<char>((v >> 8) & 0xff));
    out->push_back(static_cast<char>(v & 0xff));
}

void append_u64(std::string* out, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(static_cast<char>((v >> shift) & 0xff));
    }
}

std::string mp_str(const std::string& s) {
    std::string out;
    if (s.size() <= 31) {
        out.push_back(static_cast<char>(0xa0 | s.size()));
    } else if (s.size() <= std::numeric_limits<uint8_t>::max()) {
        out.push_back(static_cast<char>(0xd9));
        out.push_back(static_cast<char>(s.size()));
    } else {
        out.push_back(static_cast<char>(0xda));
        append_u16(&out, static_cast<uint16_t>(s.size()));
    }
    out += s;
    return out;
}

std::string mp_i64(int64_t v) {
    std::string out;
    if (v >= 0 && v <= 0x7f) {
        out.push_back(static_cast<char>(v));
    } else if (v >= -32 && v < 0) {
        out.push_back(static_cast<char>(static_cast<int8_t>(v)));
    } else {
        out.push_back(static_cast<char>(0xd3));
        append_u64(&out, static_cast<uint64_t>(v));
    }
    return out;
}

std::string mp_f64(double v) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    std::string out(1, static_cast<char>(0xcb));
    append_u64(&out, bits);
    return out;
}

std::string mp_array(const std::vector<std::string>& values) {
    std::string out;
    if (values.size() <= 15) {
        out.push_back(static_cast<char>(0x90 | values.size()));
    } else {
        out.push_back(static_cast<char>(0xdc));
        append_u16(&out, static_cast<uint16_t>(values.size()));
    }
    for (const auto& value : values) out += value;
    return out;
}

std::string mp_map(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    if (fields.size() <= 15) {
        out.push_back(static_cast<char>(0x80 | fields.size()));
    } else {
        out.push_back(static_cast<char>(0xde));
        append_u16(&out, static_cast<uint16_t>(fields.size()));
    }
    for (const auto& [key, value] : fields) {
        out += mp_str(key);
        out += value;
    }
    return out;
}

std::unique_ptr<ZeptoPipeline> make_pipeline() {
    PipelineConfig cfg;
    cfg.storage_mode = StorageMode::PURE_IN_MEMORY;
    return std::make_unique<ZeptoPipeline>(cfg);
}

class HttpMsgpackIngestTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = zepto_test_util::pick_free_port();
        pipeline_ = make_pipeline();
        executor_ = std::make_unique<QueryExecutor>(*pipeline_);
        auto created = executor_->execute(
            "CREATE TABLE trades "
            "(symbol SYMBOL, price INT64, volume INT64, timestamp INT64)",
            nullptr);
        ASSERT_TRUE(created.ok()) << created.error;

        server_ = std::make_unique<zeptodb::server::HttpServer>(*executor_, port_);
        tenant_ = std::make_shared<zeptodb::auth::TenantManager>();
        server_->set_tenant_manager(tenant_);
        server_->start_async();
        std::this_thread::sleep_for(80ms);
    }

    void TearDown() override { server_->stop(); }

    uint16_t port_{};
    std::unique_ptr<ZeptoPipeline> pipeline_;
    std::unique_ptr<QueryExecutor> executor_;
    std::unique_ptr<zeptodb::server::HttpServer> server_;
    std::shared_ptr<zeptodb::auth::TenantManager> tenant_;
};

} // namespace

TEST_F(HttpMsgpackIngestTest, StringSymbolsTimestampAndScales_IngestsRows) {
    const std::string body = mp_map({
        {"symbol", mp_array({mp_str("PACK"), mp_str("PACK")})},
        {"price", mp_array({mp_f64(101.25), mp_f64(202.50)})},
        {"volume", mp_array({mp_i64(10), mp_i64(20)})},
        {"timestamp", mp_array({mp_i64(1'711'000'000'000'001'000LL),
                                 mp_i64(1'711'000'000'000'001'001LL)})},
    });

    auto r = http_post(
        port_, body,
        "/insert/msgpack?table=trades&sym_col=symbol&price_scale=100");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.x_zepto_format, "msgpack-columnar");
    EXPECT_NE(r.content_type.find("application/json"), std::string::npos);
    EXPECT_NE(r.body.find("\"inserted\":2"), std::string::npos);

    auto count = executor_->execute(
        "SELECT count(*) AS n FROM trades WHERE symbol = 'PACK'");
    ASSERT_TRUE(count.ok()) << count.error;
    ASSERT_FALSE(count.rows.empty());
    EXPECT_EQ(count.rows[0][0], 2);

    auto sum = executor_->execute(
        "SELECT sum(price) AS s FROM trades WHERE symbol = 'PACK'");
    ASSERT_TRUE(sum.ok()) << sum.error;
    ASSERT_FALSE(sum.rows.empty());
    EXPECT_EQ(sum.rows[0][0], 30375);
}

TEST_F(HttpMsgpackIngestTest, DefaultSymColumnAndMissingTimestamp_IngestsRows) {
    const std::string body = mp_map({
        {"sym", mp_array({mp_i64(42)})},
        {"price", mp_array({mp_i64(9001)})},
        {"volume", mp_array({mp_i64(7)})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=trades");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"inserted\":1"), std::string::npos);

    auto count = executor_->execute("SELECT count(*) AS n FROM trades");
    ASSERT_TRUE(count.ok()) << count.error;
    ASSERT_FALSE(count.rows.empty());
    EXPECT_EQ(count.rows[0][0], 1);
}

TEST_F(HttpMsgpackIngestTest, EmptyArrays_InsertZero) {
    const std::string body = mp_map({
        {"sym", mp_array({})},
        {"price", mp_array({})},
        {"volume", mp_array({})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=trades");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"inserted\":0"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, MissingRequiredColumn_Returns400) {
    const std::string body = mp_map({
        {"sym", mp_array({mp_i64(1)})},
        {"price", mp_array({mp_i64(100)})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=trades");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("volume"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, ColumnLengthMismatch_Returns400) {
    const std::string body = mp_map({
        {"sym", mp_array({mp_i64(1), mp_i64(2)})},
        {"price", mp_array({mp_i64(100)})},
        {"volume", mp_array({mp_i64(10), mp_i64(20)})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=trades");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("length"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, MalformedPayload_Returns400) {
    std::string body;
    body.push_back(static_cast<char>(0x81)); // map with one key/value
    body += mp_str("sym");                   // missing value

    auto r = http_post(port_, body, "/insert/msgpack?table=trades");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("MessagePack"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, DeclaredArrayLengthTooLarge_Returns400) {
    std::string body;
    body.push_back(static_cast<char>(0x81)); // map with one key/value
    body += mp_str("sym");
    body.push_back(static_cast<char>(0xdd)); // array32
    append_u32(&body, std::numeric_limits<uint32_t>::max());

    auto r = http_post(port_, body, "/insert/msgpack?table=trades");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("array length"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, UnknownTable_Returns400BeforeClaimingRows) {
    const std::string body = mp_map({
        {"sym", mp_array({mp_i64(1)})},
        {"price", mp_array({mp_i64(100)})},
        {"volume", mp_array({mp_i64(10)})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=missing");
    EXPECT_EQ(r.status, 400);
    EXPECT_NE(r.body.find("does not exist"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, TableAclDenied_Returns403) {
    const std::string body = mp_map({
        {"sym", mp_array({mp_i64(1)})},
        {"price", mp_array({mp_i64(100)})},
        {"volume", mp_array({mp_i64(10)})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=trades",
                       "application/msgpack",
                       "X-Zepto-Allowed-Tables: quotes\r\n");
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(r.body.find("not in allowed list"), std::string::npos);
}

TEST_F(HttpMsgpackIngestTest, TenantNamespaceDenied_Returns403) {
    zeptodb::auth::TenantConfig cfg;
    cfg.tenant_id = "tenant_a";
    cfg.table_namespace = "tenant_a_";
    ASSERT_TRUE(tenant_->create_tenant(cfg));

    const std::string body = mp_map({
        {"sym", mp_array({mp_i64(1)})},
        {"price", mp_array({mp_i64(100)})},
        {"volume", mp_array({mp_i64(10)})},
    });

    auto r = http_post(port_, body, "/insert/msgpack?table=trades",
                       "application/msgpack",
                       "X-Zepto-Tenant-Id: tenant_a\r\n");
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(r.body.find("cannot access table"), std::string::npos);
}
