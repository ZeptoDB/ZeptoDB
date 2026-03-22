# APEX-DB 백로그

## 즉시 (다음 커밋)
- [ ] **설계 문서 전체 업데이트** — 현재 구현 상태에 맞게 동기화
  - high_level_architecture.md: SQL/HTTP/Cluster 레이어 추가
  - initial_doc.md: 범용 OLAP/TSDb 타겟 확장
  - system_requirements.md: SQL/HTTP/JOIN/Window 요구사항
  - layer4: nanobind → pybind11, DSL 실제 구현
  - README.md: 전체 기능 + 최신 벤치마크

## 높은 우선순위
- [ ] **SQL 파서 + HTTP API** — ClickHouse 사용자 유입 핵심
- [ ] **시간 범위 인덱스** — 거의 공짜, 이미 정렬된 데이터
- [ ] **asof JOIN** — 금융 핵심, 투 포인터 O(n) + SIMD
- [ ] **GROUP BY 집계** — low cardinality(금융) 확실한 우위
- [ ] **Graviton (ARM) 빌드 테스트** — r8g 인스턴스, Highway SVE

## 중간 우선순위
- [ ] **Data/Compute 노드 분리** — JOIN을 별도 Compute Node에서 RDMA remote_read로 실행, Data Node 영향 제로
- [ ] **equi hash JOIN** — 리스크/컴플라이언스, Compute Node 전용
- [ ] **window 함수** — rolling avg, rank (퀀트 필수)
- [ ] **DuckDB 임베딩 (복잡한 JOIN 위임)** — Arrow zero-copy 전달
- [ ] **JIT SIMD emit** — LLVM JIT에서 AVX2/512 벡터 IR 생성
- [ ] **멀티스레드 drain** — sharded drain threads
- [ ] **Ring Buffer 동적 조정** — direct-to-storage 경로
- [ ] **HugePages 튜닝** — 자동화
- [ ] **리소스 격리** — realtime(코어0-3) vs analytics(코어4-7) CPU pinning

## 낮은 우선순위 (Phase C-3 이후)
- [ ] **AWS Fleet API 통합** — Warm Pool + Placement Group
- [ ] **DynamoDB 메타데이터** — 파티션 맵
- [ ] **레플리케이션** — WAL 기반 비동기
- [ ] **Graph 인덱스 (CSR)** — FDS 자금 이동 추적
- [ ] **ClickHouse 마이그레이션 도구**
- [ ] **ClickHouse wire protocol 호환**

## 완료
- [x] Phase E — End-to-End Pipeline MVP
- [x] Phase B — Highway SIMD + LLVM JIT
- [x] Phase B v2 — BitMask filter (11x), JIT O3 (2.6x)
- [x] Phase A — HDB Tiered Storage + LZ4
- [ ] Phase D — Python Bridge (진행 중)
- [ ] Phase C — Distributed Memory (예정)
