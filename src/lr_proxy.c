/*
 * LR_JS - Proxy Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the Proxy constructor and helper functions.
 * Trap invocation is handled by the engine layer (lr_engine.c).
 */
#include "lr_proxy.h"
#include "lr_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Forward Declarations ──────────────────────────────────────────────── */

static LRValue js_proxy_constructor(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
static LRValue js_proxy_revocable(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);

/* ── Proxy Data Management ─────────────────────────────────────────────── */

static LRProxyData *proxy_data_new(LRContext *ctx, LRValue target, LRValue handler)
{
    LRProxyData *pd = (LRProxyData *)calloc(1, sizeof(LRProxyData));
    if (!pd) return NULL;
    pd->target = lr_dup_value(ctx, target);
    pd->handler = lr_dup_value(ctx, handler);
    return pd;
}

static void proxy_data_free(LRContext *ctx, LRProxyData *pd)
{
    if (!pd) return;
    lr_free_value(ctx, pd->target);
    lr_free_value(ctx, pd->handler);
    free(pd);
}

/* ── Public API ────────────────────────────────────────────────────────── */

int lr_is_proxy(LRValue val)
{
    if (val.tag != LR_TYPE_OBJECT) return 0;
    LRObject *obj = (LRObject *)val.u.ptr;
    return obj->type == LR_OBJ_PROXY;
}

LRProxyData *lr_get_proxy_data(LRValue proxy)
{
    if (!lr_is_proxy(proxy)) return NULL;
    LRObject *obj = (LRObject *)proxy.u.ptr;
    return (LRProxyData *)obj->extra;
}

LRValue lr_new_proxy(LRContext *ctx, LRValue target, LRValue handler)
{
    /* Validate target is an object */
    if (target.tag != LR_TYPE_OBJECT) {
        return lr_throw_type_error(ctx, "Proxy target must be an object");
    }

    /* Validate handler is an object */
    if (handler.tag != LR_TYPE_OBJECT) {
        return lr_throw_type_error(ctx, "Proxy handler must be an object");
    }

    LRObject *obj = (LRObject *)calloc(1, sizeof(LRObject));
    if (!obj) {
        return LR_VALUE_EXCEPTION;
    }
    obj->ref_count = 1;
    obj->type = LR_OBJ_PROXY;
    obj->ctx = ctx;
    obj->is_extensible = 1;

    /* Create Proxy data */
    LRProxyData *pd = proxy_data_new(ctx, target, handler);
    if (!pd) {
        free(obj);
        return LR_VALUE_EXCEPTION;
    }
    obj->extra = pd;

    ctx->rt->obj_count++;

    LRValue v;
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    return v;
}

/* ── Proxy Constructor Data ────────────────────────────────────────────── */

typedef struct {
    LRValue proxy;   /* the proxy to revoke */
    int     revoked;
} RevocableData;

static void revocable_data_free(LRContext *ctx, RevocableData *rd)
{
    if (!rd) return;
    lr_free_value(ctx, rd->proxy);
    free(rd);
}

/* ── Proxy Revocation Function ─────────────────────────────────────────── */

static LRValue revoke_func(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;

    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    RevocableData *rd = (RevocableData *)cf->data;
    if (!rd || rd->revoked) return LR_VALUE_UNDEFINED;

    rd->revoked = 1;

    /* Nullify the proxy's handler */
    if (lr_is_proxy(rd->proxy)) {
        LRObject *pobj = (LRObject *)rd->proxy.u.ptr;
        LRProxyData *pd = (LRProxyData *)pobj->extra;
        if (pd) {
            /* Replace handler with null to make the proxy non-functional */
            lr_free_value(ctx, pd->handler);
            pd->handler = LR_VALUE_NULL;
        }
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Proxy.revocable ───────────────────────────────────────────────────── */

static LRValue js_proxy_revocable(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue target = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    LRValue handler = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    /* Create the proxy */
    LRValue proxy = lr_new_proxy(ctx, target, handler);
    if (lr_is_exception(proxy)) return proxy;

    /* Create the revoke function */
    LRValue revoke_fn = lr_new_cfunction(ctx, revoke_func, "revoke", 0);

    /* Create revocable data */
    RevocableData *rd = (RevocableData *)calloc(1, sizeof(RevocableData));
    if (!rd) {
        lr_free_value(ctx, proxy);
        lr_free_value(ctx, revoke_fn);
        return LR_VALUE_EXCEPTION;
    }
    rd->proxy = lr_dup_value(ctx, proxy);
    rd->revoked = 0;

    /* Store revocable data in the function */
    if (revoke_fn.tag == LR_TYPE_OBJECT) {
        LRObject *fobj = (LRObject *)revoke_fn.u.ptr;
        if (fobj && fobj->extra) {
            LRCFunction *cf = (LRCFunction *)fobj->extra;
            cf->data = rd;
        }
    }

    /* Create result object */
    LRValue result = lr_new_object(ctx);
    lr_set_property_str(ctx, result, "proxy", lr_dup_value(ctx, proxy));
    lr_set_property_str(ctx, result, "revoke", revoke_fn);

    lr_free_value(ctx, proxy);

    return result;
}

/* ── Proxy Constructor ─────────────────────────────────────────────────── */

static LRValue js_proxy_constructor(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;

    /* Check that we were called with new */
    /* (In this engine, we just check if we can construct) */

    LRValue target = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    LRValue handler = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    /* Validate target is an object */
    if (target.tag != LR_TYPE_OBJECT) {
        return lr_throw_type_error(ctx, "Proxy target must be an object");
    }

    /* Validate handler is an object */
    if (handler.tag != LR_TYPE_OBJECT) {
        return lr_throw_type_error(ctx, "Proxy handler must be an object");
    }

    return lr_new_proxy(ctx, target, handler);
}

/* ── Registration ──────────────────────────────────────────────────────── */

void lr_proxy_init(struct LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create Proxy constructor */
    JSValue proxy_ctor = JS_NewCFunction(ctx, js_proxy_constructor, "Proxy", 2);

    /* Add static methods */
    static const JSCFunctionListEntry proxy_static_methods[] = {
        JS_CFUNC_DEF("revocable", 2, js_proxy_revocable),
    };

    JS_SetPropertyFunctionList(ctx, proxy_ctor,
                                proxy_static_methods,
                                sizeof(proxy_static_methods) / sizeof(proxy_static_methods[0]));

    /* Register on global object */
    JS_SetPropertyStr(ctx, global, "Proxy", JS_DupValue(ctx, proxy_ctor));
    JS_FreeValue(ctx, proxy_ctor);
    JS_FreeValue(ctx, global);
}