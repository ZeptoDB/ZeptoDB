# ZeptoDB C++ API 레퍼런스

*최종 업데이트: 2026-03-22*

---

## 목차

- [ZeptoPipeline](#zeptopipeline)
- [QueryExecutor (SQL)](#queryexecutor-sql)
- [PartitionManager & Partition](#partitionmanager--partition)
- [TickMessage](#tickmessage)
- [Auth — CancellationToken](#auth--cancellationtoken)

---

## 빠른 시작 예제

### 완전한 예제: 틱 인제스트 → SQL 실행 → 컬럼 직접 접근

```cpp
#include "zeptodb/core/pipeline.h"
#include "zeptodb/sql/executor.h"
#include "zeptodb/common/types.h"
#include <iostream>

int main() {
    using namespace zeptodb::core;
    using namespace zeptodb::sql;

    // 1. 파이프라인 생성 및 시작
    ZeptoPipeline pipeline;
    pipeline.start();

    // 2. 틱 인제스트
    for (int i = 0; i < 1000; ++i) {
        TickMessage msg;
        msg.symbol_id = 1;
        msg.price     = 15000 + i * 10;
        msg.volume    = 100 + i;
        msg.recv_ts   = now_ns();
        pipeline.ingest_tick(msg);
    }
    pipeline.drain_sync();  // 컬럼 스토어로 동기 플러시

    // 3. 직접 쿼리 (C++ API)
    auto r = pipeline.query_vwap(1);
    std::cout << "VWAP: " << r.value
              << "  rows_scanned: " << r.rows_scanned << "\n";

    // 4. SQL 쿼리
    QueryExecutor exec{pipeline};
    exec.enable_parallel();

    auto result = exec.execute(
        "SELECT count(*), sum(volume), avg(price), vwap(price, volume) "
        "FROM trades WHERE symbol = 1"
    );

    if (!result.ok()) {
        std::cerr << "Error: " << result.error << "\n";
        return 1;
    }
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        std::cout << result.column_names[i] << " = "
                  << result.rows[0][i] << "\n";
    }
    std::cout << "실행 시간: " << result.execution_time_us << " μs\n";

    // 5. 제로카피 컬럼 직접 접근
    auto& pm    = pipeline.partition_manager();
    auto  parts = pm.get_partitions(1);
    for (auto* part : parts) {
        const int64_t* prices = part->get_column("price");
        size_t n = part->row_count();
        std::cout << "파티션: " << n << " 행, "
                  << "첫 가격 = " << prices[0] << "\n";
    }

    pipeline.stop();
    return 0;
}
```

### 5분 바 집계 SQL

```cpp
auto result = exec.execute(R"sql(
    SELECT xbar(timestamp, 300000000000) AS bar,
           first(price) AS open,
           max(price)   AS high,
           min(price)   AS low,
           last(price)  AS close,
           sum(volume)  AS volume
    FROM trades
    WHERE symbol = 1
    GROUP BY xbar(timestamp, 300000000000)
    ORDER BY bar ASC
)sql");

for (const auto& row : result.rows) {
    std::cout << "bar=" << row[0]
              << " O=" << row[1] << " H=" << row[2]
              << " L=" << row[3] << " C=" << row[4]
              << " V=" << row[5] << "\n";
}
```

### 시간 범위 쿼리 (파티션 프루닝 포함)

```cpp
int64_t from_ns = now_ns() - 3600LL * 1'000'000'000LL;  // 최근 1시간
int64_t to_ns   = now_ns();

auto result = exec.execute(
    "SELECT vwap(price, volume), count(*) FROM trades "
    "WHERE symbol = 1 AND timestamp BETWEEN "
    + std::to_string(from_ns) + " AND " + std::to_string(to_ns)
);
```

---

## ZeptoPipeline

`#include "zeptodb/core/pipeline.h"` — 네임스페이스: `zeptodb::core`

틱 인제스트 → 컬럼 스토어 → 쿼리 실행을 담당하는 최상위 엔드투엔드 파이프라인입니다.

### 생성

```cpp
#include "zeptodb/core/pipeline.h"
using namespace zeptodb::core;

// 기본 설정 (순수 인메모리, 파티션당 32 MB 아레나)
ZeptoPipeline pipeline;

// 커스텀 설정
PipelineConfig cfg;
cfg.arena_size_per_partition = 64ULL * 1024 * 1024;
cfg.drain_batch_size         = 512;
cfg.drain_sleep_us           = 5;
cfg.storage_mode             = StorageMode::TIERED;
cfg.hdb_base_path            = "/data/zepto_hdb";
ZeptoPipeline pipeline{cfg};
```

### 생명주기

```cpp
pipeline.start();         // 백그라운드 드레인 스레드 시작
pipeline.stop();          // 큐 플러시 + 드레인 스레드 종료

// 동기 드레인 (테스트용)
size_t drained = pipeline.drain_sync();
size_t drained = pipeline.drain_sync(/*max_items=*/1000);
```

### PipelineConfig 필드

```cpp
struct PipelineConfig {
    size_t      arena_size_per_partition = 32ULL * 1024 * 1024;
    size_t      drain_batch_size         = 256;
    uint32_t    drain_sleep_us           = 10;
    StorageMode storage_mode             = StorageMode::PURE_IN_MEMORY;
    std::string hdb_base_path            = "/tmp/zepto_hdb";
    FlushConfig flush_config{};
};
```

### StorageMode

```cpp
enum class StorageMode : uint8_t {
    PURE_IN_MEMORY = 0,  // HFT: HDB 없음, 최대 레이턴시
    TIERED         = 1,  // RDB (당일) + HDB (과거) 혼합
    PURE_ON_DISK   = 2,  // 백테스트: HDB만 사용
};
```

### 인제스트

```cpp
TickMessage msg;
msg.symbol_id = 1;
msg.price     = 15000;
msg.volume    = 100;
msg.recv_ts   = now_ns();

// 락프리, 스레드 안전 — 링 버퍼가 가득 차면 false 반환
bool ok = pipeline.ingest_tick(msg);
```

### 직접 쿼리

```cpp
QueryResult r = pipeline.query_vwap(symbol_id);                      // 전체 기간 VWAP
QueryResult r = pipeline.query_vwap(symbol_id, from_ns, to_ns);      // 시간 범위 VWAP
QueryResult r = pipeline.query_count(symbol_id);                     // 행 수
QueryResult r = pipeline.query_filter_sum(symbol_id, "volume", 100); // sum(volume) WHERE volume > 100
size_t total  = pipeline.total_stored_rows();
```

### QueryResult

```cpp
struct QueryResult {
    double      value        = 0.0;   // VWAP, AVG
    int64_t     ivalue       = 0;     // COUNT, SUM
    size_t      rows_scanned = 0;
    int64_t     latency_ns   = 0;
    std::string error_msg;

    bool ok() const;
};
```

### 통계

```cpp
const PipelineStats& s = pipeline.stats();
s.ticks_ingested.load()        // 수신된 총 틱 수
s.ticks_stored.load()          // 컬럼 스토어에 기록된 틱 수
s.ticks_dropped.load()         // 드롭된 틱 (링 버퍼 오버플로우)
s.queries_executed.load()
s.total_rows_scanned.load()
s.partitions_created.load()
s.last_ingest_latency_ns.load()
```

### 하위 컴포넌트 접근

```cpp
PartitionManager& pm  = pipeline.partition_manager();
TickPlant&        tp  = pipeline.tick_plant();

// PURE_IN_MEMORY 모드에서는 nullptr
HDBReader*    hdb = pipeline.hdb_reader();
FlushManager* fm  = pipeline.flush_manager();
```

---

## QueryExecutor (SQL)

`#include "zeptodb/sql/executor.h"` — 네임스페이스: `zeptodb::sql`

SQL 문자열을 파싱하고 `ZeptoPipeline`에 대해 실행합니다.

### 생성

```cpp
QueryExecutor exec{pipeline};

// 커스텀 스케줄러 주입 (테스트 또는 분산 환경)
auto sched = std::make_unique<MyDistributedScheduler>(...);
QueryExecutor exec{pipeline, std::move(sched)};
```

### 병렬 실행

```cpp
exec.enable_parallel();                        // 자동 스레드 수 (hardware_concurrency)
exec.enable_parallel(8, 100'000);              // 8 스레드, 10만 행 미만은 직렬
exec.disable_parallel();

const ParallelOptions& opts = exec.parallel_options();
// opts.enabled, opts.num_threads, opts.row_threshold
```

### SQL 실행

```cpp
QueryResultSet result = exec.execute(
    "SELECT vwap(price, volume), count(*) FROM trades WHERE symbol = 1"
);

if (!result.ok()) {
    std::cerr << "Error: " << result.error << "\n";
    return;
}

for (const std::string& col : result.column_names) { ... }
for (const std::vector<int64_t>& row : result.rows) { ... }
std::cout << result.execution_time_us << " μs\n";
```

### 취소 토큰과 함께 실행

```cpp
#include "zeptodb/auth/cancellation_token.h"

zeptodb::auth::CancellationToken token;

std::thread canceller([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    token.cancel();
});

QueryResultSet result = exec.execute(sql, &token);
canceller.join();
// 취소되면 result.error == "Query cancelled"
```

### QueryResultSet

```cpp
struct QueryResultSet {
    std::vector<std::string>          column_names;
    std::vector<ColumnType>           column_types;
    std::vector<std::vector<int64_t>> rows;

    double      execution_time_us = 0.0;
    size_t      rows_scanned      = 0;
    std::string error;

    bool ok() const { return error.empty(); }
};
```

---

## PartitionManager & Partition

`#include "zeptodb/storage/partition_manager.h"` — 네임스페이스: `zeptodb::storage`

### PartitionManager

```cpp
PartitionManager& pm = pipeline.partition_manager();

// 파티션 조회/생성
Partition& part = pm.get_or_create(symbol_id, timestamp_ns);

// symbol의 모든 파티션 (시간순)
std::vector<Partition*> parts = pm.get_partitions(symbol_id);

// [from_ns, to_ns]와 겹치는 파티션 (O(1) 겹침 체크)
std::vector<Partition*> parts = pm.get_partitions_for_time_range(
    symbol_id, from_ns, to_ns
);

size_t n = pm.partition_count();
```

### Partition

```cpp
// 제로카피 컬럼 포인터 (읽기 전용)
const int64_t* prices     = part.get_column("price");
const int64_t* volumes    = part.get_column("volume");
const int64_t* timestamps = part.get_column("timestamp");
size_t         row_count  = part.row_count();

const PartitionKey& key = part.key();
key.symbol_id    // SymbolId (int64)
key.date         // 날짜 버킷 (int64, 일(day)로 내림한 나노초)

// 타임스탬프 범위 이진 탐색 O(log n) — [begin_row, end_row) 반환
auto [begin_row, end_row] = part.timestamp_range(from_ns, to_ns);

// O(1) 겹침 확인 (first/last 행 타임스탬프 사용)
bool overlaps = part.overlaps_time_range(from_ns, to_ns);
```

---

## TickMessage

`#include "zeptodb/common/types.h"`

```cpp
using SymbolId  = int64_t;
using Timestamp = int64_t;   // 유닉스 에포크 이후 나노초

struct TickMessage {
    SymbolId  symbol_id = 0;
    int64_t   price     = 0;     // 스케일 정수 (예: 150.25 → 15025 at scale 100)
    int64_t   volume    = 0;
    Timestamp recv_ts   = 0;     // 유닉스 에포크 이후 나노초
    int64_t   bid       = 0;     // 선택사항
    int64_t   ask       = 0;     // 선택사항
    int64_t   extra[4]  = {};    // 사용자 정의 컬럼
};
```

### 타임스탬프 유틸리티

```cpp
Timestamp ts = now_ns();   // 현재 나노초 타임스탬프

// 에포크 초 → 나노초
Timestamp ts = 1711000000LL * 1'000'000'000LL;

// 에포크 밀리초 → 나노초
Timestamp ts = 1711000000000LL * 1'000'000LL;
```

---

## Auth — CancellationToken

`#include "zeptodb/auth/cancellation_token.h"` — 네임스페이스: `zeptodb::auth`

다른 스레드에서 실행 중인 쿼리를 취소하는 데 사용합니다.

```cpp
zeptodb::auth::CancellationToken token;

// 다른 스레드에서:
token.cancel();
token.is_cancelled();   // bool
token.reset();          // 재사용을 위한 초기화
```

---

## 빠른 빌드 참조

```bash
mkdir -p build && cd build
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 \
  -DCMAKE_CXX_COMPILER=clang++-19 \
  -DAPEX_USE_PARQUET=OFF \
  -DAPEX_USE_S3=OFF \
  -DAPEX_BUILD_PYTHON=OFF
ninja -j$(nproc)

./tests/zepto_tests
```

---

*참고: [SQL 레퍼런스](SQL_REFERENCE_ko.md) · [Python 레퍼런스](PYTHON_REFERENCE_ko.md) · [HTTP 레퍼런스](HTTP_REFERENCE_ko.md)*
