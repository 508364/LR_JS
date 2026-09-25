/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: internal — Shared declarations between cache, value, and exec.
 *
 * Symbols declared here are NOT part of the public API (no one outside the
 * bytecode module should include this header).  They exist solely so that
 * cache.c, value.c, and exec.c can share implementation details after the
 * monolithic lr_bytecode.c was split into separate translation units.
 */
#ifndef LR_BYTECODE_INTERNAL
#define LR_BYTECODE_INTERNAL

  #include "lr_engine.h"
  #include "lr_bytecode.h"
  #include "lr_bytecode_cache.h"

/* =======================================================================
   MODULE-LOCAL GLOBALS (defined in cache.c, declared here for exec.c)
   ======================================================================= */

/* Dynamic property cache (thread-local). */
extern LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];

/* Cached env debug flags. */
extern int bc_env_debug_call;
extern int bc_env_debug_jitcall;
extern int bc_env_debug_inline;
extern int bc_env_debug_pgo;

/* =======================================================================
   CACHE-CLASS HELPERS (defined in cache.c, used in exec.c)
   ======================================================================= */

uint32_t str_hash_fnv1a(const char *str, size_t len);
int bc_debug_var(void);
uint64_t bc_memo_hash_args(const LRValue *argv, int nargs);

/* =======================================================================
   VALUE-CLASS HELPERS (defined in value.c, used in exec.c)
   ======================================================================= */

double bcv_to_number(LRContext *ctx, LRValue v);
int32_t bcv_to_int32(LRContext *ctx, LRValue v);
LRValue bcv_number(LRContext *ctx, double d);
const char *bcv_typeof(LRContext *ctx, LRValue v);
LRValue bcv_binop(Interpreter *interp, int op, LRValue a, LRValue b);

/* =======================================================================
   INLINE HELPERS (used across cache, value, and exec modules)
   ======================================================================= */

/* Fast value duplication: bumps refcount for heap types, copies primitives. */
static inline LRValue dup_value_fast(LRValue v)
{
    if (v.tag == LR_TYPE_STRING) {
        LRString *s = (LRString *)v.u.ptr;
        if (s) s->ref_count++;
    } else if (v.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)v.u.ptr;
        if (obj) obj->ref_count++;
    } else if (v.tag == LR_TYPE_SYMBOL) {
        v.u.ptr = strdup((const char *)v.u.ptr);
    }
    return v;
}

/* =======================================================================
   MACROS (defined in cache.c, used in exec.c and value.c)
   ======================================================================= */

/* Specialized FREE_OBJ for when value is guaranteed LR_TYPE_OBJECT */
#define FREE_OBJ(ctx, o_val) do {                                      \
    LRObject *_o2 = (LRObject *)(o_val).u.ptr;                         \
    if (__builtin_expect(_o2 && --_o2->ref_count <= 0, 0))             \
        lr_free_value(ctx, o_val);                                     \
} while (0)

#ifdef __cplusplus
}
#endif
#endif /* LR_BYTECODE_INTERNAL */
