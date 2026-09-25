/*
 * LR_JS - Unified JIT Library: MIR Frontend + SLJIT Backend
 *
 * Architecture:
 *   Bytecode -> MIR (frontend) -> native code via SLJIT (backend)
 *
 * This is the single public header for the JIT library. It defines all types,
 * constants, and API functions exposed to callers.
 */
#ifndef LR_JIT_H
#define LR_JIT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "lr_engine.h"
#include "lr_interp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── JIT compilation thresholds ───────────────────────────────────────── */
/* V8-inspired: compile only after sufficient profiling to amortize
 * compilation overhead.  Simple functions need many more calls before
 * compilation pays off.
 * Tuned for LR_JS: lower thresholds to trigger JIT earlier for hot paths. */
#define LR_JIT_HOT_FUNC_CALLS   3        /* calls before compiling a function (was 10) */
#define LR_JIT_HOT_LOOP_ITERS   50       /* loop iterations before compiling (was 100) */
#define LR_JIT_MAX_CODE_SIZE    (64 * 1024)
#define LR_JIT_CACHE_MAX_SIZE   (4 * 1024 * 1024)
#define LR_JIT_INLINE_THRESHOLD 3        /* inline if callee has <=3 MIR ops */
#define LR_JIT_UNROLL_FACTOR    8        /* unroll simple loops by this factor */

/* Hotspot analysis thresholds (runtime profiling-based) */
#define LR_JIT_HOTSPOT_FUNC_MIN_CALLS  2    /* min calls to consider function hot */
#define LR_JIT_HOTSPOT_LOOP_MIN_ITER   100   /* min iterations to consider loop hot */
#define LR_JIT_HOTSPOT_AGRESSIVE      1     /* 1 = compile aggressively when hotspot detected */

/* Stack frame limits for JIT codegen (must match lr_jit_codegen.c MAX_LOCALS) */
#define LR_JIT_MAX_LOCALS  128

/* Debug flag: JIT debug output is gated at RUNTIME by --debug / env, NOT by
 * compile-time macro.  This keeps the Release CLI output clean (matching V8 /
 * QuickJS) while still letting `lr_js --debug script.js` emit full diagnostics. */
#ifndef LR_DEBUG_JIT
#define LR_DEBUG_JIT 0
#endif

/* Runtime verbosity switch.  Set to 1 when the user passes --debug (see
 * cli/main.c) or when LR_DEBUG / LR_DEBUG_JIT env vars are present. */
extern int g_lr_debug;

static inline int lr_debug_jit_enabled(void) {
    if (g_lr_debug) return 1;
    if (getenv("LR_DEBUG_JIT") != NULL) return 1;
    if (getenv("LR_DEBUG") != NULL) return 1;
    return 0;
}

/* ── Unified JIT debug logging ─────────────────────────────────────────
 * LR_JIT_DBG  : emits to stderr ONLY when debugging is enabled
 *               (LR_DEBUG_JIT / LR_DEBUG env, or --debug).  This is the
 *               ONE macro for every JIT/MIR/CGEN/RT chain trace so a
 *               single switch turns the whole pipeline on/off and the
 *               prefix is predictable: [JIT], [MIR], [CGEN], [RT].
 * LR_JIT_ERR  : ALWAYS emits to stderr, independent of debug.  Used for
 *               genuine failures (SLJIT errors, bad codegen) that must
 *               never be silenced.  Prefix: [JIT-ERR] or [MIR-ERR].
 * LR_DEBUG_PRINT : legacy alias of LR_JIT_DBG (kept for compatibility). */
#define LR_JIT_DBG(...)   do { if (lr_debug_jit_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)
#define LR_JIT_ERR(...)   do { fprintf(stderr, __VA_ARGS__); } while (0)
#define LR_DEBUG_PRINT(...) LR_JIT_DBG(__VA_ARGS__)

/* ── JIT entry point signature ──────────────────────────────────────────
 * Compiled function receives:
 *   rdi: Interpreter *interp
 *   rsi: uint8_t *ip  (resume IP for bailout)
 *   edx: LRValue *result  (hidden return value pointer)
 * Writes LRValue to *result. Bailout writes tag=-1 to result. */
typedef void (*LRJITEntry)(Interpreter *interp, uint8_t *ip, LRValue *result);

/* ── MIR Instruction Set (frontend internal) ────────────────────────────
 * Compact IR used between bytecode and native code generation. */

typedef enum {
    /* Constants */
    MIR_OP_const_i32,

    /* Variable operations */
    MIR_OP_load_local,
    MIR_OP_store_local,

    /* Unary ops */
    MIR_OP_not_i32,
    MIR_OP_neg_i32,
    MIR_OP_neg_f64,

    /* Binary ops (i32) */
    MIR_OP_add_i32,
    MIR_OP_sub_i32,
    MIR_OP_mul_i32,
    MIR_OP_div_i32,
    MIR_OP_mod_i32,
    MIR_OP_and_i32,
    MIR_OP_or_i32,
    MIR_OP_xor_i32,
    MIR_OP_shl_i32,
    MIR_OP_shr_i32,
    MIR_OP_sar_i32,

    /* Binary ops (f64) — mixed int32/f64 → promote to f64 */
    MIR_OP_add_f64,
    MIR_OP_sub_f64,
    MIR_OP_mul_f64,
    MIR_OP_div_f64,
    MIR_OP_mod_f64,

    /* Comparison ops (i32 inputs, produce i32: 0 or 1) */
    MIR_OP_lt_i32,
    MIR_OP_gt_i32,
    MIR_OP_le_i32,
    MIR_OP_ge_i32,
    MIR_OP_eq_i32,
    MIR_OP_ne_i32,

    /* Comparison ops (f64 inputs, produce i32: 0 or 1) */
    MIR_OP_lt_f64,
    MIR_OP_gt_f64,
    MIR_OP_le_f64,
    MIR_OP_ge_f64,
    MIR_OP_eq_f64,
    MIR_OP_ne_f64,

    /* Control flow */
    MIR_OP_jump,
    MIR_OP_jump_if_false,
    MIR_OP_jump_if_true,
    MIR_OP_loop_start,
    MIR_OP_loop_end,
    MIR_OP_loop_guard,

    /* Call / return */
    MIR_OP_call,
    MIR_OP_ret,

    /* Bailout (unsupported op -> fall back to interpreter) */
    MIR_OP_bailout,

    /* Runtime callbacks (call C helper to handle complex ops) */
    MIR_OP_load_var,       /* imm=name_idx, ptr=prog -> dst=LRValue  */
    MIR_OP_runtime_call,   /* imm=argc, src1=callee_var -> dst=result */
    MIR_OP_move,           /* dst = copy of src1 (16-byte LRValue)    */

    /* Stack operations */
    MIR_OP_push_undefined,
    MIR_OP_push_null,
    MIR_OP_push_true,
    MIR_OP_push_false,
    MIR_OP_push_int32,
    MIR_OP_pop,
    MIR_OP_dup,
    MIR_OP_swap,

    /* ── Extended core MIR instructions ─────────────────────────────────
     * These extend JIT coverage to bytecode ops that previously bailed
     * out, letting recursive / object-heavy functions stay in native code.
     * All use runtime-callback bridges (same pattern as MIR_OP_load_var)
     * except MIR_OP_jump_if_not_nullish which is in-line tag inspection. */

    /* Constant pool loads (BC_PUSH_FLOAT64 / BC_PUSH_STRING) */
    MIR_OP_const_f64,      /* imm=pool_idx, ptr=prog -> dst=LRValue  */
    MIR_OP_const_string,   /* imm=pool_idx, ptr=prog -> dst=LRValue  */

    /* `this` binding (BC_PUSH_THIS) */
    MIR_OP_load_this,      /* dst = LRValue (current `this`)          */

    /* Variable write-back (BC_STORE_VAR / BC_INC_VAR) */
    MIR_OP_store_var,       /* imm=name_idx, ptr=prog, src1=val_var   */
    MIR_OP_inc_var,         /* imm=name_idx, ptr=prog -> dst=old_val  */

    /* typeof family (BC_TYPEOF / BC_TYPEOF_VAR) */
    MIR_OP_typeof,          /* src1=val -> dst=string                 */
    MIR_OP_typeof_var,      /* imm=name_idx, ptr=prog -> dst=string   */

    /* Inline coercions (BC_TO_STRING / BC_TO_NUMBER / BC_TO_BOOL / BC_POS) */
    MIR_OP_to_string,       /* src1=val -> dst=string                 */
    MIR_OP_to_number,       /* src1=val -> dst=number                */
    MIR_OP_to_bool,         /* src1=val -> dst=bool                   */
    MIR_OP_pos,             /* src1=val -> dst=number (unary +)       */

    /* Arithmetic runtime (BC_POW) */
    MIR_OP_pow,             /* src1=base, src2=exp -> dst=number      */

    /* Object/array allocation (BC_NEW_OBJECT / BC_NEW_ARRAY) */
    MIR_OP_new_object,      /* dst = {}                               */
    MIR_OP_new_array,       /* imm=n, src1=elems_base -> dst=array    */

    /* Property/element access (BC_GET/SET_PROP, BC_GET/SET_ELEM) */
    MIR_OP_get_prop,        /* imm=name_idx, ptr=prog, src1=obj -> dst=val */
    MIR_OP_set_prop,        /* imm=name_idx, ptr=prog, src1=obj, src2=val -> dst=val */
    MIR_OP_get_elem,        /* src1=obj, src2=key -> dst=val          */
    MIR_OP_set_elem,        /* src1=base(obj,key,val) -> dst=val     */
    MIR_OP_add_prop,        /* fused add prop: imm=name_idx, src1=obj, src2=rhs -> dst=obj_in */

    /* Operators requiring runtime (BC_IN / BC_INSTANCEOF) */
    MIR_OP_in,              /* src1=obj, src2=key -> dst=bool         */
    MIR_OP_instanceof,      /* src1=obj, src2=ctor -> dst=bool        */

    /* Exception (BC_THROW) */
    MIR_OP_throw,           /* src1=val -> (does not return)          */

    /* Nullish jump (BC_JUMP_IF_NOT_NULLISH) — inlined tag check */
    MIR_OP_jump_if_not_nullish, /* imm=byte_off, src1=val (kept on stack) */

    /* Method/element calls (BC_CALL_METHOD / BC_CALL_ELEM / BC_NEW) */
    MIR_OP_call_method,     /* imm=(argc<<16)|name_idx, ptr=prog,
                             * src1=base_var(obj+args) -> dst=result   */
    MIR_OP_call_elem,       /* imm=argc, src1=base_var(callee,obj,key,args)
                             * -> dst=result                            */
    MIR_OP_construct,       /* imm=argc, src1=base_var(callee+args) -> dst=instance */

    /* Scope-aware call (V8 per-call-frame style for recursive JIT)
     * From scope dynamically loads each arg, then calls runtime.
     * imm=argc, ptr=caller_prog, src1=callee_var, dst=result.
     * Preceding MIR_OP_load_scope instructions load args from scope. */
    MIR_OP_scope_call,      /* recursive-safe call: imm=argc, ptr=prog
                             * src1=callee_var, dst=result.
                             * Previous instructions should load each arg
                             * from scope via MIR_OP_load_scope. */
    MIR_OP_load_scope,      /* Load from current_scope->values[slot] to spill var.
                             * dst=spill_var, src1=scope_slot_index */
    MIR_OP_inline_call,     /* Direct native-to-native call: imm=argc, ptr=callee_prog,
                             * src1=callee_var, dst=result.  Emits direct CALL to
                             * callee_prog->jit_entry, skipping runtime bridge. */

    MIR_OP_load_prop,          /* (result, obj, prop_name) = obj[prop] — string property load */
    MIR_OP_get_prop_cached,    /* (result, obj, prop_name, cache_slot) — cached property access */
    MIR_OP_store_prop_cached,  /* obj[prop] = val with cache update */
    MIR_OP_array_new,          /* (result) = [] */
    MIR_OP_array_push,         /* push(val) onto arr */
    MIR_OP_array_pop,          /* (result) = pop(arr) */
    MIR_OP_array_length,       /* (result) = arr.length */
    MIR_OP_array_map,          /* (result) = arr.map(func) */
    MIR_OP_array_filter,       /* (result) = arr.filter(func) */
    MIR_OP_array_foreach,      /* arr.forEach(func) */
    MIR_OP_array_concat,       /* (result) = arr.concat(other) */
    MIR_OP_array_slice,        /* (result) = arr.slice(start, end) */
    MIR_OP_array_splice,       /* arr.splice(start, deleteCount, ...items) */
    MIR_OP_array_sort,         /* arr.sort([cmp]) */
    MIR_OP_array_reverse,      /* arr.reverse() */
    MIR_OP_array_join,         /* (result) = arr.join(separator) */
    MIR_OP_array_includes,     /* (result) = arr.includes(value) */
    MIR_OP_array_find,         /* (result) = arr.find(predicate) */
    MIR_OP_array_find_index,   /* (result) = arr.findIndex(predicate) */
    MIR_OP_array_reduce,       /* (result) = arr.reduce(callback, initial) */
    MIR_OP_array_from,         /* (result) = Array.from(source) */
    MIR_OP_array_of,           /* (result) = Array.of(...items) */
    MIR_OP_object_keys,        /* (result) = Object.keys(obj) */
    MIR_OP_object_values,      /* (result) = Object.values(obj) */
    MIR_OP_object_entries,     /* (result) = Object.entries(obj) */
    MIR_OP_string_split,       /* (result) = str.split(separator) */
    MIR_OP_string_trim,        /* (result) = str.trim() */
    MIR_OP_string_replace,     /* (result) = str.replace(search, replace) */
    MIR_OP_string_substring,   /* (result) = str.substring(start, end) */
    MIR_OP_string_charAt,      /* (result) = str.charAt(index) */
    MIR_OP_string_indexOf,     /* (result) = str.indexOf(search) */
    MIR_OP_string_toLowerCase, /* (result) = str.toLowerCase() */
    MIR_OP_string_toUpperCase, /* (result) = str.toUpperCase() */
    MIR_OP_number_parseInt,    /* (result) = parseInt(str) */
    MIR_OP_number_parseFloat,  /* (result) = parseFloat(str) */
    MIR_OP_number_isNaN,       /* (result) = isNaN(num) */
    MIR_OP_number_isFinite,    /* (result) = isFinite(num) */
    MIR_OP_math_min,           /* (result) = Math.min(a, b) */
    MIR_OP_math_max,           /* (result) = Math.max(a, b) */
    MIR_OP_math_floor,         /* (result) = Math.floor(a) */
    MIR_OP_math_ceil,          /* (result) = Math.ceil(a) */
    MIR_OP_math_round,         /* (result) = Math.round(a) */
    MIR_OP_math_sqrt,          /* (result) = Math.sqrt(a) */
    MIR_OP_math_random,        /* (result) = Math.random() */
    MIR_OP_math_abs,           /* (result) = Math.abs(a) */
    MIR_OP_math_pow,           /* (result) = Math.pow(base, exp) */
    MIR_OP_math_sign,          /* (result) = Math.sign(a) */
    MIR_OP_string_length,      /* (result) = string.length */
    MIR_OP_string_concat,      /* (result) = a + b (string concat) */
    MIR_OP_string_substr,      /* (result) = string.substr(start, length) */
    MIR_OP_string_repeat,      /* (result) = string.repeat(count) */
    MIR_OP_object_create,      /* (result) = Object.create(proto) */
    MIR_OP_object_assign,      /* (result) = Object.assign(target, ...sources) */
    MIR_OP_object_has_own,     /* (result) = Object.hasOwn(obj, prop) */
    MIR_OP_number_toString,    /* (result) = num.toString() */
    MIR_OP_number_toFixed,     /* (result) = num.toFixed(digits) */
    MIR_OP_number_toExponential,/* (result) = num.toExponential(fractionDigits) */
    MIR_OP_number_toPrecision, /* (result) = num.toPrecision(precision) */
    MIR_OP_set_elem_str,       /* arr["key"] = val — string-indexed */
    MIR_OP_get_elem_str,       /* (result) = arr["key"] — string-indexed */
    MIR_OP_array_shift,        /* (result) = shift(arr) */
    MIR_OP_array_unshift,      /* unshift(arr, ...items) */
    MIR_OP_array_reverse_inplace, /* reverse(arr) in-place */
    MIR_OP_array_fill,         /* fill(arr, value, start, end) */
    MIR_OP_array_find_last,    /* (result) = arr.findLast(predicate) */
    MIR_OP_array_find_last_index,/* (result) = arr.findLastIndex(predicate) */
    MIR_OP_array_flat,         /* (result) = arr.flat(depth) */
    MIR_OP_array_flat_map,     /* (result) = arr.flatMap(callback) */
    MIR_OP_array_with,         /* (result) = arr.with(index, value) */
    MIR_OP_array_to_reversed,  /* (result) = arr.toReversed() */
    MIR_OP_array_to_sorted,    /* (result) = arr.toSorted(cmp) */
    MIR_OP_array_to_spliced,   /* (result) = arr.toSpliced(start, deleteCount, ...items) */
    MIR_OP_array_to_string,    /* (result) = arr.toString() */
    MIR_OP_array_at,           /* (result) = arr.at(index) */
    MIR_OP_object_from_entries,/* (result) = Object.fromEntries(entries) */
    MIR_OP_string_pad_start,   /* (result) = str.padStart(targetLength, pad) */
    MIR_OP_string_pad_end,     /* (result) = str.padEnd(targetLength, pad) */
    MIR_OP_string_match,       /* (result) = str.match(regex) */
    MIR_OP_string_search,      /* (result) = str.search(regex) */
    MIR_OP_string_match_all,   /* (result) = str.matchAll(regex) */
    MIR_OP_array_is_array,     /* (result) = Array.isArray(val) */
    MIR_OP_number_is_integer,  /* (result) = Number.isInteger(val) */
    MIR_OP_number_is_safe_integer,/* (result) = Number.isSafeInteger(val) */
    MIR_OP_number_is_finite_user,/* (result) = Number.isFinite(val) */
    MIR_OP_number_is_nan_user, /* (result) = Number.isNaN(val) */
    MIR_OP_number_value_of,    /* (result) = num.valueOf() */
    MIR_OP_string_value_of,    /* (result) = str.valueOf() */
    MIR_OP_object_get_own_property_descriptor,/* (result) = Object.getOwnPropertyDescriptor(obj, prop) */
    MIR_OP_object_define_property,/* defineProperty(obj, prop, descriptor) */
    MIR_OP_object_freeze,      /* (result) = Object.freeze(obj) */
    MIR_OP_object_seal,        /* (result) = Object.seal(obj) */
    MIR_OP_object_prevent_extensions,/* (result) = Object.preventExtensions(obj) */
    MIR_OP_object_is_extensible,/* (result) = Object.isExtensible(obj) */
    MIR_OP_object_is_frozen,   /* (result) = Object.isFrozen(obj) */
    MIR_OP_object_is_sealed,   /* (result) = Object.isSealed(obj) */
    MIR_OP_global_this,        /* (result) = globalThis */
    MIR_OP_void_0,             /* (result) = void 0 (undefined) */
    MIR_OP_typeof_obj,         /* (result) = typeof object — full type check */
    MIR_OP_symbol_for,         /* (result) = Symbol.for(key) */
    MIR_OP_symbol_key_for,     /* (result) = Symbol.keyFor(sym) */
    MIR_OP_weak_map_new,       /* (result) = new WeakMap() */
    MIR_OP_weak_map_set,       /* weakMap.set(key, value) */
    MIR_OP_weak_map_get,       /* (result) = weakMap.get(key) */
    MIR_OP_weak_map_has,       /* (result) = weakMap.has(key) */
    MIR_OP_weak_map_delete,    /* weakMap.delete(key) */
    MIR_OP_weak_set_new,       /* (result) = new WeakSet() */
    MIR_OP_weak_set_add,       /* weakSet.add(value) */
    MIR_OP_weak_set_has,       /* (result) = weakSet.has(value) */
    MIR_OP_weak_set_delete,    /* weakSet.delete(value) */
    MIR_OP_map_new,            /* (result) = new Map() */
    MIR_OP_map_set,            /* map.set(key, value) */
    MIR_OP_map_get,            /* (result) = map.get(key) */
    MIR_OP_map_has,            /* (result) = map.has(key) */
    MIR_OP_map_delete,         /* map.delete(key) */
    MIR_OP_map_clear,          /* map.clear() */
    MIR_OP_map_size,           /* (result) = map.size */
    MIR_OP_set_new,            /* (result) = new Set() */
    MIR_OP_set_add,            /* set.add(value) */
    MIR_OP_set_has,            /* (result) = set.has(value) */
    MIR_OP_set_delete,         /* set.delete(value) */
    MIR_OP_set_clear,          /* set.clear() */
    MIR_OP_set_size,           /* (result) = set.size */
    MIR_OP_regexp_new,         /* (result) = new RegExp(pattern, flags) */
    MIR_OP_regexp_exec,        /* (result) = regexp.exec(str) */
    MIR_OP_date_new,           /* (result) = new Date() */
    MIR_OP_date_now,           /* (result) = Date.now() */
    MIR_OP_typed_array_new,    /* (result) = new Uint8Array(buffer) */
    MIR_OP_data_view_new,      /* (result) = new DataView(buffer) */
    MIR_OP_error_new,          /* (result) = new Error(message) */
    MIR_OP_type_error_new,     /* (result) = new TypeError(message) */
    MIR_OP_range_error_new,    /* (result) = new RangeError(message) */
    MIR_OP_reference_error_new,/* (result) = new ReferenceError(message) */
    MIR_OP_syntax_error_new,   /* (result) = new SyntaxError(message) */
    MIR_OP_eval_error_new,     /* (result) = new EvalError(message) */
    MIR_OP_uri_error_new,      /* (result) = new URIError(message) */
    MIR_OP_console_log,        /* console.log(args...) */
    MIR_OP_console_error,      /* console.error(args...) */
    MIR_OP_console_warn,       /* console.warn(args...) */
    MIR_OP_process_exit,       /* process.exit(code) */
    MIR_OP_process_env_get,    /* (result) = process.env[key] */
    MIR_OP_process_env_set,    /* process.env[key] = value */
    MIR_OP_require,            /* (result) = require(module) */
    MIR_OP_module_exports,     /* module.exports = val */
    MIR_OP_module_require,     /* module.require(path) */
    MIR_OP_class_new,          /* (result) = new ClassName(args) */
    MIR_OP_class_static_call,  /* (result) = ClassName.staticMethod(args) */
    MIR_OP_super_call,         /* super(args) */
    MIR_OP_super_prop,         /* (result) = super.prop */
    MIR_OP_super_assign,       /* super.prop = val */
    MIR_OP_private_field_get,  /* (result) = obj.#field */
    MIR_OP_private_field_set,  /* obj.#field = val */
    MIR_OP_private_field_in,   /* (result) = #field in obj */
    MIR_OP_iterator_next,      /* (result) = iterator.next() */
    MIR_OP_iterator_return,    /* (result) = iterator.return(value) */
    MIR_OP_iterator_throw,     /* (result) = iterator.throw(error) */
    MIR_OP_async_await,        /* (result) = await promise */
    MIR_OP_async_resolve,      /* (result) = Promise.resolve(value) */
    MIR_OP_async_reject,       /* (result) = Promise.reject(error) */
    MIR_OP_async_all,          /* (result) = Promise.all(iterable) */
    MIR_OP_async_all_settled,  /* (result) = Promise.allSettled(iterable) */
    MIR_OP_async_race,         /* (result) = Promise.race(iterable) */
    MIR_OP_async_any,          /* (result) = Promise.any(iterable) */
    MIR_OP_generator_yield,    /* yield value */
    MIR_OP_generator_throw,    /* generator.throw(error) */
    MIR_OP_generator_return,   /* generator.return(value) */
    MIR_OP_big_int_new,        /* (result) = BigInt(value) */
    MIR_OP_big_int_to_string,  /* (result) = bigint.toString() */
    MIR_OP_big_int_to_locale_string,/* (result) = bigint.toLocaleString() */
    MIR_OP_big_int_compare,    /* (result) = bigint1 === bigint2 */
    MIR_OP_temporal_now,       /* (result) = Temporal.Now.instant() */
    MIR_OP_temporal_zoned_now, /* (result) = Temporal.Now.zonedDateTimeISO(timeZone) */
    MIR_OP_import_meta,        /* (result) = import.meta */
    MIR_OP_dynamic_import,     /* (result) = import(module) */
    MIR_OP_top_LEVEL_AWAIT,    /* await expression at top level */
    MIR_OP_debugger,           /* debugger statement */
    MIR_OP_label_break,        /* break label */
    MIR_OP_label_continue,     /* continue label */
    MIR_OP_destructure_array,  /* [a, b, ...rest] = expr */
    MIR_OP_destructure_object, /* {a, b, ...rest} = expr */
    MIR_OP_spread_array,       /* [a, ...rest] */
    MIR_OP_spread_object,      /* {a, ...rest} */
    MIR_OP_template_literal,   /* `template ${expr}` */
    MIR_OP_module_export_star, /* export * from 'module' */
    MIR_OP_module_export_named,/* export {name} */
    MIR_OP_module_export_default,/* export default expr */
    MIR_OP_module_import_decl, /* import decl */
    MIR_OP_module_namespace_import,/* import * as ns from 'module' */
    MIR_OP_module_default_import,/* import def from 'module' */
    MIR_OP_module_named_import,/* import {a, b} from 'module' */
    MIR_OP_module_side_effect_import,/* import 'module' */
    MIR_OP_WebAssembly_instantiate,/* (result) = WebAssembly.instantiate(bytes) */
    MIR_OP_WebAssembly_compile,/* (result) = WebAssembly.compile(bytes) */
    MIR_OP_shared_array_buffer_new,/* (result) = new SharedArrayBuffer(byteLength) */
    MIR_OP_atomics_load,       /* (result) = Atomics.load(typedArray, index) */
    MIR_OP_atomics_store,      /* Atomics.store(typedArray, index, value) */
    MIR_OP_atomics_add,        /* (result) = Atomics.add(typedArray, index, value) */
    MIR_OP_atomics_sub,        /* (result) = Atomics.sub(typedArray, index, value) */
    MIR_OP_atomics_and,        /* (result) = Atomics.and(typedArray, index, value) */
    MIR_OP_atomics_or,         /* (result) = Atomics.or(typedArray, index, value) */
    MIR_OP_atomics_xor,        /* (result) = Atomics.xor(typedArray, index, value) */
    MIR_OP_atomics_exchange,   /* (result) = Atomics.exchange(typedArray, index, value) */
    MIR_OP_atomics_compare_exchange,/* (result) = Atomics.compareExchange(typedArray, index, expected, replacement) */
    MIR_OP_atomics_is_lock_free,/* (result) = Atomics.isLockFree(size) */
    MIR_OP_atomics_wait,       /* (result) = Atomics.wait(typedArray, index, value[, timeout]) */
    MIR_OP_atomics_notify,     /* (result) = Atomics.notify(typedArray, index[, count]) */
    MIR_OP_atomics_cache,      /* cache TypedArray info: dst[0]=base_ptr, dst[1]=offset, dst[2]=esize, dst[3]=magic; src1=typedArray */
    MIR_OP_atomics_inline_load,/* same as load but uses pre-cached TypedArray info at dst+0..3 */
    MIR_OP_atomics_inline_store,
    MIR_OP_atomics_inline_add,
    MIR_OP_atomics_inline_sub,
    MIR_OP_atomics_inline_and,
    MIR_OP_atomics_inline_or,
    MIR_OP_atomics_inline_xor,
    MIR_OP_atomics_inline_exchange,
    MIR_OP_atomics_inline_compare_exchange,

    /* Class optimization (m7): method call cache */
    MIR_OP_method_cache,        /* src1=obj_var, dst=cache_var; imm=name_idx, ptr=prog */
    MIR_OP_call_cached_method,  /* src1=cache_var, dst=out; imm=(argc<<16), args_base=src1+5 */
} MIROpcode;

typedef struct MIRInstr {
    MIROpcode opcode;
    int dst;        /* destination MIR variable (-1 if none) */
    int src1;       /* first source MIR variable */
    int src2;       /* second source MIR variable */
    int64_t imm;    /* immediate value */
    void *ptr;      /* pointer operand (for calls) */
} MIRInstr;

typedef struct MIRProgram {
    MIRInstr  *instructions;
    int        num_instructions;
    int        capacity;
    int        num_vars;
    int       *const_values;
    double    *const_f64_values;  /* f64 constants (NaN = unknown) */
    int       *bc_offsets;    /* bytecode offset for each MIR instruction */
    int        is_partial;    /* 1 if compilation bailed out early (incomplete) */
} MIRProgram;

/* ══════════════════════════════════════════════════════════════════════
 * Frontend API (bytecode -> MIR)
 * ══════════════════════════════════════════════════════════════════════ */

MIRProgram *mir_create(int max_vars);
void mir_free(MIRProgram *mir);

int mir_add_instr(MIRProgram *mir, MIROpcode op, int dst, int src1, int src2,
                  int64_t imm, void *ptr);
int mir_new_var(MIRProgram *mir);

/* Compile bytecode to MIR program. Returns NULL on bail */
MIRProgram *mir_compile_bytecode(Interpreter *interp, LRProgram *prog);
void mir_analyze_liveness(MIRProgram *mir);
void mir_dump(MIRProgram *mir, const char *label);

/* Serialize a MIR program to a portable, little-endian byte buffer (for the
 * IOME586 cross-platform cache).  The MIRInstr.ptr field is write-only in
 * codegen (never read), so it is NOT serialized; codegen_emit receives the
 * live BCProgram separately.  Returns a malloc'd buffer (caller frees) and
 * sets *out_len; NULL on error.  `prog` (BCProgram*) supplies nparams/prog_id
 * but is not embedded as a pointer. */
uint8_t *mir_serialize(MIRProgram *mir, void *prog, size_t *out_len);

/* Deserialize a MIR program produced by mir_serialize.  Returns a new
 * MIRProgram (caller frees with mir_free) or NULL on error.  Sets
 * *out_nparams (0 when unavailable).  All reconstructed instructions have
 * ptr=NULL and capacity equal to num_instructions, ready for codegen_emit. */
MIRProgram *mir_deserialize(const uint8_t *data, size_t len, int *out_nparams);

/* ══════════════════════════════════════════════════════════════════════
 * Backend API (MIR -> native code via SLJIT)
 * ══════════════════════════════════════════════════════════════════════ */

/* Emit native code from MIR. Returns executable code buffer, sets out_size.
 * nparams is the number of function parameters (for arg prologue).
 * prog is the BCProgram pointer (embedded in runtime callbacks). */
uint8_t *codegen_emit(MIRProgram *mir, size_t *out_size, int nparams, void *prog);

/* ══════════════════════════════════════════════════════════════════════
 * Runtime API (code cache management + compilation driver)
 * ══════════════════════════════════════════════════════════════════════ */

/* ── JIT code block (one per hot function) ───────────────────────────── */
typedef struct LRJITCode LRJITCode;

struct LRJITCode {
    uint8_t     *code;
    size_t       code_size;
    size_t       code_capacity;
    LRProgram   *prog;
    int          hot_counter;
    int          compiled;
    int          bailout_occurred;
    int          loop_compiled;
    int          recompile_requested;
    LRJITCode   *next;
};

/* ── Runtime JIT state ───────────────────────────────────────────────── */
typedef struct LRJITRuntime LRJITRuntime;

struct LRJITRuntime {
    LRJITCode   *code_head;
    int          enabled;
    size_t       total_code_size;
    int          compile_count;
    int          bailout_count;
    int          exec_count;
    /* MIR cache: accumulated serialized MIR for all JIT-compiled functions.
     * Format: [num_functions(u32)][len0(u32)+data0][len1(u32)+data1]... */
    uint8_t     *mir_ser;
    size_t       mir_ser_len;
    /* Hotspot analysis state */
    int          hotspot_active;      /* 1 if hotspot analysis is enabled */
    int          hotspot_compile_queue_size; /* number of programs pending compilation */
};

/* Initialize JIT runtime state. */
void lr_jit_init(LRJITRuntime *jit);

/* Enable/disable JIT. */
void lr_jit_set_enabled(LRJITRuntime *jit, int enabled);

/* Notify the JIT of a function call (hotness counter).
 * Returns hotness count; >0 means eligible for compilation. */
int lr_jit_notify_call(Interpreter *interp, LRProgram *prog);

/* Compile a function's bytecode to native code. Returns entry point or NULL. */
LRJITEntry lr_jit_compile(Interpreter *interp, LRProgram *prog);

/* Look up a compiled function by program pointer. */
LRJITEntry lr_jit_lookup(LRJITRuntime *jit, LRProgram *prog);

/* Look up the LRJITCode block for a program (used for hot-loop ticks). */
LRJITCode *lr_jit_lookup_code(LRJITRuntime *jit, LRProgram *prog);

/* Mark a JIT-compiled program as having bailed out. */
void lr_jit_mark_bailout(LRJITRuntime *jit, LRProgram *prog);

/* ── Runtime helpers (called from JIT-generated native code) ────────── */
void lr_jit_rt_load_var(Interpreter *interp, void *prog,
                        uint32_t name_idx, LRValue *out);
void lr_jit_rt_call(Interpreter *interp,
                    uint32_t argc, LRValue *args_base, LRValue *out);
/* Direct JIT-to-JIT call: resolves callee at runtime, dispatches to native
 * entry when possible, falls back to interp_bc_call.
 * Used by MIR_OP_inline_call for known-function callees.
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
 *   R3(CX)=interp, R1(DX)=argc, R2(R8)=args_base, TMP_REG1(R9)=out */
void lr_jit_rt_inline_call(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out);

/* Extended runtime helpers (called by the new core MIR instructions). */
void lr_jit_rt_load_const_f64(Interpreter *interp, void *prog,
                              uint32_t pool_idx, LRValue *out);
void lr_jit_rt_load_const_string(Interpreter *interp, void *prog,
                                 uint32_t pool_idx, LRValue *out);
void lr_jit_rt_load_this(Interpreter *interp, LRValue *out);
void lr_jit_rt_store_var(Interpreter *interp, void *prog,
                         uint32_t name_idx, const LRValue *val);
void lr_jit_rt_inc_var(Interpreter *interp, void *prog,
                       uint32_t name_idx, LRValue *out);
void lr_jit_rt_typeof(Interpreter *interp, const LRValue *val, LRValue *out);
void lr_jit_rt_typeof_var(Interpreter *interp, void *prog,
                          uint32_t name_idx, LRValue *out);
void lr_jit_rt_to_string(Interpreter *interp, const LRValue *val, LRValue *out);
void lr_jit_rt_string_concat(Interpreter *interp, const LRValue *a,
                             const LRValue *b, LRValue *out);
void lr_jit_rt_to_number(Interpreter *interp, const LRValue *val, LRValue *out);
void lr_jit_rt_to_bool(Interpreter *interp, const LRValue *val, LRValue *out);
void lr_jit_rt_pos(Interpreter *interp, const LRValue *val, LRValue *out);
void lr_jit_rt_pow(Interpreter *interp,
                   const LRValue *base, const LRValue *exp, LRValue *out);
void lr_jit_rt_new_object(Interpreter *interp, LRValue *out);
void lr_jit_rt_new_array(Interpreter *interp,
                         uint32_t n, const LRValue *elems, LRValue *out);
void lr_jit_rt_get_prop(Interpreter *interp, void *prog,
                        uint32_t name_idx, LRValue *args_base);
/* args_base[0] = obj (input), args_base[1] = result (output) */
void lr_jit_rt_set_prop(Interpreter *interp, void *prog,
                        uint32_t name_idx, LRValue *args_base);
/* args_base[0] = obj, args_base[1] = val (inputs), args_base[2] = result (output) */
void lr_jit_rt_get_elem(Interpreter *interp,
                        const LRValue *obj, const LRValue *key, LRValue *out);
void lr_jit_rt_set_elem(Interpreter *interp, const LRValue *args_base,
                        LRValue *out);  /* args_base: [obj, key, val] */
void lr_jit_rt_in(Interpreter *interp,
                  const LRValue *obj, const LRValue *key, LRValue *out);
void lr_jit_rt_instanceof(Interpreter *interp,
                          const LRValue *obj, const LRValue *ctor, LRValue *out);
/* Cached property access: checks shape + flat slot directly in JIT.
 * Used by MIR_OP_get_prop_cached.  args_base[0]=obj (input), args_base[1]=result. */
void lr_jit_rt_get_prop_cached(Interpreter *interp, void *prog,
                               uint32_t name_idx, LRValue *args_base);
/* Fused property-add: result = obj[name] + rhs. Used by MIR_OP_add_prop miss path.
 * args_base[0]=obj, args_base[1]=rhs, args_base[2]=result. */
void lr_jit_rt_add_prop(Interpreter *interp, void *prog,
                        uint32_t name_idx, LRValue *args_base);
void lr_jit_rt_throw(Interpreter *interp, const LRValue *val);  /* noreturn */
void lr_jit_rt_call_method(Interpreter *interp, void *prog,
                           uint32_t packed, LRValue *args_base);
/* packed = (name_idx << 16) | argc.
 * args_base[0..argc] = [this, arg1..argN] (inputs),
 * args_base[argc+1]   = result (output). */
void lr_jit_rt_call_elem(Interpreter *interp,
                         uint32_t argc, LRValue *args_base, LRValue *out);
void lr_jit_rt_construct(Interpreter *interp,
                         uint32_t argc, LRValue *args_base, LRValue *out);

/* fmod(a, b) — called by MIR_OP_mod_f64 codegen.
 * Args: interp, a (LRValue*), b (LRValue*), out (LRValue*). */
void lr_jit_rt_fmod(Interpreter *interp, const LRValue *a, const LRValue *b, LRValue *out);

/* Atomics.load(typedArray, index) — called by MIR_OP_atomics_load.
 * args_base[0]=typedArray, args_base[1]=index, out=args_base[2]. */
void lr_jit_rt_atomics_load(Interpreter *interp, uint32_t argc,
                            LRValue *args_base, LRValue *out);

/* Atomics.store(typedArray, index, value) — called by MIR_OP_atomics_store.
 * args_base[0]=typedArray, args_base[1]=index, args_base[2]=value, out=args_base[3]. */
void lr_jit_rt_atomics_store(Interpreter *interp, uint32_t argc,
                             LRValue *args_base, LRValue *out);

/* Atomics.add(typedArray, index, value) — called by MIR_OP_atomics_add.
 * args_base[0]=typedArray, args_base[1]=index, args_base[2]=value, out=args_base[3]. */
void lr_jit_rt_atomics_add(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out);

/* Atomics.sub(typedArray, index, value) — called by MIR_OP_atomics_sub. */
void lr_jit_rt_atomics_sub(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out);

/* Atomics.and(typedArray, index, value) — called by MIR_OP_atomics_and. */
void lr_jit_rt_atomics_and(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out);

/* Atomics.or(typedArray, index, value) — called by MIR_OP_atomics_or. */
void lr_jit_rt_atomics_or(Interpreter *interp, uint32_t argc,
                          LRValue *args_base, LRValue *out);

/* Atomics.xor(typedArray, index, value) — called by MIR_OP_atomics_xor. */
void lr_jit_rt_atomics_xor(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out);

/* Atomics.exchange(typedArray, index, value) — called by MIR_OP_atomics_exchange. */
void lr_jit_rt_atomics_exchange(Interpreter *interp, uint32_t argc,
                                LRValue *args_base, LRValue *out);

/* Atomics.compareExchange(typedArray, index, expected, replacement)
 * — called by MIR_OP_atomics_compare_exchange. */
void lr_jit_rt_atomics_compare_exchange(Interpreter *interp, uint32_t argc,
                                        LRValue *args_base, LRValue *out);

/* Atomics.isLockFree(size) — called by MIR_OP_atomics_is_lock_free.
 * args_base[0]=size, out=args_base[1]. */
void lr_jit_rt_atomics_is_lock_free(Interpreter *interp, uint32_t argc,
                                    LRValue *args_base, LRValue *out);

/* Atomics cache: extracts TypedArray info into 5 LRValue slots at dst_ptr.
 * Called by MIR_OP_atomics_cache. */
void lr_jit_rt_atomics_cache(Interpreter *interp, void *ta_ptr, void *dst_ptr);

/* Atomics inline variants: use pre-cached TypedArray info (4-arg ABI).
 * Called by MIR_OP_atomics_inline_*.
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG):
 *   R3(CX)=interp, R1(DX)=cache_ptr (points to 5-LRValue cache at src1),
 *   R2(R8)=args_base_ptr (points to src1's LRValue array),
 *   TMP_REG1(R9)=out_ptr (points to dst).
 */
void lr_jit_rt_atomics_inline_load(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_store(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_add(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_sub(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_and(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_or(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_xor(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_exchange(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);
void lr_jit_rt_atomics_inline_compare_exchange(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);

/* Class optimization (m7): method cache runtime helpers.
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
 *   R3(CX)=interp, R1(DX)=src1_base (LRValue array),
 *   R2(R8)=dst_base (LRValue cache output), TMP_REG1(R9)=method_name */
void lr_jit_rt_method_cache(Interpreter *interp, const LRValue *src1_base,
    LRValue *dst_base, const char *method_name);
void lr_jit_rt_call_cached_method(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out);

/* Private field optimization (m8): runtime helpers.
 * ABI (same as get_prop/set_prop):
 *   R3(CX)=interp, R1(DX)=prog_raw, R2(R8)=name_idx, TMP_REG1(R9)=base* */
void lr_jit_rt_private_field_get(Interpreter *interp, void *prog_raw,
    uint32_t name_idx, LRValue *args_base);
void lr_jit_rt_private_field_set(Interpreter *interp, void *prog_raw,
    uint32_t name_idx, LRValue *args_base);

/* Free all JIT code and reset runtime state. */
void lr_jit_destroy(LRJITRuntime *jit);

/* Force immediate compilation regardless of hotness threshold. */
void lr_jit_force_compile(LRJITRuntime *jit, LRProgram *prog);

/* Warm-path: pre-JIT-compile all bodies from serialized MIR cache.
 * Returns number of functions compiled.  Bodies without jit_entry get
 * code emitted directly from cached MIR, skipping the bytecode→MIR frontend. */
int lr_jit_precompile_from_mir_cache(Interpreter *interp,
                                      const uint8_t *mir_data, size_t mir_len,
                                      int *out_compiled_count);

/* Get the persistent Interpreter pointer from a LRContext (for warm-path
 * JIT precompilation from MIR cache). Returns NULL if not available. */
Interpreter *lr_engine_get_interp(LRContext *ctx);

/* ── Hot loop detection (V8-style counter-based triggering) ──────────── */
/* Notify JIT of a loop iteration. Returns 1 when hot enough to compile. */
int lr_jit_notify_loop(Interpreter *interp, LRProgram *prog);

/* Runtime helper called by MIR_OP_loop_guard from JIT code.
 * Returns 1 if compilation was triggered (caller should bail out). */
int lr_jit_rt_notify_loop(Interpreter *interp, LRProgram *prog);

/* Compile a function specifically for hot-loop optimization.
 * Uses type specialization and loop unrolling for simple patterns. */
LRJITEntry lr_jit_compile_hot_loop(Interpreter *interp, LRProgram *prog);

/* ── Hotspot Analysis API ─────────────────────────────────────────────── */
/* Initialize hotspot analysis system. Call once after lr_jit_init(). */
void lr_jit_hotspot_init(LRJITRuntime *jit);

/* Enable/disable hotspot analysis (default: enabled). */
void lr_jit_hotspot_set_enabled(LRJITRuntime *jit, int enabled);

/* Check if a function is a hotspot based on runtime profiling.
 * Returns 1 if the function should be compiled immediately. */
int lr_jit_is_hotspot_func(Interpreter *interp, LRProgram *prog);

/* Check if a loop is a hotspot based on iteration count.
 * Returns 1 if the loop should be compiled immediately. */
int lr_jit_is_hotspot_loop(Interpreter *interp, LRProgram *prog);

/* Get hotspot statistics for debugging/benchmarking. */
void lr_jit_hotspot_get_stats(LRJITRuntime *jit, int *func_hotspots,
                               int *loop_hotspots, int *total_compiles);

/* ── Multi-Process Compilation (分身) API ─────────────────────────────── */
/* Compilation task for worker process */
typedef struct LR_JITCompileTask {
    Interpreter *interp;         /* Original interpreter (for context) */
    LRProgram   *prog;           /* Program to compile */
    LRJITEntry   result_entry;   /* Compiled entry point (output) */
    int          is_hot_loop;    /* 1 if hot-loop compilation */
    int          task_status;    /* 0=pending, 1=running, 2=done, -1=error */
} LR_JITCompileTask;

/* Forward declaration for thread pool */
typedef struct LR_ThreadPool LR_ThreadPool;

/* Initialize multi-process compilation system with N worker processes */
void lr_jit_worker_init(int num_workers);

/* Shutdown multi-process compilation system */
void lr_jit_worker_shutdown(void);

/* Submit a JIT compilation task to worker pool
 * Returns 0 on success, -1 on failure */
int lr_jit_submit_compile_task(LR_JITCompileTask *task);

/* Wait for all submitted compilation tasks to complete */
void lr_jit_wait_for_compilations(void);

/* Get worker pool stats */
void lr_jit_worker_get_stats(int *pending_tasks, int *completed_tasks);

#ifdef __cplusplus
}
#endif

#endif /* LR_JIT_H */
