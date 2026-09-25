# -*- coding: utf-8 -*-
"""Regenerate all modules from backup.
Each module .c gets the standard preamble + its content.
Multi-line comment headers at split boundaries go with the NEXT module."""
import os

SRC = r'f:/C-C++/LR_JS/src/engine/lr_bytecode.c.bak_20250823'
DST = r'f:/C-C++/LR_JS/src/engine'

with open(SRC, 'r', encoding='utf-8') as f:
    lines = f.readlines()
total = len(lines)
print(f"Total lines in original: {total}")

# ── Section boundaries (1-based, inclusive) ──────────────────────────────────
# The comment header at the boundary goes with the NEXT module.
# cache content: 1-380  | header 381-383
# emit content:  384-775 | header 776-778
# compile content: 779-2602 | header 2603-2608
# value content: 2609-3010 | header 3011-3018
# exec content:  3019-6829 | header 6830-6833
# serialize content: 6834-6961 | header 6962-6964
# disasm content: 6965-7170
splits = [
    (1,     380,  'cache',   ["lr_engine.h", "lr_ast.h", "lr_bytecode.h"]),
    (381,   775,  'emit',    ["lr_engine.h", "lr_ast.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
    (776,   2602, 'compile', ["lr_engine.h", "lr_ast.h", "lr_bytecode.h", "lr_bytecode_cache.h", "lr_bytecode_emit.h"]),
    (2603,  3010, 'value',   ["lr_engine.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
    (3011,  6829, 'exec',    ["lr_engine.h", "lr_interp.h", "lr_bytecode.h", "lr_bytecode_cache.h", "lr_bytecode_value.h"]),
    (6830,  6961, 'serialize', ["lr_engine.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
    (6962,  total, 'disasm', ["lr_engine.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
]

# Verify boundaries
for start, end, name, _ in splits:
    first = lines[start-1].rstrip() if start <= total else ''
    last  = lines[end-1].rstrip()   if end   <= total else ''
    print("  [%s] %d..%d  first=%s  last=%s" % (name, start, end, first[:60], last[:60]))

# ── Standard preamble ────────────────────────────────────────────────────────
STD_PREAMBLE = '''\
/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: %s
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

'''

# ── Box-drawing char replacement ─────────────────────────────────────────────
REPL = {
    '\u2550': '=', '\u2551': '|', '\u2554': '+', '\u2560': '+',
    '\u2563': '+', '\u2557': '+', '\u255a': '+', '\u256c': '+',
    '\u2569': '+', '\u2566': '+', '\u256b': '+', '\u250c': '+',
    '\u2510': '+', '\u2514': '+', '\u2518': '+', '\u251c': '+',
    '\u2524': '|', '\u252c': '|', '\u2534': '|', '\u253c': '+',
    '\u2500': '-', '\u2502': '|',
}

def asciiize(text):
    return ''.join(REPL.get(ch, ch) for ch in text)

# ── Write modules ─────────────────────────────────────────────────────────────
for start, end, name, _deps in splits:
    chunk = lines[start-1:end]
    chunk_str = asciiize(''.join(chunk))
    preamble = STD_PREAMBLE % name
    full = preamble + chunk_str

    outpath = os.path.join(DST, 'lr_bytecode_%s.c' % name)
    with open(outpath, 'w', encoding='utf-8', newline='\n') as f:
        f.write(full)
    print("WROTE [%s] %d..%d -> %s (%d lines)" % (name, start, end, outpath, len(chunk)))

# ── Write header files ────────────────────────────────────────────────────────
for _, _, name, deps in splits:
    guard = 'LR_BYTECODE_' + name.upper()
    includes = '\n'.join('  #include "%s"' % d for d in deps)
    hdr = (
        '/*\n'
        ' * LR_JS — Bytecode VM (modular)\n'
        ' *\n'
        ' * Module: %s\n'
        ' */\n'
        '#ifndef %s\n'
        '#define %s\n'
        '\n'
        '%s\n'
        '\n'
        '#ifdef __cplusplus\n'
        '}\n'
        '#endif\n'
        '#endif /* %s */\n'
    ) % (name, guard, guard, includes, guard)
    outpath = os.path.join(DST, 'lr_bytecode_%s.h' % name)
    with open(outpath, 'w', encoding='utf-8', newline='\n') as f:
        f.write(hdr)
    print("WROTE [%s] header -> %s" % (name, outpath))

# ── Write stub (main lr_bytecode.c) ───────────────────────────────────────────
stub = (
    '/*\n'
    ' * LR_JS — Bytecode VM (modular stub)\n'
    ' */\n'
    '#include "lr_bytecode.h"\n'
    '#include "lr_interp.h"\n'
    '#include "lr_jit.h"\n'
    '#include "lr_ast.h"\n'
    '#include "lr_platform.h"\n'
    '\n'
    'static int bc_env_debug_call    = -1;\n'
    'static int bc_env_debug_jitcall = -1;\n'
    'static int bc_env_debug_inline  = -1;\n'
)
for _, _, name, _ in splits:
    stub += '#include "lr_bytecode_%s.h"\n' % name

outpath = os.path.join(DST, 'lr_bytecode.c')
with open(outpath, 'w', encoding='utf-8', newline='\n') as f:
    f.write(stub)
print("WROTE [stub] -> %s (%d bytes)" % (outpath, len(stub)))

print("\nDone!")
