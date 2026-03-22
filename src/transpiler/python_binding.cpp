// ============================================================================
// APEX-DB Phase D: Python Binding (pybind11)
// ============================================================================
// Python ↔ C++ 브리지. Zero-copy numpy 뷰, 배치 인제스트, 쿼리 노출.
//
// 핵심 설계:
//   - get_column(): ArenaAllocator 위의 ColumnVector raw pointer를 numpy array가
//     직접 가리키게 함. 소유권은 Pipeline이 유지 (Python 쪽은 view).
//   - 배치 ingest: numpy array / list 양쪽 처리.
//   - QueryResult → Python 객체로 노출 (value, rows_scanned, latency_ns).
// ============================================================================

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include "apex/core/pipeline.h"
#include "apex/ingestion/tick_plant.h"
#include "apex/storage/column_store.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace py = pybind11;
using namespace apex::core;
using namespace apex::ingestion;
using namespace apex::storage;

// ============================================================================
// 내부 헬퍼
// ============================================================================
static inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// PyPipeline: ApexPipeline 래퍼
// ============================================================================
class PyPipeline {
public:
    PyPipeline() : pipeline_(std::make_unique<ApexPipeline>()) {}

    void start() { pipeline_->start(); }
    void stop()  { pipeline_->stop(); }

    // -------------------------------------------------------------------------
    // 단건 인제스트
    // -------------------------------------------------------------------------
    void ingest(uint32_t symbol, int64_t price, int64_t volume) {
        TickMessage msg{};
        msg.symbol_id = symbol;
        msg.price     = price;
        msg.volume    = volume;
        msg.recv_ts   = now_ns();
        msg.msg_type  = 0;  // Trade
        if (!pipeline_->ingest_tick(msg)) {
            throw std::runtime_error("ingest failed: queue full");
        }
    }

    // -------------------------------------------------------------------------
    // 배치 인제스트: numpy array 또는 Python list 허용
    // -------------------------------------------------------------------------
    void ingest_batch(py::object symbols_obj,
                      py::object prices_obj,
                      py::object volumes_obj)
    {
        // numpy array로 변환 (forcecast: int32 등 다른 정수 타입도 수용)
        // pybind11 3.x: c_contiguous 제거됨 → forcecast만 사용
        py::array_t<int64_t, py::array::forcecast> syms(symbols_obj);
        py::array_t<int64_t, py::array::forcecast> prs(prices_obj);
        py::array_t<int64_t, py::array::forcecast> vols(volumes_obj);

        auto s_buf = syms.request();
        auto p_buf = prs.request();
        auto v_buf = vols.request();

        size_t n = static_cast<size_t>(s_buf.size);
        if (static_cast<size_t>(p_buf.size) != n || static_cast<size_t>(v_buf.size) != n) {
            throw std::invalid_argument("symbols, prices, volumes must have the same length");
        }

        const int64_t* sym_ptr = static_cast<int64_t*>(s_buf.ptr);
        const int64_t* prc_ptr = static_cast<int64_t*>(p_buf.ptr);
        const int64_t* vol_ptr = static_cast<int64_t*>(v_buf.ptr);

        int64_t ts = now_ns();
        for (size_t i = 0; i < n; ++i) {
            TickMessage msg{};
            msg.symbol_id = static_cast<uint32_t>(sym_ptr[i]);
            msg.price     = prc_ptr[i];
            msg.volume    = vol_ptr[i];
            msg.recv_ts   = ts + static_cast<int64_t>(i);  // 순서 보존
            msg.msg_type  = 0;
            if (!pipeline_->ingest_tick(msg)) {
                throw std::runtime_error(
                    "ingest_batch: queue full at index " + std::to_string(i));
            }
        }
    }

    // -------------------------------------------------------------------------
    // 배치 인제스트: float64 가격/볼륨 배열 (scale 적용 후 int64 변환)
    //
    // Polars/pandas DataFrame의 float 컬럼을 직접 받아 C++ 내부에서 변환.
    // Python 루프 없이 단일 C++ 루프로 처리 — row-by-row ingest 대비 ~100x.
    //
    // price_scale: 소수점 변환 계수 (예: 100.0 → 센트 단위로 저장)
    // vol_scale:   볼륨 변환 계수 (보통 1.0)
    // -------------------------------------------------------------------------
    void ingest_float_batch(py::object symbols_obj,
                            py::object prices_obj,
                            py::object volumes_obj,
                            double price_scale = 1.0,
                            double vol_scale   = 1.0)
    {
        py::array_t<int64_t, py::array::forcecast> syms(symbols_obj);
        py::array_t<double,  py::array::forcecast> prs(prices_obj);
        py::array_t<double,  py::array::forcecast> vols(volumes_obj);

        auto s_buf = syms.request();
        auto p_buf = prs.request();
        auto v_buf = vols.request();

        size_t n = static_cast<size_t>(s_buf.size);
        if (static_cast<size_t>(p_buf.size) != n ||
            static_cast<size_t>(v_buf.size) != n) {
            throw std::invalid_argument(
                "ingest_float_batch: symbols, prices, volumes must have the same length");
        }

        const int64_t* sym_ptr = static_cast<int64_t*>(s_buf.ptr);
        const double*  prc_ptr = static_cast<double*>(p_buf.ptr);
        const double*  vol_ptr = static_cast<double*>(v_buf.ptr);

        int64_t ts = now_ns();
        for (size_t i = 0; i < n; ++i) {
            TickMessage msg{};
            msg.symbol_id = static_cast<uint32_t>(sym_ptr[i]);
            msg.price     = static_cast<int64_t>(prc_ptr[i] * price_scale);
            msg.volume    = static_cast<int64_t>(vol_ptr[i] * vol_scale);
            msg.recv_ts   = ts + static_cast<int64_t>(i);
            msg.msg_type  = 0;
            if (!pipeline_->ingest_tick(msg)) {
                throw std::runtime_error(
                    "ingest_float_batch: queue full at index " + std::to_string(i));
            }
        }
    }

    // -------------------------------------------------------------------------
    // drain: ticks_ingested == ticks_stored 될 때까지 대기
    //
    // queue_depth == 0 체크만으론 부족하다:
    //   드레인 스레드가 큐에서 뽑은 직후 store_tick() 완료 전에
    //   queue_depth == 0이 되기 때문이다.
    // ticks_stored 카운터가 ticks_ingested에 따라잡을 때까지 폴링.
    // -------------------------------------------------------------------------
    size_t drain() {
        uint64_t target = pipeline_->stats().ticks_ingested.load(
            std::memory_order_acquire);

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(5);

        while (std::chrono::steady_clock::now() < deadline) {
            uint64_t stored = pipeline_->stats().ticks_stored.load(
                std::memory_order_acquire);
            if (stored >= target) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return static_cast<size_t>(
            pipeline_->stats().ticks_stored.load(std::memory_order_relaxed));
    }

    // -------------------------------------------------------------------------
    // 쿼리
    // -------------------------------------------------------------------------
    QueryResult vwap(uint32_t symbol,
                     int64_t from = 0,
                     int64_t to   = INT64_MAX)
    {
        return pipeline_->query_vwap(symbol, from, to);
    }

    QueryResult filter_sum(uint32_t symbol,
                           const std::string& column,
                           int64_t threshold,
                           int64_t from = 0,
                           int64_t to   = INT64_MAX)
    {
        return pipeline_->query_filter_sum(symbol, column, threshold, from, to);
    }

    QueryResult count(uint32_t symbol,
                      int64_t from = 0,
                      int64_t to   = INT64_MAX)
    {
        return pipeline_->query_count(symbol, from, to);
    }

    // -------------------------------------------------------------------------
    // Zero-copy 컬럼 접근: numpy array가 RDB 메모리를 직접 가리킴
    // -------------------------------------------------------------------------
    py::array_t<int64_t> get_column(uint32_t symbol, const std::string& name)
    {
        // 파티션 매니저에서 해당 symbol의 파티션을 모두 찾아
        // 첫 번째 파티션의 컬럼 포인터를 numpy에 묶는다.
        // (멀티-파티션 케이스는 가장 최근 파티션 단건으로 단순화)
        auto& pm = pipeline_->partition_manager();

        // PartitionManager에는 공개 API가 없으므로 pipeline의 drain 후
        // symbol로 파티션을 찾아야 함. pipeline이 노출하는 partition_index_로
        // 직접 접근 대신, 파티션 매니저 전체를 순회해서 symbol 찾기.
        //
        // pipeline.h에 find_partitions()는 private → 우회: drain_sync() 후
        // 파티션 매니저를 우회해 컬럼 포인터를 얻는 helper를 사용.

        ColumnVector* col = find_column_in_partitions(pm, symbol, name);
        if (!col) {
            throw std::runtime_error(
                "get_column: column '" + name + "' not found for symbol " +
                std::to_string(symbol));
        }

        const int64_t* raw = static_cast<const int64_t*>(col->raw_data());
        size_t nrows = col->size();

        if (!raw || nrows == 0) {
            throw std::runtime_error(
                "get_column: no data for symbol " + std::to_string(symbol));
        }

        // Zero-copy: numpy array가 raw ptr을 직접 참조.
        // `pipeline_` 객체가 살아있는 한 유효. Python 쪽 lifetime은 호출자 책임.
        py::capsule base(
            raw,
            [](void*) { /* 소유권 없음 — ArenaAllocator가 관리 */ }
        );

        return py::array_t<int64_t>(
            { static_cast<py::ssize_t>(nrows) },  // shape
            { sizeof(int64_t) },                   // strides
            raw,                                   // data ptr
            base                                   // base object (소유권 캡슐)
        );
    }

    // -------------------------------------------------------------------------
    // 통계
    // -------------------------------------------------------------------------
    py::dict stats() {
        const auto& s = pipeline_->stats();
        py::dict d;
        d["ticks_ingested"]     = s.ticks_ingested.load();
        d["ticks_stored"]       = s.ticks_stored.load();
        d["ticks_dropped"]      = s.ticks_dropped.load();
        d["queries_executed"]   = s.queries_executed.load();
        d["total_rows_scanned"] = s.total_rows_scanned.load();
        d["partitions_created"] = s.partitions_created.load();
        d["last_ingest_latency_ns"] = s.last_ingest_latency_ns.load();
        return d;
    }

private:
    // PartitionManager의 내부 파티션을 symbol로 검색 (최신 파티션 우선)
    static ColumnVector* find_column_in_partitions(
        PartitionManager& pm,
        uint32_t symbol,
        const std::string& name)
    {
        // PartitionManager에 공개 반복자가 없으므로 파이프라인이 노출하는
        // partition_manager().get_or_create() 대신, get_sealed_partitions()와
        // 합쳐서 활성 파티션도 찾도록 한다.
        //
        // 가장 안전한 방법: drain() 후 sealed 파티션 확인 → 없으면 활성도 확인.
        // 여기서는 sealed + active 모두를 커버하는 search용 helper를 쓴다.

        // sealed 파티션 순회
        auto sealed = pm.get_sealed_partitions();
        ColumnVector* best = nullptr;

        for (auto* part : sealed) {
            if (part->key().symbol_id == symbol) {
                auto* col = part->get_column(name);
                if (col && col->size() > 0) {
                    if (!best || col->size() > best->size()) {
                        best = col;
                    }
                }
            }
        }

        // active 파티션에서도 찾기: get_or_create는 없는 경우 새로 만드니
        // 직접 탐색. PartitionManager에 get_all() 같은 메서드가 없으므로
        // probe용 타임스탬프로 탐색하는 대신, 현재 시각 기준 파티션 접근.
        if (!best) {
            // 현재 시각 기준 파티션 탐색 (활성 파티션)
            int64_t ts = now_ns();
            try {
                auto& part = pm.get_or_create(symbol, ts);
                auto* col = part.get_column(name);
                if (col && col->size() > 0) {
                    best = col;
                }
            } catch (...) {}
        }

        return best;
    }

    std::unique_ptr<ApexPipeline> pipeline_;
};

// ============================================================================
// PyStats 래퍼 (dict 대신 속성 접근도 지원)
// ============================================================================
struct PyStats {
    uint64_t ticks_ingested;
    uint64_t ticks_stored;
    uint64_t ticks_dropped;
    uint64_t queries_executed;
    uint64_t total_rows_scanned;
    uint64_t partitions_created;
    int64_t  last_ingest_latency_ns;
};

// ============================================================================
// pybind11 모듈 정의
// ============================================================================
PYBIND11_MODULE(apex, m) {
    m.doc() = "APEX-DB Python Binding — Ultra-Low Latency HFT In-Memory Database";

    // -------------------------------------------------------------------------
    // QueryResult
    // -------------------------------------------------------------------------
    py::class_<QueryResult>(m, "QueryResult")
        .def_readonly("value",        &QueryResult::value,
                      "쿼리 결과값 (VWAP/SUM은 float, COUNT는 int 캐스팅 필요)")
        .def_readonly("ivalue",       &QueryResult::ivalue,
                      "정수 결과값 (SUM, COUNT)")
        .def_readonly("rows_scanned", &QueryResult::rows_scanned,
                      "스캔한 행 수")
        .def_readonly("latency_ns",   &QueryResult::latency_ns,
                      "쿼리 실행 시간 (nanoseconds)")
        .def("ok",    &QueryResult::ok,
             "쿼리 성공 여부")
        .def("__repr__", [](const QueryResult& r) {
            return "QueryResult(value=" + std::to_string(r.value) +
                   ", rows=" + std::to_string(r.rows_scanned) +
                   ", latency=" + std::to_string(r.latency_ns) + "ns, ok=" +
                   (r.ok() ? "True" : "False") + ")";
        });

    // -------------------------------------------------------------------------
    // Pipeline
    // -------------------------------------------------------------------------
    py::class_<PyPipeline>(m, "Pipeline")
        .def(py::init<>(), "APEX-DB 파이프라인 생성")

        .def("start", &PyPipeline::start,
             "파이프라인 시작 (drain 스레드 기동)")
        .def("stop",  &PyPipeline::stop,
             "파이프라인 중지 (드레인 후 스레드 종료)")

        // 인제스트
        .def("ingest", &PyPipeline::ingest,
             py::arg("symbol"), py::arg("price"), py::arg("volume"),
             "단건 틱 인제스트")
        .def("ingest_batch", &PyPipeline::ingest_batch,
             py::arg("symbols"), py::arg("prices"), py::arg("volumes"),
             "배치 틱 인제스트 (numpy int64 array 또는 list)")
        .def("ingest_float_batch", &PyPipeline::ingest_float_batch,
             py::arg("symbols"),
             py::arg("prices"),
             py::arg("volumes"),
             py::arg("price_scale") = 1.0,
             py::arg("vol_scale")   = 1.0,
             "배치 인제스트 (float64 가격/볼륨).\n"
             "price_scale: float→int64 변환 계수 (예: 100.0 = 센트 단위)\n"
             "Polars/pandas float 컬럼을 직접 전달 가능 — Python 루프 없음")
        .def("drain", &PyPipeline::drain,
             "큐의 틱을 동기적으로 스토리지에 반영. 반환값: 저장된 틱 수")

        // 쿼리
        .def("vwap", &PyPipeline::vwap,
             py::arg("symbol"),
             py::arg("from_ts") = static_cast<int64_t>(0),
             py::arg("to_ts")   = INT64_MAX,
             "VWAP 쿼리. result.value = sum(price*vol)/sum(vol)")
        .def("filter_sum", &PyPipeline::filter_sum,
             py::arg("symbol"),
             py::arg("column"),
             py::arg("threshold"),
             py::arg("from_ts") = static_cast<int64_t>(0),
             py::arg("to_ts")   = INT64_MAX,
             "Filter+Sum: column > threshold 인 rows의 sum(column)")
        .def("count", &PyPipeline::count,
             py::arg("symbol"),
             py::arg("from_ts") = static_cast<int64_t>(0),
             py::arg("to_ts")   = INT64_MAX,
             "COUNT 쿼리. result.ivalue = 총 행 수")

        // Zero-copy 컬럼 접근
        .def("get_column", &PyPipeline::get_column,
             py::arg("symbol"), py::arg("name"),
             py::return_value_policy::reference_internal,
             "Zero-copy numpy view. RDB 메모리를 직접 참조 (복사 없음).\n"
             "name: 'price', 'volume', 'timestamp', 'msg_type'")

        // 통계
        .def("stats", &PyPipeline::stats,
             "파이프라인 통계 dict 반환");
}
