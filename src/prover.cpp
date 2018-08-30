/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "prover.hpp"
#include "value.hpp"
#include "types.hpp"
#include "error.hpp"
#include "closure.hpp"
#include "stream_ast.hpp"
#include "hash.hpp"
#include "timer.hpp"
#include "gc.hpp"
#include "builtin.hpp"
#include "verify_tools.inc"
#include "dyn_cast.inc"
#include "coro/coro.h"
#include "compiler_flags.hpp"
#include "gen_llvm.hpp"
#include "list.hpp"
#include "expander.hpp"
#include "scopes/scopes.h"

#include <unordered_set>
#include <deque>

#pragma GCC diagnostic ignored "-Wvla-extension"

namespace scopes {

#define SCOPES_ARITH_OPS() \
    IARITH_NUW_NSW_OPS(Add) \
    IARITH_NUW_NSW_OPS(Sub) \
    IARITH_NUW_NSW_OPS(Mul) \
    \
    IARITH_OP(SDiv, i) \
    IARITH_OP(UDiv, u) \
    IARITH_OP(SRem, i) \
    IARITH_OP(URem, u) \
    \
    IARITH_OP(BAnd, u) \
    IARITH_OP(BOr, u) \
    IARITH_OP(BXor, u) \
    \
    IARITH_OP(Shl, u) \
    IARITH_OP(LShr, u) \
    IARITH_OP(AShr, i) \
    \
    FARITH_OP(FAdd) \
    FARITH_OP(FSub) \
    FARITH_OP(FMul) \
    FARITH_OP(FDiv) \
    FARITH_OP(FRem) \
    \
    FUN_OP(FAbs) \
    \
    IUN_OP(SSign, i) \
    FUN_OP(FSign) \
    \
    FUN_OP(Radians) FUN_OP(Degrees) \
    FUN_OP(Sin) FUN_OP(Cos) FUN_OP(Tan) \
    FUN_OP(Asin) FUN_OP(Acos) FUN_OP(Atan) FARITH_OP(Atan2) \
    FUN_OP(Exp) FUN_OP(Log) FUN_OP(Exp2) FUN_OP(Log2) \
    FUN_OP(Trunc) FUN_OP(Floor) FARITH_OP(Step) \
    FARITH_OP(Pow) FUN_OP(Sqrt) FUN_OP(InverseSqrt) \
    \
    FTRI_OP(FMix)

//------------------------------------------------------------------------------

namespace FunctionSet {
    struct Hash {
        std::size_t operator()(const Function *s) const {
            std::size_t h = std::hash<Function *>{}(s->frame);
            h = hash2(h, std::hash<Template *>{}(s->original));
            for (auto arg : s->instance_args) {
                h = hash2(h, std::hash<const Type *>{}(arg));
            }
            return h;
        }
    };

    struct KeyEqual {
        bool operator()( const Function *lhs, const Function *rhs ) const {
            if (lhs->frame != rhs->frame) return false;
            if (lhs->original != rhs->original) return false;
            if (lhs->instance_args.size() != rhs->instance_args.size()) return false;
            for (size_t i = 0; i < lhs->instance_args.size(); ++i) {
                auto lparam = lhs->instance_args[i];
                auto rparam = rhs->instance_args[i];
                if (lparam != rparam) return false;
            }
            return true;
        }
    };
} // namespace FunctionSet

static std::unordered_set<Function *, FunctionSet::Hash, FunctionSet::KeyEqual> functions;

//------------------------------------------------------------------------------

// reduce typekind to compatible
static TypeKind canonical_typekind(TypeKind k) {
    if (k == TK_Real)
        return TK_Integer;
    return k;
}

static SCOPES_RESULT(void) verify_readable(const Type *T) {
    SCOPES_RESULT_TYPE(void);
    auto pi = cast<PointerType>(T);
    if (!pi->is_readable()) {
        StyledString ss;
        ss.out << "can not load value from address of type " << T
            << " because the target is non-readable";
        SCOPES_LOCATION_ERROR(ss.str());
    }
    return true;
}

static SCOPES_RESULT(void) verify_writable(const Type *T) {
    SCOPES_RESULT_TYPE(void);
    auto pi = cast<PointerType>(T);
    if (!pi->is_writable()) {
        StyledString ss;
        ss.out << "can not store value at address of type " << T
            << " because the target is non-writable";
        SCOPES_LOCATION_ERROR(ss.str());
    }
    return true;
}

//------------------------------------------------------------------------------

enum EvalTarget {
    EvalTarget_Void,
    EvalTarget_Symbol,
    EvalTarget_Return,
};

struct ASTContext {
    Function *frame;
    EvalTarget target;
    Loop *loop;
    Try *_try;

    const Type *transform_return_type(const Type *T) const {
        if (is_returning(T) && is_target_void())
            return empty_arguments_type();
        return T;
    }

    bool is_target_void() const {
        return target == EvalTarget_Void;
    }

    ASTContext with_return_target() const { return with_target(EvalTarget_Return); }
    ASTContext with_void_target() const { return with_target(EvalTarget_Void); }
    ASTContext with_symbol_target() const { return with_target(EvalTarget_Symbol); }

    ASTContext with_target(EvalTarget target) const {
        return ASTContext(frame, target, loop, _try);
    }

    ASTContext for_loop(Loop *loop) const {
        return ASTContext(frame, EvalTarget_Symbol, loop, _try);
    }

    ASTContext for_try(Try *_try) const {
        return ASTContext(frame, target, loop, _try);
    }

    ASTContext() {}

    ASTContext(Function *_frame, EvalTarget _target, Loop *_loop, Try *xtry) :
        frame(_frame), target(_target), loop(_loop), _try(xtry) {
    }
};

// returns
static SCOPES_RESULT(Value *) specialize(const ASTContext &ctx, Value *node);

struct SpecializeJob {
    ASTContext ctx;
    Value *node;
    Result<Value *> result;
    coro_stack stack;
    coro_context from;
    coro_context job;
    bool done;
};

static std::deque<SpecializeJob *> jobs;

static int process_jobs() {
    int processed = 0;
    while (!jobs.empty()) {
        auto job = jobs.front();
        jobs.pop_front();
        coro_create(&job->from, nullptr, nullptr, nullptr, 0);
        coro_transfer(&job->from, &job->job);
        processed++;
    }
    return processed;
}

static void specialize_coroutine(void *ptr) {
    SpecializeJob *job = (SpecializeJob *)ptr;
    #if 0
    StyledStream ss;
    ss << "processing: ";
    stream_ast(ss, job->node, StreamASTFormat());
    #endif
    job->result = specialize(job->ctx, job->node);
    job->done = true;
    coro_transfer(&job->job, &job->from);
}

static bool wait_for_return_type(Function *f) {
    // do more branches and try again
    int processed = process_jobs();
    return (f->return_type != nullptr);
}

static SCOPES_RESULT(void) specialize_jobs(const ASTContext &ctx, int count, Value **nodes) {
    SCOPES_RESULT_TYPE(void);
    SpecializeJob local_jobs[count];
    for (int i = 0; i < count; ++i) {
        auto &&job = local_jobs[i];
        job.ctx = ctx;
        job.node = nodes[i];
        job.done = false;
        coro_stack_alloc(&job.stack, 0);
        coro_create(&job.job, specialize_coroutine, &job, job.stack.sptr, job.stack.ssze);
        jobs.push_back(&job);
    }
    process_jobs();
    for (int i = 0; i < count; ++i) {
        assert(local_jobs[i].done);
    }
    for (int i = 0; i < count; ++i) {
        auto &&job = local_jobs[i];
        coro_destroy(&job.job);
        coro_stack_free(&job.stack);
    }
    for (int i = 0; i < count; ++i) {
        auto &&job = local_jobs[i];
        auto result = job.result;
        nodes[i] = SCOPES_GET_RESULT(job.result);
    }
    return true;
}

static SCOPES_RESULT(Value *) specialize_inline(const ASTContext &ctx, Function *frame, Template *func, const Values &nodes);

static SCOPES_RESULT(const Type *) merge_value_type(const ASTContext &ctx, const Type *T1, const Type *T2) {
    SCOPES_RESULT_TYPE(const Type *);
    assert(T2);
    T2 = ctx.transform_return_type(T2);
    if (!T1)
        return T2;
    if (T1 == T2)
        return T1;
    if (!is_returning(T1))
        return T2;
    if (!is_returning(T2))
        return T1;
    SCOPES_EXPECT_ERROR(error_cannot_merge_expression_types(T1, T2));
}

static SCOPES_RESULT(const Type *) merge_return_type(const Type *T1, const Type *T2) {
    SCOPES_RESULT_TYPE(const Type *);
    assert(T2);
    if (!T1)
        return T2;
    if (T1 == T2)
        return T1;
    if (!is_returning(T1))
        return T2;
    if (!is_returning(T2))
        return T1;
    SCOPES_EXPECT_ERROR(error_cannot_merge_expression_types(T1, T2));
}

static bool is_useless(Value *node) {
    if (Const::classof(node)) return true;
    switch(node->kind()) {
    case VK_Template:
    case VK_Function:
    case VK_Symbol:
        return true;
    case VK_Let: {
        auto let = cast<Let>(node);
        if (!let->params.size())
            return true;
    } break;
    default: break;
    }
    return false;
}

static SCOPES_RESULT(Value *) specialize_Block(const ASTContext &ctx, Block *block) {
    SCOPES_RESULT_TYPE(Value *);
    Block *newblock = Block::from(block->anchor());
    auto subctx = ctx.with_void_target();
    for (auto &&src : block->body) {
        auto newsrc = SCOPES_GET_RESULT(specialize(subctx, src));
        if (!is_returning(newsrc->get_type())) {
            SCOPES_ANCHOR(newsrc->anchor());
            SCOPES_CHECK_RESULT(error_noreturn_not_last_expression());
        }
        if (!is_useless(newsrc)) {
            newblock->append(newsrc);
        }
    }
    newblock->value = SCOPES_GET_RESULT(specialize(ctx, block->value));
    auto rtype = ctx.transform_return_type(newblock->value->get_type());
    newblock->set_type(rtype);
    return newblock->canonicalize();
}

static Value *extract_argument(Value *value, int index) {
    const Anchor *anchor = value->anchor();
    const Type *T = value->get_type();
    if (!is_returning(T))
        return value;
    if (is_arguments_type(T)) {
        auto rt = cast<TupleType>(storage_type(T).assert_ok());
        const Type *T = rt->type_at_index_or_nothing(index);
        if (T == TYPE_Nothing) {
            return ConstTuple::none_from(anchor);
        } else {
            auto arglist = dyn_cast<ArgumentList>(value);
            if (arglist) {
                return arglist->values[index];
            } else {
                auto result = ExtractArgument::from(anchor, value, index);
                result->set_type(T);
                return result;
            }
        }
    } else if (index == 0) {
        return value;
    } else {
        return ConstTuple::none_from(anchor);
    }
}

// used by Let, Loop, ArgumentList, Repeat, Call
static SCOPES_RESULT(void) specialize_arguments(
    const ASTContext &ctx, Values &outargs, const Values &values) {
    SCOPES_RESULT_TYPE(void);
    auto subctx = ctx.with_symbol_target();
    int count = (int)values.size();
    for (int i = 0; i < count; ++i) {
        auto value = SCOPES_GET_RESULT(specialize(subctx, values[i]));
        const Type *T = value->get_type();
        if (!is_returning(T)) {
            SCOPES_EXPECT_ERROR(error_noreturn_not_last_expression());
        }
        if (is_arguments_type(T)) {
            auto rt = cast<TupleType>(storage_type(T).assert_ok());
            if ((i + 1) == count) {
                // last argument is appended in full
                int valcount = (int)rt->values.size();
                for (int j = 0; j < valcount; ++j) {
                    outargs.push_back(extract_argument(value, j));
                }
                break;
            } else {
                value = extract_argument(value, 0);
            }
        }
        outargs.push_back(value);
    }
    return true;
}

static const Type *arguments_type_from_arguments(const Values &values) {
    ArgTypes types;
    for (auto arg : values) {
        types.push_back(arg->get_type());
    }
    return arguments_type(types);
}

static Value *build_argument_list(const Anchor *anchor, const Values &values) {
    if (values.size() == 1) {
        return values[0];
    }
    ArgumentList *newnlist = ArgumentList::from(anchor, values);
    newnlist->set_type(arguments_type_from_arguments(values));
    return newnlist;
}

static SCOPES_RESULT(Value *) specialize_ArgumentList(const ASTContext &ctx, ArgumentList *nlist) {
    SCOPES_RESULT_TYPE(Value *);
    Values values;
    SCOPES_CHECK_RESULT(specialize_arguments(ctx, values, nlist->values));
    return build_argument_list(nlist->anchor(), values);
}

static SCOPES_RESULT(Value *) specialize_ExtractArgument(
    const ASTContext &ctx, ExtractArgument *node) {
    SCOPES_RESULT_TYPE(Value *);
    auto value = SCOPES_GET_RESULT(specialize(ctx, node->value));
    return extract_argument(value, node->index);
}

// used by Let, Loop and inlined functions
static SCOPES_RESULT(void) specialize_bind_specialized_arguments(const ASTContext &ctx,
    SymbolValues &outparams, Values &outargs,
    const SymbolValues &params, const Values &tmpargs, bool inline_constants) {
    SCOPES_RESULT_TYPE(void);
    int count = (int)params.size();
    for (int i = 0; i < count; ++i) {
        auto oldsym = params[i];
        Value *newval = nullptr;
        if (oldsym->is_variadic()) {
            if ((i + 1) < count) {
                SCOPES_ANCHOR(oldsym->anchor());
                SCOPES_EXPECT_ERROR(error_variadic_symbol_not_in_last_place());
            }
            if ((i + 1) == (int)tmpargs.size()) {
                newval = tmpargs[i];
            } else {
                auto arglist = ArgumentList::from(oldsym->anchor());
                for (int j = i; j < tmpargs.size(); ++j) {
                    arglist->append(tmpargs[j]);
                }
                arglist->set_type(arguments_type_from_arguments(arglist->values));
                newval = arglist;
            }
        } else if (i < tmpargs.size()) {
            newval = tmpargs[i];
        } else {
            newval = ConstTuple::none_from(oldsym->anchor());
        }
        if (inline_constants && newval->is_symbolic()) {
            ctx.frame->bind(oldsym, newval);
        } else {
            auto newsym = SymbolValue::from(oldsym->anchor(), oldsym->name, newval->get_type());
            ctx.frame->bind(oldsym, newsym);
            outparams.push_back(newsym);
            outargs.push_back(newval);
        }
    }
    return true;
}

static SCOPES_RESULT(void) specialize_bind_arguments(const ASTContext &ctx,
    SymbolValues &outparams, Values &outargs,
    const SymbolValues &params, const Values &values, bool inline_constants) {
    SCOPES_RESULT_TYPE(void);
    Values tmpargs;
    SCOPES_CHECK_RESULT(specialize_arguments(ctx, tmpargs, values));
    return specialize_bind_specialized_arguments(ctx, outparams, outargs,
        params, tmpargs, inline_constants);
}

static SCOPES_RESULT(Value *) specialize_Try(const ASTContext &ctx, Try *_try) {
    SCOPES_RESULT_TYPE(Value *);
    auto try_body = SCOPES_GET_RESULT(specialize(ctx, _try->try_body));

    auto except_body = SCOPES_GET_RESULT(specialize(ctx, _try->except_body));
    assert(false);
}

static SCOPES_RESULT(Value *) specialize_Let(const ASTContext &ctx, Let *let) {
    SCOPES_RESULT_TYPE(Value *);
    SCOPES_ANCHOR(let->anchor());
    Let *newlet = Let::from(let->anchor());
    SCOPES_CHECK_RESULT(specialize_bind_arguments(ctx,
        newlet->params, newlet->args, let->params, let->args, true));
    newlet->set_type(empty_arguments_type());
    return newlet;
}

static SCOPES_RESULT(Loop *) specialize_Loop(const ASTContext &ctx, Loop *loop) {
    SCOPES_RESULT_TYPE(Loop *);
    SCOPES_ANCHOR(loop->anchor());
    Loop *newloop = Loop::from(loop->anchor());
    SCOPES_CHECK_RESULT(specialize_bind_arguments(ctx,
        newloop->params, newloop->args, loop->params, loop->args, false));
    newloop->value = SCOPES_GET_RESULT(specialize(ctx.for_loop(newloop), loop->value));
    auto rtype = newloop->value->get_type();
    newloop->return_type = SCOPES_GET_RESULT(merge_value_type(ctx, newloop->return_type, rtype));
    newloop->set_type(newloop->return_type);
    return newloop;
}

#define CONST_SPECIALIZER(NAME) \
    static SCOPES_RESULT(Value *) specialize_ ## NAME(const ASTContext &ctx, NAME *node) { return node; }

CONST_SPECIALIZER(ConstInt)
CONST_SPECIALIZER(ConstReal)
CONST_SPECIALIZER(ConstPointer)
CONST_SPECIALIZER(ConstTuple)
CONST_SPECIALIZER(ConstArray)
CONST_SPECIALIZER(ConstVector)
CONST_SPECIALIZER(Extern)

const Type *try_get_const_type(Value *node) {
    if (isa<Const>(node))
        return node->get_type();
    return TYPE_Unknown;
}

const String *try_extract_string(Value *node) {
    auto ptr = dyn_cast<ConstPointer>(node);
    if (ptr && (ptr->get_type() == TYPE_String))
        return (const String *)ptr->value;
    return nullptr;
}

static SCOPES_RESULT(Break *) specialize_Break(const ASTContext &ctx, Break *_break) {
    SCOPES_RESULT_TYPE(Break *);
    SCOPES_ANCHOR(_break->anchor());
    if (!ctx.loop) {
        SCOPES_EXPECT_ERROR(error_illegal_break_outside_loop());
    }
    auto subctx = ctx.with_symbol_target();
    Value *value = SCOPES_GET_RESULT(specialize(subctx, _break->value));
    ctx.loop->return_type = SCOPES_GET_RESULT(merge_value_type(subctx, ctx.loop->return_type, value->get_type()));
    auto newbreak = Break::from(_break->anchor(), value);
    newbreak->set_type(TYPE_NoReturn);
    return newbreak;
}

static SCOPES_RESULT(Repeat *) specialize_Repeat(const ASTContext &ctx, Repeat *_repeat) {
    SCOPES_RESULT_TYPE(Repeat *);
    SCOPES_ANCHOR(_repeat->anchor());
    if (!ctx.loop) {
        SCOPES_EXPECT_ERROR(error_illegal_repeat_outside_loop());
    }
    auto newrepeat = Repeat::from(_repeat->anchor());
    SCOPES_CHECK_RESULT(specialize_arguments(ctx, newrepeat->args, _repeat->args));
    newrepeat->set_type(TYPE_NoReturn);
    return newrepeat;
}

static SCOPES_RESULT(Return *) make_return(const ASTContext &ctx, const Anchor *anchor, Value *value) {
    SCOPES_RESULT_TYPE(Return *);
    SCOPES_ANCHOR(anchor);
    auto frame = ctx.frame;
    assert(frame);
    while (frame->original && frame->original->is_inline()) {
        frame = frame->frame;
        assert(frame);
    }
    frame->return_type = SCOPES_GET_RESULT(merge_return_type(frame->return_type, value->get_type()));
    auto newreturn = Return::from(anchor, value);
    newreturn->set_type(TYPE_NoReturn);
    return newreturn;
}

static SCOPES_RESULT(Value *) specialize_Return(const ASTContext &ctx, Return *_return) {
    SCOPES_RESULT_TYPE(Value *);
    if (ctx.frame->original
        && ctx.frame->original->is_inline()) {
        SCOPES_EXPECT_ERROR(error_illegal_return_in_inline());
    }
    Value *value = SCOPES_GET_RESULT(specialize(ctx.with_symbol_target(), _return->value));
    if (ctx.target == EvalTarget_Return) {
        return value;
    }
    return SCOPES_GET_RESULT(make_return(ctx, _return->anchor(), value));
}

static SCOPES_RESULT(Value *) specialize_Raise(const ASTContext &ctx, Raise *_raise) {
    SCOPES_RESULT_TYPE(Value *);
    assert(ctx.frame);
    Value *value = SCOPES_GET_RESULT(specialize(ctx.with_symbol_target(), _raise->value));
    ctx.frame->except_type = SCOPES_GET_RESULT(merge_return_type(ctx.frame->except_type, value->get_type()));
    auto newraise = Raise::from(_raise->anchor(), value);
    newraise->set_type(TYPE_NoReturn);
    return newraise;
}

static SCOPES_RESULT(Value *) specialize_SyntaxExtend(const ASTContext &ctx, SyntaxExtend *sx) {
    SCOPES_RESULT_TYPE(Value *);
    assert(sx->func->scope);
    Function *frame = ctx.frame->find_frame(sx->func->scope);
    if (!frame) {
        SCOPES_ANCHOR(sx->func->anchor());
        SCOPES_EXPECT_ERROR(error_cannot_find_frame(sx->func));
    }
#if 0 //SCOPES_DEBUG_CODEGEN
    StyledStream ss(std::cout);
    std::cout << "syntax-extend non-normalized:" << std::endl;
    stream_ast(ss, sx->func, StreamASTFormat::debug());
    std::cout << std::endl;
#endif
    Function *fn = SCOPES_GET_RESULT(specialize(frame, sx->func, {TYPE_Scope}));
#if 0 //SCOPES_DEBUG_CODEGEN
    std::cout << "syntax-extend normalized:" << std::endl;
    stream_ast(ss, fn, StreamASTFormat());
    std::cout << std::endl;
#endif
    //StyledStream ss;
    //stream_ast(ss, fn, StreamASTFormat());
    auto ftype = native_ro_pointer_type(function_type(TYPE_Scope, {TYPE_Scope}));
    const void *ptr = SCOPES_GET_RESULT(compile(fn, 0/*CF_DumpModule*/))->value;
    Scope *env = nullptr;
    if (fn->get_type() == ftype) {
        typedef Scope *(*SyntaxExtendFuncType)(Scope *);
        SyntaxExtendFuncType fptr = (SyntaxExtendFuncType)ptr;
        env = fptr(sx->env);
        assert(env);
    } else {
        auto ftype2 = native_ro_pointer_type(raising_function_type(TYPE_Scope, {TYPE_Scope}));
        if (fn->get_type() == ftype2) {
            typedef struct { bool ok; Error *err; Scope *scope; } ScopeRet;
            typedef ScopeRet (*SyntaxExtendFuncType)(Scope *);
            SyntaxExtendFuncType fptr = (SyntaxExtendFuncType)ptr;
            auto ret = fptr(sx->env);
            if (ret.ok) {
                env = ret.scope;
                assert(env);
            } else {
                set_last_error(ret.err);
                SCOPES_RETURN_ERROR();
            }
        } else {
            SCOPES_ANCHOR(sx->anchor());
            StyledString ss;
            ss.out << "syntax-extend has wrong return type (expected function of type "
                << ftype
                << " or "
                << ftype2
                << ", got " << fn->get_type() << ")";
            SCOPES_LOCATION_ERROR(ss.str());
        }
    }
    auto anchor = sx->next?sx->next->at->anchor():fn->anchor();
    auto nextfn = SCOPES_GET_RESULT(expand_inline(
        ctx.frame->original,
        ConstPointer::list_from(anchor, sx->next), env));
    return specialize(ctx, nextfn->value);
}

static SCOPES_RESULT(Keyed *) specialize_Keyed(const ASTContext &ctx, Keyed *keyed) {
    SCOPES_RESULT_TYPE(Keyed *);
    return Keyed::from(keyed->anchor(), keyed->key,
        SCOPES_GET_RESULT(specialize(ctx, keyed->value)));
}

template<typename T>
SCOPES_RESULT(T *) extract_constant(Value *value) {
    SCOPES_RESULT_TYPE(T *);
    auto constval = dyn_cast<T>(value);
    if (!constval) {
        SCOPES_ANCHOR(value->anchor());
        SCOPES_CHECK_RESULT(error_constant_expected(value));
    }
    return constval;
}

SCOPES_RESULT(const Type *) extract_type_constant(Value *value) {
    SCOPES_RESULT_TYPE(const Type *);
    ConstPointer* x = SCOPES_GET_RESULT(extract_constant<ConstPointer>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_Type));
    return (const Type *)x->value;
}

SCOPES_RESULT(const Closure *) extract_closure_constant(Value *value) {
    SCOPES_RESULT_TYPE(const Closure *);
    ConstPointer* x = SCOPES_GET_RESULT(extract_constant<ConstPointer>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_Closure));
    return (const Closure *)x->value;
}

SCOPES_RESULT(sc_ast_macro_func_t) extract_astmacro_constant(Value *value) {
    SCOPES_RESULT_TYPE(sc_ast_macro_func_t);
    ConstPointer* x = SCOPES_GET_RESULT(extract_constant<ConstPointer>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_ASTMacro));
    return (sc_ast_macro_func_t)x->value;
}

SCOPES_RESULT(const List *) extract_list_constant(Value *value) {
    SCOPES_RESULT_TYPE(const List *);
    ConstPointer* x = SCOPES_GET_RESULT(extract_constant<ConstPointer>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_List));
    return (const List *)x->value;
}

SCOPES_RESULT(const String *) extract_string_constant(Value *value) {
    SCOPES_RESULT_TYPE(const String *);
    ConstPointer* x = SCOPES_GET_RESULT(extract_constant<ConstPointer>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_String));
    return (const String *)x->value;
}

SCOPES_RESULT(Builtin) extract_builtin_constant(Value *value) {
    SCOPES_RESULT_TYPE(Builtin);
    ConstInt* x = SCOPES_GET_RESULT(extract_constant<ConstInt>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_Builtin));
    return Builtin((KnownSymbol)x->value);
}

SCOPES_RESULT(Symbol) extract_symbol_constant(Value *value) {
    SCOPES_RESULT_TYPE(Symbol);
    ConstInt* x = SCOPES_GET_RESULT(extract_constant<ConstInt>(value));
    SCOPES_ANCHOR(value->anchor());
    SCOPES_CHECK_RESULT(verify(x->get_type(), TYPE_Symbol));
    return Symbol::wrap(x->value);
}

SCOPES_RESULT(uint64_t) extract_integer_constant(Value *value) {
    SCOPES_RESULT_TYPE(uint64_t);
    ConstInt* x = SCOPES_GET_RESULT(extract_constant<ConstInt>(value));
    return x->value;
}

static SCOPES_RESULT(const Type *) bool_op_return_type(const Type *T) {
    SCOPES_RESULT_TYPE(const Type *);
    T = SCOPES_GET_RESULT(storage_type(T));
    if (T->kind() == TK_Vector) {
        auto vi = cast<VectorType>(T);
        return vector_type(TYPE_Bool, vi->count);
    } else {
        return TYPE_Bool;
    }
}

static SCOPES_RESULT(void) verify_integer_ops(const Type *x) {
    SCOPES_RESULT_TYPE(void);
    return verify_integer_vector(SCOPES_GET_RESULT(storage_type(x)));
}

static SCOPES_RESULT(void) verify_real_ops(const Type *x) {
    SCOPES_RESULT_TYPE(void);
    return verify_real_vector(SCOPES_GET_RESULT(storage_type(x)));
}

static SCOPES_RESULT(void) verify_integer_ops(const Type *a, const Type *b) {
    SCOPES_RESULT_TYPE(void);
    SCOPES_CHECK_RESULT(verify_integer_vector(SCOPES_GET_RESULT(storage_type(a))));
    return verify(a, b);
}

static SCOPES_RESULT(void) verify_real_ops(const Type *a, const Type *b) {
    SCOPES_RESULT_TYPE(void);
    SCOPES_CHECK_RESULT(verify_real_vector(SCOPES_GET_RESULT(storage_type(a))));
    return verify(a, b);
}

static SCOPES_RESULT(void) verify_real_ops(const Type *a, const Type *b, const Type *c) {
    SCOPES_RESULT_TYPE(void);
    SCOPES_CHECK_RESULT(verify_real_vector(SCOPES_GET_RESULT(storage_type(a))));
    SCOPES_CHECK_RESULT(verify(a, b));
    return verify(a, c);
}

#define CHECKARGS(MINARGS, MAXARGS) \
    SCOPES_CHECK_RESULT((checkargs<MINARGS, MAXARGS>(argcount)))
#define RETARGTYPES(...) \
    { \
        Call *newcall = Call::from(call->anchor(), callee, values); \
        newcall->set_type(arguments_type({ __VA_ARGS__ })); \
        return newcall; \
    }
#define READ_TYPEOF(NAME) \
        assert(argn < argcount); \
        auto &&_ ## NAME = values[argn++]; \
        const Type *NAME = _ ## NAME->get_type();
#define READ_STORAGETYPEOF(NAME) \
        assert(argn < argcount); \
        auto &&_ ## NAME = values[argn++]; \
        const Type *NAME = SCOPES_GET_RESULT(storage_type(_ ## NAME->get_type()));
#define READ_INT_CONST(NAME) \
        assert(argn < argcount); \
        auto &&_ ## NAME = values[argn++]; \
        auto NAME = SCOPES_GET_RESULT(extract_integer_constant(_ ## NAME));
#define READ_TYPE_CONST(NAME) \
        assert(argn < argcount); \
        auto NAME = SCOPES_GET_RESULT(extract_type_constant(values[argn++]));

static const Type *get_function_type(Function *fn) {
    ArgTypes params;
    for (int i = 0; i < fn->params.size(); ++i) {
        params.push_back(fn->params[i]->get_type());
    }
    return native_ro_pointer_type(raising_function_type(fn->except_type, fn->return_type, params));
}

static SCOPES_RESULT(Value *) specialize_call_interior(const ASTContext &ctx, Call *call) {
    SCOPES_RESULT_TYPE(Value *);
    SCOPES_ANCHOR(call->anchor());
    auto subctx = ctx.with_symbol_target();
    Value *callee = SCOPES_GET_RESULT(specialize(subctx, call->callee));
    Values values;
    SCOPES_CHECK_RESULT(specialize_arguments(ctx, values, call->args));
    const Type *T = callee->get_type();
    if (T == TYPE_Closure) {
        const Closure *cl = SCOPES_GET_RESULT((extract_closure_constant(callee)));
        if (cl->func->is_inline()) {
            return SCOPES_GET_RESULT(specialize_inline(ctx, cl->frame, cl->func, values));
        } else {
            ArgTypes types;
            for (auto &&arg : values) {
                types.push_back(arg->get_type());
            }
            callee = SCOPES_GET_RESULT(specialize(cl->frame, cl->func, types));
            Function *f = cast<Function>(callee);
            if (f->complete) {
                T = callee->get_type();
            } else if (f->return_type) {
                T = get_function_type(f);
            } else {
                if (wait_for_return_type(f)) {
                    T = get_function_type(f);
                } else {
                    SCOPES_ANCHOR(call->anchor());
                    SCOPES_EXPECT_ERROR(error_untyped_recursive_call());
                }
            }
        }
    } else if (T == TYPE_ASTMacro) {
        auto fptr = SCOPES_GET_RESULT(extract_astmacro_constant(callee));
        assert(fptr);
        auto result = fptr(&values[0], values.size());
        if (result.ok) {
            Value *value = result._0;
            assert(value);
            return specialize(ctx, value);
        } else {
            set_last_error(result.except);
            SCOPES_RETURN_ERROR();
        }
    } else if (T == TYPE_Builtin) {
        //SCOPES_CHECK_RESULT(anycl.verify(TYPE_Builtin));
        Builtin b = SCOPES_GET_RESULT(extract_builtin_constant(callee));
        size_t argcount = values.size();
        size_t argn = 0;
        SCOPES_ANCHOR(call->anchor());
        switch(b.value()) {
        case FN_Dump: {
            StyledStream ss(SCOPES_CERR);
            ss << call->anchor() << " dump:";
            for (auto arg : values) {
                ss << " ";
                stream_ast(ss, arg, StreamASTFormat());
            }
            return build_argument_list(call->anchor(), values);
        } break;
        case FN_Undef: {
            CHECKARGS(1, 1);
            READ_TYPE_CONST(T);
            RETARGTYPES(T);
        } break;
        case FN_TypeOf: {
            CHECKARGS(1, 1);
            READ_TYPEOF(A);
            return ConstPointer::type_from(call->anchor(), A);
        } break;
        case OP_Tertiary: {
            CHECKARGS(3, 3);
            READ_STORAGETYPEOF(T1);
            READ_TYPEOF(T2);
            READ_TYPEOF(T3);
            SCOPES_CHECK_RESULT(verify_bool_vector(T1));
            if (T1->kind() == TK_Vector) {
                SCOPES_CHECK_RESULT(verify_vector_sizes(T1, T2));
            }
            SCOPES_CHECK_RESULT(verify(T2, T3));
            RETARGTYPES(T2);
        } break;
        case FN_Bitcast: {
            CHECKARGS(2, 2);
            READ_TYPEOF(SrcT);
            READ_TYPE_CONST(DestT);
            if (SrcT == DestT) {
                return _SrcT;
            } else {
                const Type *SSrcT = SCOPES_GET_RESULT(storage_type(SrcT));
                const Type *SDestT = SCOPES_GET_RESULT(storage_type(DestT));
                if (canonical_typekind(SSrcT->kind())
                        != canonical_typekind(SDestT->kind())) {
                    StyledString ss;
                    ss.out << "can not bitcast value of type " << SrcT
                        << " to type " << DestT
                        << " because storage types are not of compatible category";
                    SCOPES_LOCATION_ERROR(ss.str());
                }
                if (SSrcT != SDestT) {
                    switch (SDestT->kind()) {
                    case TK_Array:
                    //case TK_Vector:
                    case TK_Tuple:
                    case TK_Union: {
                        StyledString ss;
                        ss.out << "can not bitcast value of type " << SrcT
                            << " to type " << DestT
                            << " with aggregate storage type " << SDestT;
                        SCOPES_LOCATION_ERROR(ss.str());
                    } break;
                    default: break;
                    }
                }
                RETARGTYPES(DestT);
            }
        } break;
        case FN_IntToPtr: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_integer(T));
            SCOPES_CHECK_RESULT((verify_kind<TK_Pointer>(SCOPES_GET_RESULT(storage_type(DestT)))));
            RETARGTYPES(DestT);
        } break;
        case FN_PtrToInt: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_kind<TK_Pointer>(T));
            SCOPES_CHECK_RESULT(verify_integer(SCOPES_GET_RESULT(storage_type(DestT))));
            RETARGTYPES(DestT);
        } break;
        case FN_ITrunc: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_integer(T));
            SCOPES_CHECK_RESULT(verify_integer(SCOPES_GET_RESULT(storage_type(DestT))));
            RETARGTYPES(DestT);
        } break;
        case FN_FPTrunc: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_real(T));
            SCOPES_CHECK_RESULT(verify_real(SCOPES_GET_RESULT(storage_type(DestT))));
            if (cast<RealType>(T)->width < cast<RealType>(DestT)->width) {
                SCOPES_EXPECT_ERROR(error_invalid_operands(T, DestT));
            }
            RETARGTYPES(DestT);
        } break;
        case FN_FPExt: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_real(T));
            SCOPES_CHECK_RESULT(verify_real(SCOPES_GET_RESULT(storage_type(DestT))));
            if (cast<RealType>(T)->width > cast<RealType>(DestT)->width) {
                SCOPES_EXPECT_ERROR(error_invalid_operands(T, DestT));
            }
            RETARGTYPES(DestT);
        } break;
        case FN_FPToUI:
        case FN_FPToSI: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_real(T));
            SCOPES_CHECK_RESULT(verify_integer(SCOPES_GET_RESULT(storage_type(DestT))));
            if ((T != TYPE_F32) && (T != TYPE_F64)) {
                SCOPES_EXPECT_ERROR(error_invalid_operands(T, DestT));
            }
            RETARGTYPES(DestT);
        } break;
        case FN_UIToFP:
        case FN_SIToFP: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_integer(T));
            SCOPES_CHECK_RESULT(verify_real(SCOPES_GET_RESULT(storage_type(DestT))));
            if ((DestT != TYPE_F32) && (DestT != TYPE_F64)) {
                SCOPES_CHECK_RESULT(error_invalid_operands(T, DestT));
            }
            RETARGTYPES(DestT);
        } break;
        case FN_ZExt:
        case FN_SExt: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_TYPE_CONST(DestT);
            SCOPES_CHECK_RESULT(verify_integer(T));
            SCOPES_CHECK_RESULT(verify_integer(SCOPES_GET_RESULT(storage_type(DestT))));
            RETARGTYPES(DestT);
        } break;
        case FN_ExtractValue: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(T);
            READ_INT_CONST(idx);
            switch(T->kind()) {
            case TK_Array: {
                auto ai = cast<ArrayType>(T);
                RETARGTYPES(SCOPES_GET_RESULT(ai->type_at_index(idx)));
            } break;
            case TK_Tuple: {
                auto ti = cast<TupleType>(T);
                RETARGTYPES(SCOPES_GET_RESULT(ti->type_at_index(idx)));
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(T);
                RETARGTYPES(SCOPES_GET_RESULT(ui->type_at_index(idx)));
            } break;
            default: {
                StyledString ss;
                ss.out << "can not extract value from type " << T;
                SCOPES_LOCATION_ERROR(ss.str());
            } break;
            }
        } break;
        case FN_InsertValue: {
            CHECKARGS(3, 3);
            READ_TYPEOF(AT);
            READ_STORAGETYPEOF(ET);
            READ_INT_CONST(idx);
            auto T = SCOPES_GET_RESULT(storage_type(AT));
            switch(T->kind()) {
            case TK_Array: {
                auto ai = cast<ArrayType>(T);
                SCOPES_CHECK_RESULT(verify(SCOPES_GET_RESULT(storage_type(SCOPES_GET_RESULT(ai->type_at_index(idx)))), ET));
            } break;
            case TK_Tuple: {
                auto ti = cast<TupleType>(T);
                SCOPES_CHECK_RESULT(verify(SCOPES_GET_RESULT(storage_type(SCOPES_GET_RESULT(ti->type_at_index(idx)))), ET));
            } break;
            case TK_Union: {
                auto ui = cast<UnionType>(T);
                SCOPES_CHECK_RESULT(verify(SCOPES_GET_RESULT(storage_type(SCOPES_GET_RESULT(ui->type_at_index(idx)))), ET));
            } break;
            default: {
                StyledString ss;
                ss.out << "can not insert value into type " << T;
                SCOPES_LOCATION_ERROR(ss.str());
            } break;
            }
            RETARGTYPES(AT);
        } break;
        case FN_GetElementPtr: {
            CHECKARGS(2, -1);
            READ_STORAGETYPEOF(T);
            SCOPES_CHECK_RESULT(verify_kind<TK_Pointer>(T));
            auto pi = cast<PointerType>(T);
            T = pi->element_type;
            READ_STORAGETYPEOF(arg);
            SCOPES_CHECK_RESULT(verify_integer(arg));
            while (argn < argcount) {
                const Type *ST = SCOPES_GET_RESULT(storage_type(T));
                switch(ST->kind()) {
                case TK_Array: {
                    auto ai = cast<ArrayType>(ST);
                    T = ai->element_type;
                    READ_STORAGETYPEOF(arg);
                    SCOPES_CHECK_RESULT(verify_integer(arg));
                } break;
                case TK_Tuple: {
                    auto ti = cast<TupleType>(ST);
                    READ_INT_CONST(arg);
                    if (_arg->get_type() == TYPE_Symbol) {
                        auto sym = Symbol::wrap(arg);
                        size_t idx = ti->field_index(sym);
                        if (idx == (size_t)-1) {
                            StyledString ss;
                            ss.out << "no such field " << sym << " in storage type " << ST;
                            SCOPES_LOCATION_ERROR(ss.str());
                        }
                        // rewrite field
                        arg = idx;
                        _arg = ConstInt::from(_arg->anchor(), TYPE_I32, idx);
                    }
                    T = SCOPES_GET_RESULT(ti->type_at_index(arg));
                } break;
                default: {
                    StyledString ss;
                    ss.out << "can not get element pointer from type " << T;
                    SCOPES_LOCATION_ERROR(ss.str());
                } break;
                }
            }
            T = pointer_type(T, pi->flags, pi->storage_class);
            RETARGTYPES(T);
        } break;
        case FN_VolatileLoad:
        case FN_Load: {
            CHECKARGS(1, 1);
            READ_STORAGETYPEOF(T);
            SCOPES_GET_RESULT(verify_kind<TK_Pointer>(T));
            SCOPES_GET_RESULT(verify_readable(T));
            RETARGTYPES(cast<PointerType>(T)->element_type);
        } break;
        case FN_VolatileStore:
        case FN_Store: {
            CHECKARGS(2, 2);
            READ_STORAGETYPEOF(ElemT);
            READ_STORAGETYPEOF(DestT);
            SCOPES_GET_RESULT(verify_kind<TK_Pointer>(DestT));
            SCOPES_GET_RESULT(verify_writable(DestT));
            auto pi = cast<PointerType>(DestT);
            SCOPES_CHECK_RESULT(
                verify(SCOPES_GET_RESULT(storage_type(pi->element_type)), ElemT));
            RETARGTYPES();
        } break;
        case FN_Alloca: {
            CHECKARGS(1, 1);
            READ_TYPE_CONST(T);
            RETARGTYPES(local_pointer_type(T));
        } break;
        case FN_AllocaArray: {
            CHECKARGS(2, 2);
            READ_TYPE_CONST(T);
            READ_STORAGETYPEOF(size);
            SCOPES_CHECK_RESULT(verify_integer(size));
            RETARGTYPES(local_pointer_type(T));
        } break;
        case FN_Malloc: {
            CHECKARGS(1, 1);
            READ_TYPE_CONST(T);
            RETARGTYPES(native_pointer_type(T));
        } break;
        case FN_MallocArray: {
            CHECKARGS(2, 2);
            READ_TYPE_CONST(T);
            READ_STORAGETYPEOF(size);
            SCOPES_CHECK_RESULT(verify_integer(size));
            RETARGTYPES(native_pointer_type(T));
        } break;
        case FN_Free: {
            CHECKARGS(1, 1);
            READ_STORAGETYPEOF(T);
            SCOPES_CHECK_RESULT(verify_writable(T));
            if (cast<PointerType>(T)->storage_class != SYM_Unnamed) {
                SCOPES_LOCATION_ERROR(String::from(
                    "pointer is not a heap pointer"));
            }
            RETARGTYPES();
        } break;
        case OP_ICmpEQ:
        case OP_ICmpNE:
        case OP_ICmpUGT:
        case OP_ICmpUGE:
        case OP_ICmpULT:
        case OP_ICmpULE:
        case OP_ICmpSGT:
        case OP_ICmpSGE:
        case OP_ICmpSLT:
        case OP_ICmpSLE: {
            CHECKARGS(2, 2);
            READ_TYPEOF(A); READ_TYPEOF(B);
            SCOPES_CHECK_RESULT(verify_integer_ops(A, B));
            RETARGTYPES(SCOPES_GET_RESULT(bool_op_return_type(A)));
        } break;
        case OP_FCmpOEQ:
        case OP_FCmpONE:
        case OP_FCmpORD:
        case OP_FCmpOGT:
        case OP_FCmpOGE:
        case OP_FCmpOLT:
        case OP_FCmpOLE:
        case OP_FCmpUEQ:
        case OP_FCmpUNE:
        case OP_FCmpUNO:
        case OP_FCmpUGT:
        case OP_FCmpUGE:
        case OP_FCmpULT:
        case OP_FCmpULE: {
            CHECKARGS(2, 2);
            READ_TYPEOF(A); READ_TYPEOF(B);
            SCOPES_CHECK_RESULT(verify_real_ops(A, B));
            RETARGTYPES(SCOPES_GET_RESULT(bool_op_return_type(A)));
        } break;
#define IARITH_NUW_NSW_OPS(NAME) \
        case OP_ ## NAME: \
        case OP_ ## NAME ## NUW: \
        case OP_ ## NAME ## NSW: { \
            CHECKARGS(2, 2); \
            READ_TYPEOF(A); READ_TYPEOF(B); \
            SCOPES_CHECK_RESULT(verify_integer_ops(A, B)); \
            RETARGTYPES(A); \
        } break;
#define IARITH_OP(NAME, PFX) \
        case OP_ ## NAME: { \
            CHECKARGS(2, 2); \
            READ_TYPEOF(A); READ_TYPEOF(B); \
            SCOPES_CHECK_RESULT(verify_integer_ops(A, B)); \
            RETARGTYPES(A); \
        } break;
#define FARITH_OP(NAME) \
        case OP_ ## NAME: { \
            CHECKARGS(2, 2); \
            READ_TYPEOF(A); READ_TYPEOF(B); \
            SCOPES_CHECK_RESULT(verify_real_ops(A, B)); \
            RETARGTYPES(A); \
        } break;
#define FTRI_OP(NAME) \
        case OP_ ## NAME: { \
            CHECKARGS(3, 3); \
            READ_TYPEOF(A); READ_TYPEOF(B); READ_TYPEOF(C); \
            SCOPES_CHECK_RESULT(verify_real_ops(A, B, C)); \
            RETARGTYPES(A); \
        } break;
#define IUN_OP(NAME, PFX) \
        case OP_ ## NAME: { \
            CHECKARGS(1, 1); \
            READ_TYPEOF(A); \
            SCOPES_CHECK_RESULT(verify_integer_ops(A)); \
            RETARGTYPES(A); \
        } break;
#define FUN_OP(NAME) \
        case OP_ ## NAME: { \
            CHECKARGS(1, 1); \
            READ_TYPEOF(A); \
            SCOPES_CHECK_RESULT(verify_real_ops(A)); \
            RETARGTYPES(A); \
        } break;
        SCOPES_ARITH_OPS()

#undef IARITH_NUW_NSW_OPS
#undef IARITH_OP
#undef FARITH_OP
#undef IUN_OP
#undef FUN_OP
#undef FTRI_OP
        default: {
            SCOPES_EXPECT_ERROR(error_cannot_type_builtin(b));
        } break;
        }

        return specialize(ctx, ArgumentList::from(call->anchor()));
    }
    if (!is_function_pointer(T)) {
        SCOPES_ANCHOR(call->anchor());
        SCOPES_EXPECT_ERROR(error_invalid_call_type(callee));
    }
    const FunctionType *ft = extract_function_type(T);
    int numargs = (int)ft->argument_types.size();
    if (values.size() != numargs) {
        SCOPES_ANCHOR(call->anchor());
        SCOPES_EXPECT_ERROR(error_argument_count_mismatch(numargs, values.size()));
    }
    // verify_function_argument_signature
    for (int i = 0; i < numargs; ++i) {
        const Type *Ta = values[i]->get_type();
        const Type *Tb = ft->argument_types[i];
        if (Ta == Tb)
            continue;
        Ta = SCOPES_GET_RESULT(storage_type(Ta));
        Tb = SCOPES_GET_RESULT(storage_type(Tb));
        if (isa<PointerType>(Ta) && isa<PointerType>(Tb)) {
            auto pa = cast<PointerType>(Ta);
            auto pb = cast<PointerType>(Tb);
            auto flags = pb->flags;
            auto scls = pb->storage_class;

            if (scls == SYM_Unnamed) {
                scls = pa->storage_class;
            }
            if ((pa->element_type == pb->element_type)
                && pointer_flags_compatible(pb->flags, pa->flags)
                && pointer_storage_classes_compatible(pb->storage_class, pa->storage_class))
                continue;
        }
        SCOPES_ANCHOR(values[i]->anchor());
        SCOPES_EXPECT_ERROR(error_argument_type_mismatch(Tb, Ta));
    }
    const Type *rt = ft->return_type;
    Call *newcall = Call::from(call->anchor(), callee, values);
    newcall->set_type(rt);
    if (ft->has_exception()) {
        assert(ctx.frame);
        ctx.frame->except_type = SCOPES_GET_RESULT(merge_return_type(ctx.frame->except_type, ft->except_type));
    }
    return newcall;
}

static SCOPES_RESULT(Value *) specialize_Call(const ASTContext &ctx, Call *call) {
    SCOPES_RESULT_TYPE(Value *);
    auto result = specialize_call_interior(ctx, call);
    if (result.ok()) {
        return result;
    } else {
        add_error_trace(call);
        SCOPES_RETURN_ERROR();
    }
}

static SCOPES_RESULT(Value *) specialize_SymbolValue(const ASTContext &ctx, SymbolValue *sym) {
    SCOPES_RESULT_TYPE(Value *);
    assert(ctx.frame);
    auto value = ctx.frame->resolve(sym);
    if (!value) {
        SCOPES_EXPECT_ERROR(error_unbound_symbol(sym));
    }
    return value;
}

static SCOPES_RESULT(Value *) specialize_If(const ASTContext &ctx, If *_if) {
    SCOPES_RESULT_TYPE(Value *);
    assert(!_if->clauses.empty());
    auto subctx = ctx.with_symbol_target();
    const Type *rtype = nullptr;
    Clauses clauses;
    Clause else_clause;
    for (auto &&clause : _if->clauses) {
        auto newcond = SCOPES_GET_RESULT(specialize(subctx, clause.cond));
        if (newcond->get_type() != TYPE_Bool) {
            SCOPES_ANCHOR(clause.anchor);
            SCOPES_EXPECT_ERROR(error_invalid_condition_type(newcond));
        }
        auto maybe_const = dyn_cast<ConstInt>(newcond);
        if (maybe_const) {
            bool istrue = maybe_const->value;
            if (istrue) {
                // always true - the remainder will not be evaluated
                else_clause = Clause(clause.anchor, clause.value);
                goto finalize;
            } else {
                // always false - this block will never be evaluated
                continue;
            }
        }
        clauses.push_back(Clause(clause.anchor, newcond, clause.value));
    }
    else_clause = Clause(_if->else_clause.anchor, _if->else_clause.value);
finalize:
    // run a suspendable job for each branch
    int numclauses = clauses.size() + 1;
    Value *values[numclauses];
    for (int i = 0; i < numclauses; ++i) {
        if ((i + 1) == numclauses) {
            values[i] = else_clause.value;
        } else {
            values[i] = clauses[i].value;
        }
    }
    SCOPES_CHECK_RESULT(specialize_jobs(ctx, numclauses, values));
    for (int i = 0; i < numclauses; ++i) {
        SCOPES_ANCHOR(values[i]->anchor());
        #if 0
        if (!values[i]->is_typed()) {
            StyledStream ss;
            ss << "clause untyped: ";
            stream_ast(ss, values[i], StreamASTFormat());
        }
        #endif
        rtype = SCOPES_GET_RESULT(merge_value_type(ctx, rtype, values[i]->get_type()));
        if ((i + 1) == numclauses) {
            else_clause.value = values[i];
        } else {
            clauses[i].value = values[i];
        }
    }
    if (clauses.empty()) {
        // else is always selected
        return else_clause.value;
    }
    If *newif = If::from(_if->anchor(), clauses);
    newif->else_clause = else_clause;
    rtype = ctx.transform_return_type(rtype);
    newif->set_type(rtype);
    return newif;
}

static SCOPES_RESULT(Value *) specialize_Template(const ASTContext &ctx, Template *_template) {
    SCOPES_RESULT_TYPE(Value *);
    assert(_template->scope);
    Function *frame = ctx.frame->find_frame(_template->scope);
    if (!frame) {
        SCOPES_ANCHOR(_template->anchor());
        SCOPES_EXPECT_ERROR(error_cannot_find_frame(_template));
    }
    return ConstPointer::closure_from(_template->anchor(), Closure::from(_template, frame));
}

static SCOPES_RESULT(Function *) specialize_Function(const ASTContext &ctx, Function *fn) {
    SCOPES_RESULT_TYPE(Function *);
    return fn;
}

SCOPES_RESULT(Value *) specialize(const ASTContext &ctx, Value *node) {
    SCOPES_RESULT_TYPE(Value *);
    assert(node);
    Value *result = ctx.frame->resolve(node);
    if (!result && node->is_typed())
        return node;
    if (!result) {
        if (node->is_typed()) {
            result = node;
        } else {
            // we shouldn't set an anchor here because sometimes the parent context
            // is more indicative than the node position
            //SCOPES_CHECK_RESULT(verify_stack());
            switch(node->kind()) {
#define T(NAME, BNAME, CLASS) \
            case NAME: result = SCOPES_GET_RESULT(specialize_ ## CLASS(ctx, cast<CLASS>(node))); break;
            SCOPES_VALUE_KIND()
#undef T
            default: assert(false);
            }
        }
    }
    if (ctx.target == EvalTarget_Return) {
        if (is_returning(result->get_type())) {
            result = SCOPES_GET_RESULT(make_return(ctx, result->anchor(), result));
        }
    }
    assert(result);
    #if 0
    if (node != result)
        ctx.frame->bind(node, result);
    #endif
    return result;
}

SCOPES_RESULT(Value *) specialize_inline(const ASTContext &ctx,
    Function *frame, Template *func, const Values &nodes) {
    SCOPES_RESULT_TYPE(Value *);
    Timer sum_specialize_time(TIMER_Specialize);
    assert(func);
    int count = (int)func->params.size();
    Function *fn = Function::from(func->anchor(), func->name, {}, func->value);
    fn->original = func;
    fn->frame = frame;

    ASTContext subctx(fn, ctx.target, nullptr, nullptr);
    SymbolValues params;
    Values args;
    SCOPES_CHECK_RESULT(specialize_bind_specialized_arguments(subctx,
        params, args, func->params, nodes, true));
    SCOPES_ANCHOR(fn->anchor());
    Value *result_value = fn->value;
    auto result = specialize(subctx, result_value);
    if (result.ok()) {
        result_value = result.assert_ok();
    } else {
        add_error_trace(fn);
        SCOPES_RETURN_ERROR();
    }
    if (!params.empty()) {
        auto rtype = subctx.transform_return_type(result_value->get_type());
        auto let = Let::from(fn->anchor());
        let->params = params;
        let->args = args;
        let->set_type(empty_arguments_type());
        auto block = Block::from(func->anchor(), {let}, result_value);
        block->set_type(rtype);
        fn->value = block->canonicalize();
    } else {
        fn->value = result_value;
    }
    return fn->value;
}

SCOPES_RESULT(Function *) specialize(Function *frame, Template *func, const ArgTypes &types) {
    SCOPES_RESULT_TYPE(Function *);
    Timer sum_specialize_time(TIMER_Specialize);
    assert(func);
    Function key(func->anchor(), func->name, {}, nullptr);
    key.original = func;
    key.frame = frame;
    key.instance_args = types;
    auto it = functions.find(&key);
    if (it != functions.end())
        return *it;
    int count = (int)func->params.size();
    Function *fn = Function::from(func->anchor(), func->name, {}, func->value);
    fn->return_type = TYPE_NoReturn;
    fn->except_type = TYPE_NoReturn;
    fn->original = func;
    fn->frame = frame;
    fn->instance_args = types;
    for (int i = 0; i < count; ++i) {
        auto oldparam = func->params[i];
        if (oldparam->is_variadic()) {
            if ((i + 1) < count) {
                SCOPES_ANCHOR(oldparam->anchor());
                SCOPES_EXPECT_ERROR(error_variadic_symbol_not_in_last_place());
            }
            if ((i + 1) == (int)types.size()) {
                auto newparam = SymbolValue::from(oldparam->anchor(), oldparam->name, types[i]);
                fn->append_param(newparam);
                fn->bind(oldparam, newparam);
            } else {
                ArgTypes vtypes;
                auto args = ArgumentList::from(oldparam->anchor());
                for (int j = i; j < types.size(); ++j) {
                    vtypes.push_back(types[j]);
                    auto newparam = SymbolValue::from(oldparam->anchor(), oldparam->name, types[j]);
                    fn->append_param(newparam);
                    args->values.push_back(newparam);
                }
                args->set_type(arguments_type(vtypes));
                fn->bind(oldparam, args);
            }
        } else {
            const Type *T = TYPE_Nothing;
            if (i < types.size()) {
                T = types[i];
            }
            if (oldparam->is_typed()) {
                SCOPES_ANCHOR(oldparam->anchor());
                SCOPES_CHECK_RESULT(verify(oldparam->get_type(), T));
            }
            auto newparam = SymbolValue::from(oldparam->anchor(), oldparam->name, T);
            fn->append_param(newparam);
            fn->bind(oldparam, newparam);
        }
    }
    functions.insert(fn);

    ASTContext subctx(fn, EvalTarget_Return, nullptr, nullptr);
    SCOPES_ANCHOR(fn->anchor());
    auto result = specialize(subctx, fn->value);
    if (result.ok()) {
        fn->value = result.assert_ok();
    } else {
        add_error_trace(fn);
        SCOPES_RETURN_ERROR();
    }
    assert(!is_returning(fn->value->get_type()));
    fn->complete = true;
    fn->set_type(get_function_type(fn));
    return fn;
}

} // namespace scopes
