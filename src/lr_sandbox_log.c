/*
 * L/R_JS - Sandbox Independent Logging System Implementation
 * Pure C, per-sandbox log files with UUID identification.
 *
 * Design:
 *   - Each sandbox gets a UUID v4 on creation
 *   - Log entries are buffered in-memory (non-blocking)
 *   - A dedicated flush thread periodically writes to disk
 *   - Daily log rotation: new file each day
 *   - Run count tracks executions per day
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include "lr_platform.h"

#include "lr_sandbox_log.h"
#include "lr_sandbox.h"

/* ── Platform helpers ──────────────────────────────────────────────────── */

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#define lr_mkdir(p) _mkdir(p)
#else
#define lr_mkdir(p) mkdir(p, 0755)
#endif

static int mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            lr_mkdir(tmp);
            *p = '/';
        }
    }
    return lr_mkdir(tmp);
}

/* ── UUID v4 generation ────────────────────────────────────────────────── */

void lr_uuid_v4(char out[37])
{
    /* Generate random bytes */
    unsigned char rand_bytes[16];
    int have_crypto = 0;

#ifdef _WIN32
    HCRYPTPROV hProv;
    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL,
                            CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        if (CryptGenRandom(hProv, 16, rand_bytes)) {
            have_crypto = 1;
        }
        CryptReleaseContext(hProv, 0);
    }
#else
    /* Try /dev/urandom */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(rand_bytes, 1, 16, f) == 16) {
            have_crypto = 1;
        }
        fclose(f);
    }
#endif

    /* Fallback: use time + pid + simple PRNG */
    if (!have_crypto) {
        int64_t now = (int64_t)time(NULL);
        int pid = (int)getpid();
        for (int i = 0; i < 16; i++) {
            now = now * 1103515245 + 12345;
            rand_bytes[i] = (unsigned char)((now >> 16) & 0xFF);
            rand_bytes[i] ^= (unsigned char)((pid >> (i % 4) * 8) & 0xFF);
        }
    }

    /* Set UUID v4 variant bits */
    rand_bytes[6] = (rand_bytes[6] & 0x0F) | 0x40;  /* version 4 */
    rand_bytes[8] = (rand_bytes[8] & 0x3F) | 0x80;  /* variant 1 */

    /* Format as hex string */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             rand_bytes[0], rand_bytes[1], rand_bytes[2], rand_bytes[3],
             rand_bytes[4], rand_bytes[5], rand_bytes[6], rand_bytes[7],
             rand_bytes[8], rand_bytes[9], rand_bytes[10], rand_bytes[11],
             rand_bytes[12], rand_bytes[13], rand_bytes[14], rand_bytes[15]);
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000;
}

static void get_today_date(char out[11])
{
    time_t t = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&t, &tm_buf);
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    /* bounds check: dates are always in valid range after localtime_r */
    if (y < 0 || y > 9999) y = 1970;
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

static const char *log_level_str(LR_SandboxLogLevel level)
{
    switch (level) {
    case LR_SLOG_ERROR: return "ERROR";
    case LR_SLOG_WARN:  return "WARN ";
    case LR_SLOG_INFO:  return "INFO ";
    case LR_SLOG_DEBUG: return "DEBUG";
    case LR_SLOG_TRACE: return "TRACE";
    default:            return "?????";
    }
}

/* ── Flush thread ──────────────────────────────────────────────────────── */

static void *flush_thread_func(void *arg)
{
    LR_SandboxLog *log = (LR_SandboxLog *)arg;

    while (log->flush_thread_running) {
        pthread_mutex_lock(&log->flush_mutex);

        /* Wait for flush interval or signal */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)(log->flush_interval_ms % 1000) * 1000000L;
        ts.tv_sec += log->flush_interval_ms / 1000
                     + ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;

        pthread_cond_timedwait(&log->flush_cond, &log->flush_mutex, &ts);
        pthread_mutex_unlock(&log->flush_mutex);

        if (!log->flush_thread_running) break;

        /* Flush entries */
        lr_sandbox_log_flush(log);
    }

    return NULL;
}

/* ── Init / Destroy ────────────────────────────────────────────────────── */

int lr_sandbox_log_init(LR_SandboxLog *log, LR_Sandbox *sb, const char *log_dir)
{
    memset(log, 0, sizeof(*log));

    /* Generate UUID */
    lr_uuid_v4(log->uuid);

    /* Initialize mutexes */
    pthread_mutex_init(&log->file_mutex, NULL);
    pthread_mutex_init(&log->entries_mutex, NULL);
    pthread_mutex_init(&log->flush_mutex, NULL);
    pthread_cond_init(&log->flush_cond, NULL);

    log->max_entries = 1000;
    log->flush_interval_ms = 500; /* flush every 500ms */
    log->sandbox = sb;
    log->current_level = LR_SLOG_INFO;  /* Default: INFO level */
    log->default_level = LR_SLOG_INFO;

    /* Set up log directory */
    if (log_dir && log_dir[0]) {
        if (lr_sandbox_log_set_dir(log, log_dir) != 0) {
            /* Directory creation failed, disable logging */
            log->enabled = 0;
        }
    }

    /* Set initial date */
    get_today_date(log->today_date);

    /* Start flush thread */
    log->flush_thread_running = 1;
    if (pthread_create(&log->flush_thread, NULL, flush_thread_func, log) != 0) {
        log->flush_thread_running = 0;
        /* Continue without flush thread; flushes happen on log_write */
    }

    return 0;
}

void lr_sandbox_log_destroy(LR_SandboxLog *log)
{
    /* Stop flush thread */
    if (log->flush_thread_running) {
        log->flush_thread_running = 0;
        pthread_cond_signal(&log->flush_cond);
        pthread_join(log->flush_thread, NULL);
    }

    /* Final flush */
    lr_sandbox_log_flush(log);

    /* Close log file */
    pthread_mutex_lock(&log->file_mutex);
    if (log->log_file) {
        fclose(log->log_file);
        log->log_file = NULL;
    }
    pthread_mutex_unlock(&log->file_mutex);

    /* Free remaining entries */
    pthread_mutex_lock(&log->entries_mutex);
    LR_LogEntry *entry = log->entries_head;
    while (entry) {
        LR_LogEntry *next = entry->next;
        free(entry->message);
        free(entry);
        entry = next;
    }
    log->entries_head = NULL;
    log->entries_tail = NULL;
    log->entry_count = 0;
    pthread_mutex_unlock(&log->entries_mutex);

    /* Cleanup */
    free(log->log_dir);
    pthread_mutex_destroy(&log->file_mutex);
    pthread_mutex_destroy(&log->entries_mutex);
    pthread_mutex_destroy(&log->flush_mutex);
    pthread_cond_destroy(&log->flush_cond);
}

/* ── Configuration ──────────────────────────────────────────────────────── */

int lr_sandbox_log_set_dir(LR_SandboxLog *log, const char *dir)
{
    if (!dir || !dir[0]) {
        pthread_mutex_lock(&log->file_mutex);
        if (log->log_file) {
            fclose(log->log_file);
            log->log_file = NULL;
        }
        free(log->log_dir);
        log->log_dir = NULL;
        log->enabled = 0;
        log->current_log_path[0] = '\0';
        pthread_mutex_unlock(&log->file_mutex);
        return 0;
    }

    /* Create directory */
    if (mkdir_p(dir) != 0) return -1;

    pthread_mutex_lock(&log->file_mutex);
    free(log->log_dir);
    log->log_dir = strdup(dir);
    log->enabled = 1;

    /* Build log file path */
    get_today_date(log->today_date);
    snprintf(log->current_log_path, sizeof(log->current_log_path),
             "%s/%s-%d-%s.log",
             log->log_dir, log->today_date, log->today_run_count, log->uuid);

    /* Open new log file */
    if (log->log_file) fclose(log->log_file);
    log->log_file = fopen(log->current_log_path, "a");
    pthread_mutex_unlock(&log->file_mutex);

    return 0;
}

void lr_sandbox_log_set_level(LR_SandboxLog *log, LR_SandboxLogLevel level)
{
    pthread_mutex_lock(&log->entries_mutex);
    log->current_level = level;
    pthread_mutex_unlock(&log->entries_mutex);
}

LR_SandboxLogLevel lr_sandbox_log_get_level(LR_SandboxLog *log)
{
    return log->current_level;
}

void lr_sandbox_log_set_flush_interval(LR_SandboxLog *log, int interval_ms)
{
    if (interval_ms >= 10) {
        log->flush_interval_ms = interval_ms;
    }
}

/* ── Logging ────────────────────────────────────────────────────────────── */

void lr_sandbox_log_write(LR_SandboxLog *log, LR_SandboxLogLevel level,
                          int eval_id, const char *fmt, ...)
{
    if (!log->enabled) return;

    /* Level filtering: skip messages below current level */
    if (level > log->current_level) return;

    /* Format message */
    char msg_buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
    va_end(ap);

    /* Create entry */
    LR_LogEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) return;

    entry->timestamp_us = get_time_us();
    entry->level = level;
    entry->message = strdup(msg_buf);
    entry->eval_id = eval_id;

    /* Add to buffer */
    pthread_mutex_lock(&log->entries_mutex);

    if (log->entry_count >= log->max_entries) {
        /* Drop oldest entry */
        LR_LogEntry *old = log->entries_head;
        if (old) {
            log->entries_head = old->next;
            if (!log->entries_head) log->entries_tail = NULL;
            free(old->message);
            free(old);
            log->entry_count--;
            log->dropped_entries++;
        }
    }

    if (log->entries_tail) {
        log->entries_tail->next = entry;
    } else {
        log->entries_head = entry;
    }
    log->entries_tail = entry;
    log->entry_count++;
    log->total_entries++;

    pthread_mutex_unlock(&log->entries_mutex);
}

/* ── Flush ──────────────────────────────────────────────────────────────── */

void lr_sandbox_log_flush(LR_SandboxLog *log)
{
    if (!log->enabled) return;

    /* Check date rotation */
    lr_sandbox_log_check_date(log);

    pthread_mutex_lock(&log->entries_mutex);
    pthread_mutex_lock(&log->file_mutex);

    /* Open log file if needed */
    if (!log->log_file && log->current_log_path[0]) {
        log->log_file = fopen(log->current_log_path, "a");
    }

    if (log->log_file) {
        LR_LogEntry *entry = log->entries_head;
        while (entry) {
            /* Format timestamp */
            time_t sec = (time_t)(entry->timestamp_us / 1000000);
            int usec = (int)(entry->timestamp_us % 1000000);
            struct tm tm_buf;
            struct tm *tm = localtime_r(&sec, &tm_buf);

            fprintf(log->log_file,
                    "[%04d-%02d-%02d %02d:%02d:%02d.%06d] [%s] [eval:%d] %s\n",
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec, usec,
                    log_level_str(entry->level),
                    entry->eval_id,
                    entry->message ? entry->message : "");

            entry = entry->next;
        }
        fflush(log->log_file);
    }

    /* Free flushed entries */
    LR_LogEntry *entry = log->entries_head;
    while (entry) {
        LR_LogEntry *next = entry->next;
        free(entry->message);
        free(entry);
        log->flushed_entries++;
        entry = next;
    }
    log->entries_head = NULL;
    log->entries_tail = NULL;
    log->entry_count = 0;

    pthread_mutex_unlock(&log->file_mutex);
    pthread_mutex_unlock(&log->entries_mutex);
}

/* ── Daily tracking ─────────────────────────────────────────────────────── */

void lr_sandbox_log_check_date(LR_SandboxLog *log)
{
    char today[11];
    get_today_date(today);

    if (strcmp(today, log->today_date) != 0) {
        /* Date changed, rotate log */
        memcpy(log->today_date, today, sizeof(log->today_date));
        log->today_date[sizeof(log->today_date) - 1] = '\0';
        log->today_run_count = 0;

        pthread_mutex_lock(&log->file_mutex);
        if (log->log_file) {
            fclose(log->log_file);
            log->log_file = NULL;
        }

        if (log->log_dir) {
            snprintf(log->current_log_path, sizeof(log->current_log_path),
                     "%s/%s-%d-%s.log",
                     log->log_dir, log->today_date,
                     log->today_run_count, log->uuid);
        }
        pthread_mutex_unlock(&log->file_mutex);
    }
}

void lr_sandbox_log_increment_run(LR_SandboxLog *log)
{
    lr_sandbox_log_check_date(log);
    log->today_run_count++;
}

/* ── Statistics ─────────────────────────────────────────────────────────── */

void lr_sandbox_log_stats(LR_SandboxLog *log, FILE *fp)
{
    fprintf(fp, "\n");
    fprintf(fp, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(fp, "║  Sandbox Log Statistics                                     ║\n");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  UUID:    %-48s ║\n", log->uuid);
    fprintf(fp, "║  Status:  %s                                         ║\n",
            log->enabled ? "enabled " : "disabled");
    fprintf(fp, "║  Log Dir: %-46s ║\n",
            log->log_dir ? log->log_dir : "(none)");
    fprintf(fp, "║  Log File: %-45s ║\n",
            log->current_log_path[0] ? log->current_log_path : "(none)");
    fprintf(fp, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(fp, "║  Date:        %-10s  Run count: %-6d               ║\n",
            log->today_date, log->today_run_count);
    fprintf(fp, "║  Total entries: %10lld  Flushed: %10lld           ║\n",
            (long long)log->total_entries, (long long)log->flushed_entries);
    fprintf(fp, "║  Buffered:      %10d  Dropped:  %10lld           ║\n",
            log->entry_count, (long long)log->dropped_entries);
    fprintf(fp, "║  Flush interval: %d ms                                    ║\n",
            log->flush_interval_ms);
    fprintf(fp, "╚══════════════════════════════════════════════════════════════╝\n");
    fprintf(fp, "\n");
}