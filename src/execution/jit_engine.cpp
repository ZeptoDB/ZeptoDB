// ============================================================================
// Layer 3: LLVM JIT Engine Implementation
// ============================================================================
// LLVM OrcJIT v2 (LLJIT) 기반 동적 필터 컴파일
// 지원: price/volume 컬럼 대상 AND/OR 조합 비교 표현식
// ============================================================================

#include "apex/execution/jit_engine.h"
#include "apex/common/logger.h"

// LLVM 헤더 (경고 억제)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wredundant-move"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/LegacyPassManager.h>

#pragma GCC diagnostic pop

#include <algorithm>
#include <cassert>
#include <cctype>
#include <stdexcept>
#include <atomic>

namespace apex::execution {

// ============================================================================
// JITEngine::Impl — LLVM OrcJIT 상태 보관
// ============================================================================
struct JITEngine::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::atomic<uint32_t> fn_counter{0};  // 함수 이름 고유성 보장
};

// ============================================================================
// 생성자/소멸자
// ============================================================================
JITEngine::JITEngine()
    : impl_(std::make_unique<Impl>())
{}

JITEngine::~JITEngine() = default;

JITEngine::JITEngine(JITEngine&&) noexcept = default;
JITEngine& JITEngine::operator=(JITEngine&&) noexcept = default;

// ============================================================================
// initialize: LLVM 타겟 초기화 + LLJIT 생성
// ============================================================================
bool JITEngine::initialize() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jit_or_err = llvm::orc::LLJITBuilder().create();
    if (!jit_or_err) {
        last_error_ = "LLJIT 생성 실패: " +
            llvm::toString(jit_or_err.takeError());
        return false;
    }

    impl_->jit = std::move(*jit_or_err);
    APEX_INFO("LLVM OrcJIT 초기화 완료");
    return true;
}

// ============================================================================
// AST → LLVM IR 코드 생성
// ============================================================================
static llvm::Value* codegen_node(
    const ASTNode* node,
    llvm::IRBuilder<>& builder,
    llvm::Value* price_arg,
    llvm::Value* volume_arg
) {
    if (node->kind == ASTNodeKind::AND) {
        auto* lhs = codegen_node(node->left.get(),  builder, price_arg, volume_arg);
        auto* rhs = codegen_node(node->right.get(), builder, price_arg, volume_arg);
        return builder.CreateAnd(lhs, rhs, "and");
    }
    if (node->kind == ASTNodeKind::OR) {
        auto* lhs = codegen_node(node->left.get(),  builder, price_arg, volume_arg);
        auto* rhs = codegen_node(node->right.get(), builder, price_arg, volume_arg);
        return builder.CreateOr(lhs, rhs, "or");
    }

    // COMPARE 노드
    llvm::Value* col_val = (node->col == ColId::PRICE) ? price_arg : volume_arg;

    // col * multiplier (필요 시)
    if (node->has_multiplier) {
        auto* mult = llvm::ConstantInt::get(builder.getInt64Ty(), node->multiplier);
        col_val = builder.CreateMul(col_val, mult, "mul");
    }

    auto* rhs_val = llvm::ConstantInt::get(builder.getInt64Ty(), node->rhs);

    switch (node->op) {
        case CmpOp::GT: return builder.CreateICmpSGT(col_val, rhs_val, "cmp");
        case CmpOp::GE: return builder.CreateICmpSGE(col_val, rhs_val, "cmp");
        case CmpOp::LT: return builder.CreateICmpSLT(col_val, rhs_val, "cmp");
        case CmpOp::LE: return builder.CreateICmpSLE(col_val, rhs_val, "cmp");
        case CmpOp::EQ: return builder.CreateICmpEQ (col_val, rhs_val, "cmp");
        case CmpOp::NE: return builder.CreateICmpNE (col_val, rhs_val, "cmp");
    }
    return nullptr;
}

// ============================================================================
// compile: 표현식 → JIT 컴파일 → 함수 포인터 반환
// ============================================================================
FilterFn JITEngine::compile(const std::string& expr) {
    if (!impl_->jit) {
        last_error_ = "JIT가 초기화되지 않았음. initialize() 먼저 호출 필요";
        return nullptr;
    }

    // 1. 표현식 파싱 → AST
    size_t pos = 0;
    std::unique_ptr<ASTNode> ast;
    try {
        ast = parse(expr, pos);
    } catch (const std::exception& e) {
        last_error_ = std::string("파싱 실패: ") + e.what();
        return nullptr;
    }

    if (!ast) {
        last_error_ = "파싱 결과 없음";
        return nullptr;
    }

    // 2. LLVM 모듈 + IR 빌더 생성
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("apex_jit", *ctx);

    // 함수 시그니처: bool filter(int64_t price, int64_t volume)
    llvm::Type* i64_ty  = llvm::Type::getInt64Ty(*ctx);
    llvm::Type* i1_ty   = llvm::Type::getInt1Ty(*ctx);
    llvm::FunctionType* fn_type = llvm::FunctionType::get(
        i1_ty, {i64_ty, i64_ty}, /*isVarArg=*/false
    );

    // 유니크한 함수 이름 생성
    uint32_t fn_id = impl_->fn_counter.fetch_add(1);
    std::string fn_name = "apex_filter_" + std::to_string(fn_id);

    llvm::Function* fn = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, fn_name, *mod
    );
    fn->setAttributes(
        fn->getAttributes()
            .addFnAttribute(*ctx, llvm::Attribute::NoUnwind)
            .addFnAttribute(*ctx, llvm::Attribute::OptimizeForSize)
    );

    // 인자 이름 지정
    auto* price_arg  = fn->getArg(0); price_arg->setName("price");
    auto* volume_arg = fn->getArg(1); volume_arg->setName("volume");

    // 3. 기본 블록 + IR 생성
    llvm::BasicBlock* bb = llvm::BasicBlock::Create(*ctx, "entry", fn);
    llvm::IRBuilder<> builder(bb);

    llvm::Value* result = codegen_node(ast.get(), builder, price_arg, volume_arg);
    builder.CreateRet(result);

    // 4. 검증
    std::string verify_err;
    llvm::raw_string_ostream err_stream(verify_err);
    if (llvm::verifyFunction(*fn, &err_stream)) {
        last_error_ = "IR 검증 실패: " + verify_err;
        return nullptr;
    }

    // 5. LLJIT에 모듈 추가
    llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));
    auto err = impl_->jit->addIRModule(std::move(tsm));
    if (err) {
        last_error_ = "모듈 추가 실패: " + llvm::toString(std::move(err));
        return nullptr;
    }

    // 6. 심볼 조회 → 함수 포인터
    auto sym = impl_->jit->lookup(fn_name);
    if (!sym) {
        last_error_ = "심볼 조회 실패: " + llvm::toString(sym.takeError());
        return nullptr;
    }

    return reinterpret_cast<FilterFn>(sym->getValue());
}

// ============================================================================
// apply: JIT 컴파일된 함수를 column data에 적용
// ============================================================================
std::vector<uint32_t> JITEngine::apply(
    FilterFn fn,
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
) {
    std::vector<uint32_t> result;
    result.reserve(num_rows / 4);  // 예상 selectivity ~25%

    for (size_t i = 0; i < num_rows; ++i) {
        if (fn(prices[i], volumes[i])) {
            result.push_back(static_cast<uint32_t>(i));
        }
    }
    return result;
}

// ============================================================================
// Parser: 재귀하강 파서
// 문법:
//   expr    := or_expr
//   or_expr  := and_expr ('OR' and_expr)*
//   and_expr := compare ('AND' compare)*
//   compare  := TOKEN [('*' INT)] OP INT
//   TOKEN    := 'price' | 'volume'
//   OP       := '>' | '>=' | '<' | '<=' | '==' | '!='
//   INT      := [0-9]+
// ============================================================================

void JITEngine::skip_ws(const std::string& expr, size_t& pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos])))
        ++pos;
}

std::string JITEngine::parse_token(const std::string& expr, size_t& pos) {
    skip_ws(expr, pos);
    size_t start = pos;
    while (pos < expr.size() &&
           (std::isalnum(static_cast<unsigned char>(expr[pos])) || expr[pos] == '_'))
        ++pos;
    return expr.substr(start, pos - start);
}

int64_t JITEngine::parse_int(const std::string& expr, size_t& pos) {
    skip_ws(expr, pos);
    bool negative = false;
    if (pos < expr.size() && expr[pos] == '-') {
        negative = true;
        ++pos;
    }
    size_t start = pos;
    while (pos < expr.size() && std::isdigit(static_cast<unsigned char>(expr[pos])))
        ++pos;
    if (start == pos)
        throw std::runtime_error("숫자 기대: pos=" + std::to_string(pos));
    int64_t val = std::stoll(expr.substr(start, pos - start));
    return negative ? -val : val;
}

std::unique_ptr<ASTNode> JITEngine::parse_compare(const std::string& expr, size_t& pos) {
    // TOKEN
    std::string col_name = parse_token(expr, pos);
    if (col_name.empty())
        throw std::runtime_error("컬럼 이름 없음: pos=" + std::to_string(pos));

    ColId col;
    if (col_name == "price")       col = ColId::PRICE;
    else if (col_name == "volume") col = ColId::VOLUME;
    else throw std::runtime_error("알 수 없는 컬럼: " + col_name);

    skip_ws(expr, pos);

    // 선택적 * multiplier
    bool has_mult = false;
    int64_t multiplier = 1;
    if (pos < expr.size() && expr[pos] == '*') {
        ++pos;
        multiplier = parse_int(expr, pos);
        has_mult = true;
    }

    // 비교 연산자
    skip_ws(expr, pos);
    CmpOp op;
    if (pos + 1 < expr.size() && expr[pos] == '>' && expr[pos+1] == '=') {
        op = CmpOp::GE; pos += 2;
    } else if (pos + 1 < expr.size() && expr[pos] == '<' && expr[pos+1] == '=') {
        op = CmpOp::LE; pos += 2;
    } else if (pos + 1 < expr.size() && expr[pos] == '=' && expr[pos+1] == '=') {
        op = CmpOp::EQ; pos += 2;
    } else if (pos + 1 < expr.size() && expr[pos] == '!' && expr[pos+1] == '=') {
        op = CmpOp::NE; pos += 2;
    } else if (pos < expr.size() && expr[pos] == '>') {
        op = CmpOp::GT; ++pos;
    } else if (pos < expr.size() && expr[pos] == '<') {
        op = CmpOp::LT; ++pos;
    } else {
        throw std::runtime_error("비교 연산자 없음: pos=" + std::to_string(pos));
    }

    int64_t rhs = parse_int(expr, pos);

    auto node = std::make_unique<ASTNode>();
    node->kind            = ASTNodeKind::COMPARE;
    node->col             = col;
    node->has_multiplier  = has_mult;
    node->multiplier      = multiplier;
    node->op              = op;
    node->rhs             = rhs;
    return node;
}

std::unique_ptr<ASTNode> JITEngine::parse_and(const std::string& expr, size_t& pos) {
    auto left = parse_compare(expr, pos);

    while (true) {
        skip_ws(expr, pos);
        // AND 키워드 확인
        if (pos + 3 <= expr.size() &&
            expr.substr(pos, 3) == "AND" &&
            (pos + 3 >= expr.size() || !std::isalnum(static_cast<unsigned char>(expr[pos+3]))))
        {
            pos += 3;
            auto right = parse_compare(expr, pos);
            auto node = std::make_unique<ASTNode>();
            node->kind  = ASTNodeKind::AND;
            node->left  = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
        } else {
            break;
        }
    }
    return left;
}

std::unique_ptr<ASTNode> JITEngine::parse_or(const std::string& expr, size_t& pos) {
    auto left = parse_and(expr, pos);

    while (true) {
        skip_ws(expr, pos);
        if (pos + 2 <= expr.size() &&
            expr.substr(pos, 2) == "OR" &&
            (pos + 2 >= expr.size() || !std::isalnum(static_cast<unsigned char>(expr[pos+2]))))
        {
            pos += 2;
            auto right = parse_and(expr, pos);
            auto node = std::make_unique<ASTNode>();
            node->kind  = ASTNodeKind::OR;
            node->left  = std::move(left);
            node->right = std::move(right);
            left = std::move(node);
        } else {
            break;
        }
    }
    return left;
}

std::unique_ptr<ASTNode> JITEngine::parse(const std::string& expr, size_t& pos) {
    return parse_or(expr, pos);
}

}  // namespace apex::execution
