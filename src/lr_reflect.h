/*
 * LR_JS - Reflect Implementation
 * Pure C, ES2022-compatible.
 *
 * Implements the Reflect object with all static methods.
 */
#ifndef LR_REFLECT_H
#define LR_REFLECT_H

#include "engine/lr_engine.h"

/* Forward declaration */
struct LR_Runtime;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the Reflect object on the global object. */
void lr_reflect_init(struct LR_Runtime *rt);

#ifdef __cplusplus
}
#endif

#endif /* LR_REFLECT_H */