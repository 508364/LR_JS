/*
 * LR_JS - Set Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the full Set specification including:
 *   Set constructor, .add(), .has(), .delete(), .clear()
 *   .size, .keys(), .values(), .entries(), .forEach()
 */
#ifndef LR_SET_H
#define LR_SET_H

#include "engine/lr_engine.h"

/* Forward declaration */
struct LR_Runtime;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Set Data Structures ───────────────────────────────────────────────── */

typedef struct LRSetEntry {
    LRValue value;
    int32_t hash;
    int32_t alive;  /* 1 = in use, 0 = deleted (tombstone) */
} LRSetEntry;

typedef struct LRSetData {
    LRSetEntry *entries;
    int32_t     count;
    int32_t     capacity;
    int32_t     iter_count;  /* incremented on modification for iterator invalidation */
} LRSetData;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Initialize the Set constructor on the global object. */
void lr_set_init(struct LR_Runtime *rt);

#ifdef __cplusplus
}
#endif

#endif /* LR_SET_H */