# -*- coding: utf-8 -*-
"""Regenerate cache.c with proper non-static declarations."""
import re

SRC = r'f:/C-C++/LR_JS/src/engine/lr_bytecode.c.bak_20250823'
DST = r'f:/C-C++/LR_JS/src/engine/lr_bytecode_cache.c'

with open(SRC, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Standard preamble for cache module
preamble = """/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: cache
 */
#include "lr_bytecode.h"
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

"""

# Get cache content (lines 1-380 from backup, 0-indexed: 0-379)
cache_content = lines[:380]

text = ''.join(cache_content)

# Find the start of program management section
match = re.search(r'\n/\* ════════ PROGRAM MANAGEMENT', text)
if match:
    new_text = preamble + text[match.start():]
else:
    new_text = text

# Apply specific changes
changes = [
    ('static int bc_env_debug_call    = -1;', 'int bc_env_debug_call    = -1;'),
    ('static int bc_env_debug_jitcall = -1;', 'int bc_env_debug_jitcall = -1;'),
    ('static int bc_env_debug_inline  = -1;', 'int bc_env_debug_inline  = -1;'),
    ('static inline uint32_t str_hash_fnv1a(const char *str, size_t len) {', 'uint32_t str_hash_fnv1a(const char *str, size_t len) {'),
    ('static inline int bc_debug_var(void)', 'int bc_debug_var(void)'),
    ('static char *bc_strdup(const char *s)', 'char *bc_strdup(const char *s)'),
    ('static LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];', 'LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];'),
    ('static uint64_t bc_memo_hash_args(const LRValue *argv, int nargs)', 'uint64_t bc_memo_hash_args(const LRValue *argv, int nargs)'),
    ('static int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs)', 'int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs)'),
]

for old, new in changes:
    if old in new_text:
        new_text = new_text.replace(old, new, 1)
    else:
        print(f'WARNING: Could not find: {old[:60]}')

with open(DST, 'w', encoding='utf-8', newline='\n') as f:
    f.write(new_text)

print(f'WROTE cache.c ({len(new_text.splitlines())} lines)')
