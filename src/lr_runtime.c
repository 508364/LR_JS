/*
 * L/R_JS - Runtime Implementation
 * Pure C, ES2022-compatible, high-performance JS runtime.
 */
#include "lr_platform.h"

#include "lr_runtime.h"
#include "lr_renderer.h"

/* ── Global renderer bridge (shared across all Canvas instances) ───────── */

LR_RendererBridge *g_lr_renderer_bridge = NULL;

/* ── Configuration ────────────────────────────────────────────────────── */

void lr_config_default(LR_Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->memory_limit       = 0;          /* unlimited */
    cfg->gc_threshold       = 0;          /* default (~256KB) */
    cfg->gc_mode            = LR_GC_MODE_GENERATIONAL;
    cfg->gc_generational    = 1;          /* enable generational GC */
    cfg->gc_incremental     = 1;          /* enable incremental GC */
    cfg->gc_nursery_size    = 0;          /* default 4MB */
    cfg->gc_pause_target_ns = 0;          /* default 5ms */
    cfg->min_system_memory  = 1073741824; /* 1 GB minimum */
    cfg->skip_memory_check  = 0;
    cfg->max_stack_size     = 0;          /* default (1MB) */
    cfg->timeout_ms         = 0;          /* no timeout */
    cfg->strict_mode        = 0;
    cfg->log_level          = LR_LOG_ERROR;
    cfg->dump_bytecode      = 0;
    cfg->strip_debug_info   = 0;
    cfg->stdin_override     = NULL;
    cfg->stdout_override    = NULL;
    cfg->stderr_override    = NULL;
}

/* ── Error handling ───────────────────────────────────────────────────── */

void lr_set_error(LR_Runtime *rt, const char *fmt, ...)
{
    va_list ap;
    char buf[4096];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    free(rt->last_error);
    rt->last_error = strdup(buf);
}

const char *lr_get_last_error(LR_Runtime *rt)
{
    return rt->last_error ? rt->last_error : "";
}

void lr_clear_last_error(LR_Runtime *rt)
{
    free(rt->last_error);
    rt->last_error = NULL;
}

/* ── Logging ──────────────────────────────────────────────────────────── */

void lr_log(LR_Runtime *rt, LR_LogLevel level, const char *fmt, ...)
{
    if (level > rt->config.log_level) return;

    va_list ap;
    FILE *out = rt->stderr_fp;

    static const char *prefixes[] = {
        "", "[ERROR] ", "[WARN]  ", "[INFO]  ", "[DEBUG] "
    };
    fprintf(out, "%s", prefixes[level]);

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, "\n");
    fflush(out);
}

/* ── JS exception helper ──────────────────────────────────────────────── */

int lr_check_exception(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue exc = JS_GetException(ctx);
    if (!JS_IsException(exc)) {
        /* User-thrown value (e.g. throw 42) - convert to string */
        const char *str = JS_ToCString(ctx, exc);
        if (str) {
            lr_set_error(rt, "%s", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
        ctx->current_exception = LR_VALUE_UNDEFINED;
        return -1;
    }
    /* Engine-thrown exception (lr_throw_*) - message is in ctx->error_message */
    if (ctx->error_message) {
        lr_set_error(rt, "%s", ctx->error_message);
        free(ctx->error_message);
        ctx->error_message = NULL;
    } else {
        lr_set_error(rt, "unknown error");
    }
    JS_FreeValue(ctx, exc);
    ctx->current_exception = LR_VALUE_UNDEFINED;
    return -1;
}

/* ── System memory check ──────────────────────────────────────────────── */

int64_t lr_get_available_memory(void)
{
    int64_t available = -1;

    /* Linux: read /proc/meminfo */
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        int64_t mem_free = -1, mem_available = -1, buffers = -1, cached = -1;

        while (fgets(line, sizeof(line), f)) {
            long long val;
            if (sscanf(line, "MemAvailable: %lld kB", &val) == 1) {
                mem_available = val * 1024;
            } else if (sscanf(line, "MemFree: %lld kB", &val) == 1) {
                mem_free = val * 1024;
            } else if (sscanf(line, "Buffers: %lld kB", &val) == 1) {
                buffers = val * 1024;
            } else if (sscanf(line, "Cached: %lld kB", &val) == 1) {
                cached = val * 1024;
            }
        }
        fclose(f);

        /* Prefer MemAvailable (kernel 3.14+), fallback to MemFree + Buffers + Cached */
        if (mem_available >= 0) {
            available = mem_available;
        } else if (mem_free >= 0) {
            available = mem_free;
            if (buffers >= 0) available += buffers;
            if (cached >= 0) available += cached;
        }
    }

    /* Fallback: use platform layer */
    if (available < 0) {
        long pages = lr_get_avail_mem_pages();
        long page_size = lr_get_page_size();
        if (pages > 0 && page_size > 0) {
            available = (int64_t)pages * page_size;
        }
    }

    return available;
}

int lr_check_system_memory(size_t min_bytes)
{
    if (min_bytes == 0) return 0;

    int64_t available = lr_get_available_memory();
    if (available < 0) {
        /* Cannot determine available memory, allow to proceed */
        return 0;
    }

    if ((int64_t)min_bytes > available) {
        return -1;
    }

    return 0;
}

/* ── File loading ─────────────────────────────────────────────────────── */

uint8_t *lr_load_file(LR_Runtime *rt, const char *filename, size_t *out_len)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        lr_set_error(rt, "Cannot open file: %s", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        lr_set_error(rt, "Cannot determine file size: %s", filename);
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(size + 1);
    if (!buf) {
        lr_set_error(rt, "Out of memory loading file: %s", filename);
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);

    if ((long)read != size) {
        lr_set_error(rt, "Failed to read entire file: %s", filename);
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    *out_len = size;
    return buf;
}

/* ── Module loader ────────────────────────────────────────────────────── */

static char *lr_module_normalize(JSContext *ctx, const char *base_name,
                                  const char *name, void *opaque)
{
    LR_Runtime *rt = (LR_Runtime *)opaque;

    /* Absolute path or starts with ./ or ../ */
    if (name[0] == '/' || name[0] == '.') {
        char *result = malloc(strlen(name) + 1);
        if (result) strcpy(result, name);
        return result;
    }

    /* Try module paths */
    for (int i = 0; i < rt->config.module_paths_count; i++) {
        const char *mp = rt->config.module_paths[i];
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", mp, name);

        /* Check if file exists */
        struct stat st;
        if (stat(path, &st) == 0) {
            char *result = malloc(strlen(path) + 1);
            if (result) strcpy(result, path);
            return result;
        }

        /* Try with .js extension */
        snprintf(path, sizeof(path), "%s/%s.js", mp, name);
        if (stat(path, &st) == 0) {
            char *result = malloc(strlen(path) + 1);
            if (result) strcpy(result, path);
            return result;
        }

        /* Try with .mjs extension */
        snprintf(path, sizeof(path), "%s/%s.mjs", mp, name);
        if (stat(path, &st) == 0) {
            char *result = malloc(strlen(path) + 1);
            if (result) strcpy(result, path);
            return result;
        }
    }

    /* Fallback: use as-is */
    char *result = malloc(strlen(name) + 1);
    if (result) strcpy(result, name);
    return result;
}

static JSModuleDef *lr_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    LR_Runtime *rt = (LR_Runtime *)opaque;

    size_t buf_len;
    uint8_t *buf = lr_load_file(rt, name, &buf_len);
    if (!buf) {
        JS_ThrowReferenceError(ctx, "Cannot load module '%s': %s", name,
                               rt->last_error ? rt->last_error : "unknown error");
        return NULL;
    }

    /* Determine if it's a module */
    int is_module = JS_DetectModule((const char *)buf, buf_len);
    int eval_flags = JS_EVAL_TYPE_MODULE;
    if (!is_module) {
        /* Treat as module anyway for .mjs, or if forced */
        eval_flags = JS_EVAL_TYPE_MODULE;
    }

    JSValue func_val = JS_Eval(ctx, (const char *)buf, buf_len, name,
                                eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
    free(buf);

    if (JS_IsException(func_val)) {
        lr_check_exception(rt);
        return NULL;
    }

    /* Set import.meta */
    js_module_set_import_meta(ctx, func_val, TRUE, TRUE);

    JSValue ret = JS_EvalFunction(ctx, func_val);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, ret);
        lr_check_exception(rt);
        return NULL;
    }
    JS_FreeValue(ctx, ret);

    /* Return the module from the function */
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    return m;
}

/* ── Runtime lifecycle ────────────────────────────────────────────────── */

LR_Runtime *lr_runtime_new(const LR_Config *cfg)
{
    /* ── System memory check ────────────────────────────────────────── */
    if (!cfg->skip_memory_check && cfg->min_system_memory > 0) {
        int64_t available = lr_get_available_memory();
        if (available >= 0 && (int64_t)cfg->min_system_memory > available) {
            fprintf(stderr,
                    "\n"
                    "╔══════════════════════════════════════════════════════════════╗\n"
                    "║  L/R_JS: INSUFFICIENT SYSTEM MEMORY                         ║\n"
                    "╠══════════════════════════════════════════════════════════════╣\n"
                    "║  Required : %10.2f GB (%zu bytes)                   ║\n"
                    "║  Available: %10.2f GB (%lld bytes)               ║\n"
                    "║                                                            ║\n"
                    "║  L/R_JS requires at least 1 GB of available system memory   ║\n"
                    "║  to ensure stable operation.                                 ║\n"
                    "║                                                            ║\n"
                    "║  To bypass this check:                                      ║\n"
                    "║    --no-memory-check                                        ║\n"
                    "║  To set a different minimum:                                ║\n"
                    "║    --min-memory <bytes>                                    ║\n"
                    "╚══════════════════════════════════════════════════════════════╝\n"
                    "\n",
                    (double)cfg->min_system_memory / (1024.0 * 1024.0 * 1024.0),
                    cfg->min_system_memory,
                    (double)available / (1024.0 * 1024.0 * 1024.0),
                    (long long)available);
            return NULL;
        }
    }

    LR_Runtime *rt = calloc(1, sizeof(LR_Runtime));
    if (!rt) return NULL;

    rt->config = *cfg;

    /* I/O */
    rt->stdin_fp  = cfg->stdin_override  ? cfg->stdin_override  : stdin;
    rt->stdout_fp = cfg->stdout_override ? cfg->stdout_override : stdout;
    rt->stderr_fp = cfg->stderr_override ? cfg->stderr_override : stderr;

    /* Create JS runtime */
    rt->lr_rt = JS_NewRuntime();
    if (!rt->lr_rt) {
        free(rt);
        return NULL;
    }

    /* Memory limit */
    if (cfg->memory_limit > 0) {
        JS_SetMemoryLimit(rt->lr_rt, cfg->memory_limit);
    }

    /* GC threshold */
    if (cfg->gc_threshold > 0) {
        JS_SetGCThreshold(rt->lr_rt, cfg->gc_threshold);
    }

    /* Stack size */
    if (cfg->max_stack_size > 0) {
        JS_SetMaxStackSize(rt->lr_rt, cfg->max_stack_size);
    }

    /* Create context */
    rt->lr_ctx = JS_NewContext(rt->lr_rt);
    if (!rt->lr_ctx) {
        JS_FreeRuntime(rt->lr_rt);
        free(rt);
        return NULL;
    }

    /* Set context opaque so built-in APIs can find the runtime */
    JS_SetContextOpaque(rt->lr_ctx, rt);

    /* Initialize GC context */
    lr_gc_init(&rt->gc_ctx, rt);

    /* Initialize bytecode cache */
    lr_bytecode_cache_init(&rt->bytecode_cache, rt,
                           cfg->bytecode_cache_dir);

    /* Apply GC configuration */
    {
        LR_GCConfig gc_cfg;
        lr_gc_config_default(&gc_cfg);

        gc_cfg.generational_enabled = cfg->gc_generational;
        gc_cfg.incremental_enabled  = cfg->gc_incremental;

        if (cfg->gc_nursery_size > 0)
            gc_cfg.nursery_size = cfg->gc_nursery_size;
        if (cfg->gc_pause_target_ns > 0)
            gc_cfg.pause_target_ns = cfg->gc_pause_target_ns;

        switch (cfg->gc_mode) {
        case LR_GC_MODE_MANUAL:
            gc_cfg.generational_enabled = 0;
            gc_cfg.incremental_enabled = 0;
            break;
        case LR_GC_MODE_STRESS:
            gc_cfg.stress_mode = 1;
            break;
        case LR_GC_MODE_GENERATIONAL:
            gc_cfg.generational_enabled = 1;
            gc_cfg.incremental_enabled = 0;
            break;
        case LR_GC_MODE_INCREMENTAL:
            gc_cfg.generational_enabled = 1;
            gc_cfg.incremental_enabled = 1;
            break;
        default:
            break;
        }

        lr_gc_configure(&rt->gc_ctx, &gc_cfg);
    }

    /* Initialize renderer bridge (lazy-init on first Canvas use) */
    g_lr_renderer_bridge = NULL;

    /* Set up module loader */
    JS_SetModuleLoaderFunc(rt->lr_rt, lr_module_normalize,
                            lr_module_loader, rt);

    /* Register all built-in browser APIs */
    lr_register_builtins(rt);

    lr_log(rt, LR_LOG_INFO, "L/R_JS v%s initialized (lightweight JS engine)",
           LR_JS_VERSION_STRING);

    return rt;
}

void lr_runtime_free(LR_Runtime *rt)
{
    if (!rt) return;

    lr_log(rt, LR_LOG_INFO, "L/R_JS shutting down");

    /* Cleanup per-module */
    lr_timers_cleanup(rt);

    /* Cleanup GC context */
    lr_gc_destroy(&rt->gc_ctx);

    /* Cleanup bytecode cache */
    lr_bytecode_cache_destroy(&rt->bytecode_cache);

    /* Cleanup renderer bridge */
    if (g_lr_renderer_bridge) {
        lr_renderer_destroy(g_lr_renderer_bridge);
        g_lr_renderer_bridge = NULL;
    }

    /* Free JS runtime (breaks circular references, clears object values,
     * frees atom table, but does NOT free the context struct itself so
     * that obj->ctx remains valid for the object cleanup loop). */
    if (rt->lr_ctx) {
        JS_FreeContext(rt->lr_ctx);
    }
    /* Clean remaining objects. After JS_FreeContext, circular references
     * (ctor↔proto) are already broken, and all object values in properties
     * are cleared to LR_VALUE_UNDEFINED. We need to:
     * 1. Clear prop->key (atoms were freed by lr_free_context)
     * 2. Free remaining objects */
    if (rt->lr_rt) {
        LRRuntime *eng_rt = rt->lr_rt;
        /* Clear prop->key to prevent use-after-free on freed atom table */
        LRObject *obj = eng_rt->obj_list;
        while (obj) {
            LRProperty *prop = obj->prop_hash;
            while (prop) {
                prop->key = NULL;
                prop = prop->next;
            }
            obj = obj->gc_next;
        }
        /* Free remaining objects. Advance list head before each free
         * to handle indirect freeing through data_free callbacks. */
        {
            int max_count = 100000;
            while (eng_rt->obj_list && max_count-- > 0) {
                LRObject *cur = eng_rt->obj_list;
                eng_rt->obj_list = cur->gc_next;
                cur->gc_next = NULL;
                lr_free_object(eng_rt, cur);
            }
            eng_rt->obj_list = NULL;
        }
    }
    /* Free the context struct AFTER the object cleanup loop, since
     * objects still reference ctx via obj->ctx for safe cleanup. */
    if (rt->lr_ctx) {
        free(rt->lr_ctx);
        rt->lr_ctx = NULL;
    }
    if (rt->lr_rt) {
        JS_FreeRuntime(rt->lr_rt);
    }

    free(rt->last_error);
    free(rt);
}

/* ── Script execution ─────────────────────────────────────────────────── */

int lr_eval(LR_Runtime *rt, const char *source, size_t source_len,
            const char *filename)
{
    int eval_flags = JS_EVAL_TYPE_GLOBAL;
    if (rt->config.strict_mode) {
        eval_flags |= JS_EVAL_FLAG_STRICT;
    }

    JSValue result = JS_Eval(rt->lr_ctx, source, source_len, filename,
                              eval_flags);

    if (JS_IsException(result)) {
        lr_check_exception(rt);
        JS_FreeValue(rt->lr_ctx, result);
        return -1;
    }

    JS_FreeValue(rt->lr_ctx, result);

    /* Simplified GC check: use counter-based approach instead of
     * expensive full memory scan every eval.
     * Track allocation roughly and trigger GC every N evals. */
    rt->gc_ctx.bytes_since_last_gc += source_len;
    if (rt->gc_ctx.bytes_since_last_gc >= rt->gc_ctx.config.allocation_threshold) {
        lr_gc_before_alloc(&rt->gc_ctx);
    }

    return 0;
}

int lr_eval_file(LR_Runtime *rt, const char *filename)
{
    size_t buf_len;
    uint8_t *buf = lr_load_file(rt, filename, &buf_len);
    if (!buf) return -1;

    /* Try bytecode cache first */
    size_t bc_len = 0;
    uint8_t *bc = lr_bytecode_cache_load(&rt->bytecode_cache, filename,
                                         buf, buf_len, &bc_len);
    int ret;
    if (bc) {
        /* Cache hit: execute from bytecode */
        JSValue obj = JS_ReadObject(rt->lr_ctx, bc, bc_len,
                                     JS_READ_OBJ_BYTECODE);
        free(bc);
        if (JS_IsException(obj)) {
            lr_check_exception(rt);
            JS_FreeValue(rt->lr_ctx, obj);
            free(buf);
            return -1;
        }
        /* obj is a function, execute it */
        JSValue result = JS_EvalFunction(rt->lr_ctx, obj);
        if (JS_IsException(result)) {
            lr_check_exception(rt);
            JS_FreeValue(rt->lr_ctx, result);
            free(buf);
            return -1;
        }
        JS_FreeValue(rt->lr_ctx, result);
        ret = 0;
    } else {
        /* Cache miss: compile normally */
        int eval_flags = JS_EVAL_TYPE_GLOBAL;
        if (rt->config.strict_mode) eval_flags |= JS_EVAL_FLAG_STRICT;

        JSValue val = JS_Eval(rt->lr_ctx, (const char *)buf, buf_len,
                              filename, eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(val)) {
            lr_check_exception(rt);
            JS_FreeValue(rt->lr_ctx, val);
            free(buf);
            return -1;
        }

        /* Serialize bytecode for cache */
        uint8_t *bc_out;
        size_t bc_out_len;
        bc_out = JS_WriteObject(rt->lr_ctx, &bc_out_len, val,
                                 JS_WRITE_OBJ_BYTECODE);
        if (bc_out) {
            lr_bytecode_cache_store(&rt->bytecode_cache, filename,
                                    buf, buf_len, bc_out, bc_out_len,
                                    rt->config.strict_mode ? LR_BYTECODE_FLAG_STRICT : 0);
            free(bc_out);
        }

        JSValue result = JS_EvalFunction(rt->lr_ctx, val);
        if (JS_IsException(result)) {
            lr_check_exception(rt);
            JS_FreeValue(rt->lr_ctx, result);
            free(buf);
            return -1;
        }
        JS_FreeValue(rt->lr_ctx, result);
        ret = 0;
    }

    free(buf);

    /* Simplified GC check */
    rt->gc_ctx.bytes_since_last_gc += buf_len;
    if (rt->gc_ctx.bytes_since_last_gc >= rt->gc_ctx.config.allocation_threshold) {
        lr_gc_before_alloc(&rt->gc_ctx);
    }

    return ret;
}

int lr_eval_module(LR_Runtime *rt, const char *source, size_t source_len,
                   const char *filename)
{
    int eval_flags = JS_EVAL_TYPE_MODULE;

    JSValue val = JS_Eval(rt->lr_ctx, source, source_len, filename,
                           eval_flags | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(val)) {
        lr_check_exception(rt);
        return -1;
    }

    js_module_set_import_meta(rt->lr_ctx, val, TRUE, TRUE);
    JSValue ret = JS_EvalFunction(rt->lr_ctx, val);
    JS_FreeValue(rt->lr_ctx, val);

    if (JS_IsException(ret)) {
        lr_check_exception(rt);
        JS_FreeValue(rt->lr_ctx, ret);
        return -1;
    }

    JS_FreeValue(rt->lr_ctx, ret);
    return 0;
}

/* ── Event loop ───────────────────────────────────────────────────────── */

int lr_event_loop_run(LR_Runtime *rt)
{
    rt->event_loop_running = 1;
    JSContext *ctx;

    lr_log(rt, LR_LOG_DEBUG, "Event loop started");

    while (rt->event_loop_running && !rt->event_loop_stop) {
        /* Execute pending microtasks/jobs */
        int has_jobs = 0;
        while (JS_IsJobPending(rt->lr_rt)) {
            /* Check GC before each job execution */
            lr_gc_before_alloc(&rt->gc_ctx);

            int ret = JS_ExecutePendingJob(rt->lr_rt, &ctx);
            if (ret < 0) {
                lr_check_exception(rt);
                rt->event_loop_running = 0;
                break;
            }
            has_jobs = 1;
        }

        /* Process timer callbacks */
        lr_timers_process(rt);

        if (!has_jobs && !rt->has_pending_jobs) {
            /* Idle: do incremental GC work */
            lr_gc_idle_work(&rt->gc_ctx);

            /* No more work to do */
            break;
        }

        /* Small sleep to avoid busy-waiting */
        if (!JS_IsJobPending(rt->lr_rt) && !rt->has_pending_jobs) {
            break;
        }
    }

    rt->event_loop_stop = 0;
    lr_log(rt, LR_LOG_DEBUG, "Event loop stopped");
    return 0;
}

int lr_event_loop_run_timeout(LR_Runtime *rt, int timeout_ms)
{
    rt->event_loop_running = 1;
    JSContext *ctx;

    clock_t start = clock();
    int elapsed_ms = 0;

    while (rt->event_loop_running && !rt->event_loop_stop) {
        /* Execute pending jobs */
        while (JS_IsJobPending(rt->lr_rt)) {
            int ret = JS_ExecutePendingJob(rt->lr_rt, &ctx);
            if (ret < 0) {
                lr_check_exception(rt);
                rt->event_loop_running = 0;
                break;
            }
        }

        /* Process timers */
        lr_timers_process(rt);

        /* Check timeout */
        elapsed_ms = (int)(((clock() - start) * 1000) / CLOCKS_PER_SEC);
        if (elapsed_ms >= timeout_ms) break;

        if (!JS_IsJobPending(rt->lr_rt) && !rt->has_pending_jobs) {
            break;
        }
    }

    rt->event_loop_stop = 0;
    return 0;
}

int lr_event_loop_pending(LR_Runtime *rt)
{
    return JS_IsJobPending(rt->lr_rt) || rt->has_pending_jobs;
}

void lr_event_loop_stop(LR_Runtime *rt)
{
    rt->event_loop_stop = 1;
}

/* ── GC ───────────────────────────────────────────────────────────────── */

void lr_gc(LR_Runtime *rt)
{
    /* Use our generational GC */
    lr_gc_full(&rt->gc_ctx);
    lr_log(rt, LR_LOG_DEBUG, "GC triggered (full)");
}

void lr_gc_print_stats(LR_Runtime *rt, FILE *fp)
{
    lr_gc_ctx_print_stats(&rt->gc_ctx, fp);
}

void lr_gc_reset_stats(LR_Runtime *rt)
{
    lr_gc_ctx_reset_stats(&rt->gc_ctx);
}

/* ── Bytecode cache ───────────────────────────────────────────────────── */

void lr_bytecode_cache_stats(LR_Runtime *rt, FILE *fp)
{
    lr_bytecode_cache_stats_ctx(&rt->bytecode_cache, fp);
}

void lr_bytecode_cache_clear(LR_Runtime *rt)
{
    lr_bytecode_cache_clear_ctx(&rt->bytecode_cache);
}

/* ── Memory usage ─────────────────────────────────────────────────────── */

void lr_compute_memory_usage(LR_Runtime *rt, LR_MemoryUsage *usage)
{
    LRMemoryUsage qjs_usage;
    lr_engine_compute_memory_usage(rt->lr_rt, &qjs_usage);

    usage->malloc_size            = qjs_usage.malloc_size;
    usage->malloc_limit           = qjs_usage.malloc_limit;
    usage->memory_used_size       = qjs_usage.memory_used_size;
    usage->malloc_count           = qjs_usage.malloc_count;
    usage->memory_used_count      = qjs_usage.memory_used_count;
    usage->atom_count             = qjs_usage.atom_count;
    usage->atom_size              = qjs_usage.atom_size;
    usage->str_count              = qjs_usage.str_count;
    usage->str_size               = qjs_usage.str_size;
    usage->obj_count              = qjs_usage.obj_count;
    usage->obj_size               = qjs_usage.obj_size;
    usage->prop_count             = qjs_usage.prop_count;
    usage->prop_size              = qjs_usage.prop_size;
    usage->shape_count            = qjs_usage.shape_count;
    usage->shape_size             = qjs_usage.shape_size;
    usage->js_func_count          = qjs_usage.js_func_count;
    usage->js_func_size           = qjs_usage.js_func_size;
    usage->js_func_code_size      = qjs_usage.js_func_code_size;
    usage->c_func_count           = qjs_usage.c_func_count;
    usage->array_count            = qjs_usage.array_count;
    usage->fast_array_count       = qjs_usage.fast_array_count;
    usage->fast_array_elements    = qjs_usage.fast_array_elements;
    usage->binary_object_count    = qjs_usage.binary_object_count;
    usage->binary_object_size     = qjs_usage.binary_object_size;
}

void lr_dump_memory_usage(LR_Runtime *rt, FILE *fp)
{
    LR_MemoryUsage usage;
    lr_compute_memory_usage(rt, &usage);

    fprintf(fp, "=== L/R_JS Memory Usage ===\n");
    fprintf(fp, "  Malloc:  %lld bytes (%lld allocs, limit %lld)\n",
            (long long)usage.malloc_size, (long long)usage.malloc_count,
            (long long)usage.malloc_limit);
    fprintf(fp, "  Memory:  %lld bytes (%lld allocs)\n",
            (long long)usage.memory_used_size, (long long)usage.memory_used_count);
    fprintf(fp, "  Atoms:   %lld (%lld bytes)\n",
            (long long)usage.atom_count, (long long)usage.atom_size);
    fprintf(fp, "  Strings: %lld (%lld bytes)\n",
            (long long)usage.str_count, (long long)usage.str_size);
    fprintf(fp, "  Objects: %lld (%lld bytes)\n",
            (long long)usage.obj_count, (long long)usage.obj_size);
    fprintf(fp, "  Props:   %lld (%lld bytes)\n",
            (long long)usage.prop_count, (long long)usage.prop_size);
    fprintf(fp, "  Shapes:  %lld (%lld bytes)\n",
            (long long)usage.shape_count, (long long)usage.shape_size);
    fprintf(fp, "  JS Func: %lld (%lld bytes, %lld code)\n",
            (long long)usage.js_func_count, (long long)usage.js_func_size,
            (long long)usage.js_func_code_size);
    fprintf(fp, "  C Func:  %lld\n", (long long)usage.c_func_count);
    fprintf(fp, "  Arrays:  %lld (fast: %lld, elements: %lld)\n",
            (long long)usage.array_count, (long long)usage.fast_array_count,
            (long long)usage.fast_array_elements);
}

/* ── Version ──────────────────────────────────────────────────────────── */

const char *lr_version(void)
{
    return LR_JS_VERSION_STRING;
}

/* ── JS-callable GC functions ─────────────────────────────────────────── */

static JSValue js_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    lr_gc(rt);
    return JS_UNDEFINED;
}

static JSValue js_gc_stats(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    lr_gc_print_stats(rt, stderr);
    return JS_UNDEFINED;
}

/* ── Built-in registration ────────────────────────────────────────────── */

/* Register Error.captureStackTrace, Error.stackTraceLimit, and Error.prototype.stack getter */
static void lr_register_error_builtins(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Check if Error constructor already exists */
    JSValue error_ctor = JS_GetPropertyStr(ctx, global, "Error");

    if (JS_IsUndefined(error_ctor) && !JS_IsException(error_ctor)) {
        /* Create a proper Error constructor */
        error_ctor = JS_NewCFunction(ctx, lr_error_constructor, "Error", 1);
        /* Set Error.prototype */
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, error_ctor));
        JS_SetPropertyStr(ctx, error_ctor, "prototype", JS_DupValue(ctx, proto));
        /* proto is now owned by the "prototype" property, do NOT free it */
        JS_SetPropertyStr(ctx, global, "Error", JS_DupValue(ctx, error_ctor));
    }

    /* Add Error.captureStackTrace */
    if (!JS_IsUndefined(error_ctor) && !JS_IsException(error_ctor)) {
        JSValue capture_stack_fn = JS_NewCFunction(ctx,
            lr_error_capture_stack_trace, "captureStackTrace", 2);
        JS_SetPropertyStr(ctx, error_ctor, "captureStackTrace", capture_stack_fn);

        /* Add Error.stackTraceLimit as a data property on Error constructor */
        JS_SetPropertyStr(ctx, error_ctor, "stackTraceLimit", JS_NewInt32(ctx, 10));

        /* Add stack property on Error.prototype (initially undefined, built lazily) */
        JSValue proto = JS_GetPropertyStr(ctx, error_ctor, "prototype");
        if (!JS_IsUndefined(proto) && !JS_IsException(proto)) {
            /* Set stack as a simple data property - it will be lazily built */
            JS_SetPropertyStr(ctx, proto, "stack", JS_UNDEFINED);
            JS_FreeValue(ctx, proto);
        }
    }

    JS_FreeValue(ctx, error_ctor);
    JS_FreeValue(ctx, global);
}

void lr_register_builtins(LR_Runtime *rt)
{
    /* Register gc() and gcStats() as global functions */
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "gc",
                      JS_NewCFunction(ctx, js_gc, "gc", 0));
    JS_SetPropertyStr(ctx, global, "gcStats",
                      JS_NewCFunction(ctx, js_gc_stats, "gcStats", 0));

    /* ES2020 globalThis */
    JS_SetPropertyStr(ctx, global, "globalThis", JS_DupValue(ctx, global));

    JS_FreeValue(ctx, global);

    lr_console_init(rt);
    lr_timers_init(rt);
    lr_encoding_init(rt);
    lr_url_init(rt);
    lr_event_init(rt);
    lr_performance_init(rt);
    lr_crypto_init(rt);
    lr_storage_init(rt);
    lr_fetch_init(rt);
    lr_fs_init(rt);
    lr_terminal_init(rt);
    lr_sysinfo_init(rt);
    lr_worker_init(rt);
    lr_canvas_init(rt);
    lr_promise_init(rt);
    lr_proxy_init(rt);
    lr_reflect_init(rt);
    lr_map_init(rt);
    lr_set_init(rt);

    /* Register ES2022 built-in objects */
    lr_builtins_core_init(rt);
    lr_builtins_extra_init(rt);

    /* Register Error stack trace builtins (after Object.prototype is available) */
    lr_register_error_builtins(rt);
}