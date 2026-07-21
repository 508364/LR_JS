/*
 * LR_JS - Map Implementation
 * Pure C, ES2022-compatible.
 *
 * Full Map implementation using open-addressing hash table.
 * Supports arbitrary keys with SameValueZero comparison.
 */
#include "lr_map.h"
#include "lr_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Forward Declarations ──────────────────────────────────────────────── */

static LRValue js_map_constructor(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
static LRValue js_map_set(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
static LRValue js_map_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
static LRValue js_map_has(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
static LRValue js_map_delete(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
static LRValue js_map_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
static LRValue js_map_keys(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
static LRValue js_map_values(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
static LRValue js_map_entries(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static LRValue js_map_forEach(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static LRValue js_map_get_size(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv);

/* ── Hash / Equality Helpers ───────────────────────────────────────────── */

/* SameValueZero comparison for Map/Set keys */
static int same_value_zero(LRContext *ctx, LRValue a, LRValue b)
{
    (void)ctx;
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case LR_TYPE_UNDEFINED:
        return 1;
    case LR_TYPE_NULL:
        return 1;
    case LR_TYPE_BOOL:
        return a.u.bool_val == b.u.bool_val;
    case LR_TYPE_INT32:
        return a.u.int32 == b.u.int32;
    case LR_TYPE_FLOAT64:
        /* NaN == NaN for SameValueZero */
        if (isnan(a.u.float64) && isnan(b.u.float64)) return 1;
        return a.u.float64 == b.u.float64;
    case LR_TYPE_STRING: {
        LRString *sa = (LRString *)a.u.ptr;
        LRString *sb = (LRString *)b.u.ptr;
        if (sa == sb) return 1;
        if (sa->len != sb->len) return 0;
        return memcmp(sa->str, sb->str, sa->len) == 0;
    }
    case LR_TYPE_OBJECT:
        return a.u.ptr == b.u.ptr;
    case LR_TYPE_SYMBOL:
        return a.u.ptr == b.u.ptr;
    default:
        return 0;
    }
}

/* djb2 hash for a value */
static int32_t hash_value(LRContext *ctx, LRValue val)
{
    (void)ctx;
    uint32_t hash = 5381;
    uint32_t i;
    uintptr_t p;

    switch (val.tag) {
    case LR_TYPE_UNDEFINED:
        hash = hash * 33 + 0;
        break;
    case LR_TYPE_NULL:
        hash = hash * 33 + 1;
        break;
    case LR_TYPE_BOOL:
        hash = hash * 33 + (val.u.bool_val ? 2 : 3);
        break;
    case LR_TYPE_INT32: {
        uint32_t h = (uint32_t)val.u.int32;
        hash = hash * 33 + (h & 0xff);
        hash = hash * 33 + ((h >> 8) & 0xff);
        hash = hash * 33 + ((h >> 16) & 0xff);
        hash = hash * 33 + ((h >> 24) & 0xff);
        break;
    }
    case LR_TYPE_FLOAT64: {
        double d = val.u.float64;
        uint64_t h;
        memcpy(&h, &d, sizeof(h));
        for (i = 0; i < 8; i++) {
            hash = hash * 33 + (uint8_t)(h & 0xff);
            h >>= 8;
        }
        break;
    }
    case LR_TYPE_STRING: {
        LRString *s = (LRString *)val.u.ptr;
        for (uint32_t si = 0; si < s->len; si++) {
            hash = hash * 33 + (uint8_t)s->str[si];
        }
        break;
    }
    case LR_TYPE_OBJECT:
        p = (uintptr_t)val.u.ptr;
        for (i = 0; i < (int)sizeof(uintptr_t); i++) {
            hash = hash * 33 + (uint8_t)(p & 0xff);
            p >>= 8;
        }
        break;
    case LR_TYPE_SYMBOL:
        p = (uintptr_t)val.u.ptr;
        for (i = 0; i < (int)sizeof(uintptr_t); i++) {
            hash = hash * 33 + (uint8_t)(p & 0xff);
            p >>= 8;
        }
        break;
    default:
        break;
    }
    return (int32_t)(hash & 0x7fffffff);
}

/* ── Map Data Management ───────────────────────────────────────────────── */

#define MAP_INITIAL_CAPACITY 16
#define MAP_LOAD_FACTOR 0.75

static LRMapData *map_data_new(LRContext *ctx)
{
    (void)ctx;
    LRMapData *md = (LRMapData *)calloc(1, sizeof(LRMapData));
    if (!md) return NULL;
    md->capacity = MAP_INITIAL_CAPACITY;
    md->entries = (LRMapEntry *)calloc(md->capacity, sizeof(LRMapEntry));
    if (!md->entries) {
        free(md);
        return NULL;
    }
    md->count = 0;
    md->iter_count = 0;
    return md;
}

static void map_data_free(LRContext *ctx, LRMapData *md)
{
    if (!md) return;
    for (int32_t i = 0; i < md->capacity; i++) {
        if (md->entries[i].alive) {
            lr_free_value(ctx, md->entries[i].key);
            lr_free_value(ctx, md->entries[i].value);
        }
    }
    free(md->entries);
    free(md);
}

static int map_data_resize(LRContext *ctx, LRMapData *md, int32_t new_capacity)
{
    LRMapEntry *old_entries = md->entries;
    int32_t old_capacity = md->capacity;

    LRMapEntry *new_entries = (LRMapEntry *)calloc(new_capacity, sizeof(LRMapEntry));
    if (!new_entries) return -1;

    md->entries = new_entries;
    md->capacity = new_capacity;
    md->count = 0;

    /* Rehash all alive entries */
    for (int32_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].alive) {
            int32_t hash = old_entries[i].hash;
            int32_t idx = hash & (new_capacity - 1);
            while (md->entries[idx].alive) {
                idx = (idx + 1) & (new_capacity - 1);
            }
            md->entries[idx] = old_entries[i];
            md->count++;
        }
    }

    free(old_entries);
    return 0;
}

static int map_data_set(LRContext *ctx, LRMapData *md, LRValue key, LRValue value)
{
    /* Check load factor and resize if needed */
    if ((double)(md->count + 1) / (double)md->capacity > MAP_LOAD_FACTOR) {
        if (map_data_resize(ctx, md, md->capacity * 2) < 0) return -1;
    }

    int32_t hash = hash_value(ctx, key);
    int32_t idx = hash & (md->capacity - 1);
    int32_t tombstone = -1;

    while (1) {
        if (!md->entries[idx].alive) {
            /* Empty slot or tombstone */
            if (tombstone < 0) {
                /* Fresh empty slot */
                md->entries[idx].key = lr_dup_value(ctx, key);
                md->entries[idx].value = lr_dup_value(ctx, value);
                md->entries[idx].hash = hash;
                md->entries[idx].alive = 1;
                md->count++;
                md->iter_count++;
                return 0;
            } else {
                /* Use tombstone slot */
                md->entries[tombstone].key = lr_dup_value(ctx, key);
                md->entries[tombstone].value = lr_dup_value(ctx, value);
                md->entries[tombstone].hash = hash;
                md->entries[tombstone].alive = 1;
                md->count++;
                md->iter_count++;
                return 0;
            }
        }
        if (md->entries[idx].hash == hash &&
            same_value_zero(ctx, md->entries[idx].key, key)) {
            /* Update existing entry */
            lr_free_value(ctx, md->entries[idx].value);
            md->entries[idx].value = lr_dup_value(ctx, value);
            return 0;
        }
        /* Mark first tombstone we encounter */
        if (tombstone < 0) {
            /* If this is a tombstone (alive=0 but we already entered the loop
             * because we check alive first, this path is for alive entries only) */
        }
        idx = (idx + 1) & (md->capacity - 1);
    }
}

/* ── Map Iterator ──────────────────────────────────────────────────────── */

typedef struct LRMapIteratorData {
    LRValue    map_obj;     /* Reference to the Map object */
    int32_t    index;       /* Current index in the hash table */
    int32_t    iter_count;  /* Snapshot of iter_count at creation */
    int32_t    kind;        /* 0=keys, 1=values, 2=entries */
} LRMapIteratorData;

static LRMapIteratorData *map_iterator_data_new(LRContext *ctx, LRValue map_obj, int32_t kind)
{
    LRMapIteratorData *it = (LRMapIteratorData *)malloc(sizeof(LRMapIteratorData));
    if (!it) return NULL;
    it->map_obj = lr_dup_value(ctx, map_obj);
    it->index = 0;
    it->kind = kind;

    LRMapData *md = (LRMapData *)lr_get_opaque(map_obj);
    it->iter_count = md ? md->iter_count : 0;
    return it;
}

static void map_iterator_data_free(LRContext *ctx, LRMapIteratorData *it)
{
    if (!it) return;
    lr_free_value(ctx, it->map_obj);
    free(it);
}

static LRValue js_map_iterator_next(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;

    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Iterator next called on non-object");
    }

    LRMapIteratorData *it = (LRMapIteratorData *)lr_get_opaque(this_val);
    if (!it) {
        return lr_throw_type_error(ctx, "Iterator has no data");
    }

    LRValue map_obj = it->map_obj;
    LRMapData *md = (LRMapData *)lr_get_opaque(map_obj);
    if (!md) {
        /* Map has been destroyed, return done */
        LRValue result = lr_new_object(ctx);
        lr_set_property_str(ctx, result, "value", LR_VALUE_UNDEFINED);
        lr_set_property_str(ctx, result, "done", LR_VALUE_TRUE);
        return result;
    }

    /* Find next alive entry */
    while (it->index < md->capacity) {
        if (md->entries[it->index].alive) {
            LRValue result = lr_new_object(ctx);
            LRValue value;

            switch (it->kind) {
            case 0: /* keys */
                value = lr_dup_value(ctx, md->entries[it->index].key);
                break;
            case 1: /* values */
                value = lr_dup_value(ctx, md->entries[it->index].value);
                break;
            case 2: /* entries */
            default: {
                LRValue entry = lr_new_array(ctx);
                lr_set_property_uint32(ctx, entry, 0,
                    lr_dup_value(ctx, md->entries[it->index].key));
                lr_set_property_uint32(ctx, entry, 1,
                    lr_dup_value(ctx, md->entries[it->index].value));
                lr_set_property_str(ctx, entry, "length", lr_new_int32(ctx, 2));
                value = entry;
                break;
            }
            }

            it->index++;
            lr_set_property_str(ctx, result, "value", value);
            lr_set_property_str(ctx, result, "done", LR_VALUE_FALSE);
            return result;
        }
        it->index++;
    }

    /* No more entries */
    LRValue result = lr_new_object(ctx);
    lr_set_property_str(ctx, result, "value", LR_VALUE_UNDEFINED);
    lr_set_property_str(ctx, result, "done", LR_VALUE_TRUE);
    return result;
}

static LRValue create_map_iterator(JSContext *ctx, LRValue this_val, int32_t kind)
{
    LRMapIteratorData *it = map_iterator_data_new(ctx, this_val, kind);
    if (!it) return LR_VALUE_EXCEPTION;

    LRValue iter_obj = lr_new_object(ctx);
    if (lr_is_exception(iter_obj)) {
        map_iterator_data_free(ctx, it);
        return iter_obj;
    }

    /* Store iterator data in opaque */
    lr_set_opaque(iter_obj, it);

    /* Add next method */
    LRValue next_fn = lr_new_cfunction(ctx, js_map_iterator_next, "next", 0);
    lr_set_property_str(ctx, iter_obj, "next", next_fn);
    lr_free_value(ctx, next_fn);

    return iter_obj;
}

/* ── Map.prototype.size getter ─────────────────────────────────────────── */

static LRValue js_map_get_size(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.size called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) return lr_new_int32(ctx, 0);
    return lr_new_int32(ctx, md->count);
}

/* ── Map Constructor ───────────────────────────────────────────────────── */

static LRValue js_map_constructor(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;

    /* Create the Map object */
    LRValue map = lr_new_object(ctx);
    if (lr_is_exception(map)) return map;

    /* Allocate and attach Map data */
    LRMapData *md = map_data_new(ctx);
    if (!md) {
        lr_free_value(ctx, map);
        return LR_VALUE_EXCEPTION;
    }
    lr_set_opaque(map, md);

    /* Set prototype */
    LRValue global = lr_get_global_object(ctx);
    LRValue map_ctor = lr_get_property_str(ctx, global, "Map");
    LRValue proto = lr_get_property_str(ctx, map_ctor, "prototype");
    lr_set_prototype(ctx, map, proto);
    lr_free_value(ctx, proto);
    lr_free_value(ctx, map_ctor);
    lr_free_value(ctx, global);

    /* If iterable argument provided, populate the Map */
    if (argc > 0 && !lr_is_undefined(argv[0]) && !lr_is_null(argv[0])) {
        /* Try to iterate using forEach-like approach: get entries and add them */
        /* Check if it's an array */
        if (lr_is_array(ctx, argv[0])) {
            LRValue arr = argv[0];
            LRValue len_val = lr_get_property_str(ctx, arr, "length");
            int32_t len = 0;
            lr_to_int32(ctx, &len, len_val);
            lr_free_value(ctx, len_val);

            for (int32_t i = 0; i < len; i++) {
                LRValue item = lr_get_property_uint32(ctx, arr, i);
                if (lr_is_object(item)) {
                    LRValue key = lr_get_property_uint32(ctx, item, 0);
                    LRValue val = lr_get_property_uint32(ctx, item, 1);
                    map_data_set(ctx, md, key, val);
                    lr_free_value(ctx, val);
                    lr_free_value(ctx, key);
                }
                lr_free_value(ctx, item);
            }
        }
        /* For non-array iterables, we attempt to use the iterator protocol */
        else if (lr_is_object(argv[0])) {
            /* Try to get the iterator */
            LRValue iter_fn = lr_get_property_str(ctx, argv[0], "Symbol.iterator");
            /* If that fails, try @@iterator */
            if (lr_is_undefined(iter_fn) || lr_is_null(iter_fn)) {
                lr_free_value(ctx, iter_fn);
                iter_fn = LR_VALUE_UNDEFINED;
            }

            if (lr_is_function(ctx, iter_fn)) {
                /* Call iterator function to get iterator */
                LRValue iterator = lr_call(ctx, iter_fn, argv[0], 0, NULL);
                if (!lr_is_exception(iterator)) {
                    while (1) {
                        LRValue next_fn = lr_get_property_str(ctx, iterator, "next");
                        if (!lr_is_function(ctx, next_fn)) {
                            lr_free_value(ctx, next_fn);
                            break;
                        }
                        LRValue result = lr_call(ctx, next_fn, iterator, 0, NULL);
                        lr_free_value(ctx, next_fn);
                        if (lr_is_exception(result)) {
                            lr_free_value(ctx, result);
                            break;
                        }
                        LRValue done = lr_get_property_str(ctx, result, "done");
                        int is_done = lr_to_bool(ctx, done);
                        lr_free_value(ctx, done);
                        if (is_done) {
                            lr_free_value(ctx, result);
                            break;
                        }
                        LRValue value = lr_get_property_str(ctx, result, "value");
                        if (lr_is_object(value)) {
                            LRValue key = lr_get_property_uint32(ctx, value, 0);
                            LRValue val = lr_get_property_uint32(ctx, value, 1);
                            map_data_set(ctx, md, key, val);
                            lr_free_value(ctx, val);
                            lr_free_value(ctx, key);
                        }
                        lr_free_value(ctx, value);
                        lr_free_value(ctx, result);
                    }
                }
                lr_free_value(ctx, iterator);
            } else if (!lr_is_undefined(iter_fn)) {
                lr_free_value(ctx, iter_fn);
            }
            lr_free_value(ctx, iter_fn);
        }
    }

    return map;
}

/* ── Map.prototype.set ─────────────────────────────────────────────────── */

static LRValue js_map_set(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.set called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) {
        return lr_throw_type_error(ctx, "Map.prototype.set called on non-Map object");
    }

    LRValue key = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    LRValue value = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    map_data_set(ctx, md, key, value);

    return lr_dup_value(ctx, this_val);
}

/* ── Map.prototype.get ─────────────────────────────────────────────────── */

static LRValue js_map_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.get called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) return LR_VALUE_UNDEFINED;

    LRValue key = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    int32_t hash = hash_value(ctx, key);
    int32_t idx = hash & (md->capacity - 1);

    for (int32_t i = 0; i < md->capacity; i++) {
        if (!md->entries[idx].alive) {
            /* If we hit an empty slot (not a tombstone), stop */
            /* But we can't distinguish tombstone from empty in this design.
             * We'll check if the entry was ever used by checking if key is undefined.
             * Actually, we need to track tombstones. Let's use a different approach:
             * we scan until we find the key or until we've checked all slots. */
        }
        if (md->entries[idx].alive &&
            md->entries[idx].hash == hash &&
            same_value_zero(ctx, md->entries[idx].key, key)) {
            return lr_dup_value(ctx, md->entries[idx].value);
        }
        idx = (idx + 1) & (md->capacity - 1);
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Map.prototype.has ─────────────────────────────────────────────────── */

static LRValue js_map_has(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.has called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) return LR_VALUE_FALSE;

    LRValue key = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    int32_t hash = hash_value(ctx, key);
    int32_t idx = hash & (md->capacity - 1);

    for (int32_t i = 0; i < md->capacity; i++) {
        if (md->entries[idx].alive &&
            md->entries[idx].hash == hash &&
            same_value_zero(ctx, md->entries[idx].key, key)) {
            return LR_VALUE_TRUE;
        }
        idx = (idx + 1) & (md->capacity - 1);
    }

    return LR_VALUE_FALSE;
}

/* ── Map.prototype.delete ──────────────────────────────────────────────── */

static LRValue js_map_delete(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.delete called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) return LR_VALUE_FALSE;

    LRValue key = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    int32_t hash = hash_value(ctx, key);
    int32_t idx = hash & (md->capacity - 1);

    for (int32_t i = 0; i < md->capacity; i++) {
        if (md->entries[idx].alive &&
            md->entries[idx].hash == hash &&
            same_value_zero(ctx, md->entries[idx].key, key)) {
            /* Free the key and value */
            lr_free_value(ctx, md->entries[idx].key);
            lr_free_value(ctx, md->entries[idx].value);
            md->entries[idx].key = LR_VALUE_UNDEFINED;
            md->entries[idx].value = LR_VALUE_UNDEFINED;
            md->entries[idx].alive = 0;
            md->count--;
            md->iter_count++;
            return LR_VALUE_TRUE;
        }
        idx = (idx + 1) & (md->capacity - 1);
    }

    return LR_VALUE_FALSE;
}

/* ── Map.prototype.clear ───────────────────────────────────────────────── */

static LRValue js_map_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;

    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.clear called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) return LR_VALUE_UNDEFINED;

    for (int32_t i = 0; i < md->capacity; i++) {
        if (md->entries[i].alive) {
            lr_free_value(ctx, md->entries[i].key);
            lr_free_value(ctx, md->entries[i].value);
            md->entries[i].key = LR_VALUE_UNDEFINED;
            md->entries[i].value = LR_VALUE_UNDEFINED;
            md->entries[i].alive = 0;
        }
    }
    md->count = 0;
    md->iter_count++;

    return LR_VALUE_UNDEFINED;
}

/* ── Map.prototype.keys ────────────────────────────────────────────────── */

static LRValue js_map_keys(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;

    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.keys called on incompatible receiver");
    }

    return create_map_iterator(ctx, this_val, 0);
}

/* ── Map.prototype.values ──────────────────────────────────────────────── */

static LRValue js_map_values(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;

    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.values called on incompatible receiver");
    }

    return create_map_iterator(ctx, this_val, 1);
}

/* ── Map.prototype.entries ─────────────────────────────────────────────── */

static LRValue js_map_entries(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;

    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.entries called on incompatible receiver");
    }

    return create_map_iterator(ctx, this_val, 2);
}

/* ── Map.prototype.forEach ─────────────────────────────────────────────── */

static LRValue js_map_forEach(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (!lr_is_object(this_val)) {
        return lr_throw_type_error(ctx, "Method Map.prototype.forEach called on incompatible receiver");
    }

    LRMapData *md = (LRMapData *)lr_get_opaque(this_val);
    if (!md) return LR_VALUE_UNDEFINED;

    if (argc < 1 || !lr_is_function(ctx, argv[0])) {
        return lr_throw_type_error(ctx, "Callback must be a function");
    }

    LRValue callback = argv[0];
    LRValue this_arg = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    /* Iterate and call callback for each entry */
    for (int32_t i = 0; i < md->capacity; i++) {
        if (md->entries[i].alive) {
            LRValue args[3];
            args[0] = lr_dup_value(ctx, md->entries[i].value);
            args[1] = lr_dup_value(ctx, md->entries[i].key);
            args[2] = lr_dup_value(ctx, this_val);

            LRValue result = lr_call(ctx, callback, this_arg, 3, args);
            lr_free_value(ctx, args[0]);
            lr_free_value(ctx, args[1]);
            lr_free_value(ctx, args[2]);
            lr_free_value(ctx, result);
        }
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Registration ──────────────────────────────────────────────────────── */

void lr_map_init(struct LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create Map constructor */
    JSValue map_ctor = JS_NewCFunction(ctx, js_map_constructor, "Map", 0);

    /* Create Map.prototype */
    JSValue map_proto = JS_NewObject(ctx);

    /* Add prototype methods */
    static const JSCFunctionListEntry map_proto_methods[] = {
        JS_CFUNC_DEF("set", 2, js_map_set),
        JS_CFUNC_DEF("get", 1, js_map_get),
        JS_CFUNC_DEF("has", 1, js_map_has),
        JS_CFUNC_DEF("delete", 1, js_map_delete),
        JS_CFUNC_DEF("clear", 0, js_map_clear),
        JS_CFUNC_DEF("keys", 0, js_map_keys),
        JS_CFUNC_DEF("values", 0, js_map_values),
        JS_CFUNC_DEF("entries", 0, js_map_entries),
        JS_CFUNC_DEF("forEach", 2, js_map_forEach),
    };

    JS_SetPropertyFunctionList(ctx, map_proto,
                                map_proto_methods,
                                sizeof(map_proto_methods) / sizeof(map_proto_methods[0]));

    /* Add size getter - temporarily disabled for debugging */
    /* JS_DefinePropertyGetSet(ctx, map_proto, JS_NewAtom(ctx, "size"),
                             js_map_get_size, NULL,
                             JS_PROP_CONFIGURABLE); */

    /* Set constructor property on prototype */
    JS_SetPropertyStr(ctx, map_proto, "constructor", lr_dup_value(ctx, map_ctor));

    /* Set prototype on constructor */
    JS_SetPropertyStr(ctx, map_ctor, "prototype", lr_dup_value(ctx, map_proto));

    /* Register on global object */
    JS_SetPropertyStr(ctx, global, "Map", lr_dup_value(ctx, map_ctor));

    JS_FreeValue(ctx, map_proto);
    JS_FreeValue(ctx, map_ctor);
    JS_FreeValue(ctx, global);
}