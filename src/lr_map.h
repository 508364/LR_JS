/*
 * LR_JS - Map Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the full Map specification including:
 *   Map constructor, .set(), .get(), .has(), .delete(), .clear()
 *   .size, .keys(), .values(), .entries(), .forEach()
 */
#ifndef LR_MAP_H
#define LR_MAP_H

#include "engine/lr_engine.h"

/* Forward declaration */
struct LR_Runtime;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Map Data Structures ───────────────────────────────────────────────── */

typedef struct LRMapEntry {
    LRValue key;
    LRValue value;
    int32_t hash;
    int32_t alive;  /* 1 = in use, 0 = deleted (tombstone) */
} LRMapEntry;

typedef struct LRMapData {
    LRMapEntry *entries;
    int32_t     count;
    int32_t     capacity;
    int32_t     iter_count;  /* incremented on modification for iterator invalidation */
} LRMapData;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Initialize the Map constructor on the global object. */
void lr_map_init(struct LR_Runtime *rt);

#ifdef __cplusplus
}
#endif

void lr_map_free_opaque(void *opaque);

#endif /* LR_MAP_H */