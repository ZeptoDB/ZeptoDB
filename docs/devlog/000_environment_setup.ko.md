# DevLog #000: 환경 설정 및 의존성 설치

**날짜:** 2026-03-21 (KST 03-22 01:51)
**작성:** 고생이 (AI Dev Assistant)

---

## 1. 호스트 환경 현황

| 항목 | 값 |
|---|---|
| OS | Amazon Linux 2023 (x86_64) |
| Kernel | 6.1.163-186.299.amzn2023 |
| CPU | Intel Xeon 6975P-C, 8 vCPU (4 core, HT) |
| RAM | 30 GiB |
| Disk | 200 GB NVMe (192 GB free) |
| GCC | 11.5.0 (C++20 지원) |
| CMake | 3.22.2 |
| Python | 3.9.25 |
| Git | 2.50.1 |
| Rust | ❌ 미설치 |
| LLVM | ❌ 미설치 |

## 2. 필요 패키지 및 설치 계획

### Phase 1 (즉시 설치 - dnf 패키지)
| 패키지 | 버전 | 용도 |
|---|---|---|
| llvm19-devel | 19.1.7 | JIT 컴파일러 (Layer 3) |
| clang19 | 19.1.7 | C++20 컴파일러 (GCC 11보다 최신 표준 지원 우수) |
| highway-devel | 1.2.0 | SIMD 추상화 (Layer 3) |
| numactl-devel | 2.0.14 | NUMA-aware 메모리 할당 (Layer 1) |
| ucx-devel | 1.12.1 | RDMA/통신 추상화 (Layer 2) |
| boost-devel | 1.75.0 | 유틸리티 (lockfree, program_options 등) |
| ninja-build | - | 빌드 가속 |

### Phase 2 (소스 빌드 - vcpkg/CMake FetchContent)
| 라이브러리 | 용도 |
|---|---|
| Apache Arrow (C++) | 컬럼형 데이터 포맷 (Layer 1, 4) |
| FlatBuffers | AST 직렬화 (Layer 4) |
| nanobind | Python C++ 바인딩 (Layer 4) |
| Google Test | 단위 테스트 |
| spdlog | 로깅 |
| fmt | 포매팅 |

### Phase 3 (추후 필요 시)
| 항목 | 용도 |
|---|---|
| Rust (rustup) | 안전한 패킷 파싱 (Layer 2) |
| DPDK | 커널 바이패스 네트워킹 (Layer 2, 실제 NIC 필요) |

## 3. 설치 결정 근거

- **LLVM 19** 선택: 20은 최신이라 안정성 미검증, 18도 괜찮지만 19가 균형점
- **Clang 19** 선택: GCC 11.5는 C++20 지원이 부분적 (concepts, coroutines 등). Clang 19는 C++20/23 지원이 훨씬 완전
- **Highway** 선택: 문서에서 지정된 SIMD 라이브러리. Amazon Linux에 이미 런타임 있음
- **UCX** 선택: 문서 명시 — 온프레미스/클라우드 통합 통신 레이어
- **Apache Arrow는 소스 빌드**: dnf에 없음. CMake FetchContent로 관리

## 4. 설치 로그

(아래에 실제 설치 출력 기록)

---
