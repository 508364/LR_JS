# -*- coding: utf-8 -*-
"""Regenerate all module files from backup with correct cross-module declarations."""
import os
import re

SRC = r'f:/C-C++/LR_JS/src/engine/lr_bytecode.c.bak_20250823'
DST = r'f:/C-C++/LR_JS/src/engine'

with open(SRC, 'r', encoding='utf-8') as f:
    lines = f.readlines()
total = len(lines)
print(f"Total lines in original: {total}")

# ── Section boundaries (1-based, inclusive) ──────────────────────────────────
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

# ── Modifications for each module ────────────────────────────────────────────
def apply_modifications(name, text):
    """Apply module-specific modifications."""
    mods = [
        # Common: make cross-module symbols non-static
        ('static int bc_env_debug_call    = -1;', 'int bc_env_debug_call    = -1;'),
        ('static int bc_env_debug_jitcall = -1;', 'int bc_env_debug_jitcall = -1;'),
        ('static int bc_env_debug_inline  = -1;', 'int bc_env_debug_inline  = -1;'),
        ('static inline uint32_t str_hash_fnv1a(const char *str, size_t len) {', 'uint32_t str_hash_fnv1a(const char *str, size_t len) {'),
        ('static inline int bc_debug_var(void)', 'int bc_debug_var(void)'),
        ('static char *bc_strdup(const char *s)', 'char *bc_strdup(const char *s)'),
        ('static LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];', 'LR_THREAD_LOCAL BCDynCacheEnt bc_dyn_cache[BC_DYN_CACHE_SIZE];'),
        ('static uint64_t bc_memo_hash_args(const LRValue *argv, int nargs)', 'uint64_t bc_memo_hash_args(const LRValue *argv, int nargs)'),
        ('static int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs)', 'int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs)'),
        # emit module: make bc_op_total_len non-static
        ('static int bc_op_total_len(uint8_t op);', 'int bc_op_total_len(uint8_t op);'),
        ('static int bc_op_total_len(uint8_t op)', 'int bc_op_total_len(uint8_t op)'),
    ]
    for old, new in mods:
        if old in text:
            text = text.replace(old, new, 1)
    return text

# ── Write modules ─────────────────────────────────────────────────────────────
for start, end, name, _deps in splits:
    chunk = lines[start-1:end]
    chunk_str = asciiize(''.join(chunk))
    preamble = STD_PREAMBLE % name
    full = preamble + chunk_str
    full = apply_modifications(name, full)

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

# ── Patch cache.h to include cross-module symbols ────────────────────────────
cache_h = os.path.join(DST, 'lr_bytecode_cache.h')
with open(cache_h, 'r', encoding='utf-8') as f:
    cache_h_content = f.read()

# Add cross-module declarations before the closing #endif
extra_decls = '''
/* Cross-module declarations (for exec.c, compile.c, etc.). */
extern int bc_env_debug_call;
extern int bc_env_debug_jitcall;
extern int bc_env_debug_inline;
uint32_t str_hash_fnv1a(const char *str, size_t len);
int bc_debug_var(void);
char *bc_strdup(const char *s);
uint64_t bc_memo_hash_args(const LRValue *argv, int nargs);
int bc_memo_args_eq(const BCMemoCacheEnt *e, const LRValue *argv, int nargs);
'''
# Insert before the last #endif
cache_h_content = cache_h_content.replace(
    '#endif /* LR_BYTECODE_CACHE */',
    extra_decls + '#endif /* LR_BYTECODE_CACHE */'
)
with open(cache_h, 'w', encoding='utf-8', newline='\n') as f:
    f.write(cache_h_content)
print("PATCHED cache.h")

# ── Patch emit.h to declare bc_op_total_len ──────────────────────────────────
emit_h = os.path.join(DST, 'lr_bytecode_emit.h')
with open(emit_h, 'r', encoding='utf-8') as f:
    emit_h_content = f.read()
emit_h_content = emit_h_content.replace(
    '#endif /* LR_BYTECODE_EMIT */',
    'int bc_op_total_len(uint8_t op);\n#endif /* LR_BYTECODE_EMIT */'
)
with open(emit_h, 'w', encoding='utf-8', newline='\n') as f:
    f.write(emit_h_content)
print("PATCHED emit.h")

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

# ── Add include to exec.c ────────────────────────────────────────────────────
exec_c = os.path.join(DST, 'lr_bytecode_exec.c')
with open(exec_c, 'r', encoding='utf-8') as f:
    exec_content = f.read()
if '#include "lr_bytecode_cache.h"' not in exec_content:
    exec_content = exec_content.replace(
        '#include "lr_bytecode.h"\n#include "lr_interp.h"',
        '#include "lr_bytecode.h"\n#include "lr_bytecode_cache.h"\n#include "lr_interp.h"'
    )
    with open(exec_c, 'w', encoding='utf-8', newline='\n') as f:
        f.write(exec_content)
    print("PATCHED exec.c to include lr_bytecode_cache.h")
else:
    print("exec.c already includes lr_bytecode_cache.h")

print("\nDone!")
