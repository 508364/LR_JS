/*
 * L/R_JS - Lightweight/Runtime JavaScript Engine
 * Pure C, ES2022-compatible, browser-style JS runtime built on lightweight JS engine.
 *
 * Copyright (c) 2026 L/R_JS Authors
 * MIT License
 */
#ifndef LR_JS_H
#define LR_JS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── DLL Export / Import ───────────────────────────────────────────────── */
#if defined(_WIN32) || defined(_WIN64)
  #ifdef LR_JS_BUILD_DLL
    #define LR_API __declspec(dllexport)
  #elif defined(LR_JS_USE_DLL)
    #define LR_API __declspec(dllimport)
  #else
    #define LR_API
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define LR_API __attribute__((visibility("default")))
#else
  #define LR_API
#endif

/* ── Forward declarations ─────────────────────────────────────────────── */

typedef struct LR_Runtime LR_Runtime;
typedef struct LR_Config   LR_Config;

/* ── Configuration ────────────────────────────────────────────────────── */

typedef enum {
    LR_LOG_NONE  = 0,
    LR_LOG_ERROR = 1,
    LR_LOG_WARN  = 2,
    LR_LOG_INFO  = 3,
    LR_LOG_DEBUG = 4,
} LR_LogLevel;

typedef enum {
    LR_GC_MODE_AUTO      = 0,  /* auto GC triggered by threshold */
    LR_GC_MODE_MANUAL    = 1,  /* user must call lr_gc() explicitly */
    LR_GC_MODE_STRESS    = 2,  /* GC after every allocation (for debugging) */
    LR_GC_MODE_GENERATIONAL = 3,  /* generational GC with nursery */
    LR_GC_MODE_INCREMENTAL = 4,  /* incremental GC with time slicing */
} LR_GCMode;

struct LR_Config {
    /* Memory */
    size_t        memory_limit;        /* max heap bytes, 0 = unlimited */
    size_t        gc_threshold;        /* bytes before auto GC, 0 = default */
    LR_GCMode     gc_mode;

    /* GC tuning */
    int           gc_generational;     /* enable generational GC (nursery) */
    int           gc_incremental;      /* enable incremental GC (time-sliced) */
    size_t        gc_nursery_size;     /* nursery size in bytes, 0 = default (4MB) */
    int64_t       gc_pause_target_ns;  /* target max pause per GC slice, 0 = default (5ms) */

    /* IOME586 result cache */
    char         *bytecode_cache_dir;  /* IOME586 cache directory (.lrfile.lz4 archives, NULL = disabled) */

    /* Sandbox log */
    char         *sandbox_log_dir;     /* Path to sandbox log directory (NULL = disabled) */

    size_t        min_system_memory;   /* minimum system available memory (bytes), 0 = check disabled */
    int           skip_memory_check;   /* 1 = skip the system memory check entirely */

    /* Stack */
    size_t        max_stack_size;      /* 0 = default (1MB) */

    /* Execution */
    int           timeout_ms;          /* max script execution ms, 0 = none */
    int           strict_mode;         /* force strict mode, 1 = yes */

    /* Debug */
    LR_LogLevel   log_level;
    int           dump_bytecode;       /* 1 = dump compiled bytecode */
    int           strip_debug_info;    /* 1 = strip source/debug info */

    /* Multi-threading */
    int           enable_thread_pool;  /* Enable multi-threaded execution */
    int           thread_pool_size;    /* Number of worker threads (0 = auto) */
    int           enable_sandbox;      /* Enable sandbox isolation */

    /* Performance optimizations */
    int           enable_perf_optimizations;  /* Bytecode cache, concurrent GC, etc. */

    /* Renderer */
    int           enable_renderer;     /* Enable renderer bridge */

    /* Module resolution */
    const char  **module_paths;        /* NULL-terminated array of search paths */
    int           module_paths_count;

    /* I/O */
    FILE         *stdin_override;
    FILE         *stdout_override;
    FILE         *stderr_override;
};

/* ── Default configuration ────────────────────────────────────────────── */

LR_API void lr_config_default(LR_Config *cfg);

/* ── System memory check ──────────────────────────────────────────────── */

/* Check available system memory. Returns 0 if sufficient, -1 if below min.
   On Linux, reads /proc/meminfo. On other platforms, uses sysconf. */
LR_API int  lr_check_system_memory(size_t min_bytes);

/* Get available system memory in bytes. Returns -1 on error. */
LR_API int64_t lr_get_available_memory(void);

/* ── Runtime lifecycle ────────────────────────────────────────────────── */

LR_API LR_Runtime *lr_runtime_new(const LR_Config *cfg);
LR_API void        lr_runtime_free(LR_Runtime *rt);

/* ── Script execution ─────────────────────────────────────────────────── */

/* Evaluate JS source code. Returns 0 on success, -1 on error. */
LR_API int  lr_eval(LR_Runtime *rt, const char *source, size_t source_len,
             const char *filename);

/* Evaluate a JS file. Returns 0 on success, -1 on error. */
LR_API int  lr_eval_file(LR_Runtime *rt, const char *filename);

/* Evaluate as ES module. Returns 0 on success, -1 on error. */
LR_API int  lr_eval_module(LR_Runtime *rt, const char *source, size_t source_len,
                    const char *filename);

/* ── Event loop ───────────────────────────────────────────────────────── */

/* Run the event loop until no more pending tasks.
   Returns 0 on success, -1 on error. */
LR_API int  lr_event_loop_run(LR_Runtime *rt);

/* Run the event loop for at most `timeout_ms` milliseconds.
   Returns 0 on success, -1 on error. */
LR_API int  lr_event_loop_run_timeout(LR_Runtime *rt, int timeout_ms);

/* Check if the event loop has pending work. */
LR_API int  lr_event_loop_pending(LR_Runtime *rt);

/* Stop the event loop on next iteration. */
LR_API void lr_event_loop_stop(LR_Runtime *rt);

/* ── GC ───────────────────────────────────────────────────────────────── */

LR_API void lr_gc(LR_Runtime *rt);

/* Print GC statistics (generational + incremental). */
LR_API void lr_gc_print_stats(LR_Runtime *rt, FILE *fp);

/* Reset GC statistics. */
LR_API void lr_gc_reset_stats(LR_Runtime *rt);

/* ── IOME586 Result Cache ──────────────────────────────────────────────── */
/* IOME586 archives interpreter results (AST + globals + per-node results +
 * run state) as hash-keyed LZ4 packages (<hash>.lrfile.lz4). The historical
 * function names are kept for API stability. */

/* Print IOME586 cache statistics. */
LR_API void lr_bytecode_cache_stats(LR_Runtime *rt, FILE *fp);

/* Reset IOME586 cache statistics. */
LR_API void lr_bytecode_cache_clear(LR_Runtime *rt);

/* ── Memory usage ─────────────────────────────────────────────────────── */

typedef struct LR_MemoryUsage {
    int64_t malloc_size;
    int64_t malloc_limit;
    int64_t memory_used_size;
    int64_t malloc_count;
    int64_t memory_used_count;
    int64_t atom_count, atom_size;
    int64_t str_count, str_size;
    int64_t obj_count, obj_size;
    int64_t prop_count, prop_size;
    int64_t shape_count, shape_size;
    int64_t js_func_count, js_func_size, js_func_code_size;
    int64_t c_func_count, array_count;
    int64_t fast_array_count, fast_array_elements;
    int64_t binary_object_count, binary_object_size;
} LR_MemoryUsage;

LR_API void lr_compute_memory_usage(LR_Runtime *rt, LR_MemoryUsage *usage);
LR_API void lr_dump_memory_usage(LR_Runtime *rt, FILE *fp);

/* ── Error handling ───────────────────────────────────────────────────── */

LR_API const char *lr_get_last_error(LR_Runtime *rt);
LR_API void        lr_clear_last_error(LR_Runtime *rt);

/* ══════════════════════════════════════════════════════════════════════════
 *  Host Wrapper Interfaces
 *
 *  L/R_JS delegates privileged operations (HTTP, file system, terminal)
 *  to the host application through these wrapper interfaces.
 *  See docs/API.md for detailed usage.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── HTTP Wrapper ─────────────────────────────────────────────────────── */

typedef struct LR_HttpResult {
    int         status_code;
    char       *status_text;
    char       *headers;
    char       *body;
    size_t      body_len;
    char       *error;
} LR_HttpResult;

typedef struct LR_HttpWrapper {
    void *user_data;
    int (*fetch)(void *user_data, const char *method, const char *url,
                 const char *headers, const void *body, size_t body_len,
                 LR_HttpResult *result);
} LR_HttpWrapper;

LR_API void lr_http_set_wrapper(LR_Runtime *rt, LR_HttpWrapper *wrapper);
LR_API LR_HttpWrapper *lr_http_get_wrapper(LR_Runtime *rt);
LR_API void lr_http_result_free(LR_HttpResult *result);

/* ── WebSocket Wrapper ────────────────────────────────────────────────── */
/*
 * Like the HTTP wrapper, L/R_JS implements NO WebSocket protocol internally.
 * It delegates the connection to the host application through LR_WsWrapper.
 *
 * The host's connect() callback opens the connection and returns a connection
 * handle; the actual "open" event is reported later via lr_ws_on_open().
 * The host delivers inbound data and lifecycle events by calling the engine
 * side lr_ws_on_*() functions, which must be invoked from the engine thread
 * (e.g. from the host's I/O pump integrated with lr_event_loop_run()).
 */
typedef struct LR_WsWrapper {
    void *user_data;  /* opaque data passed to every callback */

    /* Initiate a connection. On success store the host connection handle in
       *out_handle and return 0; the open event arrives later via
       lr_ws_on_open(). Return -1 for immediate failure. */
    int (*connect)(void *user_data, const char *url, const char *protocols,
                   void **out_handle);

    /* Send a text frame. Return 0 on success, -1 on error. */
    int (*send)(void *user_data, void *conn_handle,
                const void *data, size_t len);

    /* Close the connection. code/reason may be 0/NULL. Return 0 on success. */
    int (*close)(void *user_data, void *conn_handle, int code, const char *reason);
} LR_WsWrapper;

LR_API void lr_ws_set_wrapper(LR_Runtime *rt, LR_WsWrapper *wrapper);
LR_API LR_WsWrapper *lr_ws_get_wrapper(LR_Runtime *rt);

/* Engine-side callbacks the HOST invokes to push WebSocket events into JS.
   Must be called from the engine thread. */
LR_API void lr_ws_on_open(LR_Runtime *rt, void *conn_handle);
LR_API void lr_ws_on_message(LR_Runtime *rt, void *conn_handle,
                              const void *data, size_t len);
LR_API void lr_ws_on_close(LR_Runtime *rt, void *conn_handle,
                            int code, const char *reason);
LR_API void lr_ws_on_error(LR_Runtime *rt, void *conn_handle,
                            const char *message);

/* ── File System Wrapper ──────────────────────────────────────────────── */

typedef struct LR_FileResult {
    int         error_code;
    char       *error;
    char       *data;
    size_t      data_len;
    int         is_dir;
    int         is_file;
    size_t      file_size;
    char      **entries;
    int         entry_count;
} LR_FileResult;

typedef struct LR_FileWrapper {
    void *user_data;
    int (*execute)(void *user_data, const char *path, const char *operation,
                   const void *data, size_t data_len, const char *extra,
                   LR_FileResult *result);
} LR_FileWrapper;

LR_API void lr_file_set_wrapper(LR_Runtime *rt, LR_FileWrapper *wrapper);
LR_API LR_FileWrapper *lr_file_get_wrapper(LR_Runtime *rt);
LR_API void lr_file_result_free(LR_FileResult *result);

/* ── Terminal Wrapper ─────────────────────────────────────────────────── */

typedef struct LR_TerminalResult {
    int         error_code;
    char       *error;
    char       *stdout_data;
    size_t      stdout_len;
    char       *stderr_data;
    size_t      stderr_len;
    int         exit_code;
} LR_TerminalResult;

typedef struct LR_TerminalWrapper {
    void *user_data;
    int (*execute)(void *user_data, const char *command, const char *operation,
                   const void *stdin_data, size_t stdin_len,
                   LR_TerminalResult *result);
} LR_TerminalWrapper;

LR_API void lr_terminal_set_wrapper(LR_Runtime *rt, LR_TerminalWrapper *wrapper);
LR_API LR_TerminalWrapper *lr_terminal_get_wrapper(LR_Runtime *rt);
LR_API void lr_terminal_result_free(LR_TerminalResult *result);

/* ── Version ──────────────────────────────────────────────────────────── */

#define LR_JS_VERSION_MAJOR 0
#define LR_JS_VERSION_MINOR 1
#define LR_JS_VERSION_PATCH 0

/* Single source of truth: LR_JS_VERSION_STRING is derived from the three
 * numbers above, so the version only needs to be bumped in one place.
 * Build scripts (build_*.sh / build_all.bat) parse these macros too. */
#define LR_JS_VERSION_XSTR_(x) #x
#define LR_JS_VERSION_XSTR(x)  LR_JS_VERSION_XSTR_(x)
#define LR_JS_VERSION_STRING \
    LR_JS_VERSION_XSTR(LR_JS_VERSION_MAJOR) "." \
    LR_JS_VERSION_XSTR(LR_JS_VERSION_MINOR) "." \
    LR_JS_VERSION_XSTR(LR_JS_VERSION_PATCH)

LR_API const char *lr_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LR_JS_H */