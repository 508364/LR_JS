/*
 * L/R_JS - Sandbox Independent Logging System
 * Pure C, per-sandbox log files with UUID identification.
 *
 * Log file naming: {date}-{run_count}-{uuid}.log
 *   e.g. 2026-07-19-3-a1b2c3d4-e5f6-47a8-b9c0-d1e2f3a4b5c6.log
 *
 * Features:
 *   - Each sandbox has a unique UUID
 *   - Log files stored in a unified log directory
 *   - Log directory path can be absolute or relative
 *   - If log_dir is not set, logging is disabled
 *   - Log system runs independently (thread-safe, non-blocking)
 *   - Daily run count tracks how many times the sandbox ran today
 *   - Timestamped log entries with log levels
 */

#ifndef LR_SANDBOX_LOG_H
#define LR_SANDBOX_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "lr_platform.h"

/* Forward declarations */
typedef struct LR_Sandbox     LR_Sandbox;
typedef struct LR_SandboxLog  LR_SandboxLog;

/* ── Log levels ────────────────────────────────────────────────────────── */

typedef enum {
    LR_SLOG_NONE   = 0,
    LR_SLOG_ERROR  = 1,
    LR_SLOG_WARN   = 2,
    LR_SLOG_INFO   = 3,
    LR_SLOG_DEBUG  = 4,
    LR_SLOG_TRACE  = 5,
} LR_SandboxLogLevel;

/* ── Log entry ─────────────────────────────────────────────────────────── */

typedef struct LR_LogEntry {
    int64_t           timestamp_us;  /* Microsecond timestamp */
    LR_SandboxLogLevel level;
    char             *message;
    int               eval_id;       /* Which eval call this belongs to */
    struct LR_LogEntry *next;
} LR_LogEntry;

/* ── Sandbox log ───────────────────────────────────────────────────────── */

struct LR_SandboxLog {
    /* Identity */
    char            uuid[37];        /* UUID v4 string: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    char           *log_dir;         /* Log directory path (NULL = disabled) */
    int             enabled;         /* 1 = logging enabled */

    /* Level filtering (dynamic, runtime adjustable) */
    LR_SandboxLogLevel current_level; /* Current log level filter */
    LR_SandboxLogLevel default_level; /* Default level */

    /* Daily tracking */
    char            today_date[11];  /* YYYY-MM-DD */
    int             today_run_count; /* How many times sandbox ran today */
    char            current_log_path[4096]; /* Current log file path */

    /* Log file handle */
    FILE           *log_file;        /* Currently open log file */
    pthread_mutex_t file_mutex;      /* Mutex for file access */

    /* In-memory log buffer (for async flushing) */
    LR_LogEntry    *entries_head;
    LR_LogEntry    *entries_tail;
    int             entry_count;
    int             max_entries;
    pthread_mutex_t entries_mutex;

    /* Statistics */
    int64_t         total_entries;
    int64_t         flushed_entries;
    int64_t         dropped_entries;

    /* Independent thread */
    pthread_t       flush_thread;
    int             flush_thread_running;
    int             flush_interval_ms;  /* How often to flush to disk */
    pthread_cond_t  flush_cond;
    pthread_mutex_t flush_mutex;

    /* Back-reference */
    LR_Sandbox     *sandbox;
};

/* ── API ────────────────────────────────────────────────────────────────── */

/* Initialize the sandbox logging system.
 * log_dir can be NULL to disable logging.
 * Returns 0 on success, -1 on error. */
int  lr_sandbox_log_init(LR_SandboxLog *log, LR_Sandbox *sb, const char *log_dir);

/* Destroy the sandbox logging system. Flushes remaining entries. */
void lr_sandbox_log_destroy(LR_SandboxLog *log);

/* ── Configuration ──────────────────────────────────────────────────────── */

/* Set log directory. Pass NULL to disable. */
int  lr_sandbox_log_set_dir(LR_SandboxLog *log, const char *dir);

/* Set log level filter. */
void lr_sandbox_log_set_level(LR_SandboxLog *log, LR_SandboxLogLevel level);

/* Get current log level. */
LR_SandboxLogLevel lr_sandbox_log_get_level(LR_SandboxLog *log);

/* Set flush interval in milliseconds. */
void lr_sandbox_log_set_flush_interval(LR_SandboxLog *log, int interval_ms);

/* ── Logging ────────────────────────────────────────────────────────────── */

/* Log a message at the given level. Thread-safe, non-blocking.
 * Message is formatted with printf-style arguments. */
void lr_sandbox_log_write(LR_SandboxLog *log, LR_SandboxLogLevel level,
                          int eval_id, const char *fmt, ...);

/* Convenience macros */
#define lr_slog_error(log, eval_id, fmt, ...) \
    lr_sandbox_log_write(log, LR_SLOG_ERROR, eval_id, fmt, ##__VA_ARGS__)
#define lr_slog_warn(log, eval_id, fmt, ...) \
    lr_sandbox_log_write(log, LR_SLOG_WARN, eval_id, fmt, ##__VA_ARGS__)
#define lr_slog_info(log, eval_id, fmt, ...) \
    lr_sandbox_log_write(log, LR_SLOG_INFO, eval_id, fmt, ##__VA_ARGS__)
#define lr_slog_debug(log, eval_id, fmt, ...) \
    lr_sandbox_log_write(log, LR_SLOG_DEBUG, eval_id, fmt, ##__VA_ARGS__)
#define lr_slog_trace(log, eval_id, fmt, ...) \
    lr_sandbox_log_write(log, LR_SLOG_TRACE, eval_id, fmt, ##__VA_ARGS__)

/* ── Flush ──────────────────────────────────────────────────────────────── */

/* Force flush all buffered log entries to disk. */
void lr_sandbox_log_flush(LR_SandboxLog *log);

/* ── Daily tracking ─────────────────────────────────────────────────────── */

/* Check if date has changed and rotate log file if needed. */
void lr_sandbox_log_check_date(LR_SandboxLog *log);

/* Increment today's run count (called when sandbox starts a new eval). */
void lr_sandbox_log_increment_run(LR_SandboxLog *log);

/* ── Statistics ─────────────────────────────────────────────────────────── */

/* Print log statistics. */
void lr_sandbox_log_stats(LR_SandboxLog *log, FILE *fp);

/* ── UUID generation ────────────────────────────────────────────────────── */

/* Generate a UUID v4 string. Output must be at least 37 bytes. */
void lr_uuid_v4(char out[37]);

#endif /* LR_SANDBOX_LOG_H */