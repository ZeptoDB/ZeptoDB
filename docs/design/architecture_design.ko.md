# 차세대 HFT 인메모리 DB: 코어 엔진 및 아키텍처 설계

> `initial_doc.md`의 전략적 목표(금융/HFT 특화, 메모리 분리, 초저지연, C++)를 바탕으로, 기존 KDB+의 한계를 극복하고 연구(Research)와 운용(Production)을 통합하기 위한 구체적인 아키텍처 설계안입니다.

---

## 1. 코어 엔진 아키텍처 (Core Engine Architecture)

KDB+의 '극단적인 단순함'과 '데이터 지역성(Data Locality)' 철학은 계승하되, 운영체제(OS)와 단일 노드의 물리적 한계를 최신 클라우드 및 하드웨어 기술로 돌파합니다.

### 1-A. Storage & Memory Manager (글로벌 메모리 풀링)
- **한계 극복:** 기존 DB(kdb+ 등)는 OS의 가상 메모리(`mmap`)에 의존하여 페이지 폴트로 인한 치명적인 Tail Latency가 존재함.
- **설계 요주:**
  - **Memory Disaggregation:** CXL 3.0과 RDMA(AWS EFA/SRD)를 통해 로컬 RAM과 여러 원격 서버의 RAM을 묶어 하나의 **논리적 공유 메모리 공간(Global Shared Memory Pool)**으로 구성 (커널 우회, Kernel Bypass).
  - **Columnar Layout:** CPU 캐시 라인 및 SIMD 레지스터 폭에 완벽히 정렬(Padding)된 순수 컬럼형 스토리지 구조 채택. 캐시 미스 극소화.

### 1-B. Ingestion & Network Layer (초저지연 데이터 수집)
- **설계 요주:**
  - **Zero-Copy RDMA Write:** 거래소 시장 데이터(Tick)가 NIC(Network Interface Card)에 도달하는 즉시 CPU 개입 없이 분산 메모리 공간에 직접 쓰여짐.
  - **Lock-Free Queue:** 다중 생산자-다중 소비자(MPMC) 기반의 Ring Buffer를 C++20/Rust로 구현하여 컨텍스트 스위칭 없는 극저지연 트랜잭션 수집(Ingestion) 아키텍처 구축.

### 1-C. Query & Execution Engine (벡터화 및 연산 가속)
- **설계 요주:**
  - **Vectorized Execution:** C++ 템플릿 메타프로그래밍으로 쿼리를 기계어로 파이프라이닝. AWS Graviton4의 ARM SVE나 x86 AVX-512 등의 SIMD 명령어 내재화.
  - **Hardware Offloading:** VWAP, 옵션 트레이딩 그릭스(Greeks) 계산, 실시간 이상거래탐지(FDS)와 같은 무거운 수학 연산은 CXL로 연결된 **FPGA/GPU 가속기로 오프로딩**하여 메인 CPU 해방.

### 1-D. Multi-Model Data Structures (다중 모델 자료구조)
- **Time-Series (시계열):** 빠른 쓰기 속도를 보장하기 위해 Timestamp 기반 Append-Only 컬럼 모음 활용.
- **Graph (그래프):** 자금 이동 및 인맥 추적을 위한 희소 행렬 압축 구조(CSR). 시계열 컬럼 내 인덱스의 포인터 오프셋(Pointer Offset)만을 이용해 포인터 체이싱 지연을 원천 차단하는 하이브리드 연결.

---

## 2. 🌟 특화 비전: Research to Production 통합 브릿지 아키텍처

> 업계 최대의 병목인 **"Python(퀀트 연구) -> C++(실거래 배포) 번역 작업의 낭비"**를 완벽하게 제거합니다. 단일 기술 스택으로 Time-to-Market을 극단적으로 단축하는 것이 핵심 경쟁력입니다.

### 2-A. 지연 평가(Lazy Evaluation) 기반의 DSL
- 퀀트 연구원은 자신이 직관적으로 다루는 Pandas/Polars 스타일의 API(`db.filter(price > 100).rolling(1m).vwap()`)로 Python 코드를 작성합니다.
- Python 엔진은 코드를 즉시 계산하지 않고, 인터페이스로서의 역할만 하여 내부적으로 데이터 조작의 **추상 구문 트리(AST, 실행 계획)**를 작성합니다.

### 2-B. 런타임 JIT 컴파일 (LLVM 코어 활용)
- 실행 시점(Run-time)에 Python AST가 내부의 C++ 코어로 넘어가면, 탑재된 **LLVM 런타임 JIT 컴파일러**가 이 로직을 즉석에서 초고속 C++ 머신 코드로 번역 및 최적화(핫스팟 브랜치 제거 등)합니다.
- 이 단계에서 Python이 근본적으로 지원하지 못하는 SIMD, 하드웨어 오프로딩 명령이 주입됩니다.

### 2-C. 완벽한 Zero-Copy 연동 (pybind11 활용)
- Python 연구 환경이 C++의 데이터를 읽고 분석할 때 직렬화/역직렬화(Serialization/Deserialization) 비용을 내지 않도록, 메모리 레이아웃을 100% 일치(Apache Arrow 스타일) 시킵니다.
- 데이터 값 복사 없이 메모리 포인터 접근 권한만 스위칭하여, 테라바이트(TB) 단위의 데이터 앞에서도 대기 시간이 발생하지 않습니다.

---

## 3. 요약 및 권장 첫번째 마일스톤

이 아키텍처는 퀀트 개발자가 익숙한 Python으로 짠 스크립트가 단 한 줄의 손수 번역 없이 **Ultra-Low Latency C++** 레벨의 성능으로 HFT에 즉시 배포될 수 있게 합니다.

**🚀 추천하는 MVP (최소 기능 제품) 개발 단계:**
1. C++20/Rust를 이용한 **Lock-Free 기반 MPMC Ring Buffer** (수집 큐) 기초 뼈대 생성.
2. 데이터 메모리를 Apache Arrow 형식으로 할당 및 유지하는 **Custom Allocator 모듈** 설계.
3. 이를 Python에서 다이렉트로 읽을 수 있도록 **Pybind11 기반 Zero-copy Read API** 바인딩 구성.
