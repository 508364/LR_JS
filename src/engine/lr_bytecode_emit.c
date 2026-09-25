/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: emit
 */
#include "lr_bytecode.h"
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
   COMPILER STATE
   ======================================================================= */

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
    int         stack_depth;   /* current operand stack depth             */
    int         max_stack_depth; /* high-water mark for pre-allocation    */
    /* -- Local variable tracking (BC_LOAD_LOCAL/BC_STORE_LOCAL) -------- */
    uint16_t    local_count;        /* number of names in current scope   */
    const char *local_names[256];  /* name → slot index (slot == index)  */
    uint16_t    var_local_count;    /* local_count floor from var hoisting */
    /* -- Function-body slot layout --------------------------------------
     * For function bodies, local_names[0..local_base-1] are pre-populated
     * with "this" + parameter names, matching the runtime function scope
     * layout (slot 0 = "this", slot 1+i = param i, then locals).  This
     * makes slot == index a valid invariant and fixes locals colliding
     * with "this"/params.  prog->local_count still counts only the true
     * locals (local_count - local_base) so the no_scope fast path keeps
     * working for empty functions. */
    uint16_t    local_base;         /* pre-populated names ("this"+params) */
    int         disable_local_slots;/* 1 → always use name-based lookup    */
    /* -- Scope nesting for local tracking ------------------------------- */
    uint16_t    local_scope_stack[64]; /* local_count at each scope level */
    int         local_scope_depth;     /* current scope tracking depth    */
    /* -- Instruction boundary tracking ----------------------------------
     * Used to make the LOAD_PROP+ADD peephole fusion safe: `cur_inst_start`
     * is the byte offset of the opcode of the currently open instruction,
     * and inst_written/inst_total count how many of its bytes have been
     * emitted.  The fusion only rewrites code[cur_inst_start] when the
     * whole instruction really is a 5-byte BC_LOAD_PROP — never an operand
     * byte that merely equals the BC_LOAD_PROP opcode value.            */
    int         cur_inst_start;  /* opcode offset of current instruction */
    int         inst_written;    /* bytes emitted so far for it          */
    int         inst_total;      /* total expected instruction bytes     */
} BCComp;

/* -- Stack depth tracking ----------------------------------------------
 * Tracks the operand stack depth during compilation so bc_execute can
 * pre-allocate a sufficiently large stack, avoiding VM_GROW overhead.
 * UPDATE_STACK(delta) records the net effect of the emitted bytecode. */
#define UPDATE_STACK(delta) do {                                         \
        c->stack_depth += (delta);                                       \
        if (c->stack_depth > c->max_stack_depth)                         \
            c->max_stack_depth = c->stack_depth;                         \
    } while (0)

/* =======================================================================
   EMISSION
   ======================================================================= */

/* bc_op_total_len() is defined with the disassembler (it derives total
 * instruction length from bc_info's operand encoding); forward-declared
 * here so emit() can compute instruction boundaries during generation. */
int bc_op_total_len(uint8_t op);

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

void emit(BCComp *c, uint8_t byte)
{
    if (!c->ok) return;
    if (byte == BC_ADD) {
        /* Peephole: fuse "BC_LOAD_PROP slot,pool ; BC_ADD" into a single
         * BC_LOAD_PROP_ADD.  LOAD_PROP is 5 bytes (op + u16 slot + u16
         * pool); when the opcode just before the current end of stream is
         * BC_LOAD_PROP, the following ADD always consumes the property
         * value from the top of the operand stack, so the fused op
         * (which replaces the top-of-stack accumulator with acc+prop)
         * is semantically identical — and saves one dispatch per add on
         * hot loops like `s += o.a + o.b + ...`.
         *
         * CRITICAL: the candidate is verified through the instruction
         * boundary tracker (cur_inst_start/inst_written/inst_total).  We
         * only fuse when the *whole* previous instruction is a genuine
         * 5-byte BC_LOAD_PROP.  A naive `code[len-5] == BC_LOAD_PROP`
         * check can match an operand byte whose value equals the
         * BC_LOAD_PROP opcode (e.g. a CALL_METHOD's pool-index low byte
         * 70), silently corrupting the operand (70 → 71).
         *
         * NOTE: the ADD byte has NOT been written yet (we are inside
         * emit), so fusing means simply rewriting the 5-byte LOAD_PROP
         * into a 5-byte LOAD_PROP_ADD and returning without appending
         * anything. */
        int len = c->p->code_len;
        int start = c->cur_inst_start;
        if (start >= 0 && len - start == 5 && c->p->code[start] == BC_LOAD_PROP) {
            c->p->code[start] = BC_LOAD_PROP_ADD;
            /* LOAD_PROP(+1) + ADD(-1) net 0; the fused op replaces the
             * top of stack in place, so track the same net effect. */
            UPDATE_STACK(-1);
            return;
        }
    }
    /* Instruction boundary tracking: when the previous instruction is
     * complete, this byte begins a new opcode at the current end of
     * stream.  Otherwise it is an operand byte (e.g. DECLARE_VAR's u8
     * `kind`), which must not reset the instruction start. */
    if (c->inst_written >= c->inst_total) {
        c->cur_inst_start = c->p->code_len;
        c->inst_total = bc_op_total_len(byte);
        c->inst_written = 0;
    }
    c->inst_written++;
    bc_ensure(c, 1);
    if (!c->ok) return;
    c->p->code[c->p->code_len++] = byte;
}

void emit16(BCComp *c, int v)
{
    if (!c->ok) return;
    if (v < 0 || v > 0xFFFF) { c->ok = 0; return; }
    bc_ensure(c, 2);
    if (!c->ok) return;
    c->p->code[c->p->code_len++] = (uint8_t)(v & 0xFF);
    c->p->code[c->p->code_len++] = (uint8_t)((v >> 8) & 0xFF);
    c->inst_written += 2;
    if (c->inst_written > c->inst_total) c->inst_written = c->inst_total;
}

void emit32(BCComp *c, int32_t v)
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
    c->inst_written += 4;
    if (c->inst_written > c->inst_total) c->inst_written = c->inst_total;
}

int here(BCComp *c) { return c->p->code_len; }

/* Emit a jump with a placeholder target; returns the patch position. */
int emit_jump(BCComp *c, uint8_t op)
{
    emit(c, op);
    int pos = here(c);
    emit32(c, 0);
    return pos;
}

void patch_jump_to(BCComp *c, int pos, int target)
{
    if (!c->ok || pos < 0 || pos + 4 > c->p->code_len) return;
    int32_t rel = (int32_t)(target - (pos + 4));
    uint8_t *d = c->p->code + pos;
    d[0] = (uint8_t)(rel & 0xFF);
    d[1] = (uint8_t)((rel >> 8) & 0xFF);
    d[2] = (uint8_t)((rel >> 16) & 0xFF);
    d[3] = (uint8_t)((rel >> 24) & 0xFF);
}

void patch_here(BCComp *c, int pos) { patch_jump_to(c, pos, here(c)); }

/* =======================================================================
   CONSTANT POOL
   ======================================================================= */

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

int pool_add_f64(BCComp *c, double d)
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

int pool_add_str(BCComp *c, const char *s)
{
    if (!c->ok || !pool_reserve(c)) return 0;
    if (!s) s = "";
    BCProgram *p = c->p;
    int ret = -1;
    for (int i = 0; i < p->pool_count; i++)
        if (p->pool[i].kind == BC_POOL_STRING && strcmp(p->pool[i].u.str, s) == 0) {
            ret = i; break;
        }
    if (ret >= 0) return ret;
    int idx = p->pool_count++;
    p->pool[idx].kind = BC_POOL_STRING;
    p->pool[idx].u.str = bc_strdup(s);
    if (!p->pool[idx].u.str || idx > 0xFFFF) c->ok = 0;
    /* Precompute FNV-1a hash for Math fast-path dispatch */
    uint32_t h = 0;
    for (const char *p2 = s; *p2; p2++)
        h = (h << 5) - h + (uint8_t)*p2;
    p->pool[idx].str_hash = h;
    return idx;
}

int pool_add_node(BCComp *c, void *node)
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

/* =======================================================================
   ESCAPE ANALYSIS

   Before a subtree is delegated to the tree-walking interpreter we must
   be sure it cannot transfer control *out* of itself: a `break` bound to
   a VM-compiled loop, a `return`, a `yield`/`await` would be invisible to
   the VM. When that happens the whole unit bails out to the interpreter.
   ======================================================================= */

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
        /* NOTE: the parser stores while/do-while condition+body in the
         * if_stmt union fields (cond/body), NOT for_stmt (test/body). */
        return escapes(n->u.if_stmt.cond, in_loop, in_switch, in_label) ||
               escapes(n->u.if_stmt.body, 1, in_switch, in_label);
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

