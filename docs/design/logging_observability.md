# ZeptoDB: Logging & Observability Design

**Version:** 1.0
**Date:** 2026-03-25
**Status:** ✅ Implemented
**Related code:** `include/zeptodb/util/logger.h`, `include/zeptodb/common/logger.h`,
`include/zeptodb/auth/audit_buffer.h`, `src/server/http_server.cpp`

---

## 1. Overview

ZeptoDB의 로깅 시스템은 프로덕션 운영에서 필요한 세 가지 관측성(observability) 축을
모두 커버합니다:

| 축 | 구현 | 용도 |
|----|------|------|
| **Logs** | 구조화된 JSON 로거 + Access Log + Slow Query Log | 디버깅, 감사, 장애 분석 |
| **Metrics** | Prometheus `/metrics` + MetricsCollector | 대시보드, 알림, 용량 계획 |
| **Traces** | X-Request-Id + query_id 상관관계 | 요청 추적, 분산 트레이싱 |

### 설계 원칙

1. **구조화된 출력** — 모든 로그는 JSON 형식. `grep`이 아닌 `jq`로 분석
2. **비동기 I/O** — 로깅이 hot path 지연에 영향을 주지 않음 (spdlog async)
3. **자동 로테이션** — 디스크 고갈 방지 (크기 기반 rotating file)
4. **레벨 분리** — 운영 환경에서 INFO, 디버깅 시 DEBUG/TRACE로 동적 전환
5. **상관관계 ID** — request_id ↔ query_id로 end-to-end 추적

---

## 2. 로거 아키텍처

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Code                          │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │ Storage/Core │  │  HTTP Server │  │  Auth/Security   │  │
│  │ ZEPTO_INFO() │  │ util::Logger │  │  AuditBuffer     │  │
│  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
│         │                  │                    │             │
├─────────┼──────────────────┼────────────────────┼────────────┤
│         ▼                  ▼                    ▼             │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │ common::    │  │ util::Logger │  │ spdlog           │   │
│  │ Logger      │  │ (Singleton)  │  │ "zepto_audit"    │   │
│  │ (Static)    │  │              │  │ (dedicated)      │   │
│  └──────┬──────┘  └──────┬───────┘  └────────┬─────────┘   │
│         │                 │                    │              │
│         ▼                 ▼                    ▼              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │                    spdlog backend                        │ │
│  │  ┌─────────┐  ┌──────────────┐  ┌───────────────────┐  │ │
│  │  │ stdout  │  │ rotating     │  │ audit log file    │  │ │
│  │  │ (color) │  │ file sink    │  │ (7-year retain)   │  │ │
│  │  └─────────┘  └──────────────┘  └───────────────────┘  │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 로거 구분

| 로거 | 네임스페이스 | 출력 형식 | Sink | 용도 |
|------|-------------|----------|------|------|
| **common::Logger** | `zeptodb::` | 텍스트 (`[timestamp] [level] [source:line] msg`) | stdout (color) | 엔진 내부 (storage, ingestion, execution) |
| **util::Logger** | `zeptodb::util::` | JSON 구조화 | rotating file + stdout | HTTP 서버, 운영 이벤트 |
| **Audit Logger** | spdlog `"zepto_audit"` | 텍스트 | 전용 파일 | 보안 감사 (SOC2/EMIR/MiFID II) |

### 2.2 매크로 매핑

| 매크로 | 로거 | 레벨 | 소스 위치 포함 |
|--------|------|------|---------------|
| `ZEPTO_INFO(...)` | common::Logger | INFO | ✅ (`__FILE__:__LINE__`) |
| `ZEPTO_DEBUG(...)` | common::Logger | DEBUG | ✅ |
| `ZEPTO_WARN(...)` | common::Logger | WARN | ✅ |
| `ZEPTO_ERROR(...)` | common::Logger | ERROR | ✅ |
| `APEX_TRACE(...)` | common::Logger | TRACE | ✅ |
| `APEX_CRITICAL(...)` | common::Logger | CRITICAL | ✅ |
| `LOG_INFO(msg, comp)` | util::Logger | INFO | ❌ (component 태그) |
| `LOG_WARN(msg, comp)` | util::Logger | WARN | ❌ (component 태그) |
| `LOG_ERROR(msg, comp)` | util::Logger | ERROR | ❌ (component 태그) |

---

## 3. 로그 카테고리 및 정책

### 3.1 Access Log (HTTP 요청)

모든 HTTP 요청에 대해 자동 기록됩니다.

```json
{
  "request_id": "r0001a3",
  "method": "POST",
  "path": "/",
  "status": 200,
  "duration_us": 532,
  "request_bytes": 42,
  "response_bytes": 1024,
  "remote_addr": "10.0.1.5",
  "subject": "algo-service"
}
```

| 필드 | 설명 |
|------|------|
| `request_id` | 프로세스 내 유일한 monotonic hex ID (`r<hex>`) |
| `method` | HTTP 메서드 |
| `path` | 요청 경로 |
| `status` | HTTP 상태 코드 |
| `duration_us` | 요청 처리 시간 (마이크로초) |
| `request_bytes` | 요청 body 크기 |
| `response_bytes` | 응답 body 크기 |
| `remote_addr` | 클라이언트 IP |
| `subject` | 인증된 사용자 ID (API key name 또는 JWT sub) |

**로그 레벨 정책:**

| HTTP 상태 | 로그 레벨 | 근거 |
|-----------|----------|------|
| 2xx, 3xx | INFO | 정상 요청 |
| 4xx | WARN | 클라이언트 에러 (잘못된 SQL, 인증 실패 등) |
| 5xx | ERROR | 서버 에러 (즉시 조사 필요) |

**Component 태그:** `"http"`

### 3.2 Slow Query Log

100ms 이상 소요되거나 에러가 발생한 쿼리를 기록합니다.

```json
{
  "query_id": "q_a1b2c3",
  "subject": "algo-service",
  "duration_us": 150234,
  "rows": 50000,
  "ok": true,
  "sql": "SELECT vwap(price, volume) FROM trades WHERE ..."
}
```

| 정책 | 값 | 설정 가능 |
|------|-----|----------|
| Slow query 임계값 | 100ms | 향후 런타임 설정 예정 |
| SQL 최대 길이 | 200자 (truncate) | 보안상 고정 |
| 에러 쿼리 | 항상 기록 | — |

**로그 레벨:** 에러 쿼리 → WARN, slow query → INFO
**Component 태그:** `"query"`

### 3.3 Server Lifecycle Log

서버 시작/종료 시 기록됩니다.

```json
{"event":"server_start","port":8123,"tls":false,"auth":true,"async":true}
{"event":"server_stop","port":8123}
```

**Component 태그:** `"http"`

### 3.4 Engine Log (Storage/Ingestion/Execution)

엔진 내부 이벤트는 `common::Logger`를 통해 텍스트 형식으로 기록됩니다.

```
[2026-03-25 06:20:34.124668] [info] [pipeline.cpp:57] ZeptoPipeline 초기화 (arena=32MB)
[2026-03-25 06:20:34.124708] [info] [partition_manager.cpp:95] New partition: symbol=1, hour=0
[2026-03-25 06:20:34.130000] [warn] [arena_allocator.cpp:45] Hugepage allocation failed, fallback
```

**주요 이벤트:**

| 컴포넌트 | 이벤트 | 레벨 |
|----------|--------|------|
| Pipeline | 초기화, 시작, 중지 | INFO |
| PartitionManager | 파티션 생성/삭제 | INFO |
| ArenaAllocator | 메모리 할당, hugepage fallback | INFO/WARN |
| TickPlant | 초기화, 큐 오버플로 | INFO/WARN |
| WAL | 쓰기 시작/완료, 에러 | INFO/ERROR |
| HDB | flush 시작/완료, 압축 | INFO |
| JIT | 컴파일 시작/완료 | INFO |
| FlushManager | 티어링 이동 (Hot→Warm→Cold) | INFO |

### 3.5 Audit Log (보안 감사)

인증/인가 이벤트를 기록합니다. 규제 준수(SOC2, EMIR, MiFID II)를 위해
7년 보존이 필요합니다.

```json
{
  "timestamp_ns": 1711234567000000000,
  "subject": "algo-service",
  "role": "writer",
  "action": "POST /",
  "detail": "apikey-auth",
  "remote_addr": "10.0.1.5"
}
```

**이중 기록:**
1. **파일** — spdlog `"zepto_audit"` 전용 로거 → 디스크 파일 (장기 보존)
2. **메모리** — `AuditBuffer` ring buffer (10,000건) → `GET /admin/audit` API

**기록 대상:**

| 이벤트 | 기록 여부 |
|--------|----------|
| 인증 성공 | ✅ |
| 인증 실패 (401) | ✅ |
| 인가 실패 (403) | ✅ |
| Rate limit 초과 (429) | ✅ |
| API key 생성/삭제 | ✅ |
| 쿼리 kill (admin) | ✅ |

---

## 4. 로그 로테이션 및 보존 정책

### 4.1 파일 로테이션

| 로그 종류 | 파일 경로 | 최대 크기 | 최대 파일 수 | 보존 기간 |
|-----------|----------|----------|-------------|----------|
| Application (JSON) | `/var/log/zeptodb/zeptodb.log` | 100 MB | 10개 | ~1 GB 총량 |
| Audit | `/var/log/zeptodb/audit.log` | 설정 가능 | 외부 로테이션 | 7년 (규제) |
| Engine (stdout) | systemd journal 또는 리다이렉트 | — | — | 인프라 정책 |

### 4.2 환경별 권장 설정

| 환경 | 로그 레벨 | Access Log | Slow Query | Audit |
|------|----------|------------|------------|-------|
| **Development** | DEBUG | ✅ | ✅ (>0ms) | ❌ |
| **Staging** | INFO | ✅ | ✅ (>50ms) | ✅ |
| **Production** | INFO | ✅ | ✅ (>100ms) | ✅ |
| **HFT (ultra-low latency)** | WARN | ❌ (Prometheus만) | ✅ (>1ms) | ✅ |

### 4.3 디스크 사용량 추정

| 시나리오 | QPS | Access Log/일 | Slow Query/일 | 총 디스크/일 |
|----------|-----|--------------|--------------|-------------|
| 소규모 | 100 | ~50 MB | ~1 MB | ~51 MB |
| 중규모 | 1,000 | ~500 MB | ~10 MB | ~510 MB |
| 대규모 | 10,000 | ~5 GB | ~100 MB | ~5.1 GB |
| HFT | 100,000 | Access Log OFF | ~1 GB | ~1 GB |

대규모 환경에서는 access log를 샘플링하거나 외부 로그 수집기(Fluentd/Vector)로
스트리밍하는 것을 권장합니다.

---

## 5. 트레이싱 (Request Correlation)

### 5.1 Request ID 체인

```
Client                    HttpServer                    QueryExecutor
  │                          │                              │
  │  POST / (SQL)            │                              │
  │ ─────────────────────►   │                              │
  │                          │  X-Zepto-Request-Id: r00a1   │
  │                          │  X-Zepto-Start-Us: 170000    │
  │                          │  X-Zepto-Subject: algo-svc   │
  │                          │                              │
  │                          │  query_id = q_xxxx           │
  │                          │ ────────────────────────►    │
  │                          │                              │
  │                          │  Access Log (request_id)     │
  │                          │  Slow Query Log (query_id)   │
  │                          │                              │
  │  ◄─────────────────────  │                              │
  │  X-Request-Id: r00a1     │                              │
```

### 5.2 상관관계 추적 방법

**시나리오: 클라이언트가 느린 응답을 보고**

```bash
# 1. 클라이언트가 받은 X-Request-Id로 access log 검색
jq 'select(.request_id == "r00a1")' /var/log/zeptodb/zeptodb.log

# 2. 같은 시간대의 slow query log 확인
jq 'select(.component == "query" and .duration_us > 100000)' /var/log/zeptodb/zeptodb.log

# 3. 특정 subject의 모든 요청 추적
jq 'select(.subject == "algo-service")' /var/log/zeptodb/zeptodb.log

# 4. 4xx/5xx 에러만 필터
jq 'select(.status >= 400)' /var/log/zeptodb/zeptodb.log

# 5. 특정 시간 범위의 P99 latency 계산
jq -s '[.[] | select(.component == "http") | .duration_us] | sort | .[(length * 0.99 | floor)]' \
  /var/log/zeptodb/zeptodb.log
```

### 5.3 분산 환경 트레이싱

클러스터 모드에서 coordinator가 remote 노드에 scatter하는 경우:

```
Client → Coordinator (request_id: r00a1)
           ├── Node 1 (METRICS_REQUEST, logged locally)
           ├── Node 2 (METRICS_REQUEST, logged locally)
           └── Node 3 (METRICS_REQUEST, logged locally)
```

각 노드는 자체 access log를 기록합니다. 중앙 집중 로그 수집(Fluentd → S3/ES)을
통해 `request_id`로 전체 경로를 추적할 수 있습니다.

---

## 6. Prometheus Metrics (실시간 모니터링)

로그와 별도로, 실시간 모니터링을 위한 Prometheus 메트릭을 제공합니다.

### 6.1 HTTP 메트릭

| 메트릭 | 타입 | 설명 |
|--------|------|------|
| `zepto_http_requests_total` | counter | 총 HTTP 요청 수 |
| `zepto_http_active_sessions` | gauge | 현재 활성 세션 수 |

### 6.2 엔진 메트릭

| 메트릭 | 타입 | 설명 |
|--------|------|------|
| `zepto_ticks_ingested_total` | counter | 총 인제스트 틱 수 |
| `zepto_ticks_stored_total` | counter | 총 저장 틱 수 |
| `zepto_ticks_dropped_total` | counter | 드롭된 틱 수 |
| `zepto_queries_executed_total` | counter | 총 쿼리 실행 수 |
| `zepto_rows_scanned_total` | counter | 총 스캔 행 수 |
| `zepto_server_up` | gauge | 서버 가동 상태 |
| `zepto_server_ready` | gauge | 서버 준비 상태 |

### 6.3 MetricsCollector (시계열 히스토리)

`MetricsCollector`는 3초 간격으로 `PipelineStats`를 스냅샷하여 in-memory
ring buffer에 저장합니다. `GET /admin/metrics/history`로 조회 가능합니다.

| 설정 | 기본값 | 설명 |
|------|--------|------|
| `interval` | 3초 | 캡처 주기 |
| `capacity` | 1,200 | 최대 스냅샷 수 (1시간) |
| `max_memory_bytes` | 256 KB | 메모리 하드 리밋 |
| `response_limit` | 600 | API 응답 최대 건수 (30분) |

---

## 7. 운영 가이드

### 7.1 로거 초기화

```cpp
// 서버 시작 시 (main 또는 서버 초기화 코드)
#include "zeptodb/util/logger.h"

// JSON 구조화 로거 초기화
zeptodb::util::Logger::instance().init(
    "/var/log/zeptodb",    // 로그 디렉토리
    zeptodb::util::LogLevel::INFO,  // 레벨
    100,                   // 파일당 최대 100 MB
    10                     // 최대 10개 파일 (총 1 GB)
);
```

### 7.2 런타임 레벨 변경

```cpp
// 디버깅 시 레벨 낮추기
zeptodb::util::Logger::instance().set_level(zeptodb::util::LogLevel::DEBUG);

// 복구 후 원복
zeptodb::util::Logger::instance().set_level(zeptodb::util::LogLevel::INFO);
```

### 7.3 로그 수집 파이프라인 (권장)

```
ZeptoDB Node                    Log Collector              Storage
┌──────────┐                   ┌──────────┐              ┌─────────┐
│ zeptodb  │  rotating file    │ Fluentd  │   S3/ES/     │ S3      │
│ .log     │ ───────────────►  │ /Vector  │ ──────────►  │ OpenES  │
│          │                   │          │              │ Loki    │
└──────────┘                   └──────────┘              └─────────┘
│ audit    │  dedicated file   │ Fluentd  │   S3 (7yr)   │ S3      │
│ .log     │ ───────────────►  │ /Vector  │ ──────────►  │ Glacier │
└──────────┘                   └──────────┘              └─────────┘
│ /metrics │  Prometheus pull  │ Prom     │              │ Prom    │
│          │ ◄───────────────  │ Server   │ ──────────►  │ Grafana │
└──────────┘                   └──────────┘              └─────────┘
```

### 7.4 Grafana 알림 규칙 (로그 기반)

| 알림 | 조건 | 심각도 |
|------|------|--------|
| High error rate | 5xx > 1% (5분 윈도우) | Critical |
| Slow query spike | P99 > 500ms (5분 윈도우) | Warning |
| Auth failure burst | 401/403 > 100/분 | Warning |
| Disk usage | 로그 디렉토리 > 80% | Warning |

---

## 8. 보안 고려사항

| 항목 | 정책 |
|------|------|
| SQL 로깅 | 200자 truncate (민감 데이터 노출 방지) |
| API Key | 로그에 절대 기록하지 않음 (subject name만 기록) |
| PII | 로그에 가격/거래량 등 비즈니스 데이터 미포함 |
| 감사 로그 접근 | ADMIN role만 `GET /admin/audit` 접근 가능 |
| 로그 파일 권한 | `0640` (owner: zeptodb, group: ops) |
| 전송 암호화 | 로그 수집기 → S3 전송 시 TLS 필수 |

---

## 9. 파일 맵

```
include/zeptodb/
├── util/logger.h              # 구조화된 JSON 로거 (싱글톤)
├── common/logger.h            # 엔진 내부 텍스트 로거 (static)
├── auth/audit_buffer.h        # 감사 이벤트 ring buffer
└── server/
    ├── http_server.h          # Access log, X-Request-Id 문서
    └── metrics_collector.h    # 시계열 메트릭 수집기

src/
├── util/logger.cpp            # JSON 로거 구현 (async spdlog)
├── common/logger.cpp          # 텍스트 로거 구현
├── auth/auth_manager.cpp      # 감사 로거 ("zepto_audit")
└── server/http_server.cpp     # Access log, slow query log 구현

docs/
├── design/logging_observability.md  # 이 문서
└── devlog/029_http_observability.md # 구현 기록
```
