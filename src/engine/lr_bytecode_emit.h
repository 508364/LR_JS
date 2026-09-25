/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: emit
 */
#ifndef LR_BYTECODE_EMIT
#define LR_BYTECODE_EMIT

  #include "lr_engine.h"
  #include "lr_ast.h"
  #include "lr_bytecode.h"
  #include "lr_bytecode_cache.h"

#ifdef __cplusplus
}
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
 * pre-allocate a sufficiently large stack, avoiding VM_GROW overhead. */
#define UPDATE_STACK(delta) do {                                         \
        c->stack_depth += (delta);                                       \
        if (c->stack_depth > c->max_stack_depth)                         \
            c->max_stack_depth = c->stack_depth;                         \
    } while (0)

/* =======================================================================
   EMISSION (shared between emit.c and compile.c)
   ======================================================================= */

/* bc_op_total_len() is defined with the disassembler (it derives total
 * instruction length from bc_info's operand encoding); forward-declared
 * here so emit() can compute instruction boundaries during generation. */
int bc_op_total_len(uint8_t op);

void emit(BCComp *c, uint8_t byte);
void emit16(BCComp *c, int v);
void emit32(BCComp *c, int32_t v);
int here(BCComp *c);
int emit_jump(BCComp *c, uint8_t op);
void patch_jump_to(BCComp *c, int pos, int target);
void patch_here(BCComp *c, int pos);
int pool_add_f64(BCComp *c, double d);
int pool_add_str(BCComp *c, const char *s);
int pool_add_node(BCComp *c, void *node);

#endif /* LR_BYTECODE_EMIT */
