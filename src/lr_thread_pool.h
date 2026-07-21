/*
 * L/R_JS - Thread Pool Module
 * Pure C, fixed-size thread pool with per-thread JSRuntime isolation.
 * Uses CAS-based lock-free MPSC queue for task dispatch.
 * Each worker thread owns its own JS runtime + context.
 */
#ifndef LR_THREAD_POOL_H
#define LR_THREAD_POOL_H

#include "lr_platform.h"
#include <stdint.h>
#include <stddef.h>
#include "lr_lockfree_queue.h"

/* Forward declarations */
typedef struct LR_Runtime  LR_Runtime;
typedef struct LR_Sandbox  LR_Sandbox;
typedef struct LR_Task     LR_Task;
typedef struct LR_ThreadPool LR_ThreadPool;

/* ── Task ──────────────────────────────────────────────────────────────── */

typedef enum {
    LR_TASK_PRIORITY_LOW    = 0,
    LR_TASK_PRIORITY_NORMAL = 1,
    LR_TASK_PRIORITY_HIGH   = 2,
    LR_TASK_PRIORITY_CRITICAL = 3,
} LR_TaskPriority;

typedef enum {
    LR_TASK_EVAL,        /* Evaluate JS code */
    LR_TASK_EVAL_FILE,   /* Evaluate JS file */
    LR_TASK_CALLBACK,    /* Call a C callback */
    LR_TASK_RENDER,      /* Render command */
    LR_TASK_TERMINATE,   /* Sentinel to stop worker */
} LR_TaskType;

/* Task result callback */
typedef void (*LR_TaskCallback)(void *userdata, int result, const char *error);

struct LR_Task {
    LR_TaskType       type;
    LR_TaskPriority   priority;
    int               task_id;

    /* Lock-free queue node (embedded) */
    LR_LFQNode        lfq_node;

    /* JS eval params */
    char             *source;
    size_t            source_len;
    char             *filename;
    int               is_module;

    /* Generic callback */
    void            (*func)(void *arg);
    void             *func_arg;

    /* Result callback */
    LR_TaskCallback   on_complete;
    void             *userdata;

    /* Result */
    int               result_code;
    char             *result_error;

    /* Timing */
    int64_t           submit_time_us;
    int               timeout_ms;
};

/* ── Worker state ──────────────────────────────────────────────────────── */

typedef struct LR_Worker {
    pthread_t         thread;
    int               worker_id;
    volatile int      running;
    volatile int      should_stop;

    /* Per-worker JS isolation */
    LR_Runtime       *runtime;

    /* Worker-local lock-free task queue (MPSC) */
    LR_LFQueue        task_queue;

    /* Condition variable for waking up sleeping worker.
     * The queue itself is lock-free; this is only for signaling. */
    pthread_mutex_t   signal_mutex;
    pthread_cond_t    signal_cond;

    /* Stats */
    volatile int64_t  tasks_completed;
    volatile int64_t  total_exec_time_us;
} LR_Worker;

/* ── Thread pool ───────────────────────────────────────────────────────── */

struct LR_ThreadPool {
    int               num_workers;
    LR_Worker       **workers;
    volatile int      running;
    int               round_robin_idx;

    /* Global task counter */
    int               next_task_id;
    pthread_mutex_t   id_mutex;

    /* Stats */
    volatile int64_t  tasks_submitted;
    volatile int64_t  tasks_completed;
};

/* ── API ───────────────────────────────────────────────────────────────── */

/* Create thread pool with `num_workers` threads. */
LR_ThreadPool *lr_thread_pool_create(int num_workers);

/* Submit a task to the pool (round-robin distribution). Returns task_id. */
int lr_thread_pool_submit(LR_ThreadPool *pool, LR_Task *task);

/* Submit a task to a specific worker. */
int lr_thread_pool_submit_to(LR_ThreadPool *pool, int worker_id, LR_Task *task);

/* Wait for all tasks to complete. */
void lr_thread_pool_wait_all(LR_ThreadPool *pool);

/* Shutdown the pool, wait for workers to finish. */
void lr_thread_pool_destroy(LR_ThreadPool *pool);

/* Create a task (caller fills in details). */
LR_Task *lr_task_create(LR_TaskType type, LR_TaskPriority priority);

/* Free a task. */
void lr_task_free(LR_Task *task);

/* Get pool stats. */
void lr_thread_pool_stats(LR_ThreadPool *pool,
                          int *out_pending, int64_t *out_completed);

#endif /* LR_THREAD_POOL_H */