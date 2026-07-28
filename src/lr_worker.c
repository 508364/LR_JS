/*
 * L/R_JS - Worker API Module
 * Pure C, Web Worker-like multi-threaded message passing.
 * Each Worker runs in its own thread with an isolated JSRuntime.
 *
 * JS API:
 *   const w = new Worker('script.js');
 *   w.postMessage({data: 'hello'});
 *   w.onmessage = (e) => console.log(e.data);
 *   w.onerror   = (e) => console.log(e.data.message);
 *   w.terminate();
 *   // worker side: self.onmessage / postMessage(value)
 *
 * Messages are structured-cloned between runtimes. A SharedArrayBuffer is
 * NOT copied: its underlying memory is genuinely shared (atomic refcounted
 * control block) across the main thread and workers.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "lr_platform.h"

#include "engine/lr_engine.h"
#include "lr_runtime.h"
#include "lr_thread_pool.h"
#include "lr_sab.h"

/* ── Wire value (serialized form for cross-runtime structured clone) ───── */

typedef enum {
    WIRE_UNDEF, WIRE_NULL, WIRE_BOOL, WIRE_INT, WIRE_FLOAT, WIRE_STRING,
    WIRE_ARRAY, WIRE_OBJECT, WIRE_ARRAY_BUFFER, WIRE_SHARED_BUFFER,
    WIRE_TYPED_ARRAY, WIRE_ERROR
} LRWireType;

typedef struct LR_WireValue {
    LRWireType type;
    int       b;            /* bool */
    int32_t   i;            /* int */
    double    f;            /* float */
    char     *s;            /* string / error message (strdup'd) */
    /* containers */
    struct LR_WireValue **items;  /* array elements */
    char    **keys;               /* object keys (strdup'd) */
    struct LR_WireValue **vals;   /* object values */
    int       count;
    /* buffers */
    uint8_t  *bytes;              /* plain ArrayBuffer byte copy */
    size_t    size;
    LRSabBlock *block;            /* SharedArrayBuffer control block (owned ref) */
    /* typed array view */
    struct LR_WireValue *buf;     /* backing buffer wire value */
    size_t    byte_offset;
    size_t    byte_length;
    int       ta_magic;
} LR_WireValue;

static LR_WireValue *wire_new(LRWireType t)
{
    LR_WireValue *w = (LR_WireValue *)calloc(1, sizeof(LR_WireValue));
    if (w) w->type = t;
    return w;
}

static void wire_free(LR_WireValue *w)
{
    if (!w) return;
    free(w->s);
    free(w->bytes);
    if (w->block) lr_sab_unref(w->block);
    if (w->items) {
        for (int i = 0; i < w->count; i++) wire_free(w->items[i]);
        free(w->items);
    }
    if (w->keys || w->vals) {
        for (int i = 0; i < w->count; i++) {
            if (w->keys) free(w->keys[i]);
            if (w->vals) wire_free(w->vals[i]);
        }
        free(w->keys);
        free(w->vals);
    }
    wire_free(w->buf);
    free(w);
}

/* element size per TA_MAGIC_* id (must match lr_builtins_extra.c) */
static size_t ta_element_size(int magic)
{
    switch (magic) {
        case 0: case 1:         return 1;  /* Uint8 / Int8 */
        case 2: case 3:         return 2;  /* Uint16 / Int16 */
        case 4: case 5: case 6: return 4;  /* Uint32 / Int32 / Float32 */
        case 7: case 8: case 9: return 8;  /* Float64 / BigUint64 / BigInt64 */
        default:                return 1;
    }
}

static const char *ta_magic_name(int magic)
{
    switch (magic) {
        case 0:  return "Uint8Array";
        case 1:  return "Int8Array";
        case 2:  return "Uint16Array";
        case 3:  return "Int16Array";
        case 4:  return "Uint32Array";
        case 5:  return "Int32Array";
        case 6:  return "Float32Array";
        case 7:  return "Float64Array";
        case 8:  return "BigUint64Array";
        case 9:  return "BigInt64Array";
        default: return "Uint8Array";
    }
}

/* ── Serialize: JS value (source runtime) → wire value ─────────────────── */

static LR_WireValue *wire_serialize(LRContext *ctx, LRValue v, int depth)
{
    if (depth > 32) return wire_new(WIRE_UNDEF);

    switch (v.tag) {
    case LR_TYPE_UNDEFINED: return wire_new(WIRE_UNDEF);
    case LR_TYPE_NULL:      return wire_new(WIRE_NULL);
    case LR_TYPE_BOOL: {
        LR_WireValue *w = wire_new(WIRE_BOOL);
        if (w) w->b = v.u.bool_val;
        return w;
    }
    case LR_TYPE_INT32: {
        LR_WireValue *w = wire_new(WIRE_INT);
        if (w) w->i = v.u.int32;
        return w;
    }
    case LR_TYPE_FLOAT64: {
        LR_WireValue *w = wire_new(WIRE_FLOAT);
        if (w) w->f = v.u.float64;
        return w;
    }
    case LR_TYPE_STRING: {
        LR_WireValue *w = wire_new(WIRE_STRING);
        const char *c = JS_ToCString(ctx, v);
        if (w && c) w->s = strdup(c);
        if (c) JS_FreeCString(ctx, c);
        return w;
    }
    case LR_TYPE_OBJECT:
        break;
    default:
        return wire_new(WIRE_UNDEF);
    }

    LRObject *o = (LRObject *)v.u.ptr;

    if (o->type == LR_OBJ_ARRAY_BUFFER) {
        if (lr_array_buffer_is_shared(v)) {
            /* SharedArrayBuffer: share the control block, do NOT copy */
            LR_WireValue *w = wire_new(WIRE_SHARED_BUFFER);
            if (w) {
                w->block = (LRSabBlock *)lr_array_buffer_get_opaque(v);
                lr_sab_ref(w->block);
                w->size = w->block ? w->block->size : 0;
            }
            return w;
        }
        /* Plain ArrayBuffer: copy bytes */
        size_t sz = 0;
        uint8_t *base = lr_get_array_buffer(ctx, &sz, v);
        LR_WireValue *w = wire_new(WIRE_ARRAY_BUFFER);
        if (w) {
            w->size = sz;
            if (sz) {
                w->bytes = (uint8_t *)malloc(sz);
                if (w->bytes && base) memcpy(w->bytes, base, sz);
            }
        }
        return w;
    }

    if (o->type == LR_OBJ_TYPED_ARRAY) {
        TypedArrayData *tad = (TypedArrayData *)lr_get_opaque(v);
        if (!tad) return wire_new(WIRE_UNDEF);
        LR_WireValue *w = wire_new(WIRE_TYPED_ARRAY);
        if (w) {
            w->buf = wire_serialize(ctx, tad->buffer, depth + 1);
            w->byte_offset = tad->byte_offset;
            w->byte_length = tad->byte_length;
            w->ta_magic = tad->magic;
        }
        return w;
    }

    if (o->type == LR_OBJ_ARRAY) {
        LRValue len_val = lr_get_property_str(ctx, v, "length");
        int32_t len = 0;
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        if (len < 0) len = 0;

        LR_WireValue *w = wire_new(WIRE_ARRAY);
        if (!w) return NULL;
        w->count = len;
        if (len > 0) {
            w->items = (LR_WireValue **)calloc((size_t)len, sizeof(LR_WireValue *));
            for (int32_t idx = 0; idx < len; idx++) {
                LRValue item = lr_get_property_uint32(ctx, v, (uint32_t)idx);
                w->items[idx] = wire_serialize(ctx, item, depth + 1);
                lr_free_value(ctx, item);
            }
        }
        return w;
    }

    /* Plain object: own enumerable string-keyed properties */
    {
        LR_WireValue *w = wire_new(WIRE_OBJECT);
        LRPropertyEnum *tab = NULL;
        uint32_t n = 0;
        if (w && lr_get_own_property_names(ctx, &tab, &n, v,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 && tab) {
            w->count = (int)n;
            if (n > 0) {
                w->keys = (char **)calloc(n, sizeof(char *));
                w->vals = (LR_WireValue **)calloc(n, sizeof(LR_WireValue *));
            }
            for (uint32_t idx = 0; idx < n; idx++) {
                const char *name = JS_AtomToCString(ctx, tab[idx].atom);
                w->keys[idx] = name ? strdup(name) : strdup("");
                LRValue val = lr_get_property_str(ctx, v, name ? name : "");
                w->vals[idx] = wire_serialize(ctx, val, depth + 1);
                lr_free_value(ctx, val);
                if (name) JS_FreeCString(ctx, name);
            }
            lr_free_property_enum(ctx, tab, n);
        }
        return w;
    }
}

/* ── Deserialize: wire value → JS value (target runtime) ───────────────── */

static LRValue wire_deserialize(LRContext *ctx, LR_WireValue *w)
{
    if (!w) return LR_VALUE_UNDEFINED;
    switch (w->type) {
    case WIRE_UNDEF:  return LR_VALUE_UNDEFINED;
    case WIRE_NULL:   return LR_VALUE_NULL;
    case WIRE_BOOL:   return w->b ? LR_VALUE_TRUE : LR_VALUE_FALSE;
    case WIRE_INT:    return lr_new_int32(ctx, w->i);
    case WIRE_FLOAT:  return JS_NewFloat64(ctx, w->f);
    case WIRE_STRING: return lr_new_string(ctx, w->s ? w->s : "");
    case WIRE_ARRAY_BUFFER: {
        uint8_t *copy = (uint8_t *)calloc(1, w->size ? w->size : 1);
        if (copy && w->bytes && w->size) memcpy(copy, w->bytes, w->size);
        LRValue ab = lr_new_array_buffer(ctx, copy, w->size,
                                         lr_array_buffer_free, NULL, 0);
        if (JS_IsException(ab)) free(copy);
        return ab;
    }
    case WIRE_SHARED_BUFFER: {
        /* Give the new JS object its own reference; the wire value keeps
         * its own reference until wire_free. */
        lr_sab_ref(w->block);
        return lr_sab_wrap_block(ctx, w->block);
    }
    case WIRE_TYPED_ARRAY: {
        LRValue buf = wire_deserialize(ctx, w->buf);
        if (JS_IsException(buf)) return buf;
        LRValue global = JS_GetGlobalObject(ctx);
        LRValue ctor = lr_get_property_str(ctx, global, ta_magic_name(w->ta_magic));
        JS_FreeValue(ctx, global);
        LRValue args[3];
        args[0] = buf;
        args[1] = lr_new_int32(ctx, (int32_t)w->byte_offset);
        args[2] = lr_new_int32(ctx,
            (int32_t)(w->byte_length / ta_element_size(w->ta_magic)));
        LRValue res = lr_call_constructor(ctx, ctor, 3, args);
        lr_free_value(ctx, ctor);
        lr_free_value(ctx, buf);
        return res;
    }
    case WIRE_ARRAY: {
        LRValue arr = lr_new_array(ctx);
        for (int idx = 0; idx < w->count; idx++) {
            LRValue item = wire_deserialize(ctx, w->items[idx]);
            lr_set_property_uint32(ctx, arr, (uint32_t)idx, item);
        }
        lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, w->count));
        return arr;
    }
    case WIRE_OBJECT: {
        LRValue obj = lr_new_object(ctx);
        for (int idx = 0; idx < w->count; idx++) {
            LRValue val = wire_deserialize(ctx, w->vals[idx]);
            lr_set_property_str(ctx, obj, w->keys[idx] ? w->keys[idx] : "", val);
        }
        return obj;
    }
    case WIRE_ERROR: {
        LRValue err = lr_new_object(ctx);
        lr_set_property_str(ctx, err, "message",
                            lr_new_string(ctx, w->s ? w->s : "Worker error"));
        return err;
    }
    }
    return LR_VALUE_UNDEFINED;
}

/* ── Worker context data ───────────────────────────────────────────────── */

typedef struct LR_WorkerData {
    int              id;             /* Unique worker ID */
    LR_Runtime      *parent_rt;      /* Parent runtime */
    LR_Runtime      *worker_rt;      /* Worker's own runtime */
    char            *script;         /* Script to run */
    pthread_t        thread;         /* Worker thread */
    int              running;
    int              should_stop;

    /* Message queue: parent -> worker (wire values) */
    pthread_mutex_t  in_mutex;
    LR_WireValue   **in_queue;
    int              in_count;
    int              in_capacity;

    /* Message queue: worker -> parent (wire values) */
    pthread_mutex_t  out_mutex;
    LR_WireValue   **out_queue;
    int              out_count;
    int              out_capacity;

    /* JS-side callbacks (parent runtime values) */
    LRValue          onmessage_cb;
    LRValue          onerror_cb;

    /* Linked list */
    struct LR_WorkerData *next;
} LR_WorkerData;

/* ── Global worker registry ────────────────────────────────────────────── */

static LR_WorkerData *g_worker_registry = NULL;
static int            g_worker_next_id = 1;
static pthread_mutex_t g_worker_mutex = PTHREAD_MUTEX_INITIALIZER;

static LR_WorkerData *lr_worker_register(LR_WorkerData *wd)
{
    pthread_mutex_lock(&g_worker_mutex);
    wd->id = g_worker_next_id++;
    wd->next = g_worker_registry;
    g_worker_registry = wd;
    pthread_mutex_unlock(&g_worker_mutex);
    return wd;
}

static LR_WorkerData *lr_worker_lookup(int id)
{
    pthread_mutex_lock(&g_worker_mutex);
    for (LR_WorkerData *wd = g_worker_registry; wd; wd = wd->next) {
        if (wd->id == id) {
            pthread_mutex_unlock(&g_worker_mutex);
            return wd;
        }
    }
    pthread_mutex_unlock(&g_worker_mutex);
    return NULL;
}

static LR_WorkerData *lr_worker_lookup_by_worker_rt(LR_Runtime *rt)
{
    pthread_mutex_lock(&g_worker_mutex);
    for (LR_WorkerData *wd = g_worker_registry; wd; wd = wd->next) {
        if (wd->worker_rt == rt) {
            pthread_mutex_unlock(&g_worker_mutex);
            return wd;
        }
    }
    pthread_mutex_unlock(&g_worker_mutex);
    return NULL;
}

static void lr_worker_unregister(int id)
{
    pthread_mutex_lock(&g_worker_mutex);
    LR_WorkerData *prev = NULL;
    for (LR_WorkerData *wd = g_worker_registry; wd; prev = wd, wd = wd->next) {
        if (wd->id == id) {
            if (prev) prev->next = wd->next;
            else g_worker_registry = wd->next;
            break;
        }
    }
    pthread_mutex_unlock(&g_worker_mutex);
}

/* ── Worker-side postMessage (global function inside the worker) ───────── */

static LRValue lr_worker_postMessage_external(LRContext *ctx, LRValue this_val,
                                               int argc, LRValue *argv)
{
    (void)this_val;
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    if (!rt) return LR_VALUE_UNDEFINED;

    LR_WorkerData *wd = lr_worker_lookup_by_worker_rt(rt);
    if (!wd) return LR_VALUE_UNDEFINED;

    LR_WireValue *wire = wire_serialize(ctx, argc > 0 ? argv[0] : LR_VALUE_UNDEFINED, 0);
    pthread_mutex_lock(&wd->out_mutex);
    if (wd->out_count < wd->out_capacity) {
        wd->out_queue[wd->out_count++] = wire;
    } else {
        wire_free(wire);
    }
    pthread_mutex_unlock(&wd->out_mutex);
    return LR_VALUE_UNDEFINED;
}

/* ── Worker thread function ────────────────────────────────────────────── */

static void *lr_worker_thread_fn(void *arg)
{
    LR_WorkerData *wd = (LR_WorkerData *)arg;

    /* Create isolated runtime for worker */
    LR_Config cfg;
    lr_config_default(&cfg);
    cfg.log_level = LR_LOG_ERROR;
    wd->worker_rt = lr_runtime_new(&cfg);

    if (!wd->worker_rt) {
        wd->running = 0;
        return NULL;
    }

    /* Set up worker global scope */
    LRContext *ctx = wd->worker_rt->lr_ctx;
    LRValue global = JS_GetGlobalObject(ctx);

    /* Inject self (WorkerGlobalScope-like) */
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));

    /* Inject worker-side postMessage(value) that sends to the parent */
    JS_SetPropertyStr(ctx, global, "postMessage",
        JS_NewCFunction(ctx, lr_worker_postMessage_external, "postMessage", 1));

    /* Execute worker script */
    int result = lr_eval_file(wd->worker_rt, wd->script);

    JS_FreeValue(ctx, global);

    if (result < 0) {
        /* Queue error to parent (dispatched via onerror there) */
        const char *err = lr_get_last_error(wd->worker_rt);
        LR_WireValue *wire = wire_new(WIRE_ERROR);
        if (wire) {
            wire->s = strdup(err ? err : "Worker error");
            pthread_mutex_lock(&wd->out_mutex);
            if (wd->out_count < wd->out_capacity) {
                wd->out_queue[wd->out_count++] = wire;
            } else {
                wire_free(wire);
            }
            pthread_mutex_unlock(&wd->out_mutex);
        }
    }

    /* Process incoming messages */
    while (wd->running && !wd->should_stop) {
        pthread_mutex_lock(&wd->in_mutex);
        LR_WireValue *wire = NULL;
        if (wd->in_count > 0) {
            wire = wd->in_queue[0];
            memmove(wd->in_queue, wd->in_queue + 1,
                    (size_t)(wd->in_count - 1) * sizeof(LR_WireValue *));
            wd->in_count--;
        }
        pthread_mutex_unlock(&wd->in_mutex);

        if (wire) {
            LRValue data = wire_deserialize(ctx, wire);
            wire_free(wire);

            LRValue global2 = JS_GetGlobalObject(ctx);
            LRValue onmessage = JS_GetPropertyStr(ctx, global2, "onmessage");
            if (JS_IsFunction(ctx, onmessage)) {
                LRValue event = lr_new_object(ctx);
                lr_set_property_str(ctx, event, "data", JS_DupValue(ctx, data));
                LRValue ret = JS_Call(ctx, onmessage, global2, 1, &event);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, event);
            }
            JS_FreeValue(ctx, onmessage);
            JS_FreeValue(ctx, global2);
            JS_FreeValue(ctx, data);

            /* Run microtasks/timers produced by the handler */
            if (lr_event_loop_pending(wd->worker_rt)) {
                lr_event_loop_run(wd->worker_rt);
            }
        } else {
            lr_sleep_ms(1);  /* 1ms idle */
        }
    }

    /* Drain anything still queued (avoid leaks when terminated early) */
    pthread_mutex_lock(&wd->in_mutex);
    for (int i = 0; i < wd->in_count; i++) wire_free(wd->in_queue[i]);
    wd->in_count = 0;
    pthread_mutex_unlock(&wd->in_mutex);

    /* Cleanup worker runtime */
    lr_runtime_free(wd->worker_rt);
    wd->worker_rt = NULL;
    wd->running = 0;

    return NULL;
}

/* ── Worker constructor ────────────────────────────────────────────────── */

static LRValue lr_worker_constructor(LRContext *ctx, LRValue new_target,
                                      int argc, LRValue *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Worker requires at least 1 argument (script path)");
    }

    const char *script = JS_ToCString(ctx, argv[0]);
    if (!script) return JS_EXCEPTION;

    /* Create worker data */
    LR_WorkerData *wd = calloc(1, sizeof(LR_WorkerData));
    if (!wd) {
        JS_FreeCString(ctx, script);
        return JS_ThrowOutOfMemory(ctx);
    }

    LR_Runtime *parent_rt = JS_GetContextOpaque(ctx);
    wd->parent_rt = parent_rt;
    wd->script = strdup(script);
    wd->running = 1;
    wd->should_stop = 0;
    wd->onmessage_cb = LR_VALUE_UNDEFINED;
    wd->onerror_cb = LR_VALUE_UNDEFINED;

    /* Initialize message queues */
    wd->in_capacity = 64;
    wd->in_queue = calloc((size_t)wd->in_capacity, sizeof(LR_WireValue *));
    wd->out_capacity = 64;
    wd->out_queue = calloc((size_t)wd->out_capacity, sizeof(LR_WireValue *));
    pthread_mutex_init(&wd->in_mutex, NULL);
    pthread_mutex_init(&wd->out_mutex, NULL);

    /* Register in global registry */
    lr_worker_register(wd);

    /* Start worker thread */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&wd->thread, &attr, lr_worker_thread_fn, wd) != 0) {
        lr_worker_unregister(wd->id);
        JS_FreeCString(ctx, script);
        free(wd->script);
        free(wd->in_queue);
        free(wd->out_queue);
        pthread_mutex_destroy(&wd->in_mutex);
        pthread_mutex_destroy(&wd->out_mutex);
        free(wd);
        return JS_ThrowInternalError(ctx, "Failed to create worker thread");
    }

    pthread_attr_destroy(&attr);

    /* In this engine, lr_call_constructor passes the freshly created `this`
     * object (with the correct prototype already set) as new_target. */
    JS_SetPropertyStr(ctx, new_target, "__worker_id", JS_NewInt32(ctx, wd->id));

    JS_FreeCString(ctx, script);
    return JS_DupValue(ctx, new_target);
}

/* ── Worker.prototype.postMessage ──────────────────────────────────────── */

static LRValue lr_worker_postMessage(LRContext *ctx, LRValue this_val,
                                      int argc, LRValue *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "postMessage requires at least 1 argument");
    }

    /* Get worker ID from object property */
    LRValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) {
        return JS_ThrowTypeError(ctx, "Invalid Worker object");
    }
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd || !wd->running) {
        return JS_ThrowTypeError(ctx, "Worker has been terminated");
    }

    /* Structured-clone into a wire value and queue it. SharedArrayBuffers
     * keep their underlying memory shared. */
    LR_WireValue *wire = wire_serialize(ctx, argv[0], 0);
    pthread_mutex_lock(&wd->in_mutex);
    if (wd->in_count >= wd->in_capacity) {
        pthread_mutex_unlock(&wd->in_mutex);
        wire_free(wire);
        return JS_ThrowTypeError(ctx, "Worker message queue full");
    }
    wd->in_queue[wd->in_count++] = wire;
    pthread_mutex_unlock(&wd->in_mutex);

    return LR_VALUE_UNDEFINED;
}

/* ── Worker.prototype.terminate ────────────────────────────────────────── */

static LRValue lr_worker_terminate(LRContext *ctx, LRValue this_val,
                                    int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    LRValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return LR_VALUE_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (wd) {
        wd->should_stop = 1;
        wd->running = 0;
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Worker.prototype.onmessage getter/setter ──────────────────────────── */

static LRValue lr_worker_get_onmessage(LRContext *ctx, LRValue this_val,
                                       int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    LRValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return LR_VALUE_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return LR_VALUE_UNDEFINED;
    return JS_DupValue(ctx, wd->onmessage_cb);
}

static LRValue lr_worker_set_onmessage(LRContext *ctx, LRValue this_val,
                                        int argc, LRValue *argv)
{
    LRValue val = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    LRValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return LR_VALUE_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return LR_VALUE_UNDEFINED;

    JS_FreeValue(ctx, wd->onmessage_cb);
    wd->onmessage_cb = JS_DupValue(ctx, val);
    return LR_VALUE_UNDEFINED;
}

static LRValue lr_worker_get_onerror(LRContext *ctx, LRValue this_val,
                                     int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    LRValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return LR_VALUE_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return LR_VALUE_UNDEFINED;
    return JS_DupValue(ctx, wd->onerror_cb);
}

static LRValue lr_worker_set_onerror(LRContext *ctx, LRValue this_val,
                                     int argc, LRValue *argv)
{
    LRValue val = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    LRValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return LR_VALUE_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return LR_VALUE_UNDEFINED;

    JS_FreeValue(ctx, wd->onerror_cb);
    wd->onerror_cb = JS_DupValue(ctx, val);
    return LR_VALUE_UNDEFINED;
}

/* ── Parent-side poll: drain worker out queues, fire callbacks ─────────── */

/* Returns the number of workers still running whose parent is rt. */
int lr_worker_poll(LR_Runtime *rt)
{
    /* Collect matching workers first (avoid holding the registry lock
     * while running JS callbacks, which could re-enter worker APIs). */
    LR_WorkerData *matches[64];
    int nmatch = 0;
    int alive = 0;

    pthread_mutex_lock(&g_worker_mutex);
    for (LR_WorkerData *wd = g_worker_registry; wd; wd = wd->next) {
        if (wd->parent_rt != rt) continue;
        if (wd->running) alive++;
        if (nmatch < 64) matches[nmatch++] = wd;
    }
    pthread_mutex_unlock(&g_worker_mutex);

    LRContext *ctx = rt->lr_ctx;

    for (int m = 0; m < nmatch; m++) {
        LR_WorkerData *wd = matches[m];

        for (;;) {
            pthread_mutex_lock(&wd->out_mutex);
            LR_WireValue *wire = NULL;
            if (wd->out_count > 0) {
                wire = wd->out_queue[0];
                memmove(wd->out_queue, wd->out_queue + 1,
                        (size_t)(wd->out_count - 1) * sizeof(LR_WireValue *));
                wd->out_count--;
            }
            pthread_mutex_unlock(&wd->out_mutex);
            if (!wire) break;

            int is_err = (wire->type == WIRE_ERROR);
            LRValue data = wire_deserialize(ctx, wire);
            wire_free(wire);

            LRValue cb = is_err ? wd->onerror_cb : wd->onmessage_cb;
            if (JS_IsFunction(ctx, cb)) {
                LRValue event = lr_new_object(ctx);
                lr_set_property_str(ctx, event, "data", JS_DupValue(ctx, data));
                LRValue global = JS_GetGlobalObject(ctx);
                LRValue ret = JS_Call(ctx, cb, global, 1, &event);
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, global);
                JS_FreeValue(ctx, event);
            }
            JS_FreeValue(ctx, data);
        }
    }

    return alive;
}

/* ── Function list ─────────────────────────────────────────────────────── */

static const JSCFunctionListEntry lr_worker_proto_funcs[] = {
    JS_CFUNC_DEF("postMessage", 1, lr_worker_postMessage),
    JS_CFUNC_DEF("terminate",   0, lr_worker_terminate),
    JS_CGETSET_DEF("onmessage",  lr_worker_get_onmessage, lr_worker_set_onmessage),
    JS_CGETSET_DEF("onerror",    lr_worker_get_onerror, lr_worker_set_onerror),
};

/* ── Init ──────────────────────────────────────────────────────────────── */

void lr_worker_init(LR_Runtime *rt)
{
    LRContext *ctx = rt->lr_ctx;
    LRValue global = JS_GetGlobalObject(ctx);

    /* Worker prototype */
    LRValue worker_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, worker_proto, lr_worker_proto_funcs,
                                sizeof(lr_worker_proto_funcs) / sizeof(lr_worker_proto_funcs[0]));

    /* Worker constructor */
    LRValue worker_ctor = JS_NewCFunction2(ctx, lr_worker_constructor, "Worker", 1,
                                            JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, worker_ctor, "prototype", JS_DupValue(ctx, worker_proto));
    JS_FreeValue(ctx, worker_proto);
    JS_SetPropertyStr(ctx, global, "Worker", worker_ctor);

    JS_FreeValue(ctx, global);
}
