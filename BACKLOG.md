# APEX-DB 백로그

## 높은 우선순위
- [ ] **Graviton (ARM) 빌드 테스트** — r8g 인스턴스에서 빌드 + 벤치마크, Highway SVE 자동 디스패치 검증
- [ ] **JIT SIMD emit** — LLVM JIT에서 AVX2/512 벡터 IR 생성 → C++ lambda 수준 목표
- [ ] **sum 멀티컬럼 fusion** — price+volume 동시 처리로 메모리 접근 반감
- [ ] **filter+aggregate 파이프라인 fusion** — filter → sum 1-pass

## 중간 우선순위
- [ ] **멀티스레드 drain** — sharded drain threads (symbol별 분리) → 동시 인제스션 성능 개선
- [ ] **Ring Buffer 크기 동적 조정** — 64K 초과 배치 시 direct-to-storage 경로
- [ ] **HugePages 튜닝** — /proc/sys/vm/nr_hugepages 설정 자동화
- [ ] **Flush Manager 자동 seal** — 파티션 age 기반 자동 봉인

## 낮은 우선순위 (Phase C 이후)
- [ ] **UCX/RDMA 멀티노드** — 분산 메모리 풀 연결
- [ ] **Graph 인덱스 (CSR)** — 자금 이동/인맥 추적
- [ ] **FlatBuffers AST 직렬화** — Python DSL → C++ 전달 최적화
- [ ] **Apache Arrow C Data Interface** — 외부 도구 연동 (Pandas, DuckDB 등)

## 완료
- [x] Phase E — End-to-End Pipeline MVP
- [x] Phase B — Highway SIMD + LLVM JIT
- [x] Phase B v2 — BitMask filter (11x), JIT O3 (2.6x)
- [x] Phase A — HDB Tiered Storage + LZ4
- [ ] Phase D — Python Bridge (진행 중)
- [ ] Phase C — Distributed Memory (예정)
