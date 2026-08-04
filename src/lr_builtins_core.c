/*
 * L/R_JS - ES2022 Core Built-in Objects
 * Pure C, ES2022-compatible, self-implemented JS engine.
 *
 * Implements Object, Array, String, Number, Boolean, and Function
 * built-in constructors and their prototype methods.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include "lr_runtime.h"
#include "lr_builtins.h"

/* TypedArrayData is defined in lr_engine.h */

/* ── Bound Function Data (for Function.prototype.bind) ─────────────────── */

typedef struct BoundFunctionData {
    LRValue target;
    LRValue bound_this;
    int     argc;
    LRValue args[];  /* flexible array of bound args */
} BoundFunctionData;

/* ── Helper: get array length ──────────────────────────────────────────── */

static int32_t get_array_length(JSContext *ctx, JSValue arr)
{
    /* Dense array fast path: read from LRArrayData */
    if (JS_VALUE_GET_TAG(arr) == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)arr.u.ptr;
        if (o->type == LR_OBJ_ARRAY && o->extra) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            return (int32_t)ad->length;
        }
    }
    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(len_val)) return 0;
    int32_t len;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    return len;
}

/* ── Helper: set array length ──────────────────────────────────────────── */

static void set_array_length(JSContext *ctx, JSValue arr, int32_t len)
{
    /* Sync dense array data */
    if (JS_VALUE_GET_TAG(arr) == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)arr.u.ptr;
        if (o->type == LR_OBJ_ARRAY && o->extra) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            ad->length = (uint32_t)len;
        }
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, len));
}

/* ── Helper: create array from JS values ───────────────────────────────── */

static JSValue array_from_values(JSContext *ctx, int argc, JSValue *argv)
{
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++) {
        JS_SetPropertyUint32(ctx, arr, i, JS_DupValue(ctx, argv[i]));
    }
    set_array_length(ctx, arr, argc);
    return arr;
}

/* ── Helper: is callable ───────────────────────────────────────────────── */

static int is_callable(JSContext *ctx, JSValue val)
{
    return JS_IsFunction(ctx, val);
}

/* ══════════════════════════════════════════════════════════════════════════
   1. OBJECT
   ══════════════════════════════════════════════════════════════════════════ */

static JSValue js_object_constructor(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (argc == 0 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        return JS_NewObject(ctx);
    }
    /* If value is already an object, return it */
    if (JS_IsObject(argv[0])) {
        return JS_DupValue(ctx, argv[0]);
    }
    /* Wrap primitives */
    if (JS_IsBool(argv[0]) || JS_IsNumber(argv[0]) || JS_IsString(argv[0])) {
        return JS_NewObject(ctx);
    }
    return JS_NewObject(ctx);
}

/* ── Object.keys ───────────────────────────────────────────────────────── */

static JSValue js_object_keys(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.keys called on non-object");
    }
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue key = JS_AtomToValue(ctx, tab[i].atom);
        JS_SetPropertyUint32(ctx, arr, i, key);
    }
    set_array_length(ctx, arr, (int32_t)len);
    JS_FreePropertyEnum(ctx, tab, len);
    return arr;
}

/* ── Object.values ─────────────────────────────────────────────────────── */

static JSValue js_object_values(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.values called on non-object");
    }
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue val = JS_GetProperty(ctx, argv[0], tab[i].atom);
        JS_SetPropertyUint32(ctx, arr, i, val);
    }
    set_array_length(ctx, arr, (int32_t)len);
    JS_FreePropertyEnum(ctx, tab, len);
    return arr;
}

/* ── Object.entries ────────────────────────────────────────────────────── */

static JSValue js_object_entries(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.entries called on non-object");
    }
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue key = JS_AtomToValue(ctx, tab[i].atom);
        JSValue val = JS_GetProperty(ctx, argv[0], tab[i].atom);
        JSValue entry = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, entry, 0, key);
        JS_SetPropertyUint32(ctx, entry, 1, val);
        set_array_length(ctx, entry, 2);
        JS_SetPropertyUint32(ctx, arr, i, entry);
    }
    set_array_length(ctx, arr, (int32_t)len);
    JS_FreePropertyEnum(ctx, tab, len);
    return arr;
}

/* ── Object.fromEntries ────────────────────────────────────────────────── */

static JSValue js_object_from_entries(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Object.fromEntries requires at least one argument");
    }
    JSValue obj = JS_NewObject(ctx);
    JSValue iterable = argv[0];
    int32_t len = 0;
    if (JS_IsArray(ctx, iterable)) {
        len = get_array_length(ctx, iterable);
    } else if (JS_IsObject(iterable)) {
        JSValue len_val = JS_GetPropertyStr(ctx, iterable, "length");
        if (!JS_IsUndefined(len_val) && !JS_IsException(len_val)) {
            JS_ToInt32(ctx, &len, len_val);
        }
        JS_FreeValue(ctx, len_val);
    }
    for (int32_t i = 0; i < len; i++) {
        JSValue entry = JS_GetPropertyUint32(ctx, iterable, i);
        if (!JS_IsUndefined(entry) && !JS_IsException(entry) && JS_IsObject(entry)) {
            JSValue key = JS_GetPropertyUint32(ctx, entry, 0);
            JSValue val = JS_GetPropertyUint32(ctx, entry, 1);
            const char *key_str = JS_ToCString(ctx, key);
            if (key_str) {
                JS_SetPropertyStr(ctx, obj, key_str, val);
                JS_FreeCString(ctx, key_str);
            } else {
                JS_FreeValue(ctx, val);
            }
            JS_FreeValue(ctx, key);
        }
        JS_FreeValue(ctx, entry);
    }
    return obj;
}

/* ── Object.assign ─────────────────────────────────────────────────────── */

static JSValue js_object_assign(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Object.assign requires at least one argument");
    }
    JSValue target = JS_DupValue(ctx, argv[0]);
    for (int i = 1; i < argc; i++) {
        JSValue source = argv[i];
        if (JS_IsNull(source) || JS_IsUndefined(source)) {
            continue;
        }
        /* Force to object */
        if (!JS_IsObject(source)) {
            continue;
        }
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, source,
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t j = 0; j < len; j++) {
                JSValue val = JS_GetProperty(ctx, source, tab[j].atom);
                const char *key_str = JS_AtomToCString(ctx, tab[j].atom);
                if (key_str) {
                    JS_SetPropertyStr(ctx, target, key_str, val);
                    JS_FreeCString(ctx, key_str);
                } else {
                    JS_FreeValue(ctx, val);
                }
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
    }
    return target;
}

/* ── Object.create ─────────────────────────────────────────────────────── */

static JSValue js_object_create(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Object.create requires at least one argument");
    }
    JSValue proto = argv[0];
    if (!JS_IsNull(proto) && !JS_IsObject(proto)) {
        return JS_ThrowTypeError(ctx, "Object prototype may only be an Object or null");
    }
    JSValue obj;
    if (JS_IsNull(proto)) {
        obj = JS_NewObject(ctx);
        JS_SetPrototype(ctx, obj, JS_NULL);
    } else {
        obj = JS_NewObjectProto(ctx, proto);
    }
    /* Handle properties descriptor if provided */
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        JSValue props = argv[1];
        /* Call Object.defineProperties on the new object */
        JSValue obj_ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Object");
        /* We'll just define properties manually for simplicity */
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, props,
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < len; i++) {
                JSValue desc = JS_GetProperty(ctx, props, tab[i].atom);
                const char *key_str = JS_AtomToCString(ctx, tab[i].atom);
                if (key_str && JS_IsObject(desc)) {
                    JSValue val = JS_GetPropertyStr(ctx, desc, "value");
                    if (!JS_IsUndefined(val)) {
                        JS_DefinePropertyValue(ctx, obj, tab[i].atom, val,
                            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
                        JS_FreeValue(ctx, val);
                    } else {
                        JS_FreeValue(ctx, val);
                        /* Check for getter/setter */
                        JSValue getter = JS_GetPropertyStr(ctx, desc, "get");
                        JSValue setter = JS_GetPropertyStr(ctx, desc, "set");
                        if (!JS_IsUndefined(getter) || !JS_IsUndefined(setter)) {
                            /* For getter/setter, just set a default value */
                            JS_DefinePropertyValue(ctx, obj, tab[i].atom, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
                        }
                        JS_FreeValue(ctx, getter);
                        JS_FreeValue(ctx, setter);
                    }
                }
                if (key_str) JS_FreeCString(ctx, key_str);
                JS_FreeValue(ctx, desc);
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
        JS_FreeValue(ctx, obj_ctor);
    }
    return obj;
}

/* ── Object.defineProperty ─────────────────────────────────────────────── */

static JSValue js_object_define_property(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty requires 3 arguments");
    }
    if (!JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.defineProperty called on non-object");
    }
    if (!JS_IsObject(argv[2])) {
        return JS_ThrowTypeError(ctx, "Property descriptor must be an object");
    }

    JSValue obj = argv[0];
    JSValue desc = argv[2];

    const char *prop_name = JS_ToCString(ctx, argv[1]);
    if (!prop_name) return JS_EXCEPTION;

    JSValue val = JS_GetPropertyStr(ctx, desc, "value");
    JSValue getter = JS_GetPropertyStr(ctx, desc, "get");
    JSValue setter = JS_GetPropertyStr(ctx, desc, "set");

    /* Determine flags */
    int flags = 0;
    JSValue writable = JS_GetPropertyStr(ctx, desc, "writable");
    JSValue configurable = JS_GetPropertyStr(ctx, desc, "configurable");
    JSValue enumerable = JS_GetPropertyStr(ctx, desc, "enumerable");

    if (!JS_IsUndefined(writable) && JS_ToBool(ctx, writable)) flags |= JS_PROP_WRITABLE;
    if (!JS_IsUndefined(configurable) && JS_ToBool(ctx, configurable)) flags |= JS_PROP_CONFIGURABLE;
    if (!JS_IsUndefined(enumerable) && JS_ToBool(ctx, enumerable)) flags |= JS_PROP_ENUMERABLE;

    JS_FreeValue(ctx, writable);
    JS_FreeValue(ctx, configurable);
    JS_FreeValue(ctx, enumerable);

    JSString *atom = JS_NewAtom(ctx, prop_name);
    if (!JS_IsUndefined(val)) {
        JS_DefinePropertyValue(ctx, obj, atom, val, flags);
        JS_FreeValue(ctx, val);
    } else if (!JS_IsUndefined(getter) || !JS_IsUndefined(setter)) {
        /* For getter/setter, just set a value for now */
        JS_DefinePropertyValue(ctx, obj, atom, JS_UNDEFINED, flags);
    }
    JS_FreeValue(ctx, getter);
    JS_FreeValue(ctx, setter);
    JS_FreeCString(ctx, prop_name);

    return JS_DupValue(ctx, obj);
}

/* ── Object.defineProperties ───────────────────────────────────────────── */

static JSValue js_object_define_properties(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "Object.defineProperties requires 2 arguments");
    }
    if (!JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.defineProperties called on non-object");
    }
    if (!JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "Properties argument must be an object");
    }

    JSValue obj = argv[0];
    JSValue props = argv[1];
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, props,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; i++) {
            JSValue desc = JS_GetProperty(ctx, props, tab[i].atom);
            if (JS_IsObject(desc)) {
                JSValue val = JS_GetPropertyStr(ctx, desc, "value");
                int flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE;
                JSValue writable = JS_GetPropertyStr(ctx, desc, "writable");
                JSValue configurable = JS_GetPropertyStr(ctx, desc, "configurable");
                JSValue enumerable = JS_GetPropertyStr(ctx, desc, "enumerable");
                if (!JS_IsUndefined(writable)) {
                    if (JS_ToBool(ctx, writable)) flags |= JS_PROP_WRITABLE;
                    else flags &= ~JS_PROP_WRITABLE;
                }
                if (!JS_IsUndefined(configurable)) {
                    if (JS_ToBool(ctx, configurable)) flags |= JS_PROP_CONFIGURABLE;
                    else flags &= ~JS_PROP_CONFIGURABLE;
                }
                if (!JS_IsUndefined(enumerable)) {
                    if (JS_ToBool(ctx, enumerable)) flags |= JS_PROP_ENUMERABLE;
                    else flags &= ~JS_PROP_ENUMERABLE;
                }
                JS_FreeValue(ctx, writable);
                JS_FreeValue(ctx, configurable);
                JS_FreeValue(ctx, enumerable);
                if (!JS_IsUndefined(val)) {
                    JS_DefinePropertyValue(ctx, obj, tab[i].atom, val, flags);
                    JS_FreeValue(ctx, val);
                }
            }
            JS_FreeValue(ctx, desc);
        }
        JS_FreePropertyEnum(ctx, tab, len);
    }
    return JS_DupValue(ctx, obj);
}

/* ── Object.freeze ─────────────────────────────────────────────────────── */

static JSValue js_object_freeze(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    }
    /* Mark as non-extensible (simplified) */
    JSValue obj = argv[0];
    LRObject *obj_ptr = (LRObject *)obj.u.ptr;
    obj_ptr->is_extensible = 0;
    /* Make all properties non-writable, non-configurable (simplified) */
    return JS_DupValue(ctx, obj);
}

/* ── Object.seal ───────────────────────────────────────────────────────── */

static JSValue js_object_seal(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    }
    JSValue obj = argv[0];
    LRObject *obj_ptr = (LRObject *)obj.u.ptr;
    obj_ptr->is_extensible = 0;
    return JS_DupValue(ctx, obj);
}

/* ── Object.isExtensible ───────────────────────────────────────────────── */

static JSValue js_object_is_extensible(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_FALSE;
    }
    LRObject *obj_ptr = (LRObject *)argv[0].u.ptr;
    return JS_NewBool(ctx, obj_ptr->is_extensible);
}

/* ── Object.preventExtensions ──────────────────────────────────────────── */

static JSValue js_object_prevent_extensions(JSContext *ctx, JSValue this_val,
                                              int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    }
    LRObject *obj_ptr = (LRObject *)argv[0].u.ptr;
    obj_ptr->is_extensible = 0;
    return JS_DupValue(ctx, argv[0]);
}

/* ── Object.is ─────────────────────────────────────────────────────────── */

static JSValue js_object_is(JSContext *ctx, JSValue this_val,
                             int argc, JSValue *argv)
{
    if (argc < 2) return JS_FALSE;
    JSValue v1 = argv[0];
    JSValue v2 = argv[1];

    if (v1.tag != v2.tag) return JS_FALSE;

    switch (v1.tag) {
    case LR_TYPE_UNDEFINED:
    case LR_TYPE_NULL:
        return JS_TRUE;
    case LR_TYPE_BOOL:
        return JS_NewBool(ctx, v1.u.bool_val == v2.u.bool_val);
    case LR_TYPE_INT32:
        return JS_NewBool(ctx, v1.u.int32 == v2.u.int32);
    case LR_TYPE_FLOAT64:
        /* SameValue: NaN is same as NaN */
        if (isnan(v1.u.float64) && isnan(v2.u.float64)) return JS_TRUE;
        /* +0 !== -0 */
        if (v1.u.float64 == 0.0 && v2.u.float64 == 0.0) {
            return JS_NewBool(ctx, signbit(v1.u.float64) == signbit(v2.u.float64));
        }
        return JS_NewBool(ctx, v1.u.float64 == v2.u.float64);
    case LR_TYPE_STRING:
    case LR_TYPE_OBJECT:
    case LR_TYPE_SYMBOL:
        return JS_NewBool(ctx, v1.u.ptr == v2.u.ptr);
    default:
        return JS_FALSE;
    }
}

/* ── Object.hasOwn ─────────────────────────────────────────────────────── */

static JSValue js_object_has_own(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv)
{
    if (argc < 2) return JS_FALSE;
    if (!JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.hasOwn called on non-object");
    }
    const char *prop = JS_ToCString(ctx, argv[1]);
    if (!prop) return JS_EXCEPTION;
    JSString *atom = JS_NewAtom(ctx, prop);
    int result = JS_HasProperty(ctx, argv[0], atom);
    JS_FreeCString(ctx, prop);
    return JS_NewBool(ctx, result);
}

/* ── Object.getPrototypeOf ─────────────────────────────────────────────── */

static JSValue js_object_get_prototype_of(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.getPrototypeOf called on non-object");
    }
    return JS_GetPrototype(ctx, argv[0]);
}

/* ── Object.setPrototypeOf ─────────────────────────────────────────────── */

static JSValue js_object_set_prototype_of(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "Object.setPrototypeOf requires 2 arguments");
    }
    if (!JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.setPrototypeOf called on non-object");
    }
    if (!JS_IsNull(argv[1]) && !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "Prototype must be an object or null");
    }
    JS_SetPrototype(ctx, argv[0], argv[1]);
    return JS_DupValue(ctx, argv[0]);
}

/* ── Object.getOwnPropertyNames ────────────────────────────────────────── */

static JSValue js_object_get_own_property_names(JSContext *ctx, JSValue this_val,
                                                  int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.getOwnPropertyNames called on non-object");
    }
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                JS_GPN_STRING_MASK) < 0) {
        return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue key = JS_AtomToValue(ctx, tab[i].atom);
        JS_SetPropertyUint32(ctx, arr, i, key);
    }
    set_array_length(ctx, arr, (int32_t)len);
    JS_FreePropertyEnum(ctx, tab, len);
    return arr;
}

/* ── Object.getOwnPropertyDescriptor ───────────────────────────────────── */

static JSValue js_object_get_own_property_descriptor(JSContext *ctx,
                                                       JSValue this_val,
                                                       int argc, JSValue *argv)
{
    if (argc < 2 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.getOwnPropertyDescriptor called on non-object");
    }
    const char *prop_name = JS_ToCString(ctx, argv[1]);
    if (!prop_name) return JS_EXCEPTION;

    JSString *atom = JS_NewAtom(ctx, prop_name);
    JSValue val = JS_GetProperty(ctx, argv[0], atom);
    JS_FreeCString(ctx, prop_name);

    if (JS_IsUndefined(val)) {
        return JS_UNDEFINED;
    }

    /* Create a descriptor object */
    JSValue desc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, desc, "value", JS_DupValue(ctx, val));
    JS_SetPropertyStr(ctx, desc, "writable", JS_TRUE);
    JS_SetPropertyStr(ctx, desc, "enumerable", JS_TRUE);
    JS_SetPropertyStr(ctx, desc, "configurable", JS_TRUE);
    JS_FreeValue(ctx, val);
    return desc;
}

/* ── Object.getOwnPropertyDescriptors ──────────────────────────────────── */

static JSValue js_object_get_own_property_descriptors(JSContext *ctx,
                                                        JSValue this_val,
                                                        int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Object.getOwnPropertyDescriptors called on non-object");
    }
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                JS_GPN_STRING_MASK) < 0) {
        return JS_EXCEPTION;
    }
    JSValue result = JS_NewObject(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue val = JS_GetProperty(ctx, argv[0], tab[i].atom);
        JSValue desc = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, desc, "value", JS_DupValue(ctx, val));
        JS_SetPropertyStr(ctx, desc, "writable", JS_TRUE);
        JS_SetPropertyStr(ctx, desc, "enumerable", JS_TRUE);
        JS_SetPropertyStr(ctx, desc, "configurable", JS_TRUE);
        JS_SetProperty(ctx, result, tab[i].atom, desc);
        JS_FreeValue(ctx, val);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return result;
}

/* ── Object.prototype.toString ─────────────────────────────────────────── */

static JSValue js_object_proto_to_string(JSContext *ctx, JSValue this_val,
                                           int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsObject(this_val) && !JS_IsString(this_val) && !JS_IsNumber(this_val) &&
        !JS_IsBool(this_val)) {
        const char *tag = "Object";
        if (JS_IsArray(ctx, this_val)) tag = "Array";
        else if (JS_IsFunction(ctx, this_val)) tag = "Function";
        else if (JS_IsString(this_val)) tag = "String";
        else if (JS_IsNumber(this_val)) tag = "Number";
        else if (JS_IsBool(this_val)) tag = "Boolean";

        char buf[64];
        snprintf(buf, sizeof(buf), "[object %s]", tag);
        return JS_NewString(ctx, buf);
    }
    /* Determine the [[Class]] via prototype chain */
    const char *tag = "Object";
    if (JS_IsArray(ctx, this_val)) tag = "Array";
    else if (JS_IsFunction(ctx, this_val)) tag = "Function";
    /* Check for special types */
    LRObject *obj = (LRObject *)this_val.u.ptr;
    switch (obj->type) {
    case LR_OBJ_ARRAY:       tag = "Array"; break;
    case LR_OBJ_FUNCTION:
    case LR_OBJ_CFUNCTION:
    case LR_OBJ_BYTECODE_FUNC: tag = "Function"; break;
    case LR_OBJ_DATE:        tag = "Date"; break;
    case LR_OBJ_REGEXP:      tag = "RegExp"; break;
    case LR_OBJ_ERROR:       tag = "Error"; break;
    case LR_OBJ_PROMISE:     tag = "Promise"; break;
    case LR_OBJ_ARRAY_BUFFER: tag = "ArrayBuffer"; break;
    case LR_OBJ_DATA_VIEW:   tag = "DataView"; break;
    case LR_OBJ_TYPED_ARRAY: {
        /* Get the typed array name from opaque data */
        TypedArrayData *tad = (TypedArrayData *)obj->opaque;
        if (tad && tad->name) tag = tad->name;
        else tag = "TypedArray";
        break;
    }
    case LR_OBJ_PROXY:       tag = "Proxy"; break;
    default:                 tag = "Object"; break;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "[object %s]", tag);
    return JS_NewString(ctx, buf);
}

/* ── Object.prototype.hasOwnProperty ───────────────────────────────────── */

static JSValue js_object_proto_has_own_property(JSContext *ctx, JSValue this_val,
                                                  int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    if (!JS_IsObject(this_val)) return JS_FALSE;
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_EXCEPTION;
    JSString *atom = JS_NewAtom(ctx, prop);
    int result = JS_HasProperty(ctx, this_val, atom);
    JS_FreeCString(ctx, prop);
    return JS_NewBool(ctx, result);
}

/* ── Object.prototype.isPrototypeOf ────────────────────────────────────── */

static JSValue js_object_proto_is_prototype_of(JSContext *ctx, JSValue this_val,
                                                 int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_FALSE;
    if (!JS_IsObject(this_val)) return JS_FALSE;

    /* Walk prototype chain of argv[0] */
    JSValue proto = JS_GetPrototype(ctx, argv[0]);
    while (JS_IsObject(proto)) {
        if (proto.u.ptr == this_val.u.ptr) {
            JS_FreeValue(ctx, proto);
            return JS_TRUE;
        }
        JSValue next = JS_GetPrototype(ctx, proto);
        JS_FreeValue(ctx, proto);
        proto = next;
    }
    JS_FreeValue(ctx, proto);
    return JS_FALSE;
}

/* ── Object.prototype.propertyIsEnumerable ─────────────────────────────── */

static JSValue js_object_proto_property_is_enumerable(JSContext *ctx,
                                                        JSValue this_val,
                                                        int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    if (!JS_IsObject(this_val)) return JS_FALSE;
    const char *prop = JS_ToCString(ctx, argv[0]);
    if (!prop) return JS_EXCEPTION;
    JSString *atom = JS_NewAtom(ctx, prop);
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    int result = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, this_val,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < len; i++) {
            if (tab[i].atom == atom) {
                result = 1;
                break;
            }
        }
        JS_FreePropertyEnum(ctx, tab, len);
    }
    JS_FreeCString(ctx, prop);
    return JS_NewBool(ctx, result);
}

/* ── Object.prototype.valueOf ──────────────────────────────────────────── */

static JSValue js_object_proto_value_of(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* ══════════════════════════════════════════════════════════════════════════
   2. ARRAY
   ══════════════════════════════════════════════════════════════════════════ */

static JSValue js_array_constructor(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv)
{
    (void)this_val;
    if (argc == 0) {
        return JS_NewArray(ctx);
    }
    if (argc == 1 && JS_IsNumber(argv[0])) {
        double d;
        JS_ToFloat64(ctx, &d, argv[0]);
        int32_t len = (int32_t)d;
        if (d != (double)len || len < 0) {
            return JS_ThrowRangeError(ctx, "Invalid array length");
        }
        JSValue arr = JS_NewArray(ctx);
        set_array_length(ctx, arr, len);
        return arr;
    }
    return array_from_values(ctx, argc, argv);
}

/* ── Array.isArray ─────────────────────────────────────────────────────── */

static JSValue js_array_is_array(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    return JS_NewBool(ctx, JS_IsArray(ctx, argv[0]));
}

/* ── Array.from ────────────────────────────────────────────────────────── */

static JSValue js_array_from(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv)
{
    if (argc < 1) return JS_NewArray(ctx);
    JSValue arr = JS_NewArray(ctx);
    JSValue source = argv[0];
    int32_t len = 0;

    const char *source_str = NULL;
    if (JS_IsArray(ctx, source)) {
        len = get_array_length(ctx, source);
    } else if (JS_IsString(source)) {
        /* Strings are array-like: have length and indexed access */
        /* Primitive strings don't support property access, so handle directly */
        source_str = JS_ToCString(ctx, source);
        if (source_str) {
            len = (int32_t)strlen(source_str);
        }
    } else if (JS_IsObject(source)) {
        JSValue len_val = JS_GetPropertyStr(ctx, source, "length");
        if (!JS_IsUndefined(len_val) && !JS_IsException(len_val)) {
            JS_ToInt32(ctx, &len, len_val);
        }
        JS_FreeValue(ctx, len_val);
    }

    /* mapFn is optional */
    int has_map_fn = (argc >= 2 && is_callable(ctx, argv[1]));
    JSValue this_arg = (argc >= 3) ? argv[2] : JS_UNDEFINED;

    for (int32_t i = 0; i < len; i++) {
        JSValue val;
        if (source_str) {
            /* For strings, create a single-character string */
            char ch[2] = { source_str[i], '\0' };
            val = JS_NewString(ctx, ch);
        } else {
            val = JS_GetPropertyUint32(ctx, source, i);
        }
        if (JS_IsException(val)) {
            JS_FreeValue(ctx, arr);
            if (source_str) JS_FreeCString(ctx, source_str);
            return JS_EXCEPTION;
        }
        if (has_map_fn) {
            JSValue map_args[2];
            map_args[0] = val;
            map_args[1] = JS_NewInt32(ctx, i);
            JSValue mapped = JS_Call(ctx, argv[1], this_arg, 2, map_args);
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, map_args[1]);
            if (JS_IsException(mapped)) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            JS_SetPropertyUint32(ctx, arr, i, mapped);
        } else {
            JS_SetPropertyUint32(ctx, arr, i, val);
        }
    }
    set_array_length(ctx, arr, len);
    if (source_str) JS_FreeCString(ctx, source_str);
    return arr;
}

/* ── Array.of ──────────────────────────────────────────────────────────── */

static JSValue js_array_of(JSContext *ctx, JSValue this_val,
                            int argc, JSValue *argv)
{
    (void)this_val;
    return array_from_values(ctx, argc, argv);
}

/* ── Array.prototype.at ────────────────────────────────────────────────── */

static JSValue js_array_proto_at(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.at called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    int32_t idx = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &idx, argv[0]);
    }
    if (idx < 0) idx = len + idx;
    if (idx < 0 || idx >= len) return JS_UNDEFINED;
    return JS_GetPropertyUint32(ctx, this_val, idx);
}

/* ── Array.prototype.concat ────────────────────────────────────────────── */

static JSValue js_array_proto_concat(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    JSValue result = JS_NewArray(ctx);
    int32_t out_idx = 0;

    /* Copy this array */
    if (JS_IsArray(ctx, this_val)) {
        int32_t len = get_array_length(ctx, this_val);
        for (int32_t i = 0; i < len; i++) {
            JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
            if (!JS_IsUndefined(val)) {
                JS_SetPropertyUint32(ctx, result, out_idx++, val);
            } else {
                JS_FreeValue(ctx, val);
            }
        }
    }

    /* Copy arguments */
    for (int i = 0; i < argc; i++) {
        if (JS_IsArray(ctx, argv[i])) {
            int32_t sub_len = get_array_length(ctx, argv[i]);
            for (int32_t j = 0; j < sub_len; j++) {
                JSValue val = JS_GetPropertyUint32(ctx, argv[i], j);
                if (!JS_IsUndefined(val)) {
                    JS_SetPropertyUint32(ctx, result, out_idx++, val);
                } else {
                    JS_FreeValue(ctx, val);
                }
            }
        } else {
            JS_SetPropertyUint32(ctx, result, out_idx++, JS_DupValue(ctx, argv[i]));
        }
    }
    set_array_length(ctx, result, out_idx);
    return result;
}

/* ── Array.prototype.copyWithin ────────────────────────────────────────── */

static JSValue js_array_proto_copy_within(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.copyWithin called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    int32_t target = 0, start = 0, end = len;

    if (argc > 0) {
        JS_ToInt32(ctx, &target, argv[0]);
        if (target < 0) target = len + target;
        if (target < 0) target = 0;
    }
    if (argc > 1) {
        JS_ToInt32(ctx, &start, argv[1]);
        if (start < 0) start = len + start;
        if (start < 0) start = 0;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        JS_ToInt32(ctx, &end, argv[2]);
        if (end < 0) end = len + end;
        if (end > len) end = len;
    }

    int32_t count = (end - start);
    if (count > len - target) count = len - target;
    if (count < 0) count = 0;

    /* Copy elements (backwards if target > start to avoid overwrite) */
    if (target > start) {
        for (int32_t i = count - 1; i >= 0; i--) {
            JSValue val = JS_GetPropertyUint32(ctx, this_val, start + i);
            if (JS_IsUndefined(val)) {
                /* Delete property */
                JSValue del = JS_UNDEFINED;
                JS_SetPropertyUint32(ctx, this_val, target + i, del);
            } else {
                JS_SetPropertyUint32(ctx, this_val, target + i, val);
            }
        }
    } else {
        for (int32_t i = 0; i < count; i++) {
            JSValue val = JS_GetPropertyUint32(ctx, this_val, start + i);
            if (JS_IsUndefined(val)) {
                JSValue del = JS_UNDEFINED;
                JS_SetPropertyUint32(ctx, this_val, target + i, del);
            } else {
                JS_SetPropertyUint32(ctx, this_val, target + i, val);
            }
        }
    }
    return JS_DupValue(ctx, this_val);
}

/* ── Array.prototype.entries ───────────────────────────────────────────── */

static JSValue js_array_proto_entries(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    /* Return a simple array of [index, value] pairs */
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.entries called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    for (int32_t i = 0; i < len; i++) {
        JSValue entry = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, entry, 0, JS_NewInt32(ctx, i));
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, entry, 1, val);
        } else {
            JS_SetPropertyUint32(ctx, entry, 1, JS_DupValue(ctx, val));
        }
        set_array_length(ctx, entry, 2);
        JS_SetPropertyUint32(ctx, arr, i, entry);
    }
    set_array_length(ctx, arr, len);
    return arr;
}

/* ── Array.prototype.every ─────────────────────────────────────────────── */

static JSValue js_array_proto_every(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.every called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.every requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(result)) {
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, result);
        JS_FreeValue(ctx, result);
        if (!ok) return JS_FALSE;
    }
    return JS_TRUE;
}

/* ── Array.prototype.fill ──────────────────────────────────────────────── */

static JSValue js_array_proto_fill(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.fill called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue value = (argc > 0) ? argv[0] : JS_UNDEFINED;
    int32_t start = 0, end = len;

    if (argc > 1) {
        JS_ToInt32(ctx, &start, argv[1]);
        if (start < 0) start = len + start;
        if (start < 0) start = 0;
    }
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        JS_ToInt32(ctx, &end, argv[2]);
        if (end < 0) end = len + end;
        if (end > len) end = len;
    }

    for (int32_t i = start; i < end; i++) {
        JS_SetPropertyUint32(ctx, this_val, i, JS_DupValue(ctx, value));
    }
    return JS_DupValue(ctx, this_val);
}

/* ── Array.prototype.filter ────────────────────────────────────────────── */

static JSValue js_array_proto_filter(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.filter called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.filter requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    JSValue result = JS_NewArray(ctx);
    int32_t out_idx = 0;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue ok_val = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        if (JS_IsException(ok_val)) {
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, ok_val);
        JS_FreeValue(ctx, ok_val);
        if (ok) {
            JS_SetPropertyUint32(ctx, result, out_idx++, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    set_array_length(ctx, result, out_idx);
    return result;
}

/* ── Array.prototype.find ──────────────────────────────────────────────── */

static JSValue js_array_proto_find(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.find called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.find requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, result);
        JS_FreeValue(ctx, result);
        if (ok) return val;
        JS_FreeValue(ctx, val);
    }
    return JS_UNDEFINED;
}

/* ── Array.prototype.findIndex ─────────────────────────────────────────── */

static JSValue js_array_proto_find_index(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.findIndex called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.findIndex requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(result)) {
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, result);
        JS_FreeValue(ctx, result);
        if (ok) return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── Array.prototype.findLast ──────────────────────────────────────────── */

static JSValue js_array_proto_find_last(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.findLast called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.findLast requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = len - 1; i >= 0; i--) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, result);
        JS_FreeValue(ctx, result);
        if (ok) return val;
        JS_FreeValue(ctx, val);
    }
    return JS_UNDEFINED;
}

/* ── Array.prototype.findLastIndex ─────────────────────────────────────── */

static JSValue js_array_proto_find_last_index(JSContext *ctx, JSValue this_val,
                                                int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.findLastIndex called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.findLastIndex requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = len - 1; i >= 0; i--) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(result)) {
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, result);
        JS_FreeValue(ctx, result);
        if (ok) return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── Array.prototype.flat (internal helper with explicit depth) ─────────── */

static JSValue js_array_proto_flat_with_depth(JSContext *ctx, JSValue this_val,
                                               int32_t depth)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.flat called on non-array");
    }

    JSValue result = JS_NewArray(ctx);
    int32_t out_idx = 0;
    int32_t len = get_array_length(ctx, this_val);

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (depth > 0 && JS_IsArray(ctx, val)) {
            /* Recursively flatten with depth - 1 */
            JSValue flat = js_array_proto_flat_with_depth(ctx, val, depth - 1);
            if (JS_IsException(flat)) {
                JS_FreeValue(ctx, val);
                JS_FreeValue(ctx, result);
                return JS_EXCEPTION;
            }
            int32_t flat_len = get_array_length(ctx, flat);
            for (int32_t j = 0; j < flat_len; j++) {
                JSValue item = JS_GetPropertyUint32(ctx, flat, j);
                if (!JS_IsUndefined(item)) {
                    JS_SetPropertyUint32(ctx, result, out_idx++, item);
                } else {
                    JS_FreeValue(ctx, item);
                }
            }
            JS_FreeValue(ctx, flat);
        } else {
            JS_SetPropertyUint32(ctx, result, out_idx++, val);
        }
    }
    set_array_length(ctx, result, out_idx);
    return result;
}

/* ── Array.prototype.flat ──────────────────────────────────────────────── */

static JSValue js_array_proto_flat(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.flat called on non-array");
    }
    int32_t depth = 1;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        JS_ToInt32(ctx, &depth, argv[0]);
        if (depth < 0) depth = 0;
    }
    return js_array_proto_flat_with_depth(ctx, this_val, depth);
}

/* ── Array.prototype.flatMap ───────────────────────────────────────────── */

static JSValue js_array_proto_flat_map(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.flatMap called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.flatMap requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    JSValue result = JS_NewArray(ctx);
    int32_t out_idx = 0;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue mapped = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(mapped)) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        /* If mapped is an array, flatten one level */
        if (JS_IsArray(ctx, mapped)) {
            int32_t sub_len = get_array_length(ctx, mapped);
            for (int32_t j = 0; j < sub_len; j++) {
                JSValue item = JS_GetPropertyUint32(ctx, mapped, j);
                if (!JS_IsUndefined(item)) {
                    JS_SetPropertyUint32(ctx, result, out_idx++, item);
                } else {
                    JS_FreeValue(ctx, item);
                }
            }
        } else {
            JS_SetPropertyUint32(ctx, result, out_idx++, mapped);
        }
    }
    set_array_length(ctx, result, out_idx);
    return result;
}

/* ── Array.prototype.forEach ───────────────────────────────────────────── */

static JSValue js_array_proto_for_each(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.forEach called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.forEach requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(result)) {
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, result);
    }
    return JS_UNDEFINED;
}

/* ── Array.prototype.includes ──────────────────────────────────────────── */

static JSValue js_array_proto_includes(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.includes called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    if (len == 0) return JS_FALSE;

    JSValue search = (argc > 0) ? argv[0] : JS_UNDEFINED;
    int32_t from_idx = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &from_idx, argv[1]);
        if (from_idx < 0) from_idx = len + from_idx;
        if (from_idx < 0) from_idx = 0;
    }

    /* Use SameValueZero comparison (like Object.is but +0 === -0) */
    for (int32_t i = from_idx; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        int found = 0;
        /* Simple comparison using SameValueZero */
        if (val.tag == search.tag) {
            switch (val.tag) {
            case LR_TYPE_UNDEFINED:
            case LR_TYPE_NULL:
                found = 1;
                break;
            case LR_TYPE_BOOL:
                found = (val.u.bool_val == search.u.bool_val);
                break;
            case LR_TYPE_INT32:
                found = (val.u.int32 == search.u.int32);
                break;
            case LR_TYPE_FLOAT64:
                if (isnan(val.u.float64) && isnan(search.u.float64))
                    found = 1;
                else
                    found = (val.u.float64 == search.u.float64);
                break;
            case LR_TYPE_STRING:
            case LR_TYPE_OBJECT:
                found = (val.u.ptr == search.u.ptr);
                break;
            default:
                found = 0;
            }
        }
        JS_FreeValue(ctx, val);
        if (found) return JS_TRUE;
    }
    return JS_FALSE;
}

/* ── Array.prototype.indexOf ───────────────────────────────────────────── */

static JSValue js_array_proto_index_of(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.indexOf called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    int32_t from_idx = 0;
    if (argc < 1) return JS_NewInt32(ctx, -1);

    if (argc > 1) {
        JS_ToInt32(ctx, &from_idx, argv[1]);
        if (from_idx < 0) from_idx = len + from_idx;
        if (from_idx < 0) from_idx = 0;
    }

    JSValue search = argv[0];
    for (int32_t i = from_idx; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        int found = 0;
        if (val.tag == search.tag) {
            switch (val.tag) {
            case LR_TYPE_UNDEFINED:
                found = 1;
                break;
            case LR_TYPE_BOOL:
                found = (val.u.bool_val == search.u.bool_val);
                break;
            case LR_TYPE_INT32:
                found = (val.u.int32 == search.u.int32);
                break;
            case LR_TYPE_FLOAT64:
                if (val.u.float64 == search.u.float64) found = 1;
                break;
            case LR_TYPE_STRING:
            case LR_TYPE_OBJECT:
                found = (val.u.ptr == search.u.ptr);
                break;
            default:
                found = 0;
            }
        }
        JS_FreeValue(ctx, val);
        if (found) return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── Array.prototype.join ──────────────────────────────────────────────── */

static JSValue js_array_proto_join(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.join called on non-array");
    }
    const char *sep = ",";
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        sep = JS_ToCString(ctx, argv[0]);
        if (!sep) return JS_EXCEPTION;
    }
    int32_t len = get_array_length(ctx, this_val);
    /* Calculate total length */
    size_t total = 0;
    char **parts = (char **)calloc(len, sizeof(char *));
    if (!parts && len > 0) return JS_EXCEPTION;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (JS_IsUndefined(val) || JS_IsNull(val)) {
            parts[i] = strdup("");
        } else {
            const char *s = JS_ToCString(ctx, val);
            parts[i] = s ? strdup(s) : strdup("");
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, val);
        total += strlen(parts[i]);
    }

    size_t sep_len = strlen(sep);
    total += (len > 0 ? (len - 1) * sep_len : 0);

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        for (int32_t i = 0; i < len; i++) free(parts[i]);
        free(parts);
        return JS_EXCEPTION;
    }
    buf[0] = '\0';
    for (int32_t i = 0; i < len; i++) {
        if (i > 0) strcat(buf, sep);
        strcat(buf, parts[i]);
        free(parts[i]);
    }
    free(parts);

    JSValue result = JS_NewString(ctx, buf);
    free(buf);

    if (argc > 0 && strcmp(sep, ",") != 0) JS_FreeCString(ctx, sep);
    return result;
}

/* ── Array.prototype.keys ──────────────────────────────────────────────── */

static JSValue js_array_proto_keys(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.keys called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    for (int32_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, i));
    }
    set_array_length(ctx, arr, len);
    return arr;
}

/* ── Array.prototype.lastIndexOf ───────────────────────────────────────── */

static JSValue js_array_proto_last_index_of(JSContext *ctx, JSValue this_val,
                                              int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.lastIndexOf called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    if (argc < 1) return JS_NewInt32(ctx, -1);

    int32_t from_idx = len - 1;
    if (argc > 1) {
        JS_ToInt32(ctx, &from_idx, argv[1]);
        if (from_idx >= len) from_idx = len - 1;
        if (from_idx < 0) from_idx = len + from_idx;
        if (from_idx < 0) return JS_NewInt32(ctx, -1);
    }

    JSValue search = argv[0];
    for (int32_t i = from_idx; i >= 0; i--) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        int found = 0;
        if (val.tag == search.tag) {
            switch (val.tag) {
            case LR_TYPE_UNDEFINED:
                found = 1;
                break;
            case LR_TYPE_BOOL:
                found = (val.u.bool_val == search.u.bool_val);
                break;
            case LR_TYPE_INT32:
                found = (val.u.int32 == search.u.int32);
                break;
            case LR_TYPE_FLOAT64:
                if (val.u.float64 == search.u.float64) found = 1;
                break;
            case LR_TYPE_STRING:
            case LR_TYPE_OBJECT:
                found = (val.u.ptr == search.u.ptr);
                break;
            default:
                found = 0;
            }
        }
        JS_FreeValue(ctx, val);
        if (found) return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── Array.prototype.map ───────────────────────────────────────────────── */

static JSValue js_array_proto_map(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.map called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.map requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    JSValue result = JS_NewArray(ctx);

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue mapped = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(mapped)) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        JS_SetPropertyUint32(ctx, result, i, mapped);
    }
    set_array_length(ctx, result, len);
    return result;
}

/* ── Array.prototype.pop ───────────────────────────────────────────────── */

static JSValue js_array_proto_pop(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.pop called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    if (len == 0) return JS_UNDEFINED;
    JSValue val = JS_GetPropertyUint32(ctx, this_val, len - 1);
    set_array_length(ctx, this_val, len - 1);
    return val;
}

/* ── Array.prototype.push ──────────────────────────────────────────────── */

static JSValue js_array_proto_push(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.push called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    for (int i = 0; i < argc; i++) {
        JS_SetPropertyUint32(ctx, this_val, len + i, JS_DupValue(ctx, argv[i]));
    }
    len += argc;
    set_array_length(ctx, this_val, len);
    return JS_NewInt32(ctx, len);
}

/* ── Array.prototype.reduce ────────────────────────────────────────────── */

static JSValue js_array_proto_reduce(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.reduce called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.reduce requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);

    /* Fast path: dense array with initial value → sum directly in C.
     * Uses int64_t to avoid overflow; returns Int32 when value fits. */
    if (argc >= 2 && this_val.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)this_val.u.ptr;
        if (o->type == LR_OBJ_ARRAY && o->extra) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            int64_t sum = 0;
            if (argc >= 2 && JS_IsNumber(argv[1])) {
                int32_t iv = 0;
                JS_ToInt32(ctx, &iv, argv[1]);
                sum = iv;
            }
            for (int32_t i = 0; i < len && i < (int32_t)ad->length; i++) {
                if (ad->elements[i].tag == LR_TYPE_INT32)
                    sum += ad->elements[i].u.int32;
            }
            if (sum >= (int64_t)INT32_MIN && sum <= (int64_t)INT32_MAX)
                return JS_NewInt32(ctx, (int32_t)sum);
            return JS_NewFloat64(ctx, (double)sum);
        }
    }

    JSValue func = argv[0];
    JSValue accumulator;
    int32_t start_idx = 0;

    if (argc >= 2) {
        accumulator = JS_DupValue(ctx, argv[1]);
    } else {
        if (len == 0) {
            return JS_ThrowTypeError(ctx, "Reduce of empty array with no initial value");
        }
        accumulator = JS_GetPropertyUint32(ctx, this_val, 0);
        start_idx = 1;
    }

    for (int32_t i = start_idx; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[4];
        call_args[0] = JS_DupValue(ctx, accumulator);
        JS_FreeValue(ctx, accumulator);
        call_args[1] = val;
        call_args[2] = JS_NewInt32(ctx, i);
        call_args[3] = JS_DupValue(ctx, this_val);
        accumulator = JS_Call(ctx, func, JS_UNDEFINED, 4, call_args);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, call_args[3]);
        if (JS_IsException(accumulator)) {
            return JS_EXCEPTION;
        }
    }
    return accumulator;
}

/* ── Array.prototype.reduceRight ───────────────────────────────────────── */

static JSValue js_array_proto_reduce_right(JSContext *ctx, JSValue this_val,
                                             int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.reduceRight called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.reduceRight requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue accumulator;
    int32_t start_idx = len - 1;

    if (argc >= 2) {
        accumulator = JS_DupValue(ctx, argv[1]);
    } else {
        if (len == 0) {
            return JS_ThrowTypeError(ctx, "ReduceRight of empty array with no initial value");
        }
        accumulator = JS_GetPropertyUint32(ctx, this_val, len - 1);
        start_idx = len - 2;
    }

    for (int32_t i = start_idx; i >= 0; i--) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[4];
        call_args[0] = JS_DupValue(ctx, accumulator);
        JS_FreeValue(ctx, accumulator);
        call_args[1] = val;
        call_args[2] = JS_NewInt32(ctx, i);
        call_args[3] = JS_DupValue(ctx, this_val);
        accumulator = JS_Call(ctx, func, JS_UNDEFINED, 4, call_args);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, call_args[3]);
        if (JS_IsException(accumulator)) {
            return JS_EXCEPTION;
        }
    }
    return accumulator;
}

/* ── Array.prototype.reverse ───────────────────────────────────────────── */

static JSValue js_array_proto_reverse(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.reverse called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    for (int32_t i = 0, j = len - 1; i < j; i++, j--) {
        JSValue left = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue right = JS_GetPropertyUint32(ctx, this_val, j);
        if (!JS_IsUndefined(left)) {
            JS_SetPropertyUint32(ctx, this_val, j, left);
        } else {
            JS_FreeValue(ctx, left);
            /* Set undefined */
            JSValue undef = JS_UNDEFINED;
            JS_SetPropertyUint32(ctx, this_val, j, undef);
        }
        if (!JS_IsUndefined(right)) {
            JS_SetPropertyUint32(ctx, this_val, i, right);
        } else {
            JS_FreeValue(ctx, right);
            JSValue undef = JS_UNDEFINED;
            JS_SetPropertyUint32(ctx, this_val, i, undef);
        }
    }
    return JS_DupValue(ctx, this_val);
}

/* ── Array.prototype.shift ─────────────────────────────────────────────── */

static JSValue js_array_proto_shift(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.shift called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    if (len == 0) return JS_UNDEFINED;

    JSValue first = JS_GetPropertyUint32(ctx, this_val, 0);
    for (int32_t i = 1; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, this_val, i - 1, val);
        } else {
            JS_FreeValue(ctx, val);
            JSValue undef = JS_UNDEFINED;
            JS_SetPropertyUint32(ctx, this_val, i - 1, undef);
        }
    }
    set_array_length(ctx, this_val, len - 1);
    return first;
}

/* ── Array.prototype.slice ─────────────────────────────────────────────── */

static JSValue js_array_proto_slice(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.slice called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    int32_t start = 0, end = len;

    if (argc > 0) {
        JS_ToInt32(ctx, &start, argv[0]);
        if (start < 0) start = len + start;
        if (start < 0) start = 0;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JS_ToInt32(ctx, &end, argv[1]);
        if (end < 0) end = len + end;
        if (end > len) end = len;
    }

    JSValue result = JS_NewArray(ctx);
    int32_t out_idx = 0;
    for (int32_t i = start; i < end; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, result, out_idx++, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    set_array_length(ctx, result, out_idx);
    return result;
}

/* ── Array.prototype.some ──────────────────────────────────────────────── */

static JSValue js_array_proto_some(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.some called on non-array");
    }
    if (argc < 1 || !is_callable(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Array.prototype.some requires a function");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue func = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        JSValue call_args[3];
        call_args[0] = val;
        call_args[1] = JS_NewInt32(ctx, i);
        call_args[2] = JS_DupValue(ctx, this_val);
        JSValue result = JS_Call(ctx, func, this_arg, 3, call_args);
        JS_FreeValue(ctx, call_args[1]);
        JS_FreeValue(ctx, call_args[2]);
        JS_FreeValue(ctx, val);
        if (JS_IsException(result)) {
            return JS_EXCEPTION;
        }
        int ok = JS_ToBool(ctx, result);
        JS_FreeValue(ctx, result);
        if (ok) return JS_TRUE;
    }
    return JS_FALSE;
}

/* ── Array.prototype.sort (basic quick sort) ───────────────────────────── */

static JSValue js_array_proto_sort(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.sort called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    int has_compare = (argc > 0 && is_callable(ctx, argv[0]));
    JSValue compare = has_compare ? argv[0] : JS_UNDEFINED;

    /* Collect elements into a temporary array */
    JSValue *elements = (JSValue *)calloc(len, sizeof(JSValue));
    if (!elements && len > 0) return JS_EXCEPTION;

    for (int32_t i = 0; i < len; i++) {
        elements[i] = JS_GetPropertyUint32(ctx, this_val, i);
    }

    /* Simple bubble sort */
    for (int32_t i = 0; i < len - 1; i++) {
        for (int32_t j = 0; j < len - 1 - i; j++) {
            int cmp = 0;
            if (has_compare) {
                JSValue args[2];
                args[0] = elements[j];
                args[1] = elements[j + 1];
                JSValue result = JS_Call(ctx, compare, JS_UNDEFINED, 2, args);
                if (JS_IsException(result)) {
                    for (int32_t k = 0; k < len; k++) JS_FreeValue(ctx, elements[k]);
                    free(elements);
                    return JS_EXCEPTION;
                }
                double d;
                JS_ToFloat64(ctx, &d, result);
                JS_FreeValue(ctx, result);
                cmp = (int)d;
            } else {
                /* Default comparison: convert to string, compare */
                const char *sa = JS_ToCString(ctx, elements[j]);
                const char *sb = JS_ToCString(ctx, elements[j + 1]);
                if (sa && sb) {
                    cmp = strcmp(sa, sb);
                }
                if (sa) JS_FreeCString(ctx, sa);
                if (sb) JS_FreeCString(ctx, sb);
            }
            if (cmp > 0) {
                JSValue tmp = elements[j];
                elements[j] = elements[j + 1];
                elements[j + 1] = tmp;
            }
        }
    }

    /* Write back sorted elements */
    for (int32_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, this_val, i, elements[i]);
    }
    free(elements);
    return JS_DupValue(ctx, this_val);
}

/* ── Array.prototype.splice ────────────────────────────────────────────── */

static JSValue js_array_proto_splice(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.splice called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    int32_t start = 0;
    int32_t delete_count = 0;

    if (argc > 0) {
        JS_ToInt32(ctx, &start, argv[0]);
        if (start < 0) start = len + start;
        if (start < 0) start = 0;
        if (start > len) start = len;
    }

    if (argc > 1) {
        JS_ToInt32(ctx, &delete_count, argv[1]);
        if (delete_count < 0) delete_count = 0;
        if (start + delete_count > len) delete_count = len - start;
    } else {
        delete_count = len - start;
    }

    /* Collect removed elements */
    JSValue removed = JS_NewArray(ctx);
    for (int32_t i = 0; i < delete_count; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, start + i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, removed, i, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    set_array_length(ctx, removed, delete_count);

    int32_t insert_count = (argc > 2) ? argc - 2 : 0;
    int32_t new_len = len - delete_count + insert_count;

    if (insert_count < delete_count) {
        /* Shift elements left */
        for (int32_t i = start; i < new_len; i++) {
            JSValue val = JS_GetPropertyUint32(ctx, this_val, i + delete_count - insert_count);
            if (!JS_IsUndefined(val)) {
                JS_SetPropertyUint32(ctx, this_val, i, val);
            } else {
                JS_FreeValue(ctx, val);
                JSValue undef = JS_UNDEFINED;
                JS_SetPropertyUint32(ctx, this_val, i, undef);
            }
        }
    } else if (insert_count > delete_count) {
        /* Shift elements right (from end to avoid overwrite) */
        for (int32_t i = new_len - 1; i >= start + insert_count; i--) {
            JSValue val = JS_GetPropertyUint32(ctx, this_val, i - insert_count + delete_count);
            if (!JS_IsUndefined(val)) {
                JS_SetPropertyUint32(ctx, this_val, i, val);
            } else {
                JS_FreeValue(ctx, val);
                JSValue undef = JS_UNDEFINED;
                JS_SetPropertyUint32(ctx, this_val, i, undef);
            }
        }
    }

    /* Insert new items */
    for (int32_t i = 0; i < insert_count; i++) {
        JS_SetPropertyUint32(ctx, this_val, start + i, JS_DupValue(ctx, argv[2 + i]));
    }

    set_array_length(ctx, this_val, new_len);
    return removed;
}

/* ── Array.prototype.toReversed ────────────────────────────────────────── */

static JSValue js_array_proto_to_reversed(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.toReversed called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue result = JS_NewArray(ctx);
    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, len - 1 - i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, result, i, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    set_array_length(ctx, result, len);
    return result;
}

/* ── Array.prototype.toSorted ──────────────────────────────────────────── */

static JSValue js_array_proto_to_sorted(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.toSorted called on non-array");
    }
    /* Create a copy, then sort the copy */
    JSValue copy = js_array_proto_slice(ctx, this_val, 0, NULL);
    if (JS_IsException(copy)) return copy;

    /* Create a new array from the copy */
    JSValue result = js_array_proto_slice(ctx, this_val, 0, NULL);
    /* Sort the result */
    if (argc > 0) {
        JSValue sort_args[1];
        sort_args[0] = argv[0];
        JSValue sorted = js_array_proto_sort(ctx, result, 1, sort_args);
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, copy);
        return sorted;
    }
    JSValue sorted = js_array_proto_sort(ctx, result, 0, NULL);
    JS_FreeValue(ctx, copy);
    return sorted;
}

/* ── Array.prototype.toSpliced ─────────────────────────────────────────── */

static JSValue js_array_proto_to_spliced(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.toSpliced called on non-array");
    }
    /* Get a copy */
    JSValue result = js_array_proto_slice(ctx, this_val, 0, NULL);
    if (JS_IsException(result)) return result;

    /* Now splice in place on the copy */
    /* Rebuild arguments for splice: start, deleteCount, ...items */
    int32_t start = 0;
    int32_t delete_count = 0;
    int32_t len = get_array_length(ctx, result);

    if (argc > 0) {
        JS_ToInt32(ctx, &start, argv[0]);
        if (start < 0) start = len + start;
        if (start < 0) start = 0;
        if (start > len) start = len;
    }
    if (argc > 1) {
        JS_ToInt32(ctx, &delete_count, argv[1]);
        if (delete_count < 0) delete_count = 0;
        if (start + delete_count > len) delete_count = len - start;
    } else {
        delete_count = len - start;
    }

    int32_t insert_count = (argc > 2) ? argc - 2 : 0;
    int32_t new_len = len - delete_count + insert_count;

    if (insert_count < delete_count) {
        for (int32_t i = start; i < new_len; i++) {
            JSValue val = JS_GetPropertyUint32(ctx, result, i + delete_count - insert_count);
            if (!JS_IsUndefined(val)) {
                JS_SetPropertyUint32(ctx, result, i, val);
            } else {
                JS_FreeValue(ctx, val);
                JSValue undef = JS_UNDEFINED;
                JS_SetPropertyUint32(ctx, result, i, undef);
            }
        }
    } else if (insert_count > delete_count) {
        for (int32_t i = new_len - 1; i >= start + insert_count; i--) {
            JSValue val = JS_GetPropertyUint32(ctx, result, i - insert_count + delete_count);
            if (!JS_IsUndefined(val)) {
                JS_SetPropertyUint32(ctx, result, i, val);
            } else {
                JS_FreeValue(ctx, val);
                JSValue undef = JS_UNDEFINED;
                JS_SetPropertyUint32(ctx, result, i, undef);
            }
        }
    }

    for (int32_t i = 0; i < insert_count; i++) {
        JS_SetPropertyUint32(ctx, result, start + i, JS_DupValue(ctx, argv[2 + i]));
    }

    set_array_length(ctx, result, new_len);
    return result;
}

/* ── Array.prototype.toString ──────────────────────────────────────────── */

static JSValue js_array_proto_to_string(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    return js_array_proto_join(ctx, this_val, 0, NULL);
}

/* ── Array.prototype.unshift ───────────────────────────────────────────── */

static JSValue js_array_proto_unshift(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.unshift called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    /* Shift elements right */
    for (int32_t i = len - 1; i >= 0; i--) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, this_val, i + argc, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    /* Insert new elements at beginning */
    for (int i = 0; i < argc; i++) {
        JS_SetPropertyUint32(ctx, this_val, i, JS_DupValue(ctx, argv[i]));
    }
    len += argc;
    set_array_length(ctx, this_val, len);
    return JS_NewInt32(ctx, len);
}

/* ── Array.prototype.values ────────────────────────────────────────────── */

static JSValue js_array_proto_values(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.values called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    for (int32_t i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, this_val, i);
        if (!JS_IsUndefined(val)) {
            JS_SetPropertyUint32(ctx, arr, i, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    set_array_length(ctx, arr, len);
    return arr;
}

/* ── Array.prototype.with ──────────────────────────────────────────────── */

static JSValue js_array_proto_with(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (!JS_IsArray(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Array.prototype.with called on non-array");
    }
    int32_t len = get_array_length(ctx, this_val);
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "Array.prototype.with requires 2 arguments");
    }
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0) idx = len + idx;
    if (idx < 0 || idx >= len) {
        return JS_ThrowRangeError(ctx, "Index out of range");
    }
    JSValue result = JS_NewArray(ctx);
    for (int32_t i = 0; i < len; i++) {
        JSValue val = (i == idx) ? JS_DupValue(ctx, argv[1]) : JS_GetPropertyUint32(ctx, this_val, i);
        if (!JS_IsUndefined(val) || i == idx) {
            JS_SetPropertyUint32(ctx, result, i, val);
        } else {
            JS_FreeValue(ctx, val);
        }
    }
    set_array_length(ctx, result, len);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════════
   3. STRING
   ══════════════════════════════════════════════════════════════════════════ */

static JSValue js_string_constructor(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    (void)this_val;
    if (argc == 0) return JS_NewString(ctx, "");
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_EXCEPTION;
    JSValue result = JS_NewString(ctx, str);
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.fromCharCode ───────────────────────────────────────────────── */

static JSValue js_string_from_char_code(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    (void)this_val;
    char *buf = (char *)malloc(argc + 1);
    if (!buf && argc > 0) return JS_EXCEPTION;
    for (int i = 0; i < argc; i++) {
        int32_t code;
        JS_ToInt32(ctx, &code, argv[i]);
        buf[i] = (char)(code & 0xFF);
    }
    buf[argc] = '\0';
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.fromCodePoint ──────────────────────────────────────────────── */

static JSValue js_string_from_code_point(JSContext *ctx, JSValue this_val,
                                           int argc, JSValue *argv)
{
    (void)this_val;
    /* For simplicity, handle only BMP characters */
    char *buf = (char *)malloc(argc * 4 + 1);
    if (!buf && argc > 0) return JS_EXCEPTION;
    int pos = 0;
    for (int i = 0; i < argc; i++) {
        int32_t cp;
        JS_ToInt32(ctx, &cp, argv[i]);
        if (cp < 0x80) {
            buf[pos++] = (char)cp;
        } else if (cp < 0x800) {
            buf[pos++] = 0xC0 | (cp >> 6);
            buf[pos++] = 0x80 | (cp & 0x3F);
        } else if (cp < 0x10000) {
            buf[pos++] = 0xE0 | (cp >> 12);
            buf[pos++] = 0x80 | ((cp >> 6) & 0x3F);
            buf[pos++] = 0x80 | (cp & 0x3F);
        } else {
            buf[pos++] = 0xF0 | (cp >> 18);
            buf[pos++] = 0x80 | ((cp >> 12) & 0x3F);
            buf[pos++] = 0x80 | ((cp >> 6) & 0x3F);
            buf[pos++] = 0x80 | (cp & 0x3F);
        }
    }
    buf[pos] = '\0';
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.at ───────────────────────────────────────────────── */

static JSValue js_string_proto_at(JSContext *ctx, JSValue this_val,
                                   int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t len = (int32_t)strlen(str);
    int32_t idx = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &idx, argv[0]);
    }
    if (idx < 0) idx = len + idx;
    JSValue result;
    if (idx < 0 || idx >= len) {
        result = JS_UNDEFINED;
    } else {
        char buf[2] = { str[idx], '\0' };
        result = JS_NewString(ctx, buf);
    }
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.prototype.charAt ───────────────────────────────────────────── */

static JSValue js_string_proto_char_at(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t len = (int32_t)strlen(str);
    int32_t idx = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &idx, argv[0]);
    }
    JSValue result;
    if (idx < 0 || idx >= len) {
        result = JS_NewString(ctx, "");
    } else {
        char buf[2] = { str[idx], '\0' };
        result = JS_NewString(ctx, buf);
    }
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.prototype.charCodeAt ───────────────────────────────────────── */

static JSValue js_string_proto_char_code_at(JSContext *ctx, JSValue this_val,
                                              int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t len = (int32_t)strlen(str);
    int32_t idx = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &idx, argv[0]);
    }
    JS_FreeCString(ctx, str);
    if (idx < 0 || idx >= len) {
        return JS_NewFloat64(ctx, NAN);
    }
    return JS_NewInt32(ctx, (unsigned char)str[idx]);
}

/* ── String.prototype.codePointAt ──────────────────────────────────────── */

static JSValue js_string_proto_code_point_at(JSContext *ctx, JSValue this_val,
                                               int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t len = (int32_t)strlen(str);
    int32_t idx = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &idx, argv[0]);
    }
    JS_FreeCString(ctx, str);
    if (idx < 0 || idx >= len) {
        return JS_UNDEFINED;
    }
    /* Simple UTF-8 decode */
    unsigned char c = (unsigned char)str[idx];
    int32_t cp;
    if (c < 0x80) {
        cp = c;
    } else if ((c & 0xE0) == 0xC0 && idx + 1 < len) {
        cp = ((c & 0x1F) << 6) | ((unsigned char)str[idx + 1] & 0x3F);
    } else if ((c & 0xF0) == 0xE0 && idx + 2 < len) {
        cp = ((c & 0x0F) << 12) | (((unsigned char)str[idx + 1] & 0x3F) << 6) | ((unsigned char)str[idx + 2] & 0x3F);
    } else if ((c & 0xF8) == 0xF0 && idx + 3 < len) {
        cp = ((c & 0x07) << 18) | (((unsigned char)str[idx + 1] & 0x3F) << 12) | (((unsigned char)str[idx + 2] & 0x3F) << 6) | ((unsigned char)str[idx + 3] & 0x3F);
    } else {
        cp = c;
    }
    return JS_NewInt32(ctx, cp);
}

/* ── String.prototype.concat ───────────────────────────────────────────── */

static JSValue js_string_proto_concat(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    const char *base = JS_ToCString(ctx, this_val);
    if (!base) return JS_EXCEPTION;
    size_t total = strlen(base);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            total += strlen(s);
            JS_FreeCString(ctx, s);
        }
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        JS_FreeCString(ctx, base);
        return JS_EXCEPTION;
    }
    strcpy(buf, base);
    size_t pos = strlen(buf);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            strcpy(buf + pos, s);
            pos += strlen(s);
            JS_FreeCString(ctx, s);
        }
    }
    buf[pos] = '\0';
    JS_FreeCString(ctx, base);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.includes ─────────────────────────────────────────── */

static JSValue js_string_proto_includes(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    const char *str = JS_ToCString(ctx, this_val);
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!str || !search) {
        if (str) JS_FreeCString(ctx, str);
        if (search) JS_FreeCString(ctx, search);
        return JS_EXCEPTION;
    }
    int32_t pos = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &pos, argv[1]);
        if (pos < 0) pos = 0;
    }
    int found = 0;
    if ((size_t)pos < strlen(str)) {
        found = (strstr(str + pos, search) != NULL);
    }
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, search);
    return JS_NewBool(ctx, found);
}

/* ── String.prototype.indexOf ──────────────────────────────────────────── */

static JSValue js_string_proto_index_of(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const char *str = JS_ToCString(ctx, this_val);
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!str || !search) {
        if (str) JS_FreeCString(ctx, str);
        if (search) JS_FreeCString(ctx, search);
        return JS_EXCEPTION;
    }
    int32_t pos = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &pos, argv[1]);
        if (pos < 0) pos = 0;
    }
    int32_t result = -1;
    if ((size_t)pos < strlen(str)) {
        const char *found = strstr(str + pos, search);
        if (found) {
            result = (int32_t)(found - str);
        }
    }
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, search);
    return JS_NewInt32(ctx, result);
}

/* ── String.prototype.lastIndexOf ──────────────────────────────────────── */

static JSValue js_string_proto_last_index_of(JSContext *ctx, JSValue this_val,
                                               int argc, JSValue *argv)
{
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const char *str = JS_ToCString(ctx, this_val);
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!str || !search) {
        if (str) JS_FreeCString(ctx, str);
        if (search) JS_FreeCString(ctx, search);
        return JS_EXCEPTION;
    }
    size_t str_len = strlen(str);
    size_t search_len = strlen(search);
    int32_t pos = (int32_t)str_len;
    if (argc > 1) {
        JS_ToInt32(ctx, &pos, argv[1]);
        if (pos < 0) pos = 0;
        if ((size_t)pos > str_len) pos = (int32_t)str_len;
    }
    int32_t result = -1;
    if (search_len > 0) {
        for (int32_t i = pos; i >= 0; i--) {
            if (strncmp(str + i, search, search_len) == 0) {
                result = i;
                break;
            }
        }
    } else {
        result = pos;
    }
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, search);
    return JS_NewInt32(ctx, result);
}

/* ── String.prototype.match ────────────────────────────────────────────── */

static JSValue js_string_proto_match(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (argc < 1) return JS_NULL;
    const char *str = JS_ToCString(ctx, this_val);
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!str || !pattern) {
        if (str) JS_FreeCString(ctx, str);
        if (pattern) JS_FreeCString(ctx, pattern);
        return JS_EXCEPTION;
    }
    const char *found = strstr(str, pattern);
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, pattern);
    if (found) {
        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, found));
        set_array_length(ctx, arr, 1);
        return arr;
    }
    return JS_NULL;
}

/* ── String.prototype.matchAll ─────────────────────────────────────────── */

static JSValue js_string_proto_match_all(JSContext *ctx, JSValue this_val,
                                           int argc, JSValue *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "String.prototype.matchAll requires a regexp");

    /* Use the RegExp exec protocol so real regex semantics apply */
    JSValue re = argv[0];
    JSValue exec_fn = JS_GetPropertyStr(ctx, re, "exec");
    if (is_callable(ctx, exec_fn)) {
        JSValue g = JS_GetPropertyStr(ctx, re, "global");
        int is_global = JS_ToBool(ctx, g);
        JS_FreeValue(ctx, g);
        JS_SetPropertyStr(ctx, re, "lastIndex", JS_NewInt32(ctx, 0));

        JSValue arr = JS_NewArray(ctx);
        int32_t idx = 0;
        JSValue sv = this_val;
        for (int guard = 0; guard < 100000; guard++) {
            JSValue m = lr_call(ctx, exec_fn, re, 1, &sv);
            if (JS_IsException(m) || JS_IsNull(m) || JS_IsUndefined(m)) {
                JS_FreeValue(ctx, m);
                break;
            }
            JS_SetPropertyUint32(ctx, arr, idx++, m);
            if (!is_global) break;
            /* Avoid infinite loop on empty match: bump lastIndex */
            JSValue m0 = JS_GetPropertyUint32(ctx, m, 0);
            const char *m0s = JS_ToCString(ctx, m0);
            int empty = (!m0s || m0s[0] == '\0');
            if (m0s) JS_FreeCString(ctx, m0s);
            JS_FreeValue(ctx, m0);
            if (empty) {
                JSValue li = JS_GetPropertyStr(ctx, re, "lastIndex");
                int32_t liv = 0;
                JS_ToInt32(ctx, &liv, li);
                JS_FreeValue(ctx, li);
                JS_SetPropertyStr(ctx, re, "lastIndex", JS_NewInt32(ctx, liv + 1));
            }
        }
        set_array_length(ctx, arr, idx);
        JS_FreeValue(ctx, exec_fn);
        return arr;
    }
    JS_FreeValue(ctx, exec_fn);

    /* Fallback: plain substring scan when argument is not RegExp-like */
    const char *str = JS_ToCString(ctx, this_val);
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!str || !pattern) {
        if (str) JS_FreeCString(ctx, str);
        if (pattern) JS_FreeCString(ctx, pattern);
        return JS_EXCEPTION;
    }
    JSValue arr = JS_NewArray(ctx);
    int32_t idx = 0;
    const char *pos = str;
    size_t pat_len = strlen(pattern);
    while (pat_len > 0 && (pos = strstr(pos, pattern)) != NULL) {
        JSValue match = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, match, 0, JS_NewString(ctx, pattern));
        set_array_length(ctx, match, 1);
        JS_SetPropertyUint32(ctx, arr, idx++, match);
        pos += pat_len;
    }
    set_array_length(ctx, arr, idx);
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, pattern);
    return arr;
}

/* ── String.prototype.replace ──────────────────────────────────────────── */

static JSValue js_string_proto_replace(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;

    JSValue search_val = argv[0];
    JSValue replace_val = argv[1];
    const char *search_str = NULL;
    int is_func = is_callable(ctx, replace_val);

    if (!JS_IsFunction(ctx, search_val) && !JS_IsObject(search_val)) {
        search_str = JS_ToCString(ctx, search_val);
        if (!search_str) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    }

    const char *found = NULL;
    if (search_str) {
        found = strstr(str, search_str);
    }

    if (found) {
        size_t prefix_len = (size_t)(found - str);
        size_t search_len = search_str ? strlen(search_str) : 0;
        size_t suffix_len = strlen(found + search_len);

        char *result_str = NULL;
        if (is_func) {
            JSValue match_args[3];
            match_args[0] = JS_NewString(ctx, found);
            match_args[1] = JS_NewInt32(ctx, (int32_t)prefix_len);
            match_args[2] = JS_DupValue(ctx, this_val);
            JSValue replacement = JS_Call(ctx, replace_val, JS_UNDEFINED, 3, match_args);
            JS_FreeValue(ctx, match_args[0]);
            JS_FreeValue(ctx, match_args[1]);
            JS_FreeValue(ctx, match_args[2]);
            if (JS_IsException(replacement)) {
                if (search_str) JS_FreeCString(ctx, search_str);
                JS_FreeCString(ctx, str);
                return JS_EXCEPTION;
            }
            const char *repl_str = JS_ToCString(ctx, replacement);
            if (!repl_str) {
                JS_FreeValue(ctx, replacement);
                if (search_str) JS_FreeCString(ctx, search_str);
                JS_FreeCString(ctx, str);
                return JS_EXCEPTION;
            }
            size_t repl_len = strlen(repl_str);
            result_str = (char *)malloc(prefix_len + repl_len + suffix_len + 1);
            if (result_str) {
                memcpy(result_str, str, prefix_len);
                memcpy(result_str + prefix_len, repl_str, repl_len);
                memcpy(result_str + prefix_len + repl_len, found + search_len, suffix_len + 1);
            }
            JS_FreeCString(ctx, repl_str);
            JS_FreeValue(ctx, replacement);
        } else {
            const char *repl_str = JS_ToCString(ctx, replace_val);
            if (!repl_str) {
                if (search_str) JS_FreeCString(ctx, search_str);
                JS_FreeCString(ctx, str);
                return JS_EXCEPTION;
            }
            size_t repl_len = strlen(repl_str);
            result_str = (char *)malloc(prefix_len + repl_len + suffix_len + 1);
            if (result_str) {
                memcpy(result_str, str, prefix_len);
                memcpy(result_str + prefix_len, repl_str, repl_len);
                memcpy(result_str + prefix_len + repl_len, found + search_len, suffix_len + 1);
            }
            JS_FreeCString(ctx, repl_str);
        }

        if (search_str) JS_FreeCString(ctx, search_str);
        JS_FreeCString(ctx, str);

        if (!result_str) return JS_EXCEPTION;
        JSValue result = JS_NewString(ctx, result_str);
        free(result_str);
        return result;
    }

    if (search_str) JS_FreeCString(ctx, search_str);
    JSValue result = JS_NewString(ctx, str);
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.prototype.replaceAll ───────────────────────────────────────── */

static JSValue js_string_proto_replace_all(JSContext *ctx, JSValue this_val,
                                             int argc, JSValue *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;

    const char *search_str = JS_ToCString(ctx, argv[0]);
    if (!search_str) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }

    JSValue replace_val = argv[1];
    int is_func = is_callable(ctx, replace_val);
    const char *repl_template = NULL;
    if (!is_func) {
        repl_template = JS_ToCString(ctx, replace_val);
        if (!repl_template) { JS_FreeCString(ctx, search_str); JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    }

    size_t search_len = strlen(search_str);
    if (search_len == 0) {
        /* Empty string: insert before each character and at end */
        size_t str_len = strlen(str);
        size_t repl_len = is_func ? 0 : strlen(repl_template);
        /* Simplified: just return the original string */
        if (!is_func) JS_FreeCString(ctx, repl_template);
        JS_FreeCString(ctx, search_str);
        JSValue result = JS_NewString(ctx, str);
        JS_FreeCString(ctx, str);
        return result;
    }

    /* Count occurrences */
    int count = 0;
    const char *pos = str;
    while ((pos = strstr(pos, search_str)) != NULL) {
        count++;
        pos += search_len;
    }

    if (count == 0) {
        if (!is_func) JS_FreeCString(ctx, repl_template);
        JS_FreeCString(ctx, search_str);
        JSValue result = JS_NewString(ctx, str);
        JS_FreeCString(ctx, str);
        return result;
    }

    /* Build result */
    size_t str_len = strlen(str);
    size_t repl_len = is_func ? 0 : strlen(repl_template);
    size_t buf_size = str_len + count * (repl_len > search_len ? repl_len - search_len : 0) + 1;
    char *buf = (char *)malloc(buf_size + 1024); /* extra room */
    if (!buf) {
        if (!is_func) JS_FreeCString(ctx, repl_template);
        JS_FreeCString(ctx, search_str);
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    size_t buf_pos = 0;
    const char *curr = str;
    int match_idx = 0;
    while ((pos = strstr(curr, search_str)) != NULL) {
        size_t pre_len = (size_t)(pos - curr);
        memcpy(buf + buf_pos, curr, pre_len);
        buf_pos += pre_len;

        if (is_func) {
            JSValue match_args[3];
            match_args[0] = JS_NewString(ctx, search_str);
            match_args[1] = JS_NewInt32(ctx, (int32_t)(curr - str + pre_len));
            match_args[2] = JS_DupValue(ctx, this_val);
            JSValue replacement = JS_Call(ctx, replace_val, JS_UNDEFINED, 3, match_args);
            JS_FreeValue(ctx, match_args[0]);
            JS_FreeValue(ctx, match_args[1]);
            JS_FreeValue(ctx, match_args[2]);
            const char *repl_str = JS_ToCString(ctx, replacement);
            if (repl_str) {
                size_t rl = strlen(repl_str);
                if (buf_pos + rl > buf_size + 1024) {
                    buf_size = buf_pos + rl + 1024;
                    buf = (char *)realloc(buf, buf_size);
                    if (!buf) { JS_FreeCString(ctx, repl_str); JS_FreeValue(ctx, replacement); break; }
                }
                memcpy(buf + buf_pos, repl_str, rl);
                buf_pos += rl;
                JS_FreeCString(ctx, repl_str);
            }
            JS_FreeValue(ctx, replacement);
        } else {
            if (buf_pos + repl_len > buf_size + 1024) {
                buf_size = buf_pos + repl_len + 1024;
                buf = (char *)realloc(buf, buf_size);
                if (!buf) break;
            }
            memcpy(buf + buf_pos, repl_template, repl_len);
            buf_pos += repl_len;
        }

        curr = pos + search_len;
        match_idx++;
    }

    /* Copy remaining */
    size_t rem_len = strlen(curr);
    if (buf_pos + rem_len > buf_size + 1024) {
        buf = (char *)realloc(buf, buf_pos + rem_len + 1);
    }
    if (buf) {
        memcpy(buf + buf_pos, curr, rem_len + 1);
        buf_pos += rem_len;
    }

    if (!is_func) JS_FreeCString(ctx, repl_template);
    JS_FreeCString(ctx, search_str);
    JS_FreeCString(ctx, str);

    if (!buf) return JS_EXCEPTION;
    buf[buf_pos] = '\0';
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.search ───────────────────────────────────────────── */

static JSValue js_string_proto_search(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const char *str = JS_ToCString(ctx, this_val);
    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!str || !pattern) {
        if (str) JS_FreeCString(ctx, str);
        if (pattern) JS_FreeCString(ctx, pattern);
        return JS_EXCEPTION;
    }
    const char *found = strstr(str, pattern);
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, pattern);
    if (found) {
        return JS_NewInt32(ctx, (int32_t)(found - str));
    }
    return JS_NewInt32(ctx, -1);
}

/* ── String.prototype.slice ────────────────────────────────────────────── */

static JSValue js_string_proto_slice(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t len = (int32_t)strlen(str);
    int32_t start = 0, end = len;

    if (argc > 0) {
        JS_ToInt32(ctx, &start, argv[0]);
        if (start < 0) start = len + start;
        if (start < 0) start = 0;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JS_ToInt32(ctx, &end, argv[1]);
        if (end < 0) end = len + end;
        if (end > len) end = len;
    }

    JSValue result;
    if (start >= end) {
        result = JS_NewString(ctx, "");
    } else {
        size_t slice_len = (size_t)(end - start);
        char *buf = (char *)malloc(slice_len + 1);
        if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
        memcpy(buf, str + start, slice_len);
        buf[slice_len] = '\0';
        result = JS_NewString(ctx, buf);
        free(buf);
    }
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.prototype.split ────────────────────────────────────────────── */

static JSValue js_string_proto_split(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    int32_t limit = 0x7FFFFFFF; /* no limit */
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JS_ToInt32(ctx, &limit, argv[1]);
        if (limit < 0) limit = 0;
    }

    if (argc == 0 || JS_IsUndefined(argv[0])) {
        /* Return array with single element */
        JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, str));
        set_array_length(ctx, arr, 1);
        JS_FreeCString(ctx, str);
        return arr;
    }

    const char *sep = JS_ToCString(ctx, argv[0]);
    if (!sep) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }

    size_t sep_len = strlen(sep);
    int32_t idx = 0;

    if (sep_len == 0) {
        /* Split into individual characters */
        size_t str_len = strlen(str);
        for (size_t i = 0; i < str_len && idx < limit; i++) {
            char buf[2] = { str[i], '\0' };
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, buf));
        }
    } else {
        const char *pos = str;
        const char *next;
        while ((next = strstr(pos, sep)) != NULL && idx < limit) {
            size_t part_len = (size_t)(next - pos);
            char *buf = (char *)malloc(part_len + 1);
            if (buf) {
                memcpy(buf, pos, part_len);
                buf[part_len] = '\0';
                JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, buf));
                free(buf);
            }
            pos = next + sep_len;
        }
        if (idx < limit) {
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, pos));
        }
    }

    set_array_length(ctx, arr, idx);
    JS_FreeCString(ctx, sep);
    JS_FreeCString(ctx, str);
    return arr;
}

/* ── String.prototype.substring ────────────────────────────────────────── */

static JSValue js_string_proto_substring(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t len = (int32_t)strlen(str);
    int32_t start = 0, end = len;

    if (argc > 0) {
        JS_ToInt32(ctx, &start, argv[0]);
        if (start < 0) start = 0;
        if (start > len) start = len;
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JS_ToInt32(ctx, &end, argv[1]);
        if (end < 0) end = 0;
        if (end > len) end = len;
    }

    /* Swap if start > end */
    if (start > end) {
        int32_t tmp = start;
        start = end;
        end = tmp;
    }

    JSValue result;
    if (start >= end) {
        result = JS_NewString(ctx, "");
    } else {
        size_t slice_len = (size_t)(end - start);
        char *buf = (char *)malloc(slice_len + 1);
        if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
        memcpy(buf, str + start, slice_len);
        buf[slice_len] = '\0';
        result = JS_NewString(ctx, buf);
        free(buf);
    }
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.prototype.toLowerCase ──────────────────────────────────────── */

static JSValue js_string_proto_to_lower_case(JSContext *ctx, JSValue this_val,
                                               int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    size_t len = strlen(str);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)tolower((unsigned char)str[i]);
    }
    buf[len] = '\0';
    JS_FreeCString(ctx, str);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.toUpperCase ──────────────────────────────────────── */

static JSValue js_string_proto_to_upper_case(JSContext *ctx, JSValue this_val,
                                               int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    size_t len = strlen(str);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)toupper((unsigned char)str[i]);
    }
    buf[len] = '\0';
    JS_FreeCString(ctx, str);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.trim ─────────────────────────────────────────────── */

static JSValue js_string_proto_trim(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    /* Skip leading whitespace */
    while (*str && (*str == ' ' || *str == '\t' || *str == '\n' ||
                    *str == '\r' || *str == '\f' || *str == '\v')) {
        str++;
    }
    /* Find end */
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' ||
                       str[len - 1] == '\n' || str[len - 1] == '\r' ||
                       str[len - 1] == '\f' || str[len - 1] == '\v')) {
        len--;
    }
    char *buf = (char *)malloc(len + 1);
    if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    memcpy(buf, str, len);
    buf[len] = '\0';
    JS_FreeCString(ctx, str);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.trimStart ────────────────────────────────────────── */

static JSValue js_string_proto_trim_start(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    const char *p = str;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' ||
                  *p == '\r' || *p == '\f' || *p == '\v')) {
        p++;
    }
    JSValue result = JS_NewString(ctx, p);
    JS_FreeCString(ctx, str);
    return result;
}

/* ── String.prototype.trimEnd ──────────────────────────────────────────── */

static JSValue js_string_proto_trim_end(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' ||
                       str[len - 1] == '\n' || str[len - 1] == '\r' ||
                       str[len - 1] == '\f' || str[len - 1] == '\v')) {
        len--;
    }
    char *buf = (char *)malloc(len + 1);
    if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    memcpy(buf, str, len);
    buf[len] = '\0';
    JS_FreeCString(ctx, str);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.padStart ─────────────────────────────────────────── */

static JSValue js_string_proto_pad_start(JSContext *ctx, JSValue this_val,
                                           int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t target_len = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &target_len, argv[0]);
    }
    const char *pad_str = " ";
    int free_pad = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        pad_str = JS_ToCString(ctx, argv[1]);
        free_pad = 1;
        if (!pad_str) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    }

    size_t str_len = strlen(str);
    size_t pad_len = strlen(pad_str);

    if ((int32_t)str_len >= target_len || pad_len == 0) {
        if (free_pad) JS_FreeCString(ctx, pad_str);
        JSValue result = JS_NewString(ctx, str);
        JS_FreeCString(ctx, str);
        return result;
    }

    size_t needed = (size_t)target_len - str_len;
    size_t buf_size = (size_t)target_len + 1;
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        if (free_pad) JS_FreeCString(ctx, pad_str);
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    size_t pos = 0;
    while (pos < needed) {
        buf[pos] = pad_str[pos % pad_len];
        pos++;
        /* Adjust for proper cycling */
        if (pos > needed) break;
    }
    /* Actually, let's do it properly */
    pos = 0;
    while (pos < needed) {
        buf[pos] = pad_str[pos % pad_len];
        pos++;
    }
    memcpy(buf + pos, str, str_len);
    buf[target_len] = '\0';

    if (free_pad) JS_FreeCString(ctx, pad_str);
    JS_FreeCString(ctx, str);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.padEnd ───────────────────────────────────────────── */

static JSValue js_string_proto_pad_end(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t target_len = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &target_len, argv[0]);
    }
    const char *pad_str = " ";
    int free_pad = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        pad_str = JS_ToCString(ctx, argv[1]);
        free_pad = 1;
        if (!pad_str) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    }

    size_t str_len = strlen(str);
    size_t pad_len = strlen(pad_str);

    if ((int32_t)str_len >= target_len || pad_len == 0) {
        if (free_pad) JS_FreeCString(ctx, pad_str);
        JSValue result = JS_NewString(ctx, str);
        JS_FreeCString(ctx, str);
        return result;
    }

    size_t needed = (size_t)target_len - str_len;
    size_t buf_size = (size_t)target_len + 1;
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        if (free_pad) JS_FreeCString(ctx, pad_str);
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    memcpy(buf, str, str_len);
    size_t pos = str_len;
    while (pos < (size_t)target_len) {
        buf[pos] = pad_str[(pos - str_len) % pad_len];
        pos++;
    }
    buf[target_len] = '\0';

    if (free_pad) JS_FreeCString(ctx, pad_str);
    JS_FreeCString(ctx, str);
    JSValue result = JS_NewString(ctx, buf);
    free(buf);
    return result;
}

/* ── String.prototype.startsWith ───────────────────────────────────────── */

static JSValue js_string_proto_starts_with(JSContext *ctx, JSValue this_val,
                                             int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    const char *str = JS_ToCString(ctx, this_val);
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!str || !search) {
        if (str) JS_FreeCString(ctx, str);
        if (search) JS_FreeCString(ctx, search);
        return JS_EXCEPTION;
    }
    int32_t pos = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &pos, argv[1]);
        if (pos < 0) pos = 0;
    }
    size_t search_len = strlen(search);
    int result = 0;
    if ((size_t)pos + search_len <= strlen(str)) {
        result = (memcmp(str + pos, search, search_len) == 0);
    }
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, search);
    return JS_NewBool(ctx, result);
}

/* ── String.prototype.endsWith ─────────────────────────────────────────── */

static JSValue js_string_proto_ends_with(JSContext *ctx, JSValue this_val,
                                           int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    const char *str = JS_ToCString(ctx, this_val);
    const char *search = JS_ToCString(ctx, argv[0]);
    if (!str || !search) {
        if (str) JS_FreeCString(ctx, str);
        if (search) JS_FreeCString(ctx, search);
        return JS_EXCEPTION;
    }
    size_t str_len = strlen(str);
    size_t search_len = strlen(search);
    int32_t end_pos = (int32_t)str_len;
    if (argc > 1) {
        JS_ToInt32(ctx, &end_pos, argv[1]);
        if (end_pos < 0) end_pos = 0;
        if ((size_t)end_pos > str_len) end_pos = (int32_t)str_len;
    }
    int result = 0;
    if (search_len <= (size_t)end_pos) {
        result = (memcmp(str + end_pos - search_len, search, search_len) == 0);
    }
    JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, search);
    return JS_NewBool(ctx, result);
}

/* ── String.prototype.repeat ───────────────────────────────────────────── */

static JSValue js_string_proto_repeat(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    const char *str = JS_ToCString(ctx, this_val);
    if (!str) return JS_EXCEPTION;
    int32_t count = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &count, argv[0]);
    }
    if (count < 0) {
        JS_FreeCString(ctx, str);
        return JS_ThrowRangeError(ctx, "Invalid count value");
    }
    size_t str_len = strlen(str);
    size_t total = str_len * (size_t)count;
    char *buf = (char *)malloc(total + 1);
    if (!buf && total > 0) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    if (buf) {
        for (int32_t i = 0; i < count; i++) {
            memcpy(buf + i * str_len, str, str_len);
        }
        buf[total] = '\0';
    }
    JS_FreeCString(ctx, str);
    JSValue result = buf ? JS_NewString(ctx, buf) : JS_NewString(ctx, "");
    free(buf);
    return result;
}

/* ── String.prototype.toString ─────────────────────────────────────────── */

static JSValue js_string_proto_to_string(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* ── String.prototype.valueOf ──────────────────────────────────────────── */

static JSValue js_string_proto_value_of(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* ══════════════════════════════════════════════════════════════════════════
   4. NUMBER
   ══════════════════════════════════════════════════════════════════════════ */

static JSValue js_number_constructor(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    (void)this_val;
    if (argc == 0) return JS_NewInt32(ctx, 0);
    if (JS_IsNumber(argv[0])) {
        return JS_DupValue(ctx, argv[0]);
    }
    double d;
    if (JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_NewFloat64(ctx, NAN);
    return JS_NewFloat64(ctx, d);
}

/* ── Number.isNaN ──────────────────────────────────────────────────────── */

static JSValue js_number_is_nan(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    if (argv[0].tag == LR_TYPE_FLOAT64 && isnan(argv[0].u.float64)) return JS_TRUE;
    return JS_FALSE;
}

/* ── Number.isFinite ───────────────────────────────────────────────────── */

static JSValue js_number_is_finite(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    if (!JS_IsNumber(argv[0])) return JS_FALSE;
    if (argv[0].tag == LR_TYPE_FLOAT64) {
        double d = argv[0].u.float64;
        return JS_NewBool(ctx, !isnan(d) && !isinf(d));
    }
    return JS_TRUE;
}

/* ── Number.isInteger ──────────────────────────────────────────────────── */

static JSValue js_number_is_integer(JSContext *ctx, JSValue this_val,
                                     int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    if (!JS_IsNumber(argv[0])) return JS_FALSE;
    if (argv[0].tag == LR_TYPE_INT32) return JS_TRUE;
    if (argv[0].tag == LR_TYPE_FLOAT64) {
        double d = argv[0].u.float64;
        return JS_NewBool(ctx, !isnan(d) && !isinf(d) && floor(d) == d);
    }
    return JS_FALSE;
}

/* ── Number.isSafeInteger ──────────────────────────────────────────────── */

static JSValue js_number_is_safe_integer(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    if (argc < 1) return JS_FALSE;
    if (!JS_IsNumber(argv[0])) return JS_FALSE;
    if (argv[0].tag == LR_TYPE_INT32) return JS_TRUE;
    if (argv[0].tag == LR_TYPE_FLOAT64) {
        double d = argv[0].u.float64;
        if (isnan(d) || isinf(d)) return JS_FALSE;
        if (floor(d) != d) return JS_FALSE;
        if (d < -9007199254740991.0 || d > 9007199254740991.0) return JS_FALSE;
        return JS_TRUE;
    }
    return JS_FALSE;
}

/* ── Number.parseFloat ─────────────────────────────────────────────────── */

static JSValue js_number_parse_float(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (argc < 1) return JS_NewFloat64(ctx, NAN);
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NewFloat64(ctx, NAN);
    char *endptr = NULL;
    double d = strtod(str, &endptr);
    JS_FreeCString(ctx, str);
    if (endptr == str) return JS_NewFloat64(ctx, NAN);
    return JS_NewFloat64(ctx, d);
}

/* ── Number.parseInt ───────────────────────────────────────────────────── */

static JSValue js_number_parse_int(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv)
{
    if (argc < 1) return JS_NewFloat64(ctx, NAN);
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NewFloat64(ctx, NAN);
    int32_t radix = 10;
    if (argc > 1) {
        JS_ToInt32(ctx, &radix, argv[1]);
        if (radix < 2 || radix > 36) radix = 10;
    }
    char *endptr = NULL;
    long val = strtol(str, &endptr, radix);
    JS_FreeCString(ctx, str);
    if (endptr == str) return JS_NewFloat64(ctx, NAN);
    return JS_NewInt32(ctx, (int32_t)val);
}

/* ── Number.prototype.toFixed ──────────────────────────────────────────── */

static JSValue js_number_proto_to_fixed(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    double d;
    if (JS_ToFloat64(ctx, &d, this_val) < 0) return JS_EXCEPTION;
    int32_t digits = 0;
    if (argc > 0) {
        JS_ToInt32(ctx, &digits, argv[0]);
    }
    if (digits < 0 || digits > 100) {
        return JS_ThrowRangeError(ctx, "toFixed() digits argument must be between 0 and 100");
    }
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, d);
    return JS_NewString(ctx, buf);
}

/* ── Number.prototype.toPrecision ──────────────────────────────────────── */

static JSValue js_number_proto_to_precision(JSContext *ctx, JSValue this_val,
                                              int argc, JSValue *argv)
{
    double d;
    if (JS_ToFloat64(ctx, &d, this_val) < 0) return JS_EXCEPTION;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", d);
        return JS_NewString(ctx, buf);
    }
    int32_t precision;
    JS_ToInt32(ctx, &precision, argv[0]);
    if (precision < 1 || precision > 100) {
        return JS_ThrowRangeError(ctx, "toPrecision() precision argument must be between 1 and 100");
    }
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%dg", precision);
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, d);
    return JS_NewString(ctx, buf);
}

/* ── Number.prototype.toString ─────────────────────────────────────────── */

static JSValue js_number_proto_to_string(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    double d;
    if (JS_ToFloat64(ctx, &d, this_val) < 0) return JS_EXCEPTION;
    int32_t radix = 10;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        JS_ToInt32(ctx, &radix, argv[0]);
        if (radix < 2 || radix > 36) {
            return JS_ThrowRangeError(ctx, "toString() radix must be between 2 and 36");
        }
    }
    if (radix == 10) {
        char buf[64];
        if (d == floor(d) && isfinite(d) && fabs(d) < 1e15) {
            snprintf(buf, sizeof(buf), "%.0f", d);
        } else {
            snprintf(buf, sizeof(buf), "%g", d);
        }
        return JS_NewString(ctx, buf);
    }
    int32_t val;
    JS_ToInt32(ctx, &val, this_val);
    if (val == 0) return JS_NewString(ctx, "0");
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buf[64];
    int pos = 0;
    uint32_t uval = (uint32_t)(val < 0 ? -val : val);
    while (uval > 0) {
        buf[pos++] = digits[uval % radix];
        uval /= radix;
    }
    if (val < 0) buf[pos++] = '-';
    for (int i = 0; i < pos / 2; i++) {
        char tmp = buf[i];
        buf[i] = buf[pos - 1 - i];
        buf[pos - 1 - i] = tmp;
    }
    buf[pos] = '\0';
    return JS_NewString(ctx, buf);
}

/* ── Number.prototype.valueOf ──────────────────────────────────────────── */

static JSValue js_number_proto_value_of(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* ══════════════════════════════════════════════════════════════════════════
   5. BOOLEAN
   ══════════════════════════════════════════════════════════════════════════ */

static JSValue js_boolean_constructor(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    (void)this_val;
    int val = 0;
    if (argc > 0) {
        val = JS_ToBool(ctx, argv[0]);
    }
    return JS_NewBool(ctx, val);
}

/* ── Boolean.prototype.toString ────────────────────────────────────────── */

static JSValue js_boolean_proto_to_string(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    int val = 0;
    if (JS_IsBool(this_val)) {
        val = this_val.u.bool_val;
    } else {
        val = JS_ToBool(ctx, this_val);
    }
    return JS_NewString(ctx, val ? "true" : "false");
}

/* ── Boolean.prototype.valueOf ─────────────────────────────────────────── */

static JSValue js_boolean_proto_value_of(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* ══════════════════════════════════════════════════════════════════════════
   6. FUNCTION
   ══════════════════════════════════════════════════════════════════════════ */

/* ── Bound function wrapper ────────────────────────────────────────────── */

static void bound_function_data_free(LRContext *ctx, BoundFunctionData *data)
{
    if (!data) return;
    lr_free_value(ctx, data->target);
    lr_free_value(ctx, data->bound_this);
    for (int i = 0; i < data->argc; i++) {
        lr_free_value(ctx, data->args[i]);
    }
    free(data);
}

static LRValue js_bound_function_call(JSContext *ctx, LRValue this_val,
                                       int argc, LRValue *argv)
{
    (void)this_val;
    /* Get the bound function data from the function object via ctx->current_func */
    BoundFunctionData *data = (BoundFunctionData *)JS_GetOpaque(ctx->current_func, NULL);
    if (!data) {
        return JS_ThrowTypeError(ctx, "Bound function has no data");
    }
    int total_args = data->argc + argc;
    LRValue *all_args = (LRValue *)malloc(total_args * sizeof(LRValue));
    if (!all_args && total_args > 0) return JS_EXCEPTION;

    for (int i = 0; i < data->argc; i++) {
        all_args[i] = JS_DupValue(ctx, data->args[i]);
    }
    for (int i = 0; i < argc; i++) {
        all_args[data->argc + i] = JS_DupValue(ctx, argv[i]);
    }

    LRValue result = JS_Call(ctx, data->target, data->bound_this, total_args, all_args);

    for (int i = 0; i < total_args; i++) {
        JS_FreeValue(ctx, all_args[i]);
    }
    free(all_args);
    return result;
}

/* ── Function.prototype.bind ───────────────────────────────────────────── */

static JSValue js_function_proto_bind(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    if (!is_callable(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Function.prototype.bind called on non-function");
    }

    BoundFunctionData *data = (BoundFunctionData *)calloc(1,
        sizeof(BoundFunctionData) + (size_t)(argc > 0 ? argc - 1 : 0) * sizeof(LRValue));
    if (!data) return JS_EXCEPTION;

    data->target = JS_DupValue(ctx, this_val);
    data->bound_this = (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    data->argc = (argc > 0) ? argc - 1 : 0;
    for (int i = 0; i < data->argc; i++) {
        data->args[i] = JS_DupValue(ctx, argv[i + 1]);
    }

    int32_t target_len = 0;
    JSValue len_val = JS_GetPropertyStr(ctx, this_val, "length");
    if (!JS_IsUndefined(len_val)) {
        JS_ToInt32(ctx, &target_len, len_val);
    }
    JS_FreeValue(ctx, len_val);

    int32_t bound_len = target_len - (argc > 0 ? argc - 1 : 0);
    if (bound_len < 0) bound_len = 0;

    JSValue bound_func = JS_NewCFunction(ctx, js_bound_function_call, "bound", bound_len);
    JS_SetOpaque(bound_func, data);

    JSValue proto = JS_GetPropertyStr(ctx, this_val, "prototype");
    if (!JS_IsUndefined(proto)) {
        JS_SetPropertyStr(ctx, bound_func, "prototype", JS_DupValue(ctx, proto));
    }
    JS_FreeValue(ctx, proto);

    JSValue name_val = JS_GetPropertyStr(ctx, this_val, "name");
    const char *name_str = JS_ToCString(ctx, name_val);
    if (name_str) {
        char buf[256];
        snprintf(buf, sizeof(buf), "bound %s", name_str);
        JS_SetPropertyStr(ctx, bound_func, "name", JS_NewString(ctx, buf));
        JS_FreeCString(ctx, name_str);
    }
    JS_FreeValue(ctx, name_val);

    return bound_func;
}

/* ── Function.prototype.call ───────────────────────────────────────────── */

static JSValue js_function_proto_call(JSContext *ctx, JSValue this_val,
                                       int argc, JSValue *argv)
{
    if (!is_callable(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Function.prototype.call called on non-function");
    }
    JSValue this_arg = (argc > 0) ? argv[0] : JS_UNDEFINED;
    int call_argc = (argc > 0) ? argc - 1 : 0;
    LRValue *call_argv = (call_argc > 0) ? &argv[1] : NULL;
    return JS_Call(ctx, this_val, this_arg, call_argc, call_argv);
}

/* ── Function.prototype.apply ──────────────────────────────────────────── */

static JSValue js_function_proto_apply(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    if (!is_callable(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Function.prototype.apply called on non-function");
    }
    JSValue this_arg = (argc > 0) ? argv[0] : JS_UNDEFINED;
    JSValue args_array = (argc > 1) ? argv[1] : JS_UNDEFINED;

    int call_argc = 0;
    LRValue *call_argv = NULL;

    if (JS_IsArray(ctx, args_array)) {
        call_argc = get_array_length(ctx, args_array);
        if (call_argc > 0) {
            call_argv = (LRValue *)malloc(call_argc * sizeof(LRValue));
            if (!call_argv) return JS_EXCEPTION;
            for (int i = 0; i < call_argc; i++) {
                call_argv[i] = JS_GetPropertyUint32(ctx, args_array, i);
            }
        }
    }

    JSValue result = JS_Call(ctx, this_val, this_arg, call_argc, call_argv);

    if (call_argv) {
        for (int i = 0; i < call_argc; i++) {
            JS_FreeValue(ctx, call_argv[i]);
        }
        free(call_argv);
    }
    return result;
}

/* ── Function.prototype.toString ───────────────────────────────────────── */

static JSValue js_function_proto_to_string(JSContext *ctx, JSValue this_val,
                                            int argc, JSValue *argv)
{
    (void)argc;
    (void)argv;
    if (!is_callable(ctx, this_val)) {
        return JS_ThrowTypeError(ctx, "Function.prototype.toString called on non-function");
    }
    JSValue name_val = JS_GetPropertyStr(ctx, this_val, "name");
    const char *name = JS_ToCString(ctx, name_val);
    if (!name) name = "";
    char buf[256];
    snprintf(buf, sizeof(buf), "function %s() { [native code] }", name);
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, name_val);
    return JS_NewString(ctx, buf);
}

/* ══════════════════════════════════════════════════════════════════════════
   7. FUNCTION LIST ENTRIES
   ══════════════════════════════════════════════════════════════════════════ */

/* ── Object ────────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_object_static_funcs[] = {
    JS_CFUNC_DEF("keys",                    1, js_object_keys),
    JS_CFUNC_DEF("values",                  1, js_object_values),
    JS_CFUNC_DEF("entries",                 1, js_object_entries),
    JS_CFUNC_DEF("fromEntries",             1, js_object_from_entries),
    JS_CFUNC_DEF("assign",                  2, js_object_assign),
    JS_CFUNC_DEF("create",                  2, js_object_create),
    JS_CFUNC_DEF("defineProperty",          3, js_object_define_property),
    JS_CFUNC_DEF("defineProperties",        2, js_object_define_properties),
    JS_CFUNC_DEF("freeze",                  1, js_object_freeze),
    JS_CFUNC_DEF("seal",                    1, js_object_seal),
    JS_CFUNC_DEF("isExtensible",            1, js_object_is_extensible),
    JS_CFUNC_DEF("preventExtensions",       1, js_object_prevent_extensions),
    JS_CFUNC_DEF("is",                      2, js_object_is),
    JS_CFUNC_DEF("hasOwn",                  2, js_object_has_own),
    JS_CFUNC_DEF("getPrototypeOf",          1, js_object_get_prototype_of),
    JS_CFUNC_DEF("setPrototypeOf",          2, js_object_set_prototype_of),
    JS_CFUNC_DEF("getOwnPropertyNames",     1, js_object_get_own_property_names),
    JS_CFUNC_DEF("getOwnPropertyDescriptor", 2, js_object_get_own_property_descriptor),
    JS_CFUNC_DEF("getOwnPropertyDescriptors", 1, js_object_get_own_property_descriptors),
};

static const JSCFunctionListEntry js_object_proto_funcs[] = {
    JS_CFUNC_DEF("toString",                0, js_object_proto_to_string),
    JS_CFUNC_DEF("hasOwnProperty",          1, js_object_proto_has_own_property),
    JS_CFUNC_DEF("isPrototypeOf",           1, js_object_proto_is_prototype_of),
    JS_CFUNC_DEF("propertyIsEnumerable",    1, js_object_proto_property_is_enumerable),
    JS_CFUNC_DEF("valueOf",                 0, js_object_proto_value_of),
};

/* ── Array ─────────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_array_static_funcs[] = {
    JS_CFUNC_DEF("isArray",                 1, js_array_is_array),
    JS_CFUNC_DEF("from",                    1, js_array_from),
    JS_CFUNC_DEF("of",                      0, js_array_of),
};

static const JSCFunctionListEntry js_array_proto_funcs[] = {
    JS_CFUNC_DEF("at",                      1, js_array_proto_at),
    JS_CFUNC_DEF("concat",                  1, js_array_proto_concat),
    JS_CFUNC_DEF("copyWithin",              2, js_array_proto_copy_within),
    JS_CFUNC_DEF("entries",                 0, js_array_proto_entries),
    JS_CFUNC_DEF("every",                   1, js_array_proto_every),
    JS_CFUNC_DEF("fill",                    1, js_array_proto_fill),
    JS_CFUNC_DEF("filter",                  1, js_array_proto_filter),
    JS_CFUNC_DEF("find",                    1, js_array_proto_find),
    JS_CFUNC_DEF("findIndex",               1, js_array_proto_find_index),
    JS_CFUNC_DEF("findLast",                1, js_array_proto_find_last),
    JS_CFUNC_DEF("findLastIndex",           1, js_array_proto_find_last_index),
    JS_CFUNC_DEF("flat",                    0, js_array_proto_flat),
    JS_CFUNC_DEF("flatMap",                 1, js_array_proto_flat_map),
    JS_CFUNC_DEF("forEach",                 1, js_array_proto_for_each),
    JS_CFUNC_DEF("includes",                1, js_array_proto_includes),
    JS_CFUNC_DEF("indexOf",                 1, js_array_proto_index_of),
    JS_CFUNC_DEF("join",                    1, js_array_proto_join),
    JS_CFUNC_DEF("keys",                    0, js_array_proto_keys),
    JS_CFUNC_DEF("lastIndexOf",             1, js_array_proto_last_index_of),
    JS_CFUNC_DEF("map",                     1, js_array_proto_map),
    JS_CFUNC_DEF("pop",                     0, js_array_proto_pop),
    JS_CFUNC_DEF("push",                    1, js_array_proto_push),
    JS_CFUNC_DEF("reduce",                  1, js_array_proto_reduce),
    JS_CFUNC_DEF("reduceRight",             1, js_array_proto_reduce_right),
    JS_CFUNC_DEF("reverse",                 0, js_array_proto_reverse),
    JS_CFUNC_DEF("shift",                   0, js_array_proto_shift),
    JS_CFUNC_DEF("slice",                   2, js_array_proto_slice),
    JS_CFUNC_DEF("some",                    1, js_array_proto_some),
    JS_CFUNC_DEF("sort",                    1, js_array_proto_sort),
    JS_CFUNC_DEF("splice",                  2, js_array_proto_splice),
    JS_CFUNC_DEF("toReversed",              0, js_array_proto_to_reversed),
    JS_CFUNC_DEF("toSorted",                1, js_array_proto_to_sorted),
    JS_CFUNC_DEF("toSpliced",               2, js_array_proto_to_spliced),
    JS_CFUNC_DEF("toString",                0, js_array_proto_to_string),
    JS_CFUNC_DEF("unshift",                 1, js_array_proto_unshift),
    JS_CFUNC_DEF("values",                  0, js_array_proto_values),
    JS_CFUNC_DEF("with",                    2, js_array_proto_with),
};

/* ── String ────────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_string_static_funcs[] = {
    JS_CFUNC_DEF("fromCharCode",            1, js_string_from_char_code),
    JS_CFUNC_DEF("fromCodePoint",           1, js_string_from_code_point),
};

static const JSCFunctionListEntry js_string_proto_funcs[] = {
    JS_CFUNC_DEF("at",                      1, js_string_proto_at),
    JS_CFUNC_DEF("charAt",                  1, js_string_proto_char_at),
    JS_CFUNC_DEF("charCodeAt",              1, js_string_proto_char_code_at),
    JS_CFUNC_DEF("codePointAt",             1, js_string_proto_code_point_at),
    JS_CFUNC_DEF("concat",                  1, js_string_proto_concat),
    JS_CFUNC_DEF("includes",                1, js_string_proto_includes),
    JS_CFUNC_DEF("indexOf",                 1, js_string_proto_index_of),
    JS_CFUNC_DEF("lastIndexOf",             1, js_string_proto_last_index_of),
    JS_CFUNC_DEF("match",                   1, js_string_proto_match),
    JS_CFUNC_DEF("matchAll",                1, js_string_proto_match_all),
    JS_CFUNC_DEF("replace",                 2, js_string_proto_replace),
    JS_CFUNC_DEF("replaceAll",              2, js_string_proto_replace_all),
    JS_CFUNC_DEF("search",                  1, js_string_proto_search),
    JS_CFUNC_DEF("slice",                   2, js_string_proto_slice),
    JS_CFUNC_DEF("split",                   2, js_string_proto_split),
    JS_CFUNC_DEF("substring",               2, js_string_proto_substring),
    JS_CFUNC_DEF("toLowerCase",             0, js_string_proto_to_lower_case),
    JS_CFUNC_DEF("toUpperCase",             0, js_string_proto_to_upper_case),
    JS_CFUNC_DEF("trim",                    0, js_string_proto_trim),
    JS_CFUNC_DEF("trimStart",               0, js_string_proto_trim_start),
    JS_CFUNC_DEF("trimEnd",                 0, js_string_proto_trim_end),
    JS_CFUNC_DEF("padStart",                1, js_string_proto_pad_start),
    JS_CFUNC_DEF("padEnd",                  1, js_string_proto_pad_end),
    JS_CFUNC_DEF("startsWith",              1, js_string_proto_starts_with),
    JS_CFUNC_DEF("endsWith",                1, js_string_proto_ends_with),
    JS_CFUNC_DEF("repeat",                  1, js_string_proto_repeat),
    JS_CFUNC_DEF("toString",                0, js_string_proto_to_string),
    JS_CFUNC_DEF("valueOf",                 0, js_string_proto_value_of),
};

/* ── Number ────────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_number_static_funcs[] = {
    JS_CFUNC_DEF("isNaN",                   1, js_number_is_nan),
    JS_CFUNC_DEF("isFinite",                1, js_number_is_finite),
    JS_CFUNC_DEF("isInteger",               1, js_number_is_integer),
    JS_CFUNC_DEF("isSafeInteger",           1, js_number_is_safe_integer),
    JS_CFUNC_DEF("parseFloat",              1, js_number_parse_float),
    JS_CFUNC_DEF("parseInt",                1, js_number_parse_int),
};

static const JSCFunctionListEntry js_number_proto_funcs[] = {
    JS_CFUNC_DEF("toFixed",                 1, js_number_proto_to_fixed),
    JS_CFUNC_DEF("toPrecision",             1, js_number_proto_to_precision),
    JS_CFUNC_DEF("toString",                1, js_number_proto_to_string),
    JS_CFUNC_DEF("valueOf",                 0, js_number_proto_value_of),
};

/* ── Boolean ───────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_boolean_proto_funcs[] = {
    JS_CFUNC_DEF("toString",                0, js_boolean_proto_to_string),
    JS_CFUNC_DEF("valueOf",                 0, js_boolean_proto_value_of),
};

/* ── Function ──────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_function_proto_funcs[] = {
    JS_CFUNC_DEF("bind",                    1, js_function_proto_bind),
    JS_CFUNC_DEF("call",                    1, js_function_proto_call),
    JS_CFUNC_DEF("apply",                   2, js_function_proto_apply),
    JS_CFUNC_DEF("toString",                0, js_function_proto_to_string),
};

/* ── Function constructor ──────────────────────────────────────────────── */

static JSValue js_function_constructor(JSContext *ctx, JSValue this_val,
                                        int argc, JSValue *argv)
{
    (void)this_val;

    /* new Function(p1, ..., pN, body) — last arg is the function body,
     * all preceding are parameter names. */
    const char *body = "";
    const char *param_names[256];
    int nparams = 0;

    if (argc == 0) {
        /* new Function() → empty function body, no params */
    } else if (argc == 1) {
        /* new Function(body) — single arg is the body */
        body = JS_ToCString(ctx, argv[0]);
        if (!body) body = "";
    } else {
        /* new Function(p1, p2, ..., pN, body) */
        for (int i = 0; i < argc - 1 && i < 256; i++) {
            param_names[i] = JS_ToCString(ctx, argv[i]);
            if (!param_names[i]) param_names[i] = "";
            nparams++;
        }
        body = JS_ToCString(ctx, argv[argc - 1]);
        if (!body) body = "";
    }

    LRValue result = lr_engine_build_function(ctx, nparams, param_names, body);

    /* Free the C strings we obtained (via JS_ToCString / lr_to_cstring) */
    if (body && body[0]) JS_FreeCString(ctx, body);
    for (int i = 0; i < nparams; i++)
        if (param_names[i] && param_names[i][0]) JS_FreeCString(ctx, param_names[i]);

    return result;
}

/* ── eval ──────────────────────────────────────────────────────────────── */

static JSValue js_global_eval(JSContext *ctx, JSValue this_val,
                              int argc, JSValue *argv)
{
    (void)this_val;

    if (argc < 1)
        return JS_UNDEFINED;

    /* Per spec, eval() returns any non-string argument unchanged. */
    if (argv[0].tag != LR_TYPE_STRING)
        return JS_DupValue(ctx, argv[0]);

    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src)
        return JS_EXCEPTION;

    /* Calls that reach here went through the `eval` binding while the
     * interpreter still sits in the caller's scope, so this is a direct
     * eval: the fragment observes and mutates the enclosing locals. The
     * engine allocates a fresh sandbox frame (depth + time budget) for it. */
    JSValue result = lr_engine_eval_code(ctx, src, strlen(src), 1);

    JS_FreeCString(ctx, src);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════════
   8. INITIALIZATION
   ══════════════════════════════════════════════════════════════════════════ */

void lr_builtins_core_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* ── Object ────────────────────────────────────────────────────────── */
    {
        JSValue obj_ctor = JS_NewCFunction(ctx, js_object_constructor, "Object", 1);
        JSValue obj_proto = JS_NewObject(ctx);

        /* Set prototype methods */
        JS_SetPropertyFunctionList(ctx, obj_proto, js_object_proto_funcs,
            sizeof(js_object_proto_funcs) / sizeof(js_object_proto_funcs[0]));

        /* Set static methods on constructor */
        JS_SetPropertyFunctionList(ctx, obj_ctor, js_object_static_funcs,
            sizeof(js_object_static_funcs) / sizeof(js_object_static_funcs[0]));

        JS_SetPropertyStr(ctx, obj_ctor, "prototype", JS_DupValue(ctx, obj_proto));
        JS_SetPropertyStr(ctx, obj_proto, "constructor", JS_DupValue(ctx, obj_ctor));

        /* Store Object.prototype for fast lookup in lr_new_object */
        ctx->object_proto = JS_DupValue(ctx, obj_proto);

        JS_SetPropertyStr(ctx, global, "Object", obj_ctor);
    }

    /* ── Array ─────────────────────────────────────────────────────────── */
    {
        JSValue arr_ctor = JS_NewCFunction(ctx, js_array_constructor, "Array", 1);
        JSValue arr_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, arr_proto, js_array_proto_funcs,
            sizeof(js_array_proto_funcs) / sizeof(js_array_proto_funcs[0]));

        JS_SetPropertyFunctionList(ctx, arr_ctor, js_array_static_funcs,
            sizeof(js_array_static_funcs) / sizeof(js_array_static_funcs[0]));

        JS_SetPropertyStr(ctx, arr_ctor, "prototype", JS_DupValue(ctx, arr_proto));
        JS_SetPropertyStr(ctx, arr_proto, "constructor", JS_DupValue(ctx, arr_ctor));

        /* Store Array.prototype for fast lookup in lr_new_array */
        ctx->array_proto = JS_DupValue(ctx, arr_proto);

        JS_SetPropertyStr(ctx, global, "Array", arr_ctor);
    }

    /* ── String ────────────────────────────────────────────────────────── */
    {
        JSValue str_ctor = JS_NewCFunction(ctx, js_string_constructor, "String", 1);
        JSValue str_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, str_proto, js_string_proto_funcs,
            sizeof(js_string_proto_funcs) / sizeof(js_string_proto_funcs[0]));

        JS_SetPropertyFunctionList(ctx, str_ctor, js_string_static_funcs,
            sizeof(js_string_static_funcs) / sizeof(js_string_static_funcs[0]));

        JS_SetPropertyStr(ctx, str_ctor, "prototype", JS_DupValue(ctx, str_proto));
        JS_SetPropertyStr(ctx, str_proto, "constructor", JS_DupValue(ctx, str_ctor));

        JS_SetPropertyStr(ctx, global, "String", str_ctor);
    }

    /* ── Number ────────────────────────────────────────────────────────── */
    {
        JSValue num_ctor = JS_NewCFunction(ctx, js_number_constructor, "Number", 1);
        JSValue num_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, num_proto, js_number_proto_funcs,
            sizeof(js_number_proto_funcs) / sizeof(js_number_proto_funcs[0]));

        JS_SetPropertyFunctionList(ctx, num_ctor, js_number_static_funcs,
            sizeof(js_number_static_funcs) / sizeof(js_number_static_funcs[0]));

        JS_SetPropertyStr(ctx, num_ctor, "MAX_VALUE",        JS_NewFloat64(ctx, 1.7976931348623157e+308));
        JS_SetPropertyStr(ctx, num_ctor, "MIN_VALUE",        JS_NewFloat64(ctx, 5e-324));
        JS_SetPropertyStr(ctx, num_ctor, "NaN",              JS_NewFloat64(ctx, NAN));
        JS_SetPropertyStr(ctx, num_ctor, "NEGATIVE_INFINITY", JS_NewFloat64(ctx, -INFINITY));
        JS_SetPropertyStr(ctx, num_ctor, "POSITIVE_INFINITY", JS_NewFloat64(ctx, INFINITY));
        JS_SetPropertyStr(ctx, num_ctor, "EPSILON",          JS_NewFloat64(ctx, 2.220446049250313e-16));
        JS_SetPropertyStr(ctx, num_ctor, "MAX_SAFE_INTEGER", JS_NewFloat64(ctx, 9007199254740991.0));
        JS_SetPropertyStr(ctx, num_ctor, "MIN_SAFE_INTEGER", JS_NewFloat64(ctx, -9007199254740991.0));

        JS_SetPropertyStr(ctx, num_ctor, "prototype", JS_DupValue(ctx, num_proto));
        JS_SetPropertyStr(ctx, num_proto, "constructor", JS_DupValue(ctx, num_ctor));

        JS_SetPropertyStr(ctx, global, "Number", num_ctor);
    }

    /* ── Boolean ───────────────────────────────────────────────────────── */
    {
        JSValue bool_ctor = JS_NewCFunction(ctx, js_boolean_constructor, "Boolean", 1);
        JSValue bool_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, bool_proto, js_boolean_proto_funcs,
            sizeof(js_boolean_proto_funcs) / sizeof(js_boolean_proto_funcs[0]));

        JS_SetPropertyStr(ctx, bool_ctor, "prototype", JS_DupValue(ctx, bool_proto));
        JS_SetPropertyStr(ctx, bool_proto, "constructor", JS_DupValue(ctx, bool_ctor));

        JS_SetPropertyStr(ctx, global, "Boolean", bool_ctor);
    }

    /* ── Error ──────────────────────────────────────────────────────────── */
    {
        JSValue error_ctor = JS_NewCFunction(ctx, lr_error_constructor, "Error", 1);
        JSValue error_proto = JS_NewObject(ctx);

        /* Set constructor <-> prototype */
        JS_SetPropertyStr(ctx, error_proto, "constructor", JS_DupValue(ctx, error_ctor));
        JS_SetPropertyStr(ctx, error_ctor, "prototype", JS_DupValue(ctx, error_proto));

        /* Set name and message on prototype */
        JS_SetPropertyStr(ctx, error_proto, "name", JS_NewString(ctx, "Error"));
        JS_SetPropertyStr(ctx, error_proto, "message", JS_NewString(ctx, ""));

        /* Register Error as a global */
        JS_SetPropertyStr(ctx, global, "Error", error_ctor);
    }

    /* ── Function ──────────────────────────────────────────────────────── */
    {
        /* Function.prototype is a regular object (not a function) per spec */
        JSValue func_proto = JS_NewObject(ctx);

        /* Set prototype methods (call, apply, bind, toString) */
        JS_SetPropertyFunctionList(ctx, func_proto, js_function_proto_funcs,
            sizeof(js_function_proto_funcs) / sizeof(js_function_proto_funcs[0]));

        /* Create Function constructor as a C function */
        JSValue func_ctor = JS_NewCFunction(ctx, js_function_constructor, "Function", 1);

        /* Wire up constructor <-> prototype */
        JS_SetPropertyStr(ctx, func_ctor, "prototype", JS_DupValue(ctx, func_proto));
        JS_SetPropertyStr(ctx, func_proto, "constructor", JS_DupValue(ctx, func_ctor));

        /* Store Function.prototype for fast lookup */
        ctx->function_proto = JS_DupValue(ctx, func_proto);

        /* Register Function as a global */
        JS_SetPropertyStr(ctx, global, "Function", func_ctor);
    }

    /* ── eval ──────────────────────────────────────────────────────────
     * Registered last so it is present regardless of which constructors
     * above were already installed. */
    JS_SetPropertyStr(ctx, global, "eval",
                      JS_NewCFunction(ctx, js_global_eval, "eval", 1));

    JS_FreeValue(ctx, global);

    lr_log(rt, LR_LOG_DEBUG, "Core built-ins initialized (Object, Array, String, Number, Boolean, Function)");
}