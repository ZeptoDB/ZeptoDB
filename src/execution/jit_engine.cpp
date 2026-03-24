// ============================================================================
// Layer 3: LLVM JIT Engine Implementation
// ============================================================================
// LLVM OrcJIT v2 (LLJIT) 기반 동적 필터 컴파일
// 지원: price/volume 컬럼 대상 AND/OR 조합 비교 표현식
//
// Phase B v2 최적화:
//   - compile(): OptimizeForSize → O3 패스 파이프라인으로 교체
//     + alwaysinline 속성으로 인라이닝 강제
//   - compile_bulk(): 루프 포함 IR 직접 생성
//     → (prices*, volumes*, n, out_indices*, out_count*) 벌크 함수
//     → 행당 함수 호출 오버헤드 완전 제거
// ============================================================================

#include "zeptodb/execution/jit_engine.h"
#include "zeptodb/common/logger.h"

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
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Target/TargetMachine.h>

#pragma GCC diagnostic pop

#include <algorithm>
#include <cassert>
#include <cctype>
#include <stdexcept>
#include <atomic>

namespace zeptodb::execution {

// ============================================================================
// JITEngine::Impl — LLVM OrcJIT 상태 보관
// ============================================================================
struct JITEngine::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::atomic<uint32_t> fn_counter{0};  // 함수 이름 고유성 보장
};

// ============================================================================
// O3 최적화 패스 적용 헬퍼 (IRTransformLayer용)
// initialize() 앞에 위치해야 setTransform(apply_o3) 호출 가능
//
// 기존: OptimizeForSize 속성만 → 사실상 최적화 없음
// 변경: PassBuilder + O3 파이프라인 → 인라이닝, DCE, LICM, 루프 언롤 등
// ============================================================================
static llvm::Expected<llvm::orc::ThreadSafeModule>
apply_o3(llvm::orc::ThreadSafeModule tsm,
         llvm::orc::MaterializationResponsibility& /*r*/)
{
    auto err = tsm.withModuleDo([](llvm::Module& mod) -> llvm::Error {
        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;

        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);

        // O3: 공격적 인라이닝, LICM, 벡터화, 루프 언롤 등
        llvm::ModulePassManager mpm =
            pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
        mpm.run(mod, mam);
        return llvm::Error::success();
    });

    if (err) return std::move(err);
    return std::move(tsm);
}

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

    // O3 변환 레이어를 초기화 시점에 한 번만 등록
    // compile()/compile_bulk() 내부에서 setTransform 재호출 금지:
    //   → unique_function이 소유권을 가져가므로 재설정 시 이전 람다 파괴
    impl_->jit->getIRTransformLayer().setTransform(apply_o3);

    ZEPTO_INFO("LLVM OrcJIT 초기화 완료 (O3 최적화 패스 등록)");
    return true;
}

// ============================================================================
// AST → LLVM IR 코드 생성 (per-row 비교 함수)
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
// compile (v1): 표현식 → per-row JIT 함수 포인터 반환
// Phase B v2: alwaysinline + O3 패스 적용 (initialize()에서 등록)
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
    auto mod = std::make_unique<llvm::Module>("zepto_jit_v1", *ctx);

    llvm::Type* i64_ty = llvm::Type::getInt64Ty(*ctx);
    llvm::Type* i1_ty  = llvm::Type::getInt1Ty(*ctx);
    llvm::FunctionType* fn_type = llvm::FunctionType::get(
        i1_ty, {i64_ty, i64_ty}, false
    );

    uint32_t fn_id = impl_->fn_counter.fetch_add(1);
    std::string fn_name = "zepto_filter_" + std::to_string(fn_id);

    llvm::Function* fn = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, fn_name, *mod
    );

    // v2 변경: OptimizeForSize 제거 → alwaysinline + O3 패스로 대체
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    fn->addFnAttr(llvm::Attribute::AlwaysInline);  // 호출자에서 인라이닝 강제

    auto* price_arg  = fn->getArg(0); price_arg->setName("price");
    auto* volume_arg = fn->getArg(1); volume_arg->setName("volume");

    // 3. IR 생성
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

    // 5. LLJIT에 모듈 추가 (initialize()에서 등록한 O3 변환 레이어 통해)
    llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));
    auto err = impl_->jit->addIRModule(std::move(tsm));
    if (err) {
        last_error_ = "모듈 추가 실패: " + llvm::toString(std::move(err));
        return nullptr;
    }

    // 6. 심볼 조회
    auto sym = impl_->jit->lookup(fn_name);
    if (!sym) {
        last_error_ = "심볼 조회 실패: " + llvm::toString(sym.takeError());
        return nullptr;
    }

    return reinterpret_cast<FilterFn>(sym->getValue());
}

// ============================================================================
// compile_bulk (v2): 벌크 필터 함수 IR 생성
//
// 생성 IR 구조:
//   void zepto_bulk_filter_N(
//       const i64* prices, const i64* volumes,
//       i64 n, i32* out_indices, i64* out_count)
//   {
//     i64 cnt = 0
//     for (i64 i = 0; i < n; i++) {
//       i64 price  = prices[i]
//       i64 volume = volumes[i]
//       i1 pass = <AST 코드젠>
//       if (pass) {
//         out_indices[cnt] = (i32)i
//         cnt++
//       }
//     }
//     *out_count = cnt
//   }
//
// 최적화 효과:
//   - 행당 함수 호출 오버헤드 완전 제거 (JIT 호출 시 function call overhead ~5ns/call)
//   - O3 패스: 루프 언롤, LICM(threshold 상수 루프 밖으로 호이스팅), 벡터화
//   - LLVM이 native CPU 타겟으로 최적화 → AVX2/AVX-512 자동 생성 가능
// ============================================================================
BulkFilterFn JITEngine::compile_bulk(const std::string& expr) {
    if (!impl_->jit) {
        last_error_ = "JIT가 초기화되지 않았음. initialize() 먼저 호출 필요";
        return nullptr;
    }

    // 1. AST 파싱
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

    // 2. 모듈 + 타입 정의
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("zepto_jit_bulk", *ctx);

    llvm::Type* void_ty   = llvm::Type::getVoidTy(*ctx);
    llvm::Type* i64_ty    = llvm::Type::getInt64Ty(*ctx);
    llvm::Type* i32_ty    = llvm::Type::getInt32Ty(*ctx);
    llvm::Type* i64ptr_ty = llvm::PointerType::get(i64_ty, 0);
    llvm::Type* i32ptr_ty = llvm::PointerType::get(i32_ty, 0);

    // 벌크 함수 시그니처:
    // void fn(const i64* prices, const i64* volumes, i64 n,
    //         i32* out_indices, i64* out_count)
    llvm::FunctionType* bulk_fn_type = llvm::FunctionType::get(
        void_ty,
        {i64ptr_ty, i64ptr_ty, i64_ty, i32ptr_ty, i64ptr_ty},
        false
    );

    uint32_t fn_id = impl_->fn_counter.fetch_add(1);
    std::string fn_name = "zepto_bulk_filter_" + std::to_string(fn_id);

    llvm::Function* bulk_fn = llvm::Function::Create(
        bulk_fn_type, llvm::Function::ExternalLinkage, fn_name, *mod
    );
    bulk_fn->addFnAttr(llvm::Attribute::NoUnwind);

    // 인자 이름
    auto* arg_prices  = bulk_fn->getArg(0); arg_prices->setName("prices");
    auto* arg_volumes = bulk_fn->getArg(1); arg_volumes->setName("volumes");
    auto* arg_n       = bulk_fn->getArg(2); arg_n->setName("n");
    auto* arg_out_idx = bulk_fn->getArg(3); arg_out_idx->setName("out_indices");
    auto* arg_out_cnt = bulk_fn->getArg(4); arg_out_cnt->setName("out_count");

    // 3. 기본 블록 구성
    // entry → loop_cond → loop_body → store_idx → loop_inc → exit
    auto* bb_entry = llvm::BasicBlock::Create(*ctx, "entry",     bulk_fn);
    auto* bb_cond  = llvm::BasicBlock::Create(*ctx, "loop_cond", bulk_fn);
    auto* bb_body  = llvm::BasicBlock::Create(*ctx, "loop_body", bulk_fn);
    auto* bb_store = llvm::BasicBlock::Create(*ctx, "store_idx", bulk_fn);
    auto* bb_inc   = llvm::BasicBlock::Create(*ctx, "loop_inc",  bulk_fn);
    auto* bb_exit  = llvm::BasicBlock::Create(*ctx, "exit",      bulk_fn);

    llvm::IRBuilder<> b(bb_entry);

    // entry: i = 0, cnt = 0 (alloca)
    auto* i_alloca   = b.CreateAlloca(i64_ty, nullptr, "i");
    auto* cnt_alloca = b.CreateAlloca(i64_ty, nullptr, "cnt");
    b.CreateStore(llvm::ConstantInt::get(i64_ty, 0), i_alloca);
    b.CreateStore(llvm::ConstantInt::get(i64_ty, 0), cnt_alloca);
    b.CreateBr(bb_cond);

    // loop_cond: if i < n → body, else → exit
    b.SetInsertPoint(bb_cond);
    auto* i_val = b.CreateLoad(i64_ty, i_alloca, "i");
    auto* cmp_n = b.CreateICmpSLT(i_val, arg_n, "i_lt_n");
    b.CreateCondBr(cmp_n, bb_body, bb_exit);

    // loop_body: load price/volume, evaluate AST, branch on result
    b.SetInsertPoint(bb_body);
    auto* i_val2    = b.CreateLoad(i64_ty, i_alloca, "i2");
    auto* price_ptr = b.CreateInBoundsGEP(i64_ty, arg_prices,  i_val2, "p_ptr");
    auto* price_val = b.CreateLoad(i64_ty, price_ptr, "price");
    auto* vol_ptr   = b.CreateInBoundsGEP(i64_ty, arg_volumes, i_val2, "v_ptr");
    auto* vol_val   = b.CreateLoad(i64_ty, vol_ptr, "volume");

    // AST 조건 평가
    llvm::Value* pass = codegen_node(ast.get(), b, price_val, vol_val);
    b.CreateCondBr(pass, bb_store, bb_inc);

    // store_idx: out_indices[cnt] = (i32)i; cnt++
    b.SetInsertPoint(bb_store);
    auto* cnt_val = b.CreateLoad(i64_ty, cnt_alloca, "cnt");
    auto* i_i32   = b.CreateTrunc(i_val2, i32_ty, "i_i32");
    auto* out_ptr = b.CreateInBoundsGEP(i32_ty, arg_out_idx, cnt_val, "out_ptr");
    b.CreateStore(i_i32, out_ptr);
    auto* cnt_inc = b.CreateAdd(cnt_val, llvm::ConstantInt::get(i64_ty, 1), "cnt_inc");
    b.CreateStore(cnt_inc, cnt_alloca);
    b.CreateBr(bb_inc);

    // loop_inc: i++
    b.SetInsertPoint(bb_inc);
    auto* i_cur  = b.CreateLoad(i64_ty, i_alloca, "i_cur");
    auto* i_next = b.CreateAdd(i_cur, llvm::ConstantInt::get(i64_ty, 1), "i_next");
    b.CreateStore(i_next, i_alloca);
    b.CreateBr(bb_cond);

    // exit: *out_count = cnt
    b.SetInsertPoint(bb_exit);
    auto* final_cnt = b.CreateLoad(i64_ty, cnt_alloca, "final_cnt");
    b.CreateStore(final_cnt, arg_out_cnt);
    b.CreateRetVoid();

    // 4. 검증
    std::string verify_err;
    llvm::raw_string_ostream err_stream(verify_err);
    if (llvm::verifyFunction(*bulk_fn, &err_stream)) {
        last_error_ = "벌크 IR 검증 실패: " + verify_err;
        return nullptr;
    }

    // 5. LLJIT에 추가 (initialize()에서 등록한 O3 변환 레이어 통해)
    llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));
    auto err = impl_->jit->addIRModule(std::move(tsm));
    if (err) {
        last_error_ = "벌크 모듈 추가 실패: " + llvm::toString(std::move(err));
        return nullptr;
    }

    // 6. 심볼 조회
    auto sym = impl_->jit->lookup(fn_name);
    if (!sym) {
        last_error_ = "벌크 심볼 조회 실패: " + llvm::toString(sym.takeError());
        return nullptr;
    }

    return reinterpret_cast<BulkFilterFn>(sym->getValue());
}

// ============================================================================
// apply (v1): JIT 컴파일된 함수를 column data에 적용 (하위 호환)
// ============================================================================
std::vector<uint32_t> JITEngine::apply(
    FilterFn fn,
    const int64_t* prices,
    const int64_t* volumes,
    size_t num_rows
) {
    std::vector<uint32_t> result;
    result.reserve(num_rows / 4);

    for (size_t i = 0; i < num_rows; ++i) {
        if (fn(prices[i], volumes[i])) {
            result.push_back(static_cast<uint32_t>(i));
        }
    }
    return result;
}

// ============================================================================
// Parser: 재귀하강 파서 (변경 없음)
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
    std::string col_name = parse_token(expr, pos);
    if (col_name.empty())
        throw std::runtime_error("컬럼 이름 없음: pos=" + std::to_string(pos));

    ColId col;
    if (col_name == "price")       col = ColId::PRICE;
    else if (col_name == "volume") col = ColId::VOLUME;
    else throw std::runtime_error("알 수 없는 컬럼: " + col_name);

    skip_ws(expr, pos);

    bool has_mult = false;
    int64_t multiplier = 1;
    if (pos < expr.size() && expr[pos] == '*') {
        ++pos;
        multiplier = parse_int(expr, pos);
        has_mult = true;
    }

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
    node->kind           = ASTNodeKind::COMPARE;
    node->col            = col;
    node->has_multiplier = has_mult;
    node->multiplier     = multiplier;
    node->op             = op;
    node->rhs            = rhs;
    return node;
}

std::unique_ptr<ASTNode> JITEngine::parse_and(const std::string& expr, size_t& pos) {
    auto left = parse_compare(expr, pos);

    while (true) {
        skip_ws(expr, pos);
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

}  // namespace zeptodb::execution
