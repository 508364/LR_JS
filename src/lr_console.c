/*
 * L/R_JS - Console API (console.log, warn, error, info, debug, trace, time, etc.)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "lr_runtime.h"

/* ── Console write helper ─────────────────────────────────────────────── */

static void console_write(LR_Runtime *rt, const char *level, const char *prefix,
                           int argc, JSValueConst *argv)
{
    JSContext *ctx = rt->lr_ctx;
    FILE *out = rt->stdout_fp;

    /* Print prefix */
    fprintf(out, "%s", prefix);

    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', out);
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            fprintf(out, "%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    fprintf(out, "\n");
    fflush(out);
}

/* ── console.log ──────────────────────────────────────────────────────── */

static JSValue lr_console_log(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    console_write(rt, "log", "", argc, argv);
    return JS_UNDEFINED;
}

/* ── console.info ─────────────────────────────────────────────────────── */

static JSValue lr_console_info(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    console_write(rt, "info", "[INFO] ", argc, argv);
    return JS_UNDEFINED;
}

/* ── console.warn ─────────────────────────────────────────────────────── */

static JSValue lr_console_warn(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    console_write(rt, "warn", "[WARN] ", argc, argv);
    return JS_UNDEFINED;
}

/* ── console.error ────────────────────────────────────────────────────── */

static JSValue lr_console_error(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    console_write(rt, "error", "[ERROR] ", argc, argv);
    return JS_UNDEFINED;
}

/* ── console.debug ────────────────────────────────────────────────────── */

static JSValue lr_console_debug(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    console_write(rt, "debug", "[DEBUG] ", argc, argv);
    return JS_UNDEFINED;
}

/* ── console.assert ───────────────────────────────────────────────────── */

static JSValue lr_console_assert(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;

    /* Only assert if first argument is falsy */
    int cond = JS_ToBool(ctx, argv[0]);
    if (cond < 0) return JS_EXCEPTION;

    if (!cond) {
        LR_Runtime *rt = JS_GetContextOpaque(ctx);
        fprintf(rt->stderr_fp, "[ASSERT] Assertion failed: ");
        for (int i = 1; i < argc; i++) {
            if (i > 1) fputc(' ', rt->stderr_fp);
            const char *str = JS_ToCString(ctx, argv[i]);
            if (str) {
                fprintf(rt->stderr_fp, "%s", str);
                JS_FreeCString(ctx, str);
            }
        }
        fprintf(rt->stderr_fp, "\n");
        fflush(rt->stderr_fp);
    }
    return JS_UNDEFINED;
}

/* ── console.trace ────────────────────────────────────────────────────── */

static JSValue lr_console_trace(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    FILE *out = rt->stdout_fp;

    fprintf(out, "[TRACE] ");
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', out);
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            fprintf(out, "%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    fprintf(out, "\n");

    /* Print stack trace */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue err_ctor = JS_GetPropertyStr(ctx, global, "Error");
    JS_FreeValue(ctx, global);
    if (!JS_IsException(err_ctor) && !JS_IsUndefined(err_ctor)) {
        JSValue err = JS_CallConstructor(ctx, err_ctor, 0, NULL);
        if (!JS_IsException(err)) {
            JSValue stack = JS_GetPropertyStr(ctx, err, "stack");
            const char *stack_str = JS_ToCString(ctx, stack);
            if (stack_str) {
                fprintf(out, "%s\n", stack_str);
                JS_FreeCString(ctx, stack_str);
            }
            JS_FreeValue(ctx, stack);
        }
        JS_FreeValue(ctx, err);
    }
    JS_FreeValue(ctx, err_ctor);
    fflush(out);
    return JS_UNDEFINED;
}

/* ── console.time / timeEnd ───────────────────────────────────────────── */

static JSValue lr_console_time(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    /* Simple implementation: store current time keyed by label */
    const char *label = "default";
    if (argc > 0) {
        label = JS_ToCString(ctx, argv[0]);
        if (!label) return JS_EXCEPTION;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue timers = JS_GetPropertyStr(ctx, global, "__lr_console_timers__");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(timers)) {
        timers = JS_NewObject(ctx);
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__lr_console_timers__", timers);
        JS_FreeValue(ctx, global);
    }

    /* Store current time in ms */
    clock_t now = clock();
    double ms = (double)now * 1000.0 / CLOCKS_PER_SEC;
    JS_SetPropertyStr(ctx, timers, label, JS_NewFloat64(ctx, ms));

    JS_FreeValue(ctx, timers);
    if (argc > 0) JS_FreeCString(ctx, label);
    return JS_UNDEFINED;
}

static JSValue lr_console_timeEnd(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    const char *label = "default";
    if (argc > 0) {
        label = JS_ToCString(ctx, argv[0]);
        if (!label) return JS_EXCEPTION;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue timers = JS_GetPropertyStr(ctx, global, "__lr_console_timers__");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(timers)) {
        if (argc > 0) JS_FreeCString(ctx, label);
        return JS_UNDEFINED;
    }

    JSValue start_val = JS_GetPropertyStr(ctx, timers, label);
    if (!JS_IsUndefined(start_val)) {
        double start_ms;
        JS_ToFloat64(ctx, &start_ms, start_val);
        clock_t now = clock();
        double now_ms = (double)now * 1000.0 / CLOCKS_PER_SEC;
        double elapsed = now_ms - start_ms;

        LR_Runtime *rt = JS_GetContextOpaque(ctx);
        fprintf(rt->stdout_fp, "%s: %.3f ms\n", label, elapsed);
        fflush(rt->stdout_fp);

        JS_DeleteProperty(ctx, timers, JS_NewAtom(ctx, label), 0);
    }
    JS_FreeValue(ctx, start_val);
    JS_FreeValue(ctx, timers);
    if (argc > 0) JS_FreeCString(ctx, label);
    return JS_UNDEFINED;
}

/* ── console.clear ────────────────────────────────────────────────────── */

static JSValue lr_console_clear(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    fprintf(rt->stdout_fp, "\033[2J\033[H");
    fflush(rt->stdout_fp);
    return JS_UNDEFINED;
}

/* ── console.count / countReset ───────────────────────────────────────── */

static JSValue lr_console_count(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *label = "default";
    if (argc > 0) {
        label = JS_ToCString(ctx, argv[0]);
        if (!label) return JS_EXCEPTION;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue counters = JS_GetPropertyStr(ctx, global, "__lr_console_counters__");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(counters)) {
        counters = JS_NewObject(ctx);
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__lr_console_counters__", counters);
        JS_FreeValue(ctx, global);
    }

    JSValue count_val = JS_GetPropertyStr(ctx, counters, label);
    int count = 0;
    if (!JS_IsUndefined(count_val)) {
        JS_ToInt32(ctx, &count, count_val);
    }
    count++;
    JS_SetPropertyStr(ctx, counters, label, JS_NewInt32(ctx, count));

    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    fprintf(rt->stdout_fp, "%s: %d\n", label, count);
    fflush(rt->stdout_fp);

    JS_FreeValue(ctx, count_val);
    JS_FreeValue(ctx, counters);
    if (argc > 0) JS_FreeCString(ctx, label);
    return JS_UNDEFINED;
}

static JSValue lr_console_countReset(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *label = "default";
    if (argc > 0) {
        label = JS_ToCString(ctx, argv[0]);
        if (!label) return JS_EXCEPTION;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue counters = JS_GetPropertyStr(ctx, global, "__lr_console_counters__");
    JS_FreeValue(ctx, global);

    if (!JS_IsUndefined(counters)) {
        JS_SetPropertyStr(ctx, counters, label, JS_NewInt32(ctx, 0));
    }
    JS_FreeValue(ctx, counters);
    if (argc > 0) JS_FreeCString(ctx, label);
    return JS_UNDEFINED;
}

/* ── console.group / groupEnd ─────────────────────────────────────────── */

static JSValue lr_console_group(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    FILE *out = rt->stdout_fp;
    fprintf(out, "  ");
    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(out, " ");
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            fprintf(out, "%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    fprintf(out, "\n");
    fflush(out);
    return JS_UNDEFINED;
}

static JSValue lr_console_groupEnd(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    return JS_UNDEFINED;
}

/* ── console.table ────────────────────────────────────────────────────── */

static JSValue lr_console_table(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;

    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    FILE *out = rt->stdout_fp;

    if (JS_IsArray(ctx, argv[0]) || JS_IsObject(argv[0])) {
        /* Basic table output */
        JSPropertyEnum *tab = NULL;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &len, argv[0],
                                    JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < len; i++) {
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                JSValue val = JS_GetProperty(ctx, argv[0], tab[i].atom);
                const char *val_str = JS_ToCString(ctx, val);
                fprintf(out, "  %s: %s\n", key ? key : "(symbol)", val_str ? val_str : "");
                if (key) JS_FreeCString(ctx, key);
                if (val_str) JS_FreeCString(ctx, val_str);
                JS_FreeValue(ctx, val);
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
    } else {
        const char *str = JS_ToCString(ctx, argv[0]);
        fprintf(out, "%s\n", str ? str : "");
        if (str) JS_FreeCString(ctx, str);
    }
    fflush(out);
    return JS_UNDEFINED;
}

/* ── Console API function list ────────────────────────────────────────── */

static const JSCFunctionListEntry lr_console_funcs[] = {
    JS_CFUNC_DEF("log",     0, lr_console_log),
    JS_CFUNC_DEF("info",    0, lr_console_info),
    JS_CFUNC_DEF("warn",    0, lr_console_warn),
    JS_CFUNC_DEF("error",   0, lr_console_error),
    JS_CFUNC_DEF("debug",   0, lr_console_debug),
    JS_CFUNC_DEF("assert",  1, lr_console_assert),
    JS_CFUNC_DEF("trace",   0, lr_console_trace),
    JS_CFUNC_DEF("time",    1, lr_console_time),
    JS_CFUNC_DEF("timeEnd", 1, lr_console_timeEnd),
    JS_CFUNC_DEF("clear",   0, lr_console_clear),
    JS_CFUNC_DEF("count",   1, lr_console_count),
    JS_CFUNC_DEF("countReset", 1, lr_console_countReset),
    JS_CFUNC_DEF("group",   0, lr_console_group),
    JS_CFUNC_DEF("groupEnd", 0, lr_console_groupEnd),
    JS_CFUNC_DEF("table",   1, lr_console_table),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_console_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, console, lr_console_funcs,
                                sizeof(lr_console_funcs) / sizeof(lr_console_funcs[0]));

    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Console API initialized");
}