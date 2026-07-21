/*
 * L/R_JS - Event API (EventTarget, Event, CustomEvent, AbortController, AbortSignal)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include "lr_runtime.h"

/* ── EventTarget API ──────────────────────────────────────────────────── */

static JSValue lr_event_target_addEventListener(JSContext *ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "addEventListener requires 2 arguments");
    if (!JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "addEventListener: listener must be a function");
    }

    int once = 0;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue once_val = JS_GetPropertyStr(ctx, argv[2], "once");
        if (!JS_IsUndefined(once_val)) once = JS_ToBool(ctx, once_val);
        JS_FreeValue(ctx, once_val);
    }

    /* Store in __listeners array */
    JSValue listeners = JS_GetPropertyStr(ctx, this_val, "__listeners");
    if (JS_IsUndefined(listeners)) {
        JS_FreeValue(ctx, listeners);
        listeners = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__listeners", listeners);
    }

    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "callback", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyStr(ctx, item, "once", JS_NewBool(ctx, once));

    /* Append to array */
    JSValue push = JS_GetPropertyStr(ctx, listeners, "push");
    if (!JS_IsUndefined(push)) {
        JS_Call(ctx, push, listeners, 1, (JSValueConst *)&item);
    }
    JS_FreeValue(ctx, push);
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, listeners);

    return JS_UNDEFINED;
}

static JSValue lr_event_target_removeEventListener(JSContext *ctx,
                                                     JSValueConst this_val,
                                                     int argc, JSValueConst *argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_UNDEFINED;
    }

    JSValue listeners = JS_GetPropertyStr(ctx, this_val, "__listeners");
    if (JS_IsUndefined(listeners)) {
        JS_FreeValue(ctx, listeners);
        return JS_UNDEFINED;
    }

    /* Get current length */
    JSValue length_val = JS_GetPropertyStr(ctx, listeners, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, length_val);
    JS_FreeValue(ctx, length_val);

    /* Build a new filtered array */
    JSValue new_listeners = JS_NewArray(ctx);

    for (int32_t i = 0; i < len; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, listeners, (uint32_t)i);
        JSValue cb = JS_GetPropertyStr(ctx, item, "callback");

        int match = 0;
        if (JS_IsFunction(ctx, cb) && JS_IsFunction(ctx, argv[1])) {
            const char *cb_str = JS_ToCString(ctx, cb);
            const char *arg_str = JS_ToCString(ctx, argv[1]);
            if (cb_str && arg_str && strcmp(cb_str, arg_str) == 0) match = 1;
            if (cb_str) JS_FreeCString(ctx, cb_str);
            if (arg_str) JS_FreeCString(ctx, arg_str);
        }

        if (!match) {
            JS_SetPropertyUint32(ctx, new_listeners, (uint32_t)i, JS_DupValue(ctx, item));
        }

        JS_FreeValue(ctx, cb);
        JS_FreeValue(ctx, item);
    }

    JS_FreeValue(ctx, listeners);
    /* Don't free new_listeners - JS_SetPropertyStr takes ownership on existing prop */
    JS_SetPropertyStr(ctx, this_val, "__listeners", new_listeners);

    return JS_UNDEFINED;
}

static JSValue lr_event_target_dispatchEvent(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "dispatchEvent requires 1 argument");

    JSValue listeners = JS_GetPropertyStr(ctx, this_val, "__listeners");
    if (JS_IsUndefined(listeners)) {
        JS_FreeValue(ctx, listeners);
        return JS_NewBool(ctx, 1);
    }

    JSValue length = JS_GetPropertyStr(ctx, listeners, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, length);
    JS_FreeValue(ctx, length);

    JSValue event = argv[0];
    JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, this_val));

    for (int32_t i = 0; i < len; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, listeners, i);
        JSValue cb = JS_GetPropertyStr(ctx, item, "callback");
        JSValue once_val = JS_GetPropertyStr(ctx, item, "once");

        if (JS_IsFunction(ctx, cb)) {
            JSValue ret = JS_Call(ctx, cb, this_val, 1, &event);
            if (JS_IsException(ret)) {
                JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, cb);
                JS_FreeValue(ctx, once_val);
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, listeners);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, ret);
        }

        JS_FreeValue(ctx, cb);
        JS_FreeValue(ctx, once_val);
        JS_FreeValue(ctx, item);
    }

    JS_FreeValue(ctx, listeners);
    return JS_NewBool(ctx, 1);
}

/* ── Event constructor ────────────────────────────────────────────────── */

static JSValue lr_event_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);

    if (argc >= 1) {
        const char *type = JS_ToCString(ctx, argv[0]);
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, type ? type : ""));
        if (type) JS_FreeCString(ctx, type);
    } else {
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, ""));
    }

    JS_SetPropertyStr(ctx, obj, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "cancelBubble", JS_FALSE);
    JS_SetPropertyStr(ctx, obj, "returnValue", JS_TRUE);
    JS_SetPropertyStr(ctx, obj, "timeStamp", JS_NewFloat64(ctx, 0));

    /* Initialize listener array */
    JS_SetPropertyStr(ctx, obj, "__listeners", JS_NewArray(ctx));

    return obj;
}

/* ── Event prototype methods ──────────────────────────────────────────── */

static JSValue lr_event_preventDefault(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue lr_event_stopPropagation(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JS_SetPropertyStr(ctx, this_val, "cancelBubble", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue lr_event_stopImmediatePropagation(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    JS_SetPropertyStr(ctx, this_val, "cancelBubble", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "__stopImmediate", JS_TRUE);
    return JS_UNDEFINED;
}

/* ── CustomEvent ──────────────────────────────────────────────────────── */

static JSValue lr_custom_event_constructor(JSContext *ctx, JSValueConst new_target,
                                            int argc, JSValueConst *argv)
{
    JSValue obj = lr_event_constructor(ctx, new_target, argc, argv);

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue detail = JS_GetPropertyStr(ctx, argv[1], "detail");
        if (!JS_IsUndefined(detail)) {
            JS_SetPropertyStr(ctx, obj, "detail", detail);
        } else {
            JS_FreeValue(ctx, detail);
        }
    }

    return obj;
}

/* ── AbortController / AbortSignal ────────────────────────────────────── */

static JSValue lr_abort_controller_constructor(JSContext *ctx, JSValueConst new_target,
                                                int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    JSValue signal = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, signal, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, signal, "reason", JS_UNDEFINED);

    JS_SetPropertyStr(ctx, obj, "signal", signal);
    return obj;
}

static JSValue lr_abort_controller_abort(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JSValue signal = JS_GetPropertyStr(ctx, this_val, "signal");
    JS_SetPropertyStr(ctx, signal, "aborted", JS_TRUE);

    JSValue reason = argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_NewString(ctx, "Aborted");
    JS_SetPropertyStr(ctx, signal, "reason", reason);
    JS_FreeValue(ctx, reason);
    JS_FreeValue(ctx, signal);
    return JS_UNDEFINED;
}

/* ── EventTarget function list ────────────────────────────────────────── */

static const JSCFunctionListEntry lr_event_target_funcs[] = {
    JS_CFUNC_DEF("addEventListener",    2, lr_event_target_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, lr_event_target_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent",       1, lr_event_target_dispatchEvent),
};

static const JSCFunctionListEntry lr_event_proto_funcs[] = {
    JS_CFUNC_DEF("preventDefault",            0, lr_event_preventDefault),
    JS_CFUNC_DEF("stopPropagation",           0, lr_event_stopPropagation),
    JS_CFUNC_DEF("stopImmediatePropagation",  0, lr_event_stopImmediatePropagation),
};

static const JSCFunctionListEntry lr_abort_controller_funcs[] = {
    JS_CFUNC_DEF("abort", 0, lr_abort_controller_abort),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_event_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Event constructor */
    JSValue event_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, event_proto, lr_event_target_funcs,
                                sizeof(lr_event_target_funcs) / sizeof(lr_event_target_funcs[0]));
    JS_SetPropertyFunctionList(ctx, event_proto, lr_event_proto_funcs,
                                sizeof(lr_event_proto_funcs) / sizeof(lr_event_proto_funcs[0]));

    JSValue event_ctor = JS_NewCFunction2(ctx, lr_event_constructor, "Event", 1,
                                           JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, event_ctor, "prototype", JS_DupValue(ctx, event_proto));
    JS_FreeValue(ctx, event_proto);
    JS_SetPropertyStr(ctx, global, "Event", event_ctor);

    /* EventTarget constructor */
    JSValue target_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, target_proto, lr_event_target_funcs,
                                sizeof(lr_event_target_funcs) / sizeof(lr_event_target_funcs[0]));
    JSValue target_ctor = JS_NewCFunction2(ctx, lr_event_constructor, "EventTarget", 0,
                                            JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, target_ctor, "prototype", JS_DupValue(ctx, target_proto));
    JS_FreeValue(ctx, target_proto);
    JS_SetPropertyStr(ctx, global, "EventTarget", target_ctor);

    /* CustomEvent */
    JS_SetPropertyStr(ctx, global, "CustomEvent",
                      JS_NewCFunction2(ctx, lr_custom_event_constructor, "CustomEvent", 1,
                                       JS_CFUNC_constructor, 0));

    /* AbortController */
    JSValue abort_ctl = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, abort_ctl, lr_abort_controller_funcs,
                                sizeof(lr_abort_controller_funcs) / sizeof(lr_abort_controller_funcs[0]));
    JSValue abort_ctor = JS_NewCFunction2(ctx, lr_abort_controller_constructor, "AbortController", 0,
                                           JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, abort_ctor, "prototype", JS_DupValue(ctx, abort_ctl));
    JS_FreeValue(ctx, abort_ctl);
    JS_SetPropertyStr(ctx, global, "AbortController", abort_ctor);

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Event API initialized");
}