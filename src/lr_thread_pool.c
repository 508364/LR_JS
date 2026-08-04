/*
 * L/R_JS - Thread Pool Implementation
 * Pure C, fixed-size thread pool with per-thread JSRuntime isolation.
 * Uses CAS-based lock-free MPSC queue for contention-free task dispatch.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_platform.h"

#include "lr_thread_pool.h"
#include "lr_runtime.h"

#define LR_WORKER_STACK_SIZE (1024 * 1024)  /* 1MB per worker */

/* ── Time helpers ──────────────────────────────────────────────────────── */

static int64_t get_time_us(void)
{
    return lr_get_time_us();
}

/* ── Task ──────────────────────────────────────────────────────────────── */

LR_Task *lr_task_create(LR_TaskType type, LR_TaskPriority priority)
{
    LR_Task *task = calloc(1, sizeof(LR_Task));
    if (!task) return NULL;
    task->type = type;
    task->priority = priority;
    task->task_id = -1;
    return task;
}

void lr_task_free(LR_Task *task)
{
    if (!task) return;
    free(task->source);
    free(task->filename);
    free(task->result_error);
    free(task);
}

/* ── Shared results pool ──────────────────────────────────────────────── */

LR_SharedResults *lr_shared_results_create(int capacity) {
    if (capacity < 1) capacity = 64;
    LR_SharedResults *sr = (LR_SharedResults *)calloc(1, sizeof(LR_SharedResults));
    if (!sr) return NULL;
    sr->capacity = capacity;
    sr->entries = (char **)calloc(capacity, sizeof(char *));
    if (!sr->entries) { free(sr); return NULL; }
    sr->write_idx = 0;
    return sr;
}

int lr_shared_results_append(LR_SharedResults *sr, const char *str) {
    if (!sr || !str) return -1;
    int idx = LR_ATOMIC_INC((volatile int32_t *)&sr->write_idx) - 1;
    if (idx >= sr->capacity) return -1;
    sr->entries[idx] = str ? strdup(str) : NULL;
    return idx;
}

void lr_shared_results_free(LR_SharedResults *sr) {
    if (!sr) return;
    for (int i = 0; i < sr->write_idx && i < sr->capacity; i++)
        free(sr->entries[i]);
    free(sr->entries);
    free(sr);
}

/* ── Worker thread function ────────────────────────────────────────────── */

static void *lr_worker_thread(void *arg)
{
    LR_Worker *w = (LR_Worker *)arg;
    LR_Config cfg;

    /* Create per-worker JS runtime */
    lr_config_default(&cfg);
    cfg.log_level = LR_LOG_ERROR;  /* quiet workers */
    w->runtime = lr_runtime_new(&cfg);

    if (!w->runtime) {
        fprintf(stderr, "[Worker %d] Failed to create runtime\n", w->worker_id);
        w->running = 0;
        return NULL;
    }

    while (!w->should_stop) {
        /* Try to pop a task from the lock-free queue (non-blocking) */
        LR_LFQNode *node = lr_lfq_pop(&w->task_queue);

        if (node) {
            LR_Task *task = (LR_Task *)((char *)node -
                offsetof(LR_Task, lfq_node));

            if (task->type == LR_TASK_TERMINATE) {
                lr_task_free(task);
                break;
            }

            /* Execute task */
            int64_t start = get_time_us();
            int result = 0;
            const char *err = NULL;

            switch (task->type) {
            case LR_TASK_EVAL:
                if (task->is_module) {
                    result = lr_eval_module(w->runtime, task->source,
                                            task->source_len, task->filename);
                } else {
                    result = lr_eval(w->runtime, task->source,
                                     task->source_len, task->filename);
                }
                if (result < 0) err = lr_get_last_error(w->runtime);
                break;

            case LR_TASK_EVAL_FILE:
                result = lr_eval_file(w->runtime, task->filename);
                if (result < 0) err = lr_get_last_error(w->runtime);
                break;

            case LR_TASK_CALLBACK:
                if (task->func) {
                    task->func(task->func_arg);
                }
                break;

            case LR_TASK_RENDER:
                if (task->func) {
                    task->func(task->func_arg);
                }
                break;

            default:
                break;
            }

            int64_t elapsed = get_time_us() - start;
            w->tasks_completed++;
            w->total_exec_time_us += elapsed;

            /* Store result */
            task->result_code = result;
            if (err) {
                free(task->result_error);
                task->result_error = strdup(err);
            }

            /* Call completion callback */
            if (task->on_complete) {
                task->on_complete(task->userdata, result, err);
            }

            lr_task_free(task);
        } else {
            /* Queue is empty, sleep until signaled */
            pthread_mutex_lock(&w->signal_mutex);
            if (!w->should_stop && lr_lfq_is_empty(&w->task_queue)) {
                pthread_cond_wait(&w->signal_cond, &w->signal_mutex);
            }
            pthread_mutex_unlock(&w->signal_mutex);
        }
    }

    /* Cleanup worker runtime */
    if (w->runtime) {
        lr_runtime_free(w->runtime);
        w->runtime = NULL;
    }

    w->running = 0;
    return NULL;
}

/* ── Thread pool ───────────────────────────────────────────────────────── */

LR_ThreadPool *lr_thread_pool_create(int num_workers)
{
    if (num_workers <= 0) num_workers = 1;
    if (num_workers > 128) num_workers = 128;

    LR_ThreadPool *pool = calloc(1, sizeof(LR_ThreadPool));
    if (!pool) return NULL;

    pool->num_workers = num_workers;
    pool->running = 1;
    pool->workers = calloc((size_t)num_workers, sizeof(LR_Worker *));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->id_mutex, NULL);

    /* Create worker threads */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, LR_WORKER_STACK_SIZE);

    for (int i = 0; i < num_workers; i++) {
        pool->workers[i] = calloc(1, sizeof(LR_Worker));
        LR_Worker *w = pool->workers[i];
        w->worker_id = i;
        w->running = 1;
        w->should_stop = 0;

        /* Initialize lock-free queue */
        lr_lfq_init(&w->task_queue);

        /* Initialize signal mutex/cond */
        pthread_mutex_init(&w->signal_mutex, NULL);
        pthread_cond_init(&w->signal_cond, NULL);

        if (pthread_create(&w->thread, &attr, lr_worker_thread, w) != 0) {
            fprintf(stderr, "[Pool] Failed to create worker %d\n", i);
            w->running = 0;
        }
    }

    pthread_attr_destroy(&attr);
    return pool;
}

/* Enqueue a task to a worker's lock-free queue and signal the worker.
 * This is lock-free for the queue; only the signal requires a mutex. */
static void lr_thread_pool_enqueue(LR_Worker *w, LR_Task *task)
{
    /* Push to lock-free queue (no mutex needed) */
    lr_lfq_push(&w->task_queue, &task->lfq_node);

    /* Signal the worker to wake up (needs mutex for cond var) */
    pthread_mutex_lock(&w->signal_mutex);
    pthread_cond_signal(&w->signal_cond);
    pthread_mutex_unlock(&w->signal_mutex);
}

int lr_thread_pool_submit(LR_ThreadPool *pool, LR_Task *task)
{
    if (!pool || !task || !pool->running) return -1;

    /* Round-robin distribution */
    pthread_mutex_lock(&pool->id_mutex);
    task->task_id = pool->next_task_id++;
    task->submit_time_us = get_time_us();
    int idx = pool->round_robin_idx;
    pool->round_robin_idx = (pool->round_robin_idx + 1) % pool->num_workers;
    pool->tasks_submitted++;
    pthread_mutex_unlock(&pool->id_mutex);

    lr_thread_pool_enqueue(pool->workers[idx], task);
    return task->task_id;
}

int lr_thread_pool_submit_to(LR_ThreadPool *pool, int worker_id, LR_Task *task)
{
    if (!pool || !task || !pool->running) return -1;
    if (worker_id < 0 || worker_id >= pool->num_workers) return -1;

    pthread_mutex_lock(&pool->id_mutex);
    task->task_id = pool->next_task_id++;
    task->submit_time_us = get_time_us();
    pool->tasks_submitted++;
    pthread_mutex_unlock(&pool->id_mutex);

    lr_thread_pool_enqueue(pool->workers[worker_id], task);
    return task->task_id;
}

void lr_thread_pool_wait_all(LR_ThreadPool *pool)
{
    if (!pool) return;

    /* Wait until all queues are empty */
    for (int i = 0; i < pool->num_workers; i++) {
        LR_Worker *w = pool->workers[i];
        if (!w || !w->running) continue;

        while (!lr_lfq_is_empty(&w->task_queue)) {
            lr_sleep_ms(1);  /* 1ms sleep */
        }
    }
}

void lr_thread_pool_destroy(LR_ThreadPool *pool)
{
    if (!pool) return;

    pool->running = 0;

    /* Send terminate tasks to all workers */
    for (int i = 0; i < pool->num_workers; i++) {
        LR_Worker *w = pool->workers[i];
        if (!w || !w->running) continue;

        w->should_stop = 1;

        /* Push a terminate task to wake up the worker */
        LR_Task *term = lr_task_create(LR_TASK_TERMINATE, LR_TASK_PRIORITY_CRITICAL);
        if (term) {
            lr_lfq_push(&w->task_queue, &term->lfq_node);
        }

        /* Signal to wake up */
        pthread_mutex_lock(&w->signal_mutex);
        pthread_cond_signal(&w->signal_cond);
        pthread_mutex_unlock(&w->signal_mutex);
    }

    /* Join all workers */
    for (int i = 0; i < pool->num_workers; i++) {
        LR_Worker *w = pool->workers[i];
        if (!w) continue;

        if (w->running) {
            pthread_join(w->thread, NULL);
        }

        /* Cleanup remaining tasks */
        LR_LFQNode *node;
        while ((node = lr_lfq_pop(&w->task_queue)) != NULL) {
            LR_Task *t = (LR_Task *)((char *)node - offsetof(LR_Task, lfq_node));
            lr_task_free(t);
        }

        /* Destroy lock-free queue (free stub node) */
        lr_lfq_destroy(&w->task_queue, NULL);

        pthread_mutex_destroy(&w->signal_mutex);
        pthread_cond_destroy(&w->signal_cond);
        free(w);
    }

    free(pool->workers);
    pthread_mutex_destroy(&pool->id_mutex);
    free(pool);
}

void lr_thread_pool_stats(LR_ThreadPool *pool,
                          int *out_pending, int64_t *out_completed)
{
    if (!pool) {
        if (out_pending) *out_pending = 0;
        if (out_completed) *out_completed = 0;
        return;
    }

    int pending = 0;
    int64_t completed = 0;
    for (int i = 0; i < pool->num_workers; i++) {
        LR_Worker *w = pool->workers[i];
        if (!w) continue;
        pending += lr_lfq_count(&w->task_queue);
        completed += w->tasks_completed;
    }

    if (out_pending) *out_pending = pending;
    if (out_completed) *out_completed = completed;
}