/*
 * L/R_JS - Fetch API
 *
 * L/R_JS does NOT implement HTTP clients internally. It delegates HTTP
 * requests to the host application (browser, WebUI, etc.) through the
 * LR_HttpWrapper interface.
 *
 * If no wrapper is configured, fetch() returns a rejected Promise.
 * If the wrapper fails, fetch() returns a rejected Promise.
 * On success, fetch() returns a resolved Promise with a Response object.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_runtime.h"

/* ── HTTP wrapper API ──────────────────────────────────────────────────── */

void lr_http_set_wrapper(LR_Runtime *rt, LR_HttpWrapper *wrapper)
{
    rt->http_wrapper = wrapper;
}

LR_HttpWrapper *lr_http_get_wrapper(LR_Runtime *rt)
{
    return rt->http_wrapper;
}

/* ── Helper: free LR_HttpResult allocated fields ───────────────────────── */

void lr_http_result_free(LR_HttpResult *result)
{
    free(result->status_text);
    free(result->headers);
    free(result->body);
    free(result->error);
    memset(result, 0, sizeof(*result));
}

/* ── fetch() ───────────────────────────────────────────────────────────── */

static JSValue lr_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fetch requires at least 1 argument");

    LR_Runtime *rt = (LR_Runtime *)JS_GetContextOpaque(ctx);
    LR_HttpWrapper *wrapper = rt->http_wrapper;

    /* Create promise capability for async return */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    /* No wrapper → reject promise */
    if (!wrapper || !wrapper->fetch) {
        JSValue err = JS_NewString(ctx, "fetch() is not available: "
                                    "no HTTP wrapper configured. "
                                    "Call lr_http_set_wrapper() first.");
        JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return promise;
    }

    /* Parse URL */
    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) {
        JSValue err = JS_NewString(ctx, "fetch failed to parse URL");
        JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }

    /* Parse options object */
    const char *method = "GET";
    int method_alloc = 0;
    const char *body_str = NULL;
    JSValue headers_val = JS_UNDEFINED;

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(v)) { method = JS_ToCString(ctx, v); method_alloc = 1; }
        JS_FreeValue(ctx, v);

        v = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(v)) body_str = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);

        headers_val = JS_GetPropertyStr(ctx, argv[1], "headers");
    }

    /* Build header string from JS headers object */
    char *header_str = NULL;
    if (!JS_IsUndefined(headers_val) && JS_IsObject(headers_val)) {
        size_t cap = 512, len = 0;
        header_str = malloc(cap);
        if (header_str) header_str[0] = '\0';
        static const char *known[] = {
            "Content-Type", "Accept", "Authorization", "User-Agent",
            "X-Requested-With", "Origin", "Referer", "Cookie", NULL
        };
        for (int i = 0; known[i] && header_str; i++) {
            JSValue v = JS_GetPropertyStr(ctx, headers_val, known[i]);
            if (!JS_IsUndefined(v)) {
                const char *s = JS_ToCString(ctx, v);
                if (s) {
                    size_t need = strlen(known[i]) + 2 + strlen(s) + 2;
                    if (len + need + 1 > cap) {
                        cap *= 2;
                        char *n = realloc(header_str, cap);
                        if (!n) { JS_FreeCString(ctx, s); JS_FreeValue(ctx, v); break; }
                        header_str = n;
                    }
                    len += sprintf(header_str + len, "%s: %s\r\n", known[i], s);
                    JS_FreeCString(ctx, s);
                }
            }
            JS_FreeValue(ctx, v);
        }
    }

    /* Call the wrapper */
    LR_HttpResult result;
    memset(&result, 0, sizeof(result));
    int ret = wrapper->fetch(wrapper->user_data, method, url_str,
                              header_str ? header_str : "",
                              body_str ? body_str : "",
                              body_str ? strlen(body_str) : 0,
                              &result);

    /* Cleanup JS strings */
    JS_FreeCString(ctx, url_str);
    if (method_alloc) JS_FreeCString(ctx, method);
    if (body_str) JS_FreeCString(ctx, body_str);
    if (!JS_IsUndefined(headers_val)) JS_FreeValue(ctx, headers_val);
    free(header_str);

    /* Wrapper returned error */
    if (ret < 0 || result.error) {
        const char *msg = result.error ? result.error : "fetch failed";
        JSValue err = JS_NewString(ctx, msg);
        JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
        lr_http_result_free(&result);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return promise;
    }

    /* Build Response object */
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, result.status_code));
    JS_SetPropertyStr(ctx, resp, "statusText",
                      JS_NewString(ctx, result.status_text ? result.status_text : ""));
    JS_SetPropertyStr(ctx, resp, "ok",
                      JS_NewBool(ctx, result.status_code >= 200 && result.status_code < 300));

    /* Headers */
    JSValue hdrs = JS_NewObject(ctx);
    if (result.headers) {
        char *cpy = strdup(result.headers);
        char *line = strtok(cpy, "\r\n");
        while (line) {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                const char *val = colon + 1;
                while (*val == ' ') val++;
                JS_SetPropertyStr(ctx, hdrs, line, JS_NewString(ctx, val));
                *colon = ':';
            }
            line = strtok(NULL, "\r\n");
        }
        free(cpy);
    }
    JS_SetPropertyStr(ctx, resp, "headers", hdrs);

    /* Body */
    JS_SetPropertyStr(ctx, resp, "_body",
                      JS_NewStringLen(ctx, result.body ? result.body : "", result.body_len));

    lr_http_result_free(&result);

    /* Resolve promise with the Response object */
    JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&resp);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    JS_FreeValue(ctx, resp);
    return promise;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_fetch_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, lr_fetch, "fetch", 1));
    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Fetch API initialized (HTTP wrapper-based)");
}