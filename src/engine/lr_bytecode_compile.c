/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: compile
 */
#include "lr_bytecode.h"
#include "lr_bytecode_emit.h"
#include "lr_interp.h"
#include "lr_jit.h"
#include "lr_ast.h"
#include "lr_platform.h"   /* lr_get_time_us() */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#ifdef STR_POOL_DEBUG
#include <malloc.h>
#endif

#ifndef NAN
#define NAN (0.0 / 0.0)
#endif

/* ── Threading mode ──────────────────────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
  #define LR_THREADED_CODE 1
  /* BC_CASE: computed-goto label (the trailing colon comes from call site). */
  #define BC_CASE(lbl, op) lbl_##lbl:
#else
  #define LR_THREADED_CODE 0
  /* BC_CASE: switch case label (the trailing colon comes from call site). */
  #define BC_CASE(lbl, op) case op:
#endif

/* =======================================================================
   COMPILER
   ======================================================================= */

static void cexpr(BCComp *c, ASTNode *n);
static void cstmt(BCComp *c, ASTNode *n, int top);

/* Delegate a subtree to the tree-walking interpreter. */
static void emit_eval(BCComp *c, ASTNode *n, int keep_value)
{
    if (!c->ok) return;
    /* Delegate to the tree-walking interpreter for nodes that cannot be
     * compiled to bytecode (e.g. await, yield, try/catch, with, etc.).
     * The tree-walking interpreter handles these correctly at runtime. */
    int idx = pool_add_node(c, n);
    emit(c, keep_value ? BC_EVAL_NODE : BC_EVAL_NODE_POP);
    emit16(c, idx);
}

static void emit_push_string(BCComp *c, const char *s)
{
    int idx = pool_add_str(c, s);
    emit(c, BC_PUSH_STRING);
    emit16(c, idx);
    UPDATE_STACK(1);
}

static void emit_push_number(BCComp *c, double d)
{
    if (d == (double)(int32_t)d && !isnan(d) && !isinf(d) &&
        d >= -2147483648.0 && d <= 2147483647.0) {
        emit(c, BC_PUSH_INT32);
        emit32(c, (int32_t)d);
    } else {
        int idx = pool_add_f64(c, d);
        emit(c, BC_PUSH_FLOAT64);
        emit16(c, idx);
    }
    UPDATE_STACK(1);
}

/* Emit a float64 constant directly, NEVER folding an integer-valued double
 * into BC_PUSH_INT32.  This preserves float semantics for compile-time folded
 * arithmetic (e.g. `1.5 + 0.5` should stay `2.0` (float64), not become the
 * int32 literal `2` and force an int32*int32 overflow-conversion path). */
static void emit_push_f64(BCComp *c, double d)
{
    int idx = pool_add_f64(c, d);
    emit(c, BC_PUSH_FLOAT64);
    emit16(c, idx);
    UPDATE_STACK(1);
}

/* Resolve a name to a direct local slot index, or -1 if it is not a
 * local in the current (function) scope.  Only valid when no block scope
 * is active, mirroring emit_name_op's constraint. */
static int find_local_slot(BCComp *c, const char *name)
{
    if (c->local_scope_depth != 0 || !name || c->disable_local_slots) return -1;
    for (int i = 0; i < c->local_count; i++) {
        if (c->local_names[i] == name ||
            (c->local_names[i] && strcmp(c->local_names[i], name) == 0))
            return i;
    }
    return -1;
}

static void emit_name_op(BCComp *c, uint8_t op, const char *name)
{
    /* Check if this is a local variable in the current scope
     * (after the last scope boundary).  If so, emit direct slot access
     * which avoids the name-based scope chain lookup entirely.
     *
     * NOTE: direct slot access (BC_LOAD_LOCAL/BC_STORE_LOCAL) reads from
     * interp->current_scope at runtime.  However, `var` declarations are
     * hoisted to the function scope via scope_declare_name → find_function_scope,
     * while BC_LOAD_LOCAL reads from current_scope.  When there is an active
     * block scope (local_scope_depth > 0), current_scope is the block scope,
     * but the variable's value lives in the function scope (parent of the
     * block scope).  Therefore we must only use direct slot access when
     * local_scope_depth == 0 (no block scopes active), so that the slot
     * index refers to the function scope which is also current_scope.
     * Fall back to name-based scope chain lookup otherwise.             */
    if ((op == BC_LOAD_VAR || op == BC_STORE_VAR) && c->local_scope_depth == 0 &&
        !c->disable_local_slots) {
        for (int i = 0; i < c->local_count; i++) {
            if (c->local_names[i] == name ||
                (c->local_names[i] && strcmp(c->local_names[i], name) == 0)) {
                uint16_t slot = (uint16_t)i;
                emit(c, op == BC_LOAD_VAR ? BC_LOAD_LOCAL : BC_STORE_LOCAL);
                emit16(c, slot);
                if (op == BC_LOAD_VAR) UPDATE_STACK(1);
                else UPDATE_STACK(-1);
                return;
            }
        }
    }

    int idx = pool_add_str(c, name);
    emit(c, op);
    emit16(c, idx);
    /* Track stack depth: LOAD_VAR pushes, STORE_VAR pops, DECLARE_VAR pops */
    if (op == BC_LOAD_VAR || op == BC_TYPEOF_VAR)
        UPDATE_STACK(1);
    else if (op == BC_STORE_VAR || op == BC_DECLARE_VAR)
        UPDATE_STACK(-1);
}

/* Map a binary operator string to an opcode; BC_NOP when unsupported. */
static uint8_t binop_of(const char *op)
{
    if (!op) return BC_NOP;
    if (!strcmp(op, "+"))  return BC_ADD;
    if (!strcmp(op, "-"))  return BC_SUB;
    if (!strcmp(op, "*"))  return BC_MUL;
    if (!strcmp(op, "/"))  return BC_DIV;
    if (!strcmp(op, "%"))  return BC_MOD;
    if (!strcmp(op, "**")) return BC_POW;
    if (!strcmp(op, "<"))  return BC_LT;
    if (!strcmp(op, ">"))  return BC_GT;
    if (!strcmp(op, "<=")) return BC_LE;
    if (!strcmp(op, ">=")) return BC_GE;
    if (!strcmp(op, "==")) return BC_EQ;
    if (!strcmp(op, "!=")) return BC_NE;
    if (!strcmp(op, "===")) return BC_STRICT_EQ;
    if (!strcmp(op, "!==")) return BC_STRICT_NE;
    if (!strcmp(op, "<<")) return BC_SHL;
    if (!strcmp(op, ">>")) return BC_SAR;   /* signed (arithmetic) right shift */
    if (!strcmp(op, ">>>")) return BC_SHR;  /* unsigned (logical) right shift */
    if (!strcmp(op, "&"))  return BC_BIT_AND;
    if (!strcmp(op, "|"))  return BC_BIT_OR;
    if (!strcmp(op, "^"))  return BC_BIT_XOR;
    if (!strcmp(op, "in")) return BC_IN;
    if (!strcmp(op, "instanceof")) return BC_INSTANCEOF;
    return BC_NOP;
}

static int is_plain_member(ASTNode *n)
{
    return n && n->type == AST_MEMBER && n->u.member.obj &&
           n->u.member.obj->type != AST_SUPER &&
           n->u.member.prop && n->u.member.prop->type == AST_IDENTIFIER &&
           n->u.member.prop->u.ident.name && !n->u.member.is_optional;
}

static int is_computed_member(ASTNode *n)
{
    return n && n->type == AST_COMPUTED_MEMBER && n->u.member.obj &&
           n->u.member.obj->type != AST_SUPER && n->u.member.prop;
}

/* ── Common-subexpression factoring helpers ──────────────────────────────
 * Detect `a * b + a * c` (a is the same identifier on both sides) and
 * factor it to `a * (b + c)`, which replaces 2 MUL + 1 ADD with
 * 1 ADD + 1 MUL.  Also handles `a * b - a * c` → `a * (b - c)`.
 * This is a pure AST-level rewrite: it does NOT rebind any `a` side-
 * effects because both operands of the MUL are loaded/read-only.        */

static int is_ident(ASTNode *n, const char *want)
{
    return n && n->type == AST_IDENTIFIER && n->u.ident.name &&
           strcmp(n->u.ident.name, want) == 0;
}

/* Is `n` a constant numeric literal? Returns 1 and sets *out to its value. */
static int is_num_literal(ASTNode *n, double *out)
{
    if (!n || n->type != TOK_NUMBER) return 0;
    if (out) *out = n->u.number.num;
    return 1;
}

/* Does `n` look like `ident * X` (or `X * ident`) where X is a pure
 * expression (identifier or literal)?  Returns 0 if not, 1 if it matches
 * and sets *other to the non-ident operand. */
static int is_mul_with_ident(ASTNode *n, const char *ident, ASTNode **other)
{
    if (!n || n->type != AST_BINARY) return 0;
    if (strcmp(n->u.binary.op, "*") != 0) return 0;
    ASTNode *l = n->u.binary.left, *r = n->u.binary.right;
    if (is_ident(l, ident)) { *other = r; return 1; }
    if (is_ident(r, ident)) { *other = l; return 1; }
    return 0;
}

/* Return 1 if `n` is a "safe to duplicate" pure expression that can be
 * evaluated twice without side effects (identifier or numeric literal). */
static int is_pure_node(ASTNode *n)
{
    if (!n) return 0;
    if (n->type == AST_IDENTIFIER || n->type == AST_THIS) return 1;
    if (n->type == AST_LITERAL) return 1;
    return 0;
}

/* Try to factor `left + right` (or `left - right`) where both operands
 * share a common identifier factor.  On success, emit bytecode for
 * `(left_factor * (left_other < op> right_other))` and return 1. */
static int try_factor_cse(BCComp *c, ASTNode *n)
{
    const char *op = n->u.binary.op;
    if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0) return 0;
    ASTNode *l = n->u.binary.left, *r = n->u.binary.right;
    if (!l || !r || l->type != AST_BINARY || r->type != AST_BINARY) return 0;
    if (strcmp(l->u.binary.op, "*") != 0) return 0;

    /* Case 1: `ident * X + ident * Y` */
    if (l->u.binary.left && l->u.binary.left->type == AST_IDENTIFIER &&
        l->u.binary.left->u.ident.name) {
        const char *id = l->u.binary.left->u.ident.name;
        ASTNode *r_other = NULL;
        if (is_mul_with_ident(r, id, &r_other)) {
            ASTNode *l_other = l->u.binary.right;
            if (is_pure_node(l_other) && is_pure_node(r_other)) {
                /* Constant-fold `b op c` when both are numeric literals.
                 * emit_push_f64 preserves float64 identity so a folded value
                 * like `1.5 + 0.5 = 2.0` stays float64, avoiding the
                 * int32*int32 overflow-conversion path. */
                double bv, cv;
                if (is_num_literal(l_other, &bv) && is_num_literal(r_other, &cv)) {
                    double folded = (op[0] == '+') ? (bv + cv) : (bv - cv);
                    cexpr(c, l->u.binary.left);      /* push ident   */
                    emit_push_f64(c, folded);        /* push b op c  */
                    emit(c, BC_MUL);
                    UPDATE_STACK(-1);                /* 2 in, 1 out */
                    return 1;
                }
                cexpr(c, l->u.binary.left);      /* push ident   */
                cexpr(c, l_other);               /* push b       */
                cexpr(c, r_other);               /* push c       */
                emit(c, (op[0] == '+') ? BC_ADD : BC_SUB);
                emit(c, BC_MUL);
                UPDATE_STACK(-1);                /* 3 in, 1 out */
                return 1;
            }
        }
    }

    /* Case 2: `X * ident + Y * ident` (ident on the right) */
    if (l->u.binary.right && l->u.binary.right->type == AST_IDENTIFIER &&
        l->u.binary.right->u.ident.name) {
        const char *id = l->u.binary.right->u.ident.name;
        ASTNode *r_other = NULL;
        if (is_mul_with_ident(r, id, &r_other)) {
            ASTNode *l_other = l->u.binary.left;
            if (is_pure_node(l_other) && is_pure_node(r_other)) {
                double bv, cv;
                if (is_num_literal(l_other, &bv) && is_num_literal(r_other, &cv)) {
                    double folded = (op[0] == '+') ? (bv + cv) : (bv - cv);
                    cexpr(c, l->u.binary.right);     /* push ident   */
                    emit_push_f64(c, folded);        /* push b op c  */
                    emit(c, BC_MUL);
                    UPDATE_STACK(-1);
                    return 1;
                }
                cexpr(c, l->u.binary.right);     /* push ident   */
                cexpr(c, l_other);               /* push b       */
                cexpr(c, r_other);               /* push c       */
                emit(c, (op[0] == '+') ? BC_ADD : BC_SUB);
                emit(c, BC_MUL);
                UPDATE_STACK(-1);
                return 1;
            }
        }
    }

    return 0;
}

/* Does the argument list contain a spread element? */
static int has_spread(ASTNode **args, int argc)
{
    for (int i = 0; i < argc; i++)
        if (args[i] && (args[i]->type == AST_SPREAD_ELEMENT ||
                        args[i]->type == AST_SPREAD)) return 1;
    return 0;
}

/* -- Expressions ------------------------------------------------------- */

static void ccall(BCComp *c, ASTNode *n)
{
    ASTNode *callee = n->u.call.callee;
    int argc = n->u.call.argc;

    /* Cases the interpreter must handle: super(), import(), spread args,
     * optional calls, super.method(). */
    if (!callee || n->type == AST_OPTIONAL_CALL || n->u.call.is_optional ||
        callee->type == AST_SUPER || has_spread(n->u.call.args, argc) ||
        argc > 0xFFFF ||
        (callee->type == AST_IDENTIFIER && callee->u.ident.name &&
         !strcmp(callee->u.ident.name, "import")) ||
        ((callee->type == AST_MEMBER || callee->type == AST_COMPUTED_MEMBER) &&
         callee->u.member.obj && callee->u.member.obj->type == AST_SUPER)) {
        emit_eval(c, n, 1);
        return;
    }

    if (is_plain_member(callee)) {
        cexpr(c, callee->u.member.obj);
        for (int i = 0; i < argc; i++) cexpr(c, n->u.call.args[i]);
        int idx = pool_add_str(c, callee->u.member.prop->u.ident.name);
        emit(c, BC_CALL_METHOD);
        emit16(c, idx);
        emit16(c, argc);
        return;
    }
    if (is_computed_member(callee)) {
        cexpr(c, callee->u.member.obj);
        cexpr(c, callee->u.member.prop);
        for (int i = 0; i < argc; i++) cexpr(c, n->u.call.args[i]);
        emit(c, BC_CALL_ELEM);
        emit16(c, argc);
        return;
    }
    /* Plain call: the tree-walker special-cases directly-invoked function
     * expressions, so keep those on the interpreter path. */
    if (callee->type == AST_FUNC_EXPR || callee->type == AST_ARROW ||
        callee->type == AST_FUNC_DECL) {
        emit_eval(c, n, 1);
        return;
    }
    /* Type conversion fast path: String/Number/Boolean called as plain
     * functions with a single argument.  Emit inline conversion opcodes
     * instead of going through the full BC_CALL dispatch (function load,
     * argument setup, C function call, result boxing).  This avoids the
     * ~55× overhead of function calls for these trivial conversions. */
    if (callee->type == AST_IDENTIFIER && argc == 1) {
        const char *name = callee->u.ident.name;
        if (name) {
            if (!strcmp(name, "String")) {
                cexpr(c, n->u.call.args[0]);
                emit(c, BC_TO_STRING);
                return;
            }
            if (!strcmp(name, "Number")) {
                cexpr(c, n->u.call.args[0]);
                emit(c, BC_TO_NUMBER);
                return;
            }
            if (!strcmp(name, "Boolean")) {
                cexpr(c, n->u.call.args[0]);
                emit(c, BC_TO_BOOL);
                return;
            }
        }
    }
    cexpr(c, callee);
    for (int i = 0; i < argc; i++) cexpr(c, n->u.call.args[i]);
    emit(c, BC_CALL);
    emit16(c, argc);
}

static void cassign(BCComp *c, ASTNode *n)
{
    const char *op = n->u.assign.op;
    ASTNode *tgt = n->u.assign.target;
    ASTNode *val = n->u.assign.value;
    if (!op || !tgt) { c->ok = 0; return; }

    if (!strcmp(op, "=")) {
        if (tgt->type == AST_IDENTIFIER && tgt->u.ident.name) {
            const char *name = tgt->u.ident.name;
            /* -- Fused X = X + Y on a slot local ---------------------
             * The left operand is never loaded onto the stack, so its
             * refcount stays 1 and the string concat can reuse the slot
             * buffer in place (turning the O(n²) copy of `s = s + x` in
             * a loop into an amortized append).  Only for the `+` opcode
             * (string-producing) on direct slot access. */
            if (val && val->type == AST_BINARY && c->local_scope_depth == 0 &&
                 !c->disable_local_slots &&
                 val->u.binary.op[0] &&
                !strcmp(val->u.binary.op, "+") &&
                val->u.binary.left && val->u.binary.left->type == AST_IDENTIFIER &&
                val->u.binary.left->u.ident.name &&
                !strcmp(val->u.binary.left->u.ident.name, name)) {
                for (int i = 0; i < c->local_count; i++) {
                    if (c->local_names[i] == name ||
                        (c->local_names[i] && strcmp(c->local_names[i], name) == 0)) {
                        cexpr(c, val->u.binary.right);
                        emit(c, BC_ADD_SELF);
                        emit16(c, (uint16_t)i);
                        UPDATE_STACK(0);   /* pop rhs, push result: net 0 */
                        return;
                    }
                }
            }
            cexpr(c, val);
            emit(c, BC_DUP);
            emit_name_op(c, BC_STORE_VAR, tgt->u.ident.name);
            return;
        }
        if (is_plain_member(tgt)) {
            cexpr(c, tgt->u.member.obj);
            cexpr(c, val);
            emit_name_op(c, BC_SET_PROP, tgt->u.member.prop->u.ident.name);
            return;
        }
        if (is_computed_member(tgt)) {
            cexpr(c, tgt->u.member.obj);
            cexpr(c, tgt->u.member.prop);
            cexpr(c, val);
            emit(c, BC_SET_ELEM);
            return;
        }
        emit_eval(c, n, 1);   /* destructuring & friends */
        return;
    }

    /* Compound assignment: x op= y */
    size_t oplen = strlen(op);
    if (oplen >= 2 && op[oplen - 1] == '=' &&
        /* NOTE: "?\?=" — the backslash breaks up the ??= trigraph, which some
         * compilers (strict -std=c99/c11, AppleClang) otherwise rewrite to "#". */
        strcmp(op, "&&=") && strcmp(op, "||=") && strcmp(op, "?\?=")) {
        char base[8];
        if (oplen - 1 >= sizeof(base)) { emit_eval(c, n, 1); return; }
        memcpy(base, op, oplen - 1);
        base[oplen - 1] = '\0';
        uint8_t bop = binop_of(base);
        if (bop == BC_NOP) { emit_eval(c, n, 1); return; }

        if (tgt->type == AST_IDENTIFIER && tgt->u.ident.name) {
            /* -- Fused X += Y on a slot local (same in-place reuse) -- */
            if ((bop == BC_ADD || bop == BC_MUL) && c->local_scope_depth == 0 && !c->disable_local_slots) {
                const char *name = tgt->u.ident.name;
                for (int i = 0; i < c->local_count; i++) {
                    if (c->local_names[i] == name ||
                        (c->local_names[i] && strcmp(c->local_names[i], name) == 0)) {
                        cexpr(c, val);
                        emit(c, (bop == BC_ADD) ? BC_ADD_SELF : BC_MUL_SELF);
                        emit16(c, (uint16_t)i);
                        UPDATE_STACK(0);   /* pop rhs, push result: net 0 */
                        return;
                    }
                }
            }
            emit_name_op(c, BC_LOAD_VAR, tgt->u.ident.name);
            cexpr(c, val);
            emit(c, bop);
            emit(c, BC_DUP);
            emit_name_op(c, BC_STORE_VAR, tgt->u.ident.name);
            return;
        }
        if (is_plain_member(tgt)) {
            const char *prop = tgt->u.member.prop->u.ident.name;
            cexpr(c, tgt->u.member.obj);      /* obj                    */
            emit(c, BC_DUP);                  /* obj obj                */
            emit_name_op(c, BC_GET_PROP, prop); /* obj cur              */
            cexpr(c, val);                    /* obj cur rhs            */
            emit(c, bop);                     /* obj res                */
            emit_name_op(c, BC_SET_PROP, prop); /* res                  */
            return;
        }
        if (is_computed_member(tgt)) {
            cexpr(c, tgt->u.member.obj);      /* obj                    */
            cexpr(c, tgt->u.member.prop);     /* obj key                */
            emit(c, BC_DUP2);                 /* obj key obj key        */
            emit(c, BC_GET_ELEM);             /* obj key cur            */
            cexpr(c, val);                    /* obj key cur rhs        */
            emit(c, bop);                     /* obj key res            */
            emit(c, BC_SET_ELEM);             /* res                    */
            return;
        }
    }

    emit_eval(c, n, 1);   /* &&=, ||=, ?\?=, patterns */
}

static void cincdec(BCComp *c, ASTNode *n)
{
    const char *op = n->u.unary.op;
    int prefix = n->u.unary.prefix;
    int is_inc = (op[0] == '+');
    ASTNode *tgt = n->u.unary.arg;
    uint8_t aop = is_inc ? BC_ADD : BC_SUB;

    if (tgt && tgt->type == AST_IDENTIFIER && tgt->u.ident.name) {
        const char *name = tgt->u.ident.name;
        /* Check if this is a local variable — if so, use BC_INC_LOCAL */
        if (is_inc && c->local_scope_depth == 0 && !c->disable_local_slots) {
            for (int i = 0; i < c->local_count; i++) {
                if (c->local_names[i] == name ||
                    (c->local_names[i] && strcmp(c->local_names[i], name) == 0)) {
                    emit(c, BC_INC_LOCAL);
                    emit16(c, (uint16_t)i);
                    if (prefix) {
                        /* postfix pushes old value, but prefix needs new */
                        emit(c, BC_POP);
                        emit(c, BC_LOAD_LOCAL);
                        emit16(c, (uint16_t)i);
                        UPDATE_STACK(0); /* POP then PUSH = net 0 */
                    } else {
                        /* postfix: INC_LOCAL already pushed old value */
                        UPDATE_STACK(1);
                    }
                    return;
                }
            }
        }
        /* Fallback for non-local or decrement */
        /* Use fused BC_INC_VAR for increment (replaces 6 opcodes with 1).
         * For prefix increment, BC_INC_VAR pushes old value, so POP + reload.
         * Decrement still uses the original LOAD+MODIFY+STORE sequence. */
        if (is_inc) {
            emit_name_op(c, BC_INC_VAR, name);
            if (prefix) {
                /* BC_INC_VAR pushes old value; prefix needs new value.
                 * POP old, then LOAD_VAR to get the new value. */
                emit(c, BC_POP);
                emit_name_op(c, BC_LOAD_VAR, name);
                UPDATE_STACK(0); /* POP + PUSH = net 0 */
            } else {
                UPDATE_STACK(1);
            }
        } else {
            emit_name_op(c, BC_LOAD_VAR, name);
            emit(c, BC_POS);                       /* ToNumber              */
            if (prefix) {
                emit(c, BC_PUSH_INT32); emit32(c, 1);
                emit(c, aop);                      /* new                   */
                emit(c, BC_DUP);
                emit_name_op(c, BC_STORE_VAR, name);
            } else {
                emit(c, BC_DUP);                   /* old old               */
                emit(c, BC_PUSH_INT32); emit32(c, 1);
                emit(c, aop);                      /* old new               */
                emit_name_op(c, BC_STORE_VAR, name);
            }
        }
        return;
    }
    if (is_plain_member(tgt)) {
        const char *prop = tgt->u.member.prop->u.ident.name;
        cexpr(c, tgt->u.member.obj);           /* obj                   */
        emit(c, BC_DUP);                       /* obj obj               */
        emit_name_op(c, BC_GET_PROP, prop);    /* obj cur               */
        emit(c, BC_POS);                       /* obj n                 */
        if (prefix) {
            emit(c, BC_PUSH_INT32); emit32(c, 1);
            emit(c, aop);                      /* obj n'                */
            emit_name_op(c, BC_SET_PROP, prop);/* n'                    */
        } else {
            emit(c, BC_DUP2);                  /* obj n obj n           */
            emit(c, BC_PUSH_INT32); emit32(c, 1);
            emit(c, aop);                      /* obj n obj n'          */
            emit_name_op(c, BC_SET_PROP, prop);/* obj n n'              */
            emit(c, BC_POP);                   /* obj n                 */
            emit(c, BC_SWAP);                  /* n obj                 */
            emit(c, BC_POP);                   /* n                     */
        }
        return;
    }
    if (is_computed_member(tgt)) {
        cexpr(c, tgt->u.member.obj);           /* obj                   */
        cexpr(c, tgt->u.member.prop);          /* obj key               */
        emit(c, BC_DUP2);                      /* obj key obj key       */
        emit(c, BC_GET_ELEM);                  /* obj key cur           */
        emit(c, BC_POS);                       /* obj key n             */
        if (prefix) {
            emit(c, BC_PUSH_INT32); emit32(c, 1);
            emit(c, aop);                      /* obj key n'            */
            emit(c, BC_SET_ELEM);              /* n'                    */
        } else {
            emit_eval(c, n, 1);                /* rare: keep it simple  */
        }
        return;
    }
    emit_eval(c, n, 1);
}

static void ctemplate(BCComp *c, ASTNode *n)
{
    if (n->u.template_lit.tag) { emit_eval(c, n, 1); return; }
    int nparts = n->u.template_lit.nparts;
    int nexp   = n->u.template_lit.nexp;
    char **parts = n->u.template_lit.parts;

    char *cooked = interp_bc_cook_template(nparts > 0 && parts ? parts[0] : "");
    emit_push_string(c, cooked ? cooked : "");
    free(cooked);

    for (int i = 0; i < nexp; i++) {
        cexpr(c, n->u.template_lit.exprs[i]);
        emit(c, BC_ADD);
        if (i + 1 < nparts && parts) {
            char *ck = interp_bc_cook_template(parts[i + 1]);
            if (ck && ck[0]) {
                emit_push_string(c, ck);
                emit(c, BC_ADD);
            }
            free(ck);
        }
    }
}

static void cobject(BCComp *c, ASTNode *n)
{
    int nprops = n->u.object.nprops;
    ASTNode **props = n->u.object.props;

    /* Accessors and spreads stay with the interpreter. */
    for (int i = 0; i < nprops; i++) {
        ASTNode *pn = props[i];
        if (!pn || pn->type != AST_PROPERTY) { emit_eval(c, n, 1); return; }
        ASTNode *v = pn->u.property.val;
        if (v && v->type == AST_FUNC_EXPR &&
            (v->u.func.is_getter || v->u.func.is_setter)) { emit_eval(c, n, 1); return; }
        if (!pn->u.property.key) { emit_eval(c, n, 1); return; }
    }

    emit(c, BC_NEW_OBJECT);
    for (int i = 0; i < nprops; i++) {
        ASTNode *pn = props[i];
        ASTNode *k = pn->u.property.key;
        if (k->type == AST_IDENTIFIER && k->u.ident.name) {
            cexpr(c, pn->u.property.val);
            emit_name_op(c, BC_DEF_PROP, k->u.ident.name);
        } else if (k->type == AST_LITERAL && k->token.type == TOK_STRING) {
            cexpr(c, pn->u.property.val);
            emit_name_op(c, BC_DEF_PROP, k->u.string.str ? k->u.string.str : "");
        } else {
            cexpr(c, k);
            cexpr(c, pn->u.property.val);
            emit(c, BC_DEF_ELEM);
        }
    }
}

static void cexpr(BCComp *c, ASTNode *n)
{
    if (!c->ok) return;
    if (!n) { emit(c, BC_PUSH_UNDEFINED); return; }

    switch (n->type) {
    case AST_LITERAL:
        switch (n->token.type) {
        case TOK_NUMBER:        emit_push_number(c, n->u.number.num); break;
        case TOK_STRING:        emit_push_string(c, n->u.string.str ? n->u.string.str : ""); break;
        case TOK_BOOL_LIT:      emit(c, n->u.bool_val.val ? BC_PUSH_TRUE : BC_PUSH_FALSE); break;
        case TOK_NULL_LIT:      emit(c, BC_PUSH_NULL); break;
        case TOK_UNDEFINED_LIT: emit(c, BC_PUSH_UNDEFINED); break;
        default:                emit_eval(c, n, 1); break;   /* BigInt, RegExp… */
        }
        break;

    case AST_IDENTIFIER:
        if (!n->u.ident.name) { emit(c, BC_PUSH_UNDEFINED); break; }
        emit_name_op(c, BC_LOAD_VAR, n->u.ident.name);
        break;

    case AST_THIS:
        c->p->uses_this = 1;
        emit(c, BC_PUSH_THIS);
        break;

    case AST_SUPER:
        c->p->uses_super = 1;
        emit_eval(c, n, 1);
        break;

    case AST_BINARY: {
        const char *op = n->u.binary.op;
        if (!strcmp(op, "&&")) {
            cexpr(c, n->u.binary.left);
            int j = emit_jump(c, BC_JUMP_IF_FALSE_KEEP);
            emit(c, BC_POP);
            cexpr(c, n->u.binary.right);
            patch_here(c, j);
            break;
        }
        if (!strcmp(op, "||")) {
            cexpr(c, n->u.binary.left);
            int j = emit_jump(c, BC_JUMP_IF_TRUE_KEEP);
            emit(c, BC_POP);
            cexpr(c, n->u.binary.right);
            patch_here(c, j);
            break;
        }
        if (!strcmp(op, "??")) {
            cexpr(c, n->u.binary.left);
            int j = emit_jump(c, BC_JUMP_IF_NOT_NULLISH);
            emit(c, BC_POP);
            cexpr(c, n->u.binary.right);
            patch_here(c, j);
            break;
        }
        uint8_t bop = binop_of(op);
        if (bop == BC_NOP) { emit_eval(c, n, 1); break; }
        /* -- CSE factoring: `a*b + a*c` -> `a*(b+c)` -- */
        if ((bop == BC_ADD || bop == BC_SUB) && try_factor_cse(c, n)) break;
        cexpr(c, n->u.binary.left);
        cexpr(c, n->u.binary.right);
        emit(c, bop);
        break;
    }

    case AST_UNARY: {
        const char *op = n->u.unary.op;
        if (!op) { emit_eval(c, n, 1); break; }
        if (!strcmp(op, "++") || !strcmp(op, "--")) { cincdec(c, n); break; }
        if (!strcmp(op, "typeof")) {
            ASTNode *a = n->u.unary.arg;
            if (a && a->type == AST_IDENTIFIER && a->u.ident.name)
                emit_name_op(c, BC_TYPEOF_VAR, a->u.ident.name);
            else { cexpr(c, a); emit(c, BC_TYPEOF); }
            break;
        }
        if (!strcmp(op, "void")) { cexpr(c, n->u.unary.arg); emit(c, BC_VOID); break; }
        if (!strcmp(op, "delete")) {
            ASTNode *a = n->u.unary.arg;
            if (is_plain_member(a)) {
                cexpr(c, a->u.member.obj);
                emit_name_op(c, BC_DELETE_PROP, a->u.member.prop->u.ident.name);
            } else if (is_computed_member(a)) {
                cexpr(c, a->u.member.obj);
                cexpr(c, a->u.member.prop);
                emit(c, BC_DELETE_ELEM);
            } else {
                emit(c, BC_PUSH_TRUE);
            }
            break;
        }
        if (!strcmp(op, "!"))  { cexpr(c, n->u.unary.arg); emit(c, BC_NOT); break; }
        if (!strcmp(op, "~"))  { cexpr(c, n->u.unary.arg); emit(c, BC_BIT_NOT); break; }
        if (!strcmp(op, "-") && n->u.unary.prefix) { cexpr(c, n->u.unary.arg); emit(c, BC_NEG); break; }
        if (!strcmp(op, "+") && n->u.unary.prefix) { cexpr(c, n->u.unary.arg); emit(c, BC_POS); break; }
        emit_eval(c, n, 1);
        break;
    }

    case AST_CONDITIONAL: {
        cexpr(c, n->u.conditional.cond);
        int jfalse = emit_jump(c, BC_JUMP_IF_FALSE);
        cexpr(c, n->u.conditional.consequent);
        int jend = emit_jump(c, BC_JUMP);
        patch_here(c, jfalse);
        cexpr(c, n->u.conditional.alternate);
        patch_here(c, jend);
        break;
    }

    case AST_CALL: case AST_OPTIONAL_CALL:
        ccall(c, n);
        break;

    case AST_NEW: {
        int argc = n->u.new_expr.argc;
        if (has_spread(n->u.new_expr.args, argc) || argc > 0xFFFF) {
            emit_eval(c, n, 1);
            break;
        }
        cexpr(c, n->u.new_expr.callee);
        for (int i = 0; i < argc; i++) cexpr(c, n->u.new_expr.args[i]);
        emit(c, BC_NEW);
        emit16(c, argc);
        break;
    }

    case AST_MEMBER:
        if (!is_plain_member(n)) { emit_eval(c, n, 1); break; }
        /* Fused LOAD_LOCAL + GET_PROP: when the receiver is a local
         * variable (direct slot), emit a single BC_LOAD_PROP that loads
         * the slot and does the property get without an intermediate
         * refcount inc/dec of the receiver. */
        if (n->u.member.obj && n->u.member.obj->type == AST_IDENTIFIER &&
            getenv("LR_FORCE_NO_FUSE") == NULL) {
            int slot = find_local_slot(c, n->u.member.obj->u.ident.name);
            if (slot >= 0 && slot < 0xFFFF) {
                int pi = pool_add_str(c, n->u.member.prop->u.ident.name);
                emit(c, BC_LOAD_PROP);
                emit16(c, (uint16_t)slot);
                emit16(c, (uint16_t)pi);
                UPDATE_STACK(1);
                break;
            }
        }
        cexpr(c, n->u.member.obj);
        emit_name_op(c, BC_GET_PROP, n->u.member.prop->u.ident.name);
        break;

    case AST_COMPUTED_MEMBER:
        if (!is_computed_member(n)) { emit_eval(c, n, 1); break; }
        cexpr(c, n->u.member.obj);
        cexpr(c, n->u.member.prop);
        emit(c, BC_GET_ELEM);
        break;

    case AST_ARRAY: {
        int nelem = n->u.array.nelem;
        if (nelem > 0xFFFF) { emit_eval(c, n, 1); break; }
        for (int i = 0; i < nelem; i++) {
            ASTNode *e = n->u.array.elements[i];
            if (e && (e->type == AST_SPREAD_ELEMENT || e->type == AST_SPREAD)) {
                emit_eval(c, n, 1);
                return;
            }
        }
        for (int i = 0; i < nelem; i++) {
            ASTNode *e = n->u.array.elements[i];
            if (!e) emit(c, BC_PUSH_UNDEFINED);   /* hole */
            else cexpr(c, e);
        }
        emit(c, BC_NEW_ARRAY);
        emit16(c, nelem);
        break;
    }

    case AST_OBJECT:
        cobject(c, n);
        break;

    case AST_ASSIGN:
        cassign(c, n);
        break;

    case AST_SEQUENCE: {
        int count = n->u.sequence.count;
        if (count == 0) { emit(c, BC_PUSH_UNDEFINED); break; }
        for (int i = 0; i < count; i++) {
            cexpr(c, n->u.sequence.exprs[i]);
            if (i + 1 < count) emit(c, BC_POP);
        }
        break;
    }

    case AST_TEMPLATE:
        ctemplate(c, n);
        break;

    default:
        emit_eval(c, n, 1);
        break;
    }
}

/* -- Loop bookkeeping -------------------------------------------------- */

static void loop_push(BCComp *c, BCLoop *lp, int is_switch)
{
    memset(lp, 0, sizeof(*lp));
    lp->prev = c->loop;
    lp->is_switch = is_switch;
    lp->label = c->pending_label;
    c->pending_label = NULL;
    c->loop = lp;
}

static void patch_list(BCComp *c, int *list, int count, int target)
{
    for (int i = 0; i < count; i++) patch_jump_to(c, list[i], target);
}

static void loop_pop(BCComp *c, BCLoop *lp, int break_target, int continue_target)
{
    patch_list(c, lp->brk, lp->nbrk, break_target);
    if (!lp->is_switch) patch_list(c, lp->cont, lp->ncont, continue_target);
    free(lp->brk);
    free(lp->cont);
    c->loop = lp->prev;
}

static void loop_add(BCComp *c, int **list, int *count, int *cap, int pos)
{
    if (*count >= *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        int *nl = (int *)realloc(*list, sizeof(int) * (size_t)ncap);
        if (!nl) { c->ok = 0; return; }
        *list = nl;
        *cap = ncap;
    }
    (*list)[(*count)++] = pos;
}

/* Find the loop a labelled/unlabelled break or continue targets. */
static BCLoop *find_target(BCComp *c, const char *label, int need_loop)
{
    BCLoop *lp = c->loop;
    while (lp) {
        if (!label) {
            if (!need_loop || !lp->is_switch) return lp;
        } else if (lp->label && !strcmp(lp->label, label)) {
            return lp;
        }
        lp = lp->prev;
    }
    return NULL;
}

/* -- Statements -------------------------------------------------------- */

static const char *label_name(ASTNode *n)
{
    if (n && n->type == AST_IDENTIFIER && n->u.ident.name && n->u.ident.name[0])
        return n->u.ident.name;
    return NULL;
}

static int var_decl_kind(ASTNode *n)
{
    switch (n->token.type) {
    case TOK_LET:   return 1;
    case TOK_CONST: return 2;
    default:        return 0;
    }
}

/* -- Block-scope elision ------------------------------------------------
 * A block only needs a runtime lexical scope if it directly contains a
 * lexical binding: a let/const declaration, a class declaration, or a
 * block-level function declaration.  Blocks whose statements are only
 * `var` declarations and plain statements use the enclosing function
 * scope, so the SCOPE_ENTER/SCOPE_LEAVE pair (and the local_scope_depth
 * guard that disables direct slot access) can be skipped entirely.
 * This removes 2 opcodes + a heap/pooled scope push/pop from every
 * iteration of hot loops like `for (var i = 0; i < N; i++) { var x = i; }`.
 *
 * Nested blocks and for/if bodies that are themselves blocks create their
 * own scopes and are compiled independently, so they are NOT counted here
 * (only non-block single-statement bodies — Annex B block-level functions
 * and friends — are descended into).  */

static int node_has_lexical(ASTNode *n);

static int block_has_lexical(ASTNode *n)
{
    for (int i = 0; i < n->u.list.count; i++) {
        if (node_has_lexical(n->u.list.items[i])) return 1;
    }
    return 0;
}

static int node_has_lexical(ASTNode *n)
{
    if (!n) return 0;
    switch (n->type) {
    case AST_VAR_DECL:  return var_decl_kind(n) != 0;
    case AST_CLASS_DECL:
    case AST_FUNC_DECL: return 1;
    /* Descend into non-block single-statement bodies only.  Block bodies
     * (AST_BLOCK / AST_PROGRAM) create their own scope and are skipped. */
    case AST_IF:        return node_has_lexical(n->u.if_stmt.body) ||
                               node_has_lexical(n->u.if_stmt.else_body);
    case AST_FOR:       return node_has_lexical(n->u.for_stmt.init) ||
                               node_has_lexical(n->u.for_stmt.body);
    case AST_FOR_IN:    return node_has_lexical(n->u.for_in.body);
    case AST_FOR_OF:    return node_has_lexical(n->u.for_of.body);
    case AST_WHILE:
    case AST_DO_WHILE:  return node_has_lexical(n->u.if_stmt.body);
    case AST_LABEL:     return node_has_lexical(n->u.label_stmt.stmt);
    case AST_TRY:       return node_has_lexical(n->u.try_stmt.body) ||
                               node_has_lexical(n->u.try_stmt.catch_body) ||
                               node_has_lexical(n->u.try_stmt.finally_body);
    case AST_SWITCH:
        for (int i = 0; i < n->u.switch_stmt.ncases; i++)
            if (node_has_lexical(n->u.switch_stmt.cases[i])) return 1;
        return 0;
    case AST_CASE:
    case AST_DEFAULT:   return node_has_lexical(n->u.if_stmt.body);
    default:            return 0;
    }
}

/* Register a name in the compiler's local slot table (duplicate-safe).
 * Keeps local_names index order aligned with the order the runtime scope
 * appends names via scope_declare_name, so that BC_LOAD_LOCAL/BC_STORE_LOCAL
 * slot indices resolve to the correct variables. */
static void register_local_name(BCComp *c, const char *name)
{
    if (!name || c->local_count >= 256) return;
    for (int j = 0; j < c->local_count; j++) {
        if (c->local_names[j] == name ||
            (c->local_names[j] && strcmp(c->local_names[j], name) == 0))
            return;
    }
    c->local_names[c->local_count++] = name;
}

/* Mirror the runtime declaration order of eval_pattern (lr_interp.c): walk a
 * destructuring pattern and register every bound identifier in local_names.
 * The tree-walker declares these names in the same order via
 * scope_declare_name; pre-registering them here keeps the compiler's slot
 * indices aligned with the runtime scope.  Without this, a destructuring
 * statement delegated to BC_EVAL_NODE adds names to the runtime scope that
 * the compiler does not know about, shifting every subsequent slot and
 * corrupting unrelated variables. */
static void register_pattern_names(BCComp *c, ASTNode *node)
{
    if (!c || !node) return;

    int is_obj = (node->type == AST_OBJECT) ||
        (node->type == AST_PATTERN && node->u.pattern_object.is_object);

    /* Array destructuring: [a, b, ...rest] */
    if (!is_obj) {
        int nelem = node->u.pattern_array.nelem;
        ASTNode **elements = node->u.pattern_array.elements;
        for (int i = 0; i < nelem; i++) {
            ASTNode *elem = elements ? elements[i] : NULL;
            if (!elem) continue;                       /* hole */
            if (elem->type == AST_REST || elem->type == AST_SPREAD_ELEMENT) {
                ASTNode *t = elem->type == AST_REST ? elem->u.rest_elem.arg
                                                    : elem->u.spread.arg;
                if (t && t->type == AST_IDENTIFIER)
                    register_local_name(c, t->u.ident.name);
                break;                                 /* rest is last */
            }
            if (elem->type == AST_DEFAULT_VALUE) {
                ASTNode *left = elem->u.default_val.left;
                if (left && left->type == AST_IDENTIFIER)
                    register_local_name(c, left->u.ident.name);
            } else if (elem->type == AST_IDENTIFIER) {
                register_local_name(c, elem->u.ident.name);
            } else if (elem->type == AST_ASSIGN &&
                       elem->u.assign.target &&
                       elem->u.assign.target->type == AST_IDENTIFIER) {
                register_local_name(c, elem->u.assign.target->u.ident.name);
            } else if (elem->type == AST_PATTERN || elem->type == AST_ARRAY ||
                       elem->type == AST_OBJECT) {
                register_pattern_names(c, elem);
            }
        }
        return;
    }

    /* Object destructuring: { a, b: c, ...rest } */
    {
        int nprops = node->u.pattern_object.nprops;
        ASTNode **props = node->u.pattern_object.props;
        for (int i = 0; i < nprops; i++) {
            ASTNode *prop = props ? props[i] : NULL;
            if (!prop) continue;
            if (prop->type == AST_REST || prop->type == AST_SPREAD ||
                prop->type == AST_SPREAD_ELEMENT) {
                ASTNode *t = prop->type == AST_REST ? prop->u.rest_elem.arg
                                                    : prop->u.spread.arg;
                if (t && t->type == AST_IDENTIFIER)
                    register_local_name(c, t->u.ident.name);
                break;                                 /* rest is last */
            }
            if (prop->type != AST_PROPERTY || !prop->u.property.val) continue;
            ASTNode *val = prop->u.property.val;
            if (val->type == AST_DEFAULT_VALUE) {
                ASTNode *left = val->u.default_val.left;
                if (left && left->type == AST_IDENTIFIER)
                    register_local_name(c, left->u.ident.name);
            } else if (val->type == AST_IDENTIFIER) {
                const char *nm = val->u.ident.name;
                if (prop->u.property.shorthand && prop->u.property.key &&
                    prop->u.property.key->type == AST_IDENTIFIER)
                    nm = prop->u.property.key->u.ident.name;
                register_local_name(c, nm);
            } else if (val->type == AST_ASSIGN &&
                       val->u.assign.target &&
                       val->u.assign.target->type == AST_IDENTIFIER) {
                register_local_name(c, val->u.assign.target->u.ident.name);
            } else if (val->type == AST_PATTERN || val->type == AST_ARRAY ||
                       val->type == AST_OBJECT) {
                register_pattern_names(c, val);
            }
        }
    }
}

static void cvar_decl(BCComp *c, ASTNode *n)
{
    int kind = var_decl_kind(n);

    /* A destructuring declarator carries its let/const-ness on the *parent*
     * AST_VAR_DECL, so delegating the bare declarator would lose the kind.
     * If any declarator needs the interpreter, hand over the whole statement
     * once — emitting per-declarator would re-run the other initialisers. */
    for (int i = 0; i < n->u.var_decl.nvars; i++) {
        ASTNode *d = n->u.var_decl.vars[i];
        if (!d || d->type != AST_VAR_DECLARATOR) continue;
        ASTNode *var = d->u.declarator.var;
        if (!var || var->type != AST_IDENTIFIER || !var->u.ident.name) {
            /* The whole statement is delegated to the tree-walker
             * (eval_var_declarator), which declares every bound name in the
             * runtime scope.  Pre-register those names here, in declarator
             * order, so BC_LOAD_LOCAL/BC_STORE_LOCAL slot indices stay
             * aligned — otherwise names added by the tree-walker shift every
             * subsequent slot and corrupt unrelated variables. */
            for (int k = 0; k < n->u.var_decl.nvars; k++) {
                ASTNode *d2 = n->u.var_decl.vars[k];
                if (!d2 || d2->type != AST_VAR_DECLARATOR) continue;
                ASTNode *v = d2->u.declarator.var;
                if (!v) continue;
                if (v->type == AST_IDENTIFIER) {
                    register_local_name(c, v->u.ident.name);
                } else if (v->type == AST_PATTERN || v->type == AST_ARRAY ||
                           v->type == AST_OBJECT) {
                    register_pattern_names(c, v);
                }
            }
            emit_eval(c, n, 0);
            return;
        }
    }

    for (int i = 0; i < n->u.var_decl.nvars; i++) {
        ASTNode *d = n->u.var_decl.vars[i];
        if (!d || d->type != AST_VAR_DECLARATOR) continue;
        ASTNode *var  = d->u.declarator.var;
        ASTNode *init = d->u.declarator.init;
        if (init) cexpr(c, init);
        else emit(c, BC_PUSH_UNDEFINED);
        int idx = pool_add_str(c, var->u.ident.name);
        emit(c, BC_DECLARE_VAR);
        emit16(c, idx);
        emit(c, (uint8_t)kind);
        /* Track local variable for direct slot access.
         * var declarations are hoisted to the function scope and survive
         * block scope boundaries — track their count separately so that
         * block scope exit doesn't remove them from the slot map.
         * let/const are block-scoped and removed on scope exit.
         * NOTE: check for duplicate names — the same `var k` can appear
         * in multiple for-loops or blocks, and adding it again would
         * corrupt the slot map (local_count becomes > actual slot count). */
        if (c->local_count < 256) {
            int found = 0;
            for (int j = 0; j < c->local_count; j++) {
                if (c->local_names[j] == var->u.ident.name ||
                    (c->local_names[j] && strcmp(c->local_names[j], var->u.ident.name) == 0)) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                c->local_names[c->local_count++] = var->u.ident.name;
            }
            if (kind == 0) {
                c->var_local_count = c->local_count;
            }
        }
    }
}

static void cfor_of(BCComp *c, ASTNode *n)
{
    ASTNode *each = n->u.for_of.each;
    const char *bind = NULL;
    int kind = 1;      /* let by default */
    int declare = 0;

    if (each && each->type == AST_VAR_DECL && each->u.var_decl.nvars == 1) {
        ASTNode *d = each->u.var_decl.vars[0];
        if (d && d->type == AST_VAR_DECLARATOR && d->u.declarator.var &&
            d->u.declarator.var->type == AST_IDENTIFIER) {
            bind = d->u.declarator.var->u.ident.name;
            kind = var_decl_kind(each);
            declare = 1;
        }
    } else if (each && each->type == AST_IDENTIFIER) {
        bind = each->u.ident.name;
        declare = 0;
    }
    if (!bind) {
        /* Destructuring loop binding is delegated to the tree-walker
         * (eval_pattern), which declares the bound names in the runtime
         * scope.  Pre-register them so slot indices stay aligned. */
        if (each) {
            if (each->type == AST_VAR_DECL) {
                for (int i = 0; i < each->u.var_decl.nvars; i++) {
                    ASTNode *d = each->u.var_decl.vars[i];
                    if (!d || d->type != AST_VAR_DECLARATOR) continue;
                    ASTNode *v = d->u.declarator.var;
                    if (!v) continue;
                    if (v->type == AST_IDENTIFIER) {
                        register_local_name(c, v->u.ident.name);
                    } else if (v->type == AST_PATTERN || v->type == AST_ARRAY ||
                               v->type == AST_OBJECT) {
                        register_pattern_names(c, v);
                    }
                }
            } else if (each->type == AST_PATTERN || each->type == AST_ARRAY ||
                       each->type == AST_OBJECT) {
                register_pattern_names(c, each);
            }
        }
        emit_eval(c, n, 0);
        return;
    }

    /* Register the loop binding variable in local_names so that
     * compiler slot indices match the runtime function scope.
     * BC_DECLARE_VAR (used below for let/const) adds the name to
     * the runtime scope; local_names must mirror that for correct
     * BC_LOAD_LOCAL / BC_STORE_LOCAL slot resolution. */
    if (declare && c->local_count < 256) {
        int found = 0;
        for (int j = 0; j < c->local_count; j++) {
            if (c->local_names[j] == bind ||
                (c->local_names[j] && strcmp(c->local_names[j], bind) == 0)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            c->local_names[c->local_count++] = bind;
        }
    }

    cexpr(c, n->u.for_of.source);
    /* Only emit a block scope for let/const (which need block-level scoping).
     * For var (hoisted to function scope) and bare variable assignments,
     * skip the block scope so that BC_LOAD_LOCAL / BC_STORE_LOCAL slot
     * indices in the loop body refer to the function scope and not an
     * empty block scope.  Without this guard, s += v inside a function
     * reads/writes the wrong slot and the loop body becomes a no-op. */
    int has_block = (declare && kind != 0);
    if (has_block) {
        /* Mirror the runtime BC_SCOPE_ENTER with the compiler-side
         * local_scope_depth guard: while the block scope is active,
         * BC_LOAD_LOCAL/BC_STORE_LOCAL would read current_scope (the
         * empty block scope) at runtime while let/const values are
         * hoisted to the function scope, so the loop body must use
         * name-based lookup (see the same guard in cblock). */
        c->local_scope_stack[c->local_scope_depth++] = c->local_count;
        emit(c, BC_SCOPE_ENTER);
    }
    emit(c, BC_ITER_INIT);

    BCLoop lp;
    loop_push(c, &lp, 0);

    int top = here(c);
    emit(c, BC_LOOP_TICK);
    emit(c, BC_ITER_NEXT);
    int done_patch = here(c);
    emit32(c, 0);

    int idx = pool_add_str(c, bind);
    if (declare) { emit(c, BC_DECLARE_VAR); emit16(c, idx); emit(c, (uint8_t)kind); }
    else         { emit(c, BC_STORE_VAR);   emit16(c, idx); }

    cstmt(c, n->u.for_of.body, 0);

    emit(c, BC_JUMP);
    int back = here(c);
    emit32(c, 0);
    patch_jump_to(c, back, top);

    int end = here(c);
    patch_jump_to(c, done_patch, end);
    loop_pop(c, &lp, end, top);

    emit(c, BC_ITER_CLOSE);
    if (has_block) {
        emit(c, BC_SCOPE_LEAVE);
        c->local_scope_depth--;
    }
}

static void cswitch(BCComp *c, ASTNode *n)
{
    int ncases = n->u.switch_stmt.ncases;
    ASTNode **cases = n->u.switch_stmt.cases;

    cexpr(c, n->u.switch_stmt.test);      /* discriminant stays on stack */

    BCLoop lp;
    loop_push(c, &lp, 1);

    int *body_jumps = (int *)calloc((size_t)(ncases > 0 ? ncases : 1), sizeof(int));
    int *body_pos   = (int *)calloc((size_t)(ncases > 0 ? ncases : 1), sizeof(int));
    if (!body_jumps || !body_pos) {
        free(body_jumps); free(body_pos);
        c->ok = 0;
        loop_pop(c, &lp, here(c), here(c));
        return;
    }
    int default_index = -1;

    for (int i = 0; i < ncases; i++) {
        ASTNode *cs = cases[i];
        if (!cs) continue;
        if (cs->type == AST_DEFAULT) { default_index = i; body_jumps[i] = -1; continue; }
        emit(c, BC_DUP);
        cexpr(c, cs->u.if_stmt.cond);
        emit(c, BC_STRICT_EQ);
        body_jumps[i] = emit_jump(c, BC_JUMP_IF_TRUE);
    }
    int no_match_jump = emit_jump(c, BC_JUMP);

    for (int i = 0; i < ncases; i++) {
        ASTNode *cs = cases[i];
        if (!cs) continue;
        body_pos[i] = here(c);
        ASTNode *body = cs->u.if_stmt.body;
        if (body && body->type == AST_BLOCK) {
            for (int j = 0; j < body->u.list.count; j++)
                cstmt(c, body->u.list.items[j], 0);
        } else if (body) {
            cstmt(c, body, 0);
        }
    }
    int end = here(c);

    for (int i = 0; i < ncases; i++)
        if (body_jumps[i] >= 0) patch_jump_to(c, body_jumps[i], body_pos[i]);
    patch_jump_to(c, no_match_jump,
                  default_index >= 0 ? body_pos[default_index] : end);

    loop_pop(c, &lp, end, end);
    free(body_jumps);
    free(body_pos);

    emit(c, BC_POP);   /* drop discriminant */
}

static void cstmt(BCComp *c, ASTNode *n, int top)
{
    if (!c->ok || !n) return;

    switch (n->type) {
    case AST_PROGRAM:
        /* Two-pass: function declarations are hoisted to the top of the
         * scope per JS semantics.  First emit all function declarations,
         * then emit everything else.                                    */
        for (int i = 0; i < n->u.list.count; i++)
            if (n->u.list.items[i]->type == AST_FUNC_DECL)
                cstmt(c, n->u.list.items[i], 1);
        for (int i = 0; i < n->u.list.count; i++)
            if (n->u.list.items[i]->type != AST_FUNC_DECL)
                cstmt(c, n->u.list.items[i], 1);
        break;

    case AST_BLOCK:
        /* Track scope nesting for local variable slot resolution.
         * var declarations are hoisted to the function scope, so they
         * survive block scope boundaries — keep var_local_count as a
         * floor so that var slots are not lost on scope exit.
         *
         * OPTIMIZATION: a block that contains no lexical bindings
         * (no let/const/class/function) never needs a runtime scope —
         * all its `var` declarations hoist to the function scope anyway.
         * Skip the SCOPE_ENTER/SCOPE_LEAVE pair AND keep local_scope_depth
         * at 0, which lets direct slot access (BC_LOAD_LOCAL/STORE_LOCAL)
         * and the loop fast ops fire inside the block.  This removes a
         * pooled-scope push/pop from every iteration of hot loops. */
        {   int prev_count = c->local_count;
            int needs_scope = block_has_lexical(n);
            if (needs_scope) {
                c->local_scope_stack[c->local_scope_depth++] = prev_count;
                emit(c, BC_SCOPE_ENTER);
            }
            /* Two-pass: hoist function declarations within block too */
            for (int i = 0; i < n->u.list.count; i++)
                if (n->u.list.items[i]->type == AST_FUNC_DECL)
                    cstmt(c, n->u.list.items[i], 0);
            for (int i = 0; i < n->u.list.count; i++)
                if (n->u.list.items[i]->type != AST_FUNC_DECL)
                    cstmt(c, n->u.list.items[i], 0);
            if (needs_scope) {
                emit(c, BC_SCOPE_LEAVE);
                /* Do NOT restore local_count here — the runtime function
                 * scope (scope_declare_name → find_function_scope) keeps
                 * all let/const/class/var names permanently, so removing
                 * block-scoped names from local_names would create a slot
                 * index mismatch for every subsequent variable.  The
                 * local_scope_depth guard already prevents direct slot
                 * access inside the block scope, so keeping the extra
                 * slots in local_names is harmless and keeps slot indices
                 * aligned with the runtime scope. */
                c->local_scope_depth--;
            }
        }
        break;

    case AST_EXPR_STMT:
        cexpr(c, n->u.expr_stmt.expr);
        emit(c, top ? BC_SET_RESULT : BC_POP);
        break;

    case AST_VAR_DECL:
        cvar_decl(c, n);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;

    case AST_IF: {
        cexpr(c, n->u.if_stmt.cond);
        int jfalse = emit_jump(c, BC_JUMP_IF_FALSE);
        cstmt(c, n->u.if_stmt.body, 0);
        if (n->u.if_stmt.else_body) {
            int jend = emit_jump(c, BC_JUMP);
            patch_here(c, jfalse);
            cstmt(c, n->u.if_stmt.else_body, 0);
            patch_here(c, jend);
        } else {
            patch_here(c, jfalse);
        }
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_FOR: {
        /* NOTE: no BC_SCOPE_ENTER here — `var` declarations in the init
         * must be hoisted to the function scope (JS semantics).  If we
         * wrapped the for-loop body in a scope, `var i` would be destroyed
         * on BC_SCOPE_LEAVE and become inaccessible after the loop.
         * `let`/`const` in for-init are delegated to the tree-walker
         * via emit_eval (cvar_decl checks for non-var kinds), so they
         * are not affected by this omission. */
        if (n->u.for_stmt.init) {
            if (n->u.for_stmt.init->type == AST_VAR_DECL) cvar_decl(c, n->u.for_stmt.init);
            else cstmt(c, n->u.for_stmt.init, 0);
        }
        BCLoop lp;
        loop_push(c, &lp, 0);

        int test_pos = here(c);
        emit(c, BC_LOOP_TICK);
        int exit_patch = -1;
        if (n->u.for_stmt.test) {
            /* Try to optimize `local < constant` into a single opcode.
             * NOTE: only valid when NO block scope is active — slot-based
             * reads hit interp->current_scope at runtime, but `var`
             * declarations are hoisted to the function scope.  With a
             * block scope active (e.g. a function body), current_scope is
             * the block scope whose slots do NOT hold the hoisted vars,
             * so the single-opcode test would read garbage and break the
             * loop (see the same local_scope_depth guard used by
             * BC_INC_LOCAL / BC_INC_LOCAL_DISCARD). */
            int optimized = 0;
            if (c->local_scope_depth == 0 && !c->disable_local_slots &&
                n->u.for_stmt.test->type == AST_BINARY &&
                n->u.for_stmt.test->u.binary.op[0] == '<' &&
                n->u.for_stmt.test->u.binary.op[1] == '\0' &&
                n->u.for_stmt.test->u.binary.left->type == AST_IDENTIFIER &&
                n->u.for_stmt.test->u.binary.right->type == AST_LITERAL &&
                n->u.for_stmt.test->u.binary.right->token.type == TOK_NUMBER) {
                const char *name = n->u.for_stmt.test->u.binary.left->u.ident.name;
                double imm_val = n->u.for_stmt.test->u.binary.right->u.number.num;
                int slot = -1;
                for (int i = 0; i < c->local_count; i++) {
                    if (c->local_names[i] == name ||
                        (c->local_names[i] && strcmp(c->local_names[i], name) == 0)) {
                        slot = i;
                        break;
                    }
                }
                if (slot >= 0 && imm_val == (int32_t)imm_val) {
                    emit(c, BC_JUMP_IF_LOCAL_LT_IMM);
                    emit16(c, (uint16_t)slot);
                    emit32(c, (int32_t)imm_val);
                    exit_patch = here(c);
                    emit32(c, 0);  /* placeholder offset */
                    optimized = 1;
                }
            }
            if (!optimized) {
                cexpr(c, n->u.for_stmt.test);
                exit_patch = emit_jump(c, BC_JUMP_IF_FALSE);
            }
        }
        cstmt(c, n->u.for_stmt.body, 0);

        int update_pos = here(c);
        if (n->u.for_stmt.update) {
            /* Check if update is a postfix increment on a local variable,
             * so we can use BC_INC_LOCAL_DISCARD to avoid push+pop */
            int use_discard = 0;
            if (c->local_scope_depth == 0 && !c->disable_local_slots &&
                n->u.for_stmt.update->type == AST_UNARY &&
                !n->u.for_stmt.update->u.unary.prefix &&
                n->u.for_stmt.update->u.unary.op[0] == '+' &&
                n->u.for_stmt.update->u.unary.arg &&
                n->u.for_stmt.update->u.unary.arg->type == AST_IDENTIFIER) {
                const char *name = n->u.for_stmt.update->u.unary.arg->u.ident.name;
                for (int i = 0; i < c->local_count; i++) {
                    if (c->local_names[i] == name ||
                        (c->local_names[i] && strcmp(c->local_names[i], name) == 0)) {
                        emit(c, BC_INC_LOCAL_DISCARD);
                        emit16(c, (uint16_t)i);
                        use_discard = 1;
                        break;
                    }
                }
            }
            if (!use_discard) {
                cexpr(c, n->u.for_stmt.update);
                emit(c, BC_POP);
            }
        }
        int back = emit_jump(c, BC_JUMP);
        patch_jump_to(c, back, test_pos);

        int end = here(c);
        if (exit_patch >= 0) patch_jump_to(c, exit_patch, end);
        loop_pop(c, &lp, end, update_pos);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_WHILE: {
        BCLoop lp;
        loop_push(c, &lp, 0);
        int test_pos = here(c);
        emit(c, BC_LOOP_TICK);
        /* Parser stores while's cond/body in the if_stmt union fields. */
        cexpr(c, n->u.if_stmt.cond);
        int exit_patch = emit_jump(c, BC_JUMP_IF_FALSE);
        cstmt(c, n->u.if_stmt.body, 0);
        int back = emit_jump(c, BC_JUMP);
        patch_jump_to(c, back, test_pos);
        int end = here(c);
        patch_jump_to(c, exit_patch, end);
        loop_pop(c, &lp, end, test_pos);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_DO_WHILE: {
        BCLoop lp;
        loop_push(c, &lp, 0);
        int body_pos = here(c);
        emit(c, BC_LOOP_TICK);
        /* Parser stores do-while's body/cond in the if_stmt union fields. */
        cstmt(c, n->u.if_stmt.body, 0);
        int test_pos = here(c);
        cexpr(c, n->u.if_stmt.cond);
        int back = emit_jump(c, BC_JUMP_IF_TRUE);
        patch_jump_to(c, back, body_pos);
        int end = here(c);
        loop_pop(c, &lp, end, test_pos);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_FOR_OF:
        cfor_of(c, n);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;

    case AST_FOR_IN: {
        /* for-in is always delegated to the tree-walker (eval_for_in), which
         * declares its loop binding in the runtime scope.  Register the name
         * here so slot indices stay aligned with the runtime scope. */
        ASTNode *each = n->u.for_in.each;
        if (each && each->type == AST_VAR_DECL && each->u.var_decl.nvars > 0) {
            ASTNode *d = each->u.var_decl.vars[0];
            if (d && d->type == AST_VAR_DECLARATOR && d->u.declarator.var &&
                d->u.declarator.var->type == AST_IDENTIFIER)
                register_local_name(c, d->u.declarator.var->u.ident.name);
        }
        emit_eval(c, n, 0);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_TRY: {
        /* try/catch is always delegated to the tree-walker (eval_try), which
         * declares the catch parameter in the function/global scope via
         * scope_declare_name (the engine treats let/const like var, so the
         * catch binding lands in the function scope, not a block scope).
         * Register the catch variable here so slot indices stay aligned with
         * the runtime scope; otherwise every variable declared after a
         * try/catch resolves to the wrong slot (e.g. `let a = 1` reads the
         * Error value stored in the catch parameter's slot). */
        char *cv = n->u.try_stmt.catch_var;
        if (cv) register_local_name(c, cv);
        emit_eval(c, n, 0);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_SWITCH:
        cswitch(c, n);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;

    case AST_BREAK: {
        const char *lbl = label_name(n->u.break_stmt.label);
        BCLoop *lp = find_target(c, lbl, 0);
        if (!lp) { c->ok = 0; break; }
        int pos = emit_jump(c, BC_JUMP);
        loop_add(c, &lp->brk, &lp->nbrk, &lp->cbrk, pos);
        break;
    }

    case AST_CONTINUE: {
        const char *lbl = label_name(n->u.continue_stmt.label);
        BCLoop *lp = find_target(c, lbl, 1);
        if (!lp || lp->is_switch) { c->ok = 0; break; }
        int pos = emit_jump(c, BC_JUMP);
        loop_add(c, &lp->cont, &lp->ncont, &lp->ccont, pos);
        break;
    }

    case AST_LABEL: {
        ASTNode *inner = n->u.label_stmt.stmt;
        const char *lbl = label_name(n->u.label_stmt.label);
        if (!lbl || !inner) { emit_eval(c, n, 0); break; }
        if (inner->type == AST_FOR || inner->type == AST_WHILE ||
            inner->type == AST_DO_WHILE || inner->type == AST_FOR_OF ||
            inner->type == AST_SWITCH) {
            c->pending_label = lbl;
            cstmt(c, inner, 0);
            c->pending_label = NULL;
        } else {
            emit_eval(c, n, 0);
        }
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_THROW:
        cexpr(c, n->u.throw_stmt.arg);
        emit(c, BC_THROW);
        break;

    case AST_RETURN:
        if (n->u.return_stmt.arg) cexpr(c, n->u.return_stmt.arg);
        else emit(c, BC_PUSH_UNDEFINED);
        emit(c, BC_RETURN);
        break;

    case AST_DEBUGGER:
        emit(c, BC_NOP);
        break;

    case AST_FUNC_DECL:
        /* Track function name in local slot table to keep bytecode compiler's
         * local_count in sync with the runtime scope's slot allocation.
         * eval_func_decl calls scope_declare_name(..., 0) which adds the
         * function name to the function scope — same as var.   Without this,
         * BC_LOAD_LOCAL/BC_STORE_LOCAL would use wrong slot indices for any
         * variable declared after a function declaration. */
        if (n->u.func.name && c->local_count < 256) {
            c->local_names[c->local_count++] = n->u.func.name;
            /* function declarations are hoisted like var */
            if (c->var_local_count < c->local_count)
                c->var_local_count = c->local_count;
        }
        emit_eval(c, n, 0);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;

    case AST_CLASS_DECL:
        /* Track class name in local slot table, mirroring AST_FUNC_DECL.
         * eval_class_decl calls scope_declare_name(..., 1) which adds the
         * class name to the function/global scope.  Without this the
         * compiler's local_names misses the class slots, so BC_LOAD_LOCAL /
         * BC_STORE_LOCAL would use wrong slot indices for every variable
         * declared after a class declaration. */
        if (n->u.class_decl.name && c->local_count < 256) {
            int found = 0;
            for (int j = 0; j < c->local_count; j++) {
                if (c->local_names[j] == n->u.class_decl.name ||
                    (c->local_names[j] &&
                     strcmp(c->local_names[j], n->u.class_decl.name) == 0)) {
                    found = 1;
                    break;
                }
            }
            if (!found)
                c->local_names[c->local_count++] = n->u.class_decl.name;
        }
        emit_eval(c, n, 0);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;

    /* Arrow function expression body: compile as expression with result */
    case AST_LITERAL: case AST_IDENTIFIER: case AST_BINARY: case AST_UNARY:
    case AST_CONDITIONAL: case AST_CALL: case AST_NEW: case AST_MEMBER:
    case AST_COMPUTED_MEMBER: case AST_ASSIGN: case AST_SEQUENCE:
    case AST_ARRAY: case AST_OBJECT: case AST_TEMPLATE: case AST_TAGGED_TEMPLATE:
    case AST_THIS: case AST_SUPER: case AST_SPREAD: case AST_SPREAD_ELEMENT:
    case AST_OPTIONAL_CALL: case AST_OPTIONAL_MEMBER:
        cexpr(c, n);
        emit(c, top ? BC_SET_RESULT : BC_POP);
        break;

    default:
        /* Functions, classes, try/catch, for-in, with, modules, … */
        emit_eval(c, n, 0);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }
}

static void bc_collect_vars_hoist(BCComp *c, ASTNode *n, int depth);

/* -- Entry point ------------------------------------------------------- */

/* A function body can use direct slot access (BC_LOAD_LOCAL/STORE_LOCAL)
 * only when the runtime function-scope layout is statically known to be:
 *     slot 0 = "this", slots 1..nparams = params, then locals.
 * This holds when every parameter is a plain identifier AND nothing else
 * is injected into the function scope between "this" and the locals
 * (derived class methods inject %superctor%/%superproto% at slots 1..2;
 * non-arrow functions that reference `arguments` bind the arguments
 * object after the params).  Otherwise we fall back to name-based lookup,
 * which is layout-independent and always correct. */
static int bc_func_slot_safe(ASTNode *func_node, ASTNode *body,
                             ASTNode **params, int nparams)
{
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        if (func_node->u.func.class_node &&
            func_node->u.func.class_node->u.class_decl.extends)
            return 0;                       /* super refs shift the slots */
        if (body && lr_ast_scans_arguments(body))
            return 0;                       /* arguments object shifts locals */
    }
    for (int i = 0; i < nparams; i++) {
        ASTNode *p = params ? params[i] : NULL;
        if (!p || p->type != AST_IDENTIFIER || !p->u.ident.name)
            return 0;                       /* destructuring/default/rest */
    }
    return 1;
}

/* Pre-visit to collect all var declarations for hoisting:
 * all var declarations are emitted (with undefined initial value)
 * at the top of the function scope before any executable code,
 * per JavaScript hoisting semantics. */
static void bc_collect_vars_hoist(BCComp *c, ASTNode *n, int depth)
{
    if (!n || !c || !c->ok) return;
    switch (n->type) {
    case AST_PROGRAM:
    case AST_BLOCK:
        for (int i = 0; i < n->u.list.count && c->ok; i++)
            bc_collect_vars_hoist(c, n->u.list.items[i], depth + 1);
        break;
    case AST_IF:
        bc_collect_vars_hoist(c, n->u.if_stmt.body, depth + 1);
        if (n->u.if_stmt.else_body)
            bc_collect_vars_hoist(c, n->u.if_stmt.else_body, depth + 1);
        break;
    case AST_FOR:
        if (n->u.for_stmt.init)
            bc_collect_vars_hoist(c, n->u.for_stmt.init, depth + 1);
        bc_collect_vars_hoist(c, n->u.for_stmt.body, depth + 1);
        break;
    case AST_WHILE:
    case AST_DO_WHILE:
        bc_collect_vars_hoist(c, n->u.if_stmt.body, depth + 1);
        break;
    case AST_FOR_IN:
        bc_collect_vars_hoist(c, n->u.for_in.body, depth + 1);
        break;
    case AST_FOR_OF:
        bc_collect_vars_hoist(c, n->u.for_of.body, depth + 1);
        break;
    case AST_SWITCH:
        for (int i = 0; i < n->u.switch_stmt.ncases; i++) {
            if (n->u.switch_stmt.cases[i])
                bc_collect_vars_hoist(c, n->u.switch_stmt.cases[i], depth + 1);
        }
        break;
    case AST_TRY:
        bc_collect_vars_hoist(c, n->u.try_stmt.body, depth + 1);
        if (n->u.try_stmt.catch_body)
            bc_collect_vars_hoist(c, n->u.try_stmt.catch_body, depth + 1);
        if (n->u.try_stmt.finally_body)
            bc_collect_vars_hoist(c, n->u.try_stmt.finally_body, depth + 1);
        break;
    case AST_VAR_DECL: {
        /* Only var (kind=0) needs function-level hoisting;
         * let/const are block-scoped and handled by proper scope entry/exit. */
        for (int i = 0; i < n->u.var_decl.nvars; i++) {
            ASTNode *d = n->u.var_decl.vars[i];
            if (!d || d->type != AST_VAR_DECLARATOR) continue;
            ASTNode *var = d->u.declarator.var;
            if (!var || var->type != AST_IDENTIFIER || !var->u.ident.name) continue;
            int kind = var_decl_kind(n);
            if (kind != 0) continue;  /* skip let/const */
            /* Check if we've already added this name (duplicate var is allowed).
             * Only add to local_names once so slot count doesn't explode. */
            int found = 0;
            for (int j = 0; j < c->local_count; j++) {
                if (c->local_names[j] == var->u.ident.name ||
                    (c->local_names[j] && strcmp(c->local_names[j], var->u.ident.name) == 0)) {
                    found = 1;
                    break;
                }
            }
            if (!found && c->local_count < 256) {
                c->local_names[c->local_count++] = var->u.ident.name;
                c->var_local_count = c->local_count;
                /* Emit the hoisted declaration: initial value is undefined. */
                int idx = pool_add_str(c, var->u.ident.name);
                emit(c, BC_PUSH_UNDEFINED);
                emit(c, BC_DECLARE_VAR);
                emit16(c, idx);
                emit(c, (uint8_t)kind);
            }
        }
        break;
    }
    default:
        /* Expression node that may appear in arrow function body context.
         * Just recurse to collect any var declarations inside. */
        break;
    }
}

static int bc_compile_ex(BCProgram *prog, ASTNode *node, int is_module,
                         const char *const *pre_names, int pre_count,
                         int disable_local_slots)
{
    (void)is_module;
    if (!prog || !node) return -1;

    BCComp c;
    memset(&c, 0, sizeof(c));
    c.p = prog;
    c.ok = 1;
    c.disable_local_slots = disable_local_slots;
    if (pre_names && pre_count > 0) {
        /* Pre-populate "this" + params.  Because slot == local_names index,
         * locals added afterwards get slot indices that line up with the
         * runtime function scope (locals start at index pre_count). */
        int n = pre_count > 256 ? 256 : pre_count;
        for (int i = 0; i < n; i++) c.local_names[i] = pre_names[i];
        c.local_count   = (uint16_t)n;
        c.local_base    = (uint16_t)n;
        c.var_local_count = (uint16_t)n;    /* "this"+params are hoisted, so
                                             * they act as a block-scope floor */
    }

    /* Hoist all var declarations to the top of the current compilation unit
     * (function body or top-level program), before any executable statements.
     * Per JS spec: "all var declarations are hoisted to the top of the
     * containing scope before any code is executed." */
    bc_collect_vars_hoist(&c, node, 0);

    cstmt(&c, node, node->type == AST_PROGRAM ? 0 : 1);
    emit(&c, BC_STOP);

    while (c.loop) {          /* defensive: release any dangling loop ctx */
        BCLoop *lp = c.loop;
        c.loop = lp->prev;
        free(lp->brk);
        free(lp->cont);
    }

    /* prog->local_count counts only the true locals (excluding the
     * pre-populated "this"+params) so the inline no_scope fast path
     * keeps working for empty functions. */
    prog->local_count = (uint16_t)(c.local_count >= c.local_base
                                     ? c.local_count - c.local_base
                                     : c.local_count);
    /* Scan for name-based reads/writes.  If the body only uses direct
     * slot access (BC_LOAD_LOCAL/BC_STORE_LOCAL), the inline-call path
     * can skip BOTH the parameter cache pre-population AND the cache
     * generation bump/restore — the callee never touches the variable
     * cache, so the caller's entries stay valid across the call. */
    prog->uses_name_ops = 0;
    for (int i = 0; i < c.p->code_len; i++) {
        uint8_t op = c.p->code[i];
        if (op == BC_LOAD_VAR || op == BC_STORE_VAR ||
            op == BC_DECLARE_VAR || op == BC_TYPEOF_VAR) {
            prog->uses_name_ops = 1;
            break;
        }
    }
    /* Scan for BC_EVAL_NODE: the only bytecode that can modify the
     * loop/control state on the Interpreter (break_target,
     * continue_target, return_target, has_returned, return_value,
     * pending_label).  Pure-bytecode bodies never touch these, so the
     * inline-call frame save/restore can skip them entirely. */
    prog->touches_eval_node = 0;
    for (int i = 0; i < c.p->code_len; i++) {
        if (c.p->code[i] == BC_EVAL_NODE || c.p->code[i] == BC_EVAL_NODE_POP) {
            prog->touches_eval_node = 1;
            break;
        }
    }
    /* Compute the `arguments` scan once at compile time.  The hot
     * inline-call path checks this flag instead of re-walking the AST
     * (lr_ast_scans_arguments) on every single call — that walk showed
     * up as 800k+ calls in profiles of function-call benchmarks. */
    prog->scans_arguments = lr_ast_scans_arguments(node) ? 1 : 0;
    /* IOME586 pure-function memoization: a function is memoizable when it
     * is inline-eligible AND its emitted bytecode shows no external
     * interference (only slot-based param/local access + primitive
     * arithmetic).  uses_name_ops / uses_this / scans_arguments /
     * touches_eval_node are final here; bc_code_is_pure rejects calls,
     * property access, closures, iteration, eval nodes and throws.  Such
     * a function's result depends only on its primitive arguments, so the
     * BC_CALL handler can replay a cached result instead of executing. */
    prog->is_pure = 0;
    if (prog->can_inline && !prog->uses_this &&
        !prog->scans_arguments && !prog->touches_eval_node &&
        bc_code_is_pure(c.p->code, c.p->code_len)) {
        prog->is_pure = 1;
    }
    prog->compiled = c.ok;
    prog->max_stack = 64;
    /* LARGE_SCRIPT: track high-water mark for stack pre-allocation */
    if (c.max_stack_depth > prog->max_stack)
        prog->max_stack = c.max_stack_depth;
    return c.ok ? 0 : -1;
}

int bc_compile(BCProgram *prog, ASTNode *node, int is_module)
{
    return bc_compile_ex(prog, node, is_module, NULL, 0, 0);
}

/* Compile a function body with the "this" + parameter slots pre-bound to
 * match the runtime function-scope layout.  Returns 0 on success, -1 on
 * failure (caller falls back to the tree-walker). */
int bc_compile_func(BCProgram *prog, ASTNode *func_node)
{
    if (!prog || !func_node) return -1;
    ASTNode *body = NULL;
    ASTNode **params = NULL;
    int nparams = 0;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        body   = func_node->u.func.body;
        nparams = func_node->u.func.nparams;
        params  = func_node->u.func.params;
    } else if (func_node->type == AST_ARROW) {
        body   = func_node->u.arrow.body;
        nparams = func_node->u.arrow.nparams;
        params  = func_node->u.arrow.params;
    } else {
        return bc_compile(prog, func_node, 0);
    }
    if (!body) return -1;

    if (!bc_func_slot_safe(func_node, body, params, nparams)) {
        /* Layout cannot be statically pinned — use name-based lookup
         * everywhere.  Still fully correct. */
        return bc_compile_ex(prog, body, 0, NULL, 0, 1);
    }

    const char *pre[256];
    int pre_count = 0;
    if (pre_count < 256) pre[pre_count++] = "this";
    for (int i = 0; i < nparams && pre_count < 256; i++)
        pre[pre_count++] = params[i]->u.ident.name;

    /* Cache inline-call metadata on the program so the hot BC_CALL
     * handler avoids re-walking the AST (extracting body/nparams/params
     * and validating every param is a plain identifier) on every call.
     * func_call benchmarks spent a large fraction of their time in that
     * per-call validation loop. */
    prog->nparams = (uint8_t)(nparams > 255 ? 255 : nparams);
    prog->can_inline = 1;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        if (func_node->u.func.is_generator || func_node->u.func.is_async)
            prog->can_inline = 0;
    } else if (func_node->type == AST_ARROW) {
        if (func_node->u.arrow.is_async)
            prog->can_inline = 0;
    }
    if (prog->can_inline)
        prog->can_inline = bc_func_slot_safe(func_node, body, params, nparams) ? 1 : 0;

    /* Store function name + slot for recursive call detection in JIT.
     * The function name is registered as local_names[pre_count] (slot == pre_count)
     * because "this" is slot 0 and params occupy slots 1..nparams. */
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        const char *fname = func_node->u.func.name;
        if (fname) {
            prog->func_name = fname;
            prog->func_name_slot = pre_count; /* slot index = number of pre-bound names */
        }
    }

    /* is_pure is computed in bc_compile_ex (after the body is compiled),
     * where the final opcode stream and all the scan flags are available. */
    return bc_compile_ex(prog, body, 0, pre, pre_count, 0);
}

