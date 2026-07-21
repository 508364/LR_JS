/*
 * L/R_JS - Storage API (localStorage, sessionStorage)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include "lr_runtime.h"

/* ── Storage.getItem(key) ─────────────────────────────────────────────── */

static JSValue lr_storage_getItem(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JSValue val = JS_GetPropertyStr(ctx, data, key);
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);

    if (JS_IsUndefined(val)) {
        JS_FreeValue(ctx, val);
        return JS_NULL;
    }
    return val;
}

/* ── Storage.setItem(key, value) ──────────────────────────────────────── */

static JSValue lr_storage_setItem(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "Storage.setItem requires 2 arguments");

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    const char *value = JS_ToCString(ctx, argv[1]);
    if (!value) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JS_SetPropertyStr(ctx, data, key, JS_NewString(ctx, value));
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

/* ── Storage.removeItem(key) ──────────────────────────────────────────── */

static JSValue lr_storage_removeItem(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JS_DeleteProperty(ctx, data, JS_NewAtom(ctx, key), 0);
    JS_FreeValue(ctx, data);
    JS_FreeCString(ctx, key);
    return JS_UNDEFINED;
}

/* ── Storage.clear() ──────────────────────────────────────────────────── */

static JSValue lr_storage_clear(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JS_SetPropertyStr(ctx, this_val, "__data", JS_NewObject(ctx));
    return JS_UNDEFINED;
}

/* ── Storage.key(index) ───────────────────────────────────────────────── */

static JSValue lr_storage_key(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;

    int32_t index;
    if (JS_ToInt32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;

    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, data,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        if (index >= 0 && (uint32_t)index < len) {
            const char *key = JS_AtomToCString(ctx, tab[index].atom);
            JSValue result = JS_NewString(ctx, key ? key : "");
            if (key) JS_FreeCString(ctx, key);
            JS_FreePropertyEnum(ctx, tab, len);
            JS_FreeValue(ctx, data);
            return result;
        }
        JS_FreePropertyEnum(ctx, tab, len);
    }
    JS_FreeValue(ctx, data);
    return JS_NULL;
}

/* ── Storage.length getter ────────────────────────────────────────────── */

static JSValue lr_storage_length(JSContext *ctx, JSValueConst this_val)
{
    JSValue data = JS_GetPropertyStr(ctx, this_val, "__data");
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, data,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        JS_FreePropertyEnum(ctx, tab, len);
        JS_FreeValue(ctx, data);
        return JS_NewUint32(ctx, len);
    }
    JS_FreeValue(ctx, data);
    return JS_NewInt32(ctx, 0);
}

/* ── Storage function list ────────────────────────────────────────────── */

static const JSCFunctionListEntry lr_storage_funcs[] = {
    JS_CFUNC_DEF("getItem",    1, lr_storage_getItem),
    JS_CFUNC_DEF("setItem",    2, lr_storage_setItem),
    JS_CFUNC_DEF("removeItem", 1, lr_storage_removeItem),
    JS_CFUNC_DEF("clear",      0, lr_storage_clear),
    JS_CFUNC_DEF("key",        1, lr_storage_key),
    JS_CGETSET_DEF("length", lr_storage_length, NULL),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_storage_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create localStorage and sessionStorage as plain objects */
    JSValue localStorage = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, localStorage, lr_storage_funcs,
                                sizeof(lr_storage_funcs) / sizeof(lr_storage_funcs[0]));
    JS_SetPropertyStr(ctx, localStorage, "__data", JS_NewObject(ctx));

    JSValue sessionStorage = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sessionStorage, lr_storage_funcs,
                                sizeof(lr_storage_funcs) / sizeof(lr_storage_funcs[0]));
    JS_SetPropertyStr(ctx, sessionStorage, "__data", JS_NewObject(ctx));

    JS_SetPropertyStr(ctx, global, "localStorage", localStorage);
    JS_SetPropertyStr(ctx, global, "sessionStorage", sessionStorage);

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Storage API initialized");
}