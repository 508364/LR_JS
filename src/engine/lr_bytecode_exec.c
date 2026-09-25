/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: exec
 */
#include "lr_bytecode.h"
#include "lr_bytecode_cache.h"
#include "lr_bytecode_internal.h"
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

/* Stack overflow guard depth for bc_execute() re-entrancy.
 * Each bc_execute() frame alloca's ~4KB+; deep generator delegation
 * chains can quickly exceed safe C stack limits.
 * Set to 512 to support stress_test deepGen(500) with yield* chains. */
#define BC_STACK_GUARD_DEPTH 1024

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
   EXECUTOR — Direct/Indirect Threaded Bytecode Interpreter

   Direct threading (GCC/Clang): computed goto (&&label) — zero overhead.
   Indirect threading (MSVC): switch-based dispatch loop.

   v0.1.1+: SOLE execution engine. AST tree-walking interpreter is retired.
   ======================================================================= */

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

/* =======================================================================
   INLINE CALL FRAME
   Used by bc_execute when inlining function calls (IOME586 optimization).
   Saved on entry, restored on return.  The callee's bytecode runs with
   the same value stack as the caller (sp keeps growing), so only sp,
   scope depth, and interpreter flags need to be restored on exit.     */

#define MAX_INLINE_CALL_DEPTH 32

struct InlineCallFrame {
    /* -- VM state -------------------------------------------------- */
    uint8_t *ip;               /* return address */
    BCProgram *prog;           /* caller's program (for pool lookups) */
    int sp;                    /* value stack pointer to restore */
    int scope_depth;           /* block scope depth to restore */
    /* NOTE: The callee does NOT reset the value-stack pointer to 0.
     * It executes on top of the caller's stack (sp keeps growing), so
     * there is no need to save/restore the caller's stack values via
     * memcpy.  On return, sp is simply restored to cf->sp, which
     * discards the callee's temporaries (they were already consumed
     * by the callee's bytecode before BC_RETURN). */
    /* -- Interpreter flags ----------------------------------------- */
    int break_target;
    int continue_target;
    int return_target;
    int has_returned;
    int error_flag;
    LRValue return_value;
    const char *pending_label;
    /* 1 if the cold control-flow fields (break_target through
     * pending_label) were saved.  Pure-bytecode callees that never
     * touch BC_EVAL_NODE skip saving these, setting this flag to 0
     * so the restore path knows they are uninitialized. */
    uint8_t cold_saved;
    /* -- Scope state ----------------------------------------------- */
    InterpScope *current_scope;
    InterpScope *func_scope;
    /* 1 if the scope was created by scope_new_inline_fast (pure body:
     * no names, no closures, no block scopes).  BC_RETURN uses the
     * lighter scope_release_inline when this flag is set. */
    uint8_t pure_call;
    /* The callee function object must stay alive while its body executes
     * (it owns the AST the bytecode was compiled from).  Its popped stack
     * reference is released after the callee returns. */
    LRValue saved_callee;
    /* -- IOME586 pure-function memo state ---------------------------
     * When a memoizable pure function (is_pure) misses the result cache,
     * the call tracks which cache slot to fill on return.  memo_prog_id
     * != 0 means "fill bc_memo_cache[memo_slot] with the (primitive)
     * return value in BC_RETURN". */
    uint32_t memo_prog_id;
    uint8_t  memo_slot;
    /* -- JIT fast-path flag ----------------------------------------
     * 1 if the callee has JIT-compiled code and we're taking the JIT
     * path (skip cold save, use lighter restore).  Reduces per-call
     * overhead for hot JIT-compiled functions by ~40%. */
    uint8_t jit_call;
};

static LR_THREAD_LOCAL struct InlineCallFrame inline_call_stack[MAX_INLINE_CALL_DEPTH];
static LR_THREAD_LOCAL int inline_call_depth = 0;

/* ---- Inline Call Cache (ICC) ----
 * LRU-style cache of recent call sites to skip repeated parameter
 * validation and type checking for hot functions.
 * Hash: (caller_ip_hash + callee_id) & ICC_MASK.
 * On hit: argc and arg type signature match → skip full check. */
#define ICC_SIZE       64
#define ICC_MASK       (ICC_SIZE - 1)
#define ICC_MAX_TAG_SZ 4

typedef struct {
    uint8_t   valid;
    uint8_t   argc;
    uint8_t   tags[ICC_MAX_TAG_SZ];  /* arg type tags (LR_TYPE_INT32, etc.) */
    uint16_t  tag_size;              /* actual number of tags stored */
    uint32_t  callee_prog_id;        /* hash of callee identity */
    uint32_t  caller_ip_hint;        /* approximate caller program counter */
} ICCEntry;

static LR_THREAD_LOCAL ICCEntry ic_cache[ICC_SIZE];
static LR_THREAD_LOCAL uint64_t ic_hit_count = 0;
static LR_THREAD_LOCAL uint64_t ic_miss_count = 0;

/* PGO counters — collected at runtime, can be dumped for optimization. */
typedef struct {
    uint32_t  prog_id;
    uint64_t  call_count;
    uint64_t  jit_compile_count;
    uint64_t  memo_hit_count;
    uint64_t  avg_argc;
} PGOProgStats;

#define PGO_HASH_SIZE 1024
static LR_THREAD_LOCAL PGOProgStats pgo_stats[PGO_HASH_SIZE];
static LR_THREAD_LOCAL uint8_t pgo_dirty;  /* 1 if any counter changed */

/* Thread-local IOME586 pure-function result cache.  Lives across
 * bc_execute() invocations so repeated calls to the same pure function
 * with the same primitive args replay the stored result.  Direct-mapped:
 * the slot is derived from the arg hash. */
static LR_THREAD_LOCAL BCMemoCacheEnt bc_memo_cache[BC_MEMO_CACHE_SIZE];
static LR_THREAD_LOCAL uint64_t bc_memo_hit_count = 0;
static LR_THREAD_LOCAL uint64_t bc_memo_miss_count = 0;
static LR_THREAD_LOCAL int bc_call_counter = 0;

/* Cache state for savestate around BC_EVAL_NODE calls.
 * BC_EVAL_NODE invokes the tree-walking interpreter, which may call
 * bc_execute() again (nested VM), corrupting these thread-local caches. */
typedef struct {
    uint8_t ic_valid[BC_IC_PROP_SIZE];
    BCICPropCache ic_data[BC_IC_PROP_SIZE];
    uint8_t var_valid[BC_VAR_CACHE_SIZE];
    BCVarCacheEnt var_data[BC_VAR_CACHE_SIZE];
    uint8_t elem_valid[BC_ELEM_CACHE_SIZE];
    BCElemCacheEnt elem_data[BC_ELEM_CACHE_SIZE];
    uint8_t dyn_valid[BC_DYN_CACHE_SIZE];
    BCDynCacheEnt dyn_data[BC_DYN_CACHE_SIZE];
} BCCacheSnapshot;

/* Thread-local stack of cache snapshots for nested BC_EVAL_NODE saves.
 * Eliminates ~32KB per bc_execute() C-stack frame, critical for deep
 * generator delegation chains (e.g. deepGen(500) with yield*). */
static LR_THREAD_LOCAL BCCacheSnapshot bc_saved_cache_stack[64];
static LR_THREAD_LOCAL int bc_saved_cache_depth = 0;

/* ---- Inline Call Cache (ICC) helpers ----
 * Simple hash: combine callee prog_id and caller ip to index into
 * the ICC.  On a hit, argc and type tags match → skip parameter
 * validation. */
static inline uint32_t ic_hash(uint32_t prog_id, uintptr_t ip_hint)
{
    /* FNV-1a style mix */
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= (uint64_t)prog_id;
    h *= 0x100000001b3ULL;
    h ^= (uint64_t)ip_hint;
    h *= 0x100000001b3ULL;
    return (uint32_t)(h ^ (h >> 32));
}

static inline int ic_match(const ICCEntry *e, uint8_t argc,
                           const LRValue *argv, uint32_t prog_id,
                           uintptr_t ip_hint)
{
    if (!e->valid) return 0;
    if (e->callee_prog_id != prog_id) return 0;
    if (e->argc != argc) return 0;
    if (e->tag_size != (uint16_t)argc) return 0;
    for (uint8_t i = 0; i < argc; i++) {
        if (e->tags[i] != argv[i].tag) return 0;
    }
    return 1;
}

static inline void ic_update(ICCEntry *e, uint8_t argc,
                             const LRValue *argv, uint32_t prog_id,
                             uintptr_t ip_hint)
{
    e->valid = 1;
    e->argc = argc;
    e->tag_size = (uint16_t)argc;
    uint8_t sz = (argc > ICC_MAX_TAG_SZ) ? ICC_MAX_TAG_SZ : argc;
    for (uint8_t i = 0; i < sz; i++)
        e->tags[i] = argv[i].tag;
    e->callee_prog_id = prog_id;
    e->caller_ip_hint = (uint32_t)(ip_hint & 0xFFFFFFFF);
}

static inline void pgo_update(uint32_t prog_id, uint8_t argc)
{
    if (!prog_id || !pgo_dirty) return;
    uint32_t idx = (uint32_t)(prog_id & (PGO_HASH_SIZE - 1));
    PGOProgStats *s = &pgo_stats[idx];
    if (s->prog_id != prog_id) {
        s->prog_id = prog_id;
        s->call_count = 0;
        s->jit_compile_count = 0;
        s->memo_hit_count = 0;
        s->avg_argc = 0;
    }
    s->call_count++;
    s->avg_argc = (s->avg_argc + argc) / 2;
}

/* Classify arg types and update per-program type specialization counters.
 * Called from the call path to feed the JIT's type-specialization decisions. */
static inline void pgo_update_spec(BCProgram *body_prog, uint8_t argc,
                                   const LRValue *argv)
{
    if (!body_prog || argc == 0) return;
    uint8_t all_int32 = 1, all_f64 = 1, has_mixed = 0, has_other = 0;
    for (uint8_t i = 0; i < argc; i++) {
        int32_t t = argv[i].tag;
        if (t != LR_TYPE_INT32) all_int32 = 0;
        if (t != LR_TYPE_FLOAT64) all_f64 = 0;
        if (t == LR_TYPE_OBJECT || t == LR_TYPE_STRING || t == LR_TYPE_NULL
            || t == LR_TYPE_UNDEFINED || t == LR_TYPE_BOOL)
            has_other = 1;
        else if (t == LR_TYPE_INT32 && all_f64)
            all_f64 = 0;
        else if (t == LR_TYPE_FLOAT64 && all_int32)
            all_int32 = 0;
    }
    if (all_int32)      body_prog->spec_int32_count++;
    else if (all_f64)   body_prog->spec_float64_count++;
    else if (has_other) body_prog->spec_other_count++;
    else                body_prog->spec_mixed_count++;
    uint8_t *counts[] = {
        &body_prog->spec_int32_count,
        &body_prog->spec_float64_count,
        &body_prog->spec_mixed_count,
        &body_prog->spec_other_count
    };
    uint8_t max = body_prog->spec_int32_count;
    body_prog->spec_dominant = 0;
    for (int i = 1; i < 4; i++)
        if (*counts[i] > max) { max = *counts[i]; body_prog->spec_dominant = (uint8_t)i; }
}

void bc_dump_pgo_stats(void);

LRValue bc_execute(BCProgram *prog, LRContext *ctx)
{
    if (!prog || !prog->code || !ctx || !prog->compiled) return LR_VALUE_UNDEFINED;
    Interpreter *interp = (Interpreter *)ctx->opaque_interp;
    if (!interp) return LR_VALUE_UNDEFINED;
    /* Track current program for hot-loop JIT triggering. */
    (void)interp; /* used in BC_LOOP_TICK below */

    /* Init PGO env flag on first call */
    if (__builtin_expect(bc_env_debug_pgo < 0, 1))
        bc_env_debug_pgo = (getenv("LR_DEBUG_PGO") != NULL);
    if (bc_env_debug_pgo) pgo_dirty = 1;

    /* Minimum stack: 256 entries (4 KB) — enough for the vast majority of
     * scripts.  Large scripts with deep expression trees still get a larger
     * stack via the code_len/32 estimate.  This generous minimum ensures
     * PUSH_FAST (which omits the VM_GROW call) almost never needs to grow. */
    int est = prog->code_len / 32;
    int cap = prog->max_stack;
    if (cap < 256) cap = 256;
    if (est > cap) cap = est;
    /* Use a thread-local fixed-size buffer for small stacks to avoid malloc
     * overhead on every function call.  This is the common case for small
     * closure bodies.  Fall back to heap allocation for larger stacks.
     *
     * NESTED CALLS MUST NOT REUSE THIS BUFFER: bc_execute is re-entrant —
     * a JS function called from C (e.g. a property getter invoked via
     * lr_property_get → interp_call_function → bc_execute) runs a NESTED
     * bc_execute on the same thread.  Reusing bc_small_stack for the nested
     * frame would clobber the outer frame's live stack (the outer
     * LOAD_PROP_ADD accumulator was observed being overwritten by the
     * getter's return value).  The in_use flag makes the nested call take
     * a private heap buffer instead. */
    static LR_THREAD_LOCAL LRValue bc_small_stack[256];
    static LR_THREAD_LOCAL int bc_small_stack_depth = 0;
    /* Each bc_execute increments the depth.  Only the outermost call
     * (depth transitions 0→1) may use bc_small_stack.  Nested calls must
     * allocate a private heap buffer to avoid clobbering the outer frame's
     * live stack data. */
    /* Stack overflow guard: each bc_execute() frame alloca's ~4KB+ of
     * stack space.  Deep generator delegation chains (e.g. deepGen(500)
     * with yield*) can exceed safe C stack limits.  Cap C-side recursion
     * to prevent stack overflow before JS-level MAX_CALL_DEPTH (4096). */
    if (++bc_small_stack_depth > BC_STACK_GUARD_DEPTH) {
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "Maximum call stack size exceeded");
        interp->error_flag = 1;
        --bc_small_stack_depth;
        return LR_VALUE_UNDEFINED;
    }
    int own_small_stack = 0;
    LRValue *stack;
    int heap_stack = 0;
    int shared_stack = 0;
    /* Only the OUTERMOST bc_execute frame (depth==1) may use the
     * thread-local bc_small_stack buffer.  Nested frames MUST allocate a
     * private heap buffer instead: while an outer frame is suspended (e.g.
     * evaluating a call into a JS function/constructor that runs a nested
     * bc_execute on the same thread), the nested frame writing to the same
     * bc_small_stack array would clobber the outer frame's live stack data
     * (e.g. the `this` receiver already pushed for an outer method call). */
    if (bc_small_stack_depth == 1 && (size_t)cap <= 256) {
        stack = bc_small_stack;
        own_small_stack = 1;
        shared_stack = 1;
    } else {
        stack = (LRValue *)malloc(sizeof(LRValue) * (size_t)cap);
        if (!stack) return LR_VALUE_UNDEFINED;
        heap_stack = 1;
    }
    int sp = 0;
    int scope_depth = 0;
    LRValue result = LR_VALUE_UNDEFINED;
    uint8_t *ip = prog->code;
    uint8_t *code_end = prog->code + prog->code_len;
    int entry_depth = bc_small_stack_depth;
    /* Shared property-index operand for the fused BC_LOAD_PROP path.
     * get_prop sets it before get_prop_common; load_prop sets its own
     * copy then hands off via the same variable. */
    uint16_t prop_si;
    /* Shared receiver value for the same fused path.  Both get_prop and
     * load_prop must set this BEFORE reaching get_prop_common, since the
     * label can be entered from either handler via goto (a block-local
     * declaration would be uninitialized in the non-fall-through path). */
    LRValue prop_rcv;

    /* -- Persistent caches (from BCProgram) ------------------------------
     * Property, element, and variable caches all survive across
     * bc_execute() calls so that repeated invocations of the same
     * function body keep warm cache entries.
     * The variable cache uses var_depth (relative scope depth) instead
     * of raw scope pointers, so entries persist across calls even when
     * the scope chain is rebuilt for each closure invocation.          */
    BCICPropCache *bc_ic_prop = (BCICPropCache *)BC_CACHE_ICPROP(prog->cache_data);
    BCElemCacheEnt *bc_elem_cache = (BCElemCacheEnt *)BC_CACHE_ELEM(prog->cache_data);

    /* Persistent variable cache in BCProgram::cache_data.
     * Uses var_depth (relative scope depth) instead of raw scope pointers,
     * so cache entries survive across bc_execute() calls.  Validated at
     * lookup time by walking var_depth parent links and checking
     * names[slot] matches.  No per-call setup required.                 */
    BCVarCacheEnt *bc_var_cache = (BCVarCacheEnt *)BC_CACHE_VAR(prog->cache_data);

    /* Macro to update all cache pointers when prog changes (inline call
     * path switches to the callee's program, then back on return).     */
    #define BC_UPDATE_CACHE_PTRS() do {                                           \
        bc_ic_prop   = (BCICPropCache *)BC_CACHE_ICPROP(prog->cache_data);       \
        bc_var_cache = (BCVarCacheEnt *)BC_CACHE_VAR(prog->cache_data);          \
        bc_elem_cache = (BCElemCacheEnt *)BC_CACHE_ELEM(prog->cache_data);       \
    } while (0)

    /* Debug counters for inline call optimization */
    static int inline_cache_hit = 0;
    static int inline_cache_inline = 0;
    static int inline_cache_depth = 0;
    static int bc_inc_var_hit_total = 0;
    static int bc_inc_var_miss_total = 0;
    static int bc_inc_var_tier2_total = 0;

    /* Save/restore inline_call_depth for nested bc_execute calls */
    int saved_inline_call_depth = inline_call_depth;

    /* IOME586 pure-function memo pending state: set by BC_CALL when a pure
     * callee misses the result cache, consumed (and reset) when the inline
     * frame is pushed so BC_RETURN knows which slot to fill. */
    uint32_t cf_memo_pending_prog = 0;
    uint8_t  cf_memo_pending_slot = 0;

#define VM_GROW() do {                                                     \
        if (__builtin_expect(sp >= cap, 0)) {                              \
            int ncap = cap * 2;                                            \
            LRValue *ns = (LRValue *)malloc(sizeof(LRValue) * (size_t)ncap); \
            if (!ns) goto vm_abort;                                        \
            memcpy(ns, stack, sizeof(LRValue) * (size_t)sp);               \
            if (heap_stack) free(stack);                                   \
            if (shared_stack) { shared_stack = 0; } \
            stack = ns; cap = ncap; heap_stack = 1;                        \
        }                                                                  \
    } while (0)
/* VM_GROW is a safety net — bc_get_stack_size() should provide a
 * sufficient capacity upfront.  The initial allocation uses alloca,
 * and if growth is needed, malloc + memcpy + free promotes it to the
 * heap (tracked by heap_stack).  realloc is never called on alloca'd
 * memory, avoiding heap corruption. */
#define PUSH(v) do { VM_GROW(); stack[sp++] = (v); } while (0)
/* PUSH_FAST: Hot-path push with inline bounds check.  The branch is
 * almost never taken (initial capacity is generous at 64+ entries), so
 * __builtin_expect keeps the common case fast.  Unlike PUSH, the growth
 * logic is inlined rather than called via VM_GROW, saving a macro call
 * and allowing the compiler to optimize the common path better. */
#define PUSH_FAST(v) do {                                                  \
        if (__builtin_expect(sp >= cap, 0)) {                              \
            int ncap = cap * 2;                                            \
            LRValue *ns = (LRValue *)malloc(sizeof(LRValue) * (size_t)ncap);\
            if (!ns) goto vm_abort;                                        \
            memcpy(ns, stack, sizeof(LRValue) * (size_t)sp);               \
            if (heap_stack) free(stack);                                   \
            if (shared_stack) { shared_stack = 0; } \
            stack = ns; cap = ncap; heap_stack = 1;                        \
        }                                                                  \
        stack[sp++] = (v);                                                 \
    } while (0)
#define POP()   (__builtin_expect(sp > 0, 1) ? stack[--sp] : LR_VALUE_UNDEFINED)
/* CHECK macro: in debug mode, check all flags for verbose diagnostics.
 * In release mode, only error_flag and exception_pending are checked on
 * every instruction. has_returned / break_target / continue_target are
 * checked explicitly at the few points where they are relevant (BC_CALL,
 * BC_EVAL_NODE, loops). This reduces the per-instruction branch overhead
 * from 5 field loads + 5 comparisons to 2 loads + 2 cmp. */
#ifdef NDEBUG
/* Release mode: only error_flag and exception_pending are checked on every
 * instruction.  Branch prediction hints tell the CPU that errors are
 * vanishingly rare, keeping the common-case pipeline clean. */
#define CHECK() do { if (unlikely(interp->error_flag || interp->exception_pending)) \
                         goto vm_abort; } while (0)
#define CHECK_RET() do { if (unlikely(interp->has_returned)) goto vm_abort; } while (0)
#define CHECK_BREAK() do { if (unlikely(interp->break_target || interp->continue_target)) \
                               goto vm_abort; } while (0)
#else
/* Debug mode: full check with all flags for verbose error output */
#define CHECK() do { if (unlikely(interp->error_flag || interp->exception_pending ||  \
                        interp->has_returned || interp->break_target ||      \
                        interp->continue_target)) goto vm_abort; } while (0)
#define CHECK_RET() CHECK()
#define CHECK_BREAK() CHECK()
#endif

/* -- Inline function call stack ---------------------------------------- */
/* Allows the bytecode VM to call JS functions without going through the
 * heavyweight interp_call_function → bc_execute path.  The call stack
 * saves and restores VM state across function boundaries, eliminating
 * function call overhead, alloca, and cache re-initialization.
 *
 * LIMIT: 32 levels.  Deeper recursion falls back to interp_call_function. */

#if LR_THREADED_CODE
    /* -- Direct threaded dispatch table -------------------------------- */
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
        DOP(BC_LOAD_LOCAL,     load_local)
        DOP(BC_STORE_LOCAL,    store_local)
        DOP(BC_INC_LOCAL,      inc_local)
        DOP(BC_INC_LOCAL_DISCARD, inc_local_discard)
        DOP(BC_INC_VAR,        inc_var)
        DOP(BC_DECLARE_VAR,    declare_var)
        DOP(BC_TYPEOF_VAR,     typeof_var)
        DOP(BC_ADD_SELF,       add_self)
        DOP(BC_MUL_SELF,       mul_self)
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
        DOP(BC_IN,             in)
        DOP(BC_INSTANCEOF,     instanceof)
        DOP(BC_NEG,            neg)
        DOP(BC_POS,            pos)
        DOP(BC_NOT,            not)
        DOP(BC_BIT_NOT,        bit_not)
        DOP(BC_TYPEOF,         typeof)
        DOP(BC_VOID,           void)
        DOP(BC_JUMP,           jump)
        DOP(BC_JUMP_IF_FALSE,  jump_if_false)
        DOP(BC_JUMP_IF_TRUE,   jump_if_true)
        DOP(BC_JUMP_IF_FALSE_KEEP, jump_if_false_keep)
        DOP(BC_JUMP_IF_TRUE_KEEP,  jump_if_true_keep)
        DOP(BC_JUMP_IF_NOT_NULLISH, jump_if_not_nullish)
        DOP(BC_JUMP_IF_LOCAL_LT_IMM, jump_if_local_lt_imm)
        DOP(BC_LOOP_TICK,      loop_tick)
        DOP(BC_CALL,           call)
        DOP(BC_CALL_METHOD,    call)
        DOP(BC_CALL_ELEM,      call)
        DOP(BC_NEW,            call)
        DOP(BC_RETURN,         return)
        DOP(BC_NEW_OBJECT,     new_object)
        DOP(BC_NEW_ARRAY,      new_array)
        DOP(BC_DEF_PROP,       def_prop)
        DOP(BC_DEF_ELEM,       def_elem)
        DOP(BC_GET_PROP,       get_prop)
        DOP(BC_LOAD_PROP,      load_prop)
        DOP(BC_LOAD_PROP_ADD,  load_prop_add)
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
        DOP(BC_THROW,          throw)
        DOP(BC_TO_STRING,      to_string)
        DOP(BC_TO_NUMBER,      to_number)
        DOP(BC_TO_BOOL,        to_bool)
        #undef DOP
        dispatch_init = 1;
    }
    #define DISPATCH() do { uint8_t _op = *ip++; goto *dispatch[_op]; } while (0)
#else
    #define DISPATCH() goto vm_next
#endif

#if LR_THREADED_CODE
    /* -- Start direct-threaded dispatch -------------------------------- */
    {
        uint8_t op = *ip++;
        goto *dispatch[op];
    }
#else
    /* -- Start indirect-threaded (switch) dispatch -------------------- */
    goto vm_next;
#endif

    /* ===================================================================
       OPCODE HANDLERS
       Each handler reads operands, executes, then dispatches the next
       opcode via DISPATCH() (direct) or goto vm_next (indirect).        */

#if !LR_THREADED_CODE
    for (;;) {
    vm_next:
        if (ip < prog->code || ip >= code_end) goto vm_abort;
        switch (*ip++) {
#endif

        BC_CASE(stop, BC_STOP) {
            if (inline_call_depth > saved_inline_call_depth) {
                /* Implicit return: function body ended without a return
                 * statement (or with BC_SET_RESULT from an expression body).
                 * `result` already holds the correct value — BC_SET_RESULT
                 * sets it, and if no BC_SET_RESULT was executed, `result`
                 * was initialized to LR_VALUE_UNDEFINED at the top of
                 * bc_execute.  Do NOT overwrite it here. */
                struct InlineCallFrame *cf = &inline_call_stack[--inline_call_depth];
                if (cf->func_scope) {
                    while (scope_depth > 0) {
                        interp_bc_pop_scope(interp);
                        scope_depth--;
                    }
                    while (interp->current_scope &&
                           interp->current_scope != cf->func_scope)
                        interp_pop_scope(interp);
                    interp->current_scope = cf->current_scope;
                    scope_release(cf->func_scope, ctx);
                } else {
                    /* No-scope: pop block scopes from callee's bytecode */
                    while (scope_depth > 0) {
                        interp_bc_pop_scope(interp);
                        scope_depth--;
                    }
                    interp->current_scope = cf->current_scope;
                }
                ip = cf->ip;
                prog = cf->prog;
                BC_UPDATE_CACHE_PTRS();
                code_end = prog->code + prog->code_len;
                sp = cf->sp;
                scope_depth = cf->scope_depth;
                if (cf->cold_saved) {
                    interp->break_target = cf->break_target;
                    interp->continue_target = cf->continue_target;
                    interp->return_target = cf->return_target;
                    interp->has_returned = cf->has_returned;
                    interp->return_value = cf->return_value;
                    interp->pending_label = cf->pending_label;
                }
                interp->error_flag = cf->error_flag;
                FREE_IF_HEAP(ctx, cf->saved_callee);
                PUSH_FAST(dup_value_fast(result));
                DISPATCH();
            }
            goto vm_done;
        }
        BC_CASE(nop, BC_NOP) DISPATCH();

        BC_CASE(push_undefined, BC_PUSH_UNDEFINED) PUSH_FAST(LR_VALUE_UNDEFINED); DISPATCH();
        BC_CASE(push_null, BC_PUSH_NULL)           PUSH_FAST(LR_VALUE_NULL); DISPATCH();
        BC_CASE(push_true, BC_PUSH_TRUE) {
            LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = 1;
            PUSH_FAST(_v); DISPATCH();
        }
        BC_CASE(push_false, BC_PUSH_FALSE) {
            LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = 0;
            PUSH_FAST(_v); DISPATCH();
        }
        BC_CASE(push_this, BC_PUSH_THIS) {
            LRValue tv;
            interp_bc_push_this(interp, &tv);
            PUSH_FAST(tv);
            DISPATCH();
        }
        BC_CASE(push_int32, BC_PUSH_INT32) {
            LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = rd32(&ip);
            PUSH_FAST(_v); DISPATCH();
        }
        BC_CASE(push_float64, BC_PUSH_FLOAT64) {
            uint16_t si = rd16(&ip);
            LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = prog->pool[si].u.f64;
            PUSH_FAST(_v);
            DISPATCH();
        }
        BC_CASE(push_string, BC_PUSH_STRING) {
            uint16_t si = rd16(&ip);
            PUSH_FAST(lr_new_string(ctx, prog->pool[si].u.str));
            DISPATCH();
        }

        BC_CASE(pop, BC_POP) { LRValue v = POP(); FREE_IF_HEAP(ctx, v); DISPATCH(); }
        BC_CASE(dup, BC_DUP) {
            LRValue v = sp > 0 ? stack[sp - 1] : LR_VALUE_UNDEFINED;
            PUSH_FAST(dup_value_fast(v));
            DISPATCH();
        }
        BC_CASE(dup2, BC_DUP2) {
            if (sp < 2) goto vm_abort;
            LRValue a = stack[sp - 2], b = stack[sp - 1];
            PUSH_FAST(dup_value_fast(a));
            PUSH_FAST(dup_value_fast(b));
            DISPATCH();
        }
        BC_CASE(swap, BC_SWAP) {
            if (sp < 2) goto vm_abort;
            LRValue t = stack[sp - 1];
            stack[sp - 1] = stack[sp - 2];
            stack[sp - 2] = t;
            DISPATCH();
        }
        BC_CASE(rot3, BC_ROT3) {
            if (sp < 3) goto vm_abort;
            LRValue t = stack[sp - 1];
            stack[sp - 1] = stack[sp - 2];
            stack[sp - 2] = stack[sp - 3];
            stack[sp - 3] = t;
            DISPATCH();
        }

        BC_CASE(load_var, BC_LOAD_VAR) {
            uint16_t si = rd16(&ip);
            LRValue v;
            int found = 0;

            if (bc_debug_var()) {
                fprintf(stderr, "[VAR] LOAD_VAR si=%d name=%s prog=%p scope_depth=%d\n",
                        si, prog->pool[si].u.str, (void*)prog, scope_depth);
            }

            /* Direct-mapped inline cache: O(1) scope slot access.
             * Two-tier validation:
             *   1. Fast path: cached scope_ptr with gen check (no walk)
             *   2. Slow path: walk scope chain by var_depth, update cache
             *
             * OPTIMIZATION: The reachability walk is UNROLLED for the common
             * cases var_depth==0 (same scope) and var_depth==1 (parent scope).
             * This avoids the while-loop overhead for the majority of accesses
             * while still correctly detecting stale cache entries from a
             * previous invocation of the enclosing function.  The reachability
             * check is REQUIRED for correctness — without it, multiple closure
             * instances sharing the same bytecode program would incorrectly
             * read/write each other's captured scopes. */
            {   int ci = si & (BC_VAR_CACHE_SIZE - 1);
                BCVarCacheEnt *vc = &bc_var_cache[ci];
                if (__builtin_expect(vc->name_key == ((uint32_t)scope_depth << 16 | si) && vc->name_ptr != NULL, 1)) {
                    /* Tier 1: cached scope pointer (no scope chain walk) */
                    if (__builtin_expect(vc->scope_ptr != NULL &&
                        vc->scope_ptr->cache_gen == vc->scope_gen &&
                        vc->scope_ptr->names[vc->slot] == vc->name_ptr, 1)) {
                        /* Unrolled reachability check: avoid while-loop for
                         * the common var_depth==0 and var_depth==1 cases. */
                        InterpScope *chk = interp->current_scope;
                        int vd = (int)vc->var_depth;
                        if (vd == 0) {
                            /* same scope, nothing to walk */
                        } else if (__builtin_expect(vd == 1, 1)) {
                            chk = chk ? chk->parent : NULL;
                        } else {
                            int cd = 0;
                            while (chk && cd < vd) {
                                chk = chk->parent;
                                cd++;
                            }
                        }
                        if (chk == vc->scope_ptr) {
                            /* Hot path: load the value and dispatch directly */
                            PUSH_FAST(dup_value_fast(vc->scope_ptr->values[vc->slot]));
                            DISPATCH();
                        }
                        vc->scope_ptr = NULL; /* invalidate stale entry */
                    }
                    /* Tier 2: walk scope chain by depth */
                    {   InterpScope *s = interp->current_scope;
                        int d = 0;
                        while (s && d < (int)vc->var_depth) {
                            s = s->parent;
                            d++;
                        }
                        if (s &&
                            s->names[vc->slot] == vc->name_ptr) {
                            /* Update cached scope pointer */
                            vc->scope_ptr = s;
                            vc->scope_gen = s->cache_gen;
                            v = dup_value_fast(s->values[vc->slot]);
                            found = 1;
                        }
                    }
                }
            }
            if (bc_debug_var()) fprintf(stderr, "[VAR] AFTER_CACHE found=%d\n", found); fflush(stderr);

            if (!found) {
                /* Cache miss: full lookup */
                const char *name = prog->pool[si].u.str;
                if (!getenv("LR_DEBUG_VAR") && 0) {} else if (getenv("LR_DEBUG_VAR")) {
                    if (!interp_bc_load_var(interp, name, &v)) {
                        fprintf(stderr, "[VAR-MISS] prog=%p name='%s' current_scope chain:\n", (void*)prog, name);
                        InterpScope *cs = interp->current_scope;
                        int g2 = 0;
                        while (cs && g2++ < 32) {
                            fprintf(stderr, "   scope=%p parent=%p count=%d names=[", (void*)cs, (void*)cs->parent, cs->count);
                            for (int k2 = 0; k2 < cs->count; k2++)
                                if (cs->names && cs->names[k2]) fprintf(stderr, "'%s',", cs->names[k2]);
                            fprintf(stderr, "]\n");
                            cs = cs->parent;
                        }
                        goto vm_abort;
                    }
                } else if (!interp_bc_load_var(interp, name, &v)) goto vm_abort;
                /* Populate cache by walking scope chain */
                InterpScope *scope = interp->current_scope;
                while (scope) {
                    for (int i = 0; i < scope->count; i++) {
                        if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                            int ci = si & (BC_VAR_CACHE_SIZE - 1);
                            /* Compute var_depth: how many parent links from
                             * current_scope to the target scope. */
                            uint16_t vd = 0;
                            InterpScope *tmp = interp->current_scope;
                            while (tmp && tmp != scope) {
                                tmp = tmp->parent;
                                vd++;
                            }
                            bc_var_cache[ci].var_depth = vd;
                            bc_var_cache[ci].name_key = (uint32_t)scope_depth << 16 | si;
                            bc_var_cache[ci].slot = (uint16_t)i;
                            bc_var_cache[ci].name_ptr = scope->names[i];
                            bc_var_cache[ci].scope_ptr = scope;
                            bc_var_cache[ci].scope_gen = scope->cache_gen;
                            bc_var_cache[ci].is_const = (uint8_t)scope->is_const[i];
                            bc_var_cache[ci].is_declared = 0; /* visible via chain */
                            scope = NULL;  /* break outer loop */
                            break;
                        }
                    }
                    if (scope) scope = scope->parent;
                }
            }

            PUSH_FAST(v);
            DISPATCH();
        }
        BC_CASE(store_var, BC_STORE_VAR) {
            uint16_t si = rd16(&ip);
            LRValue v = POP();
            int found = 0;

            /* Direct-mapped inline cache: O(1) scope slot write.
             * Same unrolled reachability check as BC_LOAD_VAR. */
            {   int ci = si & (BC_VAR_CACHE_SIZE - 1);
                BCVarCacheEnt *vc = &bc_var_cache[ci];
                if (__builtin_expect(vc->name_key == ((uint32_t)scope_depth << 16 | si) && vc->name_ptr != NULL, 1)) {
                    InterpScope *s = NULL;
                    /* Tier 1: cached scope pointer */
                    if (__builtin_expect(vc->scope_ptr &&
                        vc->scope_ptr->cache_gen == vc->scope_gen &&
                        vc->scope_ptr->names[vc->slot] == vc->name_ptr, 1)) {
                        /* Unrolled reachability check (see BC_LOAD_VAR) */
                        InterpScope *chk = interp->current_scope;
                        int vd = (int)vc->var_depth;
                        if (vd == 0) {
                            /* same scope */
                        } else if (__builtin_expect(vd == 1, 1)) {
                            chk = chk ? chk->parent : NULL;
                        } else {
                            int cd = 0;
                            while (chk && cd < vd) {
                                chk = chk->parent;
                                cd++;
                            }
                        }
                        if (chk == vc->scope_ptr) {
                            s = vc->scope_ptr;
                        } else {
                            vc->scope_ptr = NULL;
                        }
                    } else {
                        /* Tier 2: walk scope chain by depth */
                        s = interp->current_scope;
                        int d = 0;
                        while (s && d < (int)vc->var_depth) {
                            s = s->parent;
                            d++;
                        }
                        if (s &&
                            s->names[vc->slot] == vc->name_ptr) {
                            /* Update cached scope pointer */
                            vc->scope_ptr = s;
                            vc->scope_gen = s->cache_gen;
                        } else {
                            s = NULL;
                        }
                    }
                    if (s) {
                        if (vc->is_const) {
                            snprintf(interp->error_message, sizeof(interp->error_message),
                                     "Assignment to constant variable");
                            interp->error_flag = 1;
                        } else {
                            if (s->values[vc->slot].tag == 6 && v.tag == 3)
                                LR_DEBUG_PRINT("[CORRUPT] STORE_VAR '%s' writes num(3) over slot=%d obj scope=%p names[slot]='%s' cur_name='%s' prog=%p\n",
                                        prog->pool[si].u.str, vc->slot, (void*)s,
                                        s->names[vc->slot]?s->names[vc->slot]:"?", vc->name_ptr, (void*)prog);
                            FREE_IF_HEAP(ctx, s->values[vc->slot]);
                            s->values[vc->slot] = dup_value_fast(v);
                            /* Maintain global mirror for script-mode top-level bindings */
                            if (s->is_global_scope && s->mirror_globals) {
                                LRValue global = lr_get_global_object(interp->ctx);
                                lr_set_property_str(interp->ctx, global,
                                                    prog->pool[si].u.str,
                                                    dup_value_fast(v));
                                lr_free_value(interp->ctx, global);
                            }
                        }
                        found = 1;
                    }
                }
            }

            if (!found) {
                /* Cache miss: full lookup */
                const char *name = prog->pool[si].u.str;
                interp_bc_store_var(interp, name, v);
                /* Populate cache by walking scope chain */
                if (!interp->error_flag) {
                    InterpScope *scope = interp->current_scope;
                    while (scope) {
                        for (int i = 0; i < scope->count; i++) {
                            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                                int ci = si & (BC_VAR_CACHE_SIZE - 1);
                                uint16_t vd = 0;
                                InterpScope *tmp = interp->current_scope;
                                while (tmp && tmp != scope) {
                                    tmp = tmp->parent;
                                    vd++;
                                }
                                bc_var_cache[ci].var_depth = vd;
                                bc_var_cache[ci].name_key = (uint32_t)scope_depth << 16 | si;
                                bc_var_cache[ci].slot = (uint16_t)i;
                                bc_var_cache[ci].name_ptr = scope->names[i];
                                bc_var_cache[ci].scope_ptr = scope;
                                bc_var_cache[ci].scope_gen = scope->cache_gen;
                                bc_var_cache[ci].is_const = (uint8_t)scope->is_const[i];
                                bc_var_cache[ci].is_declared = 0; /* visible via chain */
                                scope = NULL;
                                break;
                            }
                        }
                        if (scope) scope = scope->parent;
                    }
                }
            }

            FREE_IF_HEAP(ctx, v);
            CHECK();
            DISPATCH();
        }
        BC_CASE(load_local, BC_LOAD_LOCAL) {
            /* Direct slot access: no scope chain walk, no strcmp.
             * The slot index is relative to the current scope.
             * For well-formed bytecode, the scope and slot are always valid,
             * so we use __builtin_expect to guide branch prediction. */
            uint16_t slot = rd16(&ip);
            InterpScope *scope = interp->current_scope;
            if (__builtin_expect(scope != NULL && slot < (uint16_t)scope->count, 1)) {
                LRValue lv = scope->values[slot];
                PUSH_FAST(dup_value_fast(lv));
            } else {
                PUSH_FAST(LR_VALUE_UNDEFINED);
            }
            DISPATCH();
        }
        BC_CASE(store_local, BC_STORE_LOCAL) {
            /* Direct slot access: no scope chain walk, no strcmp */
            uint16_t slot = rd16(&ip);
            LRValue v = POP();
            InterpScope *scope = interp->current_scope;
            if (__builtin_expect(scope != NULL && slot < (uint16_t)scope->count, 1)) {
                if (scope->values[slot].tag == 6 && v.tag == 3)
                    LR_DEBUG_PRINT("[CORRUPT] STORE_LOCAL slot=%d writes num(3) over obj scope=%p name='%s' prog=%p\n",
                            slot, (void*)scope, scope->names[slot]?scope->names[slot]:"?", (void*)prog);
                FREE_IF_HEAP(ctx, scope->values[slot]);
                scope->values[slot] = v;  /* move, don't dup/free */
            } else {
                FREE_IF_HEAP(ctx, v);
            }
            CHECK();
            DISPATCH();
        }
        BC_CASE(inc_local, BC_INC_LOCAL) {
            /* Fast path: increment an int32 local variable in-place.
             * Replaces: LOAD_LOCAL + POS + DUP + PUSH 1 + ADD + STORE_LOCAL
             * (6 opcodes → 1) for the common case in tight loops. */
            uint16_t slot = rd16(&ip);
            InterpScope *scope = interp->current_scope;
            if (__builtin_expect(scope != NULL && slot < (uint16_t)scope->count, 1)) {
                LRValue old = scope->values[slot];
                if (__builtin_expect(old.tag == LR_TYPE_INT32, 1)) {
                    /* Fast path: direct int32 increment, no allocation */
                    int32_t old_val = old.u.int32;
                    int32_t new_val = old_val + 1;
                    LRValue result;
                    result.tag = LR_TYPE_INT32;
                    result.u.int32 = old_val;  /* push old value (postfix) */
                    scope->values[slot].u.int32 = new_val;
                    PUSH_FAST(result);
                } else {
                    /* Slow path: ToNumber + ADD + store */
                    LRValue one; one.tag = LR_TYPE_INT32; one.u.int32 = 1;
                    LRValue old_val = dup_value_fast(old);
                    LRValue new_val = bcv_binop(interp, BC_ADD, old_val, one);
                    lr_free_value(ctx, scope->values[slot]);
                    scope->values[slot] = new_val;
                    PUSH_FAST(old_val);
                }
            } else {
                PUSH_FAST(LR_VALUE_UNDEFINED);
            }
            DISPATCH();
        }
        BC_CASE(inc_local_discard, BC_INC_LOCAL_DISCARD) {
            /* Like BC_INC_LOCAL but does NOT push the old value.
             * Used for i++ as a statement (e.g. for loop update) where
             * the result is discarded, saving a push+pop cycle. */
            uint16_t slot = rd16(&ip);
            InterpScope *scope = interp->current_scope;
            if (__builtin_expect(scope != NULL && slot < (uint16_t)scope->count, 1)) {
                LRValue old = scope->values[slot];
                if (__builtin_expect(old.tag == LR_TYPE_INT32, 1)) {
                    scope->values[slot].u.int32 = old.u.int32 + 1;
                } else {
                    LRValue one; one.tag = LR_TYPE_INT32; one.u.int32 = 1;
                    LRValue old_val = dup_value_fast(old);
                    LRValue new_val = bcv_binop(interp, BC_ADD, old_val, one);
                    lr_free_value(ctx, scope->values[slot]);
                    scope->values[slot] = new_val;
                }
            }
            DISPATCH();
        }
        BC_CASE(inc_var, BC_INC_VAR) {
            /* Fused load + increment + store for non-local variables.
             * Replaces: LOAD_VAR + POS + DUP + PUSH 1 + ADD + STORE_VAR
             * (6 opcodes → 1) for the common case in tight loops.
             * Uses the same two-tier inline cache as LOAD_VAR/STORE_VAR. */
            uint16_t si = rd16(&ip);
            LRValue old_val;
            int found = 0;
            {   int ci = si & (BC_VAR_CACHE_SIZE - 1);
                BCVarCacheEnt *vc = &bc_var_cache[ci];
                if (__builtin_expect(vc->name_key == ((uint32_t)scope_depth << 16 | si) && vc->name_ptr != NULL, 1)) {
                    InterpScope *s = NULL;
                    /* Tier 1: cached scope pointer */
                    if (__builtin_expect(vc->scope_ptr &&
                        vc->scope_ptr->cache_gen == vc->scope_gen &&
                        vc->scope_ptr->names[vc->slot] == vc->name_ptr, 1)) {
                        /* Unrolled reachability check (see BC_LOAD_VAR) */
                        InterpScope *chk = interp->current_scope;
                        int vd = (int)vc->var_depth;
                        if (vd == 0) {
                            /* same scope */
                        } else if (__builtin_expect(vd == 1, 1)) {
                            chk = chk ? chk->parent : NULL;
                        } else {
                            int cd = 0;
                            while (chk && cd < vd) {
                                chk = chk->parent;
                                cd++;
                            }
                        }
                        if (chk == vc->scope_ptr) {
                            s = vc->scope_ptr;
                            bc_inc_var_hit_total++;
                        } else {
                            vc->scope_ptr = NULL;
                        }
                    } else {
                        /* Tier 2: walk scope chain by depth */
                        s = interp->current_scope;
                        int d = 0;
                        while (s && d < (int)vc->var_depth) {
                            s = s->parent;
                            d++;
                        }
                        if (s && s->names[vc->slot] == vc->name_ptr) {
                            vc->scope_ptr = s;
                            vc->scope_gen = s->cache_gen;
                            bc_inc_var_tier2_total++;
                        } else {
                            s = NULL;
                        }
                    }
                    if (s) {
                        LRValue cur = s->values[vc->slot];
                        if (__builtin_expect(cur.tag == LR_TYPE_INT32, 1)) {
                            /* Fast path: direct int32 increment, no allocation */
                            int32_t cur_val = cur.u.int32;
                            old_val.tag = LR_TYPE_INT32;
                            old_val.u.int32 = cur_val;      /* push old */
                            s->values[vc->slot].u.int32 = cur_val + 1;
                        } else {
                            /* Slow path: ToNumber + ADD + store */
                            LRValue one; one.tag = LR_TYPE_INT32; one.u.int32 = 1;
                            LRValue cur_dup = dup_value_fast(cur);
                            LRValue new_val = bcv_binop(interp, BC_ADD, cur_dup, one);
                            old_val = dup_value_fast(cur);
                            FREE_IF_HEAP(ctx, s->values[vc->slot]);
                            s->values[vc->slot] = new_val;
                        }
                        found = 1;
                    }
                }
            }

            if (!found) {
                bc_inc_var_miss_total++;
                /* Cache miss: fall back to full LOAD_VAR + INC + STORE_VAR */
                const char *name = prog->pool[si].u.str;
                LRValue v;
                if (!interp_bc_load_var(interp, name, &v)) goto vm_abort;
                LRValue one; one.tag = LR_TYPE_INT32; one.u.int32 = 1;
                LRValue new_val = bcv_binop(interp, BC_ADD, dup_value_fast(v), one);
                interp_bc_store_var(interp, name, new_val);
                old_val = v;
                /* Populate cache */
                if (!interp->error_flag) {
                    InterpScope *scope = interp->current_scope;
                    while (scope) {
                        for (int i = 0; i < scope->count; i++) {
                            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                                int ci = si & (BC_VAR_CACHE_SIZE - 1);
                                uint16_t vd = 0;
                                InterpScope *tmp = interp->current_scope;
                                while (tmp && tmp != scope) {
                                    tmp = tmp->parent;
                                    vd++;
                                }
                                bc_var_cache[ci].var_depth = vd;
                                bc_var_cache[ci].name_key = (uint32_t)scope_depth << 16 | si;
                                bc_var_cache[ci].slot = (uint16_t)i;
                                bc_var_cache[ci].name_ptr = scope->names[i];
                                bc_var_cache[ci].scope_ptr = scope;
                                bc_var_cache[ci].scope_gen = scope->cache_gen;
                                bc_var_cache[ci].is_const = (uint8_t)scope->is_const[i];
                                bc_var_cache[ci].is_declared = 0;
                                scope = NULL;
                                break;
                            }
                        }
                        if (scope) scope = scope->parent;
                    }
                }
            }

            PUSH_FAST(old_val);
            DISPATCH();
        }
        BC_CASE(declare_var, BC_DECLARE_VAR) {
            uint16_t si = rd16(&ip);
            uint8_t kind = *ip++;
            LRValue v = POP();
            const char *name = prog->pool[si].u.str;

            /* Fast path: inline cache hit — direct slot write, no scope walk.
             * This is the common case for `var x = i` in loop bodies where
             * the variable was already declared in a previous iteration.
             * Only entries with is_declared set may be used: a READ cache
             * entry may resolve to a parent (e.g. global) scope, and writing
             * through it here would leak the function-local var to the global
             * scope instead of hoisting it into the function scope. */
            int found = 0;
            {   int ci = si & (BC_VAR_CACHE_SIZE - 1);
                BCVarCacheEnt *vc = &bc_var_cache[ci];
                if (__builtin_expect(vc->name_key == ((uint32_t)scope_depth << 16 | si) &&
                    vc->is_declared, 1)) {
                    InterpScope *s = NULL;
                    /* Tier 1: cached scope pointer (no scope chain walk) */
                    if (__builtin_expect(vc->scope_ptr &&
                        vc->scope_ptr->cache_gen == vc->scope_gen &&
                        vc->scope_ptr->names[vc->slot] == vc->name_ptr, 1)) {
                        /* Verify cached scope reachability (see BC_LOAD_VAR) */
                        {   InterpScope *chk = interp->current_scope;
                            int cd = 0;
                            while (chk && cd < (int)vc->var_depth) {
                                chk = chk->parent;
                                cd++;
                            }
                            if (chk == vc->scope_ptr) {
                                s = vc->scope_ptr;
                            } else {
                                vc->scope_ptr = NULL;
                            }
                        }
                    } else {
                        /* Tier 2: walk scope chain by depth */
                        s = interp->current_scope;
                        int d = 0;
                        while (s && d < (int)vc->var_depth) {
                            s = s->parent;
                            d++;
                        }
                        if (s &&
                            s->names[vc->slot] == vc->name_ptr) {
                            vc->scope_ptr = s;
                            vc->scope_gen = s->cache_gen;
                        } else {
                            s = NULL;
                        }
                    }
                    if (s) {
                        /* Keep the global-object mirror in sync for script-mode
                         * top-level bindings (mirrors the STORE_VAR fast path). */
                        if (s->is_global_scope && s->mirror_globals) {
                            LRValue global = lr_get_global_object(ctx);
                            lr_set_property_str(ctx, global,
                                                prog->pool[si].u.str,
                                                dup_value_fast(v));
                            lr_free_value(ctx, global);
                        }
                        FREE_IF_HEAP(ctx, s->values[vc->slot]);
                        s->values[vc->slot] = v;  /* move, don't dup/free */
                        found = 1;
                    }
                }
            }

            if (!found) {
                interp_bc_declare_var(interp, name, v, (int)kind);
                /* Populate cache by walking scope chain */
                if (!interp->error_flag) {
                    InterpScope *scope = interp->current_scope;
                    while (scope) {
                        for (int i = 0; i < scope->count; i++) {
                            if (scope->names[i] && strcmp(scope->names[i], name) == 0) {
                                int ci = si & (BC_VAR_CACHE_SIZE - 1);
                                uint16_t vd = 0;
                                InterpScope *tmp = interp->current_scope;
                                while (tmp && tmp != scope) {
                                    tmp = tmp->parent;
                                    vd++;
                                }
                                bc_var_cache[ci].var_depth = vd;
                                bc_var_cache[ci].name_key = (uint32_t)scope_depth << 16 | si;
                                bc_var_cache[ci].slot = (uint16_t)i;
                                bc_var_cache[ci].name_ptr = scope->names[i];
                                bc_var_cache[ci].scope_ptr = scope;
                                bc_var_cache[ci].scope_gen = scope->cache_gen;
                                bc_var_cache[ci].is_const = (uint8_t)scope->is_const[i];
                                bc_var_cache[ci].is_declared = 1; /* declared here */
                                scope = NULL;
                                break;
                            }
                        }
                        if (scope) scope = scope->parent;
                    }
                }
                FREE_IF_HEAP(ctx, v);
            }
            CHECK();
            DISPATCH();
        }
        BC_CASE(typeof_var, BC_TYPEOF_VAR) {
            uint16_t si = rd16(&ip);
            LRValue v;
            if (interp_bc_typeof_var(interp, prog->pool[si].u.str, &v)) {
                PUSH_FAST(lr_new_string(ctx, bcv_typeof(ctx, v)));
                FREE_IF_HEAP(ctx, v);
            } else {
                PUSH_FAST(lr_new_string(ctx, "undefined"));
            }
            DISPATCH();
        }

        /* -- Inlined int32 fast path for hot arithmetic ops --------------
         * BC_ADD and BC_SUB are the hottest ops in tight loops.  Inlining
         * the int32-only path here avoids the function call to bcv_binop
         * AND the switch on op inside it.  Non-int32 values fall through
         * to binop_shared.  BC_MUL and BC_LT are also common in loops.   */
        /* -- Fused self-add: slot = slot + rhs (u16 slot) ---------------
         * Emitted by cassign for `X = X + Y` / `X += Y` on slot locals.
         * Because the left operand is NEVER loaded onto the stack, its
         * refcount stays 1 while concat runs, so the string concat can
         * reuse (realloc) the slot buffer in place — turning the O(n²)
         * full-buffer copy of `s = s + x` in a loop into an amortized
         * in-place append.  Non-string / shared-ref / non-ADD cases fall
         * back to the generic dup+ADD+store sequence. */
        BC_CASE(add_self, BC_ADD_SELF) {
            uint16_t slot = rd16(&ip);
            LRValue b = POP();
            InterpScope *scope = interp->current_scope;
            if (__builtin_expect(scope != NULL && slot < (uint16_t)scope->count, 1)) {
                LRValue a = scope->values[slot];
                /* -- string fast path: in-place append when sole owner -- */
                if (a.tag == LR_TYPE_STRING) {
                    LRString *as = (LRString *)a.u.ptr;
                    char num_buf[32];
                    int nlen = 0;
                    int str_rhs = 0;
                    size_t lb = 0;
                    if (b.tag == LR_TYPE_STRING) {
                        str_rhs = 1;
                        lb = ((LRString *)b.u.ptr)->len;
                    } else if (b.tag == LR_TYPE_INT32) {
                        nlen = snprintf(num_buf, sizeof(num_buf), "%d", b.u.int32);
                        lb = (size_t)nlen;
                    } else {
                        goto add_self_generic;
                    }
                    size_t la = as->len, total = la + lb;
                    LRString *os = NULL;
                    int reuse = 0;
                    if (as->ref_count == 1 && !as->is_atom) {
                        /* clear the small-string cache entry BEFORE realloc
                         * so it can't hold a dangling pointer */
                        if (as->len > 0 && as->len <= 32) {
                            unsigned int _h = 5381;
                            for (size_t _i = 0; _i < as->len; _i++)
                                _h = ((_h << 5) + _h) + (unsigned char)as->str[_i];
                            unsigned int _idx = _h & (LR_SMALL_STRING_CACHE_SIZE - 1);
                            if (ctx->rt->small_string_cache[_idx] == as)
                                ctx->rt->small_string_cache[_idx] = NULL;
                        }
                        os = (LRString *)realloc(as, sizeof(LRString) + total + 1);
                        if (os) reuse = 1;
                    }
                    if (!os) {
                        os = (LRString *)malloc(sizeof(LRString) + total + 1);
                        if (os) {
                            /* refcount 2: one for the slot store below,
                             * one for the stack push below.  (The existing
                             * LOAD+ADD+STORE path reaches the same count via
                             * DUP; here there is no DUP.) */
                            os->ref_count = 2;
                            os->is_atom = 0;
                            if (la) memcpy(os->str, as->str, la);
                        }
                    }
                    if (!os) {   /* OOM: fall back to empty-string result */
                        PUSH(lr_new_string(ctx, ""));
                        FREE_IF_HEAP(ctx, b);
                        goto add_self_done;
                    }
                    if (str_rhs)
                        memcpy(os->str + la, ((LRString *)b.u.ptr)->str, lb);
                    else
                        memcpy(os->str + la, num_buf, (size_t)nlen);
                    os->str[total] = '\0';
                    os->len = (uint32_t)total;
                    os->hash = reuse ? 0 : str_hash_fnv1a(os->str, total);
                    LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os;
                    if (reuse) {
                        os->ref_count++;   /* +1 for the stack push below */
                        scope->values[slot] = r;   /* same buffer, slot ref kept */
                    } else {
                        FREE_IF_HEAP(ctx, a);       /* release the old slot ref */
                        scope->values[slot] = r;
                    }
                    FREE_IF_HEAP(ctx, b);
                    PUSH_FAST(r);
                    DISPATCH();
                }
            add_self_generic:
                /* -- generic: dup slot value, ADD, store back -- */
                {
                    LRValue a2 = dup_value_fast(a);
                    LRValue r = bcv_binop(interp, BC_ADD, a2, b);
                    FREE_IF_HEAP(ctx, a2);
                    FREE_IF_HEAP(ctx, b);
                    if (interp->error_flag || interp->exception_pending) {
                        FREE_IF_HEAP(ctx, r);
                        goto vm_abort;
                    }
                    FREE_IF_HEAP(ctx, scope->values[slot]);
                    scope->values[slot] = dup_value_fast(r);
                    PUSH_FAST(r);
                }
            add_self_done:
                DISPATCH();
            }
            FREE_IF_HEAP(ctx, b);
            PUSH_FAST(LR_VALUE_UNDEFINED);
            DISPATCH();
        }

        /* -- Fused self-multiply: slot = slot * rhs (u16 slot) -----------
         * Emitted by cexpr for `x *= y` on slot locals at depth 0.
         * Avoids a stack push/pop of the slot value, saving dispatch overhead. */
        BC_CASE(mul_self, BC_MUL_SELF) {
            uint16_t slot = rd16(&ip);
            LRValue b = POP();
            InterpScope *scope = interp->current_scope;
            if (__builtin_expect(scope != NULL && slot < (uint16_t)scope->count, 1)) {
                LRValue a = scope->values[slot];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    double d = (double)a.u.int32 * (double)b.u.int32;
                    LRValue _v;
                    if (d == (double)(int32_t)d && !isnan(d) && !isinf(d)) {
                        _v.tag = LR_TYPE_INT32; _v.u.int32 = (int32_t)d;
                    } else {
                        _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = d;
                    }
                    FREE_IF_HEAP(ctx, scope->values[slot]);
                    scope->values[slot] = dup_value_fast(_v);
                    FREE_IF_HEAP(ctx, b);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) {
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 * b.u.float64;
                    FREE_IF_HEAP(ctx, scope->values[slot]);
                    scope->values[slot] = dup_value_fast(_v);
                    FREE_IF_HEAP(ctx, b);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_FLOAT64) {
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = (double)a.u.int32 * b.u.float64;
                    FREE_IF_HEAP(ctx, scope->values[slot]);
                    scope->values[slot] = dup_value_fast(_v);
                    FREE_IF_HEAP(ctx, b);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_INT32) {
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 * (double)b.u.int32;
                    FREE_IF_HEAP(ctx, scope->values[slot]);
                    scope->values[slot] = dup_value_fast(_v);
                    FREE_IF_HEAP(ctx, b);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            /* Fallback: generic LOAD + MUL + STORE */
            {
                InterpScope *scope2 = interp->current_scope;
                LRValue a_fallback = LR_VALUE_UNDEFINED;
                if (scope2 && slot < (uint16_t)scope2->count)
                    a_fallback = scope2->values[slot];
                LRValue r = bcv_binop(interp, BC_MUL, a_fallback, b);
                FREE_IF_HEAP(ctx, a_fallback);
                FREE_IF_HEAP(ctx, b);
                if (interp->error_flag) { FREE_IF_HEAP(ctx, r); goto vm_abort; }
                if (scope2 && slot < (uint16_t)scope2->count)
                    FREE_IF_HEAP(ctx, scope2->values[slot]);
                if (scope2 && slot < (uint16_t)scope2->count)
                    scope2->values[slot] = dup_value_fast(r);
                PUSH_FAST(r);
            }
            DISPATCH();
        }

        BC_CASE(add, BC_ADD) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                /* -- int32 + int32 ------------------------------------ */
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    int64_t sum = (int64_t)a.u.int32 + (int64_t)b.u.int32;
                    sp -= 2;
                    LRValue _v;
                    if (sum >= INT32_MIN && sum <= INT32_MAX) {
                        _v.tag = LR_TYPE_INT32; _v.u.int32 = (int32_t)sum;
                    } else {
                        _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = (double)sum;
                    }
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                /* -- float64 fast path: avoid bcv_binop function call -- */
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 + b.u.float64;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                /* -- mixed int32+float64: avoid bcv_to_number calls ---- */
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_FLOAT64) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = (double)a.u.int32 + b.u.float64;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 + (double)b.u.int32;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                /* -- string + string ---------------------------------- */
                if (a.tag == LR_TYPE_STRING && b.tag == LR_TYPE_STRING) {
                    LRString *as = (LRString *)a.u.ptr;
                    LRString *bs = (LRString *)b.u.ptr;
                    size_t la = as->len, lb = bs->len, total = la + lb;
                    sp -= 2;
                    LRString *os = NULL;
                    int reuse_a = 0, reuse_b = 0;
                    /* OPTIMIZATION: Reuse buffer when one source is the
                     * sole reference — avoids malloc + memcpy of the first
                     * string + free of the source.  This is a common case
                     * in string-building loops like s += 'x' + n.
                     * IMPORTANT: clear the small-string cache BEFORE realloc
                     * so that the cache doesn't hold a dangling pointer. */
                    if (as->ref_count == 1 && !as->is_atom) {
#ifdef STR_POOL_DEBUG
                        /* Off by default: the pool scan is O(pool size) and
                         * would make string-heavy loops quadratic.  Enable
                         * only when debugging pool corruption. */
                        if (getenv("LR_DEBUG_POOL") && lr_string_in_pool(ctx->rt, as)) {
                            fprintf(stderr, "[CONCAT-REUSE] reusing 'a' that is IN POOL: len=%u str='%.16s' ref=%d\n",
                                    as->len, as->str, as->ref_count);
                            abort();
                        }
#endif
                        if (as->len > 0 && as->len <= 32) {
                            unsigned int _h = 5381;
                            for (size_t _i = 0; _i < as->len; _i++)
                                _h = ((_h << 5) + _h) + (unsigned char)as->str[_i];
                            unsigned int _idx = _h & (LR_SMALL_STRING_CACHE_SIZE - 1);
                            if (ctx->rt->small_string_cache[_idx] == as)
                                ctx->rt->small_string_cache[_idx] = NULL;
                        }
                        os = (LRString *)realloc(as, sizeof(LRString) + total + 1);
                        if (os) {
                            if (lb) memcpy(os->str + la, bs->str, lb);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->hash = 0;
                            reuse_a = 1;
                        }
                    }
                    if (!os && bs->ref_count == 1 && !bs->is_atom) {
#ifdef STR_POOL_DEBUG
                        if (getenv("LR_DEBUG_POOL") && lr_string_in_pool(ctx->rt, bs)) {
                            fprintf(stderr, "[CONCAT-REUSE] reusing 'b' that is IN POOL: len=%u str='%.16s' ref=%d\n",
                                    bs->len, bs->str, bs->ref_count);
                            abort();
                        }
#endif
                        if (bs->len > 0 && bs->len <= 32) {
                            unsigned int _h = 5381;
                            for (size_t _i = 0; _i < bs->len; _i++)
                                _h = ((_h << 5) + _h) + (unsigned char)bs->str[_i];
                            unsigned int _idx = _h & (LR_SMALL_STRING_CACHE_SIZE - 1);
                            if (ctx->rt->small_string_cache[_idx] == bs)
                                ctx->rt->small_string_cache[_idx] = NULL;
                        }
                        os = (LRString *)realloc(bs, sizeof(LRString) + total + 1);
                        if (os) {
                            memmove(os->str + la, os->str, lb);
                            if (la) memcpy(os->str, as->str, la);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->hash = 0;
                            reuse_b = 1;
                        }
                    }
                    if (!os) {
                        os = (LRString *)malloc(sizeof(LRString) + total + 1);
                        if (os) {
                            if (la) memcpy(os->str, as->str, la);
                            if (lb) memcpy(os->str + la, bs->str, lb);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->ref_count = 1;
                            os->is_atom = 0;
                            os->hash = str_hash_fnv1a(os->str, total);
                        } else {
                            PUSH(lr_new_string(ctx, ""));
                            FREE_IF_HEAP(ctx, a);
                            FREE_IF_HEAP(ctx, b);
                            DISPATCH();
                        }
                    }
                    LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os;
                    PUSH(r);
                    if (!reuse_a) FREE_IF_HEAP(ctx, a);
                    if (!reuse_b) FREE_IF_HEAP(ctx, b);
                    DISPATCH();
                }
                /* -- string + int32 ----------------------------------- */
                if (a.tag == LR_TYPE_STRING && b.tag == LR_TYPE_INT32) {
                    LRString *as = (LRString *)a.u.ptr;
                    size_t la = as->len;
                    char num_buf[32];
                    int nlen = snprintf(num_buf, sizeof(num_buf), "%d", b.u.int32);
                    size_t total = la + (size_t)nlen;
                    sp -= 2;
                    LRString *os = NULL;
                    int reuse_a = 0;
                    if (as->ref_count == 1 && !as->is_atom) {
#ifdef STR_POOL_DEBUG
                        if (getenv("LR_DEBUG_POOL") && lr_string_in_pool(ctx->rt, as)) {
                            fprintf(stderr, "[CONCAT-REUSE] reusing 'a' string+int IN POOL: len=%u str='%.16s' ref=%d\n",
                                    as->len, as->str, as->ref_count);
                            abort();
                        }
#endif
                        if (as->len > 0 && as->len <= 32) {
                            unsigned int _h = 5381;
                            for (size_t _i = 0; _i < as->len; _i++)
                                _h = ((_h << 5) + _h) + (unsigned char)as->str[_i];
                            unsigned int _idx = _h & (LR_SMALL_STRING_CACHE_SIZE - 1);
                            if (ctx->rt->small_string_cache[_idx] == as)
                                ctx->rt->small_string_cache[_idx] = NULL;
                        }
                        os = (LRString *)realloc(as, sizeof(LRString) + total + 1);
                        if (os) {
                            memcpy(os->str + la, num_buf, (size_t)nlen);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->hash = 0;
                            reuse_a = 1;
                        }
                    }
                    if (!os) {
                        os = (LRString *)malloc(sizeof(LRString) + total + 1);
                        if (os) {
                            if (la) memcpy(os->str, as->str, la);
                            memcpy(os->str + la, num_buf, (size_t)nlen);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->ref_count = 1;
                            os->is_atom = 0;
                            os->hash = str_hash_fnv1a(os->str, total);
                        } else {
                            PUSH(lr_new_string(ctx, ""));
                            FREE_IF_HEAP(ctx, a);
                            FREE_IF_HEAP(ctx, b);
                            DISPATCH();
                        }
                    }
                    LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os;
                    PUSH(r);
                    if (!reuse_a) FREE_IF_HEAP(ctx, a);
                    FREE_IF_HEAP(ctx, b);
                    DISPATCH();
                }
                /* -- int32 + string ----------------------------------- */
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_STRING) {
                    LRString *bs = (LRString *)b.u.ptr;
                    size_t lb = bs->len;
                    char num_buf[32];
                    int nlen = snprintf(num_buf, sizeof(num_buf), "%d", a.u.int32);
                    size_t total = (size_t)nlen + lb;
                    sp -= 2;
                    LRString *os = NULL;
                    int reuse_b = 0;
                    if (bs->ref_count == 1 && !bs->is_atom) {
#ifdef STR_POOL_DEBUG
                        if (getenv("LR_DEBUG_POOL") && lr_string_in_pool(ctx->rt, bs)) {
                            fprintf(stderr, "[CONCAT-REUSE] reusing 'b' int+string IN POOL: len=%u str='%.16s' ref=%d\n",
                                    bs->len, bs->str, bs->ref_count);
                            abort();
                        }
#endif
                        if (bs->len > 0 && bs->len <= 32) {
                            unsigned int _h = 5381;
                            for (size_t _i = 0; _i < bs->len; _i++)
                                _h = ((_h << 5) + _h) + (unsigned char)bs->str[_i];
                            unsigned int _idx = _h & (LR_SMALL_STRING_CACHE_SIZE - 1);
                            if (ctx->rt->small_string_cache[_idx] == bs)
                                ctx->rt->small_string_cache[_idx] = NULL;
                        }
                        os = (LRString *)realloc(bs, sizeof(LRString) + total + 1);
                        if (os) {
                            memmove(os->str + nlen, os->str, lb);
                            memcpy(os->str, num_buf, (size_t)nlen);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->hash = 0;
                            reuse_b = 1;
                        }
                    }
                    if (!os) {
                        os = (LRString *)malloc(sizeof(LRString) + total + 1);
                        if (os) {
                            memcpy(os->str, num_buf, (size_t)nlen);
                            if (lb) memcpy(os->str + nlen, bs->str, lb);
                            os->str[total] = '\0';
                            os->len = (uint32_t)total;
                            os->ref_count = 1;
                            os->is_atom = 0;
                            os->hash = str_hash_fnv1a(os->str, total);
                        } else {
                            PUSH(lr_new_string(ctx, ""));
                            FREE_IF_HEAP(ctx, a);
                            FREE_IF_HEAP(ctx, b);
                            DISPATCH();
                        }
                    }
                    LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os;
                    PUSH(r);
                    FREE_IF_HEAP(ctx, a);
                    if (!reuse_b) FREE_IF_HEAP(ctx, b);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(sub, BC_SUB) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    int64_t diff = (int64_t)a.u.int32 - (int64_t)b.u.int32;
                    sp -= 2;
                    LRValue _v;
                    if (diff >= INT32_MIN && diff <= INT32_MAX) {
                        _v.tag = LR_TYPE_INT32; _v.u.int32 = (int32_t)diff;
                    } else {
                        _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = (double)diff;
                    }
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                /* Float64 fast path: avoid bcv_binop function call */
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 - b.u.float64;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_FLOAT64) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = (double)a.u.int32 - b.u.float64;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 - (double)b.u.int32;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(mul, BC_MUL) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    double d = (double)a.u.int32 * (double)b.u.int32;
                    sp -= 2;
                    LRValue _v;
                    if (d == (double)(int32_t)d && !isnan(d) && !isinf(d)) {
                        _v.tag = LR_TYPE_INT32; _v.u.int32 = (int32_t)d;
                    } else {
                        _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = d;
                    }
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                /* Float64 fast path: avoid bcv_binop function call */
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 * b.u.float64;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_FLOAT64) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = (double)a.u.int32 * b.u.float64;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_FLOAT64; _v.u.float64 = a.u.float64 * (double)b.u.int32;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        /* -- Numeric comparison quick paths (LT/GT/LE/GE/EQ/NE) ----------
         * Avoid the generic bcv_binop() call (which handles all coercions)
         * when both operands are already numbers. int32/float64 mixes
         * compare numerically, matching JS `==` / `</>` semantics. */
#define NUM_CMP_QUICK(cmp) do { \
    if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) { \
        sp -= 2; LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.float64 cmp b.u.float64); \
        PUSH_FAST(_v); DISPATCH(); \
    } \
    if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_FLOAT64) { \
        sp -= 2; LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = ((double)a.u.int32 cmp b.u.float64); \
        PUSH_FAST(_v); DISPATCH(); \
    } \
    if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_INT32) { \
        sp -= 2; LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.float64 cmp (double)b.u.int32); \
        PUSH_FAST(_v); DISPATCH(); \
    } \
} while (0)
        BC_CASE(lt, BC_LT) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 < b.u.int32);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                NUM_CMP_QUICK(<);
            }
            goto binop_shared;
        }
        BC_CASE(gt, BC_GT) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 > b.u.int32);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                NUM_CMP_QUICK(>);
            }
            goto binop_shared;
        }
        BC_CASE(le, BC_LE) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 <= b.u.int32);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(ge, BC_GE) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 >= b.u.int32);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                NUM_CMP_QUICK(>=);
            }
            goto binop_shared;
        }
        /* Inlined int32 division */
        BC_CASE(div, BC_DIV) {
            /* JS `/` is always IEEE-754 float division (7/2 === 3.5).  Use the
             * shared float path so results and edge cases (INT_MIN/-1, /0)
             * match the spec instead of integer idiv. */
            goto binop_shared;
        }
        /* Inlined int32 modulus */
        BC_CASE(mod, BC_MOD) {
            /* JS `%` is IEEE-754 float modulo.  Use the shared float path to
             * avoid x86 idiv SIGFPE on INT_MIN%-1 and match JS semantics. */
            goto binop_shared;
        }
        BC_CASE(pow, BC_POW) { goto binop_shared; }
        /* Comparisons that go through bcv_binop's fast path too */
        BC_CASE(eq, BC_EQ) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 == b.u.int32);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                NUM_CMP_QUICK(==);
            }
            goto binop_shared;
        }
        BC_CASE(ne, BC_NE) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 != b.u.int32);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
                NUM_CMP_QUICK(!=);
            }
            goto binop_shared;
        }
        /* -- Strict equality quick paths (===/!==): only compare within the
         * same number type; mixed int32/float64 falls to bcv_binop which
         * enforces JS strict-equality type identity (5 !== 5.0). */
#define STRICT_NUM_CMP_QUICK(cmp) do { \
    if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) { \
        sp -= 2; LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.float64 cmp b.u.float64); \
        PUSH_FAST(_v); DISPATCH(); \
    } \
    if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) { \
        sp -= 2; LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (a.u.int32 cmp b.u.int32); \
        PUSH_FAST(_v); DISPATCH(); \
    } \
} while (0)
        BC_CASE(strict_eq, BC_STRICT_EQ) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                STRICT_NUM_CMP_QUICK(==);
            }
            goto binop_shared;
        }
        BC_CASE(strict_ne, BC_STRICT_NE) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                STRICT_NUM_CMP_QUICK(!=);
            }
            goto binop_shared;
        }
        /* -- Inlined bitwise ops ---------------------------------------
         * These are common in tight loops (e.g. hashing, flags, bit
         * manipulation).  Inlining avoids the function call to bcv_binop
         * AND the switch on op inside it.                               */
        BC_CASE(shl, BC_SHL) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = (int32_t)((uint32_t)a.u.int32 << (b.u.int32 & 31));
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(shr, BC_SHR) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = (int32_t)((uint32_t)a.u.int32 >> (b.u.int32 & 31));
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(sar, BC_SAR) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = a.u.int32 >> (b.u.int32 & 31);
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(bit_and, BC_BIT_AND) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = a.u.int32 & b.u.int32;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(bit_or, BC_BIT_OR) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = a.u.int32 | b.u.int32;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(bit_xor, BC_BIT_XOR) {
            if (sp >= 2) {
                LRValue b = stack[sp-1], a = stack[sp-2];
                if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
                    sp -= 2;
                    LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = a.u.int32 ^ b.u.int32;
                    PUSH_FAST(_v);
                    DISPATCH();
                }
            }
            goto binop_shared;
        }
        BC_CASE(in, BC_IN)   { goto binop_shared; }
        BC_CASE(instanceof, BC_INSTANCEOF) { goto binop_shared; }
        binop_shared: {
            if (sp < 2) goto vm_abort;
            LRValue b = POP(), a = POP();
            LRValue r = bcv_binop(interp, (int)*(ip - 1), a, b);
            FREE_IF_HEAP(ctx, a);
            FREE_IF_HEAP(ctx, b);
            if (interp->error_flag) { FREE_IF_HEAP(ctx, r); goto vm_abort; }
            PUSH_FAST(r);
            DISPATCH();
        }

        BC_CASE(neg, BC_NEG) {
            LRValue a = POP();
            LRValue r = bcv_number(ctx, -bcv_to_number(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(r);
            DISPATCH();
        }
        BC_CASE(pos, BC_POS) {
            LRValue a = POP();
            LRValue r = bcv_number(ctx, bcv_to_number(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(r);
            DISPATCH();
        }
        BC_CASE(not, BC_NOT) {
            LRValue a = POP();
            int t = lr_to_bool(ctx, a);
            FREE_IF_HEAP(ctx, a);
            LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = !t;
            PUSH_FAST(_v);
            DISPATCH();
        }
        BC_CASE(bit_not, BC_BIT_NOT) {
            LRValue a = POP();
            LRValue _v; _v.tag = LR_TYPE_INT32; _v.u.int32 = ~bcv_to_int32(ctx, a);
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(_v);
            DISPATCH();
        }
        BC_CASE(typeof, BC_TYPEOF) {
            LRValue a = POP();
            LRValue r = lr_new_string(ctx, bcv_typeof(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(r);
            DISPATCH();
        }
        BC_CASE(void, BC_VOID) {
            LRValue a = POP();
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(LR_VALUE_UNDEFINED);
            DISPATCH();
        }

        /* -- Type conversion (inline fast paths for String/Number/Boolean) -- */
        BC_CASE(to_string, BC_TO_STRING) {
            LRValue a = POP();
            LRValue r = lr_new_string(ctx, lr_to_cstring(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(r);
            DISPATCH();
        }
        BC_CASE(to_number, BC_TO_NUMBER) {
            LRValue a = POP();
            LRValue r = bcv_number(ctx, bcv_to_number(ctx, a));
            FREE_IF_HEAP(ctx, a);
            PUSH_FAST(r);
            DISPATCH();
        }
        BC_CASE(to_bool, BC_TO_BOOL) {
            LRValue a = POP();
            int t = lr_to_bool(ctx, a);
            FREE_IF_HEAP(ctx, a);
            LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = (t ? 1 : 0);
            PUSH_FAST(_v);
            DISPATCH();
        }

        BC_CASE(jump, BC_JUMP) { int32_t off = rd32(&ip); ip += off; DISPATCH(); }
        BC_CASE(jump_if_false, BC_JUMP_IF_FALSE) {
            int32_t off = rd32(&ip);
            LRValue v = POP();
            /* Fast path: int32 (including bool) values avoid lr_to_bool call */
            if (v.tag == LR_TYPE_INT32) {
                if (v.u.int32 == 0 || v.u.int32 == 1) {
                    if (!v.u.int32) ip += off;
                    DISPATCH();
                }
            }
            int t = lr_to_bool(ctx, v);
            FREE_IF_HEAP(ctx, v);
            if (!t) ip += off;
            DISPATCH();
        }
        BC_CASE(jump_if_true, BC_JUMP_IF_TRUE) {
            int32_t off = rd32(&ip);
            LRValue v = POP();
            /* Fast path: int32 (including bool) values avoid lr_to_bool call */
            if (v.tag == LR_TYPE_INT32) {
                if (v.u.int32 == 0 || v.u.int32 == 1) {
                    if (v.u.int32) ip += off;
                    DISPATCH();
                }
            }
            int t = lr_to_bool(ctx, v);
            FREE_IF_HEAP(ctx, v);
            if (t) ip += off;
            DISPATCH();
        }
        BC_CASE(jump_if_false_keep, BC_JUMP_IF_FALSE_KEEP) {
            int32_t off = rd32(&ip);
            if (sp < 1) goto vm_abort;
            if (!lr_to_bool(ctx, stack[sp - 1])) ip += off;
            DISPATCH();
        }
        BC_CASE(jump_if_true_keep, BC_JUMP_IF_TRUE_KEEP) {
            int32_t off = rd32(&ip);
            if (sp < 1) goto vm_abort;
            if (lr_to_bool(ctx, stack[sp - 1])) ip += off;
            DISPATCH();
        }
        BC_CASE(jump_if_not_nullish, BC_JUMP_IF_NOT_NULLISH) {
            int32_t off = rd32(&ip);
            if (sp < 1) goto vm_abort;
            LRValue v = stack[sp - 1];
            if (v.tag != LR_TYPE_UNDEFINED && v.tag != LR_TYPE_NULL) ip += off;
            DISPATCH();
        }
        BC_CASE(jump_if_local_lt_imm, BC_JUMP_IF_LOCAL_LT_IMM) {
            uint16_t slot = rd16(&ip);
            int32_t imm = rd32(&ip);
            int32_t offset = rd32(&ip);
            InterpScope *scope = interp->current_scope;
            int cond = 0;
            if (scope && slot < (uint16_t)scope->count) {
                LRValue v = scope->values[slot];
                if (__builtin_expect(v.tag == LR_TYPE_INT32, 1)) {
                    cond = v.u.int32 < imm;
                } else {
                    double d = bcv_to_number(ctx, v);
                    cond = d < (double)imm;
                }
            }
            /* Jump to exit when condition is false (local >= imm) */
            if (!cond) {
                ip += offset;
            }
            DISPATCH();
        }
        BC_CASE(loop_tick, BC_LOOP_TICK)
            /* Batch timeout check: only call clock() every 1024 iterations.
             * In hot loops without timeout (timeout_ms == 0), this skips
             * all the branching entirely via compiler branch prediction. */
            if (__builtin_expect(interp->timeout_ms > 0, 0)) {
                if (++interp->stmt_counter >= 1024) {
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
            }
            /* JIT hot-loop detection: only when JIT is enabled and nparams > 0.
             * For top-level scripts (nparams == 0), this entire block is
             * eliminated by the compiler's dead-code elimination. */
            {
                LRRuntime *lr_rt = (LRRuntime *)interp->ctx->rt;
                LRJITRuntime *jit_rt_local = NULL;
                if (__builtin_expect(lr_rt && lr_rt->jit_runtime, 0)) {
                    jit_rt_local = (LRJITRuntime *)lr_rt->jit_runtime;
                    if (__builtin_expect(jit_rt_local->enabled, 0)) {
                        if (__builtin_expect(!prog->jit_skip && prog->nparams > 0, 1)) {
                            interp->jit_loop_counter++;
                            if (__builtin_expect(interp->jit_loop_counter >= LR_JIT_HOT_LOOP_ITERS, 0)) {
                                interp->jit_loop_counter = 0;
                                BCProgram *cur = prog;
                                if (cur && !cur->jit_entry) {
                                    lr_jit_compile(interp, (LRProgram *)cur);
                                    if (!cur->jit_entry) {
                                        cur->jit_skip = 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            DISPATCH();

        BC_CASE(call, BC_CALL)
#if !LR_THREADED_CODE
 case BC_CALL_METHOD: case BC_CALL_ELEM: case BC_NEW:
#endif
 {
            /* NOTE: We do NOT invalidate the variable cache here because each
             * function body is executed via its own bc_execute() call with
             * a fresh cache.  The outer scope's cache remains valid across
             * function calls, avoiding repeated linear-scope walks for
             * loop variables like `for (var i = 0; i < N; i++) f(i)`. */
            uint8_t saved_op = *(ip - 1);
            uint16_t name_idx = 0;
            if (saved_op == BC_CALL_METHOD) { name_idx = rd16(&ip); }
            uint16_t argc_lo = (uint8_t)ip[0];
            uint16_t argc_hi = (uint8_t)ip[1];
            uint16_t argc = argc_lo | (argc_hi << 8);
            ip += 2;
            LRValue argbuf[8];
            LRValue *argv = argbuf;
            if (argc > 8) {
                argv = (LRValue *)malloc(sizeof(LRValue) * argc);
                if (!argv) goto vm_abort;
            }
            if (sp < (int)argc) goto vm_abort;
            for (int i = (int)argc - 1; i >= 0; i--) argv[i] = POP();

            LRValue callee = LR_VALUE_UNDEFINED, this_val = LR_VALUE_UNDEFINED;
            LRValue key = LR_VALUE_UNDEFINED;
            LRValue r = LR_VALUE_UNDEFINED;
            if (saved_op == BC_CALL_METHOD) {
                /* MATH FAST PATH: inline Math.sqrt/sin/cos/etc. for
                 * single-argument calls. Uses hash-based dispatch for
                 * O(1) function name resolution instead of O(n) if-else
                 * chain. Avoids property lookup and C function dispatch
                 * overhead in tight numeric loops. */
                if (argc >= 1 && this_val.tag == LR_TYPE_OBJECT &&
                    ((LRObject *)this_val.u.ptr) == interp->math_obj) {
                    double x = bcv_to_number(ctx, argv[0]);
                    double result;
                    /* Use precomputed FNV-1a hash from the constant pool */
                    uint32_t h = prog->pool[name_idx].str_hash;
                    switch (h) {
                        case 0x0035FD20u: result = sqrt(x);    break; /* sqrt */
                        case 0x0001BCD8u: result = sin(x);     break; /* sin */
                        case 0x00018187u: result = cos(x);     break; /* cos */
                        case 0x05D0240Cu: result = floor(x);   break; /* floor */
                        case 0x002E8905u: result = ceil(x);    break; /* ceil */
                        case 0x067AB18Eu: result = round(x);   break; /* round */
                        case 0x00017872u: result = fabs(x);    break; /* abs */
                        case 0x0001BFA1u: result = tan(x);     break; /* tan */
                        case 0x0001A344u: result = log(x);     break; /* log */
                        case 0x00018A1Du: result = exp(x);     break; /* exp */
                        case 0x002DD4D7u: result = asin(x);    break; /* asin */
                        case 0x002D9986u: result = acos(x);    break; /* acos */
                        case 0x002DD7A0u: result = atan(x);    break; /* atan */
                        case 0x0035DE90u: result = sinh(x);    break; /* sinh */
                        case 0x002EAFC1u: result = cosh(x);    break; /* cosh */
                        case 0x003634E7u: result = tanh(x);    break; /* tanh */
                        case 0x002E7EE1u: result = cbrt(x);    break; /* cbrt */
                        case 0x0032C56Eu: result = log2(x);    break; /* log2 */
                        case 0x0625E863u: result = log10(x);   break; /* log10 */
                        case 0x0625E8A3u: result = log1p(x);   break; /* log1p */
                        case 0x05C78441u: result = expm1(x);   break; /* expm1 */
                        case 0xB48900E8u: result = (float)x;   break; /* fround */
                        case 0x06983DACu: result = trunc(x);   break; /* trunc */
                        case 0x0035DDBDu: result = (x > 0) ? 1 : (x < 0) ? -1 : (x == 0) ? 0 : NAN; break; /* sign */
                        default: goto math_skip;
                    }
                    callee = LR_VALUE_UNDEFINED; /* skip FREE_IF_HEAP */
                    r = bcv_number(ctx, result);
                    goto call_cleanup;
                    math_skip: ;
                }
                this_val = POP();
                callee = lr_get_property_str(ctx, this_val, prog->pool[name_idx].u.str);
                if (callee.tag == LR_TYPE_UNDEFINED) {
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "value is not a function");
                    interp->error_flag = 1;
                    goto call_cleanup;
                }
            } else if (saved_op == BC_CALL_ELEM) {
                key = POP();
                this_val = POP();
                LRString *atom = lr_to_atom(ctx, key);
                callee = lr_get_property(ctx, this_val, atom);
            } else {
                callee = POP();
                /* Defer the global-object `this` fetch.  Every plain call
                 * used to bump+drop the global object refcount, which on a
                 * tight func_call loop was 10M+ lr_get_global_object calls.
                 * The value is now fetched lazily only when a fallback call
                 * path, or an inline callee that actually reads `this`,
                 * needs it (see below). */
                this_val = LR_VALUE_UNDEFINED;
                if (callee.tag == 0 && sp == 0) {
                    goto vm_abort;
                }
            }

            /* -- MEMO CACHE FAST PATH (early check) ---------------------
             * Check for pure-function memoization cache hit before any
             * other work (C function fast path, inline call setup, JIT
             * notification, can_inline checks).  The cache hit path is
             * ~20 instructions vs ~150 through the full inline path,
             * which is the dominant case for tight loops calling pure
             * functions like `empty() { return 1; }`.
             *
             * The hash and args_eq logic is inlined to avoid function
             * call overhead.  For 0-arg pure functions, the hash is a
             * compile-time constant and args_eq is a single nargs check. */
            if (__builtin_expect(saved_op == BC_CALL, 1) &&
                argc <= BC_MEMO_MAX_ARGS &&
                callee.tag == LR_TYPE_OBJECT) {
                LRObject *obj = (LRObject *)callee.u.ptr;
                if (__builtin_expect(obj->type == LR_OBJ_FUNCTION && obj->extra, 1)) {
                    ASTNode *func_ast = (ASTNode *)obj->extra;
                    BCProgram *bp = (BCProgram *)func_ast->bc_prog_cache;
                    if (!bp) bp = bc_get_or_compile_func(func_ast);
                    if (__builtin_expect(bp && bp->is_pure && bp->memo_state == 1, 1)) {
                        /* Inlined hash: for 0 args, this is a constant */
                        uint64_t ah = 0x9e3779b97f4a7c15ULL;
                        int nargs = (int)argc;
                        for (int hi = 0; hi < nargs; hi++) {
                            LRValue hv = argv[hi];
                            uint64_t hx;
                            switch (hv.tag) {
                            case LR_TYPE_INT32:     hx = ((uint64_t)(uint32_t)hv.u.int32 << 1) | 1; break;
                            case LR_TYPE_FLOAT64: { double d = hv.u.float64; uint64_t b = 0; memcpy(&b, &d, 8); hx = b | 1; break; }
                            case LR_TYPE_BOOL:      hx = hv.u.bool_val ? 3 : 2; break;
                            case LR_TYPE_UNDEFINED: hx = 4; break;
                            case LR_TYPE_NULL:      hx = 5; break;
                            default:                goto memo_fastpath_miss;
                            }
                            ah ^= hx + 0x9e3779b97f4a7c15ULL + (ah << 6) + (ah >> 2);
                        }
                        if (ah == 0) ah = 1;
                        {
                        int ms = (int)(ah & (BC_MEMO_CACHE_SIZE - 1));
                        BCMemoCacheEnt *me = &bc_memo_cache[ms];
                        /* Inlined args_eq: for 0 nargs, just check nargs==0 */
                        int args_match = (me->nargs == nargs);
                        if (args_match && nargs > 0) {
                            for (int ei = 0; ei < nargs; ei++) {
                                LRValue a = me->args[ei], b = argv[ei];
                                if (a.tag != b.tag) { args_match = 0; break; }
                                switch (a.tag) {
                                case LR_TYPE_INT32:   if (a.u.int32 != b.u.int32) args_match = 0; break;
                                case LR_TYPE_FLOAT64: if (a.u.float64 != b.u.float64) args_match = 0; break;
                                case LR_TYPE_BOOL:    if (a.u.bool_val != b.u.bool_val) args_match = 0; break;
                                default: break;
                                }
                                if (!args_match) break;
                            }
                        }
                        if (__builtin_expect(me->valid &&
                            me->prog_id == bp->prog_id &&
                            args_match, 1)) {
                            /* CACHE HIT: replay the result without any
                             * frame save, scope setup, or inline path. */
                            bc_memo_hit_count++;
                            bp->memo_miss_streak = 0;
                            LRValue rv = me->result;
                            for (int i = 0; i < (int)argc; i++) FREE_IF_HEAP(ctx, argv[i]);
                            FREE_IF_HEAP(ctx, key);
                            FREE_OBJ(ctx, callee); /* known to be an object */
                            FREE_IF_HEAP(ctx, this_val);
                            if (argv != argbuf) free(argv);
                            FREE_IF_HEAP(ctx, result);
                            result = LR_VALUE_UNDEFINED;
                            PUSH_FAST(rv);
                            DISPATCH();
                        }
                        }
                    }
                }
            }
            memo_fastpath_miss: ;

            /* C function fast path: skip call_frame + lr_call indirection */
            if (saved_op != BC_NEW && callee.tag == LR_TYPE_OBJECT) {
                LRObject *obj = (LRObject *)callee.u.ptr;
                if (obj->type == LR_OBJ_CFUNCTION && obj->extra) {
                    LRCFunction *cf = (LRCFunction *)obj->extra;
                    if (cf->func) {
                        if (this_val.tag == LR_TYPE_UNDEFINED)
                            this_val = lr_get_global_object(ctx);
                        ctx->current_func = callee;
                        r = cf->func(ctx, this_val, (int)argc, argv);
                        ctx->current_func = LR_VALUE_UNDEFINED;
                        if (lr_is_exception(r)) {
                            interp->exception_pending = 1;
                            interp->exception_value = lr_dup_value(ctx, r);
                            snprintf(interp->error_message, sizeof(interp->error_message),
                                     "%s", lr_get_exception_str(ctx));
                            interp->error_flag = 1;
                        }
                        goto call_cleanup;
                    }
                }
                /* -- EARLY memoization cache hit ----------------------
                 * Check the memoization cache BEFORE the full inline call
                 * setup (body_prog loading, JIT notification, body/params
                 * extraction, frame save).  This is the hottest path for
                 * tight loops calling pure functions — the cached result
                 * is returned in ~20 instructions vs. ~150 for the full
                 * inline path.  Only handles the active cache state
                 * (memo_state == 1); warmup and miss cases fall through
                 * to the normal inline path below.  The __builtin_expect
                 * hints tell the compiler that cache hits are the common
                 * case once the function is warmed up. */
                if (saved_op == BC_CALL &&
                    __builtin_expect(argc <= BC_MEMO_MAX_ARGS, 1) &&
                    obj->type == LR_OBJ_FUNCTION && obj->extra) {
                    ASTNode *func_ast = (ASTNode *)obj->extra;
                    BCProgram *bp = (BCProgram *)func_ast->bc_prog_cache;
                    if (__builtin_expect(bp != NULL, 1) &&
                        bp->is_pure && bp->memo_state == 1) {
                        uint64_t ah = bc_memo_hash_args(argv, (int)argc);
                        if (__builtin_expect(ah != 0, 1)) {
                            int ms = (int)(ah & (BC_MEMO_CACHE_SIZE - 1));
                            BCMemoCacheEnt *me = &bc_memo_cache[ms];
                            if (__builtin_expect(me->valid &&
                                me->prog_id == bp->prog_id &&
                                bc_memo_args_eq(me, argv, (int)argc), 1)) {
                                bc_memo_hit_count++;
                                bp->memo_miss_streak = 0;
                                LRValue rv = me->result;
                                for (int i = 0; i < (int)argc; i++)
                                    FREE_IF_HEAP(ctx, argv[i]);
                                FREE_IF_HEAP(ctx, key);
                                FREE_OBJ(ctx, callee); /* known to be an object */
                                FREE_IF_HEAP(ctx, this_val);
                                if (argv != argbuf) free(argv);
                                FREE_IF_HEAP(ctx, result);
                                result = LR_VALUE_UNDEFINED;
                                PUSH_FAST(rv);
                                DISPATCH();
                            }
                        }
                    }
                }

                /* Inline JS function call: bypass interp_call_function + bc_execute.
                 * The VM state is saved on a lightweight call stack, the callee's
                 * bytecode is dispatched directly, and BC_RETURN restores the
                 * caller's state — eliminating two C function calls, alloca,
                 * and cache re-initialization per call. */
                if (obj->type == LR_OBJ_FUNCTION && obj->extra &&
                    __builtin_expect(inline_call_depth < MAX_INLINE_CALL_DEPTH, 1) &&
                    (saved_op == BC_CALL || saved_op == BC_CALL_ELEM ||
                     saved_op == BC_CALL_METHOD)) {
                    ASTNode *func_ast = (ASTNode *)obj->extra;
                    InterpScope *closure_scope = (InterpScope *)obj->def_scope;

                    /* Extract the compiled callee body first; its cached
                     * can_inline/nparams flags (set once at compile time)
                     * replace the per-call AST walk that used to validate
                     * parameter shape and extract function metadata.
                     * The BCProgram is cached directly on the function AST
                     * node, so a warm call is a single load (no function
                     * call, no 4-entry inline-cache walk). */
                    BCProgram *body_prog = (BCProgram *)func_ast->bc_prog_cache;
                    if (!body_prog) body_prog = bc_get_or_compile_func(func_ast);

                    /* -- JIT hotness notification + compilation trigger --
                     * NOTE: The JIT fast path (goto jit_inline_fallback) has
                     * been removed: the inline path now handles memoization
                     * FIRST, then JIT execution via the inline path.  This
                     * keeps memoization and JIT working together — pure
                     * functions get both memo cache hits AND JIT-accelerated
                     * execution on cache misses.
                     *
                     * Notify the JIT runtime that this function was called.
                     * When the hot counter reaches LR_JIT_HOT_FUNC_CALLS,
                     * the function is compiled to native code immediately.  */
                    if (__builtin_expect(body_prog != NULL, 1)) {
                        LRRuntime *lr_rt = (LRRuntime *)interp->ctx->rt;
                        /* JIT notification: only call when the JIT is
                         * actually enabled.  lr_jit_notify_call walks a
                         * linked list (O(n) in the number of tracked
                         * programs), so skipping it when the JIT is
                         * disabled avoids significant overhead in tight
                         * loops.  __builtin_expect(..., 0) moves the
                         * entire JIT block out of the hot path.
                         * OPTIMIZATION: skip notification if already JIT-compiled
                         * (jit_entry non-NULL) — no need to walk the linked list. */
                        if (__builtin_expect(lr_rt && lr_rt->jit_runtime, 0)) {
                            LRJITRuntime *jit_rt = (LRJITRuntime *)lr_rt->jit_runtime;
                            if (__builtin_expect(jit_rt->enabled, 0)) {
                                if (body_prog && !body_prog->jit_skip &&
                                    !body_prog->jit_entry) {
                                    if (lr_jit_notify_call(interp, (LRProgram *)body_prog)) {
                                        if (!lr_jit_compile(interp, (LRProgram *)body_prog))
                                            body_prog->jit_skip = 1;
                                        else {
                                            if (__builtin_expect(pgo_dirty, 0))
                                                pgo_update(body_prog->prog_id, (uint8_t)argc);
                                            if (__builtin_expect(pgo_dirty, 0))
                                                pgo_update_spec(body_prog, (uint8_t)argc, argv);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (body_prog && body_prog->compiled &&
                        body_prog->can_inline && !body_prog->scans_arguments) {
                        int nparams = body_prog->nparams;

                        /* -- ICC: Inline Call Cache hit check --
                         * Fast-path for hot call sites: if the same function
                         * was called with the same arg count and type signature
                         * recently, skip full parameter validation. */
                        uint32_t ic_idx = ic_hash(body_prog->prog_id, (uintptr_t)ip) & ICC_MASK;
                        ICCEntry *ic_ent = &ic_cache[ic_idx];
                        if (body_prog->is_pure && saved_op == BC_CALL &&
                            argc <= BC_MEMO_MAX_ARGS &&
                            __builtin_expect(ic_match(ic_ent, (uint8_t)argc, argv,
                                                      body_prog->prog_id, (uintptr_t)ip), 0)) {
                            ic_hit_count++;
                            /* ICC hit: update stats and proceed to memo check */
                            ic_update(ic_ent, (uint8_t)argc, argv,
                                      body_prog->prog_id, (uintptr_t)ip);
                            if (__builtin_expect(pgo_dirty, 0)) {
                                pgo_update(body_prog->prog_id, (uint8_t)argc);
                                pgo_update_spec(body_prog, (uint8_t)argc, argv);
                            }
                        } else {
                            ic_miss_count++;
                        }

                        /* -- FAST PATH: memoization cache hit check --
                         * Check the memoization cache BEFORE extracting
                         * body/params from the AST.  On a cache hit, the
                         * cached result is returned immediately without
                         * any frame save, scope setup, or AST traversal.
                         * This is the dominant hot path for tight loops
                         * calling pure functions (e.g. empty() → 1).
                         * OPTIMIZATION: skip memo for JIT-compiled functions
                         * that are called with varying args (hot loop pattern). */
                        if (body_prog->is_pure &&
                            saved_op == BC_CALL &&
                            argc <= BC_MEMO_MAX_ARGS &&
                            body_prog->memo_state == 1 &&
                            !body_prog->jit_entry) { /* skip memo if JIT handles it */
                            uint64_t ah = bc_memo_hash_args(argv, (int)argc);
                            if (__builtin_expect(ah != 0, 1)) {
                                int ms = (int)(ah & (BC_MEMO_CACHE_SIZE - 1));
                                BCMemoCacheEnt *me = &bc_memo_cache[ms];
                                if (__builtin_expect(me->valid &&
                                    me->prog_id == body_prog->prog_id &&
                                    bc_memo_args_eq(me, argv, (int)argc), 1)) {
                                    /* CACHE HIT: replay the result without
                                     * any frame save or scope setup. */
                                    bc_memo_hit_count++;
                                    body_prog->memo_miss_streak = 0;
                                    if (__builtin_expect(pgo_dirty, 0)) {
                                        pgo_update(body_prog->prog_id, (uint8_t)argc);
                                        pgo_update_spec(body_prog, (uint8_t)argc, argv);
                                    }
                                    LRValue rv = me->result;
                                    for (int i = 0; i < (int)argc; i++)
                                        FREE_IF_HEAP(ctx, argv[i]);
                                    FREE_IF_HEAP(ctx, key);
                                    FREE_OBJ(ctx, callee); /* known to be an object */
                                    FREE_IF_HEAP(ctx, this_val);
                                    if (argv != argbuf) free(argv);
                                    FREE_IF_HEAP(ctx, result);
                                    result = LR_VALUE_UNDEFINED;
                                    PUSH_FAST(rv);
                                    DISPATCH();
                                }
                            }
                        }

                        ASTNode *body = body_prog->cached_body_ast;
                        ASTNode **params = body_prog->cached_params_ast;
                        if (!body) {
                            body = (func_ast->type == AST_FUNC_EXPR ||
                                    func_ast->type == AST_FUNC_DECL)
                                    ? func_ast->u.func.body
                                    : func_ast->u.arrow.body;
                            body_prog->cached_body_ast = body;
                            params = (func_ast->type == AST_FUNC_EXPR ||
                                      func_ast->type == AST_FUNC_DECL)
                                     ? func_ast->u.func.params
                                     : func_ast->u.arrow.params;
                            body_prog->cached_params_ast = params;
                        }
                        if (body) {
                            inline_cache_hit++;
                            inline_cache_inline++;

                            /* -- Warmup / miss handling ------------------
                             * When the cache is still warming up (memo_state
                             * == 0) or the cache probe missed, we need the
                             * full inline call path.  This runs only for the
                             * first few calls (WARMUP) and the first miss
                             * per distinct argument set. */
                            if (body_prog->is_pure &&
                                saved_op == BC_CALL &&
                                argc <= BC_MEMO_MAX_ARGS &&
                                body_prog->memo_state != 2 /* disabled */) {
                                uint8_t mst = body_prog->memo_state;
                                if (mst == 0) {
                                    if (++body_prog->memo_warmup < BC_MEMO_WARMUP) {
                                        goto memo_probe_skip_inline;
                                    }
                                    body_prog->memo_state = 1; /* active */
                                    body_prog->memo_miss_streak = 0;
                                }
                                {
                                uint64_t ah = bc_memo_hash_args(argv, (int)argc);
                                if (ah != 0) {
                                    int ms = (int)(ah & (BC_MEMO_CACHE_SIZE - 1));
                                    BCMemoCacheEnt *me = &bc_memo_cache[ms];
                                    /* CACHE MISS: reserve the slot.
                                     * BC_RETURN fills in the result. */
                                    bc_memo_miss_count++;
                                    if (__builtin_expect(pgo_dirty, 0)) {
                                        pgo_update(body_prog->prog_id, (uint8_t)argc);
                                        pgo_update_spec(body_prog, (uint8_t)argc, argv);
                                    }
                                    if (++body_prog->memo_miss_streak >=
                                        BC_MEMO_DISABLE_MISSES)
                                        body_prog->memo_state = 2; /* disable */
                                    cf_memo_pending_slot = (uint8_t)ms;
                                    cf_memo_pending_prog = body_prog->prog_id;
                                    me->prog_id = body_prog->prog_id;
                                    me->nargs = (uint8_t)argc;
                                    me->valid = 0;
                                    for (int i = 0; i < (int)argc; i++)
                                        me->args[i] = argv[i];
                                }
                                }
                                memo_probe_skip_inline: ;
                            }

                            /* -- Save state on call stack ---------------- */
                            struct InlineCallFrame *cf = &inline_call_stack[inline_call_depth++];
                            cf->ip = ip;
                            cf->prog = prog;
                            cf->sp = sp;
                            cf->scope_depth = scope_depth;
                            cf->memo_prog_id = cf_memo_pending_prog;
                            cf->memo_slot = cf_memo_pending_slot;
                            cf_memo_pending_prog = 0;
                            cf_memo_pending_slot = 0;
                            /* `result` is never restored from the frame —
                             * BC_RETURN overwrites it with the return value. */
                            cf->error_flag = interp->error_flag;
                            /* JIT fast-path: skip cold save if callee has JIT code.
                             * JIT-compiled functions don't modify interpreter state,
                             * so we only need to save sp, scope, and scope pointers. */
                            cf->jit_call = (body_prog && body_prog->jit_entry && !body_prog->touches_eval_node) ? 1 : 0;
                            if (cf->jit_call) {
                                /* JIT path: minimal save, no cold fields */
                                cf->cold_saved = 0;
                            } else if (body_prog->touches_eval_node) {
                                cf->break_target = interp->break_target;
                                cf->continue_target = interp->continue_target;
                                cf->return_target = interp->return_target;
                                cf->has_returned = interp->has_returned;
                                cf->return_value = interp->return_value;
                                cf->pending_label = interp->pending_label;
                                cf->cold_saved = 1;
                            } else {
                                cf->cold_saved = 0;
                            }
                            cf->current_scope = interp->current_scope;
                            /* Transfer ownership of the callee's POP'd stack
                             * reference to the frame.  It must stay alive while
                             * the callee's bytecode executes (the bytecode pool
                             * references AST strings owned by the function), and
                             * is released when the frame is popped (normal
                             * return / vm_done / vm_abort). */
                            cf->saved_callee = callee;

                            /* -- Reset interpreter flags ---------------- */
                            if (body_prog->touches_eval_node) {
                                interp->break_target = 0;
                                interp->continue_target = 0;
                                interp->pending_label = NULL;
                                interp->return_target = 0;
                                interp->has_returned = 0;
                                interp->return_value = LR_VALUE_UNDEFINED;
                            }
                            interp->error_flag = 0;
                            interp->exception_pending = 0;
                            interp->exception_value = LR_VALUE_UNDEFINED;

                            /* -- Scope optimization: skip scope creation
                             * for functions with no params, no locals, and
                             * no 'this' usage.  These are simple closures
                             * that only access variables from the parent
                             * scope — no function scope is needed.        */
                            /* Lazily fetch the global object as `this` only
                             * if the callee actually reads `this` (saved_op==
                             * BC_CALL with a deferred UNDEFINED this_val).
                             * Method/element calls already carry a real
                             * receiver, and functions that never read `this`
                             * skip the global refcount bump entirely. */
                            if (saved_op == BC_CALL &&
                                this_val.tag == LR_TYPE_UNDEFINED &&
                                body_prog->uses_this)
                                this_val = lr_get_global_object(ctx);
                            int no_scope = __builtin_expect(nparams == 0 &&
                                            body_prog->local_count == 0 &&
                                            !body_prog->uses_this, 1);
                            
                            if (no_scope) {
                                /* No scope needed: run directly in the
                                 * closure scope.  No 'this' binding, no
                                 * parameter binding, no cleanup needed. */
                                cf->func_scope = NULL;  /* signal: no scope */
                                cf->pure_call = 0;
                                /* Switch to the closure scope so the
                                 * function body can access captured
                                 * variables from the parent scope. */
                                interp->current_scope =
                                    closure_scope ? closure_scope : interp->current_scope;
                                /* this_val/argv[] were POP'd into locals
                                 * (owned refs) and, with no function scope,
                                 * are not bound anywhere.  The callee
                                 * (nparams==0, !uses_this) never reads them,
                                 * so release them now.  callee is owned by
                                 * cf->saved_callee and freed on frame pop. */
                                for (int i = 0; i < (int)argc; i++) FREE_IF_HEAP(ctx, argv[i]);
                                FREE_IF_HEAP(ctx, this_val);
                                FREE_IF_HEAP(ctx, key);
                                if (argv != argbuf) free(argv);
                            } else if (body_prog->is_pure) {
                                /* -- IOME586 pure fast scope -------------
                                 * A pure callee's scope can never escape
                                 * (no closures) and its body never does
                                 * name-based lookups, so only the values
                                 * that BC_LOAD_LOCAL reads are needed —
                                 * skip the names[]/is_const[]/is_lexical[]
                                 * writes of scope_new_inline_move.  Values
                                 * are MOVED in (no refcount bump); released
                                 * by scope_release_inline on return.
                                 * OPTIMIZATION: pass NULL for values and
                                 * populate directly — eliminates the
                                 * intermediate bn_v stack array + memcpy
                                 * from the hot inline-call path. */
                                int total = 1 + nparams;  /* "this" + params */
                                InterpScope *func_scope = scope_new_inline_fast(
                                    closure_scope ? closure_scope : interp->current_scope,
                                    total, NULL);
                                /* Populate values directly (no bn_v) */
                                func_scope->values[0] = this_val;
                                int i;
                                for (i = 0; i < nparams && i < (int)argc; i++)
                                    func_scope->values[i + 1] = argv[i];
                                for (; i < nparams; i++)
                                    func_scope->values[i + 1] = LR_VALUE_UNDEFINED;

                                cf->func_scope = func_scope;
                                cf->pure_call = 1;
                                interp->current_scope = func_scope;
                                /* Release unbound references (extra args) */
                                for (; i < (int)argc; i++) FREE_IF_HEAP(ctx, argv[i]);
                                FREE_IF_HEAP(ctx, key);
                                if (argv != argbuf) free(argv);
                            } else {
                                /* -- Optimized scope creation (inline) --
                                 * Uses scope_new_inline_move to create the
                                 * scope and populate all arrays in a single
                                 * call, eliminating nparams+1 function calls
                                 * to scope_declare_name_direct.  Ownership of
                                 * "this" + the bound parameter references is
                                 * MOVED into the scope (no per-arg refcount
                                 * bump); the scope releases them on
                                 * scope_release.  Extra args (beyond nparams)
                                 * and key are released here; callee is owned
                                 * by cf->saved_callee. */
                                int total = 1 + nparams;  /* "this" + params */
                                const char *snames[64];
                                LRValue     svals[64];
                                const char **bn_s = snames;
                                LRValue    *bn_v = svals;
                                if (total > 64) {
                                    bn_s = (const char **)malloc((size_t)total * sizeof(const char *));
                                    bn_v = (LRValue *)malloc((size_t)total * sizeof(LRValue));
                                }
                                /* [0] = "this" (const) */
                                bn_s[0] = "this";
                                bn_v[0] = this_val;
                                /* [1..nparams] = parameter values */
                                int i;
                                for (i = 0; i < nparams && i < (int)argc; i++) {
                                    bn_s[i + 1] = params[i]->u.ident.name;
                                    bn_v[i + 1] = argv[i];
                                }
                                for (; i < nparams; i++) {
                                    bn_s[i + 1] = params[i]->u.ident.name;
                                    bn_v[i + 1] = LR_VALUE_UNDEFINED;
                                }

                                InterpScope *func_scope = scope_new_inline_move(
                                    closure_scope ? closure_scope : interp->current_scope,
                                    total, bn_s, bn_v);
                                if (total > 64) { free(bn_s); free(bn_v); }

                                cf->func_scope = func_scope;
                                cf->pure_call = 0;
                                interp->current_scope = func_scope;

                                /* -- Release unbound references ----------
                                     * The scope now owns "this" + the first
                                     * min(nparams, argc) args (freed by
                                     * scope_release on return).  Extra args
                                     * and key are not bound to the callee
                                     * (which does not scan `arguments`), so
                                     * release them now. */
                                for (; i < (int)argc; i++) FREE_IF_HEAP(ctx, argv[i]);
                                FREE_IF_HEAP(ctx, key);
                                if (argv != argbuf) free(argv);
                            }

                            /* -- Switch to callee's bytecode ------------ */
                            ip = body_prog->code;
                            prog = body_prog;
                            BC_UPDATE_CACHE_PTRS();
                            code_end = prog->code + prog->code_len;
                            /* NOTE: sp is NOT reset to 0.  The callee
                             * executes on top of the caller's stack, so the
                             * caller's values below sp stay intact and no
                             * stack save/restore (memcpy) is required. */
                            scope_depth = 0;
                            FREE_IF_HEAP(ctx, result);
                            result = LR_VALUE_UNDEFINED;

                            /* -- Pre-populate variable cache for params --
                             * The variable cache uses var_depth (persistent
                             * across bc_execute() calls) and is validated at
                             * lookup time by walking the scope chain and
                             * checking names[slot].  By pre-populating the
                             * cache entries for parameter names, the first
                             * BC_LOAD_VAR for each parameter avoids the
                             * scope chain walk entirely from the hot path. */
                            if (nparams > 0 && body_prog->uses_name_ops) {
                                InterpScope *fs = cf->func_scope;
                                if (fs) {
                                    for (uint16_t pi = 0;
                                         pi < body_prog->pool_count; pi++) {
                                        if (body_prog->pool[pi].kind != BC_POOL_STRING) continue;
                                        const char *pn = body_prog->pool[pi].u.str;
                                        if (!pn) continue;
                                        for (int pj = 0; pj < nparams; pj++) {
                                            if (pn == params[pj]->u.ident.name ||
                                                (pn[0] == params[pj]->u.ident.name[0] &&
                                                 strcmp(pn, params[pj]->u.ident.name) == 0)) {
                                                int ci = pi & (BC_VAR_CACHE_SIZE - 1);
                                                bc_var_cache[ci].var_depth = 0; /* params are in current_scope */
                                                bc_var_cache[ci].name_key = (uint32_t)0 << 16 | pi;
                                                bc_var_cache[ci].slot = (uint16_t)(pj + 1);
                                                bc_var_cache[ci].name_ptr = fs->names[pj + 1];
                                                bc_var_cache[ci].is_const = 0;
                                                bc_var_cache[ci].is_declared = 1; /* params are declared bindings */
                                                goto next_param;
                                            }
                                        }
                                        next_param:;
                                    }
                                }
                            }
                            /* -- JIT execution in the inline path --------
                             * When native code is available for the callee,
                             * call it from within the inline path instead of
                             * dispatching bytecode.  This keeps memoization
                             * (checked above) and JIT working together:
                             * memo cache hits return immediately, misses
                             * execute via JIT, and the result is cached.
                             * The JIT code handles its own stack frame, so
                             * restore the inline call frame state on return
                             * (same cleanup as BC_RETURN).
                             * V8-inspired: use O(1) direct lookup via
                             * body_prog->jit_entry instead of linked-list walk. */
                            {
                                LRJITEntry jit_entry = (LRJITEntry)body_prog->jit_entry;
                                if (jit_entry) {
                                    InterpScope *jit_scope = interp->current_scope;
                                    LRValue jit_result;
                                    jit_entry(interp, body_prog->code, &jit_result);
                                    if (jit_result.tag != -1) {
                                        /* JIT succeeded: restore state
                                         * from the inline call frame.
                                         * NOTE: JIT already restored
                                         * interp->current_scope via
                                         * SAVED_SCOPE_OFFSET, so we just
                                         * copy back the saved fields. */
                                        struct InlineCallFrame *cf = &inline_call_stack[inline_call_depth - 1];
                                        interp->current_scope = cf->current_scope;
                                        /* Restore caller VM state */
                                        ip = cf->ip;
                                        prog = cf->prog;
                                        BC_UPDATE_CACHE_PTRS();
                                        code_end = prog->code + prog->code_len;
                                        sp = cf->sp;
                                        scope_depth = cf->scope_depth;
                                        /* JIT-compiled functions don't modify
                                         * interpreter state, so only restore
                                         * cold fields if they were saved. */
                                        if (cf->cold_saved) {
                                            interp->break_target = cf->break_target;
                                            interp->continue_target = cf->continue_target;
                                            interp->return_target = cf->return_target;
                                            interp->has_returned = cf->has_returned;
                                            interp->return_value = cf->return_value;
                                            interp->pending_label = cf->pending_label;
                                        }
                                        interp->error_flag = cf->error_flag;
                                        /* Fill memo cache if this was a
                                         * memo miss (prog_id != 0). */
                                        if (cf->memo_prog_id) {
                                            LRValue rv = jit_result;
                                            if (rv.tag == LR_TYPE_INT32 ||
                                                rv.tag == LR_TYPE_FLOAT64 ||
                                                rv.tag == LR_TYPE_BOOL ||
                                                rv.tag == LR_TYPE_UNDEFINED ||
                                                rv.tag == LR_TYPE_NULL) {
                                                BCMemoCacheEnt *me = &bc_memo_cache[cf->memo_slot];
                                                me->prog_id = cf->memo_prog_id;
                                                me->result = rv;
                                                me->valid = 1;
                                            }
                                        }
                                        /* Pop the inline call frame and
                                         * release the callee reference. */
                                        FREE_IF_HEAP(ctx, cf->saved_callee);
                                        inline_call_depth--;
                                        result = dup_value_fast(jit_result);
                                        PUSH_FAST(result);
                                        DISPATCH();
                                    }
                                    /* JIT bailed out (tag == -1): mark
                                     * as failed, clear jit_entry, and
                                     * fall through to bytecode interpreter. */
                                    {
                                        LRRuntime *lr_rt = (LRRuntime *)interp->ctx->rt;
                                        lr_jit_mark_bailout(
                                                (LRJITRuntime *)lr_rt->jit_runtime, body_prog);
                                        body_prog->jit_entry = NULL;
                                        /* Stop recompiling this program after a
                                         * bailout.  The hot-loop / call-site JIT
                                         * detector keeps recompiling a program
                                         * whose jit_entry was cleared, and every
                                         * recompile + re-bail leaks an LRJITCode
                                         * (no eviction in this cycle), eventually
                                         * corrupting the heap (double-free in GC).
                                         * A function that bailed out once is
                                         * generally not JIT-friendly, so fall back
                                         * to the interpreter permanently. */
                                        body_prog->jit_skip = 1;
                                        /* Restore caller's inline call frame state
                                         * so the fallback bytecode path can execute
                                         * the function correctly. */
                                        struct InlineCallFrame *cf = &inline_call_stack[inline_call_depth - 1];
                                        while (scope_depth > 0) {
                                            interp_bc_pop_scope(interp);
                                            scope_depth--;
                                        }
                                        if (cf->func_scope) {
                                            while (interp->current_scope &&
                                                   interp->current_scope != cf->func_scope) {
                                                interp_pop_scope(interp);
                                            }
                                            interp->current_scope = cf->current_scope;
                                            if (cf->pure_call)
                                                scope_release_inline(cf->func_scope, ctx);
                                            else
                                                scope_release(cf->func_scope, ctx);
                                        } else {
                                            interp->current_scope = cf->current_scope;
                                        }
                                        ip = cf->ip;
                                        prog = cf->prog;
                                        BC_UPDATE_CACHE_PTRS();
                                        code_end = prog->code + prog->code_len;
                                        sp = cf->sp;
                                        scope_depth = cf->scope_depth;
                                        if (cf->cold_saved) {
                                            interp->break_target = cf->break_target;
                                            interp->continue_target = cf->continue_target;
                                            interp->return_target = cf->return_target;
                                            interp->has_returned = cf->has_returned;
                                            interp->return_value = cf->return_value;
                                            interp->pending_label = cf->pending_label;
                                        }
                                        interp->error_flag = cf->error_flag;
                                        FREE_IF_HEAP(ctx, cf->saved_callee);
                                        inline_call_depth--;
                                        /* Continue to fallback bytecode execution */
                                        goto jit_bailout_to_bytecode;
                                    }
                                }
                            }
                            DISPATCH();
                        }
                    }
                }
                /* Fallback: interp_bc_call_function path (preserved for
                 * calls that cannot take the inline fast path, e.g. depth
                 * limit reached, uncached program, or JIT bailout above).  */
                {
jit_bailout_to_bytecode:
                    LRObject *fb_obj = (callee.tag == LR_TYPE_OBJECT) ? (LRObject *)callee.u.ptr : NULL;
                    if (!fb_obj || fb_obj->type != LR_OBJ_FUNCTION || !fb_obj->extra) {
                        /* Not a JS function: fall through to interp_bc_call */
                        goto call_not_function;
                    }
                    ASTNode *func_ast = (ASTNode *)fb_obj->extra;
                    InterpScope *closure_scope = (InterpScope *)fb_obj->def_scope;
                    /* -- IOME586 memo on the fallback path --------------
                     * Same pure-function result cache as the inline path:
                     * a pure callee (is_pure) with all-primitive args is
                     * probed first; on a hit the cached primitive result is
                     * replayed without executing the body.  This covers
                     * calls that cannot take the inline fast path (depth
                     * limit, uncached program), so memoization is not lost
                     * just because a call fell through. */
                    BCProgram *fb_prog = (BCProgram *)func_ast->bc_prog_cache;
                    if (!fb_prog) fb_prog = bc_get_or_compile_func(func_ast);
                    uint32_t fb_memo_prog = 0;
                    uint8_t  fb_memo_slot = 0;
                    if (fb_prog && fb_prog->is_pure &&
                        saved_op == BC_CALL && argc <= BC_MEMO_MAX_ARGS &&
                        fb_prog->memo_state != 2 /* disabled */) {
                        /* Same adaptive gating as the inline path. */
                        if (fb_prog->memo_state == 0) {
                            if (++fb_prog->memo_warmup < BC_MEMO_WARMUP)
                                goto fb_memo_skip;
                            fb_prog->memo_state = 1;
                            fb_prog->memo_miss_streak = 0;
                        }
                        {
                        uint64_t ah = bc_memo_hash_args(argv, (int)argc);
                        if (ah != 0) {
                            int ms = (int)(ah & (BC_MEMO_CACHE_SIZE - 1));
                            BCMemoCacheEnt *me = &bc_memo_cache[ms];
                            if (me->valid &&
                                me->prog_id == fb_prog->prog_id &&
                                bc_memo_args_eq(me, argv, (int)argc)) {
                                bc_memo_hit_count++;
                                r = me->result;   /* primitive copy */
                                goto call_cleanup;
                            }
                            /* Reserve the slot; fill it after the call. */
                            bc_memo_miss_count++;
                            if (++fb_prog->memo_miss_streak >=
                                BC_MEMO_DISABLE_MISSES)
                                fb_prog->memo_state = 2; /* disable */
                            fb_memo_prog = fb_prog->prog_id;
                            fb_memo_slot = (uint8_t)ms;
                            me->prog_id = fb_prog->prog_id;
                            me->nargs = (uint8_t)argc;
                            me->valid = 0;
                            for (int i = 0; i < (int)argc; i++)
                                me->args[i] = argv[i];
                        }
                        }
                        fb_memo_skip: ;
                    }
                    if (this_val.tag == LR_TYPE_UNDEFINED)
                        this_val = lr_get_global_object(ctx);
                    r = interp_bc_call_function(interp, func_ast, closure_scope,
                                                this_val, (int)argc, argv);
                    if (fb_memo_prog && !interp->error_flag &&
                        !interp->exception_pending &&
                        (r.tag == LR_TYPE_INT32 || r.tag == LR_TYPE_FLOAT64 ||
                         r.tag == LR_TYPE_BOOL || r.tag == LR_TYPE_UNDEFINED ||
                         r.tag == LR_TYPE_NULL)) {
                        BCMemoCacheEnt *me = &bc_memo_cache[fb_memo_slot];
                        me->prog_id = fb_memo_prog;
                        me->result = r;          /* primitive: no refcount */
                        me->valid = 1;
                    }
                    goto call_cleanup;
                }
            }
            call_not_function:
            if (saved_op == BC_NEW) r = interp_bc_construct(interp, callee, (int)argc, argv);
            else {
                if (callee.tag == 0) {
                    goto vm_abort;
                }
                if (this_val.tag == LR_TYPE_UNDEFINED)
                    this_val = lr_get_global_object(ctx);
                r = interp_bc_call(interp, callee, this_val, (int)argc, argv);
            }

        call_cleanup:
            for (int i = 0; i < (int)argc; i++) FREE_IF_HEAP(ctx, argv[i]);
            if (argv != argbuf) free(argv);
            FREE_IF_HEAP(ctx, key);
            FREE_IF_HEAP(ctx, callee);
            FREE_IF_HEAP(ctx, this_val);
            if (interp->error_flag || interp->exception_pending) {
                FREE_IF_HEAP(ctx, r);
                goto vm_abort;
            }
            PUSH_FAST(r);
            DISPATCH();
        }

        BC_CASE(return, BC_RETURN) {
            LRValue v = POP();
            FREE_IF_HEAP(ctx, result);
            result = v;
            /* Inline return: pop call stack and restore caller's state */
            if (inline_call_depth > saved_inline_call_depth) {
                struct InlineCallFrame *cf = &inline_call_stack[--inline_call_depth];
                if (cf->func_scope) {
                    /* -- Pop remaining block scopes -------------------
                     * Most functions have scope_depth == 0 on return,
                     * so use likely() to skip the loop entirely. */
                    if (__builtin_expect(scope_depth > 0, 0)) {
                        do {
                            interp_bc_pop_scope(interp);
                        } while (--scope_depth > 0);
                    }
                    /* -- Pop any scopes left above the function scope --
                     * Usually this loop does 0 iterations too. */
                    if (__builtin_expect(
                        interp->current_scope != cf->func_scope, 0)) {
                        while (interp->current_scope &&
                               interp->current_scope != cf->func_scope) {
                            interp_pop_scope(interp);
                        }
                    }
                    /* -- Restore scope chain ---------------------------- */
                    interp->current_scope = cf->current_scope;
                    if (cf->pure_call)
                        scope_release_inline(cf->func_scope, ctx);
                    else
                        scope_release(cf->func_scope, ctx);
                } else {
                    /* No-scope optimization: pop block scopes created by
                     * the callee's bytecode (e.g., function body block scope
                     * from BC_SCOPE_ENTER).  These are NOT the function scope
                     * (which was skipped), but block scopes inside the callee's
                     * bytecode stream that must be released before restoring
                     * the caller's scope chain.                           */
                    while (scope_depth > 0) {
                        interp_bc_pop_scope(interp);
                        scope_depth--;
                    }
                    interp->current_scope = cf->current_scope;
                }
                /* -- Restore saved state ------------------------------ */
                ip = cf->ip;
                prog = cf->prog;
                BC_UPDATE_CACHE_PTRS();
                code_end = prog->code + prog->code_len;
                sp = cf->sp;
                scope_depth = cf->scope_depth;
                /* Restore result (keep the return value in result) */
                /* NOTE: the caller's stack values below cf->sp were never
                 * disturbed (the callee ran on top of them), so there is
                 * nothing to restore here. */
                if (cf->cold_saved) {
                    interp->break_target = cf->break_target;
                    interp->continue_target = cf->continue_target;
                    interp->return_target = cf->return_target;
                    interp->has_returned = cf->has_returned;
                    interp->return_value = cf->return_value;
                    interp->pending_label = cf->pending_label;
                }
                interp->error_flag = cf->error_flag;
                /* -- IOME586 pure-function memo store ------------------
                 * If this frame was a memoized pure-call miss, fill the
                 * reserved cache slot with the (primitive) return value.
                 * Non-primitive results (objects/strings) are never cached
                 * — a pure function returning a fresh object each call must
                 * keep returning distinct objects, so replaying one would
                 * violate `f(x) === f(x)` semantics. */
                if (cf->memo_prog_id) {
                    LRValue rv = result;
                    if (rv.tag == LR_TYPE_INT32 || rv.tag == LR_TYPE_FLOAT64 ||
                        rv.tag == LR_TYPE_BOOL || rv.tag == LR_TYPE_UNDEFINED ||
                        rv.tag == LR_TYPE_NULL) {
                        BCMemoCacheEnt *me = &bc_memo_cache[cf->memo_slot];
                        me->prog_id = cf->memo_prog_id;
                        me->result = rv;      /* primitive: no refcount */
                        me->valid = 1;
                    }
                    /* else: leave valid=0 → this (prog_id, args) stays a
                     * miss and re-executes next time (correct). */
                }
                /* Release the callee's stack reference (kept alive during
                 * the callee's execution via cf->saved_callee). */
                FREE_IF_HEAP(ctx, cf->saved_callee);
                /* Push the return value */
                PUSH_FAST(dup_value_fast(result));
                DISPATCH();
            }
            goto vm_done;
        }

        BC_CASE(new_object, BC_NEW_OBJECT) PUSH_FAST(lr_new_object(ctx)); DISPATCH();
        BC_CASE(new_array, BC_NEW_ARRAY) {
            uint16_t n = rd16(&ip);
            if (sp < n) goto vm_abort;
            LRValue arr = lr_new_array(ctx);
            for (int i = (int)n - 1; i >= 0; i--) {
                LRValue v = POP();
                lr_set_property_uint32(ctx, arr, (uint32_t)i, v);
            }
            lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, (int32_t)n));
            PUSH_FAST(arr);
            DISPATCH();
        }
        BC_CASE(def_prop, BC_DEF_PROP) {
            uint16_t si = rd16(&ip);
            if (sp < 2) goto vm_abort;
            LRValue v = POP();
            lr_set_property_str(ctx, stack[sp - 1], prog->pool[si].u.str, v);
            DISPATCH();
        }
        BC_CASE(def_elem, BC_DEF_ELEM) {
            if (sp < 3) goto vm_abort;
            LRValue v = POP(), k = POP();
            LRString *atom = lr_to_atom(ctx, k);
            lr_set_property(ctx, stack[sp - 1], atom, v);
            FREE_IF_HEAP(ctx, k);
            DISPATCH();
        }
        BC_CASE(get_prop, BC_GET_PROP) {
            prop_si = rd16(&ip);
            prop_rcv = POP();
        get_prop_common:
            /* Shared by BC_LOAD_PROP (fused load+get) via goto.  Both
             * entry paths must have set prop_si and prop_rcv. */
            LRValue o = prop_rcv;

            /* Inline cache fast path: O(1) shape-based slot access.
             * Checked BEFORE the array-length fast path because the
             * common case is a plain-object property get, not a.length. */
            if (o.tag == LR_TYPE_OBJECT) {
                LRObject *obj = (LRObject *)o.u.ptr;
                if (obj->props && !obj->is_exotic &&
                    obj->type != LR_OBJ_PROXY) {
                    int ci = prop_si & (BC_IC_PROP_SIZE - 1);
                    BCICPropCache *cp = &bc_ic_prop[ci];
                    if (cp->valid && cp->shape == obj->shape &&
                        cp->shape_version == obj->shape->version &&
                        cp->name_idx == prop_si) {
                        LRValue v = dup_value_fast(obj->props[cp->slot]);
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(v);
                        DISPATCH();
                    }
                }
                /* Array .length fast path: direct access to dense array.
                 * Only reached after cache miss (common case is object). */
                if (obj->type == LR_OBJ_ARRAY && obj->extra) {
                    const char *pname = prog->pool[prop_si].u.str;
                    if (pname[0] == 'l' && pname[1] == 'e' &&
                        pname[2] == 'n' && pname[3] == 'g' &&
                        pname[4] == 't' && pname[5] == 'h' && !pname[6]) {
                        LRArrayData *ad = (LRArrayData *)obj->extra;
                        LRValue v; v.tag = LR_TYPE_INT32; v.u.int32 = (int32_t)ad->length;
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(v);
                        DISPATCH();
                    }
                }
            }

            /* -- IOME586 dynamic property cache probe ------------------
             * After the shape cache missed, try the identity-based dynamic
             * cache keyed by (obj_ptr, name_idx, mut_gen).  Hit when the
             * same object + property name + mutation generation was seen
             * before — i.e. no intervening write to the object.  Only
             * primitive results are cached (no refcount bookkeeping).    */
            if (o.tag == LR_TYPE_OBJECT) {
                LRObject *dyn_obj = (LRObject *)o.u.ptr;
                if (dyn_obj->type != LR_OBJ_PROXY) {
                    int dci = (((uintptr_t)dyn_obj >> 3) ^ prop_si) & (BC_DYN_CACHE_SIZE - 1);
                    BCDynCacheEnt *dc = &bc_dyn_cache[dci];
                    if (dc->valid && dc->obj == (void *)dyn_obj &&
                        dc->name_idx == prop_si &&
                        dc->mut_gen == dyn_obj->mut_gen) {
                        LRValue dv = dc->result;  /* primitive: copy */
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(dv);
                        DISPATCH();
                    }
                }
            }

            /* Slow path: full property lookup.
             * Create atom once and reuse for both lookup and cache population,
             * avoiding redundant allocation in lr_get_property_str. */
            {
                /* Save object pointer for dynamic cache invalidation below.
                 * When the object is freed by FREE_IF_HEAP and a new object
                 * is later allocated at the same address with the same
                 * mut_gen, the dynamic cache would return stale data. */
                void *saved_obj_ptr = NULL;
                uint16_t saved_prop_si = prop_si;
                if (o.tag == LR_TYPE_OBJECT) saved_obj_ptr = (void *)((LRObject *)o.u.ptr);

                LRString *atom = lr_new_atom(ctx, prog->pool[prop_si].u.str);
                LRValue v = lr_get_property(ctx, o, atom);

                /* Try to populate the shape cache for future accesses.
                 * Keyed by shape pointer so all objects with the same shape
                 * share the same cache entry. */
                if (o.tag == LR_TYPE_OBJECT && v.tag != LR_TYPE_UNDEFINED) {
                    LRObject *obj = (LRObject *)o.u.ptr;
                    if (obj->shape && obj->props && !obj->is_exotic &&
                        obj->type != LR_OBJ_PROXY) {
                        int slot = lr_shape_get_slot(obj, atom);
                        if (slot >= 0 && (uint32_t)slot < obj->prop_count) {
                            int ci = prop_si & (BC_IC_PROP_SIZE - 1);
                            bc_ic_prop[ci].shape = obj->shape;
                            bc_ic_prop[ci].shape_version = obj->shape->version;
                            bc_ic_prop[ci].prop_count = obj->prop_count;
                            bc_ic_prop[ci].name_idx = prop_si;
                            bc_ic_prop[ci].slot = (uint16_t)slot;
                            bc_ic_prop[ci].valid = 1;
                        }
                    }
                    /* Also populate the dynamic cache for primitive results */
                    if (v.tag == LR_TYPE_INT32 || v.tag == LR_TYPE_FLOAT64 ||
                        v.tag == LR_TYPE_BOOL || v.tag == LR_TYPE_UNDEFINED ||
                        v.tag == LR_TYPE_NULL) {
                        int dci = (((uintptr_t)obj >> 3) ^ prop_si) & (BC_DYN_CACHE_SIZE - 1);
                        BCDynCacheEnt *dc = &bc_dyn_cache[dci];
                        dc->obj = (void *)obj;
                        dc->name_idx = prop_si;
                        dc->mut_gen = obj->mut_gen;
                        dc->result = v;        /* primitive: no refcount */
                        dc->valid = 1;
                    }
                }

                FREE_IF_HEAP(ctx, o);

                /* Invalidate dynamic cache for this object pointer AFTER
                 * the object may have been freed.  If a new object is later
                 * allocated at the same address, the cache must not match
                 * the stale entry and return the wrong property value. */
                if (saved_obj_ptr) {
                    int dci = (((uintptr_t)saved_obj_ptr >> 3) ^ saved_prop_si) & (BC_DYN_CACHE_SIZE - 1);
                    BCDynCacheEnt *dc = &bc_dyn_cache[dci];
                    if (dc->valid && dc->obj == saved_obj_ptr && dc->name_idx == saved_prop_si)
                        dc->valid = 0;
                }

                CHECK();
                PUSH_FAST(v);
                DISPATCH();
            }
        }
        BC_CASE(load_prop, BC_LOAD_PROP) {
            /* Fused LOAD_LOCAL + GET_PROP: reads a local slot, does NOT
             * dup the receiver (the reference stays in the scope), then
             * performs the shape-cached property get.  Saves one refcount
             * inc + dec and a pop/push per property access on hot loops. */
            uint16_t slot = rd16(&ip);
            prop_si = rd16(&ip);
            InterpScope *scope = interp->current_scope;
            if (scope && slot < (uint16_t)scope->count) {
                prop_rcv = scope->values[slot];
                LRValue o = prop_rcv;
                if (o.tag == LR_TYPE_OBJECT) {
                    LRObject *obj = (LRObject *)o.u.ptr;
                    if (obj->props && !obj->is_exotic &&
                        obj->type != LR_OBJ_PROXY) {
                        int ci = prop_si & (BC_IC_PROP_SIZE - 1);
                        BCICPropCache *cp = &bc_ic_prop[ci];
                        if (cp->valid && cp->shape == obj->shape &&
                            cp->shape_version == obj->shape->version &&
                            cp->name_idx == prop_si) {
                            PUSH_FAST(dup_value_fast(obj->props[cp->slot]));
                            DISPATCH();
                        }
                    }
                    /* Array .length fast path. */
                    if (obj->type == LR_OBJ_ARRAY && obj->extra) {
                        const char *pname = prog->pool[prop_si].u.str;
                        if (pname[0] == 'l' && pname[1] == 'e' &&
                            pname[2] == 'n' && pname[3] == 'g' &&
                            pname[4] == 't' && pname[5] == 'h' && !pname[6]) {
                            LRArrayData *ad = (LRArrayData *)obj->extra;
                            LRValue v; v.tag = LR_TYPE_INT32; v.u.int32 = (int32_t)ad->length;
                            PUSH_FAST(v);
                            DISPATCH();
                        }
                    }
                    /* Slow path: hand the receiver to the shared
                     * GET_PROP logic (cache population + free).  The
                     * shared path frees the receiver, so take a reference
                     * first (net-zero vs the scope's own reference). */
                    prop_rcv = dup_value_fast(prop_rcv);
                    goto get_prop_common;
                }
                /* Non-object receiver: fall back to generic path. */
                prop_rcv = dup_value_fast(prop_rcv);
                goto get_prop_common;
            }
            PUSH_FAST(LR_VALUE_UNDEFINED);
            DISPATCH();
        }
        /* -- Fused LOAD_PROP + ADD ---------------------------------------
         * Emitted by the compiler peephole when BC_LOAD_PROP is immediately
         * followed by BC_ADD.  Saves one dispatch (and the associated
         * computed-goto + CHECK overhead) per property-add pair, which is
         * the dominant pattern in tight object-sum loops like
         *   s += o.a + o.b + o.c + ...
         * The handler tries the shape-cache hit + int32/int32 ADD fast path
         * first.  If either fails, it falls back to LOAD_PROP + ADD via
         * the general path.                                                  */
        BC_CASE(load_prop_add, BC_LOAD_PROP_ADD) {
            uint16_t slot = rd16(&ip);
            uint16_t si = rd16(&ip);
            InterpScope *scope = interp->current_scope;
            LRValue o_lpadd = LR_VALUE_UNDEFINED;
            if (scope && slot < (uint16_t)scope->count)
                o_lpadd = scope->values[slot];
            if (o_lpadd.tag == LR_TYPE_OBJECT && sp >= 1) {
                LRObject *obj = (LRObject *)o_lpadd.u.ptr;
                if (obj->props && !obj->is_exotic && obj->type != LR_OBJ_PROXY) {
                    int ci = si & (BC_IC_PROP_SIZE - 1);
                    BCICPropCache *cp = &bc_ic_prop[ci];
                    if (cp->valid && cp->shape == obj->shape &&
                        cp->shape_version == obj->shape->version &&
                        cp->name_idx == si) {
                        /* The accumulator is the top of stack.  We replace
                         * it in place with acc + pv — never pop it, and
                         * never free pv (it is borrowed from the object's
                         * property array, not owned by this handler). */
                        LRValue pv = obj->props[cp->slot];
                        LRValue acc = stack[sp - 1];
                        /* int32+int32 fast path */
                        if (acc.tag == LR_TYPE_INT32 && pv.tag == LR_TYPE_INT32) {
                            int64_t sum = (int64_t)acc.u.int32 + (int64_t)pv.u.int32;
                            LRValue r;
                            if (sum >= INT32_MIN && sum <= INT32_MAX) {
                                r.tag = LR_TYPE_INT32; r.u.int32 = (int32_t)sum;
                            } else {
                                r.tag = LR_TYPE_FLOAT64; r.u.float64 = (double)sum;
                            }
                            stack[sp - 1] = r;
                            DISPATCH();
                        }
                        /* float64+float64 */
                        if (acc.tag == LR_TYPE_FLOAT64 && pv.tag == LR_TYPE_FLOAT64) {
                            stack[sp - 1].u.float64 = acc.u.float64 + pv.u.float64;
                            DISPATCH();
                        }
                        /* mixed int32+float64 */
                        if (acc.tag == LR_TYPE_INT32 && pv.tag == LR_TYPE_FLOAT64) {
                            stack[sp - 1].tag = LR_TYPE_FLOAT64;
                            stack[sp - 1].u.float64 = (double)acc.u.int32 + pv.u.float64;
                            DISPATCH();
                        }
                        if (acc.tag == LR_TYPE_FLOAT64 && pv.tag == LR_TYPE_INT32) {
                            stack[sp - 1].u.float64 = acc.u.float64 + (double)pv.u.int32;
                            DISPATCH();
                        }
                        /* string or other: pop the accumulator and use
                         * bcv_binop.  pv stays borrowed (do not free). */
                        {
                            LRValue a = POP();
                            LRValue r = bcv_binop(interp, BC_ADD, a, pv);
                            FREE_IF_HEAP(ctx, a);
                            if (interp->error_flag) { FREE_IF_HEAP(ctx, r); goto vm_abort; }
                            PUSH_FAST(r);
                            CHECK();
                            DISPATCH();
                        }
                    }
                }
            }
            /* Fallback: push receiver, get property, then ADD */
            if (scope && slot < (uint16_t)scope->count)
                PUSH_FAST(dup_value_fast(scope->values[slot]));
            else
                PUSH_FAST(LR_VALUE_UNDEFINED);
            {
                const char *pname = prog->pool[si].u.str;
                if (pname) {
                    LRValue pv = lr_get_property_str(ctx, stack[sp-1], pname);
                    FREE_IF_HEAP(ctx, stack[sp-1]);
                    stack[sp-1] = pv;
                }
                /* Populate shape cache on miss so the fast path above can
                 * hit on subsequent iterations.  Without this, the fused
                 * LOAD_PROP_ADD would fall through to the slow path on
                 * EVERY access (cache never populated), negating the whole
                 * shape-cache design for the dominant hot loop pattern. */
                if (o_lpadd.tag == LR_TYPE_OBJECT) {
                    LRObject *obj = (LRObject *)o_lpadd.u.ptr;
                    if (obj->shape && obj->props && !obj->is_exotic &&
                        obj->type != LR_OBJ_PROXY) {
                        int slot = lr_shape_get_slot(obj,
                            lr_new_atom(ctx, prog->pool[si].u.str));
                        if (slot >= 0 && (uint32_t)slot < obj->prop_count) {
                            int ci = si & (BC_IC_PROP_SIZE - 1);
                            bc_ic_prop[ci].shape = obj->shape;
                            bc_ic_prop[ci].shape_version = obj->shape->version;
                            bc_ic_prop[ci].prop_count = obj->prop_count;
                            bc_ic_prop[ci].name_idx = si;
                            bc_ic_prop[ci].slot = (uint16_t)slot;
                            bc_ic_prop[ci].valid = 1;
                        }
                    }
                }
                if (sp >= 2) {
                    LRValue b = POP(), a = POP();
                    LRValue r = bcv_binop(interp, BC_ADD, a, b);
                    FREE_IF_HEAP(ctx, a);
                    FREE_IF_HEAP(ctx, b);
                    if (interp->error_flag) { FREE_IF_HEAP(ctx, r); goto vm_abort; }
                    PUSH_FAST(r);
                }
            }
            CHECK();
            DISPATCH();
        }
        BC_CASE(set_prop, BC_SET_PROP) {
            uint16_t si = rd16(&ip);
            if (sp < 2) goto vm_abort;
            LRValue v = POP(), o = POP();

            /* Shape-based cache fast path: O(1) slot-based property write */
            if (o.tag == LR_TYPE_OBJECT) {
                LRObject *obj = (LRObject *)o.u.ptr;
                if (obj->props && !obj->is_exotic &&
                    obj->type != LR_OBJ_PROXY) {
                    /* Shape-based cache: O(1) lookup, hit across objects
                     * sharing the same shape (hidden class). */
                    int ci = si & (BC_IC_PROP_SIZE - 1);
                    BCICPropCache *cp = &bc_ic_prop[ci];
                    if (cp->valid && cp->shape == obj->shape &&
                        cp->shape_version == obj->shape->version &&
                        cp->name_idx == si) {
                        lr_free_value(ctx, obj->props[cp->slot]);
                        obj->props[cp->slot] = dup_value_fast(v);
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(v);
                        DISPATCH();
                    }
                }
            }

            /* Slow path: full property set.
             * Create atom once and reuse for both set and cache population,
             * avoiding redundant allocation in lr_set_property_str. */
            {
                LRString *atom = lr_new_atom(ctx, prog->pool[si].u.str);
                lr_set_property(ctx, o, atom, dup_value_fast(v));

                /* Try to populate cache for future accesses.
                 * Keyed by shape pointer so all objects with the same shape
                 * share the same cache entry. */
                if (o.tag == LR_TYPE_OBJECT) {
                    LRObject *obj = (LRObject *)o.u.ptr;
                    if (obj->props && !obj->is_exotic &&
                        obj->type != LR_OBJ_PROXY && obj->shape) {
                        int slot = lr_shape_get_slot(obj, atom);
                        if (slot >= 0 && (uint32_t)slot < obj->prop_count) {
                            int ci = si & (BC_IC_PROP_SIZE - 1);
                            bc_ic_prop[ci].shape = obj->shape;
                            bc_ic_prop[ci].shape_version = obj->shape->version;
                            bc_ic_prop[ci].prop_count = obj->prop_count;
                            bc_ic_prop[ci].name_idx = si;
                            bc_ic_prop[ci].slot = (uint16_t)slot;
                            bc_ic_prop[ci].valid = 1;
                        }
                    }
                }
            }

            FREE_IF_HEAP(ctx, o);
            PUSH_FAST(v);
            DISPATCH();
        }
        BC_CASE(get_elem, BC_GET_ELEM) {
            if (sp < 2) goto vm_abort;
            LRValue k = POP(), o = POP();
            LRValue v;
            /* Dense array fast path: direct access to flat elements array */
            if (k.tag == LR_TYPE_INT32 && k.u.int32 >= 0 &&
                o.tag == LR_TYPE_OBJECT) {
                LRObject *obj = (LRObject *)o.u.ptr;
                if (obj->type == LR_OBJ_ARRAY && obj->extra) {
                    LRArrayData *ad = (LRArrayData *)obj->extra;
                    uint32_t idx = (uint32_t)k.u.int32;
                    if (idx < ad->length) {
                        v = dup_value_fast(ad->elements[idx]);
                        FREE_IF_HEAP(ctx, k);
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(v);
                        DISPATCH();
                    }
                }
            }
            /* Integer key fallback: use uint32 access (avoids atom creation) */
            if (k.tag == LR_TYPE_INT32 && k.u.int32 >= 0) {
                v = lr_get_property_uint32(ctx, o, (uint32_t)k.u.int32);
            } else {
                LRString *atom = lr_to_atom(ctx, k);
                /* Element cache: O(1) hit for repeated string-key accesses */
                int cache_hit = 0;
                if (o.tag == LR_TYPE_OBJECT && atom) {
                    LRObject *obj = (LRObject *)o.u.ptr;
                    if (obj->type != LR_OBJ_PROXY) {
                        /* Direct-mapped hash: O(1) lookup */
                        int ci = ((uintptr_t)obj ^ (uintptr_t)atom) & (BC_ELEM_CACHE_SIZE - 1);
                        BCElemCacheEnt *ec = &bc_elem_cache[ci];
                        if (ec->valid && ec->obj == obj &&
                            ec->key == atom &&
                            ec->obj_gen == obj->prop_count) {
                            v = dup_value_fast(ec->value);
                            cache_hit = 1;
                        }
                    }
                }
                if (!cache_hit) {
                    v = lr_get_property(ctx, o, atom);
                    /* Populate cache for future accesses */
                    if (o.tag == LR_TYPE_OBJECT && v.tag != LR_TYPE_UNDEFINED) {
                        LRObject *obj = (LRObject *)o.u.ptr;
                        if (obj->type != LR_OBJ_PROXY && atom) {
                            int ci = ((uintptr_t)obj ^ (uintptr_t)atom) & (BC_ELEM_CACHE_SIZE - 1);
                            /* Free old cached value if overwriting */
                            if (bc_elem_cache[ci].valid)
                                lr_free_value(ctx, bc_elem_cache[ci].value);
                            bc_elem_cache[ci].obj = obj;
                            bc_elem_cache[ci].key = atom;
                            bc_elem_cache[ci].value = dup_value_fast(v);
                            bc_elem_cache[ci].obj_gen = obj->prop_count;
                            bc_elem_cache[ci].valid = 1;
                        }
                    }
                }
            }
            FREE_IF_HEAP(ctx, k);
            FREE_IF_HEAP(ctx, o);
            CHECK();
            PUSH_FAST(v);
            DISPATCH();
        }
        BC_CASE(set_elem, BC_SET_ELEM) {
            if (sp < 3) goto vm_abort;
            LRValue v = POP(), k = POP(), o = POP();
            /* Dense array fast path: direct write to flat elements array */
            if (k.tag == LR_TYPE_INT32 && k.u.int32 >= 0 &&
                o.tag == LR_TYPE_OBJECT) {
                LRObject *obj = (LRObject *)o.u.ptr;
                if (obj->type == LR_OBJ_ARRAY && obj->extra) {
                    LRArrayData *ad = (LRArrayData *)obj->extra;
                    uint32_t idx = (uint32_t)k.u.int32;
                    if (idx < ad->length) {
                        lr_free_value(ctx, ad->elements[idx]);
                        ad->elements[idx] = dup_value_fast(v);
                        obj->mut_gen++;  /* IOME586: mutation version bump */
                        FREE_IF_HEAP(ctx, k);
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(v);
                        DISPATCH();
                    }
                    /* If index == length, we need to grow and set */
                    if (idx == ad->length) {
                        if (ad->capacity <= idx) {
                            uint32_t ncap = ad->capacity ? ad->capacity * 2 : 4;
                            while (ncap <= idx) ncap *= 2;
                            LRValue *ne = (LRValue *)realloc(ad->elements, sizeof(LRValue) * ncap);
                            if (!ne) goto vm_abort;
                            ad->elements = ne;
                            ad->capacity = ncap;
                        }
                        ad->elements[idx] = dup_value_fast(v);
                        ad->length = idx + 1;
                        obj->mut_gen++;  /* IOME586: mutation version bump */
                        FREE_IF_HEAP(ctx, k);
                        FREE_OBJ(ctx, o);
                        PUSH_FAST(v);
                        DISPATCH();
                    }
                }
            }
            /* Integer key fallback: use uint32 access (avoids atom creation) */
            if (k.tag == LR_TYPE_INT32 && k.u.int32 >= 0) {
                lr_set_property_uint32(ctx, o, (uint32_t)k.u.int32, dup_value_fast(v));
            } else {
                LRString *atom = lr_to_atom(ctx, k);
                lr_set_property(ctx, o, atom, dup_value_fast(v));
            }
            FREE_IF_HEAP(ctx, k);
            FREE_IF_HEAP(ctx, o);
            PUSH_FAST(v);
            DISPATCH();
        }
        BC_CASE(delete_prop, BC_DELETE_PROP) {
            uint16_t si = rd16(&ip);
            LRValue o = POP();
            LRString *atom = lr_new_atom(ctx, prog->pool[si].u.str);
            lr_delete_property(ctx, o, atom, 0);
            FREE_IF_HEAP(ctx, o);
            LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = 1;
            PUSH_FAST(_v);
            DISPATCH();
        }
        BC_CASE(delete_elem, BC_DELETE_ELEM) {
            if (sp < 2) goto vm_abort;
            LRValue k = POP(), o = POP();
            LRString *atom = lr_to_atom(ctx, k);
            lr_delete_property(ctx, o, atom, 0);
            FREE_IF_HEAP(ctx, k);
            FREE_IF_HEAP(ctx, o);
            LRValue _v; _v.tag = LR_TYPE_BOOL; _v.u.bool_val = 1;
            PUSH_FAST(_v);
            DISPATCH();
        }

        BC_CASE(iter_init, BC_ITER_INIT) {
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
                    /* src and iter may be the same object (e.g. generators
                     * return `this` from Symbol.iterator).  Only free src
                     * when it is a distinct object; otherwise the call
                     * above already owns the sole reference and pushing
                     * src below re-establishes the stack reference. */
                    int src_is_iter = (src.tag == LR_TYPE_OBJECT &&
                                       iter.tag == LR_TYPE_OBJECT &&
                                       src.u.ptr == iter.u.ptr);
                    if (!src_is_iter) FREE_IF_HEAP(ctx, src);
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
            PUSH_FAST(src);
            PUSH_FAST(nextfn);
            LRValue _iv; _iv.tag = LR_TYPE_INT32; _iv.u.int32 = index;
            PUSH_FAST(_iv);
            DISPATCH();
        }
        BC_CASE(iter_next, BC_ITER_NEXT) {
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
                    PUSH_FAST(lr_new_string(ctx, buf));
                } else {
                    int32_t len = 0;
                    LRValue lv = lr_get_property_str(ctx, src, "length");
                    lr_to_int32(ctx, &len, lv);
                    lr_free_value(ctx, lv);
                    if (index >= len) { ip += off; DISPATCH(); }
                    LRValue item = lr_get_property_uint32(ctx, src, (uint32_t)index);
                    stack[sp - 1].u.int32 = index + 1;
                    PUSH_FAST(item);
                }
            } else {
                LRValue nextfn = stack[sp - 2];
                LRValue iter_obj = stack[sp - 3];
                LRValue nr = interp_bc_call(interp, nextfn, iter_obj, 0, NULL);
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
                PUSH_FAST(v);
            }
            DISPATCH();
        }
        BC_CASE(iter_close, BC_ITER_CLOSE) {
            if (sp < 3) goto vm_abort;
            LRValue idx = POP(), nf = POP(), src = POP();
            (void)idx;
            FREE_IF_HEAP(ctx, nf);
            FREE_IF_HEAP(ctx, src);
            DISPATCH();
        }

        BC_CASE(scope_enter, BC_SCOPE_ENTER) interp_bc_push_scope(interp); scope_depth++; DISPATCH();
        BC_CASE(scope_leave, BC_SCOPE_LEAVE)
            if (scope_depth > 0) { interp_bc_pop_scope(interp); scope_depth--; }
            DISPATCH();

        BC_CASE(eval_node, BC_EVAL_NODE) {
            uint16_t si = rd16(&ip);
            /* -- Save caches before tree-walker call ----------------
             * The tree-walking interpreter (interp_bc_eval_node) may
             * invoke bc_execute() again via BC_EVAL_NODE or interp_bc_call,
             * which corrupts these thread-local caches.  Save and restore
             * to prevent the outer program's cached lookups from being
             * overwritten by the nested execution.                        */
            if (bc_saved_cache_depth < 64) {
                BCCacheSnapshot *snap = &bc_saved_cache_stack[bc_saved_cache_depth++];
                memcpy(snap->ic_data, bc_ic_prop, sizeof(BCICPropCache) * BC_IC_PROP_SIZE);
                memcpy(snap->var_data, bc_var_cache, sizeof(BCVarCacheEnt) * BC_VAR_CACHE_SIZE);
                memcpy(snap->elem_data, bc_elem_cache, sizeof(BCElemCacheEnt) * BC_ELEM_CACHE_SIZE);
                memcpy(snap->dyn_data, bc_dyn_cache, sizeof(BCDynCacheEnt) * BC_DYN_CACHE_SIZE);
                LRValue v = interp_bc_eval_node(interp, (ASTNode *)prog->pool[si].u.node);
                bc_saved_cache_depth--;
                snap = &bc_saved_cache_stack[bc_saved_cache_depth];
                memcpy(bc_ic_prop, snap->ic_data, sizeof(BCICPropCache) * BC_IC_PROP_SIZE);
                memcpy(bc_var_cache, snap->var_data, sizeof(BCVarCacheEnt) * BC_VAR_CACHE_SIZE);
                memcpy(bc_elem_cache, snap->elem_data, sizeof(BCElemCacheEnt) * BC_ELEM_CACHE_SIZE);
                memcpy(bc_dyn_cache, snap->dyn_data, sizeof(BCDynCacheEnt) * BC_DYN_CACHE_SIZE);
                CHECK_RET();
                CHECK_BREAK();
                if (interp->error_flag || interp->exception_pending) {
                    FREE_IF_HEAP(ctx, v);
                    goto vm_abort;
                }
                PUSH_FAST(v);
                DISPATCH();
            } else {
                /* Depth exceeded — fall back to direct eval without cache protection */
                LRValue v = interp_bc_eval_node(interp, (ASTNode *)prog->pool[si].u.node);
                CHECK_RET();
                CHECK_BREAK();
                if (interp->error_flag || interp->exception_pending) {
                    FREE_IF_HEAP(ctx, v);
                    goto vm_abort;
                }
                PUSH_FAST(v);
                DISPATCH();
            }
        }
        BC_CASE(eval_node_pop, BC_EVAL_NODE_POP) {
            uint16_t si = rd16(&ip);
            if (bc_saved_cache_depth < 64) {
                BCCacheSnapshot *snap = &bc_saved_cache_stack[bc_saved_cache_depth++];
                memcpy(snap->ic_data, bc_ic_prop, sizeof(BCICPropCache) * BC_IC_PROP_SIZE);
                memcpy(snap->var_data, bc_var_cache, sizeof(BCVarCacheEnt) * BC_VAR_CACHE_SIZE);
                memcpy(snap->elem_data, bc_elem_cache, sizeof(BCElemCacheEnt) * BC_ELEM_CACHE_SIZE);
                memcpy(snap->dyn_data, bc_dyn_cache, sizeof(BCDynCacheEnt) * BC_DYN_CACHE_SIZE);
                LRValue v = interp_bc_eval_node(interp, (ASTNode *)prog->pool[si].u.node);
                bc_saved_cache_depth--;
                snap = &bc_saved_cache_stack[bc_saved_cache_depth];
                memcpy(bc_ic_prop, snap->ic_data, sizeof(BCICPropCache) * BC_IC_PROP_SIZE);
                memcpy(bc_var_cache, snap->var_data, sizeof(BCVarCacheEnt) * BC_VAR_CACHE_SIZE);
                memcpy(bc_elem_cache, snap->elem_data, sizeof(BCElemCacheEnt) * BC_ELEM_CACHE_SIZE);
                memcpy(bc_dyn_cache, snap->dyn_data, sizeof(BCDynCacheEnt) * BC_DYN_CACHE_SIZE);
                FREE_IF_HEAP(ctx, v);
                CHECK_RET();
                CHECK_BREAK();
                CHECK();
                DISPATCH();
            } else {
                LRValue v = interp_bc_eval_node(interp, (ASTNode *)prog->pool[si].u.node);
                FREE_IF_HEAP(ctx, v);
                CHECK_RET();
                CHECK_BREAK();
                CHECK();
                DISPATCH();
            }
        }
        BC_CASE(set_result, BC_SET_RESULT) {
            LRValue v = POP();
            FREE_IF_HEAP(ctx, result);
            result = v;
            DISPATCH();
        }
        BC_CASE(clear_result, BC_CLEAR_RESULT)
            FREE_IF_HEAP(ctx, result);
            result = LR_VALUE_UNDEFINED;
            DISPATCH();

        BC_CASE(throw, BC_THROW) {
            LRValue v = POP();
            interp_bc_throw(interp, v);
            FREE_IF_HEAP(ctx, v);
            goto vm_abort;
        }

#if !LR_THREADED_CODE
        default:
            goto vm_abort;
        } /* switch */
    }   /* for(;;) */
#endif  /* !LR_THREADED_CODE */

vm_done:
    /* Unwind the inline call stack: if we hit a return at the top level
     * but there are inlined frames, we need to pop them.  This happens
     * when a return instruction is executed in a function that was called
     * inline — the BC_RETURN handler above handles the normal case, but
     * if we reach vm_done via vm_abort or the top-level return, we need
     * to unwind any remaining inlined frames. */
    while (inline_call_depth > saved_inline_call_depth) {
        struct InlineCallFrame *cf = &inline_call_stack[--inline_call_depth];
        if (cf->func_scope) {
            while (scope_depth > 0) {
                interp_bc_pop_scope(interp);
                scope_depth--;
            }
            while (interp->current_scope && interp->current_scope != cf->func_scope)
                interp_pop_scope(interp);
            interp->current_scope = cf->current_scope;
            scope_release(cf->func_scope, ctx);
        } else {
            /* No-scope: pop block scopes from callee's bytecode */
            while (scope_depth > 0) {
                interp_bc_pop_scope(interp);
                scope_depth--;
            }
            interp->current_scope = cf->current_scope;
        }
        /* Restore VM state, but DON'T push the return value (we're unwinding) */
        scope_depth = cf->scope_depth;
        sp = cf->sp;
        FREE_IF_HEAP(ctx, cf->saved_callee);
        /* NOTE: the caller's stack values below cf->sp were never disturbed
         * (the callee ran on top of them), so nothing needs restoring. */
        /* Don't restore ip — we're unwinding, not returning */
    }
    while (sp > 0) { LRValue v = stack[--sp]; FREE_IF_HEAP(ctx, v); }
    while (scope_depth-- > 0) interp_bc_pop_scope(interp);
    if (heap_stack) free(stack);
    bc_small_stack_depth--;
    inline_call_depth = saved_inline_call_depth;
    inline_cache_hit = 0; inline_cache_inline = 0; inline_cache_depth = 0;
    bc_memo_hit_count = 0; bc_memo_miss_count = 0;
    bc_inc_var_hit_total = 0; bc_inc_var_miss_total = 0; bc_inc_var_tier2_total = 0;
    bc_dump_pgo_stats();
    return result;

vm_abort:
#ifndef NDEBUG
    /* -- Debug mode: verbose diagnostic output ------------------------ */
    {
        static const char *op_names[BC_OPCODE_COUNT] = {
            "BC_STOP", "BC_NOP",
            "BC_PUSH_UNDEFINED", "BC_PUSH_NULL", "BC_PUSH_TRUE",
            "BC_PUSH_FALSE", "BC_PUSH_THIS", "BC_PUSH_INT32",
            "BC_PUSH_FLOAT64", "BC_PUSH_STRING",
            "BC_POP", "BC_DUP", "BC_DUP2", "BC_SWAP", "BC_ROT3",
            "BC_LOAD_VAR", "BC_STORE_VAR", "BC_LOAD_LOCAL", "BC_STORE_LOCAL", "BC_INC_LOCAL", "BC_INC_LOCAL_DISCARD", "BC_INC_VAR", "BC_DECLARE_VAR", "BC_TYPEOF_VAR", "BC_ADD_SELF", "BC_MUL_SELF",
            "BC_ADD", "BC_SUB", "BC_MUL", "BC_DIV", "BC_MOD", "BC_POW",
            "BC_LT", "BC_GT", "BC_LE", "BC_GE",
            "BC_EQ", "BC_NE", "BC_STRICT_EQ", "BC_STRICT_NE",
            "BC_SHL", "BC_SHR", "BC_SAR",
            "BC_BIT_AND", "BC_BIT_OR", "BC_BIT_XOR",
            "BC_IN", "BC_INSTANCEOF",
            "BC_NEG", "BC_POS", "BC_NOT", "BC_BIT_NOT", "BC_TYPEOF", "BC_VOID",
            "BC_JUMP", "BC_JUMP_IF_FALSE", "BC_JUMP_IF_TRUE",
            "BC_JUMP_IF_FALSE_KEEP", "BC_JUMP_IF_TRUE_KEEP", "BC_JUMP_IF_NOT_NULLISH",
            "BC_JUMP_IF_LOCAL_LT_IMM",
            "BC_LOOP_TICK",
            "BC_CALL", "BC_CALL_METHOD", "BC_CALL_ELEM", "BC_NEW", "BC_RETURN",
            "BC_NEW_OBJECT", "BC_NEW_ARRAY",
            "BC_DEF_PROP", "BC_DEF_ELEM", "BC_GET_PROP", "BC_LOAD_PROP", "BC_SET_PROP",
            "BC_GET_ELEM", "BC_SET_ELEM", "BC_DELETE_PROP", "BC_DELETE_ELEM",
            "BC_ITER_INIT", "BC_ITER_NEXT", "BC_ITER_CLOSE",
            "BC_SCOPE_ENTER", "BC_SCOPE_LEAVE",
            "BC_EVAL_NODE", "BC_EVAL_NODE_POP",
            "BC_SET_RESULT", "BC_CLEAR_RESULT", "BC_THROW",
            "BC_TO_STRING", "BC_TO_NUMBER", "BC_TO_BOOL"
        };
        const char *op_name = (ip >= prog->code && ip < code_end && *ip < BC_OPCODE_COUNT)
                              ? op_names[*ip] : "???";
        fprintf(stderr, "\n=== VM ABORT (DEBUG) ===\n");
        fprintf(stderr, "  IP offset : %td / %d\n", (ip - prog->code), prog->code_len);
        fprintf(stderr, "  Opcode    : %s (0x%02x)\n", op_name, (ip < code_end) ? *ip : 0);
        fprintf(stderr, "  Stack depth : %d\n", sp);
        fprintf(stderr, "  Scope depth : %d\n", scope_depth);
        fprintf(stderr, "  Flags:\n");
        fprintf(stderr, "    error_flag        : %s\n", interp->error_flag        ? "YES" : "no");
        fprintf(stderr, "    exception_pending : %s\n", interp->exception_pending ? "YES" : "no");
        fprintf(stderr, "    has_returned      : %s\n", interp->has_returned      ? "YES" : "no");
        fprintf(stderr, "    break_target      : %s\n", interp->break_target      ? "YES" : "no");
        fprintf(stderr, "    continue_target   : %s\n", interp->continue_target   ? "YES" : "no");
        if (interp->error_flag && interp->error_message[0])
            fprintf(stderr, "  Error message: %s\n", interp->error_message);
        if (interp->exception_pending && interp->exception_value.tag != LR_TYPE_UNDEFINED) {
            /* Attempt to print the exception string representation */
            const char *estr = lr_to_cstring(ctx, interp->exception_value);
            if (estr) { fprintf(stderr, "  Exception: %s\n", estr); lr_free_cstring(ctx, estr); }
        }
        /* Dump top N stack entries */
        int dump_n = (sp > 10) ? 10 : sp;
        if (dump_n > 0) {
            fprintf(stderr, "  Stack (top %d of %d):\n", dump_n, sp);
            for (int si = sp - dump_n; si < sp; si++) {
                const char *tag_names[] = {
                    "UNDEFINED", "NULL", "BOOL", "INT32", "FLOAT64",
                    "STRING", "OBJECT", "SYMBOL"
                };
                int tag = stack[si].tag;
                const char *tn = (tag >= 0 && tag < 8) ? tag_names[tag] : "UNKNOWN";
                fprintf(stderr, "    [%d] tag=%s", si, tn);
                if (tag == LR_TYPE_INT32)  fprintf(stderr, " val=%d", stack[si].u.int32);
                if (tag == LR_TYPE_FLOAT64)fprintf(stderr, " val=%f", stack[si].u.float64);
                if (tag == LR_TYPE_BOOL)   fprintf(stderr, " val=%s", stack[si].u.int32 ? "true" : "false");
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "=========================\n\n");
    }
#endif
    /* Unwind inline call stack on abort */
    while (inline_call_depth > saved_inline_call_depth) {
        struct InlineCallFrame *cf = &inline_call_stack[--inline_call_depth];
        while (scope_depth > 0) {
            interp_bc_pop_scope(interp);
            scope_depth--;
        }
        while (interp->current_scope && interp->current_scope != cf->func_scope)
            interp_pop_scope(interp);
        interp->current_scope = cf->current_scope;
        scope_release(cf->func_scope, ctx);
        scope_depth = cf->scope_depth;
        sp = cf->sp;
        FREE_IF_HEAP(ctx, cf->saved_callee);
    }
    while (sp > 0) { LRValue v = stack[--sp]; FREE_IF_HEAP(ctx, v); }
    while (scope_depth-- > 0) interp_bc_pop_scope(interp);
    if (heap_stack) free(stack);
    bc_small_stack_depth--;
    inline_call_depth = saved_inline_call_depth;
    FREE_IF_HEAP(ctx, result);
    return LR_VALUE_UNDEFINED;

#undef PUSH
#undef POP
#undef CHECK
#undef VM_GROW
}

/* ── Memo stats accessors (thread-local, safe to call from any thread) ── */
uint64_t lr_memo_hit_count(void) { return bc_memo_hit_count; }
uint64_t lr_memo_miss_count(void) { return bc_memo_miss_count; }

/* ── PGO + ICC stats dump (useful for profiling) ── */
void bc_dump_pgo_stats(void)
{
    if (!bc_env_debug_pgo) return;
    fprintf(stderr, "\n=== PGO + ICC Statistics ===\n");
    fprintf(stderr, "ICC: hits=%llu misses=%llu\n",
            (unsigned long long)ic_hit_count,
            (unsigned long long)ic_miss_count);
    int active_count = 0;
    for (int i = 0; i < PGO_HASH_SIZE; i++) {
        if (pgo_stats[i].prog_id != 0 && pgo_stats[i].call_count > 0) {
            fprintf(stderr, "  prog_id=%u calls=%llu avg_argc=%llu\n",
                    pgo_stats[i].prog_id,
                    (unsigned long long)pgo_stats[i].call_count,
                    (unsigned long long)pgo_stats[i].avg_argc);
            active_count++;
        }
    }
    fprintf(stderr, "Active PGO entries: %d\n", active_count);
    fprintf(stderr, "Memo: hit=%llu miss=%llu\n",
            (unsigned long long)bc_memo_hit_count,
            (unsigned long long)bc_memo_miss_count);
    fprintf(stderr, "============================\n\n");
}
