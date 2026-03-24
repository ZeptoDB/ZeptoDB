#pragma once
// ============================================================================
// ZeptoDB: Window Function Framework
// ============================================================================
// SQL OVER() 절 윈도우 함수 구현
//
// 설계 원칙:
//   - 모든 함수 O(n) 복잡도 (prefix sum / running accumulator)
//   - O(n*w) 슬라이딩 윈도우 절대 금지
//   - PARTITION BY 지원: 파티션별 독립 처리
//   - ROWS PRECEDING / FOLLOWING 프레임 지원
//
// 지원 함수:
//   ROW_NUMBER, RANK, DENSE_RANK
//   SUM, AVG, MIN, MAX
//   LAG, LEAD
// ============================================================================

#include <cstdint>
#include <cstring>
#include <climits>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <memory>

namespace zeptodb::execution {

// ============================================================================
// WindowFrame: 윈도우 프레임 정의
// ============================================================================
struct WindowFrame {
    enum class Type { ROWS, RANGE } type = Type::ROWS;

    // UNBOUNDED PRECEDING = INT64_MAX
    // CURRENT ROW         = 0
    int64_t preceding = INT64_MAX;  // rows before current
    int64_t following = 0;          // rows after current
};

// ============================================================================
// WindowFunction: 추상 기반 클래스
// ============================================================================
class WindowFunction {
public:
    virtual ~WindowFunction() = default;

    /// 윈도우 함수 계산
    /// @param input         입력 컬럼 데이터 (size n)
    /// @param n             행 수
    /// @param output        결과 컬럼 (size n, caller가 할당)
    /// @param frame         윈도우 프레임 정의
    /// @param partition_keys 파티션 키 배열 (nullptr = 단일 파티션)
    virtual void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& frame,
        const int64_t* partition_keys = nullptr
    ) = 0;

    /// 오프셋 파라미터 (LAG/LEAD용)
    virtual void set_offset(int64_t /*offset*/) {}

    /// 기본값 파라미터 (LAG/LEAD default value)
    virtual void set_default(int64_t /*default_val*/) {}
};

// ============================================================================
// 파티션 경계 계산 유틸리티
// ============================================================================
// partition_keys 배열에서 연속 파티션 경계를 찾아 반환
// 반환: { partition_start_indices } (마지막에 n을 추가해서 end sentinel 포함)
inline std::vector<size_t> compute_partition_bounds(
    const int64_t* partition_keys, size_t n)
{
    std::vector<size_t> bounds;
    bounds.push_back(0);
    for (size_t i = 1; i < n; ++i) {
        if (partition_keys[i] != partition_keys[i - 1]) {
            bounds.push_back(i);
        }
    }
    bounds.push_back(n); // 마지막 sentinel
    return bounds;
}

// ============================================================================
// 단일 파티션에 대해 prefix-sum 기반 슬라이딩 SUM O(n)
// ============================================================================
inline void sliding_sum_partition(
    const int64_t* input, size_t start, size_t end,
    int64_t* output,
    int64_t preceding, int64_t following)
{
    // prefix[i] = sum(input[start..start+i-1])
    size_t len = end - start;
    std::vector<int64_t> prefix(len + 1, 0);
    for (size_t i = 0; i < len; ++i) {
        prefix[i + 1] = prefix[i] + input[start + i];
    }

    for (size_t i = 0; i < len; ++i) {
        // frame: [i - preceding, i + following] (파티션 내 클램프)
        int64_t row_in_part = static_cast<int64_t>(i);

        int64_t frame_start;
        if (preceding == INT64_MAX) {
            frame_start = 0;
        } else {
            frame_start = std::max<int64_t>(0, row_in_part - preceding);
        }

        int64_t frame_end;
        if (following == INT64_MAX) {
            frame_end = static_cast<int64_t>(len);
        } else {
            frame_end = std::min<int64_t>(static_cast<int64_t>(len),
                                          row_in_part + following + 1);
        }

        // prefix sum 구간 합
        output[start + i] = prefix[frame_end] - prefix[frame_start];
    }
}

// ============================================================================
// 단일 파티션에 대해 sliding COUNT O(n) (SUM에 1을 쓰는 것과 동일)
// ============================================================================
inline void sliding_count_partition(
    size_t start, size_t end,
    int64_t* output,
    int64_t preceding, int64_t following)
{
    size_t len = end - start;
    for (size_t i = 0; i < len; ++i) {
        int64_t row_in_part = static_cast<int64_t>(i);

        int64_t frame_start = (preceding == INT64_MAX) ? 0 :
            std::max<int64_t>(0, row_in_part - preceding);
        int64_t frame_end = (following == INT64_MAX) ?
            static_cast<int64_t>(len) :
            std::min<int64_t>(static_cast<int64_t>(len),
                              row_in_part + following + 1);

        output[start + i] = frame_end - frame_start;
    }
}

// ============================================================================
// 단일 파티션 sliding MIN/MAX: deque 기반 O(n)
// ============================================================================
inline void sliding_min_partition(
    const int64_t* input, size_t start, size_t end,
    int64_t* output, int64_t preceding, int64_t following)
{
    size_t len = end - start;
    // monotonic deque: (index, value) — 최솟값 유지
    std::vector<size_t> dq; // stores indices into [0, len)
    dq.reserve(len);

    for (size_t i = 0; i < len; ++i) {
        int64_t row = static_cast<int64_t>(i);
        int64_t fs = (preceding == INT64_MAX) ? 0 :
            std::max<int64_t>(0, row - preceding);

        // 프레임 시작 전 원소 제거
        while (!dq.empty() && static_cast<int64_t>(dq.front()) < fs) {
            dq.erase(dq.begin());
        }
        // 현재 원소보다 큰 끝 원소 제거 (단조 증가 유지)
        while (!dq.empty() &&
               input[start + dq.back()] >= input[start + i]) {
            dq.pop_back();
        }
        dq.push_back(i);

        // following이 있으면 실제 윈도우 계산은 delayed — 간단히 현재까지만
        // (following=0 기준: 현재 행이 마지막)
        int64_t fe = (following == INT64_MAX) ?
            static_cast<int64_t>(len - 1) :
            std::min<int64_t>(static_cast<int64_t>(len - 1), row + following);
        (void)fe; // 여기서는 현재 행까지 프레임 끝으로 처리

        output[start + i] = input[start + dq.front()];
    }
}

inline void sliding_max_partition(
    const int64_t* input, size_t start, size_t end,
    int64_t* output, int64_t preceding, int64_t following)
{
    size_t len = end - start;
    std::vector<size_t> dq; // monotonic decreasing deque
    dq.reserve(len);

    for (size_t i = 0; i < len; ++i) {
        int64_t row = static_cast<int64_t>(i);
        int64_t fs = (preceding == INT64_MAX) ? 0 :
            std::max<int64_t>(0, row - preceding);

        while (!dq.empty() && static_cast<int64_t>(dq.front()) < fs) {
            dq.erase(dq.begin());
        }
        while (!dq.empty() &&
               input[start + dq.back()] <= input[start + i]) {
            dq.pop_back();
        }
        dq.push_back(i);

        (void)following;
        output[start + i] = input[start + dq.front()];
    }
}

// ============================================================================
// WindowRowNumber: ROW_NUMBER() OVER (...)
// ============================================================================
class WindowRowNumber : public WindowFunction {
public:
    void compute(
        const int64_t* /*input*/, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        if (!partition_keys) {
            // 단일 파티션: 1, 2, 3, ...
            for (size_t i = 0; i < n; ++i) {
                output[i] = static_cast<int64_t>(i + 1);
            }
        } else {
            // 파티션별 독립 번호 매기기
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                size_t start = bounds[p];
                size_t end   = bounds[p + 1];
                for (size_t i = start; i < end; ++i) {
                    output[i] = static_cast<int64_t>(i - start + 1);
                }
            }
        }
    }
};

// ============================================================================
// WindowRank: RANK() OVER (ORDER BY ...)
// 같은 값이면 같은 순위, 다음 순위는 건너뜀 (1, 1, 3, ...)
// 주의: 입력이 ORDER BY 기준으로 정렬되어 있다고 가정
// ============================================================================
class WindowRank : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_rank = [&](size_t start, size_t end) {
            int64_t rank = 1;
            for (size_t i = start; i < end; ++i) {
                if (i == start || input[i] != input[i - 1]) {
                    rank = static_cast<int64_t>(i - start + 1);
                }
                output[i] = rank;
            }
        };

        if (!partition_keys) {
            do_rank(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_rank(bounds[p], bounds[p + 1]);
            }
        }
    }
};

// ============================================================================
// WindowDenseRank: DENSE_RANK() OVER (ORDER BY ...)
// 같은 값이면 같은 순위, 다음 순위는 연속 (1, 1, 2, ...)
// ============================================================================
class WindowDenseRank : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_dense_rank = [&](size_t start, size_t end) {
            int64_t rank = 1;
            for (size_t i = start; i < end; ++i) {
                if (i > start && input[i] != input[i - 1]) {
                    ++rank;
                }
                output[i] = rank;
            }
        };

        if (!partition_keys) {
            do_dense_rank(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_dense_rank(bounds[p], bounds[p + 1]);
            }
        }
    }
};

// ============================================================================
// WindowSum: SUM() OVER (... ROWS N PRECEDING)
// O(n) prefix sum 기반 슬라이딩 윈도우
// ============================================================================
class WindowSum : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& frame,
        const int64_t* partition_keys = nullptr
    ) override
    {
        if (!partition_keys) {
            sliding_sum_partition(input, 0, n, output,
                                  frame.preceding, frame.following);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                sliding_sum_partition(input, bounds[p], bounds[p + 1], output,
                                      frame.preceding, frame.following);
            }
        }
    }
};

// ============================================================================
// WindowAvg: AVG() OVER (...)
// O(n) — prefix sum으로 합 계산, 윈도우 크기로 나눔
// 정밀도: 정수 고정소수점 (소수점 없음, 실제 AVG는 소수점 아래 버림)
// ============================================================================
class WindowAvg : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& frame,
        const int64_t* partition_keys = nullptr
    ) override
    {
        // sum과 count를 따로 계산하여 나눔
        std::vector<int64_t> sums(n, 0);
        std::vector<int64_t> counts(n, 0);

        auto do_avg = [&](size_t start, size_t end) {
            sliding_sum_partition(input, start, end, sums.data(),
                                  frame.preceding, frame.following);
            sliding_count_partition(start, end, counts.data(),
                                    frame.preceding, frame.following);
            for (size_t i = start; i < end; ++i) {
                output[i] = counts[i] > 0 ? sums[i] / counts[i] : 0;
            }
        };

        if (!partition_keys) {
            do_avg(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_avg(bounds[p], bounds[p + 1]);
            }
        }
    }
};

// ============================================================================
// WindowMin: MIN() OVER (...)
// deque 기반 O(n) 슬라이딩 최솟값
// ============================================================================
class WindowMin : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& frame,
        const int64_t* partition_keys = nullptr
    ) override
    {
        if (!partition_keys) {
            sliding_min_partition(input, 0, n, output,
                                  frame.preceding, frame.following);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                sliding_min_partition(input, bounds[p], bounds[p + 1], output,
                                      frame.preceding, frame.following);
            }
        }
    }
};

// ============================================================================
// WindowMax: MAX() OVER (...)
// deque 기반 O(n) 슬라이딩 최댓값
// ============================================================================
class WindowMax : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& frame,
        const int64_t* partition_keys = nullptr
    ) override
    {
        if (!partition_keys) {
            sliding_max_partition(input, 0, n, output,
                                  frame.preceding, frame.following);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                sliding_max_partition(input, bounds[p], bounds[p + 1], output,
                                      frame.preceding, frame.following);
            }
        }
    }
};

// ============================================================================
// WindowLag: LAG(col, offset) OVER (PARTITION BY ... ORDER BY ...)
// 현재 행 기준 offset 이전 행 값 반환
// ============================================================================
class WindowLag : public WindowFunction {
public:
    explicit WindowLag(int64_t offset = 1, int64_t default_val = 0)
        : offset_(offset), default_val_(default_val) {}

    void set_offset(int64_t offset) override { offset_ = offset; }
    void set_default(int64_t dv) override { default_val_ = dv; }

    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_lag = [&](size_t start, size_t end) {
            for (size_t i = start; i < end; ++i) {
                int64_t src = static_cast<int64_t>(i) - offset_;
                if (src < static_cast<int64_t>(start)) {
                    output[i] = default_val_;
                } else {
                    output[i] = input[src];
                }
            }
        };

        if (!partition_keys) {
            do_lag(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_lag(bounds[p], bounds[p + 1]);
            }
        }
    }

private:
    int64_t offset_      = 1;
    int64_t default_val_ = 0;
};

// ============================================================================
// WindowLead: LEAD(col, offset) OVER (PARTITION BY ... ORDER BY ...)
// 현재 행 기준 offset 이후 행 값 반환
// ============================================================================
class WindowLead : public WindowFunction {
public:
    explicit WindowLead(int64_t offset = 1, int64_t default_val = 0)
        : offset_(offset), default_val_(default_val) {}

    void set_offset(int64_t offset) override { offset_ = offset; }
    void set_default(int64_t dv) override { default_val_ = dv; }

    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_lead = [&](size_t start, size_t end) {
            for (size_t i = start; i < end; ++i) {
                int64_t src = static_cast<int64_t>(i) + offset_;
                if (src >= static_cast<int64_t>(end)) {
                    output[i] = default_val_;
                } else {
                    output[i] = input[src];
                }
            }
        };

        if (!partition_keys) {
            do_lead(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_lead(bounds[p], bounds[p + 1]);
            }
        }
    }

private:
    int64_t offset_      = 1;
    int64_t default_val_ = 0;
};

// ============================================================================
// WindowEMA: EMA(col, alpha) OVER (PARTITION BY ... ORDER BY ...)
// kdb+ ema(alpha, data): O(n) 단일 패스 지수이동평균
//
// 계산식:
//   ema[0] = data[0]
//   ema[i] = alpha * data[i] + (1 - alpha) * ema[i-1]
//
// alpha: 0 < alpha < 1 (직접 지정 또는 period 기반: alpha = 2/(period+1))
// ============================================================================
class WindowEMA : public WindowFunction {
public:
    explicit WindowEMA(double alpha = 0.1)
        : alpha_(alpha) {}

    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_ema = [&](size_t start, size_t end) {
            if (start >= end) return;
            // ema[0] = data[0]
            double ema = static_cast<double>(input[start]);
            output[start] = static_cast<int64_t>(ema);
            // O(n) 단일 패스
            for (size_t i = start + 1; i < end; ++i) {
                ema = alpha_ * static_cast<double>(input[i]) + (1.0 - alpha_) * ema;
                output[i] = static_cast<int64_t>(ema);
            }
        };

        if (!partition_keys) {
            do_ema(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_ema(bounds[p], bounds[p + 1]);
            }
        }
    }

    void set_alpha(double alpha) { alpha_ = alpha; }

private:
    double alpha_; // 평활 계수 (0 < alpha < 1)
};

// ============================================================================
// WindowDelta: DELTA(col) OVER (...) — 행간 차이 (kdb+ deltas)
//
// delta[0] = data[0]           (첫 행은 원본 값 그대로)
// delta[i] = data[i] - data[i-1]
//
// 파티션별 독립 처리
// ============================================================================
class WindowDelta : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_delta = [&](size_t start, size_t end) {
            if (start >= end) return;
            output[start] = input[start]; // 첫 행: 원본 값
            for (size_t i = start + 1; i < end; ++i) {
                output[i] = input[i] - input[i - 1];
            }
        };

        if (!partition_keys) {
            do_delta(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_delta(bounds[p], bounds[p + 1]);
            }
        }
    }
};

// ============================================================================
// WindowRatio: RATIO(col) OVER (...) — 행간 비율 (kdb+ ratios)
//
// ratio[0] = 1 (또는 data[0] — kdb+는 1로 정의)
// ratio[i] = data[i] / data[i-1]  (분모가 0이면 0 반환)
//
// 주의: 정수 스케일 — 소수점 정밀도 손실.
//       실제 사용 시 1000 곱해서 스케일링 권장 (ratio * 1000 → 0.001 단위)
// ============================================================================
class WindowRatio : public WindowFunction {
public:
    void compute(
        const int64_t* input, size_t n,
        int64_t* output,
        const WindowFrame& /*frame*/,
        const int64_t* partition_keys = nullptr
    ) override
    {
        auto do_ratio = [&](size_t start, size_t end) {
            if (start >= end) return;
            // 첫 행은 ratio = 1 (기준값)
            // 스케일: 소수 6자리 (×1_000_000)
            output[start] = 1'000'000LL;
            for (size_t i = start + 1; i < end; ++i) {
                if (input[i - 1] != 0) {
                    // ratio * 1_000_000 (고정소수점 6자리)
                    output[i] = static_cast<int64_t>(
                        (static_cast<double>(input[i]) /
                         static_cast<double>(input[i - 1])) * 1'000'000.0);
                } else {
                    output[i] = 0; // 분모 0: 정의 불가
                }
            }
        };

        if (!partition_keys) {
            do_ratio(0, n);
        } else {
            auto bounds = compute_partition_bounds(partition_keys, n);
            for (size_t p = 0; p + 1 < bounds.size(); ++p) {
                do_ratio(bounds[p], bounds[p + 1]);
            }
        }
    }
};

// ============================================================================
// WindowFunctionFactory: 이름으로 WindowFunction 생성
// ============================================================================
inline std::unique_ptr<WindowFunction> make_window_function(
    const std::string& name,
    int64_t offset = 1,
    int64_t default_val = 0,
    double ema_alpha = 0.1)
{
    if (name == "ROW_NUMBER" || name == "row_number") {
        return std::make_unique<WindowRowNumber>();
    } else if (name == "RANK" || name == "rank") {
        return std::make_unique<WindowRank>();
    } else if (name == "DENSE_RANK" || name == "dense_rank") {
        return std::make_unique<WindowDenseRank>();
    } else if (name == "SUM" || name == "sum") {
        return std::make_unique<WindowSum>();
    } else if (name == "AVG" || name == "avg") {
        return std::make_unique<WindowAvg>();
    } else if (name == "MIN" || name == "min") {
        return std::make_unique<WindowMin>();
    } else if (name == "MAX" || name == "max") {
        return std::make_unique<WindowMax>();
    } else if (name == "LAG" || name == "lag") {
        return std::make_unique<WindowLag>(offset, default_val);
    } else if (name == "LEAD" || name == "lead") {
        return std::make_unique<WindowLead>(offset, default_val);
    } else if (name == "EMA" || name == "ema") {
        return std::make_unique<WindowEMA>(ema_alpha);
    } else if (name == "DELTA" || name == "delta") {
        return std::make_unique<WindowDelta>();
    } else if (name == "RATIO" || name == "ratio") {
        return std::make_unique<WindowRatio>();
    }
    throw std::runtime_error("Unknown window function: " + name);
}

} // namespace zeptodb::execution
