/*
 * LR_JS - Self-implemented JavaScript Engine Core
 * Pure C implementation - value system, object model, property access,
 * type checking, runtime/context lifecycle, GC, and evaluation stubs.
 *
 * This is a fully self-implemented engine. Design follows standard
 * JS engine architecture patterns (hidden classes, reference counting,
 * tagged values) but all code is original.
 */

#include "lr_engine.h"
#include "lr_lexer.h"
#include "lr_ast.h"
#include "lr_interp.h"
#include "../lr_promise.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Memory Allocation ────────────────────────────────────────────────── */

static void *lr_malloc(LRRuntime *rt, size_t size)
{
    void *ptr = malloc(size);
    if (ptr) {
        rt->malloc_size += size;
        rt->malloc_count++;
        rt->memory_used_count++;
        rt->memory_used_size += size;
    }
    return ptr;
}

static void lr_free(LRRuntime *rt, void *ptr, size_t size)
{
    if (ptr) {
        free(ptr);
        if (rt) {
            rt->malloc_size -= size;
            rt->malloc_count--;
            rt->memory_used_count--;
            rt->memory_used_size -= size;
        }
    }
}

/* ── String Operations ────────────────────────────────────────────────── */

static LRString *lr_string_alloc(LRRuntime *rt, const char *str, size_t len)
{
    LRString *s = (LRString *)lr_malloc(rt, sizeof(LRString) + len + 1);
    if (!s) return NULL;
    s->ref_count = 1;
    s->len = (uint32_t)len;
    s->is_atom = 0;
    memcpy(s->str, str, len);
    s->str[len] = '\0';
    rt->str_count++;
    rt->str_size += (int64_t)(sizeof(LRString) + len + 1);
    return s;
}

static void lr_string_free(LRRuntime *rt, LRString *s)
{
    if (!s) return;
    /* Clear from small string cache if present, to prevent dangling pointer */
    if (rt && s->len > 0 && s->len <= 32) {
        unsigned int hash = 5381;
        for (size_t i = 0; i < s->len; i++) {
            hash = ((hash << 5) + hash) + (unsigned char)s->str[i];
        }
        unsigned int idx = hash & (LR_SMALL_STRING_CACHE_SIZE - 1);
        if (rt->small_string_cache[idx] == s) {
            rt->small_string_cache[idx] = NULL;
        }
    }
    if (rt) {
        rt->str_count--;
        rt->str_size -= (int64_t)(sizeof(LRString) + s->len + 1);
    }
    lr_free(rt, s, sizeof(LRString) + s->len + 1);
}

static void lr_string_release(LRRuntime *rt, LRString *s)
{
    if (!s) return;
    if (--s->ref_count <= 0) {
        if (rt) {
            lr_string_free(rt, s);
        } else {
            free(s->str);
            free(s);
        }
    }
}

static LRString *lr_string_dup(LRString *s)
{
    if (s) s->ref_count++;
    return s;
}

/* ── Object Operations ────────────────────────────────────────────────── */

/* Check if a string atom represents a valid numeric index */
static int lr_is_numeric_index(LRString *atom, uint32_t *pindex)
{
    if (!atom || atom->len == 0 || atom->len > 10) return 0;
    const char *str = atom->str;
    size_t len = atom->len;
    if (len == 1 && str[0] >= '0' && str[0] <= '9') {
        *pindex = (uint32_t)(str[0] - '0');
        return 1;
    }
    if (str[0] == '0') return 0; /* no leading zeros */
    uint32_t val = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;
        val = val * 10 + (uint32_t)(str[i] - '0');
    }
    *pindex = val;
    return 1;
}

/* TypedArray element getter */
static LRValue typed_array_get_elem(LRContext *ctx, TypedArrayData *tad, size_t index)
{
    size_t buf_size = 0;
    uint8_t *base = lr_get_array_buffer(ctx, &buf_size, tad->buffer);
    if (!base) return LR_VALUE_UNDEFINED;
    size_t offset = tad->byte_offset + index * tad->element_size;
    if (offset + tad->element_size > tad->byte_offset + tad->byte_length) {
        return LR_VALUE_UNDEFINED;
    }
    switch (tad->magic) {
    case 0: /* TA_MAGIC_UINT8 */
        return lr_new_int32(ctx, (int32_t)((uint8_t *)base)[offset]);
    case 1: /* TA_MAGIC_INT8 */
        return lr_new_int32(ctx, (int32_t)((int8_t *)base)[offset]);
    case 2: { /* TA_MAGIC_UINT16 */
        uint16_t v; memcpy(&v, base + offset, 2);
        return lr_new_int32(ctx, (int32_t)v);
    }
    case 3: { /* TA_MAGIC_INT16 */
        int16_t v; memcpy(&v, base + offset, 2);
        return lr_new_int32(ctx, (int32_t)v);
    }
    case 4: { /* TA_MAGIC_UINT32 */
        uint32_t v; memcpy(&v, base + offset, 4);
        return lr_new_float64(ctx, (double)v);
    }
    case 5: { /* TA_MAGIC_INT32 */
        int32_t v; memcpy(&v, base + offset, 4);
        return lr_new_int32(ctx, v);
    }
    case 6: { /* TA_MAGIC_FLOAT32 */
        float v; memcpy(&v, base + offset, 4);
        return lr_new_float64(ctx, (double)v);
    }
    case 7: { /* TA_MAGIC_FLOAT64 */
        double v; memcpy(&v, base + offset, 8);
        return lr_new_float64(ctx, v);
    }
    case 8: { /* TA_MAGIC_BIGUINT64 */
        uint64_t v; memcpy(&v, base + offset, 8);
        return lr_new_float64(ctx, (double)v);
    }
    case 9: { /* TA_MAGIC_BIGINT64 */
        int64_t v; memcpy(&v, base + offset, 8);
        return lr_new_float64(ctx, (double)v);
    }
    default:
        return LR_VALUE_UNDEFINED;
    }
}

/* TypedArray element setter */
static int typed_array_set_elem(LRContext *ctx, TypedArrayData *tad, size_t index, LRValue val)
{
    size_t buf_size = 0;
    uint8_t *base = lr_get_array_buffer(ctx, &buf_size, tad->buffer);
    if (!base) return -1;
    size_t offset = tad->byte_offset + index * tad->element_size;
    if (offset + tad->element_size > tad->byte_offset + tad->byte_length) return -1;
    double d;
    lr_to_float64(ctx, &d, val);
    switch (tad->magic) {
    case 0: ((uint8_t *)base)[offset] = (uint8_t)(int32_t)d; break;
    case 1: ((int8_t *)base)[offset] = (int8_t)(int32_t)d; break;
    case 2: { uint16_t v = (uint16_t)(int32_t)d; memcpy(base + offset, &v, 2); break; }
    case 3: { int16_t v = (int16_t)(int32_t)d; memcpy(base + offset, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)d; memcpy(base + offset, &v, 4); break; }
    case 5: { int32_t v = (int32_t)d; memcpy(base + offset, &v, 4); break; }
    case 6: { float v = (float)d; memcpy(base + offset, &v, 4); break; }
    case 7: { double v = d; memcpy(base + offset, &v, 8); break; }
    case 8: { uint64_t v = (uint64_t)d; memcpy(base + offset, &v, 8); break; }
    case 9: { int64_t v = (int64_t)d; memcpy(base + offset, &v, 8); break; }
    default: return -1;
    }
    return 0;
}

static LRObject *lr_object_alloc(LRRuntime *rt)
{
    LRObject *obj = (LRObject *)calloc(1, sizeof(LRObject));
    if (!obj) return NULL;
    obj->ref_count = 1;
    obj->type = LR_OBJ_PLAIN;
    obj->is_extensible = 1;
    obj->opaque_free = NULL;
    /* Link into runtime object list for GC tracking */
    obj->gc_next = rt->obj_list;
    rt->obj_list = obj;
    rt->obj_count++;
    return obj;
}

/* Metadata for ArrayBuffer / SharedArrayBuffer: stores the free_func and
 * its opaque argument. */
typedef struct ArrayBufferMeta {
    void (*free_func)(void *opaque, void *ptr);
    void *opaque;
    int is_shared;
} ArrayBufferMeta;

void lr_free_object(LRRuntime *rt, LRObject *obj)
{
    if (!obj) return;
    /* Unlink from runtime object list - use the object's own context
     * if rt is not provided (e.g. when called from lr_free_value(NULL, ...)) */
    if (!rt && obj->ctx) {
        rt = obj->ctx->rt;
    }
    if (rt) {
        LRObject **pprev = &rt->obj_list;
        while (*pprev && *pprev != obj) {
            pprev = &(*pprev)->gc_next;
        }
        if (*pprev == obj) {
            *pprev = obj->gc_next;
        }
    }
    /* Determine a valid context for freeing property values */
    LRContext *free_ctx = obj->ctx;
    if (!free_ctx && rt && rt->ctx_list) {
        free_ctx = rt->ctx_list;
    }
    /* Free properties */
    if (obj->props) {
        for (uint32_t i = 0; i < obj->prop_count; i++) {
            lr_free_value(free_ctx, obj->props[i]);
        }
        lr_free(rt, obj->props, obj->prop_count * sizeof(LRValue));
    }
    /* Free property hash chain */
    {
        LRProperty *prop = obj->prop_hash;
        while (prop) {
            LRProperty *next = prop->next;
            lr_string_release(rt, prop->key);
            lr_free_value(free_ctx, prop->value);
            if (prop->setter.tag != LR_TYPE_UNDEFINED)
                lr_free_value(free_ctx, prop->setter);
            if (rt) {
                lr_free(rt, prop, sizeof(LRProperty));
                rt->prop_count--;
                rt->prop_size -= sizeof(LRProperty);
            } else {
                free(prop);
            }
            prop = next;
        }
        obj->prop_hash = NULL;
    }
    /* Free shape */
    if (obj->shape) {
        obj->shape->ref_count--;
    }
    /* Free class */
    if (obj->class_def) {
        obj->class_def->ref_count--;
    }
    /* Free proto */
    lr_free_value(free_ctx, obj->proto);
    /* Free extra data */
    if (obj->extra) {
        if (obj->type == LR_OBJ_CFUNCTION) {
            LRCFunction *cf = (LRCFunction *)obj->extra;
            if (cf) {
                free((void *)cf->name);
                if (cf->data_free) {
                    cf->data_free(cf->data);
                } else {
                    /* Legacy: free data for functions that set it directly */
                }
                free(cf);
            }
            obj->extra = NULL;
        } else if (obj->type == LR_OBJ_ARRAY) {
            /* Array data is just a realloc'd buffer */
            free(obj->extra);
        } else if (obj->type == LR_OBJ_PROXY) {
            LRProxyData *pd = (LRProxyData *)obj->extra;
            if (pd) {
                lr_free_value(free_ctx, pd->target);
                lr_free_value(free_ctx, pd->handler);
                free(pd);
            }
        } else if (obj->type == LR_OBJ_ARRAY_BUFFER) {
            /* ArrayBuffer: free the buffer data using the stored free_func */
            ArrayBufferMeta *meta = (ArrayBufferMeta *)obj->opaque;
            if (meta && meta->free_func) {
                meta->free_func(meta->opaque, obj->extra);
            } else {
                free(obj->extra);
            }
            free(meta);
            obj->extra = NULL;
            obj->opaque = NULL;
        }
    }

    /* Free opaque data (typed arrays / data views / promises / generic) */
    if (obj->opaque) {
        if (obj->type == LR_OBJ_TYPED_ARRAY) {
            TypedArrayData *tad = (TypedArrayData *)obj->opaque;
            lr_free_value(free_ctx, tad->buffer);
            free((void *)(tad->name));
            free(tad);
            obj->opaque = NULL;
        } else if (obj->type == LR_OBJ_DATA_VIEW) {
            DataViewData *dvd = (DataViewData *)obj->opaque;
            lr_free_value(free_ctx, dvd->buffer);
            free(dvd);
            obj->opaque = NULL;
        } else if (obj->type == LR_OBJ_PROMISE) {
            LRPromiseData *pd = (LRPromiseData *)obj->opaque;
            if (pd) {
                lr_free_value(free_ctx, pd->result);
                for (int i = 0; i < pd->fulfill_count; i++) {
                    lr_free_value(free_ctx, pd->fulfill_reactions[i].handler);
                    lr_free_value(free_ctx, pd->fulfill_reactions[i].resolve);
                    lr_free_value(free_ctx, pd->fulfill_reactions[i].reject);
                    lr_free_value(free_ctx, pd->fulfill_reactions[i].promise);
                }
                for (int i = 0; i < pd->reject_count; i++) {
                    lr_free_value(free_ctx, pd->reject_reactions[i].handler);
                    lr_free_value(free_ctx, pd->reject_reactions[i].resolve);
                    lr_free_value(free_ctx, pd->reject_reactions[i].reject);
                    lr_free_value(free_ctx, pd->reject_reactions[i].promise);
                }
                free(pd->fulfill_reactions);
                free(pd->reject_reactions);
                free(pd);
            }
            obj->opaque = NULL;
        } else {
            /* Generic opaque data: use destructor if set, otherwise free() */
            if (obj->opaque_free) {
                obj->opaque_free(obj->opaque);
            } else {
                free(obj->opaque);
            }
            obj->opaque = NULL;
        }
    }
    if (rt) rt->obj_count--;
    free(obj);
}

/* ── Weak references & finalization support ────────────────────────────── */

static void lr_free_pending_object(LRRuntime *rt, LRObject *obj)
{
    /* Remove from the pending-finalization list */
    LRObject **pp = &rt->pending_finalize_list;
    while (*pp) {
        if (*pp == obj) { *pp = obj->finalize_next; break; }
        pp = &(*pp)->finalize_next;
    }
    obj->finalization_pending = 0;
    lr_free_object(rt, obj);
}

static int lr_has_finalization_registrations(LRRuntime *rt, LRObject *obj)
{
    LRFinalizationEntry *e = rt->finalization_entries;
    while (e) {
        if (e->target == obj) return 1;
        e = e->next;
    }
    return 0;
}

static void lr_finalization_job_data_free(void *p)
{
    LRFinalizationJobData *jd = (LRFinalizationJobData *)p;
    if (!jd) return;
    lr_free_value(jd->ctx, jd->callback);
    lr_free_value(jd->ctx, jd->heldValue);
    free(jd);
}

static LRValue js_finalization_job_func(LRContext *ctx, LRValue this_val,
                                        int argc, LRValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    LRObject *fobj = (LRObject *)ctx->current_func.u.ptr;
    if (!fobj || fobj->type != LR_OBJ_CFUNCTION) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)fobj->extra;
    LRFinalizationJobData *jd = (LRFinalizationJobData *)cf->data;
    if (!jd) return LR_VALUE_UNDEFINED;
    LRValue arg = lr_dup_value(ctx, jd->heldValue);
    LRValue result = lr_call(ctx, jd->callback, LR_VALUE_UNDEFINED, 1, &arg);
    lr_free_value(ctx, arg);
    lr_free_value(ctx, result);
    return LR_VALUE_UNDEFINED;
}

static void lr_enqueue_finalization_job(LRContext *ctx, LRValue callback, LRValue heldValue)
{
    LRValue fn = lr_new_cfunction(ctx, js_finalization_job_func, "finalizationJob", 0);
    if (fn.tag != LR_TYPE_OBJECT) { lr_free_value(ctx, fn); return; }
    LRObject *fobj = (LRObject *)fn.u.ptr;
    LRCFunction *cf = (LRCFunction *)fobj->extra;
    LRFinalizationJobData *jd = (LRFinalizationJobData *)malloc(sizeof(LRFinalizationJobData));
    if (!jd) { lr_free_value(ctx, fn); return; }
    jd->ctx = ctx;
    jd->callback = lr_dup_value(ctx, callback);
    jd->heldValue = lr_dup_value(ctx, heldValue);
    cf->data = jd;
    cf->data_free = lr_finalization_job_data_free;
    lr_enqueue_job(ctx->rt, ctx, fn);
    lr_free_value(ctx, fn);
}

/* Fire any FinalizationRegistry callbacks registered for `obj` as microtasks. */
static void lr_process_finalization_registrations(LRRuntime *rt, LRObject *obj)
{
    LRContext *ctx = rt->ctx_list;
    LRFinalizationEntry **pp = &rt->finalization_entries;
    while (*pp) {
        LRFinalizationEntry *e = *pp;
        if (e->target != obj) { pp = &e->next; continue; }
        if (ctx && !JS_IsUndefined(e->callback)) {
            lr_enqueue_finalization_job(ctx, e->callback, e->heldValue);
        }
        *pp = e->next;
        lr_free_value(ctx, e->heldValue);
        lr_free_value(ctx, e->callback);
        lr_free_value(ctx, e->registry);
        lr_free_value(ctx, e->token);
        free(e);
    }
}

static int lr_token_matches(LRValue a, LRValue b)
{
    if (JS_IsUndefined(a) || JS_IsUndefined(b)) return 0;
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case LR_TYPE_INT32:   return a.u.int32 == b.u.int32;
    case LR_TYPE_FLOAT64: return a.u.float64 == b.u.float64;
    case LR_TYPE_BOOL:    return a.u.bool_val == b.u.bool_val;
    case LR_TYPE_STRING:  return strcmp(((LRString *)a.u.ptr)->str,
                                        ((LRString *)b.u.ptr)->str) == 0;
    case LR_TYPE_OBJECT:  return a.u.ptr == b.u.ptr;
    default:              return 0;
    }
}

static void lr_object_release(LRRuntime *rt, LRObject *obj)
{
    if (!obj) return;
    if (!rt && obj->ctx) rt = obj->ctx->rt;
    if (--obj->ref_count > 0) return;
    /* Already deferred by a prior release (still kept alive by WeakRefs or
     * pending finalization). A deref() may have temporarily revived it; just
     * leave it pending — it will be freed once its last WeakRef is gone. */
    if (obj->finalization_pending) return;
    /* The object is now unreachable via strong references. */
    if (obj->weak_ref_count > 0 || lr_has_finalization_registrations(rt, obj)) {
        /* Defer finalization: keep the object alive (ref_count stays 0) until
         * its last WeakRef and any pending finalization work is gone. */
        obj->ref_count = 0;
        obj->finalization_pending = 1;
        obj->finalize_next = rt->pending_finalize_list;
        rt->pending_finalize_list = obj;
        lr_process_finalization_registrations(rt, obj);
        if (obj->weak_ref_count == 0) {
            lr_free_pending_object(rt, obj);
        }
        return;
    }
    lr_free_object(rt, obj);
}

/* ── Public WeakRef / FinalizationRegistry API ─────────────────────────── */

void lr_weak_ref_retain(LRRuntime *rt, LRObject *target)
{
    (void)rt;
    if (target) target->weak_ref_count++;
}

void lr_weak_ref_release(LRRuntime *rt, LRObject *target)
{
    if (!target) return;
    if (!rt && target->ctx) rt = target->ctx->rt;
    if (target->weak_ref_count > 0) target->weak_ref_count--;
    if (target->weak_ref_count == 0 && target->finalization_pending) {
        lr_free_pending_object(rt, target);
    }
}

LRValue lr_weak_ref_deref(LRContext *ctx, LRObject *target)
{
    if (!target) return LR_VALUE_UNDEFINED;
    /* The target is still physically alive as long as it is kept around by a
     * strong reference or by at least one WeakRef (which implies this WeakRef
     * is still live, so the pointer remains valid). Once both drop to zero the
     * object has been freed and we must report it as collected. */
    if (target->ref_count > 0 || target->weak_ref_count > 0) {
        LRValue v;
        v.tag = LR_TYPE_OBJECT;
        v.u.ptr = target;
        return lr_dup_value(ctx, v);
    }
    return LR_VALUE_UNDEFINED;
}

void lr_register_finalization(LRRuntime *rt, LRObject *target,
                              LRValue callback, LRValue heldValue,
                              LRValue registry, LRValue token)
{
    if (!rt || !target) {
        lr_free_value(NULL, callback);
        lr_free_value(NULL, heldValue);
        lr_free_value(NULL, registry);
        lr_free_value(NULL, token);
        return;
    }
    LRFinalizationEntry *e = (LRFinalizationEntry *)malloc(sizeof(LRFinalizationEntry));
    if (!e) {
        lr_free_value(NULL, callback);
        lr_free_value(NULL, heldValue);
        lr_free_value(NULL, registry);
        lr_free_value(NULL, token);
        return;
    }
    e->target = target;
    e->callback = callback;     /* takes ownership */
    e->heldValue = heldValue;   /* takes ownership */
    e->registry = registry;     /* takes ownership */
    e->token = token;           /* takes ownership */
    e->next = rt->finalization_entries;
    rt->finalization_entries = e;
}

int lr_unregister_finalization(LRRuntime *rt, LRValue token)
{
    int removed = 0;
    if (!rt) return 0;
    LRFinalizationEntry **pp = &rt->finalization_entries;
    while (*pp) {
        LRFinalizationEntry *e = *pp;
        if (lr_token_matches(e->token, token)) {
            *pp = e->next;
            lr_free_value(NULL, e->heldValue);
            lr_free_value(NULL, e->callback);
            lr_free_value(NULL, e->registry);
            lr_free_value(NULL, e->token);
            free(e);
            removed++;
        } else {
            pp = &e->next;
        }
    }
    return removed;
}

static LRObject *lr_object_dup(LRObject *obj)
{
    if (obj) obj->ref_count++;
    return obj;
}

/* ── Value Creation ───────────────────────────────────────────────────── */

LRValue lr_new_bool(LRContext *ctx, int val)
{
    (void)ctx;
    LRValue v;
    v.tag = LR_TYPE_BOOL;
    v.u.bool_val = (uint8_t)(val ? 1 : 0);
    return v;
}

LRValue lr_new_int32(LRContext *ctx, int32_t val)
{
    (void)ctx;
    LRValue v;
    v.tag = LR_TYPE_INT32;
    v.u.int32 = val;
    return v;
}

LRValue lr_new_float64(LRContext *ctx, double val)
{
    (void)ctx;
    LRValue v;
    v.tag = LR_TYPE_FLOAT64;
    v.u.float64 = val;
    return v;
}

LRValue lr_new_string(LRContext *ctx, const char *str)
{
    if (!str) str = "";
    return lr_new_string_len(ctx, str, strlen(str));
}

LRValue lr_new_string_len(LRContext *ctx, const char *str, size_t len)
{
    /* Small string cache: for strings with length <= 32, cache the result
     * to avoid allocating duplicate strings for common short strings.
     * This is a significant optimization for property names, identifiers, etc. */
    if (len > 0 && len <= 32) {
        /* Compute a simple hash for the string */
        unsigned int hash = 5381;
        for (size_t i = 0; i < len; i++) {
            hash = ((hash << 5) + hash) + (unsigned char)str[i];
        }
        unsigned int idx = hash & (LR_SMALL_STRING_CACHE_SIZE - 1);

        /* Check cache */
        LRString *cached = ctx->rt->small_string_cache[idx];
        if (cached && cached->len == len && memcmp(cached->str, str, len) == 0) {
            /* Cache hit: increment reference count and return */
            cached->ref_count++;
            LRValue v;
            v.tag = LR_TYPE_STRING;
            v.u.ptr = cached;
            return v;
        }

        /* Cache miss: create the string */
        LRValue v;
        LRString *s = lr_string_alloc(ctx->rt, str, len);
        if (!s) {
            v.tag = LR_TYPE_EXCEPTION;
            v.u.ptr = NULL;
            return v;
        }

        /* Update cache (replace old entry) */
        ctx->rt->small_string_cache[idx] = s;

        v.tag = LR_TYPE_STRING;
        v.u.ptr = s;
        return v;
    }

    LRValue v;
    LRString *s = lr_string_alloc(ctx->rt, str, len);
    if (!s) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    v.tag = LR_TYPE_STRING;
    v.u.ptr = s;
    return v;
}

/* ── Object / Array Creation ──────────────────────────────────────────── */

LRValue lr_new_object(LRContext *ctx)
{
    LRValue v;
    LRObject *obj = lr_object_alloc(ctx->rt);
    if (!obj) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    obj->ctx = ctx;
    /* Use stored Object.prototype if available, otherwise fall back to global object */
    if (ctx->object_proto.tag == LR_TYPE_OBJECT) {
        obj->proto = lr_dup_value(ctx, ctx->object_proto);
    } else {
        obj->proto = lr_get_global_object(ctx);
    }
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    return v;
}

LRValue lr_new_object_proto(LRContext *ctx, LRValue proto)
{
    LRValue v;
    LRObject *obj = lr_object_alloc(ctx->rt);
    if (!obj) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    obj->ctx = ctx;
    obj->proto = lr_dup_value(ctx, proto);
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    return v;
}

LRValue lr_new_array(LRContext *ctx)
{
    LRValue v;
    LRObject *obj = lr_object_alloc(ctx->rt);
    if (!obj) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    obj->type = LR_OBJ_ARRAY;
    obj->ctx = ctx;
    /* Use stored Array.prototype if available, otherwise fall back to global object */
    if (ctx->array_proto.tag == LR_TYPE_OBJECT) {
        obj->proto = lr_dup_value(ctx, ctx->array_proto);
    } else {
        obj->proto = lr_get_global_object(ctx);
    }
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    return v;
}

/* ── C Function Creation ──────────────────────────────────────────────── */

LRValue lr_new_cfunction(LRContext *ctx, LRCFunctionFunc func,
                         const char *name, int length)
{
    return lr_new_cfunction2(ctx, func, name, length, 0, 0);
}

LRValue lr_new_cfunction2(LRContext *ctx, LRCFunctionFunc func,
                          const char *name, int length,
                          uint8_t cproto, int magic)
{
    (void)cproto;
    LRValue v;
    LRObject *obj = lr_object_alloc(ctx->rt);
    if (!obj) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    obj->type = LR_OBJ_CFUNCTION;
    obj->ctx = ctx;

    LRCFunction *cf = (LRCFunction *)malloc(sizeof(LRCFunction));
    cf->func = func;
    cf->name = name ? strdup(name) : NULL;
    cf->length = length;
    cf->magic = magic;
    cf->data = NULL;
    cf->data_free = NULL;
    obj->extra = cf;

    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;

    /* Set Function.prototype as the prototype for C functions.
     * This ensures fn.call, fn.apply, fn.bind, fn.constructor all work.
     * Note: during initialization ctx->function_proto may not be set yet;
     * those early functions will get their prototype from the first
     * re-creation or from the engine's internal handling. */
    if (ctx->function_proto.tag == LR_TYPE_OBJECT) {
        obj->proto = lr_dup_value(ctx, ctx->function_proto);
    }

    /* Set the "name" property to support fn.name in JS */
    if (name) {
        lr_set_property_str(ctx, v, "name", lr_new_string(ctx, name));
    }

    return v;
}

/* ── Value Management ─────────────────────────────────────────────────── */

LRValue lr_dup_value(LRContext *ctx, LRValue val)
{
    (void)ctx;
    if (val.tag == LR_TYPE_STRING) {
        LRString *s = (LRString *)val.u.ptr;
        lr_string_dup(s);
    } else if (val.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)val.u.ptr;
        lr_object_dup(obj);
    }
    return val;
}

void lr_free_value(LRContext *ctx, LRValue val)
{
    if (val.tag == LR_TYPE_STRING) {
        LRString *s = (LRString *)val.u.ptr;
        if (ctx) {
            lr_string_release(ctx->rt, s);
        } else {
            /* Global free without context */
            if (--s->ref_count <= 0) {
                free(s);
            }
        }
    } else if (val.tag == LR_TYPE_OBJECT) {
        LRObject *obj = (LRObject *)val.u.ptr;
        if (ctx) {
            lr_object_release(ctx->rt, obj);
        } else {
            if (--obj->ref_count <= 0) {
                lr_free_object(NULL, obj);
            }
        }
    }
}

const char *lr_to_cstring(LRContext *ctx, LRValue val)
{
    (void)ctx;
    char buf[128];
    switch (val.tag) {
    case LR_TYPE_UNDEFINED:
        return strdup("undefined");
    case LR_TYPE_NULL:
        return strdup("null");
    case LR_TYPE_BOOL:
        return strdup(val.u.bool_val ? "true" : "false");
    case LR_TYPE_INT32:
        snprintf(buf, sizeof(buf), "%d", val.u.int32);
        return strdup(buf);
    case LR_TYPE_FLOAT64:
        snprintf(buf, sizeof(buf), "%.15g", val.u.float64);
        return strdup(buf);
    case LR_TYPE_STRING: {
        LRString *s = (LRString *)val.u.ptr;
        return strdup(s->str);
    }
    case LR_TYPE_OBJECT: {
        LRObject *obj = (LRObject *)val.u.ptr;
        if (obj->type == LR_OBJ_CFUNCTION) {
            LRCFunction *cf = (LRCFunction *)obj->extra;
            snprintf(buf, sizeof(buf), "function %s() { [native code] }",
                     cf->name ? cf->name : "");
            return strdup(buf);
        }
        /* For arrays, build a string representation */
        if (obj->type == LR_OBJ_ARRAY) {
            /* Build comma-separated string from array elements */
            LRValue len_val = lr_get_property_str(ctx, val, "length");
            int32_t len = 0;
            if (len_val.tag == LR_TYPE_INT32) {
                len = len_val.u.int32;
            }
            lr_free_value(ctx, len_val);

            if (len == 0) {
                return strdup("");
            }

            char *result = NULL;
            size_t result_len = 0;
            for (int32_t i = 0; i < len; i++) {
                if (i > 0) {
                    char *tmp = (char *)realloc(result, result_len + 2);
                    if (!tmp) { free(result); return strdup("[object Object]"); }
                    result = tmp;
                    result[result_len++] = ',';
                    result[result_len] = '\0';
                }
                LRValue elem = lr_get_property_uint32(ctx, val, i);
                const char *elem_str = lr_to_cstring(ctx, elem);
                lr_free_value(ctx, elem);
                if (elem_str) {
                    size_t elem_len = strlen(elem_str);
                    char *tmp = (char *)realloc(result, result_len + elem_len + 1);
                    if (!tmp) { free(result); free((void *)elem_str); return strdup("[object Object]"); }
                    result = tmp;
                    memcpy(result + result_len, elem_str, elem_len);
                    result_len += elem_len;
                    result[result_len] = '\0';
                    free((void *)elem_str);
                } else {
                    /* Insert "undefined" for missing or undefined elements */
                    const char *undef = "undefined";
                    size_t undef_len = strlen(undef);
                    char *tmp = (char *)realloc(result, result_len + undef_len + 1);
                    if (!tmp) { free(result); return strdup("[object Object]"); }
                    result = tmp;
                    memcpy(result + result_len, undef, undef_len);
                    result_len += undef_len;
                    result[result_len] = '\0';
                }
            }
            if (!result) return strdup("");
            return result;
        }
        return strdup("[object Object]");
    }
    case LR_TYPE_SYMBOL:
        return strdup("Symbol()");
    case LR_TYPE_EXCEPTION:
        return strdup("exception");
    default:
        return strdup("");
    }
}

void lr_free_cstring(LRContext *ctx, const char *str)
{
    (void)ctx;
    free((void *)str);
}

/* ── Type Checking ────────────────────────────────────────────────────── */

int lr_is_undefined(LRValue val) { return val.tag == LR_TYPE_UNDEFINED; }
int lr_is_null(LRValue val)      { return val.tag == LR_TYPE_NULL; }
int lr_is_bool(LRValue val)      { return val.tag == LR_TYPE_BOOL; }
int lr_is_number(LRValue val)    { return val.tag == LR_TYPE_INT32 || val.tag == LR_TYPE_FLOAT64; }
int lr_is_string(LRValue val)    { return val.tag == LR_TYPE_STRING; }
int lr_is_object(LRValue val)    { return val.tag == LR_TYPE_OBJECT; }
int lr_is_exception(LRValue val) { return val.tag == LR_TYPE_EXCEPTION; }
int lr_is_symbol(LRValue val)    { return val.tag == LR_TYPE_SYMBOL; }

int lr_is_array(LRContext *ctx, LRValue val)
{
    (void)ctx;
    if (val.tag != LR_TYPE_OBJECT) return 0;
    LRObject *obj = (LRObject *)val.u.ptr;
    return obj->type == LR_OBJ_ARRAY;
}

int lr_is_function(LRContext *ctx, LRValue val)
{
    (void)ctx;
    if (val.tag != LR_TYPE_OBJECT) return 0;
    LRObject *obj = (LRObject *)val.u.ptr;
    if (obj->type == LR_OBJ_CFUNCTION || obj->type == LR_OBJ_FUNCTION ||
        obj->type == LR_OBJ_BYTECODE_FUNC) return 1;
    /* Proxy wrapping a function is callable */
    if (obj->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)obj->extra;
        if (pd && pd->target.tag == LR_TYPE_OBJECT) {
            return lr_is_function(ctx, pd->target);
        }
    }
    return 0;
}

int lr_is_array_buffer(LRContext *ctx, LRValue val)
{
    (void)ctx;
    if (val.tag != LR_TYPE_OBJECT) return 0;
    LRObject *obj = (LRObject *)val.u.ptr;
    return obj->type == LR_OBJ_ARRAY_BUFFER;
}

int lr_is_promise(LRContext *ctx, LRValue val)
{
    (void)ctx;
    if (val.tag != LR_TYPE_OBJECT) return 0;
    LRObject *obj = (LRObject *)val.u.ptr;
    return obj->type == LR_OBJ_PROMISE;
}

/* ── Type Conversion ──────────────────────────────────────────────────── */

int lr_to_bool(LRContext *ctx, LRValue val)
{
    (void)ctx;
    switch (val.tag) {
    case LR_TYPE_UNDEFINED: return 0;
    case LR_TYPE_NULL:      return 0;
    case LR_TYPE_BOOL:      return val.u.bool_val;
    case LR_TYPE_INT32:     return val.u.int32 != 0;
    case LR_TYPE_FLOAT64:   return val.u.float64 != 0.0 && !isnan(val.u.float64);
    case LR_TYPE_STRING:    return 1; /* non-empty string is truthy */
    case LR_TYPE_OBJECT:    return 1;
    case LR_TYPE_SYMBOL:    return 1;
    default:                return 0;
    }
}

int lr_to_int32(LRContext *ctx, int32_t *pres, LRValue val)
{
    (void)ctx;
    switch (val.tag) {
    case LR_TYPE_UNDEFINED: *pres = 0; return 0;
    case LR_TYPE_NULL:      *pres = 0; return 0;
    case LR_TYPE_BOOL:      *pres = val.u.bool_val ? 1 : 0; return 0;
    case LR_TYPE_INT32:     *pres = val.u.int32; return 0;
    case LR_TYPE_FLOAT64:   *pres = (int32_t)val.u.float64; return 0;
    case LR_TYPE_STRING: {
        LRString *s = (LRString *)val.u.ptr;
        *pres = (int32_t)strtol(s->str, NULL, 0);
        return 0;
    }
    default: *pres = 0; return -1;
    }
}

int lr_to_int64(LRContext *ctx, int64_t *pres, LRValue val)
{
    (void)ctx;
    switch (val.tag) {
    case LR_TYPE_UNDEFINED: *pres = 0; return 0;
    case LR_TYPE_NULL:      *pres = 0; return 0;
    case LR_TYPE_BOOL:      *pres = val.u.bool_val ? 1 : 0; return 0;
    case LR_TYPE_INT32:     *pres = (int64_t)val.u.int32; return 0;
    case LR_TYPE_FLOAT64:   *pres = (int64_t)val.u.float64; return 0;
    case LR_TYPE_STRING: {
        LRString *s = (LRString *)val.u.ptr;
        *pres = (int64_t)strtoll(s->str, NULL, 0);
        return 0;
    }
    default: *pres = 0; return -1;
    }
}

int lr_to_float64(LRContext *ctx, double *pres, LRValue val)
{
    (void)ctx;
    switch (val.tag) {
    case LR_TYPE_UNDEFINED: *pres = NAN; return 0;
    case LR_TYPE_NULL:      *pres = 0.0; return 0;
    case LR_TYPE_BOOL:      *pres = val.u.bool_val ? 1.0 : 0.0; return 0;
    case LR_TYPE_INT32:     *pres = (double)val.u.int32; return 0;
    case LR_TYPE_FLOAT64:   *pres = val.u.float64; return 0;
    case LR_TYPE_STRING: {
        LRString *s = (LRString *)val.u.ptr;
        char *end;
        *pres = strtod(s->str, &end);
        return 0;
    }
    default: *pres = NAN; return -1;
    }
}

/* ── Shape Cache for Property Access ────────────────────────────────────── */

/* Lookup shape cache for a (object, property) pair.
 * Returns the cached property value pointer, or NULL if not in cache. */
static LRValue *shape_cache_lookup(LRContext *ctx, LRObject *obj, LRString *atom)
{
    LRRuntime *rt = ctx->rt;
    unsigned int idx = ((uintptr_t)obj ^ (uintptr_t)atom) & (LR_SHAPE_CACHE_SIZE - 1);
    LRShapeCacheEntry *entry = &rt->shape_cache[idx];
    if (entry->valid && entry->obj == obj && entry->prop == atom) {
        if (entry->offset >= 0 && entry->offset < 1000) {
            /* Fast path: attempt direct access by offset through the hash chain */
            /* Since we can't guarantee a fixed offset, just return the cached value */
            /* We use the hash chain directly but with known key */
            LRProperty *prop = obj->prop_hash;
            while (prop) {
                if (prop->key == atom) {
                    /* Accessor properties must go through the slow path so
                     * the getter is actually invoked. */
                    if (prop->flags & (LR_PROP_GETTER | LR_PROP_SETTER))
                        return NULL;
                    return &prop->value;
                }
                prop = prop->next;
            }
        }
    }
    return NULL;
}

/* Update shape cache for a (object, property) pair */
static void shape_cache_update(LRContext *ctx, LRObject *obj, LRString *atom, int offset)
{
    LRRuntime *rt = ctx->rt;
    unsigned int idx = ((uintptr_t)obj ^ (uintptr_t)atom) & (LR_SHAPE_CACHE_SIZE - 1);
    LRShapeCacheEntry *entry = &rt->shape_cache[idx];
    entry->obj = obj;
    entry->prop = atom;
    entry->offset = offset;
    entry->valid = 1;
}

/* ── Property Access ──────────────────────────────────────────────────── */

/* Get property from object's own properties (not prototype chain) */
static LRProperty *lr_object_find_own_prop(LRObject *obj, LRString *key)
{
    if (!obj->prop_hash) return NULL;
    /* Simple linear search through hash chain */
    LRProperty *prop = obj->prop_hash;
    while (prop) {
        if (prop->key == key || (prop->key && key &&
            prop->key->len == key->len &&
            memcmp(prop->key->str, key->str, key->len) == 0)) {
            return prop;
        }
        prop = prop->next;
    }
    return NULL;
}

/* Invoke a property's getter/setter semantics.
 * For an accessor property, call the getter (if any) with `receiver` as this.
 * For a normal property, return the stored value. */
static LRValue lr_property_get(LRContext *ctx, LRValue receiver, LRProperty *prop)
{
    if (prop->flags & LR_PROP_GETTER) {
        if (prop->value.tag == LR_TYPE_OBJECT) {
            LRValue result = lr_call_direct(ctx, prop->value, receiver, 0, NULL);
            return result;
        }
        return LR_VALUE_UNDEFINED;
    }
    return lr_dup_value(ctx, prop->value);
}

/* Set an accessor (getter/setter) property on an object.
 * getter/setter are function values (or LR_VALUE_UNDEFINED). */
static int lr_set_accessor_property(LRContext *ctx, LRValue obj, LRString *atom,
                                    LRValue getter, LRValue setter)
{
    if (obj.tag != LR_TYPE_OBJECT) {
        lr_free_value(ctx, getter);
        lr_free_value(ctx, setter);
        return -1;
    }
    LRObject *o = (LRObject *)obj.u.ptr;

    uint8_t flags = LR_PROP_ENUMERABLE | LR_PROP_CONFIGURABLE;
    if (getter.tag == LR_TYPE_OBJECT) flags |= LR_PROP_GETTER;
    if (setter.tag == LR_TYPE_OBJECT) flags |= LR_PROP_SETTER;

    LRProperty *prop = o->prop_hash;
    while (prop) {
        if (prop->key == atom || (prop->key && atom &&
            prop->key->len == atom->len &&
            memcmp(prop->key->str, atom->str, atom->len) == 0)) {
            lr_free_value(ctx, prop->value);
            lr_free_value(ctx, prop->setter);
            prop->value = getter;
            prop->setter = setter;
            prop->flags = flags;
            lr_string_dup(atom);
            shape_cache_update(ctx, o, atom, 1);
            return 0;
        }
        prop = prop->next;
    }

    LRProperty *new_prop = (LRProperty *)calloc(1, sizeof(LRProperty));
    new_prop->key = lr_string_dup(atom);
    new_prop->value = getter;
    new_prop->setter = setter;
    new_prop->flags = flags;
    new_prop->next = o->prop_hash;
    o->prop_hash = new_prop;
    shape_cache_update(ctx, o, atom, 1);
    ctx->rt->prop_count++;
    ctx->rt->prop_size += sizeof(LRProperty);
    return 0;
}

static int lr_set_accessor_property_str(LRContext *ctx, LRValue obj,
                                        const char *name, LRValue getter, LRValue setter)
{
    LRString *atom = lr_new_atom(ctx, name);
    return lr_set_accessor_property(ctx, obj, atom, getter, setter);
}

/* Helper: invoke a property's setter if it is an accessor.
 * Returns 1 if handled (setter invoked or no-op for read-only accessor),
 * 0 if the property is a normal data property (caller should store value). */
static int lr_property_try_set_accessor(LRContext *ctx, LRValue receiver,
                                        LRProperty *prop, LRValue val)
{
    if (!(prop->flags & (LR_PROP_GETTER | LR_PROP_SETTER)))
        return 0;
    if (prop->flags & LR_PROP_SETTER) {
        if (prop->setter.tag == LR_TYPE_OBJECT) {
            LRValue arg = lr_dup_value(ctx, val);
            LRValue args[1] = { arg };
            LRValue result = lr_call_direct(ctx, prop->setter, receiver, 1, args);
            lr_free_value(ctx, result);
            lr_free_value(ctx, arg);
        }
    }
    lr_free_value(ctx, val);
    return 1;
}

LRValue lr_get_property(LRContext *ctx, LRValue obj, LRString *atom)
{
    if (obj.tag != LR_TYPE_OBJECT) {
        return LR_VALUE_UNDEFINED;
    }
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check if it's a Proxy */
    if (o->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)o->extra;
        if (!pd) return LR_VALUE_UNDEFINED;
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has a "get" trap */
        LRValue get_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "get"));
        if (lr_is_function(ctx, get_trap)) {
            LRValue args[3];
            args[0] = lr_dup_value(ctx, target);
            args[1] = lr_atom_to_value(ctx, atom);
            args[2] = lr_dup_value(ctx, obj); /* receiver is the proxy itself */
            LRValue result = lr_call_direct(ctx, get_trap, handler, 3, args);
            lr_free_value(ctx, get_trap);
            lr_free_value(ctx, args[0]);
            lr_free_value(ctx, args[1]);
            lr_free_value(ctx, args[2]);
            return result;
        }
        lr_free_value(ctx, get_trap);
        /* No "get" trap - forward to target */
        return lr_get_property_direct(ctx, target, atom);
    }

    /* Handle TypedArray numeric index access */
    if (o->type == LR_OBJ_TYPED_ARRAY && o->opaque) {
        uint32_t idx;
        if (lr_is_numeric_index(atom, &idx)) {
            TypedArrayData *tad = (TypedArrayData *)o->opaque;
            size_t len = tad->byte_length / tad->element_size;
            if (idx < len) {
                return typed_array_get_elem(ctx, tad, (size_t)idx);
            }
            return LR_VALUE_UNDEFINED;
        }
    }

    /* Shape cache: fast lookup for (obj, atom) pairs we've seen before */
    LRValue *cached = shape_cache_lookup(ctx, o, atom);
    if (cached) {
        return lr_dup_value(ctx, *cached);
    }

    /* Check own properties */
    LRProperty *found = lr_object_find_own_prop(o, atom);
    if (found) {
        /* Update shape cache */
        shape_cache_update(ctx, o, atom, 1);
        return lr_property_get(ctx, obj, found);
    }

    /* Walk prototype chain */
    LRValue proto = o->proto;
    while (proto.tag == LR_TYPE_OBJECT) {
        LRObject *po = (LRObject *)proto.u.ptr;
        if (po->type == LR_OBJ_PROXY) {
            /* Proxy in prototype chain - use Proxy-aware get */
            return lr_get_property(ctx, proto, atom);
        }
        found = lr_object_find_own_prop(po, atom);
        if (found) {
            return lr_property_get(ctx, obj, found);
        }
        proto = po->proto;
    }

    return LR_VALUE_UNDEFINED;
}

/* Direct property access - bypasses Proxy traps, does ordinary [[Get]] */
LRValue lr_get_property_direct(LRContext *ctx, LRValue obj, LRString *atom)
{
    if (obj.tag != LR_TYPE_OBJECT) {
        return LR_VALUE_UNDEFINED;
    }
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check own properties */
    LRProperty *found = lr_object_find_own_prop(o, atom);
    if (found) {
        return lr_property_get(ctx, obj, found);
    }

    /* Walk prototype chain */
    LRValue proto = o->proto;
    while (proto.tag == LR_TYPE_OBJECT) {
        LRObject *po = (LRObject *)proto.u.ptr;
        found = lr_object_find_own_prop(po, atom);
        if (found) {
            return lr_property_get(ctx, obj, found);
        }
        proto = po->proto;
    }

    return LR_VALUE_UNDEFINED;
}

LRValue lr_get_property_str(LRContext *ctx, LRValue obj, const char *name)
{
    LRString *atom = lr_new_atom(ctx, name);
    LRValue result = lr_get_property(ctx, obj, atom);
    return result;
}

LRValue lr_get_property_uint32(LRContext *ctx, LRValue obj, uint32_t idx)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", idx);
    return lr_get_property_str(ctx, obj, buf);
}

int lr_set_property(LRContext *ctx, LRValue obj, LRString *atom, LRValue val)
{
    if (obj.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check if it's a Proxy */
    if (o->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)o->extra;
        if (!pd) return -1;
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has a "set" trap */
        LRValue set_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "set"));
        if (lr_is_function(ctx, set_trap)) {
            LRValue args[4];
            args[0] = lr_dup_value(ctx, target);
            args[1] = lr_atom_to_value(ctx, atom);
            args[2] = lr_dup_value(ctx, val);
            args[3] = lr_dup_value(ctx, obj); /* receiver is the proxy itself */
            LRValue result = lr_call_direct(ctx, set_trap, handler, 4, args);
            lr_free_value(ctx, set_trap);
            lr_free_value(ctx, args[0]);
            lr_free_value(ctx, args[1]);
            lr_free_value(ctx, args[2]);
            lr_free_value(ctx, args[3]);
            if (lr_is_exception(result)) {
                lr_free_value(ctx, result);
                return -1;
            }
            int success = lr_to_bool(ctx, result);
            lr_free_value(ctx, result);
            return success ? 0 : -1;
        }
        lr_free_value(ctx, set_trap);
        /* No "set" trap - forward to target */
        return lr_set_property_direct(ctx, target, atom, lr_dup_value(ctx, val));
    }

    /* Handle TypedArray numeric index set */
    if (o->type == LR_OBJ_TYPED_ARRAY && o->opaque) {
        uint32_t idx;
        if (lr_is_numeric_index(atom, &idx)) {
            TypedArrayData *tad = (TypedArrayData *)o->opaque;
            size_t len = tad->byte_length / tad->element_size;
            if (idx < len) {
                typed_array_set_elem(ctx, tad, (size_t)idx, val);
                lr_free_value(ctx, val); /* val consumed by setter */
                return 0;
            }
            /* Out of bounds - ignore (per spec, silently ignore) */
            lr_free_value(ctx, val);
            return 0;
        }
    }

    /* Shape cache lookup: check if we've seen this (obj, atom) pair before */
    {
        LRRuntime *rt = ctx->rt;
        unsigned int idx = ((uintptr_t)o ^ (uintptr_t)atom) & (LR_SHAPE_CACHE_SIZE - 1);
        LRShapeCacheEntry *entry = &rt->shape_cache[idx];
        if (entry->valid && entry->obj == o && entry->prop == atom) {
            /* Cache hit: directly update the known property */
            LRProperty *prop = o->prop_hash;
            while (prop) {
                if (prop->key == atom) {
                    if (lr_property_try_set_accessor(ctx, obj, prop, val))
                        return 0;
                    lr_free_value(ctx, prop->value);
                    prop->value = val;
                    lr_string_dup(atom);
                    return 0;
                }
                prop = prop->next;
            }
        }
    }

    /* Check if property already exists */
    LRProperty *prop = o->prop_hash;
    while (prop) {
        if (prop->key == atom || (prop->key && atom &&
            prop->key->len == atom->len &&
            memcmp(prop->key->str, atom->str, atom->len) == 0)) {
            /* Accessor property: invoke setter */
            if (lr_property_try_set_accessor(ctx, obj, prop, val))
                return 0;
            /* Property exists - update value */
            lr_free_value(ctx, prop->value);
            prop->value = val; /* Takes ownership of val */
            lr_string_dup(atom); /* Keep atom alive */
            /* Update shape cache */
            shape_cache_update(ctx, o, atom, 1);
            return 0;
        }
        prop = prop->next;
    }

    /* Spec [[Set]]: no own property - walk the prototype chain looking for
     * an ACCESSOR property (e.g. a setter defined on a class prototype).
     * If found, invoke the setter with the original receiver. Data
     * properties on the prototype do NOT block shadowing. */
    {
        LRValue proto_v = o->proto;
        while (proto_v.tag == LR_TYPE_OBJECT) {
            LRObject *po = (LRObject *)proto_v.u.ptr;
            LRProperty *pp = lr_object_find_own_prop(po, atom);
            if (pp) {
                if (pp->flags & (LR_PROP_GETTER | LR_PROP_SETTER)) {
                    if (lr_property_try_set_accessor(ctx, obj, pp, val))
                        return 0;
                }
                break; /* data property on proto: shadow with own prop */
            }
            proto_v = po->proto;
        }
    }

    /* Create new property */
    LRProperty *new_prop = (LRProperty *)calloc(1, sizeof(LRProperty));
    new_prop->key = lr_string_dup(atom);
    new_prop->value = val; /* Takes ownership */
    new_prop->flags = LR_PROP_NORMAL | LR_PROP_ENUMERABLE | LR_PROP_WRITABLE | LR_PROP_CONFIGURABLE;
    new_prop->next = o->prop_hash;
    o->prop_hash = new_prop;

    /* Update shape cache */
    shape_cache_update(ctx, o, atom, 1);

    ctx->rt->prop_count++;
    ctx->rt->prop_size += sizeof(LRProperty);
    return 0;
}

/* Direct property set - bypasses Proxy traps, but still honours
 * accessor (getter/setter) semantics. */
int lr_set_property_direct(LRContext *ctx, LRValue obj, LRString *atom, LRValue val)
{
    if (obj.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check if property already exists */
    LRProperty *prop = o->prop_hash;
    while (prop) {
        if (prop->key == atom || (prop->key && atom &&
            prop->key->len == atom->len &&
            memcmp(prop->key->str, atom->str, atom->len) == 0)) {
            /* Accessor property: invoke setter */
            if (lr_property_try_set_accessor(ctx, obj, prop, val))
                return 0;
            /* Property exists - update value */
            lr_free_value(ctx, prop->value);
            prop->value = val; /* Takes ownership of val */
            lr_string_dup(atom); /* Keep atom alive */
            return 0;
        }
        prop = prop->next;
    }

    /* Walk prototype chain for an accessor property (setter) */
    {
        LRValue proto_v = o->proto;
        while (proto_v.tag == LR_TYPE_OBJECT) {
            LRObject *po = (LRObject *)proto_v.u.ptr;
            LRProperty *pp = lr_object_find_own_prop(po, atom);
            if (pp) {
                if (pp->flags & (LR_PROP_GETTER | LR_PROP_SETTER)) {
                    if (lr_property_try_set_accessor(ctx, obj, pp, val))
                        return 0;
                }
                break;
            }
            proto_v = po->proto;
        }
    }

    /* Create new property */
    LRProperty *new_prop = (LRProperty *)calloc(1, sizeof(LRProperty));
    new_prop->key = lr_string_dup(atom);
    new_prop->value = val; /* Takes ownership */
    new_prop->flags = LR_PROP_NORMAL | LR_PROP_ENUMERABLE | LR_PROP_WRITABLE | LR_PROP_CONFIGURABLE;
    new_prop->next = o->prop_hash;
    o->prop_hash = new_prop;

    ctx->rt->prop_count++;
    ctx->rt->prop_size += sizeof(LRProperty);
    return 0;
}

int lr_set_property_str(LRContext *ctx, LRValue obj, const char *name, LRValue val)
{
    LRString *atom = lr_new_atom(ctx, name);
    return lr_set_property(ctx, obj, atom, val);
}

int lr_set_property_uint32(LRContext *ctx, LRValue obj, uint32_t idx, LRValue val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", idx);
    return lr_set_property_str(ctx, obj, buf, val);
}

int lr_delete_property(LRContext *ctx, LRValue obj, LRString *atom, int flags)
{
    (void)flags;
    if (obj.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check if it's a Proxy */
    if (o->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)o->extra;
        if (!pd) return -1;
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has a "deleteProperty" trap */
        LRValue del_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "deleteProperty"));
        if (lr_is_function(ctx, del_trap)) {
            LRValue args[2];
            args[0] = lr_dup_value(ctx, target);
            args[1] = lr_atom_to_value(ctx, atom);
            LRValue result = lr_call_direct(ctx, del_trap, handler, 2, args);
            lr_free_value(ctx, del_trap);
            lr_free_value(ctx, args[0]);
            lr_free_value(ctx, args[1]);
            if (lr_is_exception(result)) {
                lr_free_value(ctx, result);
                return -1;
            }
            int success = lr_to_bool(ctx, result);
            lr_free_value(ctx, result);
            return success ? 0 : -1;
        }
        lr_free_value(ctx, del_trap);
        /* No "deleteProperty" trap - forward to target */
        return lr_delete_property_direct(ctx, target, atom, flags);
    }

    LRProperty **prev = &o->prop_hash;
    LRProperty *prop = o->prop_hash;
    while (prop) {
        if (prop->key == atom || (prop->key && atom &&
            prop->key->len == atom->len &&
            memcmp(prop->key->str, atom->str, atom->len) == 0)) {
            *prev = prop->next;
            lr_string_release(ctx->rt, prop->key);
            lr_free_value(ctx, prop->value);
            ctx->rt->prop_count--;
            ctx->rt->prop_size -= sizeof(LRProperty);
            free(prop);
            return 0;
        }
        prev = &prop->next;
        prop = prop->next;
    }
    return -1;
}

/* Direct property delete - bypasses Proxy traps */
int lr_delete_property_direct(LRContext *ctx, LRValue obj, LRString *atom, int flags)
{
    (void)flags;
    if (obj.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)obj.u.ptr;

    LRProperty **prev = &o->prop_hash;
    LRProperty *prop = o->prop_hash;
    while (prop) {
        if (prop->key == atom || (prop->key && atom &&
            prop->key->len == atom->len &&
            memcmp(prop->key->str, atom->str, atom->len) == 0)) {
            *prev = prop->next;
            lr_string_release(ctx->rt, prop->key);
            lr_free_value(ctx, prop->value);
            ctx->rt->prop_count--;
            ctx->rt->prop_size -= sizeof(LRProperty);
            free(prop);
            return 0;
        }
        prev = &prop->next;
        prop = prop->next;
    }
    return -1;
}

int lr_has_property(LRContext *ctx, LRValue obj, LRString *atom)
{
    if (obj.tag != LR_TYPE_OBJECT) return 0;
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check if it's a Proxy */
    if (o->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)o->extra;
        if (!pd) return 0;
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has a "has" trap */
        LRValue has_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "has"));
        if (lr_is_function(ctx, has_trap)) {
            LRValue args[2];
            args[0] = lr_dup_value(ctx, target);
            args[1] = lr_atom_to_value(ctx, atom);
            LRValue result = lr_call_direct(ctx, has_trap, handler, 2, args);
            lr_free_value(ctx, has_trap);
            lr_free_value(ctx, args[0]);
            lr_free_value(ctx, args[1]);
            if (lr_is_exception(result)) {
                lr_free_value(ctx, result);
                return 0;
            }
            int bool_result = lr_to_bool(ctx, result);
            lr_free_value(ctx, result);
            return bool_result;
        }
        lr_free_value(ctx, has_trap);
        /* No "has" trap - forward to target */
        return lr_has_property_direct(ctx, target, atom);
    }

    /* Check own properties */
    LRValue *found = lr_object_find_own_prop(o, atom);
    if (found) return 1;

    /* Walk prototype chain */
    LRValue proto = o->proto;
    while (proto.tag == LR_TYPE_OBJECT) {
        LRObject *po = (LRObject *)proto.u.ptr;
        if (po->type == LR_OBJ_PROXY) {
            /* Proxy in prototype chain - check with Proxy-aware has */
            return lr_has_property(ctx, proto, atom);
        }
        found = lr_object_find_own_prop(po, atom);
        if (found) return 1;
        proto = po->proto;
    }

    return 0;
}

/* Direct has property - bypasses Proxy traps */
int lr_has_property_direct(LRContext *ctx, LRValue obj, LRString *atom)
{
    if (obj.tag != LR_TYPE_OBJECT) return 0;
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check own properties */
    LRValue *found = lr_object_find_own_prop(o, atom);
    if (found) return 1;

    /* Walk prototype chain */
    LRValue proto = o->proto;
    while (proto.tag == LR_TYPE_OBJECT) {
        LRObject *po = (LRObject *)proto.u.ptr;
        found = lr_object_find_own_prop(po, atom);
        if (found) return 1;
        proto = po->proto;
    }

    return 0;
}

int lr_define_property_value(LRContext *ctx, LRValue obj, LRString *atom,
                              LRValue val, int flags)
{
    return lr_set_property(ctx, obj, atom, val);
}

int lr_get_own_property_names(LRContext *ctx, LRPropertyEnum **ptab,
                               uint32_t *plen, LRValue obj, int flags)
{
    (void)flags;
    if (obj.tag != LR_TYPE_OBJECT) {
        *ptab = NULL;
        *plen = 0;
        return 0;
    }
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Check if it's a Proxy */
    if (o->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)o->extra;
        if (!pd) {
            *ptab = NULL;
            *plen = 0;
            return 0;
        }
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has an "ownKeys" trap */
        LRValue ownkeys_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "ownKeys"));
        if (lr_is_function(ctx, ownkeys_trap)) {
            LRValue args[1];
            args[0] = lr_dup_value(ctx, target);
            LRValue result = lr_call_direct(ctx, ownkeys_trap, handler, 1, args);
            lr_free_value(ctx, ownkeys_trap);
            lr_free_value(ctx, args[0]);
            if (lr_is_exception(result)) {
                lr_free_value(ctx, result);
                *ptab = NULL;
                *plen = 0;
                return -1;
            }
            /* Convert result array to property enum array */
            if (lr_is_array(ctx, result)) {
                LRValue len_val = lr_get_property_direct(ctx, result,
                    lr_new_atom(ctx, "length"));
                int32_t len = 0;
                lr_to_int32(ctx, &len, len_val);
                lr_free_value(ctx, len_val);
                *plen = (uint32_t)len;
                if (len == 0) {
                    *ptab = NULL;
                    lr_free_value(ctx, result);
                    return 0;
                }
                *ptab = (LRPropertyEnum *)malloc(len * sizeof(LRPropertyEnum));
                for (int32_t i = 0; i < len; i++) {
                    LRValue key = lr_get_property_uint32(ctx, result, i);
                    LRString *atom = lr_to_atom(ctx, key);
                    (*ptab)[i].atom = lr_string_dup(atom);
                    (*ptab)[i].flags = LR_PROP_ENUMERABLE | LR_PROP_CONFIGURABLE | LR_PROP_WRITABLE;
                    lr_free_value(ctx, key);
                }
                lr_free_value(ctx, result);
                return 0;
            }
            lr_free_value(ctx, result);
            *ptab = NULL;
            *plen = 0;
            return -1;
        }
        lr_free_value(ctx, ownkeys_trap);
        /* No "ownKeys" trap - forward to target */
        return lr_get_own_property_names_direct(ctx, ptab, plen, target, flags);
    }

    /* Count properties from prop_hash */
    uint32_t count = 0;
    LRProperty *prop = o->prop_hash;
    while (prop) { count++; prop = prop->next; }

    uint32_t total = count;
    *plen = total;
    if (total == 0) {
        *ptab = NULL;
        return 0;
    }

    *ptab = (LRPropertyEnum *)malloc(total * sizeof(LRPropertyEnum));
    uint32_t i = 0;
    /* Copy prop_hash entries */
    prop = o->prop_hash;
    while (prop) {
        (*ptab)[i].atom = lr_string_dup(prop->key);
        (*ptab)[i].flags = prop->flags;
        i++;
        prop = prop->next;
    }
    return 0;
}

/* Direct get own property names - bypasses Proxy traps */
int lr_get_own_property_names_direct(LRContext *ctx, LRPropertyEnum **ptab,
                                      uint32_t *plen, LRValue obj, int flags)
{
    (void)flags;
    if (obj.tag != LR_TYPE_OBJECT) {
        *ptab = NULL;
        *plen = 0;
        return 0;
    }
    LRObject *o = (LRObject *)obj.u.ptr;

    uint32_t count = 0;
    LRProperty *prop = o->prop_hash;
    while (prop) { count++; prop = prop->next; }

    *plen = count;
    if (count == 0) {
        *ptab = NULL;
        return 0;
    }

    *ptab = (LRPropertyEnum *)malloc(count * sizeof(LRPropertyEnum));
    uint32_t i = 0;
    prop = o->prop_hash;
    while (prop) {
        (*ptab)[i].atom = lr_string_dup(prop->key);
        (*ptab)[i].flags = prop->flags;
        i++;
        prop = prop->next;
    }
    return 0;
}

void lr_free_property_enum(LRContext *ctx, LRPropertyEnum *tab, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        lr_string_release(ctx->rt, tab[i].atom);
    }
    free(tab);
}

/* ── Function Calls ───────────────────────────────────────────────────── */

LRValue lr_call(LRContext *ctx, LRValue func, LRValue this_val,
                int argc, LRValue *argv)
{
    if (func.tag != LR_TYPE_OBJECT) {
        return LR_VALUE_EXCEPTION;
    }
    LRObject *obj = (LRObject *)func.u.ptr;

    /* Fast path: C function call (most common case, no proxy) */
    if (obj->type == LR_OBJ_CFUNCTION) {
        LRCFunction *cf = (LRCFunction *)obj->extra;
        if (cf && cf->func) {
            ctx->current_func = func;
            LRValue result = cf->func(ctx, this_val, argc, argv);
            ctx->current_func = LR_VALUE_UNDEFINED;
            return result;
        }
    }

    /* Check if it's a Proxy (function call trap) */
    if (obj->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)obj->extra;
        if (!pd) return LR_VALUE_EXCEPTION;
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has an "apply" trap */
        LRValue apply_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "apply"));
        if (lr_is_function(ctx, apply_trap)) {
            /* Create args array for the trap */
            LRValue args_arr = lr_new_array(ctx);
            for (int i = 0; i < argc; i++) {
                lr_set_property_uint32(ctx, args_arr, i, lr_dup_value(ctx, argv[i]));
            }
            lr_set_property_str(ctx, args_arr, "length", lr_new_int32(ctx, argc));

            LRValue trap_args[3];
            trap_args[0] = lr_dup_value(ctx, target);
            trap_args[1] = lr_dup_value(ctx, this_val);
            trap_args[2] = args_arr;
            LRValue result = lr_call_direct(ctx, apply_trap, handler, 3, trap_args);
            lr_free_value(ctx, apply_trap);
            lr_free_value(ctx, trap_args[0]);
            lr_free_value(ctx, trap_args[1]);
            lr_free_value(ctx, trap_args[2]);
            return result;
        }
        lr_free_value(ctx, apply_trap);
        /* No "apply" trap - forward to target */
        return lr_call_direct(ctx, target, this_val, argc, argv);
    }

    /* Call JS interpreter functions via callback (set by interpreter) */
    if (ctx->call_js_function && (obj->type == LR_OBJ_FUNCTION ||
                                  obj->type == LR_OBJ_BYTECODE_FUNC)) {
        return ctx->call_js_function(ctx, func, this_val, argc, argv);
    }

    /* TODO: bytecode function calls */
    return LR_VALUE_UNDEFINED;
}

/* Direct call - bypasses Proxy traps */
LRValue lr_call_direct(LRContext *ctx, LRValue func, LRValue this_val,
                       int argc, LRValue *argv)
{
    if (func.tag != LR_TYPE_OBJECT) {
        return LR_VALUE_EXCEPTION;
    }
    LRObject *obj = (LRObject *)func.u.ptr;
    if (obj->type == LR_OBJ_CFUNCTION) {
        LRCFunction *cf = (LRCFunction *)obj->extra;
        if (cf && cf->func) {
            ctx->current_func = func;
            LRValue result = cf->func(ctx, this_val, argc, argv);
            ctx->current_func = LR_VALUE_UNDEFINED;
            return result;
        }
    }
    /* Call JS interpreter functions via callback (set by interpreter) */
    if (ctx->call_js_function && (obj->type == LR_OBJ_FUNCTION ||
                                  obj->type == LR_OBJ_BYTECODE_FUNC)) {
        return ctx->call_js_function(ctx, func, this_val, argc, argv);
    }
    /* TODO: bytecode function calls */
    return LR_VALUE_UNDEFINED;
}

LRValue lr_call_constructor(LRContext *ctx, LRValue func,
                            int argc, LRValue *argv)
{
    if (func.tag != LR_TYPE_OBJECT) {
        return LR_VALUE_EXCEPTION;
    }
    LRObject *obj = (LRObject *)func.u.ptr;

    /* Check if it's a Proxy (construct trap) */
    if (obj->type == LR_OBJ_PROXY) {
        LRProxyData *pd = (LRProxyData *)obj->extra;
        if (!pd) return LR_VALUE_EXCEPTION;
        LRValue handler = pd->handler;
        LRValue target = pd->target;
        /* Check if handler has a "construct" trap */
        LRValue construct_trap = lr_get_property_direct(ctx, handler,
            lr_new_atom(ctx, "construct"));
        if (lr_is_function(ctx, construct_trap)) {
            /* Create args array for the trap */
            LRValue args_arr = lr_new_array(ctx);
            for (int i = 0; i < argc; i++) {
                lr_set_property_uint32(ctx, args_arr, i, lr_dup_value(ctx, argv[i]));
            }
            lr_set_property_str(ctx, args_arr, "length", lr_new_int32(ctx, argc));

            LRValue trap_args[3];
            trap_args[0] = lr_dup_value(ctx, target);
            trap_args[1] = args_arr;
            trap_args[2] = lr_dup_value(ctx, func); /* newTarget */
            LRValue result = lr_call_direct(ctx, construct_trap, handler, 3, trap_args);
            lr_free_value(ctx, construct_trap);
            lr_free_value(ctx, trap_args[0]);
            lr_free_value(ctx, trap_args[1]);
            lr_free_value(ctx, trap_args[2]);
            return result;
        }
        lr_free_value(ctx, construct_trap);
        /* No "construct" trap - forward to target */
        return lr_call_constructor_direct(ctx, target, argc, argv);
    }

    if (obj->type == LR_OBJ_CFUNCTION) {
        LRCFunction *cf = (LRCFunction *)obj->extra;
        if (cf && cf->func) {
            /* Create a new object with the constructor's prototype.
             * This matches the JavaScript spec: OrdinaryCreateFromConstructor. */
            LRValue new_obj = lr_new_object(ctx);
            LRValue proto_val = lr_get_property_str(ctx, func, "prototype");
            if (lr_is_object(proto_val)) {
                LRObject *new_obj_ptr = (LRObject *)new_obj.u.ptr;
                lr_free_value(ctx, new_obj_ptr->proto);
                new_obj_ptr->proto = lr_dup_value(ctx, proto_val);
            }
            lr_free_value(ctx, proto_val);

            ctx->current_func = func;
            LRValue result = cf->func(ctx, new_obj, argc, argv);
            ctx->current_func = LR_VALUE_UNDEFINED;

            /* If the constructor returns an object, return that (unless it's
             * the same object as new_obj, to avoid double-free);
             * otherwise return the new object (per JS spec). */
            if (lr_is_object(result)) {
                if (result.u.ptr != new_obj.u.ptr) {
                    lr_free_value(ctx, new_obj);
                }
                return result;
            }
            lr_free_value(ctx, result);
            return new_obj;
        }
    }
    return LR_VALUE_UNDEFINED;
}

/* Direct constructor call */
LRValue lr_call_constructor_direct(LRContext *ctx, LRValue func,
                                    int argc, LRValue *argv)
{
    if (func.tag != LR_TYPE_OBJECT) {
        return LR_VALUE_EXCEPTION;
    }
    LRObject *obj = (LRObject *)func.u.ptr;
    if (obj->type == LR_OBJ_CFUNCTION) {
        LRCFunction *cf = (LRCFunction *)obj->extra;
        if (cf && cf->func) {
            LRValue new_target = func;
            ctx->current_func = func;
            LRValue result = cf->func(ctx, new_target, argc, argv);
            ctx->current_func = LR_VALUE_UNDEFINED;
            return result;
        }
    }
    return LR_VALUE_UNDEFINED;
}

/* ── Property Function List ────────────────────────────────────────────── */

void lr_set_property_function_list(LRContext *ctx, LRValue obj,
    const LRCFunctionListEntry *tab, int count)
{
    for (int i = 0; i < count; i++) {
        if (!tab[i].name) continue;
        if (tab[i].def_type == 0) {
            /* function */
            LRValue fn = lr_new_cfunction2(ctx, tab[i].u.func.generic, tab[i].name,
                tab[i].u.func.length, tab[i].u.func.cproto, tab[i].magic);
            lr_set_property_str(ctx, obj, tab[i].name, fn);
        } else if (tab[i].def_type == 1) {
            /* getset - create accessor property */
            LRValue getter = LR_VALUE_UNDEFINED;
            LRValue setter = LR_VALUE_UNDEFINED;
            if (tab[i].u.getset.get) {
                getter = lr_new_cfunction2(ctx, tab[i].u.getset.get, tab[i].name,
                    0, 0, tab[i].magic);
            }
            if (tab[i].u.getset.set) {
                setter = lr_new_cfunction2(ctx, tab[i].u.getset.set, tab[i].name,
                    1, 0, tab[i].magic);
            }
            lr_set_accessor_property_str(ctx, obj, tab[i].name, getter, setter);
        } else {
            /* value */
            LRValue fn = lr_new_int32(ctx, tab[i].u.val.value);
            lr_set_property_str(ctx, obj, tab[i].name, fn);
        }
    }
}

/* ── Prototype ─────────────────────────────────────────────────────────── */

LRValue lr_get_prototype(LRContext *ctx, LRValue obj)
{
    (void)ctx;
    if (obj.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *o = (LRObject *)obj.u.ptr;
    return lr_dup_value(ctx, o->proto);
}

int lr_set_prototype(LRContext *ctx, LRValue obj, LRValue proto)
{
    (void)ctx;
    if (obj.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)obj.u.ptr;
    lr_free_value(ctx, o->proto);
    o->proto = lr_dup_value(ctx, proto);
    return 0;
}

/* ── Global Object ────────────────────────────────────────────────────── */

LRValue lr_get_global_object(LRContext *ctx)
{
    return lr_dup_value(ctx, ctx->global_obj);
}

/* ── Error Handling ───────────────────────────────────────────────────── */

static LRValue lr_throw_error(LRContext *ctx, int tag, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    ctx->error_message = strdup(buf);
    LRValue exc;
    exc.tag = tag;
    exc.u.ptr = NULL;
    ctx->current_exception = exc;
    return exc;
}

LRValue lr_throw_type_error(LRContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LRValue ret = lr_throw_error(ctx, LR_TYPE_EXCEPTION, fmt, ap);
    va_end(ap);
    return ret;
}

LRValue lr_throw_reference_error(LRContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LRValue ret = lr_throw_error(ctx, LR_TYPE_EXCEPTION, fmt, ap);
    va_end(ap);
    return ret;
}

LRValue lr_throw_range_error(LRContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LRValue ret = lr_throw_error(ctx, LR_TYPE_EXCEPTION, fmt, ap);
    va_end(ap);
    return ret;
}

LRValue lr_throw_syntax_error(LRContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LRValue ret = lr_throw_error(ctx, LR_TYPE_EXCEPTION, fmt, ap);
    va_end(ap);
    return ret;
}

LRValue lr_throw_internal_error(LRContext *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LRValue ret = lr_throw_error(ctx, LR_TYPE_EXCEPTION, fmt, ap);
    va_end(ap);
    return ret;
}

LRValue lr_get_exception(LRContext *ctx)
{
    return ctx->current_exception;
}

const char *lr_get_exception_str(LRContext *ctx)
{
    return ctx->error_message ? ctx->error_message : "";
}

/* ── Stack Trace Support ──────────────────────────────────────────────── */

void lr_push_call_frame(LRContext *ctx, const char *function_name,
                         const char *filename, int line_number)
{
    if (ctx->call_stack_depth >= LR_MAX_CALL_STACK_DEPTH) return;
    LRCallStackFrame *frame = &ctx->call_stack[ctx->call_stack_depth];
    frame->function_name = function_name ? strdup(function_name) : strdup("");
    frame->filename = filename ? strdup(filename) : strdup("");
    frame->line_number = line_number;
    ctx->call_stack_depth++;
}

void lr_pop_call_frame(LRContext *ctx)
{
    if (ctx->call_stack_depth <= 0) return;
    ctx->call_stack_depth--;
    LRCallStackFrame *frame = &ctx->call_stack[ctx->call_stack_depth];
    free((void *)frame->function_name);
    frame->function_name = NULL;
    free((void *)frame->filename);
    frame->filename = NULL;
}

char **lr_capture_stack_trace(LRContext *ctx, int *out_count)
{
    int limit = ctx->stack_trace_limit > 0 ? ctx->stack_trace_limit : ctx->call_stack_depth;
    int count = ctx->call_stack_depth < limit ? ctx->call_stack_depth : limit;
    *out_count = count;

    if (count == 0) return NULL;

    char **trace = (char **)calloc((size_t)count, sizeof(char *));
    if (!trace) {
        *out_count = 0;
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        LRCallStackFrame *frame = &ctx->call_stack[ctx->call_stack_depth - 1 - i];
        char buf[512];
        const char *fn = frame->function_name ? frame->function_name : "";
        const char *file = frame->filename ? frame->filename : "<unknown>";
        if (fn[0] == '\0') {
            snprintf(buf, sizeof(buf), "    at %s:%d", file, frame->line_number);
        } else {
            snprintf(buf, sizeof(buf), "    at %s (%s:%d)", fn, file, frame->line_number);
        }
        trace[i] = strdup(buf);
    }

    return trace;
}

void lr_free_stack_trace(LRContext *ctx, char **trace, int count)
{
    (void)ctx;
    if (!trace) return;
    for (int i = 0; i < count; i++) {
        free(trace[i]);
    }
    free(trace);
}

char *lr_build_stack_string(LRContext *ctx, const char *error_message)
{
    int count;
    char **trace = lr_capture_stack_trace(ctx, &count);

    /* Calculate total length */
    size_t total = 0;
    if (error_message) {
        total += strlen(error_message) + 2; /* message + newline */
    }
    for (int i = 0; i < count; i++) {
        total += strlen(trace[i]) + 1; /* line + newline */
    }

    char *result = (char *)malloc(total + 1);
    if (!result) {
        lr_free_stack_trace(ctx, trace, count);
        return NULL;
    }

    result[0] = '\0';
    if (error_message) {
        strcat(result, error_message);
        strcat(result, "\n");
    }
    for (int i = 0; i < count; i++) {
        strcat(result, trace[i]);
        strcat(result, "\n");
    }

    lr_free_stack_trace(ctx, trace, count);
    return result;
}

/* Error constructor - creates a proper Error object with message and stack.
 * Note: When called via 'new', lr_call_constructor passes a new object
 * with the correct prototype as this_val. */
LRValue lr_error_constructor(LRContext *ctx, LRValue this_val,
                              int argc, LRValue *argv)
{
    /* Use the object passed as this_val (created by lr_call_constructor) */
    LRValue obj = this_val;

    /* Set message property */
    const char *msg = "";
    if (argc > 0 && !lr_is_undefined(argv[0])) {
        msg = lr_to_cstring(ctx, argv[0]);
    }
    LRValue msg_val = lr_new_string(ctx, msg);
    lr_set_property_str(ctx, obj, "message", msg_val);

    /* Capture stack trace and set stack property */
    int count;
    char **trace = lr_capture_stack_trace(ctx, &count);
    char *stack = lr_build_stack_string(ctx, msg);

    /* Free the C string — safe now because lr_build_stack_string is done with it */
    if (argc > 0 && !lr_is_undefined(argv[0])) {
        lr_free_cstring(ctx, msg);
    }
    if (stack) {
        LRValue stack_val = lr_new_string(ctx, stack);
        lr_set_property_str(ctx, obj, "stack", stack_val);
        free(stack);
    }
    lr_free_stack_trace(ctx, trace, count);

    /* Handle options.cause: if second argument is an object with a 'cause' property */
    if (argc > 1 && !lr_is_undefined(argv[1]) && lr_is_object(argv[1])) {
        LRValue cause_val = lr_get_property_str(ctx, argv[1], "cause");
        if (!lr_is_undefined(cause_val)) {
            lr_set_property_str(ctx, obj, "cause", cause_val);
        }
    }

    return lr_dup_value(ctx, obj);
}

/* Error.captureStackTrace(target, constructorOpt) */
LRValue lr_error_capture_stack_trace(LRContext *ctx, LRValue this_val,
                                      int argc, LRValue *argv)
{
    (void)this_val;
    if (argc < 1 || !lr_is_object(argv[0])) {
        return lr_throw_type_error(ctx, "Error.captureStackTrace: target must be an object");
    }

    LRValue target = argv[0];

    /* Capture the stack trace as a string array and store it */
    int count;
    char **trace = lr_capture_stack_trace(ctx, &count);

    /* Build the full stack string */
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += strlen(trace[i]) + 1;
    }
    char *stack_str = (char *)malloc(total + 1);
    if (stack_str) {
        stack_str[0] = '\0';
        for (int i = 0; i < count; i++) {
            strcat(stack_str, trace[i]);
            strcat(stack_str, "\n");
        }
        /* Store as 'stack' property on the target.
         * Note: lr_set_property_str takes ownership of stack_val, so do NOT free it. */
        LRValue stack_val = lr_new_string(ctx, stack_str);
        lr_set_property_str(ctx, target, "stack", stack_val);
        free(stack_str);
    }

    lr_free_stack_trace(ctx, trace, count);
    return LR_VALUE_UNDEFINED;
}

/* Getter for Error.prototype.stack - lazily builds the stack string */
LRValue lr_error_proto_stack_getter(LRContext *ctx, LRValue this_val,
                                     int argc, LRValue *argv)
{
    (void)argc;
    (void)argv;
    if (!lr_is_object(this_val)) {
        return LR_VALUE_UNDEFINED;
    }

    /* Check if 'stack' property already exists */
    LRValue existing = lr_get_property_str(ctx, this_val, "stack");
    if (!lr_is_undefined(existing)) {
        return existing; /* caller must free */
    }
    lr_free_value(ctx, existing);

    /* Check if this is an error object by looking for 'message' property */
    LRValue msg_val = lr_get_property_str(ctx, this_val, "message");
    const char *msg_str = lr_to_cstring(ctx, msg_val);
    /* Build the stack string from the current call stack */
    char *stack = lr_build_stack_string(ctx, msg_str);
    lr_free_cstring(ctx, msg_str);
    lr_free_value(ctx, msg_val);

    LRValue result;
    if (stack) {
        result = lr_new_string(ctx, stack);
        free(stack);
    } else {
        result = LR_VALUE_UNDEFINED;
    }

    /* Cache the stack property */
    lr_set_property_str(ctx, this_val, "stack", lr_dup_value(ctx, result));

    return result;
}

/* ── Runtime / Context Lifecycle ──────────────────────────────────────── */

LRRuntime *lr_new_runtime(void)
{
    return lr_new_runtime2(NULL, NULL, NULL, NULL);
}

LRRuntime *lr_new_runtime2(void *mem_opaque, void *alloc_opaque,
                           void *free_opaque, void *realloc_opaque)
{
    (void)mem_opaque; (void)alloc_opaque; (void)free_opaque; (void)realloc_opaque;
    LRRuntime *rt = (LRRuntime *)calloc(1, sizeof(LRRuntime));
    if (!rt) return NULL;
    rt->gc_threshold = 1024 * 1024;  /* 1MB default (previously 256KB) - reduces GC pauses */
    rt->max_stack_size = 1024 * 1024; /* 1MB default */
    rt->gc_nursery_size = 8 * 1024 * 1024; /* 8MB (previously 4MB) */
    rt->gc_pause_target_ns = 10000000; /* 10ms (previously 5ms) - allows more work per GC pause */
    return rt;
}

void lr_free_runtime(LRRuntime *rt)
{
    if (!rt) return;
    /* Free all contexts */
    LRContext *ctx = rt->ctx_list;
    while (ctx) {
        LRContext *next = ctx->next_ctx;
        free(ctx->error_message);
        free(ctx);
        ctx = next;
    }
    free(rt);
}

LRContext *lr_new_context(LRRuntime *rt)
{
    LRContext *ctx = (LRContext *)calloc(1, sizeof(LRContext));
    if (!ctx) return NULL;
    ctx->rt = rt;

    /* Create global object */
    LRObject *global = lr_object_alloc(rt);
    global->ctx = ctx;
    ctx->global_obj.tag = LR_TYPE_OBJECT;
    ctx->global_obj.u.ptr = global;
    ctx->global_var_obj = ctx->global_obj;

    /* Initialize call stack */
    ctx->call_stack_depth = 0;
    ctx->stack_trace_limit = 10;

    /* Add to context list */
    ctx->next_ctx = rt->ctx_list;
    rt->ctx_list = ctx;

    /* Init atom table */
    ctx->atom_capacity = 64;
    ctx->atom_table = (LRString **)calloc(ctx->atom_capacity, sizeof(LRString *));

    return ctx;
}

void lr_free_context(LRContext *ctx)
{
    if (!ctx) return;
    LRRuntime *rt = ctx->rt;

    /* Free the persistent interpreter first: its scopes hold strong
     * references to JS values that must be released while the context
     * is still fully alive. Also frees all retained ASTs/sources. */
    lr_context_free_persistent_interp(ctx);

    /* Remove from runtime's context list */
    if (rt && rt->ctx_list) {
        if (rt->ctx_list == ctx) {
            rt->ctx_list = ctx->next_ctx;
        } else {
            LRContext *prev = rt->ctx_list;
            while (prev && prev->next_ctx != ctx) prev = prev->next_ctx;
            if (prev) prev->next_ctx = ctx->next_ctx;
        }
    }

    /* Free global object - break globalThis self-reference first */
    if (ctx->global_obj.tag == LR_TYPE_OBJECT) {
        lr_set_property_str(ctx, ctx->global_obj, "globalThis", LR_VALUE_UNDEFINED);
    }
    lr_free_value(ctx, ctx->global_obj);

    /* Break circular references (ctor↔proto) in remaining objects.
     * Clear all property values that are objects to LR_VALUE_UNDEFINED,
     * and clear obj->proto to break prototype chains.
     * This allows the remaining objects to be freed safely. */
    if (rt && rt->obj_list) {
        LRObject *obj = rt->obj_list;
        while (obj) {
            LRProperty *prop = obj->prop_hash;
            while (prop) {
                if (prop->value.tag == LR_TYPE_OBJECT) {
                    prop->value = LR_VALUE_UNDEFINED;
                }
                if (prop->setter.tag == LR_TYPE_OBJECT) {
                    prop->setter = LR_VALUE_UNDEFINED;
                }
                prop = prop->next;
            }
            for (uint32_t i = 0; i < obj->prop_count; i++) {
                if (obj->props[i].tag == LR_TYPE_OBJECT) {
                    obj->props[i] = LR_VALUE_UNDEFINED;
                }
            }
            if (obj->proto.tag == LR_TYPE_OBJECT) {
                obj->proto = LR_VALUE_UNDEFINED;
            }
            obj = obj->gc_next;
        }
    }
    /* Free atom table */
    for (uint32_t i = 0; i < ctx->atom_count; i++) {
        lr_string_free(rt, ctx->atom_table[i]);
    }
    free(ctx->atom_table);
    free(ctx->error_message);
    /* Note: ctx itself is NOT freed here. The caller (lr_runtime_free)
     * is responsible for freeing ctx AFTER the object cleanup loop,
     * because objects still reference ctx via obj->ctx and need it
     * for safe cleanup (e.g., data_free callbacks, opaque freeing). */
}

void lr_set_memory_limit(LRRuntime *rt, size_t limit)  { rt->malloc_limit = limit; }
void lr_set_gc_threshold(LRRuntime *rt, size_t threshold) { rt->gc_threshold = threshold; }
void lr_set_max_stack_size(LRRuntime *rt, size_t size) { rt->max_stack_size = size; }

void lr_set_context_opaque(LRContext *ctx, void *opaque) { ctx->opaque = opaque; }
void *lr_get_context_opaque(LRContext *ctx) { return ctx->opaque; }

void lr_set_can_block(LRRuntime *rt, int can_block) { (void)rt; (void)can_block; }

/* ── Module Loader ────────────────────────────────────────────────────── */

void lr_set_module_loader_func(LRRuntime *rt,
    LRModuleNormalizeFunc normalize, LRModuleLoaderFunc loader, void *opaque)
{
    rt->module_normalize_func = normalize;
    rt->module_loader_func = loader;
    rt->module_opaque = opaque;
}

/* ── Evaluation ────────────────────────────────────────────────────────── */

/* One retained compilation unit: heap copy of the source, the parser that
 * owns the AST node pool + interned strings, and the AST itself.
 * These must stay alive as long as JS function objects created from this
 * unit can still be called (their LRObject.extra points into the AST). */
typedef struct LREvalUnit {
    char    *src;
    Parser  *parser;
    ASTNode *ast;
    struct LREvalUnit *next;
} LREvalUnit;

/* Persistent per-context interpreter state. Created on first eval and
 * kept alive until lr_free_context, so that async callbacks (timers,
 * worker messages, promise jobs) can call JS functions after the initial
 * script evaluation has returned. */
typedef struct LRPersistentInterp {
    Interpreter interp;
    LREvalUnit *units;
} LRPersistentInterp;

static void lr_eval_unit_free(LREvalUnit *unit)
{
    if (!unit) return;
    if (unit->ast) ast_free_ex(unit->ast, unit->parser);
    if (unit->parser) {
        parser_free(unit->parser);
        free(unit->parser);
    }
    free(unit->src);
    free(unit);
}

void lr_context_free_persistent_interp(LRContext *ctx)
{
    LRPersistentInterp *ps = (LRPersistentInterp *)ctx->persistent_interp;
    if (!ps) return;
    interp_free(&ps->interp);
    LREvalUnit *u = ps->units;
    while (u) {
        LREvalUnit *next = u->next;
        lr_eval_unit_free(u);
        u = next;
    }
    free(ps);
    ctx->persistent_interp = NULL;
}

LRValue lr_engine_eval(LRContext *ctx, const char *input, size_t input_len,
                const char *filename, int flags)
{
    if (!input || input_len == 0) {
        return LR_VALUE_UNDEFINED;
    }

    /* Allocate the compilation unit up-front. The source is copied because
     * AST tokens keep pointers into it and the caller's buffer may be
     * freed right after this call. */
    LREvalUnit *unit = (LREvalUnit *)calloc(1, sizeof(LREvalUnit));
    if (!unit) return lr_throw_internal_error(ctx, "eval: out of memory");
    unit->src = (char *)malloc(input_len + 1);
    unit->parser = (Parser *)calloc(1, sizeof(Parser));
    if (!unit->src || !unit->parser) {
        free(unit->src);
        free(unit->parser);
        free(unit);
        return lr_throw_internal_error(ctx, "eval: out of memory");
    }
    memcpy(unit->src, input, input_len);
    unit->src[input_len] = '\0';

    /* Initialize lexer on the retained copy */
    Lexer lex;
    lexer_init(&lex, unit->src, input_len);

    /* Initialize parser */
    parser_init(unit->parser, &lex);

    /* Parse into AST */
    int is_module = flags & JS_EVAL_TYPE_MODULE;
    unit->ast = parse_program(unit->parser);

    if (!unit->ast || unit->parser->has_error) {
        const char *err_msg = parser_get_error(unit->parser, NULL, NULL);
        if (err_msg) {
            fprintf(stderr, "[LR_JS] Parse error: %s\n", err_msg);
        }
        lr_eval_unit_free(unit);
        return lr_throw_syntax_error(ctx, "%s: parse error",
            filename ? filename : "input");
    }

    /* Get or create the persistent interpreter for this context */
    LRPersistentInterp *ps = (LRPersistentInterp *)ctx->persistent_interp;
    if (!ps) {
        ps = (LRPersistentInterp *)calloc(1, sizeof(LRPersistentInterp));
        if (!ps) {
            lr_eval_unit_free(unit);
            return lr_throw_internal_error(ctx, "eval: out of memory");
        }
        interp_init(&ps->interp, ctx, is_module);
        ctx->persistent_interp = ps;
    } else {
        /* Re-attach callbacks in case they were cleared */
        interp_reattach(&ps->interp, ctx);
    }
    Interpreter *interp = &ps->interp;

    /* Support nested evals: run program-level code in the global scope,
     * then restore whatever scope the outer execution was in. */
    InterpScope *saved_scope = interp->current_scope;
    int saved_is_module = interp->is_module;
    interp->current_scope = interp->global_scope;
    interp->is_module = is_module;

    /* Evaluate the AST */
    LRValue result = interp_eval(interp, unit->ast);

    interp->current_scope = saved_scope;
    interp->is_module = saved_is_module;

    /* Process pending microtasks (Promise jobs) */
    if (lr_is_job_pending(ctx->rt)) {
        int job_count = 0;
        while (lr_is_job_pending(ctx->rt) && job_count < 1000) {
            LRContext *job_ctx = NULL;
            if (lr_execute_pending_job(ctx->rt, &job_ctx) != 0) {
                break;
            }
            job_count++;
        }
    }

    /* Retain the unit: JS functions created during eval reference the AST
     * (LRObject.extra), so it must outlive this call. Freed with the
     * context in lr_context_free_persistent_interp(). */
    unit->next = ps->units;
    ps->units = unit;

    return result;
}

LRValue lr_engine_eval_function(LRContext *ctx, LRValue func_obj)
{
    (void)ctx; (void)func_obj;
    return LR_VALUE_UNDEFINED;
}

int lr_engine_detect_module(const char *input, size_t input_len)
{
    /* Simple heuristic: check for import/export statements */
    (void)input; (void)input_len;
    return 0;
}

/* ── Bytecode Serialization ───────────────────────────────────────────── */

/* Serialization markers */
#define LR_SERIALIZE_UNDEFINED   0
#define LR_SERIALIZE_NULL        1
#define LR_SERIALIZE_FALSE       2
#define LR_SERIALIZE_TRUE        3
#define LR_SERIALIZE_INT32       4
#define LR_SERIALIZE_FLOAT64     5
#define LR_SERIALIZE_STRING      6
#define LR_SERIALIZE_OBJECT      7
#define LR_SERIALIZE_ARRAY       8
#define LR_SERIALIZE_REFERENCE   9

/* ── Serialization buffer (dynamic grow) ───────────────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} SerialBuffer;

static SerialBuffer *sb_new(void)
{
    SerialBuffer *sb = (SerialBuffer *)malloc(sizeof(SerialBuffer));
    if (!sb) return NULL;
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    return sb;
}

static void sb_free(SerialBuffer *sb)
{
    if (sb) {
        free(sb->data);
        free(sb);
    }
}

static int sb_grow(SerialBuffer *sb, size_t needed)
{
    if (sb->len + needed <= sb->cap)
        return 0;
    size_t new_cap = sb->cap ? sb->cap * 2 : 256;
    while (new_cap < sb->len + needed)
        new_cap *= 2;
    uint8_t *new_data = (uint8_t *)realloc(sb->data, new_cap);
    if (!new_data) return -1;
    sb->data = new_data;
    sb->cap  = new_cap;
    return 0;
}

static int sb_write(SerialBuffer *sb, const void *data, size_t len)
{
    if (sb_grow(sb, len) < 0) return -1;
    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    return 0;
}

static int sb_write_u8(SerialBuffer *sb, uint8_t val)
{
    return sb_write(sb, &val, 1);
}

static int sb_write_u32(SerialBuffer *sb, uint32_t val)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
    return sb_write(sb, buf, 4);
}

static int sb_write_f64(SerialBuffer *sb, double val)
{
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    uint8_t buf[8];
    buf[0] = (uint8_t)( bits       & 0xFF);
    buf[1] = (uint8_t)((bits >>  8) & 0xFF);
    buf[2] = (uint8_t)((bits >> 16) & 0xFF);
    buf[3] = (uint8_t)((bits >> 24) & 0xFF);
    buf[4] = (uint8_t)((bits >> 32) & 0xFF);
    buf[5] = (uint8_t)((bits >> 40) & 0xFF);
    buf[6] = (uint8_t)((bits >> 48) & 0xFF);
    buf[7] = (uint8_t)((bits >> 56) & 0xFF);
    return sb_write(sb, buf, 8);
}

/* ── Object tracker (write-side: detect circular references) ───────────── */

typedef struct {
    LRObject **objects;
    uint32_t   count;
    uint32_t   capacity;
} ObjTracker;

static ObjTracker *ot_new(void)
{
    ObjTracker *t = (ObjTracker *)malloc(sizeof(ObjTracker));
    if (!t) return NULL;
    t->objects  = NULL;
    t->count    = 0;
    t->capacity = 0;
    return t;
}

static void ot_free(ObjTracker *t)
{
    if (t) {
        free(t->objects);
        free(t);
    }
}

static int ot_add(ObjTracker *t, LRObject *obj)
{
    if (t->count >= t->capacity) {
        uint32_t new_cap = t->capacity ? t->capacity * 2 : 16;
        LRObject **new_objs = (LRObject **)realloc(t->objects,
                                    new_cap * sizeof(LRObject *));
        if (!new_objs) return -1;
        t->objects  = new_objs;
        t->capacity = new_cap;
    }
    t->objects[t->count++] = obj;
    return (int)(t->count - 1);
}

static int ot_find(ObjTracker *t, LRObject *obj)
{
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->objects[i] == obj) return (int)i;
    }
    return -1;
}

/* ── Read tracker (read-side: reconstruct circular references) ─────────── */

typedef struct {
    LRValue   *values;
    uint32_t   count;
    uint32_t   capacity;
    LRContext *ctx;
} ReadTracker;

static ReadTracker *rt_new(LRContext *ctx)
{
    ReadTracker *t = (ReadTracker *)malloc(sizeof(ReadTracker));
    if (!t) return NULL;
    t->values   = NULL;
    t->count    = 0;
    t->capacity = 0;
    t->ctx      = ctx;
    return t;
}

static void rt_free(ReadTracker *t)
{
    if (t) {
        for (uint32_t i = 0; i < t->count; i++)
            lr_free_value(t->ctx, t->values[i]);
        free(t->values);
        free(t);
    }
}

static int rt_add(ReadTracker *t, LRValue val)
{
    if (t->count >= t->capacity) {
        uint32_t new_cap = t->capacity ? t->capacity * 2 : 16;
        LRValue *new_vals = (LRValue *)realloc(t->values,
                                    new_cap * sizeof(LRValue));
        if (!new_vals) return -1;
        t->values   = new_vals;
        t->capacity = new_cap;
    }
    /* Store a dup'd reference so the tracker keeps the value alive */
    t->values[t->count] = lr_dup_value(t->ctx, val);
    t->count++;
    return (int)(t->count - 1);
}

/* ── Forward declarations ──────────────────────────────────────────────── */

static int serialize_value(LRContext *ctx, SerialBuffer *sb,
                            LRValue val, ObjTracker *tracker);

/* ── Serialize a single value ──────────────────────────────────────────── */

static int serialize_value(LRContext *ctx, SerialBuffer *sb,
                            LRValue val, ObjTracker *tracker)
{
    switch (val.tag) {
    case LR_TYPE_UNDEFINED:
        return sb_write_u8(sb, LR_SERIALIZE_UNDEFINED);

    case LR_TYPE_NULL:
        return sb_write_u8(sb, LR_SERIALIZE_NULL);

    case LR_TYPE_BOOL:
        return sb_write_u8(sb, val.u.bool_val
                            ? LR_SERIALIZE_TRUE : LR_SERIALIZE_FALSE);

    case LR_TYPE_INT32:
        if (sb_write_u8(sb, LR_SERIALIZE_INT32) < 0) return -1;
        return sb_write_u32(sb, (uint32_t)val.u.int32);

    case LR_TYPE_FLOAT64:
        if (sb_write_u8(sb, LR_SERIALIZE_FLOAT64) < 0) return -1;
        return sb_write_f64(sb, val.u.float64);

    case LR_TYPE_STRING: {
        LRString *s = (LRString *)val.u.ptr;
        if (sb_write_u8(sb, LR_SERIALIZE_STRING) < 0) return -1;
        if (sb_write_u32(sb, s ? s->len : 0) < 0) return -1;
        if (s && s->len > 0) {
            if (sb_write(sb, s->str, s->len) < 0) return -1;
        }
        return 0;
    }

    case LR_TYPE_OBJECT: {
        LRObject *obj = (LRObject *)val.u.ptr;
        if (!obj) {
            return sb_write_u8(sb, LR_SERIALIZE_UNDEFINED);
        }

        /* Check for circular reference */
        int idx = ot_find(tracker, obj);
        if (idx >= 0) {
            if (sb_write_u8(sb, LR_SERIALIZE_REFERENCE) < 0) return -1;
            return sb_write_u32(sb, (uint32_t)idx);
        }

        /* Add to tracker before recursing into children */
        if (ot_add(tracker, obj) < 0) return -1;

        if (obj->type == LR_OBJ_ARRAY) {
            /* ── Serialize as array ── */
            if (sb_write_u8(sb, LR_SERIALIZE_ARRAY) < 0) return -1;

            LRValue len_val = lr_get_property_str(ctx, val, "length");
            int32_t len = 0;
            lr_to_int32(ctx, &len, len_val);
            lr_free_value(ctx, len_val);

            if (len < 0) len = 0;
            if (sb_write_u32(sb, (uint32_t)len) < 0) return -1;

            for (int32_t i = 0; i < len; i++) {
                LRValue elem = lr_get_property_uint32(ctx, val, (uint32_t)i);
                if (serialize_value(ctx, sb, elem, tracker) < 0) {
                    lr_free_value(ctx, elem);
                    return -1;
                }
                lr_free_value(ctx, elem);
            }
        } else {
            /* ── Serialize as plain object ── */
            if (sb_write_u8(sb, LR_SERIALIZE_OBJECT) < 0) return -1;

            LRPropertyEnum *props = NULL;
            uint32_t prop_count = 0;
            lr_get_own_property_names(ctx, &props, &prop_count, val, 0);

            if (sb_write_u32(sb, prop_count) < 0) {
                lr_free_property_enum(ctx, props, prop_count);
                return -1;
            }

            for (uint32_t i = 0; i < prop_count; i++) {
                LRString *key = props[i].atom;
                uint32_t key_len = key ? key->len : 0;

                /* Write key as a serialized string */
                if (sb_write_u8(sb, LR_SERIALIZE_STRING) < 0) {
                    lr_free_property_enum(ctx, props, prop_count);
                    return -1;
                }
                if (sb_write_u32(sb, key_len) < 0) {
                    lr_free_property_enum(ctx, props, prop_count);
                    return -1;
                }
                if (key && key_len > 0) {
                    if (sb_write(sb, key->str, key_len) < 0) {
                        lr_free_property_enum(ctx, props, prop_count);
                        return -1;
                    }
                }

                /* Write property value */
                LRValue prop_val = lr_get_property(ctx, val, key);
                int ser_ret = serialize_value(ctx, sb, prop_val, tracker);
                lr_free_value(ctx, prop_val);
                if (ser_ret < 0) {
                    lr_free_property_enum(ctx, props, prop_count);
                    return -1;
                }
            }
            lr_free_property_enum(ctx, props, prop_count);
        }
        return 0;
    }

    default:
        /* Unknown type → serialize as undefined */
        return sb_write_u8(sb, LR_SERIALIZE_UNDEFINED);
    }
}

/* ── Read helpers ──────────────────────────────────────────────────────── */

static uint8_t read_u8(const uint8_t *buf, size_t *pos, size_t buf_len)
{
    if (*pos + 1 > buf_len) return 0;
    return buf[(*pos)++];
}

static uint32_t read_u32(const uint8_t *buf, size_t *pos, size_t buf_len)
{
    if (*pos + 4 > buf_len) return 0;
    uint32_t val = (uint32_t)buf[*pos]
                 | ((uint32_t)buf[*pos + 1] << 8)
                 | ((uint32_t)buf[*pos + 2] << 16)
                 | ((uint32_t)buf[*pos + 3] << 24);
    *pos += 4;
    return val;
}

static double read_f64(const uint8_t *buf, size_t *pos, size_t buf_len)
{
    if (*pos + 8 > buf_len) return 0.0;
    uint64_t bits = (uint64_t)buf[*pos]
                  | ((uint64_t)buf[*pos + 1] << 8)
                  | ((uint64_t)buf[*pos + 2] << 16)
                  | ((uint64_t)buf[*pos + 3] << 24)
                  | ((uint64_t)buf[*pos + 4] << 32)
                  | ((uint64_t)buf[*pos + 5] << 40)
                  | ((uint64_t)buf[*pos + 6] << 48)
                  | ((uint64_t)buf[*pos + 7] << 56);
    *pos += 8;
    double val;
    memcpy(&val, &bits, sizeof(val));
    return val;
}

/* ── Forward declarations ──────────────────────────────────────────────── */

static LRValue deserialize_value(LRContext *ctx, const uint8_t *buf,
                                  size_t *pos, size_t buf_len,
                                  ReadTracker *rtracker);

/* ── Deserialize a single value ────────────────────────────────────────── */

static LRValue deserialize_value(LRContext *ctx, const uint8_t *buf,
                                  size_t *pos, size_t buf_len,
                                  ReadTracker *rtracker)
{
    if (*pos >= buf_len)
        return LR_VALUE_UNDEFINED;

    uint8_t marker = read_u8(buf, pos, buf_len);

    switch (marker) {
    case LR_SERIALIZE_UNDEFINED:
        return LR_VALUE_UNDEFINED;

    case LR_SERIALIZE_NULL:
        return LR_VALUE_NULL;

    case LR_SERIALIZE_FALSE:
        return LR_VALUE_FALSE;

    case LR_SERIALIZE_TRUE:
        return LR_VALUE_TRUE;

    case LR_SERIALIZE_INT32: {
        int32_t val = (int32_t)read_u32(buf, pos, buf_len);
        return lr_new_int32(ctx, val);
    }

    case LR_SERIALIZE_FLOAT64: {
        double val = read_f64(buf, pos, buf_len);
        return lr_new_float64(ctx, val);
    }

    case LR_SERIALIZE_STRING: {
        uint32_t len = read_u32(buf, pos, buf_len);
        if (*pos + len > buf_len) return LR_VALUE_UNDEFINED;
        const char *str = (const char *)(buf + *pos);
        LRValue result = lr_new_string_len(ctx, str, len);
        *pos += len;
        return result;
    }

    case LR_SERIALIZE_OBJECT: {
        LRValue obj = lr_new_object(ctx);
        if (obj.tag == LR_TYPE_EXCEPTION) return obj;

        /* Add to tracker BEFORE reading properties (needed for circular refs) */
        rt_add(rtracker, obj);

        uint32_t prop_count = read_u32(buf, pos, buf_len);
        for (uint32_t i = 0; i < prop_count; i++) {
            /* Read key as a serialized string */
            LRValue key_val = deserialize_value(ctx, buf, pos, buf_len, rtracker);
            if (key_val.tag != LR_TYPE_STRING) {
                lr_free_value(ctx, key_val);
                lr_free_value(ctx, obj);
                return LR_VALUE_UNDEFINED;
            }
            LRString *key = (LRString *)key_val.u.ptr;

            /* Read value (set_property takes ownership) */
            LRValue prop_val = deserialize_value(ctx, buf, pos, buf_len, rtracker);
            lr_set_property(ctx, obj, key, prop_val);
            lr_free_value(ctx, key_val);
        }
        return obj;
    }

    case LR_SERIALIZE_ARRAY: {
        LRValue arr = lr_new_array(ctx);
        if (arr.tag == LR_TYPE_EXCEPTION) return arr;

        /* Add to tracker BEFORE reading elements (needed for circular refs) */
        rt_add(rtracker, arr);

        uint32_t len = read_u32(buf, pos, buf_len);
        for (uint32_t i = 0; i < len; i++) {
            LRValue elem = deserialize_value(ctx, buf, pos, buf_len, rtracker);
            lr_set_property_uint32(ctx, arr, i, elem);
        }

        /* Set length property */
        lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, (int32_t)len));
        return arr;
    }

    case LR_SERIALIZE_REFERENCE: {
        uint32_t idx = read_u32(buf, pos, buf_len);
        if (idx < rtracker->count) {
            return lr_dup_value(rtracker->ctx, rtracker->values[idx]);
        }
        return LR_VALUE_UNDEFINED;
    }

    default:
        return LR_VALUE_UNDEFINED;
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

LRValue lr_read_object(LRContext *ctx, const uint8_t *buf, size_t buf_len, int flags)
{
    (void)flags;
    if (!ctx || !buf || buf_len == 0)
        return LR_VALUE_UNDEFINED;

    ReadTracker *rtracker = rt_new(ctx);
    if (!rtracker)
        return LR_VALUE_UNDEFINED;

    size_t pos = 0;
    LRValue result = deserialize_value(ctx, buf, &pos, buf_len, rtracker);

    rt_free(rtracker);
    return result;
}

uint8_t *lr_write_object(LRContext *ctx, size_t *pout_len, LRValue obj, int flags)
{
    (void)flags;
    if (!ctx) {
        if (pout_len) *pout_len = 0;
        return NULL;
    }

    SerialBuffer *sb = sb_new();
    if (!sb) {
        if (pout_len) *pout_len = 0;
        return NULL;
    }

    ObjTracker *tracker = ot_new();
    if (!tracker) {
        sb_free(sb);
        if (pout_len) *pout_len = 0;
        return NULL;
    }

    if (serialize_value(ctx, sb, obj, tracker) < 0) {
        ot_free(tracker);
        sb_free(sb);
        if (pout_len) *pout_len = 0;
        return NULL;
    }

    ot_free(tracker);

    if (pout_len) *pout_len = sb->len;
    uint8_t *result = sb->data;
    free(sb);  /* keep the data buffer, free the struct */
    return result;
}

/* ── Job Queue ────────────────────────────────────────────────────────── */

int lr_is_job_pending(LRRuntime *rt)
{
    return rt->job_list != NULL;
}

int lr_execute_pending_job(LRRuntime *rt, LRContext **pctx)
{
    if (!rt->job_list) return 0;
    LRJobEntry *job = rt->job_list;
    rt->job_list = job->next;
    if (!rt->job_list) rt->job_list_tail = NULL;

    if (pctx) *pctx = job->ctx;

    LRValue result = lr_call(job->ctx, job->func, LR_VALUE_UNDEFINED, 0, NULL);
    lr_free_value(job->ctx, result);
    lr_free_value(job->ctx, job->func);
    free(job);
    return 0;
}

void lr_enqueue_job(LRRuntime *rt, LRContext *ctx, LRValue func)
{
    LRJobEntry *job = (LRJobEntry *)malloc(sizeof(LRJobEntry));
    job->ctx = ctx;
    job->func = lr_dup_value(ctx, func);
    job->next = NULL;

    if (rt->job_list_tail) {
        rt->job_list_tail->next = job;
    } else {
        rt->job_list = job;
    }
    rt->job_list_tail = job;
    rt->has_pending_jobs = 1;
}

/* ── GC ───────────────────────────────────────────────────────────────── */

/* Forward declaration of recursive mark helper */
static void lr_gc_mark_value(LRRuntime *rt, LRValue val);

/* Recursively mark an object and all objects reachable from it */
static void lr_gc_mark_object(LRRuntime *rt, LRObject *obj)
{
    if (!obj || obj->gc_mark) return;
    obj->gc_mark = 1;

    /* Mark prototype */
    if (obj->proto.tag == LR_TYPE_OBJECT && obj->proto.u.ptr) {
        lr_gc_mark_object(rt, (LRObject *)obj->proto.u.ptr);
    }

    /* Mark properties in the props array (shape-based) */
    if (obj->props) {
        for (uint32_t i = 0; i < obj->prop_count; i++) {
            lr_gc_mark_value(rt, obj->props[i]);
        }
    }

    /* Mark properties in the hash table */
    LRProperty *prop = obj->prop_hash;
    while (prop) {
        lr_gc_mark_value(rt, prop->value);
        lr_gc_mark_value(rt, prop->setter);
        prop = prop->next;
    }

    /* Mark type-specific data */
    switch (obj->type) {
    case LR_OBJ_PROXY: {
        LRProxyData *pd = (LRProxyData *)obj->extra;
        if (pd) {
            lr_gc_mark_value(rt, pd->target);
            lr_gc_mark_value(rt, pd->handler);
        }
        break;
    }
    case LR_OBJ_PROMISE: {
        LRPromiseData *pd = (LRPromiseData *)obj->opaque;
        if (pd) {
            lr_gc_mark_value(rt, pd->result);
            for (int i = 0; i < pd->fulfill_count; i++) {
                lr_gc_mark_value(rt, pd->fulfill_reactions[i].handler);
                lr_gc_mark_value(rt, pd->fulfill_reactions[i].resolve);
                lr_gc_mark_value(rt, pd->fulfill_reactions[i].reject);
                lr_gc_mark_value(rt, pd->fulfill_reactions[i].promise);
            }
            for (int i = 0; i < pd->reject_count; i++) {
                lr_gc_mark_value(rt, pd->reject_reactions[i].handler);
                lr_gc_mark_value(rt, pd->reject_reactions[i].resolve);
                lr_gc_mark_value(rt, pd->reject_reactions[i].reject);
                lr_gc_mark_value(rt, pd->reject_reactions[i].promise);
            }
        }
        break;
    }
    case LR_OBJ_FUNCTION:
    case LR_OBJ_BYTECODE_FUNC: {
        /* Mark the function's prototype property if set */
        /* Functions store their prototype in the property hash */
        break;
    }
    case LR_OBJ_TYPED_ARRAY: {
        TypedArrayData *tad = (TypedArrayData *)obj->opaque;
        if (tad) {
            lr_gc_mark_value(rt, tad->buffer);
        }
        break;
    }
    case LR_OBJ_DATA_VIEW: {
        DataViewData *dvd = (DataViewData *)obj->opaque;
        if (dvd) {
            lr_gc_mark_value(rt, dvd->buffer);
        }
        break;
    }
    default:
        break;
    }
}

/* Mark a value if it is an object */
static void lr_gc_mark_value(LRRuntime *rt, LRValue val)
{
    if (val.tag != LR_TYPE_OBJECT) return;
    LRObject *obj = (LRObject *)val.u.ptr;
    if (obj && !obj->gc_mark) {
        lr_gc_mark_object(rt, obj);
    }
}

/* Run mark-and-sweep garbage collection */
void lr_gc_run(LRRuntime *rt)
{
    if (!rt) return;

    /* ── Mark Phase ── */
    /* Clear all marks first */
    LRObject *obj = rt->obj_list;
    while (obj) {
        obj->gc_mark = 0;
        obj = obj->gc_next;
    }

    /* Mark roots from all contexts */
    LRContext *ctx = rt->ctx_list;
    while (ctx) {
        lr_gc_mark_value(rt, ctx->global_obj);
        lr_gc_mark_value(rt, ctx->global_var_obj);
        lr_gc_mark_value(rt, ctx->object_proto);
        lr_gc_mark_value(rt, ctx->array_proto);
        lr_gc_mark_value(rt, ctx->string_proto);
        lr_gc_mark_value(rt, ctx->number_proto);
        lr_gc_mark_value(rt, ctx->function_proto);
        lr_gc_mark_value(rt, ctx->current_func);
        lr_gc_mark_value(rt, ctx->current_exception);
        ctx = ctx->next_ctx;
    }

    /* Mark job queue functions (Promise microtasks) */
    LRJobEntry *job = rt->job_list;
    while (job) {
        lr_gc_mark_value(rt, job->func);
        job = job->next;
    }

    /* ── Sweep Phase ── */

    /* Protect root objects: increment ref counts so they cannot be
     * accidentally freed by property cleanup (lr_free_value(NULL, ...))
     * when an unmarked object is freed during sweep. */
    {
        LRContext *c = rt->ctx_list;
        while (c) {
            if (c->global_obj.tag == LR_TYPE_OBJECT && c->global_obj.u.ptr)
                ((LRObject *)c->global_obj.u.ptr)->ref_count++;
            if (c->global_var_obj.tag == LR_TYPE_OBJECT && c->global_var_obj.u.ptr)
                ((LRObject *)c->global_var_obj.u.ptr)->ref_count++;
            if (c->object_proto.tag == LR_TYPE_OBJECT && c->object_proto.u.ptr)
                ((LRObject *)c->object_proto.u.ptr)->ref_count++;
            if (c->array_proto.tag == LR_TYPE_OBJECT && c->array_proto.u.ptr)
                ((LRObject *)c->array_proto.u.ptr)->ref_count++;
            if (c->string_proto.tag == LR_TYPE_OBJECT && c->string_proto.u.ptr)
                ((LRObject *)c->string_proto.u.ptr)->ref_count++;
            if (c->number_proto.tag == LR_TYPE_OBJECT && c->number_proto.u.ptr)
                ((LRObject *)c->number_proto.u.ptr)->ref_count++;
            if (c->function_proto.tag == LR_TYPE_OBJECT && c->function_proto.u.ptr)
                ((LRObject *)c->function_proto.u.ptr)->ref_count++;
            if (c->current_func.tag == LR_TYPE_OBJECT && c->current_func.u.ptr)
                ((LRObject *)c->current_func.u.ptr)->ref_count++;
            if (c->current_exception.tag == LR_TYPE_OBJECT && c->current_exception.u.ptr)
                ((LRObject *)c->current_exception.u.ptr)->ref_count++;
            c = c->next_ctx;
        }
    }

    /* Sweep: traverse obj_list, free unmarked objects that are NOT
     * referenced by any non-GC-tracked code.
     *
     * IMPORTANT: We only free objects with gc_mark == 0 AND ref_count == 0
     * (already freed by ref counting, just need to clean up the list).
     * Objects with ref_count > 0 are NOT freed here because they might be
     * referenced by non-GC-tracked code (e.g., timer callbacks, C stack
     * variables). Freeing them would cause use-after-free crashes.
     *
     * Before freeing an unmarked object, detach its properties
     * (props and proto) to prevent lr_free_object from calling
     * lr_free_value(NULL, ...) on them. Otherwise, sub-objects would be
     * freed by free() directly without being unlinked from obj_list,
     * creating dangling pointers that corrupt the sweep traversal. */
    {
        LRObject **pprev = &rt->obj_list;
        while (*pprev) {
            LRObject *obj = *pprev;
            if (obj->gc_mark == 0) {
                if (obj->ref_count > 0 || obj->finalization_pending) {
                    /* Object is referenced but not GC-marked, OR it is
                     * awaiting deferred finalization (ref_count 0 but kept
                     * alive for WeakRefs / FinalizationRegistry). Keep it
                     * in the object list; it will be freed explicitly. */
                    pprev = &(*pprev)->gc_next;
                    continue;
                }
                /* ref_count == 0 — already freed by ref counting, skip */
                *pprev = obj->gc_next;
                continue;
            } else {
                /* Marked — reset for next GC cycle */
                obj->gc_mark = 0;
            }
            pprev = &(*pprev)->gc_next;
        }
    }

    /* Restore root object ref counts */
    {
        LRContext *c = rt->ctx_list;
        while (c) {
            if (c->global_obj.tag == LR_TYPE_OBJECT && c->global_obj.u.ptr) {
                LRObject *o = (LRObject *)c->global_obj.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->global_var_obj.tag == LR_TYPE_OBJECT && c->global_var_obj.u.ptr) {
                LRObject *o = (LRObject *)c->global_var_obj.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->object_proto.tag == LR_TYPE_OBJECT && c->object_proto.u.ptr) {
                LRObject *o = (LRObject *)c->object_proto.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->array_proto.tag == LR_TYPE_OBJECT && c->array_proto.u.ptr) {
                LRObject *o = (LRObject *)c->array_proto.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->string_proto.tag == LR_TYPE_OBJECT && c->string_proto.u.ptr) {
                LRObject *o = (LRObject *)c->string_proto.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->number_proto.tag == LR_TYPE_OBJECT && c->number_proto.u.ptr) {
                LRObject *o = (LRObject *)c->number_proto.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->function_proto.tag == LR_TYPE_OBJECT && c->function_proto.u.ptr) {
                LRObject *o = (LRObject *)c->function_proto.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->current_func.tag == LR_TYPE_OBJECT && c->current_func.u.ptr) {
                LRObject *o = (LRObject *)c->current_func.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            if (c->current_exception.tag == LR_TYPE_OBJECT && c->current_exception.u.ptr) {
                LRObject *o = (LRObject *)c->current_exception.u.ptr;
                if (--o->ref_count <= 0) lr_free_object(rt, o);
            }
            c = c->next_ctx;
        }
    }
}

void lr_engine_compute_memory_usage(LRRuntime *rt, LRMemoryUsage *usage)
{
    memset(usage, 0, sizeof(*usage));
    usage->malloc_size = (int64_t)rt->malloc_size;
    usage->malloc_limit = (int64_t)rt->malloc_limit;
    usage->memory_used_size = (int64_t)rt->memory_used_size;
    usage->malloc_count = rt->malloc_count;
    usage->memory_used_count = rt->memory_used_count;
    usage->atom_count = rt->atom_count;
    usage->atom_size = rt->atom_size;
    usage->str_count = rt->str_count;
    usage->str_size = rt->str_size;
    usage->obj_count = rt->obj_count;
    usage->prop_count = rt->prop_count;
    usage->prop_size = rt->prop_size;
    usage->shape_count = rt->shape_count;
    usage->shape_size = rt->shape_size;
    usage->c_func_count = rt->c_func_count;
    usage->array_count = rt->array_count;
}

/* ── Atom Operations ──────────────────────────────────────────────────── */

LRString *lr_new_atom(LRContext *ctx, const char *str)
{
    return lr_new_atom_len(ctx, str, strlen(str));
}

LRString *lr_new_atom_len(LRContext *ctx, const char *str, size_t len)
{
    /* Check if atom already exists */
    for (uint32_t i = 0; i < ctx->atom_count; i++) {
        if (ctx->atom_table[i]->len == len &&
            memcmp(ctx->atom_table[i]->str, str, len) == 0) {
            return lr_string_dup(ctx->atom_table[i]);
        }
    }

    /* Create new atom */
    LRString *atom = lr_string_alloc(ctx->rt, str, len);
    if (!atom) return NULL;
    atom->is_atom = 1;

    /* Add to atom table */
    if (ctx->atom_count >= ctx->atom_capacity) {
        ctx->atom_capacity *= 2;
        ctx->atom_table = (LRString **)realloc(ctx->atom_table,
            ctx->atom_capacity * sizeof(LRString *));
    }
    ctx->atom_table[ctx->atom_count++] = atom;
    ctx->rt->atom_count++;
    ctx->rt->atom_size += (int64_t)(sizeof(LRString) + len + 1);

    return lr_string_dup(atom);
}

const char *lr_atom_to_cstring(LRContext *ctx, LRString *atom)
{
    (void)ctx;
    if (atom) {
        /* Return a malloc'd copy so callers can free it with lr_free_cstring/free() */
        char *copy = (char *)malloc(atom->len + 1);
        if (copy) {
            memcpy(copy, atom->str, atom->len + 1);
        }
        return copy;
    }
    return strdup("");
}

LRString *lr_to_atom(LRContext *ctx, LRValue val)
{
    if (val.tag == LR_TYPE_STRING) {
        LRString *s = (LRString *)val.u.ptr;
        return lr_new_atom_len(ctx, s->str, s->len);
    }
    const char *str = lr_to_cstring(ctx, val);
    LRString *atom = lr_new_atom(ctx, str);
    lr_free_cstring(ctx, str);
    return atom;
}

LRValue lr_atom_to_value(LRContext *ctx, LRString *atom)
{
    (void)ctx;
    LRValue v;
    v.tag = LR_TYPE_STRING;
    v.u.ptr = lr_string_dup(atom);
    return v;
}

/* ── Class System (Stubs) ─────────────────────────────────────────────── */

LRClass *lr_new_class(const char *name, LRValue ctor, LRValue proto)
{
    LRClass *cls = (LRClass *)calloc(1, sizeof(LRClass));
    cls->ref_count = 1;
    cls->name = name ? strdup(name) : NULL;
    cls->constructor = ctor;
    cls->prototype = proto;
    cls->class_id = 0;
    return cls;
}

LRClass *lr_dup_class(LRClass *cls) { if (cls) cls->ref_count++; return cls; }

void lr_free_class(LRRuntime *rt, LRClass *cls)
{
    (void)rt;
    if (!cls) return;
    if (--cls->ref_count <= 0) {
        free((void *)cls->name);
        free(cls);
    }
}

LRClass *lr_get_object_class(LRValue obj) {
    if (obj.tag != LR_TYPE_OBJECT) return NULL;
    return ((LRObject *)obj.u.ptr)->class_def;
}

int lr_get_object_class_id(LRValue obj) {
    LRClass *cls = lr_get_object_class(obj);
    return cls ? cls->class_id : -1;
}

void lr_set_object_class(LRValue obj, LRClass *cls) {
    if (obj.tag != LR_TYPE_OBJECT) return;
    ((LRObject *)obj.u.ptr)->class_def = cls;
}

/* ── Array Buffer Support ──────────────────────────────────────────────── */

LRValue lr_new_array_buffer(LRContext *ctx, uint8_t *buf, size_t len,
    void (*free_func)(void *opaque, void *ptr), void *opaque, int is_shared)
{
    (void)is_shared;
    LRValue v;
    LRObject *obj = lr_object_alloc(ctx->rt);
    if (!obj) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    obj->type = LR_OBJ_ARRAY_BUFFER;
    obj->ctx = ctx;
    obj->extra = buf;
    /* Store free_func and opaque for cleanup */
    if (free_func) {
        ArrayBufferMeta *meta = (ArrayBufferMeta *)malloc(sizeof(ArrayBufferMeta));
        if (meta) {
            meta->free_func = free_func;
            meta->opaque = opaque;
            meta->is_shared = is_shared;
        }
        obj->opaque = meta;
    } else {
        obj->opaque = NULL;
    }
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    /* Set length property */
    lr_set_property_str(ctx, v, "byteLength", lr_new_int32(ctx, (int32_t)len));
    return v;
}

int lr_array_buffer_is_shared(LRValue obj)
{
    if (obj.tag != LR_TYPE_OBJECT) return 0;
    LRObject *o = (LRObject *)obj.u.ptr;
    if (o->type != LR_OBJ_ARRAY_BUFFER) return 0;
    ArrayBufferMeta *meta = (ArrayBufferMeta *)o->opaque;
    return meta ? meta->is_shared : 0;
}

void *lr_array_buffer_get_opaque(LRValue obj)
{
    if (obj.tag != LR_TYPE_OBJECT) return NULL;
    LRObject *o = (LRObject *)obj.u.ptr;
    if (o->type != LR_OBJ_ARRAY_BUFFER) return NULL;
    ArrayBufferMeta *meta = (ArrayBufferMeta *)o->opaque;
    return meta ? meta->opaque : NULL;
}

uint8_t *lr_get_array_buffer(LRContext *ctx, size_t *psize, LRValue obj)
{
    (void)ctx;
    if (obj.tag != LR_TYPE_OBJECT) return NULL;
    LRObject *o = (LRObject *)obj.u.ptr;
    if (o->type != LR_OBJ_ARRAY_BUFFER) return NULL;
    if (psize) {
        LRValue len_val = lr_get_property_str(ctx, obj, "byteLength");
        int32_t len = 0;
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        *psize = (size_t)len;
    }
    return (uint8_t *)o->extra;
}

void lr_array_buffer_free(void *opaque, void *ptr) { (void)opaque; free(ptr); }

LRValue lr_new_array_buffer_copy(LRContext *ctx, const uint8_t *buf, size_t len)
{
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return LR_VALUE_EXCEPTION;
    memcpy(copy, buf, len);
    return lr_new_array_buffer(ctx, copy, len, lr_array_buffer_free, NULL, 0);
}

LRValue lr_get_typed_array_buffer(LRContext *ctx, LRValue obj,
    size_t *poffset, size_t *plen, size_t *pbytes)
{
    /* Simplified: treat typed arrays as array buffers */
    if (obj.tag != LR_TYPE_OBJECT) return LR_VALUE_EXCEPTION;
    LRObject *o = (LRObject *)obj.u.ptr;
    if (o->type != LR_OBJ_ARRAY_BUFFER) return LR_VALUE_EXCEPTION;
    if (poffset) *poffset = 0;
    if (pbytes) *pbytes = 1;
    if (plen) {
        LRValue len_val = lr_get_property_str(ctx, obj, "byteLength");
        int32_t len = 0;
        lr_to_int32(ctx, &len, len_val);
        lr_free_value(ctx, len_val);
        *plen = (size_t)len;
    }
    return lr_dup_value(ctx, obj);
}

/* ── Promise Capability ───────────────────────────────────────────────── */

LRValue lr_new_promise_capability(LRContext *ctx, LRValue *resolving_funcs)
{
    if (!resolving_funcs) {
        return LR_VALUE_UNDEFINED;
    }

    /* Create a new promise */
    LRValue promise = lr_new_promise(ctx);
    if (lr_is_exception(promise)) {
        resolving_funcs[0] = LR_VALUE_UNDEFINED;
        resolving_funcs[1] = LR_VALUE_UNDEFINED;
        return promise;
    }

    /* Set the prototype from Promise.prototype */
    {
        LRValue global = lr_get_global_object(ctx);
        LRValue promise_ctor = lr_get_property_str(ctx, global, "Promise");
        if (lr_is_object(promise_ctor)) {
            LRValue proto = lr_get_property_str(ctx, promise_ctor, "prototype");
            if (lr_is_object(proto)) {
                lr_set_prototype(ctx, promise, proto);
            }
            lr_free_value(ctx, proto);
        }
        lr_free_value(ctx, promise_ctor);
        lr_free_value(ctx, global);
    }

    /* Create resolve and reject functions that act on this promise */
    LRValue resolve_func = lr_new_cfunction(ctx, promise_resolve_func, "resolve", 1);
    LRValue reject_func = lr_new_cfunction(ctx, promise_reject_func, "reject", 1);

    /* Store the promise in the resolve/reject functions' data */
    if (resolve_func.tag == LR_TYPE_OBJECT) {
        LRObject *robj = (LRObject *)resolve_func.u.ptr;
        if (robj->extra) {
            LRCFunction *cf = (LRCFunction *)robj->extra;
            cf->data = (void *)lr_promise_resolve_data_new(ctx, promise);
        }
    }
    if (reject_func.tag == LR_TYPE_OBJECT) {
        LRObject *robj = (LRObject *)reject_func.u.ptr;
        if (robj->extra) {
            LRCFunction *cf = (LRCFunction *)robj->extra;
            cf->data = (void *)lr_promise_resolve_data_new(ctx, promise);
        }
    }

    resolving_funcs[0] = resolve_func;
    resolving_funcs[1] = reject_func;
    return promise;
}

/* ── Object Opaque Data ───────────────────────────────────────────────── */

void lr_set_opaque(LRValue obj, void *opaque)
{
    if (obj.tag != LR_TYPE_OBJECT || !obj.u.ptr) return;
    LRObject *o = (LRObject *)obj.u.ptr;
    o->opaque = opaque;
    o->opaque_free = NULL; /* reset destructor to default */
}

void lr_set_opaque_with_free(LRValue obj, void *opaque,
                              void (*free_func)(void *opaque))
{
    if (obj.tag != LR_TYPE_OBJECT || !obj.u.ptr) return;
    LRObject *o = (LRObject *)obj.u.ptr;
    o->opaque = opaque;
    o->opaque_free = free_func;
}

void *lr_get_opaque(LRValue obj)
{
    if (obj.tag != LR_TYPE_OBJECT || !obj.u.ptr) return NULL;
    LRObject *o = (LRObject *)obj.u.ptr;
    return o->opaque;
}

/* ── Property Getter/Setter Definition ─────────────────────────────────── */

int lr_define_property_getset(LRContext *ctx, LRValue obj, LRString *atom,
                               LRCFunctionFunc getter, LRCFunctionFunc setter, int flags)
{
    (void)flags;
    if (obj.tag != LR_TYPE_OBJECT || !obj.u.ptr) return -1;

    const char *name = atom ? atom->str : "";

    /* Create function objects for getter and setter */
    LRValue getter_val = LR_VALUE_UNDEFINED;
    LRValue setter_val = LR_VALUE_UNDEFINED;

    if (getter) {
        getter_val = lr_new_cfunction(ctx, getter, name, 0);
    }
    if (setter) {
        char setter_name[128];
        snprintf(setter_name, sizeof(setter_name), "set_%s", name);
        setter_val = lr_new_cfunction(ctx, setter, setter_name, 1);
    }

    /* Store getter and setter as hidden properties */
    char getter_key[128], setter_key[128];
    snprintf(getter_key, sizeof(getter_key), "__get_%s", name);
    snprintf(setter_key, sizeof(setter_key), "__set_%s", name);

    lr_set_property_str(ctx, obj, getter_key, getter_val);
    lr_set_property_str(ctx, obj, setter_key, setter_val);

    /* Define a dummy property that triggers getter/setter lookup */
    lr_set_property_str(ctx, obj, name, LR_VALUE_UNDEFINED);

    return 0;
}