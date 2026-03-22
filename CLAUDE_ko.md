# APEX-DB Claude Code 설정

> 이 파일은 Claude Code가 APEX-DB 프로젝트에서 작업할 때 반드시 따라야 할 규칙과 워크플로우를 정의합니다.

---

## 🚨 핵심 원칙: 문서-코드 동기화 필수

**모든 코드 변경은 반드시 관련 문서 업데이트를 동반해야 합니다.**

문서와 코드가 불일치하면 프로젝트의 신뢰성이 무너집니다. 아키텍처 결정, 설계 변경, 성능 개선은 반드시 문서에 반영되어야 합니다.

---

## 📁 문서 구조 및 매핑

### docs/design/ (설계 문서)
프로젝트의 아키텍처 및 설계 결정을 담은 핵심 문서들입니다.

| 문서 | 담당 영역 | 관련 코드 |
|------|----------|---------|
| `architecture_design.md` | 전체 아키텍처, 코어 엔진 설계 | 전체 프로젝트 |
| `initial_doc.md` | 프로젝트 비전, 목표, 전략 | - |
| `system_requirements.md` | 기능/비기능 요구사항 | 전체 프로젝트 |
| `layer1_storage_memory.md` | Storage & Memory 설계 | `include/apex/storage/`, `src/storage/` |
| `layer2_ingestion_network.md` | Ingestion & Network 설계 | `include/apex/ingestion/`, `src/ingestion/` |
| `layer3_execution_engine.md` | Execution Engine 설계 | `include/apex/execution/`, `src/execution/` |
| `layer4_transpiler_client.md` | SQL + Python 클라이언트 | `include/apex/sql/`, `include/apex/transpiler/`, `src/sql/`, `src/transpiler/` |
| `phase_c_distributed.md` | 분산 클러스터 설계 | `include/apex/cluster/`, `src/cluster/` |
| `feature_performance_analysis.md` | 성능 분석 및 최적화 전략 | 전체 프로젝트 |
| `kdb_replacement_analysis.md` | kdb+ 대체 분석 | - |
| `high_level_architecture.md` | High-level 아키텍처 다이어그램 | 전체 프로젝트 |

### docs/devlog/ (개발 로그)
개발 과정의 순차적 히스토리를 기록합니다. 각 Phase의 구현 내용, 벤치마크 결과, 배운 점을 담습니다.

- `000_environment_setup.md` ~ `011_parallel_query.md`
- 새로운 기능 추가/최적화 시 새 devlog 파일 생성

### docs/bench/ (벤치마크 결과)
성능 벤치마크 결과를 기록합니다.

- `results_*.md` : 각 Phase별 벤치마크 결과
- `kdb_reference.md` : kdb+ 레퍼런스 비교

### docs/requirements/ (요구사항)
시스템 요구사항 명세서입니다.

---

## 🔄 워크플로우: 코드 변경 시 문서 업데이트

### 1. 기능 추가/변경 전 (Planning Phase)

**반드시 먼저 문서를 업데이트하거나 새로 작성합니다.**

```
1. 변경하려는 기능이 어떤 Layer에 속하는지 확인
2. 관련 docs/design/ 문서 읽기
3. 설계 변경이 필요하면 문서에 먼저 반영
4. BACKLOG.md에 작업 항목 추가/업데이트
```

### 2. 구현 중 (Implementation Phase)

```
1. 코드 작성
2. 테스트 작성 및 실행
3. 벤치마크 실행 (성능 critical한 변경인 경우)
```

### 3. 구현 완료 후 (Documentation Phase)

**반드시 아래 체크리스트를 완료해야 커밋 가능합니다.**

#### 📋 문서 업데이트 체크리스트

- [ ] **README.md** 업데이트
  - 새 기능이 추가되었다면 Overview/Quick Start 섹션 업데이트
  - 성능 수치가 변경되었다면 핵심 성능 테이블 업데이트
  - Architecture 다이어그램이 변경되었다면 반영

- [ ] **BACKLOG.md** 업데이트
  - 완료된 작업은 체크 표시 또는 "완료" 섹션으로 이동
  - 새로운 작업이 발견되었다면 추가

- [ ] **docs/design/** 업데이트
  - 변경된 Layer의 설계 문서 업데이트 (layer1~4)
  - 새로운 아키텍처 결정이 있다면 `architecture_design.md` 업데이트
  - 성능 목표가 변경되었다면 `system_requirements.md` 업데이트

- [ ] **docs/devlog/** 추가
  - 중요한 기능 추가/최적화는 새 devlog 작성 (012_*.md)
  - 구현 내용, 벤치마크 결과, 배운 점 기록

- [ ] **docs/bench/** 업데이트
  - 벤치마크를 실행했다면 결과 파일 업데이트

- [ ] **CMakeLists.txt** 동기화
  - 새 파일이 추가되었다면 빌드 시스템에 반영되었는지 확인

### 4. 커밋 메시지 규칙

```
<type>: <간단한 설명>

<상세 설명>

Docs updated:
- [x] README.md
- [x] docs/design/layer3_execution_engine.md
- [x] docs/devlog/012_new_feature.md
- [x] BACKLOG.md

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>
```

**Type 종류:**
- `feat`: 새 기능
- `fix`: 버그 수정
- `perf`: 성능 개선
- `refactor`: 리팩토링
- `docs`: 문서만 변경
- `test`: 테스트 추가/수정
- `build`: 빌드 시스템 변경

---

## 🎯 특수 시나리오별 가이드

### 시나리오 1: 새로운 Layer/모듈 추가

1. `docs/design/` 에 새 문서 생성 (예: `layer5_monitoring.md`)
2. `architecture_design.md` 및 `high_level_architecture.md` 업데이트
3. `system_requirements.md` 에 요구사항 추가
4. 코드 구현
5. `README.md` 의 Architecture 섹션 업데이트

### 시나리오 2: 성능 최적화

1. 현재 벤치마크 실행 및 기록
2. 최적화 구현
3. 새 벤치마크 실행
4. `docs/bench/` 에 결과 저장
5. `README.md` 의 성능 테이블 업데이트
6. `docs/devlog/` 에 최적화 내용 기록
7. `feature_performance_analysis.md` 업데이트

### 시나리오 3: 설계 변경 (Architecture Decision)

1. **반드시 먼저** 관련 `docs/design/` 문서 업데이트
2. 설계 변경 이유, 대안, 트레이드오프 문서화
3. `architecture_design.md` 에 ADR (Architecture Decision Record) 스타일로 기록
4. 코드 구현
5. README.md 업데이트

### 시나리오 4: API 변경

1. `layer4_transpiler_client.md` 업데이트 (Python/SQL API)
2. `README.md` 의 Quick Start 예제 업데이트
3. 테스트 코드 업데이트
4. Breaking change인 경우 CHANGELOG 기록

---

## 🔍 Claude Code 작업 시 자동 체크

### 코드 변경을 요청받았을 때:

1. **먼저 관련 문서를 읽습니다**
   - 해당 Layer의 설계 문서
   - `system_requirements.md` 의 요구사항
   - 최근 devlog

2. **설계와 코드가 일치하는지 확인합니다**
   - 불일치하면 사용자에게 알립니다
   - 문서 업데이트가 필요한지 물어봅니다

3. **코드 변경 후 반드시 문서 업데이트를 제안합니다**
   - "이 변경사항을 문서에 반영하시겠습니까?"
   - 업데이트가 필요한 문서 목록 제시

### 문서 변경을 요청받았을 때:

1. **관련 코드가 문서와 일치하는지 확인합니다**
   - 불일치하면 사용자에게 알립니다
   - 코드 수정이 필요한지 물어봅니다

---

## 📊 문서 품질 기준

### 모든 문서는 다음을 포함해야 합니다:

1. **최종 업데이트 날짜** (YYYY-MM-DD)
2. **명확한 제목과 목적**
3. **현재 구현 상태** (계획/진행중/완료)
4. **관련 코드 경로** (해당하는 경우)
5. **벤치마크 결과** (성능 관련 문서)

### 설계 문서 (docs/design/)는 추가로:

1. **설계 이유 (Why)** - 왜 이 방식을 선택했는가?
2. **대안 및 트레이드오프** - 다른 옵션은 무엇이었는가?
3. **구현 세부사항** - 어떻게 구현하는가?
4. **성능 목표** - 목표 지표는?

### 개발 로그 (docs/devlog/)는 추가로:

1. **구현 내용** - 무엇을 만들었는가?
2. **벤치마크 결과** - 성능은 어떠한가?
3. **배운 점 (Lessons Learned)** - 무엇을 배웠는가?
4. **다음 단계** - 다음은 무엇을 할 것인가?

---

## 🚀 빠른 참조

### 자주 업데이트되는 문서들:

1. **BACKLOG.md** - 거의 매 작업마다
2. **README.md** - 새 기능, 성능 개선 시
3. **system_requirements.md** - 요구사항 변경 시
4. **해당 Layer 설계 문서** - Layer 변경 시
5. **새 devlog** - 중요 기능 완료 시

### 문서 업데이트 우선순위:

1. **필수** (코드 변경 시 반드시):
   - BACKLOG.md
   - 해당 Layer 설계 문서
   - README.md (사용자 영향이 있는 경우)

2. **권장** (중요 변경 시):
   - system_requirements.md
   - 새 devlog
   - 벤치마크 결과

3. **선택** (시간이 있을 때):
   - architecture_design.md
   - high_level_architecture.md

---

## 📝 예시: 완벽한 작업 플로우

```
사용자: "GROUP BY 최적화를 구현해줘"

Claude:
1. [READ] docs/design/layer3_execution_engine.md
2. [READ] docs/design/system_requirements.md
3. [READ] BACKLOG.md
4. "BACKLOG에 'GROUP BY 최적화'가 높은 우선순위로 등록되어 있네요."
5. "현재 layer3_execution_engine.md에 GROUP BY 설계가 누락되어 있습니다."
6. "먼저 설계 문서를 업데이트하고 구현하겠습니다."
7. [EDIT] docs/design/layer3_execution_engine.md - GROUP BY 설계 추가
8. [IMPLEMENT] 코드 구현
9. [TEST] 테스트 실행
10. [BENCHMARK] 벤치마크 실행
11. [WRITE] docs/devlog/012_groupby_optimization.md
12. [EDIT] docs/bench/results_groupby.md
13. [EDIT] README.md - 성능 테이블 업데이트
14. [EDIT] BACKLOG.md - 완료 체크
15. "문서와 코드를 모두 업데이트했습니다. 커밋하시겠습니까?"
```

---

## ✅ 코드 리뷰 체크리스트

### 모든 코드 변경 시 반드시 확인:

#### 1. 기능 정확성
- [ ] 요구사항 구현 완료
- [ ] Edge case 처리 (null, empty, 경계값)
- [ ] 에러 핸들링 적절성
- [ ] API 계약 준수 (함수 시그니처, 반환값)

#### 2. 성능 (고성능 DB이므로 Critical!)
- [ ] **Hot path 최적화** - 자주 실행되는 코드 최적화 확인
- [ ] **메모리 할당 최소화** - Arena allocator 사용 여부
- [ ] **SIMD 활용** - Highway SIMD 최적화 가능 여부
- [ ] **불필요한 복사 제거** - std::move, 참조 사용
- [ ] **Loop unrolling** - 컴파일러 힌트 적절성
- [ ] **Branch prediction** - likely/unlikely 사용
- [ ] **Cache locality** - 데이터 구조 메모리 레이아웃

#### 3. 안전성 & 정확성
- [ ] **Thread safety** - 동시성 이슈 없음
- [ ] **Race condition** - 데이터 레이스 체크
- [ ] **Memory safety** - 댕글링 포인터, use-after-free 없음
- [ ] **Integer overflow** - 정수 오버플로우 체크
- [ ] **Lock-free 정확성** - atomic 연산 올바름

#### 4. 코드 품질
- [ ] **명확성** - 코드 의도가 명확함
- [ ] **일관성** - 프로젝트 스타일 준수
- [ ] **주석** - 복잡한 로직만 주석 (Why, not What)
- [ ] **함수 크기** - 함수가 너무 크지 않음 (<50 lines)
- [ ] **중복 제거** - DRY 원칙

#### 5. APEX-DB 특화 체크
- [ ] **Zero-copy** - Python 바인딩에서 zero-copy 보장
- [ ] **Column-oriented** - 데이터 구조가 column store에 적합
- [ ] **LLVM JIT 호환** - JIT 컴파일 가능 여부
- [ ] **SIMD width** - 256-bit/512-bit SIMD 활용
- [ ] **Allocator 사용** - Arena allocator 일관성

#### 6. 테스트 커버리지
- [ ] 단위 테스트 작성
- [ ] 통합 테스트 필요 여부 확인
- [ ] Python 바인딩 테스트 (API 변경 시)
- [ ] 벤치마크 실행 (성능 관련 변경)

---

## 🧪 테스트 가이드라인

### 테스트 원칙

1. **Fast** - 테스트는 빨라야 함 (전체 < 30초)
2. **Independent** - 테스트 간 독립성 보장
3. **Repeatable** - 항상 같은 결과
4. **Self-Validating** - 자동으로 pass/fail 판단
5. **Timely** - 코드와 함께 작성

### C++ 테스트 (Google Test)

**위치:** `tests/`

**실행:**
```bash
cd build && ninja -j$(nproc)
./tests/apex_tests
```

**작성 규칙:**
```cpp
// tests/test_storage.cpp
TEST(StorageTest, InsertAndRetrieve) {
    // Given
    ColumnStore store;

    // When
    store.insert("symbol", "AAPL");

    // Then
    EXPECT_EQ(store.get("symbol", 0), "AAPL");
}
```

**필수 테스트 시나리오:**
- [ ] **Happy path** - 정상 동작
- [ ] **Edge cases** - 경계값 (0, max, -1)
- [ ] **Error cases** - 에러 조건
- [ ] **Concurrency** - 동시성 시나리오 (해당 시)
- [ ] **Performance** - 성능 임계값 체크

### Python 테스트 (pytest)

**위치:** `tests/test_python.py`

**실행:**
```bash
python3 -m pytest ../tests/test_python.py -v
```

**작성 규칙:**
```python
# tests/test_python.py
def test_zero_copy_view():
    # Given
    db = apex.Database()
    db.insert([1, 2, 3])

    # When
    arr = db.to_numpy()  # zero-copy

    # Then
    assert arr.base is not None  # view 확인
    assert not arr.flags['OWNDATA']  # zero-copy 확인
```

**Python 테스트 체크:**
- [ ] Zero-copy 동작 확인
- [ ] NumPy/Polars 호환성
- [ ] 타입 힌트 정확성
- [ ] 에러 메시지 명확성

### 벤치마크 테스트

**위치:** `bench/`

**실행:**
```bash
cd bench && ./run_benchmarks.sh
```

**필수 벤치마크:**
1. **Ingestion** - 데이터 입력 속도 (M rows/sec)
2. **Query** - 쿼리 지연 (μs)
3. **VWAP** - 금융 함수 성능
4. **Memory** - 메모리 사용량
5. **Python binding** - zero-copy 오버헤드

**벤치마크 실행 시기:**
- [ ] 성능 최적화 후 필수
- [ ] Hot path 변경 시
- [ ] SIMD 코드 추가/변경 시
- [ ] 메모리 할당 방식 변경 시

**결과 기록:**
- `docs/bench/results_<feature>.md` 에 저장
- 이전 결과와 비교 (회귀 없는지 확인)
- README.md 성능 테이블 업데이트

### 성능 회귀 방지

**임계값 체크:**
```cpp
// 성능 회귀 테스트 예시
TEST(PerformanceTest, FilterNoRegression) {
    auto start = high_resolution_clock::now();
    filter_1m_rows();
    auto duration = duration_cast<microseconds>(end - start).count();

    // 목표: < 300μs (kdb+ 수준)
    EXPECT_LT(duration, 300);
}
```

**회귀 발견 시:**
1. 즉시 사용자에게 알림
2. 원인 분석
3. 최적화 또는 롤백 결정

### 테스트 작성 타이밍

**반드시 테스트 작성:**
- [ ] 새 기능 추가
- [ ] 버그 수정 (회귀 방지)
- [ ] API 변경
- [ ] 성능 최적화

**테스트 생략 가능:**
- 문서만 변경
- 주석만 변경
- 빌드 설정만 변경

---

## 🛡️ 절대 금지 사항

1. **문서 없이 코드만 변경하지 않습니다**
2. **테스트 없이 코드 변경하지 않습니다** (문서 외)
3. **성능 회귀를 체크하지 않고 머지하지 않습니다**
4. **오래된 문서를 방치하지 않습니다**
5. **벤치마크 결과를 문서화하지 않고 버리지 않습니다**
6. **설계 결정을 코드 주석에만 남기지 않습니다** (문서에도 기록)
7. **README의 성능 수치가 실제와 다르게 방치하지 않습니다**
8. **Thread-unsafe 코드를 hot path에 추가하지 않습니다**
9. **메모리 릭을 방치하지 않습니다** (Valgrind 체크)

---

## 🎓 추가 리소스

- 프로젝트 목표: `docs/design/initial_doc.md`
- 전체 아키텍처: `docs/design/architecture_design.md`
- 요구사항: `docs/requirements/system_requirements.md`
- 개발 히스토리: `docs/devlog/`

---

**이 문서 자체도 프로젝트 워크플로우가 변경되면 업데이트되어야 합니다.**

Last updated: 2026-03-22
