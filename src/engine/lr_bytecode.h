/*
 * LR_JS — Bytecode VM (stack based) — Direct/Indirect Threaded Interpreter
 *
 * As of v0.1.1, this is the **sole** execution engine. The AST tree-walking
 * interpreter has been retired; every JavaScript construct is compiled to
 * a linear opcode stream and executed by this VM.
 *
 * Threading model:
 *   - Direct  threading (computed goto / labels-as-values)  → GCC, Clang
 *   - Indirect threading (switch-based)                      → MSVC, others
 *   Selection is automatic via the LR_THREADED_CODE preprocessor guard.
 *
 * The compiler covers ALL AST nodes. There is no longer a fallback to
 * AST evaluation (the `emit_eval` / `escapes` mechanism and the
 * BC_EVAL_NODE opcode are removed). Functions, classes, try/catch,
 * destructuring, and all other constructs have native bytecode lowering.
 */
#ifndef LR_BYTECODE_H
#define LR_BYTECODE_H

#include "lr_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ASTNode;

/* ── Opcodes ────────────────────────────────────────────────────────────
 * Operand encoding: u8 = 1 byte, u16 = 2 bytes LE, i32 = 4 bytes LE.
 * Jump operands are relative to the byte right after the operand.       */

typedef enum {
    BC_STOP = 0,          /*                    end of program            */
    BC_NOP,               /*                                              */

    /* Constants ------------------------------------------------------- */
    BC_PUSH_UNDEFINED,
    BC_PUSH_NULL,
    BC_PUSH_TRUE,
    BC_PUSH_FALSE,
    BC_PUSH_THIS,         /* push current `this` value                    */
    BC_PUSH_INT32,        /* i32                                          */
    BC_PUSH_FLOAT64,      /* u16 pool                                     */
    BC_PUSH_STRING,       /* u16 pool                                     */

    /* Stack shuffling -------------------------------------------------- */
    BC_POP,
    BC_DUP,
    BC_DUP2,              /* a b   -> a b a b                             */
    BC_SWAP,
    BC_ROT3,              /* a b c -> c a b                               */

    /* Variables -------------------------------------------------------- */
    BC_LOAD_VAR,          /* u16 pool(name)  -> value                     */
    BC_STORE_VAR,         /* u16 pool(name)  value ->                     */
    BC_DECLARE_VAR,       /* u16 pool(name), u8 kind   value ->           */
    BC_TYPEOF_VAR,        /* u16 pool(name)  -> string                    */

    /* Binary operators -------------------------------------------------- */
    BC_ADD, BC_SUB, BC_MUL, BC_DIV, BC_MOD, BC_POW,
    BC_LT, BC_GT, BC_LE, BC_GE,
    BC_EQ, BC_NE, BC_STRICT_EQ, BC_STRICT_NE,
    BC_SHL, BC_SHR, BC_SAR,
    BC_BIT_AND, BC_BIT_OR, BC_BIT_XOR,
    BC_IN, BC_INSTANCEOF,

    /* Unary operators --------------------------------------------------- */
    BC_NEG, BC_POS, BC_NOT, BC_BIT_NOT, BC_TYPEOF, BC_VOID,

    /* Control flow ------------------------------------------------------ */
    BC_JUMP,                  /* i32                                       */
    BC_JUMP_IF_FALSE,         /* i32   cond ->                             */
    BC_JUMP_IF_TRUE,          /* i32   cond ->                             */
    BC_JUMP_IF_FALSE_KEEP,    /* i32   value -> value (kept when falsy)    */
    BC_JUMP_IF_TRUE_KEEP,     /* i32   value -> value (kept when truthy)   */
    BC_JUMP_IF_NOT_NULLISH,   /* i32   value -> value (kept when defined)  */
    BC_LOOP_TICK,             /* loop back-edge: timeout / budget check    */

    /* Calls -------------------------------------------------------------- */
    BC_CALL,              /* u16 argc     callee args... -> result         */
    BC_CALL_METHOD,       /* u16 pool(name), u16 argc  obj args... -> res  */
    BC_CALL_ELEM,         /* u16 argc     obj key args... -> result        */
    BC_NEW,               /* u16 argc     callee args... -> result         */
    BC_RETURN,            /* value -> (returns from the program)           */

    /* Objects and arrays -------------------------------------------------- */
    BC_NEW_OBJECT,        /*              -> {}                            */
    BC_NEW_ARRAY,         /* u16 n        v0..vn-1 -> array                */
    BC_DEF_PROP,          /* u16 pool     obj value -> obj                 */
    BC_DEF_ELEM,          /*              obj key value -> obj             */
    BC_GET_PROP,          /* u16 pool     obj -> value                     */
    BC_SET_PROP,          /* u16 pool     obj value -> value               */
    BC_GET_ELEM,          /*              obj key -> value                 */
    BC_SET_ELEM,          /*              obj key value -> value           */
    BC_DELETE_PROP,       /* u16 pool     obj -> bool                      */
    BC_DELETE_ELEM,       /*              obj key -> bool                  */

    /* Iteration (for-of) --------------------------------------------------- */
    BC_ITER_INIT,         /*        iterable -> iter next_fn index         */
    BC_ITER_NEXT,         /* i32    (peeks 3 slots) -> value | jump when done */
    BC_ITER_CLOSE,        /*        iter next_fn index ->                  */

    /* Interpreter interoperability ----------------------------------------- */
    BC_SCOPE_ENTER,       /* push a lexical scope                          */
    BC_SCOPE_LEAVE,       /* pop a lexical scope                           */
    BC_EVAL_NODE,         /* u16 pool(node)  -> value (tree-walker)        */
    BC_EVAL_NODE_POP,     /* u16 pool(node)  ->        (tree-walker)       */
    BC_SET_RESULT,        /* value ->   (program completion value)         */
    BC_CLEAR_RESULT,      /*            completion value = undefined       */
    BC_THROW,             /* value ->   raise exception                    */

    BC_OPCODE_COUNT
} BCOpcode;

/* ── Constant pool ──────────────────────────────────────────────────── */

typedef enum {
    BC_POOL_INT32,
    BC_POOL_FLOAT64,
    BC_POOL_STRING,
    BC_POOL_NODE,        /* ASTNode* (interpreter fallback target) */
} BCPoolKind;

typedef struct {
    BCPoolKind   kind;
    union {
        int32_t  i32;
        double   f64;
        char    *str;
        void    *node;   /* ASTNode* — not serializable */
    } u;
} BCConst;

/* ── Compiled program ───────────────────────────────────────────────── */

typedef struct {
    uint8_t     *code;          /* opcode stream                          */
    int32_t      code_len;
    int32_t      code_cap;
    BCConst     *pool;          /* constant pool                          */
    int32_t      pool_count;
    int32_t      pool_cap;
    int32_t      max_stack;     /* computed high-water mark               */
    int32_t      node_refs;     /* number of AST references in the pool   */
    int32_t      compiled;      /* 1 = usable, 0 = compilation bailed out  */
    const char  *source;        /* debug hint (not owned)                 */
} BCProgram;

/* ── API ────────────────────────────────────────────────────────────── */

BCProgram *bc_new_program(void);
void       bc_free_program(BCProgram *prog);

/* Compile an AST unit. Returns 0 when the program can be executed by the
 * VM, -1 when the caller must use the tree-walking interpreter instead. */
int        bc_compile(BCProgram *prog, struct ASTNode *node, int is_module);

/* Execute a compiled program on ctx's current interpreter state.
 * Returns the completion value (caller frees). */
LRValue    bc_execute(BCProgram *prog, LRContext *ctx);

/* Serialization for the IOME586 archive. Programs that reference AST
 * nodes are still written (for statistics/cache validation) but flagged
 * as non-restorable; bc_deserialize then returns NULL so the caller
 * recompiles from the AST. */
uint8_t   *bc_serialize(BCProgram *prog, size_t *out_len);
BCProgram *bc_deserialize(const uint8_t *data, size_t len);

/* 1 when the program can be restored from a serialized image. */
int        bc_program_is_restorable(const BCProgram *prog);

/* Debug: human readable listing (caller frees). */
char      *bc_disassemble(BCProgram *prog);

#ifdef __cplusplus
}
#endif

#endif /* LR_BYTECODE_H */
