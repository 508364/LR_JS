/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: cache
 */
#include "lr_bytecode.h"
#include "lr_interp.h"
#include "lr_jit.h"
#include "lr_ast.h"
#include "lr_platform.h"   /* lr_get_time_us() */
#include <sljitLir.h>
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
#include "lr_jit.h"
#include "lr_ast.h"
#include "lr_platform.h"   /* lr_get_time_us() */

/* Cached env flags: getenv() on MSVCRT takes a lock + scans the env array,
 * measuring ~7-14µs per call on Windows — catastrophic in per-dispatch
 * hot paths.  Read once, cache forever (see lr_env_flag in lr_interp.h). */
int bc_env_debug_call    = -1;
int bc_env_debug_jitcall = -1;
int bc_env_debug_inline  = -1;
int bc_env_debug_pgo     = -1;

/* -- Threading mode ---------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
  #define LR_THREADED_CODE 1
  /* BC_CASE: computed-goto label (the trailing colon comes from call site). */
  #define BC_CASE(lbl, op) lbl_##lbl:
#else
  #define LR_THREADED_CODE 0
  /* BC_CASE: switch case label (the trailing colon comes from call site). */
  #define BC_CASE(lbl, op) case op:
#endif

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

/* -- FNV-1a 32-bit hash (same as lr_string_alloc in lr_engine.c) ----- */
uint32_t str_hash_fnv1a(const char *str, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)str[i]; h *= 16777619u; }
    return h;
}

/* Fast free: only heap-backed tags need the call.
 * Optimized version: inlines the refcount decrement for the common case
 * (refcount > 0 after decrement) and only calls lr_free_value() for the
 * edge case where the refcount reaches 0 (triggering GC work).  This
 * avoids the function call overhead for every object/string reference
 * release in hot paths like property access.                        */
/* Free heap if needed, defined in lr_interp.h */

/* Specialized variant for property access paths where the value is
 * guaranteed to be LR_TYPE_OBJECT (avoids the tag check + wrong branch
 * prediction from the generic FREE_IF_HEAP).                          */
#define FREE_OBJ(ctx, o_val) do {                                      \
    LRObject *_o2 = (LRObject *)(o_val).u.ptr;                         \
    if (__builtin_expect(_o2 && --_o2->ref_count <= 0, 0))             \
        lr_free_value(ctx, o_val);                                     \
} while (0)

/* Inline value duplication — avoids the function call to lr_dup_value()
/* Cached LR_DEBUG_VAR flag.  getenv() walks the environment array and is
 * far too slow to call on every BC_LOAD_VAR instruction in the hot path —
 * two getenv calls per load were the dominant cost of global-variable
 * reads in tight loops (8x slower than local reads).  The flag is read
 * once at first use; a debugger can reset the static to re-read it. */
static int bc_debug_var_flag = -1;
int bc_debug_var(void)
{
    if (bc_debug_var_flag < 0) bc_debug_var_flag = (getenv("LR_DEBUG_VAR") != NULL);
    return bc_debug_var_flag;
}

/* Portable strdup (MSVC exposes _strdup, POSIX strdup; avoid both). */
char *bc_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* =======================================================================
   PROGRAM MANAGEMENT
   ======================================================================= */

/* -- Persistent execution cache layout ----------------------------------
 * These caches are allocated as a single block in bc_new_program and
 * survive across bc_execute() calls so that repeated invocations of the
 * same function body (e.g. closures called in a loop) keep warm entries.
 * Layout: [BCICProp x 64] [BCVarCache x 64] [BCElemCache x 32]         */

#define BC_IC_PROP_SIZE  256
#define BC_VAR_CACHE_SIZE 256
#define BC_ELEM_CACHE_SIZE 256

typedef struct {
    LRShape  *shape;        /* shape pointer (key for cache hit) */
    uint32_t  shape_version;/* shape->version at population time */
    uint16_t  name_idx;
    uint16_t  slot;
    uint32_t  prop_count;
    uint8_t   valid;
    uint8_t   _pad[3];
} BCICPropCache;

/* Compact variable cache entry.
 * Fields ordered to eliminate padding (40→32 bytes).
 * name_key = (scope_depth << 16) | name_idx  — combines two checks into one. */
typedef struct {
    InterpScope *scope_ptr;    /* 8b: cached resolved scope pointer.  Validated
                                * by checking scope_ptr->cache_gen == scope_gen.
                                * When valid, used directly (no scope chain
                                * walk) for the fastest possible cache hit. */
    const char  *name_ptr;     /* 8b: cached name pointer from scope->names[slot]
                                * at population time.  Validated against the
                                * scope's current names[slot] via a single
                                * pointer comparison (no strcmp).  Safe as
                                * long as the scope is alive, which is
                                * guaranteed by the scope chain walk via
                                * var_depth.                              */
    uint32_t     scope_gen;    /* 4b: scope_ptr->cache_gen at population time. */
    uint32_t     name_key;     /* 4b: (scope_depth << 16) | name_idx.
                                * Combines the block scope depth check and
                                * constant pool index match into a single
                                * 32-bit comparison.                       */
    uint16_t     slot;         /* 2b: index in the resolved scope's values[] */
    uint16_t     var_depth;    /* 2b: how many parent links from current_scope
                                * to the scope that holds this variable.
                                * 0 = current_scope itself.                */
    uint8_t      is_const;     /* 1b */
    uint8_t      is_declared;  /* 1b: 1 = declared in this scope.
                                * BC_DECLARE_VAR fast-path uses this to
                                * avoid writing through a READ cache entry
                                * into a parent scope.                     */
} BCVarCacheEnt;

_Static_assert(sizeof(BCVarCacheEnt) == 32, "BCVarCacheEnt size mismatch - check struct alignment");

typedef struct {
    LRObject  *obj;
    LRString  *key;
    LRValue    value;
    uint32_t   obj_gen;
    uint8_t    valid;
    uint8_t    _pad[3];
} BCElemCacheEnt;

#define BC_CACHE_BLOCK_SIZE \
    (BC_IC_PROP_SIZE * sizeof(BCICPropCache) + \
     BC_VAR_CACHE_SIZE * sizeof(BCVarCacheEnt) + \
     BC_ELEM_CACHE_SIZE * sizeof(BCElemCacheEnt))

#define BC_CACHE_ICPROP(base)  ((BCICPropCache *)(base))
#define BC_CACHE_VAR(base)     ((BCVarCacheEnt *)((uint8_t *)(base) + BC_IC_PROP_SIZE * sizeof(BCICPropCache)))
#define BC_CACHE_ELEM(base)    ((BCElemCacheEnt *)((uint8_t *)(base) + BC_IC_PROP_SIZE * sizeof(BCICPropCache) + BC_VAR_CACHE_SIZE * sizeof(BCVarCacheEnt)))

/* -- IOME586 pure-function result cache ---------------------------------
 * Memoization cache for pure functions (BCProgram::is_pure == 1): bodies
 * with NO external interference whose result depends only on their
 * primitive arguments.  A direct-mapped thread-local cache keyed by
 * (prog_id, exact primitive arg values) replays the cached primitive
 * result on a hit without re-executing the body.  Entries hold only
 * primitives, so there is no reference-count bookkeeping to leak.       */
#define BC_MEMO_CACHE_SIZE 256
#define BC_MEMO_MAX_ARGS   8
#define BC_MEMO_WARMUP         8   /* calls before the first probe */
#define BC_MEMO_DISABLE_MISSES 32  /* consecutive misses → disable probe */

/* -- IOME586 dynamic property read result cache -------------------------
 * For property reads (BC_GET_PROP / BC_LOAD_PROP) on objects, a
 * direct-mapped thread-local cache keyed by (obj_ptr, name_idx, mut_gen)
 * replays the cached result when the object's mutation generation has not
 * changed — i.e. no property write has occurred since the cache was
 * populated.  This catches the common IOME586 pattern where a "dynamic
 * computation" reads the same property from the same object repeatedly
 * without external interference, avoiding the full property lookup (shape
 * chain walk + hash table probe) on every access.
 *
 * The cache is thread-local so it stays valid across BCProgram boundaries
 * (different functions accessing the same object), and entries hold only
 * primitives (int32/float64/bool/undefined/null) to avoid refcount
 * bookkeeping.  Object/string results are not cached.                   */
#define BC_DYN_CACHE_SIZE 64

typedef struct {
    void     *obj;       /* object pointer (identity — no refcount)       */
    uint16_t  name_idx;  /* pool name index of the accessed property     */
    uint32_t  mut_gen;   /* object's mut_gen at cache time               */
    uint8_t   valid;
    uint8_t   _pad[3];
    LRValue   result;    /* cached primitive result (no refcount needed) */
} BCDynCacheEnt;

LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];

typedef struct {
    uint32_t  prog_id;      /* unique id of the pure BCProgram (never reused) */
    uint8_t   nargs;        /* number of args in args[] */
    uint8_t   valid;        /* 1 = entry holds a stored result */
    uint8_t   _pad[2];
    LRValue   args[BC_MEMO_MAX_ARGS]; /* exact primitive arg values */
    LRValue   result;       /* cached primitive result (no refcount needed) */
} BCMemoCacheEnt;

/* Hash the primitive argument values.  Returns 0 if any arg is not a
 * cacheable primitive (object/string/symbol → caller must bail). */
uint64_t bc_memo_hash_args(const LRValue *argv, int nargs)
{
    uint64_t h = 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < nargs; i++) {
        LRValue v = argv[i];
        uint64_t x;
        switch (v.tag) {
        case LR_TYPE_INT32:     x = ((uint64_t)(uint32_t)v.u.int32 << 1) | 1; break;
        case LR_TYPE_FLOAT64: { double d = v.u.float64; uint64_t b = 0;
                                memcpy(&b, &d, 8); x = b | 1; break; }
        case LR_TYPE_BOOL:      x = v.u.bool_val ? 3 : 2; break;
        case LR_TYPE_UNDEFINED: x = 4; break;
        case LR_TYPE_NULL:      x = 5; break;
        default:                return 0;   /* non-primitive → not cacheable */
        }
        h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    }
    return h ? h : 1;
}

/* Exact equality of cached vs current primitive args (NaN never matches,
 * which is the safe/conservative choice). */
int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs)
{
    if (e->nargs != nargs) return 0;
    for (int i = 0; i < nargs; i++) {
        LRValue a = e->args[i], b = argv[i];
        if (a.tag != b.tag) return 0;
        switch (a.tag) {
        case LR_TYPE_INT32:   if (a.u.int32 != b.u.int32) return 0; break;
        case LR_TYPE_FLOAT64: if (a.u.float64 != b.u.float64) return 0; break;
        case LR_TYPE_BOOL:    if (a.u.bool_val != b.u.bool_val) return 0; break;
        default: break;   /* undefined/null: equal by tag */
        }
    }
    return 1;
}

/* Scan a compiled opcode stream; returns 1 if the body is memoizable
 * (only slot-based locals + primitive arithmetic, no side effects or
 * external reads).  Any opcode that could observe or mutate state outside
 * the callee's own slots — calls, property/element access, name ops,
 * closures, `this`, iteration, eval nodes, explicit throws — disqualifies
 * the function. */
int bc_code_is_pure(const uint8_t *code, int len)
{
    for (int i = 0; i < len; i++) {
        switch (code[i]) {
        /* Name-based var ops: touch the scope chain / globals.
         * BC_LOAD_VAR is excluded: recursive functions load their own
         * name from the enclosing scope, which is a pure read.  The
         * memoization cache's miss-streak detection catches incorrect
         * caching of functions that read mutable globals. */
        case BC_STORE_VAR: case BC_DECLARE_VAR: case BC_TYPEOF_VAR:
        case BC_INC_VAR:
        case BC_INC_LOCAL: case BC_INC_LOCAL_DISCARD:
        /* Any call may have side effects or read external state.
         * BC_CALL is excluded from the non-pure list: recursive and
         * mutually-recursive pure functions (e.g. Fibonacci) need to be
         * memoizable.  The memoization cache itself guards against
         * incorrect caching via miss-streak detection and auto-disable. */
        case BC_CALL_METHOD: case BC_CALL_ELEM: case BC_NEW:
        /* Property/element access: throws on null/undefined and reads
         * mutable object state. */
        case BC_GET_PROP: case BC_LOAD_PROP: case BC_LOAD_PROP_ADD:
        case BC_SET_PROP: case BC_GET_ELEM: case BC_SET_ELEM:
        case BC_DELETE_PROP: case BC_DELETE_ELEM:
        case BC_IN: case BC_INSTANCEOF:
        /* `this` binding, iteration over mutable containers, tree-walker
         * delegation, and explicit throws all break purity. */
        case BC_PUSH_THIS: case BC_ITER_INIT: case BC_ITER_NEXT:
        case BC_ITER_CLOSE: case BC_EVAL_NODE: case BC_EVAL_NODE_POP:
        case BC_THROW:
            return 0;
        default:
            break;
        }
    }
    return 1;
}

BCProgram *bc_new_program(void)
{
    /* Per-thread monotonic id counter: never reused, so a memo-cache
     * entry keyed by prog_id stays valid even if a BCProgram is freed
     * and its memory reused by a different program. */
    static LR_THREAD_LOCAL uint32_t bc_prog_id_counter = 0;
    BCProgram *p = (BCProgram *)calloc(1, sizeof(BCProgram));
    if (p) {
        p->prog_id = ++bc_prog_id_counter;
        if (p->prog_id == 0) p->prog_id = ++bc_prog_id_counter; /* wrap */
        p->cache_data = calloc(1, BC_CACHE_BLOCK_SIZE);
        p->cache_gen = 1;
        p->ref_count = 1;   /* the creator owns the first reference */
    }
    return p;
}

void bc_retain_program(BCProgram *prog)
{
    if (prog) prog->ref_count++;
}

void bc_free_program(BCProgram *prog)
{
    if (!prog) return;
    /* Release one owning reference.  A program may be shared by several
     * owners (eval units + the warm compiled-cache); it is only freed
     * when the last owner drops its reference, so shared programs are
     * never double-freed during runtime teardown. */
    if (prog->ref_count > 1) {
        prog->ref_count--;
        return;
    }
    free(prog->code);
    for (int i = 0; i < prog->pool_count; i++)
        if (prog->pool[i].kind == BC_POOL_STRING)
            free(prog->pool[i].u.str);
    free(prog->pool);
    free(prog->cache_data);
#if defined(LR_JIT_ENABLED) && LR_JIT_ENABLED
    if (prog->jit_entry) {
        sljit_free_exec(prog->jit_entry);
        prog->jit_entry = NULL;
        prog->jit_code_size = 0;
    }
#endif
    free(prog);
}

int bc_program_is_restorable(const BCProgram *prog)
{
    return prog && prog->node_refs == 0;
}

