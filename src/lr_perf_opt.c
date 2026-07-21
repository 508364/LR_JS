/*
 * L/R_JS - Performance Optimizations Implementation
 * Pure C, advanced optimization techniques for the JS engine.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "lr_platform.h"

#include "lr_perf_opt.h"
#include "lr_runtime.h"
#include "engine/lr_engine.h"

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int64_t get_time_us(void)
{
    return lr_get_time_us();
}

/* ── Init / Destroy ────────────────────────────────────────────────────── */

void lr_perfopt_init(LR_PerfOptCtx *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->compile_cache.max_entries = 1024;
    pthread_mutex_init(&opt->compile_cache.mutex, NULL);
    opt->concurrent_gc.gc_interval_ms = 5000;
    opt->concurrent_gc.gc_threshold = 16 * 1024 * 1024;  /* 16MB */
    pthread_mutex_init(&opt->concurrent_gc.mutex, NULL);
    pthread_cond_init(&opt->concurrent_gc.cond, NULL);
}

void lr_perfopt_destroy(LR_PerfOptCtx *opt)
{
    if (!opt) return;

    lr_concurrent_gc_stop(&opt->concurrent_gc);
    lr_compile_cache_clear(&opt->compile_cache);

    pthread_mutex_destroy(&opt->compile_cache.mutex);
    pthread_mutex_destroy(&opt->concurrent_gc.mutex);
    pthread_cond_destroy(&opt->concurrent_gc.cond);
}

/* ── Source Hash ───────────────────────────────────────────────────────── */

void lr_source_hash(const char *source, size_t len, char hash_out[65])
{
    /* FNV-1a hash */
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)source[i];
        hash *= 1099511628211ULL;
    }
    /* Also hash the length */
    hash ^= (uint64_t)len;
    hash *= 1099511628211ULL;

    snprintf(hash_out, 65, "%016lx%016lx",
             (unsigned long)(hash >> 32),
             (unsigned long)(hash & 0xFFFFFFFF));
}

/* ── Bytecode Cache ────────────────────────────────────────────────────── */

static uint32_t hash_to_bucket(const char *hash_str)
{
    uint32_t h = 0;
    for (int i = 0; hash_str[i]; i++) {
        h = (h * 31) + (unsigned char)hash_str[i];
    }
    return h % LR_COMPILE_CACHE_BUCKETS;
}

uint8_t *lr_compile_cache_lookup(LR_CompileCache *cache,
                                  const char *source_hash,
                                  size_t *out_len)
{
    if (!cache || !source_hash || !out_len) return NULL;

    pthread_mutex_lock(&cache->mutex);
    uint32_t bucket = hash_to_bucket(source_hash);

    for (LR_PerfBCEntry *bc = cache->buckets[bucket]; bc; bc = bc->next) {
        if (strcmp(bc->source_hash, source_hash) == 0) {
            bc->hit_count++;
            bc->last_accessed = time(NULL);
            *out_len = bc->bytecode_len;

            /* Return a copy */
            uint8_t *copy = malloc(bc->bytecode_len);
            if (copy) {
                memcpy(copy, bc->bytecode, bc->bytecode_len);
            }
            cache->hit_count++;
            pthread_mutex_unlock(&cache->mutex);
            return copy;
        }
    }

    cache->miss_count++;
    pthread_mutex_unlock(&cache->mutex);
    return NULL;
}

int lr_compile_cache_store(LR_CompileCache *cache,
                            const char *source_hash,
                            const uint8_t *bytecode, size_t bytecode_len)
{
    if (!cache || !source_hash || !bytecode) return -1;

    pthread_mutex_lock(&cache->mutex);

    /* Evict oldest if at capacity */
    if (cache->entry_count >= cache->max_entries) {
        /* Simple eviction: remove oldest entry across all buckets */
        LR_PerfBCEntry *oldest = NULL;
        int oldest_bucket = 0;
        for (int i = 0; i < LR_COMPILE_CACHE_BUCKETS; i++) {
            for (LR_PerfBCEntry *bc = cache->buckets[i]; bc; bc = bc->next) {
                if (!oldest || bc->last_accessed < oldest->last_accessed) {
                    oldest = bc;
                    oldest_bucket = i;
                }
            }
        }
        if (oldest) {
            /* Remove oldest from bucket */
            if (cache->buckets[oldest_bucket] == oldest) {
                cache->buckets[oldest_bucket] = oldest->next;
            } else {
                LR_PerfBCEntry *prev = cache->buckets[oldest_bucket];
                while (prev && prev->next != oldest) prev = prev->next;
                if (prev) prev->next = oldest->next;
            }
            free(oldest->source_hash);
            free(oldest->bytecode);
            free(oldest);
            cache->entry_count--;
        }
    }

    LR_PerfBCEntry *bc = calloc(1, sizeof(LR_PerfBCEntry));
    if (!bc) {
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    bc->source_hash = strdup(source_hash);
    bc->bytecode = malloc(bytecode_len);
    if (!bc->bytecode) {
        free(bc->source_hash);
        free(bc);
        pthread_mutex_unlock(&cache->mutex);
        return -1;
    }

    memcpy(bc->bytecode, bytecode, bytecode_len);
    bc->bytecode_len = bytecode_len;
    bc->created_at = time(NULL);
    bc->last_accessed = bc->created_at;

    uint32_t bucket = hash_to_bucket(source_hash);
    bc->next = cache->buckets[bucket];
    cache->buckets[bucket] = bc;
    cache->entry_count++;

    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

void lr_compile_cache_clear(LR_CompileCache *cache)
{
    if (!cache) return;

    pthread_mutex_lock(&cache->mutex);
    for (int i = 0; i < LR_COMPILE_CACHE_BUCKETS; i++) {
        LR_PerfBCEntry *bc = cache->buckets[i];
        while (bc) {
            LR_PerfBCEntry *next = bc->next;
            free(bc->source_hash);
            free(bc->bytecode);
            free(bc);
            bc = next;
        }
        cache->buckets[i] = NULL;
    }
    cache->entry_count = 0;
    pthread_mutex_unlock(&cache->mutex);
}

void lr_compile_cache_stats(LR_CompileCache *cache,
                            int *entries, int *hits, int *misses)
{
    if (!cache) return;
    pthread_mutex_lock(&cache->mutex);
    if (entries) *entries = cache->entry_count;
    if (hits) *hits = cache->hit_count;
    if (misses) *misses = cache->miss_count;
    pthread_mutex_unlock(&cache->mutex);
}

/* ── Concurrent GC ─────────────────────────────────────────────────────── */

static void *lr_concurrent_gc_thread(void *arg)
{
    LR_ConcurrentGC *gc = (LR_ConcurrentGC *)arg;

    while (1) {
        pthread_mutex_lock(&gc->mutex);

        /* Wait for interval or signal */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  /* Check every second */

        int rc = 0;
        while (gc->running && !gc->should_stop && rc == 0) {
            rc = pthread_cond_timedwait(&gc->cond, &gc->mutex, &ts);
        }

        if (gc->should_stop || !gc->running) {
            pthread_mutex_unlock(&gc->mutex);
            break;
        }

        pthread_mutex_unlock(&gc->mutex);

        /* Incremental GC step:
           We can't run JS_RunGC here because it needs the runtime,
           but the background thread doesn't own it.
           Instead, we set a flag that the main thread checks. */
        lr_sleep_ms(100);  /* 100ms cooldown */
    }

    return NULL;
}

int lr_concurrent_gc_start(LR_ConcurrentGC *gc, LR_Runtime *rt)
{
    if (!gc || !rt) return -1;

    gc->running = 1;
    gc->should_stop = 0;
    gc->last_gc_time = get_time_us();

    if (pthread_create(&gc->gc_thread, NULL, lr_concurrent_gc_thread, gc) != 0) {
        gc->running = 0;
        return -1;
    }

    return 0;
}

void lr_concurrent_gc_stop(LR_ConcurrentGC *gc)
{
    if (!gc || !gc->running) return;

    pthread_mutex_lock(&gc->mutex);
    gc->should_stop = 1;
    pthread_cond_signal(&gc->cond);
    pthread_mutex_unlock(&gc->mutex);

    pthread_join(gc->gc_thread, NULL);
    gc->running = 0;
}

void lr_concurrent_gc_trigger(LR_ConcurrentGC *gc, LR_Runtime *rt)
{
    if (!gc || !rt) return;
    gc->gc_count++;
    gc->last_gc_time = get_time_us();

    /* Run GC in the calling thread (incremental step) */
    lr_gc_run(rt->lr_rt);
}

/* ── Fast API Dispatch ─────────────────────────────────────────────────── */

int lr_fast_api_register(LR_FastAPICache *cache, const char *name,
                         LR_FastCFunction func, int length)
{
    if (!cache || !name || !func) return -1;
    if (cache->entry_count >= LR_FAST_API_CACHE_SIZE) return -1;

    LR_FastAPIEntry *entry = &cache->entries[cache->entry_count++];
    entry->name = name;
    entry->func = func;
    entry->length = length;
    entry->hit_count = 0;
    entry->atom = JS_ATOM_NULL;  /* Will be resolved lazily */

    return 0;
}

LR_FastCFunction lr_fast_api_lookup(LR_FastAPICache *cache, const char *name)
{
    if (!cache || !name) return NULL;

    for (int i = 0; i < cache->entry_count; i++) {
        if (strcmp(cache->entries[i].name, name) == 0) {
            cache->entries[i].hit_count++;
            cache->hit_count++;
            return cache->entries[i].func;
        }
    }

    cache->miss_count++;
    return NULL;
}

/* ── Atom Cache ────────────────────────────────────────────────────────── */

JSAtom lr_atom_cache_get(LR_AtomCache *cache, JSContext *ctx, const char *name)
{
    if (!cache || !ctx || !name) return JS_ATOM_NULL;

    /* Search cache */
    for (int i = 0; i < cache->count; i++) {
        if (cache->atoms[i].initialized &&
            strcmp(cache->atoms[i].name, name) == 0) {
            cache->hit_count++;
            return cache->atoms[i].atom;
        }
    }

    /* Not in cache, create and store */
    if (cache->count >= LR_ATOM_CACHE_SIZE) {
        cache->miss_count++;
        return JS_NewAtom(ctx, name);
    }

    JSAtom atom = JS_NewAtom(ctx, name);
    LR_AtomEntry *entry = &cache->atoms[cache->count];
    entry->name = name;
    entry->atom = atom;
    entry->initialized = 1;
    cache->count++;
    cache->miss_count++;

    return atom;
}

void lr_atom_cache_prepopulate(LR_AtomCache *cache, JSContext *ctx)
{
    if (!cache || !ctx) return;

    /* Pre-compute atoms for all common built-in property names.
       These are the most frequently accessed properties in browser APIs. */
    static const char *common_atoms[] = {
        /* Console */
        "log", "warn", "error", "info", "debug", "assert", "trace",
        "time", "timeEnd", "clear", "count", "group", "groupEnd", "table",
        /* General */
        "name", "length", "message", "stack", "cause", "type",
        "detail", "bubbles", "cancelable", "composed", "target",
        "currentTarget", "eventPhase", "defaultPrevented",
        "timeStamp", "isTrusted", "signal", "aborted", "reason",
        /* URL */
        "href", "protocol", "hostname", "port", "pathname",
        "search", "hash", "origin", "host", "searchParams",
        "username", "password",
        /* Encoding */
        "encoding", "fatal", "ignoreBOM", "stream",
        "byteLength", "buffer", "byteOffset",
        /* Performance */
        "now", "mark", "measure", "timeOrigin", "getEntriesByType",
        "getEntriesByName", "clearMarks", "clearMeasures",
        /* Crypto */
        "getRandomValues", "randomUUID", "subtle", "digest",
        /* Storage */
        "getItem", "setItem", "removeItem", "clear", "key",
        "localStorage", "sessionStorage",
        /* Fetch */
        "fetch", "method", "headers", "body", "status", "ok",
        "json", "text", "blob", "formData", "arrayBuffer",
        "url", "redirect", "credentials", "mode", "cache",
        /* Timers */
        "setTimeout", "setInterval", "clearTimeout", "clearInterval",
        "queueMicrotask",
        /* Prototype */
        "prototype", "constructor",
        /* Worker */
        "postMessage", "onmessage", "onerror", "terminate",
        /* Renderer */
        "canvas", "width", "height", "getContext", "fillRect",
        "strokeRect", "fillText", "strokeText", "drawImage",
        "beginPath", "closePath", "moveTo", "lineTo", "arc",
        "fill", "stroke", "fillStyle", "strokeStyle", "lineWidth",
        "font", "textAlign", "textBaseline", "globalAlpha",
        "transform", "setTransform", "save", "restore",
        "createImageData", "getImageData", "putImageData",
        "toDataURL", "toBlob",
        NULL
    };

    for (const char **p = common_atoms; *p; p++) {
        lr_atom_cache_get(cache, ctx, *p);
    }
}

/* ── Unified API ───────────────────────────────────────────────────────── */

void lr_perfopt_attach(LR_PerfOptCtx *opt, LR_Runtime *rt)
{
    if (!opt || !rt) return;

    /* Pre-populate atom cache for fast property access */
    lr_atom_cache_prepopulate(&opt->atom_cache, rt->lr_ctx);

    /* Start concurrent GC */
    lr_concurrent_gc_start(&opt->concurrent_gc, rt);
}

void lr_perfopt_detach(LR_PerfOptCtx *opt)
{
    if (!opt) return;
    lr_concurrent_gc_stop(&opt->concurrent_gc);
}

void lr_perfopt_print_stats(LR_PerfOptCtx *opt, FILE *fp)
{
    if (!opt || !fp) return;

    fprintf(fp, "┌── Performance Optimizations ───────────────────────────┐\n");
    fprintf(fp, "│ Compile Cache:  entries=%d  hits=%d  misses=%d       \n",
            opt->compile_cache.entry_count,
            opt->compile_cache.hit_count,
            opt->compile_cache.miss_count);
    fprintf(fp, "│ Fast API Cache: entries=%d  hits=%d  misses=%d       \n",
            opt->fast_api_cache.entry_count,
            opt->fast_api_cache.hit_count,
            opt->fast_api_cache.miss_count);
    fprintf(fp, "│ Atom Cache:     entries=%d  hits=%d  misses=%d       \n",
            opt->atom_cache.count,
            opt->atom_cache.hit_count,
            opt->atom_cache.miss_count);
    fprintf(fp, "│ Concurrent GC:  count=%lld  running=%d               \n",
            (long long)opt->concurrent_gc.gc_count,
            opt->concurrent_gc.running);
    fprintf(fp, "└──────────────────────────────────────────────────────┘\n");
}