/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: cache
 */
#ifndef LR_BYTECODE_CACHE
#define LR_BYTECODE_CACHE

  #include "lr_engine.h"
  #include "lr_platform.h"
  #include "lr_interp.h"
  #include "lr_ast.h"
  #include "lr_bytecode.h"

/* Dynamic property cache constants and types. */
#define BC_DYN_CACHE_SIZE 64

typedef struct {
    void     *obj;       /* object pointer (identity — no refcount)       */
    uint16_t  name_idx;  /* pool name index of the accessed property     */
    uint32_t  mut_gen;   /* object's mut_gen at cache time               */
    uint8_t   valid;
    uint8_t   _pad[3];
    LRValue   result;    /* cached primitive result (no refcount needed) */
} BCDynCacheEnt;

/* Thread-local dynamic property cache (defined in cache.c). */
extern LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];

/* Inline call cache types (used by exec.c). */
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

typedef struct {
    InterpScope *scope_ptr;
    const char  *name_ptr;
    uint32_t     scope_gen;
    uint32_t     name_key;
    uint16_t     slot;
    uint16_t     var_depth;
    uint8_t      is_const;
    uint8_t      is_declared;
} BCVarCacheEnt;

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

/* Memo cache constants (defined in cache.c, used by exec.c). */
#define BC_MEMO_CACHE_SIZE 256
#define BC_MEMO_MAX_ARGS   8
#define BC_MEMO_WARMUP         8
#define BC_MEMO_DISABLE_MISSES 32

typedef struct {
    uint32_t  prog_id;      /* unique id of the pure BCProgram (never reused) */
    uint8_t   nargs;        /* number of args in args[] */
    uint8_t   valid;        /* 1 = entry holds a stored result */
    uint8_t   _pad[2];
    LRValue   args[BC_MEMO_MAX_ARGS]; /* exact primitive arg values */
    LRValue   result;       /* cached primitive result (no refcount needed) */
} BCMemoCacheEnt;

/* Cached env debug flags (defined in cache.c). */
extern int bc_env_debug_call;
extern int bc_env_debug_jitcall;
extern int bc_env_debug_inline;
extern int bc_env_debug_pgo;     /* 1 if PGO stats should be dumped */

/* Cross-module helper functions (defined in cache.c). */
uint32_t str_hash_fnv1a(const char *str, size_t len);
int bc_debug_var(void);
char *bc_strdup(const char *s);
uint64_t bc_memo_hash_args(const LRValue *argv, int nargs);
int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs);
int bc_code_is_pure(const uint8_t *code, int len);

#ifdef __cplusplus
}
#endif
#endif /* LR_BYTECODE_CACHE */
