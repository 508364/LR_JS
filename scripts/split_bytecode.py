# -*- coding: utf-8 -*-
"""
Split lr_bytecode.c into modular .c/.h files.
Replaces Unicode box-drawing chars with ASCII equivalents to avoid
compilation errors on Windows/MinGW.
Each module gets its own standard includes so it compiles independently.
"""
import os, re

SRC = r'f:/C-C++/LR_JS/src/engine/lr_bytecode.c.bak_20250823'
DST = r'f:/C-C++/LR_JS/src/engine'

with open(SRC, 'r', encoding='utf-8') as f:
    lines = f.readlines()
total = len(lines)
print(f"Total lines in original: {total}")

# ── Section boundaries ────────────────────────────────────────────────────────
splits = [
    (1,     380, 'cache',  ["lr_engine.h", "lr_ast.h", "lr_bytecode.h"]),
    (381,   776, 'emit',   ["lr_engine.h", "lr_ast.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
    (777,  2603, 'compile', ["lr_engine.h", "lr_ast.h", "lr_bytecode.h", "lr_bytecode_cache.h", "lr_bytecode_emit.h"]),
    (2604, 3011, 'value',  ["lr_engine.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
    (3012, 6829, 'exec',   ["lr_engine.h", "lr_interp.h", "lr_bytecode.h", "lr_bytecode_cache.h", "lr_bytecode_value.h"]),
    (6830, 6961, 'serialize', ["lr_engine.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
    (6962, total, 'disasm', ["lr_engine.h", "lr_bytecode.h", "lr_bytecode_cache.h"]),
]

# ── Standard preamble (repeated in every module .c) ───────────────────────────
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

# ── Module definitions ────────────────────────────────────────────────────────
module_headers = {}
for _, _, name, deps in splits:
    guard = 'LR_BYTECODE_' + name.upper()
    includes = '\n'.join('  #include "%s"' % d for d in deps)
    module_headers[name] = (
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

# ── Common C preamble (only in main stub) ────────────────────────────────────
common_c_preamble = (
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

# ── Write modules ─────────────────────────────────────────────────────────────
for start, end, name, _deps in splits:
    chunk = lines[start-1:end]
    chunk_str = ''.join(chunk)

    # Replace box-drawing chars with ASCII
    REPL = {
        '\u2550': '=', '\u2551': '|', '\u2554': '+', '\u2560': '+',
        '\u2563': '+', '\u2557': '+', '\u255a': '+', '\u256c': '+',
        '\u2569': '+', '\u2566': '+', '\u256b': '+', '\u250c': '+',
        '\u2510': '+', '\u2514': '+', '\u2518': '+', '\u251c': '+',
        '\u2524': '|', '\u252c': '|', '\u2534': '|', '\u253c': '+',
        '\u2500': '-', '\u2502': '|',
    }
    out_chars = []
    for ch in chunk_str:
        out_chars.append(REPL.get(ch, ch))
    chunk_str = ''.join(out_chars)

    # Prepend standard preamble
    preamble = STD_PREAMBLE % name
    full = preamble + chunk_str

    outpath = os.path.join(DST, 'lr_bytecode_%s.c' % name)
    with open(outpath, 'w', encoding='utf-8', newline='\n') as f:
        f.write(full)
    print("  [%s] %d..%d -> %s (%d lines)" % (name, start, end, outpath, len(chunk)))

# ── Write header files ────────────────────────────────────────────────────────
for name, hdr in module_headers.items():
    outpath = os.path.join(DST, 'lr_bytecode_%s.h' % name)
    with open(outpath, 'w', encoding='utf-8', newline='\n') as f:
        f.write(hdr)
    print("  [%s] -> %s" % (name, outpath))

# ── Write stub (main lr_bytecode.c) ───────────────────────────────────────────
stub = common_c_preamble
for _, _, name, _ in splits:
    stub += '#include "lr_bytecode_%s.h"\n' % name

outpath = os.path.join(DST, 'lr_bytecode.c')
with open(outpath, 'w', encoding='utf-8', newline='\n') as f:
    f.write(stub)
print("  [stub] -> %s (%d bytes)" % (outpath, len(stub)))

print("\nDone!")
