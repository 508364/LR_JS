/*
 * L/R_JS - Generational & Incremental GC
 * Pure C, smooth pause-free garbage collection.
 *
 * Architecture:
 *   ┌─────────────────────────────────────────────────┐
 *   │  Nursery (Young Gen)                            │
 *   │  ┌───────┬───────┬───────┬───────┬──────────┐  │
 *   │  │  obj  │  obj  │  obj  │  obj  │  ...      │  │
 *   │  └───────┴───────┴───────┴───────┴──────────┘  │
 *   │  Bump-pointer allocator,  fast allocation.      │
 *   │  Minor GC: copy survivors → Old Gen.            │
 *   └─────────────────────────────────────────────────┘
 *                         ↓ promotion
 *   ┌─────────────────────────────────────────────────┐
 *   │  Old Generation                                 │
 *   │  ┌───────┬───────┬───────┬───────┬──────────┐  │
 *   │  │  obj  │  obj  │  obj  │  obj  │  ...      │  │
 *   │  └───────┴───────┴───────┴───────┴──────────┘  │
 *   │  Major GC: full mark-sweep, less frequent.     │
 *   └─────────────────────────────────────────────────┘
 *
 * Incremental GC:
 *   - Time-sliced marking: mark N objects per slice
 *   - Lazy sweeping: sweep a few pages during allocation
 *   - Idle-time GC: do GC work in event loop idle periods
 *   - Pause target: configurable max pause per slice
 */

#ifndef LR_GC_H
#define LR_GC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "lr_platform.h"

/* Forward declarations */
typedef struct LR_Runtime  LR_Runtime;
typedef struct LR_GCContext LR_GCContext;

/* ── GC Configuration ──────────────────────────────────────────────────── */

typedef struct LR_GCConfig {
    /* Generational */
    int     generational_enabled;   /* Enable generational GC */
    size_t  nursery_size;           /* Nursery buffer size (bytes), default 4MB */
    int     promotion_age;          /* Minor GCs before promotion, default 3 */
    int     minor_gc_interval_ms;   /* Min interval between minor GCs, default 10 */

    /* Incremental */
    int     incremental_enabled;    /* Enable incremental marking */
    int64_t pause_target_ns;        /* Target max pause per GC slice, default 5ms */
    int     mark_step_size;         /* Objects to mark per step, default 1024 */
    int     sweep_step_size;        /* Objects to sweep per step, default 2048 */

    /* Scheduling */
    double  gc_time_ratio;          /* Target GC time / mutator time ratio, default 0.05 */
    int     major_gc_interval_ms;   /* Minimum interval between major GCs, default 5000 */
    size_t  allocation_threshold;   /* Bytes allocated before triggering minor GC */

    /* Idle GC */
    int     idle_gc_enabled;        /* Run GC during event loop idle time */
    int     idle_gc_step_ms;        /* Max ms to spend on idle GC per tick */

    /* Stress */
    int     stress_mode;            /* GC after every operation (debug) */
} LR_GCConfig;

/* ── GC Statistics ──────────────────────────────────────────────────────── */

typedef struct LR_GCStats {
    /* Counters */
    int64_t   minor_gc_count;
    int64_t   major_gc_count;
    int64_t   full_gc_count;

    /* Timing */
    int64_t   total_gc_time_ns;       /* Total time spent in GC */
    int64_t   last_gc_time_ns;        /* Duration of last GC */
    int64_t   max_gc_time_ns;         /* Max GC pause observed */
    int64_t   total_mark_time_ns;
    int64_t   total_sweep_time_ns;

    /* Incremental */
    int64_t   incremental_steps;      /* Number of incremental steps */
    int64_t   idle_gc_steps;          /* Number of idle GC steps */

    /* Memory */
    size_t    nursery_bytes_allocated;
    size_t    nursery_bytes_freed;
    size_t    promoted_bytes;         /* Bytes promoted to old gen */
    size_t    swept_bytes;

    /* Nursery */
    size_t    nursery_current_size;
    size_t    nursery_peak_size;

    /* Pause distribution */
    int64_t   pause_histogram[10];    /* buckets: <1ms, 1-2, 2-5, 5-10, 10-20, 20-50, 50-100, 100-200, 200-500, >500ms */
} LR_GCStats;

/* ── GC Context ─────────────────────────────────────────────────────────── */

/* Nursery block: fast bump-pointer allocation area */
typedef struct LR_NurseryBlock {
    uint8_t             *base;        /* Start of memory */
    uint8_t             *top;         /* Current allocation pointer */
    uint8_t             *end;         /* End of memory */
    struct LR_NurseryBlock *next;     /* Linked list of blocks */
} LR_NurseryBlock;

/* Incremental marking state */
typedef enum {
    LR_GC_PHASE_IDLE      = 0,
    LR_GC_PHASE_MARK_ROOTS,
    LR_GC_PHASE_MARK,
    LR_GC_PHASE_REMARK,
    LR_GC_PHASE_SWEEP,
    LR_GC_PHASE_COMPLETE,
} LR_GCPhase;

/* Worklist entry for incremental marking */
typedef struct LR_GCMarkEntry {
    void   *ptr;
    struct LR_GCMarkEntry *next;
} LR_GCMarkEntry;

struct LR_GCContext {
    LR_GCConfig     config;
    LR_GCStats      stats;

    /* Nursery (young generation) */
    LR_NurseryBlock *nursery_head;
    LR_NurseryBlock *nursery_current;
    int              nursery_age;       /* Current age counter for nursery */

    /* Incremental GC state */
    LR_GCPhase       phase;
    int              phase_in_progress;

    /* Marking worklist */
    LR_GCMarkEntry  *mark_worklist;
    int              mark_entries;
    int              mark_entries_done;

    /* Sweep state */
    void            *sweep_cursor;      /* Current sweep position */
    int              sweep_generation;  /* 0=nursery, 1=old */

    /* Allocation tracking */
    size_t           bytes_since_last_gc;
    int64_t          last_gc_check_time;

    /* Lock for concurrent access */
    pthread_mutex_t  mutex;

    /* Back-reference to runtime */
    LR_Runtime      *runtime;
};

/* ── API ────────────────────────────────────────────────────────────────── */

/* Initialize GC context with default config. */
void lr_gc_init(LR_GCContext *gc, LR_Runtime *rt);

/* Configure GC parameters. */
void lr_gc_configure(LR_GCContext *gc, const LR_GCConfig *config);

/* Destroy GC context. */
void lr_gc_destroy(LR_GCContext *gc);

/* ── Allocation ─────────────────────────────────────────────────────────── */

/* Allocate from nursery. Returns NULL if nursery is full (trigger minor GC). */
void *lr_gc_nursery_alloc(LR_GCContext *gc, size_t size);

/* Track external allocation (for GC scheduling). */
void lr_gc_track_allocation(LR_GCContext *gc, size_t bytes);

/* ── GC Triggers ────────────────────────────────────────────────────────── */

/* Perform a minor GC (nursery only). Fast, < 1ms target. */
void lr_gc_minor(LR_GCContext *gc);

/* Perform a major GC (full heap). Slower, but less frequent. */
void lr_gc_major(LR_GCContext *gc);

/* Perform a full GC immediately. */
void lr_gc_full(LR_GCContext *gc);

/* ── Incremental GC ─────────────────────────────────────────────────────── */

/* Start an incremental GC cycle. */
void lr_gc_incremental_start(LR_GCContext *gc);

/* Do one incremental GC step. Returns 1 if more work remains, 0 if done.
   Each step targets pause_target_ns in duration. */
int  lr_gc_incremental_step(LR_GCContext *gc);

/* Get the current incremental GC phase name. */
const char *lr_gc_phase_name(LR_GCContext *gc);

/* ── Idle-time GC ───────────────────────────────────────────────────────── */

/* Perform GC work during idle time. Called from event loop.
   Spends at most idle_gc_step_ms doing GC work. */
void lr_gc_idle_work(LR_GCContext *gc);

/* ── GC Hooks ───────────────────────────────────────────────────────────── */

/* Called before every JS allocation. Checks if GC is needed. */
void lr_gc_before_alloc(LR_GCContext *gc);

/* Called after every JS allocation. Updates allocation tracking. */
void lr_gc_after_alloc(LR_GCContext *gc, size_t bytes);

/* ── Statistics ─────────────────────────────────────────────────────────── */

/* Get GC statistics. */
void lr_gc_get_stats(LR_GCContext *gc, LR_GCStats *stats);

/* Print GC statistics to file. */
void lr_gc_ctx_print_stats(LR_GCContext *gc, FILE *fp);

/* Reset GC statistics. */
void lr_gc_ctx_reset_stats(LR_GCContext *gc);

/* ── Utilities ──────────────────────────────────────────────────────────── */

/* Get current timestamp in nanoseconds. */
int64_t lr_gc_now_ns(void);

/* Default GC config. */
void lr_gc_config_default(LR_GCConfig *cfg);

#endif /* LR_GC_H */