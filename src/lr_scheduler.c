/*
 * L/R_JS - Task Scheduler Implementation
 * Pure C, priority-based async task scheduler.
 * Cross-platform: no epoll dependency (uses timer-based dispatch).
 * All tasks are dispatched to the thread pool for execution.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_platform.h"

#include "lr_scheduler.h"

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int64_t get_time_us(void)
{
    return lr_get_time_us();
}

/* ── Scheduler ─────────────────────────────────────────────────────────── */

LR_Scheduler *lr_scheduler_create(LR_ThreadPool *pool)
{
    LR_Scheduler *sched = calloc(1, sizeof(LR_Scheduler));
    if (!sched) return NULL;

    sched->pool = pool;
    sched->next_task_id = 1;
    sched->running = 0;
    pthread_mutex_init(&sched->mutex, NULL);

    return sched;
}

/* ── Task management ───────────────────────────────────────────────────── */

static void lr_scheduler_insert_task(LR_Scheduler *sched, LR_SchedTask *st)
{
    /* Insert sorted by next_run_us (earliest first) */
    if (!sched->task_head || st->next_run_us < sched->task_head->next_run_us) {
        st->next = sched->task_head;
        sched->task_head = st;
    } else {
        LR_SchedTask *prev = sched->task_head;
        while (prev->next && prev->next->next_run_us <= st->next_run_us) {
            prev = prev->next;
        }
        st->next = prev->next;
        prev->next = st;
    }
}

int lr_scheduler_schedule(LR_Scheduler *sched, LR_Task *task,
                          LR_TaskPriority priority,
                          int64_t interval_us, int repeat_count)
{
    if (!sched || !task) return -1;

    LR_SchedTask *st = calloc(1, sizeof(LR_SchedTask));
    if (!st) return -1;

    pthread_mutex_lock(&sched->mutex);

    st->id = sched->next_task_id++;
    st->type = (repeat_count != 0) ? LR_SCHED_REPEAT : LR_SCHED_ONESHOT;
    st->task = task;
    st->priority = priority;
    st->interval_us = interval_us;
    st->next_run_us = get_time_us();
    st->repeat_count = repeat_count;
    st->run_count = 0;
    st->cancelled = 0;

    lr_scheduler_insert_task(sched, st);
    sched->tasks_scheduled++;

    pthread_mutex_unlock(&sched->mutex);
    return st->id;
}

int lr_scheduler_schedule_delayed(LR_Scheduler *sched, LR_Task *task,
                                  LR_TaskPriority priority,
                                  int64_t delay_us)
{
    if (!sched || !task) return -1;

    LR_SchedTask *st = calloc(1, sizeof(LR_SchedTask));
    if (!st) return -1;

    pthread_mutex_lock(&sched->mutex);

    st->id = sched->next_task_id++;
    st->type = LR_SCHED_ONESHOT;
    st->task = task;
    st->priority = priority;
    st->interval_us = 0;
    st->next_run_us = get_time_us() + delay_us;
    st->repeat_count = 0;
    st->run_count = 0;
    st->cancelled = 0;

    lr_scheduler_insert_task(sched, st);
    sched->tasks_scheduled++;

    pthread_mutex_unlock(&sched->mutex);
    return st->id;
}

int lr_scheduler_schedule_now(LR_Scheduler *sched, LR_Task *task,
                              LR_TaskPriority priority)
{
    return lr_scheduler_schedule_delayed(sched, task, priority, 0);
}

int lr_scheduler_cancel(LR_Scheduler *sched, int sched_task_id)
{
    if (!sched) return -1;

    pthread_mutex_lock(&sched->mutex);
    for (LR_SchedTask *st = sched->task_head; st; st = st->next) {
        if (st->id == sched_task_id) {
            st->cancelled = 1;
            pthread_mutex_unlock(&sched->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&sched->mutex);
    return -1;
}

/* ── Processing ────────────────────────────────────────────────────────── */

int lr_scheduler_process(LR_Scheduler *sched)
{
    if (!sched) return 0;

    int64_t now = get_time_us();
    int executed = 0;

    pthread_mutex_lock(&sched->mutex);

    LR_SchedTask *st = sched->task_head;
    LR_SchedTask *prev = NULL;

    while (st) {
        if (st->cancelled) {
            /* Remove cancelled task */
            LR_SchedTask *to_free = st;
            if (prev) {
                prev->next = st->next;
            } else {
                sched->task_head = st->next;
            }
            st = st->next;
            lr_task_free(to_free->task);
            free(to_free);
            sched->tasks_dropped++;
            continue;
        }

        if (st->next_run_us <= now) {
            /* Execute task via thread pool */
            lr_thread_pool_submit(sched->pool, st->task);
            st->run_count++;
            sched->tasks_executed++;
            executed++;

            /* Handle repeat */
            if (st->type == LR_SCHED_REPEAT) {
                if (st->repeat_count < 0 || st->run_count < st->repeat_count) {
                    st->next_run_us = now + st->interval_us;
                    /* Re-sort: remove and re-insert */
                    LR_SchedTask *to_resched = st;
                    if (prev) {
                        prev->next = st->next;
                    } else {
                        sched->task_head = st->next;
                    }
                    st = st->next;
                    to_resched->next = NULL;
                    lr_scheduler_insert_task(sched, to_resched);
                    continue;
                } else {
                    /* Done repeating */
                    LR_SchedTask *to_free = st;
                    if (prev) {
                        prev->next = st->next;
                    } else {
                        sched->task_head = st->next;
                    }
                    st = st->next;
                    free(to_free);
                    continue;
                }
            } else {
                /* Oneshot - remove from queue */
                LR_SchedTask *to_free = st;
                if (prev) {
                    prev->next = st->next;
                } else {
                    sched->task_head = st->next;
                }
                st = st->next;
                free(to_free);
                continue;
            }
        }

        prev = st;
        st = st->next;
    }

    pthread_mutex_unlock(&sched->mutex);
    return executed;
}

/* ── Event loop ────────────────────────────────────────────────────────── */

int lr_scheduler_run(LR_Scheduler *sched)
{
    if (!sched) return -1;

    sched->running = 1;

    while (sched->running) {
        int executed = lr_scheduler_process(sched);

        if (executed == 0) {
            /* No tasks ready, sleep briefly */
            lr_sleep_ms(1);
        }

        /* Check if there are pending tasks */
        pthread_mutex_lock(&sched->mutex);
        int has_tasks = (sched->task_head != NULL);
        pthread_mutex_unlock(&sched->mutex);

        if (!has_tasks) {
            lr_sleep_ms(10);  /* 10ms sleep when idle */
        }
    }

    return 0;
}

int lr_scheduler_run_for(LR_Scheduler *sched, int64_t timeout_us)
{
    if (!sched) return -1;

    int64_t start = get_time_us();
    sched->running = 1;

    while (sched->running) {
        lr_scheduler_process(sched);

        if (get_time_us() - start >= timeout_us) break;

        lr_sleep_ms(1);
    }

    return 0;
}

void lr_scheduler_stop(LR_Scheduler *sched)
{
    if (!sched) return;
    sched->running = 0;
}

int lr_scheduler_pending_count(LR_Scheduler *sched)
{
    if (!sched) return 0;

    pthread_mutex_lock(&sched->mutex);
    int count = 0;
    for (LR_SchedTask *st = sched->task_head; st; st = st->next) {
        if (!st->cancelled) count++;
    }
    pthread_mutex_unlock(&sched->mutex);
    return count;
}

void lr_scheduler_destroy(LR_Scheduler *sched)
{
    if (!sched) return;

    sched->running = 0;

    /* Cleanup remaining tasks */
    LR_SchedTask *st = sched->task_head;
    while (st) {
        LR_SchedTask *next = st->next;
        if (!st->cancelled) {
            lr_task_free(st->task);
        }
        free(st);
        st = next;
    }

    pthread_mutex_destroy(&sched->mutex);
    free(sched);
}