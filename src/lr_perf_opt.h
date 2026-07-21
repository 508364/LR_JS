/*
 * L/R_JS - Performance Optimization Subsystem
 * Pure C, advanced optimization techniques for the JS engine.
 *
 * Techniques:
 *   1. Bytecode Cache (Code Serialization)     - Cache compiled bytecode
 *   2. Compilation Cache (Hash-based)           - Avoid recompilation
 *   3. Concurrent GC (Background marking)       - Non-blocking GC
 *   4. Fast API Dispatch (Direct C calls)       - Minimal overhead C<->JS
 *   5. Atom Caching (Pre-computed property keys) - Fast property access
 *   6. Lazy Module Initialization               - Defer init until needed
 *   7. Incremental Parsing (Large script split)  - Parse in chunks
 */
#ifndef LR_PERF_OPT_H
#define LR_PERF_OPT_H

#include <stdint.h>
#include <stddef.h>
#include "lr_platform.h"
#include "engine/lr_engine.h"

/* Forward declarations */
typedef struct LR_Runtime    LR_Runtime;
typedef struct LR_PerfOptCtx LR_PerfOptCtx;

/* ── Bytecode Cache ────────────────────────────────────────────────────── */

/* Code Serializer: Serialize/Deserialize compiled code.
   Engine equivalent: JS_WriteObject / JS_ReadObject for bytecode. */

typedef struct LR_PerfBCEntry {
    char            *source_hash;     /* FNV-1a hash of source */
    uint8_t         *bytecode;        /* Serialized bytecode blob */
    size_t           bytecode_len;
    int64_t          created_at;
    int64_t          last_accessed;
    int              hit_count;
    struct LR_PerfBCEntry *next;
} LR_PerfBCEntry;

/* ── Compilation Cache ─────────────────────────────────────────────────── */

/* Compilation Cache: Hash-based cache for compiled scripts.
   Key = source hash, Value = compiled bytecode. */

#define LR_COMPILE_CACHE_BUCKETS 256

typedef struct LR_CompileCache {
    LR_PerfBCEntry *buckets[LR_COMPILE_CACHE_BUCKETS];
    int             entry_count;
    int             hit_count;
    int             miss_count;
    int             max_entries;
    pthread_mutex_t mutex;
} LR_CompileCache;

/* ── Concurrent GC ─────────────────────────────────────────────────────── */

/* Incremental Marking: Run GC in small steps in background threads.
   Engine: JS_RunGC in a dedicated background thread. */

typedef struct LR_ConcurrentGC {
    pthread_t         gc_thread;
    int               running;
    int               should_stop;
    int               gc_interval_ms;    /* How often to run GC */
    int               gc_threshold;      /* Memory threshold to trigger */
    int64_t           last_gc_time;
    int64_t           gc_count;
    pthread_mutex_t   mutex;
    pthread_cond_t    cond;
} LR_ConcurrentGC;

/* ── Fast API Dispatch ─────────────────────────────────────────────────── */

/* Fast API Call: Direct C function dispatch with typed signatures.
   We cache the function pointer to avoid JSCFunctionData lookup. */

typedef JSValue (*LR_FastCFunction)(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);

typedef struct LR_FastAPIEntry {
    const char       *name;
    LR_FastCFunction  func;
    int               length;
    int               hit_count;
    JSAtom            atom;         /* Cached atom for fast lookup */
} LR_FastAPIEntry;

/* Cache of frequently accessed built-in API functions */
#define LR_FAST_API_CACHE_SIZE 128

typedef struct LR_FastAPICache {
    LR_FastAPIEntry   entries[LR_FAST_API_CACHE_SIZE];
    int               entry_count;
    int               hit_count;
    int               miss_count;
} LR_FastAPICache;

/* ── Atom Cache ────────────────────────────────────────────────────────── */

/* Hidden classes (shape-based property access) use property indices for fast access.
   We pre-compute JSAtom values for all built-in property names. */

#define LR_ATOM_CACHE_SIZE 256

typedef struct LR_AtomEntry {
    const char *name;
    JSAtom      atom;
    int         initialized;
} LR_AtomEntry;

typedef struct LR_AtomCache {
    LR_AtomEntry  atoms[LR_ATOM_CACHE_SIZE];
    int           count;
    int           hit_count;
    int           miss_count;
} LR_AtomCache;

/* ── Master optimization context ───────────────────────────────────────── */

/* Performance optimization context for each runtime.
   Bundles all optimization subsystems together. */

struct LR_PerfOptCtx {
    /* Compilation cache */
    LR_CompileCache   compile_cache;

    /* Concurrent GC */
    LR_ConcurrentGC   concurrent_gc;

    /* Fast API cache */
    LR_FastAPICache   fast_api_cache;

    /* Atom cache */
    LR_AtomCache      atom_cache;

    /* Lazy init tracking */
    int               modules_initialized;
    int               modules_lazy;

    /* Stats */
    int64_t           total_compile_time_us;
    int64_t           total_gc_time_us;
    int64_t           saved_compile_time_us;  /* Time saved by caching */
};

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize the performance optimization context. */
void lr_perfopt_init(LR_PerfOptCtx *opt);

/* Destroy the performance optimization context. */
void lr_perfopt_destroy(LR_PerfOptCtx *opt);

/* ── Bytecode Cache API ────────────────────────────────────────────────── */

/* Check if bytecode is cached for a source. Returns cached blob or NULL. */
uint8_t *lr_compile_cache_lookup(LR_CompileCache *cache,
                                 const char *source_hash,
                                 size_t *out_len);

/* Store bytecode in cache. */
int lr_compile_cache_store(LR_CompileCache *cache,
                           const char *source_hash,
                           const uint8_t *bytecode, size_t bytecode_len);

/* Compute source hash (simple FNV-1a). */
void lr_source_hash(const char *source, size_t len, char hash_out[65]);

/* Clear the bytecode cache. */
void lr_compile_cache_clear(LR_CompileCache *cache);

/* Get cache stats. */
void lr_compile_cache_stats(LR_CompileCache *cache,
                            int *entries, int *hits, int *misses);

/* ── Concurrent GC API ─────────────────────────────────────────────────── */

/* Start concurrent GC thread. */
int lr_concurrent_gc_start(LR_ConcurrentGC *gc, LR_Runtime *rt);

/* Stop concurrent GC thread. */
void lr_concurrent_gc_stop(LR_ConcurrentGC *gc);

/* Trigger GC now (used by background thread). */
void lr_concurrent_gc_trigger(LR_ConcurrentGC *gc, LR_Runtime *rt);

/* ── Fast API Dispatch API ─────────────────────────────────────────────── */

/* Register a fast API function. */
int lr_fast_api_register(LR_FastAPICache *cache, const char *name,
                         LR_FastCFunction func, int length);

/* Look up a fast API function by name. */
LR_FastCFunction lr_fast_api_lookup(LR_FastAPICache *cache, const char *name);

/* ── Atom Cache API ────────────────────────────────────────────────────── */

/* Get or create a cached atom. */
JSAtom lr_atom_cache_get(LR_AtomCache *cache, JSContext *ctx, const char *name);

/* Pre-populate atom cache with common property names. */
void lr_atom_cache_prepopulate(LR_AtomCache *cache, JSContext *ctx);

/* ── Unified perf optimization API ─────────────────────────────────────── */

/* Attach optimizations to a runtime. */
void lr_perfopt_attach(LR_PerfOptCtx *opt, LR_Runtime *rt);

/* Detach optimizations from a runtime. */
void lr_perfopt_detach(LR_PerfOptCtx *opt);

/* Print optimization stats. */
void lr_perfopt_print_stats(LR_PerfOptCtx *opt, FILE *fp);

#endif /* LR_PERF_OPT_H */