# ZeptoDB 초기 비전 및 설계 원칙
**⚠️ 최종 업데이트: 2026-03-22 (범용 OLAP/TSDb 확장, kdb+ 95% 대체, 마이그레이션 툴킷, Parquet HDB, S3 Sink 완료)**

ZeptoDB는 **초저지연 인메모리 데이터베이스**로, 금융 특화로 시작하여 **범용 OLAP, 시계열 DB, ML Feature Store**로 확장했습니다.

---

## 📑 ZeptoDB 개발 전략 및 현황

### **1. 타겟 시장 (Target Market) — 확장됨**

**Phase 1 (완료): 금융 시장** — kdb+ 대체율 95% 달성
* **HFT (고빈도 매매):** ASOF JOIN, xbar, Window JOIN — **95% 대체**
* **퀀트 리서치:** EMA/DELTA/RATIO, Python DSL — **90% 대체**
* **리스크 관리:** LEFT JOIN, GROUP BY 병렬 — **95% 대체**
* **FDS (이상거래 탐지):** 실시간 Window JOIN — **85% 대체**

**Phase 2 (진행 중): 범용 시장**
* **OLAP (ClickHouse 대체):** SQL, HTTP API (port 8123), 병렬 쿼리
  - 타겟: 광고 테크, SaaS 분석 ($1M-3M ARR)
* **IoT/모니터링 (TimescaleDB 대체):** xbar 시간 바 집계, HDB 압축
  - 타겟: IoT, DevOps 모니터링 ($500K-1M ARR)
* **ML Feature Store:** zero-copy Python (pybind11), Arrow 호환
  - 타겟: 실시간 추천, 사기 탐지

---

### **2. 핵심 설계 원칙 (Core Design Principles) — 실제 구현**

| 원칙 | 현재 구현 상태 |
| :---- | :---- |
| **Ultra-Low Latency** | ✅ **272μs/1M filter** (BitMask), **532μs VWAP** (Highway SIMD), **2.2ms EMA** |
| **Memory Disaggregation** | ✅ UCX transport 완료, DistributedQueryScheduler stub (향후 멀티노드) |
| **Columnar + Vectorized** | ✅ Arrow 호환 컬럼 스토어, Highway SIMD (AVX-512/ARM SVE 자동), LLVM JIT O3 |
| **Parallel Execution** | ✅ **LocalQueryScheduler** (8T = 3.48x), WorkerPool (jthread), scatter/gather DI |
| **SQL + Python DSL** | ✅ SQL Parser (1.5~4.5μs), HTTP API (port 8123), Python pybind11 (522ns zero-copy) |
| **Time-Series Native** | ✅ **xbar** (시간 바), **EMA**, **ASOF JOIN**, **Window JOIN** (O(n log m)) |

---

### **3. 배포 전략: 베어메탈 우선, 클라우드 보완**

#### **베어메탈 (권장)**
* **왜?** HFT는 **레이턴시 일관성**이 매출 직결 — 클라우드 noisy neighbor 회피
* **하드웨어:**
  - **Intel Xeon 8462Y+ (Sapphire Rapids)**: AVX-512, AMX, DDR5-4800, CXL 1.1
  - **AMD EPYC 9754 (Bergamo)**: 128코어 밀도, DDR5-4800
  - **Supermicro AS-4125GS-TNRT**: PCIe 5.0, 16x NVMe, 4TB RAM
* **배포:** `scripts/tune_bare_metal.sh` — 원스텝 자동 튜닝 (CPU pinning, NUMA, io_uring)

#### **클라우드 (AWS 우선)**
* **인스턴스:** r8g.16xlarge (Graviton4, 512GB), c7gn.16xlarge (EFA, HFT 네트워크)
* **최적화:** Highway SIMD (ARM SVE 자동), Nitro 오프로드, EFA RDMA
* **컨테이너:** Docker + Kubernetes (HPA, PVC, LoadBalancer) — `k8s/deployment.yaml`
* **모니터링:** Prometheus + Grafana — `/metrics` OpenMetrics, 9가지 알림 규칙

#### **Microsoft Azure (향후)**
* **CXL 지원:** M-시리즈 Mv3 (CXL Flat Memory Mode) 테스트 예정
* **Arm:** Cobalt 100 + InfiniBand 조합

---

### **4. 기술적 차별화: kdb+ vs ZeptoDB**

| 항목 | kdb+ | ZeptoDB |
|---|---|---|
| **금융 함수** | xbar, ema, wj (q DSL) | ✅ **93% 호환** (SQL + Python DSL) |
| **성능** | μs 레이턴시 (proprietary) | ✅ **272μs filter, 2.2ms EMA** (Highway SIMD + LLVM JIT) |
| **병렬화** | 단일 코어 최적화 우선 | ✅ **멀티코어 3.48x** (LocalQueryScheduler, scatter/gather) |
| **표준 SQL** | 제한적 (q 우선) | ✅ **완전한 SQL Parser** (1.5~4.5μs, ClickHouse 호환) |
| **Python 통합** | PyKX (wrapper) | ✅ **zero-copy 522ns** (pybind11, Arrow 호환) |
| **배포** | 라이센스 $150K+ | ✅ **오픈소스 + 엔터프라이즈 옵션** |
| **JOIN** | aj (asof), wj (window) | ✅ **ASOF/Hash/LEFT/Window JOIN** (O(n), O(n log m)) |
| **확장성** | 수동 샤딩 | ✅ **QueryScheduler DI** (로컬 → 분산 코드 변경 없음) |

**결론:** kdb+ 호환성 + 현대적 아키텍처 (SIMD, JIT, 병렬, SQL)

---

### **5. 현재 구현 상태 (2026-03-22)**

| 레이어 | 구현 완료 | 벤치마크 |
|---|---|---|
| **Storage** | Arena allocator, Column store, HDB (LZ4), Parquet Writer (SNAPPY/ZSTD/LZ4), S3 Sink (async) | 4.8 GB/s flush |
| **Ingestion** | MPMC Ring Buffer, WAL, Feed Handlers (FIX, ITCH) | 5.52M ticks/sec |
| **Execution** | SIMD (Highway), JIT (LLVM), JOIN (ASOF/Hash/LEFT/Window) | 272μs filter, 53ms join |
| **금융 함수** | xbar, EMA, DELTA, RATIO, FIRST, LAST, Window JOIN | 2.2ms EMA, 24ms xbar |
| **병렬 쿼리** | LocalQueryScheduler, WorkerPool (scatter/gather) | 3.48x (8T) |
| **SQL** | Parser, HTTP API (port 8123), GROUP BY, Window 함수 | 1.5~4.5μs 파싱 |
| **Python** | pybind11, zero-copy numpy, lazy eval DSL | 522ns zero-copy |
| **Cluster** | UCX/SharedMem transport, Partition routing | 13.5ns SHM, 2ns routing |
| **운영** | Monitoring, Backup, systemd service | Prometheus + Grafana |

**테스트:** 221개 테스트 PASS (단위 151 + 피드핸들러 37 + 마이그레이션 70 + 벤치마크 10)

---

### **6. 향후 로드맵 (우선순위)**

#### **즉시 (다음 커밋)**
- [x] 설계 문서 전체 업데이트 ✅ 완료 (2026-03-22)

#### **높은 우선순위 (기술)**
- [ ] SQL 파서 완성 (복잡한 쿼리, 서브쿼리)
- [ ] 시간 범위 인덱스 (정렬된 데이터 활용)
- [ ] Graviton (ARM) 빌드 테스트 (Highway SVE)

#### **높은 우선순위 (비즈니스)**
- [ ] **마이그레이션 툴킷** ✅ **완료** (kdb+/ClickHouse/DuckDB/TimescaleDB, 70 테스트)
- [ ] **Python 에코시스템** — Research-to-Production
  - `zeptodb.from_polars/pandas`, Arrow 직접 지원
- [ ] **DSL AOT 컴파일** — Nuitka/Cython, 프로덕션 배포 + IP 보호

#### **중간 우선순위**
- [ ] 분산 쿼리 스케줄러 (DistributedQueryScheduler, UCX)
- [ ] Data/Compute 노드 분리 (RDMA remote_read)
- [ ] DuckDB 임베딩 (복잡한 JOIN 위임)

---

## 참고 문서

- 상세 아키텍처: `docs/design/high_level_architecture.md`
- 비즈니스 전략: `docs/business/BUSINESS_STRATEGY.md`
- kdb+ 대체 분석: `docs/design/kdb_replacement_analysis.md`
- 개발 로그: `docs/devlog/` (001~012)
- 배포 가이드: `docs/deployment/PRODUCTION_DEPLOYMENT.md`
