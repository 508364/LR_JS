/*
 * L/R_JS - Worker API Module
 * Pure C, Web Worker-like multi-threaded message passing.
 * Each Worker runs in its own thread with an isolated JSRuntime.
 *
 * JS API:
 *   const w = new Worker('script.js');
 *   w.postMessage({data: 'hello'});
 *   w.onmessage = (e) => console.log(e.data);
 *   w.terminate();
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "lr_platform.h"

#include "engine/lr_engine.h"
#include "lr_runtime.h"
#include "lr_thread_pool.h"

/* ── Worker context data ───────────────────────────────────────────────── */

typedef struct LR_WorkerData {
    int              id;             /* Unique worker ID */
    LR_Runtime      *parent_rt;      /* Parent runtime */
    LR_Runtime      *worker_rt;      /* Worker's own runtime */
    char            *script;         /* Script to run */
    pthread_t        thread;         /* Worker thread */
    int              running;
    int              should_stop;

    /* Message queue: parent -> worker */
    pthread_mutex_t  in_mutex;
    JSValue         *in_queue;
    int              in_count;
    int              in_capacity;

    /* Message queue: worker -> parent */
    pthread_mutex_t  out_mutex;
    JSValue         *out_queue;
    int              out_count;
    int              out_capacity;

    /* JS-side callback */
    JSValue          onmessage_cb;
    JSValue          onerror_cb;

    /* Linked list */
    struct LR_WorkerData *next;
} LR_WorkerData;

/* ── Global worker registry (avoids class registration GC issues) ─────── */

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
    JSContext *ctx = wd->worker_rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Inject self (WorkerGlobalScope-like) */
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));

    /* Execute worker script */
    int result = lr_eval_file(wd->worker_rt, wd->script);

    JS_FreeValue(ctx, global);

    if (result < 0 && wd->onerror_cb.tag != JS_TAG_UNDEFINED) {
        /* Queue error to parent */
        pthread_mutex_lock(&wd->out_mutex);
        if (wd->out_count < wd->out_capacity) {
            const char *err = lr_get_last_error(wd->worker_rt);
            JSValue err_obj = JS_NewError(ctx);
            JS_DefinePropertyValueStr(ctx, err_obj, "message",
                                      JS_NewString(ctx, err ? err : "Worker error"),
                                      JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
            wd->out_queue[wd->out_count++] = JS_DupValue(ctx, err_obj);
            JS_FreeValue(ctx, err_obj);
        }
        pthread_mutex_unlock(&wd->out_mutex);
    }

    /* Process incoming messages */
    while (wd->running && !wd->should_stop) {
        pthread_mutex_lock(&wd->in_mutex);
        int has_msg = (wd->in_count > 0);
        pthread_mutex_unlock(&wd->in_mutex);

        if (has_msg) {
            /* Process one message */
            pthread_mutex_lock(&wd->in_mutex);
            JSValue msg = wd->in_queue[0];
            memmove(wd->in_queue, wd->in_queue + 1,
                    (size_t)(wd->in_count - 1) * sizeof(JSValue));
            wd->in_count--;
            pthread_mutex_unlock(&wd->in_mutex);

            /* Call worker's onmessage if set */
            JSValue global2 = JS_GetGlobalObject(ctx);
            JSValue onmessage = JS_GetPropertyStr(ctx, global2, "onmessage");
            if (JS_IsFunction(ctx, onmessage)) {
                JSValue event = JS_NewObject(ctx);
                JS_DefinePropertyValueStr(ctx, event, "data",
                                          msg, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
                JS_Call(ctx, onmessage, global2, 1, &event);
                JS_FreeValue(ctx, event);
            }
            JS_FreeValue(ctx, onmessage);
            JS_FreeValue(ctx, global2);
            JS_FreeValue(ctx, msg);
        } else {
            lr_sleep_ms(1);  /* 1ms idle */
        }
    }

    /* Cleanup worker runtime */
    lr_runtime_free(wd->worker_rt);
    wd->worker_rt = NULL;
    wd->running = 0;

    return NULL;
}

/* ── Worker constructor ────────────────────────────────────────────────── */

static JSValue lr_worker_constructor(JSContext *ctx, JSValueConst new_target,
                                      int argc, JSValueConst *argv)
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
    wd->onmessage_cb = JS_UNDEFINED;
    wd->onerror_cb = JS_UNDEFINED;

    /* Initialize message queues */
    wd->in_capacity = 64;
    wd->in_queue = calloc((size_t)wd->in_capacity, sizeof(JSValue));
    wd->out_capacity = 64;
    wd->out_queue = calloc((size_t)wd->out_capacity, sizeof(JSValue));
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

    /* Create JS worker object with worker ID stored as property */
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);

    /* Store worker ID in the object */
    JS_SetPropertyStr(ctx, obj, "__worker_id", JS_NewInt32(ctx, wd->id));

    JS_FreeCString(ctx, script);
    return obj;
}

/* ── Worker.prototype.postMessage ──────────────────────────────────────── */

static JSValue lr_worker_postMessage(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "postMessage requires at least 1 argument");
    }

    /* Get worker ID from object property */
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
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

    /* Queue message to worker */
    pthread_mutex_lock(&wd->in_mutex);
    if (wd->in_count >= wd->in_capacity) {
        pthread_mutex_unlock(&wd->in_mutex);
        return JS_ThrowTypeError(ctx, "Worker message queue full");
    }
    wd->in_queue[wd->in_count++] = JS_DupValue(ctx, argv[0]);
    pthread_mutex_unlock(&wd->in_mutex);

    return JS_UNDEFINED;
}

/* ── Worker.prototype.terminate ────────────────────────────────────────── */

static JSValue lr_worker_terminate(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return JS_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (wd) {
        wd->should_stop = 1;
        wd->running = 0;
    }

    return JS_UNDEFINED;
}

/* ── Worker.prototype.onmessage getter/setter ──────────────────────────── */

static JSValue lr_worker_get_onmessage(JSContext *ctx, JSValueConst this_val)
{
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return JS_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return JS_UNDEFINED;
    return JS_DupValue(ctx, wd->onmessage_cb);
}

static JSValue lr_worker_set_onmessage(JSContext *ctx, JSValueConst this_val,
                                        JSValueConst val)
{
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return JS_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return JS_UNDEFINED;

    JS_FreeValue(ctx, wd->onmessage_cb);
    wd->onmessage_cb = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
}

static JSValue lr_worker_get_onerror(JSContext *ctx, JSValueConst this_val)
{
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return JS_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return JS_UNDEFINED;
    return JS_DupValue(ctx, wd->onerror_cb);
}

static JSValue lr_worker_set_onerror(JSContext *ctx, JSValueConst this_val,
                                      JSValueConst val)
{
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "__worker_id");
    if (JS_IsUndefined(id_val)) return JS_UNDEFINED;
    int worker_id;
    JS_ToInt32(ctx, &worker_id, id_val);
    JS_FreeValue(ctx, id_val);

    LR_WorkerData *wd = lr_worker_lookup(worker_id);
    if (!wd) return JS_UNDEFINED;

    JS_FreeValue(ctx, wd->onerror_cb);
    wd->onerror_cb = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
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
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Worker prototype */
    JSValue worker_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, worker_proto, lr_worker_proto_funcs,
                                sizeof(lr_worker_proto_funcs) / sizeof(lr_worker_proto_funcs[0]));

    /* Worker constructor */
    JSValue worker_ctor = JS_NewCFunction2(ctx, lr_worker_constructor, "Worker", 1,
                                            JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, worker_ctor, "prototype", JS_DupValue(ctx, worker_proto));
    JS_FreeValue(ctx, worker_proto);
    JS_SetPropertyStr(ctx, global, "Worker", worker_ctor);

    JS_FreeValue(ctx, global);
}