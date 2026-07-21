/*
 * L/R_JS - Task Scheduler Module
 * Pure C, priority-based async task scheduler (cross-platform).
 * Manages the lifecycle of async tasks across the thread pool.
 */
#ifndef LR_SCHEDULER_H
#define LR_SCHEDULER_H

#include <stdint.h>
#include <stddef.h>
#include "lr_platform.h"
#include "lr_thread_pool.h"

/* Forward declarations */
typedef struct LR_Scheduler    LR_Scheduler;
typedef struct LR_SchedTask    LR_SchedTask;

/* ── Scheduler task ────────────────────────────────────────────────────── */

typedef enum {
    LR_SCHED_ONESHOT  = 0,   /* Run once */
    LR_SCHED_REPEAT   = 1,   /* Repeat at interval */
    LR_SCHED_CRON     = 2,   /* Cron-like schedule */
} LR_SchedType;

struct LR_SchedTask {
    int               id;
    LR_SchedType      type;
    LR_Task          *task;           /* Underlying thread pool task */
    LR_TaskPriority   priority;
    int64_t           interval_us;    /* Repeat interval (0 = oneshot) */
    int64_t           next_run_us;    /* Next scheduled time */
    int               repeat_count;   /* -1 = infinite */
    int               run_count;
    int               cancelled;
    LR_SchedTask     *next;
};

/* ── Scheduler ─────────────────────────────────────────────────────────── */

struct LR_Scheduler {
    LR_ThreadPool    *pool;            /* Thread pool for execution */
    LR_SchedTask     *task_head;       /* Priority queue of scheduled tasks */
    int               next_task_id;
    int               running;

    /* Stats */
    int64_t           tasks_scheduled;
    int64_t           tasks_executed;
    int64_t           tasks_dropped;

    pthread_mutex_t   mutex;
};

/* ── API ───────────────────────────────────────────────────────────────── */

/* Create scheduler backed by a thread pool. */
LR_Scheduler *lr_scheduler_create(LR_ThreadPool *pool);

/* Schedule a task to run at a specific interval. */
int lr_scheduler_schedule(LR_Scheduler *sched, LR_Task *task,
                          LR_TaskPriority priority,
                          int64_t interval_us, int repeat_count);

/* Schedule a task to run after a delay. */
int lr_scheduler_schedule_delayed(LR_Scheduler *sched, LR_Task *task,
                                  LR_TaskPriority priority,
                                  int64_t delay_us);

/* Schedule a task to run immediately. */
int lr_scheduler_schedule_now(LR_Scheduler *sched, LR_Task *task,
                              LR_TaskPriority priority);

/* Cancel a scheduled task. */
int lr_scheduler_cancel(LR_Scheduler *sched, int sched_task_id);

/* Run the scheduler event loop (blocking). */
int lr_scheduler_run(LR_Scheduler *sched);

/* Run the scheduler for a limited time. */
int lr_scheduler_run_for(LR_Scheduler *sched, int64_t timeout_us);

/* Stop the scheduler. */
void lr_scheduler_stop(LR_Scheduler *sched);

/* Get pending task count. */
int lr_scheduler_pending_count(LR_Scheduler *sched);

/* Process pending tasks (non-blocking, call from main loop). */
int lr_scheduler_process(LR_Scheduler *sched);

/* Destroy the scheduler. */
void lr_scheduler_destroy(LR_Scheduler *sched);

#endif /* LR_SCHEDULER_H */