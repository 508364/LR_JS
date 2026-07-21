/*
 * L/R_JS - Sandbox Isolation Implementation
 * Pure C, per-sandbox resource limits, timeout control, security isolation.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_platform.h"

#include "lr_sandbox.h"
#include "lr_sandbox_log.h"
#include "lr_runtime.h"

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int64_t get_time_us(void)
{
    return lr_get_time_us();
}

const char *lr_sandbox_state_str(LR_SandboxState state)
{
    switch (state) {
    case LR_SANDBOX_IDLE:      return "idle";
    case LR_SANDBOX_RUNNING:   return "running";
    case LR_SANDBOX_TIMED_OUT: return "timed_out";
    case LR_SANDBOX_OOM:       return "out_of_memory";
    case LR_SANDBOX_ERROR:     return "error";
    case LR_SANDBOX_STOPPED:   return "stopped";
    default:                   return "unknown";
    }
}

void lr_sandbox_config_default(LR_SandboxConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->name              = NULL;
    cfg->memory_limit      = 0;
    cfg->stack_size        = 0;
    cfg->timeout_ms        = 0;
    cfg->max_eval_depth    = 0;
    cfg->allow_network     = 1;
    cfg->allow_filesystem  = 1;
    cfg->allow_workers     = 0;
    cfg->isolated_context  = 0;
}

/* ── Sandbox manager ───────────────────────────────────────────────────── */

LR_SandboxManager *lr_sandbox_manager_create(int max_sandboxes)
{
    LR_SandboxManager *mgr = calloc(1, sizeof(LR_SandboxManager));
    if (!mgr) return NULL;

    mgr->max_sandboxes = max_sandboxes > 0 ? max_sandboxes : 64;
    mgr->active_count = 0;
    mgr->next_id = 1;
    pthread_mutex_init(&mgr->mutex, NULL);
    return mgr;
}

void lr_sandbox_manager_destroy(LR_SandboxManager *mgr)
{
    if (!mgr) return;

    /* Destroy all sandboxes */
    LR_Sandbox *sb = mgr->head;
    while (sb) {
        LR_Sandbox *next = sb->next;
        if (sb->runtime) lr_runtime_free(sb->runtime);
        free(sb->name);
        pthread_mutex_destroy(&sb->exec_mutex);
        free(sb);
        sb = next;
    }

    pthread_mutex_destroy(&mgr->mutex);
    free(mgr);
}

/* ── Sandbox ───────────────────────────────────────────────────────────── */

LR_Sandbox *lr_sandbox_create(LR_SandboxManager *mgr,
                              const LR_SandboxConfig *config)
{
    if (!mgr) return NULL;

    pthread_mutex_lock(&mgr->mutex);
    if (mgr->active_count >= mgr->max_sandboxes) {
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }
    mgr->active_count++;
    pthread_mutex_unlock(&mgr->mutex);

    LR_Sandbox *sb = calloc(1, sizeof(LR_Sandbox));
    if (!sb) {
        pthread_mutex_lock(&mgr->mutex);
        mgr->active_count--;
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    sb->id = mgr->next_id++;
    sb->name = config->name ? strdup(config->name) : NULL;
    sb->config = *config;
    sb->state = LR_SANDBOX_IDLE;
    pthread_mutex_init(&sb->exec_mutex, NULL);

    /* Create isolated JS runtime */
    LR_Config rt_cfg;
    lr_config_default(&rt_cfg);
    rt_cfg.log_level = LR_LOG_ERROR;

    if (config->memory_limit > 0) {
        rt_cfg.memory_limit = config->memory_limit;
    }
    if (config->stack_size > 0) {
        rt_cfg.max_stack_size = config->stack_size;
    }

    sb->runtime = lr_runtime_new(&rt_cfg);
    if (!sb->runtime) {
        pthread_mutex_destroy(&sb->exec_mutex);
        free(sb->name);
        free(sb);
        pthread_mutex_lock(&mgr->mutex);
        mgr->active_count--;
        pthread_mutex_unlock(&mgr->mutex);
        return NULL;
    }

    /* Generate UUID */
    lr_uuid_v4(sb->uuid);

    /* Initialize logging if log_dir is configured */
    if (config->log_dir && config->log_dir[0]) {
        sb->log = calloc(1, sizeof(LR_SandboxLog));
        if (sb->log) {
            lr_sandbox_log_init(sb->log, sb, config->log_dir);
            lr_slog_info(sb->log, -1, "Sandbox created: %s (uuid=%s)",
                         sb->name ? sb->name : "(unnamed)", sb->uuid);
        }
    }

    /* Add to manager list */
    pthread_mutex_lock(&mgr->mutex);
    sb->next = mgr->head;
    mgr->head = sb;
    pthread_mutex_unlock(&mgr->mutex);

    return sb;
}

void lr_sandbox_destroy(LR_SandboxManager *mgr, LR_Sandbox *sb)
{
    if (!mgr || !sb) return;

    sb->state = LR_SANDBOX_STOPPED;

    /* Log final entry */
    if (sb->log) {
        lr_slog_info(sb->log, -1, "Sandbox destroyed: %s (uuid=%s, evals=%d, errors=%d)",
                     sb->name ? sb->name : "(unnamed)", sb->uuid,
                     sb->eval_count, sb->error_count);
        lr_sandbox_log_destroy(sb->log);
        free(sb->log);
        sb->log = NULL;
    }

    /* Remove from manager list */
    pthread_mutex_lock(&mgr->mutex);
    if (mgr->head == sb) {
        mgr->head = sb->next;
    } else {
        LR_Sandbox *prev = mgr->head;
        while (prev && prev->next != sb) prev = prev->next;
        if (prev) prev->next = sb->next;
    }
    mgr->active_count--;
    pthread_mutex_unlock(&mgr->mutex);

    if (sb->runtime) lr_runtime_free(sb->runtime);
    free(sb->name);
    pthread_mutex_destroy(&sb->exec_mutex);
    free(sb);
}

int lr_sandbox_eval(LR_Sandbox *sb, const char *source,
                    size_t source_len, const char *filename)
{
    if (!sb || !source) return -1;

    pthread_mutex_lock(&sb->exec_mutex);

    if (sb->state == LR_SANDBOX_STOPPED) {
        pthread_mutex_unlock(&sb->exec_mutex);
        return -1;
    }

    sb->state = LR_SANDBOX_RUNNING;
    sb->start_time_us = get_time_us();
    sb->timeout_triggered = 0;
    sb->eval_count++;

    /* Increment run count for log naming */
    if (sb->log) {
        lr_sandbox_log_increment_run(sb->log);
        lr_slog_info(sb->log, sb->eval_count, "Eval started: %s",
                     filename ? filename : "(eval)");
    }

    int result = lr_eval(sb->runtime, source, source_len, filename);

    int64_t elapsed = get_time_us() - sb->start_time_us;
    sb->total_cpu_time_us += elapsed;

    if (result < 0) {
        sb->error_count++;
        sb->state = LR_SANDBOX_ERROR;
        if (sb->log) {
            lr_slog_error(sb->log, sb->eval_count, "Eval failed after %.2f ms: %s",
                          (double)elapsed / 1000.0, filename ? filename : "(eval)");
        }
    } else {
        sb->state = LR_SANDBOX_IDLE;
        if (sb->log) {
            lr_slog_info(sb->log, sb->eval_count, "Eval completed in %.2f ms",
                         (double)elapsed / 1000.0);
        }
    }

    /* Check timeout */
    if (sb->config.timeout_ms > 0 &&
        elapsed > (int64_t)sb->config.timeout_ms * 1000) {
        sb->timeout_triggered = 1;
        sb->state = LR_SANDBOX_TIMED_OUT;
        result = -1;
    }

    /* Track peak memory */
    LR_MemoryUsage usage;
    lr_compute_memory_usage(sb->runtime, &usage);
    if ((size_t)usage.malloc_size > sb->peak_memory) {
        sb->peak_memory = (size_t)usage.malloc_size;
    }

    /* Check OOM */
    if (sb->config.memory_limit > 0 && usage.malloc_size >= (int64_t)sb->config.memory_limit) {
        sb->state = LR_SANDBOX_OOM;
    }

    pthread_mutex_unlock(&sb->exec_mutex);
    return result;
}

int lr_sandbox_check_limits(LR_Sandbox *sb)
{
    if (!sb) return 0;

    if (sb->timeout_triggered) return 1;
    if (sb->state == LR_SANDBOX_OOM) return 1;
    if (sb->error_count > 100) return 1;  /* Too many errors */

    return 0;
}

LR_Sandbox *lr_sandbox_get(LR_SandboxManager *mgr, int id)
{
    if (!mgr) return NULL;

    pthread_mutex_lock(&mgr->mutex);
    for (LR_Sandbox *sb = mgr->head; sb; sb = sb->next) {
        if (sb->id == id) {
            pthread_mutex_unlock(&mgr->mutex);
            return sb;
        }
    }
    pthread_mutex_unlock(&mgr->mutex);
    return NULL;
}

LR_Sandbox *lr_sandbox_get_by_name(LR_SandboxManager *mgr, const char *name)
{
    if (!mgr || !name) return NULL;

    pthread_mutex_lock(&mgr->mutex);
    for (LR_Sandbox *sb = mgr->head; sb; sb = sb->next) {
        if (sb->name && strcmp(sb->name, name) == 0) {
            pthread_mutex_unlock(&mgr->mutex);
            return sb;
        }
    }
    pthread_mutex_unlock(&mgr->mutex);
    return NULL;
}

void lr_sandbox_foreach(LR_SandboxManager *mgr,
                        void (*cb)(LR_Sandbox *sb, void *arg),
                        void *arg)
{
    if (!mgr || !cb) return;

    pthread_mutex_lock(&mgr->mutex);
    for (LR_Sandbox *sb = mgr->head; sb; sb = sb->next) {
        cb(sb, arg);
    }
    pthread_mutex_unlock(&mgr->mutex);
}