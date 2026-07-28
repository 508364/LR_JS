/*
 * L/R_JS - WebSocket API (host-delegated)
 *
 * L/R_JS does NOT implement the WebSocket protocol. Connections are delegated
 * to the host application through the LR_WsWrapper interface, exactly like the
 * HTTP/fetch wrapper. The host reports lifecycle events back into the engine
 * by calling lr_ws_on_open() / lr_ws_on_message() / lr_ws_on_close() /
 * lr_ws_on_error(), which must be invoked from the engine thread (e.g. from
 * the host's I/O pump integrated with lr_event_loop_run()).
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_runtime.h"

/* ── Per-instance state ───────────────────────────────────────────────── */
typedef struct LR_WsState {
    void *conn_handle;   /* host connection handle (NULL until connected) */
    int   ready_state;   /* 0 CONNECTING, 1 OPEN, 2 CLOSING, 3 CLOSED */
    char *url;
    char *protocol;      /* requested protocols (comma separated), or NULL */
} LR_WsState;

/* ── Connection registry: conn_handle -> JS object (dup'd) ──────────────── */
typedef struct LR_WsEntry {
    void            *conn_handle;
    JSValue          obj;     /* JS_DupValue'd; kept alive until close */
    struct LR_WsEntry *next;
} LR_WsEntry;

static void ws_registry_add(LR_Runtime *rt, void *handle, JSValue obj)
{
    LR_WsEntry *e = (LR_WsEntry *)calloc(1, sizeof(*e));
    if (!e) return;
    e->conn_handle = handle;
    e->obj = JS_DupValue(rt->lr_ctx, obj);
    e->next = (LR_WsEntry *)rt->ws_registry;
    rt->ws_registry = e;
}

static LR_WsEntry *ws_find(LR_Runtime *rt, void *handle)
{
    for (LR_WsEntry *e = (LR_WsEntry *)rt->ws_registry; e; e = e->next)
        if (e->conn_handle == handle) return e;
    return NULL;
}

/* ── Wrapper API ──────────────────────────────────────────────────────── */

void lr_ws_set_wrapper(LR_Runtime *rt, LR_WsWrapper *wrapper)
{
    rt->ws_wrapper = wrapper;
}

LR_WsWrapper *lr_ws_get_wrapper(LR_Runtime *rt)
{
    return rt->ws_wrapper;
}

/* ── Event dispatch helper ────────────────────────────────────────────── */

/* Delivers an event to both addEventListener listeners and the on<Type>
   handler property of the given WebSocket object. Takes ownership of `event`. */
static void ws_dispatch(LR_Runtime *rt, JSValue obj, const char *type, JSValue event)
{
    JSContext *ctx = rt->lr_ctx;

    /* Fire addEventListener-style listeners via dispatchEvent */
    JSValue disp = JS_GetPropertyStr(ctx, obj, "dispatchEvent");
    if (JS_IsFunction(ctx, disp)) {
        JSValue ret = JS_Call(ctx, disp, obj, 1, (JSValueConst *)&event);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, disp);

    /* Fire the on<Type> handler property if present */
    char hname[32];
    snprintf(hname, sizeof(hname), "on%s", type);
    JSValue cb = JS_GetPropertyStr(ctx, obj, hname);
    if (JS_IsFunction(ctx, cb)) {
        JSValue ret = JS_Call(ctx, cb, obj, 1, (JSValueConst *)&event);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, cb);

    JS_FreeValue(ctx, event);
}

/* ── Deferred error dispatch ──────────────────────────────────────────── */
/* Per spec, a connection error must be reported as an asynchronous event so
 * that listeners (onerror / addEventListener("error", ...)) attached right
 * after `new WebSocket()` still receive it. We therefore defer the dispatch
 * to a microtask (job) instead of firing it synchronously inside the ctor. */

typedef struct {
    LR_Runtime *rt;
    JSValue     obj;     /* dup'd; kept alive until the job runs */
    char       *message;
} WsErrorJobData;

static void ws_error_job_data_free(void *p)
{
    WsErrorJobData *jd = (WsErrorJobData *)p;
    if (!jd) return;
    JS_FreeValue(jd->rt->lr_ctx, jd->obj);
    free(jd->message);
    free(jd);
}

static JSValue ws_error_job_func(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    LRObject *fobj = (LRObject *)ctx->current_func.u.ptr;
    if (!fobj || fobj->type != LR_OBJ_CFUNCTION) return JS_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)fobj->extra;
    WsErrorJobData *jd = (WsErrorJobData *)cf->data;
    if (!jd) return JS_UNDEFINED;
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "error"));
    JS_SetPropertyStr(ctx, ev, "message",
                      JS_NewString(ctx, jd->message ? jd->message : ""));
    ws_dispatch(jd->rt, jd->obj, "error", ev);
    return JS_UNDEFINED;
}

/* Enqueue the error event as a microtask so it runs after the current script
 * finishes and any synchronously-attached listeners are in place. */
static void ws_dispatch_error_deferred(LR_Runtime *rt, JSValue obj,
                                       const char *message)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue fn = JS_NewCFunction(ctx, ws_error_job_func, "wsError", 0);
    if (fn.tag != LR_TYPE_OBJECT) return;
    LRObject *fobj = (LRObject *)fn.u.ptr;
    LRCFunction *cf = (LRCFunction *)fobj->extra;
    WsErrorJobData *jd = (WsErrorJobData *)malloc(sizeof(*jd));
    if (!jd) { JS_FreeValue(ctx, fn); return; }
    jd->rt      = rt;
    jd->obj     = JS_DupValue(ctx, obj);
    jd->message = message ? strdup(message) : NULL;
    cf->data = jd;
    cf->data_free = ws_error_job_data_free;
    /* lr_enqueue_job expects the engine runtime (LRRuntime*), not the
     * high-level LR_Runtime* wrapper. */
    lr_enqueue_job(rt->lr_rt, ctx, fn);
    JS_FreeValue(ctx, fn);
}

/* ── Engine-side callbacks (called by the host) ───────────────────────── */

void lr_ws_on_open(LR_Runtime *rt, void *conn_handle)
{
    LR_WsEntry *e = ws_find(rt, conn_handle);
    if (!e) return;
    JSContext *ctx = rt->lr_ctx;
    LR_WsState *st = (LR_WsState *)JS_GetOpaque(e->obj, NULL);
    if (st) {
        st->ready_state = 1;
        JS_SetPropertyStr(ctx, e->obj, "readyState", JS_NewInt32(ctx, 1));
    }
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "open"));
    ws_dispatch(rt, e->obj, "open", ev);
}

void lr_ws_on_message(LR_Runtime *rt, void *conn_handle,
                      const void *data, size_t len)
{
    LR_WsEntry *e = ws_find(rt, conn_handle);
    if (!e) return;
    JSContext *ctx = rt->lr_ctx;
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "message"));
    JS_SetPropertyStr(ctx, ev, "data",
                      JS_NewStringLen(ctx, data ? (const char *)data : "", len));
    ws_dispatch(rt, e->obj, "message", ev);
}

void lr_ws_on_close(LR_Runtime *rt, void *conn_handle, int code, const char *reason)
{
    LR_WsEntry *e = ws_find(rt, conn_handle);
    if (!e) return;
    JSContext *ctx = rt->lr_ctx;

    /* Capture the object and unlink the entry FIRST, so a re-entrant close()
       (triggered from a listener) finds no entry and does nothing. */
    JSValue obj = e->obj;
    if (rt->ws_registry == e) {
        rt->ws_registry = e->next;
    } else {
        for (LR_WsEntry *p = (LR_WsEntry *)rt->ws_registry; p; p = p->next) {
            if (p->next == e) { p->next = e->next; break; }
        }
    }
    free(e);

    /* Free per-instance state (NULL-class opaque has no finalizer). */
    LR_WsState *st = (LR_WsState *)JS_GetOpaque(obj, NULL);
    if (st) {
        free(st->url);
        free(st->protocol);
        free(st);
        JS_SetOpaque(obj, NULL);
    }

    JS_SetPropertyStr(ctx, obj, "readyState", JS_NewInt32(ctx, 3)); /* CLOSED */
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "close"));
    JS_SetPropertyStr(ctx, ev, "code", JS_NewInt32(ctx, code));
    JS_SetPropertyStr(ctx, ev, "reason", JS_NewString(ctx, reason ? reason : ""));
    ws_dispatch(rt, obj, "close", ev);

    JS_FreeValue(ctx, obj);
}

void lr_ws_on_error(LR_Runtime *rt, void *conn_handle, const char *message)
{
    LR_WsEntry *e = ws_find(rt, conn_handle);
    if (!e) return;
    JSContext *ctx = rt->lr_ctx;
    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "error"));
    JS_SetPropertyStr(ctx, ev, "message", JS_NewString(ctx, message ? message : ""));
    ws_dispatch(rt, e->obj, "error", ev);
}

/* ── JS WebSocket class ───────────────────────────────────────────────── */

static JSValue lr_ws_constructor(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    /* In this engine constructors receive the engine-created instance as
     * this_val; its [[Prototype]] is already WebSocket.prototype, so we must
     * use it directly rather than allocating a fresh object. */
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "WebSocket must be called with 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WebSocket requires a url argument");

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);

    JSValue obj = this_val;  /* use the engine-created instance */

    /* Listener storage for addEventListener/dispatchEvent */
    JS_SetPropertyStr(ctx, obj, "__listeners", JS_NewArray(ctx));

    LR_WsState *st = (LR_WsState *)calloc(1, sizeof(*st));
    st->ready_state = 0; /* CONNECTING */

    const char *url = JS_ToCString(ctx, argv[0]);
    st->url = url ? strdup(url) : NULL;
    if (url) JS_FreeCString(ctx, url);

    /* protocols: string or array -> comma separated */
    char *proto_str = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_IsString(argv[1])) {
            const char *p = JS_ToCString(ctx, argv[1]);
            proto_str = p ? strdup(p) : NULL;
            if (p) JS_FreeCString(ctx, p);
        } else if (JS_IsArray(ctx, argv[1])) {
            size_t cap = 64, len = 0;
            proto_str = (char *)malloc(cap);
            if (proto_str) {
                proto_str[0] = '\0';
                JSValue length_v = JS_GetPropertyStr(ctx, argv[1], "length");
                int32_t n = 0;
                JS_ToInt32(ctx, &n, length_v);
                JS_FreeValue(ctx, length_v);
                for (int32_t i = 0; i < n; i++) {
                    JSValue item = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
                    const char *s = JS_ToCString(ctx, item);
                    if (s) {
                        size_t need = len + (len ? 1 : 0) + strlen(s) + 1;
                        if (need > cap) {
                            cap = need * 2;
                            char *nw = (char *)realloc(proto_str, cap);
                            if (!nw) { free(proto_str); proto_str = NULL; break; }
                            proto_str = nw;
                        }
                        len += sprintf(proto_str + len, "%s%s", len ? "," : "", s);
                        JS_FreeCString(ctx, s);
                    }
                    JS_FreeValue(ctx, item);
                }
            }
        }
    }
    st->protocol = proto_str;

    JS_SetOpaque(obj, st);

    JS_SetPropertyStr(ctx, obj, "readyState", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, st->url ? st->url : ""));
    JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "extensions", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, obj, "binaryType", JS_NewString(ctx, "blob"));
    JS_SetPropertyStr(ctx, obj, "bufferedAmount", JS_NewInt32(ctx, 0));

    LR_WsWrapper *wrapper = rt->ws_wrapper;
    if (!wrapper || !wrapper->connect) {
        st->ready_state = 3;
        JS_SetPropertyStr(ctx, obj, "readyState", JS_NewInt32(ctx, 3));
        /* Defer so listeners attached right after `new WebSocket()` fire. */
        ws_dispatch_error_deferred(rt, obj,
            "WebSocket requires a host wrapper; call lr_ws_set_wrapper() first.");
        return obj;
    }

    void *handle = NULL;
    int ret = wrapper->connect(wrapper->user_data, st->url ? st->url : "",
                               st->protocol ? st->protocol : "", &handle);
    if (ret < 0 || !handle) {
        st->ready_state = 3;
        JS_SetPropertyStr(ctx, obj, "readyState", JS_NewInt32(ctx, 3));
        ws_dispatch_error_deferred(rt, obj, "WebSocket connection failed");
        return obj;
    }

    st->conn_handle = handle;
    ws_registry_add(rt, handle, obj);
    return obj;
}

static JSValue lr_ws_send(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "send requires 1 argument");
    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);
    LR_WsState *st = (LR_WsState *)JS_GetOpaque(this_val, NULL);
    if (!st) return JS_ThrowTypeError(ctx, "WebSocket instance required");
    if (st->ready_state != 1)
        return JS_ThrowTypeError(ctx, "WebSocket is not open");
    if (!rt->ws_wrapper || !rt->ws_wrapper->send)
        return JS_ThrowTypeError(ctx, "no WebSocket wrapper configured");

    const char *data = JS_ToCString(ctx, argv[0]);
    if (!data) return JS_EXCEPTION;
    int ret = rt->ws_wrapper->send(rt->ws_wrapper->user_data, st->conn_handle,
                                   data, strlen(data));
    JS_FreeCString(ctx, data);
    if (ret < 0) return JS_ThrowTypeError(ctx, "WebSocket send failed");
    return JS_UNDEFINED;
}

static JSValue lr_ws_close(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);
    LR_WsState *st = (LR_WsState *)JS_GetOpaque(this_val, NULL);
    if (!st) return JS_ThrowTypeError(ctx, "WebSocket instance required");

    if (st->ready_state == 0 || st->ready_state == 1) {
        st->ready_state = 2; /* CLOSING */
        JS_SetPropertyStr(ctx, this_val, "readyState", JS_NewInt32(ctx, 2));
        if (rt->ws_wrapper && rt->ws_wrapper->close && st->conn_handle) {
        int32_t code = 0;
        const char *reason = "";
        const char *reason_cstr = NULL;
        if (argc >= 1) JS_ToInt32(ctx, &code, argv[0]);
            if (argc >= 2) {
                reason_cstr = JS_ToCString(ctx, argv[1]);
                reason = reason_cstr ? reason_cstr : "";
            }
            rt->ws_wrapper->close(rt->ws_wrapper->user_data, st->conn_handle,
                                  code, reason);
            if (reason_cstr) JS_FreeCString(ctx, reason_cstr);
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry lr_ws_proto_funcs[] = {
    JS_CFUNC_DEF("send",               1, lr_ws_send),
    JS_CFUNC_DEF("close",              0, lr_ws_close),
    JS_CFUNC_DEF("addEventListener",   2, lr_event_target_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, lr_event_target_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent",      1, lr_event_target_dispatchEvent),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_ws_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, lr_ws_proto_funcs,
                               sizeof(lr_ws_proto_funcs) / sizeof(lr_ws_proto_funcs[0]));

    JSValue ctor = JS_NewCFunction2(ctx, lr_ws_constructor, "WebSocket", 1,
                                    JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));
    JS_FreeValue(ctx, proto);

    /* Static readyState constants */
    JS_SetPropertyStr(ctx, ctor, "CONNECTING", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, ctor, "OPEN",       JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, ctor, "CLOSING",    JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, ctor, "CLOSED",     JS_NewInt32(ctx, 3));

    JS_SetPropertyStr(ctx, global, "WebSocket", ctor);
    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "WebSocket API initialized (host-delegated wrapper)");
}
