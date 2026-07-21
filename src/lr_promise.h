/*
 * LR_JS - Promise Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the full Promise specification including:
 *   Promise constructor, .then(), .catch(), .finally()
 *   Promise.resolve(), Promise.reject(), Promise.all()
 *   Promise.race(), Promise.allSettled(), Promise.any()
 *
 * Uses lr_enqueue_job() for microtask scheduling.
 */
#ifndef LR_PROMISE_H
#define LR_PROMISE_H

#include "engine/lr_engine.h"

/* Forward declaration */
struct LR_Runtime;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Promise States ────────────────────────────────────────────────────── */

#define LR_PROMISE_PENDING    0
#define LR_PROMISE_FULFILLED  1
#define LR_PROMISE_REJECTED   2

/* ── Promise Data Structure ────────────────────────────────────────────── */

/* A reaction record stored for each .then()/.catch() call */
typedef struct LRPromiseReaction {
    LRValue  handler;    /* onFulfilled or onRejected */
    LRValue  resolve;    /* resolve function of derived capability */
    LRValue  reject;     /* reject function of derived capability */
    LRValue  promise;    /* derived promise */
    int      type;       /* 0=fulfill, 1=reject */
} LRPromiseReaction;

/* Internal data attached to each Promise object via opaque */
typedef struct LRPromiseData {
    int                 state;               /* LR_PROMISE_PENDING/_FULFILLED/_REJECTED */
    LRValue             result;              /* [[PromiseResult]] */
    int                 has_handler;         /* [[PromiseIsHandled]] */
    LRPromiseReaction  *fulfill_reactions;   /* array of fulfill reactions */
    int                 fulfill_count;
    int                 fulfill_capacity;
    LRPromiseReaction  *reject_reactions;    /* array of reject reactions */
    int                 reject_count;
    int                 reject_capacity;
} LRPromiseData;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Initialize the Promise constructor on the global object. */
void lr_promise_init(struct LR_Runtime *rt);

/* Get the internal [[PromiseResult]] of a Promise object.
 * Returns the result value (caller must free), or LR_VALUE_UNDEFINED if not a promise. */
LRValue lr_promise_get_result(LRValue promise);

/* Check if a value is a Promise object. */
int lr_is_promise_val(LRValue val);

/* Create a new Promise object (internal). */
LRValue lr_new_promise(LRContext *ctx);

/* ── Internal helpers (exported for engine use) ─────────────────────────── */

/* Promise resolve/reject data, used by lr_new_promise_capability */
typedef struct {
    LRValue promise;
} PromiseResolveData;

/* Create a PromiseResolveData (non-static wrapper) */
PromiseResolveData *lr_promise_resolve_data_new(LRContext *ctx, LRValue promise);

/* Resolve/reject functions for promise capabilities */
LRValue promise_resolve_func(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
LRValue promise_reject_func(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);

/* Trigger a promise reaction via microtask */
void lr_promise_trigger_reaction(LRContext *ctx, LRPromiseReaction *reaction,
                                  LRValue value);

/* Resolve a promise with a value */
void lr_promise_resolve_internal(LRContext *ctx, LRValue promise, LRValue value);

/* Reject a promise with a reason */
void lr_promise_reject_internal(LRContext *ctx, LRValue promise, LRValue reason);

#ifdef __cplusplus
}
#endif

#endif /* LR_PROMISE_H */