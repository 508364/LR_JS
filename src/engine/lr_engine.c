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
#include "lr_bytecode.h"
#include "../lr_promise.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Forward declaration to avoid pulling in lr_iome586.h which
 * transitively includes <windows.h> → winnt.h → TokenType collision. */
uint64_t lr_iome586_hash64(const uint8_t *data, size_t len);

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

/* Hook installed by the interpreter to release captured closure scopes. */
void (*lr_closure_scope_release)(void *scope, LRContext *ctx) = NULL;

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
    /* Free shape chain */
    if (obj->shape) {
        LRShape *s = obj->shape;
        while (s) { LRShape *prev = s->prev; free(s); s = prev; }
        obj->shape = NULL;
    }
    /* Free class */
    if (obj->class_def) {
        obj->class_def->ref_count--;
    }
    /* Free proto */
    lr_free_value(free_ctx, obj->proto);
    /* Release captured closure scope (interpreter functions) */
    if (obj->def_scope && lr_closure_scope_release) {
        lr_closure_scope_release(obj->def_scope, free_ctx);
        obj->def_scope = NULL;
    }
    /* Free extra data */
    if (obj->extra) {
        if (obj->type == LR_OBJ_FUNCTION) {
            /* AST owned by LREvalUnit, just NULL the pointer */
            obj->extra = NULL;
        } else if (obj->type == LR_OBJ_CFUNCTION) {
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
            /* Dense array data: free elements then the struct */
            LRArrayData *ad = (LRArrayData *)obj->extra;
            if (ad) {
                for (uint32_t i = 0; i < ad->capacity; i++) {
                    lr_free_value(free_ctx, ad->elements[i]);
                }
                free(ad->elements);
                free(ad);
            }
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
    
    /* Initialize dense array storage for O(1) indexed access */
    LRArrayData *ad = (LRArrayData *)calloc(1, sizeof(LRArrayData));
    if (ad) {
        ad->length = 0;
        ad->capacity = 8;
        ad->elements = (LRValue *)calloc(ad->capacity, sizeof(LRValue));
    }
    obj->extra = ad;
    
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    return v;
}

/* ── BigInt Creation / Query ───────────────────────────────────────────── */

LRValue lr_new_bigint(LRContext *ctx, int64_t value)
{
    LRBigIntData *bd = (LRBigIntData *)malloc(sizeof(LRBigIntData));
    if (!bd) return LR_VALUE_UNDEFINED;
    bd->value = value;
    LRValue v = lr_new_object_proto(ctx, ctx->bigint_proto);
    if (v.tag != LR_TYPE_OBJECT) { free(bd); return LR_VALUE_UNDEFINED; }
    LRObject *o = (LRObject *)v.u.ptr;
    o->type = LR_OBJ_BIGINT;
    o->opaque = bd;
    o->opaque_free = free;
    return v;
}

int lr_is_bigint(LRValue v)
{
    if (v.tag != LR_TYPE_OBJECT) return 0;
    LRObject *o = (LRObject *)v.u.ptr;
    return (o->type == LR_OBJ_BIGINT);
}

int lr_to_bigint64(LRContext *ctx, int64_t *out, LRValue v)
{
    (void)ctx;
    if (!lr_is_bigint(v)) return 0;
    LRObject *o = (LRObject *)v.u.ptr;
    LRBigIntData *bd = (LRBigIntData *)o->opaque;
    if (!bd) return 0;
    if (out) *out = bd->value;
    return 1;
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
    case LR_TYPE_INT32: {
        /* Fast integer-to-string without snprintf (hot path for concat) */
        int32_t n = val.u.int32;
        int neg = n < 0;
        if (neg) n = -n;
        char tmp[32];
        int pos = 31;
        tmp[31] = '\0';
        if (n == 0) tmp[--pos] = '0';
        else while (n > 0) { tmp[--pos] = (char)('0' + (n % 10)); n /= 10; }
        if (neg) tmp[--pos] = '-';
        return strdup(tmp + pos);
    }
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
 * Returns a direct pointer to the property VALUE, or NULL on miss.
 * On hit, verifies the cached LRProperty pointer is still valid
 * (key==atom pointer compare, O(1)) and returns &prop->value. */
static LRValue *shape_cache_lookup(LRContext *ctx, LRObject *obj, LRString *atom)
{
    LRRuntime *rt = ctx->rt;
    unsigned int idx = ((uintptr_t)obj ^ (uintptr_t)atom) & (LR_SHAPE_CACHE_SIZE - 1);
    LRShapeCacheEntry *entry = &rt->shape_cache[idx];
    if (entry->valid && entry->obj == obj && entry->prop == atom) {
        LRProperty *prop = entry->prop_ptr;
        /* Verify the property still belongs to this object and key matches */
        if (prop && prop->key == atom) {
            if (prop->flags & (LR_PROP_GETTER | LR_PROP_SETTER))
                return NULL; /* accessor: must go through slow path */
            return &prop->value;
        }
    }
    return NULL;
}

/* Update shape cache with the found (or newly created) property pointer */
static void shape_cache_update(LRContext *ctx, LRObject *obj, LRString *atom,
                                LRProperty *prop)
{
    LRRuntime *rt = ctx->rt;
    unsigned int idx = ((uintptr_t)obj ^ (uintptr_t)atom) & (LR_SHAPE_CACHE_SIZE - 1);
    LRShapeCacheEntry *entry = &rt->shape_cache[idx];
    entry->obj = obj;
    entry->prop = atom;
    entry->prop_ptr = prop;
    entry->valid = 1;
}

/* ── Property Access ──────────────────────────────────────────────────── */

/* Hash bucket index for property lookup */
static int prop_bucket_idx(LRString *key) {
    return ((key->str[0] * 31 + key->len) & (OBJ_PROP_BUCKETS - 1));
}
static void prop_add_bucket(LRObject *obj, LRProperty *prop) {
    int bi = prop_bucket_idx(prop->key);
    prop->bnext = obj->prop_buckets[bi];
    obj->prop_buckets[bi] = prop;
}

/* Get property from object's own properties (bucketed hash lookup) */
static LRProperty *lr_object_find_own_prop(LRObject *obj, LRString *key)
{
    if (!key) return NULL;
    int bi = prop_bucket_idx(key);
    /* Check bucket chain first (O(1) amortized) */
    LRProperty *prop = obj->prop_buckets[bi];
    while (prop) {
        if (prop->key == key || (prop->key && key &&
            prop->key->len == key->len &&
            memcmp(prop->key->str, key->str, key->len) == 0))
            return prop;
        prop = prop->bnext;
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
 * getter/setter are function values (or LR_VALUE_UNDEFINED).
 * If the property already exists as an accessor, the provided sides are
 * merged (so `get x` followed by `set x` keeps both). */
int lr_set_accessor_property(LRContext *ctx, LRValue obj, LRString *atom,
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
            if (prop->flags & (LR_PROP_GETTER | LR_PROP_SETTER)) {
                /* Merge into existing accessor property */
                if (getter.tag == LR_TYPE_OBJECT) {
                    lr_free_value(ctx, prop->value);
                    prop->value = getter;
                    prop->flags |= LR_PROP_GETTER;
                } else {
                    lr_free_value(ctx, getter);
                }
                if (setter.tag == LR_TYPE_OBJECT) {
                    lr_free_value(ctx, prop->setter);
                    prop->setter = setter;
                    prop->flags |= LR_PROP_SETTER;
                } else {
                    lr_free_value(ctx, setter);
                }
                shape_cache_update(ctx, o, atom, prop);
                return 0;
            }
            lr_free_value(ctx, prop->value);
            lr_free_value(ctx, prop->setter);
            prop->value = getter;
            prop->setter = setter;
            prop->flags = flags;
            lr_string_dup(atom);
            shape_cache_update(ctx, o, atom, prop);
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
    prop_add_bucket(o, new_prop);
    shape_cache_update(ctx, o, atom, new_prop);
    ctx->rt->prop_count++;
    ctx->rt->prop_size += sizeof(LRProperty);
    return 0;
}

int lr_set_accessor_property_str(LRContext *ctx, LRValue obj,
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

/* Look up a property on the prototype object of a global constructor
 * (e.g. String.prototype.padStart) for primitive values. */
static LRValue lr_primitive_proto_get(LRContext *ctx, const char *ctor_name,
                                      LRString *atom)
{
    LRValue ctor = lr_get_property_direct(ctx, ctx->global_obj,
                                          lr_new_atom(ctx, ctor_name));
    if (ctor.tag != LR_TYPE_OBJECT) {
        lr_free_value(ctx, ctor);
        return LR_VALUE_UNDEFINED;
    }
    LRValue proto = lr_get_property_direct(ctx, ctor, lr_new_atom(ctx, "prototype"));
    lr_free_value(ctx, ctor);
    if (proto.tag != LR_TYPE_OBJECT) {
        lr_free_value(ctx, proto);
        return LR_VALUE_UNDEFINED;
    }
    LRValue result = lr_get_property_direct(ctx, proto, atom);
    lr_free_value(ctx, proto);
    return result;
}

/* Forward declarations for shape helpers (defined below after lr_property_get) */
static int  shape_get_slot(LRObject *o, LRString *atom);
static void shape_add_prop(LRObject *o, LRString *atom, LRValue val);

LRValue lr_get_property(LRContext *ctx, LRValue obj, LRString *atom)
{
    /* Primitive values delegate to their wrapper prototypes */
    if (obj.tag == LR_TYPE_STRING) {
        LRString *s = (LRString *)obj.u.ptr;
        if (atom && strcmp(atom->str, "length") == 0) {
            return lr_new_int32(ctx, (int32_t)s->len);
        }
        uint32_t idx;
        if (atom && lr_is_numeric_index(atom, &idx)) {
            if (idx < s->len) {
                char b[2] = { s->str[idx], '\0' };
                return lr_new_string(ctx, b);
            }
            return LR_VALUE_UNDEFINED;
        }
        return lr_primitive_proto_get(ctx, "String", atom);
    }
    if (obj.tag == LR_TYPE_INT32 || obj.tag == LR_TYPE_FLOAT64) {
        return lr_primitive_proto_get(ctx, "Number", atom);
    }
    if (obj.tag == LR_TYPE_BOOL) {
        return lr_primitive_proto_get(ctx, "Boolean", atom);
    }
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

    /* Dense array fast path: for LR_OBJ_ARRAY with numeric indices,
     * read directly from the linear buffer (set by lr_set_property_uint32).
     * Also fast-path the "length" property to avoid hash lookup. */
    if (o->type == LR_OBJ_ARRAY) {
        uint32_t idx;
        if (lr_is_numeric_index(atom, &idx)) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            if (ad && idx < ad->length &&
                ad->elements[idx].tag != LR_TYPE_UNDEFINED) {
                return lr_dup_value(ctx, ad->elements[idx]);
            }
        }
        /* Fast-path "length" for dense arrays */
        if (atom && strcmp(atom->str, "length") == 0) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            if (ad)
                return lr_new_int32(ctx, (int32_t)ad->length);
        }
    }

    /* Shape flat-hash O(1) read: direct props[] access */
    if (o->shape && o->props) {
        int slot = shape_get_slot(o, atom);
        if (slot >= 0 && (uint32_t)slot < o->prop_count) {
            LRValue *v = &o->props[slot];
            if (v->tag != LR_TYPE_UNDEFINED) return lr_dup_value(ctx, *v);
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
        shape_cache_update(ctx, o, atom, found);
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
    /* Dense array fast path: read directly from linear buffer */
    if (obj.tag == LR_TYPE_OBJECT && ((LRObject *)obj.u.ptr)->type == LR_OBJ_ARRAY) {
        LRObject *o = (LRObject *)obj.u.ptr;
        LRArrayData *ad = (LRArrayData *)o->extra;
        if (ad && idx < ad->length && ad->elements[idx].tag != LR_TYPE_UNDEFINED)
            return lr_dup_value(ctx, ad->elements[idx]);
        /* Index beyond length or empty slot: fall through to property lookup
         * (prototype chain, sparse array support) */
    }
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u", idx);
        return lr_get_property_str(ctx, obj, buf);
    }
}

/* ── Shape flat-hash O(1) slot lookup + transition cache ─────────────── */

static int shape_get_slot(LRObject *o, LRString *atom) {
    if (!o || !o->shape || !atom) return -1;
    LRShape *s = o->shape;
    if (!s->flat_count) {
        while (s) { if (s->prop_name == atom) return (int)s->slot_index; s = s->prev; }
        return -1;
    }
    unsigned h = ((uintptr_t)atom) & SHAPE_FLAT_MASK;
    for (unsigned i = 0; i < SHAPE_FLAT_SIZE; i++) {
        unsigned idx = (h + i) & SHAPE_FLAT_MASK;
        if (s->flat_keys[idx] == atom) return (int)s->flat_slots[idx];
        if (!s->flat_keys[idx]) return -1;
    }
    while (s) { if (s->prop_name == atom) return (int)s->slot_index; s = s->prev; }
    return -1;
}

#define SHAPE_TRANS_CACHE_SIZE 256
typedef struct { LRShape *old_shape; LRString *key; LRShape *new_shape; uint32_t slot_index; } ShapeTransEntry;
static ShapeTransEntry shape_trans_cache[SHAPE_TRANS_CACHE_SIZE];
static int shape_trans_next = 0;

static void shape_add_prop(LRObject *o, LRString *atom, LRValue val) {
    if (!o || !atom) return;
    LRShape *old_shape = o->shape;
    for (int i = 0; i < SHAPE_TRANS_CACHE_SIZE; i++) {
        ShapeTransEntry *e = &shape_trans_cache[i];
        if (e->old_shape == old_shape && e->key == atom) {
            uint32_t slot = e->slot_index, nc = slot + 1;
            if (nc > o->prop_count) {
                LRValue *np = (LRValue *)realloc(o->props, nc * sizeof(LRValue));
                if (!np) return;
                o->props = np; o->prop_count = nc;
            }
            o->props[slot] = val;
            o->shape = e->new_shape;
            if (e->new_shape) e->new_shape->ref_count++;
            return;
        }
    }
    LRShape *s = (LRShape *)calloc(1, sizeof(LRShape));
    if (!s) return;
    s->prop_name = atom; s->prev = old_shape;
    s->slot_index = o->prop_count; s->ref_count = 1;
    if (old_shape && old_shape->flat_count) {
        memcpy(s->flat_keys, old_shape->flat_keys, sizeof(s->flat_keys));
        memcpy(s->flat_slots, old_shape->flat_slots, sizeof(s->flat_slots));
        s->flat_count = old_shape->flat_count;
    }
    if (s->flat_count < SHAPE_FLAT_SIZE) {
        unsigned h = ((uintptr_t)atom) & SHAPE_FLAT_MASK;
        for (unsigned i = 0; i < SHAPE_FLAT_SIZE; i++) {
            unsigned idx = (h + i) & SHAPE_FLAT_MASK;
            if (!s->flat_keys[idx]) {
                s->flat_keys[idx] = atom;
                s->flat_slots[idx] = (uint16_t)s->slot_index;
                s->flat_count++; break;
            }
        }
    }
    uint32_t nc = o->prop_count + 1;
    LRValue *np = (LRValue *)realloc(o->props, nc * sizeof(LRValue));
    if (!np) { free(s); return; }
    o->props = np; o->props[o->prop_count] = val;
    o->prop_count = nc; o->shape = s;
    int si = shape_trans_next++ % SHAPE_TRANS_CACHE_SIZE;
    shape_trans_cache[si].old_shape = old_shape;
    shape_trans_cache[si].key = atom;
    shape_trans_cache[si].new_shape = s;
    shape_trans_cache[si].slot_index = s->slot_index;
}

int lr_set_property(LRContext *ctx, LRValue obj, LRString *atom, LRValue val)
{
    if (obj.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)obj.u.ptr;

    /* Shape fast path: O(1) direct write to props[] array */
    if (o->shape && o->props) {
        int slot = shape_get_slot(o, atom);
        if (slot >= 0 && (uint32_t)slot < o->prop_count) {
            lr_free_value(ctx, o->props[slot]);
            o->props[slot] = val;
            return 0;
        }
    }

    /* Two-way global binding: an external write to the global object that
     * targets a top-level var/function binding is mirrored back into the
     * interpreter's global scope so bare reads see it. (Reads stay on the
     * scope path; this only handles the globalThis.x = v direction.) */
    if (obj.u.ptr == ctx->global_obj.u.ptr)
        interp_sync_global_binding(ctx, atom->str, val);

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

    /* Dense array: sync numeric index writes to the linear buffer */
    if (o->type == LR_OBJ_ARRAY) {
        uint32_t idx;
        if (lr_is_numeric_index(atom, &idx)) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            if (ad) {
                if (idx >= ad->capacity) {
                    uint32_t new_cap = ad->capacity;
                    while (new_cap <= idx) new_cap *= 2;
                    LRValue *new_elems = (LRValue *)realloc(ad->elements,
                                                            new_cap * sizeof(LRValue));
                    if (!new_elems) goto dense_fallback;
                    memset(new_elems + ad->capacity, 0,
                           (new_cap - ad->capacity) * sizeof(LRValue));
                    ad->elements = new_elems;
                    ad->capacity = new_cap;
                }
                if (ad->elements[idx].tag != LR_TYPE_UNDEFINED)
                    lr_free_value(ctx, ad->elements[idx]);
                ad->elements[idx] = lr_dup_value(ctx, val);
                if (idx >= ad->length) ad->length = idx + 1;
            }
            /* Fall through to property hash for compatibility (e.g. for...in) */
        }
        /* Sync "length" property to dense array */
        if (atom && strcmp(atom->str, "length") == 0) {
            LRArrayData *ad = (LRArrayData *)o->extra;
            if (ad) {
                int32_t new_len = 0;
                lr_to_int32(ctx, &new_len, val);
                if (new_len >= 0) ad->length = (uint32_t)new_len;
            }
        }
    }
dense_fallback:

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
            shape_cache_update(ctx, o, atom, prop);
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
    prop_add_bucket(o, new_prop);
    shape_cache_update(ctx, o, atom, new_prop);

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
    prop_add_bucket(o, new_prop);

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
    /* Dense array fast path: for LR_OBJ_ARRAY, write directly into the
     * linear buffer instead of allocating a per-element LRProperty. */
    if (obj.tag == LR_TYPE_OBJECT && ((LRObject *)obj.u.ptr)->type == LR_OBJ_ARRAY) {
        LRObject *o = (LRObject *)obj.u.ptr;
        LRArrayData *ad = (LRArrayData *)o->extra;
        if (ad) {
            /* Grow capacity if needed (double when full) */
            if (idx >= ad->capacity) {
                uint32_t new_cap = ad->capacity;
                while (new_cap <= idx) new_cap *= 2;
                LRValue *new_elems = (LRValue *)realloc(ad->elements,
                                                        new_cap * sizeof(LRValue));
                if (!new_elems) goto fallback;
                memset(new_elems + ad->capacity, 0,
                       (new_cap - ad->capacity) * sizeof(LRValue));
                ad->elements = new_elems;
                ad->capacity = new_cap;
            }
            /* Free old value at index (if any) */
            if (ad->elements[idx].tag != LR_TYPE_UNDEFINED)
                lr_free_value(ctx, ad->elements[idx]);
            /* Store new value (take ownership) */
            ad->elements[idx] = val;
            /* Update length if index >= current length */
            if (idx >= ad->length) ad->length = idx + 1;
            return 0;
        }
    }
fallback:
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u", idx);
        return lr_set_property_str(ctx, obj, buf, val);
    }
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
    LRProperty *found = lr_object_find_own_prop(o, atom);
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
    LRProperty *found = lr_object_find_own_prop(o, atom);
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

    /* JS-defined function or class (interpreted via AST callback) */
    if (obj->type == LR_OBJ_FUNCTION || obj->type == LR_OBJ_BYTECODE_FUNC) {
        /* Fetch (or lazily create) the .prototype object */
        LRValue proto_val = lr_get_property_str(ctx, func, "prototype");
        if (!lr_is_object(proto_val)) {
            lr_free_value(ctx, proto_val);
            proto_val = lr_new_object(ctx);
            lr_set_property_str(ctx, proto_val, "constructor",
                                lr_dup_value(ctx, func));
            lr_set_property_str(ctx, func, "prototype",
                                lr_dup_value(ctx, proto_val));
        }

        /* OrdinaryCreateFromConstructor */
        LRValue new_obj = lr_new_object_proto(ctx, proto_val);
        lr_free_value(ctx, proto_val);

        LRValue result = LR_VALUE_UNDEFINED;
        if (ctx->call_js_function) {
            ctx->current_func = func;
            result = ctx->call_js_function(ctx, func, new_obj, argc, argv);
            ctx->current_func = LR_VALUE_UNDEFINED;
        }

        if (lr_is_exception(result)) {
            lr_free_value(ctx, new_obj);
            return result;
        }
        /* If the constructor explicitly returns an object, use it */
        if (lr_is_object(result)) {
            if (result.u.ptr != new_obj.u.ptr) {
                lr_free_value(ctx, new_obj);
            }
            return result;
        }
        lr_free_value(ctx, result);
        return new_obj;
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

/* Build the error object that `catch (e)` will observe.
 *
 * `kind` is the ECMAScript constructor name ("TypeError", "SyntaxError", …).
 * We instantiate the real global constructor when it exists so that both
 * `e.name` and `e instanceof TypeError` behave; if the constructor is not
 * registered yet (errors thrown during bootstrap), fall back to a plain
 * object carrying `name`/`message` so the value is still inspectable. */
static LRValue lr_build_error_object(LRContext *ctx, const char *kind,
                                     const char *msg)
{
    LRValue global = lr_get_global_object(ctx);
    LRValue ctor   = lr_get_property_str(ctx, global, kind);

    LRValue err = LR_VALUE_UNDEFINED;
    if (lr_is_function(ctx, ctor)) {
        LRValue arg = lr_new_string(ctx, msg);
        err = lr_call_constructor(ctx, ctor, 1, &arg);
        lr_free_value(ctx, arg);
        /* A failed construction must not mask the original error. */
        if (err.tag != LR_TYPE_OBJECT) {
            ctx->current_exception = LR_VALUE_UNDEFINED;
            err = LR_VALUE_UNDEFINED;
        }
    }

    if (err.tag != LR_TYPE_OBJECT) {
        err = lr_new_object(ctx);
        if (err.tag == LR_TYPE_OBJECT) {
            lr_set_property_str(ctx, err, "name",    lr_new_string(ctx, kind));
            lr_set_property_str(ctx, err, "message", lr_new_string(ctx, msg));
        }
    }

    lr_free_value(ctx, ctor);
    lr_free_value(ctx, global);
    return err;
}

static LRValue lr_throw_error_named(LRContext *ctx, const char *kind,
                                    const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    free(ctx->error_message);
    ctx->error_message = strdup(buf);

    /* Publish the thrown value so interp_capture_exception / lr_get_exception
     * hand the caller a real error instead of a bare sentinel. */
    ctx->current_exception = lr_build_error_object(ctx, kind, buf);

    LRValue exc;
    exc.tag   = LR_TYPE_EXCEPTION;
    exc.u.ptr = NULL;
    return exc;
}

#define LR_DEFINE_THROWER(fn, kind)                                  \
    LRValue fn(LRContext *ctx, const char *fmt, ...)                 \
    {                                                                \
        va_list ap;                                                  \
        va_start(ap, fmt);                                           \
        LRValue ret = lr_throw_error_named(ctx, kind, fmt, ap);      \
        va_end(ap);                                                  \
        return ret;                                                  \
    }

LR_DEFINE_THROWER(lr_throw_type_error,      "TypeError")
LR_DEFINE_THROWER(lr_throw_reference_error, "ReferenceError")
LR_DEFINE_THROWER(lr_throw_range_error,     "RangeError")
LR_DEFINE_THROWER(lr_throw_syntax_error,    "SyntaxError")
LR_DEFINE_THROWER(lr_throw_internal_error,  "InternalError")

#undef LR_DEFINE_THROWER

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

    /* Free the resolved-module registry. Each LREvalUnit has just dropped its
     * reference to its module namespace, so releasing the registry's reference
     * is the final one and frees the namespace object cleanly. */
    {
        LRModuleCacheEntry *e = (LRModuleCacheEntry *)ctx->module_registry;
        while (e) {
            LRModuleCacheEntry *next = e->next;
            if (e->def) {
                if (e->def->obj)
                    lr_free_value(ctx, (LRValue){ .tag = LR_TYPE_OBJECT, .u.ptr = e->def->obj });
                free(e->def->name);
                free(e->def);
            }
            free(e->name);
            free(e);
            e = next;
        }
        ctx->module_registry = NULL;
    }

    /* Release all captured closure scopes while refcounts are still
     * consistent. This must happen BEFORE the force-free object loop in
     * lr_runtime_free, otherwise the def_scope hook would touch values
     * pointing at already force-freed objects (heap corruption).
     * Releasing a scope can cascade-free objects (unlinking them from
     * obj_list), so restart the scan after each release. */
    if (rt && lr_closure_scope_release) {
        int again = 1;
        while (again) {
            again = 0;
            for (LRObject *obj = rt->obj_list; obj; obj = obj->gc_next) {
                if (obj->def_scope) {
                    void *sc = obj->def_scope;
                    obj->def_scope = NULL;
                    lr_closure_scope_release(sc, ctx);
                    again = 1;
                    break; /* obj_list may have changed; restart */
                }
            }
        }
    }

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
    int      is_module;   /* 1 if this unit is a module (drives namespace) */
    LRValue  ns;          /* module namespace object when is_module (owns a ref) */
    LRContext *ctx;
    struct LREvalUnit *next;
    BCProgram  *bc_prog;    /* compiled bytecode (nullable; IOME586 warm-path) */
    uint8_t    *bc_ser;     /* serialized bytecode for IOME586 */
    size_t      bc_ser_len;
} LREvalUnit;

/* Persistent per-context interpreter state. Created on first eval and
 * kept alive until lr_free_context, so that async callbacks (timers,
 * worker messages, promise jobs) can call JS functions after the initial
 * script evaluation has returned. */
typedef struct LRPersistentInterp {
    Interpreter interp;
    LREvalUnit *units;
    /* One-shot request from lr_engine_eval_code(direct): keep the caller's
     * lexical scope instead of resetting to the global scope. Consumed (and
     * cleared) by lr_engine_exec_unit on entry, so nesting is safe. */
    int         keep_caller_scope;
    /* IOME586 warm-cache: compiled bytecode keyed by source hash. When the
     * same source is evaluated multiple times within the same process
     * (e.g. eval in a loop, or CLI re-runs via the IOME586 loader), we
     * skip parsing and reuse the cached bytecode program. */
    struct      LRCompiledCacheEntry {
        uint64_t  hash;
        BCProgram *prog;
        struct LRCompiledCacheEntry *next;
    }          *compiled_cache[64];
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
    if (unit->ns.tag == LR_TYPE_OBJECT) lr_free_value(unit->ctx, unit->ns);
    if (unit->bc_prog) bc_free_program(unit->bc_prog);
    free(unit->bc_ser);
    free(unit);
}

void lr_context_free_persistent_interp(LRContext *ctx)
{
    LRPersistentInterp *ps = (LRPersistentInterp *)ctx->persistent_interp;
    if (!ps) return;
    interp_free(&ps->interp);

    /* Before freeing AST units, NULL all LR_OBJ_FUNCTION->extra pointers
     * that point into those ASTs, preventing dangling pointer access. */
    LRObject *o = ctx->rt->obj_list;
    while (o) {
        if (o->type == LR_OBJ_FUNCTION && o->extra) {
            o->extra = NULL;
        }
        o = o->gc_next;
    }

    /* Free compiled cache entries (the BCPrograms are freed by lr_eval_unit_free) */
    for (int i = 0; i < 64; i++) {
        struct LRCompiledCacheEntry *ce = ps->compiled_cache[i];
        while (ce) {
            struct LRCompiledCacheEntry *next = ce->next;
            free(ce);
            ce = next;
        }
        ps->compiled_cache[i] = NULL;
    }
    LREvalUnit *u = ps->units;
    while (u) {
        LREvalUnit *next = u->next;
        lr_eval_unit_free(u);
        u = next;
    }
    free(ps);
    ctx->persistent_interp = NULL;
}

/* ── Evaluation (parse + execute; optional AST serialization) ────────────── */

/* Parse source into a retained compilation unit. Returns the unit (with AST
 * and parser) on success, or NULL after setting a syntax/oom exception. */
static LREvalUnit *lr_engine_parse_source(LRContext *ctx, const char *input,
                                          size_t input_len, int is_module,
                                          const char *filename)
{
    LREvalUnit *unit = (LREvalUnit *)calloc(1, sizeof(LREvalUnit));
    if (!unit) { lr_throw_internal_error(ctx, "eval: out of memory"); return NULL; }
    unit->ctx = ctx;
    unit->is_module = is_module;
    unit->ns = LR_VALUE_UNDEFINED;
    unit->src = (char *)malloc(input_len + 1);
    unit->parser = (Parser *)calloc(1, sizeof(Parser));
    if (!unit->src || !unit->parser) {
        free(unit->src);
        free(unit->parser);
        free(unit);
        lr_throw_internal_error(ctx, "eval: out of memory");
        return NULL;
    }
    memcpy(unit->src, input, input_len);
    unit->src[input_len] = '\0';

    Lexer lex;
    lexer_init(&lex, unit->src, input_len);
    parser_init(unit->parser, &lex);

    unit->ast = parse_program(unit->parser);

    /* Eager precompile: compile all function bodies to bytecode after parse.
     * One-time cost; every function call thereafter hits the bytecode cache. */
    if (unit->ast)
        interp_precompile_all_bodies(unit->ast);

    if (!unit->ast || unit->parser->has_error) {
        const char *err_msg = parser_get_error(unit->parser, NULL, NULL);
        /* Inside eval() a SyntaxError is a normal, catchable outcome, so it
         * must not be reported on stderr — only genuinely uncaught top-level
         * parse failures are worth printing. */
        if (err_msg && ctx->eval_depth == 0) {
            fprintf(stderr, "[LR_JS] Parse error: %s\n", err_msg);
        }
        /* err_msg points into the parser, which lr_eval_unit_free destroys,
         * so snapshot it before releasing the unit. */
        char msg[512];
        snprintf(msg, sizeof(msg), "%s",
                 err_msg ? err_msg : (filename ? filename : "parse error"));

        lr_eval_unit_free(unit);
        lr_throw_syntax_error(ctx, "%s", msg);
        return NULL;
    }
    return unit;
}

/* Execute an already-parsed unit (parse + execute split for the cache). */
static LRValue lr_engine_exec_unit(LRContext *ctx, LREvalUnit *unit,
                                   int is_module, const char *filename)
{
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
        interp_reattach(&ps->interp, ctx);
    }
    Interpreter *interp = &ps->interp;

    InterpScope *saved_scope = interp->current_scope;
    int saved_is_module = interp->is_module;
    LRObject *saved_ns = interp->module_ns;
    const char *saved_filename = interp->filename;
    LRValue saved_meta = interp->import_meta;
    /* Direct eval keeps the caller's scope chain so the evaluated code can
     * read and write the enclosing function's locals (ES PerformEval). */
    int keep_scope = ps->keep_caller_scope;
    ps->keep_caller_scope = 0;
    if (!keep_scope)
        interp->current_scope = interp->global_scope;
    interp->is_module = is_module;
    interp->filename = filename;
    interp->import_meta = LR_VALUE_UNDEFINED;
    if (is_module) {
        if (unit->ns.tag != LR_TYPE_OBJECT)
            unit->ns = lr_new_object(ctx);
        interp->module_ns = (LRObject *)unit->ns.u.ptr;
    }

    /* Compile AST → bytecode (mandatory as of v0.1.1+).
     * The AST tree-walking interpreter is retired; the bytecode VM
     * (direct/indirect threaded) is the sole execution engine.
     *
     * IOME586 warm-cache: check the compiled cache first to skip
     * recompilation when the same source is evaluated again. */
    LRValue result = LR_VALUE_UNDEFINED;
    if (!unit->bc_prog) {
        /* Check the compiled cache (keyed by source hash) */
        uint64_t src_hash = lr_iome586_hash64((const uint8_t *)unit->src,
                                              strlen(unit->src));
        uint32_t bucket = (uint32_t)(src_hash & 63);
        struct LRCompiledCacheEntry *ce = ps->compiled_cache[bucket];
        while (ce) {
            if (ce->hash == src_hash) {
                unit->bc_prog = ce->prog;
                break;
            }
            ce = ce->next;
        }

        if (!unit->bc_prog) {
            unit->bc_prog = bc_new_program();
            if (unit->bc_prog) {
                if (bc_compile(unit->bc_prog, unit->ast, is_module) != 0) {
                    bc_free_program(unit->bc_prog);
                    unit->bc_prog = NULL;
                    interp->error_flag = 1;
                    snprintf(interp->error_message, sizeof(interp->error_message),
                             "bytecode compilation failed");
                    goto exec_done;
                }
                /* Serialize for IOME586 archive. */
                unit->bc_ser = bc_serialize(unit->bc_prog, &unit->bc_ser_len);
                /* Store in compiled cache for warm re-use. */
                ce = (struct LRCompiledCacheEntry *)calloc(1,
                    sizeof(struct LRCompiledCacheEntry));
                if (ce) {
                    ce->hash = src_hash;
                    ce->prog = unit->bc_prog;
                    ce->next = ps->compiled_cache[bucket];
                    ps->compiled_cache[bucket] = ce;
                }
            }
        }
    }

    /* Execute: bytecode VM only (direct/indirect threaded as of v0.1.1+). */
    if (unit->bc_prog && unit->bc_prog->compiled) {
        result = bc_execute(unit->bc_prog, ctx);
    } else {
        interp->error_flag = 1;
        snprintf(interp->error_message, sizeof(interp->error_message),
                 "bytecode program not available");
    }

exec_done:

    interp->current_scope = saved_scope;
    interp->is_module = saved_is_module;
    interp->module_ns = saved_ns;
    if (interp->import_meta.tag == LR_TYPE_OBJECT)
        lr_free_value(ctx, interp->import_meta);
    interp->import_meta = saved_meta;
    interp->filename = saved_filename;

    if (lr_is_job_pending(ctx->rt)) {
        int job_count = 0;
        while (lr_is_job_pending(ctx->rt) && job_count < 1000) {
            LRContext *job_ctx = NULL;
            if (lr_execute_pending_job(ctx->rt, &job_ctx) != 0) break;
            job_count++;
        }
    }

    /* Retain the unit so JS functions/closures keep referencing a live AST. */
    unit->next = ps->units;
    ps->units = unit;

    /* Surface an uncaught error/throw to the caller. Without this the
     * interpreter's error_flag stays internal, lr_exec_file_cached sees a
     * plain value and returns 0, and the CLI exits silently with status 0
     * even though the script threw. */
    if (interp->error_flag || interp->exception_pending) {
        lr_free_value(ctx, result);

        if (interp->exception_pending) {
            /* A JS `throw` — hand the thrown value over as the exception. */
            ctx->current_exception = interp->exception_value;
            interp->exception_value = LR_VALUE_UNDEFINED;
        } else if (!ctx->error_message) {
            /* Engine-raised error — carry its message across. */
            ctx->error_message = strdup(interp->error_message[0]
                                        ? interp->error_message
                                        : "uncaught error");
        }

        interp->error_flag = 0;
        interp->exception_pending = 0;
        interp->error_message[0] = '\0';
        return LR_VALUE_EXCEPTION;
    }

    return result;
}

LRValue lr_engine_eval_source(LRContext *ctx, const char *input, size_t input_len,
                              int is_module, const char *filename,
                              uint8_t **out_bc, size_t *out_bc_len)
{
    LREvalUnit *unit = lr_engine_parse_source(ctx, input, input_len, is_module, filename);
    if (!unit) return LR_VALUE_EXCEPTION;
    if (out_bc && out_bc_len) {
        *out_bc = lr_ast_serialize(unit->ast, out_bc_len);
    }
    return lr_engine_exec_unit(ctx, unit, is_module, filename);
}

/* ── IOME586 support: split parse / execute phases ─────────────────────── */

void *lr_engine_parse_unit(LRContext *ctx, const char *input, size_t input_len,
                           int is_module, const char *filename,
                           uint8_t **out_bc, size_t *out_bc_len)
{
    LREvalUnit *unit = lr_engine_parse_source(ctx, input, input_len,
                                              is_module, filename);
    if (!unit) return NULL;
    if (out_bc && out_bc_len)
        *out_bc = lr_ast_serialize(unit->ast, out_bc_len);
    return unit;
}

LRValue lr_engine_exec_unit_handle(LRContext *ctx, void *unit_handle,
                                   int is_module, const char *filename)
{
    if (!unit_handle) return LR_VALUE_EXCEPTION;
    return lr_engine_exec_unit(ctx, (LREvalUnit *)unit_handle,
                               is_module, filename);
}

const ASTNode *lr_engine_unit_ast_handle(void *unit_handle)
{
    return unit_handle ? ((LREvalUnit *)unit_handle)->ast : NULL;
}

const uint8_t *lr_engine_unit_bc_data(void *unit_handle, size_t *out_len)
{
    if (!unit_handle) { if (out_len) *out_len = 0; return NULL; }
    LREvalUnit *u = (LREvalUnit *)unit_handle;
    if (out_len) *out_len = u->bc_ser_len;
    return u->bc_ser;
}

int lr_engine_unit_load_bytecode(void *unit_handle,
                                  const uint8_t *data, size_t len)
{
    if (!unit_handle || !data || !len) return -1;
    LREvalUnit *u = (LREvalUnit *)unit_handle;
    if (u->bc_prog) bc_free_program(u->bc_prog);
    u->bc_prog = bc_deserialize(data, len);
    return u->bc_prog ? 0 : -1;
}

int lr_engine_program_count(const ASTNode *program)
{
    if (!program || program->type != AST_PROGRAM) return 0;
    return program->u.list.count > 0 ? program->u.list.count : 0;
}

int lr_engine_program_node_info(const ASTNode *program, int i,
                                uint16_t *out_type, uint32_t *out_line,
                                const char **out_binding_name)
{
    if (out_type) *out_type = 0xFFFF;
    if (out_line) *out_line = 0;
    if (out_binding_name) *out_binding_name = NULL;
    if (!program || program->type != AST_PROGRAM) return -1;
    if (i < 0 || i >= program->u.list.count) return -1;
    const ASTNode *n = program->u.list.items[i];
    if (!n) return 0;
    if (out_type) *out_type = (uint16_t)n->type;
    if (out_line) *out_line = (uint32_t)n->token.line;
    if (out_binding_name) {
        switch (n->type) {
        case AST_FUNC_DECL:  *out_binding_name = n->u.func.name; break;
        case AST_CLASS_DECL: *out_binding_name = n->u.class_decl.name; break;
        case AST_VAR_DECL:
            if (n->u.var_decl.nvars > 0 && n->u.var_decl.vars[0] &&
                n->u.var_decl.vars[0]->type == AST_VAR_DECLARATOR) {
                const ASTNode *v = n->u.var_decl.vars[0]->u.declarator.var;
                if (v && v->type == AST_IDENTIFIER)
                    *out_binding_name = v->u.ident.name;
            }
            break;
        default: break;
        }
    }
    return 0;
}

LRValue lr_engine_eval_ast(LRContext *ctx, ASTNode *ast, Parser *parser,
                           int is_module, const char *filename)
{
    LREvalUnit *unit = (LREvalUnit *)calloc(1, sizeof(LREvalUnit));
    if (!unit) return lr_throw_internal_error(ctx, "eval: out of memory");
    unit->ast = ast;
    unit->parser = parser;
    unit->src = NULL;
    /* bc_prog stays NULL → lr_engine_exec_unit will compile the AST */
    return lr_engine_exec_unit(ctx, unit, is_module, filename);
}

/* Evaluate an already-parsed AST with pre-compiled bytecode (IOME586 warm path).
 * When bc_data/bc_len is provided, skips bc_compile entirely. */
LRValue lr_engine_eval_ast_with_bytecode(LRContext *ctx, ASTNode *ast,
                                          Parser *parser, int is_module,
                                          const char *filename,
                                          const uint8_t *bc_data, size_t bc_len)
{
    LREvalUnit *unit = (LREvalUnit *)calloc(1, sizeof(LREvalUnit));
    if (!unit) return lr_throw_internal_error(ctx, "eval: out of memory");
    unit->ast = ast;
    unit->parser = parser;
    unit->src = NULL;
    /* Deserialize bytecode from IOME586 archive so we skip compilation */
    if (bc_data && bc_len > 0) {
        unit->bc_prog = bc_deserialize(bc_data, bc_len);
        if (unit->bc_prog) {
            unit->bc_prog->compiled = 1;
            /* Keep serialized copy for the archive */
            unit->bc_ser = (uint8_t *)malloc(bc_len);
            if (unit->bc_ser) {
                memcpy(unit->bc_ser, bc_data, bc_len);
                unit->bc_ser_len = bc_len;
            }
        }
    }
    return lr_engine_exec_unit(ctx, unit, is_module, filename);
}

/* Forward declaration (defined below lr_engine_eval). */
static LRValue lr_new_script(LRContext *ctx, LREvalUnit *unit);

LRValue lr_engine_eval(LRContext *ctx, const char *input, size_t input_len,
                const char *filename, int flags)
{
    int is_module = flags & JS_EVAL_TYPE_MODULE;

    /* COMPILE_ONLY: parse the source but do not run it; return a script
     * object that lr_engine_eval_function can execute later. */
    if (flags & JS_EVAL_FLAG_COMPILE_ONLY) {
        LREvalUnit *unit = lr_engine_parse_source(ctx, input, input_len,
                                                  is_module, filename);
        if (!unit) return LR_VALUE_EXCEPTION;
        return lr_new_script(ctx, unit);
    }

    return lr_engine_eval_source(ctx, input, input_len, is_module, filename, NULL, NULL);
}

/* Wrap a compiled unit in a script object. The object owns the unit; once the
 * unit is executed via lr_engine_eval_function, ownership transfers to the
 * persistent interpreter and the object's opaque is cleared. */
static LRValue lr_new_script(LRContext *ctx, LREvalUnit *unit)
{
    LRValue obj = lr_new_object(ctx);
    if (obj.tag != LR_TYPE_OBJECT) {
        lr_eval_unit_free(unit);
        return obj;
    }
    LRObject *o = (LRObject *)obj.u.ptr;
    o->type = LR_OBJ_SCRIPT;
    o->opaque = unit;
    o->opaque_free = (void (*)(void *))lr_eval_unit_free;
    return obj;
}

LRValue lr_engine_eval_function(LRContext *ctx, LRValue func_obj)
{
    if (func_obj.tag != LR_TYPE_OBJECT)
        return LR_VALUE_UNDEFINED;
    LRObject *o = (LRObject *)func_obj.u.ptr;
    if (!o || o->type != LR_OBJ_SCRIPT)
        return LR_VALUE_UNDEFINED;
    LREvalUnit *unit = (LREvalUnit *)o->opaque;
    if (!unit)
        return LR_VALUE_UNDEFINED;
    /* Ownership of the unit now moves to the persistent interpreter. */
    o->opaque = NULL;
    return lr_engine_exec_unit(ctx, unit, unit->is_module, NULL);
}

LRValue lr_engine_run_module(LRContext *ctx, const char *input, size_t input_len,
                     const char *filename, LRValue *out_ns)
{
    LREvalUnit *unit = lr_engine_parse_source(ctx, input, input_len, 1, filename);
    if (!unit) {
        if (out_ns) *out_ns = LR_VALUE_UNDEFINED;
        return LR_VALUE_EXCEPTION;
    }
    LRValue result = lr_engine_exec_unit(ctx, unit, 1, filename);
    if (out_ns)
        *out_ns = (unit->ns.tag == LR_TYPE_OBJECT)
                    ? lr_dup_value(ctx, unit->ns) : LR_VALUE_UNDEFINED;
    return result;
}

/* ── new Function(…) helper ─────────────────────────────────────────────── */

LRValue lr_engine_build_function(LRContext *ctx, int nparams,
                                  const char **params, const char *body)
{
    /* Build source: (function(p1, p2, ...) { body }) */
    char buf[65536];
    int off = snprintf(buf, sizeof(buf), "(function(");
    for (int i = 0; i < nparams && off < (int)sizeof(buf) - 1; i++) {
        if (i > 0) off += snprintf(buf + off, sizeof(buf) - off, ",");
        off += snprintf(buf + off, sizeof(buf) - off, "%s",
                        params[i] ? params[i] : "");
    }
    snprintf(buf + off, sizeof(buf) - off, "){%s})", body ? body : "");
    LRValue r = lr_engine_eval_source(ctx, buf, strlen(buf), 0,
                                      "<anonymous>", NULL, NULL);
    if (r.tag != LR_TYPE_OBJECT) return r;

    LRObject *obj = (LRObject *)r.u.ptr;
    if (obj->type != LR_OBJ_FUNCTION) return r;

    /* new Function() always runs in global scope, never captures a lexical
     * closure. Clear def_scope so the interpreter doesn't try to use it. */
    if (obj->def_scope && lr_closure_scope_release) {
        lr_closure_scope_release(obj->def_scope, ctx);
        obj->def_scope = NULL;
    }

    /* Eagerly compile the function body into a BCProgram so the function
     * can execute independently of the eval unit's AST. Store it in
     * obj->opaque; the interpreter's function-call path checks opaque
     * for a BCProgram before falling back to the AST via obj->extra. */
    ASTNode *func_ast = (ASTNode *)obj->extra;
    if (func_ast) {
        BCProgram *prog = bc_new_program();
        if (prog && bc_compile(prog, func_ast->u.func.body, 0) == 0) {
            obj->opaque = prog;
            obj->opaque_free = (void (*)(void *))bc_free_program;
        } else if (prog) {
            bc_free_program(prog);
        }
    }

    /* Break the AST dependency: new Function objects must not hold
     * pointers into the eval unit's AST, which will be freed below. */
    obj->extra = NULL;

    /* Immediately unlink and free the eval unit so it does not accumulate
     * in the persistent interpreter's unit list. The BCProgram in
     * obj->opaque is the only thing the function needs. */
    LRPersistentInterp *ps = (LRPersistentInterp *)ctx->persistent_interp;
    if (ps) {
        LREvalUnit **pp = &ps->units;
        while (*pp) {
            LREvalUnit *u = *pp;
            /* Find the unit that owns this function's AST. Since
             * lr_engine_eval_source creates exactly one unit for this
             * source, we free the most recently appended unit (head). */
            *pp = u->next;
            lr_eval_unit_free(u);
            break;  /* one unit per new Function call */
        }
    }

    return r;
}

/* ── eval() with a per-call runtime sandbox ─────────────────────────────
 *
 * Each eval() gets its own sandbox frame. The frame is what makes evaluating
 * attacker-supplied source safe(r) than splicing it into the host script:
 *
 *   - nesting depth is capped, so eval("eval(...)") recursion cannot blow
 *     the C stack;
 *   - a wall-clock budget is installed on the interpreter, so a runaway
 *     loop inside eval terminates even when the host script had no timeout;
 *   - the interpreter's control-flow accounting (break/continue/return,
 *     statement counter) is snapshotted on entry and restored on exit, so a
 *     malformed fragment cannot leave the host interpreter in a torn state;
 *   - frames are linked, so nested evals inherit the tightest deadline.
 */
typedef struct LREvalSandbox {
    struct LREvalSandbox *parent;
    int      depth;
    /* interpreter state captured on entry */
    int      saved_timeout_ms;
    int      saved_stmt_counter;
    int      saved_break_target;
    int      saved_continue_target;
    int      saved_return_target;
    int      saved_has_returned;
} LREvalSandbox;

static int lr_eval_sandbox_enter(LRContext *ctx, Interpreter *interp,
                                 LREvalSandbox *sb)
{
    int max_depth = ctx->max_eval_depth > 0 ? ctx->max_eval_depth
                                            : LR_EVAL_MAX_DEPTH_DEFAULT;
    if (ctx->eval_depth >= max_depth)
        return -1;

    sb->parent                = (LREvalSandbox *)ctx->eval_sandbox;
    sb->depth                 = ctx->eval_depth + 1;
    sb->saved_timeout_ms      = interp->timeout_ms;
    sb->saved_stmt_counter    = interp->stmt_counter;
    sb->saved_break_target    = interp->break_target;
    sb->saved_continue_target = interp->continue_target;
    sb->saved_return_target   = interp->return_target;
    sb->saved_has_returned    = interp->has_returned;

    /* Install the sandbox deadline: the tightest of the host timeout and the
     * configured eval budget. 0 on both sides means "no limit". */
    int budget = ctx->eval_timeout_ms > 0 ? ctx->eval_timeout_ms
                                          : interp->timeout_ms;
    if (interp->timeout_ms > 0 && budget > interp->timeout_ms)
        budget = interp->timeout_ms;
    interp->timeout_ms  = budget;
    interp->stmt_counter = 0;

    /* A fresh fragment must not inherit a pending break/continue/return. */
    interp->break_target    = 0;
    interp->continue_target = 0;
    interp->return_target   = 0;
    interp->has_returned    = 0;

    ctx->eval_sandbox = sb;
    ctx->eval_depth   = sb->depth;
    return 0;
}

static void lr_eval_sandbox_leave(LRContext *ctx, Interpreter *interp,
                                  LREvalSandbox *sb)
{
    interp->timeout_ms      = sb->saved_timeout_ms;
    interp->stmt_counter    = sb->saved_stmt_counter;
    interp->break_target    = sb->saved_break_target;
    interp->continue_target = sb->saved_continue_target;
    interp->return_target   = sb->saved_return_target;
    interp->has_returned    = sb->saved_has_returned;

    ctx->eval_sandbox = sb->parent;
    ctx->eval_depth   = sb->depth - 1;
}

LRValue lr_engine_eval_code(LRContext *ctx, const char *src, size_t src_len,
                            int direct)
{
    if (!ctx || !src)
        return LR_VALUE_UNDEFINED;

    if (ctx->eval_disabled)
        return lr_throw_type_error(ctx, "eval is disabled in this context");

    /* eval() needs a live interpreter: without one there is no caller scope
     * to share and nothing has been executed yet, so fall back to a plain
     * global-scope evaluation. */
    LRPersistentInterp *ps = (LRPersistentInterp *)ctx->persistent_interp;
    if (!ps)
        return lr_engine_eval_source(ctx, src, src_len, 0, "<eval>", NULL, NULL);

    Interpreter *interp = &ps->interp;

    LREvalSandbox sb;
    if (lr_eval_sandbox_enter(ctx, interp, &sb) != 0)
        return lr_throw_range_error(ctx, "maximum eval nesting depth exceeded");

    /* Parse first: a SyntaxError must not disturb the caller's scope. */
    LREvalUnit *unit = lr_engine_parse_source(ctx, src, src_len, 0, "<eval>");
    if (!unit) {
        lr_eval_sandbox_leave(ctx, interp, &sb);
        return LR_VALUE_EXCEPTION;   /* parse_source already threw */
    }

    /* Direct eval shares the caller's scope chain; indirect eval does not. */
    ps->keep_caller_scope = direct ? 1 : 0;

    /* eval_program returns the last statement's value, which is exactly the
     * script completion value eval() must produce. */
    LRValue result = lr_engine_exec_unit(ctx, unit, 0, "<eval>");

    lr_eval_sandbox_leave(ctx, interp, &sb);
    return result;
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
    /* FNV-1a 32-bit hash */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)str[i]) * 16777619u;
    int bi = (int)(h & (ATOM_HASH_SIZE - 1));

    /* Check hash cache first (O(1)) */
    LRString *cached = ctx->atom_hash[bi];
    if (cached && cached->len == len && memcmp(cached->str, str, len) == 0)
        return lr_string_dup(cached);

    /* Fallback: linear search in atom table */
    for (uint32_t i = 0; i < ctx->atom_count; i++) {
        if (ctx->atom_table[i]->len == len &&
            memcmp(ctx->atom_table[i]->str, str, len) == 0) {
            ctx->atom_hash[bi] = ctx->atom_table[i]; /* update cache */
            return lr_string_dup(ctx->atom_table[i]);
        }
    }

    /* Create new atom */
    LRString *atom = lr_string_alloc(ctx->rt, str, len);
    if (!atom) return NULL;
    atom->is_atom = 1;

    /* Add to atom table + hash cache */
    if (ctx->atom_count >= ctx->atom_capacity) {
        ctx->atom_capacity *= 2;
        ctx->atom_table = (LRString **)realloc(ctx->atom_table,
            ctx->atom_capacity * sizeof(LRString *));
    }
    ctx->atom_table[ctx->atom_count++] = atom;
    ctx->atom_hash[bi] = atom;  /* cache for next lookup */
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
    if (val.tag == LR_TYPE_OBJECT) {
        LRObject *o = (LRObject *)val.u.ptr;
        if (o && o->opaque) {
            const char *op = (const char *)o->opaque;
            /* Well-known symbols (Symbol.iterator, Symbol.asyncIterator, ...)
             * are represented as plain objects whose opaque data is the string
             * "Symbol.<name>". Key properties by that name so the iterable
             * protocol (which looks up the string "Symbol.iterator") can
             * locate them, and so obj[Symbol.iterator] is symmetric. */
            if (op && strncmp(op, "Symbol.", 7) == 0)
                return lr_new_atom(ctx, op);
        }
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

/* ── AST dependency-based parallel split ─────────────────────────────────
 * Scans the top-level AST, collects function/class declarations as prefix,
 * then splits remaining statements into independent groups where each group
 * depends only on the prefix. Returns NULL-terminated string chunks.     */

char **lr_engine_split_source(LRContext *ctx, const char *input, size_t input_len,
                               int num_threads, int *out_count,
                               size_t *out_prefix_len)
{
    if (!ctx || !input || input_len < 100 || num_threads < 2) {
        if (out_count) *out_count = 0;
        if (out_prefix_len) *out_prefix_len = 0;
        return NULL;
    }
    if (num_threads > 16) num_threads = 16;

    LREvalUnit *unit = lr_engine_parse_source(ctx, input, input_len, 0, "");
    if (!unit || !unit->ast || unit->ast->type != AST_PROGRAM) {
        if (unit) lr_eval_unit_free(unit);
        if (out_count) *out_count = 0;
        return NULL;
    }
    ASTNode *prog = unit->ast;
    int n = prog->u.list.count;
    if (n < num_threads * 2) { lr_eval_unit_free(unit); return NULL; }

    /* Find prefix end: all function/class declarations must be shared */
    int prefix_end = 0;
    for (int i = 0; i < n; i++) {
        ASTNode *s = prog->u.list.items[i];
        if (s->type == AST_FUNC_DECL || s->type == AST_CLASS_DECL)
            prefix_end = i + 1;
    }
    if (prefix_end == 0 || prefix_end >= n - 2) {
        lr_eval_unit_free(unit); return NULL;
    }

    size_t pfx = (size_t)(prog->u.list.items[prefix_end - 1]->token.start
                         + prog->u.list.items[prefix_end - 1]->token.len
                         - unit->src);
    *out_prefix_len = pfx;

    /* Split remaining body into equal groups */
    int body_n = n - prefix_end;
    int per = body_n / num_threads;
    if (per < 1) per = 1;
    if (per > 32) per = 32;

    int ngroups = 0, cap = num_threads + 1;
    char **chunks = (char **)calloc(cap + 1, sizeof(char *));
    if (!chunks) { lr_eval_unit_free(unit); return NULL; }

    for (int start = prefix_end; start < n; start += per) {
        int end = start + per;
        if (end > n) end = n;
        ASTNode *first = prog->u.list.items[start];
        ASTNode *last  = prog->u.list.items[end - 1];
        size_t body_start = (size_t)(first->token.start - unit->src);
        size_t body_end   = (size_t)(last->token.start + last->token.len - unit->src);
        size_t body_len   = body_end - body_start;

        char *chunk = (char *)malloc(pfx + body_len + 1);
        if (chunk) {
            memcpy(chunk, unit->src, pfx);
            memcpy(chunk + pfx, unit->src + body_start, body_len);
            chunk[pfx + body_len] = 0;
            chunks[ngroups++] = chunk;
        }
    }
    chunks[ngroups] = NULL;
    *out_count = ngroups;
    lr_eval_unit_free(unit);
    return chunks;
}

void lr_engine_free_split_chunks(char **chunks) {
    if (!chunks) return;
    for (int i = 0; chunks[i]; i++) free(chunks[i]);
    free(chunks);
}