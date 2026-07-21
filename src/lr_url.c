/*
 * L/R_JS - URL API (URL, URLSearchParams)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lr_runtime.h"

/* ── URL helper: parse URL components ─────────────────────────────────── */

typedef struct {
    char *href;
    char *protocol;
    char *host;
    char *hostname;
    char *port;
    char *pathname;
    char *search;
    char *hash;
    char *origin;
    char *username;
    char *password;
} URLComponents;

static void url_components_free(URLComponents *c)
{
    free(c->href);
    free(c->protocol);
    free(c->host);
    free(c->hostname);
    free(c->port);
    free(c->pathname);
    free(c->search);
    free(c->hash);
    free(c->origin);
    free(c->username);
    free(c->password);
    memset(c, 0, sizeof(*c));
}

static void url_parse(const char *url_str, URLComponents *out)
{
    memset(out, 0, sizeof(*out));

    if (!url_str || !*url_str) {
        out->href = strdup("");
        out->protocol = strdup("");
        out->hostname = strdup("");
        out->port = strdup("");
        out->pathname = strdup("");
        out->search = strdup("");
        out->hash = strdup("");
        out->origin = strdup("null");
        out->username = strdup("");
        out->password = strdup("");
        out->host = strdup("");
        return;
    }

    out->href = strdup(url_str);

    const char *p = url_str;
    const char *start;

    /* Protocol */
    start = p;
    while (*p && *p != ':' && *p != '/') p++;
    if (*p == ':') {
        size_t len = p - start + 1;
        out->protocol = malloc(len + 1);
        memcpy(out->protocol, start, len);
        out->protocol[len] = '\0';
        p++;
    } else {
        out->protocol = strdup("");
        p = url_str;
    }

    /* Skip // */
    if (p[0] == '/' && p[1] == '/') {
        p += 2;

        /* Username:password@ */
        start = p;
        const char *at_sign = strchr(start, '@');
        const char *slash = strchr(start, '/');

        if (at_sign && (!slash || at_sign < slash)) {
            const char *colon = strchr(start, ':');
            if (colon && colon < at_sign) {
                size_t user_len = colon - start;
                out->username = malloc(user_len + 1);
                memcpy(out->username, start, user_len);
                out->username[user_len] = '\0';

                size_t pass_len = at_sign - colon - 1;
                out->password = malloc(pass_len + 1);
                memcpy(out->password, colon + 1, pass_len);
                out->password[pass_len] = '\0';
            } else {
                size_t user_len = at_sign - start;
                out->username = malloc(user_len + 1);
                memcpy(out->username, start, user_len);
                out->username[user_len] = '\0';
                out->password = strdup("");
            }
            p = at_sign + 1;
        } else {
            out->username = strdup("");
            out->password = strdup("");
        }

        /* Hostname:port */
        start = p;
        const char *colon = strchr(p, ':');
        const char *slash2 = strchr(p, '/');
        const char *end = p + strlen(p);

        if (colon && (!slash2 || colon < slash2)) {
            size_t host_len = colon - start;
            out->hostname = malloc(host_len + 1);
            memcpy(out->hostname, start, host_len);
            out->hostname[host_len] = '\0';

            const char *port_start = colon + 1;
            const char *port_end = slash2 ? slash2 : end;
            size_t port_len = port_end - port_start;
            out->port = malloc(port_len + 1);
            memcpy(out->port, port_start, port_len);
            out->port[port_len] = '\0';

            out->host = malloc(host_len + 1 + port_len + 1);
            sprintf(out->host, "%s:%s", out->hostname, out->port);
            p = port_end;
        } else {
            const char *host_end2 = slash2 ? slash2 : end;
            size_t host_len = host_end2 - start;
            out->hostname = malloc(host_len + 1);
            memcpy(out->hostname, start, host_len);
            out->hostname[host_len] = '\0';
            out->port = strdup("");
            out->host = strdup(out->hostname);
            p = host_end2;
        }
    } else {
        out->hostname = strdup("");
        out->port = strdup("");
        out->host = strdup("");
        out->username = strdup("");
        out->password = strdup("");
    }

    /* Pathname */
    start = p;
    const char *qmark = strchr(p, '?');
    const char *hash_mark = strchr(p, '#');
    const char *path_end = qmark ? (hash_mark && hash_mark < qmark ? hash_mark : qmark) : (hash_mark ? hash_mark : p + strlen(p));
    size_t path_len = path_end - start;
    if (path_len == 0) {
        out->pathname = strdup("/");
    } else {
        out->pathname = malloc(path_len + 1);
        memcpy(out->pathname, start, path_len);
        out->pathname[path_len] = '\0';
    }
    p = path_end;

    /* Search */
    if (*p == '?') {
        p++;
        start = p;
        const char *hash2 = strchr(p, '#');
        const char *search_end = hash2 ? hash2 : p + strlen(p);
        size_t search_len = search_end - start;
        out->search = malloc(search_len + 1);
        memcpy(out->search, start, search_len);
        out->search[search_len] = '\0';
        p = search_end;
    } else {
        out->search = strdup("");
    }

    /* Hash */
    if (*p == '#') {
        p++;
        out->hash = strdup(p);
    } else {
        out->hash = strdup("");
    }

    /* Origin */
    if (out->protocol && strlen(out->protocol) > 0 && out->hostname && strlen(out->hostname) > 0) {
        size_t origin_len = strlen(out->protocol) + 3 + strlen(out->hostname) + (strlen(out->port) > 0 ? 1 + strlen(out->port) : 0) + 1;
        out->origin = malloc(origin_len);
        if (strlen(out->port) > 0) {
            snprintf(out->origin, origin_len, "%s//%s:%s", out->protocol, out->hostname, out->port);
        } else {
            snprintf(out->origin, origin_len, "%s//%s", out->protocol, out->hostname);
        }
    } else {
        out->origin = strdup("null");
    }
}

/* ── Set URL properties on a JS object ────────────────────────────────── */

static void url_set_props(JSContext *ctx, JSValue obj, const URLComponents *c)
{
    JS_SetPropertyStr(ctx, obj, "href",     JS_NewString(ctx, c->href));
    JS_SetPropertyStr(ctx, obj, "protocol", JS_NewString(ctx, c->protocol));
    JS_SetPropertyStr(ctx, obj, "host",     JS_NewString(ctx, c->host));
    JS_SetPropertyStr(ctx, obj, "hostname", JS_NewString(ctx, c->hostname));
    JS_SetPropertyStr(ctx, obj, "port",     JS_NewString(ctx, c->port));
    JS_SetPropertyStr(ctx, obj, "pathname", JS_NewString(ctx, c->pathname));
    JS_SetPropertyStr(ctx, obj, "search",   JS_NewString(ctx, c->search));
    JS_SetPropertyStr(ctx, obj, "hash",     JS_NewString(ctx, c->hash));
    JS_SetPropertyStr(ctx, obj, "origin",   JS_NewString(ctx, c->origin));
    JS_SetPropertyStr(ctx, obj, "username", JS_NewString(ctx, c->username));
    JS_SetPropertyStr(ctx, obj, "password", JS_NewString(ctx, c->password));
}

/* ── URLSearchParams forward declarations ─────────────────────────────── */

static JSValue lr_url_search_params_toString(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv);
static JSValue lr_url_search_params_get(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
static JSValue lr_url_search_params_has(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
static JSValue lr_url_search_params_append(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static JSValue lr_url_search_params_delete(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
static JSValue lr_url_search_params_entries(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv);

/* ── URLSearchParams function list ────────────────────────────────────── */

static const JSCFunctionListEntry lr_url_search_params_funcs[] = {
    JS_CFUNC_DEF("toString",  0, lr_url_search_params_toString),
    JS_CFUNC_DEF("get",       1, lr_url_search_params_get),
    JS_CFUNC_DEF("has",       1, lr_url_search_params_has),
    JS_CFUNC_DEF("append",    2, lr_url_search_params_append),
    JS_CFUNC_DEF("delete",    1, lr_url_search_params_delete),
    JS_CFUNC_DEF("entries",   0, lr_url_search_params_entries),
};

/* ── URL Constructor ──────────────────────────────────────────────────── */

static JSValue lr_url_constructor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "URL requires at least 1 argument");
    }

    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_EXCEPTION;

    const char *base = NULL;
    if (argc >= 2) {
        base = JS_ToCString(ctx, argv[1]);
    }

    char *final_url = NULL;
    if (base && url_str[0] != '/' && !strstr(url_str, "://")) {
        URLComponents base_components;
        url_parse(base, &base_components);

        size_t len = strlen(base_components.origin) + strlen(url_str) + 2;
        final_url = malloc(len);
        if (url_str[0] == '/') {
            snprintf(final_url, len, "%s%s", base_components.origin, url_str);
        } else {
            const char *base_path = base_components.pathname;
            const char *last_slash = strrchr(base_path, '/');
            if (last_slash) {
                size_t base_dir_len = last_slash - base_path + 1;
                snprintf(final_url, len, "%s%.*s%s", base_components.origin,
                         (int)base_dir_len, base_path, url_str);
            } else {
                snprintf(final_url, len, "%s/%s", base_components.origin, url_str);
            }
        }
        url_components_free(&base_components);
    } else {
        final_url = strdup(url_str);
    }

    JS_FreeCString(ctx, url_str);
    if (base) JS_FreeCString(ctx, base);

    URLComponents components;
    url_parse(final_url, &components);
    free(final_url);

    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    url_set_props(ctx, obj, &components);

    /* Create URLSearchParams */
    JSValue sp = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sp, lr_url_search_params_funcs,
                                sizeof(lr_url_search_params_funcs) / sizeof(lr_url_search_params_funcs[0]));
    JS_SetPropertyStr(ctx, sp, "_search",
                      JS_NewString(ctx, components.search));
    JS_SetPropertyStr(ctx, obj, "searchParams", sp);

    url_components_free(&components);
    return obj;
}

/* ── URL getters ──────────────────────────────────────────────────────── */

#define URL_GETTER(name, field) \
static JSValue lr_url_get_##name(JSContext *ctx, JSValueConst this_val) { \
    JSValue val = JS_GetPropertyStr(ctx, this_val, field); \
    const char *str = JS_ToCString(ctx, val); \
    JSValue result = JS_NewString(ctx, str ? str : ""); \
    if (str) JS_FreeCString(ctx, str); \
    JS_FreeValue(ctx, val); \
    return result; \
}

URL_GETTER(href, "__href")
URL_GETTER(protocol, "__protocol")
URL_GETTER(host, "__host")
URL_GETTER(hostname, "__hostname")
URL_GETTER(port, "__port")
URL_GETTER(pathname, "__pathname")
URL_GETTER(search, "__search")
URL_GETTER(hash, "__hash")
URL_GETTER(origin, "__origin")
URL_GETTER(username, "__username")
URL_GETTER(password, "__password")

/* ── URL setters ──────────────────────────────────────────────────────── */

#define URL_SETTER(name, field) \
static JSValue lr_url_set_##name(JSContext *ctx, JSValueConst this_val, JSValueConst val) { \
    const char *str = JS_ToCString(ctx, val); \
    if (!str) return JS_EXCEPTION; \
    JS_SetPropertyStr(ctx, this_val, field, JS_NewString(ctx, str)); \
    JS_FreeCString(ctx, str); \
    return JS_UNDEFINED; \
}

URL_SETTER(href, "__href")
URL_SETTER(protocol, "__protocol")
URL_SETTER(hostname, "__hostname")
URL_SETTER(port, "__port")
URL_SETTER(pathname, "__pathname")
URL_SETTER(search, "__search")
URL_SETTER(hash, "__hash")
URL_SETTER(username, "__username")
URL_SETTER(password, "__password")

/* ── URL.toString / toJSON ────────────────────────────────────────────── */

static JSValue lr_url_toString(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue val = JS_GetPropertyStr(ctx, this_val, "href");
    const char *str = JS_ToCString(ctx, val);
    JSValue result = JS_NewString(ctx, str ? str : "");
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, val);
    return result;
}

/* ── URLSearchParams ──────────────────────────────────────────────────── */

static JSValue lr_url_search_params_toString(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    JSValue search = JS_GetPropertyStr(ctx, this_val, "_search");
    const char *str = JS_ToCString(ctx, search);
    JS_FreeValue(ctx, search);
    JSValue result = JS_NewString(ctx, str ? str : "");
    if (str) JS_FreeCString(ctx, str);
    return result;
}

static JSValue lr_url_search_params_get(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue search = JS_GetPropertyStr(ctx, this_val, "_search");
    const char *search_str = JS_ToCString(ctx, search);
    JS_FreeValue(ctx, search);

    if (!search_str || !*search_str) {
        if (key) JS_FreeCString(ctx, key);
        if (search_str) JS_FreeCString(ctx, search_str);
        return JS_NULL;
    }

    const char *p = search_str;
    JSValue result = JS_NULL;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);

        size_t k_len = eq ? (size_t)(eq - p) : (size_t)(end - p);
        if (k_len == strlen(key) && strncmp(p, key, k_len) == 0) {
            if (eq && eq < end) {
                result = JS_NewStringLen(ctx, eq + 1, end - eq - 1);
            } else {
                result = JS_NewString(ctx, "");
            }
            break;
        }

        if (!amp) break;
        p = amp + 1;
    }

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, search_str);
    return result;
}

static JSValue lr_url_search_params_has(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValue val = lr_url_search_params_get(ctx, this_val, argc, argv);
    int has = !JS_IsNull(val);
    JS_FreeValue(ctx, val);
    return JS_NewBool(ctx, has);
}

static JSValue lr_url_search_params_append(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;

    const char *key = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (!key || !val) {
        if (key) JS_FreeCString(ctx, key);
        if (val) JS_FreeCString(ctx, val);
        return JS_EXCEPTION;
    }

    JSValue search = JS_GetPropertyStr(ctx, this_val, "_search");
    const char *old = JS_ToCString(ctx, search);
    JS_FreeValue(ctx, search);

    size_t new_len = (old ? strlen(old) : 0) + strlen(key) + strlen(val) + 3;
    char *new_search = malloc(new_len);
    if (old && *old) {
        snprintf(new_search, new_len, "%s&%s=%s", old, key, val);
    } else {
        snprintf(new_search, new_len, "%s=%s", key, val);
    }

    JS_SetPropertyStr(ctx, this_val, "_search", JS_NewString(ctx, new_search));
    free(new_search);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);
    if (old) JS_FreeCString(ctx, old);
    return JS_UNDEFINED;
}

static JSValue lr_url_search_params_delete(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue search = JS_GetPropertyStr(ctx, this_val, "_search");
    const char *search_str = JS_ToCString(ctx, search);
    JS_FreeValue(ctx, search);

    if (!search_str || !*search_str) {
        if (key) JS_FreeCString(ctx, key);
        if (search_str) JS_FreeCString(ctx, search_str);
        return JS_UNDEFINED;
    }

    size_t buf_size = strlen(search_str) + 1;
    char *new_search = malloc(buf_size);
    new_search[0] = '\0';

    const char *p = search_str;
    int first = 1;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);

        size_t k_len = eq ? (size_t)(eq - p) : (size_t)(end - p);
        if (k_len != strlen(key) || strncmp(p, key, k_len) != 0) {
            if (!first) strcat(new_search, "&");
            strncat(new_search, p, end - p);
            first = 0;
        }

        if (!amp) break;
        p = amp + 1;
    }

    JS_SetPropertyStr(ctx, this_val, "_search", JS_NewString(ctx, new_search));
    free(new_search);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, search_str);
    return JS_UNDEFINED;
}

static JSValue lr_url_search_params_entries(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    JSValue search = JS_GetPropertyStr(ctx, this_val, "_search");
    const char *search_str = JS_ToCString(ctx, search);
    JS_FreeValue(ctx, search);

    JSValue result = JS_NewArray(ctx);
    if (!search_str || !*search_str) {
        if (search_str) JS_FreeCString(ctx, search_str);
        return result;
    }

    const char *p = search_str;
    uint32_t idx = 0;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);

        JSValue pair = JS_NewArray(ctx);
        if (eq && eq < end) {
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewStringLen(ctx, p, eq - p));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewStringLen(ctx, eq + 1, end - eq - 1));
        } else {
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewStringLen(ctx, p, end - p));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewString(ctx, ""));
        }
        JS_SetPropertyUint32(ctx, result, idx++, pair);
        JS_FreeValue(ctx, pair);

        if (!amp) break;
        p = amp + 1;
    }

    JS_FreeCString(ctx, search_str);
    return result;
}

/* ── URL class function list ──────────────────────────────────────────── */

static const JSCFunctionListEntry lr_url_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, lr_url_toString),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_url_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* URL constructor */
    JSValue url_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, url_proto, lr_url_proto_funcs,
                                sizeof(lr_url_proto_funcs) / sizeof(lr_url_proto_funcs[0]));

    JSValue url_ctor = JS_NewCFunction2(ctx, lr_url_constructor, "URL", 1,
                                         JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, url_ctor, "prototype", JS_DupValue(ctx, url_proto));
    JS_FreeValue(ctx, url_proto);
    JS_SetPropertyStr(ctx, global, "URL", url_ctor);

    /* URLSearchParams */
    JSValue search_params_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, search_params_proto, lr_url_search_params_funcs,
                                sizeof(lr_url_search_params_funcs) / sizeof(lr_url_search_params_funcs[0]));
    JS_SetPropertyStr(ctx, global, "URLSearchParams", search_params_proto);

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "URL API initialized");
}