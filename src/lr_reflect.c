/*
 * LR_JS - Reflect Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the Reflect object with static methods that correspond
 * to Proxy traps and other fundamental operations.
 */
#include "lr_reflect.h"
#include "lr_runtime.h"
#include "lr_proxy.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Forward Declarations ──────────────────────────────────────────────── */

static LRValue js_reflect_get(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static LRValue js_reflect_set(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static LRValue js_reflect_has(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static LRValue js_reflect_delete_property(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);
static LRValue js_reflect_own_keys(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv);
static LRValue js_reflect_apply(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
static LRValue js_reflect_construct(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
static LRValue js_reflect_define_property(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv);
static LRValue js_reflect_get_prototype_of(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static LRValue js_reflect_set_prototype_of(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static LRValue js_reflect_is_extensible(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

/* ── Reflect.get ───────────────────────────────────────────────────────── */

static LRValue js_reflect_get(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.get: target must be an object");
    }

    LRValue target = argv[0];
    LRValue prop = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;
    LRValue receiver = (argc > 2) ? argv[2] : target;

    /* If receiver is not specified, use target */
    if (argc < 3) {
        receiver = target;
    }

    /* Convert property key to string atom */
    LRString *atom = lr_to_atom(ctx, prop);

    /* Use the engine's property get, which handles Proxy traps */
    return lr_get_property(ctx, receiver, atom);
}

/* ── Reflect.set ───────────────────────────────────────────────────────── */

static LRValue js_reflect_set(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.set: target must be an object");
    }

    LRValue target = argv[0];
    LRValue prop = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;
    LRValue value = (argc > 2) ? argv[2] : LR_VALUE_UNDEFINED;
    LRValue receiver = (argc > 3) ? argv[3] : target;

    if (argc < 4) {
        receiver = target;
    }

    /* Convert property key to string atom */
    LRString *atom = lr_to_atom(ctx, prop);

    int result = lr_set_property(ctx, receiver, atom, lr_dup_value(ctx, value));
    return lr_new_bool(ctx, result == 0);
}

/* ── Reflect.has ───────────────────────────────────────────────────────── */

static LRValue js_reflect_has(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.has: target must be an object");
    }

    LRValue target = argv[0];
    LRValue prop = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    LRString *atom = lr_to_atom(ctx, prop);
    int result = lr_has_property(ctx, target, atom);
    return lr_new_bool(ctx, result);
}

/* ── Reflect.deleteProperty ────────────────────────────────────────────── */

static LRValue js_reflect_delete_property(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.deleteProperty: target must be an object");
    }

    LRValue target = argv[0];
    LRValue prop = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    LRString *atom = lr_to_atom(ctx, prop);
    int result = lr_delete_property(ctx, target, atom, 0);
    return lr_new_bool(ctx, result == 0);
}

/* ── Reflect.ownKeys ───────────────────────────────────────────────────── */

static LRValue js_reflect_own_keys(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.ownKeys: target must be an object");
    }

    LRValue target = argv[0];

    LRPropertyEnum *ptab;
    uint32_t plen;
    int ret = lr_get_own_property_names(ctx, &ptab, &plen, target, 0);
    if (ret != 0) {
        return LR_VALUE_UNDEFINED;
    }

    LRValue arr = lr_new_array(ctx);
    for (uint32_t i = 0; i < plen; i++) {
        LRValue key = lr_atom_to_value(ctx, ptab[i].atom);
        lr_set_property_uint32(ctx, arr, i, key);
        lr_free_value(ctx, key);
    }
    lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, (int32_t)plen));

    lr_free_property_enum(ctx, ptab, plen);

    return arr;
}

/* ── Reflect.apply ─────────────────────────────────────────────────────── */

static LRValue js_reflect_apply(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.apply: target must be a function");
    }

    LRValue target = argv[0];
    LRValue this_arg = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;
    LRValue args_arr = (argc > 2) ? argv[2] : LR_VALUE_UNDEFINED;

    /* Convert args array to C array */
    int arg_count = 0;
    LRValue *args = NULL;

    if (lr_is_object(args_arr)) {
        LRValue len_val = lr_get_property_direct(ctx, args_arr, lr_new_atom(ctx, "length"));
        lr_to_int32(ctx, &arg_count, len_val);
        lr_free_value(ctx, len_val);

        if (arg_count > 0) {
            args = (LRValue *)malloc(arg_count * sizeof(LRValue));
            for (int i = 0; i < arg_count; i++) {
                args[i] = lr_dup_value(ctx, lr_get_property_uint32(ctx, args_arr, i));
            }
        }
    }

    LRValue result = lr_call(ctx, target, this_arg, arg_count, args);

    for (int i = 0; i < arg_count; i++) {
        lr_free_value(ctx, args[i]);
    }
    free(args);

    return result;
}

/* ── Reflect.construct ─────────────────────────────────────────────────── */

static LRValue js_reflect_construct(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.construct: target must be a function");
    }

    LRValue target = argv[0];
    LRValue args_arr = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    /* Convert args array to C array */
    int arg_count = 0;
    LRValue *args = NULL;

    if (lr_is_object(args_arr)) {
        LRValue len_val = lr_get_property_direct(ctx, args_arr, lr_new_atom(ctx, "length"));
        lr_to_int32(ctx, &arg_count, len_val);
        lr_free_value(ctx, len_val);

        if (arg_count > 0) {
            args = (LRValue *)malloc(arg_count * sizeof(LRValue));
            for (int i = 0; i < arg_count; i++) {
                args[i] = lr_dup_value(ctx, lr_get_property_uint32(ctx, args_arr, i));
            }
        }
    }

    LRValue result = lr_call_constructor(ctx, target, arg_count, args);

    for (int i = 0; i < arg_count; i++) {
        lr_free_value(ctx, args[i]);
    }
    free(args);

    return result;
}

/* ── Reflect.defineProperty ────────────────────────────────────────────── */

static LRValue js_reflect_define_property(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.defineProperty: target must be an object");
    }

    LRValue target = argv[0];
    LRValue prop = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;
    LRValue attributes = (argc > 2) ? argv[2] : LR_VALUE_UNDEFINED;

    LRString *atom = lr_to_atom(ctx, prop);

    /* Simplified: just set the property value */
    LRValue value = LR_VALUE_UNDEFINED;
    if (lr_is_object(attributes)) {
        LRValue val_attr = lr_get_property_direct(ctx, attributes, lr_new_atom(ctx, "value"));
        if (!lr_is_undefined(val_attr)) {
            value = val_attr;
        } else {
            lr_free_value(ctx, val_attr);
        }
    }

    int result;
    if (!lr_is_undefined(value)) {
        result = lr_set_property(ctx, target, atom, value);
    } else {
        result = lr_set_property(ctx, target, atom, lr_dup_value(ctx, attributes));
    }

    return lr_new_bool(ctx, result == 0);
}

/* ── Reflect.getPrototypeOf ────────────────────────────────────────────── */

static LRValue js_reflect_get_prototype_of(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.getPrototypeOf: target must be an object");
    }

    LRValue target = argv[0];
    return lr_get_prototype(ctx, target);
}

/* ── Reflect.setPrototypeOf ────────────────────────────────────────────── */

static LRValue js_reflect_set_prototype_of(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.setPrototypeOf: target must be an object");
    }

    LRValue target = argv[0];
    LRValue proto = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    int result = lr_set_prototype(ctx, target, proto);
    return lr_new_bool(ctx, result == 0);
}

/* ── Reflect.isExtensible ──────────────────────────────────────────────── */

static LRValue js_reflect_is_extensible(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1) {
        return lr_throw_type_error(ctx, "Reflect.isExtensible: target must be an object");
    }

    LRValue target = argv[0];
    if (target.tag != LR_TYPE_OBJECT) {
        return lr_new_bool(ctx, 0);
    }

    LRObject *obj = (LRObject *)target.u.ptr;
    return lr_new_bool(ctx, obj->is_extensible);
}

/* ── Registration ──────────────────────────────────────────────────────── */

void lr_reflect_init(struct LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create Reflect object */
    JSValue reflect = JS_NewObject(ctx);

    /* Add all static methods */
    static const JSCFunctionListEntry reflect_methods[] = {
        JS_CFUNC_DEF("get", 2, js_reflect_get),
        JS_CFUNC_DEF("set", 3, js_reflect_set),
        JS_CFUNC_DEF("has", 2, js_reflect_has),
        JS_CFUNC_DEF("deleteProperty", 2, js_reflect_delete_property),
        JS_CFUNC_DEF("ownKeys", 1, js_reflect_own_keys),
        JS_CFUNC_DEF("apply", 3, js_reflect_apply),
        JS_CFUNC_DEF("construct", 2, js_reflect_construct),
        JS_CFUNC_DEF("defineProperty", 3, js_reflect_define_property),
        JS_CFUNC_DEF("getPrototypeOf", 1, js_reflect_get_prototype_of),
        JS_CFUNC_DEF("setPrototypeOf", 2, js_reflect_set_prototype_of),
        JS_CFUNC_DEF("isExtensible", 1, js_reflect_is_extensible),
    };

    JS_SetPropertyFunctionList(ctx, reflect,
                                reflect_methods,
                                sizeof(reflect_methods) / sizeof(reflect_methods[0]));

    /* Register on global object */
    JS_SetPropertyStr(ctx, global, "Reflect", JS_DupValue(ctx, reflect));

    JS_FreeValue(ctx, reflect);
    JS_FreeValue(ctx, global);
}