/*
 * LR_JS — Bytecode VM: AST→bytecode compiler + stack machine executor.
 *
 * All JavaScript operators and data handling implemented here are pure C:
 * numeric coercion, string concatenation, abstract/strict equality,
 * relational comparison, bitwise arithmetic, property and element access,
 * object/array construction, template literals and the for-of iteration
 * protocol never go back through the tree-walking interpreter.
 *
 * Constructs whose semantics live in the interpreter (closures, classes,
 * generators, async/await, try/catch, destructuring, modules) are lowered
 * to BC_EVAL_NODE and delegated per subtree, which keeps behaviour bit
 * identical with the tree-walker while everything around them still runs
 * in the VM.
 */
#include "lr_bytecode.h"
#include "lr_interp.h"
#include "lr_ast.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#ifndef NAN
#define NAN (0.0 / 0.0)
#endif

/* Fast free: only heap-backed tags need the call. */
#define FREE_IF_HEAP(ctx, v) do {                                   \
    if ((v).tag == LR_TYPE_STRING || (v).tag == LR_TYPE_OBJECT ||   \
        (v).tag == LR_TYPE_SYMBOL) lr_free_value(ctx, v);           \
} while (0)

/* Portable strdup (MSVC exposes _strdup, POSIX strdup; avoid both). */
static char *bc_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ═══════════════════════════════════════════════════════════════════════
   PROGRAM MANAGEMENT
   ═══════════════════════════════════════════════════════════════════════ */

BCProgram *bc_new_program(void)
{
    return (BCProgram *)calloc(1, sizeof(BCProgram));
}

void bc_free_program(BCProgram *prog)
{
    if (!prog) return;
    free(prog->code);
    for (int i = 0; i < prog->pool_count; i++)
        if (prog->pool[i].kind == BC_POOL_STRING)
            free(prog->pool[i].u.str);
    free(prog->pool);
    free(prog);
}

int bc_program_is_restorable(const BCProgram *prog)
{
    return prog && prog->node_refs == 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   COMPILER STATE
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct BCLoop {
    struct BCLoop *prev;
    const char    *label;      /* label attached to this loop (or NULL)   */
    int            is_switch;  /* `continue` is not valid for switch      */
    int           *brk;        /* pending break jump patch positions      */
    int            nbrk, cbrk;
    int           *cont;       /* pending continue jump patch positions   */
    int            ncont, ccont;
} BCLoop;

typedef struct {
    BCProgram  *p;
    BCLoop     *loop;
    const char *pending_label; /* label to attach to the next loop        */
    int         ok;            /* 0 → bail out, caller uses the tree-walker */
} BCComp;

/* ═══════════════════════════════════════════════════════════════════════
   EMISSION
   ═══════════════════════════════════════════════════════════════════════ */

static void bc_ensure(BCComp *c, int extra)
{
    BCProgram *p = c->p;
    if (p->code_len + extra <= p->code_cap) return;
    int cap = p->code_cap ? p->code_cap * 2 : 256;
    if (p->code_len + extra > cap) cap = p->code_len + extra + 64;
    uint8_t *nc = (uint8_t *)realloc(p->code, (size_t)cap);
    if (!nc) { c->ok = 0; return; }
    p->code = nc;
    p->code_cap = cap;
}

static void emit(BCComp *c, uint8_t byte)
{
    if (!c->ok) return;
    bc_ensure(c, 1);
    if (!c->ok) return;
    c->p->code[c->p->code_len++] = byte;
}

static void emit16(BCComp *c, int v)
{
    if (!c->ok) return;
    if (v < 0 || v > 0xFFFF) { c->ok = 0; return; }
    bc_ensure(c, 2);
    if (!c->ok) return;
    c->p->code[c->p->code_len++] = (uint8_t)(v & 0xFF);
    c->p->code[c->p->code_len++] = (uint8_t)((v >> 8) & 0xFF);
}

static void emit32(BCComp *c, int32_t v)
{
    if (!c->ok) return;
    bc_ensure(c, 4);
    if (!c->ok) return;
    uint8_t *d = c->p->code + c->p->code_len;
    d[0] = (uint8_t)(v & 0xFF);
    d[1] = (uint8_t)((v >> 8) & 0xFF);
    d[2] = (uint8_t)((v >> 16) & 0xFF);
    d[3] = (uint8_t)((v >> 24) & 0xFF);
    c->p->code_len += 4;
}

static int here(BCComp *c) { return c->p->code_len; }

/* Emit a jump with a placeholder target; returns the patch position. */
static int emit_jump(BCComp *c, uint8_t op)
{
    emit(c, op);
    int pos = here(c);
    emit32(c, 0);
    return pos;
}

static void patch_jump_to(BCComp *c, int pos, int target)
{
    if (!c->ok || pos < 0 || pos + 4 > c->p->code_len) return;
    int32_t rel = (int32_t)(target - (pos + 4));
    uint8_t *d = c->p->code + pos;
    d[0] = (uint8_t)(rel & 0xFF);
    d[1] = (uint8_t)((rel >> 8) & 0xFF);
    d[2] = (uint8_t)((rel >> 16) & 0xFF);
    d[3] = (uint8_t)((rel >> 24) & 0xFF);
}

static void patch_here(BCComp *c, int pos) { patch_jump_to(c, pos, here(c)); }

/* ═══════════════════════════════════════════════════════════════════════
   CONSTANT POOL
   ═══════════════════════════════════════════════════════════════════════ */

static int pool_reserve(BCComp *c)
{
    BCProgram *p = c->p;
    if (p->pool_count < p->pool_cap) return 1;
    int cap = p->pool_cap ? p->pool_cap * 2 : 32;
    BCConst *np = (BCConst *)realloc(p->pool, sizeof(BCConst) * (size_t)cap);
    if (!np) { c->ok = 0; return 0; }
    p->pool = np;
    p->pool_cap = cap;
    return 1;
}

static int pool_add_f64(BCComp *c, double d)
{
    if (!c->ok || !pool_reserve(c)) return 0;
    BCProgram *p = c->p;
    for (int i = 0; i < p->pool_count; i++)
        if (p->pool[i].kind == BC_POOL_FLOAT64 && p->pool[i].u.f64 == d) return i;
    int idx = p->pool_count++;
    p->pool[idx].kind = BC_POOL_FLOAT64;
    p->pool[idx].u.f64 = d;
    if (idx > 0xFFFF) c->ok = 0;
    return idx;
}

static int pool_add_str(BCComp *c, const char *s)
{
    if (!c->ok || !pool_reserve(c)) return 0;
    if (!s) s = "";
    BCProgram *p = c->p;
    for (int i = 0; i < p->pool_count; i++)
        if (p->pool[i].kind == BC_POOL_STRING && strcmp(p->pool[i].u.str, s) == 0)
            return i;
    int idx = p->pool_count++;
    p->pool[idx].kind = BC_POOL_STRING;
    p->pool[idx].u.str = bc_strdup(s);
    if (!p->pool[idx].u.str || idx > 0xFFFF) c->ok = 0;
    return idx;
}

static int pool_add_node(BCComp *c, void *node)
{
    if (!c->ok || !pool_reserve(c)) return 0;
    BCProgram *p = c->p;
    int idx = p->pool_count++;
    p->pool[idx].kind = BC_POOL_NODE;
    p->pool[idx].u.node = node;
    p->node_refs++;
    if (idx > 0xFFFF) c->ok = 0;
    return idx;
}

/* ═══════════════════════════════════════════════════════════════════════
   ESCAPE ANALYSIS

   Before a subtree is delegated to the tree-walking interpreter we must
   be sure it cannot transfer control *out* of itself: a `break` bound to
   a VM-compiled loop, a `return`, a `yield`/`await` would be invisible to
   the VM. When that happens the whole unit bails out to the interpreter.
   ═══════════════════════════════════════════════════════════════════════ */

static int escapes(ASTNode *n, int in_loop, int in_switch, int in_label);

static int escapes_list(ASTNode **items, int count, int l, int s, int lb)
{
    for (int i = 0; i < count; i++)
        if (escapes(items[i], l, s, lb)) return 1;
    return 0;
}

static int escapes(ASTNode *n, int in_loop, int in_switch, int in_label)
{
    if (!n) return 0;
    switch (n->type) {
    /* Opaque: their bodies belong to a different function. */
    case AST_FUNC_DECL: case AST_FUNC_EXPR: case AST_ARROW:
    case AST_CLASS_DECL: case AST_CLASS_BODY:
        return 0;

    case AST_RETURN:
    case AST_AWAIT:
    case AST_YIELD:
        return 1;

    case AST_BREAK: {
        ASTNode *lbl = n->u.break_stmt.label;
        if (lbl) return in_label ? 0 : 1;
        return (in_loop || in_switch) ? 0 : 1;
    }
    case AST_CONTINUE: {
        ASTNode *lbl = n->u.continue_stmt.label;
        if (lbl) return in_label ? 0 : 1;
        return in_loop ? 0 : 1;
    }

    case AST_PROGRAM: case AST_BLOCK:
        return escapes_list(n->u.list.items, n->u.list.count, in_loop, in_switch, in_label);

    case AST_EXPR_STMT: return escapes(n->u.expr_stmt.expr, in_loop, in_switch, in_label);
    case AST_IF:
        return escapes(n->u.if_stmt.cond, in_loop, in_switch, in_label) ||
               escapes(n->u.if_stmt.body, in_loop, in_switch, in_label) ||
               escapes(n->u.if_stmt.else_body, in_loop, in_switch, in_label);
    case AST_FOR:
        return escapes(n->u.for_stmt.init, in_loop, in_switch, in_label) ||
               escapes(n->u.for_stmt.test, in_loop, in_switch, in_label) ||
               escapes(n->u.for_stmt.update, 1, in_switch, in_label) ||
               escapes(n->u.for_stmt.body, 1, in_switch, in_label);
    case AST_WHILE: case AST_DO_WHILE:
        return escapes(n->u.for_stmt.test, in_loop, in_switch, in_label) ||
               escapes(n->u.for_stmt.body, 1, in_switch, in_label);
    case AST_FOR_IN:
        return escapes(n->u.for_in.source, in_loop, in_switch, in_label) ||
               escapes(n->u.for_in.body, 1, in_switch, in_label);
    case AST_FOR_OF:
        return escapes(n->u.for_of.source, in_loop, in_switch, in_label) ||
               escapes(n->u.for_of.body, 1, in_switch, in_label);
    case AST_SWITCH:
        if (escapes(n->u.switch_stmt.test, in_loop, in_switch, in_label)) return 1;
        return escapes_list(n->u.switch_stmt.cases, n->u.switch_stmt.ncases,
                            in_loop, 1, in_label);
    case AST_CASE: case AST_DEFAULT:
        return escapes(n->u.if_stmt.cond, in_loop, in_switch, in_label) ||
               escapes(n->u.if_stmt.body, in_loop, in_switch, in_label);
    case AST_LABEL:
        return escapes(n->u.label_stmt.stmt, in_loop, in_switch, 1);
    case AST_WITH:
        return escapes(n->u.with_stmt.obj, in_loop, in_switch, in_label) ||
               escapes(n->u.with_stmt.body, in_loop, in_switch, in_label);
    case AST_TRY:
        return escapes(n->u.try_stmt.body, in_loop, in_switch, in_label) ||
               escapes(n->u.try_stmt.catch_body, in_loop, in_switch, in_label) ||
               escapes(n->u.try_stmt.finally_body, in_loop, in_switch, in_label);
    case AST_THROW:
        return escapes(n->u.throw_stmt.arg, in_loop, in_switch, in_label);

    case AST_VAR_DECL:
        return escapes_list(n->u.var_decl.vars, n->u.var_decl.nvars,
                            in_loop, in_switch, in_label);
    case AST_VAR_DECLARATOR:
        return escapes(n->u.declarator.var, in_loop, in_switch, in_label) ||
               escapes(n->u.declarator.init, in_loop, in_switch, in_label);

    case AST_BINARY:
        return escapes(n->u.binary.left, in_loop, in_switch, in_label) ||
               escapes(n->u.binary.right, in_loop, in_switch, in_label);
    case AST_UNARY:  return escapes(n->u.unary.arg, in_loop, in_switch, in_label);
    case AST_CONDITIONAL:
        return escapes(n->u.conditional.cond, in_loop, in_switch, in_label) ||
               escapes(n->u.conditional.consequent, in_loop, in_switch, in_label) ||
               escapes(n->u.conditional.alternate, in_loop, in_switch, in_label);
    case AST_CALL: case AST_OPTIONAL_CALL:
        return escapes(n->u.call.callee, in_loop, in_switch, in_label) ||
               escapes_list(n->u.call.args, n->u.call.argc, in_loop, in_switch, in_label);
    case AST_NEW:
        return escapes(n->u.new_expr.callee, in_loop, in_switch, in_label) ||
               escapes_list(n->u.new_expr.args, n->u.new_expr.argc, in_loop, in_switch, in_label);
    case AST_MEMBER: case AST_COMPUTED_MEMBER: case AST_OPTIONAL_MEMBER:
        return escapes(n->u.member.obj, in_loop, in_switch, in_label) ||
               escapes(n->u.member.prop, in_loop, in_switch, in_label);
    case AST_ASSIGN:
        return escapes(n->u.assign.target, in_loop, in_switch, in_label) ||
               escapes(n->u.assign.value, in_loop, in_switch, in_label);
    case AST_SEQUENCE:
        return escapes_list(n->u.sequence.exprs, n->u.sequence.count,
                            in_loop, in_switch, in_label);
    case AST_ARRAY:
        return escapes_list(n->u.array.elements, n->u.array.nelem,
                            in_loop, in_switch, in_label);
    case AST_OBJECT:
        return escapes_list(n->u.object.props, n->u.object.nprops,
                            in_loop, in_switch, in_label);
    case AST_PROPERTY:
        return escapes(n->u.property.key, in_loop, in_switch, in_label) ||
               escapes(n->u.property.val, in_loop, in_switch, in_label);
    case AST_SPREAD: case AST_SPREAD_ELEMENT: case AST_REST:
        return escapes(n->u.spread.arg, in_loop, in_switch, in_label);
    case AST_TEMPLATE: case AST_TAGGED_TEMPLATE:
        return escapes(n->u.template_lit.tag, in_loop, in_switch, in_label) ||
               escapes_list(n->u.template_lit.exprs, n->u.template_lit.nexp,
                            in_loop, in_switch, in_label);
    case AST_PATTERN:
        return escapes_list(n->u.pattern_array.elements, n->u.pattern_array.nelem,
                            in_loop, in_switch, in_label);
    case AST_DEFAULT_VALUE:
        return escapes(n->u.default_val.left, in_loop, in_switch, in_label) ||
               escapes(n->u.default_val.right, in_loop, in_switch, in_label);

    /* Leaves and declarations without inner control flow. */
    case AST_LITERAL: case AST_IDENTIFIER: case AST_THIS: case AST_SUPER:
    case AST_DEBUGGER: case AST_IMPORT: case AST_EXPORT: case AST_EXPORT_DEFAULT:
    case AST_EXPORT_NAMED: case AST_EXPORT_ALL: case AST_IMPORT_SPECIFIER:
    case AST_IMPORT_NAMESPACE: case AST_TEMPLATE_PART:
        return 0;
    default:
        return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   COMPILER
   ═══════════════════════════════════════════════════════════════════════ */

static void cexpr(BCComp *c, ASTNode *n);
static void cstmt(BCComp *c, ASTNode *n, int top);

/* Delegate a subtree to the tree-walking interpreter. */
static void emit_eval(BCComp *c, ASTNode *n, int keep_value)
{
    if (!c->ok) return;
    if (escapes(n, 0, 0, 0)) { c->ok = 0; return; }
    int idx = pool_add_node(c, n);
    emit(c, keep_value ? BC_EVAL_NODE : BC_EVAL_NODE_POP);
    emit16(c, idx);
}

static void emit_push_string(BCComp *c, const char *s)
{
    int idx = pool_add_str(c, s);
    emit(c, BC_PUSH_STRING);
    emit16(c, idx);
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
}

static void emit_name_op(BCComp *c, uint8_t op, const char *name)
{
    int idx = pool_add_str(c, name);
    emit(c, op);
    emit16(c, idx);
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
    if (!strcmp(op, ">>")) return BC_SHR;
    if (!strcmp(op, ">>>")) return BC_SAR;
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

/* Does the argument list contain a spread element? */
static int has_spread(ASTNode **args, int argc)
{
    for (int i = 0; i < argc; i++)
        if (args[i] && (args[i]->type == AST_SPREAD_ELEMENT ||
                        args[i]->type == AST_SPREAD)) return 1;
    return 0;
}

/* ── Expressions ─────────────────────────────────────────────────────── */

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
        emit(c, BC_PUSH_THIS);
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

/* ── Loop bookkeeping ────────────────────────────────────────────────── */

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

/* ── Statements ──────────────────────────────────────────────────────── */

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
    if (!bind) { emit_eval(c, n, 0); return; }

    cexpr(c, n->u.for_of.source);
    emit(c, BC_SCOPE_ENTER);
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
    emit(c, BC_SCOPE_LEAVE);
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
        for (int i = 0; i < n->u.list.count; i++)
            cstmt(c, n->u.list.items[i], 1);
        break;

    case AST_BLOCK:
        emit(c, BC_SCOPE_ENTER);
        for (int i = 0; i < n->u.list.count; i++)
            cstmt(c, n->u.list.items[i], 0);
        emit(c, BC_SCOPE_LEAVE);
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
        emit(c, BC_SCOPE_ENTER);
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
            cexpr(c, n->u.for_stmt.test);
            exit_patch = emit_jump(c, BC_JUMP_IF_FALSE);
        }
        cstmt(c, n->u.for_stmt.body, 0);

        int update_pos = here(c);
        if (n->u.for_stmt.update) {
            cexpr(c, n->u.for_stmt.update);
            emit(c, BC_POP);
        }
        int back = emit_jump(c, BC_JUMP);
        patch_jump_to(c, back, test_pos);

        int end = here(c);
        if (exit_patch >= 0) patch_jump_to(c, exit_patch, end);
        loop_pop(c, &lp, end, update_pos);
        emit(c, BC_SCOPE_LEAVE);
        if (top) emit(c, BC_CLEAR_RESULT);
        break;
    }

    case AST_WHILE: {
        BCLoop lp;
        loop_push(c, &lp, 0);
        int test_pos = here(c);
        emit(c, BC_LOOP_TICK);
        cexpr(c, n->u.for_stmt.test);
        int exit_patch = emit_jump(c, BC_JUMP_IF_FALSE);
        cstmt(c, n->u.for_stmt.body, 0);
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
        cstmt(c, n->u.for_stmt.body, 0);
        int test_pos = here(c);
        cexpr(c, n->u.for_stmt.test);
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

/* ── Entry point ─────────────────────────────────────────────────────── */

int bc_compile(BCProgram *prog, ASTNode *node, int is_module)
{
    (void)is_module;
    if (!prog || !node) return -1;

    BCComp c;
    memset(&c, 0, sizeof(c));
    c.p = prog;
    c.ok = 1;

    cstmt(&c, node, node->type == AST_PROGRAM ? 0 : 1);
    emit(&c, BC_STOP);

    while (c.loop) {          /* defensive: release any dangling loop ctx */
        BCLoop *lp = c.loop;
        c.loop = lp->prev;
        free(lp->brk);
        free(lp->cont);
    }

    prog->compiled = c.ok;
    prog->max_stack = 64;
    return c.ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════
   C-LEVEL JAVASCRIPT VALUE OPERATIONS

   These are the semantics of the language expressed directly in C: no
   AST, no interpreter round-trip.
   ═══════════════════════════════════════════════════════════════════════ */

static double bcv_to_number(LRContext *ctx, LRValue v)
{
    double d = 0;
    lr_to_float64(ctx, &d, v);
    return d;
}

static int32_t bcv_to_int32(LRContext *ctx, LRValue v)
{
    int32_t i = 0;
    lr_to_int32(ctx, &i, v);
    return i;
}

/* Box a double, preferring the int32 representation (matches the
 * tree-walking interpreter and keeps the integer fast path hot). */
static LRValue bcv_number(LRContext *ctx, double d)
{
    if (d == (double)(int32_t)d && !isnan(d) && !isinf(d))
        return lr_new_int32(ctx, (int32_t)d);
    return lr_new_float64(ctx, d);
}

static int bcv_strict_eq(LRValue a, LRValue b)
{
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case LR_TYPE_UNDEFINED: return 1;
    case LR_TYPE_NULL:      return 1;
    case LR_TYPE_BOOL:      return a.u.bool_val == b.u.bool_val;
    case LR_TYPE_INT32:     return a.u.int32 == b.u.int32;
    case LR_TYPE_FLOAT64:   return a.u.float64 == b.u.float64;
    case LR_TYPE_STRING: {
        LRString *sa = (LRString *)a.u.ptr;
        LRString *sb = (LRString *)b.u.ptr;
        if (sa == sb) return 1;
        if (!sa || !sb) return 0;
        if (sa->len != sb->len) return 0;
        return memcmp(sa->str, sb->str, sa->len) == 0;
    }
    case LR_TYPE_OBJECT:    return a.u.ptr == b.u.ptr;
    case LR_TYPE_SYMBOL:    return a.u.ptr == b.u.ptr;
    default:                return 0;
    }
}

static int bcv_is_number(LRValue v)
{
    return v.tag == LR_TYPE_INT32 || v.tag == LR_TYPE_FLOAT64;
}

static int bcv_abstract_eq(LRContext *ctx, LRValue a, LRValue b)
{
    if (a.tag == b.tag) return bcv_strict_eq(a, b);

    if ((a.tag == LR_TYPE_UNDEFINED && b.tag == LR_TYPE_NULL) ||
        (a.tag == LR_TYPE_NULL && b.tag == LR_TYPE_UNDEFINED)) return 1;

    /* number <-> string, boolean coercions */
    if ((bcv_is_number(a) && b.tag == LR_TYPE_STRING) ||
        (a.tag == LR_TYPE_STRING && bcv_is_number(b)) ||
        a.tag == LR_TYPE_BOOL || b.tag == LR_TYPE_BOOL) {
        if (a.tag == LR_TYPE_UNDEFINED || a.tag == LR_TYPE_NULL ||
            b.tag == LR_TYPE_UNDEFINED || b.tag == LR_TYPE_NULL) return 0;
        double da = bcv_to_number(ctx, a);
        double db = bcv_to_number(ctx, b);
        if (isnan(da) || isnan(db)) return 0;
        return da == db;
    }

    /* object <-> primitive: compare string forms */
    if (a.tag == LR_TYPE_OBJECT && (b.tag == LR_TYPE_STRING || bcv_is_number(b))) {
        const char *s = lr_to_cstring(ctx, a);
        LRValue prim = lr_new_string(ctx, s ? s : "");
        lr_free_cstring(ctx, s);
        int r = bcv_abstract_eq(ctx, prim, b);
        lr_free_value(ctx, prim);
        return r;
    }
    if (b.tag == LR_TYPE_OBJECT && (a.tag == LR_TYPE_STRING || bcv_is_number(a))) {
        const char *s = lr_to_cstring(ctx, b);
        LRValue prim = lr_new_string(ctx, s ? s : "");
        lr_free_cstring(ctx, s);
        int r = bcv_abstract_eq(ctx, a, prim);
        lr_free_value(ctx, prim);
        return r;
    }
    return 0;
}

/* String concatenation entirely in C with a fast path for the extremely
 * common pattern: (string) + (number) or (string) + (string), which avoids
 * an extra strdup/free pair by reading the left string's buffer directly. */
static LRValue bcv_concat(LRContext *ctx, LRValue a, LRValue b)
{
    /* Optimized: build LRString directly, avoiding intermediate buffer.
     * For string+string: 1 malloc + 2 memcpy (was 2 malloc + 3 memcpy). */
    if (a.tag == LR_TYPE_STRING) {
        LRString *as = (LRString *)a.u.ptr;
        size_t la = as ? as->len : 0;

        if (b.tag == LR_TYPE_STRING) {
            LRString *bs = (LRString *)b.u.ptr;
            size_t lb = bs ? bs->len : 0;
            size_t total = la + lb;
            LRString *os = (LRString *)malloc(sizeof(LRString) + total + 1);
            if (!os) return lr_new_string(ctx, "");
            if (la) memcpy(os->str, as->str, la);
            if (lb) memcpy(os->str + la, bs->str, lb);
            os->str[total] = '\0';
            os->len = (uint32_t)total;
            os->ref_count = 1;
            os->is_atom = 0;
            LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os; return r;
        }

        const char *sb = lr_to_cstring(ctx, b);
        size_t lb = sb ? strlen(sb) : 0;
        size_t total = la + lb;
        LRString *os = (LRString *)malloc(sizeof(LRString) + total + 1);
        LRValue out = LR_VALUE_UNDEFINED;
        if (os) {
            if (la) memcpy(os->str, as->str, la);
            if (lb) memcpy(os->str + la, sb, lb);
            os->str[total] = '\0';
            os->len = (uint32_t)total;
            os->ref_count = 1;
            os->is_atom = 0;
            out.tag = LR_TYPE_STRING; out.u.ptr = os;
        }
        lr_free_cstring(ctx, sb);
        return out.tag != LR_TYPE_UNDEFINED ? out : lr_new_string(ctx, "");
    }
    {
        const char *sa = lr_to_cstring(ctx, a);
        const char *sb = lr_to_cstring(ctx, b);
        size_t la = sa ? strlen(sa) : 0;
        size_t lb = sb ? strlen(sb) : 0;
        size_t total = la + lb;
        LRString *os = (LRString *)malloc(sizeof(LRString) + total + 1);
        LRValue out = LR_VALUE_UNDEFINED;
        if (os) {
            if (la) memcpy(os->str, sa, la);
            if (lb) memcpy(os->str + la, sb, lb);
            os->str[total] = '\0';
            os->len = (uint32_t)total;
            os->ref_count = 1;
            os->is_atom = 0;
            out.tag = LR_TYPE_STRING; out.u.ptr = os;
        } else {
            out = lr_new_string(ctx, "");
        }
        lr_free_cstring(ctx, sa);
        lr_free_cstring(ctx, sb);
        return out;
    }
}

/* Relational comparison with proper string ordering. op: 0 '<' 1 '>' 2 '<=' 3 '>=' */
static int bcv_relational(LRContext *ctx, LRValue a, LRValue b, int op)
{
    if (a.tag == LR_TYPE_STRING && b.tag == LR_TYPE_STRING) {
        LRString *sa = (LRString *)a.u.ptr;
        LRString *sb = (LRString *)b.u.ptr;
        size_t na = sa ? sa->len : 0, nb = sb ? sb->len : 0;
        size_t nmin = na < nb ? na : nb;
        int cmp = nmin ? memcmp(sa->str, sb->str, nmin) : 0;
        if (cmp == 0) cmp = (na == nb) ? 0 : (na < nb ? -1 : 1);
        switch (op) {
        case 0: return cmp < 0;
        case 1: return cmp > 0;
        case 2: return cmp <= 0;
        default: return cmp >= 0;
        }
    }
    double da = bcv_to_number(ctx, a);
    double db = bcv_to_number(ctx, b);
    if (isnan(da) || isnan(db)) return 0;
    switch (op) {
    case 0: return da < db;
    case 1: return da > db;
    case 2: return da <= db;
    default: return da >= db;
    }
}

static const char *bcv_typeof(LRContext *ctx, LRValue v)
{
    switch (v.tag) {
    case LR_TYPE_UNDEFINED: return "undefined";
    case LR_TYPE_NULL:      return "object";
    case LR_TYPE_BOOL:      return "boolean";
    case LR_TYPE_INT32:
    case LR_TYPE_FLOAT64:   return "number";
    case LR_TYPE_STRING:    return "string";
    case LR_TYPE_SYMBOL:    return "symbol";
    case LR_TYPE_OBJECT: {
        LRObject *o = (LRObject *)v.u.ptr;
        if (o && o->type == LR_OBJ_BIGINT) return "bigint";
        return lr_is_function(ctx, v) ? "function" : "object";
    }
    default: return "undefined";
    }
}

static int bcv_instanceof(LRContext *ctx, LRValue a, LRValue b)
{
    if (!lr_is_object(a)) return 0;
    LRValue proto = lr_get_property_str(ctx, b, "prototype");
    if (!lr_is_object(proto)) { lr_free_value(ctx, proto); return 0; }
    int found = 0;
    LRValue p = lr_get_prototype(ctx, a);
    while (lr_is_object(p)) {
        if (bcv_strict_eq(p, proto)) { found = 1; break; }
        LRValue next = lr_get_prototype(ctx, p);
        lr_free_value(ctx, p);
        p = next;
    }
    lr_free_value(ctx, p);
    lr_free_value(ctx, proto);
    return found;
}

/* Full binary operator dispatch. Consumes a and b, returns a new value. */
static LRValue bcv_binop(Interpreter *interp, int op, LRValue a, LRValue b)
{
    LRContext *ctx = interp->ctx;
    LRValue r = LR_VALUE_UNDEFINED;

    /* ── int32 fast path ─────────────────────────────────────────────── */
    if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
        int32_t x = a.u.int32, y = b.u.int32;
        switch (op) {
        case BC_ADD: return lr_new_int32(ctx, x + y);
        case BC_SUB: return lr_new_int32(ctx, x - y);
        case BC_MUL: {
            double d = (double)x * (double)y;
            return bcv_number(ctx, d);
        }
        case BC_BIT_AND: return lr_new_int32(ctx, x & y);
        case BC_BIT_OR:  return lr_new_int32(ctx, x | y);
        case BC_BIT_XOR: return lr_new_int32(ctx, x ^ y);
        case BC_SHL: return lr_new_int32(ctx, (int32_t)((uint32_t)x << (y & 31)));
        case BC_SHR: return lr_new_int32(ctx, x >> (y & 31));
        case BC_SAR: return lr_new_int32(ctx, (int32_t)((uint32_t)x >> (y & 31)));
        case BC_LT:  return lr_new_bool(ctx, x <  y);
        case BC_GT:  return lr_new_bool(ctx, x >  y);
        case BC_LE:  return lr_new_bool(ctx, x <= y);
        case BC_GE:  return lr_new_bool(ctx, x >= y);
        case BC_EQ:  case BC_STRICT_EQ: return lr_new_bool(ctx, x == y);
        case BC_NE:  case BC_STRICT_NE: return lr_new_bool(ctx, x != y);
        default: break;
        }
    }

    switch (op) {
    case BC_ADD:
        if (lr_is_string(a) || lr_is_string(b)) {
            r = bcv_concat(ctx, a, b);
        } else if (lr_is_object(a) || lr_is_object(b)) {
            /* ToPrimitive: objects that stringify are concatenated, the
             * remainder falls back to numeric addition. */
            LRObject *oa = lr_is_object(a) ? (LRObject *)a.u.ptr : NULL;
            LRObject *ob = lr_is_object(b) ? (LRObject *)b.u.ptr : NULL;
            int numeric = (!oa || oa->type == LR_OBJ_BIGINT) &&
                          (!ob || ob->type == LR_OBJ_BIGINT);
            if (numeric) r = bcv_number(ctx, bcv_to_number(ctx, a) + bcv_to_number(ctx, b));
            else         r = bcv_concat(ctx, a, b);
        } else {
            r = bcv_number(ctx, bcv_to_number(ctx, a) + bcv_to_number(ctx, b));
        }
        break;
    case BC_SUB: r = bcv_number(ctx, bcv_to_number(ctx, a) - bcv_to_number(ctx, b)); break;
    case BC_MUL: r = bcv_number(ctx, bcv_to_number(ctx, a) * bcv_to_number(ctx, b)); break;
    case BC_DIV: r = lr_new_float64(ctx, bcv_to_number(ctx, a) / bcv_to_number(ctx, b)); break;
    case BC_MOD: {
        double da = bcv_to_number(ctx, a), db = bcv_to_number(ctx, b);
        r = (db == 0.0) ? lr_new_float64(ctx, NAN) : bcv_number(ctx, fmod(da, db));
        break;
    }
    case BC_POW: r = lr_new_float64(ctx, pow(bcv_to_number(ctx, a), bcv_to_number(ctx, b))); break;

    case BC_LT: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 0)); break;
    case BC_GT: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 1)); break;
    case BC_LE: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 2)); break;
    case BC_GE: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 3)); break;

    case BC_EQ: r = lr_new_bool(ctx, bcv_abstract_eq(ctx, a, b)); break;
    case BC_NE: r = lr_new_bool(ctx, !bcv_abstract_eq(ctx, a, b)); break;
    case BC_STRICT_EQ: r = lr_new_bool(ctx, bcv_strict_eq(a, b)); break;
    case BC_STRICT_NE: r = lr_new_bool(ctx, !bcv_strict_eq(a, b)); break;

    case BC_SHL: r = lr_new_int32(ctx, (int32_t)((uint32_t)bcv_to_int32(ctx, a) << (bcv_to_int32(ctx, b) & 31))); break;
    case BC_SHR: r = lr_new_int32(ctx, bcv_to_int32(ctx, a) >> (bcv_to_int32(ctx, b) & 31)); break;
    case BC_SAR: r = lr_new_int32(ctx, (int32_t)((uint32_t)bcv_to_int32(ctx, a) >> (bcv_to_int32(ctx, b) & 31))); break;
    case BC_BIT_AND: r = lr_new_int32(ctx, bcv_to_int32(ctx, a) & bcv_to_int32(ctx, b)); break;
    case BC_BIT_OR:  r = lr_new_int32(ctx, bcv_to_int32(ctx, a) | bcv_to_int32(ctx, b)); break;
    case BC_BIT_XOR: r = lr_new_int32(ctx, bcv_to_int32(ctx, a) ^ bcv_to_int32(ctx, b)); break;

    case BC_IN: {
        if (!lr_is_object(b)) {
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "right-hand side of 'in' must be an object");
            interp->error_flag = 1;
            break;
        }
        const char *prop = lr_to_cstring(ctx, a);
        LRString *atom = lr_new_atom(ctx, prop ? prop : "");
        r = lr_new_bool(ctx, lr_has_property(ctx, b, atom));
        lr_free_cstring(ctx, prop);
        break;
    }
    case BC_INSTANCEOF:
        r = lr_new_bool(ctx, bcv_instanceof(ctx, a, b));
        break;
    default:
        break;
    }
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════
   EXECUTOR — Direct/Indirect Threaded Bytecode Interpreter

   Direct threading (GCC/Clang): computed goto (&&label) — zero overhead.
   Indirect threading (MSVC): switch-based dispatch loop.

   v0.1.1+: SOLE execution engine. AST tree-walking interpreter is retired.
   ═══════════════════════════════════════════════════════════════════════ */

/* ── Threading mode ──────────────────────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
  #define LR_THREADED_CODE 1
#else
  #define LR_THREADED_CODE 0
#endif

static uint16_t rd16(uint8_t **ip)
{
    uint16_t v = (uint16_t)((*ip)[0] | ((*ip)[1] << 8));
    *ip += 2;
    return v;
}

static int32_t rd32(uint8_t **ip)
{
    int32_t v = (int32_t)((uint32_t)(*ip)[0] | ((uint32_t)(*ip)[1] << 8) |
                          ((uint32_t)(*ip)[2] << 16) | ((uint32_t)(*ip)[3] << 24));
    *ip += 4;
    return v;
}

LRValue bc_execute(BCProgram *prog, LRContext *ctx)
{
    if (!prog || !prog->code || !ctx || !prog->compiled) return LR_VALUE_UNDEFINED;
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp) return LR_VALUE_UNDEFINED;

    int cap = prog->max_stack > 64 ? prog->max_stack : 64;
    LRValue *stack = (LRValue *)malloc(sizeof(LRValue) * (size_t)cap);
    if (!stack) return LR_VALUE_UNDEFINED;
    int sp = 0;
    int scope_depth = 0;
    LRValue result = LR_VALUE_UNDEFINED;
    uint8_t *ip = prog->code;
    uint8_t *code_end = prog->code + prog->code_len;

#define VM_GROW() do {                                                     \
        if (sp >= cap) {                                                   \
            int ncap = cap * 2;                                            \
            LRValue *ns = (LRValue *)realloc(stack, sizeof(LRValue) * (size_t)ncap); \
            if (!ns) goto vm_abort;                                        \
            stack = ns; cap = ncap;                                        \
        }                                                                  \
    } while (0)
#define PUSH(v) do { VM_GROW(); stack[sp++] = (v); } while (0)
#define POP()   (sp > 0 ? stack[--sp] : LR_VALUE_UNDEFINED)
#define CHECK() do { if (interp->error_flag || interp->exception_pending ||  \
                        interp->has_returned || interp->break_target ||      \
                        interp->continue_target) goto vm_abort; } while (0)

#if LR_THREADED_CODE
    /* ── Direct threaded dispatch table ──────────────────────────────── */
    static const void *dispatch[BC_OPCODE_COUNT];
    static int dispatch_init = 0;
    if (!dispatch_init) {
        #define DOP(op, lbl) dispatch[op] = &&lbl_##lbl;
        DOP(BC_STOP,           stop)
        DOP(BC_NOP,            nop)
        DOP(BC_PUSH_UNDEFINED, push_undefined)
        DOP(BC_PUSH_NULL,      push_null)
        DOP(BC_PUSH_TRUE,      push_true)
        DOP(BC_PUSH_FALSE,     push_false)
        DOP(BC_PUSH_THIS,      push_this)
        DOP(BC_PUSH_INT32,     push_int32)
        DOP(BC_PUSH_FLOAT64,   push_float64)
        DOP(BC_PUSH_STRING,    push_string)
        DOP(BC_POP,            pop)
        DOP(BC_DUP,            dup)
        DOP(BC_DUP2,           dup2)
        DOP(BC_SWAP,           swap)
        DOP(BC_ROT3,           rot3)
        DOP(BC_LOAD_VAR,       load_var)
        DOP(BC_STORE_VAR,      store_var)
        DOP(BC_DECLARE_VAR,    declare_var)
        DOP(BC_TYPEOF_VAR,     typeof_var)
        DOP(BC_ADD,            add)
        DOP(BC_SUB,            sub)
        DOP(BC_MUL,            mul)
        DOP(BC_DIV,            div)
        DOP(BC_MOD,            mod)
        DOP(BC_POW,            pow)
        DOP(BC_LT,             lt)
        DOP(BC_GT,             gt)
        DOP(BC_LE,             le)
        DOP(BC_GE,             ge)
        DOP(BC_EQ,             eq)
        DOP(BC_NE,             ne)
        DOP(BC_STRICT_EQ,      strict_eq)
        DOP(BC_STRICT_NE,      strict_ne)
        DOP(BC_SHL,            shl)
        DOP(BC_SHR,            shr)
        DOP(BC_SAR,            sar)
        DOP(BC_BIT_AND,        bit_and)
        DOP(BC_BIT_OR,         bit_or)
        DOP(BC_BIT_XOR,        bit_xor)
        DOP(BC_IN,             in_op)
        DOP(BC_INSTANCEOF,     instanceof)
        DOP(BC_NEG,            neg)
        DOP(BC_POS,            pos)
        DOP(BC_NOT,            not_op)
        DOP(BC_BIT_NOT,        bit_not)
        DOP(BC_TYPEOF,         typeof_op)
        DOP(BC_VOID,           void_op)
        DOP(BC_JUMP,           jump)
        DOP(BC_JUMP_IF_FALSE,  jump_if_false)
        DOP(BC_JUMP_IF_TRUE,   jump_if_true)
        DOP(BC_JUMP_IF_FALSE_KEEP, jump_if_false_keep)
        DOP(BC_JUMP_IF_TRUE_KEEP,  jump_if_true_keep)
        DOP(BC_JUMP_IF_NOT_NULLISH, jump_if_not_nullish)
        DOP(BC_LOOP_TICK,      loop_tick)
        DOP(BC_CALL,           call)
        DOP(BC_CALL_METHOD,    call_method)
        DOP(BC_CALL_ELEM,      call_elem)
        DOP(BC_NEW,            new_op)
        DOP(BC_RETURN,         return_op)
        DOP(BC_NEW_OBJECT,     new_object)
        DOP(BC_NEW_ARRAY,      new_array)
        DOP(BC_DEF_PROP,       def_prop)
        DOP(BC_DEF_ELEM,       def_elem)
        DOP(BC_GET_PROP,       get_prop)
        DOP(BC_SET_PROP,       set_prop)
        DOP(BC_GET_ELEM,       get_elem)
        DOP(BC_SET_ELEM,       set_elem)
        DOP(BC_DELETE_PROP,    delete_prop)
        DOP(BC_DELETE_ELEM,    delete_elem)
        DOP(BC_ITER_INIT,      iter_init)
        DOP(BC_ITER_NEXT,      iter_next)
        DOP(BC_ITER_CLOSE,     iter_close)
        DOP(BC_SCOPE_ENTER,    scope_enter)
        DOP(BC_SCOPE_LEAVE,    scope_leave)
        DOP(BC_EVAL_NODE,      eval_node)
        DOP(BC_EVAL_NODE_POP,  eval_node_pop)
        DOP(BC_SET_RESULT,     set_result)
        DOP(BC_CLEAR_RESULT,   clear_result)
        DOP(BC_THROW,          throw_op)
        #undef DOP
        dispatch_init = 1;
    }
    #define DISPATCH() do { uint8_t _op = *ip++; goto *dispatch[_op]; } while (0)
#else
    #define DISPATCH() goto vm_next
#endif

#if LR_THREADED_CODE
    /* ── Start direct-threaded dispatch ──────────────────────────────── */
    {
        uint8_t op = *ip++;
        goto *dispatch[op];
    }
#else
    /* ── Start indirect-threaded (switch) dispatch ──────────────────── */
    goto vm_next;
#endif

    /* ═══════════════════════════════════════════════════════════════════
       OPCODE HANDLERS
       Each handler reads operands, executes, then dispatches the next
       opcode via DISPATCH() (direct) or goto vm_next (indirect).

       BC_CASE macro: defines a named label for computed-goto AND a switch
       case. Direct threading jumps to &&lbl_XXX; indirect falls through.  */

#if LR_THREADED_CODE
#define BC_CASE(lbl, op) lbl_##lbl: case op
#else
#define BC_CASE(lbl, op) case op
#endif

#if !LR_THREADED_CODE
    for (;;) {
    vm_next:
        if (ip < prog->code || ip >= code_end) goto vm_abort;
        switch (*ip++) {
#endif

        case BC_STOP: goto vm_done;
        case BC_NOP: DISPATCH();

        case BC_PUSH_UNDEFINED: PUSH(LR_VALUE_UNDEFINED); DISPATCH();
        case BC_PUSH_NULL:      PUSH(LR_VALUE_NULL); DISPATCH();
        case BC_PUSH_TRUE:      PUSH(lr_new_bool(ctx, 1)); DISPATCH();
        case BC_PUSH_FALSE:     PUSH(lr_new_bool(ctx, 0)); DISPATCH();
        case BC_PUSH_THIS: {
            LRValue tv;
            interp_bc_push_this(interp, &tv);
            PUSH(tv);
            DISPATCH();
        }
        case BC_PUSH_INT32:     PUSH(lr_new_int32(ctx, rd32(&ip))); DISPATCH();
        case BC_PUSH_FLOAT64: {
            uint16_t si = rd16(&ip);
            PUSH(lr_new_float64(ctx, prog->pool[si].u.f64));
            DISPATCH();
        }
        case BC_PUSH_STRING: {
            uint16_t si = rd16(&ip);
            PUSH(lr_new_string(ctx, prog->pool[si].u.str));
            DISPATCH();
        }

        case BC_POP: { LRValue v = POP(); FREE_IF_HEAP(ctx, v); DISPATCH(); }
        case BC_DUP: {
            LRValue v = sp > 0 ? stack[sp - 1] : LR_VALUE_UNDEFINED;
            PUSH(lr_dup_value(ctx, v));
            DISPATCH();
        }
        case BC_DUP2: {
            if (sp < 2) goto vm_abort;
            LRValue a = stack[sp - 2], b = stack[sp - 1];
            PUSH(lr_dup_value(ctx, a));
            PUSH(lr_dup_value(ctx, b));
            DISPATCH();
        }
        case BC_SWAP: {
            if (sp < 2) goto vm_abort;
            LRValue t = stack[sp - 1];
            stack[sp - 1] = stack[sp - 2];
            stack[sp - 2] = t;
            DISPATCH();
        }
        case BC_ROT3: {
            if (sp < 3) goto vm_abort;
            LRValue t = stack[sp - 1];
            stack[sp - 1] = stack[sp - 2];
            stack[sp - 2] = stack[sp - 3];
            stack[sp - 3] = t;
            DISPATCH();
        }

        case BC_LOAD_VAR: {
            uint16_t si = rd16(&ip);
            LRValue v;
            if (!interp_bc_load_var(interp, prog->pool[si].u.str, &v)) goto vm_abort;
            PUSH(v);
            DISPATCH();
        }
        case BC_STORE_VAR: {
            uint16_t si = rd16(&ip);
            LRValue v = POP();
            interp_bc_store_var(interp, prog->pool[si].u.str, v);
            FREE_IF_HEAP(ctx, v);
            CHECK();
            DISPATCH();
        }
        case BC_DECLARE_VAR: {
            uint16_t si = rd16(&ip);
            uint8_t kind = *ip++;
            LRValue v = POP();
            interp_bc_declare_var(interp, prog->pool[si].u.str, v, (int)kind);
            FREE_IF_HEAP(ctx, v);
            CHECK();
            DISPATCH();
        }
        case BC_TYPEOF_VAR: {
            uint16_t si = rd16(&ip);
            LRValue v;
            if (interp_bc_typeof_var(interp, prog->pool[si].u.str, &v)) {
                PUSH(lr_new_string(ctx, bcv_typeof(ctx, v)));
                FREE_IF_HEAP(ctx, v);
            } else {
                PUSH(lr_new_string(ctx, "undefined"));
            }
            DISPATCH();
        }

        case BC_ADD: case BC_SUB: case BC_MUL: case BC_DIV: case BC_MOD:
        case BC_POW: case BC_LT: case BC_GT: case BC_LE: case BC_GE:
        case BC_EQ: case BC_NE: case BC_STRICT_EQ: case BC_STRICT_NE:
        case BC_SHL: case BC_SHR: case BC_SAR:
        case BC_BIT_AND: case BC_BIT_OR: case BC_BIT_XOR:
        case BC_IN: case BC_INSTANCEOF: {
            if (sp < 2) goto vm_abort;
            LRValue b = POP(), a = POP();
            LRValue r = bcv_binop(interp, (int)*(ip - 1), a, b);
            FREE_IF_HEAP(ctx, a);
            FREE_IF_HEAP(ctx, b);
            if (interp->error_flag) { FREE_IF_HEAP(ctx, r); goto vm_abort; }
            PUSH(r);
            DISPATCH();
        }

        case BC_NEG: {
            LRValue a = POP();
            LRValue r = bcv_number(ctx, -bcv_to_number(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH(r);
            DISPATCH();
        }
        case BC_POS: {
            LRValue a = POP();
            LRValue r = bcv_number(ctx, bcv_to_number(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH(r);
            DISPATCH();
        }
        case BC_NOT: {
            LRValue a = POP();
            int t = lr_to_bool(ctx, a);
            FREE_IF_HEAP(ctx, a);
            PUSH(lr_new_bool(ctx, !t));
            DISPATCH();
        }
        case BC_BIT_NOT: {
            LRValue a = POP();
            int32_t i = bcv_to_int32(ctx, a);
            FREE_IF_HEAP(ctx, a);
            PUSH(lr_new_int32(ctx, ~i));
            DISPATCH();
        }
        case BC_TYPEOF: {
            LRValue a = POP();
            LRValue r = lr_new_string(ctx, bcv_typeof(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH(r);
            DISPATCH();
        }
        case BC_VOID: {
            LRValue a = POP();
            FREE_IF_HEAP(ctx, a);
            PUSH(LR_VALUE_UNDEFINED);
            DISPATCH();
        }

        case BC_JUMP: { int32_t off = rd32(&ip); ip += off; DISPATCH(); }
        case BC_JUMP_IF_FALSE: {
            int32_t off = rd32(&ip);
            LRValue v = POP();
            int t = lr_to_bool(ctx, v);
            FREE_IF_HEAP(ctx, v);
            if (!t) ip += off;
            DISPATCH();
        }
        case BC_JUMP_IF_TRUE: {
            int32_t off = rd32(&ip);
            LRValue v = POP();
            int t = lr_to_bool(ctx, v);
            FREE_IF_HEAP(ctx, v);
            if (t) ip += off;
            DISPATCH();
        }
        case BC_JUMP_IF_FALSE_KEEP: {
            int32_t off = rd32(&ip);
            if (sp < 1) goto vm_abort;
            if (!lr_to_bool(ctx, stack[sp - 1])) ip += off;
            DISPATCH();
        }
        case BC_JUMP_IF_TRUE_KEEP: {
            int32_t off = rd32(&ip);
            if (sp < 1) goto vm_abort;
            if (lr_to_bool(ctx, stack[sp - 1])) ip += off;
            DISPATCH();
        }
        case BC_JUMP_IF_NOT_NULLISH: {
            int32_t off = rd32(&ip);
            if (sp < 1) goto vm_abort;
            LRValue v = stack[sp - 1];
            if (v.tag != LR_TYPE_UNDEFINED && v.tag != LR_TYPE_NULL) ip += off;
            DISPATCH();
        }
        case BC_LOOP_TICK:
            if (interp->timeout_ms > 0 && ++interp->stmt_counter >= 1024) {
                interp->stmt_counter = 0;
                clock_t now = clock();
                clock_t elapsed = (now * 1000) / CLOCKS_PER_SEC;
                if (elapsed >= (clock_t)interp->timeout_ms) {
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "Execution timeout exceeded (%d ms)", interp->timeout_ms);
                    interp->error_flag = 1;
                    goto vm_abort;
                }
            }
            DISPATCH();

        case BC_CALL: case BC_CALL_METHOD: case BC_CALL_ELEM: case BC_NEW: {
            uint8_t saved_op = *(ip - 1);
            uint16_t name_idx = 0;
            if (saved_op == BC_CALL_METHOD) name_idx = rd16(&ip);
            uint16_t argc = rd16(&ip);

            LRValue argbuf[8];
            LRValue *argv = argbuf;
            if (argc > 8) {
                argv = (LRValue *)malloc(sizeof(LRValue) * argc);
                if (!argv) goto vm_abort;
            }
            for (int i = (int)argc - 1; i >= 0; i--) argv[i] = POP();

            LRValue callee = LR_VALUE_UNDEFINED, this_val = LR_VALUE_UNDEFINED;
            LRValue key = LR_VALUE_UNDEFINED;
            if (saved_op == BC_CALL_METHOD) {
                this_val = POP();
                callee = lr_get_property_str(ctx, this_val, prog->pool[name_idx].u.str);
            } else if (saved_op == BC_CALL_ELEM) {
                key = POP();
                this_val = POP();
                LRString *atom = lr_to_atom(ctx, key);
                callee = lr_get_property(ctx, this_val, atom);
            } else {
                callee = POP();
                this_val = (saved_op == BC_NEW) ? LR_VALUE_UNDEFINED : lr_get_global_object(ctx);
            }

            LRValue r;
            if (saved_op == BC_NEW) r = interp_bc_construct(interp, callee, (int)argc, argv);
            else                   r = interp_bc_call(interp, callee, this_val, (int)argc, argv);

            for (int i = 0; i < (int)argc; i++) FREE_IF_HEAP(ctx, argv[i]);
            if (argv != argbuf) free(argv);
            FREE_IF_HEAP(ctx, key);
            FREE_IF_HEAP(ctx, callee);
            FREE_IF_HEAP(ctx, this_val);
            if (interp->error_flag || interp->exception_pending) {
                FREE_IF_HEAP(ctx, r);
                goto vm_abort;
            }
            PUSH(r);
            DISPATCH();
        }

        case BC_RETURN: {
            LRValue v = POP();
            FREE_IF_HEAP(ctx, result);
            result = v;
            goto vm_done;
        }

        case BC_NEW_OBJECT: PUSH(lr_new_object(ctx)); DISPATCH();
        case BC_NEW_ARRAY: {
            uint16_t n = rd16(&ip);
            if (sp < n) goto vm_abort;
            LRValue arr = lr_new_array(ctx);
            for (int i = (int)n - 1; i >= 0; i--) {
                LRValue v = POP();
                lr_set_property_uint32(ctx, arr, (uint32_t)i, v);
            }
            lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, (int32_t)n));
            PUSH(arr);
            DISPATCH();
        }
        case BC_DEF_PROP: {
            uint16_t si = rd16(&ip);
            if (sp < 2) goto vm_abort;
            LRValue v = POP();
            lr_set_property_str(ctx, stack[sp - 1], prog->pool[si].u.str, v);
            DISPATCH();
        }
        case BC_DEF_ELEM: {
            if (sp < 3) goto vm_abort;
            LRValue v = POP(), k = POP();
            LRString *atom = lr_to_atom(ctx, k);
            lr_set_property(ctx, stack[sp - 1], atom, v);
            FREE_IF_HEAP(ctx, k);
            DISPATCH();
        }
        case BC_GET_PROP: {
            uint16_t si = rd16(&ip);
            LRValue o = POP();
            LRValue v = lr_get_property_str(ctx, o, prog->pool[si].u.str);
            FREE_IF_HEAP(ctx, o);
            CHECK();
            PUSH(v);
            DISPATCH();
        }
        case BC_SET_PROP: {
            uint16_t si = rd16(&ip);
            if (sp < 2) goto vm_abort;
            LRValue v = POP(), o = POP();
            lr_set_property_str(ctx, o, prog->pool[si].u.str, lr_dup_value(ctx, v));
            FREE_IF_HEAP(ctx, o);
            PUSH(v);
            DISPATCH();
        }
        case BC_GET_ELEM: {
            if (sp < 2) goto vm_abort;
            LRValue k = POP(), o = POP();
            LRString *atom = lr_to_atom(ctx, k);
            LRValue v = lr_get_property(ctx, o, atom);
            FREE_IF_HEAP(ctx, k);
            FREE_IF_HEAP(ctx, o);
            CHECK();
            PUSH(v);
            DISPATCH();
        }
        case BC_SET_ELEM: {
            if (sp < 3) goto vm_abort;
            LRValue v = POP(), k = POP(), o = POP();
            LRString *atom = lr_to_atom(ctx, k);
            lr_set_property(ctx, o, atom, lr_dup_value(ctx, v));
            FREE_IF_HEAP(ctx, k);
            FREE_IF_HEAP(ctx, o);
            PUSH(v);
            DISPATCH();
        }
        case BC_DELETE_PROP: {
            uint16_t si = rd16(&ip);
            LRValue o = POP();
            LRString *atom = lr_new_atom(ctx, prog->pool[si].u.str);
            lr_delete_property(ctx, o, atom, 0);
            FREE_IF_HEAP(ctx, o);
            PUSH(lr_new_bool(ctx, 1));
            DISPATCH();
        }
        case BC_DELETE_ELEM: {
            if (sp < 2) goto vm_abort;
            LRValue k = POP(), o = POP();
            LRString *atom = lr_to_atom(ctx, k);
            lr_delete_property(ctx, o, atom, 0);
            FREE_IF_HEAP(ctx, k);
            FREE_IF_HEAP(ctx, o);
            PUSH(lr_new_bool(ctx, 1));
            DISPATCH();
        }

        case BC_ITER_INIT: {
            LRValue src = POP();
            LRValue nextfn = LR_VALUE_UNDEFINED;
            int32_t index = 0;
            if (lr_is_string(src) || lr_is_array(ctx, src)) {
                index = 0;
            } else if (lr_is_object(src)) {
                LRValue iter_fn = lr_get_property_str(ctx, src, "Symbol.iterator");
                if (lr_is_function(ctx, iter_fn)) {
                    LRValue argv1[1]; argv1[0] = src;
                    LRValue iter = interp_bc_call(interp, iter_fn, src, 1, argv1);
                    lr_free_value(ctx, iter_fn);
                    if (interp->error_flag || interp->exception_pending) {
                        FREE_IF_HEAP(ctx, iter);
                        FREE_IF_HEAP(ctx, src);
                        goto vm_abort;
                    }
                    FREE_IF_HEAP(ctx, src);
                    src = iter;
                    nextfn = lr_get_property_str(ctx, src, "next");
                    index = -1;
                } else {
                    lr_free_value(ctx, iter_fn);
                    index = -2;   /* not iterable: loop body never runs */
                }
            } else {
                index = -2;
            }
            PUSH(src);
            PUSH(nextfn);
            PUSH(lr_new_int32(ctx, index));
            DISPATCH();
        }
        case BC_ITER_NEXT: {
            int32_t off = rd32(&ip);
            if (sp < 3) goto vm_abort;
            int32_t index = stack[sp - 1].u.int32;
            if (index == -2) { ip += off; DISPATCH(); }
            if (index >= 0) {
                LRValue src = stack[sp - 3];
                if (lr_is_string(src)) {
                    const char *s = lr_to_cstring(ctx, src);
                    size_t slen = s ? strlen(s) : 0;
                    if ((size_t)index >= slen) {
                        lr_free_cstring(ctx, s);
                        ip += off;
                        DISPATCH();
                    }
                    char buf[2];
                    buf[0] = s[index];
                    buf[1] = '\0';
                    lr_free_cstring(ctx, s);
                    stack[sp - 1].u.int32 = index + 1;
                    PUSH(lr_new_string(ctx, buf));
                } else {
                    int32_t len = 0;
                    LRValue lv = lr_get_property_str(ctx, src, "length");
                    lr_to_int32(ctx, &len, lv);
                    lr_free_value(ctx, lv);
                    if (index >= len) { ip += off; DISPATCH(); }
                    LRValue item = lr_get_property_uint32(ctx, src, (uint32_t)index);
                    stack[sp - 1].u.int32 = index + 1;
                    PUSH(item);
                }
            } else {
                LRValue nr = interp_bc_call(interp, stack[sp - 2], stack[sp - 3], 0, NULL);
                if (interp->error_flag || interp->exception_pending) {
                    FREE_IF_HEAP(ctx, nr);
                    goto vm_abort;
                }
                LRValue done = lr_get_property_str(ctx, nr, "done");
                int is_done = lr_to_bool(ctx, done);
                lr_free_value(ctx, done);
                if (is_done) { FREE_IF_HEAP(ctx, nr); ip += off; DISPATCH(); }
                LRValue v = lr_get_property_str(ctx, nr, "value");
                FREE_IF_HEAP(ctx, nr);
                PUSH(v);
            }
            DISPATCH();
        }
        case BC_ITER_CLOSE: {
            if (sp < 3) goto vm_abort;
            LRValue idx = POP(), nf = POP(), src = POP();
            (void)idx;
            FREE_IF_HEAP(ctx, nf);
            FREE_IF_HEAP(ctx, src);
            DISPATCH();
        }

        case BC_SCOPE_ENTER: interp_bc_push_scope(interp); scope_depth++; DISPATCH();
        case BC_SCOPE_LEAVE:
            if (scope_depth > 0) { interp_bc_pop_scope(interp); scope_depth--; }
            DISPATCH();

        case BC_EVAL_NODE: {
            uint16_t si = rd16(&ip);
            LRValue v = interp_bc_eval_node(interp, (ASTNode *)prog->pool[si].u.node);
            if (interp->error_flag || interp->exception_pending ||
                interp->has_returned || interp->break_target || interp->continue_target) {
                FREE_IF_HEAP(ctx, v);
                goto vm_abort;
            }
            PUSH(v);
            DISPATCH();
        }
        case BC_EVAL_NODE_POP: {
            uint16_t si = rd16(&ip);
            LRValue v = interp_bc_eval_node(interp, (ASTNode *)prog->pool[si].u.node);
            FREE_IF_HEAP(ctx, v);
            CHECK();
            DISPATCH();
        }
        case BC_SET_RESULT: {
            LRValue v = POP();
            FREE_IF_HEAP(ctx, result);
            result = v;
            DISPATCH();
        }
        case BC_CLEAR_RESULT:
            FREE_IF_HEAP(ctx, result);
            result = LR_VALUE_UNDEFINED;
            DISPATCH();

        case BC_THROW: {
            LRValue v = POP();
            interp_bc_throw(interp, v);
            FREE_IF_HEAP(ctx, v);
            goto vm_abort;
        }

        default:
            goto vm_abort;

#if !LR_THREADED_CODE
        } /* switch */
    }   /* for(;;) */
#endif  /* !LR_THREADED_CODE */

vm_done:
    while (sp > 0) { LRValue v = stack[--sp]; FREE_IF_HEAP(ctx, v); }
    while (scope_depth-- > 0) interp_bc_pop_scope(interp);
    free(stack);
    return result;

vm_abort:
    while (sp > 0) { LRValue v = stack[--sp]; FREE_IF_HEAP(ctx, v); }
    while (scope_depth-- > 0) interp_bc_pop_scope(interp);
    free(stack);
    FREE_IF_HEAP(ctx, result);
    return LR_VALUE_UNDEFINED;

#undef PUSH
#undef POP
#undef CHECK
#undef VM_GROW
}

/* ═══════════════════════════════════════════════════════════════════════
   SERIALIZATION  (IOME586 archive)
   ═══════════════════════════════════════════════════════════════════════ */

#define BC_SER_MAGIC   "LRBC"
#define BC_SER_VERSION 2u
#define BC_SER_FLAG_NODE_REFS 1u

static void put_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint8_t *bc_serialize(BCProgram *prog, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!prog || !prog->code || !out_len) return NULL;

    size_t total = 4 + 4 + 4 + 4 + 4 + (size_t)prog->code_len;
    for (int i = 0; i < prog->pool_count; i++) {
        total += 1;
        switch (prog->pool[i].kind) {
        case BC_POOL_INT32:   total += 4; break;
        case BC_POOL_FLOAT64: total += 8; break;
        case BC_POOL_STRING:  total += 4 + strlen(prog->pool[i].u.str) + 1; break;
        case BC_POOL_NODE:    break;
        }
    }
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    size_t pos = 0;
    memcpy(buf, BC_SER_MAGIC, 4); pos += 4;
    put_u32(buf + pos, BC_SER_VERSION); pos += 4;
    put_u32(buf + pos, prog->node_refs ? BC_SER_FLAG_NODE_REFS : 0u); pos += 4;
    put_u32(buf + pos, (uint32_t)prog->code_len); pos += 4;
    put_u32(buf + pos, (uint32_t)prog->pool_count); pos += 4;
    memcpy(buf + pos, prog->code, (size_t)prog->code_len);
    pos += (size_t)prog->code_len;

    for (int i = 0; i < prog->pool_count; i++) {
        buf[pos++] = (uint8_t)prog->pool[i].kind;
        switch (prog->pool[i].kind) {
        case BC_POOL_INT32:
            put_u32(buf + pos, (uint32_t)prog->pool[i].u.i32); pos += 4;
            break;
        case BC_POOL_FLOAT64:
            memcpy(buf + pos, &prog->pool[i].u.f64, 8); pos += 8;
            break;
        case BC_POOL_STRING: {
            size_t sl = strlen(prog->pool[i].u.str);
            put_u32(buf + pos, (uint32_t)sl); pos += 4;
            memcpy(buf + pos, prog->pool[i].u.str, sl + 1); pos += sl + 1;
            break;
        }
        case BC_POOL_NODE:
            break;
        }
    }
    *out_len = pos;
    return buf;
}

BCProgram *bc_deserialize(const uint8_t *data, size_t len)
{
    if (!data || len < 20 || memcmp(data, BC_SER_MAGIC, 4) != 0) return NULL;
    if (get_u32(data + 4) != BC_SER_VERSION) return NULL;
    uint32_t flags = get_u32(data + 8);
    if (flags & BC_SER_FLAG_NODE_REFS) return NULL;   /* needs the AST */

    uint32_t code_len = get_u32(data + 12);
    uint32_t pool_count = get_u32(data + 16);
    size_t pos = 20;
    if (pos + code_len > len) return NULL;

    BCProgram *p = bc_new_program();
    if (!p) return NULL;
    p->code_len = (int32_t)code_len;
    p->code_cap = (int32_t)code_len + 16;
    p->code = (uint8_t *)malloc((size_t)p->code_cap);
    if (!p->code) { bc_free_program(p); return NULL; }
    memcpy(p->code, data + pos, code_len);
    pos += code_len;

    p->pool_count = (int32_t)pool_count;
    p->pool_cap = (int32_t)pool_count + 8;
    p->pool = (BCConst *)calloc((size_t)p->pool_cap, sizeof(BCConst));
    if (!p->pool) { bc_free_program(p); return NULL; }

    for (uint32_t i = 0; i < pool_count; i++) {
        if (pos >= len) { bc_free_program(p); return NULL; }
        p->pool[i].kind = (BCPoolKind)data[pos++];
        switch (p->pool[i].kind) {
        case BC_POOL_INT32:
            if (pos + 4 > len) { bc_free_program(p); return NULL; }
            p->pool[i].u.i32 = (int32_t)get_u32(data + pos); pos += 4;
            break;
        case BC_POOL_FLOAT64:
            if (pos + 8 > len) { bc_free_program(p); return NULL; }
            memcpy(&p->pool[i].u.f64, data + pos, 8); pos += 8;
            break;
        case BC_POOL_STRING: {
            if (pos + 4 > len) { bc_free_program(p); return NULL; }
            uint32_t sl = get_u32(data + pos); pos += 4;
            if (pos + sl + 1 > len) { bc_free_program(p); return NULL; }
            p->pool[i].u.str = (char *)malloc(sl + 1);
            if (!p->pool[i].u.str) { bc_free_program(p); return NULL; }
            memcpy(p->pool[i].u.str, data + pos, sl + 1);
            p->pool[i].u.str[sl] = '\0';
            pos += sl + 1;
            break;
        }
        case BC_POOL_NODE:
            bc_free_program(p);
            return NULL;
        }
    }
    p->compiled = 1;
    p->max_stack = 64;
    return p;
}

/* ═══════════════════════════════════════════════════════════════════════
   DISASSEMBLER (debugging aid)
   ═══════════════════════════════════════════════════════════════════════ */

typedef struct { const char *name; int operands; } BCInfo;

/* operands: 0 none, 1 = u16, 2 = i32, 3 = u16+u16, 4 = u16+u8 */
static BCInfo bc_info(uint8_t op)
{
    BCInfo t;
    t.name = "?"; t.operands = 0;
    switch (op) {
    case BC_STOP: t.name = "STOP"; break;
    case BC_NOP: t.name = "NOP"; break;
    case BC_PUSH_UNDEFINED: t.name = "PUSH_UNDEFINED"; break;
    case BC_PUSH_NULL: t.name = "PUSH_NULL"; break;
    case BC_PUSH_TRUE: t.name = "PUSH_TRUE"; break;
    case BC_PUSH_FALSE: t.name = "PUSH_FALSE"; break;
    case BC_PUSH_THIS: t.name = "PUSH_THIS"; break;
    case BC_PUSH_INT32: t.name = "PUSH_INT32"; t.operands = 2; break;
    case BC_PUSH_FLOAT64: t.name = "PUSH_FLOAT64"; t.operands = 1; break;
    case BC_PUSH_STRING: t.name = "PUSH_STRING"; t.operands = 1; break;
    case BC_POP: t.name = "POP"; break;
    case BC_DUP: t.name = "DUP"; break;
    case BC_DUP2: t.name = "DUP2"; break;
    case BC_SWAP: t.name = "SWAP"; break;
    case BC_ROT3: t.name = "ROT3"; break;
    case BC_LOAD_VAR: t.name = "LOAD_VAR"; t.operands = 1; break;
    case BC_STORE_VAR: t.name = "STORE_VAR"; t.operands = 1; break;
    case BC_DECLARE_VAR: t.name = "DECLARE_VAR"; t.operands = 4; break;
    case BC_TYPEOF_VAR: t.name = "TYPEOF_VAR"; t.operands = 1; break;
    case BC_ADD: t.name = "ADD"; break;
    case BC_SUB: t.name = "SUB"; break;
    case BC_MUL: t.name = "MUL"; break;
    case BC_DIV: t.name = "DIV"; break;
    case BC_MOD: t.name = "MOD"; break;
    case BC_POW: t.name = "POW"; break;
    case BC_LT: t.name = "LT"; break;
    case BC_GT: t.name = "GT"; break;
    case BC_LE: t.name = "LE"; break;
    case BC_GE: t.name = "GE"; break;
    case BC_EQ: t.name = "EQ"; break;
    case BC_NE: t.name = "NE"; break;
    case BC_STRICT_EQ: t.name = "STRICT_EQ"; break;
    case BC_STRICT_NE: t.name = "STRICT_NE"; break;
    case BC_SHL: t.name = "SHL"; break;
    case BC_SHR: t.name = "SHR"; break;
    case BC_SAR: t.name = "SAR"; break;
    case BC_BIT_AND: t.name = "BIT_AND"; break;
    case BC_BIT_OR: t.name = "BIT_OR"; break;
    case BC_BIT_XOR: t.name = "BIT_XOR"; break;
    case BC_IN: t.name = "IN"; break;
    case BC_INSTANCEOF: t.name = "INSTANCEOF"; break;
    case BC_NEG: t.name = "NEG"; break;
    case BC_POS: t.name = "POS"; break;
    case BC_NOT: t.name = "NOT"; break;
    case BC_BIT_NOT: t.name = "BIT_NOT"; break;
    case BC_TYPEOF: t.name = "TYPEOF"; break;
    case BC_VOID: t.name = "VOID"; break;
    case BC_JUMP: t.name = "JUMP"; t.operands = 2; break;
    case BC_JUMP_IF_FALSE: t.name = "JUMP_IF_FALSE"; t.operands = 2; break;
    case BC_JUMP_IF_TRUE: t.name = "JUMP_IF_TRUE"; t.operands = 2; break;
    case BC_JUMP_IF_FALSE_KEEP: t.name = "JUMP_IF_FALSE_KEEP"; t.operands = 2; break;
    case BC_JUMP_IF_TRUE_KEEP: t.name = "JUMP_IF_TRUE_KEEP"; t.operands = 2; break;
    case BC_JUMP_IF_NOT_NULLISH: t.name = "JUMP_IF_NOT_NULLISH"; t.operands = 2; break;
    case BC_LOOP_TICK: t.name = "LOOP_TICK"; break;
    case BC_CALL: t.name = "CALL"; t.operands = 1; break;
    case BC_CALL_METHOD: t.name = "CALL_METHOD"; t.operands = 3; break;
    case BC_CALL_ELEM: t.name = "CALL_ELEM"; t.operands = 1; break;
    case BC_NEW: t.name = "NEW"; t.operands = 1; break;
    case BC_RETURN: t.name = "RETURN"; break;
    case BC_NEW_OBJECT: t.name = "NEW_OBJECT"; break;
    case BC_NEW_ARRAY: t.name = "NEW_ARRAY"; t.operands = 1; break;
    case BC_DEF_PROP: t.name = "DEF_PROP"; t.operands = 1; break;
    case BC_DEF_ELEM: t.name = "DEF_ELEM"; break;
    case BC_GET_PROP: t.name = "GET_PROP"; t.operands = 1; break;
    case BC_SET_PROP: t.name = "SET_PROP"; t.operands = 1; break;
    case BC_GET_ELEM: t.name = "GET_ELEM"; break;
    case BC_SET_ELEM: t.name = "SET_ELEM"; break;
    case BC_DELETE_PROP: t.name = "DELETE_PROP"; t.operands = 1; break;
    case BC_DELETE_ELEM: t.name = "DELETE_ELEM"; break;
    case BC_ITER_INIT: t.name = "ITER_INIT"; break;
    case BC_ITER_NEXT: t.name = "ITER_NEXT"; t.operands = 2; break;
    case BC_ITER_CLOSE: t.name = "ITER_CLOSE"; break;
    case BC_SCOPE_ENTER: t.name = "SCOPE_ENTER"; break;
    case BC_SCOPE_LEAVE: t.name = "SCOPE_LEAVE"; break;
    case BC_EVAL_NODE: t.name = "EVAL_NODE"; t.operands = 1; break;
    case BC_EVAL_NODE_POP: t.name = "EVAL_NODE_POP"; t.operands = 1; break;
    case BC_SET_RESULT: t.name = "SET_RESULT"; break;
    case BC_CLEAR_RESULT: t.name = "CLEAR_RESULT"; break;
    case BC_THROW: t.name = "THROW"; break;
    default: break;
    }
    return t;
}

char *bc_disassemble(BCProgram *prog)
{
    if (!prog || !prog->code) return NULL;
    size_t cap = 4096, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    char line[256];
    int pc = 0;
    while (pc < prog->code_len) {
        uint8_t op = prog->code[pc];
        BCInfo info = bc_info(op);
        int start = pc++;
        int n = 0;
        switch (info.operands) {
        case 1: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            pc += 2;
            n = snprintf(line, sizeof(line), "%05d  %-20s %u\n", start, info.name, a);
            break;
        }
        case 2: {
            int32_t a = (int32_t)((uint32_t)prog->code[pc] |
                                  ((uint32_t)prog->code[pc + 1] << 8) |
                                  ((uint32_t)prog->code[pc + 2] << 16) |
                                  ((uint32_t)prog->code[pc + 3] << 24));
            pc += 4;
            n = snprintf(line, sizeof(line), "%05d  %-20s %d (→%d)\n",
                         start, info.name, a, pc + a);
            break;
        }
        case 3: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            uint16_t b = (uint16_t)(prog->code[pc + 2] | (prog->code[pc + 3] << 8));
            pc += 4;
            n = snprintf(line, sizeof(line), "%05d  %-20s %u, %u\n", start, info.name, a, b);
            break;
        }
        case 4: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            uint8_t b = prog->code[pc + 2];
            pc += 3;
            n = snprintf(line, sizeof(line), "%05d  %-20s %u, kind=%u\n",
                         start, info.name, a, b);
            break;
        }
        default:
            n = snprintf(line, sizeof(line), "%05d  %s\n", start, info.name);
            break;
        }
        if (n < 0) break;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *no = (char *)realloc(out, cap);
            if (!no) break;
            out = no;
        }
        memcpy(out + len, line, (size_t)n);
        len += (size_t)n;
        out[len] = '\0';
    }
    return out;
}
