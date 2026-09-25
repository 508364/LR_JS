/*
 * LR_JS - JavaScript Engine AST Tree-Walking Interpreter
 * Pure C implementation.
 *
 * Evaluates AST nodes produced by the parser using a tree-walking approach.
 * Handles all ES2022 expressions, statements, declarations, and control flow.
 */
#include "lr_interp.h"
#include "lr_bytecode.h"
#include "lr_jit.h"
#include "../lr_promise.h"
#include "../lr_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

/* Better hash for atom pointers: shift right by 4 to avoid alignment
 * collisions (atoms are 16-byte aligned, so low 4 bits are always 0).
 * Same definition as in lr_engine.c — used by the inline cache fast path. */
#define SHAPE_ATOM_HASH(atom)  ((((uintptr_t)(atom)) >> 4) ^ (((uintptr_t)(atom)) >> 10)) & SHAPE_FLAT_MASK

/* ── CAS primitives ────────────────────────────────────────────────────── */
#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_InterlockedCompareExchangePointer)
#pragma intrinsic(_InterlockedExchangeAdd)
#define LR_CAS_PTR(ptr, old, new) _InterlockedCompareExchangePointer((void *volatile *)(ptr), (new), (old))
#define LR_ATOMIC_INC32(ptr)  _InterlockedExchangeAdd((volatile long *)(ptr), 1)
#else
#define LR_CAS_PTR(ptr, old, new) __sync_val_compare_and_swap((void *volatile *)(ptr), (old), (new))
#define LR_ATOMIC_INC32(ptr)  __sync_fetch_and_add((ptr), 1)
#endif

/* ── Constants ─────────────────────────────────────────────────────────── */

#define MAX_CALL_DEPTH 4096
#define SCOPE_INIT_CAP 8

/* Cached env flags: getenv() on MSVCRT locks + scans the env array (~7-14µs
 * per call on Windows).  Only read once; see lr_env_flag in lr_interp.h. */
static int g_lr_env_debug_var    = -1;
static int g_lr_env_debug_call2  = -1;
static int g_lr_env_debug_stack  = -1;
static int g_lr_env_debug_jitcall = -1;

/* ── Function body bytecode cache ──────────────────────────────────────── */
#define BC_BODY_CACHE_SIZE 2048
#define BC_BODY_HASH_SIZE   256

/* ── Extends resolution cache ──────────────────────────────────────────
 * Caches (extends_AST_node → resolved_parent_class) so constructors
 * don't re-evaluate the extends clause on every `new` call. The AST
 * node lives for the lifetime of the program; the cache holds a dup'd
 * reference. Simple direct-mapped hash, collisions just overwrite. */
#define EXTENDS_CACHE_BITS 8
#define EXTENDS_CACHE_SIZE (1 << EXTENDS_CACHE_BITS)
typedef struct ExtendsEntry {
    ASTNode   *key;
    LRValue    parent;
    LRValue    superctor;
    LRValue    superproto;
} ExtendsEntry;

static LR_THREAD_LOCAL ExtendsEntry extends_cache[EXTENDS_CACHE_SIZE];

static LRValue extends_cache_get(ASTNode *ext_ast) {
    if (!ext_ast) return LR_VALUE_UNDEFINED;
    unsigned h = ((uintptr_t)ext_ast >> 3) & (EXTENDS_CACHE_SIZE - 1);
    if (extends_cache[h].key == ext_ast)
        return extends_cache[h].parent;
    return LR_VALUE_UNDEFINED;
}
static void extends_cache_set(LRContext *ctx, ASTNode *ext_ast, LRValue parent) {
    if (!ext_ast) return;
    unsigned h = ((uintptr_t)ext_ast >> 3) & (EXTENDS_CACHE_SIZE - 1);
    /* Free old entry if overwriting */
    if (extends_cache[h].key) {
        if (extends_cache[h].parent.tag != LR_TYPE_UNDEFINED)
            lr_free_value(ctx, extends_cache[h].parent);
        if (extends_cache[h].superctor.tag != LR_TYPE_UNDEFINED)
            lr_free_value(ctx, extends_cache[h].superctor);
        if (extends_cache[h].superproto.tag != LR_TYPE_UNDEFINED)
            lr_free_value(ctx, extends_cache[h].superproto);
    }
    extends_cache[h].key = ext_ast;
    extends_cache[h].parent = lr_dup_value(ctx, parent);
    extends_cache[h].superctor = LR_VALUE_UNDEFINED;
    extends_cache[h].superproto = LR_VALUE_UNDEFINED;
    /* Cache %superctor% and %superproto% together with parent so method
     * calls can short-circuit the super-lookup entirely. */
    if (lr_is_object(parent)) {
        LRValue sproto = lr_get_property_str(ctx, parent, "prototype");
        extends_cache[h].superctor = lr_dup_value(ctx, parent);
        extends_cache[h].superproto = sproto;
    }
}
/* Resolve %superctor% and %superproto% from the cache entry for `ext_ast`.
 * Returns 1 on cache hit (both out values are valid, caller owns refs),
 * 0 on miss (caller must resolve and call extends_cache_set_super). */
static int extends_cache_get_super(LRContext *ctx, ASTNode *ext_ast,
                                   LRValue *out_superctor, LRValue *out_superproto) {
    if (!ext_ast) return 0;
    unsigned h = ((uintptr_t)ext_ast >> 3) & (EXTENDS_CACHE_SIZE - 1);
    ExtendsEntry *e = &extends_cache[h];
    if (e->key != ext_ast) return 0;
    if (e->superctor.tag == LR_TYPE_UNDEFINED) return 0;
    *out_superctor = lr_dup_value(ctx, e->superctor);
    *out_superproto = lr_dup_value(ctx, e->superproto);
    return 1;
}
/* Populate the superctor/superproto fields after first-time resolution. */
static void extends_cache_set_super(LRContext *ctx, ASTNode *ext_ast,
                                    LRValue superctor, LRValue superproto) {
    if (!ext_ast) return;
    unsigned h = ((uintptr_t)ext_ast >> 3) & (EXTENDS_CACHE_SIZE - 1);
    ExtendsEntry *e = &extends_cache[h];
    if (e->key != ext_ast) return;
    if (e->superctor.tag != LR_TYPE_UNDEFINED)
        lr_free_value(ctx, e->superctor);
    if (e->superproto.tag != LR_TYPE_UNDEFINED)
        lr_free_value(ctx, e->superproto);
    e->superctor = lr_dup_value(ctx, superctor);
    e->superproto = lr_dup_value(ctx, superproto);
    fprintf(stderr, "[EXT-CACHE-SET-SUP] h=%u superctor.tag=%d superctor_type=%d ptr=%p\n",
            h, e->superctor.tag, ((LRObject*)e->superctor.u.ptr)->type, (void*)e->superctor.u.ptr);
}

typedef struct BCBodyEntry {
    struct BCBodyEntry *next;   /* hash chain link */
    ASTNode            *ast_body;
    BCProgram          *prog;
} BCBodyEntry;

static LR_THREAD_LOCAL BCBodyEntry  bc_body_cache[BC_BODY_CACHE_SIZE];
static LR_THREAD_LOCAL int          bc_body_cache_count = 0;
static LR_THREAD_LOCAL BCBodyEntry *bc_body_hash[BC_BODY_HASH_SIZE];

static BCProgram *bc_body_cache_lookup(ASTNode *body) {
    unsigned h = ((uintptr_t)body >> 2) & (BC_BODY_HASH_SIZE - 1);
    for (BCBodyEntry *e = bc_body_hash[h]; e; e = e->next)
        if (e->ast_body == body) return e->prog;
    return NULL;
}
static int bc_body_cache_insert(ASTNode *body, BCProgram *prog) {
    if (bc_body_cache_count >= BC_BODY_CACHE_SIZE) return -1;
    int idx = bc_body_cache_count++;
    bc_body_cache[idx].ast_body = body;
    bc_body_cache[idx].prog = prog;
    unsigned h = ((uintptr_t)body >> 2) & (BC_BODY_HASH_SIZE - 1);
    bc_body_cache[idx].next = bc_body_hash[h];
    bc_body_hash[h] = &bc_body_cache[idx];
    return idx;
}
/* MRU inline cache: last 4 body→BCProgram mappings, O(1) without hash */
static LR_THREAD_LOCAL ASTNode   *bc_ic_body[4];
static LR_THREAD_LOCAL BCProgram *bc_ic_prog[4];
static LR_THREAD_LOCAL int        bc_ic_next = 0;

/* Thread-local reusable buffer for interp_bc_call_function's inline scope.
 * Holds up to 256 entries (this + params), each entry: one const char* + one LRValue.
 * One pair is used at a time since the VM is single-threaded per interpreter. */
#define INLINE_SCOPE_BUF_CAP 256
static LR_THREAD_LOCAL const char *inline_scope_buf_names[INLINE_SCOPE_BUF_CAP];
static LR_THREAD_LOCAL LRValue      inline_scope_buf_vals[INLINE_SCOPE_BUF_CAP];

BCProgram *bc_get_or_compile_body(ASTNode *body) {
    if (!body) return NULL;
    /* Inline cache: check last 4 */
    for (int i = 0; i < 4; i++)
        if (bc_ic_body[i] == body) return bc_ic_prog[i];
    /* Hash cache fallback */
    BCProgram *c = bc_body_cache_lookup(body);
    if (c) {
        int slot = bc_ic_next++ & 3;
        bc_ic_body[slot] = body;
        bc_ic_prog[slot] = c;
        return c;
    }
    /* Compile */
    BCProgram *p = bc_new_program();
    if (!p) return NULL;
    if (bc_compile(p, body, 0) != 0) { bc_free_program(p); return NULL; }
    bc_body_cache_insert(body, p);
    int slot = bc_ic_next++ & 3;
    bc_ic_body[slot] = body;
    bc_ic_prog[slot] = p;
    return p;
}

/* Like bc_get_or_compile_body but for a full function node (FUNC_EXPR /
 * FUNC_DECL / ARROW).  The compiler pre-binds the "this" + parameter slots
 * so direct local-slot access lines up with the runtime function scope. */
BCProgram *bc_get_or_compile_func(ASTNode *func_node) {
    if (!func_node) return NULL;
    /* Direct cache on the AST node: avoids the 4-entry inline-cache walk
     * below on every hot-path call.  Populated on first compile. */
    if (func_node->bc_prog_cache)
        return (BCProgram *)func_node->bc_prog_cache;
    ASTNode *body = NULL;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL)
        body = func_node->u.func.body;
    else if (func_node->type == AST_ARROW)
        body = func_node->u.arrow.body;
    if (!body) return NULL;
    /* Inline cache: check last 4 (keyed by body AST) */
    for (int i = 0; i < 4; i++)
        if (bc_ic_body[i] == body) {
            func_node->bc_prog_cache = bc_ic_prog[i];
            return bc_ic_prog[i];
        }
    BCProgram *c = bc_body_cache_lookup(body);
    if (c) {
        int slot = bc_ic_next++ & 3;
        bc_ic_body[slot] = body;
        bc_ic_prog[slot] = c;
        func_node->bc_prog_cache = c;
        return c;
    }
    BCProgram *p = bc_new_program();
    if (!p) return NULL;
    if (bc_compile_func(p, func_node) != 0) { bc_free_program(p); return NULL; }
    if (getenv("LR_DUMP_BYTECODE")) {
        char *d = bc_disassemble(p);
        if (d) { fprintf(stderr, "── func dump ──\n%s\n", d); free(d); }
    }
    bc_body_cache_insert(body, p);
    int slot = bc_ic_next++ & 3;
    bc_ic_body[slot] = body;
    bc_ic_prog[slot] = p;
    func_node->bc_prog_cache = p;
    return p;
}

/* ── BCProgram precompile infrastructure ──────────────────────────────────
 * Walk the AST and precompile all function bodies to bytecode.             */

static void precompile_bodies_rec(ASTNode *node);
static void precompile_bodies_list(ASTNode **nodes, int count) {
    for (int i = 0; i < count; i++) precompile_bodies_rec(nodes[i]);
}
static void precompile_bodies_rec(ASTNode *node) {
    if (!node) return;
    switch (node->type) {
    case AST_PROGRAM:
        precompile_bodies_list(node->u.list.items, node->u.list.count);
        break;
    case AST_BLOCK:
        precompile_bodies_list(node->u.list.items, node->u.list.count);
        break;
    case AST_FUNC_DECL: case AST_FUNC_EXPR:
        if (node->u.func.body) { bc_get_or_compile_func(node); precompile_bodies_rec(node->u.func.body); }
        break;
    case AST_ARROW:
        if (node->u.arrow.body) { bc_get_or_compile_func(node); precompile_bodies_rec(node->u.arrow.body); }
        break;
    case AST_CLASS_DECL:
        if (node->u.class_decl.extends) precompile_bodies_rec(node->u.class_decl.extends);
        precompile_bodies_list(node->u.class_decl.methods, node->u.class_decl.nmethods);
        break;
    case AST_EXPR_STMT: precompile_bodies_rec(node->u.expr_stmt.expr); break;
    case AST_IF: precompile_bodies_rec(node->u.if_stmt.cond); precompile_bodies_rec(node->u.if_stmt.body); if(node->u.if_stmt.else_body)precompile_bodies_rec(node->u.if_stmt.else_body); break;
    case AST_FOR: if(node->u.for_stmt.init)precompile_bodies_rec(node->u.for_stmt.init); if(node->u.for_stmt.test)precompile_bodies_rec(node->u.for_stmt.test); if(node->u.for_stmt.update)precompile_bodies_rec(node->u.for_stmt.update); precompile_bodies_rec(node->u.for_stmt.body); break;
    case AST_FOR_IN: precompile_bodies_rec(node->u.for_in.source); precompile_bodies_rec(node->u.for_in.each); precompile_bodies_rec(node->u.for_in.body); break;
    case AST_FOR_OF: precompile_bodies_rec(node->u.for_of.source); precompile_bodies_rec(node->u.for_of.each); precompile_bodies_rec(node->u.for_of.body); break;
    case AST_WHILE: case AST_DO_WHILE: precompile_bodies_rec(node->u.if_stmt.cond); precompile_bodies_rec(node->u.if_stmt.body); break;
    case AST_SWITCH: precompile_bodies_rec(node->u.switch_stmt.test); precompile_bodies_list(node->u.switch_stmt.cases,node->u.switch_stmt.ncases); break;
    case AST_CASE: if(node->u.if_stmt.cond)precompile_bodies_rec(node->u.if_stmt.cond); precompile_bodies_rec(node->u.if_stmt.body); break;
    case AST_RETURN: if(node->u.return_stmt.arg)precompile_bodies_rec(node->u.return_stmt.arg); break;
    case AST_THROW: if(node->u.throw_stmt.arg)precompile_bodies_rec(node->u.throw_stmt.arg); break;
    case AST_TRY: precompile_bodies_rec(node->u.try_stmt.body); if(node->u.try_stmt.catch_body)precompile_bodies_rec(node->u.try_stmt.catch_body); if(node->u.try_stmt.finally_body)precompile_bodies_rec(node->u.try_stmt.finally_body); break;
    case AST_WITH: precompile_bodies_rec(node->u.with_stmt.obj); precompile_bodies_rec(node->u.with_stmt.body); break;
    case AST_VAR_DECL: for(int i=0;i<node->u.var_decl.nvars;i++){ASTNode*d=node->u.var_decl.vars[i];if(d&&d->type==AST_VAR_DECLARATOR&&d->u.declarator.init)precompile_bodies_rec(d->u.declarator.init);} break;
    case AST_BINARY: precompile_bodies_rec(node->u.binary.left); precompile_bodies_rec(node->u.binary.right); break;
    case AST_UNARY: precompile_bodies_rec(node->u.unary.arg); break;
    case AST_CONDITIONAL: precompile_bodies_rec(node->u.conditional.cond); precompile_bodies_rec(node->u.conditional.consequent); precompile_bodies_rec(node->u.conditional.alternate); break;
    case AST_CALL: precompile_bodies_rec(node->u.call.callee); precompile_bodies_list(node->u.call.args,node->u.call.argc); break;
    case AST_NEW: precompile_bodies_rec(node->u.new_expr.callee); precompile_bodies_list(node->u.new_expr.args,node->u.new_expr.argc); break;
    case AST_MEMBER: precompile_bodies_rec(node->u.member.obj); break;
    case AST_COMPUTED_MEMBER: precompile_bodies_rec(node->u.computed.obj); precompile_bodies_rec(node->u.computed.prop); break;
    case AST_ASSIGN: precompile_bodies_rec(node->u.assign.target); precompile_bodies_rec(node->u.assign.value); break;
    case AST_SEQUENCE: precompile_bodies_list(node->u.sequence.exprs,node->u.sequence.count); break;
    case AST_ARRAY: precompile_bodies_list(node->u.array.elements,node->u.array.nelem); break;
    case AST_OBJECT: precompile_bodies_list(node->u.object.props,node->u.object.nprops); break;
    case AST_PROPERTY: if(node->u.property.key)precompile_bodies_rec(node->u.property.key); precompile_bodies_rec(node->u.property.val); break;
    case AST_SPREAD: case AST_SPREAD_ELEMENT: precompile_bodies_rec(node->u.spread.arg); break;
    case AST_TEMPLATE: precompile_bodies_list(node->u.template_lit.exprs,node->u.template_lit.nexp); break;
    case AST_YIELD: case AST_AWAIT: if(node->u.unary.arg)precompile_bodies_rec(node->u.unary.arg); break;
    case AST_LABEL: precompile_bodies_rec(node->u.label_stmt.stmt); break;
    default: break;
    }
}

void interp_precompile_all_bodies(ASTNode *ast) {
    if (ast) precompile_bodies_rec(ast);
}
int interp_precompile_bodies_cas(ASTNode *ast) {
    if (!ast) return 0;
    int b = bc_body_cache_count;
    precompile_bodies_rec(ast);
    return bc_body_cache_count - b;
}

int interp_collect_all_bodies(ASTNode *ast, ASTNode ***bodies_out, int *count_out) {
    if (!ast || !bodies_out || !count_out) return -1;
    interp_precompile_bodies_cas(ast);
    *count_out = bc_body_cache_count;
    *bodies_out = (ASTNode **)calloc(*count_out, sizeof(ASTNode *));
    if (!*bodies_out) return -1;
    for (int i = 0; i < *count_out; i++)
        (*bodies_out)[i] = bc_body_cache[i].ast_body;
    return 0;
}

void *interp_compile_body_cas(ASTNode *body) {
    return (void *)bc_get_or_compile_body(body);
}

/* Iterate over all precompiled body→BCProgram pairs. The callback is invoked
 * for each entry in bc_body_cache with (ast_body, prog, userdata). */
void interp_iterate_precompiled_bodies(
    void (*callback)(ASTNode *ast_body, BCProgram *prog, void *userdata),
    void *userdata)
{
    if (!callback) return;
    for (int i = 0; i < bc_body_cache_count; i++)
        callback(bc_body_cache[i].ast_body, bc_body_cache[i].prog, userdata);
}

/* ── Arguments scanner: does the function body reference "arguments"? ──── */
static int ast_scans_arguments(ASTNode *n);
static int ast_scans_args_list(ASTNode **items, int c) {
    for (int i = 0; i < c; i++) if (ast_scans_arguments(items[i])) return 1;
    return 0;
}
static int ast_scans_arguments(ASTNode *n) {
    if (!n) return 0;
    switch (n->type) {
    case AST_IDENTIFIER: return (n->u.ident.name && strcmp(n->u.ident.name, "arguments") == 0);
    case AST_PROGRAM: case AST_BLOCK: return ast_scans_args_list(n->u.list.items, n->u.list.count);
    case AST_EXPR_STMT: return ast_scans_arguments(n->u.expr_stmt.expr);
    case AST_RETURN: return n->u.return_stmt.arg && ast_scans_arguments(n->u.return_stmt.arg);
    case AST_BINARY: return ast_scans_arguments(n->u.binary.left) || ast_scans_arguments(n->u.binary.right);
    case AST_UNARY: return ast_scans_arguments(n->u.unary.arg);
    case AST_ASSIGN: return ast_scans_arguments(n->u.assign.target) || ast_scans_arguments(n->u.assign.value);
    case AST_CALL: return ast_scans_arguments(n->u.call.callee) || ast_scans_args_list(n->u.call.args, n->u.call.argc);
    case AST_MEMBER: return ast_scans_arguments(n->u.member.obj);
    case AST_IF: return ast_scans_arguments(n->u.if_stmt.cond) || ast_scans_arguments(n->u.if_stmt.body) || (n->u.if_stmt.else_body && ast_scans_arguments(n->u.if_stmt.else_body));
    case AST_FOR: return (n->u.for_stmt.init&&ast_scans_arguments(n->u.for_stmt.init))||(n->u.for_stmt.test&&ast_scans_arguments(n->u.for_stmt.test))||(n->u.for_stmt.update&&ast_scans_arguments(n->u.for_stmt.update))||ast_scans_arguments(n->u.for_stmt.body);
    case AST_WHILE: case AST_DO_WHILE: return ast_scans_arguments(n->u.if_stmt.cond) || ast_scans_arguments(n->u.if_stmt.body);
    case AST_FOR_IN: return ast_scans_arguments(n->u.for_in.each)||ast_scans_arguments(n->u.for_in.source)||ast_scans_arguments(n->u.for_in.body);
    case AST_FOR_OF: return ast_scans_arguments(n->u.for_of.each)||ast_scans_arguments(n->u.for_of.source)||ast_scans_arguments(n->u.for_of.body);
    case AST_CONDITIONAL: return ast_scans_arguments(n->u.conditional.cond)||ast_scans_arguments(n->u.conditional.consequent)||ast_scans_arguments(n->u.conditional.alternate);
    case AST_ARRAY: return ast_scans_args_list(n->u.array.elements, n->u.array.nelem);
    case AST_OBJECT: return ast_scans_args_list(n->u.object.props, n->u.object.nprops);
    case AST_PROPERTY: return ast_scans_arguments(n->u.property.key) || ast_scans_arguments(n->u.property.val);
    case AST_NEW: return ast_scans_arguments(n->u.new_expr.callee)||ast_scans_args_list(n->u.new_expr.args,n->u.new_expr.argc);
    case AST_SEQUENCE: return ast_scans_args_list(n->u.sequence.exprs, n->u.sequence.count);
    case AST_SWITCH: { if(ast_scans_arguments(n->u.switch_stmt.test))return 1; for(int i=0;i<n->u.switch_stmt.ncases;i++) if(ast_scans_arguments(n->u.switch_stmt.cases[i]))return 1; return 0; }
    case AST_CASE: return (n->u.if_stmt.cond&&ast_scans_arguments(n->u.if_stmt.cond))||ast_scans_arguments(n->u.if_stmt.body);
    case AST_THROW: return ast_scans_arguments(n->u.throw_stmt.arg);
    case AST_TRY: return ast_scans_arguments(n->u.try_stmt.body)||(n->u.try_stmt.catch_body&&ast_scans_arguments(n->u.try_stmt.catch_body))||(n->u.try_stmt.finally_body&&ast_scans_arguments(n->u.try_stmt.finally_body));
    case AST_TEMPLATE: return ast_scans_args_list(n->u.template_lit.exprs,n->u.template_lit.nexp);
    case AST_SPREAD: case AST_SPREAD_ELEMENT: return ast_scans_arguments(n->u.spread.arg);
    case AST_VAR_DECL: for(int i=0;i<n->u.var_decl.nvars;i++){ASTNode*d=n->u.var_decl.vars[i];if(d&&d->type==AST_VAR_DECLARATOR&&d->u.declarator.init&&ast_scans_arguments(d->u.declarator.init))return 1;} return 0;
    case AST_FUNC_DECL: case AST_FUNC_EXPR: case AST_ARROW: return 0;
    default: return 0;
    }
}

/* Public wrapper so the bytecode VM can decide whether a function body may
 * be inlined: the inline fast path does not bind the `arguments` object, so
 * any function that references it MUST NOT be inlined. */
int lr_ast_scans_arguments(ASTNode *n)
{
    return ast_scans_arguments(n);
}

/* ── Forward Declarations ──────────────────────────────────────────────── */

static LRValue interp_eval_node(Interpreter *interp, ASTNode *node);
static LRValue eval_literal(Interpreter *interp, ASTNode *node);
static LRValue eval_identifier(Interpreter *interp, ASTNode *node);
static LRValue eval_binary(Interpreter *interp, ASTNode *node);
static LRValue eval_unary(Interpreter *interp, ASTNode *node);
static LRValue eval_assign(Interpreter *interp, ASTNode *node);
static LRValue eval_member(Interpreter *interp, ASTNode *node);
static LRValue eval_computed_member(Interpreter *interp, ASTNode *node);
static LRValue eval_call(Interpreter *interp, ASTNode *node);
static LRValue eval_new(Interpreter *interp, ASTNode *node);
static LRValue eval_conditional(Interpreter *interp, ASTNode *node);
static LRValue eval_array(Interpreter *interp, ASTNode *node);
static LRValue eval_object(Interpreter *interp, ASTNode *node);
static LRValue eval_func_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_arrow(Interpreter *interp, ASTNode *node);
static LRValue eval_template(Interpreter *interp, ASTNode *node);
static LRValue eval_sequence(Interpreter *interp, ASTNode *node);
static LRValue eval_spread(Interpreter *interp, ASTNode *node);
static LRValue eval_await(Interpreter *interp, ASTNode *node);
static LRValue eval_class_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_pattern(Interpreter *interp, ASTNode *node, LRValue value);
static LRValue eval_program(Interpreter *interp, ASTNode *node);

/* Functions used by init_eval_handlers dispatch table */
static LRValue eval_this_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_super_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_tagged_template(Interpreter *interp, ASTNode *node);
static LRValue eval_yield_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_pattern_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_rest_expr(Interpreter *interp, ASTNode *node);
static LRValue eval_default_val(Interpreter *interp, ASTNode *node);
static LRValue eval_property(Interpreter *interp, ASTNode *node);

/* Functions needed by interp_eval_stmt's default guard */
typedef LRValue (*EvalHandler)(Interpreter *interp, ASTNode *node);
static LRValue eval_statement_dispatch(Interpreter *interp, ASTNode *node);
static EvalHandler eval_handlers[4096];

static void    interp_raise_reference_error(Interpreter *interp, const char *name);
static LRValue eval_var_declarator(Interpreter *interp, ASTNode *declarator, int kind);

static LRValue interp_eval_stmt(Interpreter *interp, ASTNode *node);
static LRValue eval_block(Interpreter *interp, ASTNode *node);
static LRValue eval_if(Interpreter *interp, ASTNode *node);
static LRValue eval_for(Interpreter *interp, ASTNode *node);
static LRValue eval_for_in(Interpreter *interp, ASTNode *node);
static LRValue eval_for_of(Interpreter *interp, ASTNode *node);
static LRValue eval_with(Interpreter *interp, ASTNode *node);
static LRValue eval_while(Interpreter *interp, ASTNode *node);
static LRValue eval_do_while(Interpreter *interp, ASTNode *node);
static LRValue eval_switch(Interpreter *interp, ASTNode *node);
static LRValue eval_break(Interpreter *interp, ASTNode *node);
static LRValue eval_continue(Interpreter *interp, ASTNode *node);
static LRValue eval_return(Interpreter *interp, ASTNode *node);
static LRValue eval_throw(Interpreter *interp, ASTNode *node);
static LRValue eval_try(Interpreter *interp, ASTNode *node);
static LRValue eval_var_decl(Interpreter *interp, ASTNode *node);
static LRValue eval_func_decl(Interpreter *interp, ASTNode *node);
static LRValue eval_class_decl(Interpreter *interp, ASTNode *node);
static LRValue eval_import(Interpreter *interp, ASTNode *node);
static LRValue eval_export(Interpreter *interp, ASTNode *node);

/* Call a JS function (closure-style) with given args */
LRValue interp_call_function(Interpreter *interp, ASTNode *func_node,
                              LRValue this_val, int argc, LRValue *argv);

/* Call a class "constructor" (field init + explicit/implicit constructor) */
static LRValue interp_call_class_function(Interpreter *interp, ASTNode *class_node,
                                          LRValue this_val, int argc, LRValue *argv);

/* Dispatch any callable AST node (function, arrow, or class) */
LRValue interp_invoke_function_ast(Interpreter *interp, ASTNode *ast,
                                   LRValue this_val, int argc, LRValue *argv);

/* ── Scope Name Lookup Cache ──────────────────────────────────────────────
 * The scope cache avoids O(n) strcmp in the scope chain by hashing
 * (name_ptr ^ scope_ptr) into a fixed-size table.  Invalidated whenever
 * the scope chain changes (push/pop).  Cache entries are stable for the
 * lifetime of a scope because names are never removed from a scope.       */

/* Fast string hash for scope cache key (DJB2 variant) */
static inline unsigned scope_name_hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

static inline intptr_t scope_cache_key(const char *name, InterpScope *scope) {
    return (intptr_t)((intptr_t)(uintptr_t)scope_name_hash(name) ^ (intptr_t)scope);
}

static inline int scope_cache_lookup(Interpreter *interp, const char *name,
                                     InterpScope *scope, int *out_idx)
{
    if (!interp->scope_cache_gen) return 0; /* cache not initialized */
    intptr_t key = scope_cache_key(name, scope);
    int slot = (unsigned)(key ^ (key >> 8)) & (SCOPE_CACHE_SIZE - 1);
    if (interp->scope_cache[slot].key == key) {
        *out_idx = interp->scope_cache[slot].index;
        return 1;
    }
    return 0;
}

static inline void scope_cache_add(Interpreter *interp, const char *name,
                                   InterpScope *scope, int idx)
{
    if (!interp->scope_cache_gen) return;
    intptr_t key = scope_cache_key(name, scope);
    int slot = (unsigned)(key ^ (key >> 8)) & (SCOPE_CACHE_SIZE - 1);
    interp->scope_cache[slot].key = key;
    interp->scope_cache[slot].index = idx;
}

static inline void scope_cache_invalidate(Interpreter *interp) {
    interp->scope_cache_gen++;
    if (interp->scope_cache_gen == 0) interp->scope_cache_gen = 1;
    /* Clear cache entries on overflow to avoid stale lookups */
    if (interp->scope_cache_gen == 0 || interp->scope_cache_gen == 1) {
        memset(interp->scope_cache, 0, sizeof(interp->scope_cache));
    }
}

/* ── Scope Management ──────────────────────────────────────────────────── */

/* Thread-local pool of pre-allocated function scopes.
 * Avoids calloc/free overhead on every function call. */
LR_THREAD_LOCAL ScopePool scope_pool = { {NULL}, 0 };

/* Drain the scope pool: free all cached scopes.
 * Exposed so that cleanup code can call it directly. */
void interp_drain_scope_pool(void)
{
    while (scope_pool.count > 0) {
        InterpScope *ps = scope_pool.scopes[--scope_pool.count];
        if (ps->packed_alloc) {
            free(ps);
        }
    }
}

/* __attribute__((destructor)) backstop: drain the scope pool at program exit.
 * This catches any scopes that were pushed to the pool after the last
 * explicit interp_drain_scope_pool call (e.g. by interp_free_scopes). */
__attribute__((destructor))
static void interp_drain_scope_pool_atexit(void)
{
    interp_drain_scope_pool();
}

static void scope_reset(InterpScope *s) {
    /* Values and names are already freed by scope_release before pool push.
     * Only reset flags and metadata — no need to loop over values. */
    s->count = 0;
    s->borrowed_count = 0;
    s->parent = NULL;
    s->is_function_scope = 0;
    s->is_global_scope = 0;
    s->mirror_globals = 0;
    s->refcount = 1;
    s->cache_gen++;  /* invalidate bytecode VM cache entries — the cached
                      * scope_ptr in the bytecode VM's var cache may point
                      * to a previous incarnation of this pooled scope, and
                      * the old scope_gen won't match the new cache_gen. */
}

InterpScope *scope_new(InterpScope *parent, int is_function, int is_global)
{
    int cap = (is_function && !is_global) ? SCOPE_FUNC_CAP : SCOPE_INIT_CAP;
    InterpScope *s;

    /* Try thread-local pool first (avoids malloc/free overhead) */
    s = scope_pool_pop();
    if (likely(s != NULL)) {
        scope_reset(s);
        /* Pool should always have SCOPE_FUNC_CAP-sized scopes, but
         * guard against stale smaller scopes (rare edge case). */
        if (unlikely(s->capacity < cap)) {
            free(s);
            s = NULL;
        }
    }
    if (!s) {
        /* Single packed allocation: struct + 4 arrays in one calloc.
         * All scope types use packed_alloc so they can be pooled. */
        size_t sz = sizeof(InterpScope) + cap*(sizeof(char*)+sizeof(LRValue)+2*sizeof(int));
        s = (InterpScope *)calloc(1, sz);
        if (!s) return NULL;
        char *p = (char *)(s + 1);
        s->names     = (char **)p;  p += cap*sizeof(char*);
        s->values    = (LRValue *)p; p += cap*sizeof(LRValue);
        s->is_const   = (int *)p;    p += cap*sizeof(int);
        s->is_lexical = (int *)p;
        s->packed_alloc = 1;
        s->capacity = cap;
    }

    s->parent = parent;
    if (parent) parent->refcount++;
    s->refcount = 1;
    s->is_function_scope = is_function;
    s->is_global_scope = is_global;
    return s;
}

/* Ensure a scope can hold at least `needed` entries, growing the backing
 * arrays when necessary.  Handles both packed (single-calloc) and
 * heap-allocated scopes.  Returns 1 on success, 0 on OOM. */
static int scope_ensure_capacity(InterpScope *scope, int needed)
{
    if (needed <= scope->capacity) return 1;
    int new_cap = scope->capacity;
    while (new_cap < needed) new_cap *= 2;

    if (scope->packed_alloc) {
        int old_cap = scope->capacity;
        char **old_names = scope->names;
        LRValue *old_values = scope->values;
        int *old_const = scope->is_const;
        int *old_lex = scope->is_lexical;
        scope->names      = (char **)calloc(new_cap, sizeof(char *));
        scope->values     = (LRValue *)calloc(new_cap, sizeof(LRValue));
        scope->is_const   = (int *)calloc(new_cap, sizeof(int));
        scope->is_lexical = (int *)calloc(new_cap, sizeof(int));
        if (!scope->names || !scope->values || !scope->is_const || !scope->is_lexical) {
            free(scope->names);      scope->names = old_names;
            free(scope->values);     scope->values = old_values;
            free(scope->is_const);   scope->is_const = old_const;
            free(scope->is_lexical); scope->is_lexical = old_lex;
            return 0;
        }
        memcpy(scope->names, old_names, (size_t)old_cap * sizeof(char *));
        memcpy(scope->values, old_values, (size_t)old_cap * sizeof(LRValue));
        memcpy(scope->is_const, old_const, (size_t)old_cap * sizeof(int));
        memcpy(scope->is_lexical, old_lex, (size_t)old_cap * sizeof(int));
        /* Do NOT free old arrays — they are part of the packed allocation.
         * packed_alloc is now cleared so scope_release frees the new ones. */
        scope->packed_alloc = 0;
        scope->capacity = new_cap;
    } else {
        char **nn = (char **)realloc(scope->names, (size_t)new_cap * sizeof(char *));
        LRValue *nv = (LRValue *)realloc(scope->values, (size_t)new_cap * sizeof(LRValue));
        int *nc = (int *)realloc(scope->is_const, (size_t)new_cap * sizeof(int));
        int *nl = (int *)realloc(scope->is_lexical, (size_t)new_cap * sizeof(int));
        if (!nn || !nv || !nc || !nl) return 0;
        scope->names = nn; scope->values = nv;
        scope->is_const = nc; scope->is_lexical = nl;
        for (int i = scope->capacity; i < new_cap; i++) {
            scope->names[i] = NULL;
            scope->values[i] = LR_VALUE_UNDEFINED;
            scope->is_const[i] = 0;
            scope->is_lexical[i] = 0;
        }
        scope->capacity = new_cap;
    }
    return 1;
}

/* Pre-populate the global scope with the global object's own data
 * properties (built-ins: Math, Date, JSON, console, ...) so bare
 * identifier reads hit the bytecode variable cache instead of falling
 * through to the slow global-object property lookup on every access
 * (this was the dominant cost in the float_arith benchmark: ~134ns per
 * bare `Math` access).
 *
 * Property names are borrowed (keys are context-lifetime atoms, they
 * outlive the scope); values are dup'd so the scope owns an independent
 * reference.  Entries are non-lexical var-like bindings, so writes mirror
 * back to the global object via the existing mirror path, and external
 * writes to the global object keep the scope in sync via
 * interp_sync_global_binding.  Accessor (getter/setter) properties are
 * skipped.  Runs once at interpreter creation, before any user code. */
void interp_prepopulate_global_scope(Interpreter *interp)
{
    LRContext *ctx = interp->ctx;
    InterpScope *gs = interp->global_scope;
    if (!gs || gs->count > 0) return;

    LRValue global = lr_get_global_object(ctx);
    if (global.tag != LR_TYPE_OBJECT) return;
    LRObject *gobj = (LRObject *)global.u.ptr;

    int n = 0;
    for (LRProperty *p = gobj->prop_hash; p; p = p->next)
        if (!(p->flags & (LR_PROP_GETTER | LR_PROP_SETTER))) n++;

    if (!scope_ensure_capacity(gs, gs->count + n)) {
        lr_free_value(ctx, global);
        return;
    }

    for (LRProperty *p = gobj->prop_hash; p; p = p->next) {
        if (p->flags & (LR_PROP_GETTER | LR_PROP_SETTER)) continue;
        if (!p->key || p->key->len == 0) continue;
        int idx = gs->count++;
        gs->names[idx] = p->key->str;          /* borrowed: atom outlives scope */
        gs->values[idx] = lr_dup_value(ctx, p->value);
        gs->is_const[idx] = 0;
        gs->is_lexical[idx] = 0;
    }
    gs->borrowed_count = gs->count;            /* prepopulated names are borrowed */
    lr_free_value(ctx, global);
}

/* ── Optimized scope creation for bytecode inline call path ─────────────
 *
 * Creates a function scope with `count` entries (this + params) and
 * directly populates all arrays in one shot, avoiding per-entry function
 * call overhead (nparams+1 calls to scope_declare_name_direct eliminated).
 *
 * Values are DUP'd (matching scope_declare_name_direct semantics) — the
 * scope owns one reference per entry and the caller keeps its originals.
 * This is essential: the caller's stack still holds the original values
 * (they are restored after an inline call returns), so a plain memcpy
 * "move" would leave two live references with only one refcount token,
 * causing a use-after-free / double-free when scope_release runs.
 *
 * names[0] should be "this", names[1..count-1] are parameter names.
 * names[0] is marked const (is_const=1, is_lexical=1); the rest are
 * regular (var) bindings.  All names are borrowed pointers.
 *
 * Returns NULL on allocation failure.                                          */
InterpScope *scope_new_inline(InterpScope *parent, int count,
                               const char **names, LRValue *values)
{
    /* ── Core allocation ─────────────────────────────────────────────── */
    int cap = count < SCOPE_FUNC_CAP ? SCOPE_FUNC_CAP : count;
    InterpScope *s;

    s = scope_pool_pop();
    if (likely(s != NULL)) {
        scope_reset(s);
        if (unlikely(s->capacity < cap)) {
            free(s);
            s = NULL;
        }
    }
    if (!s) {
        size_t sz = sizeof(InterpScope) +
                    (size_t)cap * (sizeof(char *) + sizeof(LRValue) + 2 * sizeof(int));
        s = (InterpScope *)calloc(1, sz);
        if (!s) return NULL;
        char *p = (char *)(s + 1);
        s->names      = (char **)p;           p += (size_t)cap * sizeof(char *);
        s->values     = (LRValue *)p;          p += (size_t)cap * sizeof(LRValue);
        s->is_const   = (int *)p;              p += (size_t)cap * sizeof(int);
        s->is_lexical = (int *)p;
        s->packed_alloc = 1;
        s->capacity = cap;
    }

    s->parent = parent;
    if (parent) parent->refcount++;
    s->refcount = 1;
    s->is_function_scope = 1;
    s->is_global_scope = 0;
    s->count = count;
    s->borrowed_count = count;

    /* Duplicate values for the scope.
     * Optimize: only bump refcount for heap-allocated types (object/string).
     * Primitive types just copy the value (no refcount change needed).
     * This avoids unnecessary atomic increments/decrements for primitive
     * parameters which are the common case in small function calls.
     * (Proper refcount handling is preserved for heap types, which was
     * the fix for the ASAN double-free.) */
    for (int i = 0; i < count; i++) {
        LRValue v = values[i];
        if (v.tag == LR_TYPE_STRING) {
            LRString *s_ = (LRString *)v.u.ptr;
            if (s_) s_->ref_count++;
            s->values[i] = v;
        } else if (v.tag == LR_TYPE_OBJECT) {
            LRObject *o_ = (LRObject *)v.u.ptr;
            if (o_) o_->ref_count++;
            s->values[i] = v;
        } else {
            /* Primitive (int32/float64/bool/undefined/null):
             * just copy — no refcount to bump */
            s->values[i] = v;
        }
    }

    /* Set names and flags.  "this" (index 0) is const/lexical. */
    for (int i = 0; i < count; i++) {
        s->names[i] = (char *)names[i];
        s->is_const[i]   = (i == 0) ? 1 : 0;
        s->is_lexical[i] = (i == 0) ? 1 : 0;
    }
    return s;
}

/* scope_new_inline_move: like scope_new_inline but MOVES the values into
 * the scope instead of duplicating them (no refcount bump).  Ownership of
 * the references held by `values[]` is transferred to the scope, which
 * releases them on scope_release.  Used by the inline-call fast path to
 * avoid both a per-arg refcount increment AND a later freeing of the
 * original arg references (which used to leak). */
InterpScope *scope_new_inline_move(InterpScope *parent, int count,
                                   const char **names, LRValue *values)
{
    int cap = count < SCOPE_FUNC_CAP ? SCOPE_FUNC_CAP : count;
    InterpScope *s;

    s = scope_pool_pop();
    if (likely(s != NULL)) {
        /* OPTIMIZATION: Skip scope_reset() — all mutable fields are
         * immediately overwritten below (parent, refcount, flags, count,
         * borrowed_count, values, names, is_const, is_lexical).
         * Only clear mirror_globals since it's not set by this function. */
        s->mirror_globals = 0;
        if (unlikely(s->capacity < cap)) {
            free(s);
            s = NULL;
        }
    }
    if (!s) {
        size_t sz = sizeof(InterpScope) +
                    (size_t)cap * (sizeof(char *) + sizeof(LRValue) + 2 * sizeof(int));
        s = (InterpScope *)calloc(1, sz);
        if (!s) return NULL;
        char *p = (char *)(s + 1);
        s->names      = (char **)p;           p += (size_t)cap * sizeof(char *);
        s->values     = (LRValue *)p;          p += (size_t)cap * sizeof(LRValue);
        s->is_const   = (int *)p;              p += (size_t)cap * sizeof(int);
        s->is_lexical = (int *)p;
        s->packed_alloc = 1;
        s->capacity = cap;
    }

    s->parent = parent;
    if (parent) parent->refcount++;
    s->refcount = 1;
    s->is_function_scope = 1;
    s->is_global_scope = 0;
    s->count = count;
    s->borrowed_count = count;

    /* Move (not dup): the scope now owns the references in `values[]`.
     * Primitive values are copied as-is. */
    for (int i = 0; i < count; i++)
        s->values[i] = values[i];

    /* Set names and flags.  "this" (index 0) is const/lexical. */
    for (int i = 0; i < count; i++) {
        s->names[i] = (char *)names[i];
        s->is_const[i]   = (i == 0) ? 1 : 0;
        s->is_lexical[i] = (i == 0) ? 1 : 0;
    }
    return s;
}

/* Ultra-lean scope for PURE inline calls.  See header for the rationale:
 * is_pure guarantees the body never does name-based lookups and never
 * creates closures, so names[]/is_const[]/is_lexical[] are never read and
 * borrowed_count = count makes scope_release skip name frees. */
/* scope_new_inline_fast and scope_release_inline are static inline
 * in lr_interp.h now — they are inlined directly into the hot call
 * path in bytecode dispatch for maximum performance via cross-TU LTO. */

/* Drop one reference; frees the scope (and cascades up the parent chain)
 * when the count reaches zero. Function scopes (packed_alloc) are returned
 * to a thread-local pool for reuse instead of being freed. */
void scope_release(InterpScope *scope, LRContext *ctx)
{
    while (scope) {
        if (--scope->refcount > 0) return;
        /* Sentinel: if refcount was already set negative by a prior
         * invocation of scope_release on the same scope (i.e. we are
         * in a recursive call triggered by lr_free_value of a scope
         * value whose def_scope points back to this scope), return
         * immediately to prevent double-free. */
        if (scope->refcount < 0) return;
        InterpScope *parent = scope->parent;
        /* Set count=0 and detach from parent BEFORE freeing values.
         * This prevents re-entrant scope_release (triggered when a
         * function object's def_scope points back to this scope, which
         * happens for closures defined in block scopes) from
         * double-freeing values or cascading to the already-freed
         * parent. */
        int cnt = scope->count;
        scope->count = 0;
        scope->parent = NULL;
        /* Set refcount negative as a sentinel so that if a recursive
         * scope_release is triggered by lr_free_value below (when a
         * closure's def_scope points to the same scope), it returns
         * early via the sentinel check above instead of double-freeing. */
        scope->refcount = -1;
        if (scope->packed_alloc) {
            /* For packed_alloc (function scopes), free names/values but
             * keep the memory block for reuse via the pool. */
            int borrow = scope->borrowed_count;
            for (int i = 0; i < cnt; i++) {
                if (i >= borrow && scope->names[i]) { free(scope->names[i]); scope->names[i] = NULL; }
                FREE_IF_HEAP(ctx, scope->values[i]);
                scope->values[i] = LR_VALUE_UNDEFINED;
            }
            scope_pool_push(scope);
            scope = parent;
            continue;
        }
        /* Non-function scope (not packed_alloc): free everything */
        int borrow = scope->borrowed_count;
        for (int i = 0; i < cnt; i++) {
            if (i >= borrow && scope->names[i]) { free(scope->names[i]); scope->names[i] = NULL; }
            FREE_IF_HEAP(ctx, scope->values[i]);
            scope->values[i] = LR_VALUE_UNDEFINED;
        }
        free(scope->names); free(scope->values);
        free(scope->is_const); free(scope->is_lexical);
        free(scope);
        scope = parent;
    }
}

/* Hook target for lr_engine: release a function object's captured scope */
static void interp_closure_release_hook(void *scope, LRContext *ctx)
{
    scope_release((InterpScope *)scope, ctx);
}

/* Free all scopes in the interpreter's scope chain during runtime cleanup.
 * Called from lr_runtime_free after JS_FreeContext breaks circular refs.
 *
 * Uses scope_release (which handles re-entrant cleanup via the refcount
 * sentinel) instead of manually freeing values.  This is critical because
 * generator objects (opaque_free = gen_lazy_data_free) call scope_release
 * on their creation scope during lr_free_object, which would race with
 * manual value freeing and cause a use-after-free.
 *
 * scope_release cascades through the parent chain automatically, so we
 * only need to call it on the head of the chain. */
void interp_free_scopes(LRContext *ctx)
{
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp) return;
    /* scope_release handles the entire chain via its while loop, so
     * just call it on the head scope.  Force refcount to 1 so that
     * scope_release will actually free it (the previous manual path
     * ignored refcount entirely). */
    InterpScope *head = interp->current_scope;
    if (head) {
        head->refcount = 1;
        scope_release(head, ctx);
    }
    interp->current_scope = NULL;
    interp->global_scope = NULL;
}

/* Capture the current scope into a function object for lexical closures */
static void interp_capture_closure(Interpreter *interp, LRValue fn_obj)
{
    if (fn_obj.tag != LR_TYPE_OBJECT || !interp->current_scope) return;
    LRObject *o = (LRObject *)fn_obj.u.ptr;
    if (o->def_scope) return;
    interp->current_scope->refcount++;
    o->def_scope = interp->current_scope;
}

/* Forward decl (defined below, after scope helpers). */
static void shadow_restore_for_scope(Interpreter *interp, InterpScope *popped);

static void interp_push_scope(Interpreter *interp, int is_function_scope)
{
    InterpScope *s = scope_new(interp->current_scope, is_function_scope, 0);
    if (interp->current_scope == NULL) {
        s->is_global_scope = 1;
        s->mirror_globals = !interp->is_module;
    }
    interp->current_scope = s;
}

void interp_pop_scope(Interpreter *interp)
{
    if (!interp->current_scope) return;
    InterpScope *old = interp->current_scope;
    interp->current_scope = old->parent;
    /* Restore any let/const bindings this block scope shadowed from outer
     * scopes *before* releasing the scope (scope_release frees values). */
    shadow_restore_for_scope(interp, old);
    scope_release(old, interp->ctx);
}

/* Find the nearest function scope (or global) - for var hoisting */
static InterpScope *find_function_scope(Interpreter *interp)
{
    InterpScope *s = interp->current_scope;
    while (s) {
        if (s->is_function_scope || s->is_global_scope) return s;
        s = s->parent;
    }
    return interp->global_scope;
}

    /* Look up a name in the scope chain.
     * Returns 1 if found (value set), 0 if not found.
     * For mirrored script-mode global var/function bindings the value is
     * read authoritatively from the global object so that bare `x` and
     * `globalThis.x` are the same binding (two-way consistency). */
static int scope_lookup_internal(Interpreter *interp, InterpScope *scope,
                                 const char *name, LRValue *value)
{
    LRContext *ctx = interp->ctx;
    (void)ctx;
    while (__builtin_expect(scope != NULL, 1)) {
        int idx;
        /* Check cache first for O(1) lookup (hot path) */
        if (__builtin_expect(scope_cache_lookup(interp, name, scope, &idx), 1)) {
            if (__builtin_expect(idx >= 0 && idx < scope->count, 1)) {
                char *sname = scope->names[idx];
                if (__builtin_expect(sname != NULL && sname[0] == name[0] && strcmp(sname, name) == 0, 1)) {
                    if (value) *value = lr_dup_value(NULL, scope->values[idx]);
                    return 1;
                }
            }
        }
        /* Linear search (cache miss) */
        for (int i = 0; i < scope->count; i++) {
            if (scope->names[i] && scope->names[i][0] == name[0] &&
                strcmp(scope->names[i], name) == 0) {
                scope_cache_add(interp, name, scope, i);
                if (value) *value = lr_dup_value(NULL, scope->values[i]);
                return 1;
            }
        }
        scope = scope->parent;
    }
    return 0;
}

/* ES spec (GlobalDeclarationInstantiation): in Script mode, top-level var
 * and function declarations create properties of the global object.
 * Mirror such bindings onto the global object (scope stays source of truth
 * for reads; writes are kept in sync by scope_set_name). */
static void mirror_global_binding(Interpreter *interp, const char *name, LRValue value)
{
    LRValue global = lr_get_global_object(interp->ctx);
    lr_set_property_str(interp->ctx, global, name,
                        lr_dup_value(interp->ctx, value));
    lr_free_value(interp->ctx, global);
}

/* Two-way global binding (closure of the external-write gap): when something
 * assigns to the global object directly (e.g. `globalThis.x = v`), and `name`
 * is a top-level var/function binding in the global scope, mirror the value
 * back into the interpreter's global scope so a subsequent bare `x` sees it.
 * The scope stays the source of truth for reads; this is called only from
 * lr_set_property (write path), never from the hot identifier-lookup path, so
 * it cannot reintroduce the memory-corruption issue that reading the global
 * object in scope_lookup_internal caused. */
void interp_sync_global_binding(LRContext *ctx, const char *name, LRValue val)
{
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp || !interp->global_scope || interp->is_module)
        return;
    InterpScope *gs = interp->global_scope;
    for (int i = 0; i < gs->count; i++) {
        if (gs->names[i] && strcmp(gs->names[i], name) == 0
            && !gs->is_lexical[i]) {
            lr_free_value(ctx, gs->values[i]);
            gs->values[i] = lr_dup_value(ctx, val);
            break;
        }
    }
}

/* Fast path: declare a name in a freshly-created function scope.
 * Skips duplicate check and scope-chain walk since we know the scope
 * is brand new. Used by interp_call_function for parameter binding.
 * Names are borrowed from AST (or string literals) — no strdup needed
 * since the source lives for the entire program lifetime. */
void scope_declare_name_direct(InterpScope *scope, const char *name,
                                       LRValue value, int kind)
{
    if (scope->count >= scope->capacity) return;
    int idx = scope->count++;
    scope->names[idx] = (char *)name;  /* borrowed pointer, not strdup'd */
    scope->values[idx] = lr_dup_value(NULL, value);
    scope->is_const[idx] = 0;
    scope->is_lexical[idx] = (kind != 0) ? 1 : 0;
    scope->borrowed_count = scope->count;  /* all names so far are borrowed */
}

/* ── Block-scope shadow restoration ────────────────────────────────────
 * Interaction between scope_declare_name and interp_pop_scope.
 *
 * This engine hoists every let/const to the nearest function/global scope
 * (see scope_declare_name), so a block `{ let y = 2; }` blindly overwrites
 * the slot of an outer `let y = 1` and, when the block ends, the outer
 * binding is lost (V8 expects it restored to 1).  We record the previous
 * value whenever a let/const declared inside a *block scope* (i.e. the
 * scope pushed by a block, not a function scope) shadows an already-existing
 * binding, and re-apply it when that block scope is popped.  Only kind != 0
 * (let/const/class) participates: `var` is intentionally function-scoped and
 * must survive past the block. */
typedef struct ShadowRestore ShadowRestore;
struct ShadowRestore {
    ShadowRestore *next;
    InterpScope   *scope;   /* block scope on whose pop we restore */
    char          *name;    /* strdup'd: owned by this entry */
    LRValue        saved;   /* dup'd: moved into the scope on restore */
    int            applied; /* set when saved has been moved into a scope */
};
static LR_THREAD_LOCAL ShadowRestore *shadow_restore_head = NULL;

/* Save the current binding of `name` so it can be restored when `scope` pops. */
static void shadow_save(InterpScope *scope, const char *name, LRValue old)
{
    ShadowRestore *e = (ShadowRestore *)malloc(sizeof(ShadowRestore));
    if (!e) return;
    e->scope    = scope;
    e->name     = strdup(name);
    e->saved    = old;
    e->applied  = 0;
    e->next     = shadow_restore_head;
    shadow_restore_head = e;
}

/* Restore all let/const bindings that `popped` shadowed from an outer scope. */
static void shadow_restore_for_scope(Interpreter *interp, InterpScope *popped)
{
    ShadowRestore **pp = &shadow_restore_head;
    while (*pp) {
        ShadowRestore *e = *pp;
        if (e->scope != popped) { pp = &e->next; continue; }
        /* Remove this entry from the list */
        *pp = e->next;
        /* The binding lives in some ancestor of the popped block scope. */
        InterpScope *s = popped->parent;
        while (s) {
            int found = 0;
            for (int i = 0; i < s->count; i++) {
                if (s->names[i] && strcmp(s->names[i], e->name) == 0) {
                    lr_free_value(interp->ctx, s->values[i]);
                    s->values[i] = e->saved;
                    e->applied = 1;
                    found = 1;
                    /* keep global mirror (if any) in sync */
                    if (s->is_global_scope && !s->is_lexical[i] && s->mirror_globals)
                        mirror_global_binding(interp, e->name, e->saved);
                    scope_cache_invalidate(interp);
                    break;
                }
            }
            if (found) break;
            s = s->parent;
        }
        if (!e->applied) {
            /* binding no longer resolvable — release the saved value */
            lr_free_value(interp->ctx, e->saved);
        }
        free(e->name);
        free(e);
    }
}

/* Declare a variable in the current scope (for let/const) or function scope (for var).
 * kind: 0=var, 1=let, 2=const */
static void scope_declare_name(Interpreter *interp, const char *name, LRValue value, int kind)
{
    /* NOTE: the engine currently treats let/const like var for scoping and
     * const-enforcement purposes (pre-existing behavior, preserved here so we
     * only change what the user asked for). We therefore always hoist the
     * binding to the nearest function/global scope. The *only* thing driven by
     * the real declaration kind is the global-object mirror: per the ES spec,
     * only top-level var/function (kind == 0) become properties of the global
     * object in Script mode; let/const/class do not. */
    InterpScope *scope = find_function_scope(interp);

    int mirror = (kind == 0 && scope->is_global_scope && scope->mirror_globals);

    /* Check if already declared in this scope */
    for (int i = 0; i < scope->count; i++) {
        if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
            /* Block-scope shadow: a let/const declared while a block scope is
             * current is hoisted here and overwrites an outer binding.  Save
             * the old value so interp_pop_scope restores it on block exit. */
            if (kind != 0 && interp->current_scope &&
                interp->current_scope != scope &&
                !interp->current_scope->is_function_scope &&
                !interp->current_scope->is_global_scope) {
                shadow_save(interp->current_scope, name,
                            lr_dup_value(interp->ctx, scope->values[i]));
            }
            /* Redeclaration in same scope - update value */
            lr_free_value(interp->ctx, scope->values[i]);
            scope->values[i] = lr_dup_value(interp->ctx, value);
            if (mirror && !scope->is_lexical[i])
                mirror_global_binding(interp, name, value);
            return;
        }
    }

    /* Add new entry */
    if (scope->count >= scope->capacity) {
        if (scope->packed_alloc) {
            /* Function scope capacity exhausted — convert to heap-allocated
             * so we can grow dynamically.  This is rare (only happens for
             * functions with > SCOPE_FUNC_CAP locals), but must be handled
             * correctly instead of silently dropping the variable. */
            int old_cap = scope->capacity;
            int new_cap = old_cap * 2;
            char **old_names = scope->names;
            LRValue *old_values = scope->values;
            int *old_const = scope->is_const;
            int *old_lex = scope->is_lexical;
            scope->names     = (char **)calloc(new_cap, sizeof(char *));
            scope->values    = (LRValue *)calloc(new_cap, sizeof(LRValue));
            scope->is_const  = (int *)calloc(new_cap, sizeof(int));
            scope->is_lexical = (int *)calloc(new_cap, sizeof(int));
            if (!scope->names || !scope->values || !scope->is_const || !scope->is_lexical) {
                /* allocation failure — try to limp along with the old arrays */
                free(scope->names); scope->names = old_names;
                free(scope->values); scope->values = old_values;
                free(scope->is_const); scope->is_const = old_const;
                free(scope->is_lexical); scope->is_lexical = old_lex;
                return;
            }
            memcpy(scope->names, old_names, old_cap * sizeof(char *));
            memcpy(scope->values, old_values, old_cap * sizeof(LRValue));
            memcpy(scope->is_const, old_const, old_cap * sizeof(int));
            memcpy(scope->is_lexical, old_lex, old_cap * sizeof(int));
            /* Do NOT free old arrays — they are part of the packed allocation.
             * packed_alloc flag is now cleared so scope_release will free them. */
            scope->packed_alloc = 0;
            scope->capacity = new_cap;
        } else {
            scope->capacity *= 2;
            scope->names = (char **)realloc(scope->names, scope->capacity * sizeof(char *));
            scope->values = (LRValue *)realloc(scope->values, scope->capacity * sizeof(LRValue));
            scope->is_const = (int *)realloc(scope->is_const, scope->capacity * sizeof(int));
            scope->is_lexical = (int *)realloc(scope->is_lexical, scope->capacity * sizeof(int));
            for (int i = scope->count; i < scope->capacity; i++) {
                scope->names[i] = NULL;
                scope->values[i] = LR_VALUE_UNDEFINED;
                scope->is_const[i] = 0;
                scope->is_lexical[i] = 0;
            }
        }
    }
    scope->names[scope->count] = strdup(name);
    scope->values[scope->count] = lr_dup_value(interp->ctx, value);
    scope->is_const[scope->count] = 0;   /* const enforcement left as the
                                           * engine's pre-existing behavior */
    scope->is_lexical[scope->count] = (kind != 0) ? 1 : 0;
    scope->count++;

    if (mirror)
        mirror_global_binding(interp, name, value);
}

/* Set a variable's value in the scope chain.
 * Returns 1 if found and set, 0 if not found. */
static int scope_set_name(Interpreter *interp, const char *name, LRValue value)
{
    InterpScope *scope = interp->current_scope;
    while (scope) {
        int idx;
        /* Check cache first for O(1) lookup */
        if (scope_cache_lookup(interp, name, scope, &idx)) {
            if (idx >= 0 && idx < scope->count && scope->names[idx] &&
                strcmp(scope->names[idx], name) == 0) {
                if (scope->is_const[idx]) {
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "Assignment to constant variable '%s'", name);
                    interp->error_flag = 1;
                    return 0;
                }
                lr_free_value(interp->ctx, scope->values[idx]);
                scope->values[idx] = lr_dup_value(interp->ctx, value);
                if (scope->is_global_scope && !scope->is_lexical[idx] &&
                    scope->mirror_globals)
                    mirror_global_binding(interp, name, value);
                return 1;
            }
        }
        /* Linear search */
        for (int i = 0; i < scope->count; i++) {
            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                scope_cache_add(interp, name, scope, i);
                if (scope->is_const[i]) {
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "Assignment to constant variable '%s'", name);
                    interp->error_flag = 1;
                    return 0;
                }
                lr_free_value(interp->ctx, scope->values[i]);
                scope->values[i] = lr_dup_value(interp->ctx, value);
                if (scope->is_global_scope && !scope->is_lexical[i] &&
                    scope->mirror_globals)
                    mirror_global_binding(interp, name, value);
                return 1;
            }
        }
        scope = scope->parent;
    }
    return 0;
}

/* ── Helper Functions ──────────────────────────────────────────────────── */

static double to_number(LRContext *ctx, LRValue val)
{
    double d;
    lr_to_float64(ctx, &d, val);
    return d;
}

static int32_t to_int32(LRContext *ctx, LRValue val)
{
    int32_t i;
    lr_to_int32(ctx, &i, val);
    return i;
}

/* Strict equality comparison */
static int strict_eq(LRValue a, LRValue b)
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
        if (sa->len != sb->len) return 0;
        return memcmp(sa->str, sb->str, sa->len) == 0;
    }
    case LR_TYPE_OBJECT:
        return a.u.ptr == b.u.ptr;
    case LR_TYPE_SYMBOL:
        return a.u.ptr == b.u.ptr;
    default:
        return 0;
    }
}

/* Abstract equality comparison (==) */
static int abstract_eq(LRContext *ctx, LRValue a, LRValue b)
{
    /* Same type - use strict equality */
    if (a.tag == b.tag) return strict_eq(a, b);

    /* undefined == null */
    if ((a.tag == LR_TYPE_UNDEFINED && b.tag == LR_TYPE_NULL) ||
        (a.tag == LR_TYPE_NULL && b.tag == LR_TYPE_UNDEFINED)) return 1;

    /* Number vs String - convert string to number */
    if (a.tag == LR_TYPE_STRING && (b.tag == LR_TYPE_INT32 || b.tag == LR_TYPE_FLOAT64)) {
        double da = to_number(ctx, a);
        double db = to_number(ctx, b);
        return da == db;
    }
    if ((a.tag == LR_TYPE_INT32 || a.tag == LR_TYPE_FLOAT64) && b.tag == LR_TYPE_STRING) {
        double da = to_number(ctx, a);
        double db = to_number(ctx, b);
        return da == db;
    }

    /* Bool vs anything - convert bool to number */
    if (a.tag == LR_TYPE_BOOL) {
        LRValue na = lr_new_int32(ctx, a.u.bool_val ? 1 : 0);
        int r = abstract_eq(ctx, na, b);
        lr_free_value(ctx, na);
        return r;
    }
    if (b.tag == LR_TYPE_BOOL) {
        LRValue nb = lr_new_int32(ctx, b.u.bool_val ? 1 : 0);
        int r = abstract_eq(ctx, a, nb);
        lr_free_value(ctx, nb);
        return r;
    }

    /* Object vs String/Number - convert to primitive */
    if (a.tag == LR_TYPE_OBJECT && (b.tag == LR_TYPE_STRING || b.tag == LR_TYPE_INT32 || b.tag == LR_TYPE_FLOAT64)) {
        const char *str = lr_to_cstring(ctx, a);
        LRValue prim = lr_new_string(ctx, str);
        lr_free_cstring(ctx, str);
        int r = abstract_eq(ctx, prim, b);
        lr_free_value(ctx, prim);
        return r;
    }
    if ((a.tag == LR_TYPE_STRING || a.tag == LR_TYPE_INT32 || a.tag == LR_TYPE_FLOAT64) && b.tag == LR_TYPE_OBJECT) {
        const char *str = lr_to_cstring(ctx, b);
        LRValue prim = lr_new_string(ctx, str);
        lr_free_cstring(ctx, str);
        int r = abstract_eq(ctx, a, prim);
        lr_free_value(ctx, prim);
        return r;
    }

    /* Number vs undefined/null */
    if ((a.tag == LR_TYPE_INT32 || a.tag == LR_TYPE_FLOAT64) &&
        (b.tag == LR_TYPE_UNDEFINED || b.tag == LR_TYPE_NULL)) return 0;
    if ((a.tag == LR_TYPE_UNDEFINED || a.tag == LR_TYPE_NULL) &&
        (b.tag == LR_TYPE_INT32 || b.tag == LR_TYPE_FLOAT64)) return 0;

    return 0;
}

/* ── Expression Evaluators ─────────────────────────────────────────────── */

static LRValue eval_literal(Interpreter *interp, ASTNode *node)
{
    (void)interp;
    LRTokType tt = node->token.type;
    if (tt == TOK_NUMBER) {
        double d = node->u.number.num;
        if (d == (double)(int32_t)d && !isnan(d) && !isinf(d)) {
            return lr_new_int32(interp->ctx, (int32_t)d);
        }
        return lr_new_float64(interp->ctx, d);
    }
    if (tt == TOK_BIGINT_LIT) {
        return lr_new_bigint(interp->ctx, node->u.bigint_val.val);
    }
    if (tt == TOK_STRING) {
        const char *s = node->u.string.str;
        return lr_new_string(interp->ctx, s ? s : "");
    }
    if (tt == TOK_BOOL_LIT) {
        return lr_new_bool(interp->ctx, node->u.bool_val.val);
    }
    if (tt == TOK_NULL_LIT) {
        return LR_VALUE_NULL;
    }
    if (tt == TOK_UNDEFINED_LIT) {
        return LR_VALUE_UNDEFINED;
    }
    /* Fallback: check token type via string comparison */
    if (node->u.number.num == -1) return LR_VALUE_UNDEFINED;
    return LR_VALUE_NULL;
}

static LRValue eval_identifier(Interpreter *interp, ASTNode *node)
{
    const char *name = node->u.ident.name;
    if (!name) return LR_VALUE_UNDEFINED;

    LRValue val;
    if (scope_lookup_internal(interp, interp->current_scope, name, &val)) {
        return val;
    }

    /* Global object cache: hash the name to check if we've recently
     * accessed this global property. Cache hit returns O(1). */
    int gi = ((name[0] * 31 + strlen(name)) & (GLOBAL_CACHE_SIZE - 1));
    if (interp->global_cache[gi].name == name) {
        return lr_dup_value(interp->ctx, interp->global_cache[gi].val);
    }

    /* Check global object (slow) */
    LRValue global = lr_get_global_object(interp->ctx);
    val = lr_get_property_str(interp->ctx, global, name);
    lr_free_value(interp->ctx, global);
    if (!lr_is_undefined(val)) {
        /* Cache the result for next time */
        interp->global_cache[gi].name = name;
        interp->global_cache[gi].val = val;
        return val;
    }
    lr_free_value(interp->ctx, val);

    /* ReferenceError for undeclared identifiers */
    interp_raise_reference_error(interp, name);
    return LR_VALUE_UNDEFINED;
}

/* Fast free: only call lr_free_value for heap-allocated types (string/object).
 * int32/float64/bool/undefined/null are value types with no heap resource,
 * so skipping the function call saves measurable overhead in tight loops.
 * (The fast FREE_IF_HEAP with inline refcount decrement is defined above
 * scope_release; it is used by both the VM hot paths and scope teardown.) */

/* Raise a ReferenceError by calling lr_throw_reference_error and adopting
 * the returned Error object as the interpreter's exception value. */
static void interp_raise_reference_error(Interpreter *interp, const char *name)
{
    LRValue err = lr_throw_reference_error(interp->ctx, "%s is not defined", name);
    if (interp->error_flag) {
        /* lr_throw_* sets error_flag + exception_value via the preamble */
        interp->error_flag = 1;
        /* Copy the real thrown value if the engine gave us one */
        if (interp->ctx->current_exception.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, interp->exception_value);
            interp->exception_value = lr_dup_value(interp->ctx, interp->ctx->current_exception);
            interp->exception_pending = 1;
        } else if (err.tag != LR_TYPE_EXCEPTION && err.tag != LR_TYPE_UNDEFINED) {
            interp->exception_value = err;
            interp->exception_pending = 1;
            return;
        }
    } else {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "'%s' is not defined", name);
        interp->error_flag = 1;
    }
    if (err.tag != LR_TYPE_EXCEPTION && err.tag != LR_TYPE_UNDEFINED)
        lr_free_value(interp->ctx, err);
}

static LRValue eval_binary(Interpreter *interp, ASTNode *node)
{
    const char *op = node->u.binary.op;

    /* Short-circuit logical operators */
    if (strcmp(op, "&&") == 0) {
        LRValue left = interp_eval_node(interp, node->u.binary.left);
        if (interp->error_flag) return left;
        int truthy = lr_to_bool(interp->ctx, left);
        if (!truthy) return left;
        lr_free_value(interp->ctx, left);
        return interp_eval_node(interp, node->u.binary.right);
    }
    if (strcmp(op, "||") == 0) {
        LRValue left = interp_eval_node(interp, node->u.binary.left);
        if (interp->error_flag) return left;
        int truthy = lr_to_bool(interp->ctx, left);
        if (truthy) return left;
        lr_free_value(interp->ctx, left);
        return interp_eval_node(interp, node->u.binary.right);
    }
    if (strcmp(op, "??") == 0) {
        LRValue left = interp_eval_node(interp, node->u.binary.left);
        if (interp->error_flag) return left;
        if (!lr_is_null(left) && !lr_is_undefined(left)) return left;
        lr_free_value(interp->ctx, left);
        return interp_eval_node(interp, node->u.binary.right);
    }

    /* Evaluate both sides */
    LRValue left = interp_eval_node(interp, node->u.binary.left);
    if (interp->error_flag) return left;
    LRValue right = interp_eval_node(interp, node->u.binary.right);
    if (interp->error_flag) {
        lr_free_value(interp->ctx, left);
        return right;
    }

    LRValue result = LR_VALUE_UNDEFINED;

    /* ── Integer Fast Path ──────────────────────────────────────────────── */
    /* For common arithmetic operations on two int32 values, avoid boxing.
     * This is a critical optimization for tight loops. */
    if (left.tag == LR_TYPE_INT32 && right.tag == LR_TYPE_INT32) {
        int32_t a = left.u.int32;
        int32_t b = right.u.int32;
        int32_t res;
        int fast = 1;

        if (strcmp(op, "+") == 0) {
            res = a + b;
        } else if (strcmp(op, "-") == 0) {
            res = a - b;
        } else if (strcmp(op, "*") == 0) {
            res = a * b;
        } else if (strcmp(op, "&") == 0) {
            res = a & b;
        } else if (strcmp(op, "|") == 0) {
            res = a | b;
        } else if (strcmp(op, "^") == 0) {
            res = a ^ b;
        } else if (strcmp(op, "<<") == 0) {
            res = a << (b & 31);
        } else if (strcmp(op, ">>") == 0) {
            res = a >> (b & 31);
        } else if (strcmp(op, ">>>") == 0) {
            res = (int32_t)((uint32_t)a >> (b & 31));
        } else if (strcmp(op, "<") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a < b);
        } else if (strcmp(op, ">") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a > b);
        } else if (strcmp(op, "<=") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a <= b);
        } else if (strcmp(op, ">=") == 0) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_bool(interp->ctx, a >= b);
        } else {
            fast = 0;
        }

        if (fast) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return lr_new_int32(interp->ctx, res);
        }
    }

    /* ── End Integer Fast Path ──────────────────────────────────────────── */

    /* String concatenation with + */
    if (strcmp(op, "+") == 0) {
        if (lr_is_string(left) || lr_is_string(right)) {
            const char *sl = lr_to_cstring(interp->ctx, left);
            const char *sr = lr_to_cstring(interp->ctx, right);
            size_t llen = strlen(sl);
            size_t rlen = strlen(sr);
            char *buf = (char *)malloc(llen + rlen + 1);
            if (buf) {
                memcpy(buf, sl, llen);
                memcpy(buf + llen, sr, rlen);
                buf[llen + rlen] = '\0';
                result = lr_new_string(interp->ctx, buf);
                free(buf);
            }
            lr_free_cstring(interp->ctx, sl);
            lr_free_cstring(interp->ctx, sr);
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            return result;
        }
        /* Numeric addition */
        double da = to_number(interp->ctx, left);
        double db = to_number(interp->ctx, right);
        double sum = da + db;
        if (sum == (double)(int32_t)sum && !isnan(sum) && !isinf(sum)) {
            result = lr_new_int32(interp->ctx, (int32_t)sum);
        } else {
            result = lr_new_float64(interp->ctx, sum);
        }
        lr_free_value(interp->ctx, left);
        lr_free_value(interp->ctx, right);
        return result;
    }

    /* Numeric-only operations */
    double da = to_number(interp->ctx, left);
    double db = to_number(interp->ctx, right);

    if (strcmp(op, "-") == 0) {
        double v = da - db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "*") == 0) {
        double v = da * db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "/") == 0) {
        if (db == 0.0) {
            result = lr_new_float64(interp->ctx, da / db); /* Infinity or NaN */
        } else {
            double v = da / db;
            result = lr_new_float64(interp->ctx, v);
        }
    } else if (strcmp(op, "%") == 0) {
        if (db == 0.0) {
            result = lr_new_float64(interp->ctx, NAN);
        } else {
            double v = fmod(da, db);
            if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
                result = lr_new_int32(interp->ctx, (int32_t)v);
            else
                result = lr_new_float64(interp->ctx, v);
        }
    } else if (strcmp(op, "**") == 0) {
        double v = pow(da, db);
        result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "<") == 0) {
        result = lr_new_bool(interp->ctx, da < db);
    } else if (strcmp(op, ">") == 0) {
        result = lr_new_bool(interp->ctx, da > db);
    } else if (strcmp(op, "<=") == 0) {
        result = lr_new_bool(interp->ctx, da <= db);
    } else if (strcmp(op, ">=") == 0) {
        result = lr_new_bool(interp->ctx, da >= db);
    } else if (strcmp(op, "==") == 0) {
        result = lr_new_bool(interp->ctx, abstract_eq(interp->ctx, left, right));
    } else if (strcmp(op, "!=") == 0) {
        result = lr_new_bool(interp->ctx, !abstract_eq(interp->ctx, left, right));
    } else if (strcmp(op, "===") == 0) {
        result = lr_new_bool(interp->ctx, strict_eq(left, right));
    } else if (strcmp(op, "!==") == 0) {
        result = lr_new_bool(interp->ctx, !strict_eq(left, right));
    } else if (strcmp(op, "&") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da & (int32_t)db);
    } else if (strcmp(op, "|") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da | (int32_t)db);
    } else if (strcmp(op, "^") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da ^ (int32_t)db);
    } else if (strcmp(op, "<<") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da << (int32_t)db);
    } else if (strcmp(op, ">>") == 0) {
        result = lr_new_int32(interp->ctx, (int32_t)da >> (int32_t)db);
    } else if (strcmp(op, ">>>") == 0) {
        result = lr_new_int32(interp->ctx, (uint32_t)(int32_t)da >> (int32_t)db);
    } else if (strcmp(op, "in") == 0) {
        /* property in object */
        if (!lr_is_object(right)) {
            lr_free_value(interp->ctx, left);
            lr_free_value(interp->ctx, right);
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "right-hand side of 'in' must be an object");
            interp->error_flag = 1;
            return LR_VALUE_UNDEFINED;
        }
        const char *prop = lr_to_cstring(interp->ctx, left);
        LRString *atom = lr_new_atom(interp->ctx, prop);
        result = lr_new_bool(interp->ctx, lr_has_property(interp->ctx, right, atom));
        lr_free_cstring(interp->ctx, prop);
    } else if (strcmp(op, "instanceof") == 0) {
        /* obj instanceof Func - check right.prototype in left's prototype chain */
        if (!lr_is_object(left)) {
            result = lr_new_bool(interp->ctx, 0);
        } else {
            LRValue proto = lr_get_property_str(interp->ctx, right, "prototype");
            if (!lr_is_object(proto)) {
                lr_free_value(interp->ctx, proto);
                result = lr_new_bool(interp->ctx, 0);
            } else {
                int found = 0;
                LRValue p = lr_get_prototype(interp->ctx, left);
                while (lr_is_object(p)) {
                    if (strict_eq(p, proto)) {
                        found = 1;
                        break;
                    }
                    LRValue next = lr_get_prototype(interp->ctx, p);
                    lr_free_value(interp->ctx, p);
                    p = next;
                }
                lr_free_value(interp->ctx, p);
                lr_free_value(interp->ctx, proto);
                result = lr_new_bool(interp->ctx, found);
            }
        }
    } else {
        /* Unknown operator, return undefined */
        result = LR_VALUE_UNDEFINED;
    }

    lr_free_value(interp->ctx, left);
    lr_free_value(interp->ctx, right);
    return result;
}

static LRValue eval_unary(Interpreter *interp, ASTNode *node)
{
    const char *op = node->u.unary.op;
    int prefix = node->u.unary.prefix;

    if (strcmp(op, "typeof") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        /* ES spec: typeof on an unresolvable identifier must NOT throw a
         * ReferenceError; it evaluates to "undefined". */
        if (interp->error_flag &&
            node->u.unary.arg && node->u.unary.arg->type == AST_IDENTIFIER) {
            interp->error_flag = 0;
            interp->error_message[0] = '\0';
            interp->exception_pending = 0;
            lr_free_value(interp->ctx, arg);
            return lr_new_string(interp->ctx, "undefined");
        }
        const char *type_str = "undefined";
        switch (arg.tag) {
        case LR_TYPE_UNDEFINED: type_str = "undefined"; break;
        case LR_TYPE_NULL:      type_str = "object"; break;
        case LR_TYPE_BOOL:      type_str = "boolean"; break;
        case LR_TYPE_INT32:
        case LR_TYPE_FLOAT64:   type_str = "number"; break;
        case LR_TYPE_STRING:    type_str = "string"; break;
        case LR_TYPE_OBJECT: {
            if (lr_is_function(interp->ctx, arg)) {
                type_str = "function";
            } else {
                type_str = "object";
            }
            break;
        }
        case LR_TYPE_SYMBOL:    type_str = "symbol"; break;
        default: break;
        }
        lr_free_value(interp->ctx, arg);
        return lr_new_string(interp->ctx, type_str);
    }

    if (strcmp(op, "void") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        lr_free_value(interp->ctx, arg);
        return LR_VALUE_UNDEFINED;
    }

    if (strcmp(op, "delete") == 0) {
        /* delete property */
        if (node->u.unary.arg->type == AST_MEMBER) {
            ASTNode *member = node->u.unary.arg;
            LRValue obj = interp_eval_node(interp, member->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); return LR_VALUE_UNDEFINED; }
            const char *prop = member->u.member.prop->u.ident.name;
            if (prop) {
                LRString *atom = lr_new_atom(interp->ctx, prop);
                lr_delete_property(interp->ctx, obj, atom, 0);
            }
            lr_free_value(interp->ctx, obj);
            return lr_new_bool(interp->ctx, 1);
        }
        if (node->u.unary.arg->type == AST_COMPUTED_MEMBER) {
            ASTNode *member = node->u.unary.arg;
            LRValue obj = interp_eval_node(interp, member->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); return LR_VALUE_UNDEFINED; }
            LRValue prop = interp_eval_node(interp, member->u.member.prop);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, prop); return LR_VALUE_UNDEFINED; }
            LRString *atom = lr_to_atom(interp->ctx, prop);
            lr_delete_property(interp->ctx, obj, atom, 0);
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
            return lr_new_bool(interp->ctx, 1);
        }
        /* delete of a simple identifier - in non-strict mode, returns true */
        return lr_new_bool(interp->ctx, 1);
    }

    if (strcmp(op, "!") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        int b = lr_to_bool(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        return lr_new_bool(interp->ctx, !b);
    }

    if (strcmp(op, "~") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        int32_t i = to_int32(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        return lr_new_int32(interp->ctx, ~i);
    }

    if (strcmp(op, "+") == 0 && prefix) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        double d = to_number(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        if (d == (double)(int32_t)d && !isnan(d) && !isinf(d))
            return lr_new_int32(interp->ctx, (int32_t)d);
        return lr_new_float64(interp->ctx, d);
    }

    if (strcmp(op, "-") == 0 && prefix) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        double d = to_number(interp->ctx, arg);
        lr_free_value(interp->ctx, arg);
        d = -d;
        if (d == (double)(int32_t)d && !isnan(d) && !isinf(d))
            return lr_new_int32(interp->ctx, (int32_t)d);
        return lr_new_float64(interp->ctx, d);
    }

    /* Pre/post increment/decrement */
    if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
        LRValue arg = interp_eval_node(interp, node->u.unary.arg);
        if (interp->error_flag) return arg;
        double old_val = to_number(interp->ctx, arg);
        double new_val = (op[0] == '+') ? old_val + 1.0 : old_val - 1.0;

        /* Try to assign back to the identifier */
        if (node->u.unary.arg->type == AST_IDENTIFIER) {
            const char *name = node->u.unary.arg->u.ident.name;
            LRValue nv;
            if (new_val == (double)(int32_t)new_val && !isnan(new_val) && !isinf(new_val))
                nv = lr_new_int32(interp->ctx, (int32_t)new_val);
            else
                nv = lr_new_float64(interp->ctx, new_val);
            scope_set_name(interp, name, nv);
            lr_free_value(interp->ctx, nv);
        }

        LRValue result;
        if (prefix) {
            /* Return new value */
            if (new_val == (double)(int32_t)new_val && !isnan(new_val) && !isinf(new_val))
                result = lr_new_int32(interp->ctx, (int32_t)new_val);
            else
                result = lr_new_float64(interp->ctx, new_val);
        } else {
            /* Return old value */
            result = arg; /* already have it */
            lr_free_value(interp->ctx, arg); /* no, we don't want to free it */
            /* Actually, we dup'd arg earlier, so we need to handle this differently */
            if (old_val == (double)(int32_t)old_val && !isnan(old_val) && !isinf(old_val))
                result = lr_new_int32(interp->ctx, (int32_t)old_val);
            else
                result = lr_new_float64(interp->ctx, old_val);
        }
        lr_free_value(interp->ctx, arg);
        return result;
    }

    return LR_VALUE_UNDEFINED;
}

static LRValue eval_assign(Interpreter *interp, ASTNode *node)
{
    const char *op = node->u.assign.op;
    ASTNode *target = node->u.assign.target;
    ASTNode *value_node = node->u.assign.value;

    /* Simple assignment: = */
    if (strcmp(op, "=") == 0) {
        LRValue val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return val;

        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, val)) {
                /* Not found in scope chain - declare in global scope */
                if (interp->error_flag) {
                    interp->error_flag = 0; /* Clear const error - just declare */
                    scope_declare_name(interp, name, val, 0);
                } else {
                    scope_declare_name(interp, name, val, 0);
                }
            }
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            const char *prop = target->u.member.prop->u.ident.name;
            lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, val));
            lr_free_value(interp->ctx, obj);
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, prop); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            LRString *atom = lr_to_atom(interp->ctx, prop);
            lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, val));
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        /* Destructuring assignment */
        if (target->type == AST_PATTERN || target->type == AST_ARRAY || target->type == AST_OBJECT) {
            eval_pattern(interp, target, val);
            if (interp->error_flag) { lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            LRValue result = lr_dup_value(interp->ctx, val);
            lr_free_value(interp->ctx, val);
            return result;
        }

        lr_free_value(interp->ctx, val);
        return LR_VALUE_UNDEFINED;
    }

    /* Logical assignment: &&=, ||=, ??= */
    if (strcmp(op, "&&=") == 0) {
        LRValue left_val = interp_eval_node(interp, target);
        if (interp->error_flag) return left_val;
        int truthy = lr_to_bool(interp->ctx, left_val);
        if (!truthy) {
            /* Short-circuit: x &&= y → x when x is falsy */
            return left_val;
        }
        lr_free_value(interp->ctx, left_val);
        LRValue right_val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return right_val;
        LRValue result = lr_dup_value(interp->ctx, right_val);
        /* Assign back to target */
        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, result)) {
                if (!interp->error_flag)
                    scope_declare_name(interp, name, result, 0);
                interp->error_flag = 0;
            }
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (!interp->error_flag) {
                LRString *atom = lr_to_atom(interp->ctx, prop);
                lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
        }
        lr_free_value(interp->ctx, right_val);
        LRValue ret = lr_dup_value(interp->ctx, result);
        lr_free_value(interp->ctx, result);
        return ret;
    }

    if (strcmp(op, "||=") == 0) {
        LRValue left_val = interp_eval_node(interp, target);
        if (interp->error_flag) return left_val;
        int truthy = lr_to_bool(interp->ctx, left_val);
        if (truthy) {
            /* Short-circuit: x ||= y → x when x is truthy */
            return left_val;
        }
        lr_free_value(interp->ctx, left_val);
        LRValue right_val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return right_val;
        LRValue result = lr_dup_value(interp->ctx, right_val);
        /* Assign back to target */
        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, result)) {
                if (!interp->error_flag)
                    scope_declare_name(interp, name, result, 0);
                interp->error_flag = 0;
            }
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (!interp->error_flag) {
                LRString *atom = lr_to_atom(interp->ctx, prop);
                lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
        }
        lr_free_value(interp->ctx, right_val);
        LRValue ret = lr_dup_value(interp->ctx, result);
        lr_free_value(interp->ctx, result);
        return ret;
    }

    if (strcmp(op, "?" "?=") == 0) {
        LRValue left_val = interp_eval_node(interp, target);
        if (interp->error_flag) return left_val;
        if (!lr_is_null(left_val) && !lr_is_undefined(left_val)) {
            /* Short-circuit: x ??= y → x when x is not null/undefined */
            return left_val;
        }
        lr_free_value(interp->ctx, left_val);
        LRValue right_val = interp_eval_node(interp, value_node);
        if (interp->error_flag) return right_val;
        LRValue result = lr_dup_value(interp->ctx, right_val);
        /* Assign back to target */
        if (target->type == AST_IDENTIFIER) {
            const char *name = target->u.ident.name;
            if (!scope_set_name(interp, name, result)) {
                if (!interp->error_flag)
                    scope_declare_name(interp, name, result, 0);
                interp->error_flag = 0;
            }
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            LRValue prop = interp_eval_node(interp, target->u.member.prop);
            if (!interp->error_flag) {
                LRString *atom = lr_to_atom(interp->ctx, prop);
                lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
            }
            lr_free_value(interp->ctx, obj);
            lr_free_value(interp->ctx, prop);
        }
        lr_free_value(interp->ctx, right_val);
        LRValue ret = lr_dup_value(interp->ctx, result);
        lr_free_value(interp->ctx, result);
        return ret;
    }

    /* Compound assignment: +=, -=, *=, /=, %=, **= */
    LRValue left_val = interp_eval_node(interp, target);
    if (interp->error_flag) return left_val;
    LRValue right_val = interp_eval_node(interp, value_node);
    if (interp->error_flag) { lr_free_value(interp->ctx, left_val); return right_val; }

    LRValue result = LR_VALUE_UNDEFINED;
    double da = to_number(interp->ctx, left_val);
    double db = to_number(interp->ctx, right_val);

    if (strcmp(op, "+=") == 0) {
        if (lr_is_string(left_val) || lr_is_string(right_val)) {
            const char *sl = lr_to_cstring(interp->ctx, left_val);
            const char *sr = lr_to_cstring(interp->ctx, right_val);
            size_t llen = strlen(sl);
            size_t rlen = strlen(sr);
            char *buf = (char *)malloc(llen + rlen + 1);
            if (buf) {
                memcpy(buf, sl, llen);
                memcpy(buf + llen, sr, rlen);
                buf[llen + rlen] = '\0';
                result = lr_new_string(interp->ctx, buf);
                free(buf);
            }
            lr_free_cstring(interp->ctx, sl);
            lr_free_cstring(interp->ctx, sr);
        } else {
            double v = da + db;
            if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
                result = lr_new_int32(interp->ctx, (int32_t)v);
            else
                result = lr_new_float64(interp->ctx, v);
        }
    } else if (strcmp(op, "-=") == 0) {
        double v = da - db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "*=") == 0) {
        double v = da * db;
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "/=") == 0) {
        result = lr_new_float64(interp->ctx, db == 0.0 ? da / db : da / db);
    } else if (strcmp(op, "%=") == 0) {
        double v = fmod(da, db);
        if (v == (double)(int32_t)v && !isnan(v) && !isinf(v))
            result = lr_new_int32(interp->ctx, (int32_t)v);
        else
            result = lr_new_float64(interp->ctx, v);
    } else if (strcmp(op, "**=") == 0) {
        result = lr_new_float64(interp->ctx, pow(da, db));
    } else {
        result = lr_dup_value(interp->ctx, left_val);
    }

    lr_free_value(interp->ctx, left_val);
    lr_free_value(interp->ctx, right_val);

    /* Assign back to target */
    if (target->type == AST_IDENTIFIER) {
        const char *name = target->u.ident.name;
        if (!scope_set_name(interp, name, result)) {
            if (!interp->error_flag)
                scope_declare_name(interp, name, result, 0);
            interp->error_flag = 0;
        }
    } else if (target->type == AST_MEMBER) {
        LRValue obj = interp_eval_node(interp, target->u.member.obj);
        if (!interp->error_flag) {
            const char *prop = target->u.member.prop->u.ident.name;
            lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, result));
        }
        lr_free_value(interp->ctx, obj);
    } else if (target->type == AST_COMPUTED_MEMBER) {
        LRValue obj = interp_eval_node(interp, target->u.member.obj);
        LRValue prop = interp_eval_node(interp, target->u.member.prop);
        if (!interp->error_flag) {
            LRString *atom = lr_to_atom(interp->ctx, prop);
            lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, result));
        }
        lr_free_value(interp->ctx, obj);
        lr_free_value(interp->ctx, prop);
    }

    LRValue ret = lr_dup_value(interp->ctx, result);
    lr_free_value(interp->ctx, result);
    return ret;
}

/* C callback for import.meta.resolve(specifier).
 * Resolves a relative specifier against the module's directory. */
static LRValue import_meta_resolve_cfunc(LRContext *ctx, LRValue this_val,
                                          int argc, LRValue *argv)
{
    (void)this_val;
    const char *base_dir = NULL;
    if (this_val.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)this_val.u.ptr;
        /* The captured directory lives in the function object's opaque */
        LRObject *go = NULL;
        LRValue resolve_fn = lr_get_property_str(ctx,
            lr_dup_value(ctx, this_val), "resolve");
        if (resolve_fn.tag == LR_TYPE_OBJECT)
            go = (LRObject *)resolve_fn.u.ptr;
        if (go && go->opaque)
            base_dir = (const char *)go->opaque;
        lr_free_value(ctx, resolve_fn);
    }
    /* Fallback: use the fn opaque from the call context */
    if (!base_dir) {
        if (ctx->current_func.tag == LR_TYPE_OBJECT) {
            LRObject *cf = (LRObject *)ctx->current_func.u.ptr;
            if (cf->opaque)
                base_dir = (const char *)cf->opaque;
        }
    }

    const char *spec = (argc >= 1)
        ? lr_to_cstring(ctx, argv[0]) : "";
    if (!spec) spec = "";

    /* Simple path resolution: if spec starts with ./ or ../, concat;
     * otherwise return spec as-is (absolute / protocol / bare name). */
    char resolved[4096];
    if (strncmp(spec, "./", 2) == 0 || strncmp(spec, "../", 3) == 0) {
        snprintf(resolved, sizeof(resolved), "%s/%s",
                 base_dir ? base_dir : ".", spec);
    } else {
        strncpy(resolved, spec, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    return lr_new_string(ctx, resolved);
}

/* Build (or return cached) import.meta object for the current unit.
 * Exposes: url (file:// URL), filename (absolute path), dirname, resolve. */
static LRValue interp_get_import_meta(Interpreter *interp)
{
    LRContext *ctx = interp->ctx;

    if (interp->import_meta.tag == LR_TYPE_OBJECT)
        return lr_dup_value(ctx, interp->import_meta);

    LRValue meta = lr_new_object(ctx);
    const char *fn = interp->filename ? interp->filename : "";

    /* Resolve to an absolute path when possible */
    char abs[4096];
    abs[0] = '\0';
#ifdef _WIN32
    if (fn[0] == '\0' || fn[0] == '<' || !_fullpath(abs, fn, sizeof(abs))) {
        strncpy(abs, fn, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = '\0';
    }
#else
    if (fn[0] == '\0' || fn[0] == '<' || !realpath(fn, abs)) {
        strncpy(abs, fn, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = '\0';
    }
#endif

    /* file:// URL: normalize separators to '/' */
    char url[4352];
    {
        char norm[4096];
        size_t i;
        for (i = 0; abs[i] && i < sizeof(norm) - 1; i++)
            norm[i] = (abs[i] == '\\') ? '/' : abs[i];
        norm[i] = '\0';
        if (norm[0] == '/')
            snprintf(url, sizeof(url), "file://%s", norm);
        else if (norm[0])
            snprintf(url, sizeof(url), "file:///%s", norm);
        else
            snprintf(url, sizeof(url), "file:///");
    }
    lr_set_property_str(ctx, meta, "url", lr_new_string(ctx, url));
    lr_set_property_str(ctx, meta, "filename", lr_new_string(ctx, abs));

    /* dirname: strip last path component */
    char dir[4096], *last_sep = NULL;
    strncpy(dir, abs, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    for (char *p = dir; *p; p++)
        if (*p == '/' || *p == '\\') last_sep = p;
    if (last_sep) *last_sep = '\0';
    lr_set_property_str(ctx, meta, "dirname", lr_new_string(ctx, dir));

    /* import.meta.resolve(specifier) — resolves a specifier relative to the
     * module's directory (path concatenation + basic normalization). */
    {
        char *captured_dir = strdup(dir[0] ? dir : ".");
        LRValue resolve_fn = lr_new_cfunction(ctx,
            import_meta_resolve_cfunc, "resolve", 1);
        if (resolve_fn.tag == LR_TYPE_OBJECT) {
            LRObject *fo = (LRObject *)resolve_fn.u.ptr;
            fo->opaque = captured_dir;
            fo->opaque_free = free;
        } else {
            free(captured_dir);
        }
        lr_set_property_str(ctx, meta, "resolve", resolve_fn);
    }

    interp->import_meta = lr_dup_value(ctx, meta);
    return meta;
}

static LRValue eval_member(Interpreter *interp, ASTNode *node)
{
    int is_optional = node->u.member.is_optional;

    /* import.meta: the parser encodes it as member expr `import`.`meta` */
    if (node->u.member.obj && node->u.member.obj->type == AST_IDENTIFIER &&
        node->u.member.obj->u.ident.name &&
        strcmp(node->u.member.obj->u.ident.name, "import") == 0 &&
        node->u.member.prop && node->u.member.prop->type == AST_IDENTIFIER &&
        node->u.member.prop->u.ident.name &&
        strcmp(node->u.member.prop->u.ident.name, "meta") == 0) {
        return interp_get_import_meta(interp);
    }

    LRValue obj = interp_eval_node(interp, node->u.member.obj);
    if (interp->error_flag) return obj;

    if (is_optional && (lr_is_null(obj) || lr_is_undefined(obj))) {
        lr_free_value(interp->ctx, obj);
        return LR_VALUE_UNDEFINED;
    }

    const char *prop = NULL;
    if (node->u.member.prop) {
        if (node->u.member.prop->type == AST_IDENTIFIER) {
            prop = node->u.member.prop->u.ident.name;
        }
    }

    if (prop) {
        /* Try inline cache: if the property name is the same, use cached atom */
        int cache_idx = (int)((uintptr_t)prop % LR_IC_SIZE);
        LRInlineCache *cache = &interp->member_cache[cache_idx];

        if (__builtin_expect(cache->is_active && cache->prop_name == prop, 1)) {
            /* Cache hit: fast path — if the object's shape matches the cached
             * shape and the version is unchanged, do a direct props[slot] read
             * (O(1), no shape walk, no hash lookup). */
            if (obj.tag == LR_TYPE_OBJECT) {
                LRObject *o = (LRObject *)obj.u.ptr;
                if (__builtin_expect(o->type == LR_OBJ_PLAIN &&
                                     o->shape == cache->shape &&
                                     cache->shape_version == o->shape->version &&
                                     cache->slot_index < o->prop_count, 1)) {
                    LRValue *v = &o->props[cache->slot_index];
                    if (__builtin_expect(v->tag != LR_TYPE_UNDEFINED, 1)) {
                        LRValue result = lr_dup_value(interp->ctx, *v);
                        lr_free_value(interp->ctx, obj);
                        cache->hit_count++;
                        return result;
                    }
                }
            }
            /* Fallback: use cached atom for the general lookup */
            LRValue result = lr_get_property(interp->ctx, obj, cache->prop_atom);
            lr_free_value(interp->ctx, obj);
            cache->hit_count++;
            return result;
        }

        /* Cache miss: create atom ONCE, use for both lookup and caching */
        LRString *atom = lr_new_atom(interp->ctx, prop);
        LRValue result = lr_get_property(interp->ctx, obj, atom);
        lr_free_value(interp->ctx, obj);

        /* Update cache with the atom we already created, plus the shape/slot
         * info for the fast path on subsequent hits. */
        LRInlineCache *ic = &interp->member_cache[cache_idx];
        ic->prop_name = prop;
        ic->prop_atom = atom;
        ic->is_active = 1;
        ic->hit_count = 0;
        ic->shape = NULL;
        ic->slot_index = 0;
        ic->shape_version = 0;
        if (obj.tag == LR_TYPE_OBJECT) {
            LRObject *o = (LRObject *)obj.u.ptr;
            if (o->type == LR_OBJ_PLAIN && o->shape) {
                /* Find the slot index for this atom in the shape's flat hash */
                LRShape *s = o->shape;
                if (s->flat_count != 0) {
                    unsigned h = SHAPE_ATOM_HASH(atom);
                    for (unsigned i = 0; i < SHAPE_FLAT_SIZE; i++) {
                        unsigned idx = (h + i) & SHAPE_FLAT_MASK;
                        LRString *k = s->flat_keys[idx];
                        if (k == atom) {
                            ic->shape = s;
                            ic->slot_index = (uint32_t)s->flat_slots[idx];
                            ic->shape_version = s->version;
                            break;
                        }
                        if (!k) break;
                    }
                } else {
                    while (s) {
                        if (s->prop_name == atom) {
                            ic->shape = o->shape;
                            ic->slot_index = s->slot_index;
                            ic->shape_version = o->shape->version;
                            break;
                        }
                        s = s->prev;
                    }
                }
            }
        }

        return result;
    }

    lr_free_value(interp->ctx, obj);
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_computed_member(Interpreter *interp, ASTNode *node)
{
    int is_optional = node->u.member.is_optional;
    LRValue obj = interp_eval_node(interp, node->u.member.obj);
    if (interp->error_flag) return obj;

    if (is_optional && (lr_is_null(obj) || lr_is_undefined(obj))) {
        lr_free_value(interp->ctx, obj);
        return LR_VALUE_UNDEFINED;
    }

    LRValue prop = interp_eval_node(interp, node->u.member.prop);
    if (interp->error_flag) {
        lr_free_value(interp->ctx, obj);
        return prop;
    }

    LRString *atom = lr_to_atom(interp->ctx, prop);
    LRValue result = lr_get_property(interp->ctx, obj, atom);
    lr_free_value(interp->ctx, obj);
    lr_free_value(interp->ctx, prop);
    return result;
}

/* Stack buffer for small argument lists (avoid calloc/free overhead) */
#define EVAL_CALL_STACK_ARGS 16
#define EVAL_CALL_FREE_ARGV() do { if (argv_on_heap) free(argv); } while(0)

/* ES2015+: a function is strict if strict mode is forced on the context
 * (config --strict), or if its body starts with a "use strict" directive
 * prologue.  Arrows are excluded: they always inherit `this` lexically, so
 * their `this` must never be rewritten at the call site even when strict. */
static int ast_func_is_strict(Interpreter *interp, ASTNode *fn)
{
    if (interp && (interp->is_module || (interp->ctx && interp->ctx->strict_mode)))
        return 1; /* ES modules and --strict force strict mode everywhere */
    if (!fn) return 0;
    ASTNode *body = NULL;
    if (fn->type == AST_FUNC_EXPR || fn->type == AST_FUNC_DECL)
        body = fn->u.func.body;
    else
        return 0; /* arrows: lexical this */
    if (!body || body->type != AST_BLOCK || body->u.list.count < 1)
        return 0;
    ASTNode *first = body->u.list.items[0];
    if (first->type == AST_EXPR_STMT && first->u.expr_stmt.expr &&
        first->u.expr_stmt.expr->type == AST_LITERAL &&
        first->u.expr_stmt.expr->token.type == TOK_STRING) {
        const char *s = first->u.expr_stmt.expr->u.string.str;
        if (s && strcmp(s, "use strict") == 0) return 1;
    }
    return 0;
}

static LRValue eval_call(Interpreter *interp, ASTNode *node)
{
    ASTNode *callee_node = node->u.call.callee;
    int argc = node->u.call.argc;
    ASTNode **args = node->u.call.args;

    /* Evaluate arguments with spread support */
    LRValue stack_argv[EVAL_CALL_STACK_ARGS];
    LRValue *argv = NULL;
    int total_argc = 0;
    int argv_cap = argc > 0 ? argc : 0;
    int argv_on_heap = 0; /* track whether argv needs free() */
    if (argc > 0) {
        /* Use stack buffer for small arg lists without spread elements */
        if (argc <= EVAL_CALL_STACK_ARGS) {
            argv = stack_argv;
            memset(argv, 0, argc * sizeof(LRValue));
            argv_on_heap = 0;
        } else {
            argv = (LRValue *)calloc(argv_cap, sizeof(LRValue));
            argv_on_heap = 1;
        }
        for (int i = 0; i < argc; i++) {
            if (args[i]->type == AST_SPREAD_ELEMENT) {
                LRValue spread_val = interp_eval_node(interp, args[i]->u.spread.arg);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    if (argv_on_heap) free(argv);
                    return LR_VALUE_UNDEFINED;
                }
                if (lr_is_array(interp->ctx, spread_val)) {
                    int32_t len = 0;
                    LRValue len_val = lr_get_property_str(interp->ctx, spread_val, "length");
                    lr_to_int32(interp->ctx, &len, len_val);
                    lr_free_value(interp->ctx, len_val);
                    /* Reallocate argv if needed */
                    if (total_argc + len > argv_cap) {
                        argv_cap = total_argc + len;
                        if (!argv_on_heap) {
                            argv = (LRValue *)malloc(argv_cap * sizeof(LRValue));
                            if (argv) memcpy(argv, stack_argv, total_argc * sizeof(LRValue));
                            argv_on_heap = 1;
                        } else {
                            argv = (LRValue *)realloc(argv, argv_cap * sizeof(LRValue));
                        }
                    }
                    for (int32_t j = 0; j < len; j++) {
                        argv[total_argc] = lr_get_property_uint32(interp->ctx, spread_val, j);
                        total_argc++;
                    }
                }
                lr_free_value(interp->ctx, spread_val);
            } else {
                argv[total_argc] = interp_eval_node(interp, args[i]);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    EVAL_CALL_FREE_ARGV();
                    return LR_VALUE_UNDEFINED;
                }
                total_argc++;
            }
        }
    }
    argc = total_argc; /* Update argc to reflect expanded arguments */

    /* Handle dynamic import() */
    if (callee_node->type == AST_IDENTIFIER &&
        strcmp(callee_node->u.ident.name, "import") == 0) {
        /* Dynamic import */
        if (argc < 1) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            LRValue err = JS_ThrowTypeError(interp->ctx, "import() requires at least 1 argument");
            interp->error_flag = 1;
            interp->exception_pending = 1;
            interp->exception_value = lr_dup_value(interp->ctx, err);
            lr_free_value(interp->ctx, err);
            return LR_VALUE_UNDEFINED;
        }
        const char *spec = JS_ToCString(interp->ctx, argv[0]);
        if (!spec) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            interp->error_flag = 1;
            return LR_VALUE_UNDEFINED;
        }
        /* Normalize the module specifier */
        char *normalized = NULL;
        if (interp->ctx->rt->module_normalize_func) {
            normalized = interp->ctx->rt->module_normalize_func(interp->ctx, NULL, spec, NULL);
        }
        const char *load_name = normalized ? normalized : spec;
        /* Load the module via the runtime's module loader */
        JSModuleDef *mod = NULL;
        if (interp->ctx->rt->module_loader_func) {
            mod = interp->ctx->rt->module_loader_func(interp->ctx, load_name, NULL);
        }
        JS_FreeCString(interp->ctx, spec);
        if (normalized) free(normalized);
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
        if (!mod) {
            interp->error_flag = 1;
            interp->exception_pending = 1;
            return LR_VALUE_UNDEFINED;
        }
        /* Return the module namespace wrapped in a resolved promise */
        LRValue ns_val;
        ns_val.tag = LR_TYPE_OBJECT;
        ns_val.u.ptr = mod->obj;
        if (mod->obj) mod->obj->ref_count++;
        LRValue promise = lr_new_promise(interp->ctx);
        lr_promise_resolve_internal(interp->ctx, promise, ns_val);
        lr_free_value(interp->ctx, ns_val);
        return promise;
    }

    /* Determine this binding and callee value */
    LRValue this_val = LR_VALUE_UNDEFINED;
    LRValue callee;

    /* super(...) call inside a derived class constructor */
    if (callee_node->type == AST_SUPER) {
        LRValue sup = LR_VALUE_UNDEFINED;
        if (!scope_lookup_internal(interp, interp->current_scope, "%superctor%", &sup)) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "'super' keyword unexpected here");
            interp->error_flag = 1;
            return LR_VALUE_UNDEFINED;
        }
        if (!scope_lookup_internal(interp, interp->current_scope, "this", &this_val)) {
            this_val = LR_VALUE_UNDEFINED;
        }
        LRValue result_sup = LR_VALUE_UNDEFINED;
        if (sup.tag == LR_TYPE_OBJECT) {
            LRObject *so = (LRObject *)sup.u.ptr;
            if (so->type == LR_OBJ_FUNCTION && so->extra) {
                interp->pending_closure = so->def_scope;
                result_sup = interp_invoke_function_ast(interp, (ASTNode *)so->extra,
                                                        this_val, argc, argv);
            } else {
                /* C-function base class (e.g. Error): call on this */
                lr_push_call_frame(interp->ctx, "super", NULL, 0);
                result_sup = lr_call(interp->ctx, sup, this_val, argc, argv);
                lr_pop_call_frame(interp->ctx);
            }
        }
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
        lr_free_value(interp->ctx, this_val);
        lr_free_value(interp->ctx, sup);
        return result_sup;
    }

    /* super.method(...) call: look up on parent prototype, this = current this */
    if ((callee_node->type == AST_MEMBER || callee_node->type == AST_COMPUTED_MEMBER) &&
        callee_node->u.member.obj && callee_node->u.member.obj->type == AST_SUPER) {
        LRValue sproto = LR_VALUE_UNDEFINED;
        scope_lookup_internal(interp, interp->current_scope, "%superproto%", &sproto);
        if (callee_node->type == AST_MEMBER &&
            callee_node->u.member.prop &&
            callee_node->u.member.prop->type == AST_IDENTIFIER) {
            callee = lr_get_property_str(interp->ctx, sproto,
                                         callee_node->u.member.prop->u.ident.name);
        } else {
            LRValue pk = interp_eval_node(interp, callee_node->u.member.prop);
            LRString *atom = lr_to_atom(interp->ctx, pk);
            callee = lr_get_property(interp->ctx, sproto, atom);
            lr_free_value(interp->ctx, pk);
        }
        lr_free_value(interp->ctx, sproto);
        if (!scope_lookup_internal(interp, interp->current_scope, "this", &this_val)) {
            this_val = LR_VALUE_UNDEFINED;
        }
    }
    /* Check if this is a method call (member expression as callee) */
    else if (callee_node->type == AST_MEMBER) {
        this_val = interp_eval_node(interp, callee_node->u.member.obj);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            lr_free_value(interp->ctx, this_val);
            return LR_VALUE_UNDEFINED;
        }
        const char *prop = callee_node->u.member.prop->u.ident.name;
        callee = lr_get_property_str(interp->ctx, this_val, prop);
    } else if (callee_node->type == AST_COMPUTED_MEMBER) {
        this_val = interp_eval_node(interp, callee_node->u.member.obj);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            lr_free_value(interp->ctx, this_val);
            return LR_VALUE_UNDEFINED;
        }
        LRValue prop = interp_eval_node(interp, callee_node->u.member.prop);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            lr_free_value(interp->ctx, this_val);
            lr_free_value(interp->ctx, prop);
            return LR_VALUE_UNDEFINED;
        }
        LRString *atom = lr_to_atom(interp->ctx, prop);
        callee = lr_get_property(interp->ctx, this_val, atom);
        lr_free_value(interp->ctx, prop);
    } else {
        /* Normal (non-method) function call.  Per ES OrdinaryCallBindThis an
         * undefined `this` becomes the global object for non-strict functions
         * but stays undefined for strict ones. */
        callee = interp_eval_node(interp, callee_node);
        if (interp->error_flag) {
            if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
            return LR_VALUE_UNDEFINED;
        }
        if (ast_func_is_strict(interp, callee_node))
            this_val = LR_VALUE_UNDEFINED;                 /* strict: this === undefined */
        else
            this_val = lr_get_global_object(interp->ctx);  /* sloppy: this === global */
    }

    if (interp->error_flag) {
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
        lr_free_value(interp->ctx, this_val);
        lr_free_value(interp->ctx, callee);
        return LR_VALUE_UNDEFINED;
    }

    /* Optional chaining short-circuit: if callee is null/undefined, return undefined */
    if (node->type == AST_OPTIONAL_CALL && (lr_is_null(callee) || lr_is_undefined(callee))) {
        if (argv) {
            for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]);
            EVAL_CALL_FREE_ARGV();
        }
        lr_free_value(interp->ctx, this_val);
        lr_free_value(interp->ctx, callee);
        return LR_VALUE_UNDEFINED;
    }

    LRValue result;

    /* Check if callee is a interpreted function (AST_FUNC_EXPR or AST_ARROW).
     * This check is fast (just AST type comparison) and handles the common
     * case of direct function calls.  Do it BEFORE the generic lr_is_function
     * check to avoid the overhead of lr_is_function for the common path. */
    if (callee_node->type == AST_FUNC_EXPR || callee_node->type == AST_ARROW ||
        callee_node->type == AST_FUNC_DECL) {
        /* Direct function expression call */
        result = interp_call_function(interp, callee_node, this_val, argc, argv);
        goto call_done;
    }

    /* Check if callee is a native C function */
    if (lr_is_function(interp->ctx, callee)) {
        if (callee.tag == LR_TYPE_OBJECT) {
            LRObject *obj = (LRObject *)callee.u.ptr;
            if (obj->type == LR_OBJ_CFUNCTION) {
                /* Push a call frame for C function */
                LRCFunction *cf = (LRCFunction *)obj->extra;
                const char *cname = cf->name ? cf->name : "";
                lr_push_call_frame(interp->ctx, cname, NULL, 0);

                result = lr_call(interp->ctx, callee, this_val, argc, argv);

                lr_pop_call_frame(interp->ctx);

                if (lr_is_exception(result)) {
                    /* Capture exception */
                    interp->exception_pending = 1;
                    interp->exception_value = lr_dup_value(interp->ctx, result);
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "%s", lr_get_exception_str(interp->ctx));
                    interp->error_flag = 1;
                }
                goto call_done;
            }
        }
    }

    /* Also handle the case where callee is a function expression that was stored */
    if (callee.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_BYTECODE_FUNC) {
            /* Bytecode function - for now, try calling via lr_call */
            lr_push_call_frame(interp->ctx, "", NULL, 0);
            result = lr_call(interp->ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(interp->ctx);
            goto call_done;
        }
        if (obj->type == LR_OBJ_CFUNCTION) {
            const char *cname = "";
            LRCFunction *cf = (LRCFunction *)obj->extra;
            if (cf) cname = cf->name ? cf->name : "";
            lr_push_call_frame(interp->ctx, cname, NULL, 0);
            result = lr_call(interp->ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(interp->ctx);
            goto call_done;
        }
        if (obj->type == LR_OBJ_FUNCTION) {
            /* Interpreted function object - try to get the AST from extra */
            if (obj->extra) {
                ASTNode *func_ast = (ASTNode *)obj->extra;
                interp->pending_closure = obj->def_scope;
                result = interp_invoke_function_ast(interp, func_ast, this_val, argc, argv);
                goto call_done;
            }
        }
    }

    /* Fallback: try lr_call */
    lr_push_call_frame(interp->ctx, "", NULL, 0);
    result = lr_call(interp->ctx, callee, this_val, argc, argv);
    lr_pop_call_frame(interp->ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(interp->ctx));
        interp->error_flag = 1;
    }

call_done:
    if (lr_env_flag(&g_lr_env_debug_call2, "LR_DEBUG_CALL2")) {
        fprintf(stderr, "[CALL2] this=%p(rc=%d) callee=%p argc=%d",
                (this_val.tag == LR_TYPE_OBJECT) ? this_val.u.ptr : NULL,
                (this_val.tag == LR_TYPE_OBJECT && this_val.u.ptr) ? ((LRObject *)this_val.u.ptr)->ref_count : -1,
                (callee.tag == LR_TYPE_OBJECT) ? callee.u.ptr : NULL, argc);
        for (int i = 0; i < argc; i++) {
            if (argv[i].tag == LR_TYPE_OBJECT)
                fprintf(stderr, " argv[%d]=%p(rc=%d)", i, argv[i].u.ptr,
                        argv[i].u.ptr ? ((LRObject *)argv[i].u.ptr)->ref_count : -1);
            else
                fprintf(stderr, " argv[%d]=tag%d", i, (int)argv[i].tag);
        }
        fprintf(stderr, "\n");
    }
    if (argv) {
        for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]);
        EVAL_CALL_FREE_ARGV();
    }
    lr_free_value(interp->ctx, this_val);
    lr_free_value(interp->ctx, callee);
    return result;
}

static LRValue eval_new(Interpreter *interp, ASTNode *node)
{
    int argc = node->u.new_expr.argc;
    ASTNode **args = node->u.new_expr.args;
    ASTNode *callee_node = node->u.new_expr.callee;

    /* Evaluate arguments with spread support */
    LRValue *argv = NULL;
    int total_argc = 0;
    int argv_cap = argc > 0 ? argc : 0;
    int argv_on_heap = 1;
    if (argc > 0) {
        argv = (LRValue *)calloc(argv_cap, sizeof(LRValue));
        for (int i = 0; i < argc; i++) {
            if (args[i]->type == AST_SPREAD_ELEMENT) {
                LRValue spread_val = interp_eval_node(interp, args[i]->u.spread.arg);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    if (argv_on_heap) free(argv);
                    return LR_VALUE_UNDEFINED;
                }
                if (lr_is_array(interp->ctx, spread_val)) {
                    int32_t len = 0;
                    LRValue len_val = lr_get_property_str(interp->ctx, spread_val, "length");
                    lr_to_int32(interp->ctx, &len, len_val);
                    lr_free_value(interp->ctx, len_val);
                    if (total_argc + len > argv_cap) {
                        argv_cap = total_argc + len;
                        argv = (LRValue *)realloc(argv, argv_cap * sizeof(LRValue));
                    }
                    for (int32_t j = 0; j < len; j++) {
                        argv[total_argc] = lr_get_property_uint32(interp->ctx, spread_val, j);
                        total_argc++;
                    }
                }
                lr_free_value(interp->ctx, spread_val);
            } else {
                argv[total_argc] = interp_eval_node(interp, args[i]);
                if (interp->error_flag) {
                    for (int j = 0; j < total_argc; j++) lr_free_value(interp->ctx, argv[j]);
                    EVAL_CALL_FREE_ARGV();
                    return LR_VALUE_UNDEFINED;
                }
                total_argc++;
            }
        }
    }
    argc = total_argc; /* Update argc to reflect expanded arguments */

    LRValue callee = interp_eval_node(interp, callee_node);
    if (interp->error_flag) {
        if (argv) { for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]); EVAL_CALL_FREE_ARGV(); }
        return LR_VALUE_UNDEFINED;
    }

    /* Push call frame for constructor */
    const char *ctor_name = "";
    if (callee.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_CFUNCTION) {
            LRCFunction *cf = (LRCFunction *)obj->extra;
            if (cf && cf->name) ctor_name = cf->name;
        }
    }
    lr_push_call_frame(interp->ctx, ctor_name, NULL, 0);
    LRValue result = lr_call_constructor(interp->ctx, callee, argc, argv);
    lr_pop_call_frame(interp->ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(interp->ctx));
        interp->error_flag = 1;
    }

    if (argv) {
        for (int i = 0; i < argc; i++) lr_free_value(interp->ctx, argv[i]);
        EVAL_CALL_FREE_ARGV();
    }
    lr_free_value(interp->ctx, callee);
    return result;
}

static LRValue eval_conditional(Interpreter *interp, ASTNode *node)
{
    LRValue cond = interp_eval_node(interp, node->u.conditional.cond);
    if (interp->error_flag) return cond;

    int truthy = lr_to_bool(interp->ctx, cond);
    lr_free_value(interp->ctx, cond);

    if (truthy) {
        return interp_eval_node(interp, node->u.conditional.consequent);
    } else {
        return interp_eval_node(interp, node->u.conditional.alternate);
    }
}

static LRValue call_value_with_args(Interpreter *interp, ASTNode *callee_node,
                                    LRValue callee, LRValue this_val,
                                    int argc, LRValue *argv);

/* Spread `src` into array `arr` starting at *out_idx. Supports arrays,
 * strings, and iterables (Symbol.iterator, e.g. generator objects).
 * Returns 0 on success, -1 on error (error_flag set). */
static int spread_into_array(Interpreter *interp, LRValue arr, LRValue src, uint32_t *out_idx)
{
    LRContext *ctx = interp->ctx;
    if (lr_is_array(ctx, src)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(ctx, src, "length");
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        for (int32_t j = 0; j < len; j++) {
            LRValue item = lr_get_property_uint32(ctx, src, (uint32_t)j);
            lr_set_property_uint32(ctx, arr, (*out_idx)++, item);
        }
        return 0;
    }
    if (lr_is_string(src)) {
        const char *s = lr_to_cstring(ctx, src);
        size_t slen = s ? strlen(s) : 0;
        char buf[2];
        for (size_t j = 0; j < slen; j++) {
            buf[0] = s[j]; buf[1] = '\0';
            lr_set_property_uint32(ctx, arr, (*out_idx)++, lr_new_string(ctx, buf));
        }
        lr_free_cstring(ctx, s);
        return 0;
    }
    if (lr_is_object(src)) {
        LRValue iter_fn = lr_get_property_str(ctx, src, "Symbol.iterator");
        if (lr_is_function(ctx, iter_fn)) {
            LRValue iter = call_value_with_args(interp, NULL, iter_fn, src, 0, NULL);
            lr_free_value(ctx, iter_fn);
            if (interp->error_flag) { lr_free_value(ctx, iter); return -1; }
            if (lr_is_object(iter)) {
                LRValue next_fn = lr_get_property_str(ctx, iter, "next");
                while (!interp->error_flag) {
                    LRValue nr = call_value_with_args(interp, NULL, next_fn, iter, 0, NULL);
                    if (interp->error_flag) { lr_free_value(ctx, nr); break; }
                    LRValue done = lr_get_property_str(ctx, nr, "done");
                    int is_done = lr_to_bool(ctx, done);
                    lr_free_value(ctx, done);
                    if (is_done) { lr_free_value(ctx, nr); break; }
                    LRValue value = lr_get_property_str(ctx, nr, "value");
                    lr_set_property_uint32(ctx, arr, (*out_idx)++, value);
                    lr_free_value(ctx, nr);
                }
                lr_free_value(ctx, next_fn);
            }
            lr_free_value(ctx, iter);
            return interp->error_flag ? -1 : 0;
        }
        lr_free_value(ctx, iter_fn);
    }
    snprintf(interp->error_message, sizeof(interp->error_message),
             "value is not iterable (cannot spread)");
    interp->exception_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 1;
    return -1;
}

static LRValue eval_array(Interpreter *interp, ASTNode *node)
{
    LRValue arr = lr_new_array(interp->ctx);
    int nelem = node->u.array.nelem;
    ASTNode **elements = node->u.array.elements;
    uint32_t out = 0;

    for (int i = 0; i < nelem; i++) {
        ASTNode *elem = elements[i];
        if (elem == NULL) {
            /* Array hole - skip (remains undefined) */
            out++;
            continue;
        }
        if (elem->type == AST_SPREAD_ELEMENT) {
            /* Spread element */
            LRValue spread_val = interp_eval_node(interp, elem->u.spread.arg);
            if (interp->error_flag) { lr_free_value(interp->ctx, arr); lr_free_value(interp->ctx, spread_val); return LR_VALUE_UNDEFINED; }
            if (spread_into_array(interp, arr, spread_val, &out) != 0) {
                lr_free_value(interp->ctx, spread_val);
                lr_free_value(interp->ctx, arr);
                return LR_VALUE_UNDEFINED;
            }
            lr_free_value(interp->ctx, spread_val);
        } else {
            LRValue val = interp_eval_node(interp, elem);
            if (interp->error_flag) { lr_free_value(interp->ctx, arr); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
            /* lr_set_property_uint32 takes ownership of val, do NOT free it */
            lr_set_property_uint32(interp->ctx, arr, out++, val);
        }
    }

    /* Set length */
    lr_set_property_str(interp->ctx, arr, "length", lr_new_int32(interp->ctx, (int32_t)out));
    return arr;
}

static LRValue eval_object(Interpreter *interp, ASTNode *node)
{
    LRValue obj = lr_new_object(interp->ctx);
    int nprops = node->u.object.nprops;
    ASTNode **props = node->u.object.props;

    for (int i = 0; i < nprops; i++) {
        ASTNode *prop = props[i];
        if (prop->type == AST_SPREAD) {
            /* Spread property */
            LRValue spread_val = interp_eval_node(interp, prop->u.spread.arg);
            if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, spread_val); return LR_VALUE_UNDEFINED; }
            if (lr_is_object(spread_val)) {
                /* Copy own properties */
                LRPropertyEnum *pe = NULL;
                uint32_t npe = 0;
                lr_get_own_property_names(interp->ctx, &pe, &npe, spread_val, 0);
                for (uint32_t j = 0; j < npe; j++) {
                    LRValue val = lr_get_property(interp->ctx, spread_val, pe[j].atom);
                    /* lr_set_property takes ownership of val; do NOT free it
                     * (freeing here would double-free the stored property). */
                    lr_set_property(interp->ctx, obj, pe[j].atom, val);
                }
                lr_free_property_enum(interp->ctx, pe, npe);
            }
            lr_free_value(interp->ctx, spread_val);
        } else {
            ASTNode *key_node = prop->u.property.key;
            ASTNode *val_node = prop->u.property.val;
            int shorthand = prop->u.property.shorthand;

            /* Accessor property: { get x() {...} } / { set x(v) {...} } */
            int is_accessor = (val_node && val_node->type == AST_FUNC_EXPR &&
                               (val_node->u.func.is_getter || val_node->u.func.is_setter));

            LRValue key_val;
            if (key_node->type == AST_IDENTIFIER) {
                const char *kname = key_node->u.ident.name;
                if (is_accessor) {
                    LRValue fn = eval_func_expr(interp, val_node);
                    if (val_node->u.func.is_getter) {
                        lr_set_accessor_property_str(interp->ctx, obj, kname,
                                                     fn, LR_VALUE_UNDEFINED);
                    } else {
                        lr_set_accessor_property_str(interp->ctx, obj, kname,
                                                     LR_VALUE_UNDEFINED, fn);
                    }
                } else {
                    /* Regular or shorthand property: { x: v } / { x } */
                    (void)shorthand;
                    LRValue val = interp_eval_node(interp, val_node);
                    if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                    /* lr_set_property_str takes ownership of val */
                    lr_set_property_str(interp->ctx, obj, kname, val);
                }
            } else {
                /* Computed or literal key */
                key_val = interp_eval_node(interp, key_node);
                if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, key_val); return LR_VALUE_UNDEFINED; }
                LRString *atom = lr_to_atom(interp->ctx, key_val);
                if (is_accessor) {
                    LRValue fn = eval_func_expr(interp, val_node);
                    if (val_node->u.func.is_getter) {
                        lr_set_accessor_property(interp->ctx, obj, atom,
                                                 fn, LR_VALUE_UNDEFINED);
                    } else {
                        lr_set_accessor_property(interp->ctx, obj, atom,
                                                 LR_VALUE_UNDEFINED, fn);
                    }
                } else {
                    LRValue val = interp_eval_node(interp, val_node);
                    if (interp->error_flag) { lr_free_value(interp->ctx, obj); lr_free_value(interp->ctx, key_val); lr_free_value(interp->ctx, val); return LR_VALUE_UNDEFINED; }
                    /* lr_set_property takes ownership of val */
                    lr_set_property(interp->ctx, obj, atom, val);
                }
                lr_free_value(interp->ctx, key_val);
            }
        }
    }

    return obj;
}

static LRValue eval_func_expr(Interpreter *interp, ASTNode *node)
{
    /* Create a function object with Function.prototype as its prototype */
    LRValue obj = lr_new_object_proto(interp->ctx, interp->ctx->function_proto);
    if (obj.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)obj.u.ptr;
        o->type = LR_OBJ_FUNCTION;
        /* Store the AST node as extra data so we can find it later */
        o->extra = (void *)node;
        /* Capture the defining scope for lexical closures */
        interp_capture_closure(interp, obj);

        /* Per ECMAScript, every function (except arrow) has a .prototype
         * property that is an object with a .constructor back-link. */
        LRValue proto = lr_new_object(interp->ctx);
        lr_set_property_str(interp->ctx, proto, "constructor",
                            lr_dup_value(interp->ctx, obj));
        lr_set_property_str(interp->ctx, obj, "prototype",
                            lr_dup_value(interp->ctx, proto));
        lr_free_value(interp->ctx, proto);

        /* ECMA-262: named function expressions / declarations expose their
         * name via the `name` own property (inferred names for assignments
         * are out of scope here — only the explicit AST name is set). */
        if (node && node->u.func.name) {
            lr_set_property_str(interp->ctx, obj, "name",
                                lr_new_string(interp->ctx, node->u.func.name));
        }
    }
    return obj;
}

static LRValue eval_arrow(Interpreter *interp, ASTNode *node)
{
    /* Arrow functions are similar to function expressions */
    LRValue obj = lr_new_object_proto(interp->ctx, interp->ctx->function_proto);
    if (obj.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)obj.u.ptr;
        o->type = LR_OBJ_FUNCTION;
        o->extra = (void *)node;
        interp_capture_closure(interp, obj);
    }
    return obj;
}

/* Unescape a template literal *cooked* text fragment into a freshly malloc'd
 * string (caller must free). Handles the common escape sequences. */
static char *template_unescape(const char *s)
{
    if (!s) { char *e = (char *)malloc(1); if (e) e[0] = '\0'; return e; }
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char n = s[i + 1];
            switch (n) {
            case 'n': out[j++] = '\n'; i++; break;
            case 't': out[j++] = '\t'; i++; break;
            case 'r': out[j++] = '\r'; i++; break;
            case 'b': out[j++] = '\b'; i++; break;
            case 'f': out[j++] = '\f'; i++; break;
            case 'v': out[j++] = '\v'; i++; break;
            case '0':
                if (i + 2 >= len || !isdigit((unsigned char)s[i + 2])) { out[j++] = '\0'; i++; }
                else out[j++] = c;
                break;
            case 'x':
                if (i + 3 < len) {
                    unsigned v = 0; int ok = 1;
                    for (int k = 0; k < 2; k++) {
                        char h = s[i + 2 + k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) { ok = 0; break; }
                        v = (v << 4) | d;
                    }
                    if (ok) { out[j++] = (char)v; i += 3; break; }
                }
                out[j++] = c; break;
            case 'u':
                if (i + 2 < len && s[i + 2] == '{') {
                    unsigned v = 0; int ok = 1; size_t k = i + 3;
                    while (k < len && s[k] != '}') {
                        char h = s[k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) { ok = 0; break; }
                        v = (v << 4) | d; k++;
                    }
                    if (ok && k < len) { out[j++] = (char)v; i = k; break; }
                    out[j++] = c; break;
                }
                if (i + 5 < len) {
                    unsigned v = 0; int ok = 1;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i + 2 + k];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) { ok = 0; break; }
                        v = (v << 4) | d;
                    }
                    if (ok) { out[j++] = (char)v; i += 5; break; }
                }
                out[j++] = c; break;
            case '\n': i++; break;                       /* line continuation */
            case '\r': i++; if (i + 1 < len && s[i + 1] == '\n') i++; break;
            case '\\': case '\'': case '"': case '`':
                out[j++] = n; i++; break;
            default: out[j++] = n; i++; break;          /* unknown escape: keep char */
            }
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

/* Invoke a callable value (function object or AST function) with already
 * evaluated arguments. Mirrors the tail of eval_call(). */
static LRValue call_value_with_args(Interpreter *interp, ASTNode *callee_node,
                                    LRValue callee, LRValue this_val,
                                    int argc, LRValue *argv)
{
    LRContext *ctx = interp->ctx;
    LRValue result;

    if (lr_is_function(ctx, callee) && callee.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_CFUNCTION) {
            LRCFunction *cf = (LRCFunction *)obj->extra;
            const char *cname = cf && cf->name ? cf->name : "";
            lr_push_call_frame(ctx, cname, NULL, 0);
            result = lr_call(ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(ctx);
            if (lr_is_exception(result)) {
                interp->exception_pending = 1;
                interp->exception_value = lr_dup_value(ctx, result);
                snprintf(interp->error_message, sizeof(interp->error_message),
                         "%s", lr_get_exception_str(ctx));
                interp->error_flag = 1;
            }
            return result;
        }
        if (obj->type == LR_OBJ_FUNCTION && obj->extra) {
            interp->pending_closure = obj->def_scope;
            return interp_invoke_function_ast(interp, (ASTNode *)obj->extra,
                                              this_val, argc, argv);
        }
        if (obj->type == LR_OBJ_BYTECODE_FUNC) {
            lr_push_call_frame(ctx, "", NULL, 0);
            result = lr_call(ctx, callee, this_val, argc, argv);
            lr_pop_call_frame(ctx);
            return result;
        }
    }

    if (callee_node && (callee_node->type == AST_FUNC_EXPR ||
                        callee_node->type == AST_ARROW ||
                        callee_node->type == AST_FUNC_DECL)) {
        return interp_call_function(interp, callee_node, this_val, argc, argv);
    }

    lr_push_call_frame(ctx, "", NULL, 0);
    result = lr_call(ctx, callee, this_val, argc, argv);
    lr_pop_call_frame(ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(ctx));
        interp->error_flag = 1;
    }
    return result;
}

static LRValue eval_template(Interpreter *interp, ASTNode *node)
{
    /* Template literal: `text ${expr} text` */
    int nparts = node->u.template_lit.nparts;
    int nexp   = node->u.template_lit.nexp;
    ASTNode **exprs = node->u.template_lit.exprs;

    /* Estimate total length using cooked (unescaped) fragments. */
    size_t total_len = 0;
    for (int i = 0; i < nparts; i++) {
        char *cooked = template_unescape(node->u.template_lit.parts[i]);
        total_len += strlen(cooked ? cooked : "");
        if (cooked) free(cooked);
    }

    LRValue *expr_vals = NULL;
    if (nexp > 0) {
        expr_vals = (LRValue *)calloc(nexp, sizeof(LRValue));
        for (int i = 0; i < nexp; i++) {
            expr_vals[i] = interp_eval_node(interp, exprs[i]);
            if (interp->error_flag) {
                for (int j = 0; j < i; j++) lr_free_value(interp->ctx, expr_vals[j]);
                free(expr_vals);
                return LR_VALUE_UNDEFINED;
            }
            const char *s = lr_to_cstring(interp->ctx, expr_vals[i]);
            total_len += strlen(s);
            lr_free_cstring(interp->ctx, s);
        }
    }

    char *buf = (char *)malloc(total_len + 1);
    if (!buf) {
        if (expr_vals) {
            for (int i = 0; i < nexp; i++) lr_free_value(interp->ctx, expr_vals[i]);
            free(expr_vals);
        }
        return LR_VALUE_UNDEFINED;
    }

    size_t pos = 0;
    for (int i = 0; i < nparts; i++) {
        char *cooked = template_unescape(node->u.template_lit.parts[i]);
        const char *p = cooked ? cooked : "";
        size_t plen = strlen(p);
        if (plen) { memcpy(buf + pos, p, plen); pos += plen; }
        if (cooked) free(cooked);
        if (i < nexp) {
            const char *s = lr_to_cstring(interp->ctx, expr_vals[i]);
            size_t slen = strlen(s);
            if (slen) { memcpy(buf + pos, s, slen); pos += slen; }
            lr_free_cstring(interp->ctx, s);
        }
    }
    buf[pos] = '\0';

    LRValue result = lr_new_string(interp->ctx, buf);
    free(buf);

    if (expr_vals) {
        for (int i = 0; i < nexp; i++) lr_free_value(interp->ctx, expr_vals[i]);
        free(expr_vals);
    }
    return result;
}

static LRValue eval_sequence(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;
    int count = node->u.sequence.count;
    ASTNode **exprs = node->u.sequence.exprs;

    for (int i = 0; i < count; i++) {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, exprs[i]);
        if (interp->error_flag) return result;
    }

    return result;
}

static LRValue eval_spread(Interpreter *interp, ASTNode *node)
{
    /* Spread element in expression context - evaluate the argument */
    return interp_eval_node(interp, node->u.spread.arg);
}

static LRValue eval_await(Interpreter *interp, ASTNode *node)
{
    /* Evaluate the argument */
    LRValue arg = interp_eval_node(interp, node->u.await_expr.arg);
    if (interp->error_flag) return arg;

    /* If the result is a Promise, settle it (draining microtasks if
     * needed), then unwrap: fulfilled -> value, rejected -> throw */
    if (lr_is_promise(interp->ctx, arg)) {
        LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(arg);
        if (pd && pd->state == LR_PROMISE_PENDING) {
            LRContext *jctx = NULL;
            int guard = 100000;
            while (pd->state == LR_PROMISE_PENDING && guard-- > 0) {
                if (!lr_is_job_pending(interp->ctx->rt)) break;
                lr_execute_pending_job(interp->ctx->rt, &jctx);
            }
        }
        if (pd && pd->state == LR_PROMISE_REJECTED) {
            interp->exception_pending = 1;
            interp->exception_value = lr_dup_value(interp->ctx, pd->result);
            const char *s = lr_to_cstring(interp->ctx, pd->result);
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "%s", s ? s : "Promise rejected");
            lr_free_cstring(interp->ctx, s);
            interp->error_flag = 1;
            lr_free_value(interp->ctx, arg);
            return LR_VALUE_UNDEFINED;
        }
        LRValue result = pd ? lr_dup_value(interp->ctx, pd->result)
                            : LR_VALUE_UNDEFINED;
        lr_free_value(interp->ctx, arg);
        return result;
    }

    /* If it's a thenable (object with .then), we handle it as a promise */
    if (lr_is_object(arg)) {
        LRValue then_val = lr_get_property_str(interp->ctx, arg, "then");
        int has_then = lr_is_function(interp->ctx, then_val);
        lr_free_value(interp->ctx, then_val);
        if (has_then) {
            /* For now, just return the value directly.
             * In a full implementation, we'd need to suspend/resume the
             * interpreter, which requires async/await support at the
             * interpreter level. */
            return arg;
        }
    }

    /* Non-promise values are returned directly */
    return arg;
}

static LRValue eval_class_expr(Interpreter *interp, ASTNode *node)
{
    LRContext *ctx = interp->ctx;

    /* Evaluate 'extends' clause */
    LRValue parent = LR_VALUE_UNDEFINED;
    if (node->u.class_decl.extends) {
        parent = interp_eval_node(interp, node->u.class_decl.extends);
        if (interp->error_flag) {
            lr_free_value(ctx, parent);
            return LR_VALUE_UNDEFINED;
        }
        /* Cache resolved parent for fast super() dispatch */
        if (lr_is_object(parent))
            extends_cache_set(ctx, node->u.class_decl.extends, parent);
    }

    /* Create the prototype object (inherits from parent.prototype) */
    LRValue proto;
    if (lr_is_object(parent)) {
        LRValue pproto = lr_get_property_str(ctx, parent, "prototype");
        if (lr_is_object(pproto)) {
            proto = lr_new_object_proto(ctx, pproto);
        } else {
            proto = lr_new_object(ctx);
        }
        lr_free_value(ctx, pproto);
    } else {
        proto = lr_new_object(ctx);
    }

    /* Create the constructor function object.
     * Its extra points at the class AST node so calls/new dispatch through
     * interp_call_class_function (field init + ctor body + implicit super).
     * Static members are inherited from the parent class object. */
    LRValue ctor;
    if (lr_is_object(parent)) {
        ctor = lr_new_object_proto(ctx, parent);
    } else {
        ctor = lr_new_object_proto(ctx, ctx->function_proto);
    }
    if (ctor.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)ctor.u.ptr;
        o->type = LR_OBJ_FUNCTION;
        o->extra = (void *)node;
        interp_capture_closure(interp, ctor);
    }

    const char *class_name = node->u.class_decl.name;
    if (class_name) {
        lr_set_property_str(ctx, ctor, "name", lr_new_string(ctx, class_name));
    }

    /* Wire prototype <-> constructor */
    lr_set_property_str(ctx, ctor, "prototype", lr_dup_value(ctx, proto));
    lr_set_property_str(ctx, proto, "constructor", lr_dup_value(ctx, ctor));

    /* Install methods, accessors, and static fields.
     * An inner scope binds the class name so static fields/blocks can
     * reference the class being defined (e.g. static { C.v = 42; }). */
    interp_push_scope(interp, 0);
    if (class_name) {
        scope_declare_name(interp, class_name, lr_dup_value(ctx, ctor), 1);
    }
    int nmethods = node->u.class_decl.nmethods;
    ASTNode **methods = node->u.class_decl.methods;
    for (int i = 0; i < nmethods; i++) {
        ASTNode *m = methods[i];
        if (!m) continue;

        if (m->type == AST_FUNC_EXPR) {
            const char *mname = m->u.func.name;

            /* The constructor body is invoked via the class node itself */
            if (!m->u.func.is_static && !m->u.func.is_getter && !m->u.func.is_setter &&
                mname && strcmp(mname, "constructor") == 0) {
                continue;
            }

            /* Static initialization block: run now with this = class */
            if (m->u.func.is_static && mname &&
                strcmp(mname, "__static_block__") == 0) {
                LRValue fn = eval_func_expr(interp, m);
                LRValue r = call_value_with_args(interp, m, fn, ctor, 0, NULL);
                lr_free_value(ctx, r);
                lr_free_value(ctx, fn);
                if (interp->error_flag) break;
                continue;
            }

            LRValue target = m->u.func.is_static ? ctor : proto;
            LRValue fn = eval_func_expr(interp, m);

            if (m->u.func.key_expr) {
                /* Computed method name: [expr]() {} */
                LRValue kv = interp_eval_node(interp, m->u.func.key_expr);
                if (interp->error_flag) {
                    lr_free_value(ctx, fn);
                    lr_free_value(ctx, kv);
                    continue;
                }
                LRString *atom = lr_to_atom(ctx, kv);
                if (m->u.func.is_getter) {
                    lr_set_accessor_property(ctx, target, atom, fn, LR_VALUE_UNDEFINED);
                } else if (m->u.func.is_setter) {
                    lr_set_accessor_property(ctx, target, atom, LR_VALUE_UNDEFINED, fn);
                } else {
                    lr_set_property(ctx, target, atom, fn);
                }
                lr_free_value(ctx, kv);
            } else {
                const char *key = mname ? mname : "";
                if (m->u.func.is_getter) {
                    lr_set_accessor_property_str(ctx, target, key, fn, LR_VALUE_UNDEFINED);
                } else if (m->u.func.is_setter) {
                    lr_set_accessor_property_str(ctx, target, key, LR_VALUE_UNDEFINED, fn);
                } else {
                    lr_set_property_str(ctx, target, key, fn);
                }
            }
        } else if (m->type == AST_PROPERTY && m->u.property.is_static) {
            /* Static class field: evaluated once at class definition */
            LRValue v = m->u.property.val
                ? interp_eval_node(interp, m->u.property.val)
                : LR_VALUE_UNDEFINED;
            if (interp->error_flag) {
                lr_free_value(ctx, v);
                continue;
            }
            if (m->u.property.key && m->u.property.key->type == AST_IDENTIFIER) {
                lr_set_property_str(ctx, ctor, m->u.property.key->u.ident.name, v);
            } else if (m->u.property.key) {
                LRValue kv = interp_eval_node(interp, m->u.property.key);
                LRString *atom = lr_to_atom(ctx, kv);
                lr_set_property(ctx, ctor, atom, v);
                lr_free_value(ctx, kv);
            } else {
                lr_free_value(ctx, v);
            }
        }
        /* Instance fields (AST_PROPERTY, !is_static) are initialized in
         * interp_call_class_function at construction time. */
    }
    interp_pop_scope(interp);

    lr_free_value(ctx, proto);
    lr_free_value(ctx, parent);
    return ctor;
}

/* ── Destructuring ─────────────────────────────────────────────────────── */

static LRValue eval_pattern(Interpreter *interp, ASTNode *node, LRValue value)
{
    /* Discriminate array vs object patterns: AST_ARRAY/AST_OBJECT literals
     * used as assignment targets, or AST_PATTERN with is_object flag. */
    int is_obj_pattern = (node->type == AST_OBJECT) ||
        (node->type == AST_PATTERN && node->u.pattern_object.is_object);

    /* Handle array destructuring: [a, b] = arr */
    if (!is_obj_pattern) {
        /* Array destructuring */
        int nelem = node->u.pattern_array.nelem;
        ASTNode **elements = node->u.pattern_array.elements;

        for (int i = 0; i < nelem; i++) {
            ASTNode *elem = elements[i];
            if (elem == NULL) continue; /* hole */

            if (elem->type == AST_REST || elem->type == AST_SPREAD_ELEMENT) {
                /* Rest element: ...rest */
                ASTNode *rest_target = elem->type == AST_REST ? elem->u.rest_elem.arg
                                                              : elem->u.spread.arg;
                if (rest_target && rest_target->type == AST_IDENTIFIER) {
                    const char *name = rest_target->u.ident.name;
                    /* Create a new array with remaining elements */
                    LRValue rest_arr = lr_new_array(interp->ctx);
                    int32_t len = 0;
                    LRValue len_val = lr_get_property_str(interp->ctx, value, "length");
                    lr_to_int32(interp->ctx, &len, len_val);
                    lr_free_value(interp->ctx, len_val);
                    int idx = 0;
                    for (int32_t j = i; j < len; j++) {
                        LRValue item = lr_get_property_uint32(interp->ctx, value, j);
                        lr_set_property_uint32(interp->ctx, rest_arr, idx++, item);
                        lr_free_value(interp->ctx, item);
                    }
                    lr_set_property_str(interp->ctx, rest_arr, "length", lr_new_int32(interp->ctx, idx));
                    scope_declare_name(interp, name, rest_arr, 0);
                    lr_free_value(interp->ctx, rest_arr);
                }
                break;
            }

            if (elem->type == AST_DEFAULT_VALUE) {
                ASTNode *left = elem->u.default_val.left;
                /* Get value or default */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                if (lr_is_undefined(item_val)) {
                    lr_free_value(interp->ctx, item_val);
                    item_val = interp_eval_node(interp, elem->u.default_val.right);
                }
                if (left->type == AST_IDENTIFIER) {
                    scope_declare_name(interp, left->u.ident.name, item_val, 0);
                }
                lr_free_value(interp->ctx, item_val);
            } else if (elem->type == AST_IDENTIFIER) {
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                scope_declare_name(interp, elem->u.ident.name, item_val, 0);
                lr_free_value(interp->ctx, item_val);
            } else if (elem->type == AST_PATTERN || elem->type == AST_ARRAY ||
                       elem->type == AST_OBJECT) {
                /* Nested destructuring */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                eval_pattern(interp, elem, item_val);
                lr_free_value(interp->ctx, item_val);
            } else if (elem->type == AST_ASSIGN &&
                       elem->u.assign.target &&
                       elem->u.assign.target->type == AST_IDENTIFIER) {
                /* Default from expression form: [a = 1] = ... */
                LRValue item_val = lr_get_property_uint32(interp->ctx, value, i);
                if (lr_is_undefined(item_val)) {
                    lr_free_value(interp->ctx, item_val);
                    item_val = interp_eval_node(interp, elem->u.assign.value);
                }
                scope_declare_name(interp, elem->u.assign.target->u.ident.name, item_val, 0);
                lr_free_value(interp->ctx, item_val);
            }
        }
        return LR_VALUE_UNDEFINED;
    }

    /* Object destructuring: {a, b} = obj */
    {
        int nprops = node->u.pattern_object.nprops;
        ASTNode **props = node->u.pattern_object.props;

        for (int i = 0; i < nprops; i++) {
            ASTNode *prop = props[i];
            if (prop->type == AST_REST || prop->type == AST_SPREAD ||
                prop->type == AST_SPREAD_ELEMENT) {
                /* Rest in object pattern */
                ASTNode *rest_target = prop->type == AST_REST ? prop->u.rest_elem.arg
                                                              : prop->u.spread.arg;
                if (rest_target && rest_target->type == AST_IDENTIFIER) {
                    LRValue rest_obj = lr_new_object(interp->ctx);
                    /* Copy own properties, excluding keys consumed by the
                     * preceding pattern properties. */
                    LRPropertyEnum *pe = NULL;
                    uint32_t npe = 0;
                    lr_get_own_property_names(interp->ctx, &pe, &npe, value, 0);
                    for (uint32_t j = 0; j < npe; j++) {
                        const char *kname = lr_atom_to_cstring(interp->ctx, pe[j].atom);
                        int consumed = 0;
                        for (int k = 0; k < i && !consumed; k++) {
                            ASTNode *pp = props[k];
                            if (pp && pp->type == AST_PROPERTY && pp->u.property.key &&
                                pp->u.property.key->type == AST_IDENTIFIER &&
                                kname &&
                                strcmp(pp->u.property.key->u.ident.name, kname) == 0) {
                                consumed = 1;
                            }
                        }
                        if (!consumed) {
                            LRValue v = lr_get_property(interp->ctx, value, pe[j].atom);
                            lr_set_property(interp->ctx, rest_obj, pe[j].atom, v);
                            lr_free_value(interp->ctx, v);
                        }
                    }
                    lr_free_property_enum(interp->ctx, pe, npe);
                    scope_declare_name(interp, rest_target->u.ident.name, rest_obj, 0);
                    lr_free_value(interp->ctx, rest_obj);
                }
                break;
            }

            if (prop->type == AST_PROPERTY) {
                ASTNode *key_node = prop->u.property.key;
                ASTNode *val_node = prop->u.property.val;
                int shorthand = prop->u.property.shorthand;

                const char *key_name = NULL;
                if (key_node->type == AST_IDENTIFIER) {
                    key_name = key_node->u.ident.name;
                }

                if (key_name) {
                    LRValue prop_val = lr_get_property_str(interp->ctx, value, key_name);

                    if (val_node->type == AST_DEFAULT_VALUE) {
                        ASTNode *left = val_node->u.default_val.left;
                        if (lr_is_undefined(prop_val)) {
                            lr_free_value(interp->ctx, prop_val);
                            prop_val = interp_eval_node(interp, val_node->u.default_val.right);
                        }
                        if (left->type == AST_IDENTIFIER) {
                            scope_declare_name(interp, left->u.ident.name, prop_val, 0);
                        }
                        lr_free_value(interp->ctx, prop_val);
                    } else if (val_node->type == AST_IDENTIFIER) {
                        const char *target_name = val_node->u.ident.name;
                        if (shorthand) {
                            target_name = key_name;
                        }
                        scope_declare_name(interp, target_name, prop_val, 0);
                        lr_free_value(interp->ctx, prop_val);
                    } else if (val_node->type == AST_ASSIGN &&
                               val_node->u.assign.target &&
                               val_node->u.assign.target->type == AST_IDENTIFIER) {
                        /* Shorthand default from object literal: { a = 1 } */
                        if (lr_is_undefined(prop_val)) {
                            lr_free_value(interp->ctx, prop_val);
                            prop_val = interp_eval_node(interp, val_node->u.assign.value);
                        }
                        scope_declare_name(interp, val_node->u.assign.target->u.ident.name,
                                           prop_val, 0);
                        lr_free_value(interp->ctx, prop_val);
                    } else if (val_node->type == AST_PATTERN || val_node->type == AST_ARRAY ||
                               val_node->type == AST_OBJECT) {
                        eval_pattern(interp, val_node, prop_val);
                        lr_free_value(interp->ctx, prop_val);
                    } else {
                        lr_free_value(interp->ctx, prop_val);
                    }
                }
            }
        }
        return LR_VALUE_UNDEFINED;
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Generator Support (eager evaluation) ──────────────────────────────
 * Generator bodies run eagerly at call time; yields are buffered into a
 * JS array. The returned generator object steps through the buffer via
 * next()/return() and is iterable (Symbol.iterator returns itself).
 * A yield cap guards against unbounded/infinite generators. */

#define GEN_ITEMS_PROP "__gen_items"
#define GEN_BUF_PROP   "__gen_buf"   /* buffered yields from nested eval */
#define GEN_BUF_IDX    "__gen_bufi"  /* next index in buf */
#define GEN_INDEX_PROP "__gen_i"
#define GEN_DONE_PROP  "__gen_done"
#define GEN_RET_PROP   "__gen_ret"
#define GEN_BODY_PROP  "__gen_body"  /* AST_BLOCK: function body statements */
#define GEN_PC_PROP    "__gen_pc"    /* next statement index in body */
#define GEN_SCOPE_PROP "__gen_scope" /* scope at creation time */

/* Build a { value, done } iterator result (takes ownership of value).
 * Recycles from the generator's result-object pool when possible,
 * avoiding per-call lr_new_object allocation. */
static LRValue gen_make_result(LREagerGenData *gd, LRValue value, int done)
{
    LRContext *ctx = gd->ctx;
    /* Try to reuse a pooled object */
    int pc = gd->result_pool_count;
    if (pc < GEN_RESULT_POOL_SIZE) {
        LRValue obj = gd->result_pool[pc];
        gd->result_pool_count = pc + 1;
        if (obj.tag == LR_TYPE_OBJECT) {
            /* Reuse: update properties in place */
            lr_set_property_str(ctx, obj, "value", value);
            lr_set_property_str(ctx, obj, "done", done ? LR_VALUE_TRUE : LR_VALUE_FALSE);
            return obj;
        }
        /* Slot is empty — need to allocate */
    } else {
        /* Pool full — recycle the first slot */
        LRValue obj = gd->result_pool[0];
        if (obj.tag == LR_TYPE_OBJECT) {
            lr_set_property_str(ctx, obj, "value", value);
            lr_set_property_str(ctx, obj, "done", done ? LR_VALUE_TRUE : LR_VALUE_FALSE);
            return obj;
        }
    }
    /* Pool exhausted or invalid — allocate new object */
    LRValue res = lr_new_object(ctx);
    lr_set_property_str(ctx, res, "value", value);
    lr_set_property_str(ctx, res, "done", done ? LR_VALUE_TRUE : LR_VALUE_FALSE);
    return res;
}

static LRValue gen_next_cfunc(LRContext *ctx, LRValue this_val,
                              int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    /* Get the LREagerGenData stored in the generator object's opaque */
    LREagerGenData *gd = NULL;
    if (this_val.tag == LR_TYPE_OBJECT)
        gd = (LREagerGenData *)((LRObject *)this_val.u.ptr)->opaque;
    if (!gd || gd->freed || !gd->body || gd->body->type != AST_BLOCK)
        return gen_make_result(gd, LR_VALUE_UNDEFINED, 1);
    if (gd->done)
        return gen_make_result(gd, LR_VALUE_UNDEFINED, 1);

    /* If we've already executed the body and collected yields, drain from items */
    if (gd->count > 0) {
        if (gd->idx < gd->count) {
            LRValue val = lr_get_property_uint32(ctx, gd->items, (uint32_t)gd->idx);
            gd->idx++;
            return gen_make_result(gd, lr_dup_value(ctx, val), 0);
        }
        gd->done = 1;
        lr_set_property_str(ctx, this_val, GEN_DONE_PROP, LR_VALUE_TRUE);
        return gen_make_result(gd, lr_dup_value(ctx, gd->ret), 1);
    }

    /* Eager mode: execute body to completion, collect yields into items */
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp) {
        gd->done = 1;
        return gen_make_result(gd, LR_VALUE_UNDEFINED, 1);
    }

    InterpScope *saved_scope = interp->current_scope;
    if (gd->scope) interp->current_scope = gd->scope;

    int saved_gen_active = interp->gen_active;
    LRValue saved_gen_items = interp->gen_items;
    int saved_gen_count = interp->gen_count;
    int saved_has_returned = interp->has_returned;

    interp->gen_active = 1;
    interp->gen_items = lr_dup_value(ctx, gd->items);
    interp->gen_count = gd->count;
    interp->has_returned = 0;

    gd->ret = lr_dup_value(ctx, interp_eval_node(interp, gd->body));
    if (interp->has_returned) {
        /* Body had an explicit return — use interp->return_value instead */
        lr_free_value(ctx, gd->ret);
        gd->ret = lr_dup_value(ctx, interp->return_value);
    }
    interp->has_returned = saved_has_returned;

    gd->count = interp->gen_count;
    lr_set_property_str(ctx, gd->items, "length",
        lr_new_int32(ctx, gd->count));

    lr_free_value(ctx, interp->gen_items);
    interp->gen_active = saved_gen_active;
    interp->gen_items = saved_gen_items;
    interp->gen_count = saved_gen_count;
    interp->current_scope = saved_scope;

    if (interp->error_flag) {
        interp->error_flag = 0;
    }

    /* Now drain from collected items */
    if (gd->idx < gd->count) {
        LRValue val = lr_get_property_uint32(ctx, gd->items, (uint32_t)gd->idx);
        gd->idx++;
        return gen_make_result(gd, lr_dup_value(ctx, val), 0);
    }

    gd->done = 1;
    lr_set_property_str(ctx, this_val, GEN_DONE_PROP, LR_VALUE_TRUE);
    return gen_make_result(gd, lr_dup_value(ctx, gd->ret), 1);
}

static LRValue gen_return_cfunc(LRContext *ctx, LRValue this_val,
                                int argc, LRValue *argv)
{
    LREagerGenData *gd = NULL;
    if (this_val.tag == LR_TYPE_OBJECT)
        gd = (LREagerGenData *)((LRObject *)this_val.u.ptr)->opaque;
    if (gd && !gd->freed) gd->done = 1;
    lr_set_property_str(ctx, this_val, GEN_DONE_PROP, LR_VALUE_TRUE);
    LRValue v = (argc > 0) ? lr_dup_value(ctx, argv[0]) : LR_VALUE_UNDEFINED;
    return gen_make_result(gd, v, 1);
}

static LRValue gen_throw_cfunc(LRContext *ctx, LRValue this_val,
                               int argc, LRValue *argv)
{
    LREagerGenData *gd = NULL;
    if (this_val.tag == LR_TYPE_OBJECT)
        gd = (LREagerGenData *)((LRObject *)this_val.u.ptr)->opaque;
    if (gd && !gd->freed) gd->done = 1;
    lr_set_property_str(ctx, this_val, GEN_DONE_PROP, LR_VALUE_TRUE);
    if (argc > 0)
        return lr_dup_value(ctx, argv[0]);   /* throw exception to caller */
    return LR_VALUE_UNDEFINED;
}

static LRValue gen_self_cfunc(LRContext *ctx, LRValue this_val,
                              int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    return lr_dup_value(ctx, this_val);
}

/* Free LREagerGenData, releasing the scope reference and the cached
 * items/ret values.  Uses the saved ctx to ensure scope_release properly
 * tracks the runtime.
 *
 * The freed flag prevents double-free when multiple objects share the same
 * LREagerGenData pointer (which can happen when the scope_release triggered
 * below indirectly frees another object whose opaque pointer points to the
 * same LREagerGenData). */
void gen_lazy_data_free(void *ptr) {
    LREagerGenData *gd = (LREagerGenData *)ptr;
    if (gd && !gd->freed) {
        gd->freed = 1;
        if (gd->scope) {
            scope_release(gd->scope, gd->ctx);
        }
        /* The items array is shared with GEN_ITEMS_PROP (which keeps its
         * own reference); drop ours.  Same for ret / GEN_RET_PROP. */
        lr_free_value(gd->ctx, gd->items);
        lr_free_value(gd->ctx, gd->ret);
        free(gd);
    }
}

/* Build the generator object. For lazy generators, stores the body AST
 * and creation scope so gen_next can drive incremental execution. */
static LRValue gen_build_object(Interpreter *interp, ASTNode *body,
                                 InterpScope *scope, LRValue items)
{
    LRContext *ctx = interp->ctx;
    LRValue gen = lr_new_object(ctx);
    /* Store body AST + creation scope for lazy incremental evaluation */
    lr_set_property_str(ctx, gen, GEN_BODY_PROP, lr_new_int32(ctx, 0));
    /* Use opaque: store body pointer & scope for gen_next */
    if (gen.tag == LR_TYPE_OBJECT && body) {
        LRObject *o = (LRObject *)gen.u.ptr;
        LREagerGenData *gd = (LREagerGenData *)calloc(1, sizeof(LREagerGenData));
        if (gd) {
            gd->body = body;     /* AST_BLOCK of function body */
            gd->scope = scope;
            gd->pc = 0;
            gd->done = 0;
            gd->ctx = ctx;       /* save context for cleanup */
            gd->freed = 0;
            gd->ret = LR_VALUE_UNDEFINED;
            /* Initialize result pool to all UNDEFINED */
            for (int i = 0; i < GEN_RESULT_POOL_SIZE; i++) {
                gd->result_pool[i] = LR_VALUE_UNDEFINED;
            }
            gd->result_pool_count = 0;
            /* Initialize lazy yield tracking */
            gd->yield_target = 0;
            /* Keep the scope alive past interp_call_function's pop */
            if (scope) scope->refcount++;
            o->opaque = gd;
            o->opaque_free = gen_lazy_data_free;
        }
    }
    /* Create the items array for buffered yields */
    LRValue items_arr = lr_new_array(ctx);
    lr_set_property_str(ctx, gen, GEN_ITEMS_PROP, lr_dup_value(ctx, items_arr));
    if (gen.tag == LR_TYPE_OBJECT && body) {
        LRObject *o = (LRObject *)gen.u.ptr;
        LREagerGenData *gd = (LREagerGenData *)o->opaque;
        if (gd) gd->items = lr_dup_value(ctx, items_arr);
    }
    lr_free_value(ctx, items_arr);
    lr_set_property_str(ctx, gen, GEN_INDEX_PROP, lr_new_int32(ctx, 0));
    lr_set_property_str(ctx, gen, GEN_DONE_PROP, LR_VALUE_FALSE);
    lr_set_property_str(ctx, gen, GEN_RET_PROP, LR_VALUE_UNDEFINED);
    lr_set_property_str(ctx, gen, GEN_PC_PROP, lr_new_int32(ctx, 0));
    lr_set_property_str(ctx, gen, "next",
        lr_new_cfunction(ctx, gen_next_cfunc, "next", 0));
    lr_set_property_str(ctx, gen, "return",
        lr_new_cfunction(ctx, gen_return_cfunc, "return", 1));
    lr_set_property_str(ctx, gen, "throw",
        lr_new_cfunction(ctx, gen_throw_cfunc, "throw", 1));
    lr_set_property_str(ctx, gen, "Symbol.iterator",
        lr_new_cfunction(ctx, gen_self_cfunc, "[Symbol.iterator]", 0));
    return gen;
}

/* Append one yielded value to the active generator buffer (dups v) */
static void gen_append(Interpreter *interp, LRValue v)
{
    lr_set_property_uint32(interp->ctx, interp->gen_items,
                           (uint32_t)interp->gen_count++,
                           lr_dup_value(interp->ctx, v));
}

/* yield* delegation: append every element of an iterable.
 * Cap nesting depth to prevent stack overflow from deep `yield*` chains.
 * Increased to 8192 to support deep generator recursion (e.g., 500+ levels). */
#define GEN_DELEGATE_MAX_DEPTH 8192
static int gen_delegate_depth = 0;

static void gen_delegate(Interpreter *interp, LRValue src)
{
    if (++gen_delegate_depth > GEN_DELEGATE_MAX_DEPTH) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "yield* delegation depth exceeded (%d)", GEN_DELEGATE_MAX_DEPTH);
        interp->error_flag = 1;
        gen_delegate_depth--;
        return;
    }
    LRContext *ctx = interp->ctx;
    if (lr_is_string(src)) {
        const char *s = lr_to_cstring(ctx, src);
        size_t slen = s ? strlen(s) : 0;
        char buf[2] = {0, 0};
        for (size_t i = 0; i < slen && !interp->error_flag; i++) {
            buf[0] = s[i];
            LRValue item = lr_new_string(ctx, buf);
            gen_append(interp, item);
            lr_free_value(ctx, item);
        }
        lr_free_cstring(ctx, s);
        gen_delegate_depth--;
        return;
    }
    if (lr_is_array(ctx, src)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(ctx, src, "length");
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        for (int32_t i = 0; i < len && !interp->error_flag; i++) {
            LRValue item = lr_get_property_uint32(ctx, src, (uint32_t)i);
            gen_append(interp, item);
            lr_free_value(ctx, item);
        }
        gen_delegate_depth--;
        return;
    }
    if (lr_is_object(src)) {
        LRValue iter_fn = lr_get_property_str(ctx, src, "Symbol.iterator");
        if (lr_is_function(ctx, iter_fn)) {
            LRValue iter = call_value_with_args(interp, NULL, iter_fn, src, 0, NULL);
            lr_free_value(ctx, iter_fn);
            if (interp->error_flag) { lr_free_value(ctx, iter); return; }
            if (lr_is_object(iter)) {
                LRValue next_fn = lr_get_property_str(ctx, iter, "next");
                while (!interp->error_flag) {
                    LRValue nr = call_value_with_args(interp, NULL, next_fn, iter, 0, NULL);
                    if (interp->error_flag) { lr_free_value(ctx, nr); break; }
                    LRValue done = lr_get_property_str(ctx, nr, "done");
                    int is_done = lr_to_bool(ctx, done);
                    lr_free_value(ctx, done);
                    if (is_done) { lr_free_value(ctx, nr); break; }
                    LRValue value = lr_get_property_str(ctx, nr, "value");
                    gen_append(interp, value);
                    lr_free_value(ctx, value);
                    lr_free_value(ctx, nr);
                }
                lr_free_value(ctx, next_fn);
            }
            lr_free_value(ctx, iter);
            gen_delegate_depth--;
            return;
        }
        lr_free_value(ctx, iter_fn);
    }
    snprintf(interp->error_message, sizeof(interp->error_message),
             "yield* operand is not iterable");
    interp->exception_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 1;
    gen_delegate_depth--;
}

/* ── Function Call Support ─────────────────────────────────────────────── */

/* Lightweight function call path for the bytecode VM.
 *
 * Optimised for the BC_CALL handler in lr_bytecode.c: skips function name
 * extraction (unused by the VM), call frame push/pop (unnecessary for
 * bytecode stack traces), and lazy error-message save/restore.  Generator
 * and async functions should use the full interp_call_function path.
 *
 * closure_scope is passed directly (not via interp->pending_closure) so
 * the caller does not need to set a temporary field on the interpreter. */
LRValue interp_bc_call_function(Interpreter *interp, ASTNode *func_node,
                                InterpScope *closure_scope, LRValue this_val,
                                int argc, LRValue *argv)
{
    if (interp->depth >= MAX_CALL_DEPTH) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "Maximum call stack size exceeded");
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }
    interp->depth++;

    /* LR_DEBUG_STACK: trace deep/infinite recursion from a single run of
     * any script.  Prints the callee name each time the call depth crosses
     * a new 100-level bucket, revealing which function recurses deeply.
     * Each bc_execute level alloca's ~4KB+, so the C stack can overflow
     * (8MB default) before MAX_CALL_DEPTH (4096) is reached. */
    if (lr_env_flag(&g_lr_env_debug_stack, "LR_DEBUG_STACK") && (interp->depth % 100 == 0)) {
        const char *dbg_name = (func_node->type == AST_FUNC_EXPR ||
                                func_node->type == AST_FUNC_DECL)
                               ? (func_node->u.func.name ? func_node->u.func.name
                                                         : "(anon)")
                               : "(arrow)";
        fprintf(stderr, "[STACK] depth=%d fn='%s'\n", interp->depth, dbg_name);
    }
    /* Extract body, params, nparams, flags */
    ASTNode *body = NULL;
    int nparams = 0;
    ASTNode **params = NULL;
    int is_arrow = 0;
    int is_generator = 0;
    int is_async = 0;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        body = func_node->u.func.body;
        nparams = func_node->u.func.nparams;
        params = func_node->u.func.params;
        is_async = func_node->u.func.is_async;
        is_generator = func_node->u.func.is_generator;
    } else if (func_node->type == AST_ARROW) {
        body = func_node->u.arrow.body;
        nparams = func_node->u.arrow.nparams;
        params = func_node->u.arrow.params;
        is_arrow = 1;
        is_async = func_node->u.arrow.is_async;
    }
    /* ── Compact state save (no call frame, no lazy error copy) ──────── */
    struct {
        int          break_target;
        int          continue_target;
        int          return_target;
        int          has_returned;
        int          error_flag;
        LRValue      return_value;
        const char  *pending_label;
        InterpScope *current_scope;
    } saved = {
        interp->break_target, interp->continue_target,
        interp->return_target, interp->has_returned,
        interp->error_flag, interp->return_value,
        interp->pending_label, interp->current_scope
    };

    interp->break_target = 0;
    interp->continue_target = 0;
    interp->pending_label = NULL;
    interp->return_target = 0;
    interp->has_returned = 0;
    interp->return_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 0;

    /* Generator state: only save/restore for actual generator functions.
     * For the common case (non-generator, ~99.9% of calls), skip entirely. */
    int saved_gen_active = 0;
    LRValue saved_gen_items = LR_VALUE_UNDEFINED;
    int saved_gen_count = 0;
    if (is_generator) {
        saved_gen_active = interp->gen_active;
        saved_gen_items = interp->gen_items;
        saved_gen_count = interp->gen_count;
        interp->gen_active = 1;
        interp->gen_items = lr_new_array(interp->ctx);
        interp->gen_count = 0;
    }

    /* ── Create function scope ───────────────────────────────────────── */
    InterpScope *func_scope = NULL;

    /* ── Fast path: batch scope + parameter binding with scope_new_inline ──
     * Instead of scope_new + N×scope_declare_name_direct (which has per-call
     * function call overhead), pre-compute the names/values arrays and create
     * the scope with all entries in one shot.  This is ~3× faster for the
     * common case of simple functions with a few parameters. */
    int use_inline_scope = 0;
    const char **inline_names = NULL;
    LRValue *inline_vals = NULL;
    int inline_count = 0;

    /* We can use the inline fast path when:
     *   - No class extends (no super bindings)
     *   - Function body doesn't reference `arguments` (~95% of functions)
     *   - All parameters are simple identifiers
     *   - No rest/default/destructuring parameters */
    int has_super = 0;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        if (func_node->u.func.class_node &&
            func_node->u.func.class_node->u.class_decl.extends)
            has_super = 1;
    } else if (func_node->type == AST_CLASS_DECL) {
        /* When called via super() dispatch, func_node is the class itself.
         * Check the class's extends directly. */
        has_super = (func_node->u.class_decl.extends != NULL);
    }
    int has_args = !is_arrow && body && ast_scans_arguments(body);

    if (!has_super && !has_args && nparams > 0) {
        int all_simple = 1;
        for (int i = 0; i < nparams; i++) {
            if (params[i]->type != AST_IDENTIFIER) { all_simple = 0; break; }
        }
        if (all_simple) {
            use_inline_scope = 1;
            inline_count = 1 + nparams; /* "this" + params */
            const char **tmp_names = (const char **)malloc(sizeof(const char *) * inline_count);
            LRValue *tmp_vals = (LRValue *)malloc(sizeof(LRValue) * inline_count);
            if (tmp_names && tmp_vals) {
                tmp_names[0] = "this";
                tmp_vals[0]  = this_val;
                for (int i = 0; i < nparams; i++) {
                    tmp_names[1 + i] = params[i]->u.ident.name;
                    tmp_vals[1 + i]  = (i < argc) ? argv[i] : LR_VALUE_UNDEFINED;
                }
                func_scope = scope_new_inline(
                    closure_scope ? closure_scope : interp->current_scope,
                    inline_count, tmp_names, tmp_vals);
                interp->current_scope = func_scope;
            }
            free(tmp_names);
            free(tmp_vals);
        }
    }

    if (!use_inline_scope) {
        func_scope = scope_new(
            closure_scope ? closure_scope : interp->current_scope, 1, 0);
        interp->current_scope = func_scope;

        /* Bind 'this' */
        scope_declare_name_direct(func_scope, "this", this_val, 1);

        /* Bind super references for class methods (derived classes) */
        if (has_super) {
            ASTNode *ext = (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL)
                ? func_node->u.func.class_node->u.class_decl.extends
                : func_node->u.class_decl.extends;
            LRValue superctor, sproto;
            if (extends_cache_get_super(interp->ctx, ext, &superctor, &sproto)) {
                scope_declare_name_direct(func_scope, "%superctor%", superctor, 1);
                scope_declare_name_direct(func_scope, "%superproto%", sproto, 1);
            } else {
                LRValue parent = extends_cache_get(ext);
                int cached_ok = lr_is_object(parent);
                if (!cached_ok) {
                    parent = interp_eval_node(interp, ext);
                    if (!interp->error_flag && lr_is_object(parent))
                        extends_cache_set(interp->ctx, ext, parent);
                }
                if (cached_ok || (lr_is_object(parent) && !interp->error_flag)) {
                    sproto = lr_get_property_str(interp->ctx, parent, "prototype");
                    scope_declare_name_direct(func_scope, "%superctor%", parent, 1);
                    scope_declare_name_direct(func_scope, "%superproto%", sproto, 1);
                    extends_cache_set_super(interp->ctx, ext, parent, sproto);
                    lr_free_value(interp->ctx, sproto);
                }
                if (!cached_ok) lr_free_value(interp->ctx, parent);
            }
        }

        /* Bind 'arguments' object (not for arrow functions, only when referenced) */
        if (has_args) {
            LRValue args_obj = lr_new_object(interp->ctx);
            for (int i = 0; i < argc; i++) {
                lr_set_property_uint32(interp->ctx, args_obj, i, lr_dup_value(interp->ctx, argv[i]));
            }
            lr_set_property_str(interp->ctx, args_obj, "length", lr_new_int32(interp->ctx, argc));
            lr_set_property_str(interp->ctx, args_obj, "callee", LR_VALUE_UNDEFINED);
            scope_declare_name_direct(func_scope, "arguments", args_obj, 0);
            lr_free_value(interp->ctx, args_obj);
        }

        /* Bind parameters */
        for (int i = 0; i < nparams; i++) {
            ASTNode *param = params[i];
            if (param->type == AST_IDENTIFIER) {
                const char *pname = param->u.ident.name;
                if (i < argc) {
                    scope_declare_name_direct(func_scope, pname, argv[i], 0);
                } else {
                    scope_declare_name_direct(func_scope, pname, LR_VALUE_UNDEFINED, 0);
                }
            } else if (param->type == AST_REST || param->type == AST_SPREAD_ELEMENT) {
                /* … rest param handling … */
                ASTNode *rest_target = param->type == AST_REST ? param->u.rest_elem.arg
                                                               : param->u.spread.arg;
                if (rest_target && rest_target->type == AST_IDENTIFIER) {
                    LRValue rest_arr = lr_new_array(interp->ctx);
                    int idx = 0;
                    for (int j = i; j < argc; j++) {
                        lr_set_property_uint32(interp->ctx, rest_arr, idx++, lr_dup_value(interp->ctx, argv[j]));
                    }
                    lr_set_property_str(interp->ctx, rest_arr, "length", lr_new_int32(interp->ctx, idx));
                    scope_declare_name_direct(func_scope, rest_target->u.ident.name, rest_arr, 0);
                    lr_free_value(interp->ctx, rest_arr);
                }
                break;
            } else if (param->type == AST_DEFAULT_VALUE) {
                ASTNode *left = param->u.default_val.left;
                ASTNode *right = param->u.default_val.right;
                if (left->type == AST_IDENTIFIER) {
                    const char *pname = left->u.ident.name;
                    if (i < argc && !lr_is_undefined(argv[i])) {
                        scope_declare_name_direct(func_scope, pname, argv[i], 0);
                    } else {
                        LRValue def_val = interp_eval_node(interp, right);
                        scope_declare_name_direct(func_scope, pname, def_val, 0);
                        lr_free_value(interp->ctx, def_val);
                    }
                }
            } else if (param->type == AST_PATTERN || param->type == AST_ARRAY ||
                       param->type == AST_OBJECT) {
                eval_pattern(interp, param, i < argc ? argv[i] : LR_VALUE_UNDEFINED);
            } else if (param->type == AST_ASSIGN &&
                       param->u.assign.target &&
                       param->u.assign.target->type == AST_IDENTIFIER) {
                const char *pname = param->u.assign.target->u.ident.name;
                if (i < argc && !lr_is_undefined(argv[i])) {
                    scope_declare_name_direct(func_scope, pname, argv[i], 0);
                } else {
                    LRValue def_val = interp_eval_node(interp, param->u.assign.value);
                    scope_declare_name_direct(func_scope, pname, def_val, 0);
                    lr_free_value(interp->ctx, def_val);
                }
            }
        }
    }

    /* ── Evaluate the body via bytecode (preferred) or tree-walker ──── */
    LRValue result = LR_VALUE_UNDEFINED;
    if (body && !is_generator) {
        BCProgram *body_prog = bc_get_or_compile_func(func_node);
        if (body_prog) {
            /* ── JIT fast path ────────────────────────────────────────
             * If the function has been compiled to native code by the JIT
             * engine, dispatch directly to the native entry point instead
             * of interpreting the bytecodes.  The JIT code handles its own
             * stack frame and value stack.
             *
             * If the JIT code returns a bailout sentinel (tag == -1), it
             * means an unsupported bytecode was encountered.  Fall back to
             * the bytecode interpreter, which starts from the beginning of
             * the function (safe for side-effect-free functions).         */
            LRRuntime *rt = (LRRuntime *)interp->ctx->rt;
            if (rt && rt->jit_runtime && !body_prog->jit_skip) {
                LRJITEntry jit_entry = (LRJITEntry)body_prog->jit_entry;
                if (jit_entry) {
                    if (lr_env_flag(&g_lr_env_debug_jitcall, "LR_DEBUG_JITCALL")) {
                        fprintf(stderr, "[JITCALL] depth=%d has_ret=%d ret_val.tag=%d nargs=%d a0.tag=%d\n",
                                interp->depth, interp->has_returned, interp->return_value.tag,
                                argc, argc > 0 ? argv[0].tag : -1);
                        for (int ai = 0; ai < argc && ai < 4; ai++)
                            fprintf(stderr, "    argv[%d] tag=%d int32=%d\n",
                                    ai, argv[ai].tag, argv[ai].u.int32);
                    }
                    /* Point the interpreter at this function's JIT code block
                     * so the loop-tick handler can request hot-loop
                     * recompilation.  Save/restore for nested calls. */
                    void *saved_jit_code_ptr = interp->jit_code_ptr;
                    interp->jit_code_ptr = lr_jit_lookup_code(
                        (LRJITRuntime *)rt->jit_runtime, body_prog);
                    LRValue jit_result;
                    jit_entry(interp, body_prog->code, &jit_result);
                    result = jit_result;
                    interp->jit_code_ptr = saved_jit_code_ptr;
                    if (getenv("LR_DEBUG_JITCALL")) {
                        fprintf(stderr, "[JITCALL] return depth=%d has_ret=%d ret_val.tag=%d result.tag=%d result.int32=%d\n",
                                interp->depth, interp->has_returned, interp->return_value.tag,
                                result.tag, result.u.int32);
                    }
                    /* Bailout sentinel check: tag == -1 means JIT gave up */
                    if (result.tag != -1)
                        goto jit_bc_call_done;
                    /* Mark this function as bailed out so subsequent calls
                     * skip the JIT entry entirely and go to interpreter. */
                    lr_jit_mark_bailout((LRJITRuntime *)rt->jit_runtime, body_prog);
                    /* CRITICAL: Clear jit_entry so recursive calls fall through
                     * to the bytecode interpreter instead of hitting the JIT
                     * again and bail outing in an infinite loop. */
                    body_prog->jit_entry = NULL;
                    /* Fall through to bc_execute on bailout */
                    result = LR_VALUE_UNDEFINED;
                }
            }
            result = bc_execute(body_prog, interp->ctx);
        } else {
            result = interp_eval_node(interp, body);
        }
    }
    jit_bc_call_done:

    /* Capture return value when a `return` statement was executed */
    if (interp->has_returned) {
        result = lr_dup_value(interp->ctx, interp->return_value);
    }

    /* Generator: build the generator object with the body AST
     * stored for lazy incremental evaluation. */
    if (is_generator) {
        if (interp->error_flag) {
            lr_free_value(interp->ctx, interp->gen_items);
            lr_free_value(interp->ctx, result);
            result = LR_VALUE_UNDEFINED;
        } else {
            result = gen_build_object(interp, body, interp->current_scope,
                                      interp->gen_items);
            /* gen_build_object dups the array; release our reference */
            lr_free_value(interp->ctx, interp->gen_items);
        }
    }

    /* ── Pop scopes and restore caller state ─────────────────────────── */
    while (interp->current_scope && interp->current_scope != func_scope)
        interp_pop_scope(interp);
    interp->current_scope = saved.current_scope;
    scope_release(func_scope, interp->ctx);

    if (is_generator) {
        interp->gen_active = saved_gen_active;
        interp->gen_items = saved_gen_items;
        interp->gen_count = saved_gen_count;
    }

    interp->depth--;
    interp->break_target = saved.break_target;
    interp->continue_target = saved.continue_target;
    interp->pending_label = saved.pending_label;
    interp->return_target = saved.return_target;
    interp->has_returned = saved.has_returned;
    if (!saved.has_returned) {
        lr_free_value(interp->ctx, interp->return_value);
        interp->return_value = LR_VALUE_UNDEFINED;
    }
    interp->return_value = saved.return_value;
    if (!interp->error_flag) {
        interp->error_flag = saved.error_flag;
    }

    return result;
}

LRValue interp_call_function(Interpreter *interp, ASTNode *func_node,
                                     LRValue this_val, int argc, LRValue *argv)
{
    /* Consume the closure scope handed off by the caller (if any) */
    InterpScope *closure_scope = (InterpScope *)interp->pending_closure;
    if (getenv("LR_DEBUG_CLOSURE")) {
        fprintf(stderr, "[CL] call closure=%p names:[", (void*)closure_scope);
        InterpScope *cs_ = (InterpScope *)closure_scope;
        int guard_ = 0;
        while (cs_ && guard_++ < 32) {
            for (int k_ = 0; k_ < cs_->count; k_++) {
                if (cs_->names && cs_->names[k_]) fprintf(stderr, "'%s',", cs_->names[k_]);
            }
            cs_ = cs_->parent;
        }
        fprintf(stderr, "]\n");
    }
    interp->pending_closure = NULL;

    if (interp->depth >= MAX_CALL_DEPTH) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "Maximum call stack size exceeded");
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }

    interp->depth++;

    /* Determine function name and source location for call stack */
    const char *func_name = NULL;
    const char *filename = NULL;
    int line_number = 0;

    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        func_name = func_node->u.func.name;
        if (func_node->u.func.body) {
            line_number = func_node->u.func.body->token.line;
            (void)line_number; /* line info from token */
        }
    } else if (func_node->type == AST_ARROW) {
        func_name = NULL; /* arrow functions are anonymous */
        if (func_node->u.arrow.body) {
            line_number = func_node->u.arrow.body->token.line;
        }
    }

    /* Extract body, params, nparams early for scope cache lookup */
    ASTNode *body = NULL;
    int nparams = 0;
    ASTNode **params = NULL;
    int is_arrow = 0;
    int is_async = 0;
    int is_generator = 0;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        body = func_node->u.func.body;
        nparams = func_node->u.func.nparams;
        params = func_node->u.func.params;
        is_async = func_node->u.func.is_async;
        is_generator = func_node->u.func.is_generator;
    } else if (func_node->type == AST_ARROW) {
        body = func_node->u.arrow.body;
        nparams = func_node->u.arrow.nparams;
        params = func_node->u.arrow.params;
        is_arrow = 1;
        is_async = func_node->u.arrow.is_async;
    }

    /* NOTE: No function scope cache here — it was removed because it
     * caused recursive calls to corrupt parameter values.  The scope pool
     * + scope_declare_name_direct provide sufficient performance. */
    InterpScope *func_scope = NULL;

    /* Push a call frame onto the engine's call stack */
    lr_push_call_frame(interp->ctx, func_name ? func_name : "",
                       filename, func_node->token.line);

    /* ── Hot state: save/restore on every function call ──────────────── */
    /* Batch all scalar state into a compact struct and save with a single
     * struct copy (compiler sees this as a memcpy of contiguous fields,
     * which is faster than 10+ individual assignments). */
    struct CallFrameSave {
        int          break_target;
        int          continue_target;
        int          return_target;
        int          has_returned;
        int          error_flag;
        LRValue      return_value;
        const char  *pending_label;
        InterpScope *current_scope;
    };
    struct CallFrameSave saved = {
        interp->break_target, interp->continue_target,
        interp->return_target, interp->has_returned,
        interp->error_flag, interp->return_value,
        interp->pending_label, interp->current_scope
    };

    /* Lazy save: error_message is only copied when actually in error */
    char saved_error_message[256];
    int need_restore_error = 0;
    if (saved.error_flag) {
        memcpy(saved_error_message, interp->error_message, 256);
        need_restore_error = 1;
    }

    interp->break_target = 0;
    interp->continue_target = 0;
    interp->break_label[0] = '\0';
    interp->continue_label[0] = '\0';
    interp->pending_label = NULL;
    interp->return_target = 0;
    interp->has_returned = 0;
    interp->return_value = LR_VALUE_UNDEFINED;
    interp->error_flag = 0;

    /* Create a new function scope. Its parent is the function's captured
     * (lexical) defining scope when available, otherwise the call-time
     * scope (legacy dynamic behavior for direct AST invocations).
     * NOTE: Always creates a new scope — function scope cache was removed
     * because it corrupted parameter values in recursive calls. */
    func_scope = scope_new(
        closure_scope ? closure_scope : interp->current_scope, 1, 0);
    interp->current_scope = func_scope;

    /* Bind 'this' using direct fast path (fresh scope, no duplicate check needed) */
    scope_declare_name_direct(func_scope, "this", this_val, 1); /* const-like */

    /* Bind super references for class methods (derived classes).
     * Check extends_cache first: avoids re-evaluating extends AST every call. */
    int has_super = 0;
    if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
        if (func_node->u.func.class_node &&
            func_node->u.func.class_node->u.class_decl.extends)
            has_super = 1;
    } else if (func_node->type == AST_CLASS_DECL) {
        has_super = (func_node->u.class_decl.extends != NULL);
    }
    if (has_super) {
        ASTNode *ext = NULL;
        if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL) {
            ext = func_node->u.func.class_node->u.class_decl.extends;
        } else {
            ext = func_node->u.class_decl.extends;
        }
        LRValue superctor, sproto;
        if (extends_cache_get_super(interp->ctx, ext, &superctor, &sproto)) {
            scope_declare_name_direct(func_scope, "%superctor%", superctor, 1);
            scope_declare_name_direct(func_scope, "%superproto%", sproto, 1);
        } else {
            LRValue parent = extends_cache_get(ext);
            int cached_ok = lr_is_object(parent);
            if (!cached_ok) {
                parent = interp_eval_node(interp, ext);
                if (!interp->error_flag && lr_is_object(parent))
                    extends_cache_set(interp->ctx, ext, parent);
            }
            if (cached_ok || (lr_is_object(parent) && !interp->error_flag)) {
                sproto = lr_get_property_str(interp->ctx, parent, "prototype");
                scope_declare_name_direct(func_scope, "%superctor%", parent, 1);
                scope_declare_name_direct(func_scope, "%superproto%", sproto, 1);
                extends_cache_set_super(interp->ctx, ext, parent, sproto);
                lr_free_value(interp->ctx, sproto);
            }
            if (!cached_ok) lr_free_value(interp->ctx, parent);
        }
    }

    /* Generator state: only save/restore for actual generator functions.
     * For the common case (non-generator, ~99.9% of calls), skip entirely. */
    int saved_gen_active = 0;
    LRValue saved_gen_items = LR_VALUE_UNDEFINED;
    int saved_gen_count = 0;
    if (is_generator) {
        saved_gen_active = interp->gen_active;
        saved_gen_items = interp->gen_items;
        saved_gen_count = interp->gen_count;
        interp->gen_active = 1;
        interp->gen_items = lr_new_array(interp->ctx);
        interp->gen_count = 0;
    }

    /* Bind 'arguments' object (not for arrow functions).  Skip when the
     * function body does not reference `arguments` (~95% of functions). */
    if (!is_arrow) {
        ASTNode *args_body = NULL;
        if (func_node->type == AST_FUNC_EXPR || func_node->type == AST_FUNC_DECL)
            args_body = func_node->u.func.body;
        else if (func_node->type == AST_ARROW)
            args_body = func_node->u.arrow.body;
        if (args_body && ast_scans_arguments(args_body)) {
            LRValue args_obj = lr_new_object(interp->ctx);
            for (int i = 0; i < argc; i++) {
                lr_set_property_uint32(interp->ctx, args_obj, i, lr_dup_value(interp->ctx, argv[i]));
            }
            lr_set_property_str(interp->ctx, args_obj, "length", lr_new_int32(interp->ctx, argc));
            lr_set_property_str(interp->ctx, args_obj, "callee", LR_VALUE_UNDEFINED);
            scope_declare_name_direct(func_scope, "arguments", args_obj, 0);
            lr_free_value(interp->ctx, args_obj);
        }
    }

    /* Bind parameters using direct fast path for simple identifiers.
     * The scope is freshly created, so we can skip the duplicate check. */
    for (int i = 0; i < nparams; i++) {
        ASTNode *param = params[i];
        if (param->type == AST_IDENTIFIER) {
            const char *pname = param->u.ident.name;
            if (i < argc) {
                scope_declare_name_direct(func_scope, pname, argv[i], 0);
            } else {
                scope_declare_name_direct(func_scope, pname, LR_VALUE_UNDEFINED, 0);
            }
        } else if (param->type == AST_REST || param->type == AST_SPREAD_ELEMENT) {
            /* Rest parameter (AST_SPREAD_ELEMENT when the arrow params were
             * reconstructed from a parenthesized expression) */
            ASTNode *rest_target = param->type == AST_REST ? param->u.rest_elem.arg
                                                           : param->u.spread.arg;
            if (rest_target && rest_target->type == AST_IDENTIFIER) {
                LRValue rest_arr = lr_new_array(interp->ctx);
                int idx = 0;
                for (int j = i; j < argc; j++) {
                    lr_set_property_uint32(interp->ctx, rest_arr, idx++, lr_dup_value(interp->ctx, argv[j]));
                }
                lr_set_property_str(interp->ctx, rest_arr, "length", lr_new_int32(interp->ctx, idx));
                scope_declare_name_direct(func_scope, rest_target->u.ident.name, rest_arr, 0);
                lr_free_value(interp->ctx, rest_arr);
            }
            break;
        } else if (param->type == AST_DEFAULT_VALUE) {
            ASTNode *left = param->u.default_val.left;
            ASTNode *right = param->u.default_val.right;
            if (left->type == AST_IDENTIFIER) {
                const char *pname = left->u.ident.name;
                if (i < argc && !lr_is_undefined(argv[i])) {
                    scope_declare_name_direct(func_scope, pname, argv[i], 0);
                } else {
                    LRValue def_val = interp_eval_node(interp, right);
                    scope_declare_name_direct(func_scope, pname, def_val, 0);
                    lr_free_value(interp->ctx, def_val);
                }
            }
        } else if (param->type == AST_PATTERN || param->type == AST_ARRAY ||
                   param->type == AST_OBJECT) {
            /* Destructuring parameter (array/object literal nodes appear when
             * arrow params were reconstructed from an expression) */
            eval_pattern(interp, param, i < argc ? argv[i] : LR_VALUE_UNDEFINED);
        } else if (param->type == AST_ASSIGN &&
                   param->u.assign.target &&
                   param->u.assign.target->type == AST_IDENTIFIER) {
            /* Default parameter reconstructed from expression: (a = 1) => */
            const char *pname = param->u.assign.target->u.ident.name;
            if (i < argc && !lr_is_undefined(argv[i])) {
                scope_declare_name_direct(func_scope, pname, argv[i], 0);
            } else {
                LRValue def_val = interp_eval_node(interp, param->u.assign.value);
                scope_declare_name_direct(func_scope, pname, def_val, 0);
                lr_free_value(interp->ctx, def_val);
            }
        }
    }

    /* Evaluate the body */
    LRValue result = LR_VALUE_UNDEFINED;

    /* body was already extracted above for cache lookup, reuse it here. */
    if (body && !is_generator) {
        BCProgram *body_prog = bc_get_or_compile_func(func_node);
        if (body_prog) {
            result = bc_execute(body_prog, interp->ctx);
        } else {
            result = interp_eval_node(interp, body);
        }

        /* Check for return statement */
        if (interp->has_returned) {
            if (result.tag != LR_TYPE_UNDEFINED) {
                /* Free the block result if we already have a return value */
                /* Actually, the return value is already set in interp->return_value */
                /* But we need to handle the case where the body evaluation returned something */
            }
            /* The return value is in interp->return_value */
            /* But we need to be careful about which value to use */
        }
    }

    /* Get the actual return value */
    if (interp->has_returned) {
        result = lr_dup_value(interp->ctx, interp->return_value);
    }

    /* Generator: build the generator object with the body AST
     * stored for lazy incremental evaluation. */
    if (is_generator) {
        if (interp->error_flag) {
            lr_free_value(interp->ctx, interp->gen_items);
            lr_free_value(interp->ctx, result);
            result = LR_VALUE_UNDEFINED;
        } else {
            result = gen_build_object(interp, body, interp->current_scope,
                                      interp->gen_items);
            /* gen_build_object dups the array; release our reference */
            lr_free_value(interp->ctx, interp->gen_items);
        }
    }
    if (is_generator) {
        interp->gen_active = saved_gen_active;
        interp->gen_items = saved_gen_items;
        interp->gen_count = saved_gen_count;
    }

    /* Async: wrap the outcome in a Promise. Throws become rejections. */
    if (is_async && !is_generator) {
        LRValue p = lr_new_promise(interp->ctx);
        /* Give the bare promise its prototype so .then/.catch work */
        {
            LRValue g = lr_get_global_object(interp->ctx);
            LRValue ctor = lr_get_property_str(interp->ctx, g, "Promise");
            if (lr_is_object(ctor)) {
                LRValue proto = lr_get_property_str(interp->ctx, ctor, "prototype");
                if (lr_is_object(proto)) lr_set_prototype(interp->ctx, p, proto);
                lr_free_value(interp->ctx, proto);
            }
            lr_free_value(interp->ctx, ctor);
            lr_free_value(interp->ctx, g);
        }
        if (interp->error_flag) {
            LRValue reason;
            if (interp->exception_pending &&
                interp->exception_value.tag != LR_TYPE_EXCEPTION &&
                interp->exception_value.tag != LR_TYPE_UNDEFINED) {
                reason = lr_dup_value(interp->ctx, interp->exception_value);
            } else {
                reason = lr_new_string(interp->ctx, interp->error_message);
            }
            lr_promise_reject_internal(interp->ctx, p, reason);
            lr_free_value(interp->ctx, reason);
            /* The async function absorbs the throw into the rejection */
            interp->error_flag = 0;
            if (interp->exception_pending) {
                lr_free_value(interp->ctx, interp->exception_value);
                interp->exception_value = LR_VALUE_UNDEFINED;
                interp->exception_pending = 0;
            }
            interp->error_message[0] = '\0';
        } else {
            lr_promise_resolve_internal(interp->ctx, p, result);
        }
        lr_free_value(interp->ctx, result);
        result = p;
    }

    /* Pop any scopes left by early exits, then the function scope itself,
     * and restore the caller's scope (may differ from func_scope->parent
     * when a closure scope was used). */
    while (interp->current_scope && interp->current_scope != func_scope)
        interp_pop_scope(interp);
    /* No cache invalidation needed here: interp_pop_scope already handles
     * invalidation when scopes are popped, and the cache key includes the
     * scope pointer so entries for different scopes don't collide.
     * Keeping the cache warm across function calls improves performance
     * for repeated lookups of the same variables. */
    interp->current_scope = saved.current_scope;
    scope_release(func_scope, interp->ctx);

    /* Restore interpreter state from saved struct */
    interp->depth--;
    interp->break_target = saved.break_target;
    interp->continue_target = saved.continue_target;
    interp->pending_label = saved.pending_label;
    interp->return_target = saved.return_target;
    interp->has_returned = saved.has_returned;
    /* Don't restore return_value if we're inside a return (it propagates) */
    if (!saved.has_returned) {
        lr_free_value(interp->ctx, interp->return_value);
        interp->return_value = LR_VALUE_UNDEFINED;
    }
    interp->return_value = saved.return_value;
    /* Errors raised inside the function propagate to the caller;
     * otherwise restore the caller's error state */
    if (!interp->error_flag) {
        interp->error_flag = saved.error_flag;
        if (need_restore_error) {
            memcpy(interp->error_message, saved_error_message, 256);
        }
    }

    /* Pop call frame */
    lr_pop_call_frame(interp->ctx);

    return result;
}

/* Call a class as a constructor body:
 * 1. implicit super(...) when derived and no explicit constructor
 * 2. instance field initialization
 * 3. explicit constructor body (if any) */
static LRValue interp_call_class_function(Interpreter *interp, ASTNode *class_node,
                                          LRValue this_val, int argc, LRValue *argv)
{
    /* Take the class's closure scope; re-injected for the ctor body below */
    InterpScope *cls_closure = (InterpScope *)interp->pending_closure;
    interp->pending_closure = NULL;

    LRContext *ctx = interp->ctx;
    int nmethods = class_node->u.class_decl.nmethods;
    ASTNode **methods = class_node->u.class_decl.methods;

    /* Find the constructor: it may appear anywhere in the member list.
     * Instance fields (e.g. `#p = 1;` or `x = 1;`) are parsed as
     * AST_PROPERTY members and can precede the constructor, so a scan is
     * required — assuming methods[0] is the ctor silently drops the
     * constructor body (this.a = v never runs) when a field comes first. */
    ASTNode *ctor_ast = NULL;
    for (int i = 0; i < nmethods; i++) {
        ASTNode *m = methods[i];
        if (m && m->type == AST_FUNC_EXPR &&
            !m->u.func.is_static &&
            !m->u.func.is_getter &&
            !m->u.func.is_setter &&
            m->u.func.name &&
            strcmp(m->u.func.name, "constructor") == 0) {
            ctor_ast = m;
            break;
        }
    }

    /* Check for instance fields (independent of whether a ctor exists:
     * fields are initialized before the ctor body runs, so a class with an
     * explicit ctor must still initialize its fields). */
    int has_fields = 0;
    for (int i = 0; i < nmethods; i++) {
        ASTNode *m = methods[i];
        if (m && m->type == AST_PROPERTY && !m->u.property.is_static) {
            has_fields = 1;
            break;
        }
    }

    /* Implicit super(...args) for derived classes without explicit ctor */
    if (!ctor_ast && class_node->u.class_decl.extends) {
        LRValue parent;
        ASTNode *ext = class_node->u.class_decl.extends;
        LRValue cached = extends_cache_get(ext);
        if (lr_is_object(cached)) {
            parent = cached;
            lr_dup_value(ctx, parent); /* keep our own ref */
        } else {
            parent = interp_eval_node(interp, ext);
            if (interp->error_flag) {
                lr_free_value(ctx, parent);
                return LR_VALUE_UNDEFINED;
            }
            if (lr_is_object(parent))
                extends_cache_set(ctx, ext, parent);
        }
        if (lr_is_object(parent)) {
            LRObject *po = (LRObject *)parent.u.ptr;
            LRValue r = LR_VALUE_UNDEFINED;
            if (po->type == LR_OBJ_FUNCTION && po->extra) {
                interp->pending_closure = po->def_scope;
                r = interp_invoke_function_ast(interp, (ASTNode *)po->extra,
                                               this_val, argc, argv);
            } else if (po->type == LR_OBJ_CFUNCTION) {
                r = lr_call(ctx, parent, this_val, argc, argv);
            }
            lr_free_value(ctx, r);
        }
        lr_free_value(ctx, parent);
        if (interp->error_flag) return LR_VALUE_UNDEFINED;
    }

    /* Merge field init + ctor into a single scope to avoid extra push/pop */
    if (ctor_ast) {
        /* Bind super references from cache (or resolve on miss) */
        if (class_node->u.class_decl.extends) {
            LRValue superctor, superproto;
            if (!extends_cache_get_super(ctx, class_node->u.class_decl.extends,
                                         &superctor, &superproto)) {
                /* Cache miss: resolve and populate */
                LRValue parent = interp_eval_node(interp, class_node->u.class_decl.extends);
                if (interp->error_flag) {
                    interp->error_flag = 0;
                    lr_free_value(ctx, parent);
                } else if (lr_is_object(parent)) {
                    superctor = parent;
                    superproto = lr_get_property_str(ctx, parent, "prototype");
                    extends_cache_set_super(ctx, class_node->u.class_decl.extends,
                                            superctor, superproto);
                    lr_free_value(ctx, superproto);
                    lr_free_value(ctx, parent);
                } else {
                    lr_free_value(ctx, parent);
                }
            }
            interp_push_scope(interp, 1);
            scope_declare_name_direct(interp->current_scope, "%superctor%", superctor, 1);
            scope_declare_name_direct(interp->current_scope, "%superproto%", superproto, 1);
            lr_free_value(ctx, superctor);
            lr_free_value(ctx, superproto);
        } else {
            interp_push_scope(interp, 1);
        }
        scope_declare_name(interp, "this", this_val, 1);

        /* Instance field initialization (only if no explicit constructor,
         * since explicit ctor handles fields itself per ES spec — actually
         * fields are initialized before ctor body runs). */
        if (has_fields) {
            for (int i = 0; i < nmethods; i++) {
                ASTNode *m = methods[i];
                if (!m || m->type != AST_PROPERTY || m->u.property.is_static) continue;
                LRValue v = m->u.property.val
                    ? interp_eval_node(interp, m->u.property.val)
                    : LR_VALUE_UNDEFINED;
                if (interp->error_flag) {
                    lr_free_value(ctx, v);
                    break;
                }
                if (m->u.property.key && m->u.property.key->type == AST_IDENTIFIER) {
                    lr_set_property_str(ctx, this_val,
                                        m->u.property.key->u.ident.name, v);
                } else if (m->u.property.key) {
                    LRValue kv = interp_eval_node(interp, m->u.property.key);
                    LRString *atom = lr_to_atom(ctx, kv);
                    lr_set_property(ctx, this_val, atom, v);
                    lr_free_value(ctx, kv);
                } else {
                    lr_free_value(ctx, v);
                }
            }
            if (interp->error_flag) {
                interp_pop_scope(interp);
                return LR_VALUE_UNDEFINED;
            }
        }

        interp->pending_closure = cls_closure;
        LRValue result = interp_call_function(interp, ctor_ast, this_val, argc, argv);
        interp_pop_scope(interp);
        return result;
    }

    /* No explicit constructor: fields-only path (single scope) */
    if (has_fields) {
        /* Bind super references from cache for field init scope */
        if (class_node->u.class_decl.extends) {
            LRValue superctor, superproto;
            if (!extends_cache_get_super(ctx, class_node->u.class_decl.extends,
                                         &superctor, &superproto)) {
                LRValue parent = interp_eval_node(interp, class_node->u.class_decl.extends);
                if (interp->error_flag) {
                    interp->error_flag = 0;
                    lr_free_value(ctx, parent);
                } else if (lr_is_object(parent)) {
                    superctor = parent;
                    superproto = lr_get_property_str(ctx, parent, "prototype");
                    extends_cache_set_super(ctx, class_node->u.class_decl.extends,
                                            superctor, superproto);
                    lr_free_value(ctx, superproto);
                    lr_free_value(ctx, parent);
                } else {
                    lr_free_value(ctx, parent);
                }
            }
            interp_push_scope(interp, 1);
            scope_declare_name_direct(interp->current_scope, "%superctor%", superctor, 1);
            scope_declare_name_direct(interp->current_scope, "%superproto%", superproto, 1);
            lr_free_value(ctx, superctor);
            lr_free_value(ctx, superproto);
        } else {
            interp_push_scope(interp, 1);
        }
        scope_declare_name(interp, "this", this_val, 1);
        for (int i = 0; i < nmethods; i++) {
            ASTNode *m = methods[i];
            if (!m || m->type != AST_PROPERTY || m->u.property.is_static) continue;
            LRValue v = m->u.property.val
                ? interp_eval_node(interp, m->u.property.val)
                : LR_VALUE_UNDEFINED;
            if (interp->error_flag) {
                lr_free_value(ctx, v);
                break;
            }
            if (m->u.property.key && m->u.property.key->type == AST_IDENTIFIER) {
                lr_set_property_str(ctx, this_val,
                                    m->u.property.key->u.ident.name, v);
            } else if (m->u.property.key) {
                LRValue kv = interp_eval_node(interp, m->u.property.key);
                LRString *atom = lr_to_atom(ctx, kv);
                lr_set_property(ctx, this_val, atom, v);
                lr_free_value(ctx, kv);
            } else {
                lr_free_value(ctx, v);
            }
        }
        interp_pop_scope(interp);
        if (interp->error_flag) return LR_VALUE_UNDEFINED;
    }

    return LR_VALUE_UNDEFINED;
}

/* Dispatch any callable AST node: function/arrow or class */
LRValue interp_invoke_function_ast(Interpreter *interp, ASTNode *ast,
                                          LRValue this_val, int argc, LRValue *argv)
{
    if (!ast) { interp->pending_closure = NULL; return LR_VALUE_UNDEFINED; }
    /* Detect derived-class constructors (AST_CLASS_DECL with extends) so we
     * can short-circuit to the bytecode VM instead of recursing. */
    if (ast->type == AST_CLASS_DECL) {
        return interp_call_class_function(interp, ast, this_val, argc, argv);
    }
    if (ast->type == AST_FUNC_EXPR || ast->type == AST_FUNC_DECL ||
        ast->type == AST_ARROW) {
        return interp_call_function(interp, ast, this_val, argc, argv);
    }
    interp->pending_closure = NULL;
    return LR_VALUE_UNDEFINED;
}

/* ── Statement Evaluators ──────────────────────────────────────────────── */

static LRValue eval_block(Interpreter *interp, ASTNode *node)
{
    /* Blocks create a new scope for let/const */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;
    int count = node->u.list.count;
    ASTNode **items = node->u.list.items;

    for (int i = 0; i < count; i++) {
        if (interp->break_target || interp->continue_target ||
            interp->has_returned || interp->error_flag) {
            break;
        }
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, items[i]);
        if (interp->error_flag) {
            break;
        }
        /* Lazy generator mode: pause after a yield has been captured.
         * eval_yield_expr sets yield_pending=1 when it appends a value. */
        if (interp->gen_lazy && interp->yield_pending) {
            interp->yield_pending = 0;
            interp_pop_scope(interp);
            return LR_VALUE_UNDEFINED;
        }
    }

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_if(Interpreter *interp, ASTNode *node)
{
    LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
    if (interp->error_flag) return cond;

    int truthy = lr_to_bool(interp->ctx, cond);
    lr_free_value(interp->ctx, cond);

    if (truthy) {
        return interp_eval_node(interp, node->u.if_stmt.body);
    } else if (node->u.if_stmt.else_body) {
        return interp_eval_node(interp, node->u.if_stmt.else_body);
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Labeled break/continue helpers ────────────────────────────────────
 * A pending break/continue with an empty label targets the innermost
 * loop; with a label it targets the loop tagged with that label (via
 * interp->pending_label, set by the AST_LABEL handler). Non-matching
 * flags propagate outward through enclosing loops. */

static int break_is_mine(Interpreter *interp, const char *label)
{
    if (!interp->break_target) return 0;
    if (interp->break_label[0] == '\0') return 1;
    return label && strcmp(interp->break_label, label) == 0;
}

static int continue_is_mine(Interpreter *interp, const char *label)
{
    if (!interp->continue_target) return 0;
    if (interp->continue_label[0] == '\0') return 1;
    return label && strcmp(interp->continue_label, label) == 0;
}

static void consume_break(Interpreter *interp)
{
    interp->break_target = 0;
    interp->break_label[0] = '\0';
}

static void consume_continue(Interpreter *interp)
{
    interp->continue_target = 0;
    interp->continue_label[0] = '\0';
}

static LRValue eval_for(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    /* Evaluate init */
    if (node->u.for_stmt.init) {
        LRValue init_val = interp_eval_node(interp, node->u.for_stmt.init);
        lr_free_value(interp->ctx, init_val);
        if (interp->error_flag) { interp_pop_scope(interp); return LR_VALUE_UNDEFINED; }
    }

    LRValue result = LR_VALUE_UNDEFINED;

    while (!interp->break_target && !interp->has_returned && !interp->error_flag) {
        /* Test */
        if (node->u.for_stmt.test) {
            LRValue test = interp_eval_node(interp, node->u.for_stmt.test);
            int truthy = lr_to_bool(interp->ctx, test);
            lr_free_value(interp->ctx, test);
            if (!truthy) break;
        }

        /* Body */
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.for_stmt.body);
        if (interp->break_target) {
            if (break_is_mine(interp, my_label)) consume_break(interp);
            break;   /* consumed here, or propagates to an outer loop */
        }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;   /* labeled continue for an outer loop */
        }

        /* Update */
        if (node->u.for_stmt.update) {
            LRValue update = interp_eval_node(interp, node->u.for_stmt.update);
            lr_free_value(interp->ctx, update);
            if (interp->error_flag) break;
        }
    }

    interp_pop_scope(interp);
    return result;
}

static LRValue eval_for_in(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    /* Evaluate the source collection */
    LRValue source = interp_eval_node(interp, node->u.for_in.source);
    if (interp->error_flag) return source;

    /* Get enumerable property names */
    LRPropertyEnum *props = NULL;
    uint32_t nprops = 0;
    lr_get_own_property_names(interp->ctx, &props, &nprops, source, 0);

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;

    for (uint32_t i = 0; i < nprops; i++) {
        if (interp->break_target || interp->has_returned || interp->error_flag) break;

        const char *prop_name = lr_atom_to_cstring(interp->ctx, props[i].atom);

        /* Assign property name to the loop variable */
        LRValue prop_val = lr_new_string(interp->ctx, prop_name);
        lr_free_cstring(interp->ctx, prop_name);

        ASTNode *each = node->u.for_in.each;
        if (each) {
            if (each->type == AST_VAR_DECL) {
                /* var x in obj */
                if (each->u.var_decl.nvars > 0) {
                    ASTNode *declarator = each->u.var_decl.vars[0];
                    if (declarator && declarator->type == AST_VAR_DECLARATOR) {
                        ASTNode *var = declarator->u.declarator.var;
                        if (var && var->type == AST_IDENTIFIER) {
                            scope_declare_name(interp, var->u.ident.name, prop_val, 0);
                        }
                    }
                }
            } else if (each->type == AST_IDENTIFIER) {
                scope_set_name(interp, each->u.ident.name, prop_val);
            } else if (each->type == AST_ASSIGN) {
                /* Handle for (x in obj) - x is already assigned */
                /* This is for the case where for_in.each is the expression statement */
            }
        }

        lr_free_value(interp->ctx, prop_val);

        /* Execute body */
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.for_in.body);
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;   /* labeled continue for an outer loop */
        }
    }

    if (break_is_mine(interp, my_label)) consume_break(interp);

    lr_free_property_enum(interp->ctx, props, nprops);
    lr_free_value(interp->ctx, source);

    interp_pop_scope(interp);
    return result;
}

static void for_of_assign_var(Interpreter *interp, ASTNode *each, LRValue item)
{
    if (!each) return;
    if (each->type == AST_VAR_DECL) {
        if (each->u.var_decl.nvars > 0) {
            ASTNode *decl = each->u.var_decl.vars[0];
            if (decl && decl->type == AST_VAR_DECLARATOR) {
                ASTNode *var = decl->u.declarator.var;
                if (var && var->type == AST_IDENTIFIER)
                    scope_declare_name(interp, var->u.ident.name, item, 0);
                else if (var && (var->type == AST_PATTERN || var->type == AST_ARRAY ||
                                 var->type == AST_OBJECT))
                    eval_pattern(interp, var, item);
            }
        }
    } else if (each->type == AST_IDENTIFIER) {
        scope_set_name(interp, each->u.ident.name, item);
    } else if (each->type == AST_PATTERN || each->type == AST_ARRAY ||
               each->type == AST_OBJECT) {
        eval_pattern(interp, each, item);
    } else if (each->type == AST_ASSIGN) {
        ASTNode *target = each->u.assign.target;
        if (target->type == AST_IDENTIFIER) {
            scope_set_name(interp, target->u.ident.name, item);
        } else if (target->type == AST_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                const char *prop = target->u.member.prop->u.ident.name;
                lr_set_property_str(interp->ctx, obj, prop, lr_dup_value(interp->ctx, item));
            }
            lr_free_value(interp->ctx, obj);
        } else if (target->type == AST_COMPUTED_MEMBER) {
            LRValue obj = interp_eval_node(interp, target->u.member.obj);
            if (!interp->error_flag) {
                LRValue prop = interp_eval_node(interp, target->u.member.prop);
                if (!interp->error_flag) {
                    LRString *atom = lr_to_atom(interp->ctx, prop);
                    lr_set_property(interp->ctx, obj, atom, lr_dup_value(interp->ctx, item));
                }
                lr_free_value(interp->ctx, prop);
            }
            lr_free_value(interp->ctx, obj);
        } else {
            eval_pattern(interp, target, item);
        }
    }
}

static LRValue eval_for_of(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    /* Evaluate the source iterable */
    LRValue source = interp_eval_node(interp, node->u.for_of.source);
    if (interp->error_flag) return source;

    /* Create a scope for the loop variable */
    interp_push_scope(interp, 0);

    LRValue result = LR_VALUE_UNDEFINED;
    ASTNode *each = node->u.for_of.each;

    if (lr_is_string(source)) {
        const char *s = lr_to_cstring(interp->ctx, source);
        size_t slen = s ? strlen(s) : 0;
        char buf[8];
        for (size_t i = 0; i < slen; i++) {
            if (interp->break_target || interp->has_returned || interp->error_flag) break;
            /* UTF-8 best-effort: emit one byte as a string (ASCII-safe). */
            buf[0] = s[i]; buf[1] = '\0';
            LRValue item = lr_new_string(interp->ctx, buf);
            for_of_assign_var(interp, each, item);
            lr_free_value(interp->ctx, item);

            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = interp_eval_node(interp, node->u.for_of.body);
            if (interp->continue_target) {
                if (continue_is_mine(interp, my_label)) consume_continue(interp);
                else break;
            }
        }
        lr_free_cstring(interp->ctx, s);
    } else if (lr_is_array(interp->ctx, source)) {
        int32_t len = 0;
        LRValue len_val = lr_get_property_str(interp->ctx, source, "length");
        lr_to_int32(interp->ctx, &len, len_val);
        lr_free_value(interp->ctx, len_val);

        for (int32_t i = 0; i < len; i++) {
            if (interp->break_target || interp->has_returned || interp->error_flag) break;

            LRValue item = lr_get_property_uint32(interp->ctx, source, i);
            for_of_assign_var(interp, each, item);
            lr_free_value(interp->ctx, item);

            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = interp_eval_node(interp, node->u.for_of.body);
            if (interp->continue_target) {
                if (continue_is_mine(interp, my_label)) consume_continue(interp);
                else break;
            }
        }
    } else if (lr_is_object(source)) {
        /* Iterable protocol: const iter = source[Symbol.iterator](); then iter.next() */
        LRValue iter_fn = lr_get_property_str(interp->ctx, source, "Symbol.iterator");
        if (lr_is_function(interp->ctx, iter_fn) && iter_fn.tag == LR_TYPE_OBJECT) {
            LRValue iter_argv[1] = { source };
            LRValue iter = call_value_with_args(interp, NULL, iter_fn, source, 1, iter_argv);
            lr_free_value(interp->ctx, iter_fn);
            if (interp->error_flag) {
                lr_free_value(interp->ctx, iter);
                lr_free_value(interp->ctx, source);
                interp_pop_scope(interp);
                return result;
            }
            if (lr_is_object(iter)) {
                LRValue next_fn = lr_get_property_str(interp->ctx, iter, "next");
                while (!interp->break_target && !interp->has_returned && !interp->error_flag) {
                    LRValue nr = call_value_with_args(interp, NULL, next_fn, iter, 0, NULL);
                    if (interp->error_flag) { lr_free_value(interp->ctx, nr); break; }
                    LRValue done = lr_get_property_str(interp->ctx, nr, "done");
                    int is_done = lr_to_bool(interp->ctx, done);
                    lr_free_value(interp->ctx, done);
                    if (is_done) { lr_free_value(interp->ctx, nr); break; }
                    LRValue value = lr_get_property_str(interp->ctx, nr, "value");
                    for_of_assign_var(interp, each, value);
                    lr_free_value(interp->ctx, value);
                    lr_free_value(interp->ctx, nr);

                    if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
                    result = interp_eval_node(interp, node->u.for_of.body);
                    if (interp->continue_target) {
                        if (continue_is_mine(interp, my_label)) consume_continue(interp);
                        else break;
                    }
                }
                lr_free_value(interp->ctx, next_fn);
            }
            lr_free_value(interp->ctx, iter);
        } else {
            lr_free_value(interp->ctx, iter_fn);
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "%s is not iterable", "value");
            interp->exception_value = LR_VALUE_UNDEFINED;
            interp->error_flag = 1;
        }
    } else {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s is not iterable", "value");
        interp->exception_value = LR_VALUE_UNDEFINED;
        interp->error_flag = 1;
    }

    if (break_is_mine(interp, my_label)) consume_break(interp);

    lr_free_value(interp->ctx, source);

    interp_pop_scope(interp);
    return result;
}

/* with (obj) body — pushes the object's properties as a temporary scope so
 * that unqualified identifiers in the body resolve against the object first,
 * then fall through to the outer scope chain. */
static LRValue eval_with(Interpreter *interp, ASTNode *node)
{
    LRContext *ctx = interp->ctx;
    LRValue obj = interp_eval_node(interp, node->u.with_stmt.obj);
    if (interp->error_flag) return obj;

    if (obj.tag != LR_TYPE_OBJECT) {
        lr_free_value(ctx, obj);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "with statement requires an object");
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }

    /* Push a scope that will hold the object's enumerable own properties.
     * We enumerate the object and declare each key as a var in the
     * inner scope so that identifier resolution finds them first. */
    interp_push_scope(interp, 0);

    LRPropertyEnum *props = NULL;
    uint32_t nprops = 0;
    lr_get_own_property_names(ctx, &props, &nprops, obj,
                              JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);

    for (uint32_t i = 0; i < nprops; i++) {
        if (!props[i].atom) continue;
        LRValue pv = lr_get_property(ctx, obj, props[i].atom);
        scope_declare_name(interp, props[i].atom->str, pv, 1 /* let-like */);
        lr_free_value(ctx, pv);
    }
    if (props) lr_free_property_enum(ctx, props, nprops);

    LRValue result = interp_eval_node(interp, node->u.with_stmt.body);

    interp_pop_scope(interp);
    lr_free_value(ctx, obj);
    return result;
}

static LRValue eval_while(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    LRValue result = LR_VALUE_UNDEFINED;

    while (!interp->break_target && !interp->has_returned && !interp->error_flag) {
        LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
        int truthy = lr_to_bool(interp->ctx, cond);
        lr_free_value(interp->ctx, cond);

        if (!truthy) break;

        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.if_stmt.body);
        if (interp->break_target) {
            if (break_is_mine(interp, my_label)) consume_break(interp);
            break;
        }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;
        }
    }

    return result;
}

static LRValue eval_do_while(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    LRValue result = LR_VALUE_UNDEFINED;

    do {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, node->u.if_stmt.body);
        if (interp->break_target) {
            if (break_is_mine(interp, my_label)) consume_break(interp);
            break;
        }
        if (interp->has_returned || interp->error_flag) break;
        if (interp->continue_target) {
            if (continue_is_mine(interp, my_label)) consume_continue(interp);
            else break;
        }

        LRValue cond = interp_eval_node(interp, node->u.if_stmt.cond);
        int truthy = lr_to_bool(interp->ctx, cond);
        lr_free_value(interp->ctx, cond);
        if (!truthy) break;
    } while (!interp->break_target && !interp->has_returned && !interp->error_flag);

    return result;
}

static LRValue eval_switch(Interpreter *interp, ASTNode *node)
{
    const char *my_label = interp->pending_label;
    interp->pending_label = NULL;

    LRValue test = interp_eval_node(interp, node->u.switch_stmt.test);
    if (interp->error_flag) return test;

    int ncases = node->u.switch_stmt.ncases;
    ASTNode **cases = node->u.switch_stmt.cases;
    int matched = 0;
    LRValue result = LR_VALUE_UNDEFINED;

    for (int i = 0; i < ncases; i++) {
        ASTNode *case_node = cases[i];
        if (case_node->type == AST_DEFAULT) {
            matched = 1;
        } else if (case_node->type == AST_CASE) {
            if (!matched) {
                LRValue case_test = interp_eval_node(interp, case_node->u.if_stmt.cond);
                if (interp->error_flag) { lr_free_value(interp->ctx, test); return case_test; }
                matched = strict_eq(test, case_test);
                lr_free_value(interp->ctx, case_test);
            }
        }

        if (matched) {
            /* Execute case body */
            ASTNode *body = case_node->u.if_stmt.body;
            if (body && body->type == AST_BLOCK) {
                for (int j = 0; j < body->u.list.count; j++) {
                    if (interp->break_target || interp->has_returned || interp->error_flag) break;
                    if (result.tag != LR_TYPE_UNDEFINED) {
                        lr_free_value(interp->ctx, result);
                    }
                    result = interp_eval_node(interp, body->u.list.items[j]);
                }
            }
            if (interp->break_target) {
                if (break_is_mine(interp, my_label)) consume_break(interp);
                break;
            }
            if (interp->has_returned || interp->error_flag) break;
        }
    }

    lr_free_value(interp->ctx, test);
    return result;
}

static LRValue eval_break(Interpreter *interp, ASTNode *node)
{
    interp->break_label[0] = '\0';
    if (node && node->u.break_stmt.label &&
        node->u.break_stmt.label->type == AST_IDENTIFIER &&
        node->u.break_stmt.label->u.ident.name &&
        node->u.break_stmt.label->u.ident.name[0]) {
        snprintf(interp->break_label, sizeof(interp->break_label), "%s",
                 node->u.break_stmt.label->u.ident.name);
    }
    interp->break_target = 1;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_continue(Interpreter *interp, ASTNode *node)
{
    interp->continue_label[0] = '\0';
    if (node && node->u.continue_stmt.label &&
        node->u.continue_stmt.label->type == AST_IDENTIFIER &&
        node->u.continue_stmt.label->u.ident.name &&
        node->u.continue_stmt.label->u.ident.name[0]) {
        snprintf(interp->continue_label, sizeof(interp->continue_label), "%s",
                 node->u.continue_stmt.label->u.ident.name);
    }
    interp->continue_target = 1;
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_return(Interpreter *interp, ASTNode *node)
{
    interp->has_returned = 1;
    if (node->u.return_stmt.arg) {
        interp->return_value = interp_eval_node(interp, node->u.return_stmt.arg);
    } else {
        interp->return_value = LR_VALUE_UNDEFINED;
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_throw(Interpreter *interp, ASTNode *node)
{
    LRValue arg = interp_eval_node(interp, node->u.throw_stmt.arg);
    if (interp->error_flag) return arg;

    /* Set exception state */
    interp->exception_pending = 1;
    interp->exception_value = lr_dup_value(interp->ctx, arg);
    const char *str = lr_to_cstring(interp->ctx, arg);
    snprintf(interp->error_message, sizeof(interp->error_message), "%s", str);
    lr_free_cstring(interp->ctx, str);
    interp->error_flag = 1;
    lr_free_value(interp->ctx, arg);
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_try(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;

    /* Save exception state */
    int had_exception = interp->exception_pending;
    LRValue saved_exception = interp->exception_value;
    char saved_err_msg[512];
    memcpy(saved_err_msg, interp->error_message, 512);

    /* Clear exception state for the try block */
    interp->exception_pending = 0;
    interp->error_flag = 0;

    /* Pre-declare the catch parameter in the function/global scope so the
     * runtime scope slot layout is deterministic and independent of whether
     * the try body actually throws.  scope_declare_name hoists let/const to
     * the function scope (this engine treats them like var), so without this
     * pre-declaration the catch binding would only be added when an exception
     * is caught — shifting every slot of variables declared after this
     * try/catch statement and corrupting them (a later `let a = 1` would read
     * the Error value stored in the catch parameter's slot).  The catch
     * handler below then updates this pre-declared binding with the thrown
     * value. */
    if (node->u.try_stmt.catch_body && node->u.try_stmt.catch_var) {
        LRValue undef = LR_VALUE_UNDEFINED;
        scope_declare_name(interp, node->u.try_stmt.catch_var, undef, 1);
    }

    /* Execute try body */
    result = interp_eval_node(interp, node->u.try_stmt.body);

    int caught = 0;
    LRValue catch_result = LR_VALUE_UNDEFINED;

    /* Handle catch */
    if (interp->error_flag && node->u.try_stmt.catch_body) {
        caught = 1;
        interp->error_flag = 0;

        /* Push scope for catch variable */
        interp_push_scope(interp, 0);
        if (node->u.try_stmt.catch_var) {
            LRContext *ctx = interp->ctx;
            LRValue global = lr_get_global_object(ctx);
            LRValue exc_val = LR_VALUE_UNDEFINED;

            if (interp->exception_pending) {
                /* Engine exception (tag == LR_TYPE_EXCEPTION) or user-thrown value */
                if (JS_IsException(interp->exception_value)) {
                    /* Engine marker — create a proper Error object with the stored message */
                    lr_free_value(interp->ctx, interp->exception_value);
                    interp->exception_value = LR_VALUE_UNDEFINED;
                    /* Create an object with Error.prototype as its prototype */
                    JSValue err_obj = JS_NewObject(ctx);
                    JSValue err_ctor = JS_GetPropertyStr(ctx, global, "Error");
                    JSValue err_proto = JS_GetPropertyStr(ctx, err_ctor, "prototype");
                    if (JS_IsObject(err_proto)) {
                        LRObject *obj_ptr = (LRObject *)err_obj.u.ptr;
                        lr_free_value(ctx, obj_ptr->proto);
                        obj_ptr->proto = lr_dup_value(ctx, err_proto);
                    }
                    JS_FreeValue(ctx, err_proto);
                    JS_FreeValue(ctx, err_ctor);
                    JSValue msg = JS_NewString(ctx, interp->error_message);
                    exc_val = lr_error_constructor(ctx, err_obj, 1, &msg);
                    JS_FreeValue(ctx, msg);
                } else {
                    /* User-thrown value — use directly */
                    exc_val = lr_dup_value(interp->ctx, interp->exception_value);
                }
            } else if (interp->error_message[0]) {
                /* error_flag set but no pending exception — create an Error from the error message */
                JSValue err_obj = JS_NewObject(ctx);
                JSValue err_ctor = JS_GetPropertyStr(ctx, global, "Error");
                JSValue err_proto = JS_GetPropertyStr(ctx, err_ctor, "prototype");
                if (JS_IsObject(err_proto)) {
                    LRObject *obj_ptr = (LRObject *)err_obj.u.ptr;
                    lr_free_value(ctx, obj_ptr->proto);
                    obj_ptr->proto = lr_dup_value(ctx, err_proto);
                }
                JS_FreeValue(ctx, err_proto);
                JS_FreeValue(ctx, err_ctor);
                JSValue msg = JS_NewString(ctx, interp->error_message);
                exc_val = lr_error_constructor(ctx, err_obj, 1, &msg);
                JS_FreeValue(ctx, msg);
            }

            lr_free_value(ctx, global);
            scope_declare_name(interp, node->u.try_stmt.catch_var, exc_val, 1);
            lr_free_value(interp->ctx, exc_val);
        }

        interp->exception_pending = 0;
        catch_result = interp_eval_node(interp, node->u.try_stmt.catch_body);
        interp_pop_scope(interp);

        if (interp->error_flag) {
            /* Exception in catch block - propagate */
            lr_free_value(interp->ctx, result);
            result = catch_result;
            goto finally_check;
        }
    }

    /* If we caught, use catch result instead of try result */
    if (caught) {
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = catch_result;
    }

finally_check:
    /* Execute finally block (always).  The finally body is a Block whose
     * statement loop stops as soon as interp->has_returned is set, so if the
     * try/catch already executed `return`, a `return` inside finally would be
     * skipped (JS requires finally's return to override the try/catch one).
     * Temporarily clear has_returned while running finally; if finally itself
     * returns/throws it wins, otherwise we restore the try/catch return state. */
    if (node->u.try_stmt.finally_body) {
        int    saved_fin_hr  = interp->has_returned;
        LRValue saved_fin_rv = interp->return_value;
        interp->has_returned = 0;

        LRValue finally_result = interp_eval_node(interp, node->u.try_stmt.finally_body);

        if (interp->error_flag) {
            /* Exception in finally - propagate it */
            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = finally_result;
            /* release the try/catch return value that eval_return left behind */
            if (saved_fin_hr) lr_free_value(interp->ctx, saved_fin_rv);
        } else if (interp->has_returned) {
            /* finally returned - overrides the try/catch return */
            if (result.tag != LR_TYPE_UNDEFINED) lr_free_value(interp->ctx, result);
            result = LR_VALUE_UNDEFINED;
            lr_free_value(interp->ctx, finally_result);
            /* eval_return in finally overwrote interp->return_value without
             * freeing the try/catch return value — release it here */
            lr_free_value(interp->ctx, saved_fin_rv);
        } else {
            /* finally completed normally - restore try/catch return state */
            interp->has_returned = saved_fin_hr;
            if (saved_fin_hr)
                interp->return_value = saved_fin_rv;
            if (!caught) {
                /* If try succeeded and no catch, restore original result */
                /* But if we had a pending exception that wasn't caught, restore it */
                if (had_exception && !node->u.try_stmt.catch_body) {
                    interp->exception_pending = 1;
                    interp->exception_value = lr_dup_value(interp->ctx, saved_exception);
                    interp->error_flag = 1;
                }
            }
            lr_free_value(interp->ctx, finally_result);
        }
    }

    /* If we didn't catch and there's an error, propagate it */
    if (!caught && interp->error_flag) {
        return result;
    }

    /* Clear exception if we caught it */
    if (caught && node->u.try_stmt.catch_body) {
        interp->exception_pending = 0;
        interp->error_flag = 0;
        if (interp->exception_value.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, interp->exception_value);
            interp->exception_value = LR_VALUE_UNDEFINED;
        }
    }

    return result;
}

/* Evaluate a single variable declarator (used by both eval_var_decl and
 * the AST_VAR_DECLARATOR case in interp_eval_stmt for the bytecode path). */
static LRValue eval_var_declarator(Interpreter *interp, ASTNode *declarator,
                                   int kind)
{
    if (!declarator || declarator->type != AST_VAR_DECLARATOR)
        return LR_VALUE_UNDEFINED;

    ASTNode *var_node = declarator->u.declarator.var;
    ASTNode *init_node = declarator->u.declarator.init;

    if (var_node->type == AST_IDENTIFIER) {
        const char *name = var_node->u.ident.name;
        if (init_node) {
            LRValue val = interp_eval_node(interp, init_node);
            if (interp->error_flag) return val;
            scope_declare_name(interp, name, val, kind);
            lr_free_value(interp->ctx, val);
        } else {
            scope_declare_name(interp, name, LR_VALUE_UNDEFINED, kind);
        }
    } else if (var_node->type == AST_PATTERN) {
        if (init_node) {
            LRValue val = interp_eval_node(interp, init_node);
            if (interp->error_flag) return val;
            eval_pattern(interp, var_node, val);
            lr_free_value(interp->ctx, val);
        }
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_var_decl(Interpreter *interp, ASTNode *node)
{
    /* Determine declaration kind from token type */
    int kind = 0; /* 0=var, 1=let, 2=const */
    switch (node->token.type) {
    case TOK_LET:   kind = 1; break;
    case TOK_CONST: kind = 2; break;
    default:        kind = 0; break;
    }

    int nvars = node->u.var_decl.nvars;
    ASTNode **vars = node->u.var_decl.vars;

    for (int i = 0; i < nvars; i++) {
        LRValue r = eval_var_declarator(interp, vars[i], kind);
        if (interp->error_flag) return r;
    }

    return LR_VALUE_UNDEFINED;
}

static LRValue eval_func_decl(Interpreter *interp, ASTNode *node)
{
    const char *name = node->u.func.name;
    if (name) {
        /* Create function object and declare it */
        LRValue func_obj = eval_func_expr(interp, node);
        /* Function declarations are hoisted - use var-like behavior */
        scope_declare_name(interp, name, func_obj, 0);
        lr_free_value(interp->ctx, func_obj);
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_class_decl(Interpreter *interp, ASTNode *node)
{
    LRValue class_obj = eval_class_expr(interp, node);
    const char *name = node->u.class_decl.name;
    if (name) {
        scope_declare_name(interp, name, class_obj, 1); /* let-like */
    }
    /* Return the class object so class *expressions*
     * (var X = class {...}) evaluate to the constructor. */
    return class_obj;
}

/* Collect bound identifier names from a binding pattern node (used by
 * `export var/let/const <pattern>` to learn which names to export).
 * Appends borrowed char* pointers (owned by the AST, not copied) into the
 * supplied growable array. */
static void collect_export_names(ASTNode *binding, char ***pnames, int *pn, int *pcap)
{
    if (!binding) return;
    switch (binding->type) {
    case AST_IDENTIFIER:
        if (binding->u.ident.name) {
            if (*pn >= *pcap) {
                *pcap = *pcap ? *pcap * 2 : 4;
                *pnames = (char **)realloc(*pnames, *pcap * sizeof(char *));
            }
            (*pnames)[(*pn)++] = binding->u.ident.name;
        }
        break;
    case AST_REST:
        collect_export_names(binding->u.rest_elem.arg, pnames, pn, pcap);
        break;
    case AST_DEFAULT_VALUE:
        collect_export_names(binding->u.default_val.left, pnames, pn, pcap);
        break;
    case AST_PATTERN:
        if (binding->u.pattern_array.is_object) {
            for (int i = 0; i < binding->u.pattern_object.nprops; i++) {
                ASTNode *p = binding->u.pattern_object.props[i];
                if (!p) continue;
                ASTNode *target = (p->type == AST_PROPERTY && p->u.property.val)
                                    ? p->u.property.val : p;
                collect_export_names(target, pnames, pn, pcap);
            }
        } else {
            for (int i = 0; i < binding->u.pattern_array.nelem; i++)
                collect_export_names(binding->u.pattern_array.elements[i], pnames, pn, pcap);
        }
        break;
    case AST_PROPERTY:
        collect_export_names(binding->u.property.val ? binding->u.property.val
                                                       : binding->u.property.key,
                             pnames, pn, pcap);
        break;
    default:
        break;
    }
}

/* Resolve a module specifier to its namespace object via the runtime's module
 * loader. Returns an LRValue wrapping the namespace (borrowed reference) and,
 * on success, sets *out_mod to the module definition. On failure it records an
 * exception on the interpreter and returns an undefined value with *out_mod
 * left NULL. */
static LRValue eval_resolve_module(Interpreter *interp, const char *spec,
                                   JSModuleDef **out_mod)
{
    *out_mod = NULL;
    if (!spec) return LR_VALUE_UNDEFINED;

    char *normalized = NULL;
    if (interp->ctx->rt->module_normalize_func)
        normalized = interp->ctx->rt->module_normalize_func(interp->ctx, NULL, spec, NULL);
    const char *load_name = normalized ? normalized : spec;

    JSModuleDef *mod = NULL;
    if (interp->ctx->rt->module_loader_func)
        mod = interp->ctx->rt->module_loader_func(interp->ctx, load_name, NULL);
    if (normalized) free(normalized);

    if (!mod || !mod->obj) {
        LRValue err = JS_ThrowReferenceError(interp->ctx, "Cannot load module '%s'", spec);
        interp->error_flag = 1;
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, err);
        lr_free_value(interp->ctx, err);
        return LR_VALUE_UNDEFINED;
    }

    *out_mod = mod;
    LRValue ns;
    ns.tag = LR_TYPE_OBJECT;
    ns.u.ptr = mod->obj;
    return ns;
}

static LRValue eval_import(Interpreter *interp, ASTNode *node)
{
    ASTNode *src_node = node->u.import_decl.source;
    if (!src_node || src_node->type != AST_LITERAL) {
        /* import with no source, or a bare specifier */
        return LR_VALUE_UNDEFINED;
    }
    const char *spec = src_node->u.string.str;

    JSModuleDef *mod = NULL;
    LRValue ns = eval_resolve_module(interp, spec, &mod);
    if (!mod) return LR_VALUE_UNDEFINED;

    int nspec = node->u.import_decl.nspec;
    ASTNode **specifiers = node->u.import_decl.specifiers;
    for (int i = 0; i < nspec; i++) {
        ASTNode *spec_node = specifiers[i];
        if (!spec_node) continue;
        if (spec_node->type == AST_IMPORT_SPECIFIER) {
            const char *local_name =
                (spec_node->u.import_spec.local
                 && spec_node->u.import_spec.local->type == AST_IDENTIFIER)
                    ? spec_node->u.import_spec.local->u.ident.name : NULL;
            const char *export_name = spec_node->u.import_spec.is_default
                ? "default" : spec_node->u.import_spec.name;
            if (!local_name) local_name = export_name;
            if (!export_name) continue;
            /* Read the binding from the module namespace and bind it locally.
             * lr_get_property_str returns a caller-owned value; scope_declare_name
             * takes its own copy, so we free our copy afterwards. */
            LRValue val = lr_get_property_str(interp->ctx, ns, export_name);
            scope_declare_name(interp, local_name, val, 1);
            lr_free_value(interp->ctx, val);
        } else if (spec_node->type == AST_IMPORT_NAMESPACE) {
            ASTNode *local = spec_node->u.import_namespace.local;
            const char *local_name = (local && local->type == AST_IDENTIFIER)
                ? local->u.ident.name : NULL;
            if (local_name)
                scope_declare_name(interp, local_name, ns, 1);
        }
    }
    return LR_VALUE_UNDEFINED;
}

/* ── Module live-binding accessors (ES spec-compliant) ──────────────── */

/* Data captured by live-binding getter/setter so they can read/write the
 * interpreter scope directly instead of using a stale snapshot. */
typedef struct {
    InterpScope *scope;
    char         name[256];
} LiveBindData;

/* Forward declaration — used by live_bind_setter below */
static int scope_set_name_in_scope(InterpScope *scope, LRContext *ctx,
                                    const char *name, LRValue value);

/* Live-binding getter: searches the captured scope for `name` and returns
 * the current value. (scope is the source of truth; value can change.) */
static LRValue live_bind_getter_cfunc(LRContext *ctx, LRValue this_val,
                                       int argc, LRValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    LiveBindData *bd = NULL;
    if (ctx->current_func.tag == LR_TYPE_OBJECT) {
        LRObject *fo = (LRObject *)ctx->current_func.u.ptr;
        if (fo->type == LR_OBJ_CFUNCTION && fo->extra) {
            LRCFunction *cf = (LRCFunction *)fo->extra;
            bd = (LiveBindData *)cf->data;
        }
    }
    if (!bd || !bd->scope) return LR_VALUE_UNDEFINED;
    LRValue val = LR_VALUE_UNDEFINED;
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (interp && scope_lookup_internal(interp, bd->scope, bd->name, &val))
        return val;
    return LR_VALUE_UNDEFINED;
}

/* Live-binding setter: searches the captured scope for `name` and writes
 * the new value back. (Only installed for mutable var/let bindings.) */
static LRValue live_bind_setter_cfunc(LRContext *ctx, LRValue this_val,
                                       int argc, LRValue *argv)
{
    (void)this_val;
    LiveBindData *bd = NULL;
    if (ctx->current_func.tag == LR_TYPE_OBJECT) {
        LRObject *fo = (LRObject *)ctx->current_func.u.ptr;
        if (fo->type == LR_OBJ_CFUNCTION && fo->extra) {
            LRCFunction *cf = (LRCFunction *)fo->extra;
            bd = (LiveBindData *)cf->data;
        }
    }
    if (!bd || !bd->scope) return LR_VALUE_UNDEFINED;
    LRValue newval = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    if (scope_set_name_in_scope(bd->scope, ctx, bd->name, newval))
        return LR_VALUE_UNDEFINED;
    return LR_VALUE_UNDEFINED;
}

/* Install a live binding on the module namespace object as a getter/setter
 * accessor that aliases the named slot in the given scope. */
static void lr_export_live_binding(LRContext *ctx, LRValue ns_val,
                                    InterpScope *scope, const char *name,
                                    int mutable)
{
    LiveBindData *bd = (LiveBindData *)calloc(1, sizeof(LiveBindData));
    if (!bd) return;
    bd->scope = scope;
    strncpy(bd->name, name ? name : "", sizeof(bd->name) - 1);

    /* Create getter. Store closure data in the C function's data slot
     * (LRCFunction.data / data_free) — the same pattern used by
     * promise resolve/reject and finalization jobs. */
    LRValue getter = lr_new_cfunction(ctx, live_bind_getter_cfunc, name, 0);
    if (getter.tag == LR_TYPE_OBJECT) {
        LRObject *gobj = (LRObject *)getter.u.ptr;
        if (gobj->type == LR_OBJ_CFUNCTION && gobj->extra) {
            LRCFunction *gcf = (LRCFunction *)gobj->extra;
            gcf->data = bd;
            gcf->data_free = free;
        }
    }

    /* Setter only for mutable bindings. */
    LRValue setter = LR_VALUE_UNDEFINED;
    if (mutable) {
        setter = lr_new_cfunction(ctx, live_bind_setter_cfunc, name, 0);
        if (setter.tag == LR_TYPE_OBJECT) {
            LRObject *sobj = (LRObject *)setter.u.ptr;
            if (sobj->type == LR_OBJ_CFUNCTION && sobj->extra) {
                LRCFunction *scf = (LRCFunction *)sobj->extra;
                scf->data = bd;
                /* Don't set data_free — getter already owns bd */
            }
        }
    }

    lr_set_accessor_property_str(ctx, ns_val, name, getter, setter);

    /* accessor took ownership of getter (+ setter when present);
     * only free the unused setter stub. */
    if (setter.tag != LR_TYPE_OBJECT && setter.tag != LR_TYPE_UNDEFINED)
        lr_free_value(ctx, setter);
}

/* Like scope_set_name but searches a specific scope (not interp->current_scope). */
static int scope_set_name_in_scope(InterpScope *scope, LRContext *ctx,
                                    const char *name, LRValue value)
{
    while (scope) {
        for (int i = 0; i < scope->count; i++) {
            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                lr_free_value(ctx, scope->values[i]);
                scope->values[i] = lr_dup_value(ctx, value);
                return 1;
            }
        }
        scope = scope->parent;
    }
    return 0;
}

/* ── Export statement evaluator ──────────────────────────────────────── */

static LRValue eval_export(Interpreter *interp, ASTNode *node)
{
    LRObject *ns = interp->module_ns;
    int nspec = node->u.export_decl.nspec;
    ASTNode **specifiers = node->u.export_decl.specifiers;
    ASTNode *src_node = node->u.export_decl.source;

    /* Re-export from another module? */
    JSModuleDef *re_mod = NULL;
    LRValue re_ns = LR_VALUE_UNDEFINED;
    if (src_node && src_node->type == AST_LITERAL) {
        re_ns = eval_resolve_module(interp, src_node->u.string.str, &re_mod);
        if (!re_mod) return LR_VALUE_UNDEFINED; /* exception already recorded */
    }

    LRValue ns_val;
    ns_val.tag = (ns ? LR_TYPE_OBJECT : LR_TYPE_UNDEFINED);
    ns_val.u.ptr = ns;

    for (int i = 0; i < nspec; i++) {
        ASTNode *spec = specifiers[i];
        if (!spec) continue;

        if (spec->type == AST_EXPORT_DEFAULT) {
            /* export default <expr> */
            LRValue val = interp_eval_node(interp, spec->u.export_default.value);
            if (ns) lr_set_property_str(interp->ctx, ns_val, "default", val); /* takes ownership */
            else lr_free_value(interp->ctx, val);
        } else if (spec->type == AST_EXPORT_NAMED) {
            /* export { local } or export { local as alias } [from "mod"] */
            const char *local_name = spec->u.export_spec.name;
            const char *exported_name =
                (spec->u.export_spec.exported
                 && spec->u.export_spec.exported->type == AST_IDENTIFIER)
                    ? spec->u.export_spec.exported->u.ident.name : local_name;
            if (!exported_name) exported_name = local_name;
            if (!exported_name) continue;

            LRValue val;
            if (re_mod) {
                /* Re-export: snapshot from source module (will become
                 * live once source modules use accessor exports too). */
                val = lr_get_property_str(interp->ctx, re_ns, local_name ? local_name : "");
                if (ns) lr_set_property_str(interp->ctx, ns_val, exported_name, val);
                else lr_free_value(interp->ctx, val);
            } else if (ns) {
                /* Local export: live bidirectional binding via
                 * getter/setter accessor on the namespace object. */
                lr_export_live_binding(interp->ctx, ns_val,
                    interp->current_scope, exported_name, 1 /* mutable */);
            }
        } else if (spec->type == AST_EXPORT_ALL) {
            /* export * from "module" */
            if (re_mod) {
                LRPropertyEnum *tab = NULL;
                uint32_t plen = 0;
                lr_get_own_property_names(interp->ctx, &tab, &plen, re_ns, JS_GPN_STRING_MASK);
                for (uint32_t k = 0; k < plen; k++) {
                    const char *pname = lr_atom_to_cstring(interp->ctx, tab[k].atom);
                    if (pname && strcmp(pname, "default") != 0) {
                        LRValue val = lr_get_property_str(interp->ctx, re_ns, pname);
                        if (ns) lr_set_property_str(interp->ctx, ns_val, pname, val); /* takes ownership */
                        else lr_free_value(interp->ctx, val);
                    }
                    lr_free_cstring(interp->ctx, pname);
                }
                lr_free_property_enum(interp->ctx, tab, plen);
            }
        } else if (spec->type == AST_VAR_DECL) {
            /* export var/let/const ... : evaluate (binds names), then
             * install live bidirectional accessor bindings. */
            LRValue val = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, val);
            char **names = NULL;
            int n = 0, cap = 0;
            for (int v = 0; v < spec->u.var_decl.nvars; v++) {
                ASTNode *decl = spec->u.var_decl.vars[v];
                if (decl && decl->type == AST_VAR_DECLARATOR)
                    collect_export_names(decl->u.declarator.var, &names, &n, &cap);
            }
            if (ns) {
                for (int v = 0; v < n; v++)
                    lr_export_live_binding(interp->ctx, ns_val,
                        interp->current_scope, names[v], 1 /* mutable */);
            }
            free(names);
        } else if (spec->type == AST_FUNC_DECL) {
            const char *fname = spec->u.func.name;
            LRValue decl = interp_eval_node(interp, spec);
            lr_free_value(interp->ctx, decl);
            if (fname && ns)
                lr_export_live_binding(interp->ctx, ns_val,
                    interp->current_scope, fname, 1);
        } else if (spec->type == AST_CLASS_DECL) {
            const char *cname = spec->u.class_decl.name;
            LRValue val = interp_eval_node(interp, spec);
            if (cname && ns) {
                lr_export_live_binding(interp->ctx, ns_val,
                    interp->current_scope, cname, 1);
            }
            lr_free_value(interp->ctx, val);
        }
    }
    return LR_VALUE_UNDEFINED;
}

/* ── Main Evaluation Dispatch ──────────────────────────────────────────── */

static LRValue interp_eval_stmt(Interpreter *interp, ASTNode *node)
{
    if (!node) return LR_VALUE_UNDEFINED;

    /* Execution timeout: check every 1024 statements if a limit is set.
     * On timeout, set error_flag so the interpreter unwinds cleanly
     * instead of being killed by the OS. */
    if (interp->timeout_ms > 0) {
        if (++interp->stmt_counter >= 1024) {
            interp->stmt_counter = 0;
            clock_t now = clock();
            /* clock_t may be unsigned (macOS: unsigned long); compare in a
             * common signed 64-bit domain to avoid -Wsign-compare and any
             * implicit conversion surprise. */
            long long elapsed = (long long)((now * 1000) / CLOCKS_PER_SEC);
            if (elapsed >= (long long)interp->timeout_ms) {
                snprintf(interp->error_message, sizeof(interp->error_message),
                    "Execution timeout exceeded (%d ms)", interp->timeout_ms);
                interp->error_flag = 1;
                return LR_VALUE_UNDEFINED;
            }
        }
    }

    switch (node->type) {
    case AST_BLOCK:       return eval_block(interp, node);
    case AST_IF:          return eval_if(interp, node);
    case AST_FOR:         return eval_for(interp, node);
    case AST_FOR_IN:      return eval_for_in(interp, node);
    case AST_FOR_OF:      return eval_for_of(interp, node);
    case AST_WITH:        return eval_with(interp, node);
    case AST_WHILE:       return eval_while(interp, node);
    case AST_DO_WHILE:    return eval_do_while(interp, node);
    case AST_SWITCH:      return eval_switch(interp, node);
    case AST_BREAK:       return eval_break(interp, node);
    case AST_CONTINUE:    return eval_continue(interp, node);
    case AST_RETURN:      return eval_return(interp, node);
    case AST_THROW:       return eval_throw(interp, node);
    case AST_TRY:         return eval_try(interp, node);
    case AST_EXPR_STMT: {
        if (node->u.expr_stmt.expr) {
            LRValue val = interp_eval_node(interp, node->u.expr_stmt.expr);
            return val;
        }
        return LR_VALUE_UNDEFINED;
    }
    case AST_VAR_DECL:    return eval_var_decl(interp, node);
    case AST_FUNC_DECL:   return eval_func_decl(interp, node);
    case AST_CLASS_DECL:  return eval_class_decl(interp, node);
    case AST_IMPORT:      return eval_import(interp, node);
    case AST_EXPORT:      return eval_export(interp, node);
    case AST_LABEL: {
        ASTNode *stmt = node->u.label_stmt.stmt;
        const char *lname = (node->u.label_stmt.label &&
                             node->u.label_stmt.label->type == AST_IDENTIFIER)
                            ? node->u.label_stmt.label->u.ident.name : NULL;
        /* Hand the label to a directly-following loop/switch */
        const char *saved_pending = interp->pending_label;
        int is_breakable = stmt &&
            (stmt->type == AST_FOR || stmt->type == AST_WHILE ||
             stmt->type == AST_DO_WHILE || stmt->type == AST_FOR_IN ||
             stmt->type == AST_FOR_OF || stmt->type == AST_SWITCH);
        if (is_breakable) interp->pending_label = lname;
        LRValue r = interp_eval_node(interp, stmt);
        if (is_breakable) interp->pending_label = saved_pending;
        /* Labeled non-loop statement (labeled block): 'break lbl' exits here.
         * Also catches a propagating labeled break for a nested label. */
        if (interp->break_target && lname && interp->break_label[0] &&
            strcmp(interp->break_label, lname) == 0) {
            interp->break_target = 0;
            interp->break_label[0] = '\0';
        }
        return r;
    }
    case AST_DEBUGGER:
        /* Debugger statement - no-op */
        return LR_VALUE_UNDEFINED;
    case AST_VAR_DECLARATOR:
        return eval_var_declarator(interp, node, 0);
    default:
        /* Guard: statement-only nodes dispatched by eval_statement_dispatch
         * must not be re-routed through interp_eval_node, which would loop. */
        if (node->type <= AST_IMPORT_NAMESPACE &&
            eval_handlers[node->type] == eval_statement_dispatch)
            return LR_VALUE_UNDEFINED;
        return interp_eval_node(interp, node);
    }
}

/* ── Direct Threading: Function Pointer Table ────────────────────────── */

/* Each AST node type maps to a handler function.
 * This replaces the big switch statement, reducing branch mispredictions. */

/* Initialize the dispatch table (called once at first use) */
static void init_eval_handlers(void)
{
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    /* Statements all go through the statement dispatch */
    for (int i = 0; i <= AST_IMPORT_NAMESPACE; i++) {
        eval_handlers[i] = eval_statement_dispatch;
    }

    /* Expression handlers (override statement dispatch) */
    eval_handlers[AST_LITERAL]         = eval_literal;
    eval_handlers[AST_IDENTIFIER]      = eval_identifier;
    eval_handlers[AST_THIS]            = eval_this_expr;
    eval_handlers[AST_SUPER]           = eval_super_expr;
    eval_handlers[AST_BINARY]          = eval_binary;
    eval_handlers[AST_UNARY]           = eval_unary;
    eval_handlers[AST_CONDITIONAL]     = eval_conditional;
    eval_handlers[AST_ASSIGN]          = eval_assign;
    eval_handlers[AST_MEMBER]          = eval_member;
    eval_handlers[AST_COMPUTED_MEMBER] = eval_computed_member;
    eval_handlers[AST_OPTIONAL_MEMBER] = eval_member;
    eval_handlers[AST_CALL]            = eval_call;
    eval_handlers[AST_OPTIONAL_CALL]   = eval_call;
    eval_handlers[AST_NEW]             = eval_new;
    eval_handlers[AST_ARRAY]           = eval_array;
    eval_handlers[AST_OBJECT]          = eval_object;
    eval_handlers[AST_FUNC_EXPR]       = eval_func_expr;
    eval_handlers[AST_ARROW]           = eval_arrow;
    eval_handlers[AST_TEMPLATE]        = eval_template;
    eval_handlers[AST_TAGGED_TEMPLATE] = eval_tagged_template;
    eval_handlers[AST_SEQUENCE]        = eval_sequence;
    eval_handlers[AST_SPREAD_ELEMENT]  = eval_spread;
    eval_handlers[AST_AWAIT]           = eval_await;
    eval_handlers[AST_YIELD]           = eval_yield_expr;
    eval_handlers[AST_PATTERN]         = eval_pattern_expr;
    eval_handlers[AST_REST]            = eval_rest_expr;
    eval_handlers[AST_DEFAULT_VALUE]   = eval_default_val;
    eval_handlers[AST_PROPERTY]        = eval_property;
    eval_handlers[AST_PROGRAM]         = eval_program;
}

/* ── Expression/Statement Dispatch ─────────────────────────────────────── */

static LRValue eval_statement_dispatch(Interpreter *interp, ASTNode *node)
{
    return interp_eval_stmt(interp, node);
}

static LRValue eval_this_expr(Interpreter *interp, ASTNode *node)
{
    (void)node;
    LRValue this_val;
    if (scope_lookup_internal(interp, interp->current_scope, "this", &this_val)) {
        return this_val;
    }
    return lr_get_global_object(interp->ctx);
}

static LRValue eval_super_expr(Interpreter *interp, ASTNode *node)
{
    (void)node;
    /* 'super.prop' reads resolve against the parent prototype */
    LRValue sproto;
    if (scope_lookup_internal(interp, interp->current_scope, "%superproto%", &sproto)) {
        return sproto;
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_tagged_template(Interpreter *interp, ASTNode *node)
{
    ASTNode *tag_node = node->u.template_lit.tag;
    if (!tag_node) return LR_VALUE_UNDEFINED;
    LRContext *ctx = interp->ctx;

    LRValue tag = interp_eval_node(interp, tag_node);
    if (interp->error_flag) {
        lr_free_value(ctx, tag);
        return LR_VALUE_UNDEFINED;
    }

    int nparts = node->u.template_lit.nparts;
    int nexp   = node->u.template_lit.nexp;
    ASTNode **exprs = node->u.template_lit.exprs;

    /* Build the 'strings' array (cooked) and its '.raw' array (verbatim). */
    LRValue strings = lr_new_array(ctx);
    LRValue raw_arr = lr_new_array(ctx);
    for (int i = 0; i < nparts; i++) {
        const char *verbatim = node->u.template_lit.parts[i]
                                 ? node->u.template_lit.parts[i] : "";
        char *cooked = template_unescape(verbatim);
        lr_set_property_uint32(ctx, strings, i,
                               lr_new_string(ctx, cooked ? cooked : ""));
        lr_set_property_uint32(ctx, raw_arr, i, lr_new_string(ctx, verbatim));
        if (cooked) free(cooked);
    }
    lr_set_property_str(ctx, strings, "length", lr_new_int32(ctx, nparts));
    lr_set_property_str(ctx, raw_arr, "length", lr_new_int32(ctx, nparts));
    lr_set_property_str(ctx, strings, "raw", raw_arr); /* raw_arr ownership -> strings */

    /* Build arguments: [strings, ...exprValues] */
    int argc = nexp + 1;
    LRValue *argv = (LRValue *)calloc(argc, sizeof(LRValue));
    argv[0] = strings; /* strings is consumed via argv[0] below */
    for (int i = 0; i < nexp; i++) {
        argv[i + 1] = interp_eval_node(interp, exprs[i]);
        if (interp->error_flag) {
            for (int j = 0; j <= i; j++) lr_free_value(ctx, argv[j]);
            free(argv);
            lr_free_value(ctx, tag);
            return LR_VALUE_UNDEFINED;
        }
    }

    LRValue result = call_value_with_args(interp, tag_node, tag,
                                          LR_VALUE_UNDEFINED, argc, argv);

    for (int i = 0; i < argc; i++) lr_free_value(ctx, argv[i]);
    free(argv);
    lr_free_value(ctx, tag);
    return result;
}

static LRValue eval_yield_expr(Interpreter *interp, ASTNode *node)
{
    if (!interp->gen_active) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "yield is only valid inside a generator function");
        interp->exception_value = LR_VALUE_UNDEFINED;
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }
    LRValue arg = node->u.yield_expr.arg
        ? interp_eval_node(interp, node->u.yield_expr.arg)
        : LR_VALUE_UNDEFINED;
    if (interp->error_flag) return arg;

    if (node->u.yield_expr.is_delegate) {
        gen_delegate(interp, arg);
    } else {
        gen_append(interp, arg);
    }
    lr_free_value(interp->ctx, arg);

    /* Lazy mode: signal eval_block to pause after this yield */
    if (interp->gen_lazy) {
        interp->yield_pending = 1;
    }
    /* Eager generators cannot receive values from next(v): yield -> undefined */
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_pattern_expr(Interpreter *interp, ASTNode *node)
{
    if (!node) return LR_VALUE_UNDEFINED;
    if (node->u.pattern_array.is_object) {
        for (int i = 0; i < node->u.pattern_object.nprops; i++) {
            ASTNode *prop = node->u.pattern_object.props[i];
            if (prop && prop->type == AST_PROPERTY && prop->u.property.val) {
                LRValue v = interp_eval_node(interp, prop->u.property.val);
                if (interp->error_flag) { lr_free_value(interp->ctx, v); return LR_VALUE_UNDEFINED; }
                lr_free_value(interp->ctx, v);
            }
        }
    } else {
        for (int i = 0; i < node->u.pattern_array.nelem; i++) {
            ASTNode *elem = node->u.pattern_array.elements[i];
            if (elem) {
                LRValue v = interp_eval_node(interp, elem);
                if (interp->error_flag) { lr_free_value(interp->ctx, v); return LR_VALUE_UNDEFINED; }
                lr_free_value(interp->ctx, v);
            }
        }
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue eval_rest_expr(Interpreter *interp, ASTNode *node)
{
    if (!node || !node->u.rest_elem.arg)
        return LR_VALUE_UNDEFINED;
    return interp_eval_node(interp, node->u.rest_elem.arg);
}

static LRValue eval_default_val(Interpreter *interp, ASTNode *node)
{
    if (!node || !node->u.default_val.right)
        return LR_VALUE_UNDEFINED;
    return interp_eval_node(interp, node->u.default_val.right);
}

static LRValue eval_property(Interpreter *interp, ASTNode *node)
{
    if (!node || !node->u.property.val)
        return LR_VALUE_UNDEFINED;
    return interp_eval_node(interp, node->u.property.val);
}

static LRValue eval_program(Interpreter *interp, ASTNode *node)
{
    LRValue result = LR_VALUE_UNDEFINED;
    int count = node->u.list.count;
    ASTNode **items = node->u.list.items;
    for (int i = 0; i < count; i++) {
        if (interp->error_flag) break;
        if (interp->has_returned) break;
        if (result.tag != LR_TYPE_UNDEFINED) {
            lr_free_value(interp->ctx, result);
        }
        result = interp_eval_node(interp, items[i]);
    }
    return result;
}

/* ── Main Eval Dispatch ────────────────────────────────────────────────── */

static LRValue interp_eval_node(Interpreter *interp, ASTNode *node)
{
    if (!node) return LR_VALUE_UNDEFINED;
    if (interp->error_flag) return LR_VALUE_UNDEFINED;

    /* Initialize the dispatch table on first use */
    init_eval_handlers();

    /* Direct dispatch via function pointer table */
    if ((size_t)node->type <= AST_IMPORT_NAMESPACE && eval_handlers[node->type]) {
        return eval_handlers[node->type](interp, node);
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Public API ────────────────────────────────────────────────────────── */

LRValue interp_eval(Interpreter *interp, ASTNode *node)
{
    /* Try bytecode compilation for top-level programs for ~10-100x speedup */
    if (node && node->type == AST_PROGRAM) {
        BCProgram *prog = bc_get_or_compile_body(node);
        if (prog) {
            return bc_execute(prog, interp->ctx);
        }
    }
    return interp_eval_node(interp, node);
}

/* Callback for lr_call_direct: call a JS interpreter function from C builtins */
static LRValue interp_callback_call(LRContext *ctx, LRValue func,
                                     LRValue this_val, int argc, LRValue *argv)
{
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp) return LR_VALUE_UNDEFINED;

    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (obj->type == LR_OBJ_FUNCTION && obj->extra) {
        ASTNode *func_ast = (ASTNode *)obj->extra;
        /* Call the function/class body with its captured closure scope */
        interp->pending_closure = obj->def_scope;
        return interp_invoke_function_ast(interp, func_ast, this_val, argc, argv);
    }
    return LR_VALUE_UNDEFINED;
}

void interp_reattach(Interpreter *interp, LRContext *ctx)
{
    ctx->call_js_function = interp_callback_call;
    ctx->opaque_interp = interp;
}

void interp_init(Interpreter *interp, LRContext *ctx, int is_module)
{
    memset(interp, 0, sizeof(*interp));
    interp->ctx = ctx;
    interp->is_module = is_module;
    interp->filename = NULL;
    interp->import_meta = LR_VALUE_UNDEFINED;
    interp->timeout_ms = ctx->timeout_ms;
    interp->scope_cache_gen = 1; /* enable scope lookup cache */

    /* Set up the callback so C builtins can call JS functions */
    if (!ctx->call_js_function) {
        ctx->call_js_function = interp_callback_call;
    }
    ctx->opaque_interp = interp;

    /* Install the closure-scope release hook so lr_free_object can drop
     * captured scopes when function objects die */
    lr_closure_scope_release = interp_closure_release_hook;

    /* Create global scope */
    InterpScope *global = scope_new(NULL, 1, 1);
    global->mirror_globals = !is_module;   /* Script mode mirrors top-level
                                             * var/function onto the global object */
    interp->global_scope = global;
    interp->current_scope = global;

    /* Make 'globalThis' and 'global' refer to the global object */
    LRValue global_obj = lr_get_global_object(ctx);
    lr_set_property_str(ctx, global_obj, "globalThis", lr_dup_value(ctx, global_obj));
    lr_free_value(ctx, global_obj);

    /* Cache Math object reference for BC_CALL fast path */
    LRValue math_val = lr_get_property_str(ctx, lr_get_global_object(ctx), "Math");
    if (math_val.tag == LR_TYPE_OBJECT)
        interp->math_obj = (LRObject *)math_val.u.ptr;
    lr_free_value(ctx, math_val);
}

void interp_free(Interpreter *interp)
{
    if (!interp) return;

    /* Walk the current_scope chain and release it as one tree.
     * DO NOT detach parents — scope_release cascades up the chain safely
     * (the refcount sentinel at `scope->refcount < 0` prevents double-free
     * from recursive release triggered by lr_free_value of closures). */
    InterpScope *s = interp->current_scope;
    InterpScope *gs = interp->global_scope;
    interp->current_scope = NULL;
    interp->global_scope = NULL;
    if (s) {
        scope_release(s, interp->ctx);
    }

    /* Release the global scope only if it was NOT part of the current_scope
     * chain (e.g. if current_scope was already NULL due to an error path).
     * This avoids double-releasing the same scope when current_scope ==
     * global_scope, which would over-decrement the refcount and cause
     * the sentinel check to fire prematurely.  The caller (lr_free_context)
     * handles releasing the remaining closure-retained references. */
    if (gs && gs != s) {
        scope_release(gs, interp->ctx);
    }

    /* Free exception value */
    if (interp->exception_value.tag != LR_TYPE_UNDEFINED) {
        lr_free_value(interp->ctx, interp->exception_value);
        interp->exception_value = LR_VALUE_UNDEFINED;
    }
    /* Free return value */
    if (interp->return_value.tag != LR_TYPE_UNDEFINED) {
        lr_free_value(interp->ctx, interp->return_value);
        interp->return_value = LR_VALUE_UNDEFINED;
    }

    /* Clear context callback pointers since the interpreter is stack-allocated */
    if (interp->ctx) {
        interp->ctx->opaque_interp = NULL;
        interp->ctx->call_js_function = NULL;
    }

    /* Drain the scope pool to prevent ASAN leak reports at program exit.
     * The pool is thread-local; at interpreter shutdown all pooled scopes
     * must be freed.  Since the pool only holds packed_alloc scopes,
     * a simple free() suffices.  The pool holds scopes whose refcount
     * reached 0 and were "freed" via scope_pool_push. */
    interp_drain_scope_pool();
}

/* ═══════════════════════════════════════════════════════════════════════
   BYTECODE VM BRIDGE

   The stack-based bytecode VM in lr_bytecode.c shares this interpreter's
   scope chain, error/exception state and closure machinery. Everything the
   VM cannot (or must not) reimplement is routed through these functions so
   there is exactly one implementation of each JavaScript semantic rule.
   ═══════════════════════════════════════════════════════════════════════ */

LRValue interp_bc_eval_node(Interpreter *interp, ASTNode *node)
{
    if (!interp || !node) return LR_VALUE_UNDEFINED;
    return interp_eval_node(interp, node);
}

int interp_bc_load_var(Interpreter *interp, const char *name, LRValue *out)
{
    LRValue val;
    *out = LR_VALUE_UNDEFINED;
    if (!interp || !name) return 0;

    if (scope_lookup_internal(interp, interp->current_scope, name, &val)) {
        *out = val;
        if (lr_env_flag(&g_lr_env_debug_var, "LR_DEBUG_VAR")) fprintf(stderr, "[VAR] scope_lookup '%s' found tag=%d\n", name, val.tag);
        return 1;
    }

    if (lr_env_flag(&g_lr_env_debug_var, "LR_DEBUG_VAR")) fprintf(stderr, "[VAR] scope_lookup '%s' NOT found, trying global\n", name);
    LRValue global = lr_get_global_object(interp->ctx);
    val = lr_get_property_str(interp->ctx, global, name);
    lr_free_value(interp->ctx, global);
    if (!lr_is_undefined(val)) {
        *out = val;
        if (lr_env_flag(&g_lr_env_debug_var, "LR_DEBUG_VAR")) fprintf(stderr, "[VAR] global '%s' found tag=%d\n", name, val.tag);
        return 1;
    }
    lr_free_value(interp->ctx, val);

    if (lr_env_flag(&g_lr_env_debug_var, "LR_DEBUG_VAR")) fprintf(stderr, "[VAR] '%s' NOT FOUND anywhere\n", name);
    interp_raise_reference_error(interp, name);
    return 0;
}

int interp_bc_typeof_var(Interpreter *interp, const char *name, LRValue *out)
{
    LRValue val;
    *out = LR_VALUE_UNDEFINED;
    if (!interp || !name) return 0;

    if (scope_lookup_internal(interp, interp->current_scope, name, &val)) {
        *out = val;
        return 1;
    }
    LRValue global = lr_get_global_object(interp->ctx);
    val = lr_get_property_str(interp->ctx, global, name);
    lr_free_value(interp->ctx, global);
    if (!lr_is_undefined(val)) {
        *out = val;
        return 1;
    }
    lr_free_value(interp->ctx, val);
    return 0;   /* unresolvable: typeof yields "undefined", no throw */
}

void interp_bc_store_var(Interpreter *interp, const char *name, LRValue val)
{
    if (!interp || !name) return;
    if (!scope_set_name(interp, name, val)) {
        /* Assignment to a constant sets error_flag; the tree-walker clears
         * it and falls back to creating the binding — mirror that here. */
        interp->error_flag = 0;
        scope_declare_name(interp, name, val, 0);
    }
}

void interp_bc_declare_var(Interpreter *interp, const char *name,
                           LRValue val, int kind)
{
    if (!interp || !name) return;
    scope_declare_name(interp, name, val, kind);
}

LRValue interp_bc_call(Interpreter *interp, LRValue callee, LRValue this_val,
                       int argc, LRValue *argv)
{
    if (!interp) return LR_VALUE_UNDEFINED;
    /* Depth check: prevent C stack overflow from deep recursion in the
     * bytecode VM. The tree-walking interpreter path (interp_call_function)
     * has its own depth check, but the bytecode → bytecode path through
     * interp_bc_call → call_value_with_args → lr_call → bc_execute does not
     * go through that path, so we must track depth here as well. */
    if (interp->depth >= MAX_CALL_DEPTH) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "Maximum call stack size exceeded");
        interp->error_flag = 1;
        return LR_VALUE_UNDEFINED;
    }
    interp->depth++;
    if (!lr_is_function(interp->ctx, callee)) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "value is not a function");
        interp->error_flag = 1;
        interp->depth--;
        return LR_VALUE_UNDEFINED;
    }
    LRValue result = call_value_with_args(interp, NULL, callee, this_val, argc, argv);
    interp->depth--;
    return result;
}

LRValue interp_bc_construct(Interpreter *interp, LRValue callee,
                            int argc, LRValue *argv)
{
    if (!interp) return LR_VALUE_UNDEFINED;
    const char *ctor_name = "";
    lr_push_call_frame(interp->ctx, ctor_name, NULL, 0);
    LRValue result = lr_call_constructor(interp->ctx, callee, argc, argv);
    lr_pop_call_frame(interp->ctx);
    if (lr_is_exception(result)) {
        interp->exception_pending = 1;
        interp->exception_value = lr_dup_value(interp->ctx, result);
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "%s", lr_get_exception_str(interp->ctx));
        interp->error_flag = 1;
    }
    return result;
}

void interp_bc_throw(Interpreter *interp, LRValue value)
{
    if (!interp) return;
    interp->exception_pending = 1;
    interp->exception_value = lr_dup_value(interp->ctx, value);
    const char *str = lr_to_cstring(interp->ctx, value);
    snprintf(interp->error_message, sizeof(interp->error_message), "%s",
             str ? str : "uncaught exception");
    lr_free_cstring(interp->ctx, str);
    interp->error_flag = 1;
}

void interp_bc_push_scope(Interpreter *interp)
{
    if (interp) interp_push_scope(interp, 0);
}

void interp_bc_pop_scope(Interpreter *interp)
{
    if (interp) interp_pop_scope(interp);
}

char *interp_bc_cook_template(const char *raw)
{
    return template_unescape(raw ? raw : "");
}

/* Push `this` from scope chain — used by BC_PUSH_THIS opcode */
void interp_bc_push_this(Interpreter *interp, LRValue *out)
{
    if (!interp || !out) return;
    if (scope_lookup_internal(interp, interp->current_scope, "this", out)) {
        /* value set by scope_lookup_internal */
    } else {
        *out = LR_VALUE_UNDEFINED;
    }
}

