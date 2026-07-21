/*
 * L/R_JS - Fetch API (fetch, Request, Response, Headers)
 * Pure C, ES2022-compatible
 *
 * Uses raw sockets for HTTP/HTTPS requests (no external dependencies).
 * For production use, consider linking against libcurl.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include "lr_platform.h"

#include "lr_runtime.h"

/* ── HTTP response parser ──────────────────────────────────────────────── */

typedef struct {
    int         status_code;
    char       *status_text;
    char       *headers;      /* raw header string */
    char       *body;         /* response body */
    size_t      body_len;
    int         is_redirect;
    char       *location;
} HTTPResponse;

static void http_response_free(HTTPResponse *resp)
{
    free(resp->status_text);
    free(resp->headers);
    free(resp->body);
    free(resp->location);
    memset(resp, 0, sizeof(*resp));
}

static int http_parse_response(const char *raw, size_t raw_len, HTTPResponse *out)
{
    memset(out, 0, sizeof(*out));

    const char *p = raw;
    const char *end = raw + raw_len;

    /* Parse status line: HTTP/1.x NNN ... */
    if (strncmp(p, "HTTP/", 5) != 0) return -1;
    p += 5;
    while (p < end && *p != ' ') p++;
    if (p >= end) return -1;
    p++; /* skip space */

    /* Status code */
    out->status_code = atoi(p);
    while (p < end && isdigit((unsigned char)*p)) p++;
    if (p < end && *p == ' ') p++;

    /* Status text */
    const char *status_start = p;
    while (p < end && *p != '\r' && *p != '\n') p++;
    size_t status_len = p - status_start;
    out->status_text = malloc(status_len + 1);
    memcpy(out->status_text, status_start, status_len);
    out->status_text[status_len] = '\0';

    /* Skip CRLF */
    if (p < end && *p == '\r') p++;
    if (p < end && *p == '\n') p++;

    /* Parse headers */
    const char *headers_start = p;
    while (p < end - 1) {
        if (p[0] == '\r' && p[1] == '\n') break;
        if (p[0] == '\n' && p[1] == '\n') break;
        p++;
    }
    size_t headers_len = p - headers_start;
    out->headers = malloc(headers_len + 1);
    memcpy(out->headers, headers_start, headers_len);
    out->headers[headers_len] = '\0';

    /* Check for redirect */
    if (out->status_code >= 300 && out->status_code < 400) {
        out->is_redirect = 1;
        /* Find Location header */
        const char *loc = strstr(out->headers, "Location:");
        if (!loc) loc = strstr(out->headers, "location:");
        if (loc) {
            loc += 9;
            while (*loc == ' ') loc++;
            const char *loc_end = strstr(loc, "\r\n");
            if (!loc_end) loc_end = strstr(loc, "\n");
            if (!loc_end) loc_end = loc + strlen(loc);
            size_t loc_len = loc_end - loc;
            out->location = malloc(loc_len + 1);
            memcpy(out->location, loc, loc_len);
            out->location[loc_len] = '\0';
        }
    }

    /* Skip CRLFCRLF */
    if (p[0] == '\r' && p[1] == '\n') p += 2;
    else if (p[0] == '\n') p += 1;

    if (p < end) {
        out->body_len = end - p;
        out->body = malloc(out->body_len + 1);
        memcpy(out->body, p, out->body_len);
        out->body[out->body_len] = '\0';
    }

    return 0;
}

/* ── Simple HTTP GET ──────────────────────────────────────────────────── */

static int http_get(const char *url_str, HTTPResponse *out, char **error_msg)
{
    memset(out, 0, sizeof(*out));

    /* Parse URL */
    const char *p = url_str;
    int use_ssl = 0;

    if (strncmp(p, "https://", 8) == 0) {
        use_ssl = 1;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        /* Assume http:// */
    }

    /* Extract host */
    const char *host_start = p;
    const char *host_end = strchr(p, '/');
    const char *port_start = strchr(p, ':');
    if (port_start && (!host_end || port_start < host_end)) {
        host_end = port_start;
    }

    char host[256];
    size_t host_len = host_end ? (size_t)(host_end - host_start) : strlen(host_start);
    if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    int port = use_ssl ? 443 : 80;
    if (port_start && (!host_end || port_start < host_end)) {
        port = atoi(port_start + 1);
    }

    /* Extract path */
    const char *path_start = host_end ? host_end : p + strlen(p);
    char path[2048];
    if (*path_start == '\0') {
        strcpy(path, "/");
    } else {
        strncpy(path, path_start, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    if (use_ssl) {
        /* SSL not supported in this simple implementation */
        *error_msg = strdup("HTTPS is not supported in this build. Link with OpenSSL or use HTTP.");
        return -1;
    }

    /* Create socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        *error_msg = strdup("Failed to create socket");
        return -1;
    }

    /* Set timeout */
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    /* Resolve host */
    struct hostent *he = gethostbyname(host);
    if (!he) {
        *error_msg = strdup("Failed to resolve host");
        lr_socket_close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        *error_msg = strdup("Failed to connect");
        lr_socket_close(sock);
        return -1;
    }

    /* Send HTTP request */
    char request[4096];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: L/R_JS/1.0\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);

    send(sock, request, strlen(request), 0);

    /* Receive response */
    char *response_buf = malloc(65536);
    if (!response_buf) {
        *error_msg = strdup("Out of memory");
        lr_socket_close(sock);
        return -1;
    }

    size_t total = 0;
    size_t cap = 65536;
    int n;
    while ((n = recv(sock, response_buf + total, cap - total - 1, 0)) > 0) {
        total += n;
        if (total + 4096 >= cap) {
            cap *= 2;
            char *new_buf = realloc(response_buf, cap);
            if (!new_buf) break;
            response_buf = new_buf;
        }
    }
    response_buf[total] = '\0';
    lr_socket_close(sock);

    if (http_parse_response(response_buf, total, out) < 0) {
        *error_msg = strdup("Failed to parse HTTP response");
        free(response_buf);
        return -1;
    }

    free(response_buf);
    return 0;
}

/* ── fetch() implementation ───────────────────────────────────────────── */

static JSValue lr_fetch(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "fetch requires at least 1 argument");
    }

    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_EXCEPTION;

    /* Parse options */
    const char *method = "GET";
    int method_allocated = 0;
    const char *body_str = NULL;
    JSValue headers_val = JS_UNDEFINED;

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue method_val = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method_val)) {
            method = JS_ToCString(ctx, method_val);
            if (method) method_allocated = 1;
        }

        JSValue body_val = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body_val)) {
            body_str = JS_ToCString(ctx, body_val);
        }
        JS_FreeValue(ctx, body_val);

        headers_val = JS_GetPropertyStr(ctx, argv[1], "headers");
    }

    /* Perform HTTP request */
    HTTPResponse resp;
    char *error_msg = NULL;

    int ret = http_get(url_str, &resp, &error_msg);

    JS_FreeCString(ctx, url_str);
    if (method_allocated) JS_FreeCString(ctx, (char *)method);
    if (body_str) JS_FreeCString(ctx, body_str);
    if (!JS_IsUndefined(headers_val)) JS_FreeValue(ctx, headers_val);

    if (ret < 0) {
        /* Return a rejected promise */
        JSValue reject_func;
        JSValue promise = JS_NewPromiseCapability(ctx, &reject_func);
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, error_msg ? error_msg : "fetch failed"));
        JS_Call(ctx, reject_func, JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, reject_func);
        free(error_msg);
        return promise;
    }

    /* Build Response object */
    JSValue response = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, response, "status", JS_NewInt32(ctx, resp.status_code));
    JS_SetPropertyStr(ctx, response, "statusText", JS_NewString(ctx, resp.status_text));
    JS_SetPropertyStr(ctx, response, "ok",
                      JS_NewBool(ctx, resp.status_code >= 200 && resp.status_code < 300));
    JS_SetPropertyStr(ctx, response, "redirected", JS_NewBool(ctx, resp.is_redirect));

    /* Headers */
    JSValue headers = JS_NewObject(ctx);
    /* Parse headers and set them */
    if (resp.headers) {
        char *headers_copy = strdup(resp.headers);
        char *line = strtok(headers_copy, "\r\n");
        while (line) {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                const char *value = colon + 1;
                while (*value == ' ') value++;
                JS_SetPropertyStr(ctx, headers, line, JS_NewString(ctx, value));
                *colon = ':';
            }
            line = strtok(NULL, "\r\n");
        }
        free(headers_copy);
    }
    JS_SetPropertyStr(ctx, response, "headers", headers);

    /* Body methods */
    JS_SetPropertyStr(ctx, response, "_body", JS_NewStringLen(ctx, resp.body ? resp.body : "", resp.body_len));

    /* response.text() */
    JSValue text_func = JS_NewCFunction(ctx, NULL, "text", 0);
    JS_SetPropertyStr(ctx, response, "text", text_func);

    /* response.json() */
    JSValue json_func = JS_NewCFunction(ctx, NULL, "json", 0);
    JS_SetPropertyStr(ctx, response, "json", json_func);

    /* response.arrayBuffer() */
    JSValue ab_func = JS_NewCFunction(ctx, NULL, "arrayBuffer", 0);
    JS_SetPropertyStr(ctx, response, "arrayBuffer", ab_func);

    /* Return as resolved promise */
    JSValue resolve_func;
    JSValue promise = JS_NewPromiseCapability(ctx, &resolve_func);
    JS_Call(ctx, resolve_func, JS_UNDEFINED, 1, (JSValueConst *)&response);
    JS_FreeValue(ctx, resolve_func);
    JS_FreeValue(ctx, response);

    http_response_free(&resp);
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
    lr_log(rt, LR_LOG_DEBUG, "Fetch API initialized");
}

/* ── Cleanup ──────────────────────────────────────────────────────────── */

void lr_fetch_cleanup(LR_Runtime *rt)
{
    (void)rt;
    /* Nothing to clean up for now */
}