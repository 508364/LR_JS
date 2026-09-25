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
    BC_LOAD_LOCAL,        /* u16 slot        -> value  (direct slot)      */
    BC_STORE_LOCAL,       /* u16 slot        value -> (direct slot)       */
    BC_INC_LOCAL,         /* u16 slot        -> old_value (slot++, post)  */
    BC_INC_LOCAL_DISCARD, /* u16 slot        -> (slot++, no push)         */
    BC_INC_VAR,           /* u16 pool(name)  -> old_value (var++, post)   */
    BC_DECLARE_VAR,       /* u16 pool(name), u8 kind   value ->           */
    BC_TYPEOF_VAR,        /* u16 pool(name)  -> string                    */
    BC_ADD_SELF,          /* u16 slot        rhs -> (slot = slot + rhs)   */
    BC_MUL_SELF,          /* u16 slot        rhs -> (slot = slot * rhs)   */

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
    BC_JUMP_IF_LOCAL_LT_IMM,  /* u16 slot, i32 imm, i32 offset  (local < imm)  */
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
    BC_LOAD_PROP,         /* u16 slot, u16 pool  -> value  (fused load+get) */
    BC_LOAD_PROP_ADD,     /* u16 slot, u16 pool  -> (fused load+get+add with TOS) */
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

    /* Type conversion (inline fast paths for String/Number/Boolean) ---------- */
    BC_TO_STRING,         /* value -> string(value)  (inline type conversion) */
    BC_TO_NUMBER,         /* value -> number(value)  (inline type conversion) */
    BC_TO_BOOL,           /* value -> bool(value)    (inline type conversion) */

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
    uint32_t     str_hash;  /* FNV-1a hash of u.str (precomputed at compile time) */
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

    /* ── Persistent execution caches ─────────────────────────────────────
     * These caches persist across bc_execute() calls so that repeated
     * invocations of the same function body keep warm cache entries.
     * Allocated as a single block in bc_new_program; layout defined in
     * lr_bytecode.c to avoid exposing internal types in this header.    */
    void        *cache_data;    /* persistent cache block (or NULL)       */
    int32_t      cache_gen;     /* generation counter for cache validation */

    /* ── Inline-call optimization hints (set during compilation) ──────── */
    uint32_t     prog_id;       /* unique id (per-thread monotonic counter,
                                 * assigned in bc_new_program, never reused).
                                 * Keys IOME586 pure-function memo entries so
                                 * they survive BCProgram free/reuse.       */
    uint16_t     local_count;   /* number of local vars in this function  */
    uint8_t      is_pure;       /* 1 if this is a MEMOIZABLE pure function:
                                 * the body has NO external interference —
                                 * only slot-based param/local access plus
                                 * primitive arithmetic.  No name ops, no
                                 * this/arguments, no calls, no property
                                 * reads/writes, no closures, no exceptions.
                                 * Such a function's result depends only on
                                 * its primitive arguments, so it can be
                                 * memoized by (program, args) in the
                                 * IOME586 result cache and replayed directly
                                 * on a cache hit. Computed once at compile
                                 * time by scanning the emitted opcodes.   */
    uint8_t      uses_this;     /* 1 if function body references `this`  */
    uint8_t      uses_super;    /* 1 if function body references `super` */
    uint8_t      uses_name_ops; /* 1 if body uses name-based var ops
                                 * (BC_LOAD_VAR/STORE_VAR/DECLARE_VAR), so
                                 * the inline-call path must pre-populate
                                 * the variable cache.  0 for slot-safe
                                 * bodies that only use BC_LOAD_LOCAL.    */
    uint8_t      scans_arguments; /* 1 if the body references the
                                 * `arguments` object.  Computed once at
                                 * compile time so the hot inline-call path
                                 * avoids re-scanning the AST on every call. */
    uint8_t      can_inline;    /* 1 if this function is a candidate for the
                                 * fast inline-call path: every parameter is
                                 * a plain identifier (no destructuring /
                                 * defaults / rest), not a generator/async.
                                 * Computed once at compile time so the hot
                                 * BC_CALL handler skips the per-call AST
                                 * walk that validates parameter shape. */
    uint8_t      nparams;       /* number of parameters (same layout as the
                                 * runtime function scope: slot 0 = "this",
                                 * slots 1..nparams = params).  Cached here
                                 * so the inline-call path does not re-read
                                 * the AST on every call. */
    uint8_t      touches_eval_node; /* 1 if the body contains BC_EVAL_NODE /
                                 * BC_EVAL_NODE_POP (which delegate to the
                                 * tree-walker and can modify the loop/control
                                 * state on the Interpreter: break_target,
                                 * continue_target, return_target,
                                 * has_returned, return_value,
                                 * pending_label).  Computed once at compile
                                 * time so the inline-call frame save/restore
                                 * can skip those fields for pure-bytecode
                                 * callees, cutting per-call frame overhead. */
    /* ── IOME586 adaptive memoization state ─────────────────────────────
     * A pure callee starts in WARMUP: the first BC_MEMO_WARMUP calls skip
     * the cache probe entirely (counting calls), so one-off or
     * always-distinct argument streams never pay hash/compare cost.  It
     * then promotes to ACTIVE (probe on every call); a hit resets the
     * miss streak, while BC_MEMO_DISABLE_MISSES consecutive misses (args
     * never repeat) permanently DISABLES the probe for this program so a
     * memo-unfriendly workload degrades back to the plain inline-call
     * path.  The probe is therefore only active for functions that have
     * actually demonstrated repeatable arguments. */
    uint8_t      memo_state;      /* 0 = warmup, 1 = active, 2 = disabled */
    uint8_t      memo_warmup;     /* calls counted during warmup          */
    uint8_t      memo_miss_streak;/* consecutive misses while active      */
    uint8_t      _memo_pad;

    /* ── Type specialization hints (PGO-driven) ─────────────────────────
     * Tracks the dominant argument type signature observed at call sites.
     * Used by the JIT compiler to emit type-specialized code paths
     * (e.g. all-int32 vs mixed float64) without runtime type checks.
     * Updated by pgo_update() in the call path. */
    uint8_t      spec_int32_count; /* how many all-int32 calls seen       */
    uint8_t      spec_float64_count; /* how many all-float64 calls seen  */
    uint8_t      spec_mixed_count;   /* mixed-type calls                */
    uint8_t      spec_other_count;   /* calls with objects/strings    */
    /* 0=int32, 1=float64, 2=mixed, 3=other */
    uint8_t      spec_dominant;

    /* Cached AST pointers (extracted once at first call, reused on all subsequent
     * inline calls).  Avoids repeated AST walk/pointer extraction on every
     * inline call for frequently called pure functions (hot path). */
    ASTNode    *cached_body_ast;     /* function body AST node */
    ASTNode   **cached_params_ast;   /* parameter AST node array */

    /* ── JIT code cache (V8-inspired: O(1) entry lookup) ────────────────
     * When lr_jit_compile() succeeds, it stores the native entry point
     * here so subsequent calls skip the O(n) linked-list walk in
     * lr_jit_lookup().  The JIT code buffer is freed when the program is
     * destroyed.  Void pointer avoids circular include dependency. */
    void       *jit_entry;          /* native entry point (NULL if not compiled) */
    size_t      jit_code_size;      /* size of jit_entry buffer for free() */
    int         jit_skip;           /* 1 if JIT compilation failed — never retry */

    /* ── Function self-reference (for recursive JIT calls) ────────────
     * When bc_compile_func compiles a named function, the function's
     * own name is registered in local_names but NOT emitted as
     * BC_DECLARE_VAR.  Store the name+slot here so the MIR compiler
     * can map BC_LOAD_VAR of the function name to the correct slot,
     * enabling recursive call detection (is_recursive=1).               */
    const char *func_name;          /* function name (NULL if anonymous)  */
    int         func_name_slot;     /* scope slot for func_name (-1 if none)*/

    /* ── Reference counting ─────────────────────────────────────────────
     * A BCProgram can be shared by several owners: the eval unit that
     * created it AND the persistent-interp warm compiled-cache (which
     * aliases unit->bc_prog for repeated eval of the same source).  Each
     * shared owner must bc_retain_program(); bc_free_program() acts as a
     * release and frees the program only when the last owner lets go.
     * This prevents a double-free / heap-use-after-free when several
     * units sharing one program are torn down. */
    uint32_t     ref_count;       /* number of owning references         */
} BCProgram;

/* LRProgram is an alias for BCProgram (used by the JIT interface).
 * Must be defined before lr_jit.h is included. */
typedef BCProgram LRProgram;
#define LRPROGRAM_DEFINED 1

/* Portable strdup (MSVC exposes _strdup, POSIX strdup; avoid both). */
char *bc_strdup(const char *s);

/* ── API ────────────────────────────────────────────────────────────── */

BCProgram *bc_new_program(void);
void       bc_retain_program(BCProgram *prog);
void       bc_free_program(BCProgram *prog);

/* Compile an AST unit. Returns 0 when the program can be executed by the
 * VM, -1 when the caller must use the tree-walking interpreter instead. */
int        bc_compile(BCProgram *prog, struct ASTNode *node, int is_module);

/* Compile a function body with the "this" + parameter slots pre-bound to
 * match the runtime function-scope layout.  Returns 0 on success, -1 on
 * failure (caller must use the tree-walking interpreter instead). */
int        bc_compile_func(BCProgram *prog, struct ASTNode *func_node);

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

/* Thread-local memo stats — exposed for benchmark / debugging */
extern uint64_t lr_memo_hit_count(void);
extern uint64_t lr_memo_miss_count(void);

#endif /* LR_BYTECODE_H */
