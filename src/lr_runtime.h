/*
 * L/R_JS - Internal Runtime Header
 * Pure C, ES2022-compatible, multi-threaded async sandbox
 */
#ifndef LR_RUNTIME_H
#define LR_RUNTIME_H

#include "lr_js.h"
#include "engine/lr_engine.h"

#include "lr_perf_opt.h"
#include "lr_gc.h"
#include "lr_iome586.h"

/* ── Runtime structure ────────────────────────────────────────────────── */

struct LR_Runtime {
    LRRuntime    *lr_rt;
    LRContext    *lr_ctx;

    LR_Config     config;
    char         *last_error;
    int           event_loop_stop;

    /* Event loop state */
    int           event_loop_running;
    int           has_pending_jobs;

    /* Timers */
    void         *timer_list;    /* opaque; see lr_timers.c */

    /* Module loader */
    void         *module_data;   /* opaque; see lr_runtime.c */

    /* Performance optimizations */
    LR_PerfOptCtx  *v8opt;         /* NULL if not enabled */

    /* GC context (generational + incremental) */
    LR_GCContext  gc_ctx;

    /* IOME586 result cache */
    LR_Iome586Cache iome586;

    /* I/O */
    FILE         *stdin_fp;
    FILE         *stdout_fp;
    FILE         *stderr_fp;

    /* HTTP fetch wrapper (delegates to host application) */
    struct LR_HttpWrapper *http_wrapper;

    /* File system wrapper (privileged operations) */
    struct LR_FileWrapper *file_wrapper;

    /* Terminal wrapper (privileged command execution) */
    struct LR_TerminalWrapper *terminal_wrapper;

    /* WebSocket wrapper (delegates to host application) */
    struct LR_WsWrapper *ws_wrapper;

    /* WebSocket connection registry (conn_handle -> JS object), opaque */
    void         *ws_registry;
};

/* ── Internal helpers ─────────────────────────────────────────────────── */

/* Set last error string (copies str). */
void lr_set_error(LR_Runtime *rt, const char *fmt, ...);

/* Log a message at the given level. */
void lr_log(LR_Runtime *rt, LR_LogLevel level, const char *fmt, ...);

/* Check and report JS exceptions. Returns -1 if exception, 0 if ok. */
int  lr_check_exception(LR_Runtime *rt);

/* Load a file into a buffer. Caller must free with free(). */
uint8_t *lr_load_file(LR_Runtime *rt, const char *filename, size_t *out_len);

/* Register all built-in browser APIs on the global object. */
void lr_register_builtins(LR_Runtime *rt);

/* ── Per-module init declarations ─────────────────────────────────────── */

void lr_console_init(LR_Runtime *rt);
void lr_timers_init(LR_Runtime *rt);
void lr_fetch_init(LR_Runtime *rt);
void lr_ws_init(LR_Runtime *rt);

/* Reusable EventTarget methods (defined in lr_event.c) for class prototypes */
JSValue lr_event_target_addEventListener(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue lr_event_target_removeEventListener(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
JSValue lr_event_target_dispatchEvent(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
void lr_fs_init(LR_Runtime *rt);
void lr_terminal_init(LR_Runtime *rt);
void lr_sysinfo_init(LR_Runtime *rt);
void lr_url_init(LR_Runtime *rt);
void lr_encoding_init(LR_Runtime *rt);
void lr_event_init(LR_Runtime *rt);
void lr_performance_init(LR_Runtime *rt);
void lr_crypto_init(LR_Runtime *rt);
void lr_storage_init(LR_Runtime *rt);
void lr_worker_init(LR_Runtime *rt);
void lr_canvas_init(LR_Runtime *rt);
void lr_promise_init(struct LR_Runtime *rt);
void lr_proxy_init(struct LR_Runtime *rt);
void lr_reflect_init(struct LR_Runtime *rt);
void lr_map_init(struct LR_Runtime *rt);
void lr_set_init(struct LR_Runtime *rt);

/* ES2022 built-in objects */
void lr_builtins_core_init(struct LR_Runtime *rt);
void lr_builtins_extra_init(struct LR_Runtime *rt);

/* Per-module cleanup */
void lr_timers_cleanup(LR_Runtime *rt);

/* Timer processing (called from event loop) */
void lr_timers_process(LR_Runtime *rt);

/* Worker message pump (called from event loop): drains worker->parent
 * message queues and fires onmessage/onerror callbacks.
 * Returns the number of still-running workers owned by rt. */
int lr_worker_poll(LR_Runtime *rt);

/* ── Performance optimization helpers (internal) ──────────────────────── */

void lr_perfopt_attach(LR_PerfOptCtx *opt, LR_Runtime *rt);
void lr_perfopt_detach(LR_PerfOptCtx *opt);
void lr_perfopt_print_stats(LR_PerfOptCtx *opt, FILE *fp);

#endif /* LR_RUNTIME_H */