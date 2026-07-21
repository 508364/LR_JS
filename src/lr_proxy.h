/*
 * LR_JS - Proxy Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the Proxy constructor and revocable proxy.
 * Trap invocation is handled by the engine layer (lr_engine.c).
 */
#ifndef LR_PROXY_H
#define LR_PROXY_H

#include "engine/lr_engine.h"

/* Forward declaration */
struct LR_Runtime;

#ifdef __cplusplus
extern "C" {
#endif

/* ── Public API ────────────────────────────────────────────────────────── */

/* Check if a value is a Proxy object */
int lr_is_proxy(LRValue val);

/* Get Proxy data from a Proxy object */
LRProxyData *lr_get_proxy_data(LRValue proxy);

/* Create a new Proxy object (internal) */
LRValue lr_new_proxy(LRContext *ctx, LRValue target, LRValue handler);

/* Initialize the Proxy constructor on the global object. */
void lr_proxy_init(struct LR_Runtime *rt);

#ifdef __cplusplus
}
#endif

#endif /* LR_PROXY_H */