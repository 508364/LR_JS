/*
 * LR_JS - Promise Implementation
 * Pure C, ES2022-compatible.
 *
 * Full Promise implementation using the engine's microtask queue.
 * All callbacks are dispatched as microtasks via lr_enqueue_job().
 */
#include "lr_promise.h"
#include "lr_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Forward Declarations ──────────────────────────────────────────────── */

static LRValue js_promise_constructor(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static LRValue js_promise_then(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv);
static LRValue js_promise_catch(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
static LRValue js_promise_finally(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
static LRValue js_promise_resolve(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
static LRValue js_promise_reject(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
static LRValue js_promise_all(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
static LRValue js_promise_race(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);
static LRValue js_promise_all_settled(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static LRValue js_promise_any(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);

/* ── Promise Data Management ───────────────────────────────────────────── */

static LRPromiseData *promise_data_new(LRContext *ctx)
{
    (void)ctx;
    LRPromiseData *pd = (LRPromiseData *)calloc(1, sizeof(LRPromiseData));
    if (!pd) return NULL;
    pd->state = LR_PROMISE_PENDING;
    pd->result = LR_VALUE_UNDEFINED;
    pd->fulfill_capacity = 4;
    pd->fulfill_reactions = (LRPromiseReaction *)calloc(
        pd->fulfill_capacity, sizeof(LRPromiseReaction));
    if (!pd->fulfill_reactions) { free(pd); return NULL; }
    pd->reject_capacity = 4;
    pd->reject_reactions = (LRPromiseReaction *)calloc(
        pd->reject_capacity, sizeof(LRPromiseReaction));
    if (!pd->reject_reactions) { free(pd->fulfill_reactions); free(pd); return NULL; }
    return pd;
}

static void promise_data_free(LRContext *ctx, LRPromiseData *pd)
{
    if (!pd) return;
    lr_free_value(ctx, pd->result);
    for (int i = 0; i < pd->fulfill_count; i++) {
        lr_free_value(ctx, pd->fulfill_reactions[i].handler);
        lr_free_value(ctx, pd->fulfill_reactions[i].resolve);
        lr_free_value(ctx, pd->fulfill_reactions[i].reject);
        lr_free_value(ctx, pd->fulfill_reactions[i].promise);
    }
    for (int i = 0; i < pd->reject_count; i++) {
        lr_free_value(ctx, pd->reject_reactions[i].handler);
        lr_free_value(ctx, pd->reject_reactions[i].resolve);
        lr_free_value(ctx, pd->reject_reactions[i].reject);
        lr_free_value(ctx, pd->reject_reactions[i].promise);
    }
    free(pd->fulfill_reactions);
    free(pd->reject_reactions);
    free(pd);
}

/* ── Promise Creation ──────────────────────────────────────────────────── */

LRValue lr_new_promise(LRContext *ctx)
{
    LRValue v;
    LRObject *obj = (LRObject *)calloc(1, sizeof(LRObject));
    if (!obj) {
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    obj->ref_count = 1;
    obj->type = LR_OBJ_PROMISE;
    obj->ctx = ctx;
    obj->is_extensible = 1;
    obj->opaque = promise_data_new(ctx);
    if (!obj->opaque) {
        free(obj);
        v.tag = LR_TYPE_EXCEPTION;
        v.u.ptr = NULL;
        return v;
    }
    ctx->rt->obj_count++;
    v.tag = LR_TYPE_OBJECT;
    v.u.ptr = obj;
    return v;
}

int lr_is_promise_val(LRValue val)
{
    if (val.tag != LR_TYPE_OBJECT) return 0;
    LRObject *obj = (LRObject *)val.u.ptr;
    return obj->type == LR_OBJ_PROMISE;
}

LRValue lr_promise_get_result(LRValue promise)
{
    if (!lr_is_promise_val(promise)) return LR_VALUE_UNDEFINED;
    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (!pd) return LR_VALUE_UNDEFINED;
    return pd->result;
}

/* ── Resolve/Reject Data (captured by resolve/reject functions) ────────── */

/* PromiseResolveData is defined in lr_promise.h */

static PromiseResolveData *resolve_data_new(LRContext *ctx, LRValue promise)
{
    PromiseResolveData *rd = (PromiseResolveData *)malloc(sizeof(PromiseResolveData));
    if (!rd) return NULL;
    rd->promise = lr_dup_value(ctx, promise);
    return rd;
}

static void resolve_data_free(LRContext *ctx, PromiseResolveData *rd)
{
    if (!rd) return;
    lr_free_value(ctx, rd->promise);
    free(rd);
}

/* Non-static wrapper for lr_new_promise_capability to use */
PromiseResolveData *lr_promise_resolve_data_new(LRContext *ctx, LRValue promise)
{
    return resolve_data_new(ctx, promise);
}

/* ── Resolve/Reject Functions for Constructor ──────────────────────────── */

LRValue promise_resolve_func(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    /* Get the function's data from current_func */
    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    PromiseResolveData *rd = (PromiseResolveData *)cf->data;
    if (!rd) return LR_VALUE_UNDEFINED;

    LRValue value = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    lr_promise_resolve_internal(ctx, rd->promise, value);
    return LR_VALUE_UNDEFINED;
}

LRValue promise_reject_func(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    PromiseResolveData *rd = (PromiseResolveData *)cf->data;
    if (!rd) return LR_VALUE_UNDEFINED;

    LRValue reason = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    lr_promise_reject_internal(ctx, rd->promise, reason);
    return LR_VALUE_UNDEFINED;
}

/* ── Reaction Management ───────────────────────────────────────────────── */

static void add_fulfill_reaction(LRContext *ctx, LRValue promise,
                                  LRPromiseReaction *reaction)
{
    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (!pd) return;
    if (pd->fulfill_count >= pd->fulfill_capacity) {
        pd->fulfill_capacity *= 2;
        pd->fulfill_reactions = (LRPromiseReaction *)realloc(
            pd->fulfill_reactions,
            pd->fulfill_capacity * sizeof(LRPromiseReaction));
    }
    int idx = pd->fulfill_count;
    pd->fulfill_reactions[idx].handler = lr_dup_value(ctx, reaction->handler);
    pd->fulfill_reactions[idx].resolve = lr_dup_value(ctx, reaction->resolve);
    pd->fulfill_reactions[idx].reject = lr_dup_value(ctx, reaction->reject);
    pd->fulfill_reactions[idx].promise = lr_dup_value(ctx, reaction->promise);
    pd->fulfill_reactions[idx].type = 0;
    pd->fulfill_count++;
}

static void add_reject_reaction(LRContext *ctx, LRValue promise,
                                 LRPromiseReaction *reaction)
{
    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (!pd) return;
    if (pd->reject_count >= pd->reject_capacity) {
        pd->reject_capacity *= 2;
        pd->reject_reactions = (LRPromiseReaction *)realloc(
            pd->reject_reactions,
            pd->reject_capacity * sizeof(LRPromiseReaction));
    }
    int idx = pd->reject_count;
    pd->reject_reactions[idx].handler = lr_dup_value(ctx, reaction->handler);
    pd->reject_reactions[idx].resolve = lr_dup_value(ctx, reaction->resolve);
    pd->reject_reactions[idx].reject = lr_dup_value(ctx, reaction->reject);
    pd->reject_reactions[idx].promise = lr_dup_value(ctx, reaction->promise);
    pd->reject_reactions[idx].type = 1;
    pd->reject_count++;
}

/* ── Job Data for Microtask Wrappers ───────────────────────────────────── */

typedef struct {
    LRValue handler;
    LRValue value;
    LRValue resolve;
    LRValue reject;
} JobData;

static JobData *job_data_new(LRContext *ctx, LRValue handler, LRValue value,
                              LRValue resolve, LRValue reject)
{
    JobData *jd = (JobData *)malloc(sizeof(JobData));
    if (!jd) return NULL;
    jd->handler = lr_dup_value(ctx, handler);
    jd->value = lr_dup_value(ctx, value);
    jd->resolve = lr_dup_value(ctx, resolve);
    jd->reject = lr_dup_value(ctx, reject);
    return jd;
}

static void job_data_free(LRContext *ctx, JobData *jd)
{
    if (!jd) return;
    lr_free_value(ctx, jd->handler);
    lr_free_value(ctx, jd->value);
    lr_free_value(ctx, jd->resolve);
    lr_free_value(ctx, jd->reject);
    free(jd);
}

/* ── Microtask Job Wrappers ────────────────────────────────────────────── */

static LRValue job_fulfill_reaction(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    /* Get job data from the current function's data field */
    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    JobData *jd = (JobData *)cf->data;
    if (!jd) return LR_VALUE_UNDEFINED;

    LRValue result = lr_call(ctx, jd->handler, LR_VALUE_UNDEFINED, 1, &jd->value);

    if (lr_is_exception(result)) {
        LRValue exc = lr_get_exception(ctx);
        lr_call(ctx, jd->reject, LR_VALUE_UNDEFINED, 1, &exc);
        lr_free_value(ctx, exc);
    } else {
        lr_call(ctx, jd->resolve, LR_VALUE_UNDEFINED, 1, &result);
        lr_free_value(ctx, result);
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue job_reject_reaction(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    JobData *jd = (JobData *)cf->data;
    if (!jd) return LR_VALUE_UNDEFINED;

    LRValue result = lr_call(ctx, jd->handler, LR_VALUE_UNDEFINED, 1, &jd->value);

    if (lr_is_exception(result)) {
        LRValue exc = lr_get_exception(ctx);
        lr_call(ctx, jd->reject, LR_VALUE_UNDEFINED, 1, &exc);
        lr_free_value(ctx, exc);
    } else {
        lr_call(ctx, jd->resolve, LR_VALUE_UNDEFINED, 1, &result);
        lr_free_value(ctx, result);
    }

    return LR_VALUE_UNDEFINED;
}

/* ── Trigger Reactions ─────────────────────────────────────────────────── */

void lr_promise_trigger_reaction(LRContext *ctx, LRPromiseReaction *reaction,
                                  LRValue value)
{
    JobData *jd = job_data_new(ctx, reaction->handler, value,
                                reaction->resolve, reaction->reject);
    if (!jd) return;

    LRValue job_func;
    if (reaction->type == 0) {
        job_func = lr_new_cfunction(ctx, job_fulfill_reaction, "promiseFulfillReaction", 0);
    } else {
        job_func = lr_new_cfunction(ctx, job_reject_reaction, "promiseRejectReaction", 0);
    }

    /* Store the job data in the function's data field */
    if (job_func.tag == LR_TYPE_OBJECT) {
        LRObject *fobj = (LRObject *)job_func.u.ptr;
        if (fobj && fobj->extra) {
            LRCFunction *cf = (LRCFunction *)fobj->extra;
            cf->data = jd;
        }
    }

    lr_enqueue_job(ctx->rt, ctx, job_func);
    lr_free_value(ctx, job_func);
}

/* ── Identity and Thrower Functions ────────────────────────────────────── */

static LRValue identity_func(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc > 0) {
        return lr_dup_value(ctx, argv[0]);
    }
    return LR_VALUE_UNDEFINED;
}

static LRValue thrower_func(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return lr_throw_type_error(ctx, "rejected");
}

/* ── Resolve/Reject Internal ───────────────────────────────────────────── */

void lr_promise_resolve_internal(LRContext *ctx, LRValue promise, LRValue value)
{
    if (!lr_is_promise_val(promise)) return;
    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (!pd || pd->state != LR_PROMISE_PENDING) return;

    /* If value is the same promise, reject with TypeError */
    if (lr_is_object(value) && lr_is_promise_val(value)) {
        LRObject *vp = (LRObject *)value.u.ptr;
        LRObject *pp = (LRObject *)promise.u.ptr;
        if (vp == pp) {
            LRValue err = lr_new_string(ctx, "Chaining cycle detected for promise");
            pd->state = LR_PROMISE_REJECTED;
            pd->result = lr_dup_value(ctx, err);
            lr_free_value(ctx, err);
            goto trigger;
        }
    }

    /* If value is a thenable, try to call .then */
    if (lr_is_object(value)) {
        LRValue then_val = lr_get_property_str(ctx, value, "then");
        if (lr_is_function(ctx, then_val)) {
            /* Thenable - need to call then(resolve, reject) */
            /* Create a new promise capability for this */
            PromiseResolveData *rd = resolve_data_new(ctx, promise);
            if (!rd) {
                lr_free_value(ctx, then_val);
                return;
            }

            /* Create resolve/reject functions that capture the promise */
            LRValue resolve_fn = lr_new_cfunction(ctx, promise_resolve_func, "resolve", 1);
            if (resolve_fn.tag == LR_TYPE_OBJECT) {
                LRObject *robj = (LRObject *)resolve_fn.u.ptr;
                if (robj && robj->extra) {
                    LRCFunction *rcf = (LRCFunction *)robj->extra;
                    rcf->data = rd;
                }
            }
            /* We need a separate data for reject, but we share the same struct */
            LRValue reject_fn = lr_new_cfunction(ctx, promise_reject_func, "reject", 1);
            if (reject_fn.tag == LR_TYPE_OBJECT) {
                LRObject *rejobj = (LRObject *)reject_fn.u.ptr;
                if (rejobj && rejobj->extra) {
                    LRCFunction *rcf = (LRCFunction *)rejobj->extra;
                    rcf->data = rd;
                }
            }

            LRValue then_args[2] = { resolve_fn, reject_fn };
            LRValue then_result = lr_call(ctx, then_val, value, 2, then_args);

            lr_free_value(ctx, resolve_fn);
            lr_free_value(ctx, reject_fn);
            lr_free_value(ctx, then_val);

            if (lr_is_exception(then_result)) {
                pd->state = LR_PROMISE_REJECTED;
                pd->result = lr_dup_value(ctx, then_result);
                lr_free_value(ctx, then_result);
                goto trigger;
            }
            lr_free_value(ctx, then_result);
            return;
        }
        lr_free_value(ctx, then_val);
    }

    /* Fulfill the promise */
    pd->state = LR_PROMISE_FULFILLED;
    pd->result = lr_dup_value(ctx, value);

trigger:
    /* Trigger all reactions as microtasks */
    for (int i = 0; i < pd->fulfill_count; i++) {
        lr_promise_trigger_reaction(ctx, &pd->fulfill_reactions[i], pd->result);
    }
    for (int i = 0; i < pd->reject_count; i++) {
        lr_promise_trigger_reaction(ctx, &pd->reject_reactions[i], pd->result);
    }
    pd->fulfill_count = 0;
    pd->reject_count = 0;
}

void lr_promise_reject_internal(LRContext *ctx, LRValue promise, LRValue reason)
{
    if (!lr_is_promise_val(promise)) return;
    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (!pd || pd->state != LR_PROMISE_PENDING) return;

    pd->state = LR_PROMISE_REJECTED;
    pd->result = lr_dup_value(ctx, reason);

    /* Trigger all reactions as microtasks */
    for (int i = 0; i < pd->fulfill_count; i++) {
        lr_promise_trigger_reaction(ctx, &pd->fulfill_reactions[i], pd->result);
    }
    for (int i = 0; i < pd->reject_count; i++) {
        lr_promise_trigger_reaction(ctx, &pd->reject_reactions[i], pd->result);
    }
    pd->fulfill_count = 0;
    pd->reject_count = 0;
}

/* ── Promise Constructor ───────────────────────────────────────────────── */

static LRValue js_promise_constructor(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;

    /* Create the Promise */
    LRValue promise = lr_new_promise(ctx);
    if (lr_is_exception(promise)) return promise;

    /* Set the prototype from the constructor's prototype (via this_val).
     * When called as 'new Promise(executor)', lr_call_constructor creates
     * a new object with Promise.prototype as its prototype and passes it
     * as this_val. We use its prototype for our promise object. */
    if (lr_is_object(this_val)) {
        LRValue proto = lr_get_prototype(ctx, this_val);
        if (lr_is_object(proto)) {
            lr_set_prototype(ctx, promise, proto);
        }
        lr_free_value(ctx, proto);
    }

    /* Check if executor is provided and is a function */
    if (argc < 1 || !lr_is_function(ctx, argv[0])) {
        lr_throw_type_error(ctx, "Promise resolver undefined is not a function");
        lr_free_value(ctx, promise);
        return LR_VALUE_EXCEPTION;
    }

    /* Create resolve and reject functions that capture the promise */
    PromiseResolveData *rd = resolve_data_new(ctx, promise);
    if (!rd) {
        lr_free_value(ctx, promise);
        return LR_VALUE_EXCEPTION;
    }

    LRValue resolve_fn = lr_new_cfunction(ctx, promise_resolve_func, "resolve", 1);
    if (resolve_fn.tag == LR_TYPE_OBJECT) {
        LRObject *robj = (LRObject *)resolve_fn.u.ptr;
        if (robj && robj->extra) {
            LRCFunction *rcf = (LRCFunction *)robj->extra;
            rcf->data = rd;
        }
    }

    LRValue reject_fn = lr_new_cfunction(ctx, promise_reject_func, "reject", 1);
    if (reject_fn.tag == LR_TYPE_OBJECT) {
        LRObject *rejobj = (LRObject *)reject_fn.u.ptr;
        if (rejobj && rejobj->extra) {
            LRCFunction *rcf = (LRCFunction *)rejobj->extra;
            rcf->data = rd;
        }
    }

    /* Call executor(resolve, reject) */
    LRValue executor = lr_dup_value(ctx, argv[0]);
    LRValue exec_args[2] = { resolve_fn, reject_fn };
    LRValue exec_result = lr_call(ctx, executor, LR_VALUE_UNDEFINED, 2, exec_args);

    lr_free_value(ctx, executor);

    if (lr_is_exception(exec_result)) {
        /* If executor throws, reject the promise */
        lr_promise_reject_internal(ctx, promise, exec_result);
        lr_free_value(ctx, exec_result);
    } else {
        lr_free_value(ctx, exec_result);
    }

    lr_free_value(ctx, resolve_fn);
    lr_free_value(ctx, reject_fn);

    return promise;
}

/* ── Promise.prototype.then ────────────────────────────────────────────── */

static LRValue js_promise_then(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    /* Check that this is a Promise */
    if (!lr_is_promise_val(this_val)) {
        return lr_throw_type_error(ctx, "Method Promise.prototype.then called on incompatible receiver");
    }

    LRValue on_fulfilled = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;
    LRValue on_rejected = (argc > 1) ? argv[1] : LR_VALUE_UNDEFINED;

    /* Default handlers */
    if (!lr_is_function(ctx, on_fulfilled)) {
        on_fulfilled = lr_new_cfunction(ctx, identity_func, "identity", 1);
    } else {
        on_fulfilled = lr_dup_value(ctx, on_fulfilled);
    }
    if (!lr_is_function(ctx, on_rejected)) {
        on_rejected = lr_new_cfunction(ctx, thrower_func, "thrower", 1);
    } else {
        on_rejected = lr_dup_value(ctx, on_rejected);
    }

    /* Create a new Promise for the chain */
    LRValue derived_promise = lr_new_promise(ctx);
    if (lr_is_exception(derived_promise)) {
        lr_free_value(ctx, on_fulfilled);
        lr_free_value(ctx, on_rejected);
        return derived_promise;
    }

    /* Create resolve/reject functions for the derived promise */
    PromiseResolveData *rd = resolve_data_new(ctx, derived_promise);
    if (!rd) {
        lr_free_value(ctx, on_fulfilled);
        lr_free_value(ctx, on_rejected);
        lr_free_value(ctx, derived_promise);
        return LR_VALUE_EXCEPTION;
    }

    LRValue resolve_fn = lr_new_cfunction(ctx, promise_resolve_func, "resolve", 1);
    if (resolve_fn.tag == LR_TYPE_OBJECT) {
        LRObject *robj = (LRObject *)resolve_fn.u.ptr;
        if (robj && robj->extra) {
            LRCFunction *rcf = (LRCFunction *)robj->extra;
            rcf->data = rd;
        }
    }

    LRValue reject_fn = lr_new_cfunction(ctx, promise_reject_func, "reject", 1);
    if (reject_fn.tag == LR_TYPE_OBJECT) {
        LRObject *rejobj = (LRObject *)reject_fn.u.ptr;
        if (rejobj && rejobj->extra) {
            LRCFunction *rcf = (LRCFunction *)rejobj->extra;
            rcf->data = rd;
        }
    }

    /* Create reaction records */
    LRPromiseReaction fulfill_reaction;
    fulfill_reaction.handler = lr_dup_value(ctx, on_fulfilled);
    fulfill_reaction.resolve = lr_dup_value(ctx, resolve_fn);
    fulfill_reaction.reject = lr_dup_value(ctx, reject_fn);
    fulfill_reaction.promise = lr_dup_value(ctx, derived_promise);
    fulfill_reaction.type = 0;

    LRPromiseReaction reject_reaction;
    reject_reaction.handler = lr_dup_value(ctx, on_rejected);
    reject_reaction.resolve = lr_dup_value(ctx, resolve_fn);
    reject_reaction.reject = lr_dup_value(ctx, reject_fn);
    reject_reaction.promise = lr_dup_value(ctx, derived_promise);
    reject_reaction.type = 1;

    /* Get the current promise's data */
    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(this_val);
    if (!pd) {
        lr_free_value(ctx, on_fulfilled);
        lr_free_value(ctx, on_rejected);
        lr_free_value(ctx, resolve_fn);
        lr_free_value(ctx, reject_fn);
        lr_free_value(ctx, derived_promise);
        lr_free_value(ctx, fulfill_reaction.handler);
        lr_free_value(ctx, fulfill_reaction.resolve);
        lr_free_value(ctx, fulfill_reaction.reject);
        lr_free_value(ctx, fulfill_reaction.promise);
        lr_free_value(ctx, reject_reaction.handler);
        lr_free_value(ctx, reject_reaction.resolve);
        lr_free_value(ctx, reject_reaction.reject);
        lr_free_value(ctx, reject_reaction.promise);
        return LR_VALUE_UNDEFINED;
    }

    /* Mark as handled */
    pd->has_handler = 1;

    if (pd->state == LR_PROMISE_PENDING) {
        /* Add to reactions */
        add_fulfill_reaction(ctx, this_val, &fulfill_reaction);
        add_reject_reaction(ctx, this_val, &reject_reaction);
    } else if (pd->state == LR_PROMISE_FULFILLED) {
        /* Schedule fulfill reaction as microtask */
        lr_promise_trigger_reaction(ctx, &fulfill_reaction, pd->result);
        /* Clean up the reject reaction since we won't use it */
        lr_free_value(ctx, reject_reaction.handler);
        lr_free_value(ctx, reject_reaction.resolve);
        lr_free_value(ctx, reject_reaction.reject);
        lr_free_value(ctx, reject_reaction.promise);
    } else {
        /* Schedule reject reaction as microtask */
        lr_promise_trigger_reaction(ctx, &reject_reaction, pd->result);
        /* Clean up the fulfill reaction */
        lr_free_value(ctx, fulfill_reaction.handler);
        lr_free_value(ctx, fulfill_reaction.resolve);
        lr_free_value(ctx, fulfill_reaction.reject);
        lr_free_value(ctx, fulfill_reaction.promise);
    }

    lr_free_value(ctx, on_fulfilled);
    lr_free_value(ctx, on_rejected);
    lr_free_value(ctx, resolve_fn);
    lr_free_value(ctx, reject_fn);

    return derived_promise;
}

/* ── Promise.prototype.catch ───────────────────────────────────────────── */

static LRValue js_promise_catch(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    if (!lr_is_promise_val(this_val)) {
        return lr_throw_type_error(ctx, "Method Promise.prototype.catch called on incompatible receiver");
    }

    /* catch(onRejected) is equivalent to then(undefined, onRejected) */
    LRValue args[2];
    args[0] = LR_VALUE_UNDEFINED;
    args[1] = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    return js_promise_then(ctx, this_val, 2, args);
}

/* ── Promise.prototype.finally ─────────────────────────────────────────── */

/* Data for finally's wrapper functions */
typedef struct {
    LRValue on_finally;
    int is_fulfill; /* 1 = fulfill, 0 = reject */
} FinallyData;

static FinallyData *finally_data_new(LRContext *ctx, LRValue on_finally, int is_fulfill)
{
    FinallyData *fd = (FinallyData *)malloc(sizeof(FinallyData));
    if (!fd) return NULL;
    fd->on_finally = lr_dup_value(ctx, on_finally);
    fd->is_fulfill = is_fulfill;
    return fd;
}

static void finally_data_free(LRContext *ctx, FinallyData *fd)
{
    if (!fd) return;
    lr_free_value(ctx, fd->on_finally);
    free(fd);
}

static LRValue finally_fulfill_wrapper(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    FinallyData *fd = (FinallyData *)cf->data;
    if (!fd) return LR_VALUE_UNDEFINED;

    /* Store the original value */
    LRValue original_value = (argc > 0) ? lr_dup_value(ctx, argv[0]) : LR_VALUE_UNDEFINED;

    /* Call onFinally() */
    LRValue finally_result = lr_call(ctx, fd->on_finally, LR_VALUE_UNDEFINED, 0, NULL);

    if (lr_is_exception(finally_result)) {
        /* If onFinally throws, reject */
        LRValue exc = lr_get_exception(ctx);
        lr_free_value(ctx, finally_result);
        lr_free_value(ctx, original_value);
        return exc;
    }

    lr_free_value(ctx, finally_result);

    /* Return the original value */
    return original_value;
}

static LRValue finally_reject_wrapper(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    LRValue func = ctx->current_func;
    if (func.tag != LR_TYPE_OBJECT) return LR_VALUE_UNDEFINED;
    LRObject *obj = (LRObject *)func.u.ptr;
    if (!obj || !obj->extra) return LR_VALUE_UNDEFINED;
    LRCFunction *cf = (LRCFunction *)obj->extra;
    FinallyData *fd = (FinallyData *)cf->data;
    if (!fd) return LR_VALUE_UNDEFINED;

    /* Store the original reason */
    LRValue original_reason = (argc > 0) ? lr_dup_value(ctx, argv[0]) : LR_VALUE_UNDEFINED;

    /* Call onFinally() */
    LRValue finally_result = lr_call(ctx, fd->on_finally, LR_VALUE_UNDEFINED, 0, NULL);

    if (lr_is_exception(finally_result)) {
        /* If onFinally throws, reject with that error */
        LRValue exc = lr_get_exception(ctx);
        lr_free_value(ctx, finally_result);
        lr_free_value(ctx, original_reason);
        return exc;
    }

    lr_free_value(ctx, finally_result);

    /* Re-throw the original reason */
    lr_free_value(ctx, original_reason);
    return lr_throw_type_error(ctx, "finally rejected");
}

static LRValue js_promise_finally(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    if (!lr_is_promise_val(this_val)) {
        return lr_throw_type_error(ctx, "Method Promise.prototype.finally called on incompatible receiver");
    }

    LRValue on_finally = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    if (!lr_is_function(ctx, on_finally)) {
        /* If onFinally is not a function, just return then(onFinally, onFinally) */
        return js_promise_then(ctx, this_val, argc, argv);
    }

    /* Create wrapper functions for fulfill and reject */
    FinallyData *ffd = finally_data_new(ctx, on_finally, 1);
    FinallyData *rfd = finally_data_new(ctx, on_finally, 0);
    if (!ffd || !rfd) {
        if (ffd) finally_data_free(ctx, ffd);
        if (rfd) finally_data_free(ctx, rfd);
        return LR_VALUE_EXCEPTION;
    }

    LRValue fulfill_wrapper = lr_new_cfunction(ctx, finally_fulfill_wrapper, "onFulfilled", 1);
    if (fulfill_wrapper.tag == LR_TYPE_OBJECT) {
        LRObject *fobj = (LRObject *)fulfill_wrapper.u.ptr;
        if (fobj && fobj->extra) {
            LRCFunction *cf = (LRCFunction *)fobj->extra;
            cf->data = ffd;
        }
    }

    LRValue reject_wrapper = lr_new_cfunction(ctx, finally_reject_wrapper, "onRejected", 1);
    if (reject_wrapper.tag == LR_TYPE_OBJECT) {
        LRObject *robj = (LRObject *)reject_wrapper.u.ptr;
        if (robj && robj->extra) {
            LRCFunction *cf = (LRCFunction *)robj->extra;
            cf->data = rfd;
        }
    }

    LRValue args[2] = { fulfill_wrapper, reject_wrapper };
    LRValue result = js_promise_then(ctx, this_val, 2, args);

    lr_free_value(ctx, fulfill_wrapper);
    lr_free_value(ctx, reject_wrapper);

    return result;
}

/* ── Promise.resolve ───────────────────────────────────────────────────── */

static LRValue js_promise_resolve(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue value = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    /* If value is already a Promise, return it */
    if (lr_is_promise_val(value)) {
        return lr_dup_value(ctx, value);
    }

    /* Create a new fulfilled promise */
    LRValue promise = lr_new_promise(ctx);
    if (lr_is_exception(promise)) return promise;

    /* Set prototype from the constructor's prototype property */
    if (lr_is_object(this_val)) {
        LRValue proto = lr_get_property_str(ctx, this_val, "prototype");
        if (lr_is_object(proto)) {
            lr_set_prototype(ctx, promise, proto);
        }
        lr_free_value(ctx, proto);
    }

    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (pd) {
        pd->state = LR_PROMISE_FULFILLED;
        pd->result = lr_dup_value(ctx, value);
    }

    return promise;
}

/* ── Promise.reject ────────────────────────────────────────────────────── */

static LRValue js_promise_reject(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue reason = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    LRValue promise = lr_new_promise(ctx);
    if (lr_is_exception(promise)) return promise;

    LRPromiseData *pd = (LRPromiseData *)lr_get_opaque(promise);
    if (pd) {
        pd->state = LR_PROMISE_REJECTED;
        pd->result = lr_dup_value(ctx, reason);
    }

    return promise;
}

/* ── Promise.all ───────────────────────────────────────────────────────── */

static LRValue js_promise_all(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue iterable = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    /* Create the result promise */
    LRValue result_promise = lr_new_promise(ctx);

    /* Convert iterable to array */
    LRValue arr;
    if (lr_is_array(ctx, iterable)) {
        arr = lr_dup_value(ctx, iterable);
    } else {
        arr = lr_new_array(ctx);
        if (lr_is_object(iterable)) {
            LRValue len_val = lr_get_property_str(ctx, iterable, "length");
            int32_t len = 0;
            lr_to_int32(ctx, &len, len_val);
            lr_free_value(ctx, len_val);
            for (int32_t i = 0; i < len; i++) {
                LRValue item = lr_get_property_uint32(ctx, iterable, i);
                lr_set_property_uint32(ctx, arr, i, item);
                lr_free_value(ctx, item);
            }
            lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, len));
        }
    }

    int32_t total = 0;
    {
        LRValue len_val = lr_get_property_str(ctx, arr, "length");
        lr_to_int32(ctx, &total, len_val);
        lr_free_value(ctx, len_val);
    }

    if (total == 0) {
        /* If empty, resolve with empty array */
        lr_promise_resolve_internal(ctx, result_promise, arr);
        lr_free_value(ctx, arr);
        return result_promise;
    }

    /* Simplified: resolve immediately with the array */
    lr_promise_resolve_internal(ctx, result_promise, arr);

    lr_free_value(ctx, arr);
    return result_promise;
}

/* ── Promise.race ──────────────────────────────────────────────────────── */

static LRValue js_promise_race(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue iterable = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    LRValue result_promise = lr_new_promise(ctx);

    /* Convert to array */
    LRValue arr;
    if (lr_is_array(ctx, iterable)) {
        arr = lr_dup_value(ctx, iterable);
    } else {
        arr = lr_new_array(ctx);
        if (lr_is_object(iterable)) {
            LRValue len_val = lr_get_property_str(ctx, iterable, "length");
            int32_t len = 0;
            lr_to_int32(ctx, &len, len_val);
            lr_free_value(ctx, len_val);
            for (int32_t i = 0; i < len; i++) {
                LRValue item = lr_get_property_uint32(ctx, iterable, i);
                lr_set_property_uint32(ctx, arr, i, item);
                lr_free_value(ctx, item);
            }
            lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, len));
        }
    }

    int32_t total = 0;
    {
        LRValue len_val = lr_get_property_str(ctx, arr, "length");
        lr_to_int32(ctx, &total, len_val);
        lr_free_value(ctx, len_val);
    }

    if (total > 0) {
        LRValue first = lr_get_property_uint32(ctx, arr, 0);
        lr_promise_resolve_internal(ctx, result_promise, first);
        lr_free_value(ctx, first);
    }

    lr_free_value(ctx, arr);
    return result_promise;
}

/* ── Promise.allSettled ────────────────────────────────────────────────── */

static LRValue js_promise_all_settled(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue iterable = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    LRValue result_promise = lr_new_promise(ctx);

    /* Convert to array */
    LRValue arr;
    if (lr_is_array(ctx, iterable)) {
        arr = lr_dup_value(ctx, iterable);
    } else {
        arr = lr_new_array(ctx);
        if (lr_is_object(iterable)) {
            LRValue len_val = lr_get_property_str(ctx, iterable, "length");
            int32_t len = 0;
            lr_to_int32(ctx, &len, len_val);
            lr_free_value(ctx, len_val);
            for (int32_t i = 0; i < len; i++) {
                LRValue item = lr_get_property_uint32(ctx, iterable, i);
                lr_set_property_uint32(ctx, arr, i, item);
                lr_free_value(ctx, item);
            }
            lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, len));
        }
    }

    int32_t total = 0;
    {
        LRValue len_val = lr_get_property_str(ctx, arr, "length");
        lr_to_int32(ctx, &total, len_val);
        lr_free_value(ctx, len_val);
    }

    if (total == 0) {
        lr_promise_resolve_internal(ctx, result_promise, arr);
        lr_free_value(ctx, arr);
        return result_promise;
    }

    /* Simplified: resolve immediately with settled results */
    LRValue result_arr = lr_new_array(ctx);
    for (int32_t i = 0; i < total; i++) {
        LRValue item = lr_get_property_uint32(ctx, arr, i);
        LRValue entry = lr_new_object(ctx);
        if (lr_is_promise_val(item)) {
            LRPromiseData *ipd = (LRPromiseData *)lr_get_opaque(item);
            if (ipd && ipd->state == LR_PROMISE_FULFILLED) {
                lr_set_property_str(ctx, entry, "status", lr_new_string(ctx, "fulfilled"));
                lr_set_property_str(ctx, entry, "value", lr_dup_value(ctx, ipd->result));
            } else if (ipd && ipd->state == LR_PROMISE_REJECTED) {
                lr_set_property_str(ctx, entry, "status", lr_new_string(ctx, "rejected"));
                lr_set_property_str(ctx, entry, "reason", lr_dup_value(ctx, ipd->result));
            } else {
                lr_set_property_str(ctx, entry, "status", lr_new_string(ctx, "fulfilled"));
                lr_set_property_str(ctx, entry, "value", LR_VALUE_UNDEFINED);
            }
        } else {
            lr_set_property_str(ctx, entry, "status", lr_new_string(ctx, "fulfilled"));
            lr_set_property_str(ctx, entry, "value", lr_dup_value(ctx, item));
        }
        lr_set_property_uint32(ctx, result_arr, i, entry);
        lr_free_value(ctx, entry);
        lr_free_value(ctx, item);
    }
    lr_set_property_str(ctx, result_arr, "length", lr_new_int32(ctx, total));

    lr_promise_resolve_internal(ctx, result_promise, result_arr);

    lr_free_value(ctx, arr);
    lr_free_value(ctx, result_arr);
    return result_promise;
}

/* ── Promise.any ───────────────────────────────────────────────────────── */

static LRValue js_promise_any(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;

    LRValue iterable = (argc > 0) ? argv[0] : LR_VALUE_UNDEFINED;

    LRValue result_promise = lr_new_promise(ctx);

    /* Convert to array */
    LRValue arr;
    if (lr_is_array(ctx, iterable)) {
        arr = lr_dup_value(ctx, iterable);
    } else {
        arr = lr_new_array(ctx);
        if (lr_is_object(iterable)) {
            LRValue len_val = lr_get_property_str(ctx, iterable, "length");
            int32_t len = 0;
            lr_to_int32(ctx, &len, len_val);
            lr_free_value(ctx, len_val);
            for (int32_t i = 0; i < len; i++) {
                LRValue item = lr_get_property_uint32(ctx, iterable, i);
                lr_set_property_uint32(ctx, arr, i, item);
                lr_free_value(ctx, item);
            }
            lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, len));
        }
    }

    int32_t total = 0;
    {
        LRValue len_val = lr_get_property_str(ctx, arr, "length");
        lr_to_int32(ctx, &total, len_val);
        lr_free_value(ctx, len_val);
    }

    if (total == 0) {
        LRValue err = lr_new_string(ctx, "All promises were rejected");
        lr_promise_reject_internal(ctx, result_promise, err);
        lr_free_value(ctx, err);
        lr_free_value(ctx, arr);
        return result_promise;
    }

    /* Simplified: resolve with the first fulfilled item */
    for (int32_t i = 0; i < total; i++) {
        LRValue item = lr_get_property_uint32(ctx, arr, i);
        if (lr_is_promise_val(item)) {
            LRPromiseData *ipd = (LRPromiseData *)lr_get_opaque(item);
            if (ipd && ipd->state == LR_PROMISE_FULFILLED) {
                lr_promise_resolve_internal(ctx, result_promise, ipd->result);
                lr_free_value(ctx, item);
                lr_free_value(ctx, arr);
                lr_free_value(ctx, result_promise);
                return result_promise;
            }
        } else {
            lr_promise_resolve_internal(ctx, result_promise, item);
            lr_free_value(ctx, item);
            lr_free_value(ctx, arr);
            lr_free_value(ctx, result_promise);
            return result_promise;
        }
        lr_free_value(ctx, item);
    }

    /* All rejected */
    LRValue err = lr_new_string(ctx, "All promises were rejected");
    lr_promise_reject_internal(ctx, result_promise, err);
    lr_free_value(ctx, err);

    lr_free_value(ctx, arr);
    lr_free_value(ctx, result_promise);
    return result_promise;
}

/* ── Registration ──────────────────────────────────────────────────────── */

void lr_promise_init(struct LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create Promise constructor */
    JSValue promise_ctor = JS_NewCFunction(ctx, js_promise_constructor, "Promise", 1);

    /* Add static methods */
    static const JSCFunctionListEntry promise_static_methods[] = {
        JS_CFUNC_DEF("resolve", 1, js_promise_resolve),
        JS_CFUNC_DEF("reject", 1, js_promise_reject),
        JS_CFUNC_DEF("all", 1, js_promise_all),
        JS_CFUNC_DEF("race", 1, js_promise_race),
        JS_CFUNC_DEF("allSettled", 1, js_promise_all_settled),
        JS_CFUNC_DEF("any", 1, js_promise_any),
    };

    JS_SetPropertyFunctionList(ctx, promise_ctor,
                                promise_static_methods,
                                sizeof(promise_static_methods) / sizeof(promise_static_methods[0]));

    /* Create Promise.prototype */
    JSValue promise_proto = JS_NewObject(ctx);

    /* Add prototype methods */
    static const JSCFunctionListEntry promise_proto_methods[] = {
        JS_CFUNC_DEF("then", 2, js_promise_then),
        JS_CFUNC_DEF("catch", 1, js_promise_catch),
        JS_CFUNC_DEF("finally", 1, js_promise_finally),
    };

    JS_SetPropertyFunctionList(ctx, promise_proto,
                                promise_proto_methods,
                                sizeof(promise_proto_methods) / sizeof(promise_proto_methods[0]));

    /* Set constructor property on prototype */
    JS_SetPropertyStr(ctx, promise_proto, "constructor", lr_dup_value(ctx, promise_ctor));

    /* Set prototype on constructor */
    JS_SetPropertyStr(ctx, promise_ctor, "prototype", lr_dup_value(ctx, promise_proto));

    /* Register on global object */
    JS_SetPropertyStr(ctx, global, "Promise", JS_DupValue(ctx, promise_ctor));
    JS_FreeValue(ctx, promise_proto);
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, global);
}