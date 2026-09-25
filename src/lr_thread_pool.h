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
    /* ── Hot path: read/written on every task pop ──────────────────────── */
    LR_LFQueue        task_queue;         /* lock-free MPSC queue         */
    volatile int      running;            /* worker is alive              */
    volatile int      should_stop;        /* shutdown signal              */
    int               worker_id;          /* index in pool                */
    LR_CACHE_PAD;                         /* isolate hot from cold        */

    /* ── Warm path: thread handle, signal, runtime, pool ptr ──────────── */
    pthread_t         thread;
    pthread_mutex_t   signal_mutex;
    pthread_cond_t    signal_cond;
    LR_Runtime       *runtime;
    struct LR_ThreadPool *pool;           /* back-pointer for work-stealing */

    /* ── Cold path: stats, rarely read ─────────────────────────────────── */
    volatile int64_t  tasks_completed;
    volatile int64_t  total_exec_time_us;
} LR_CACHE_ALIGNED LR_Worker;

/* ── Thread pool ───────────────────────────────────────────────────────── */

struct LR_ThreadPool {
    /* ── Hot path: task distribution ───────────────────────────────────── */
    int               num_workers;
    volatile int      running;
    volatile int      round_robin_idx;   /* CAS-protected, no mutex */
    LR_CACHE_PAD;

    /* ── Worker array (separate cache line) ────────────────────────────── */
    LR_Worker       **workers;

    /* ── Work-stealing state ──────────────────────────────────────────────
     * When a worker runs out of tasks in its own queue, it tries to steal
     * from the busiest neighbour.  steal_attempts tracks load imbalance.    */
    volatile int64_t  steal_attempts;
    volatile int64_t  steal_successes;
    LR_CACHE_PAD2;

    /* Global task counter — atomic, no mutex needed */
    volatile int32_t  next_task_id;

    /* ── Completion notification ──────────────────────────────────────────
     * Replaces busy-wait polling in lr_thread_pool_wait_all.
     * tasks_pending is incremented on submit, decremented on completion.
     * wait_all uses the condition variable instead of 1ms polling.         */
    volatile int64_t  tasks_pending;     /* atomic counter */
    pthread_mutex_t   done_mutex;
    pthread_cond_t    done_cond;

    /* Stats */
    volatile int64_t  tasks_submitted;
    volatile int64_t  tasks_completed;
} LR_CACHE_ALIGNED;

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

/* ── Shared results pool for parallel execution ────────────────────────
 * Multiple sandbox threads write string results to a CAS-protected
 * shared buffer; the main thread reads them after all sandboxes finish. */

typedef struct {
    char  **entries;         /* result strings (NULL-terminated array) */
    int     count;
    int     capacity;
    volatile int write_idx;  /* CAS-protected write index */
} LR_SharedResults;

/* Create a shared results buffer with room for `capacity` entries. */
LR_SharedResults *lr_shared_results_create(int capacity);

/* CAS-append a result string. Caller retains ownership of `str`.
 * Returns slot index or -1 if full. */
int lr_shared_results_append(LR_SharedResults *sr, const char *str);

/* Free the shared results buffer and all entries. */
void lr_shared_results_free(LR_SharedResults *sr);

#endif /* LR_THREAD_POOL_H */