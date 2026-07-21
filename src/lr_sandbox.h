/*
 * L/R_JS - Sandbox Isolation Module
 * Pure C, per-sandbox resource limits, timeout control, security isolation.
 */
#ifndef LR_SANDBOX_H
#define LR_SANDBOX_H

#include <stdint.h>
#include <stddef.h>
#include "lr_platform.h"

/* Forward declarations */
typedef struct LR_Runtime LR_Runtime;
typedef struct LR_Sandbox  LR_Sandbox;
typedef struct LR_SandboxLog LR_SandboxLog;

/* ── Sandbox ───────────────────────────────────────────────────────────── */

typedef enum {
    LR_SANDBOX_IDLE       = 0,
    LR_SANDBOX_RUNNING    = 1,
    LR_SANDBOX_TIMED_OUT  = 2,
    LR_SANDBOX_OOM        = 3,
    LR_SANDBOX_ERROR      = 4,
    LR_SANDBOX_STOPPED    = 5,
} LR_SandboxState;

typedef struct LR_SandboxConfig {
    char        *name;              /* Sandbox identifier */
    char        *log_dir;           /* Log directory path (NULL = disabled) */
    size_t       memory_limit;      /* Max heap bytes, 0 = inherit */
    size_t       stack_size;        /* Max stack bytes, 0 = default */
    int          timeout_ms;        /* Max execution time, 0 = no limit */
    int          max_eval_depth;    /* Max nested eval depth, 0 = default */
    int          allow_network;     /* 0 = block network APIs */
    int          allow_filesystem;  /* 0 = block file APIs */
    int          allow_workers;     /* 0 = block worker spawning */
    int          isolated_context;  /* 1 = no access to parent globals */
} LR_SandboxConfig;

struct LR_Sandbox {
    int             id;
    char           *name;
    char            uuid[37];       /* UUID v4 for unique identification */
    LR_Runtime     *runtime;
    LR_SandboxConfig config;
    LR_SandboxState  state;

    /* Logging */
    LR_SandboxLog  *log;           /* Sandbox-specific logging (NULL if disabled) */

    /* Execution control */
    pthread_mutex_t  exec_mutex;
    int              timeout_triggered;
    int64_t          start_time_us;
    int64_t          total_cpu_time_us;

    /* Resource tracking */
    size_t           peak_memory;
    int              eval_count;
    int              error_count;

    /* Linked list */
    LR_Sandbox      *next;
};

/* ── Sandbox manager ───────────────────────────────────────────────────── */

typedef struct LR_SandboxManager {
    LR_Sandbox     *head;
    int             next_id;
    int             max_sandboxes;
    int             active_count;
    pthread_mutex_t mutex;
} LR_SandboxManager;

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize sandbox manager. */
LR_SandboxManager *lr_sandbox_manager_create(int max_sandboxes);
void               lr_sandbox_manager_destroy(LR_SandboxManager *mgr);

/* Create a sandboxed runtime. */
LR_Sandbox *lr_sandbox_create(LR_SandboxManager *mgr,
                              const LR_SandboxConfig *config);

/* Destroy a sandbox. */
void lr_sandbox_destroy(LR_SandboxManager *mgr, LR_Sandbox *sb);

/* Execute JS in a sandbox. Returns 0 on success, -1 on error. */
int  lr_sandbox_eval(LR_Sandbox *sb, const char *source,
                     size_t source_len, const char *filename);

/* Check if sandbox exceeded its limits. */
int  lr_sandbox_check_limits(LR_Sandbox *sb);

/* Get sandbox by ID. */
LR_Sandbox *lr_sandbox_get(LR_SandboxManager *mgr, int id);

/* Get sandbox by name. */
LR_Sandbox *lr_sandbox_get_by_name(LR_SandboxManager *mgr, const char *name);

/* Get sandbox state as string. */
const char *lr_sandbox_state_str(LR_SandboxState state);

/* Default sandbox config. */
void lr_sandbox_config_default(LR_SandboxConfig *cfg);

/* List all sandboxes (calls callback for each). */
void lr_sandbox_foreach(LR_SandboxManager *mgr,
                        void (*cb)(LR_Sandbox *sb, void *arg),
                        void *arg);

#endif /* LR_SANDBOX_H */