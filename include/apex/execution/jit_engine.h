#pragma once
// ============================================================================
// Layer 3: LLVM JIT Engine
// ============================================================================
// 간단한 필터 표현식을 LLVM으로 JIT 컴파일
// 지원 문법:
//   "price > 100"
//   "volume * 10 > 5000"
//   "price > 100 AND volume > 50"
//   "price > 100 OR volume > 50"
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
// #include <expected>  // C++23, 아직 사용 안 함

namespace apex::execution {

// 컴파일된 필터 함수 타입
// params: price, volume (int64_t)
using FilterFn = bool(*)(int64_t, int64_t);

// ============================================================================
// AST 노드 (간단한 재귀하강 파서용)
// ============================================================================
enum class ASTNodeKind {
    COMPARE,   // col op const  또는  col * const op const
    AND,
    OR,
};

enum class CmpOp { GT, GE, LT, LE, EQ, NE };

enum class ColId { PRICE = 0, VOLUME = 1 };

struct ASTNode {
    ASTNodeKind kind;

    // COMPARE 노드
    ColId     col{ColId::PRICE};
    bool      has_multiplier{false};
    int64_t   multiplier{1};
    CmpOp     op{CmpOp::GT};
    int64_t   rhs{0};

    // AND/OR 노드
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
};

// ============================================================================
// JITEngine: LLVM OrcJIT 기반 필터 컴파일러
// ============================================================================
class JITEngine {
public:
    JITEngine();
    ~JITEngine();

    // 이동 가능, 복사 불가
    JITEngine(JITEngine&&) noexcept;
    JITEngine& operator=(JITEngine&&) noexcept;
    JITEngine(const JITEngine&) = delete;
    JITEngine& operator=(const JITEngine&) = delete;

    // 초기화 (LLVM 셋업)
    bool initialize();

    // 표현식 → 함수 포인터로 컴파일
    // 실패 시 nullptr 반환
    FilterFn compile(const std::string& expr);

    // 컴파일된 함수를 column data에 적용
    // 반환: 통과한 행 인덱스 목록
    std::vector<uint32_t> apply(
        FilterFn fn,
        const int64_t* prices,
        const int64_t* volumes,
        size_t num_rows
    );

    // 마지막 에러 메시지
    const std::string& last_error() const { return last_error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string last_error_;

    // 표현식 파서
    std::unique_ptr<ASTNode> parse(const std::string& expr, size_t& pos);
    std::unique_ptr<ASTNode> parse_or(const std::string& expr, size_t& pos);
    std::unique_ptr<ASTNode> parse_and(const std::string& expr, size_t& pos);
    std::unique_ptr<ASTNode> parse_compare(const std::string& expr, size_t& pos);

    void skip_ws(const std::string& expr, size_t& pos);
    std::string parse_token(const std::string& expr, size_t& pos);
    int64_t parse_int(const std::string& expr, size_t& pos);
};

}  // namespace apex::execution
