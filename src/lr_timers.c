/*
 * L/R_JS - Timers API (setTimeout, setInterval, clearTimeout, clearInterval, queueMicrotask)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lr_platform.h"
#include "lr_runtime.h"

/* ── Timer node ───────────────────────────────────────────────────────── */

typedef struct LR_TimerNode {
    int            id;
    int            is_interval;   /* 1 = interval, 0 = timeout */
    JSValue        callback;      /* JS function or string */
    int64_t        delay_ms;      /* original delay */
    clock_t        start_time;    /* when timer was created */
    clock_t        next_fire;     /* next fire time (for interval) */
    int            cleared;       /* 1 if cleared */
    int            is_pending;    /* 1 if pending microtask execution */
    struct LR_TimerNode *next;
} LR_TimerNode;

/* ── Timer list head ──────────────────────────────────────────────────── */

static int next_timer_id = 1;

static LR_TimerNode *timer_list = NULL;

/* ── Allocate new timer node ──────────────────────────────────────────── */

static LR_TimerNode *timer_node_new(void)
{
    LR_TimerNode *node = calloc(1, sizeof(LR_TimerNode));
    return node;
}

static void timer_node_free(LR_TimerNode *node)
{
    free(node);
}

/* ── setTimeout implementation ────────────────────────────────────────── */

static JSValue lr_set_timeout(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int is_interval)
{
    LR_Runtime *rt = JS_GetContextOpaque(ctx);
    if (argc < 1) return JS_ThrowTypeError(ctx, "setTimeout requires at least 1 argument");

    /* Validate callback */
    if (!JS_IsFunction(ctx, argv[0]) && !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "setTimeout: first argument must be a function or string");
    }

    int64_t delay = 0;
    if (argc >= 2) {
        if (JS_ToInt64(ctx, &delay, argv[1]) < 0)
            return JS_EXCEPTION;
        if (delay < 0) delay = 0;
    }

    /* Create timer node */
    LR_TimerNode *node = timer_node_new();
    node->id = next_timer_id++;
    node->is_interval = is_interval;
    node->callback = JS_DupValue(ctx, argv[0]);
    node->delay_ms = delay;
    node->start_time = clock();
    node->next_fire = node->start_time + (clock_t)(delay * CLOCKS_PER_SEC / 1000);
    node->cleared = 0;
    node->is_pending = 0;

    /* Add to timer list */
    node->next = timer_list;
    timer_list = node;

    rt->has_pending_jobs = 1;

    return JS_NewInt32(ctx, node->id);
}

static JSValue lr_setTimeout(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    return lr_set_timeout(ctx, this_val, argc, argv, 0);
}

static JSValue lr_setInterval(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    return lr_set_timeout(ctx, this_val, argc, argv, 1);
}

/* ── clearTimeout / clearInterval ─────────────────────────────────────── */

static JSValue lr_clear_timer(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;

    int32_t timer_id;
    if (JS_ToInt32(ctx, &timer_id, argv[0]) < 0)
        return JS_EXCEPTION;

    /* Find and mark as cleared */
    for (LR_TimerNode *n = timer_list; n; n = n->next) {
        if (n->id == timer_id) {
            n->cleared = 1;
            break;
        }
    }
    return JS_UNDEFINED;
}

static JSValue lr_clearTimeout(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return lr_clear_timer(ctx, this_val, argc, argv);
}

static JSValue lr_clearInterval(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return lr_clear_timer(ctx, this_val, argc, argv);
}

/* ── queueMicrotask ───────────────────────────────────────────────────── */

static JSValue lr_queueMicrotask(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "queueMicrotask: argument must be a function");
    }

    /* Enqueue the callback function directly as a microtask job */
    lr_enqueue_job(ctx->rt, ctx, argv[0]);

    return JS_UNDEFINED;
}

/* ── Timer processing (called from event loop) ────────────────────────── */

void lr_timers_process(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    clock_t now = clock();
    int has_pending = 0;

    LR_TimerNode *prev = NULL;
    LR_TimerNode *node = timer_list;

    while (node) {
        if (node->cleared) {
            /* Remove cleared timer */
            LR_TimerNode *to_free = node;
            if (prev) {
                prev->next = node->next;
                node = node->next;
            } else {
                timer_list = node->next;
                node = timer_list;
            }
            JS_FreeValue(ctx, to_free->callback);
            timer_node_free(to_free);
            continue;
        }

        if (now >= node->next_fire) {
            /* Fire the callback */
            JSValue ret = JS_Call(ctx, node->callback, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(ret)) {
                lr_check_exception(rt);
            }
            JS_FreeValue(ctx, ret);

            if (node->is_interval) {
                /* Reschedule interval */
                node->next_fire = now + (clock_t)(node->delay_ms * CLOCKS_PER_SEC / 1000);
                has_pending = 1;
            } else {
                /* Remove timeout */
                node->cleared = 1;
                has_pending = 0;
            }
        } else {
            has_pending = 1;
        }

        prev = node;
        node = node->next;
    }

    rt->has_pending_jobs = has_pending;
}

/* ── Timer function list ──────────────────────────────────────────────── */

static const JSCFunctionListEntry lr_timer_funcs[] = {
    JS_CFUNC_DEF("setTimeout",      1, lr_setTimeout),
    JS_CFUNC_DEF("setInterval",     1, lr_setInterval),
    JS_CFUNC_DEF("clearTimeout",    1, lr_clearTimeout),
    JS_CFUNC_DEF("clearInterval",   1, lr_clearInterval),
    JS_CFUNC_DEF("queueMicrotask",  1, lr_queueMicrotask),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_timers_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyFunctionList(ctx, global, lr_timer_funcs,
                                sizeof(lr_timer_funcs) / sizeof(lr_timer_funcs[0]));

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Timers API initialized");
}

/* ── Cleanup ──────────────────────────────────────────────────────────── */

void lr_timers_cleanup(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    LR_TimerNode *node = timer_list;
    while (node) {
        LR_TimerNode *next = node->next;
        if (ctx) JS_FreeValue(ctx, node->callback);
        timer_node_free(node);
        node = next;
    }
    timer_list = NULL;
}