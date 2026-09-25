/*
 * LR_JS - JIT Runtime Implementation
 *
 * Architecture: Bytecode -> MIR (frontend) -> SLJIT (backend) -> native code
 */

#include "lr_jit.h"
#include "mir/lr_jit_mir.h"
#include "lr_engine.h"
#include "lr_interp.h"
#include "lr_thread_pool.h"
#include <sljitLir.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Multi-process compilation (分身) support */
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#endif

/* Global runtime debug switch (enabled by --debug / LR_DEBUG env).  Defined
 * here so the JIT runtime and every translation unit including lr_jit.h share
 * the same flag. */
int g_lr_debug = 0;

/* Mutex to serialize SLJIT codegen across threads.
 * SLJIT's default allocator (sljitExecAllocatorCore.c) uses a non-thread-safe
 * global free-list, so concurrent JIT compilation from multiple threads races
 * on that linked list and causes crashes. This mutex serializes all
 * codegen_emit calls. */
#ifdef _WIN32
static CRITICAL_SECTION g_jit_codegen_cs;
static int g_jit_codegen_cs_init = 0;
#define LR_JIT_CODEGEN_LOCK() do { \
    if (!g_jit_codegen_cs_init) { InitializeCriticalSection(&g_jit_codegen_cs); g_jit_codegen_cs_init = 1; } \
    EnterCriticalSection(&g_jit_codegen_cs); \
} while (0)
#define LR_JIT_CODEGEN_UNLOCK() LeaveCriticalSection(&g_jit_codegen_cs)
#else
static pthread_mutex_t g_jit_codegen_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LR_JIT_CODEGEN_LOCK() pthread_mutex_lock(&g_jit_codegen_mutex)
#define LR_JIT_CODEGEN_UNLOCK() pthread_mutex_unlock(&g_jit_codegen_mutex)
#endif

/* Runtime debug logging: aliases the unified LR_JIT_DBG macro so the
 * JIT/MIR/CGEN/RT chain is controlled by a single switch (LR_DEBUG_JIT). */

/* ── Runtime helper functions (called by JIT-generated code) ────────────
 * These are invoked via sljit_emit_icall from MIR_OP_load_var and
 * MIR_OP_runtime_call.  They bridge JIT native code back to the
 * interpreter for operations that need runtime scope/call resolution. */

/* Resolve a named variable via the interpreter's scope chain.
 * Called by MIR_OP_load_var codegen.
 *   arg0 = interp (Interpreter *)
 *   arg1 = prog (BCProgram *)
 *   arg2 = name_idx (uint16_t pool index)
 *   arg3 = out   (LRValue * — 16-byte output slot in JIT spill area) */
void lr_jit_rt_load_var(Interpreter *interp, void *prog_raw,
                        uint32_t name_idx, LRValue *out) {
    BCProgram *prog = (BCProgram *)prog_raw;
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count) {
        out->tag = LR_TYPE_UNDEFINED;
        out->u.ptr = NULL;
        return;
    }
    if (!interp_bc_load_var(interp, prog->pool[name_idx].u.str, out)) {
        out->tag = LR_TYPE_UNDEFINED;
        out->u.ptr = NULL;
    }
}

/* Perform a function call.
 * Called by MIR_OP_runtime_call codegen.
 *   arg0 = interp (Interpreter *)
 *   arg1 = argc (uint16_t)
 *   arg2 = args_base (LRValue * — points to callee + args in JIT spill area)
 *   arg3 = out   (LRValue * — 16-byte output slot in JIT spill area)
 *
 * Layout at args_base:
 *   args_base[0]      = callee (LRValue)
 *   args_base[1..argc] = argv  (LRValue each)
 */
void lr_jit_rt_call(Interpreter *interp,
                    uint32_t argc, LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc == 0) {
        out->tag = LR_TYPE_UNDEFINED;
        out->u.ptr = NULL;
        return;
    }
    LRValue callee = args_base[0];
    LRValue *argv = &args_base[1];
    LRValue this_val = LR_VALUE_UNDEFINED;
    *out = interp_bc_call(interp, callee, this_val, (int)argc, argv);
}

/* Direct JIT-to-JIT call: resolves callee at runtime, dispatches to native
 * entry when the callee is a known JS function with a compiled entry point.
 * Falls back to interp_bc_call for non-function values or missing jit_entry.
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
 *   R3(CX)=interp, R1(DX)=argc, R2(R8)=args_base, TMP_REG1(R9)=out */
void lr_jit_rt_inline_call(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc == 0) {
        out->tag = LR_TYPE_UNDEFINED;
        out->u.ptr = NULL;
        return;
    }
    LRValue callee = args_base[0];
    LRValue *argv = &args_base[1];
    /* Try direct JIT dispatch for function objects with a compiled entry */
    if (callee.tag == LR_TYPE_OBJECT && callee.u.ptr) {
        LRObject *obj = (LRObject *)callee.u.ptr;
        if (obj->type == LR_OBJ_FUNCTION && obj->extra) {
            ASTNode *func_ast = (ASTNode *)obj->extra;
            BCProgram *body_prog = bc_get_or_compile_func(func_ast);
            if (body_prog) {
                LRRuntime *rt = (LRRuntime *)interp->ctx->rt;
                if (rt && rt->jit_runtime && body_prog->jit_entry && !body_prog->jit_skip) {
                    LRJITEntry jit_entry = (LRJITEntry)body_prog->jit_entry;
                    /* Set jit_args_base so the JIT prologue reads args from
                     * the pre-staged spill area instead of the shared def_scope.
                     * This avoids scope corruption on recursive calls. */
                    void *saved_args_base = interp->jit_args_base;
                    interp->jit_args_base = args_base;
                    void *saved_jit_code_ptr = interp->jit_code_ptr;
                    interp->jit_code_ptr = lr_jit_lookup_code(
                        (LRJITRuntime *)rt->jit_runtime, body_prog);
                    LRValue result;
                    jit_entry(interp, body_prog->code, &result);
                    interp->jit_code_ptr = saved_jit_code_ptr;
                    interp->jit_args_base = saved_args_base;
                    if (result.tag != -1) {
                        *out = result;
                        return;
                    }
                    /* Bailout: fall through to interpreter */
                    lr_jit_mark_bailout((LRJITRuntime *)rt->jit_runtime, body_prog);
                    body_prog->jit_entry = NULL;
                }
            }
        }
    }
    /* Fallback to interpreter */
    LRValue this_val = LR_VALUE_UNDEFINED;
    *out = interp_bc_call(interp, callee, this_val, (int)argc, argv);
}

/* ── Extended runtime helpers ────────────────────────────────────────────
 * Each bridges a single bytecode-level semantics back to the interpreter /
 * engine, letting the JIT keep emitting native code for surrounding
 * arithmetic and control flow.  All functions assume the JIT spill area
 * pointers are aligned to LRValue (16 bytes) and write a complete 16-byte
 * LRValue to *out.  Failures default to LR_VALUE_UNDEFINED. */

/* Pull the LRContext out of the interpreter the same way the existing
 * helpers do — keeps each helper self-contained. */
static LRContext *jit_interp_ctx(Interpreter *interp) {
    return (interp && interp->ctx) ? interp->ctx : NULL;
}

void lr_jit_rt_load_const_f64(Interpreter *interp, void *prog_raw,
                              uint32_t pool_idx, LRValue *out) {
    BCProgram *prog = (BCProgram *)prog_raw;
    if (!interp || !prog || pool_idx >= (uint32_t)prog->pool_count) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    /* Build LRValue from BCConst.  Float64 is a primitive (no refcount). */
    BCConst *c = &prog->pool[pool_idx];
    if (c->kind != BC_POOL_FLOAT64) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    out->tag = LR_TYPE_FLOAT64;
    out->u.float64 = c->u.f64;
}

void lr_jit_rt_load_const_string(Interpreter *interp, void *prog_raw,
                                 uint32_t pool_idx, LRValue *out) {
    BCProgram *prog = (BCProgram *)prog_raw;
    if (!interp || !prog || pool_idx >= (uint32_t)prog->pool_count) {
        LR_JIT_DBG("[JIT-RT] load_const_string FAIL: interp=%p prog=%p pool_idx=%u prog->pool_count=%d\n",
                (void*)interp, (void*)prog, pool_idx, prog ? prog->pool_count : -1);
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) {
        LR_JIT_DBG("[JIT-RT] load_const_string FAIL: no ctx\n");
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    BCConst *c = &prog->pool[pool_idx];
    LR_JIT_DBG("[JIT-RT] load_const_string: pool_idx=%u kind=%d str=%p len=%u\n",
            pool_idx, c->kind, (void*)c->u.str,
            c->kind == BC_POOL_STRING ? (int)strlen(c->u.str) : -1);
    if (c->kind != BC_POOL_STRING || !c->u.str) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    /* lr_new_string allocates a fresh LRString with ref_count=1; the spill
     * slot will FREE_IF_HEAP it on overwrite/return. */
    *out = lr_new_string(ctx, c->u.str);
    LR_JIT_DBG("[JIT-RT] load_const_string OK: tag=%d ptr=%p\n", out->tag, (void*)out->u.ptr);
}

void lr_jit_rt_load_this(Interpreter *interp, LRValue *out) {
    if (!interp || !out) {
        if (out) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; }
        return;
    }
    interp_bc_push_this(interp, out);
}

void lr_jit_rt_store_var(Interpreter *interp, void *prog_raw,
                         uint32_t name_idx, const LRValue *val) {
    BCProgram *prog = (BCProgram *)prog_raw;
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count || !val)
        return;
    const char *name = prog->pool[name_idx].u.str;
    if (!name) return;
    interp_bc_store_var(interp, name, *val);
}

void lr_jit_rt_inc_var(Interpreter *interp, void *prog_raw,
                       uint32_t name_idx, LRValue *out) {
    BCProgram *prog = (BCProgram *)prog_raw;
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count || !out) {
        if (out) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; }
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return; }
    /* Post-increment semantics: read old value, then write old+1. */
    LRValue cur;
    if (!interp_bc_load_var(interp, name, &cur)) {
        cur = LR_VALUE_UNDEFINED;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    LRValue next;
    if (cur.tag == LR_TYPE_INT32) {
        next.tag = LR_TYPE_INT32;
        next.u.int32 = cur.u.int32 + 1;
    } else if (cur.tag == LR_TYPE_FLOAT64) {
        next.tag = LR_TYPE_FLOAT64;
        next.u.float64 = cur.u.float64 + 1.0;
    } else if (ctx) {
        double d = 0.0;
        lr_to_float64(ctx, &d, cur);
        next.tag = LR_TYPE_FLOAT64;
        next.u.float64 = d + 1.0;
    } else {
        next = LR_VALUE_UNDEFINED;
    }
    interp_bc_store_var(interp, name, next);
    *out = cur;
}

void lr_jit_rt_typeof(Interpreter *interp, const LRValue *val, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !val || !ctx) {
        if (out) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; }
        return;
    }
    const char *name = "undefined";
    switch (val->tag) {
        case LR_TYPE_UNDEFINED: name = "undefined"; break;
        case LR_TYPE_NULL:       name = "object";  break;  /* historical JS */
        case LR_TYPE_BOOL:       name = "boolean"; break;
        case LR_TYPE_INT32:
        case LR_TYPE_FLOAT64:    name = "number";  break;
        case LR_TYPE_STRING:     name = "string";  break;
        case LR_TYPE_OBJECT: {
            /* Functions report "function"; everything else "object". */
            LRObject *obj = (LRObject *)val->u.ptr;
            name = (obj && lr_is_function(ctx, *val)) ? "function" : "object";
            break;
        }
        case LR_TYPE_SYMBOL:     name = "symbol";  break;
        default:                 name = "undefined"; break;
    }
    *out = lr_new_string(ctx, name);
}

void lr_jit_rt_typeof_var(Interpreter *interp, void *prog_raw,
                          uint32_t name_idx, LRValue *out) {
    BCProgram *prog = (BCProgram *)prog_raw;
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count || !out) {
        if (out) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; }
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return; }
    interp_bc_typeof_var(interp, name, out);
}

void lr_jit_rt_to_string(Interpreter *interp, const LRValue *val, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !val || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    const char *s = lr_to_cstring(ctx, *val);
    *out = lr_new_string(ctx, s);
}

void lr_jit_rt_string_concat(Interpreter *interp, const LRValue *a,
                             const LRValue *b, LRValue *out) {
    if (!out || !a || !b) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return; }
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    if (!rt) { out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return; }

    LRValue sa = JS_ToString(ctx, *a);
    LRValue sb = JS_ToString(ctx, *b);

    LRString *as = (LRString *)sa.u.ptr;
    LRString *bs = (LRString *)sb.u.ptr;
    size_t la = as ? as->len : 0;
    size_t lb = bs ? bs->len : 0;
    size_t total = la + lb;

    LRString *os = lr_string_alloc(rt, NULL, total);
    if (!os) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL;
        return;
    }
    if (la) memcpy(os->str, as->str, la);
    if (lb) memcpy(os->str + la, bs->str, lb);
    os->str[total] = '\0';
    out->tag = LR_TYPE_STRING; out->u.ptr = os;
}

void lr_jit_rt_to_number(Interpreter *interp, const LRValue *val, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !val || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    double d = 0.0;
    lr_to_float64(ctx, &d, *val);
    if (val->tag == LR_TYPE_INT32) {
        /* Preserve INT32 when the operand is already an integer.  A chain
         * like `var p = +x` followed by `add_i32` reads the value field as
         * int32; emitting FLOAT64 here makes those int32 ops misinterpret
         * the IEEE-754 bit pattern and silently drop the operand. */
        *out = *val;
    } else if (d >= (double)INT32_MIN && d <= (double)INT32_MAX &&
               d == (double)(int32_t)d) {
        /* Whole-integer result (e.g. Number("42")): emit INT32 so downstream
         * add_i32/compare ops read the value field correctly instead of
         * interpreting the low 32 bits of the FLOAT64 bit pattern. */
        out->tag = LR_TYPE_INT32;
        out->u.int32 = (int32_t)d;
    } else {
        *out = lr_new_float64(ctx, d);
    }
}

void lr_jit_rt_to_bool(Interpreter *interp, const LRValue *val, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !val || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    int b = lr_to_bool(ctx, *val);
    out->tag = LR_TYPE_BOOL;
    /* Write a clean 32-bit 0/1 at offset+8. The JIT's jump_if_false reads the
     * bool as a full 32-bit word; writing only u.bool_val (1 byte) leaves the
     * upper bytes of the union stale, so a false value could read back as true. */
    out->u.int32 = (b ? 1 : 0);
}

void lr_jit_rt_pos(Interpreter *interp, const LRValue *val, LRValue *out) {
    /* Unary + is Number(x). */
    lr_jit_rt_to_number(interp, val, out);
}

void lr_jit_rt_pow(Interpreter *interp,
                   const LRValue *base, const LRValue *exp, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !base || !exp || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    double b = 0.0, e = 0.0;
    lr_to_float64(ctx, &b, *base);
    lr_to_float64(ctx, &e, *exp);
    *out = lr_new_float64(ctx, pow(b, e));
}

void lr_jit_rt_new_object(Interpreter *interp, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    *out = lr_new_object(ctx);
}

void lr_jit_rt_new_array(Interpreter *interp,
                         uint32_t n, const LRValue *elems, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    LRValue arr = lr_new_array(ctx);
    for (uint32_t i = 0; i < n; i++) {
        if (elems) lr_set_property_uint32(ctx, arr, i, elems[i]);
    }
    lr_set_property_str(ctx, arr, "length", lr_new_int32(ctx, (int32_t)n));
    *out = arr;
}

void lr_jit_rt_get_prop(Interpreter *interp, void *prog_raw,
                        uint32_t name_idx, LRValue *args_base) {
    /* args_base[0] = obj (input), args_base[1] = result (output). */
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count || !args_base || !ctx) {
        if (args_base) args_base[1] = LR_VALUE_UNDEFINED;
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { args_base[1] = LR_VALUE_UNDEFINED; return; }
    LRValue obj = args_base[0];
    args_base[1] = lr_get_property_str(ctx, obj, name);
}

/* Shape-cached property access: inline checks obj->type / obj->shape /
 * SHAPE_FLAT_SIZE scan, then reads the slot directly from obj->props[].
 * Falls back to lr_jit_rt_get_prop on miss so JIT code can retry with
 * the generic path.
 *   args_base[0] = obj (input), args_base[1] = result (output). */
void lr_jit_rt_get_prop_cached(Interpreter *interp, void *prog_raw,
                               uint32_t name_idx, LRValue *args_base) {
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count
        || !args_base || !ctx) {
        if (args_base) args_base[1] = LR_VALUE_UNDEFINED;
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { args_base[1] = LR_VALUE_UNDEFINED; return; }

    LRValue obj = args_base[0];
    if (obj.tag != LR_TYPE_OBJECT || !obj.u.ptr) {
        args_base[1] = LR_VALUE_UNDEFINED;
        return;
    }
    LRObject *o = (LRObject *)obj.u.ptr;
    if (o->is_exotic || !o->shape) {
        /* Delegates to the full runtime path for exotic objects and objects
         * without a plain hidden-class shape. */
        args_base[1] = lr_get_property_str(ctx, obj, name);
        return;
    }

    /* Inline the flat-hash lookup from shape_get_slot_fast so the JIT can
     * avoid a function call on a cache hit. */
    LRShape *s = o->shape;
    LRString *atom = lr_new_atom(ctx, name);
    int slot = -1;
    if (s->flat_count != 0) {
        unsigned h = ((((uintptr_t)atom) >> 4) ^ (((uintptr_t)atom) >> 10)) & SHAPE_FLAT_MASK;
        for (unsigned i = 0; i < SHAPE_FLAT_SIZE; i++) {
            unsigned idx = (h + i) & SHAPE_FLAT_MASK;
            LRString *k = s->flat_keys[idx];
            if (k == atom) { slot = (int)s->flat_slots[idx]; break; }
            if (!k) break;
        }
    } else {
        while (s) {
            if (s->prop_name == atom) { slot = (int)s->slot_index; break; }
            s = s->prev;
        }
    }
    lr_string_release(ctx->rt, atom);

    if (slot >= 0 && (uint32_t)slot < o->prop_count) {
        args_base[1] = o->props[slot];
    } else {
        /* Cache miss — fall back to generic property lookup. */
        args_base[1] = lr_get_property_str(ctx, obj, name);
    }
}

/* Fused property-add runtime helper for MIR_OP_add_prop miss path.
 * Performs: result = obj[name] + rhs with full JS semantics (int32, float64,
 * string concat, coercion).  args_base[0]=obj, args_base[1]=rhs,
 * args_base[2]=result. */
void lr_jit_rt_add_prop(Interpreter *interp, void *prog_raw,
                        uint32_t name_idx, LRValue *args_base) {
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count
        || !args_base || !ctx) {
        if (args_base) args_base[2] = LR_VALUE_UNDEFINED;
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { args_base[2] = LR_VALUE_UNDEFINED; return; }

    LRValue obj = args_base[0];
    LRValue rhs = args_base[1];
    LRValue prop_val;
    if (obj.tag == LR_TYPE_OBJECT && obj.u.ptr) {
        LRObject *o = (LRObject *)obj.u.ptr;
        if (o->props && !o->is_exotic && o->type != LR_OBJ_PROXY && o->shape) {
            LRString *atom = lr_new_atom(ctx, name);
            int slot = -1;
            if (o->shape->flat_count != 0) {
                unsigned h = ((((uintptr_t)atom) >> 4) ^ (((uintptr_t)atom) >> 10)) & SHAPE_FLAT_MASK;
                for (unsigned i = 0; i < SHAPE_FLAT_SIZE; i++) {
                    unsigned idx = (h + i) & SHAPE_FLAT_MASK;
                    LRString *k = o->shape->flat_keys[idx];
                    if (k == atom) { slot = (int)o->shape->flat_slots[idx]; break; }
                    if (!k) break;
                }
            }
            lr_string_release(ctx->rt, atom);
            if (slot >= 0 && (uint32_t)slot < o->prop_count) {
                prop_val = o->props[slot];
            } else {
                prop_val = lr_get_property_str(ctx, obj, name);
            }
        } else {
            prop_val = lr_get_property_str(ctx, obj, name);
        }
    } else {
        prop_val = lr_get_property_str(ctx, obj, name);
    }

    /* int32 + int32 fast path */
    if (prop_val.tag == LR_TYPE_INT32 && rhs.tag == LR_TYPE_INT32) {
        int64_t sum = (int64_t)prop_val.u.int32 + (int64_t)rhs.u.int32;
        if (sum >= INT32_MIN && sum <= INT32_MAX) {
            args_base[2].tag = LR_TYPE_INT32;
            args_base[2].u.int32 = (int32_t)sum;
        } else {
            args_base[2].tag = LR_TYPE_FLOAT64;
            args_base[2].u.float64 = (double)sum;
        }
        return;
    }
    /* float64 + float64 */
    if (prop_val.tag == LR_TYPE_FLOAT64 && rhs.tag == LR_TYPE_FLOAT64) {
        args_base[2] = prop_val;
        args_base[2].u.float64 += rhs.u.float64;
        return;
    }
    /* mixed int32/float64 */
    if (prop_val.tag == LR_TYPE_INT32 && rhs.tag == LR_TYPE_FLOAT64) {
        args_base[2].tag = LR_TYPE_FLOAT64;
        args_base[2].u.float64 = (double)prop_val.u.int32 + rhs.u.float64;
        return;
    }
    if (prop_val.tag == LR_TYPE_FLOAT64 && rhs.tag == LR_TYPE_INT32) {
        args_base[2] = prop_val;
        args_base[2].u.float64 += (double)rhs.u.int32;
        return;
    }
    /* String concatenation or numeric fallback */
    if (prop_val.tag == LR_TYPE_STRING || rhs.tag == LR_TYPE_STRING) {
        LRValue sa, sb;
        lr_jit_rt_to_string(interp, &prop_val, &sa);
        lr_jit_rt_to_string(interp, &rhs, &sb);
        lr_jit_rt_string_concat(interp, &sa, &sb, &args_base[2]);
        return;
    }
    /* Numeric fallback: coerce both to number */
    LRValue na, nb;
    lr_jit_rt_to_number(interp, &prop_val, &na);
    lr_jit_rt_to_number(interp, &rhs, &nb);
    if (na.tag == LR_TYPE_FLOAT64 && nb.tag == LR_TYPE_FLOAT64) {
        args_base[2] = na;
        args_base[2].u.float64 += nb.u.float64;
    } else if (na.tag == LR_TYPE_INT32 && nb.tag == LR_TYPE_INT32) {
        int64_t sum = (int64_t)na.u.int32 + (int64_t)nb.u.int32;
        if (sum >= INT32_MIN && sum <= INT32_MAX) {
            args_base[2].tag = LR_TYPE_INT32;
            args_base[2].u.int32 = (int32_t)sum;
        } else {
            args_base[2].tag = LR_TYPE_FLOAT64;
            args_base[2].u.float64 = (double)sum;
        }
    } else {
        args_base[2].tag = LR_TYPE_FLOAT64;
        args_base[2].u.float64 = (double)na.u.int32 + (double)nb.u.int32;
    }
}

void lr_jit_rt_set_prop(Interpreter *interp, void *prog_raw,
                        uint32_t name_idx, LRValue *args_base) {
    /* args_base[0] = obj, args_base[1] = val (inputs),
     * args_base[2] = result (output — the assigned value). */
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count || !args_base || !ctx) {
        if (args_base) args_base[2] = LR_VALUE_UNDEFINED;
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { args_base[2] = LR_VALUE_UNDEFINED; return; }
    LRValue obj = args_base[0];
    LRValue val = args_base[1];
    lr_set_property_str(ctx, obj, name, val);
    /* set-prop pushes the assigned value (the RHS) back onto the stack. */
    args_base[2] = val;
}

void lr_jit_rt_get_elem(Interpreter *interp,
                        const LRValue *obj, const LRValue *key, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !obj || !key || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    /* Fast path: integer key on array-like object. */
    if (key->tag == LR_TYPE_INT32 && key->u.int32 >= 0) {
        *out = lr_get_property_uint32(ctx, *obj, (uint32_t)key->u.int32);
        return;
    }
    LRString *atom = lr_to_atom(ctx, *key);
    *out = lr_get_property(ctx, *obj, atom);
}

void lr_jit_rt_set_elem(Interpreter *interp, const LRValue *args_base,
                        LRValue *out) {
    /* args_base layout: [obj, key, val] (contiguous, populated by MIR_OP_move
     * instructions before the call). */
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !args_base || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    LRValue obj = args_base[0];
    LRValue key = args_base[1];
    LRValue val = args_base[2];
    if (key.tag == LR_TYPE_INT32 && key.u.int32 >= 0) {
        lr_set_property_uint32(ctx, obj, (uint32_t)key.u.int32, val);
    } else {
        LRString *atom = lr_to_atom(ctx, key);
        lr_set_property(ctx, obj, atom, val);
    }
    *out = val;
}

void lr_jit_rt_in(Interpreter *interp,
                  const LRValue *obj, const LRValue *key, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !obj || !key || !ctx) {
        out->tag = LR_TYPE_BOOL; out->u.bool_val = 0; return;
    }
    /* lr_has_property returns 1 if the property exists. */
    int found = 0;
    if (key->tag == LR_TYPE_INT32 && key->u.int32 >= 0) {
        /* Reuse uint32 get: presence is implied by non-exceptional return. */
        LRValue v = lr_get_property_uint32(ctx, *obj, (uint32_t)key->u.int32);
        found = (v.tag != LR_TYPE_UNDEFINED);
    } else {
        LRString *atom = lr_to_atom(ctx, *key);
        found = lr_has_property(ctx, *obj, atom);
    }
    out->tag = LR_TYPE_BOOL;
    out->u.bool_val = (uint8_t)(found ? 1 : 0);
}

void lr_jit_rt_instanceof(Interpreter *interp,
                          const LRValue *obj, const LRValue *ctor, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !obj || !ctor || !ctx) {
        out->tag = LR_TYPE_BOOL; out->u.bool_val = 0; return;
    }
    int is_inst = 0;
    if (lr_is_object(*obj)) {
        LRValue proto = lr_get_property_str(ctx, *ctor, "prototype");
        if (lr_is_object(proto)) {
            LRValue p = lr_get_prototype(ctx, *obj);
            while (lr_is_object(p)) {
                /* Strict equality: same tag AND same pointer / same primitive. */
                if (p.tag == proto.tag) {
                    int eq = 0;
                    if (p.tag == LR_TYPE_OBJECT && p.u.ptr == proto.u.ptr) eq = 1;
                    else if (p.tag != LR_TYPE_OBJECT &&
                             p.tag != LR_TYPE_STRING) {
                        /* primitive types: compare raw bytes */
                        eq = (p.u.int32 == proto.u.int32);
                    }
                    if (eq) { is_inst = 1; break; }
                }
                LRValue next = lr_get_prototype(ctx, p);
                lr_free_value(ctx, p);
                p = next;
            }
            lr_free_value(ctx, p);
        }
        lr_free_value(ctx, proto);
    }
    out->tag = LR_TYPE_BOOL;
    out->u.bool_val = (uint8_t)(is_inst ? 1 : 0);
}

void lr_jit_rt_throw(Interpreter *interp, const LRValue *val) {
    if (!interp || !val) return;
    interp_bc_throw(interp, *val);
    /* interp_bc_throw does not return control here; it sets exception state
     * and unwinds.  If it does return, treat as no-op. */
}

void lr_jit_rt_call_method(Interpreter *interp, void *prog_raw,
                           uint32_t packed, LRValue *args_base) {
    /* packed = (name_idx << 16) | argc.
     * args_base[0..argc] = [this, arg1..argN] (inputs),
     * args_base[argc+1]   = result (output). */
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    uint32_t name_idx = packed >> 16;
    uint32_t argc = packed & 0xFFFF;
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count || !args_base || !ctx) {
        if (args_base) args_base[argc + 1] = LR_VALUE_UNDEFINED;
        return;
    }
    LRValue this_val = args_base[0];
    LRValue *argv = &args_base[1];
    const char *name = prog->pool[name_idx].u.str;
    if (!name) { args_base[argc + 1] = LR_VALUE_UNDEFINED; return; }
    LRValue callee = lr_get_property_str(ctx, this_val, name);
    args_base[argc + 1] = interp_bc_call(interp, callee, this_val, (int)argc, argv);
}

void lr_jit_rt_call_elem(Interpreter *interp,
                         uint32_t argc, LRValue *args_base, LRValue *out) {
    if (!out) return;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !args_base || !ctx) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    /* args_base layout: [this, key, arg1, ..., argN] (argc+2 contiguous). */
    LRValue this_val = args_base[0];
    LRValue key = args_base[1];
    LRValue *argv = &args_base[2];
    LRString *atom = lr_to_atom(ctx, key);
    LRValue callee = lr_get_property(ctx, this_val, atom);
    *out = interp_bc_call(interp, callee, this_val, (int)argc, argv);
}

void lr_jit_rt_construct(Interpreter *interp,
                         uint32_t argc, LRValue *args_base, LRValue *out) {
    if (!out) return;
    if (!interp || !args_base || argc == 0) {
        out->tag = LR_TYPE_UNDEFINED; out->u.ptr = NULL; return;
    }
    LRValue callee = args_base[0];
    LRValue *argv = &args_base[1];
    *out = interp_bc_construct(interp, callee, (int)argc, argv);
}

void lr_jit_rt_fmod(Interpreter *interp, const LRValue *a, const LRValue *b, LRValue *out) {
    if (!out || !a || !b) return;
    double fa = 0.0, fb = 0.0;
    lr_to_float64(interp->ctx, &fa, *a);
    lr_to_float64(interp->ctx, &fb, *b);
    out->tag = LR_TYPE_FLOAT64;
    out->u.float64 = fmod(fa, fb);
}

void lr_jit_init(LRJITRuntime *jit) {
    if (!jit) return;
    jit->code_head = NULL;
    jit->enabled = 1;
    jit->total_code_size = 0;
    jit->compile_count = 0;
    jit->bailout_count = 0;
    jit->exec_count = 0;
}

void lr_jit_set_enabled(LRJITRuntime *jit, int enabled) {
    if (jit) jit->enabled = enabled;
}

int lr_jit_notify_call(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return 0;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return 0;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled) return 0;

    LRJITCode *code = NULL;
    for (LRJITCode *c = jit->code_head; c; c = c->next) {
        if (c->prog == prog) {
            code = c;
            break;
        }
    }
    if (!code) {
        code = (LRJITCode *)calloc(1, sizeof(LRJITCode));
        if (!code) return 0;
        code->prog = prog;
        code->hot_counter = 0;
        code->compiled = 0;
        code->bailout_occurred = 0;
        code->loop_compiled = 0;
        code->next = jit->code_head;
        jit->code_head = code;
    }
    code->hot_counter++;

    /* Return >0 only when reaching the threshold for first compilation.
     * After successful compilation (code->compiled), reset to avoid
     * re-compiling on every subsequent call. */
    if (code->compiled) {
        code->hot_counter = 0;
        return 0;
    }

    /* Use hotspot analysis to determine if compilation is needed */
    int should_compile = 0;
    if (jit->hotspot_active) {
        /* Hotspot mode: compile if function is hot OR if bailout occurred */
        should_compile = (code->hot_counter >= LR_JIT_HOTSPOT_FUNC_MIN_CALLS) ||
                         (code->bailout_occurred && code->hot_counter > 0);

        if (should_compile) {
            jit->hotspot_compile_queue_size++;
        }
    } else {
        /* Legacy mode: fixed threshold */
        should_compile = (code->hot_counter >= LR_JIT_HOT_FUNC_CALLS) ? code->hot_counter : 0;
    }

    return should_compile;
}

LRJITEntry lr_jit_compile(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return NULL;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return NULL;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled) return NULL;

    /* Log compilation attempts (only when LR_DEBUG_JIT is set) */
    LR_JIT_DBG("[JIT-COMP] compiling prog=%p code_len=%d nparams=%d jit_skip=%d jit_entry=%p\n",
            (void*)prog, prog->code_len, prog->nparams, prog->jit_skip, (void*)prog->jit_entry);

    /* Phase 1: Bytecode -> MIR (frontend) */
    MIRProgram *mir = mir_compile_bytecode(interp, prog);
    if (!mir) {
        LR_JIT_ERR("[JIT-ERR] compile failed: mir is NULL\n");
        return NULL;
    }
    /* Bail-out produces partial MIR — skip JIT if too few instructions */
    if (mir->num_instructions < 4) {
        LR_JIT_DBG("[JIT-SKIP] too few MIR instructions: %d (prog=%p)\n", mir->num_instructions, (void*)prog);
        mir_free(mir);
        return NULL;
    }
    /* Guard against stack overflow from excessive MIR variables.
     * Recursive call expansion can generate thousands of MIR vars, causing
     * codegen_emit to emit code that accesses offsets beyond the 2112-byte
     * stack frame (LR_JIT_MAX_LOCALS*16+64).  Mark jit_skip so the interpreter runs it instead. */
    int max_vars = mir->num_vars;
    if (max_vars > LR_JIT_MAX_LOCALS) {
        LR_JIT_ERR("[JIT-ERR] too many MIR vars (%d > LR_JIT_MAX_LOCALS=%d), marking jit_skip for prog=%p (code_len=%d)\n",
                max_vars, LR_JIT_MAX_LOCALS, (void*)prog, (int)prog->code_len);
        prog->jit_skip = 1;
        mir_free(mir);
        return NULL;
    }
    /* Partial MIR (bailout occurred) — mark jit_skip to prevent using
     * incomplete native code that would return wrong values. */
    if (mir->is_partial) {
        LR_JIT_ERR("[JIT-ERR] partial MIR: marking jit_skip for prog=%p\n", (void*)prog);
        prog->jit_skip = 1;
        mir_free(mir);
        return NULL;
    }

    /* Phase 2: MIR -> native code via SLJIT (backend).
     * Serialize access to the entire compilation+registration pipeline since
     * SLJIT's allocator uses a non-thread-safe global free-list, and the
     * JIT runtime linked list (code_head) is also shared. */
    size_t code_size = 0;
    int num_mir_ops = mir->num_instructions;
    if (lr_debug_jit_enabled())
        mir_dump(mir, "MIR-BEFORE-CODEGEN");
    /* Serialize MIR before emitting (for IOME586 cross-platform cache). */
    size_t mir_ser_len = 0;
    uint8_t *mir_ser_copy = mir_serialize(mir, (void*)prog, &mir_ser_len);

    LR_JIT_CODEGEN_LOCK();
    uint8_t *code = codegen_emit(mir, &code_size, prog->nparams, (void*)prog);
    mir_free(mir);
    if (!code || code_size == 0) {
        LR_JIT_DBG("[JIT-COMP] codegen_emit failed: code=%p code_size=%zu\n", code, code_size);
        LR_JIT_CODEGEN_UNLOCK();
        if (mir_ser_copy) free(mir_ser_copy);
        return NULL;
    }

    LR_JIT_DBG("[JIT-COMP] compiled %d MIR ops -> %zu bytes native code for prog=%p\n",
            num_mir_ops, code_size, (void*)prog);

    /* Make code executable */
#if defined(_WIN32)
    DWORD old_protect;
    VirtualProtect(code, code_size, PAGE_EXECUTE_READWRITE, &old_protect);
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
   || defined(__NetBSD__) || defined(__APPLE__) || defined(__ANDROID__)
    mprotect(code, code_size, PROT_EXEC | PROT_READ | PROT_WRITE);
#endif

    LR_JIT_DBG("[JIT] after VirtualProtect: code=%p size=%zu old_protect=%u\n",
            (void*)code, code_size, (unsigned)old_protect);

    /* ── V8-inspired: store entry directly on prog for O(1) lookup ────
     * This avoids the O(n) linked-list walk in lr_jit_lookup() on every
     * call.  The JIT runtime list is kept for eviction tracking only. */
    prog->jit_entry = (void*)code;
    prog->jit_code_size = code_size;

    /* Store code in JIT runtime with LRU eviction (kept for tracking) */
    LRJITCode *jc = (LRJITCode *)calloc(1, sizeof(LRJITCode));
    if (!jc) {
        LR_JIT_CODEGEN_UNLOCK();
        return NULL;
    }
    jc->code = code;
    jc->code_size = code_size;
    jc->code_capacity = code_size;
    jc->prog = prog;
    jc->hot_counter = 0;
    jc->compiled = 1;
    jc->bailout_occurred = 0;
    jc->next = jit->code_head;
    jit->code_head = jc;
    jit->total_code_size += code_size;
    jit->compile_count++;

    /* LRU eviction */
    while (jit->total_code_size > LR_JIT_CACHE_MAX_SIZE && jit->code_head) {
        LRJITCode *victim = jit->code_head;
        jit->code_head = victim->next;
        jit->total_code_size -= victim->code_size;
        if (victim->prog) {
            victim->prog->jit_entry = NULL;
            victim->prog->jit_code_size = 0;
        }
        sljit_free_exec(victim->code);
        free(victim);
    }

    LR_JIT_CODEGEN_UNLOCK();

    /* Append serialized MIR to accumulated buffer (outside lock) */
    if (mir_ser_copy) {
        if (jit) {
            uint32_t nfuncs = jit->mir_ser_len > 0 ? *(uint32_t*)jit->mir_ser : 0u;
            nfuncs++;
            size_t cur = jit->mir_ser_len > 0 ? jit->mir_ser_len : 4u;
            size_t block_size = 4u + mir_ser_len;
            size_t new_total = cur + block_size;
            uint8_t *new_buf = (uint8_t *)realloc(jit->mir_ser, new_total);
            if (new_buf) {
                jit->mir_ser = new_buf;
                memcpy(new_buf, &nfuncs, 4);
                uint32_t len32 = (uint32_t)mir_ser_len;
                memcpy(new_buf + cur, &len32, 4);
                memcpy(new_buf + cur + 4u, mir_ser_copy, mir_ser_len);
                jit->mir_ser_len = new_total;
            }
        }
        free(mir_ser_copy);
    }

    return (LRJITEntry)code;
}

LRJITEntry lr_jit_lookup(LRJITRuntime *jit, LRProgram *prog) {
    if (!jit || !prog) return NULL;
    /* V8-inspired: O(1) direct lookup via prog->jit_entry */
    if (prog->jit_entry) return (LRJITEntry)prog->jit_entry;
    /* Fallback: walk the linked list (for programs managed externally) */
    for (LRJITCode *c = jit->code_head; c; c = c->next) {
        if (c->prog == prog && c->compiled) return (LRJITEntry)c->code;
    }
    return NULL;
}

LRJITCode *lr_jit_lookup_code(LRJITRuntime *jit, LRProgram *prog) {
    if (!jit || !prog) return NULL;
    for (LRJITCode *c = jit->code_head; c; c = c->next) {
        if (c->prog == prog) return c;
    }
    return NULL;
}

void lr_jit_mark_bailout(LRJITRuntime *jit, LRProgram *prog) {
    if (!jit || !prog) return;
    for (LRJITCode *c = jit->code_head; c; c = c->next) {
        if (c->prog == prog) {
            c->bailout_occurred = 1;
            jit->bailout_count++;
            break;
        }
    }
}

/* ── Warm-path MIR cache JIT precompilation ───────────────────────────────
 *
 * When an IOME586 archive contains serialized MIR (cross-platform cache),
 * this function iterates over all precompiled function bodies and attempts
 * to emit native code directly from the cached MIR, bypassing the
 * bytecode→MIR frontend.  This eliminates JIT warm-up latency on subsequent
 * runs.
 *
 * The MIR buffer format is:
 *   [num_functions(u32)][len0(u32)+data0][len1(u32)+data1]...
 *
 * Bodies are matched sequentially: the Nth body without a jit_entry gets
 * the Nth MIR block.  This relies on both precompile order and cold-run
 * compilation order following AST pre-order traversal, which is guaranteed
 * by interp_precompile_bodies_cas.
 */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int count;
    LRJITRuntime *jit;
} WarmJitCtx;

static void warm_jit_emit_body_cb(ASTNode *ast_body, BCProgram *prog,
                                   void *userdata)
{
    (void)ast_body;
    WarmJitCtx *wc = (WarmJitCtx *)userdata;
    if (!wc || !wc->data || prog->jit_entry || prog->jit_skip) return;
    if (wc->pos >= wc->len) return;

    uint32_t blk_len = 0;
    memcpy(&blk_len, wc->data + wc->pos, 4);
    if (blk_len == 0 || wc->pos + 4u + blk_len > wc->len) return;

    MIRProgram *mir = mir_deserialize(
        wc->data + wc->pos + 4u, blk_len, NULL);
    if (!mir) return;

    size_t code_size = 0;
    LR_JIT_CODEGEN_LOCK();
    uint8_t *code = codegen_emit(mir, &code_size,
                                 prog->nparams, (void*)prog);
    LR_JIT_CODEGEN_UNLOCK();
    mir_free(mir);
    if (!code || code_size == 0) {
        prog->jit_skip = 1;
        return;
    }

#if defined(_WIN32)
    DWORD old_protect;
    VirtualProtect(code, code_size, PAGE_EXECUTE_READWRITE, &old_protect);
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
   || defined(__NetBSD__) || defined(__APPLE__) || defined(__ANDROID__) \
   || defined(__OHOS__)
    mprotect(code, code_size, PROT_EXEC | PROT_READ | PROT_WRITE);
#endif

    prog->jit_entry = (void*)code;
    prog->jit_code_size = code_size;

    LRJITCode *jc = (LRJITCode *)calloc(1, sizeof(LRJITCode));
    if (jc) {
        jc->code = code;
        jc->code_size = code_size;
        jc->code_capacity = code_size;
        jc->prog = prog;
        jc->hot_counter = 0;
        jc->compiled = 1;
        jc->bailout_occurred = 0;
        jc->next = wc->jit->code_head;
        wc->jit->code_head = jc;
        wc->jit->total_code_size += code_size;
        wc->jit->compile_count++;
    }
    wc->count++;

    /* LRU eviction */
    while (wc->jit->total_code_size > LR_JIT_CACHE_MAX_SIZE
           && wc->jit->code_head) {
        LRJITCode *victim = wc->jit->code_head;
        wc->jit->code_head = victim->next;
        wc->jit->total_code_size -= victim->code_size;
        if (victim->prog) {
            victim->prog->jit_entry = NULL;
            victim->prog->jit_code_size = 0;
        }
        sljit_free_exec(victim->code);
        free(victim);
    }

    wc->pos += 4u + blk_len;
}

int lr_jit_precompile_from_mir_cache(Interpreter *interp,
                                      const uint8_t *mir_data, size_t mir_len,
                                      int *out_compiled_count)
{
    if (!interp || !mir_data || mir_len < 4) return 0;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return 0;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled) return 0;

    uint32_t num_funcs = 0;
    memcpy(&num_funcs, mir_data, 4);
    if (num_funcs == 0) return 0;

    WarmJitCtx wctx;
    wctx.data = mir_data; wctx.len = mir_len; wctx.pos = 4u;
    wctx.count = 0; wctx.jit = jit;

    interp_iterate_precompiled_bodies(warm_jit_emit_body_cb, &wctx);

    if (out_compiled_count) *out_compiled_count = wctx.count;
    return wctx.count;
}

void lr_jit_destroy(LRJITRuntime *jit) {
    if (!jit) return;
    LRJITCode *c = jit->code_head;
    while (c) {
        LRJITCode *next = c->next;
        if (c->prog) {
            c->prog->jit_entry = NULL;
            c->prog->jit_code_size = 0;
        }
        sljit_free_exec(c->code);
        free(c);
        c = next;
    }
    jit->code_head = NULL;
    jit->total_code_size = 0;
}

void lr_jit_force_compile(LRJITRuntime *jit, LRProgram *prog) {
    if (!jit || !prog) return;
    if (!jit->enabled) return;
    if (!prog->code) return;

    if (lr_jit_lookup(jit, prog)) return;

    lr_jit_compile(NULL, prog);
}

/* ── Hot loop detection (V8-inspired counter-based triggering) ───────── */

/* ── Hotspot Analysis Implementation ─────────────────────────────────── */

void lr_jit_hotspot_init(LRJITRuntime *jit) {
    if (!jit) return;
    jit->hotspot_active = 1;
    jit->hotspot_compile_queue_size = 0;
}

void lr_jit_hotspot_set_enabled(LRJITRuntime *jit, int enabled) {
    if (!jit) return;
    jit->hotspot_active = (enabled ? 1 : 0);
}

int lr_jit_is_hotspot_func(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return 0;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return 0;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled || !jit->hotspot_active) return 0;

    /* Check if function has been called enough times to be a hotspot */
    LRJITCode *code = lr_jit_lookup_code(jit, prog);
    if (!code) return 0;

    /* Hotspot criteria:
     *  1. Called at least LR_JIT_HOTSPOT_FUNC_MIN_CALLS times, OR
     *  2. Already had a bailout (indicating it's complex enough to need optimization) */
    int is_hotspot = (code->hot_counter >= LR_JIT_HOTSPOT_FUNC_MIN_CALLS) ||
                     (code->bailout_occurred && code->hot_counter > 0);

    if (is_hotspot) {
        jit->hotspot_compile_queue_size++;
    }

    return is_hotspot;
}

int lr_jit_is_hotspot_loop(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return 0;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return 0;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled || !jit->hotspot_active) return 0;

    /* Check if loop has run enough iterations to be a hotspot */
    int loop_iters = interp->jit_loop_counter;
    int is_hotspot = (loop_iters >= LR_JIT_HOTSPOT_LOOP_MIN_ITER);

    if (is_hotspot) {
        jit->hotspot_compile_queue_size++;
    }

    return is_hotspot;
}

void lr_jit_hotspot_get_stats(LRJITRuntime *jit, int *func_hotspots,
                               int *loop_hotspots, int *total_compiles) {
    if (!jit) return;

    int funcs = 0, loops = 0;
    for (LRJITCode *c = jit->code_head; c; c = c->next) {
        if (c->hot_counter >= LR_JIT_HOTSPOT_FUNC_MIN_CALLS) funcs++;
        if (c->loop_compiled) loops++;
    }

    if (func_hotspots) *func_hotspots = funcs;
    if (loop_hotspots) *loop_hotspots = loops;
    if (total_compiles) *total_compiles = jit->compile_count;
}


int lr_jit_notify_loop(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return 0;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return 0;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled) return 0;

    /* Increment loop counter on interpreter side */
    interp->jit_loop_counter++;

    /* Check if hot enough to trigger compilation using hotspot analysis */
    int should_compile = 0;
    if (jit->hotspot_active) {
        /* Use hotspot analysis: compile if loop has run enough iterations
         * OR if the program is already known to be frequently called */
        LRJITCode *code = lr_jit_lookup_code(jit, prog);
        if (code && code->hot_counter > 0) {
            /* Function has been called multiple times - lower threshold */
            should_compile = (interp->jit_loop_counter >= (LR_JIT_HOT_LOOP_ITERS / 2));
        } else {
            /* First time seeing this loop - use standard threshold */
            should_compile = (interp->jit_loop_counter >= LR_JIT_HOT_LOOP_ITERS);
        }
    } else {
        /* Legacy behavior: fixed threshold */
        should_compile = (interp->jit_loop_counter >= LR_JIT_HOT_LOOP_ITERS);
    }

    if (should_compile) {
        /* Compile only if not already compiled */
        if (!lr_jit_lookup(jit, prog)) {
            lr_jit_compile_hot_loop(interp, prog);
        }
        /* Reset counter to avoid repeated compilation */
        interp->jit_loop_counter = 0;
        return 1;
    }
    return 0;
}

/* ── Runtime helper for MIR_OP_loop_guard ─────────────────────────── */
/* Called from JIT code (not interpreter). Returns 1 if compilation
 * was triggered and caller should bail out to interpreter. */
int lr_jit_rt_notify_loop(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return 0;
    LRContext *ctx = interp->ctx;
    if (!ctx || !ctx->rt) return 0;
    LRRuntime *rt = (LRRuntime *)ctx->rt;
    LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
    if (!jit || !jit->enabled) return 0;

    /* If the function is already JIT-compiled, skip loop_guard entirely.
     * Compilation already happened; further loop iterations should run
     * pure JIT without overhead. */
    if (prog && prog->jit_entry) return 0;

    interp->jit_loop_counter++;

    int should_compile = 0;
    if (jit->hotspot_active) {
        LRJITCode *code = lr_jit_lookup_code(jit, prog);
        if (code && code->hot_counter > 0) {
            should_compile = (interp->jit_loop_counter >= (LR_JIT_HOT_LOOP_ITERS / 2));
        } else {
            should_compile = (interp->jit_loop_counter >= LR_JIT_HOT_LOOP_ITERS);
        }
    } else {
        should_compile = (interp->jit_loop_counter >= LR_JIT_HOT_LOOP_ITERS);
    }

    if (should_compile) {
        if (!lr_jit_lookup(jit, prog)) {
            lr_jit_compile_hot_loop(interp, prog);
        }
        interp->jit_loop_counter = 0;
        return 1;
    }
    return 0;
}

/* ── Fast path compilation for hot loops ─────────────────────────────── */

LRJITEntry lr_jit_compile_hot_loop(Interpreter *interp, LRProgram *prog) {
    if (!interp || !prog) return NULL;

    /* Check if already compiled */
    LRContext *ctx = interp->ctx;
    if (ctx && ctx->rt) {
        LRRuntime *rt = (LRRuntime *)ctx->rt;
        LRJITRuntime *jit = (LRJITRuntime *)rt->jit_runtime;
        if (jit && lr_jit_lookup(jit, prog)) {
            return (LRJITEntry)jit->code_head->code;
        }
    }

    /* Use standard compilation for now (optimized version would add
     * type specialization and loop unrolling) */
    return lr_jit_compile(interp, prog);
}

/* ── Atomics JIT runtime helpers ────────────────────────────────────────
 * These bypass the full C-API call chain and directly manipulate
 * the TypedArray's underlying buffer for maximum performance. */

static int jit_atomics_get_typed_array_info(LRContext *ctx, LRValue ta_val,
    size_t *p_offset, size_t *p_esize, TypedArrayData **out_tad)
{
    if (ta_val.tag != LR_TYPE_OBJECT) return -1;
    LRObject *o = (LRObject *)ta_val.u.ptr;
    if (o->type != LR_OBJ_TYPED_ARRAY || !o->opaque) return -1;
    TypedArrayData *tad = (TypedArrayData *)o->opaque;
    if (tad->magic == 6 || tad->magic == 7 || tad->magic == 8 || tad->magic == 9)
        return -1;  /* float/bigint not supported */
    size_t buf_size = 0;
    uint8_t *base = lr_get_array_buffer(ctx, &buf_size, tad->buffer);
    if (!base) return -1;
    if (p_offset) *p_offset = tad->byte_offset;
    if (p_esize) *p_esize = tad->element_size;
    if (out_tad) *out_tad = tad;
    return (int)buf_size;
}

static uint32_t jit_atomics_apply(uint8_t *base, size_t buf_size, size_t bi,
                                   size_t esize, int op, uint32_t v, uint32_t e,
                                   int has_workers)
{
    uint32_t mask = (esize == 1) ? 0xFFu : (esize == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    v &= mask; e &= mask;
    size_t word_off = bi & ~(size_t)3;
    int aligned = ((bi & (esize - 1)) == 0);

    if (aligned && word_off + 4 <= buf_size) {
        volatile int32_t *wp = (volatile int32_t *)(base + word_off);
        unsigned shift = (unsigned)((bi - word_off) * 8);

        if (!has_workers) {
            uint32_t oldword = (uint32_t)*wp;
            uint32_t oldelem = (oldword >> shift) & mask;
            int do_write;
            uint32_t newelem = 0;
            switch (op) {
                case 0: { newelem = (oldelem + v) & mask; do_write = 1; break; }
                case 1: { newelem = (oldelem - v) & mask; do_write = 1; break; }
                case 2: { newelem = (oldelem & v) & mask; do_write = 1; break; }
                case 3: { newelem = (oldelem | v) & mask; do_write = 1; break; }
                case 4: { newelem = (oldelem ^ v) & mask; do_write = 1; break; }
                case 5: { newelem = v; do_write = 1; break; }
                case 6: { newelem = (oldelem == e) ? v : oldelem; do_write = (oldelem == e); break; }
                case 7: return oldelem;
                case 8: *wp = (int32_t)((oldword & ~(mask << shift)) | ((uint32_t)v << shift)); return v;
                default: return oldelem;
            }
            if (do_write) {
                uint32_t newword = (oldword & ~(mask << shift)) | (newelem << shift);
                *wp = (int32_t)newword;
            }
            return oldelem;
        }

        if (esize == 4) {
            switch (op) {
                case 7:  return (uint32_t)lr_atomic_load_32(wp);
                case 0:  return (uint32_t)lr_atomic_fetch_add_32(wp, (int32_t)v);
                case 1:  return (uint32_t)lr_atomic_fetch_add_32(wp, -(int32_t)v);
                case 2:  { for (;;) { uint32_t ow = (uint32_t)lr_atomic_load_32(wp); if ((uint32_t)lr_atomic_cas_32(wp, (int32_t)ow, (int32_t)(ow & v)) == ow) return ow; } }
                case 3:  { for (;;) { uint32_t ow = (uint32_t)lr_atomic_load_32(wp); if ((uint32_t)lr_atomic_cas_32(wp, (int32_t)ow, (int32_t)(ow | v)) == ow) return ow; } }
                case 4:  { for (;;) { uint32_t ow = (uint32_t)lr_atomic_load_32(wp); if ((uint32_t)lr_atomic_cas_32(wp, (int32_t)ow, (int32_t)(ow ^ v)) == ow) return ow; } }
                case 5:  return (uint32_t)lr_atomic_xchg_32(wp, (int32_t)v);
                case 6:  return (uint32_t)lr_atomic_cas_32(wp, (int32_t)e, (int32_t)v);
                case 8:  lr_atomic_store_32(wp, (int32_t)v); return v;
                default: return (uint32_t)lr_atomic_load_32(wp);
            }
        }

        for (;;) {
            uint32_t oldword = (uint32_t)lr_atomic_load_32(wp);
            uint32_t oldelem = (oldword >> shift) & mask;
            int do_write;
            uint32_t newelem;
            switch (op) {
                case 0: newelem = (oldelem + v) & mask; do_write = 1; break;
                case 1: newelem = (oldelem - v) & mask; do_write = 1; break;
                case 2: newelem = (oldelem & v) & mask; do_write = 1; break;
                case 3: newelem = (oldelem | v) & mask; do_write = 1; break;
                case 4: newelem = (oldelem ^ v) & mask; do_write = 1; break;
                case 5: newelem = v; do_write = 1; break;
                case 6: newelem = (oldelem == e) ? v : oldelem; do_write = (oldelem == e); break;
                case 7: return oldelem;
                case 8: {
                    uint32_t newword = (oldword & ~(mask << shift)) | ((uint32_t)v << shift);
                    if ((uint32_t)lr_atomic_cas_32(wp, (int32_t)oldword, (int32_t)newword) == oldword) return v;
                    continue;
                }
                default: return oldelem;
            }
            if (!do_write) return oldelem;
            uint32_t newword = (oldword & ~(mask << shift)) | (newelem << shift);
            if ((uint32_t)lr_atomic_cas_32(wp, (int32_t)oldword, (int32_t)newword) == oldword)
                return oldelem;
        }
    }

    /* Non-atomic fallback */
    uint32_t oldelem;
    if (esize == 1)      oldelem = base[bi];
    else if (esize == 2) { uint16_t t; memcpy(&t, base + bi, 2); oldelem = t; }
    else                 { uint32_t t; memcpy(&t, base + bi, 4); oldelem = t; }
    int do_write;
    uint32_t newelem;
    switch (op) {
        case 0: newelem = (oldelem + v) & mask; do_write = 1; break;
        case 1: newelem = (oldelem - v) & mask; do_write = 1; break;
        case 2: newelem = (oldelem & v) & mask; do_write = 1; break;
        case 3: newelem = (oldelem | v) & mask; do_write = 1; break;
        case 4: newelem = (oldelem ^ v) & mask; do_write = 1; break;
        case 5: newelem = v; do_write = 1; break;
        case 6: newelem = (oldelem == e) ? v : oldelem; do_write = (oldelem == e); break;
        case 7: return oldelem;
        case 8: {
            if (do_write) {
                if (esize == 1)      base[bi] = (uint8_t)v;
                else if (esize == 2) { uint16_t t = (uint16_t)v; memcpy(base + bi, &t, 2); }
                else                 { uint32_t t = v; memcpy(base + bi, &t, 4); }
            }
            return v;
        }
        default: return oldelem;
    }
    if (do_write) {
        if (esize == 1)      base[bi] = (uint8_t)newelem;
        else if (esize == 2) { uint16_t t = (uint16_t)newelem; memcpy(base + bi, &t, 2); }
        else                 { uint32_t t = newelem; memcpy(base + bi, &t, 4); }
    }
    return oldelem;
}

static LRValue jit_atomics_result(LRContext *ctx, int magic, uint32_t raw)
{
    switch (magic) {
        case 0: return lr_new_int32(ctx, (int32_t)(int8_t)raw);
        case 1: return lr_new_int32(ctx, (int32_t)(int8_t)raw);
        case 2: return lr_new_int32(ctx, (int32_t)(uint8_t)raw);
        case 3: return lr_new_int32(ctx, (int32_t)(uint16_t)raw);
        case 4: return lr_new_float64(ctx, (double)(uint32_t)raw);
        case 5: return lr_new_int32(ctx, (int32_t)raw);
        default: return lr_new_int32(ctx, (int32_t)raw);
    }
}

void lr_jit_rt_atomics_load(Interpreter *interp, uint32_t argc,
                             LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 2 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx;
    lr_to_int32(ctx, &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t offset = bi + (size_t)idx * esize;
    if (offset + esize > bi + (size_t)idx * esize + esize) { /* bounds check via element */ }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 7, 0, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_store(Interpreter *interp, uint32_t argc,
                              LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 8, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_add(Interpreter *interp, uint32_t argc,
                            LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 0, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_sub(Interpreter *interp, uint32_t argc,
                            LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 1, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_and(Interpreter *interp, uint32_t argc,
                            LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 2, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_or(Interpreter *interp, uint32_t argc,
                           LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 3, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_xor(Interpreter *interp, uint32_t argc,
                            LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 4, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_exchange(Interpreter *interp, uint32_t argc,
                                 LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 3 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &iv, args_base[2]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 5, (uint32_t)iv, 0, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_compare_exchange(Interpreter *interp, uint32_t argc,
                                         LRValue *args_base, LRValue *out) {
    if (!interp || !args_base || argc < 4 || !out) {
        if (out) *out = LR_VALUE_UNDEFINED; return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) { *out = LR_VALUE_UNDEFINED; return; }
    size_t buf_size, bi, esize;
    TypedArrayData *tad;
    int32_t idx, iv, ie;
    lr_to_int32(ctx, &idx, args_base[1]);
    lr_to_int32(ctx, &ie, args_base[2]);
    lr_to_int32(ctx, &iv, args_base[3]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int bsz = jit_atomics_get_typed_array_info(ctx, args_base[0], &bi, &esize, &tad);
    if (bsz < 0) { *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base = lr_get_array_buffer(ctx, (size_t*)&buf_size, tad->buffer);
    if (!base) { *out = LR_VALUE_UNDEFINED; return; }
    size_t total = bi + (size_t)idx * esize + esize;
    if (total > (size_t)bsz) { *out = LR_VALUE_UNDEFINED; return; }
    uint32_t raw = jit_atomics_apply(base, (size_t)bsz, bi + (size_t)idx * esize, esize, 6, (uint32_t)iv, (uint32_t)ie, g_lr_atomics_has_workers);
    *out = jit_atomics_result(ctx, tad->magic, raw);
}

void lr_jit_rt_atomics_is_lock_free(Interpreter *interp, uint32_t argc,
                                     LRValue *args_base, LRValue *out) {
    (void)interp;
    if (!args_base || !out) return;
    int32_t size = 4;
    if (argc >= 1) lr_to_int32(jit_interp_ctx(NULL), &size, args_base[0]);
    out->tag = LR_TYPE_BOOL;
    out->u.bool_val = (uint8_t)((size == 1 || size == 2 || size == 4) ? 1 : 0);
}

/* ── Atomics inline optimization helpers ────────────────────────────────
 * Cache layout (5 LRValue slots = 80 bytes at src1):
 *   src1[0]: tag=LR_TYPE_POINTER, u.ptr = base_ptr, u.float64 = buf_size
 *   src1[1]: tag=LR_TYPE_FLOAT64, u.float64 = byte_offset
 *   src1[2]: tag=LR_TYPE_FLOAT64, u.float64 = element_size
 *   src1[3]: tag=LR_TYPE_FLOAT64, u.float64 = magic
 *   src1[4]: tag=LR_TYPE_FLOAT64, u.float64 = valid (1.0=ok, 0.0=invalid)
 *
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
 *   R3(CX)=interp, R1(DX)=cache_ptr (points to src1), R2(R8)=args_base_ptr (src1),
 *   TMP_REG1(R9)=out_ptr (dst)
 */

void lr_jit_rt_atomics_cache(Interpreter *interp, void *ta_ptr, void *dst_ptr) {
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx || !ta_ptr || !dst_ptr) {
        memset(dst_ptr, 0, 80);
        return;
    }
    LRValue *dst = (LRValue *)dst_ptr;
    LRValue *ta = (LRValue *)ta_ptr;
    if (ta->tag != LR_TYPE_OBJECT) { memset(dst, 0, 80); return; }
    LRObject *o = (LRObject *)ta->u.ptr;
    if (o->type != LR_OBJ_TYPED_ARRAY || !o->opaque) { memset(dst, 0, 80); return; }
    TypedArrayData *tad = (TypedArrayData *)o->opaque;
    if (tad->magic == 6 || tad->magic == 7 || tad->magic == 8 || tad->magic == 9) {
        memset(dst, 0, 80); return;
    }
    size_t buf_size = 0;
    uint8_t *base = lr_get_array_buffer(ctx, &buf_size, tad->buffer);
    if (!base) { memset(dst, 0, 80); return; }
    dst[0].tag = LR_TYPE_OBJECT;
    dst[0].u.ptr = base;
    dst[0].u.float64 = (double)buf_size;
    dst[1].tag = LR_TYPE_FLOAT64;
    dst[1].u.float64 = (double)tad->byte_offset;
    dst[2].tag = LR_TYPE_FLOAT64;
    dst[2].u.float64 = (double)tad->element_size;
    dst[3].tag = LR_TYPE_FLOAT64;
    dst[3].u.float64 = (double)tad->magic;
    dst[4].tag = LR_TYPE_FLOAT64;
    dst[4].u.float64 = 1.0;  /* valid */
}

static LRValue lr_jit_atomics_inline_do(Interpreter *interp, uint8_t *base,
    size_t buf_size, size_t byte_offset, size_t element_size,
    int magic, uint32_t index, uint32_t value, int op) {
    LRValue out;
    out.tag = LR_TYPE_UNDEFINED;
    if (!base || element_size == 0) return out;
    size_t esize = element_size;
    size_t bi = byte_offset;
    size_t total = bi + (size_t)index * esize + esize;
    if (total > buf_size) return out;
    uint32_t raw = jit_atomics_apply(base, buf_size, bi + (size_t)index * esize,
                                      esize, op, value, 0, g_lr_atomics_has_workers);
    return jit_atomics_result(jit_interp_ctx(interp), magic, raw);
}

void lr_jit_rt_atomics_inline_load(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, 0, 7);
}

void lr_jit_rt_atomics_inline_store(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val32;
    lr_to_int32(jit_interp_ctx(interp), &val32, args_base[2]);
    (void)lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, (uint32_t)val32, 8);
    *out = LR_VALUE_UNDEFINED;
}

void lr_jit_rt_atomics_inline_add(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val32;
    lr_to_int32(jit_interp_ctx(interp), &val32, args_base[2]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, (uint32_t)val32, 0);
}

void lr_jit_rt_atomics_inline_sub(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val;
    lr_to_int32(jit_interp_ctx(interp), &val, args_base[2]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, val, 1);
}

void lr_jit_rt_atomics_inline_and(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val;
    lr_to_int32(jit_interp_ctx(interp), &val, args_base[2]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, val, 2);
}

void lr_jit_rt_atomics_inline_or(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val;
    lr_to_int32(jit_interp_ctx(interp), &val, args_base[2]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, val, 3);
}

void lr_jit_rt_atomics_inline_xor(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val32;
    lr_to_int32(jit_interp_ctx(interp), &val32, args_base[2]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, (uint32_t)val32, 4);
}

void lr_jit_rt_atomics_inline_exchange(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t val32;
    lr_to_int32(jit_interp_ctx(interp), &val32, args_base[2]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, (uint32_t)val32, 5);
}

void lr_jit_rt_atomics_inline_compare_exchange(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) { if (out) *out = LR_VALUE_UNDEFINED; return; }
    uint8_t *base_ptr = cache[0].u.ptr;
    size_t buf_size = (size_t)cache[0].u.float64;
    size_t byte_offset = (size_t)cache[1].u.float64;
    size_t element_size = (size_t)cache[2].u.float64;
    int magic = (int)cache[3].u.float64;
    if (!base_ptr || element_size == 0 || cache[4].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED; return;
    }
    int32_t idx;
    lr_to_int32(jit_interp_ctx(interp), &idx, args_base[1]);
    if (idx < 0) { *out = LR_VALUE_UNDEFINED; return; }
    int32_t expected32, replacement32;
    lr_to_int32(jit_interp_ctx(interp), &expected32, args_base[2]);
    lr_to_int32(jit_interp_ctx(interp), &replacement32, args_base[3]);
    *out = lr_jit_atomics_inline_do(interp, base_ptr, buf_size,
        byte_offset, element_size, magic, (uint32_t)idx, (uint32_t)replacement32, 6);
    if (out->tag == LR_TYPE_UNDEFINED) return;
    uint32_t raw = (uint32_t)out->u.float64;
    uint32_t old_val;
    memcpy(&old_val, (void *)((uintptr_t)base_ptr + byte_offset + (size_t)idx * element_size),
           element_size);
    out->u.float64 = (double)(*((uint32_t *)&old_val));
}

/* ── Class optimization (m7): method cache ──────────────────────────────
 * Cache layout (5 LRValue slots at dst_base):
 *   dst[0]: tag=LR_TYPE_OBJECT, u.ptr = method_callee
 *   dst[1]: tag=LR_TYPE_FLOAT64, u.float64 = 1.0 (valid)
 *   dst[2]: tag=LR_TYPE_FLOAT64, u.float64 = argc (for call_cached_method)
 *   dst[3..4]: reserved (UNDEFINED)
 *
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
 *   R3(CX)=interp, R1(DX)=src1_base (LRValue array for args),
 *   R2(R8)=dst_base (LRValue array for cache output),
 *   TMP_REG1(R9)=method_name (const char*). */
void lr_jit_rt_method_cache(Interpreter *interp, const LRValue *src1_base,
    LRValue *dst_base, const char *method_name) {
    if (!dst_base || !src1_base || !method_name) {
        if (dst_base) {
            dst_base[0] = LR_VALUE_UNDEFINED;
            for (int i = 1; i < 5; i++) dst_base[i].tag = LR_TYPE_UNDEFINED;
        }
        return;
    }
    LRContext *ctx = jit_interp_ctx(interp);
    if (!ctx) {
        dst_base[0] = LR_VALUE_UNDEFINED;
        for (int i = 1; i < 5; i++) dst_base[i].tag = LR_TYPE_UNDEFINED;
        return;
    }
    LRValue this_val = src1_base[0];
    if (this_val.tag != LR_TYPE_OBJECT) {
        dst_base[0] = LR_VALUE_UNDEFINED;
        for (int i = 1; i < 5; i++) dst_base[i].tag = LR_TYPE_UNDEFINED;
        return;
    }
    LRValue callee = lr_get_property_str(ctx, this_val, method_name);
    if (callee.tag == LR_TYPE_UNDEFINED) {
        dst_base[0] = LR_VALUE_UNDEFINED;
        for (int i = 1; i < 5; i++) dst_base[i].tag = LR_TYPE_UNDEFINED;
        return;
    }
    dst_base[0] = callee;
    dst_base[1].tag = LR_TYPE_FLOAT64;
    dst_base[1].u.float64 = 1.0;
    for (int i = 2; i < 5; i++) dst_base[i].tag = LR_TYPE_UNDEFINED;
}

/* Call a pre-looked-up method without repeated property lookup.
 * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
 *   R3(CX)=interp, R1(DX)=cache_ptr (5-LRValue cache),
 *   R2(R8)=args_base_ptr (src1_base), TMP_REG1(R9)=out_ptr
 */
void lr_jit_rt_call_cached_method(Interpreter *interp, const LRValue *cache,
    const LRValue *args_base, LRValue *out) {
    if (!out || !cache || !args_base) {
        if (out) *out = LR_VALUE_UNDEFINED;
        return;
    }
    LRValue callee = cache[0];
    if (callee.tag == LR_TYPE_UNDEFINED || cache[1].u.float64 != 1.0) {
        *out = LR_VALUE_UNDEFINED;
        return;
    }
    /* args_base layout: [this, arg1..argN] (same as MIR_OP_call_method) */
    uint32_t argc = (uint32_t)cache[2].u.float64;
    if (argc > 8) { *out = LR_VALUE_UNDEFINED; return; }
    LRValue argv[8];
    for (uint32_t i = 0; i < argc && (i + 1) < 9; i++) {
        argv[i] = args_base[i + 1];
    }
    *out = interp_bc_call(interp, callee, args_base[0], (int)argc, argv);
}

/* Private field get: (result) = obj.#field
 * ABI (same as get_prop): R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(base*)
 *   base[0] = obj, base[1] = result */
void lr_jit_rt_private_field_get(Interpreter *interp, void *prog_raw,
    uint32_t name_idx, LRValue *args_base) {
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count
        || !args_base || !ctx) {
        if (args_base) args_base[1] = LR_VALUE_UNDEFINED;
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name || name[0] != '#') { args_base[1] = LR_VALUE_UNDEFINED; return; }
    args_base[1] = lr_get_property_str(ctx, args_base[0], name);
}

/* Private field set: obj.#field = val
 * ABI (same as set_prop): R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(base*)
 *   base[0] = obj, base[1] = val, base[2] = result */
void lr_jit_rt_private_field_set(Interpreter *interp, void *prog_raw,
    uint32_t name_idx, LRValue *args_base) {
    BCProgram *prog = (BCProgram *)prog_raw;
    LRContext *ctx = jit_interp_ctx(interp);
    if (!interp || !prog || name_idx >= (uint32_t)prog->pool_count
        || !args_base || !ctx) {
        if (args_base) args_base[2] = LR_VALUE_UNDEFINED;
        return;
    }
    const char *name = prog->pool[name_idx].u.str;
    if (!name || name[0] != '#') return;
    lr_set_property_str(ctx, args_base[0], name, args_base[1]);
    args_base[2] = args_base[1];
}

/* ── Multi-Process Compilation (分身) Implementation ─────────────────── */

/* Worker thread data for JIT compilation */
typedef struct LR_JITWorkerData {
    int              worker_id;
    LRJITRuntime   *jit;
    int              completed_tasks;
    pthread_t        thread;
} LR_JITWorkerData;

/* Global compilation queue and worker pool */
static LR_JITCompileTask *g_compile_queue = NULL;
static int g_compile_queue_size = 0;
static int g_compile_queue_capacity = 0;
static int g_compile_queue_head = 0;
static int g_compile_queue_tail = 0;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond = PTHREAD_COND_INITIALIZER;
static volatile int     g_workers_running = 0;
static LR_ThreadPool   *g_jit_thread_pool = NULL;
static LR_JITWorkerData *g_worker_data = NULL;
static int              g_num_workers = 0;

/* Get next task from circular queue */
static LR_JITCompileTask *lr_jit_queue_pop(void) {
    if (g_compile_queue_size <= 0) return NULL;
    
    LR_JITCompileTask *task = &g_compile_queue[g_compile_queue_head];
    g_compile_queue_head = (g_compile_queue_head + 1) % g_compile_queue_capacity;
    g_compile_queue_size--;
    
    return task;
}

/* Add task to circular queue */
static int lr_jit_queue_push(LR_JITCompileTask *task) {
    if (g_compile_queue_size >= g_compile_queue_capacity) return -1;
    
    g_compile_queue[g_compile_queue_tail] = *task;
    g_compile_queue_tail = (g_compile_queue_tail + 1) % g_compile_queue_capacity;
    g_compile_queue_size++;
    
    return 0;
}

/* Worker thread function for JIT compilation */
static void *lr_jit_worker_thread(void *arg) {
    LR_JITWorkerData *wd = (LR_JITWorkerData *)arg;
    
    while (g_workers_running) {
        LR_JITCompileTask *task = NULL;
        
        /* Try to get a task from the queue */
        pthread_mutex_lock(&g_queue_mutex);
        if (g_compile_queue_size > 0) {
            task = lr_jit_queue_pop();
        }
        pthread_mutex_unlock(&g_queue_mutex);
        
        if (task) {
            /* Mark task as running */
            task->task_status = 1;
            
            /* Compile the program */
            if (task->is_hot_loop) {
                task->result_entry = lr_jit_compile_hot_loop(task->interp, task->prog);
            } else {
                task->result_entry = lr_jit_compile(task->interp, task->prog);
            }
            
            /* Mark task as done */
            if (task->result_entry) {
                task->task_status = 2;
                wd->completed_tasks++;
                LR_JIT_DBG("[Worker %d] Compiled program %p (hot_loop=%d)\n", 
                          wd->worker_id, (void*)task->prog, task->is_hot_loop);
            } else {
                task->task_status = -1;
                LR_JIT_ERR("[Worker %d] Failed to compile program %p\n", 
                          wd->worker_id, (void*)task->prog);
            }
        } else {
            /* No tasks available, wait for new work or shutdown */
#ifdef _WIN32
            Sleep(10); /* 10ms sleep on Windows */
#else
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10000000; /* 10ms timeout */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            
            pthread_mutex_lock(&g_queue_mutex);
            pthread_cond_timedwait(&g_queue_cond, &g_queue_mutex, &ts);
            pthread_mutex_unlock(&g_queue_mutex);
#endif
        }
    }
    
    return NULL;
}

/* Initialize multi-process compilation system */
void lr_jit_worker_init(int num_workers) {
    if (num_workers <= 0) num_workers = 4; /* Default to 4 workers */
    
    g_num_workers = num_workers;
    
    /* Create thread pool */
    g_jit_thread_pool = lr_thread_pool_create(num_workers);
    if (!g_jit_thread_pool) {
        LR_JIT_ERR("[JIT] Failed to create thread pool\n");
        return;
    }
    
    /* Allocate queue */
    g_compile_queue_capacity = num_workers * 16;
    g_compile_queue = (LR_JITCompileTask *)calloc(g_compile_queue_capacity, sizeof(LR_JITCompileTask));
    if (!g_compile_queue) {
        LR_JIT_ERR("[JIT] Failed to allocate compilation queue\n");
        lr_thread_pool_destroy(g_jit_thread_pool);
        g_jit_thread_pool = NULL;
        return;
    }
    g_compile_queue_size = 0;
    g_compile_queue_head = 0;
    g_compile_queue_tail = 0;
    
    /* Allocate worker data */
    g_worker_data = (LR_JITWorkerData *)calloc(num_workers, sizeof(LR_JITWorkerData));
    if (!g_worker_data) {
        LR_JIT_ERR("[JIT] Failed to allocate worker data\n");
        free(g_compile_queue);
        g_compile_queue = NULL;
        lr_thread_pool_destroy(g_jit_thread_pool);
        g_jit_thread_pool = NULL;
        return;
    }
    
    /* Start worker threads */
    g_workers_running = 1;
    for (int i = 0; i < num_workers; i++) {
        g_worker_data[i].worker_id = i;
        g_worker_data[i].completed_tasks = 0;
        
        int rc = pthread_create(&g_worker_data[i].thread, NULL, lr_jit_worker_thread, &g_worker_data[i]);
        if (rc != 0) {
            LR_JIT_ERR("[JIT] Failed to create worker thread %d\n", i);
            g_workers_running = 0;
            return;
        }
    }
    
    LR_JIT_DBG("[JIT] Initialized %d worker threads for JIT compilation\n", num_workers);
}

/* Shutdown multi-process compilation system */
void lr_jit_worker_shutdown(void) {
    if (!g_workers_running) return;
    
    g_workers_running = 0;
    
    /* Wake up all waiting workers */
    pthread_cond_broadcast(&g_queue_cond);
    
    /* Wait for workers to finish */
    if (g_worker_data) {
        for (int i = 0; i < g_num_workers; i++) {
            pthread_join(g_worker_data[i].thread, NULL);
        }
    }
    
    /* Clean up */
    if (g_compile_queue) {
        free(g_compile_queue);
        g_compile_queue = NULL;
    }
    if (g_worker_data) {
        free(g_worker_data);
        g_worker_data = NULL;
    }
    if (g_jit_thread_pool) {
        lr_thread_pool_destroy(g_jit_thread_pool);
        g_jit_thread_pool = NULL;
    }
    
    pthread_mutex_destroy(&g_queue_mutex);
    pthread_cond_destroy(&g_queue_cond);
    
    LR_JIT_DBG("[JIT] Shutdown multi-process compilation system\n");
}

/* Submit a JIT compilation task to worker pool */
int lr_jit_submit_compile_task(LR_JITCompileTask *task) {
    if (!task || !g_workers_running || !g_compile_queue) return -1;
    
    pthread_mutex_lock(&g_queue_mutex);
    
    /* Wait for space in queue */
    while (g_compile_queue_size >= g_compile_queue_capacity && g_workers_running) {
        pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
    }
    
    if (!g_workers_running) {
        pthread_mutex_unlock(&g_queue_mutex);
        return -1;
    }
    
    /* Add task to queue */
    if (lr_jit_queue_push(task) == 0) {
        task->task_status = 0; /* Pending */
        pthread_cond_signal(&g_queue_cond);
    }
    
    pthread_mutex_unlock(&g_queue_mutex);
    
    return 0;
}

/* Wait for all submitted compilation tasks to complete */
void lr_jit_wait_for_compilations(void) {
    if (!g_workers_running) return;
    
    /* Poll until queue is empty */
    while (1) {
        pthread_mutex_lock(&g_queue_mutex);
        int pending = g_compile_queue_size;
        pthread_mutex_unlock(&g_queue_mutex);
        
        if (pending == 0) break;
        
#ifdef _WIN32
        Sleep(1); /* 1ms sleep on Windows */
#else
        usleep(1000); /* 1ms sleep on POSIX */
#endif
    }
}

/* Get worker pool stats */
void lr_jit_worker_get_stats(int *pending_tasks, int *completed_tasks) {
    if (!pending_tasks && !completed_tasks) return;
    
    int pending = 0;
    int completed = 0;
    
    pthread_mutex_lock(&g_queue_mutex);
    pending = g_compile_queue_size;
    pthread_mutex_unlock(&g_queue_mutex);
    
    if (g_worker_data) {
        for (int i = 0; i < g_num_workers; i++) {
            completed += g_worker_data[i].completed_tasks;
        }
    }
    
    if (pending_tasks) *pending_tasks = pending;
    if (completed_tasks) *completed_tasks = completed;
}

