/*
 * L/R_JS - SharedArrayBuffer shared control block
 *
 * A SharedArrayBuffer's bytes live in a heap block with an atomic
 * reference count, so multiple runtimes (main thread + workers) can
 * genuinely share the same memory. Each JS SharedArrayBuffer object
 * holds one reference; postMessage transfers additional references
 * instead of copying the bytes.
 */
#ifndef LR_SAB_H
#define LR_SAB_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lr_platform.h"

typedef struct LRSabBlock {
    volatile int32_t ref_count;
    size_t           size;
    uint8_t         *data;   /* points at bytes[] */
    /* bytes follow the struct */
} LRSabBlock;

LR_INLINE LRSabBlock *lr_sab_alloc(size_t size)
{
    /* Pad the byte area up to a 4-byte multiple so Atomics can always
     * operate on a full aligned 32-bit word via CAS, even for 1/2-byte
     * elements at the very end of the buffer. */
    size_t padded = (size + 3u) & ~(size_t)3u;
    LRSabBlock *blk = (LRSabBlock *)calloc(1, sizeof(LRSabBlock) + padded);
    if (!blk) return NULL;
    blk->ref_count = 1;
    blk->size = size;
    blk->data = (uint8_t *)(blk + 1);
    return blk;
}

LR_INLINE void lr_sab_ref(LRSabBlock *blk)
{
    if (blk) lr_atomic_fetch_add_32(&blk->ref_count, 1);
}

LR_INLINE void lr_sab_unref(LRSabBlock *blk)
{
    if (!blk) return;
    if (lr_atomic_fetch_add_32(&blk->ref_count, -1) == 1) {
        free(blk);
    }
}

/* Wrap a shared block into a JS SharedArrayBuffer object in the given
 * context. TAKES OVER one reference to the block (the object's free_func
 * unrefs it). Implemented in lr_builtins_extra.c.
 * Note: requires engine/lr_engine.h to be included before this header. */
struct LRContext;
LRValue lr_sab_wrap_block(struct LRContext *ctx, LRSabBlock *blk);

#endif /* LR_SAB_H */
