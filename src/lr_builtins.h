/*
 * LR_JS - ES2022 Built-in Objects Header
 * Pure C, ES2022-compatible, self-implemented JS engine
 */
#ifndef LR_BUILTINS_H
#define LR_BUILTINS_H

#include "engine/lr_engine.h"

/* ── Initialization ───────────────────────────────────────────────────── */

void lr_builtins_core_init(struct LR_Runtime *rt);
void lr_builtins_extra_init(struct LR_Runtime *rt);

#endif /* LR_BUILTINS_H */