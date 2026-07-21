/*
 * L/R_JS - Performance API (performance.now, performance.mark, etc.)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lr_platform.h"
#include "lr_runtime.h"

/* ── Performance data ─────────────────────────────────────────────────── */

typedef struct {
    double start_time_ms;
} PerfData;

static PerfData *perf_get_data(LR_Runtime *rt)
{
    static PerfData data;
    static int initialized = 0;
    if (!initialized) {
        clock_t now = clock();
        data.start_time_ms = (double)now * 1000.0 / CLOCKS_PER_SEC;
        initialized = 1;
    }
    return &data;
}

/* ── performance.now() ────────────────────────────────────────────────── */

static JSValue lr_performance_now(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    PerfData *perf = perf_get_data(rt);
    clock_t now = clock();
    double now_ms = (double)now * 1000.0 / CLOCKS_PER_SEC;
    double elapsed = now_ms - perf->start_time_ms;
    return JS_NewFloat64(ctx, elapsed);
}

/* ── performance.mark() ───────────────────────────────────────────────── */

static JSValue lr_performance_mark(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "performance.mark requires a name");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    PerfData *perf = perf_get_data(rt);
    clock_t now = clock();
    double now_ms = (double)now * 1000.0 / CLOCKS_PER_SEC;
    double elapsed = now_ms - perf->start_time_ms;

    /* Store mark in a global marks object */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue marks = JS_GetPropertyStr(ctx, global, "__lr_performance_marks__");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(marks)) {
        marks = JS_NewObject(ctx);
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__lr_performance_marks__", marks);
        JS_FreeValue(ctx, global);
    }

    JSValue mark = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mark, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, mark, "entryType", JS_NewString(ctx, "mark"));
    JS_SetPropertyStr(ctx, mark, "startTime", JS_NewFloat64(ctx, elapsed));
    JS_SetPropertyStr(ctx, mark, "duration", JS_NewFloat64(ctx, 0));

    JS_SetPropertyStr(ctx, marks, name, mark);
    JS_FreeValue(ctx, mark);
    JS_FreeValue(ctx, marks);
    JS_FreeCString(ctx, name);

    return JS_UNDEFINED;
}

/* ── performance.measure() ────────────────────────────────────────────── */

static JSValue lr_performance_measure(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "performance.measure requires a name");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    PerfData *perf = perf_get_data(rt);
    clock_t now = clock();
    double now_ms = (double)now * 1000.0 / CLOCKS_PER_SEC;
    double elapsed = now_ms - perf->start_time_ms;

    double start_time = 0;
    const char *start_mark = NULL;
    const char *end_mark = NULL;

    if (argc >= 2) {
        start_mark = JS_ToCString(ctx, argv[1]);
        if (argc >= 3) {
            end_mark = JS_ToCString(ctx, argv[2]);
        }
    }

    /* Look up marks */
    if (start_mark) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue marks = JS_GetPropertyStr(ctx, global, "__lr_performance_marks__");
        JS_FreeValue(ctx, global);

        if (!JS_IsUndefined(marks)) {
            JSValue start_obj = JS_GetPropertyStr(ctx, marks, start_mark);
            if (!JS_IsUndefined(start_obj)) {
                JSValue st = JS_GetPropertyStr(ctx, start_obj, "startTime");
                JS_ToFloat64(ctx, &start_time, st);
                JS_FreeValue(ctx, st);
            }
            JS_FreeValue(ctx, start_obj);

            if (end_mark) {
                JSValue end_obj = JS_GetPropertyStr(ctx, marks, end_mark);
                if (!JS_IsUndefined(end_obj)) {
                    JSValue et = JS_GetPropertyStr(ctx, end_obj, "startTime");
                    JS_ToFloat64(ctx, &elapsed, et);
                    JS_FreeValue(ctx, et);
                }
                JS_FreeValue(ctx, end_obj);
            }
            JS_FreeValue(ctx, marks);
        }
    }

    double duration = elapsed - start_time;

    /* Store measure */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue measures = JS_GetPropertyStr(ctx, global, "__lr_performance_measures__");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(measures)) {
        measures = JS_NewObject(ctx);
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__lr_performance_measures__", measures);
        JS_FreeValue(ctx, global);
    }

    JSValue measure = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, measure, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, measure, "entryType", JS_NewString(ctx, "measure"));
    JS_SetPropertyStr(ctx, measure, "startTime", JS_NewFloat64(ctx, start_time));
    JS_SetPropertyStr(ctx, measure, "duration", JS_NewFloat64(ctx, duration));

    JS_SetPropertyStr(ctx, measures, name, measure);
    JS_FreeValue(ctx, measure);
    JS_FreeValue(ctx, measures);

    JS_FreeCString(ctx, name);
    if (start_mark) JS_FreeCString(ctx, start_mark);
    if (end_mark) JS_FreeCString(ctx, end_mark);

    return JS_UNDEFINED;
}

/* ── performance.getEntriesByType() ───────────────────────────────────── */

static JSValue lr_performance_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv)
{
    JSValue result = JS_NewArray(ctx);
    if (argc < 1) return result;

    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return result;

    JSValue global = JS_GetGlobalObject(ctx);
    const char *prop = NULL;
    if (strcmp(type, "mark") == 0) {
        prop = "__lr_performance_marks__";
    } else if (strcmp(type, "measure") == 0) {
        prop = "__lr_performance_measures__";
    }

    if (prop) {
        JSValue entries = JS_GetPropertyStr(ctx, global, prop);
        if (!JS_IsUndefined(entries)) {
            JSPropertyEnum *tab = NULL;
            uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, entries,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; i++) {
                    JSValue val = JS_GetProperty(ctx, entries, tab[i].atom);
                    JS_SetPropertyUint32(ctx, result, i, val);
                    JS_FreeValue(ctx, val);
                }
                JS_FreePropertyEnum(ctx, tab, len);
            }
            JS_FreeValue(ctx, entries);
        }
    }

    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, type);
    return result;
}

/* ── performance.timeOrigin ───────────────────────────────────────────── */

static JSValue lr_performance_timeOrigin(JSContext *ctx, JSValueConst this_val)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    PerfData *perf = perf_get_data(rt);
    return JS_NewFloat64(ctx, perf->start_time_ms);
}

/* ── Performance API function list ────────────────────────────────────── */

static const JSCFunctionListEntry lr_performance_funcs[] = {
    JS_CFUNC_DEF("now",               0, lr_performance_now),
    JS_CFUNC_DEF("mark",              1, lr_performance_mark),
    JS_CFUNC_DEF("measure",           1, lr_performance_measure),
    JS_CFUNC_DEF("getEntriesByType",  1, lr_performance_getEntriesByType),
    JS_CGETSET_DEF("timeOrigin", lr_performance_timeOrigin, NULL),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_performance_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, perf, lr_performance_funcs,
                                sizeof(lr_performance_funcs) / sizeof(lr_performance_funcs[0]));
    JS_SetPropertyStr(ctx, global, "performance", perf);

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Performance API initialized");
}