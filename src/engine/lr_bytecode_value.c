/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: value
 */
#include "lr_bytecode.h"
#include "lr_bytecode_internal.h"
#include "lr_interp.h"
#include "lr_jit.h"
#include "lr_ast.h"
#include "lr_platform.h"   /* lr_get_time_us() */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#ifdef STR_POOL_DEBUG
#include <malloc.h>
#endif

#ifndef NAN
#define NAN (0.0 / 0.0)
#endif

/* ── Threading mode ──────────────────────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
  #define LR_THREADED_CODE 1
  /* BC_CASE: computed-goto label (the trailing colon comes from call site). */
  #define BC_CASE(lbl, op) lbl_##lbl:
#else
  #define LR_THREADED_CODE 0
  /* BC_CASE: switch case label (the trailing colon comes from call site). */
  #define BC_CASE(lbl, op) case op:
#endif

/* =======================================================================
   C-LEVEL JAVASCRIPT VALUE OPERATIONS

   These are the semantics of the language expressed directly in C: no
   AST, no interpreter round-trip.
   ======================================================================= */

/* Fast-path inline: avoid function call to lr_to_float64 for the
 * common int32/float64 cases in tight numeric loops.              */
double bcv_to_number(LRContext *ctx, LRValue v)
{
    if (v.tag == LR_TYPE_INT32)  return (double)v.u.int32;
    if (v.tag == LR_TYPE_FLOAT64) return v.u.float64;
    double d = 0;
    lr_to_float64(ctx, &d, v);
    return d;
}

int32_t bcv_to_int32(LRContext *ctx, LRValue v)
{
    if (v.tag == LR_TYPE_INT32)  return v.u.int32;
    if (v.tag == LR_TYPE_FLOAT64) return (int32_t)v.u.float64;
    int32_t i = 0;
    lr_to_int32(ctx, &i, v);
    return i;
}

/* Box a double, preferring the int32 representation (matches the
 * tree-walking interpreter and keeps the integer fast path hot).
 * NOTE: -0.0 must never be demoted to int32, otherwise the negative-zero
 * sign bit is silently lost (breaks `-0` literal, `0 * -1`, etc.). */
LRValue bcv_number(LRContext *ctx, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    int is_neg_zero = (d == 0.0 && (bits >> 63));
    if (!is_neg_zero && d == (double)(int32_t)d && !isnan(d) && !isinf(d))
        return lr_new_int32(ctx, (int32_t)d);
    return lr_new_float64(ctx, d);
}

static int bcv_strict_eq(LRValue a, LRValue b)
{
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case LR_TYPE_UNDEFINED: return 1;
    case LR_TYPE_NULL:      return 1;
    case LR_TYPE_BOOL:      return a.u.bool_val == b.u.bool_val;
    case LR_TYPE_INT32:     return a.u.int32 == b.u.int32;
    case LR_TYPE_FLOAT64:   return a.u.float64 == b.u.float64;
    case LR_TYPE_STRING: {
        LRString *sa = (LRString *)a.u.ptr;
        LRString *sb = (LRString *)b.u.ptr;
        if (sa == sb) return 1;
        if (!sa || !sb) return 0;
        if (sa->len != sb->len) return 0;
        return memcmp(sa->str, sb->str, sa->len) == 0;
    }
    case LR_TYPE_OBJECT:    return a.u.ptr == b.u.ptr;
    case LR_TYPE_SYMBOL:    return a.u.ptr == b.u.ptr;
    default:                return 0;
    }
}

static int bcv_is_number(LRValue v)
{
    return v.tag == LR_TYPE_INT32 || v.tag == LR_TYPE_FLOAT64;
}

static int bcv_abstract_eq(LRContext *ctx, LRValue a, LRValue b)
{
    if (a.tag == b.tag) return bcv_strict_eq(a, b);

    if ((a.tag == LR_TYPE_UNDEFINED && b.tag == LR_TYPE_NULL) ||
        (a.tag == LR_TYPE_NULL && b.tag == LR_TYPE_UNDEFINED)) return 1;

    /* number <-> string, boolean coercions */
    if ((bcv_is_number(a) && b.tag == LR_TYPE_STRING) ||
        (a.tag == LR_TYPE_STRING && bcv_is_number(b)) ||
        a.tag == LR_TYPE_BOOL || b.tag == LR_TYPE_BOOL) {
        if (a.tag == LR_TYPE_UNDEFINED || a.tag == LR_TYPE_NULL ||
            b.tag == LR_TYPE_UNDEFINED || b.tag == LR_TYPE_NULL) return 0;
        /* object <-> bool/number/string: object must be ToPrimitive'd first
         * (e.g. ![]==[] → false==[] → 0==[] → 0=='' → 0==0). Without this,
         * bcv_to_number would coerce the object directly to NaN and we would
         * wrongly return false.  Recurse with the primitive so the general
         * comparison rules apply. */
        if (a.tag == LR_TYPE_OBJECT) {
            const char *sa = lr_to_cstring(ctx, a);
            LRValue prim = lr_new_string(ctx, sa ? sa : "");
            lr_free_cstring(ctx, sa);
            int ra = bcv_abstract_eq(ctx, prim, b);
            lr_free_value(ctx, prim);
            return ra;
        }
        if (b.tag == LR_TYPE_OBJECT) {
            const char *sb = lr_to_cstring(ctx, b);
            LRValue prim = lr_new_string(ctx, sb ? sb : "");
            lr_free_cstring(ctx, sb);
            int rb = bcv_abstract_eq(ctx, a, prim);
            lr_free_value(ctx, prim);
            return rb;
        }
        double da = bcv_to_number(ctx, a);
        double db = bcv_to_number(ctx, b);
        if (isnan(da) || isnan(db)) return 0;
        return da == db;
    }

    /* object <-> primitive: compare string forms */
    if (a.tag == LR_TYPE_OBJECT && (b.tag == LR_TYPE_STRING || bcv_is_number(b))) {
        const char *s = lr_to_cstring(ctx, a);
        LRValue prim = lr_new_string(ctx, s ? s : "");
        lr_free_cstring(ctx, s);
        int r = bcv_abstract_eq(ctx, prim, b);
        lr_free_value(ctx, prim);
        return r;
    }
    if (b.tag == LR_TYPE_OBJECT && (a.tag == LR_TYPE_STRING || bcv_is_number(a))) {
        const char *s = lr_to_cstring(ctx, b);
        LRValue prim = lr_new_string(ctx, s ? s : "");
        lr_free_cstring(ctx, s);
        int r = bcv_abstract_eq(ctx, a, prim);
        lr_free_value(ctx, prim);
        return r;
    }
    return 0;
}

/* String concatenation entirely in C with three key optimizations:
 *
 * 1. POOL ALLOC: Uses the ring-buffer string pool (O(1) allocation) for
 *    the result string, avoiding malloc/free churn in tight loops.
 *
 * 2. NUMBER DIRECT FORMAT (string+number): Instead of calling lr_to_cstring
 *    (which creates an intermediate LRString), we snprintf the number
 *    directly into the target buffer, avoiding the temp string allocation.
 *
 * 3. STRING+STRING FALLBACK: 1 malloc + 2 memcpy for the general case. */
static LRValue bcv_concat(LRContext *ctx, LRValue a, LRValue b)
{
#ifdef STR_POOL_DEBUG
    if (a.tag == LR_TYPE_STRING) {
        LRString *as = (LRString *)a.u.ptr;
        if (as && malloc_usable_size(as) < sizeof(LRString) + as->len + 1) {
            fprintf(stderr, "[CONCAT] a overreport len=%u usable=%zu str='%.16s'\n",
                    as->len, malloc_usable_size(as), as->str);
            abort();
        }
    }
    if (b.tag == LR_TYPE_STRING) {
        LRString *bs = (LRString *)b.u.ptr;
        if (bs && malloc_usable_size(bs) < sizeof(LRString) + bs->len + 1) {
            fprintf(stderr, "[CONCAT] b overreport len=%u usable=%zu str='%.16s'\n",
                    bs->len, malloc_usable_size(bs), bs->str);
            abort();
        }
    }
#endif
    if (a.tag == LR_TYPE_STRING) {
        LRString *as = (LRString *)a.u.ptr;
        size_t la = as ? as->len : 0;

        if (b.tag == LR_TYPE_STRING) {
            LRString *bs = (LRString *)b.u.ptr;
            size_t lb = bs ? bs->len : 0;
            size_t total = la + lb;

            LRString *os = lr_string_alloc(ctx->rt, NULL, total);
            if (!os) return lr_new_string(ctx, "");
            if (la) memcpy(os->str, as->str, la);
            if (lb) memcpy(os->str + la, bs->str, lb);
            os->str[total] = '\0';
            LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os; return r;
        }

        /* OPTIMIZATION: Format number directly into target buffer */
        if (b.tag == LR_TYPE_INT32) {
            char num_buf[32];
            int nlen = snprintf(num_buf, sizeof(num_buf), "%d", b.u.int32);
            size_t total = la + (size_t)nlen;

            LRString *os = lr_string_alloc(ctx->rt, NULL, total);
            if (!os) return lr_new_string(ctx, "");
            if (la) memcpy(os->str, as->str, la);
            memcpy(os->str + la, num_buf, (size_t)nlen);
            os->str[total] = '\0';
            LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os; return r;
        }
        if (b.tag == LR_TYPE_FLOAT64) {
            char num_buf[64];
            int nlen;
            double d = b.u.float64;
            if (isnan(d)) {
                nlen = snprintf(num_buf, sizeof(num_buf), "NaN");
            } else if (isinf(d)) {
                nlen = snprintf(num_buf, sizeof(num_buf), d > 0.0 ? "Infinity" : "-Infinity");
            } else {
                nlen = snprintf(num_buf, sizeof(num_buf), "%.17g", d);
            }
            size_t total = la + (size_t)nlen;

            LRString *os = lr_string_alloc(ctx->rt, NULL, total);
            if (!os) return lr_new_string(ctx, "");
            if (la) memcpy(os->str, as->str, la);
            memcpy(os->str + la, num_buf, (size_t)nlen);
            os->str[total] = '\0';
            LRValue r; r.tag = LR_TYPE_STRING; r.u.ptr = os; return r;
        }

        const char *sb = lr_to_cstring(ctx, b);
        size_t lb = sb ? strlen(sb) : 0;
        size_t total = la + lb;
        LRString *os = lr_string_alloc(ctx->rt, NULL, total);
        if (!os) { lr_free_cstring(ctx, sb); return lr_new_string(ctx, ""); }
        if (la) memcpy(os->str, as->str, la);
        if (lb) memcpy(os->str + la, sb, lb);
        os->str[total] = '\0';
        lr_free_cstring(ctx, sb);
        return (LRValue){ .tag = LR_TYPE_STRING, .u.ptr = os };
    }
    {
        const char *sa = lr_to_cstring(ctx, a);
        const char *sb = lr_to_cstring(ctx, b);
        size_t la = sa ? strlen(sa) : 0;
        size_t lb = sb ? strlen(sb) : 0;
        size_t total = la + lb;
        LRString *os = lr_string_alloc(ctx->rt, NULL, total);
        LRValue out = LR_VALUE_UNDEFINED;
        if (os) {
            if (la) memcpy(os->str, sa, la);
            if (lb) memcpy(os->str + la, sb, lb);
            os->str[total] = '\0';
            out.tag = LR_TYPE_STRING; out.u.ptr = os;
        } else {
            out = lr_new_string(ctx, "");
        }
        lr_free_cstring(ctx, sa);
        lr_free_cstring(ctx, sb);
        return out;
    }
}

/* Relational comparison with proper string ordering. op: 0 '<' 1 '>' 2 '<=' 3 '>=' */
static int bcv_relational(LRContext *ctx, LRValue a, LRValue b, int op)
{
    if (a.tag == LR_TYPE_STRING && b.tag == LR_TYPE_STRING) {
        LRString *sa = (LRString *)a.u.ptr;
        LRString *sb = (LRString *)b.u.ptr;
        size_t na = sa ? sa->len : 0, nb = sb ? sb->len : 0;
        size_t nmin = na < nb ? na : nb;
        int cmp = nmin ? memcmp(sa->str, sb->str, nmin) : 0;
        if (cmp == 0) cmp = (na == nb) ? 0 : (na < nb ? -1 : 1);
        switch (op) {
        case 0: return cmp < 0;
        case 1: return cmp > 0;
        case 2: return cmp <= 0;
        default: return cmp >= 0;
        }
    }
    double da = bcv_to_number(ctx, a);
    double db = bcv_to_number(ctx, b);
    if (isnan(da) || isnan(db)) return 0;
    switch (op) {
    case 0: return da < db;
    case 1: return da > db;
    case 2: return da <= db;
    default: return da >= db;
    }
}

const char *bcv_typeof(LRContext *ctx, LRValue v)
{
    switch (v.tag) {
    case LR_TYPE_UNDEFINED: return "undefined";
    case LR_TYPE_NULL:      return "object";
    case LR_TYPE_BOOL:      return "boolean";
    case LR_TYPE_INT32:
    case LR_TYPE_FLOAT64:   return "number";
    case LR_TYPE_STRING:    return "string";
    case LR_TYPE_SYMBOL:    return "symbol";
    case LR_TYPE_OBJECT: {
        LRObject *o = (LRObject *)v.u.ptr;
        if (o && o->type == LR_OBJ_BIGINT) return "bigint";
        return lr_is_function(ctx, v) ? "function" : "object";
    }
    default: return "undefined";
    }
}

static int bcv_instanceof(LRContext *ctx, LRValue a, LRValue b)
{
    if (!lr_is_object(a)) return 0;
    LRValue proto = lr_get_property_str(ctx, b, "prototype");
    if (!lr_is_object(proto)) { lr_free_value(ctx, proto); return 0; }
    int found = 0;
    LRValue p = lr_get_prototype(ctx, a);
    while (lr_is_object(p)) {
        if (bcv_strict_eq(p, proto)) { found = 1; break; }
        LRValue next = lr_get_prototype(ctx, p);
        lr_free_value(ctx, p);
        p = next;
    }
    lr_free_value(ctx, p);
    lr_free_value(ctx, proto);
    return found;
}

/* Full binary operator dispatch. Consumes a and b, returns a new value. */
LRValue bcv_binop(Interpreter *interp, int op, LRValue a, LRValue b)
{
    LRContext *ctx = interp->ctx;
    LRValue r = LR_VALUE_UNDEFINED;

    /* -- float64 fast path --------------------------------------------- */
    /* Handle float64-op-float64 directly to avoid the bcv_to_number +
     * bcv_number round-trip (which does tag dispatch + int32-range check).
     * This is the hot path for Math-heavy numeric code. */
    if (a.tag == LR_TYPE_FLOAT64 && b.tag == LR_TYPE_FLOAT64) {
        double fa = a.u.float64, fb = b.u.float64;
        switch (op) {
        case BC_ADD: return lr_new_float64(ctx, fa + fb);
        case BC_SUB: return lr_new_float64(ctx, fa - fb);
        case BC_MUL: return lr_new_float64(ctx, fa * fb);
        case BC_DIV: return lr_new_float64(ctx, fa / fb);
        case BC_LT:  return lr_new_bool(ctx, fa <  fb);
        case BC_GT:  return lr_new_bool(ctx, fa >  fb);
        case BC_LE:  return lr_new_bool(ctx, fa <= fb);
        case BC_GE:  return lr_new_bool(ctx, fa >= fb);
        case BC_EQ:  case BC_STRICT_EQ: return lr_new_bool(ctx, fa == fb);
        case BC_NE:  case BC_STRICT_NE: return lr_new_bool(ctx, fa != fb);
        default: break;
        }
    }

    /* -- BigInt fast path ---------------------------------------------- */
    /* BigInt wraps an int64_t in a heap object (LR_OBJ_BIGINT).  All
     * arithmetic must stay in the integer domain — coercing through
     * bcv_to_number()/lr_to_float64() would lose precision for values
     * beyond 2^53 and produce NaN for the (default) object case. */
    if (lr_is_bigint(a) && lr_is_bigint(b)) {
        int64_t x = 0, y = 0;
        lr_to_bigint64(ctx, &x, a);
        lr_to_bigint64(ctx, &y, b);
        switch (op) {
        case BC_ADD: return lr_new_bigint(ctx, x + y);
        case BC_SUB: return lr_new_bigint(ctx, x - y);
        case BC_MUL: return lr_new_bigint(ctx, x * y);
        case BC_DIV: {
            if (y == 0) { snprintf(interp->error_message, sizeof(interp->error_message),
                                   "Division by zero"); interp->error_flag = 1; return LR_VALUE_UNDEFINED; }
            return lr_new_bigint(ctx, x / y);
        }
        case BC_MOD: {
            if (y == 0) { snprintf(interp->error_message, sizeof(interp->error_message),
                                   "Division by zero"); interp->error_flag = 1; return LR_VALUE_UNDEFINED; }
            return lr_new_bigint(ctx, x % y);
        }
        case BC_POW: {
            int64_t rp = 1;
            for (int64_t e = y; e > 0; e--) rp *= x;
            return lr_new_bigint(ctx, rp);
        }
        case BC_LT:  return lr_new_bool(ctx, x <  y);
        case BC_GT:  return lr_new_bool(ctx, x >  y);
        case BC_LE:  return lr_new_bool(ctx, x <= y);
        case BC_GE:  return lr_new_bool(ctx, x >= y);
        case BC_EQ:  return lr_new_bool(ctx, x == y);
        case BC_NE:  return lr_new_bool(ctx, x != y);
        case BC_STRICT_EQ: return lr_new_bool(ctx, x == y);
        case BC_STRICT_NE: return lr_new_bool(ctx, x != y);
        case BC_SHL: return lr_new_bigint(ctx, x << (y & 63));
        case BC_SHR: return lr_new_bigint(ctx, x >> (y & 63)); /* BigInt has no >>> */
        case BC_SAR: return lr_new_bigint(ctx, x >> (y & 63));
        case BC_BIT_AND: return lr_new_bigint(ctx, x & y);
        case BC_BIT_OR:  return lr_new_bigint(ctx, x | y);
        case BC_BIT_XOR: return lr_new_bigint(ctx, x ^ y);
        default: break;
        }
    }

    /* -- int32 fast path ----------------------------------------------- */
    if (a.tag == LR_TYPE_INT32 && b.tag == LR_TYPE_INT32) {
        int32_t x = a.u.int32, y = b.u.int32;
        switch (op) {
        case BC_ADD: {
            /* Overflow-safe addition: use int64_t and demote back to int32
             * when the result fits.  Without this check, large accumulations
             * like `s += 3*i` for 100k iterations silently wrap at 2^31. */
            int64_t sum = (int64_t)x + (int64_t)y;
            if (sum >= INT32_MIN && sum <= INT32_MAX)
                return lr_new_int32(ctx, (int32_t)sum);
            return lr_new_float64(ctx, (double)sum);
        }
        case BC_SUB: {
            int64_t diff = (int64_t)x - (int64_t)y;
            if (diff >= INT32_MIN && diff <= INT32_MAX)
                return lr_new_int32(ctx, (int32_t)diff);
            return lr_new_float64(ctx, (double)diff);
        }
        case BC_MUL: {
            double d = (double)x * (double)y;
            return bcv_number(ctx, d);
        }
        case BC_BIT_AND: return lr_new_int32(ctx, x & y);
        case BC_BIT_OR:  return lr_new_int32(ctx, x | y);
        case BC_BIT_XOR: return lr_new_int32(ctx, x ^ y);
        case BC_MOD:
            /* INT_MIN % -1 makes the x86 idiv overflow and raise SIGFPE.  In
             * JS, -2147483648 % -1 === -0 (i.e. 0), so return 0 directly. */
            if (y != 0) {
                if (x == INT32_MIN && y == -1)
                    return lr_new_int32(ctx, 0);
                return lr_new_int32(ctx, x % y);
            }
            break;  /* fall through to binop_shared for y == 0 */
        case BC_SHL: return lr_new_int32(ctx, (int32_t)((uint32_t)x << (y & 31)));
        case BC_SHR: return lr_new_int32(ctx, (int32_t)((uint32_t)x >> (y & 31)));  /* >>> */
        case BC_SAR: return lr_new_int32(ctx, x >> (y & 31));                        /* >> */
        case BC_LT:  return lr_new_bool(ctx, x <  y);
        case BC_GT:  return lr_new_bool(ctx, x >  y);
        case BC_LE:  return lr_new_bool(ctx, x <= y);
        case BC_GE:  return lr_new_bool(ctx, x >= y);
        case BC_EQ:  case BC_STRICT_EQ: return lr_new_bool(ctx, x == y);
        case BC_NE:  case BC_STRICT_NE: return lr_new_bool(ctx, x != y);
        default: break;
        }
    }

    switch (op) {
    case BC_ADD:
        if (lr_is_string(a) || lr_is_string(b)) {
            r = bcv_concat(ctx, a, b);
        } else if (lr_is_object(a) || lr_is_object(b)) {
            /* ToPrimitive: objects that stringify are concatenated, the
             * remainder falls back to numeric addition. */
            LRObject *oa = lr_is_object(a) ? (LRObject *)a.u.ptr : NULL;
            LRObject *ob = lr_is_object(b) ? (LRObject *)b.u.ptr : NULL;
            int numeric = (!oa || oa->type == LR_OBJ_BIGINT) &&
                          (!ob || ob->type == LR_OBJ_BIGINT);
            if (numeric) r = bcv_number(ctx, bcv_to_number(ctx, a) + bcv_to_number(ctx, b));
            else         r = bcv_concat(ctx, a, b);
        } else {
            r = bcv_number(ctx, bcv_to_number(ctx, a) + bcv_to_number(ctx, b));
        }
        break;
    case BC_SUB: r = bcv_number(ctx, bcv_to_number(ctx, a) - bcv_to_number(ctx, b)); break;
    case BC_MUL: r = bcv_number(ctx, bcv_to_number(ctx, a) * bcv_to_number(ctx, b)); break;
    case BC_DIV: r = lr_new_float64(ctx, bcv_to_number(ctx, a) / bcv_to_number(ctx, b)); break;
    case BC_MOD: {
        double da = bcv_to_number(ctx, a), db = bcv_to_number(ctx, b);
        r = (db == 0.0) ? lr_new_float64(ctx, NAN) : bcv_number(ctx, fmod(da, db));
        break;
    }
    case BC_POW: r = lr_new_float64(ctx, pow(bcv_to_number(ctx, a), bcv_to_number(ctx, b))); break;

    case BC_LT: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 0)); break;
    case BC_GT: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 1)); break;
    case BC_LE: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 2)); break;
    case BC_GE: r = lr_new_bool(ctx, bcv_relational(ctx, a, b, 3)); break;

    case BC_EQ: r = lr_new_bool(ctx, bcv_abstract_eq(ctx, a, b)); break;
    case BC_NE: r = lr_new_bool(ctx, !bcv_abstract_eq(ctx, a, b)); break;
    case BC_STRICT_EQ: r = lr_new_bool(ctx, bcv_strict_eq(a, b)); break;
    case BC_STRICT_NE: r = lr_new_bool(ctx, !bcv_strict_eq(a, b)); break;

    case BC_SHL: r = lr_new_int32(ctx, (int32_t)((uint32_t)bcv_to_int32(ctx, a) << (bcv_to_int32(ctx, b) & 31))); break;
    case BC_SHR: r = lr_new_int32(ctx, (int32_t)((uint32_t)bcv_to_int32(ctx, a) >> (bcv_to_int32(ctx, b) & 31))); break; /* >>> */
    case BC_SAR: r = lr_new_int32(ctx, bcv_to_int32(ctx, a) >> (bcv_to_int32(ctx, b) & 31)); break; /* >> */
    case BC_BIT_AND: r = lr_new_int32(ctx, bcv_to_int32(ctx, a) & bcv_to_int32(ctx, b)); break;
    case BC_BIT_OR:  r = lr_new_int32(ctx, bcv_to_int32(ctx, a) | bcv_to_int32(ctx, b)); break;
    case BC_BIT_XOR: r = lr_new_int32(ctx, bcv_to_int32(ctx, a) ^ bcv_to_int32(ctx, b)); break;

    case BC_IN: {
        if (!lr_is_object(b)) {
            snprintf(interp->error_message, sizeof(interp->error_message),
                     "right-hand side of 'in' must be an object");
            interp->error_flag = 1;
            break;
        }
        const char *prop = lr_to_cstring(ctx, a);
        LRString *atom = lr_new_atom(ctx, prop ? prop : "");
        r = lr_new_bool(ctx, lr_has_property(ctx, b, atom));
        lr_free_cstring(ctx, prop);
        break;
    }
    case BC_INSTANCEOF:
        r = lr_new_bool(ctx, bcv_instanceof(ctx, a, b));
        break;
    default:
        break;
    }
    return r;
}

