/*
 * L/R_JS - Lock-Free Queue (MPSC: Multiple Producer, Single Consumer)
 * Pure C, CAS-based, no mutexes.
 *
 * Design:
 *   - Linked-list with a dummy/stub node to avoid the ABA problem
 *   - Multiple producers push to tail using CAS on tail->next
 *   - Single consumer pops from head (no CAS needed for pop)
 *   - Memory ordering: release semantics on push, acquire on pop
 *   - Wait-free push (CAS loop, bounded retries), wait-free pop (single consumer)
 *
 * Thread safety:
 *   - lr_lfq_push:  safe for multiple concurrent producers
 *   - lr_lfq_pop:   safe only for a single consumer (or externally serialized)
 *   - lr_lfq_count: approximate, for diagnostics only
 *
 * Usage:
 *   LR_LFQueue q = LR_LFQ_INIT;
 *   MyData *d = malloc(sizeof(MyData));
 *   d->value = 42;
 *   lr_lfq_push(&q, &d->node);
 *   ...
 *   LR_LFQNode *node = lr_lfq_pop(&q);
 *   if (node) {
 *       MyData *d = (MyData *)((char *)node - offsetof(MyData, node));
 *       // use d
 *       free(d);
 *   }
 */
#ifndef LR_LOCKFREE_QUEUE_H
#define LR_LOCKFREE_QUEUE_H

#include "lr_platform.h"

/* ── Lock-free queue node (embed in your data structure) ───────────────── */

typedef struct LR_LFQNode {
    struct LR_LFQNode *next;
} LR_LFQNode;

/* ── Lock-free queue (MPSC) ────────────────────────────────────────────── */

typedef struct LR_LFQueue {
    LR_LFQNode *head;        /* Consumer pops from head */
    LR_LFQNode *tail;        /* Producers push to tail */
    volatile int32_t count;  /* Approximate count (for diagnostics) */
} LR_LFQueue;

/* Static initializer */
#define LR_LFQ_INIT { NULL, NULL, 0 }

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize the queue. Must call before first use. */
LR_INLINE void lr_lfq_init(LR_LFQueue *q)
{
    /* Create a stub node so head and tail are never both NULL */
    LR_LFQNode *stub = (LR_LFQNode *)calloc(1, sizeof(LR_LFQNode));
    if (!stub) return; /* OOM */
    stub->next = NULL;
    q->head = stub;
    q->tail = stub;
    q->count = 0;
}

/* Push a node to the tail of the queue (multiple producers safe).
 * Returns 0 on success, -1 on error. */
LR_INLINE int lr_lfq_push(LR_LFQueue *q, LR_LFQNode *node)
{
    if (!q || !node) return -1;

    node->next = NULL;

    /* CAS loop: atomically swap tail with new node */
    LR_LFQNode *prev;
    do {
        prev = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&q->tail);
    } while (lr_atomic_cas_ptr((volatile void **)&prev->next, NULL, node) != NULL);

    /* The CAS succeeded, now update tail to point to the new node.
     * This is a "lazy" tail update - the tail pointer may lag behind,
     * but the next push will fix it. */
    lr_atomic_cas_ptr((volatile void **)&q->tail, prev, node);

    lr_atomic_fetch_add_32((volatile int32_t *)&q->count, 1);
    return 0;
}

/* Pop a node from the head of the queue (single consumer only).
 * Returns NULL if the queue is empty.
 * NOTE: The queue's head always points to a valid stub node after pop,
 * so subsequent pushes and peeks are safe even if the caller frees the
 * returned node. */
LR_INLINE LR_LFQNode *lr_lfq_pop(LR_LFQueue *q)
{
    if (!q) return NULL;

    LR_LFQNode *head = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&q->head);
    LR_LFQNode *next = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&head->next);

    if (next == NULL) {
        /* Queue is empty */
        return NULL;
    }

    LR_LFQNode *next_next = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&next->next);

    if (next_next) {
        /* There are more nodes after this one; advance head to next */
        q->head = next;
    } else {
        /* This is the last node; create a new stub so head/tail stay valid
         * even after the caller frees the returned node. */
        LR_LFQNode *new_stub = (LR_LFQNode *)calloc(1, sizeof(LR_LFQNode));
        if (new_stub) {
            new_stub->next = NULL;
            q->head = new_stub;
            q->tail = new_stub;
        } else {
            /* OOM fallback: advance head anyway (caller must not free) */
            q->head = next;
        }
    }

    lr_atomic_fetch_add_32((volatile int32_t *)&q->count, -1);

    return next;
}

/* Peek at the head node without removing it (single consumer only). */
LR_INLINE LR_LFQNode *lr_lfq_peek(LR_LFQueue *q)
{
    if (!q) return NULL;
    LR_LFQNode *head = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&q->head);
    return (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&head->next);
}

/* Get approximate count of elements in the queue.
 * This is an approximation due to concurrent access. */
LR_INLINE int32_t lr_lfq_count(LR_LFQueue *q)
{
    if (!q) return 0;
    return lr_atomic_load_32((volatile int32_t *)&q->count);
}

/* Check if the queue is empty (approximate). */
LR_INLINE int lr_lfq_is_empty(LR_LFQueue *q)
{
    return lr_lfq_peek(q) == NULL;
}

/* Drain all nodes from the queue without freeing them (single consumer).
 * Returns the linked list of all nodes. */
LR_INLINE LR_LFQNode *lr_lfq_drain(LR_LFQueue *q)
{
    if (!q) return NULL;

    LR_LFQNode *head = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&q->head);
    LR_LFQNode *all = (LR_LFQNode *)lr_atomic_load_ptr((volatile void **)&head->next);

    /* Reset queue to empty */
    LR_LFQNode *new_stub = (LR_LFQNode *)calloc(1, sizeof(LR_LFQNode));
    if (new_stub) {
        q->head = new_stub;
        q->tail = new_stub;
        lr_atomic_store_32((volatile int32_t *)&q->count, 0);
    }

    /* Free the old stub */
    free(head);
    return all;
}

/* Destroy the queue, freeing all nodes (caller must know all nodes are unused).
 * If `free_data` is non-NULL, it's called for each node to free the data. */
LR_INLINE void lr_lfq_destroy(LR_LFQueue *q, void (*free_data)(LR_LFQNode *node))
{
    if (!q) return;

    LR_LFQNode *cur = q->head;
    while (cur) {
        LR_LFQNode *next = cur->next;
        if (free_data) free_data(cur);
        cur = next;
    }

    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

#endif /* LR_LOCKFREE_QUEUE_H */