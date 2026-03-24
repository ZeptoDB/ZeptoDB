# ZeptoDB Claude Code Configuration

## 🌐 Language Policy
**ALL documentation, comments, commit messages, and written content must be in English.**
- Source code comments: English only
- Documentation (*.md): English only
- Commit messages: English only
- Korean translations are stored separately with `_ko` suffix (e.g., `README_ko.md`)
- This rule applies to all future contributions

> This file defines the rules and workflow that Claude Code must follow when working on the ZeptoDB project.

---

## 🚨 Core Principle: Document-Code Synchronization

**Every code change must be accompanied by relevant documentation updates.**

Documentation-code inconsistency undermines project credibility. Architecture decisions, design changes, and performance improvements must be reflected in documentation.

---

## 📁 Documentation Structure and Mapping

### docs/design/ (Design Documents)
Core documents covering project architecture and design decisions.

| Document | Responsibility | Related Code |
|----------|---------------|--------------|
| `architecture_design.md` | Overall architecture, core engine design | Entire project |
| `initial_doc.md` | Project vision, goals, strategy | - |
| `system_requirements.md` | Functional/non-functional requirements | Entire project |
| `layer1_storage_memory.md` | Storage & Memory design | `include/zeptodb/storage/`, `src/storage/` |
| `layer2_ingestion_network.md` | Ingestion & Network design | `include/zeptodb/ingestion/`, `src/ingestion/` |
| `layer3_execution_engine.md` | Execution Engine design | `include/zeptodb/execution/`, `src/execution/` |
| `layer4_transpiler_client.md` | SQL + Python client | `include/zeptodb/sql/`, `include/zeptodb/transpiler/`, `src/sql/`, `src/transpiler/` |
| `phase_c_distributed.md` | Distributed cluster design | `include/zeptodb/cluster/`, `src/cluster/` |
| `feature_performance_analysis.md` | Performance analysis and optimization strategy | Entire project |
| `kdb_replacement_analysis.md` | kdb+ replacement analysis | - |
| `high_level_architecture.md` | High-level architecture diagram | Entire project |

### docs/devlog/ (Development Log)
Sequential history of the development process. Records implementation details, benchmark results, and lessons learned for each Phase.

- `000_environment_setup.md` ~ `011_parallel_query.md`
- Create a new devlog file when adding new features or optimizations

### docs/bench/ (Benchmark Results)
Records performance benchmark results.

- `results_*.md` : Benchmark results per phase
- `kdb_reference.md` : kdb+ reference comparison

### docs/requirements/ (Requirements)
System requirements specification.

---

## 🔄 Workflow: Documentation Updates on Code Changes

### 1. Before Adding/Changing Features (Planning Phase)

**Always update or write documentation first.**

```
1. Identify which Layer the change belongs to
2. Read the relevant docs/design/ document
3. If a design change is needed, reflect it in the document first
4. Add/update work items in BACKLOG.md
```

### 2. During Implementation (Implementation Phase)

```
1. Write code
2. Write and run tests
3. Run benchmarks (for performance-critical changes)
```

### 3. After Implementation (Documentation Phase)

**The following checklist must be completed before committing.**

#### 📋 Documentation Update Checklist

- [ ] **README.md** update
  - Update Overview/Quick Start section if new features were added
  - Update performance table if metrics changed
  - Reflect if Architecture diagram changed

- [ ] **BACKLOG.md** update
  - Check off or move completed items to "Completed" section
  - Add newly discovered items

- [ ] **docs/design/** update
  - Update the changed Layer's design document (layer1~4)
  - Update `architecture_design.md` if there are new architecture decisions
  - Update `system_requirements.md` if performance targets changed

- [ ] **docs/devlog/** addition
  - Write a new devlog for important feature additions/optimizations (012_*.md)
  - Record implementation details, benchmark results, lessons learned

- [ ] **docs/bench/** update
  - Update results file if benchmarks were run

- [ ] **CMakeLists.txt** synchronization
  - Confirm new files are reflected in the build system

### 4. Commit Message Rules

```
<type>: <brief description>

<detailed description>

Docs updated:
- [x] README.md
- [x] docs/design/layer3_execution_engine.md
- [x] docs/devlog/012_new_feature.md
- [x] BACKLOG.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
```

**Types:**
- `feat`: New feature
- `fix`: Bug fix
- `perf`: Performance improvement
- `refactor`: Refactoring
- `docs`: Documentation only
- `test`: Adding/modifying tests
- `build`: Build system changes

---

## 🎯 Scenario-Specific Guidelines

### Scenario 1: Adding a New Layer/Module

1. Create a new document in `docs/design/` (e.g., `layer5_monitoring.md`)
2. Update `architecture_design.md` and `high_level_architecture.md`
3. Add requirements to `system_requirements.md`
4. Implement code
5. Update the Architecture section of `README.md`

### Scenario 2: Performance Optimization

1. Run and record current benchmarks
2. Implement optimization
3. Run new benchmarks
4. Save results to `docs/bench/`
5. Update performance table in `README.md`
6. Record optimization details in `docs/devlog/`
7. Update `feature_performance_analysis.md`

### Scenario 3: Design Change (Architecture Decision)

1. **First** update the relevant `docs/design/` document
2. Document the reason for the change, alternatives, and trade-offs
3. Record in `architecture_design.md` in ADR (Architecture Decision Record) style
4. Implement code
5. Update README.md

### Scenario 4: API Change

1. Update `layer4_transpiler_client.md` (Python/SQL API)
2. Update Quick Start examples in `README.md`
3. Update test code
4. Record CHANGELOG for breaking changes

---

## 🔍 Claude Code Automatic Checks

### When asked to change code:

1. **Read related documents first**
   - Design document for the relevant Layer
   - Requirements in `system_requirements.md`
   - Recent devlog

2. **Verify design and code consistency**
   - Notify the user if inconsistencies are found
   - Ask if documentation updates are needed

3. **Always propose documentation updates after code changes**
   - "Would you like to reflect this change in the documentation?"
   - Present the list of documents that need updating

### When asked to change documentation:

1. **Verify related code matches documentation**
   - Notify the user if inconsistencies are found
   - Ask if code fixes are needed

---

## 📊 Documentation Quality Standards

### All documents must include:

1. **Last updated date** (YYYY-MM-DD)
2. **Clear title and purpose**
3. **Current implementation status** (planned/in-progress/completed)
4. **Related code paths** (where applicable)
5. **Benchmark results** (for performance-related documents)

### Design documents (docs/design/) additionally require:

1. **Design rationale (Why)** - Why was this approach chosen?
2. **Alternatives and trade-offs** - What other options were considered?
3. **Implementation details** - How is it implemented?
4. **Performance targets** - What are the target metrics?

### Development logs (docs/devlog/) additionally require:

1. **Implementation details** - What was built?
2. **Benchmark results** - What is the performance?
3. **Lessons Learned** - What was learned?
4. **Next steps** - What comes next?

---

## 🚀 Quick Reference

### Frequently updated documents:

1. **BACKLOG.md** - Almost every task
2. **README.md** - New features, performance improvements
3. **system_requirements.md** - Requirement changes
4. **Relevant Layer design document** - Layer changes
5. **New devlog** - On major feature completion

### Documentation update priority:

1. **Required** (must do on code changes):
   - BACKLOG.md
   - Relevant Layer design document
   - README.md (if user-facing)

2. **Recommended** (for significant changes):
   - system_requirements.md
   - New devlog
   - Benchmark results

3. **Optional** (when time permits):
   - architecture_design.md
   - high_level_architecture.md

---

## 📝 Example: Perfect Workflow

```
User: "Implement GROUP BY optimization"

Claude:
1. [READ] docs/design/layer3_execution_engine.md
2. [READ] docs/design/system_requirements.md
3. [READ] BACKLOG.md
4. "GROUP BY optimization is registered as high priority in BACKLOG."
5. "GROUP BY design is currently missing from layer3_execution_engine.md."
6. "I will update the design document first, then implement."
7. [EDIT] docs/design/layer3_execution_engine.md - Add GROUP BY design
8. [IMPLEMENT] Code implementation
9. [TEST] Run tests
10. [BENCHMARK] Run benchmarks
11. [WRITE] docs/devlog/012_groupby_optimization.md
12. [EDIT] docs/bench/results_groupby.md
13. [EDIT] README.md - Update performance table
14. [EDIT] BACKLOG.md - Check off completed item
15. "Documentation and code are both updated. Shall I commit?"
```

---

## ✅ Code Review Checklist

### For every code change, verify:

#### 1. Functional Correctness
- [ ] Requirements fully implemented
- [ ] Edge cases handled (null, empty, boundary values)
- [ ] Error handling appropriate
- [ ] API contract respected (function signatures, return values)

#### 2. Performance (Critical for a high-performance DB!)
- [ ] **Hot path optimization** - Frequently executed code is optimized
- [ ] **Minimal memory allocation** - Arena allocator usage
- [ ] **SIMD utilization** - Highway SIMD optimization opportunities
- [ ] **Unnecessary copies eliminated** - std::move, references used
- [ ] **Loop unrolling** - Compiler hints appropriate
- [ ] **Branch prediction** - likely/unlikely used
- [ ] **Cache locality** - Data structure memory layout

#### 3. Safety & Correctness
- [ ] **Thread safety** - No concurrency issues
- [ ] **Race condition** - Data race check
- [ ] **Memory safety** - No dangling pointers, use-after-free
- [ ] **Integer overflow** - Integer overflow check
- [ ] **Lock-free correctness** - Atomic operations correct

#### 4. Code Quality
- [ ] **Clarity** - Code intent is clear
- [ ] **Consistency** - Project style followed
- [ ] **Comments** - Only complex logic commented (Why, not What)
- [ ] **Function size** - Functions not too large (<50 lines)
- [ ] **Duplication removed** - DRY principle

#### 5. ZeptoDB Specific Checks
- [ ] **Zero-copy** - Zero-copy guaranteed in Python bindings
- [ ] **Column-oriented** - Data structures suitable for column store
- [ ] **LLVM JIT compatible** - JIT compilable
- [ ] **SIMD width** - 256-bit/512-bit SIMD utilized
- [ ] **Allocator usage** - Arena allocator consistency

#### 6. Test Coverage
- [ ] Unit tests written
- [ ] Integration tests checked
- [ ] Python binding tests (on API changes)
- [ ] Benchmarks run (for performance-related changes)

---

## 🧪 Testing Guidelines

### Testing Principles

1. **Fast** - Tests must be fast (total < 30 seconds)
2. **Independent** - Tests are independent of each other
3. **Repeatable** - Always produces the same result
4. **Self-Validating** - Automatically determines pass/fail
5. **Timely** - Written together with code

### C++ Tests (Google Test)

**Location:** `tests/`

**Run:**
```bash
cd build && ninja -j$(nproc)
./tests/zepto_tests
```

**Writing rules:**
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

**Required test scenarios:**
- [ ] **Happy path** - Normal operation
- [ ] **Edge cases** - Boundary values (0, max, -1)
- [ ] **Error cases** - Error conditions
- [ ] **Concurrency** - Concurrent scenarios (where applicable)
- [ ] **Performance** - Performance threshold checks

### Python Tests (pytest)

**Location:** `tests/test_python.py`

**Run:**
```bash
python3 -m pytest ../tests/test_python.py -v
```

**Writing rules:**
```python
# tests/test_python.py
def test_zero_copy_view():
    # Given
    db = zeptodb.Database()
    db.insert([1, 2, 3])

    # When
    arr = db.to_numpy()  # zero-copy

    # Then
    assert arr.base is not None  # view check
    assert not arr.flags['OWNDATA']  # zero-copy check
```

**Python test checks:**
- [ ] Zero-copy behavior verified
- [ ] NumPy/Polars compatibility
- [ ] Type hint accuracy
- [ ] Error message clarity

### Benchmark Tests

**Location:** `bench/`

**Run:**
```bash
cd bench && ./run_benchmarks.sh
```

**Required benchmarks:**
1. **Ingestion** - Data input speed (M rows/sec)
2. **Query** - Query latency (μs)
3. **VWAP** - Financial function performance
4. **Memory** - Memory usage
5. **Python binding** - Zero-copy overhead

**When to run benchmarks:**
- [ ] After performance optimization (required)
- [ ] When hot path changes
- [ ] When SIMD code is added/changed
- [ ] When memory allocation method changes

**Recording results:**
- Save to `docs/bench/results_<feature>.md`
- Compare with previous results (check for regressions)
- Update README.md performance table

### Performance Regression Prevention

**Threshold checks:**
```cpp
// Performance regression test example
TEST(PerformanceTest, FilterNoRegression) {
    auto start = high_resolution_clock::now();
    filter_1m_rows();
    auto duration = duration_cast<microseconds>(end - start).count();

    // Target: < 300μs (kdb+ level)
    EXPECT_LT(duration, 300);
}
```

**When regression is found:**
1. Notify user immediately
2. Analyze root cause
3. Decide to optimize or rollback

### When to Write Tests

**Must write tests:**
- [ ] New feature added
- [ ] Bug fixed (regression prevention)
- [ ] API changed
- [ ] Performance optimized

**Tests can be skipped:**
- Documentation only changes
- Comment only changes
- Build configuration only changes

---

## 🛡️ Absolute Prohibitions

1. **Never change code without updating documentation**
2. **Never change code without tests** (except documentation)
3. **Never merge without checking for performance regressions**
4. **Never leave outdated documentation**
5. **Never discard benchmark results without documenting**
6. **Never record design decisions only in code comments** (document them too)
7. **Never let README performance metrics diverge from reality**
8. **Never add thread-unsafe code to hot paths**
9. **Never ignore memory leaks** (Valgrind check)

---

## 🎓 Additional Resources

- Project goals: `docs/design/initial_doc.md`
- Full architecture: `docs/design/architecture_design.md`
- Requirements: `docs/requirements/system_requirements.md`
- Development history: `docs/devlog/`

---

**This document itself must be updated when the project workflow changes.**

Last updated: 2026-03-22
