/*
 * L/R_JS - Extra ES2022 Built-in Objects (Math, JSON, Date, RegExp, Symbol, Error subclasses, WeakMap, WeakSet)
 * Pure C, ES2022-compatible
 */
#include <math.h>
#include <time.h>
#if defined(_MSC_VER)
#include "lr_regex.h"   /* MSVC has no <regex.h>; use the built-in engine */
#else
#include <regex.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <errno.h>

/* ── MSVC POSIX shims ────────────────────────────────────────────────────
 * MSVC does not provide these POSIX functions used by the Date module.
 * They are only needed on Windows; other platforms use the system libc. */
#if defined(_MSC_VER)
#include <intrin.h>

/* __builtin_clz: count leading zeros of a 32-bit unsigned int. */
static __inline int lr_builtin_clz(unsigned int x)
{
    unsigned long idx;
    if (x == 0) return 32;
    _BitScanReverse(&idx, (unsigned long)x);
    return 31 - (int)idx;
}
#define __builtin_clz lr_builtin_clz

/* timegm: like mktime but interprets the broken-down time as UTC. */
#define timegm _mkgmtime

/* Minimal strptime supporting only the formats used by this file:
 *   "%Y-%m-%dT%H:%M:%S"  and  "%Y-%m-%d" */
static __inline int lr_strptime_isdig(const char *p)
{
    return p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9';
}
static const char *lr_strptime(const char *s, const char *fmt, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    tm->tm_mday = 1;
    const char *p = s;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*p != *fmt) return NULL;
            p++;
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'Y':
            if (!(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
                  p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9'))
                return NULL;
            tm->tm_year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 +
                          (p[2] - '0') * 10 + (p[3] - '0') - 1900;
            p += 4;
            break;
        case 'm':
            if (!lr_strptime_isdig(p)) return NULL;
            tm->tm_mon = (p[0] - '0') * 10 + (p[1] - '0') - 1;
            p += 2;
            break;
        case 'd':
            if (!lr_strptime_isdig(p)) return NULL;
            tm->tm_mday = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            break;
        case 'H':
            if (!lr_strptime_isdig(p)) return NULL;
            tm->tm_hour = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            break;
        case 'M':
            if (!lr_strptime_isdig(p)) return NULL;
            tm->tm_min = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            break;
        case 'S':
            if (!lr_strptime_isdig(p)) return NULL;
            tm->tm_sec = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            break;
        default:
            return NULL;
        }
    }
    return p;
}
#define strptime lr_strptime
#endif

#include "lr_runtime.h"
#include "lr_builtins.h"

/* ========================================================================
 *  UTILITY HELPERS
 * ======================================================================== */

/* Get a double from argv with optional default */
static double get_double_arg(LRContext *ctx, int argc, LRValue *argv, int idx, double def)
{
    if (idx >= argc) return def;
    double d;
    if (JS_ToFloat64(ctx, &d, argv[idx]) < 0) return def;
    return d;
}

/* Get an int32 from argv with optional default */
static int32_t get_int_arg(LRContext *ctx, int argc, LRValue *argv, int idx, int32_t def)
{
    if (idx >= argc) return def;
    int32_t i;
    if (JS_ToInt32(ctx, &i, argv[idx]) < 0) return def;
    return i;
}

/* ========================================================================
 *  1. MATH OBJECT
 * ======================================================================== */

static LRValue js_math_abs(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, fabs(x));
}

static LRValue js_math_acos(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, acos(x));
}

static LRValue js_math_acosh(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, acosh(x));
}

static LRValue js_math_asin(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, asin(x));
}

static LRValue js_math_asinh(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, asinh(x));
}

static LRValue js_math_atan(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, atan(x));
}

static LRValue js_math_atan2(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double y = get_double_arg(ctx, argc, argv, 0, NAN);
    double x = get_double_arg(ctx, argc, argv, 1, NAN);
    return JS_NewFloat64(ctx, atan2(y, x));
}

static LRValue js_math_atanh(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, atanh(x));
}

static LRValue js_math_cbrt(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, cbrt(x));
}

static LRValue js_math_ceil(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, ceil(x));
}

static LRValue js_math_clz32(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    uint32_t n = (uint32_t)get_int_arg(ctx, argc, argv, 0, 0);
    if (n == 0) return JS_NewInt32(ctx, 32);
    int count = __builtin_clz(n);
    return JS_NewInt32(ctx, count);
}

static LRValue js_math_cos(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, cos(x));
}

static LRValue js_math_cosh(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, cosh(x));
}

static LRValue js_math_exp(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, exp(x));
}

static LRValue js_math_expm1(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, expm1(x));
}

static LRValue js_math_floor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, floor(x));
}

static LRValue js_math_fround(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    float f = (float)x;
    return JS_NewFloat64(ctx, (double)f);
}

static LRValue js_math_hypot(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double sum = 0.0;
    int has_values = 0;
    for (int i = 0; i < argc; i++) {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[i]) == 0) {
            if (isinf(d)) return JS_NewFloat64(ctx, INFINITY);
            if (isnan(d)) return JS_NewFloat64(ctx, NAN);
            sum += d * d;
            has_values = 1;
        }
    }
    if (!has_values) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, sqrt(sum));
}

static LRValue js_math_imul(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    uint32_t a = (uint32_t)get_int_arg(ctx, argc, argv, 0, 0);
    uint32_t b = (uint32_t)get_int_arg(ctx, argc, argv, 1, 0);
    return JS_NewInt32(ctx, (int32_t)(a * b));
}

static LRValue js_math_log(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, log(x));
}

static LRValue js_math_log10(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, log10(x));
}

static LRValue js_math_log1p(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, log1p(x));
}

static LRValue js_math_log2(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, log2(x));
}

static LRValue js_math_max(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc == 0) return JS_NewFloat64(ctx, -INFINITY);
    double max_val = -INFINITY;
    for (int i = 0; i < argc; i++) {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[i]) < 0) continue;
        if (isnan(d)) return JS_NewFloat64(ctx, NAN);
        if (d > max_val) max_val = d;
    }
    return JS_NewFloat64(ctx, max_val);
}

static LRValue js_math_min(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc == 0) return JS_NewFloat64(ctx, INFINITY);
    double min_val = INFINITY;
    for (int i = 0; i < argc; i++) {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[i]) < 0) continue;
        if (isnan(d)) return JS_NewFloat64(ctx, NAN);
        if (d < min_val) min_val = d;
    }
    return JS_NewFloat64(ctx, min_val);
}

static LRValue js_math_pow(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double base = get_double_arg(ctx, argc, argv, 0, NAN);
    double exp = get_double_arg(ctx, argc, argv, 1, NAN);
    return JS_NewFloat64(ctx, pow(base, exp));
}

static LRValue js_math_random(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)rand() / (double)RAND_MAX);
}

static LRValue js_math_round(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    if (isnan(x) || isinf(x)) return JS_NewFloat64(ctx, x);
    /* Round half away from zero (banker's rounding not used by JS) */
    double r = round(x);
    /* Handle -0.5 → -0 properly */
    if (x < 0.0 && x >= -0.5 && r == 0.0) return JS_NewFloat64(ctx, -0.0);
    return JS_NewFloat64(ctx, r);
}

static LRValue js_math_sign(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    if (isnan(x)) return JS_NewFloat64(ctx, NAN);
    if (x == 0.0) return JS_NewFloat64(ctx, x);
    if (x > 0.0) return JS_NewInt32(ctx, 1);
    return JS_NewInt32(ctx, -1);
}

static LRValue js_math_sin(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, sin(x));
}

static LRValue js_math_sinh(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, sinh(x));
}

static LRValue js_math_sqrt(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, sqrt(x));
}

static LRValue js_math_tan(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, tan(x));
}

static LRValue js_math_tanh(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    return JS_NewFloat64(ctx, tanh(x));
}

static LRValue js_math_trunc(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    double x = get_double_arg(ctx, argc, argv, 0, NAN);
    if (isnan(x) || isinf(x) || x == 0.0) return JS_NewFloat64(ctx, x);
    double t;
    if (x > 0.0) t = floor(x); else t = ceil(x);
    return JS_NewFloat64(ctx, t);
}

/* Math function list */
static const JSCFunctionListEntry js_math_funcs[] = {
    JS_CFUNC_DEF("abs",     1, js_math_abs),
    JS_CFUNC_DEF("acos",    1, js_math_acos),
    JS_CFUNC_DEF("acosh",   1, js_math_acosh),
    JS_CFUNC_DEF("asin",    1, js_math_asin),
    JS_CFUNC_DEF("asinh",   1, js_math_asinh),
    JS_CFUNC_DEF("atan",    1, js_math_atan),
    JS_CFUNC_DEF("atan2",   2, js_math_atan2),
    JS_CFUNC_DEF("atanh",   1, js_math_atanh),
    JS_CFUNC_DEF("cbrt",    1, js_math_cbrt),
    JS_CFUNC_DEF("ceil",    1, js_math_ceil),
    JS_CFUNC_DEF("clz32",   1, js_math_clz32),
    JS_CFUNC_DEF("cos",     1, js_math_cos),
    JS_CFUNC_DEF("cosh",    1, js_math_cosh),
    JS_CFUNC_DEF("exp",     1, js_math_exp),
    JS_CFUNC_DEF("expm1",   1, js_math_expm1),
    JS_CFUNC_DEF("floor",   1, js_math_floor),
    JS_CFUNC_DEF("fround",  1, js_math_fround),
    JS_CFUNC_DEF("hypot",   2, js_math_hypot),
    JS_CFUNC_DEF("imul",    2, js_math_imul),
    JS_CFUNC_DEF("log",     1, js_math_log),
    JS_CFUNC_DEF("log10",   1, js_math_log10),
    JS_CFUNC_DEF("log1p",   1, js_math_log1p),
    JS_CFUNC_DEF("log2",    1, js_math_log2),
    JS_CFUNC_DEF("max",     2, js_math_max),
    JS_CFUNC_DEF("min",     2, js_math_min),
    JS_CFUNC_DEF("pow",     2, js_math_pow),
    JS_CFUNC_DEF("random",  0, js_math_random),
    JS_CFUNC_DEF("round",   1, js_math_round),
    JS_CFUNC_DEF("sign",    1, js_math_sign),
    JS_CFUNC_DEF("sin",     1, js_math_sin),
    JS_CFUNC_DEF("sinh",    1, js_math_sinh),
    JS_CFUNC_DEF("sqrt",    1, js_math_sqrt),
    JS_CFUNC_DEF("tan",     1, js_math_tan),
    JS_CFUNC_DEF("tanh",    1, js_math_tanh),
    JS_CFUNC_DEF("trunc",   1, js_math_trunc),
};

/* ========================================================================
 *  2. JSON OBJECT
 * ======================================================================== */

/* ── JSON.stringify helper ─────────────────────────────────────────────── */

static LRValue js_json_stringify_internal(LRContext *ctx, LRValue val, int depth);

static void json_escape_string(LRContext *ctx, char *buf, size_t buf_size, const char *str)
{
    size_t pos = 0;
    buf[pos++] = '"';
    for (const char *p = str; *p && pos < buf_size - 6; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  buf[pos++] = '\\'; buf[pos++] = '"';  break;
        case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
        case '\b': buf[pos++] = '\\'; buf[pos++] = 'b';  break;
        case '\f': buf[pos++] = '\\'; buf[pos++] = 'f';  break;
        case '\n': buf[pos++] = '\\'; buf[pos++] = 'n';  break;
        case '\r': buf[pos++] = '\\'; buf[pos++] = 'r';  break;
        case '\t': buf[pos++] = '\\'; buf[pos++] = 't';  break;
        default:
            if (c < 0x20) {
                pos += snprintf(buf + pos, buf_size - pos, "\\u%04x", c);
            } else {
                buf[pos++] = c;
            }
            break;
        }
    }
    buf[pos++] = '"';
    buf[pos] = '\0';
}

static LRValue js_json_stringify_internal(LRContext *ctx, LRValue val, int depth)
{
    if (depth > 10) return JS_UNDEFINED;

    switch (val.tag) {
    case LR_TYPE_UNDEFINED:
        return JS_UNDEFINED;
    case LR_TYPE_NULL:
        return JS_NewString(ctx, "null");
    case LR_TYPE_BOOL:
        return JS_NewString(ctx, val.u.bool_val ? "true" : "false");
    case LR_TYPE_INT32:
    case LR_TYPE_FLOAT64: {
        double d;
        JS_ToFloat64(ctx, &d, val);
        char buf[64];
        if (isnan(d) || isinf(d)) return JS_NewString(ctx, "null");
        snprintf(buf, sizeof(buf), "%.17g", d);
        return JS_NewString(ctx, buf);
    }
    case LR_TYPE_STRING: {
        const char *s = JS_ToCString(ctx, val);
        if (!s) return JS_UNDEFINED;
        char buf[4096];
        json_escape_string(ctx, buf, sizeof(buf), s);
        JS_FreeCString(ctx, s);
        return JS_NewString(ctx, buf);
    }
    case LR_TYPE_OBJECT: {
        if (JS_IsArray(ctx, val)) {
            /* Array */
            char buf[8192];
            size_t pos = 0;
            buf[pos++] = '[';
            uint32_t len = 0;
            /* Get array length */
            JSValue len_val = JS_GetPropertyStr(ctx, val, "length");
            JS_ToInt32(ctx, (int32_t *)&len, len_val);
            JS_FreeValue(ctx, len_val);
            int first = 1;
            for (uint32_t i = 0; i < len; i++) {
                if (!first) { if (pos < sizeof(buf) - 2) buf[pos++] = ','; }
                first = 0;
                JSValue elem = JS_GetPropertyUint32(ctx, val, i);
                LRValue str_val = js_json_stringify_internal(ctx, elem, depth + 1);
                JS_FreeValue(ctx, elem);
                if (JS_IsUndefined(str_val)) {
                    /* undefined in array becomes null */
                    const char *null_str = "null";
                    size_t nlen = strlen(null_str);
                    if (pos + nlen < sizeof(buf) - 1) {
                        memcpy(buf + pos, null_str, nlen);
                        pos += nlen;
                    }
                    continue;
                }
                const char *s = JS_ToCString(ctx, str_val);
                if (s) {
                    size_t slen = strlen(s);
                    if (pos + slen < sizeof(buf) - 1) {
                        memcpy(buf + pos, s, slen);
                        pos += slen;
                    }
                    JS_FreeCString(ctx, s);
                }
                JS_FreeValue(ctx, str_val);
            }
            if (pos < sizeof(buf) - 1) buf[pos++] = ']';
            buf[pos] = '\0';
            return JS_NewString(ctx, buf);
        } else if (JS_IsFunction(ctx, val)) {
            return JS_UNDEFINED;
        } else {
            /* Object */
            char buf[8192];
            size_t pos = 0;
            buf[pos++] = '{';
            JSPropertyEnum *tab = NULL;
            uint32_t len = 0;
            int first = 1;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, val,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; i++) {
                    const char *key = JS_AtomToCString(ctx, tab[i].atom);
                    if (!key) continue;
                    JSValue prop_val = JS_GetProperty(ctx, val, tab[i].atom);
                    LRValue str_val = js_json_stringify_internal(ctx, prop_val, depth + 1);
                    JS_FreeValue(ctx, prop_val);
                    if (!JS_IsUndefined(str_val)) {
                        if (!first) { if (pos < sizeof(buf) - 2) buf[pos++] = ','; }
                        first = 0;
                        char key_buf[256];
                        json_escape_string(ctx, key_buf, sizeof(key_buf), key);
                        size_t klen = strlen(key_buf);
                        if (pos + klen + 1 < sizeof(buf) - 1) {
                            memcpy(buf + pos, key_buf, klen);
                            pos += klen;
                        }
                        if (pos < sizeof(buf) - 2) buf[pos++] = ':';
                        const char *vs = JS_ToCString(ctx, str_val);
                        if (vs) {
                            size_t vlen = strlen(vs);
                            if (pos + vlen < sizeof(buf) - 1) {
                                memcpy(buf + pos, vs, vlen);
                                pos += vlen;
                            }
                            JS_FreeCString(ctx, vs);
                        }
                        JS_FreeValue(ctx, str_val);
                    }
                    JS_FreeCString(ctx, key);
                }
                JS_FreePropertyEnum(ctx, tab, len);
            }
            if (pos < sizeof(buf) - 1) buf[pos++] = '}';
            buf[pos] = '\0';
            return JS_NewString(ctx, buf);
        }
    }
    case LR_TYPE_SYMBOL:
        return JS_UNDEFINED;
    default:
        return JS_UNDEFINED;
    }
}

static LRValue js_json_stringify(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    LRValue result = js_json_stringify_internal(ctx, argv[0], 0);
    if (JS_IsUndefined(result)) return JS_UNDEFINED;
    return result;
}

/* ── JSON.parse helper ─────────────────────────────────────────────────── */

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
} JSONParser;

static void json_skip_ws(JSONParser *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char json_peek(JSONParser *p)
{
    json_skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static char json_next(JSONParser *p)
{
    json_skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos++];
}

static LRValue json_parse_value(LRContext *ctx, JSONParser *p);

static LRValue json_parse_string(LRContext *ctx, JSONParser *p)
{
    if (json_next(p) != '"') {
        return JS_ThrowTypeError(ctx, "JSON.parse: expected string");
    }
    char buf[4096];
    size_t pos = 0;
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') break;
        if (c == '\\') {
            if (p->pos >= p->len) return JS_ThrowTypeError(ctx, "JSON.parse: unexpected end");
            char esc = p->src[p->pos++];
            switch (esc) {
            case '"':  buf[pos++] = '"';  break;
            case '\\': buf[pos++] = '\\'; break;
            case '/':  buf[pos++] = '/';  break;
            case 'b':  buf[pos++] = '\b'; break;
            case 'f':  buf[pos++] = '\f'; break;
            case 'n':  buf[pos++] = '\n'; break;
            case 'r':  buf[pos++] = '\r'; break;
            case 't':  buf[pos++] = '\t'; break;
            case 'u': {
                if (p->pos + 4 > p->len) return JS_ThrowTypeError(ctx, "JSON.parse: invalid unicode escape");
                char hex[5] = {0};
                memcpy(hex, p->src + p->pos, 4);
                p->pos += 4;
                unsigned int code = (unsigned int)strtoul(hex, NULL, 16);
                if (code < 0x80) {
                    buf[pos++] = (char)code;
                } else if (code < 0x800) {
                    buf[pos++] = (char)(0xC0 | (code >> 6));
                    buf[pos++] = (char)(0x80 | (code & 0x3F));
                } else {
                    buf[pos++] = (char)(0xE0 | (code >> 12));
                    buf[pos++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    buf[pos++] = (char)(0x80 | (code & 0x3F));
                }
                break;
            }
            default:
                buf[pos++] = esc;
                break;
            }
        } else {
            buf[pos++] = c;
        }
        if (pos >= sizeof(buf) - 1) return JS_ThrowTypeError(ctx, "JSON.parse: string too long");
    }
    buf[pos] = '\0';
    return JS_NewString(ctx, buf);
}

static LRValue json_parse_number(LRContext *ctx, JSONParser *p)
{
    size_t start = p->pos;
    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '0') {
        p->pos++;
    } else if (p->pos < p->len && p->src[p->pos] >= '1' && p->src[p->pos] <= '9') {
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }
    char buf[128];
    size_t len = p->pos - start;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, p->src + start, len);
    buf[len] = '\0';
    char *end = NULL;
    double d = strtod(buf, &end);
    (void)end;
    return JS_NewFloat64(ctx, d);
}

static LRValue json_parse_array(LRContext *ctx, JSONParser *p)
{
    if (json_next(p) != '[') {
        return JS_ThrowTypeError(ctx, "JSON.parse: expected '['");
    }
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    if (json_peek(p) != ']') {
        while (1) {
            LRValue val = json_parse_value(ctx, p);
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, arr);
                return val;
            }
            JS_SetPropertyUint32(ctx, arr, idx++, val);
            char c = json_next(p);
            if (c == ']') break;
            if (c != ',') {
                JS_FreeValue(ctx, arr);
                return JS_ThrowTypeError(ctx, "JSON.parse: expected ',' or ']'");
            }
        }
    } else {
        p->pos++; /* skip ] */
    }
    return arr;
}

static LRValue json_parse_object(LRContext *ctx, JSONParser *p)
{
    if (json_next(p) != '{') {
        return JS_ThrowTypeError(ctx, "JSON.parse: expected '{'");
    }
    JSValue obj = JS_NewObject(ctx);
    if (json_peek(p) != '}') {
        while (1) {
            LRValue key = json_parse_string(ctx, p);
            if (JS_IsException(key)) {
                JS_FreeValue(ctx, obj);
                return key;
            }
            const char *key_str = JS_ToCString(ctx, key);
            char c = json_next(p);
            if (c != ':') {
                JS_FreeCString(ctx, key_str);
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, obj);
                return JS_ThrowTypeError(ctx, "JSON.parse: expected ':'");
            }
            LRValue val = json_parse_value(ctx, p);
            if (JS_IsException(val)) {
                JS_FreeCString(ctx, key_str);
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, obj);
                return val;
            }
            if (key_str) {
                JS_SetPropertyStr(ctx, obj, key_str, val);
                JS_FreeCString(ctx, key_str);
            } else {
                JS_FreeValue(ctx, val);
            }
            JS_FreeValue(ctx, key);
            c = json_next(p);
            if (c == '}') break;
            if (c != ',') {
                JS_FreeValue(ctx, obj);
                return JS_ThrowTypeError(ctx, "JSON.parse: expected ',' or '}'");
            }
        }
    } else {
        p->pos++; /* skip } */
    }
    return obj;
}

static LRValue json_parse_value(LRContext *ctx, JSONParser *p)
{
    char c = json_peek(p);
    switch (c) {
    case '"':
        return json_parse_string(ctx, p);
    case '{':
        return json_parse_object(ctx, p);
    case '[':
        return json_parse_array(ctx, p);
    case 't':
        if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "true", 4) == 0) {
            p->pos += 4;
            return JS_TRUE;
        }
        return JS_ThrowTypeError(ctx, "JSON.parse: unexpected token");
    case 'f':
        if (p->pos + 5 <= p->len && memcmp(p->src + p->pos, "false", 5) == 0) {
            p->pos += 5;
            return JS_FALSE;
        }
        return JS_ThrowTypeError(ctx, "JSON.parse: unexpected token");
    case 'n':
        if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "null", 4) == 0) {
            p->pos += 4;
            return JS_NULL;
        }
        return JS_ThrowTypeError(ctx, "JSON.parse: unexpected token");
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return json_parse_number(ctx, p);
    default:
        return JS_ThrowTypeError(ctx, "JSON.parse: unexpected token");
    }
}

static LRValue js_json_parse(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "JSON.parse: missing argument");
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_ThrowTypeError(ctx, "JSON.parse: cannot convert to string");
    JSONParser p;
    p.src = str;
    p.pos = 0;
    p.len = strlen(str);
    LRValue result = json_parse_value(ctx, &p);
    JS_FreeCString(ctx, str);
    return result;
}

/* JSON function list */
static const JSCFunctionListEntry js_json_funcs[] = {
    JS_CFUNC_DEF("stringify", 1, js_json_stringify),
    JS_CFUNC_DEF("parse",     1, js_json_parse),
};

/* ========================================================================
 *  3. DATE CONSTRUCTOR AND PROTOTYPE
 * ======================================================================== */

/* Date data: stored as int64_t* (ms since epoch) via opaque */

static int64_t js_date_current_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static int64_t *js_date_get_data(LRValue this_val)
{
    return (int64_t *)JS_GetOpaque(this_val, NULL);
}

/* Date constructor */
static LRValue js_date_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = (int64_t *)malloc(sizeof(int64_t));
    if (!data) return JS_ThrowTypeError(ctx, "Date: out of memory");

    if (argc == 0) {
        *data = js_date_current_ms();
    } else if (argc == 1) {
        /* Single argument: number (timestamp) or string (parse) */
        if (JS_IsNumber(argv[0]) || JS_IsString(argv[0])) {
            double d;
            if (JS_ToFloat64(ctx, &d, argv[0]) == 0) {
                *data = (int64_t)d;
            } else {
                const char *s = JS_ToCString(ctx, argv[0]);
                if (s) {
                    /* Try to parse */
                    struct tm tm = {0};
                    if (strptime(s, "%Y-%m-%dT%H:%M:%S", &tm) || strptime(s, "%Y-%m-%d", &tm)) {
                        time_t t = timegm(&tm);
                        *data = (int64_t)t * 1000;
                    } else {
                        /* Try numeric */
                        char *end = NULL;
                        double d2 = strtod(s, &end);
                        *data = (int64_t)d2;
                    }
                    JS_FreeCString(ctx, s);
                } else {
                    *data = 0;
                }
            }
        } else {
            *data = js_date_current_ms();
        }
    } else {
        /* Multiple arguments: year, month, day, hour, min, sec, ms */
        int32_t year = get_int_arg(ctx, argc, argv, 0, 1900);
        int32_t month = get_int_arg(ctx, argc, argv, 1, 0);
        int32_t day = get_int_arg(ctx, argc, argv, 2, 1);
        int32_t hour = get_int_arg(ctx, argc, argv, 3, 0);
        int32_t min = get_int_arg(ctx, argc, argv, 4, 0);
        int32_t sec = get_int_arg(ctx, argc, argv, 5, 0);
        int32_t ms = get_int_arg(ctx, argc, argv, 6, 0);

        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = year - 1900;
        tm.tm_mon = month;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        tm.tm_isdst = -1;
        time_t t = timegm(&tm);
        *data = (int64_t)t * 1000 + ms;
    }

    JS_SetOpaque(this_val, data);
    return JS_DupValue(ctx, this_val);
}

/* Date.prototype methods */
static LRValue js_date_getFullYear(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getFullYear called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_year + 1900);
}

static LRValue js_date_getMonth(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getMonth called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_mon);
}

static LRValue js_date_getDate(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getDate called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_mday);
}

static LRValue js_date_getDay(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getDay called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_wday);
}

static LRValue js_date_getHours(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getHours called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_hour);
}

static LRValue js_date_getMinutes(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getMinutes called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_min);
}

static LRValue js_date_getSeconds(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getSeconds called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    return JS_NewInt32(ctx, tm.tm_sec);
}

static LRValue js_date_getMilliseconds(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getMilliseconds called on non-Date");
    return JS_NewInt32(ctx, (int32_t)(*data % 1000));
}

static LRValue js_date_getTime(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.getTime called on non-Date");
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setFullYear(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setFullYear called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm.tm_year = get_int_arg(ctx, argc, argv, 0, 1900) - 1900;
    tm.tm_isdst = -1;
    time_t new_t = timegm(&tm);
    *data = (int64_t)new_t * 1000 + (*data % 1000);
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setMonth(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setMonth called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm.tm_mon = get_int_arg(ctx, argc, argv, 0, 0);
    tm.tm_isdst = -1;
    time_t new_t = timegm(&tm);
    *data = (int64_t)new_t * 1000 + (*data % 1000);
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setDate(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setDate called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm.tm_mday = get_int_arg(ctx, argc, argv, 0, 1);
    tm.tm_isdst = -1;
    time_t new_t = timegm(&tm);
    *data = (int64_t)new_t * 1000 + (*data % 1000);
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setHours(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setHours called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm.tm_hour = get_int_arg(ctx, argc, argv, 0, 0);
    tm.tm_isdst = -1;
    time_t new_t = timegm(&tm);
    *data = (int64_t)new_t * 1000 + (*data % 1000);
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setMinutes(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setMinutes called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm.tm_min = get_int_arg(ctx, argc, argv, 0, 0);
    tm.tm_isdst = -1;
    time_t new_t = timegm(&tm);
    *data = (int64_t)new_t * 1000 + (*data % 1000);
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setSeconds(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setSeconds called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm.tm_sec = get_int_arg(ctx, argc, argv, 0, 0);
    tm.tm_isdst = -1;
    time_t new_t = timegm(&tm);
    *data = (int64_t)new_t * 1000 + (*data % 1000);
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setMilliseconds(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setMilliseconds called on non-Date");
    int32_t ms = get_int_arg(ctx, argc, argv, 0, 0);
    *data = (*data / 1000) * 1000 + ms;
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_setTime(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.setTime called on non-Date");
    double d = get_double_arg(ctx, argc, argv, 0, 0);
    *data = (int64_t)d;
    return JS_NewFloat64(ctx, (double)*data);
}

static LRValue js_date_toString(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.toString called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday], mons[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return JS_NewString(ctx, buf);
}

static LRValue js_date_toISOString(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.toISOString called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(*data % 1000));
    return JS_NewString(ctx, buf);
}

static LRValue js_date_toJSON(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    return js_date_toISOString(ctx, this_val, 0, NULL);
}

static LRValue js_date_toLocaleDateString(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.toLocaleDateString called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return JS_NewString(ctx, buf);
}

static LRValue js_date_toLocaleTimeString(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    int64_t *data = js_date_get_data(this_val);
    if (!data) return JS_ThrowTypeError(ctx, "Date.prototype.toLocaleTimeString called on non-Date");
    time_t t = (time_t)(*data / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return JS_NewString(ctx, buf);
}

static LRValue js_date_valueOf(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    return js_date_getTime(ctx, this_val, 0, NULL);
}

/* Static Date methods */
static LRValue js_date_now(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)js_date_current_ms());
}

static LRValue js_date_parse(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NewFloat64(ctx, NAN);
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_NewFloat64(ctx, NAN);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    double ms = 0.0;
    if (strptime(s, "%Y-%m-%dT%H:%M:%S", &tm)) {
        time_t t = timegm(&tm);
        ms = (double)t * 1000.0;
    } else if (strptime(s, "%Y-%m-%d", &tm)) {
        time_t t = timegm(&tm);
        ms = (double)t * 1000.0;
    } else {
        ms = NAN;
    }
    JS_FreeCString(ctx, s);
    return JS_NewFloat64(ctx, ms);
}

static LRValue js_date_utc(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = get_int_arg(ctx, argc, argv, 0, 1900) - 1900;
    tm.tm_mon = get_int_arg(ctx, argc, argv, 1, 0);
    tm.tm_mday = get_int_arg(ctx, argc, argv, 2, 1);
    tm.tm_hour = get_int_arg(ctx, argc, argv, 3, 0);
    tm.tm_min = get_int_arg(ctx, argc, argv, 4, 0);
    tm.tm_sec = get_int_arg(ctx, argc, argv, 5, 0);
    tm.tm_isdst = -1;
    int32_t ms = get_int_arg(ctx, argc, argv, 6, 0);
    time_t t = timegm(&tm);
    return JS_NewFloat64(ctx, (double)t * 1000.0 + ms);
}

/* Date prototype methods */
static const JSCFunctionListEntry js_date_proto_methods[] = {
    JS_CFUNC_DEF("getFullYear",       0, js_date_getFullYear),
    JS_CFUNC_DEF("getMonth",          0, js_date_getMonth),
    JS_CFUNC_DEF("getDate",           0, js_date_getDate),
    JS_CFUNC_DEF("getDay",            0, js_date_getDay),
    JS_CFUNC_DEF("getHours",          0, js_date_getHours),
    JS_CFUNC_DEF("getMinutes",        0, js_date_getMinutes),
    JS_CFUNC_DEF("getSeconds",        0, js_date_getSeconds),
    JS_CFUNC_DEF("getMilliseconds",   0, js_date_getMilliseconds),
    JS_CFUNC_DEF("getTime",           0, js_date_getTime),
    JS_CFUNC_DEF("setFullYear",       1, js_date_setFullYear),
    JS_CFUNC_DEF("setMonth",          1, js_date_setMonth),
    JS_CFUNC_DEF("setDate",           1, js_date_setDate),
    JS_CFUNC_DEF("setHours",          1, js_date_setHours),
    JS_CFUNC_DEF("setMinutes",        1, js_date_setMinutes),
    JS_CFUNC_DEF("setSeconds",        1, js_date_setSeconds),
    JS_CFUNC_DEF("setMilliseconds",   1, js_date_setMilliseconds),
    JS_CFUNC_DEF("setTime",           1, js_date_setTime),
    JS_CFUNC_DEF("toString",          0, js_date_toString),
    JS_CFUNC_DEF("toISOString",       0, js_date_toISOString),
    JS_CFUNC_DEF("toJSON",            1, js_date_toJSON),
    JS_CFUNC_DEF("toLocaleDateString", 0, js_date_toLocaleDateString),
    JS_CFUNC_DEF("toLocaleTimeString", 0, js_date_toLocaleTimeString),
    JS_CFUNC_DEF("valueOf",           0, js_date_valueOf),
};

/* Date static methods */
static const JSCFunctionListEntry js_date_static_methods[] = {
    JS_CFUNC_DEF("now",   0, js_date_now),
    JS_CFUNC_DEF("parse", 1, js_date_parse),
    JS_CFUNC_DEF("UTC",   7, js_date_utc),
};

/* ========================================================================
 *  4. REGEXP CONSTRUCTOR AND PROTOTYPE
 * ======================================================================== */

typedef struct {
    regex_t  *re;
    char     *pattern;
    char     *flags;
    int       cflags;       /* POSIX regcomp flags */
    int       global : 1;
    int       ignoreCase : 1;
    int       multiline : 1;
    int       dotAll : 1;
    int       sticky : 1;
    int       unicode : 1;
} RegExpData;

static void regexp_data_free(RegExpData *rd)
{
    if (rd->re) {
        regfree(rd->re);
        free(rd->re);
    }
    free(rd->pattern);
    free(rd->flags);
    free(rd);
}

/* RegExp constructor */
static LRValue js_regexp_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    const char *pattern_str = "";
    const char *flags_str = "";

    if (argc >= 1) {
        if (JS_IsObject(argv[0])) {
            /* If first arg is a RegExp, extract its pattern and flags */
            JSValue src = JS_GetPropertyStr(ctx, argv[0], "source");
            JSValue flg = JS_GetPropertyStr(ctx, argv[0], "flags");
            pattern_str = JS_ToCString(ctx, src);
            if (argc >= 2) {
                /* Flags overrides the existing flags */
                if (JS_IsString(argv[1]) || JS_IsNumber(argv[1])) {
                    const char *f = JS_ToCString(ctx, argv[1]);
                    if (f) { flags_str = f; JS_FreeCString(ctx, f); }
                }
            } else {
                flags_str = JS_ToCString(ctx, flg);
            }
            JS_FreeValue(ctx, flg);
            JS_FreeValue(ctx, src);
        } else {
            pattern_str = JS_ToCString(ctx, argv[0]);
            if (!pattern_str) pattern_str = "";
        }
    }

    /* If no pattern, use empty string */
    if (!pattern_str) pattern_str = "";

    int cflags = REG_EXTENDED;
    int global = 0, ignoreCase = 0, multiline = 0;
    int dotAll = 0, sticky = 0, unicode = 0;

    if (argc >= 2) {
        const char *f = JS_ToCString(ctx, argv[1]);
        if (f) {
            flags_str = f;
            for (const char *p = f; *p; p++) {
                switch (*p) {
                case 'g': global = 1; break;
                case 'i': ignoreCase = 1; cflags |= REG_ICASE; break;
                case 'm': multiline = 1; cflags |= REG_NEWLINE; break;
                case 's': dotAll = 1; break;
                case 'y': sticky = 1; break;
                case 'u': unicode = 1; break;
                default:
                    JS_FreeCString(ctx, f);
                    const char *pat = pattern_str;
                    if (argc < 1) pat = "";
                    JS_FreeCString(ctx, pat);
                    return JS_ThrowTypeError(ctx, "Invalid RegExp flag: %c", *p);
                }
            }
            JS_FreeCString(ctx, f);
        }
    }

    /* Compile the regex */
    regex_t *re = (regex_t *)malloc(sizeof(regex_t));
    if (!re) {
        if (argc >= 1 && !JS_IsObject(argv[0])) JS_FreeCString(ctx, pattern_str);
        return JS_ThrowTypeError(ctx, "RegExp: out of memory");
    }

    int err = regcomp(re, pattern_str, cflags);
    if (err != 0) {
        char errbuf[256];
        regerror(err, re, errbuf, sizeof(errbuf));
        free(re);
        if (argc >= 1 && !JS_IsObject(argv[0])) JS_FreeCString(ctx, pattern_str);
        return JS_ThrowTypeError(ctx, "RegExp: %s", errbuf);
    }

    RegExpData *rd = (RegExpData *)malloc(sizeof(RegExpData));
    if (!rd) {
        regfree(re);
        free(re);
        if (argc >= 1 && !JS_IsObject(argv[0])) JS_FreeCString(ctx, pattern_str);
        return JS_ThrowTypeError(ctx, "RegExp: out of memory");
    }
    rd->re = re;
    rd->pattern = strdup(pattern_str);
    rd->flags = strdup(argc >= 2 && flags_str ? flags_str : "");
    rd->cflags = cflags;
    rd->global = global;
    rd->ignoreCase = ignoreCase;
    rd->multiline = multiline;
    rd->dotAll = dotAll;
    rd->sticky = sticky;
    rd->unicode = unicode;

    if (argc >= 1 && !JS_IsObject(argv[0])) JS_FreeCString(ctx, pattern_str);

    JS_SetOpaque(this_val, rd);
    return JS_DupValue(ctx, this_val);
}

/* RegExp.prototype.exec */
static LRValue js_regexp_exec(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.exec called on non-RegExp");

    if (argc < 1) return JS_NULL;
    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str) return JS_NULL;

    /* Get lastIndex */
    int64_t lastIndex = 0;
    JSValue li_val = JS_GetPropertyStr(ctx, this_val, "lastIndex");
    if (!JS_IsUndefined(li_val)) {
        double d;
        JS_ToFloat64(ctx, &d, li_val);
        lastIndex = (int64_t)d;
    }
    JS_FreeValue(ctx, li_val);

    /* If not global/sticky, start from 0 */
    if (!rd->global && !rd->sticky) lastIndex = 0;

    if (lastIndex < 0) lastIndex = 0;
    if ((size_t)lastIndex > strlen(str)) {
        if (rd->global || rd->sticky) {
            JS_SetPropertyStr(ctx, this_val, "lastIndex", JS_NewInt32(ctx, 0));
        }
        JS_FreeCString(ctx, str);
        return JS_NULL;
    }

    /* Execute match from lastIndex position */
    regmatch_t pmatch[32];
    int eflags = 0;
    int ret = regexec(rd->re, str + lastIndex, 32, pmatch, eflags);

    if (ret != 0) {
        if (rd->global || rd->sticky) {
            JS_SetPropertyStr(ctx, this_val, "lastIndex", JS_NewInt32(ctx, 0));
        }
        JS_FreeCString(ctx, str);
        return JS_NULL;
    }

    /* Build result array */
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;

    for (int i = 0; i < 32 && pmatch[i].rm_so >= 0; i++) {
        size_t start = (size_t)pmatch[i].rm_so + (size_t)lastIndex;
        size_t end = (size_t)pmatch[i].rm_eo + (size_t)lastIndex;
        size_t match_len = end - start;
        char *match_str = (char *)malloc(match_len + 1);
        if (match_str) {
            memcpy(match_str, str + start, match_len);
            match_str[match_len] = '\0';
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, match_str));
            free(match_str);
        }
    }

    /* Set index and input properties */
    JS_SetPropertyStr(ctx, arr, "index", JS_NewInt32(ctx, (int32_t)lastIndex + pmatch[0].rm_so));
    JS_SetPropertyStr(ctx, arr, "input", JS_NewString(ctx, str));

    /* Update lastIndex for global/sticky */
    if (rd->global || rd->sticky) {
        int64_t newIndex = lastIndex + pmatch[0].rm_eo;
        JS_SetPropertyStr(ctx, this_val, "lastIndex", JS_NewFloat64(ctx, (double)newIndex));
    }

    JS_FreeCString(ctx, str);
    return arr;
}

/* RegExp.prototype.test */
static LRValue js_regexp_test(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    JSValue result = js_regexp_exec(ctx, this_val, argc, argv);
    if (JS_IsException(result)) return result;
    if (JS_IsNull(result)) return JS_FALSE;
    JS_FreeValue(ctx, result);
    return JS_TRUE;
}

/* RegExp.prototype.toString */
static LRValue js_regexp_toString(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.toString called on non-RegExp");
    char buf[4096];
    snprintf(buf, sizeof(buf), "/%s/%s", rd->pattern, rd->flags ? rd->flags : "");
    return JS_NewString(ctx, buf);
}

/* RegExp getters */
static LRValue js_regexp_get_source(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.source getter called on non-RegExp");
    return JS_NewString(ctx, rd->pattern);
}

static LRValue js_regexp_get_flags(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.flags getter called on non-RegExp");
    return JS_NewString(ctx, rd->flags ? rd->flags : "");
}

static LRValue js_regexp_get_global(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.global getter called on non-RegExp");
    return JS_NewBool(ctx, rd->global);
}

static LRValue js_regexp_get_ignoreCase(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.ignoreCase getter called on non-RegExp");
    return JS_NewBool(ctx, rd->ignoreCase);
}

static LRValue js_regexp_get_multiline(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.multiline getter called on non-RegExp");
    return JS_NewBool(ctx, rd->multiline);
}

static LRValue js_regexp_get_dotAll(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.dotAll getter called on non-RegExp");
    return JS_NewBool(ctx, rd->dotAll);
}

static LRValue js_regexp_get_sticky(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.sticky getter called on non-RegExp");
    return JS_NewBool(ctx, rd->sticky);
}

static LRValue js_regexp_get_unicode(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    RegExpData *rd = (RegExpData *)JS_GetOpaque(this_val, NULL);
    if (!rd) return JS_ThrowTypeError(ctx, "RegExp.prototype.unicode getter called on non-RegExp");
    return JS_NewBool(ctx, rd->unicode);
}

/* RegExp prototype methods */
static const JSCFunctionListEntry js_regexp_proto_methods[] = {
    JS_CFUNC_DEF("exec",      1, js_regexp_exec),
    JS_CFUNC_DEF("test",      1, js_regexp_test),
    JS_CFUNC_DEF("toString",  0, js_regexp_toString),
};

/* RegExp getter/setter definitions */
static const JSCFunctionListEntry js_regexp_proto_getters[] = {
    JS_CGETSET_DEF("source",     js_regexp_get_source,     NULL),
    JS_CGETSET_DEF("flags",      js_regexp_get_flags,      NULL),
    JS_CGETSET_DEF("global",     js_regexp_get_global,     NULL),
    JS_CGETSET_DEF("ignoreCase", js_regexp_get_ignoreCase, NULL),
    JS_CGETSET_DEF("multiline",  js_regexp_get_multiline,  NULL),
    JS_CGETSET_DEF("dotAll",     js_regexp_get_dotAll,     NULL),
    JS_CGETSET_DEF("sticky",     js_regexp_get_sticky,     NULL),
    JS_CGETSET_DEF("unicode",    js_regexp_get_unicode,    NULL),
};

/* ========================================================================
 *  5. SYMBOL CONSTRUCTOR AND PROTOTYPE
 * ======================================================================== */

/* Global symbol registry */
typedef struct GlobalSymbolEntry {
    char *key;
    LRValue sym;
    struct GlobalSymbolEntry *next;
} GlobalSymbolEntry;

static GlobalSymbolEntry *g_symbol_registry = NULL;

/* Symbol constructor */
static LRValue js_symbol_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    const char *desc = "";
    if (argc >= 1) {
        desc = JS_ToCString(ctx, argv[0]);
        if (!desc) desc = "";
    }

    /* Create a new unique symbol object */
    JSValue sym = JS_NewObject(ctx);
    JS_SetOpaque(sym, (void *)(desc ? strdup(desc) : strdup("")));
    if (desc && argc >= 1) JS_FreeCString(ctx, desc);

    /* Mark as symbol type (internal: use LR_OBJ_PLAIN, but we store description as opaque) */
    return sym;
}

/* Symbol.for */
static LRValue js_symbol_for(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Symbol.for: missing argument");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_ThrowTypeError(ctx, "Symbol.for: invalid key");

    /* Search existing registry */
    GlobalSymbolEntry *entry = g_symbol_registry;
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            JS_FreeCString(ctx, key);
            return JS_DupValue(ctx, entry->sym);
        }
        entry = entry->next;
    }

    /* Create new symbol */
    JSValue sym = JS_NewObject(ctx);
    JS_SetOpaque(sym, strdup(key));

    /* Add to registry */
    GlobalSymbolEntry *new_entry = (GlobalSymbolEntry *)malloc(sizeof(GlobalSymbolEntry));
    if (new_entry) {
        new_entry->key = strdup(key);
        new_entry->sym = JS_DupValue(ctx, sym);
        new_entry->next = g_symbol_registry;
        g_symbol_registry = new_entry;
    }

    JS_FreeCString(ctx, key);
    return sym;
}

/* Symbol.keyFor */
static LRValue js_symbol_keyFor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Symbol.keyFor: missing argument");
    if (!JS_IsSymbol(argv[0]) && !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "Symbol.keyFor: not a symbol");
    }

    const char *desc = (const char *)JS_GetOpaque(argv[0], NULL);
    if (!desc) return JS_UNDEFINED;

    /* Search registry for matching description */
    GlobalSymbolEntry *entry = g_symbol_registry;
    while (entry) {
        if (strcmp(entry->key, desc) == 0) {
            return JS_NewString(ctx, entry->key);
        }
        entry = entry->next;
    }
    return JS_UNDEFINED;
}

/* Symbol.prototype.toString */
static LRValue js_symbol_toString(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    const char *desc = (const char *)JS_GetOpaque(this_val, NULL);
    if (!desc) desc = "";
    char buf[256];
    snprintf(buf, sizeof(buf), "Symbol(%s)", desc);
    return JS_NewString(ctx, buf);
}

/* Symbol.prototype.valueOf */
static LRValue js_symbol_valueOf(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    const char *desc = (const char *)JS_GetOpaque(this_val, NULL);
    if (!desc) return JS_ThrowTypeError(ctx, "Symbol.prototype.valueOf called on non-Symbol");
    return JS_DupValue(ctx, this_val);
}

/* Symbol.prototype.description getter */
static LRValue js_symbol_get_description(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    const char *desc = (const char *)JS_GetOpaque(this_val, NULL);
    if (!desc) return JS_UNDEFINED;
    return JS_NewString(ctx, desc);
}

/* Symbol prototype methods */
static const JSCFunctionListEntry js_symbol_proto_methods[] = {
    JS_CFUNC_DEF("toString",  0, js_symbol_toString),
    JS_CFUNC_DEF("valueOf",   0, js_symbol_valueOf),
};

/* Symbol static methods */
static const JSCFunctionListEntry js_symbol_static_methods[] = {
    JS_CFUNC_DEF("for",    1, js_symbol_for),
    JS_CFUNC_DEF("keyFor", 1, js_symbol_keyFor),
};

/* ========================================================================
 *  6. ERROR SUBCLASSES
 * ======================================================================== */

/* Generic error subclass constructor */
static LRValue js_error_subclass_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc >= 1) {
        const char *msg = JS_ToCString(ctx, argv[0]);
        if (msg) {
            JS_SetPropertyStr(ctx, this_val, "message", JS_NewString(ctx, msg));
            JS_FreeCString(ctx, msg);
        }
    }
    return JS_DupValue(ctx, this_val);
}

/* Helper to create an error subclass */
static void create_error_subclass(LRContext *ctx, LRValue global, const char *name,
                                   JSValue error_proto, JSValue error_ctor)
{
    /* Create constructor function */
    JSValue ctor = JS_NewCFunction(ctx, js_error_subclass_constructor, name, 1);

    /* Create prototype that inherits from Error.prototype */
    JSValue proto = JS_NewObject(ctx);
    JS_SetPrototype(ctx, proto, error_proto);

    /* Set name property on prototype */
    JS_SetPropertyStr(ctx, proto, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, proto, "message", JS_NewString(ctx, ""));

    /* Set constructor property on prototype */
    JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));

    /* Set prototype on constructor */
    /* Note: lr_set_property takes ownership of value without duping,
     * so we must dup before passing to keep our local reference */
    JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));

    /* Register on global */
    JS_SetPropertyStr(ctx, global, name, ctor);

    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, ctor);
}

/* ========================================================================
 *  7. WEAKMAP / WEAKSET
 * ======================================================================== */

/* ── WeakMap ──────────────────────────────────────────────────────────── */

typedef struct {
    JSValue *keys;
    JSValue *values;
    int count;
    int capacity;
} WeakMapData;

static WeakMapData *weakmap_data_new(void)
{
    WeakMapData *wmd = (WeakMapData *)malloc(sizeof(WeakMapData));
    if (!wmd) return NULL;
    wmd->capacity = 8;
    wmd->count = 0;
    wmd->keys = (JSValue *)calloc(wmd->capacity, sizeof(JSValue));
    wmd->values = (JSValue *)calloc(wmd->capacity, sizeof(JSValue));
    if (!wmd->keys || !wmd->values) {
        free(wmd->keys);
        free(wmd->values);
        free(wmd);
        return NULL;
    }
    return wmd;
}

static void weakmap_data_free(WeakMapData *wmd)
{
    if (!wmd) return;
    free(wmd->keys);
    free(wmd->values);
    free(wmd);
}

/* WeakMap constructor */
static LRValue js_weakmap_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    WeakMapData *wmd = weakmap_data_new();
    if (!wmd) return JS_ThrowTypeError(ctx, "WeakMap: out of memory");
    JS_SetOpaque(this_val, wmd);
    return JS_DupValue(ctx, this_val);
}

/* WeakMap.prototype.set */
static LRValue js_weakmap_set(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "WeakMap.prototype.set: missing argument");
    WeakMapData *wmd = (WeakMapData *)JS_GetOpaque(this_val, NULL);
    if (!wmd) return JS_ThrowTypeError(ctx, "WeakMap.prototype.set called on non-WeakMap");
    if (!JS_IsObject(argv[0]) && !JS_IsSymbol(argv[0])) {
        return JS_ThrowTypeError(ctx, "WeakMap.prototype.set: invalid key");
    }

    /* Check if key already exists */
    for (int i = 0; i < wmd->count; i++) {
        if (wmd->keys[i].u.ptr == argv[0].u.ptr) {
            wmd->values[i] = JS_DupValue(ctx, argv[1]);
            return JS_DupValue(ctx, this_val);
        }
    }

    /* Grow if needed */
    if (wmd->count >= wmd->capacity) {
        int new_cap = wmd->capacity * 2;
        JSValue *new_keys = (JSValue *)realloc(wmd->keys, new_cap * sizeof(JSValue));
        JSValue *new_vals = (JSValue *)realloc(wmd->values, new_cap * sizeof(JSValue));
        if (!new_keys || !new_vals) {
            free(new_keys);
            free(new_vals);
            return JS_ThrowTypeError(ctx, "WeakMap: out of memory");
        }
        wmd->keys = new_keys;
        wmd->values = new_vals;
        wmd->capacity = new_cap;
    }

    wmd->keys[wmd->count] = JS_DupValue(ctx, argv[0]);
    wmd->values[wmd->count] = JS_DupValue(ctx, argv[1]);
    wmd->count++;
    return JS_DupValue(ctx, this_val);
}

/* WeakMap.prototype.get */
static LRValue js_weakmap_get(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    WeakMapData *wmd = (WeakMapData *)JS_GetOpaque(this_val, NULL);
    if (!wmd) return JS_ThrowTypeError(ctx, "WeakMap.prototype.get called on non-WeakMap");
    for (int i = 0; i < wmd->count; i++) {
        if (wmd->keys[i].u.ptr == argv[0].u.ptr) {
            return JS_DupValue(ctx, wmd->values[i]);
        }
    }
    return JS_UNDEFINED;
}

/* WeakMap.prototype.has */
static LRValue js_weakmap_has(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_FALSE;
    WeakMapData *wmd = (WeakMapData *)JS_GetOpaque(this_val, NULL);
    if (!wmd) return JS_ThrowTypeError(ctx, "WeakMap.prototype.has called on non-WeakMap");
    for (int i = 0; i < wmd->count; i++) {
        if (wmd->keys[i].u.ptr == argv[0].u.ptr) {
            return JS_TRUE;
        }
    }
    return JS_FALSE;
}

/* WeakMap.prototype.delete */
static LRValue js_weakmap_delete(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_FALSE;
    WeakMapData *wmd = (WeakMapData *)JS_GetOpaque(this_val, NULL);
    if (!wmd) return JS_ThrowTypeError(ctx, "WeakMap.prototype.delete called on non-WeakMap");
    for (int i = 0; i < wmd->count; i++) {
        if (wmd->keys[i].u.ptr == argv[0].u.ptr) {
            JS_FreeValue(ctx, wmd->keys[i]);
            JS_FreeValue(ctx, wmd->values[i]);
            /* Move last element to this position */
            wmd->count--;
            if (i < wmd->count) {
                wmd->keys[i] = wmd->keys[wmd->count];
                wmd->values[i] = wmd->values[wmd->count];
            }
            return JS_TRUE;
        }
    }
    return JS_FALSE;
}

/* WeakMap methods */
static const JSCFunctionListEntry js_weakmap_proto_methods[] = {
    JS_CFUNC_DEF("set",    2, js_weakmap_set),
    JS_CFUNC_DEF("get",    1, js_weakmap_get),
    JS_CFUNC_DEF("has",    1, js_weakmap_has),
    JS_CFUNC_DEF("delete", 1, js_weakmap_delete),
};

/* ── WeakSet ──────────────────────────────────────────────────────────── */

typedef struct {
    JSValue *values;
    int count;
    int capacity;
} WeakSetData;

static WeakSetData *weakset_data_new(void)
{
    WeakSetData *wsd = (WeakSetData *)malloc(sizeof(WeakSetData));
    if (!wsd) return NULL;
    wsd->capacity = 8;
    wsd->count = 0;
    wsd->values = (JSValue *)calloc(wsd->capacity, sizeof(JSValue));
    if (!wsd->values) {
        free(wsd);
        return NULL;
    }
    return wsd;
}

static void weakset_data_free(WeakSetData *wsd)
{
    if (!wsd) return;
    free(wsd->values);
    free(wsd);
}

/* WeakSet constructor */
static LRValue js_weakset_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    WeakSetData *wsd = weakset_data_new();
    if (!wsd) return JS_ThrowTypeError(ctx, "WeakSet: out of memory");
    JS_SetOpaque(this_val, wsd);
    return JS_DupValue(ctx, this_val);
}

/* WeakSet.prototype.add */
static LRValue js_weakset_add(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "WeakSet.prototype.add: missing argument");
    WeakSetData *wsd = (WeakSetData *)JS_GetOpaque(this_val, NULL);
    if (!wsd) return JS_ThrowTypeError(ctx, "WeakSet.prototype.add called on non-WeakSet");

    /* Check if already exists */
    for (int i = 0; i < wsd->count; i++) {
        if (wsd->values[i].u.ptr == argv[0].u.ptr) {
            return JS_DupValue(ctx, this_val);
        }
    }

    /* Grow if needed */
    if (wsd->count >= wsd->capacity) {
        int new_cap = wsd->capacity * 2;
        JSValue *new_vals = (JSValue *)realloc(wsd->values, new_cap * sizeof(JSValue));
        if (!new_vals) return JS_ThrowTypeError(ctx, "WeakSet: out of memory");
        wsd->values = new_vals;
        wsd->capacity = new_cap;
    }

    wsd->values[wsd->count++] = JS_DupValue(ctx, argv[0]);
    return JS_DupValue(ctx, this_val);
}

/* WeakSet.prototype.has */
static LRValue js_weakset_has(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_FALSE;
    WeakSetData *wsd = (WeakSetData *)JS_GetOpaque(this_val, NULL);
    if (!wsd) return JS_ThrowTypeError(ctx, "WeakSet.prototype.has called on non-WeakSet");
    for (int i = 0; i < wsd->count; i++) {
        if (wsd->values[i].u.ptr == argv[0].u.ptr) {
            return JS_TRUE;
        }
    }
    return JS_FALSE;
}

/* WeakSet.prototype.delete */
static LRValue js_weakset_delete(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_FALSE;
    WeakSetData *wsd = (WeakSetData *)JS_GetOpaque(this_val, NULL);
    if (!wsd) return JS_ThrowTypeError(ctx, "WeakSet.prototype.delete called on non-WeakSet");
    for (int i = 0; i < wsd->count; i++) {
        if (wsd->values[i].u.ptr == argv[0].u.ptr) {
            JS_FreeValue(ctx, wsd->values[i]);
            wsd->count--;
            if (i < wsd->count) {
                wsd->values[i] = wsd->values[wsd->count];
            }
            return JS_TRUE;
        }
    }
    return JS_FALSE;
}

/* WeakSet methods */
static const JSCFunctionListEntry js_weakset_proto_methods[] = {
    JS_CFUNC_DEF("add",    1, js_weakset_add),
    JS_CFUNC_DEF("has",    1, js_weakset_has),
    JS_CFUNC_DEF("delete", 1, js_weakset_delete),
};

/* ========================================================================
 *  7b. WEAKREF
 * ======================================================================== */

/* Opaque data carried by every WeakRef object. Holds a raw (weak) pointer to
 * the referent plus the runtime so the finalizer can release the weak ref. */
typedef struct {
    LRRuntime       *rt;
    struct LRObject *target;   /* raw pointer; never dereferenced after GC */
} WeakRefData;

/* Called when the WeakRef object itself is collected: drop our weak reference
 * so the target can be finalized if it has no other weak refs. */
static void weak_ref_data_free(void *opaque)
{
    WeakRefData *w = (WeakRefData *)opaque;
    if (!w) return;
    lr_weak_ref_release(w->rt, w->target);
    free(w);
}

/* WeakRef constructor: new WeakRef(target) */
static LRValue js_weak_ref_constructor(LRContext *ctx, LRValue this_val,
                                       int argc, LRValue *argv)
{
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "WeakRef: target must be an object");

    WeakRefData *w = (WeakRefData *)malloc(sizeof(WeakRefData));
    if (!w) return JS_ThrowTypeError(ctx, "WeakRef: out of memory");
    w->rt     = ctx->rt;
    w->target = (struct LRObject *)argv[0].u.ptr;
    lr_weak_ref_retain(ctx->rt, w->target);
    lr_set_opaque_with_free(this_val, w, weak_ref_data_free);
    return JS_DupValue(ctx, this_val);
}

/* WeakRef.prototype.deref() */
static LRValue js_weak_ref_deref(LRContext *ctx, LRValue this_val,
                                 int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    WeakRefData *w = (WeakRefData *)JS_GetOpaque(this_val, NULL);
    if (!w) return JS_ThrowTypeError(ctx, "WeakRef.prototype.deref called on non-WeakRef");
    return lr_weak_ref_deref(ctx, w->target);
}

static const JSCFunctionListEntry js_weak_ref_proto_methods[] = {
    JS_CFUNC_DEF("deref", 0, js_weak_ref_deref),
};

/* ========================================================================
 *  7c. FINALIZATIONREGISTRY
 * ======================================================================== */

/* Opaque data carried by a FinalizationRegistry: the cleanup callback. */
typedef struct {
    LRContext *ctx;
    LRValue    callback;
} FinalizationRegistryData;

static void finalization_registry_data_free(void *opaque)
{
    FinalizationRegistryData *d = (FinalizationRegistryData *)opaque;
    if (!d) return;
    lr_free_value(d->ctx, d->callback);
    free(d);
}

/* FinalizationRegistry constructor: new FinalizationRegistry(cleanupCallback) */
static LRValue js_finalization_registry_constructor(LRContext *ctx, LRValue this_val,
                                                    int argc, LRValue *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "FinalizationRegistry: cleanupCallback must be a function");

    FinalizationRegistryData *d = (FinalizationRegistryData *)malloc(sizeof(*d));
    if (!d) return JS_ThrowTypeError(ctx, "FinalizationRegistry: out of memory");
    d->ctx      = ctx;
    d->callback = lr_dup_value(ctx, argv[0]);
    lr_set_opaque_with_free(this_val, d, finalization_registry_data_free);
    return JS_DupValue(ctx, this_val);
}

/* FinalizationRegistry.prototype.register(target, heldValue [, unregisterToken]) */
static LRValue js_finalization_registry_register(LRContext *ctx, LRValue this_val,
                                                 int argc, LRValue *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "FinalizationRegistry.prototype.register: target and heldValue required");
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "FinalizationRegistry.prototype.register: target must be an object");

    FinalizationRegistryData *d =
        (FinalizationRegistryData *)JS_GetOpaque(this_val, NULL);
    if (!d)
        return JS_ThrowTypeError(ctx,
            "FinalizationRegistry.prototype.register called on non-FinalizationRegistry");

    LRValue token = (argc >= 3) ? lr_dup_value(ctx, argv[2]) : LR_VALUE_UNDEFINED;

    /* lr_register_finalization takes ownership of every value passed, so we
     * give it its own duplicates of the callback, heldValue, registry and token. */
    lr_register_finalization(ctx->rt,
                             (struct LRObject *)argv[0].u.ptr,
                             lr_dup_value(ctx, d->callback),
                             lr_dup_value(ctx, argv[1]),
                             lr_dup_value(ctx, this_val),
                             token);
    return LR_VALUE_UNDEFINED;
}

/* FinalizationRegistry.prototype.unregister(unregisterToken) -> boolean */
static LRValue js_finalization_registry_unregister(LRContext *ctx, LRValue this_val,
                                                   int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "FinalizationRegistry.prototype.unregister: unregisterToken required");
    int removed = lr_unregister_finalization(ctx->rt, argv[0]);
    return removed ? JS_TRUE : JS_FALSE;
}

static const JSCFunctionListEntry js_finalization_registry_proto_methods[] = {
    JS_CFUNC_DEF("register",   2, js_finalization_registry_register),
    JS_CFUNC_DEF("unregister", 1, js_finalization_registry_unregister),
};

/* ========================================================================
 *  8. TYPED ARRAY AND DATA VIEW
 * ======================================================================== */

/* TypedArray element sizes */
#define TA_ELEM_UINT8     1
#define TA_ELEM_INT8      1
#define TA_ELEM_UINT16    2
#define TA_ELEM_INT16     2
#define TA_ELEM_UINT32    4
#define TA_ELEM_INT32     4
#define TA_ELEM_FLOAT32   4
#define TA_ELEM_FLOAT64   8
#define TA_ELEM_BIGUINT64 8
#define TA_ELEM_BIGINT64  8

/* Magic IDs for dispatch */
#define TA_MAGIC_UINT8     0
#define TA_MAGIC_INT8      1
#define TA_MAGIC_UINT16    2
#define TA_MAGIC_INT16     3
#define TA_MAGIC_UINT32    4
#define TA_MAGIC_INT32     5
#define TA_MAGIC_FLOAT32   6
#define TA_MAGIC_FLOAT64   7
#define TA_MAGIC_BIGUINT64 8
#define TA_MAGIC_BIGINT64  9

/* TypedArray internal data stored as opaque */
/* Defined in lr_engine.h as TypedArrayData */

/* DataView internal data stored as opaque */
/* Defined in lr_engine.h as DataViewData */

/* ── TypedArray type metadata ──────────────────────────────────────────── */

static const struct {
    int    magic;
    const char *name;
    size_t elem_size;
} ta_type_info[] = {
    { TA_MAGIC_UINT8,     "Uint8Array",     TA_ELEM_UINT8 },
    { TA_MAGIC_INT8,      "Int8Array",      TA_ELEM_INT8 },
    { TA_MAGIC_UINT16,    "Uint16Array",    TA_ELEM_UINT16 },
    { TA_MAGIC_INT16,     "Int16Array",     TA_ELEM_INT16 },
    { TA_MAGIC_UINT32,    "Uint32Array",    TA_ELEM_UINT32 },
    { TA_MAGIC_INT32,     "Int32Array",     TA_ELEM_INT32 },
    { TA_MAGIC_FLOAT32,   "Float32Array",   TA_ELEM_FLOAT32 },
    { TA_MAGIC_FLOAT64,   "Float64Array",   TA_ELEM_FLOAT64 },
    { TA_MAGIC_BIGUINT64, "BigUint64Array", TA_ELEM_BIGUINT64 },
    { TA_MAGIC_BIGINT64,  "BigInt64Array",  TA_ELEM_BIGINT64 },
};

/* ── TypedArray data free helper ───────────────────────────────────────── */

static void typed_array_data_free(LRContext *ctx, TypedArrayData *tad)
{
    if (!tad) return;
    JS_FreeValue(ctx, tad->buffer);
    free((void *)(tad->name));
    free(tad);
}

/* ── TypedArray data create helper ─────────────────────────────────────── */

static TypedArrayData *typed_array_data_new(LRContext *ctx, LRValue buffer,
    size_t byte_offset, size_t byte_length, size_t elem_size, int magic, const char *name)
{
    TypedArrayData *tad = (TypedArrayData *)malloc(sizeof(TypedArrayData));
    if (!tad) return NULL;
    tad->buffer = JS_DupValue(ctx, buffer);
    tad->byte_offset = byte_offset;
    tad->byte_length = byte_length;
    tad->element_size = elem_size;
    tad->magic = magic;
    tad->name = strdup(name);
    if (!tad->name) { free(tad); return NULL; }
    return tad;
}

/* ── Create a TypedArray object ────────────────────────────────────────── */

static LRValue typed_array_create(LRContext *ctx, TypedArrayData *tad, LRValue obj)
{
    /* If obj is not an object, create a new one */
    if (!lr_is_object(obj)) {
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) { typed_array_data_free(ctx, tad); return obj; }
        /* Set correct TypedArray prototype (e.g. Uint8Array.prototype) */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, tad->name);
        if (JS_IsObject(ctor)) {
            JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
            if (JS_IsObject(proto)) {
                JS_SetPrototype(ctx, obj, JS_DupValue(ctx, proto));
            }
            JS_FreeValue(ctx, proto);
        }
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, global);
    }
    LRObject *o = (LRObject *)obj.u.ptr;
    o->type = LR_OBJ_TYPED_ARRAY;
    /* Set opaque before setting properties so toString works */
    JS_SetOpaque(obj, tad);
    /* Set properties */
    JS_SetPropertyStr(ctx, obj, "buffer", JS_DupValue(ctx, tad->buffer));
    JS_SetPropertyStr(ctx, obj, "byteOffset", JS_NewInt32(ctx, (int32_t)tad->byte_offset));
    JS_SetPropertyStr(ctx, obj, "byteLength", JS_NewInt32(ctx, (int32_t)tad->byte_length));
    JS_SetPropertyStr(ctx, obj, "length", JS_NewInt32(ctx, (int32_t)(tad->byte_length / tad->element_size)));
    JS_SetPropertyStr(ctx, obj, "BYTES_PER_ELEMENT", JS_NewInt32(ctx, (int32_t)tad->element_size));
    /* Set constructor property */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, tad->name);
    if (JS_IsObject(ctor)) {
        JS_SetPropertyStr(ctx, obj, "constructor", JS_DupValue(ctx, ctor));
    }
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return obj;
}

/* ── Get element value from a typed array at index ─────────────────────── */

static LRValue typed_array_get_elem(LRContext *ctx, TypedArrayData *tad, size_t index)
{
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) return JS_ThrowTypeError(ctx, "TypedArray: buffer is detached");
    size_t offset = tad->byte_offset + index * tad->element_size;
    if (offset + tad->element_size > tad->byte_offset + tad->byte_length) {
        return JS_UNDEFINED;
    }
    switch (tad->magic) {
    case TA_MAGIC_UINT8:
        return JS_NewInt32(ctx, (int32_t)((uint8_t *)base)[offset]);
    case TA_MAGIC_INT8:
        return JS_NewInt32(ctx, (int32_t)((int8_t *)base)[offset]);
    case TA_MAGIC_UINT16: {
        uint16_t v;
        memcpy(&v, base + offset, 2);
        return JS_NewInt32(ctx, (int32_t)v);
    }
    case TA_MAGIC_INT16: {
        int16_t v;
        memcpy(&v, base + offset, 2);
        return JS_NewInt32(ctx, (int32_t)v);
    }
    case TA_MAGIC_UINT32: {
        uint32_t v;
        memcpy(&v, base + offset, 4);
        return JS_NewFloat64(ctx, (double)v);
    }
    case TA_MAGIC_INT32: {
        int32_t v;
        memcpy(&v, base + offset, 4);
        return JS_NewInt32(ctx, v);
    }
    case TA_MAGIC_FLOAT32: {
        float v;
        memcpy(&v, base + offset, 4);
        return JS_NewFloat64(ctx, (double)v);
    }
    case TA_MAGIC_FLOAT64: {
        double v;
        memcpy(&v, base + offset, 8);
        return JS_NewFloat64(ctx, v);
    }
    case TA_MAGIC_BIGUINT64: {
        uint64_t v;
        memcpy(&v, base + offset, 8);
        return JS_NewFloat64(ctx, (double)v);
    }
    case TA_MAGIC_BIGINT64: {
        int64_t v;
        memcpy(&v, base + offset, 8);
        return JS_NewFloat64(ctx, (double)v);
    }
    default:
        return JS_UNDEFINED;
    }
}

/* ── Set element value in a typed array at index ───────────────────────── */

static int typed_array_set_elem(LRContext *ctx, TypedArrayData *tad, size_t index, LRValue val)
{
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) return -1;
    size_t offset = tad->byte_offset + index * tad->element_size;
    if (offset + tad->element_size > tad->byte_offset + tad->byte_length) return -1;
    double d;
    JS_ToFloat64(ctx, &d, val);
    switch (tad->magic) {
    case TA_MAGIC_UINT8:  ((uint8_t *)base)[offset] = (uint8_t)(int32_t)d; break;
    case TA_MAGIC_INT8:   ((int8_t *)base)[offset] = (int8_t)(int32_t)d; break;
    case TA_MAGIC_UINT16: { uint16_t v = (uint16_t)(int32_t)d; memcpy(base + offset, &v, 2); break; }
    case TA_MAGIC_INT16:  { int16_t v = (int16_t)(int32_t)d; memcpy(base + offset, &v, 2); break; }
    case TA_MAGIC_UINT32: { uint32_t v = (uint32_t)d; memcpy(base + offset, &v, 4); break; }
    case TA_MAGIC_INT32:  { int32_t v = (int32_t)d; memcpy(base + offset, &v, 4); break; }
    case TA_MAGIC_FLOAT32: { float v = (float)d; memcpy(base + offset, &v, 4); break; }
    case TA_MAGIC_FLOAT64: { double v = d; memcpy(base + offset, &v, 8); break; }
    case TA_MAGIC_BIGUINT64: { uint64_t v = (uint64_t)d; memcpy(base + offset, &v, 8); break; }
    case TA_MAGIC_BIGINT64:  { int64_t v = (int64_t)d; memcpy(base + offset, &v, 8); break; }
    default: return -1;
    }
    return 0;
}

/* ── TypedArray constructor (magic dispatch) ───────────────────────────── */

static LRValue js_typed_array_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    int magic = 0;
    /* Get magic from the function */
    LRObject *func_obj = (LRObject *)ctx->current_func.u.ptr;
    if (func_obj && func_obj->type == LR_OBJ_CFUNCTION) {
        LRCFunction *cf = (LRCFunction *)func_obj->extra;
        magic = cf->magic;
    }

    const char *name = ta_type_info[magic].name;
    size_t elem_size = ta_type_info[magic].elem_size;

    if (argc == 0) {
        /* new TypedArray() - zero-length */
        LRValue buf = lr_new_array_buffer(ctx, NULL, 0, NULL, NULL, 0);
        if (JS_IsException(buf)) return JS_DupValue(ctx, buf);
        TypedArrayData *tad = typed_array_data_new(ctx, buf, 0, 0, elem_size, magic, name);
        JS_FreeValue(ctx, buf);
        if (!tad) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
        return typed_array_create(ctx, tad, this_val);
    }

    LRValue first = argv[0];

    /* Case 1: TypedArray(length) */
    if (JS_IsNumber(first)) {
        double d;
        JS_ToFloat64(ctx, &d, first);
        int32_t length = (int32_t)d;
        if (length < 0) return JS_ThrowRangeError(ctx, "%s: invalid length", name);
        size_t byte_len = (size_t)length * elem_size;
        uint8_t *buf_data = (uint8_t *)calloc(1, byte_len);
        if (!buf_data && byte_len > 0) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
        LRValue buf = lr_new_array_buffer(ctx, buf_data, byte_len, lr_array_buffer_free, NULL, 0);
        if (JS_IsException(buf)) { free(buf_data); return buf; }
        TypedArrayData *tad = typed_array_data_new(ctx, buf, 0, byte_len, elem_size, magic, name);
        JS_FreeValue(ctx, buf);
        if (!tad) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
        return typed_array_create(ctx, tad, this_val);
    }

    /* Case 2: TypedArray(typedArray) - copy from another TypedArray */
    if (JS_IsObject(first)) {
        LRObject *o = (LRObject *)first.u.ptr;
        if (o->type == LR_OBJ_TYPED_ARRAY) {
            TypedArrayData *src = (TypedArrayData *)JS_GetOpaque(first, NULL);
            if (!src) return JS_ThrowTypeError(ctx, "%s: invalid source", name);
            size_t src_len = src->byte_length / src->element_size;
            size_t byte_len = src_len * elem_size;
            uint8_t *buf_data = (uint8_t *)calloc(1, byte_len);
            if (!buf_data && byte_len > 0) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
            /* Copy and convert elements */
            for (size_t i = 0; i < src_len; i++) {
                LRValue elem = typed_array_get_elem(ctx, src, i);
                if (JS_IsException(elem)) { free(buf_data); return elem; }
                double d;
                JS_ToFloat64(ctx, &d, elem);
                JS_FreeValue(ctx, elem);
                size_t offset = i * elem_size;
                switch (magic) {
                case TA_MAGIC_UINT8:  buf_data[offset] = (uint8_t)(int32_t)d; break;
                case TA_MAGIC_INT8:   ((int8_t *)buf_data)[offset] = (int8_t)(int32_t)d; break;
                case TA_MAGIC_UINT16: { uint16_t v = (uint16_t)(int32_t)d; memcpy(buf_data + offset, &v, 2); break; }
                case TA_MAGIC_INT16:  { int16_t v = (int16_t)(int32_t)d; memcpy(buf_data + offset, &v, 2); break; }
                case TA_MAGIC_UINT32: { uint32_t v = (uint32_t)d; memcpy(buf_data + offset, &v, 4); break; }
                case TA_MAGIC_INT32:  { int32_t v = (int32_t)d; memcpy(buf_data + offset, &v, 4); break; }
                case TA_MAGIC_FLOAT32: { float v = (float)d; memcpy(buf_data + offset, &v, 4); break; }
                case TA_MAGIC_FLOAT64: { double v = d; memcpy(buf_data + offset, &v, 8); break; }
                case TA_MAGIC_BIGUINT64: { uint64_t v = (uint64_t)d; memcpy(buf_data + offset, &v, 8); break; }
                case TA_MAGIC_BIGINT64:  { int64_t v = (int64_t)d; memcpy(buf_data + offset, &v, 8); break; }
                }
            }
            LRValue buf = lr_new_array_buffer(ctx, buf_data, byte_len, lr_array_buffer_free, NULL, 0);
            if (JS_IsException(buf)) { free(buf_data); return buf; }
            TypedArrayData *tad = typed_array_data_new(ctx, buf, 0, byte_len, elem_size, magic, name);
            JS_FreeValue(ctx, buf);
            if (!tad) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
            return typed_array_create(ctx, tad, this_val);
        }

        /* Case 3: TypedArray(array) - from iterable/array-like */
        {
            /* Skip ArrayBuffer objects - they are handled in Case 4 below */
            if (o->type != LR_OBJ_ARRAY_BUFFER) {
                /* Try to get length */
                JSValue len_val = JS_GetPropertyStr(ctx, first, "length");
                int32_t length = 0;
                if (!JS_IsUndefined(len_val)) {
                    JS_ToInt32(ctx, &length, len_val);
                }
                JS_FreeValue(ctx, len_val);
                if (length < 0) length = 0;
                size_t byte_len = (size_t)length * elem_size;
                uint8_t *buf_data = (uint8_t *)calloc(1, byte_len);
                if (!buf_data && byte_len > 0) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
                for (int32_t i = 0; i < length; i++) {
                    JSValue elem = JS_GetPropertyUint32(ctx, first, i);
                    double d;
                    JS_ToFloat64(ctx, &d, elem);
                    JS_FreeValue(ctx, elem);
                    size_t offset = (size_t)i * elem_size;
                    switch (magic) {
                    case TA_MAGIC_UINT8:  buf_data[offset] = (uint8_t)(int32_t)d; break;
                    case TA_MAGIC_INT8:   ((int8_t *)buf_data)[offset] = (int8_t)(int32_t)d; break;
                    case TA_MAGIC_UINT16: { uint16_t v = (uint16_t)(int32_t)d; memcpy(buf_data + offset, &v, 2); break; }
                    case TA_MAGIC_INT16:  { int16_t v = (int16_t)(int32_t)d; memcpy(buf_data + offset, &v, 2); break; }
                    case TA_MAGIC_UINT32: { uint32_t v = (uint32_t)d; memcpy(buf_data + offset, &v, 4); break; }
                    case TA_MAGIC_INT32:  { int32_t v = (int32_t)d; memcpy(buf_data + offset, &v, 4); break; }
                    case TA_MAGIC_FLOAT32: { float v = (float)d; memcpy(buf_data + offset, &v, 4); break; }
                    case TA_MAGIC_FLOAT64: { double v = d; memcpy(buf_data + offset, &v, 8); break; }
                    case TA_MAGIC_BIGUINT64: { uint64_t v = (uint64_t)d; memcpy(buf_data + offset, &v, 8); break; }
                    case TA_MAGIC_BIGINT64:  { int64_t v = (int64_t)d; memcpy(buf_data + offset, &v, 8); break; }
                    }
                }
                LRValue buf = lr_new_array_buffer(ctx, buf_data, byte_len, lr_array_buffer_free, NULL, 0);
                if (JS_IsException(buf)) { free(buf_data); return buf; }
                TypedArrayData *tad = typed_array_data_new(ctx, buf, 0, byte_len, elem_size, magic, name);
                JS_FreeValue(ctx, buf);
                if (!tad) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
                return typed_array_create(ctx, tad, this_val);
            } /* end skip ArrayBuffer */
        }
    }

    /* Case 4: TypedArray(buffer, byteOffset, length) - fall through */
    /* This case is handled intuitively: if first arg is an object with byteLength, treat as buffer */
    if (JS_IsObject(first)) {
        /* Check if it's an ArrayBuffer */
        JSValue bl_val = JS_GetPropertyStr(ctx, first, "byteLength");
        int is_buffer = !JS_IsUndefined(bl_val);
        JS_FreeValue(ctx, bl_val);
        if (is_buffer) {
            size_t buf_byte_len = 0;
            uint8_t *buf_data = lr_get_array_buffer(ctx, &buf_byte_len, first);
            if (!buf_data) return JS_ThrowTypeError(ctx, "%s: first argument is not an ArrayBuffer", name);
            size_t offset = 0;
            size_t length = 0;
            size_t ta_byte_len = 0;
            if (argc >= 2) {
                int32_t off;
                JS_ToInt32(ctx, &off, argv[1]);
                if (off < 0 || (size_t)off % elem_size != 0)
                    return JS_ThrowRangeError(ctx, "%s: invalid byte offset", name);
                offset = (size_t)off;
            }
            if (argc >= 3) {
                int32_t len;
                JS_ToInt32(ctx, &len, argv[2]);
                if (len < 0) return JS_ThrowRangeError(ctx, "%s: invalid length", name);
                length = (size_t)len;
                ta_byte_len = length * elem_size;
            } else {
                ta_byte_len = buf_byte_len - offset;
                length = ta_byte_len / elem_size;
            }
            if (offset + ta_byte_len > buf_byte_len)
                return JS_ThrowRangeError(ctx, "%s: buffer too small", name);
            TypedArrayData *tad = typed_array_data_new(ctx, first, offset, ta_byte_len, elem_size, magic, name);
            if (!tad) return JS_ThrowTypeError(ctx, "%s: out of memory", name);
            return typed_array_create(ctx, tad, this_val);
        }
    }

    return JS_ThrowTypeError(ctx, "%s: invalid arguments", name);
}

/* ── TypedArray.prototype.at ───────────────────────────────────────────── */

static LRValue js_typed_array_at(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.at called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    int32_t idx = get_int_arg(ctx, argc, argv, 0, 0);
    if (idx < 0) idx = (int32_t)len + idx;
    if (idx < 0 || (size_t)idx >= len) return JS_UNDEFINED;
    return typed_array_get_elem(ctx, tad, (size_t)idx);
}

/* ── TypedArray.prototype.copyWithin ───────────────────────────────────── */

static LRValue js_typed_array_copy_within(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.copyWithin called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    int32_t target = get_int_arg(ctx, argc, argv, 0, 0);
    int32_t start = get_int_arg(ctx, argc, argv, 1, 0);
    int32_t end = get_int_arg(ctx, argc, argv, 2, (int32_t)len);
    if (target < 0) target = (int32_t)len + target;
    if (start < 0) start = (int32_t)len + start;
    if (end < 0) end = (int32_t)len + end;
    if (target < 0) target = 0;
    if (start < 0) start = 0;
    if ((size_t)end > len) end = (int32_t)len;
    if (target >= (int32_t)len || start >= (int32_t)len) return JS_DupValue(ctx, this_val);
    /* Copy element by element */
    size_t count = (size_t)(end - start);
    if ((size_t)target + count > len) count = len - (size_t)target;
    if (count == 0 || target == start) return JS_DupValue(ctx, this_val);
    size_t elem_size = tad->element_size;
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) return JS_ThrowTypeError(ctx, "TypedArray: buffer is detached");
    size_t src_off = tad->byte_offset + (size_t)start * elem_size;
    size_t dst_off = tad->byte_offset + (size_t)target * elem_size;
    memmove(base + dst_off, base + src_off, count * elem_size);
    return JS_DupValue(ctx, this_val);
}

/* ── TypedArray.prototype.entries ──────────────────────────────────────── */

static LRValue js_typed_array_entries(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.entries called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    /* Create a simple array of [index, value] pairs */
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < len; i++) {
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewInt32(ctx, (int32_t)i));
        JS_SetPropertyUint32(ctx, pair, 1, typed_array_get_elem(ctx, tad, i));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, pair);
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, (int32_t)len));
    /* Return an iterator object (simplified: return the array itself) */
    return arr;
}

/* ── TypedArray.prototype.every ────────────────────────────────────────── */

static LRValue js_typed_array_every(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.every called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "every: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, elem);
        if (JS_IsException(result)) return result;
        int32_t b;
        JS_ToInt32(ctx, &b, result);
        JS_FreeValue(ctx, result);
        if (!b) return JS_FALSE;
    }
    return JS_TRUE;
}

/* ── TypedArray.prototype.fill ─────────────────────────────────────────── */

static LRValue js_typed_array_fill(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.fill called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    if (argc < 1) return JS_DupValue(ctx, this_val);
    int32_t start = get_int_arg(ctx, argc, argv, 1, 0);
    int32_t end = get_int_arg(ctx, argc, argv, 2, (int32_t)len);
    if (start < 0) start = (int32_t)len + start;
    if (end < 0) end = (int32_t)len + end;
    if (start < 0) start = 0;
    if ((size_t)end > len) end = (int32_t)len;
    for (int32_t i = start; i < end; i++) {
        typed_array_set_elem(ctx, tad, (size_t)i, argv[0]);
    }
    return JS_DupValue(ctx, this_val);
}

/* ── TypedArray.prototype.filter ───────────────────────────────────────── */

static LRValue js_typed_array_filter(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.filter called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "filter: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    LRValue result = JS_NewArray(ctx);
    uint32_t ri = 0;
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue keep = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        if (JS_IsException(keep)) { JS_FreeValue(ctx, elem); JS_FreeValue(ctx, result); return keep; }
        int32_t b;
        JS_ToInt32(ctx, &b, keep);
        JS_FreeValue(ctx, keep);
        if (b) {
            JS_SetPropertyUint32(ctx, result, ri++, JS_DupValue(ctx, elem));
        }
        JS_FreeValue(ctx, elem);
    }
    JS_SetPropertyStr(ctx, result, "length", JS_NewInt32(ctx, (int32_t)ri));
    return result;
}

/* ── TypedArray.prototype.find ─────────────────────────────────────────── */

static LRValue js_typed_array_find(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.find called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "find: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        if (JS_IsException(result)) { JS_FreeValue(ctx, elem); return result; }
        int32_t b;
        JS_ToInt32(ctx, &b, result);
        JS_FreeValue(ctx, result);
        if (b) return elem;
        JS_FreeValue(ctx, elem);
    }
    return JS_UNDEFINED;
}

/* ── TypedArray.prototype.findIndex ────────────────────────────────────── */

static LRValue js_typed_array_find_index(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.findIndex called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "findIndex: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, elem);
        if (JS_IsException(result)) return result;
        int32_t b;
        JS_ToInt32(ctx, &b, result);
        JS_FreeValue(ctx, result);
        if (b) return JS_NewInt32(ctx, (int32_t)i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── TypedArray.prototype.forEach ──────────────────────────────────────── */

static LRValue js_typed_array_for_each(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.forEach called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "forEach: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, elem);
        if (JS_IsException(result)) return result;
        JS_FreeValue(ctx, result);
    }
    return JS_UNDEFINED;
}

/* ── TypedArray.prototype.includes ─────────────────────────────────────── */

static LRValue js_typed_array_includes(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.includes called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    if (argc < 1) return JS_FALSE;
    int32_t from_idx = get_int_arg(ctx, argc, argv, 1, 0);
    if (from_idx < 0) from_idx = (int32_t)len + from_idx;
    if (from_idx < 0) from_idx = 0;
    for (size_t i = (size_t)from_idx; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        int eq = 0;
        if (JS_IsNumber(elem) && JS_IsNumber(argv[0])) {
            double d1, d2;
            JS_ToFloat64(ctx, &d1, elem);
            JS_ToFloat64(ctx, &d2, argv[0]);
            eq = (d1 == d2) || (isnan(d1) && isnan(d2));
        } else {
            /* Compare by value */
            double d1, d2;
            JS_ToFloat64(ctx, &d1, elem);
            JS_ToFloat64(ctx, &d2, argv[0]);
            eq = (d1 == d2);
        }
        JS_FreeValue(ctx, elem);
        if (eq) return JS_TRUE;
    }
    return JS_FALSE;
}

/* ── TypedArray.prototype.indexOf ──────────────────────────────────────── */

static LRValue js_typed_array_index_of(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.indexOf called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    int32_t from_idx = get_int_arg(ctx, argc, argv, 1, 0);
    if (from_idx < 0) from_idx = (int32_t)len + from_idx;
    if (from_idx < 0) from_idx = 0;
    for (size_t i = (size_t)from_idx; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        double d1, d2;
        JS_ToFloat64(ctx, &d1, elem);
        JS_ToFloat64(ctx, &d2, argv[0]);
        JS_FreeValue(ctx, elem);
        if (d1 == d2) return JS_NewInt32(ctx, (int32_t)i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── TypedArray.prototype.join ─────────────────────────────────────────── */

static LRValue js_typed_array_join(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.join called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    const char *sep = ",";
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        sep = JS_ToCString(ctx, argv[0]);
        if (!sep) sep = ",";
    }
    char buf[4096];
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        if (i > 0) {
            size_t sl = strlen(sep);
            if (pos + sl >= sizeof(buf) - 1) break;
            memcpy(buf + pos, sep, sl);
            pos += sl;
        }
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        double d;
        JS_ToFloat64(ctx, &d, elem);
        JS_FreeValue(ctx, elem);
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%g", d);
        if (n > 0) pos += (size_t)n;
        if (pos >= sizeof(buf) - 1) break;
    }
    buf[pos] = '\0';
    if (argc >= 1 && !JS_IsUndefined(argv[0])) JS_FreeCString(ctx, sep);
    return JS_NewString(ctx, buf);
}

/* ── TypedArray.prototype.keys ─────────────────────────────────────────── */

static LRValue js_typed_array_keys(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.keys called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    /* Simplified: return array of indices */
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, (int32_t)i));
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, (int32_t)len));
    return arr;
}

/* ── TypedArray.prototype.lastIndexOf ──────────────────────────────────── */

static LRValue js_typed_array_last_index_of(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.lastIndexOf called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    int32_t from_idx = get_int_arg(ctx, argc, argv, 1, (int32_t)(len - 1));
    if (from_idx < 0) from_idx = (int32_t)len + from_idx;
    if (from_idx >= (int32_t)len) from_idx = (int32_t)len - 1;
    for (int32_t i = from_idx; i >= 0; i--) {
        LRValue elem = typed_array_get_elem(ctx, tad, (size_t)i);
        double d1, d2;
        JS_ToFloat64(ctx, &d1, elem);
        JS_ToFloat64(ctx, &d2, argv[0]);
        JS_FreeValue(ctx, elem);
        if (d1 == d2) return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

/* ── TypedArray.prototype.map ──────────────────────────────────────────── */

static LRValue js_typed_array_map(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.map called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "map: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    LRValue result = JS_NewArray(ctx);
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue mapped = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, elem);
        if (JS_IsException(mapped)) { JS_FreeValue(ctx, result); return mapped; }
        JS_SetPropertyUint32(ctx, result, (uint32_t)i, mapped);
    }
    JS_SetPropertyStr(ctx, result, "length", JS_NewInt32(ctx, (int32_t)len));
    return result;
}

/* ── TypedArray.prototype.reduce ───────────────────────────────────────── */

static LRValue js_typed_array_reduce(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.reduce called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "reduce: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue acc;
    size_t start = 0;
    if (argc >= 2) {
        acc = JS_DupValue(ctx, argv[1]);
    } else {
        if (len == 0) return JS_ThrowTypeError(ctx, "reduce: empty array with no initial value");
        acc = typed_array_get_elem(ctx, tad, 0);
        start = 1;
    }
    for (size_t i = start; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[4] = { JS_DupValue(ctx, acc), JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 4, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]); JS_FreeValue(ctx, args[3]);
        JS_FreeValue(ctx, acc); JS_FreeValue(ctx, elem);
        if (JS_IsException(result)) return result;
        acc = result;
    }
    return acc;
}

/* ── TypedArray.prototype.reduceRight ──────────────────────────────────── */

static LRValue js_typed_array_reduce_right(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.reduceRight called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "reduceRight: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue acc;
    int32_t start = (int32_t)len - 1;
    if (argc >= 2) {
        acc = JS_DupValue(ctx, argv[1]);
    } else {
        if (len == 0) return JS_ThrowTypeError(ctx, "reduceRight: empty array with no initial value");
        acc = typed_array_get_elem(ctx, tad, (size_t)start);
        start--;
    }
    for (int32_t i = start; i >= 0; i--) {
        LRValue elem = typed_array_get_elem(ctx, tad, (size_t)i);
        LRValue args[4] = { JS_DupValue(ctx, acc), JS_DupValue(ctx, elem), JS_NewInt32(ctx, i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 4, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]); JS_FreeValue(ctx, args[3]);
        JS_FreeValue(ctx, acc); JS_FreeValue(ctx, elem);
        if (JS_IsException(result)) return result;
        acc = result;
    }
    return acc;
}

/* ── TypedArray.prototype.reverse ──────────────────────────────────────── */

static LRValue js_typed_array_reverse(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.reverse called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    size_t elem_size = tad->element_size;
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) return JS_ThrowTypeError(ctx, "TypedArray: buffer is detached");
    uint8_t *data = base + tad->byte_offset;
    for (size_t i = 0, j = len - 1; i < j; i++, j--) {
        uint8_t tmp[8]; /* max element size is 8 */
        memcpy(tmp, data + i * elem_size, elem_size);
        memcpy(data + i * elem_size, data + j * elem_size, elem_size);
        memcpy(data + j * elem_size, tmp, elem_size);
    }
    return JS_DupValue(ctx, this_val);
}

/* ── TypedArray.prototype.set ──────────────────────────────────────────── */

static LRValue js_typed_array_set(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.set called on non-TypedArray");
    if (argc < 1) return JS_ThrowTypeError(ctx, "TypedArray.prototype.set: missing argument");
    size_t len = tad->byte_length / tad->element_size;
    int32_t offset = get_int_arg(ctx, argc, argv, 1, 0);
    if (offset < 0) return JS_ThrowRangeError(ctx, "TypedArray.prototype.set: invalid offset");

    LRValue src = argv[0];
    size_t src_len = 0;

    if (JS_IsObject(src)) {
        LRObject *o = (LRObject *)src.u.ptr;
        if (o->type == LR_OBJ_TYPED_ARRAY) {
            TypedArrayData *src_tad = (TypedArrayData *)JS_GetOpaque(src, NULL);
            if (src_tad) {
                src_len = src_tad->byte_length / src_tad->element_size;
                size_t avail = len - (size_t)offset;
                size_t copy_len = src_len < avail ? src_len : avail;
                uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
                uint8_t *src_base = lr_get_array_buffer(ctx, NULL, src_tad->buffer);
                if (!base || !src_base) return JS_ThrowTypeError(ctx, "TypedArray: buffer is detached");
                size_t dst_byte_off = tad->byte_offset + (size_t)offset * tad->element_size;
                size_t src_byte_off = src_tad->byte_offset;
                size_t copy_bytes = copy_len * tad->element_size;
                /* Convert element by element if sizes differ */
                if (tad->element_size == src_tad->element_size) {
                    memcpy(base + dst_byte_off, src_base + src_byte_off, copy_bytes);
                } else {
                    for (size_t i = 0; i < copy_len; i++) {
                        LRValue elem = typed_array_get_elem(ctx, src_tad, i);
                        typed_array_set_elem(ctx, tad, (size_t)offset + i, elem);
                        JS_FreeValue(ctx, elem);
                    }
                }
                return JS_UNDEFINED;
            }
        }
        /* Array-like source */
        JSValue len_val = JS_GetPropertyStr(ctx, src, "length");
        if (!JS_IsUndefined(len_val)) {
            JS_ToInt32(ctx, (int32_t *)&src_len, len_val);
        }
        JS_FreeValue(ctx, len_val);
        size_t avail = len - (size_t)offset;
        size_t copy_len = src_len < avail ? src_len : avail;
        for (size_t i = 0; i < copy_len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, src, (uint32_t)i);
            typed_array_set_elem(ctx, tad, (size_t)offset + i, elem);
            JS_FreeValue(ctx, elem);
        }
        return JS_UNDEFINED;
    }

    return JS_ThrowTypeError(ctx, "TypedArray.prototype.set: invalid argument");
}

/* ── TypedArray.prototype.slice ────────────────────────────────────────── */

static LRValue js_typed_array_slice(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.slice called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    int32_t start = get_int_arg(ctx, argc, argv, 0, 0);
    int32_t end = get_int_arg(ctx, argc, argv, 1, (int32_t)len);
    if (start < 0) start = (int32_t)len + start;
    if (end < 0) end = (int32_t)len + end;
    if (start < 0) start = 0;
    if ((size_t)end > len) end = (int32_t)len;
    if (start >= end) {
        /* Return empty typed array of same type */
        LRValue buf = lr_new_array_buffer(ctx, NULL, 0, NULL, NULL, 0);
        if (JS_IsException(buf)) return buf;
        TypedArrayData *new_tad = typed_array_data_new(ctx, buf, 0, 0, tad->element_size, tad->magic, tad->name);
        JS_FreeValue(ctx, buf);
        if (!new_tad) return JS_ThrowTypeError(ctx, "TypedArray: out of memory");
        return typed_array_create(ctx, new_tad, JS_UNDEFINED);
    }
    size_t count = (size_t)(end - start);
    size_t byte_len = count * tad->element_size;
    uint8_t *buf_data = (uint8_t *)malloc(byte_len);
    if (!buf_data) return JS_ThrowTypeError(ctx, "TypedArray: out of memory");
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) { free(buf_data); return JS_ThrowTypeError(ctx, "TypedArray: buffer is detached"); }
    memcpy(buf_data, base + tad->byte_offset + (size_t)start * tad->element_size, byte_len);
    LRValue buf = lr_new_array_buffer(ctx, buf_data, byte_len, lr_array_buffer_free, NULL, 0);
    if (JS_IsException(buf)) { free(buf_data); return buf; }
    TypedArrayData *new_tad = typed_array_data_new(ctx, buf, 0, byte_len, tad->element_size, tad->magic, tad->name);
    JS_FreeValue(ctx, buf);
    if (!new_tad) return JS_ThrowTypeError(ctx, "TypedArray: out of memory");
    return typed_array_create(ctx, new_tad, JS_UNDEFINED);
}

/* ── TypedArray.prototype.some ─────────────────────────────────────────── */

static LRValue js_typed_array_some(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.some called on non-TypedArray");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "some: callback is not a function");
    size_t len = tad->byte_length / tad->element_size;
    LRValue this_arg = argc >= 2 ? argv[1] : JS_UNDEFINED;
    for (size_t i = 0; i < len; i++) {
        LRValue elem = typed_array_get_elem(ctx, tad, i);
        LRValue args[3] = { JS_DupValue(ctx, elem), JS_NewInt32(ctx, (int32_t)i), JS_DupValue(ctx, this_val) };
        LRValue result = JS_Call(ctx, argv[0], this_arg, 3, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, elem);
        if (JS_IsException(result)) return result;
        int32_t b;
        JS_ToInt32(ctx, &b, result);
        JS_FreeValue(ctx, result);
        if (b) return JS_TRUE;
    }
    return JS_FALSE;
}

/* ── TypedArray.prototype.sort ─────────────────────────────────────────── */

static int typed_array_compare_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static LRValue js_typed_array_sort(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.sort called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    size_t elem_size = tad->element_size;
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) return JS_ThrowTypeError(ctx, "TypedArray: buffer is detached");
    uint8_t *data = base + tad->byte_offset;
    /* Sort using qsort on a temporary double array for numeric sort */
    double *tmp = (double *)malloc(len * sizeof(double));
    if (!tmp && len > 0) return JS_ThrowTypeError(ctx, "TypedArray: out of memory");
    for (size_t i = 0; i < len; i++) {
        switch (tad->magic) {
        case TA_MAGIC_UINT8:  tmp[i] = (double)((uint8_t *)data)[i]; break;
        case TA_MAGIC_INT8:   tmp[i] = (double)((int8_t *)data)[i]; break;
        case TA_MAGIC_UINT16: { uint16_t v; memcpy(&v, data + i * 2, 2); tmp[i] = (double)v; break; }
        case TA_MAGIC_INT16:  { int16_t v; memcpy(&v, data + i * 2, 2); tmp[i] = (double)v; break; }
        case TA_MAGIC_UINT32: { uint32_t v; memcpy(&v, data + i * 4, 4); tmp[i] = (double)v; break; }
        case TA_MAGIC_INT32:  { int32_t v; memcpy(&v, data + i * 4, 4); tmp[i] = (double)v; break; }
        case TA_MAGIC_FLOAT32: { float v; memcpy(&v, data + i * 4, 4); tmp[i] = (double)v; break; }
        case TA_MAGIC_FLOAT64: { double v; memcpy(&v, data + i * 8, 8); tmp[i] = v; break; }
        case TA_MAGIC_BIGUINT64: { uint64_t v; memcpy(&v, data + i * 8, 8); tmp[i] = (double)v; break; }
        case TA_MAGIC_BIGINT64:  { int64_t v; memcpy(&v, data + i * 8, 8); tmp[i] = (double)v; break; }
        }
    }
    qsort(tmp, len, sizeof(double), typed_array_compare_double);
    for (size_t i = 0; i < len; i++) {
        double d = tmp[i];
        switch (tad->magic) {
        case TA_MAGIC_UINT8:  ((uint8_t *)data)[i] = (uint8_t)(int32_t)d; break;
        case TA_MAGIC_INT8:   ((int8_t *)data)[i] = (int8_t)(int32_t)d; break;
        case TA_MAGIC_UINT16: { uint16_t v = (uint16_t)(int32_t)d; memcpy(data + i * 2, &v, 2); break; }
        case TA_MAGIC_INT16:  { int16_t v = (int16_t)(int32_t)d; memcpy(data + i * 2, &v, 2); break; }
        case TA_MAGIC_UINT32: { uint32_t v = (uint32_t)d; memcpy(data + i * 4, &v, 4); break; }
        case TA_MAGIC_INT32:  { int32_t v = (int32_t)d; memcpy(data + i * 4, &v, 4); break; }
        case TA_MAGIC_FLOAT32: { float v = (float)d; memcpy(data + i * 4, &v, 4); break; }
        case TA_MAGIC_FLOAT64: { double v = d; memcpy(data + i * 8, &v, 8); break; }
        case TA_MAGIC_BIGUINT64: { uint64_t v = (uint64_t)d; memcpy(data + i * 8, &v, 8); break; }
        case TA_MAGIC_BIGINT64:  { int64_t v = (int64_t)d; memcpy(data + i * 8, &v, 8); break; }
        }
    }
    free(tmp);
    return JS_DupValue(ctx, this_val);
}

/* ── TypedArray.prototype.subarray ─────────────────────────────────────── */

static LRValue js_typed_array_subarray(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.subarray called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    int32_t begin = get_int_arg(ctx, argc, argv, 0, 0);
    int32_t end = get_int_arg(ctx, argc, argv, 1, (int32_t)len);
    if (begin < 0) begin = (int32_t)len + begin;
    if (end < 0) end = (int32_t)len + end;
    if (begin < 0) begin = 0;
    if ((size_t)end > len) end = (int32_t)len;
    if (begin > end) end = begin;
    size_t new_offset = tad->byte_offset + (size_t)begin * tad->element_size;
    size_t new_byte_len = (size_t)(end - begin) * tad->element_size;
    TypedArrayData *new_tad = typed_array_data_new(ctx, tad->buffer, new_offset, new_byte_len,
        tad->element_size, tad->magic, tad->name);
    if (!new_tad) return JS_ThrowTypeError(ctx, "TypedArray: out of memory");
    return typed_array_create(ctx, new_tad, JS_UNDEFINED);
}

/* ── TypedArray.prototype.toString ─────────────────────────────────────── */

static LRValue js_typed_array_to_string(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    /* Use join(',') */
    LRValue sep = JS_NewString(ctx, ",");
    LRValue args[1] = { sep };
    LRValue result = js_typed_array_join(ctx, this_val, 1, args);
    JS_FreeValue(ctx, sep);
    return result;
}

/* ── TypedArray.prototype.toLocaleString ───────────────────────────────── */

static LRValue js_typed_array_to_locale_string(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    return js_typed_array_to_string(ctx, this_val, 0, NULL);
}

/* ── TypedArray.prototype.values ───────────────────────────────────────── */

static LRValue js_typed_array_values(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)argc; (void)argv;
    TypedArrayData *tad = (TypedArrayData *)JS_GetOpaque(this_val, NULL);
    if (!tad) return JS_ThrowTypeError(ctx, "TypedArray.prototype.values called on non-TypedArray");
    size_t len = tad->byte_length / tad->element_size;
    /* Simplified: return an array of values */
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, typed_array_get_elem(ctx, tad, i));
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, (int32_t)len));
    return arr;
}

/* TypedArray prototype methods list */
static const JSCFunctionListEntry js_typed_array_proto_methods[] = {
    JS_CFUNC_DEF("at",              1, js_typed_array_at),
    JS_CFUNC_DEF("copyWithin",      2, js_typed_array_copy_within),
    JS_CFUNC_DEF("entries",         0, js_typed_array_entries),
    JS_CFUNC_DEF("every",           1, js_typed_array_every),
    JS_CFUNC_DEF("fill",            1, js_typed_array_fill),
    JS_CFUNC_DEF("filter",          1, js_typed_array_filter),
    JS_CFUNC_DEF("find",            1, js_typed_array_find),
    JS_CFUNC_DEF("findIndex",       1, js_typed_array_find_index),
    JS_CFUNC_DEF("forEach",         1, js_typed_array_for_each),
    JS_CFUNC_DEF("includes",        1, js_typed_array_includes),
    JS_CFUNC_DEF("indexOf",         1, js_typed_array_index_of),
    JS_CFUNC_DEF("join",            1, js_typed_array_join),
    JS_CFUNC_DEF("keys",            0, js_typed_array_keys),
    JS_CFUNC_DEF("lastIndexOf",     1, js_typed_array_last_index_of),
    JS_CFUNC_DEF("map",             1, js_typed_array_map),
    JS_CFUNC_DEF("reduce",          1, js_typed_array_reduce),
    JS_CFUNC_DEF("reduceRight",     1, js_typed_array_reduce_right),
    JS_CFUNC_DEF("reverse",         0, js_typed_array_reverse),
    JS_CFUNC_DEF("set",             1, js_typed_array_set),
    JS_CFUNC_DEF("slice",           2, js_typed_array_slice),
    JS_CFUNC_DEF("some",            1, js_typed_array_some),
    JS_CFUNC_DEF("sort",            1, js_typed_array_sort),
    JS_CFUNC_DEF("subarray",        2, js_typed_array_subarray),
    JS_CFUNC_DEF("toString",        0, js_typed_array_to_string),
    JS_CFUNC_DEF("toLocaleString",  0, js_typed_array_to_locale_string),
    JS_CFUNC_DEF("values",          0, js_typed_array_values),
};

/* ── DataView Constructor ───────────────────────────────────────────────── */

static LRValue js_dataview_constructor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "DataView: missing buffer argument");
    LRValue buffer = argv[0];
    /* Check if buffer is an ArrayBuffer */
    size_t buf_byte_len = 0;
    uint8_t *buf_data = lr_get_array_buffer(ctx, &buf_byte_len, buffer);
    if (!buf_data) return JS_ThrowTypeError(ctx, "DataView: first argument must be an ArrayBuffer");

    size_t byte_offset = 0;
    size_t byte_length = 0;

    if (argc >= 2) {
        int32_t off;
        JS_ToInt32(ctx, &off, argv[1]);
        if (off < 0) return JS_ThrowRangeError(ctx, "DataView: invalid byteOffset");
        byte_offset = (size_t)off;
    }

    if (argc >= 3) {
        int32_t len;
        JS_ToInt32(ctx, &len, argv[2]);
        if (len < 0) return JS_ThrowRangeError(ctx, "DataView: invalid byteLength");
        byte_length = (size_t)len;
    } else {
        byte_length = buf_byte_len - byte_offset;
    }

    if (byte_offset + byte_length > buf_byte_len)
        return JS_ThrowRangeError(ctx, "DataView: buffer too small");

    DataViewData *dvd = (DataViewData *)malloc(sizeof(DataViewData));
    if (!dvd) return JS_ThrowTypeError(ctx, "DataView: out of memory");
    dvd->buffer = JS_DupValue(ctx, buffer);
    dvd->byte_offset = byte_offset;
    dvd->byte_length = byte_length;

    LRObject *o = (LRObject *)this_val.u.ptr;
    o->type = LR_OBJ_DATA_VIEW;
    JS_SetOpaque(this_val, dvd);

    /* Set properties */
    JS_SetPropertyStr(ctx, this_val, "buffer", JS_DupValue(ctx, buffer));
    JS_SetPropertyStr(ctx, this_val, "byteOffset", JS_NewInt32(ctx, (int32_t)byte_offset));
    JS_SetPropertyStr(ctx, this_val, "byteLength", JS_NewInt32(ctx, (int32_t)byte_length));

    return JS_DupValue(ctx, this_val);
}

/* ── DataView getter helpers ───────────────────────────────────────────── */

static uint8_t *dataview_get_ptr(LRContext *ctx, DataViewData *dvd, size_t offset, size_t size)
{
    if (offset + size > dvd->byte_length) return NULL;
    uint8_t *base = lr_get_array_buffer(ctx, NULL, dvd->buffer);
    if (!base) return NULL;
    return base + dvd->byte_offset + offset;
}

#define DATAVIEW_GET_FUNC(name, type, result_type, result_expr) \
    static LRValue js_dataview_get_##name(LRContext *ctx, LRValue this_val, int argc, LRValue *argv) \
    { \
        (void)argc; \
        DataViewData *dvd = (DataViewData *)JS_GetOpaque(this_val, NULL); \
        if (!dvd) return JS_ThrowTypeError(ctx, "DataView.prototype.get" #name " called on non-DataView"); \
        int32_t offset = get_int_arg(ctx, argc, argv, 0, 0); \
        if (offset < 0) return JS_ThrowRangeError(ctx, "DataView: invalid offset"); \
        uint8_t *ptr = dataview_get_ptr(ctx, dvd, (size_t)offset, sizeof(type)); \
        if (!ptr) return JS_ThrowRangeError(ctx, "DataView: offset out of bounds"); \
        type v; \
        memcpy(&v, ptr, sizeof(type)); \
        return result_expr; \
    }

#define DATAVIEW_SET_FUNC(name, type, val_expr) \
    static LRValue js_dataview_set_##name(LRContext *ctx, LRValue this_val, int argc, LRValue *argv) \
    { \
        DataViewData *dvd = (DataViewData *)JS_GetOpaque(this_val, NULL); \
        if (!dvd) return JS_ThrowTypeError(ctx, "DataView.prototype.set" #name " called on non-DataView"); \
        if (argc < 2) return JS_ThrowTypeError(ctx, "DataView.prototype.set" #name ": missing argument"); \
        int32_t offset = get_int_arg(ctx, argc, argv, 0, 0); \
        if (offset < 0) return JS_ThrowRangeError(ctx, "DataView: invalid offset"); \
        uint8_t *ptr = dataview_get_ptr(ctx, dvd, (size_t)offset, sizeof(type)); \
        if (!ptr) return JS_ThrowRangeError(ctx, "DataView: offset out of bounds"); \
        type v = (type)(val_expr); \
        memcpy(ptr, &v, sizeof(type)); \
        return JS_UNDEFINED; \
    }

DATAVIEW_GET_FUNC(int8, int8_t, JS_NewInt32(ctx, (int32_t)v), JS_NewInt32(ctx, (int32_t)v))
DATAVIEW_GET_FUNC(uint8, uint8_t, JS_NewInt32(ctx, (int32_t)v), JS_NewInt32(ctx, (int32_t)v))
DATAVIEW_GET_FUNC(int16, int16_t, JS_NewInt32(ctx, (int32_t)v), JS_NewInt32(ctx, (int32_t)v))
DATAVIEW_GET_FUNC(uint16, uint16_t, JS_NewInt32(ctx, (int32_t)v), JS_NewInt32(ctx, (int32_t)v))
DATAVIEW_GET_FUNC(int32, int32_t, JS_NewInt32(ctx, (int32_t)v), JS_NewInt32(ctx, v))
DATAVIEW_GET_FUNC(uint32, uint32_t, JS_NewFloat64(ctx, (double)v), JS_NewFloat64(ctx, (double)v))
DATAVIEW_GET_FUNC(float32, float, JS_NewFloat64(ctx, (double)v), JS_NewFloat64(ctx, (double)v))
DATAVIEW_GET_FUNC(float64, double, JS_NewFloat64(ctx, v), JS_NewFloat64(ctx, v))

DATAVIEW_SET_FUNC(int8, int8_t, (int32_t)get_int_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(uint8, uint8_t, (int32_t)get_int_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(int16, int16_t, (int32_t)get_int_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(uint16, uint16_t, (int32_t)get_int_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(int32, int32_t, get_int_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(uint32, uint32_t, (uint32_t)get_double_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(float32, float, (float)get_double_arg(ctx, argc, argv, 1, 0))
DATAVIEW_SET_FUNC(float64, double, get_double_arg(ctx, argc, argv, 1, 0))

/* DataView prototype methods */
static const JSCFunctionListEntry js_dataview_proto_methods[] = {
    JS_CFUNC_DEF("getInt8",     1, js_dataview_get_int8),
    JS_CFUNC_DEF("getUint8",    1, js_dataview_get_uint8),
    JS_CFUNC_DEF("getInt16",    1, js_dataview_get_int16),
    JS_CFUNC_DEF("getUint16",   1, js_dataview_get_uint16),
    JS_CFUNC_DEF("getInt32",    1, js_dataview_get_int32),
    JS_CFUNC_DEF("getUint32",   1, js_dataview_get_uint32),
    JS_CFUNC_DEF("getFloat32",  1, js_dataview_get_float32),
    JS_CFUNC_DEF("getFloat64",  1, js_dataview_get_float64),
    JS_CFUNC_DEF("setInt8",     2, js_dataview_set_int8),
    JS_CFUNC_DEF("setUint8",    2, js_dataview_set_uint8),
    JS_CFUNC_DEF("setInt16",    2, js_dataview_set_int16),
    JS_CFUNC_DEF("setUint16",   2, js_dataview_set_uint16),
    JS_CFUNC_DEF("setInt32",    2, js_dataview_set_int32),
    JS_CFUNC_DEF("setUint32",   2, js_dataview_set_uint32),
    JS_CFUNC_DEF("setFloat32",  2, js_dataview_set_float32),
    JS_CFUNC_DEF("setFloat64",  2, js_dataview_set_float64),
};

/* ── ArrayBuffer constructor ──────────────────────────────────────────── */

static LRValue js_array_buffer_constructor(LRContext *ctx, LRValue this_val,
                                            int argc, LRValue *argv)
{
    (void)this_val;
    /* Get byteLength argument */
    int32_t byte_len = 0;
    if (argc >= 1) {
        JS_ToInt32(ctx, &byte_len, argv[0]);
        if (byte_len < 0) {
            return JS_ThrowRangeError(ctx, "ArrayBuffer: invalid array buffer length");
        }
    }
    /* Allocate buffer */
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)byte_len);
    if (!buf && byte_len > 0) {
        return JS_ThrowTypeError(ctx, "ArrayBuffer: allocation failed");
    }
    /* Create ArrayBuffer object */
    JSValue result = lr_new_array_buffer(ctx, buf, (size_t)byte_len, lr_array_buffer_free, NULL, 0);
    if (JS_IsException(result)) {
        free(buf);
    }
    return result;
}

/* ── SharedArrayBuffer constructor ───────────────────────────────────── */

static LRValue js_shared_array_buffer_constructor(LRContext *ctx, LRValue this_val,
                                                  int argc, LRValue *argv)
{
    (void)this_val;
    int32_t byte_len = 0;
    if (argc >= 1) {
        JS_ToInt32(ctx, &byte_len, argv[0]);
        if (byte_len < 0) {
            return JS_ThrowRangeError(ctx, "SharedArrayBuffer: invalid array buffer length");
        }
    }
    uint8_t *buf = (uint8_t *)calloc(1, (size_t)byte_len);
    if (!buf && byte_len > 0) {
        return JS_ThrowTypeError(ctx, "SharedArrayBuffer: allocation failed");
    }
    JSValue result = lr_new_array_buffer(ctx, buf, (size_t)byte_len, lr_array_buffer_free, NULL, 1);
    if (JS_IsException(result)) {
        free(buf);
    }
    return result;
}

static LRValue js_shared_array_buffer_slice(LRContext *ctx, LRValue this_val,
                                            int argc, LRValue *argv)
{
    LRValue len_val = JS_GetPropertyStr(ctx, this_val, "byteLength");
    int32_t src_len = 0;
    JS_ToInt32(ctx, &src_len, len_val);
    JS_FreeValue(ctx, len_val);

    int32_t start = 0, end = src_len;
    if (argc >= 1) JS_ToInt32(ctx, &start, argv[0]);
    if (argc >= 2 && !JS_IsUndefined(argv[1])) JS_ToInt32(ctx, &end, argv[1]);

    if (start < 0) start = src_len + start;
    if (start < 0) start = 0;
    if (end < 0) end = src_len + end;
    if (end > src_len) end = src_len;
    if (start > end) start = end;

    int32_t new_len = end - start;
    uint8_t *src = lr_get_array_buffer(ctx, NULL, this_val);
    uint8_t *dst = (uint8_t *)calloc(1, (size_t)(new_len > 0 ? new_len : 0));
    if (src && new_len > 0) memcpy(dst, src + start, (size_t)new_len);

    JSValue result = lr_new_array_buffer(ctx, dst, (size_t)new_len, lr_array_buffer_free, NULL, 1);
    if (JS_IsException(result)) free(dst);
    return result;
}

/* ── Atomics ────────────────────────────────────────────────────────────
 * Operate on integer TypedArrays. The engine is single-threaded, so the
 * read-modify-write steps are naturally uninterruptible; "atomic" semantics
 * are therefore satisfied without explicit locking. */

/* Validate a TypedArray for Atomics; on success returns the buffer base
 * pointer and the byte offset of `index` within it. Returns NULL (with an
 * exception already thrown) on failure. */
static uint8_t *atomics_prepare(LRContext *ctx, LRValue ta_val, LRValue index_val,
                                TypedArrayData **out_tad, size_t *out_byte_index)
{
    if (ta_val.tag != LR_TYPE_OBJECT) {
        JS_ThrowTypeError(ctx, "Atomics: first argument must be an integer TypedArray");
        return NULL;
    }
    LRObject *o = (LRObject *)ta_val.u.ptr;
    if (o->type != LR_OBJ_TYPED_ARRAY || !o->opaque) {
        JS_ThrowTypeError(ctx, "Atomics: first argument must be an integer TypedArray");
        return NULL;
    }
    TypedArrayData *tad = (TypedArrayData *)o->opaque;
    int magic = tad->magic;
    if (magic == TA_MAGIC_FLOAT32 || magic == TA_MAGIC_FLOAT64 ||
        magic == TA_MAGIC_BIGUINT64 || magic == TA_MAGIC_BIGINT64) {
        JS_ThrowTypeError(ctx, "Atomics: cannot operate on float or BigInt TypedArray");
        return NULL;
    }

    int32_t idx;
    JS_ToInt32(ctx, &idx, index_val);
    if (idx < 0) {
        JS_ThrowRangeError(ctx, "Atomics: index must be a non-negative integer");
        return NULL;
    }
    size_t elem_count = tad->byte_length / tad->element_size;
    if ((size_t)idx >= elem_count) {
        JS_ThrowRangeError(ctx, "Atomics: index out of range");
        return NULL;
    }
    uint8_t *base = lr_get_array_buffer(ctx, NULL, tad->buffer);
    if (!base) {
        JS_ThrowTypeError(ctx, "Atomics: buffer is detached");
        return NULL;
    }
    *out_tad = tad;
    *out_byte_index = tad->byte_offset + (size_t)idx * tad->element_size;
    return base;
}

static uint64_t atomics_read_raw(uint8_t *base, size_t bi, int magic, int *is_signed)
{
    switch (magic) {
    case TA_MAGIC_UINT8:  *is_signed = 0; return ((uint8_t *)base)[bi];
    case TA_MAGIC_INT8:   *is_signed = 1; return (uint64_t)(int64_t)((int8_t *)base)[bi];
    case TA_MAGIC_UINT16: { uint16_t v; memcpy(&v, base + bi, 2); *is_signed = 0; return v; }
    case TA_MAGIC_INT16:  { int16_t v;  memcpy(&v, base + bi, 2); *is_signed = 1; return (uint64_t)(int64_t)v; }
    case TA_MAGIC_UINT32: { uint32_t v; memcpy(&v, base + bi, 4); *is_signed = 0; return v; }
    case TA_MAGIC_INT32:  { int32_t v;  memcpy(&v, base + bi, 4); *is_signed = 1; return (uint64_t)(int64_t)v; }
    }
    *is_signed = 0;
    return 0;
}

static void atomics_write_raw(uint8_t *base, size_t bi, int magic, uint64_t raw)
{
    switch (magic) {
    case TA_MAGIC_UINT8:  ((uint8_t *)base)[bi] = (uint8_t)raw; break;
    case TA_MAGIC_INT8:   ((int8_t *)base)[bi]  = (int8_t)raw; break;
    case TA_MAGIC_UINT16: { uint16_t v = (uint16_t)raw; memcpy(base + bi, &v, 2); break; }
    case TA_MAGIC_INT16:  { int16_t v = (int16_t)raw;  memcpy(base + bi, &v, 2); break; }
    case TA_MAGIC_UINT32: { uint32_t v = (uint32_t)raw; memcpy(base + bi, &v, 4); break; }
    case TA_MAGIC_INT32:  { int32_t v = (int32_t)raw;  memcpy(base + bi, &v, 4); break; }
    }
}

static LRValue atomics_to_js(LRContext *ctx, int magic, uint64_t raw)
{
    if (magic == TA_MAGIC_UINT32)
        return JS_NewFloat64(ctx, (double)(uint32_t)raw);
    return JS_NewInt32(ctx, (int32_t)(int64_t)raw);
}

/* op: 0=add 1=sub 2=and 3=or 4=xor 5=exchange 6=compareExchange
 *      7=load 8=store */
static LRValue atomics_op(LRContext *ctx, LRValue ta, LRValue index, LRValue value,
                          int op, LRValue expected)
{
    TypedArrayData *tad;
    size_t bi;
    uint8_t *base = atomics_prepare(ctx, ta, index, &tad, &bi);
    if (!base) return JS_EXCEPTION;

    int is_signed;
    uint64_t old = atomics_read_raw(base, bi, tad->magic, &is_signed);
    double dv = 0, de = 0;
    if (value.tag != LR_TYPE_UNDEFINED)    JS_ToFloat64(ctx, &dv, value);
    if (expected.tag != LR_TYPE_UNDEFINED) JS_ToFloat64(ctx, &de, expected);
    uint64_t v = (uint64_t)(int64_t)dv;
    uint64_t e = (uint64_t)(int64_t)de;

    uint64_t mask = (tad->element_size == 1) ? 0xFFULL
                  : (tad->element_size == 2) ? 0xFFFFULL
                                             : 0xFFFFFFFFULL;

    if (op == 6) { /* compareExchange: return the OLD value */
        if (old == e) atomics_write_raw(base, bi, tad->magic, v & mask);
        return atomics_to_js(ctx, tad->magic, old);
    }

    uint64_t r;
    switch (op) {
    case 0: r = (old + v);  break;
    case 1: r = (old - v);  break;
    case 2: r = (old & v);  break;
    case 3: r = (old | v);  break;
    case 4: r = (old ^ v);  break;
    case 5: r = v;          break;  /* exchange */
    case 7: r = old;        break;  /* load */
    case 8: r = v;          break;  /* store */
    default: r = old;
    }
    r &= mask;
    if (op != 7) atomics_write_raw(base, bi, tad->magic, r);
    return atomics_to_js(ctx, tad->magic, (op == 7) ? old : r);
}

static LRValue js_atomics_load(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Atomics.load: requires (typedArray, index)");
    return atomics_op(ctx, argv[0], argv[1], LR_VALUE_UNDEFINED, 7, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_store(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.store: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 8, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_add(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.add: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 0, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_sub(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.sub: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 1, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_and(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.and: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 2, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_or(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.or: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 3, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_xor(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.xor: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 4, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_exchange(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Atomics.exchange: requires (typedArray, index, value)");
    return atomics_op(ctx, argv[0], argv[1], argv[2], 5, LR_VALUE_UNDEFINED);
}

static LRValue js_atomics_compareExchange(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "Atomics.compareExchange: requires (typedArray, index, expectedValue, replacementValue)");
    /* argv[2] = expected, argv[3] = replacement */
    return atomics_op(ctx, argv[0], argv[1], argv[3], 6, argv[2]);
}

static LRValue js_atomics_isLockFree(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    int32_t size = 4;
    if (argc >= 1) JS_ToInt32(ctx, &size, argv[0]);
    LRValue result = JS_NewBool(ctx, (size == 1 || size == 2 || size == 4));
    return result;
}

static LRValue js_atomics_wait(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx,
        "Atomics.wait: requires (typedArray, index, value[, timeout])");
    TypedArrayData *tad;
    size_t bi;
    uint8_t *base = atomics_prepare(ctx, argv[0], argv[1], &tad, &bi);
    if (!base) return JS_EXCEPTION;
    if (tad->magic != TA_MAGIC_INT32) {
        return JS_ThrowTypeError(ctx, "Atomics.wait: requires an Int32Array");
    }
    int is_signed;
    uint64_t cur = atomics_read_raw(base, bi, tad->magic, &is_signed);
    int32_t wait_val = 0;
    JS_ToInt32(ctx, &wait_val, argv[2]);
    if (cur != (uint64_t)(uint32_t)(int32_t)wait_val) {
        return JS_NewString(ctx, "not-equal");
    }
    /* Single-threaded engine: we cannot block the agent. A timeout of 0 or
     * any requested wait returns "timed-out" immediately (no real waiting). */
    return JS_NewString(ctx, "timed-out");
}

static LRValue js_atomics_notify(LRContext *ctx, LRValue this_val, int argc, LRValue *argv)
{
    (void)this_val;
    (void)argv;
    if (argc < 1) return JS_ThrowTypeError(ctx,
        "Atomics.notify: requires (typedArray, index[, count])");
    /* Single-threaded engine: there are no waiting agents to wake. */
    return JS_NewInt32(ctx, 0);
}

static const JSCFunctionListEntry js_atomics_funcs[] = {
    JS_CFUNC_DEF("load",            2, js_atomics_load),
    JS_CFUNC_DEF("store",           3, js_atomics_store),
    JS_CFUNC_DEF("add",             3, js_atomics_add),
    JS_CFUNC_DEF("sub",             3, js_atomics_sub),
    JS_CFUNC_DEF("and",             3, js_atomics_and),
    JS_CFUNC_DEF("or",              3, js_atomics_or),
    JS_CFUNC_DEF("xor",             3, js_atomics_xor),
    JS_CFUNC_DEF("exchange",        3, js_atomics_exchange),
    JS_CFUNC_DEF("compareExchange", 4, js_atomics_compareExchange),
    JS_CFUNC_DEF("isLockFree",      1, js_atomics_isLockFree),
    JS_CFUNC_DEF("wait",            4, js_atomics_wait),
    JS_CFUNC_DEF("notify",          3, js_atomics_notify),
};

/* ========================================================================
 *  9. INITIALIZATION FUNCTION
 * ======================================================================== */

void lr_builtins_extra_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* ── Register Math ───────────────────────────────────────────────────── */
    {
        JSValue math_obj = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, math_obj, js_math_funcs,
                                    sizeof(js_math_funcs) / sizeof(js_math_funcs[0]));

        /* Math constants */
        JS_SetPropertyStr(ctx, math_obj, "E",       JS_NewFloat64(ctx, 2.718281828459045));
        JS_SetPropertyStr(ctx, math_obj, "LN2",     JS_NewFloat64(ctx, 0.6931471805599453));
        JS_SetPropertyStr(ctx, math_obj, "LN10",    JS_NewFloat64(ctx, 2.302585092994046));
        JS_SetPropertyStr(ctx, math_obj, "LOG2E",   JS_NewFloat64(ctx, 1.4426950408889634));
        JS_SetPropertyStr(ctx, math_obj, "LOG10E",  JS_NewFloat64(ctx, 0.4342944819032518));
        JS_SetPropertyStr(ctx, math_obj, "PI",      JS_NewFloat64(ctx, 3.141592653589793));
        JS_SetPropertyStr(ctx, math_obj, "SQRT1_2", JS_NewFloat64(ctx, 0.7071067811865476));
        JS_SetPropertyStr(ctx, math_obj, "SQRT2",   JS_NewFloat64(ctx, 1.4142135623730951));

        JS_SetPropertyStr(ctx, global, "Math", math_obj);
    }

    /* ── Register JSON ───────────────────────────────────────────────── */
    {
        JSValue json_obj = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, json_obj, js_json_funcs,
                                    sizeof(js_json_funcs) / sizeof(js_json_funcs[0]));
        JS_SetPropertyStr(ctx, global, "JSON", json_obj);
    }

    /* ── Register Date ───────────────────────────────────────────────── */
    {
        /* Create Date constructor */
        JSValue date_ctor = JS_NewCFunction(ctx, js_date_constructor, "Date", 7);

        /* Create Date.prototype */
        JSValue date_proto = JS_NewObject(ctx);

        /* Add prototype methods */
        JS_SetPropertyFunctionList(ctx, date_proto, js_date_proto_methods,
                                    sizeof(js_date_proto_methods) / sizeof(js_date_proto_methods[0]));

        /* Set constructor property on prototype */
        JS_SetPropertyStr(ctx, date_proto, "constructor", JS_DupValue(ctx, date_ctor));

        /* Set prototype on constructor */
        JS_SetPropertyStr(ctx, date_ctor, "prototype", JS_DupValue(ctx, date_proto));

        /* Add static methods to Date constructor */
        JS_SetPropertyFunctionList(ctx, date_ctor, js_date_static_methods,
                                    sizeof(js_date_static_methods) / sizeof(js_date_static_methods[0]));

        /* Register on global */
        JS_SetPropertyStr(ctx, global, "Date", date_ctor);

        /* Note: date_proto is now owned by the "prototype" property, do NOT free it */
        /* date_ctor is now owned by the global property, do NOT free it */
    }

    /* ── Register RegExp ─────────────────────────────────────────────── */
    {
        /* Create RegExp constructor */
        JSValue regexp_ctor = JS_NewCFunction(ctx, js_regexp_constructor, "RegExp", 2);

        /* Create RegExp.prototype */
        JSValue regexp_proto = JS_NewObject(ctx);

        /* Add prototype methods */
        JS_SetPropertyFunctionList(ctx, regexp_proto, js_regexp_proto_methods,
                                    sizeof(js_regexp_proto_methods) / sizeof(js_regexp_proto_methods[0]));

        /* Add getters */
        JS_SetPropertyFunctionList(ctx, regexp_proto, js_regexp_proto_getters,
                                    sizeof(js_regexp_proto_getters) / sizeof(js_regexp_proto_getters[0]));

        /* Set constructor property on prototype */
        JS_SetPropertyStr(ctx, regexp_proto, "constructor", JS_DupValue(ctx, regexp_ctor));

        /* Set prototype on constructor */
        JS_SetPropertyStr(ctx, regexp_ctor, "prototype", JS_DupValue(ctx, regexp_proto));

        /* Register on global */
        JS_SetPropertyStr(ctx, global, "RegExp", regexp_ctor);
    }

    /* ── Register Symbol ─────────────────────────────────────────────── */
    {
        /* Create Symbol constructor */
        JSValue sym_ctor = JS_NewCFunction(ctx, js_symbol_constructor, "Symbol", 1);

        /* Create Symbol.prototype */
        JSValue sym_proto = JS_NewObject(ctx);

        /* Add prototype methods */
        JS_SetPropertyFunctionList(ctx, sym_proto, js_symbol_proto_methods,
                                    sizeof(js_symbol_proto_methods) / sizeof(js_symbol_proto_methods[0]));

        /* Add description getter */
        static const JSCFunctionListEntry sym_desc_getter[] = {
            JS_CGETSET_DEF("description", js_symbol_get_description, NULL),
        };
        JS_SetPropertyFunctionList(ctx, sym_proto, sym_desc_getter,
                                    sizeof(sym_desc_getter) / sizeof(sym_desc_getter[0]));

        /* Set constructor property on prototype */
        JS_SetPropertyStr(ctx, sym_proto, "constructor", JS_DupValue(ctx, sym_ctor));

        /* Set prototype on constructor */
        JS_SetPropertyStr(ctx, sym_ctor, "prototype", JS_DupValue(ctx, sym_proto));

        /* Add static methods */
        JS_SetPropertyFunctionList(ctx, sym_ctor, js_symbol_static_methods,
                                    sizeof(js_symbol_static_methods) / sizeof(js_symbol_static_methods[0]));

        /* Well-known symbols */
        #define SYMBOL_WELL_KNOWN(name) do { \
            JSValue s = JS_NewObject(ctx); \
            JS_SetOpaque(s, strdup("Symbol." name)); \
            JS_SetPropertyStr(ctx, sym_ctor, name, s); \
        } while(0)

        SYMBOL_WELL_KNOWN("iterator");
        SYMBOL_WELL_KNOWN("asyncIterator");
        SYMBOL_WELL_KNOWN("hasInstance");
        SYMBOL_WELL_KNOWN("isConcatSpreadable");
        SYMBOL_WELL_KNOWN("match");
        SYMBOL_WELL_KNOWN("replace");
        SYMBOL_WELL_KNOWN("search");
        SYMBOL_WELL_KNOWN("split");
        SYMBOL_WELL_KNOWN("toPrimitive");
        SYMBOL_WELL_KNOWN("toStringTag");
        SYMBOL_WELL_KNOWN("unscopables");
        SYMBOL_WELL_KNOWN("species");
        SYMBOL_WELL_KNOWN("matchAll");

        #undef SYMBOL_WELL_KNOWN

        /* Register on global */
        JS_SetPropertyStr(ctx, global, "Symbol", sym_ctor);
    }

    /* ── Register Error Subclasses ───────────────────────────────────── */
    {
        /* Get Error constructor and prototype */
        JSValue error_ctor = JS_GetPropertyStr(ctx, global, "Error");
        JSValue error_proto = JS_GetPropertyStr(ctx, error_ctor, "prototype");

        if (!JS_IsUndefined(error_ctor) && !JS_IsException(error_ctor) &&
            !JS_IsUndefined(error_proto) && !JS_IsException(error_proto)) {
            create_error_subclass(ctx, global, "TypeError",      error_proto, error_ctor);
            create_error_subclass(ctx, global, "RangeError",     error_proto, error_ctor);
            create_error_subclass(ctx, global, "SyntaxError",    error_proto, error_ctor);
            create_error_subclass(ctx, global, "ReferenceError", error_proto, error_ctor);
            create_error_subclass(ctx, global, "EvalError",      error_proto, error_ctor);
            create_error_subclass(ctx, global, "URIError",       error_proto, error_ctor);
        }

        JS_FreeValue(ctx, error_proto);
        JS_FreeValue(ctx, error_ctor);
    }

    /* ── Register WeakMap ────────────────────────────────────────────── */
    {
        JSValue wm_ctor = JS_NewCFunction(ctx, js_weakmap_constructor, "WeakMap", 0);
        JSValue wm_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, wm_proto, js_weakmap_proto_methods,
                                    sizeof(js_weakmap_proto_methods) / sizeof(js_weakmap_proto_methods[0]));

        JS_SetPropertyStr(ctx, wm_proto, "constructor", JS_DupValue(ctx, wm_ctor));
        JS_SetPropertyStr(ctx, wm_ctor, "prototype", JS_DupValue(ctx, wm_proto));
        JS_SetPropertyStr(ctx, global, "WeakMap", wm_ctor);
    }

    /* ── Register WeakSet ────────────────────────────────────────────── */
    {
        JSValue ws_ctor = JS_NewCFunction(ctx, js_weakset_constructor, "WeakSet", 0);
        JSValue ws_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, ws_proto, js_weakset_proto_methods,
                                    sizeof(js_weakset_proto_methods) / sizeof(js_weakset_proto_methods[0]));

        JS_SetPropertyStr(ctx, ws_proto, "constructor", JS_DupValue(ctx, ws_ctor));
        JS_SetPropertyStr(ctx, ws_ctor, "prototype", JS_DupValue(ctx, ws_proto));
        JS_SetPropertyStr(ctx, global, "WeakSet", ws_ctor);
    }

    /* ── Register WeakRef ─────────────────────────────────────────────── */
    {
        JSValue wr_ctor = JS_NewCFunction(ctx, js_weak_ref_constructor, "WeakRef", 1);
        JSValue wr_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, wr_proto, js_weak_ref_proto_methods,
                                    sizeof(js_weak_ref_proto_methods) / sizeof(js_weak_ref_proto_methods[0]));

        JS_SetPropertyStr(ctx, wr_proto, "constructor", JS_DupValue(ctx, wr_ctor));
        JS_SetPropertyStr(ctx, wr_ctor, "prototype", JS_DupValue(ctx, wr_proto));
        JS_SetPropertyStr(ctx, global, "WeakRef", wr_ctor);
    }

    /* ── Register FinalizationRegistry ────────────────────────────────── */
    {
        JSValue fr_ctor = JS_NewCFunction(ctx, js_finalization_registry_constructor,
                                          "FinalizationRegistry", 1);
        JSValue fr_proto = JS_NewObject(ctx);

        JS_SetPropertyFunctionList(ctx, fr_proto, js_finalization_registry_proto_methods,
                                    sizeof(js_finalization_registry_proto_methods)
                                        / sizeof(js_finalization_registry_proto_methods[0]));

        JS_SetPropertyStr(ctx, fr_proto, "constructor", JS_DupValue(ctx, fr_ctor));
        JS_SetPropertyStr(ctx, fr_ctor, "prototype", JS_DupValue(ctx, fr_proto));
        JS_SetPropertyStr(ctx, global, "FinalizationRegistry", fr_ctor);
    }

    /* ── Register ArrayBuffer ────────────────────────────────────────── */
    {
        JSValue ab = JS_GetPropertyStr(ctx, global, "ArrayBuffer");
        if (JS_IsUndefined(ab) || JS_IsException(ab)) {
            JS_FreeValue(ctx, ab);
            /* Create ArrayBuffer constructor */
            JSValue ab_ctor = JS_NewCFunction(ctx, js_array_buffer_constructor,
                                              "ArrayBuffer", 1);
            JSValue ab_proto = JS_NewObject(ctx);
            /* Set byteLength getter on prototype */
            JS_SetPropertyStr(ctx, ab_proto, "constructor", JS_DupValue(ctx, ab_ctor));
            JS_SetPropertyStr(ctx, ab_ctor, "prototype", JS_DupValue(ctx, ab_proto));
            JS_SetPropertyStr(ctx, global, "ArrayBuffer", JS_DupValue(ctx, ab_ctor));
            JS_FreeValue(ctx, ab_ctor);
        } else {
            JS_FreeValue(ctx, ab);
        }
    }

    /* ── Register SharedArrayBuffer ───────────────────────────────────── */
    {
        JSValue sab = JS_GetPropertyStr(ctx, global, "SharedArrayBuffer");
        if (JS_IsUndefined(sab) || JS_IsException(sab)) {
            JS_FreeValue(ctx, sab);
            JSValue sab_ctor = JS_NewCFunction(ctx, js_shared_array_buffer_constructor,
                                              "SharedArrayBuffer", 1);
            JSValue sab_proto = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, sab_proto, "constructor", JS_DupValue(ctx, sab_ctor));
            JS_SetPropertyStr(ctx, sab_proto, "slice",
                JS_NewCFunction(ctx, js_shared_array_buffer_slice, "slice", 2));
            JS_SetPropertyStr(ctx, sab_ctor, "prototype", JS_DupValue(ctx, sab_proto));
            JS_SetPropertyStr(ctx, global, "SharedArrayBuffer", JS_DupValue(ctx, sab_ctor));
            JS_FreeValue(ctx, sab_proto);
            JS_FreeValue(ctx, sab_ctor);
        } else {
            JS_FreeValue(ctx, sab);
        }
    }

    /* ── Register TypedArray Constructors ────────────────────────────── */
    {
        static const struct {
            int    magic;
            const char *name;
        } ta_ctors[] = {
            { TA_MAGIC_UINT8,     "Uint8Array" },
            { TA_MAGIC_INT8,      "Int8Array" },
            { TA_MAGIC_UINT16,    "Uint16Array" },
            { TA_MAGIC_INT16,     "Int16Array" },
            { TA_MAGIC_UINT32,    "Uint32Array" },
            { TA_MAGIC_INT32,     "Int32Array" },
            { TA_MAGIC_FLOAT32,   "Float32Array" },
            { TA_MAGIC_FLOAT64,   "Float64Array" },
            { TA_MAGIC_BIGUINT64, "BigUint64Array" },
            { TA_MAGIC_BIGINT64,  "BigInt64Array" },
        };
        int n_ctors = sizeof(ta_ctors) / sizeof(ta_ctors[0]);

        /* Create a shared prototype for all TypedArrays */
        JSValue ta_proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, ta_proto, js_typed_array_proto_methods,
            sizeof(js_typed_array_proto_methods) / sizeof(js_typed_array_proto_methods[0]));

        for (int i = 0; i < n_ctors; i++) {
            JSValue ctor = JS_NewCFunction2(ctx, js_typed_array_constructor,
                ta_ctors[i].name, 3, 0, ta_ctors[i].magic);
            JSValue proto = JS_NewObject(ctx);
            JS_SetPrototype(ctx, proto, JS_DupValue(ctx, ta_proto));
            JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));
            JS_SetPropertyStr(ctx, proto, "BYTES_PER_ELEMENT",
                JS_NewInt32(ctx, (int32_t)ta_type_info[ta_ctors[i].magic].elem_size));
            JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));
            JS_SetPropertyStr(ctx, ctor, "BYTES_PER_ELEMENT",
                JS_NewInt32(ctx, (int32_t)ta_type_info[ta_ctors[i].magic].elem_size));
            JS_SetPropertyStr(ctx, global, ta_ctors[i].name, JS_DupValue(ctx, ctor));
            JS_FreeValue(ctx, proto);
            JS_FreeValue(ctx, ctor);
        }
        JS_FreeValue(ctx, ta_proto);
    }

    /* ── Register DataView ────────────────────────────────────────────── */
    {
        JSValue dv_ctor = JS_NewCFunction(ctx, js_dataview_constructor, "DataView", 3);
        JSValue dv_proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, dv_proto, js_dataview_proto_methods,
            sizeof(js_dataview_proto_methods) / sizeof(js_dataview_proto_methods[0]));
        JS_SetPropertyStr(ctx, dv_proto, "constructor", JS_DupValue(ctx, dv_ctor));
        JS_SetPropertyStr(ctx, dv_ctor, "prototype", JS_DupValue(ctx, dv_proto));
        JS_SetPropertyStr(ctx, global, "DataView", JS_DupValue(ctx, dv_ctor));
        JS_FreeValue(ctx, dv_proto);
        JS_FreeValue(ctx, dv_ctor);
    }

    /* ── Register Atomics ─────────────────────────────────────────────── */
    {
        JSValue atomics_obj = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, atomics_obj, js_atomics_funcs,
            sizeof(js_atomics_funcs) / sizeof(js_atomics_funcs[0]));
        JS_SetPropertyStr(ctx, global, "Atomics", JS_DupValue(ctx, atomics_obj));
        JS_FreeValue(ctx, atomics_obj);
    }

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Extra built-in objects initialized (Math, JSON, Date, RegExp, Symbol, Error subclasses, WeakMap, WeakSet, TypedArray, DataView)");
}