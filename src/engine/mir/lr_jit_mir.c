/* * LR_JS - JIT Frontend: Bytecode -> MIR * * Phase 1: Stack-based bytecode compilation to compact MIR (SSA-like). * Phase 2 (inline): Simple register allocation / peephole optimization. * * This file replaces lr_jit_v2.c's frontend (BytecodeToIR) with a * cleaner MIR representation and fixes the local variable access bug. */
#include "lr_jit_mir.h"
#include "lr_interp.h"
#include "lr_ast.h"
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static void mir_log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* MIR temporaries live above this base so they never collide with bytecode
 * local slots (0..MIR_VAR_BASE-1) inside the shared JIT local array. */
#define MIR_VAR_BASE 64

/* ══════════════════════════════════════════════════════════════════════ *
 * MIR Program Management *
 * ══════════════════════════════════════════════════════════════════════ */
MIRProgram *mir_create(int max_vars) {
    MIRProgram *mir = (MIRProgram *)calloc(1, sizeof(MIRProgram));
    if (!mir) return NULL;
    mir->capacity = max_vars * 4;
    mir->instructions = (MIRInstr *)calloc(mir->capacity, sizeof(MIRInstr));
    mir->const_values = (int *)calloc(max_vars, sizeof(int));
    mir->const_f64_values = (double *)calloc(max_vars, sizeof(double));
    if (!mir->instructions || !mir->const_values || !mir->const_f64_values) {
        free(mir->instructions);
        free(mir->const_values);
        free(mir->const_f64_values);
        free(mir);
        return NULL;
    }
    mir->bc_offsets = (int *)calloc(mir->capacity, sizeof(int));
    if (!mir->bc_offsets) {
        free(mir->instructions);
        free(mir->const_values);
        free(mir->const_f64_values);
        free(mir);
        return NULL;
    }
    mir->num_instructions = 0;
    mir->num_vars = 0;
    return mir;
}

/* ══════════════════════════════════════════════════════════════════════
 * MIR serialization (cross-platform IOME586 cache)
 *
 * Format (all little-endian):
 *   [0..3]   magic = 0x4D495231 ('MIR1')
 *   [4..7]   nparams (uint32, from BCProgram->nparams)
 *   [8..11]  num_instructions (uint32)
 *   [12..15] num_vars (uint32)
 *   per instr (21 bytes): opcode(u8) + dst(i32) + src1(i32) + src2(i32) + imm(i64)
 *   bc_offsets (4*N bytes, i32 each, -1 when unknown)
 *
 * Total size = 16 + 25*N bytes.
 * The ptr field is NOT serialized: it is write-only in codegen_emit and
 * always supplied via the live BCProgram pointer at runtime.
 * ══════════════════════════════════════════════════════════════════════ */

#define MIR_CACHE_MAGIC 0x4D495231u  /* "MIR1" */

uint8_t *mir_serialize(MIRProgram *mir, void *prog, size_t *out_len) {
    if (!mir || !out_len) return NULL;
    if (mir->num_instructions <= 0) return NULL;

    /* Derive nparams from BCProgram; fall back to 0 when prog is NULL. */
    int nparams = 0;
    if (prog) {
        uint8_t np = ((uint8_t *)prog)[209]; /* BCProgram->nparams offset */
        nparams = (int)np;
    }

    int N = mir->num_instructions;
    size_t buf_size = 16u + (size_t)N * 25u;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (!buf) return NULL;

    /* ── Header (16 bytes) ── */
    uint32_t magic = MIR_CACHE_MAGIC;
    memcpy(buf + 0, &magic, 4);
    memcpy(buf + 4, &nparams, 4);
    memcpy(buf + 8, &N, 4);
    memcpy(buf + 12, &mir->num_vars, 4);

    /* ── Instructions (21 bytes each) ── */
    for (int i = 0; i < N; i++) {
        const MIRInstr *instr = &mir->instructions[i];
        size_t off = 16u + (size_t)i * 21u;
        uint8_t op = (uint8_t)instr->opcode;
        int32_t d = (int32_t)instr->dst;
        int32_t s1 = (int32_t)instr->src1;
        int32_t s2 = (int32_t)instr->src2;
        int64_t imm = instr->imm;
        memcpy(buf + off, &op, 1);
        memcpy(buf + off + 1, &d, 4);
        memcpy(buf + off + 5, &s1, 4);
        memcpy(buf + off + 9, &s2, 4);
        memcpy(buf + off + 13, &imm, 8);
    }

    /* ── bc_offsets (4 bytes each) ── */
    for (int i = 0; i < N; i++) {
        int32_t off_val = (mir->bc_offsets && i < mir->capacity)
                              ? mir->bc_offsets[i] : -1;
        memcpy(buf + 16u + (size_t)N * 21u + (size_t)i * 4u, &off_val, 4);
    }

    *out_len = buf_size;
    return buf;
}

MIRProgram *mir_deserialize(const uint8_t *data, size_t len, int *out_nparams) {
    if (!data || len < 16) return NULL;

    uint32_t magic = 0;
    memcpy(&magic, data, 4);
    if (magic != MIR_CACHE_MAGIC) return NULL;

    int nparams = 0, N = 0, num_vars = 0;
    memcpy(&nparams, data + 4, 4);
    memcpy(&N, data + 8, 4);
    memcpy(&num_vars, data + 12, 4);

    if (N <= 0 || N > 1000000) return NULL;
    size_t expected = 16u + (size_t)N * 25u;
    if (len != expected) return NULL;

    MIRProgram *mir = mir_create(num_vars > 0 ? num_vars : N);
    if (!mir) return NULL;
    mir->num_vars = num_vars;
    mir->num_instructions = N;
    mir->capacity = N;

    for (int i = 0; i < N; i++) {
        MIRInstr *instr = &mir->instructions[i];
        size_t off = 16u + (size_t)i * 21u;
        uint8_t op = 0;
        int32_t d = 0, s1 = 0, s2 = 0;
        int64_t imm = 0;
        memcpy(&op, data + off, 1);
        memcpy(&d, data + off + 1, 4);
        memcpy(&s1, data + off + 5, 4);
        memcpy(&s2, data + off + 9, 4);
        memcpy(&imm, data + off + 13, 8);
        instr->opcode = (MIROpcode)op;
        instr->dst = d;
        instr->src1 = s1;
        instr->src2 = s2;
        instr->imm = imm;
        instr->ptr = NULL;
        /* bc_offset */
        int32_t bc_off = -1;
        memcpy(&bc_off, data + 16u + (size_t)N * 21u + (size_t)i * 4u, 4);
        if (mir->bc_offsets) mir->bc_offsets[i] = bc_off;
    }

    if (out_nparams) *out_nparams = nparams;
    return mir;
}

void mir_free(MIRProgram *mir) {
    if (mir) {
        free(mir->instructions);
        free(mir->const_values);
        free(mir->const_f64_values);
        free(mir->bc_offsets);
        free(mir);
    }
}

int mir_add_instr(MIRProgram *mir, MIROpcode op, int dst, int src1, int src2,
                  int64_t imm, void *ptr) {
    if (mir->num_instructions >= mir->capacity) {
        int new_cap = mir->capacity * 2;
        MIRInstr *new_instrs = (MIRInstr *)realloc(mir->instructions,
                                                    new_cap * sizeof(MIRInstr));
        if (!new_instrs) return -1;
        int *new_offsets = (int *)realloc(mir->bc_offsets,
                                          new_cap * sizeof(int));
        if (!new_offsets) {
            free(new_instrs);
            return -1;
        }
        mir->instructions = new_instrs;
        mir->bc_offsets = new_offsets;
        mir->capacity = new_cap;
    }
    MIRInstr *instr = &mir->instructions[mir->num_instructions++];
    instr->opcode = op;
    instr->dst = dst;
    instr->src1 = src1;
    instr->src2 = src2;
    instr->imm = imm;
    instr->ptr = ptr;
    return mir->num_instructions - 1;
}

int mir_new_var(MIRProgram *mir) {
    if (mir->num_vars >= 8192) return -1;
    int v = mir->num_vars++;
    if (v < mir->capacity)
        mir->const_values[v] = 0x7FFFFFFF;
    return v;
}

void mir_analyze_liveness(MIRProgram *mir) {
    (void)mir;
}

/* ══════════════════════════════════════════════════════════════════════ *
 * MIR Optimizations (V8-inspired speculative optimization) *
 * Apply lightweight optimizations only to functions that will be JIT'd. *
 * ══════════════════════════════════════════════════════════════════════ */

/* Constant propagation: replace var with constant if known */
static void mir_optimize_constants(MIRProgram *mir) {
    if (!mir) return;
    /* Invalidate const_values for any var that is a source operand of
     * MIR_OP_move (the constant may have been copied to another slot,
     * and the destination's value depends on the runtime state, not the
     * compile-time constant).  Without this, moves that forward-declare
     * arguments for BC_CALL would leave stale constant values that
     * subsequent arithmetic folds against wrong data. */
    for (int i = 0; i < mir->num_instructions; i++) {
        MIRInstr *instr = &mir->instructions[i];
        if (instr->opcode == MIR_OP_move && instr->src1 >= MIR_VAR_BASE)
            mir->const_values[instr->src1 - MIR_VAR_BASE] = 0x7FFFFFFF;
    }
    for (int i = 0; i < mir->num_instructions; i++) {
        MIRInstr *instr = &mir->instructions[i];
        switch (instr->opcode) {
            case MIR_OP_add_i32:
            case MIR_OP_sub_i32:
            case MIR_OP_mul_i32:
            case MIR_OP_div_i32:
                /* If both operands are constants, fold at compile time */
                if (instr->src1 >= MIR_VAR_BASE && instr->src2 >= MIR_VAR_BASE &&
                    mir->const_values[instr->src1 - MIR_VAR_BASE] != 0x7FFFFFFF &&
                    mir->const_values[instr->src2 - MIR_VAR_BASE] != 0x7FFFFFFF) {
                    int32_t a = mir->const_values[instr->src1 - MIR_VAR_BASE];
                    int32_t b = mir->const_values[instr->src2 - MIR_VAR_BASE];
                    int32_t result;
                    switch (instr->opcode) {
                        case MIR_OP_add_i32: result = a + b; break;
                        case MIR_OP_sub_i32: result = a - b; break;
                        case MIR_OP_mul_i32: result = a * b; break;
                        case MIR_OP_div_i32: result = (b != 0) ? a / b : 0; break;
                        default: continue;
                    }
                    if (instr->dst >= MIR_VAR_BASE)
                        mir->const_values[instr->dst - MIR_VAR_BASE] = result;
                    /* Replace with constant load */
                    instr->opcode = MIR_OP_const_i32;
                    instr->src1 = -1;
                    instr->src2 = -1;
                    instr->imm = result;
                }
                break;
            default:
                break;
        }
    }

    /* ── f64 constant folding ─────────────────────────────────────── */
    /* Temporarily disabled for debugging crash in test_mir_all.js */
    /*
    #define F64_UNK(v) isnan(mir->const_f64_values[(v) - MIR_VAR_BASE])

    for (int i = 0; i < mir->num_instructions; i++) {
        MIRInstr *instr = &mir->instructions[i];
        switch (instr->opcode) {
            case MIR_OP_const_f64: {
                if (instr->dst >= MIR_VAR_BASE && instr->ptr) {
                    BCProgram *prog = (BCProgram *)instr->ptr;
                    if (instr->imm >= 0 && (size_t)instr->imm < (size_t)prog->pool_count) {
                        BCConst *c = &prog->pool[(size_t)instr->imm];
                        if (c->kind == BC_POOL_FLOAT64) {
                            mir->const_f64_values[instr->dst - MIR_VAR_BASE] = c->u.f64;
                        }
                    }
                }
                break;
            }
            case MIR_OP_add_f64:
            case MIR_OP_sub_f64:
            case MIR_OP_mul_f64:
            case MIR_OP_div_f64: {
                if (instr->src1 >= MIR_VAR_BASE && instr->src2 >= MIR_VAR_BASE &&
                    !F64_UNK(instr->src1) && !F64_UNK(instr->src2)) {
                    double a = mir->const_f64_values[instr->src1 - MIR_VAR_BASE];
                    double b = mir->const_f64_values[instr->src2 - MIR_VAR_BASE];
                    double result;
                    int valid = 1;
                    switch (instr->opcode) {
                        case MIR_OP_add_f64:  result = a + b; break;
                        case MIR_OP_sub_f64:  result = a - b; break;
                        case MIR_OP_mul_f64:  result = a * b; break;
                        case MIR_OP_div_f64:
                            if (b == 0.0) { result = 0.0; valid = 0; }
                            else result = a / b;
                            break;
                        default: continue;
                    }
                    if (valid && instr->dst >= MIR_VAR_BASE)
                        mir->const_f64_values[instr->dst - MIR_VAR_BASE] = result;
                    if (valid) {
                        instr->opcode = MIR_OP_const_f64;
                        instr->src1 = -1;
                        instr->src2 = -1;
                    }
                }
                break;
            }
            case MIR_OP_lt_f64:
            case MIR_OP_gt_f64:
            case MIR_OP_le_f64:
            case MIR_OP_ge_f64:
            case MIR_OP_eq_f64:
            case MIR_OP_ne_f64: {
                if (instr->src1 >= MIR_VAR_BASE && instr->src2 >= MIR_VAR_BASE &&
                    !F64_UNK(instr->src1) && !F64_UNK(instr->src2)) {
                    double a = mir->const_f64_values[instr->src1 - MIR_VAR_BASE];
                    double b = mir->const_f64_values[instr->src2 - MIR_VAR_BASE];
                    int result = 0;
                    int a_nan = isnan(a);
                    int b_nan = isnan(b);
                    if (a_nan || b_nan) {
                        result = (instr->opcode == MIR_OP_ne_f64) ? 1 : 0;
                    } else {
                        switch (instr->opcode) {
                            case MIR_OP_lt_f64: result = (a < b) ? 1 : 0; break;
                            case MIR_OP_gt_f64: result = (a > b) ? 1 : 0; break;
                            case MIR_OP_le_f64: result = (a <= b) ? 1 : 0; break;
                            case MIR_OP_ge_f64: result = (a >= b) ? 1 : 0; break;
                            case MIR_OP_eq_f64: result = (a == b) ? 1 : 0; break;
                            case MIR_OP_ne_f64: result = (a != b) ? 1 : 0; break;
                            default: continue;
                        }
                    }
                    if (instr->dst >= MIR_VAR_BASE)
                        mir->const_values[instr->dst - MIR_VAR_BASE] = result;
                    instr->opcode = MIR_OP_const_i32;
                    instr->src1 = -1;
                    instr->src2 = -1;
                    instr->imm = result;
                }
                break;
            }
            default:
                break;
        }
    }

    #undef F64_UNK
    */
}

static const char *mir_opcode_name(MIROpcode op) {
    switch (op) {
        case MIR_OP_const_i32: return "const_i32";
        case MIR_OP_load_local: return "load_local";
        case MIR_OP_store_local: return "store_local";
        case MIR_OP_not_i32: return "not_i32";
        case MIR_OP_neg_i32: return "neg_i32";
        case MIR_OP_add_i32: return "add_i32";
        case MIR_OP_sub_i32: return "sub_i32";
        case MIR_OP_mul_i32: return "mul_i32";
        case MIR_OP_div_i32: return "div_i32";
        case MIR_OP_mod_i32: return "mod_i32";
        case MIR_OP_and_i32: return "and_i32";
        case MIR_OP_or_i32: return "or_i32";
        case MIR_OP_xor_i32: return "xor_i32";
        case MIR_OP_shl_i32: return "shl_i32";
        case MIR_OP_shr_i32: return "shr_i32";
        case MIR_OP_sar_i32: return "sar_i32";
        case MIR_OP_add_f64: return "add_f64";
        case MIR_OP_sub_f64: return "sub_f64";
        case MIR_OP_mul_f64: return "mul_f64";
        case MIR_OP_div_f64: return "div_f64";
        case MIR_OP_mod_f64: return "mod_f64";
        case MIR_OP_lt_i32: return "lt_i32";
        case MIR_OP_gt_i32: return "gt_i32";
        case MIR_OP_le_i32: return "le_i32";
        case MIR_OP_ge_i32: return "ge_i32";
        case MIR_OP_eq_i32: return "eq_i32";
        case MIR_OP_ne_i32: return "ne_i32";
        case MIR_OP_lt_f64: return "lt_f64";
        case MIR_OP_gt_f64: return "gt_f64";
        case MIR_OP_le_f64: return "le_f64";
        case MIR_OP_ge_f64: return "ge_f64";
        case MIR_OP_eq_f64: return "eq_f64";
        case MIR_OP_ne_f64: return "ne_f64";
        case MIR_OP_jump: return "jump";
        case MIR_OP_jump_if_false: return "jump_if_false";
        case MIR_OP_jump_if_true: return "jump_if_true";
        case MIR_OP_loop_start: return "loop_start";
        case MIR_OP_loop_end:   return "loop_end";
        case MIR_OP_loop_guard: return "loop_guard";
        case MIR_OP_call: return "call";
        case MIR_OP_ret: return "ret";
    case MIR_OP_bailout: return "bailout";
    case MIR_OP_load_var: return "load_var";
    case MIR_OP_runtime_call: return "rt_call";
    case MIR_OP_move: return "move";
    case MIR_OP_push_undefined: return "push_undef";
        case MIR_OP_push_null: return "push_null";
        case MIR_OP_push_true: return "push_true";
        case MIR_OP_push_false: return "push_false";
        case MIR_OP_push_int32: return "push_int32";
        case MIR_OP_pop: return "pop";
        case MIR_OP_dup: return "dup";
        case MIR_OP_swap: return "swap";
        /* Extended core MIR instructions */
        case MIR_OP_const_f64: return "const_f64";
        case MIR_OP_const_string: return "const_str";
        case MIR_OP_load_this: return "load_this";
        case MIR_OP_store_var: return "store_var";
        case MIR_OP_inc_var: return "inc_var";
        case MIR_OP_typeof: return "typeof";
        case MIR_OP_typeof_var: return "typeof_var";
        case MIR_OP_to_string: return "to_string";
        case MIR_OP_to_number: return "to_number";
        case MIR_OP_to_bool: return "to_bool";
        case MIR_OP_pos: return "pos";
        case MIR_OP_pow: return "pow";
        case MIR_OP_new_object: return "new_obj";
        case MIR_OP_new_array: return "new_arr";
        case MIR_OP_get_prop: return "get_prop";
        case MIR_OP_get_prop_cached: return "get_prop_cache";
        case MIR_OP_load_prop: return "load_prop";
        case MIR_OP_set_prop: return "set_prop";
        case MIR_OP_get_elem: return "get_elem";
        case MIR_OP_set_elem: return "set_elem";
        case MIR_OP_add_prop: return "add_prop";
        case MIR_OP_in: return "in";
        case MIR_OP_instanceof: return "instof";
        case MIR_OP_throw: return "throw";
        case MIR_OP_jump_if_not_nullish: return "jn_nullish";
        case MIR_OP_call_method: return "call_m";
        case MIR_OP_call_elem: return "call_e";
        case MIR_OP_construct: return "new";
        case MIR_OP_atomics_load: return "atomics_load";
        case MIR_OP_atomics_store: return "atomics_store";
        case MIR_OP_atomics_add: return "atomics_add";
        case MIR_OP_atomics_sub: return "atomics_sub";
        case MIR_OP_atomics_and: return "atomics_and";
        case MIR_OP_atomics_or: return "atomics_or";
        case MIR_OP_atomics_xor: return "atomics_xor";
        case MIR_OP_atomics_exchange: return "atomics_xchg";
        case MIR_OP_atomics_compare_exchange: return "atomics_cexch";
        case MIR_OP_atomics_is_lock_free: return "atomics_lf";
        case MIR_OP_atomics_wait: return "atomics_wait";
        case MIR_OP_atomics_notify: return "atomics_notify";
        case MIR_OP_atomics_cache: return "atomics_cache";
        case MIR_OP_atomics_inline_load: return "atomics_iLoad";
        case MIR_OP_atomics_inline_store: return "atomics_iStore";
        case MIR_OP_atomics_inline_add: return "atomics_iAdd";
        case MIR_OP_atomics_inline_sub: return "atomics_iSub";
        case MIR_OP_atomics_inline_and: return "atomics_iAnd";
        case MIR_OP_atomics_inline_or: return "atomics_iOr";
        case MIR_OP_atomics_inline_xor: return "atomics_iXor";
        case MIR_OP_atomics_inline_exchange: return "atomics_iXchg";
        case MIR_OP_atomics_inline_compare_exchange: return "atomics_iCexch";
        case MIR_OP_method_cache: return "meth_cache";
        case MIR_OP_call_cached_method: return "call_cached";
        case MIR_OP_private_field_get: return "priv_get";
        case MIR_OP_private_field_set: return "priv_set";
        case MIR_OP_inline_call: return "inline_call";
        default: return "???";
    }
}

void mir_dump(MIRProgram *mir, const char *label) {
    if (lr_debug_jit_enabled()) {
        LR_JIT_DBG("[MIR-DUMP] %s: %d instructions\n", label, mir->num_instructions);
        for (int i = 0; i < mir->num_instructions; i++) {
            MIRInstr *inst = &mir->instructions[i];
            LR_JIT_DBG("  [%d] %s dst=%d src1=%d src2=%d imm=%lld bc_off=%d\n",
                    i, mir_opcode_name(inst->opcode),
                    inst->dst, inst->src1, inst->src2,
                    (long long)inst->imm,
                    (mir->bc_offsets ? mir->bc_offsets[i] : -1));
        }
    }
}

/* Conditional-merge optimization: for ternary expressions, two branches may
 * write to different temp vars (true_var, false_var).  The store_local after
 * the merge only reads false_var, losing the true-branch value.
 *
 * Detect the pattern:
 *   [i]   jump_if_false ...  (skip true branch)
 *   ... true branch ...
 *   [j]   jump ...           (skip false branch)
 *   ... false branch ...
 *   [k]   store_local dst=<same as at k'>
 *
 * Insert a move true_var → false_var just before the jump at [j], so both
 * branches write the same destination and the merge point reads the correct
 * value.  bc_offsets for inserted instructions is set to -1 (codegen falls
 * back to the nearest following bc offset). */
static void mir_optimize_conditional_merge(MIRProgram *mir) {
    if (!mir || mir->num_instructions < 4) return;

    int n = mir->num_instructions;
    for (int i = 0; i < n; i++) {
        if (mir->instructions[i].opcode != MIR_OP_jump_if_false)
            continue;

        /* Find the unconditional jump after the true branch. */
        int jump_idx = -1;
        for (int j = i + 1; j < n; j++) {
            MIROpcode op = mir->instructions[j].opcode;
            if (op == MIR_OP_jump && mir->instructions[j].imm > 0) {
                jump_idx = j;
                break;
            }
            if (op == MIR_OP_jump_if_false || op == MIR_OP_jump_if_true ||
                op == MIR_OP_jump_if_not_nullish || op == MIR_OP_ret ||
                op == MIR_OP_loop_start)
                break;
        }
        if (jump_idx < 0) continue;

        /* Find the store_local after the false branch. */
        int store_idx = -1;
        for (int j = jump_idx + 1; j < n && j < jump_idx + 20; j++) {
            if (mir->instructions[j].opcode == MIR_OP_store_local) {
                store_idx = j;
                break;
            }
            if (mir->instructions[j].opcode == MIR_OP_jump ||
                mir->instructions[j].opcode == MIR_OP_jump_if_false ||
                mir->instructions[j].opcode == MIR_OP_jump_if_true ||
                mir->instructions[j].opcode == MIR_OP_ret ||
                mir->instructions[j].opcode == MIR_OP_loop_start)
                break;
        }

        /* Find the true-branch result var: last instr with dst >= MIR_VAR_BASE
         * in the true branch (between jump_if_false and unconditional jump). */
        int true_var = -1;
        int true_branch_end = -1;
        for (int j = jump_idx - 1; j > i; j--) {
            int d = mir->instructions[j].dst;
            if (d >= MIR_VAR_BASE) {
                true_var = d;
                true_branch_end = j;
                break;
            }
        }
        if (true_var < 0) continue;

        /* Case 1: store_local after ternary (classic pattern) */
        if (store_idx >= 0) {
            int false_var = mir->instructions[store_idx].src1;
            if (false_var < 0 || true_var == false_var) continue;

            int insert_pos = jump_idx;
            int old_n = mir->num_instructions;
            if (old_n + 1 > mir->capacity) {
                int new_cap = mir->capacity * 2;
                MIRInstr *ni = (MIRInstr *)realloc(mir->instructions,
                                    (size_t)new_cap * sizeof(MIRInstr));
                int *no = (int *)realloc(mir->bc_offsets,
                                    (size_t)new_cap * sizeof(int));
                if (!ni || !no) { free(ni); free(no); return; }
                mir->instructions = ni;
                mir->bc_offsets = no;
                mir->capacity = new_cap;
            }
            memmove(&mir->instructions[insert_pos + 1],
                    &mir->instructions[insert_pos],
                    (size_t)(old_n - insert_pos) * sizeof(MIRInstr));
            memmove(&mir->bc_offsets[insert_pos + 1],
                    &mir->bc_offsets[insert_pos],
                    (size_t)(old_n - insert_pos) * sizeof(int));
            mir->instructions[insert_pos].opcode = MIR_OP_move;
            mir->instructions[insert_pos].dst = false_var;
            mir->instructions[insert_pos].src1 = true_var;
            mir->instructions[insert_pos].src2 = -1;
            mir->instructions[insert_pos].imm = 0;
            mir->instructions[insert_pos].ptr = NULL;
            mir->bc_offsets[insert_pos] = -1;
            mir->num_instructions = old_n + 1;

            if (true_var >= MIR_VAR_BASE &&
                true_var - MIR_VAR_BASE < 1024) {
                mir->const_values[false_var - MIR_VAR_BASE] =
                    mir->const_values[true_var - MIR_VAR_BASE];
            }

            LR_JIT_DBG("[MIR-OPT] conditional_merge(store): inserted move[%d] %d=%d before jump[%d]\n",
                    insert_pos, false_var, true_var, jump_idx);
            i = insert_pos;
            continue;
        }

        /* Case 2: ternary result used directly in next binary op (no store_local).
         *
         * Bytecode pattern for `left + (cond ? true_val : false_val)`:
         *   load_local left
         *   load_local cond
         *   jump_if_false → alternate
         *   const_i32 true_val        ← true branch result (true_var)
         *   jump → merge              (unconditional, skips false branch)
         *   const_i32 false_val       ← false branch result (false_var)
         *   ...binary ops using false_var...
         *
         * Strategy: insert `move false_var = true_var` before the unconditional
         * jump, so that on the true path false_var is also set. Then rewrite all
         * uses of false_var to use true_var. This ensures both branches produce
         * the same canonical variable without requiring dst rewriting. */
        /* Find the unconditional jump after the true branch */
        int jump_to_skip = -1;
        for (int j = true_branch_end + 1; j < n; j++) {
            if (mir->instructions[j].opcode == MIR_OP_jump) {
                jump_to_skip = j;
                break;
            }
        }
        /* Scan only the false branch (after the unconditional jump)
         * to find false_var. Stop at next control-flow instruction.
         * false_var is the FIRST variable produced in the false branch
         * (the branch value itself), not subsequent merge operations. */
        int false_var = -1;
        int false_branch_end = -1;
        int scan_start = jump_to_skip >= 0 ? jump_to_skip + 1 : true_branch_end + 1;
        for (int j = scan_start; j < n; j++) {
            MIRInstr *instr = &mir->instructions[j];
            MIROpcode op = instr->opcode;
            if (op == MIR_OP_jump || op == MIR_OP_ret ||
                op == MIR_OP_jump_if_false || op == MIR_OP_jump_if_true ||
                op == MIR_OP_jump_if_not_nullish)
                break;
            int d = instr->dst;
            if (d >= MIR_VAR_BASE && false_var < 0) {
                false_var = d;
                false_branch_end = j;
            }
        }
        if (false_var < 0 || true_var == false_var) continue;

        /* Find first instruction that uses false_var after false branch */
        int use_idx = -1;
        for (int j = false_branch_end + 1; j < n; j++) {
            MIRInstr *uj = &mir->instructions[j];
            if (uj->src1 == false_var || uj->src2 == false_var) {
                use_idx = j;
                break;
            }
            if (uj->opcode == MIR_OP_jump || uj->opcode == MIR_OP_ret ||
                uj->opcode == MIR_OP_jump_if_false ||
                uj->opcode == MIR_OP_jump_if_true ||
                uj->opcode == MIR_OP_jump_if_not_nullish)
                break;
        }
        if (use_idx < 0) continue;

        /* Insert move true_var → false_var before the unconditional jump,
         * so both branches define false_var (the canonical merge variable).
         * Then rewrite all uses of false_var. */
        int insert_pos = jump_to_skip;
        int old_n = mir->num_instructions;
        if (old_n + 1 > mir->capacity) {
            int new_cap = mir->capacity * 2;
            MIRInstr *ni = (MIRInstr *)realloc(mir->instructions,
                                (size_t)new_cap * sizeof(MIRInstr));
            int *no = (int *)realloc(mir->bc_offsets,
                                (size_t)new_cap * sizeof(int));
            if (!ni || !no) { free(ni); free(no); continue; }
            mir->instructions = ni;
            mir->bc_offsets = no;
            mir->capacity = new_cap;
        }
        memmove(&mir->instructions[insert_pos + 1],
                &mir->instructions[insert_pos],
                (size_t)(old_n - insert_pos) * sizeof(MIRInstr));
        memmove(&mir->bc_offsets[insert_pos + 1],
                &mir->bc_offsets[insert_pos],
                (size_t)(old_n - insert_pos) * sizeof(int));
        mir->instructions[insert_pos].opcode = MIR_OP_move;
        mir->instructions[insert_pos].dst = false_var;
        mir->instructions[insert_pos].src1 = true_var;
        mir->instructions[insert_pos].src2 = -1;
        mir->instructions[insert_pos].imm = 0;
        mir->instructions[insert_pos].ptr = NULL;
        mir->bc_offsets[insert_pos] = -1;
        mir->num_instructions = old_n + 1;

        /* false_var is now the canonical merge variable (both branches write to it
         * via the inserted move). No rewrite needed — consumers already use false_var. */

        /* Invalidate true_var's constant value: it depends on the runtime
         * condition, so we cannot treat it as a compile-time constant. */
        if (true_var >= MIR_VAR_BASE &&
            true_var - MIR_VAR_BASE < 1024) {
            mir->const_values[true_var - MIR_VAR_BASE] = 0x7FFFFFFF;
        }

        LR_JIT_DBG("[MIR-OPT] conditional_merge(binop): inserted move[%d] %d=%d before jump[%d]\n",
                insert_pos, false_var, true_var, jump_to_skip);
        continue;
    }
}

/* Dead code elimination: remove trailing instructions after the LAST return.
 *
 * CRITICAL: We must NOT stop at the FIRST return we encounter — loops
 * contain an early exit (e.g. break/continue or conditional return) followed
 * by a jump-back edge, then a final return.  Removing the jump-back turns
 * the compiled function into a single-pass, which breaks the interpreter's
 * hot-loop calling convention and causes ACCESS_VIOLATION.
 *
 * Strategy: two-pass scan.
 *   Pass 1: find the index of the last MIR_OP_ret.
 *   Pass 2: copy all instructions up to and including that last ret;
 *           silently drop any instructions after it.
 */
static void mir_eliminate_globally_dead(MIRProgram *mir);

static void mir_optimize_dead_code(MIRProgram *mir) {
    if (!mir) return;
    int num_before = mir->num_instructions;
    int last_ret = -1;
    /* Pass 1: locate the last return */
    for (int i = 0; i < num_before; i++) {
        if (mir->instructions[i].opcode == MIR_OP_ret)
            last_ret = i;
    }
    if (last_ret < 0) {
        /* No return found — keep everything (malformed but safer than truncating) */
        return;
    }
    /* Pass 2: truncate after the last return */
    memmove(&mir->instructions[0], &mir->instructions[0],
            (size_t)(last_ret + 1) * sizeof(MIRInstr));
    mir->num_instructions = last_ret + 1;

    /* Pass 3: remove globally-dead pure instructions (unused results). */
    mir_eliminate_globally_dead(mir);
}

/* True dead-code elimination.
 *
 * mir_optimize_dead_code() only truncates instructions after the last ret.
 * It does NOT remove an instruction whose result is never consumed, so a
 * `load_var Atomics` feeding an already-inlined atomics op (which reads the
 * typed-array/index args directly, not the Atomics object) stays in the loop
 * and calls the expensive scope-chain resolver on every iteration.
 *
 * This pass removes an instruction ONLY when it is provably side-effect-free
 * (pure) AND its result slot is never read by ANY other instruction.  The
 * "never read anywhere" test is flow-insensitive (linear scan) and therefore
 * safe under loops: we only delete when the result is globally dead, never
 * merely dead on one path. */

static int mir_is_pure_no_side_effect(MIROpcode op) {
    switch (op) {
        case MIR_OP_load_var:
        case MIR_OP_load_local:
        case MIR_OP_const_i32:
        case MIR_OP_const_f64:
        case MIR_OP_const_string:
        case MIR_OP_load_this:
        case MIR_OP_move:
        case MIR_OP_not_i32:
        case MIR_OP_neg_i32:
        case MIR_OP_neg_f64:
        case MIR_OP_add_i32:
        case MIR_OP_sub_i32:
        case MIR_OP_mul_i32:
        case MIR_OP_div_i32:
        case MIR_OP_mod_i32:
        case MIR_OP_and_i32:
        case MIR_OP_or_i32:
        case MIR_OP_xor_i32:
        case MIR_OP_shl_i32:
        case MIR_OP_shr_i32:
        case MIR_OP_sar_i32:
        case MIR_OP_add_f64:
        case MIR_OP_sub_f64:
        case MIR_OP_mul_f64:
        case MIR_OP_div_f64:
        case MIR_OP_mod_f64:
        case MIR_OP_lt_i32:
        case MIR_OP_gt_i32:
        case MIR_OP_le_i32:
        case MIR_OP_ge_i32:
        case MIR_OP_eq_i32:
        case MIR_OP_ne_i32:
        case MIR_OP_lt_f64:
        case MIR_OP_gt_f64:
        case MIR_OP_le_f64:
        case MIR_OP_ge_f64:
        case MIR_OP_eq_f64:
        case MIR_OP_ne_f64:
        case MIR_OP_add_prop:
            return 1;
        default:
            return 0;
    }
}

/* Mark every slot that instruction `t` may READ into `used`.
 * src1/src2 are always reads.  Only the variadic families below read a
 * contiguous run that starts at src1 and whose length is carried in `imm`
 * (argc or element count).  We deliberately do NOT treat `imm` as a count for
 * every opcode: jump/jump_if_* carry a byte offset and const/push ops carry a
 * value, so doing so would falsely mark the whole loop as live (an the offset
 * can be as large as the function, e.g. imm=29 marks slots 85..116). */
static int mir_op_is_variadic_read(MIROpcode op) {
    switch (op) {
        case MIR_OP_runtime_call:
        case MIR_OP_scope_call:
        case MIR_OP_inline_call:
        case MIR_OP_call_method:
        case MIR_OP_call_elem:
        case MIR_OP_construct:
        case MIR_OP_new_array:
        case MIR_OP_atomics_load:
        case MIR_OP_atomics_store:
        case MIR_OP_atomics_add:
        case MIR_OP_atomics_sub:
        case MIR_OP_atomics_and:
        case MIR_OP_atomics_or:
        case MIR_OP_atomics_xor:
        case MIR_OP_atomics_exchange:
        case MIR_OP_atomics_compare_exchange:
        case MIR_OP_atomics_is_lock_free:
        case MIR_OP_atomics_wait:
        case MIR_OP_atomics_notify:
        case MIR_OP_atomics_cache:
            return 1;
        default:
            return 0;
    }
}

/* Atomics cache optimization: when consecutive Atomics ops use the same
 * TypedArray local, insert a cache instr before the first one so the
 * codegen can skip repeated type checks on the buffer object.
 *
 * Strategy: scan for runs of contiguous MIR_OP_atomics_* (non-inline) that
 * share the same src1 (the args_base variable whose slot 0 is the TypedArray).
 * For runs of 2+ with the same src1, replace the first with
 * MIR_OP_atomics_cache and subsequent ones with the matching inline op. */
static void mir_optimize_atomics_cache(MIRProgram *mir) {
    if (!mir || mir->num_instructions < 2) return;
    int n = mir->num_instructions;

    for (int i = 0; i < n; ) {
        MIRInstr *cur = &mir->instructions[i];
        /* Check if this is a non-inline Atomics op */
        int is_atomics_std = 0;
        MIROpcode std_op = (MIROpcode)0;
        switch (cur->opcode) {
            case MIR_OP_atomics_load:           is_atomics_std = 1; std_op = MIR_OP_atomics_load;            break;
            case MIR_OP_atomics_store:          is_atomics_std = 1; std_op = MIR_OP_atomics_store;           break;
            case MIR_OP_atomics_add:            is_atomics_std = 1; std_op = MIR_OP_atomics_add;             break;
            case MIR_OP_atomics_sub:            is_atomics_std = 1; std_op = MIR_OP_atomics_sub;             break;
            case MIR_OP_atomics_and:            is_atomics_std = 1; std_op = MIR_OP_atomics_and;             break;
            case MIR_OP_atomics_or:             is_atomics_std = 1; std_op = MIR_OP_atomics_or;              break;
            case MIR_OP_atomics_xor:            is_atomics_std = 1; std_op = MIR_OP_atomics_xor;             break;
            case MIR_OP_atomics_exchange:       is_atomics_std = 1; std_op = MIR_OP_atomics_exchange;        break;
            case MIR_OP_atomics_compare_exchange: is_atomics_std = 1; std_op = MIR_OP_atomics_compare_exchange; break;
            default: break;
        }
        if (!is_atomics_std) { i++; continue; }

        /* Look ahead for consecutive same-TypedArray Atomics ops */
        int src1 = cur->src1;
        int j = i + 1;
        while (j < n) {
            MIRInstr *next = &mir->instructions[j];
            int next_is_atomics = 0;
            MIROpcode next_op = (MIROpcode)0;
            switch (next->opcode) {
                case MIR_OP_atomics_load:           next_is_atomics = 1; next_op = MIR_OP_atomics_load;            break;
                case MIR_OP_atomics_store:          next_is_atomics = 1; next_op = MIR_OP_atomics_store;           break;
                case MIR_OP_atomics_add:            next_is_atomics = 1; next_op = MIR_OP_atomics_add;             break;
                case MIR_OP_atomics_sub:            next_is_atomics = 1; next_op = MIR_OP_atomics_sub;             break;
                case MIR_OP_atomics_and:            next_is_atomics = 1; next_op = MIR_OP_atomics_and;             break;
                case MIR_OP_atomics_or:             next_is_atomics = 1; next_op = MIR_OP_atomics_or;              break;
                case MIR_OP_atomics_xor:            next_is_atomics = 1; next_op = MIR_OP_atomics_xor;             break;
                case MIR_OP_atomics_exchange:       next_is_atomics = 1; next_op = MIR_OP_atomics_exchange;        break;
                case MIR_OP_atomics_compare_exchange: next_is_atomics = 1; next_op = MIR_OP_atomics_compare_exchange; break;
                default: break;
            }
            if (!next_is_atomics || next->src1 != src1) break;
            j++;
        }

        if (j > i + 1) {
            /* We have a run of 2+ contiguous same-TypedArray Atomics ops.
             * Insert cache before the first, convert rest to inline. */
            int run_len = j - i;
            /* Allocate space: need 1 extra slot for cache + possible shift */
            if (mir->num_instructions + 1 >= mir->capacity) {
                int new_cap = mir->capacity * 2;
                MIRInstr *new_instrs = (MIRInstr *)realloc(mir->instructions, (size_t)new_cap * sizeof(MIRInstr));
                int *new_bc = (int *)realloc(mir->bc_offsets, (size_t)new_cap * sizeof(int));
                if (!new_instrs) return;
                mir->instructions = new_instrs;
                if (new_bc) mir->bc_offsets = new_bc;
                mir->capacity = new_cap;
            }
            /* Shift instructions[i+1..j] right by 1 */
            memmove(&mir->instructions[i + 1], &mir->instructions[i],
                    (size_t)(n - i) * sizeof(MIRInstr));
            if (mir->bc_offsets) {
                memmove(&mir->bc_offsets[i + 1], &mir->bc_offsets[i],
                        (size_t)(n - i) * sizeof(int));
            }
            mir->num_instructions++;

            /* Insert cache at position i+1: dst[0..3] = base, cache from src1 */
            MIRInstr *cache = &mir->instructions[i + 1];
            cache->opcode = MIR_OP_atomics_cache;
            cache->dst = src1;      /* cache slots: src1, src1+1, src1+2, src1+3 */
            cache->src1 = src1;     /* reads the TypedArray LRValue from slot 0 of args_base */
            cache->src2 = -1;
            cache->imm = 0;
            cache->ptr = NULL;

            /* Convert remaining ops to inline versions */
            for (int k = i + 2; k < j + 1; k++) {
                MIRInstr *inst = &mir->instructions[k];
                switch (inst->opcode) {
                    case MIR_OP_atomics_load:            inst->opcode = MIR_OP_atomics_inline_load;            break;
                    case MIR_OP_atomics_store:           inst->opcode = MIR_OP_atomics_inline_store;           break;
                    case MIR_OP_atomics_add:             inst->opcode = MIR_OP_atomics_inline_add;             break;
                    case MIR_OP_atomics_sub:             inst->opcode = MIR_OP_atomics_inline_sub;             break;
                    case MIR_OP_atomics_and:             inst->opcode = MIR_OP_atomics_inline_and;             break;
                    case MIR_OP_atomics_or:              inst->opcode = MIR_OP_atomics_inline_or;              break;
                    case MIR_OP_atomics_xor:             inst->opcode = MIR_OP_atomics_inline_xor;             break;
                    case MIR_OP_atomics_exchange:        inst->opcode = MIR_OP_atomics_inline_exchange;        break;
                    case MIR_OP_atomics_compare_exchange:inst->opcode = MIR_OP_atomics_inline_compare_exchange;break;
                    default: break;
                }
            }
            n = mir->num_instructions;
            i = j + 1;
        } else {
            i++;
        }
    }
}

/* Method cache optimization: when consecutive method calls use the same
 * receiver and same method name, insert a cache instr before the first one
 * so subsequent calls can skip property lookup.
 *
 * Strategy: scan for runs of contiguous MIR_OP_call_method that share the
 * same src1 (receiver base) and same imm (name_idx).
 * For runs of 2+, replace the first with MIR_OP_method_cache and subsequent
 * ones with MIR_OP_call_cached_method. */
static void mir_optimize_method_cache(MIRProgram *mir) {
    if (!mir || mir->num_instructions < 2) return;
    int n = mir->num_instructions;

    for (int i = 0; i < n; ) {
        MIRInstr *cur = &mir->instructions[i];
        if (cur->opcode != MIR_OP_call_method) { i++; continue; }

        /* Look ahead for consecutive same-receiver, same-method calls */
        int src1 = cur->src1;
        int64_t imm = cur->imm;  /* packed: name_idx<<16 | argc */
        int j = i + 1;
        while (j < n) {
            MIRInstr *next = &mir->instructions[j];
            if (next->opcode != MIR_OP_call_method
                || next->src1 != src1
                || next->imm != imm) break;
            j++;
        }

        if (j > i + 1) {
            /* We have a run of 2+ contiguous same-method calls.
             * Insert cache before the first, convert rest to cached. */
            int run_len = j - i;
            /* Allocate space: need 1 extra slot for cache */
            if (mir->num_instructions + 1 >= mir->capacity) {
                int new_cap = mir->capacity * 2;
                MIRInstr *new_instrs = (MIRInstr *)realloc(mir->instructions, (size_t)new_cap * sizeof(MIRInstr));
                int *new_bc = (int *)realloc(mir->bc_offsets, (size_t)new_cap * sizeof(int));
                if (!new_instrs) return;
                mir->instructions = new_instrs;
                if (new_bc) mir->bc_offsets = new_bc;
                mir->capacity = new_cap;
            }
            /* Shift instructions[i+1..j] right by 1 */
            memmove(&mir->instructions[i + 1], &mir->instructions[i],
                    (size_t)(n - i) * sizeof(MIRInstr));
            if (mir->bc_offsets) {
                memmove(&mir->bc_offsets[i + 1], &mir->bc_offsets[i],
                        (size_t)(n - i) * sizeof(int));
            }
            mir->num_instructions++;

            /* Insert cache at position i: dst[0..4] = callee, valid, argc, prog, name */
            MIRInstr *cache = &mir->instructions[i];
            cache->opcode = MIR_OP_method_cache;
            cache->dst = src1;      /* cache slots start at src1 position */
            cache->src1 = src1;     /* reads receiver from args_base[0] */
            cache->src2 = -1;
            cache->imm = (int32_t)(imm >> 16);  /* argc stored in high word */
            cache->ptr = (void*)(uintptr_t)imm;            /* prog pointer for method lookup */

            /* Convert remaining ops to cached versions */
            for (int k = i + 1; k < j + 1; k++) {
                MIRInstr *inst = &mir->instructions[k];
                inst->opcode = MIR_OP_call_cached_method;
                inst->dst = src1 + 5;  /* result goes to cache+5 */
                inst->src1 = src1;     /* cache area */
                inst->imm = (int32_t)(imm >> 16);  /* argc */
            }
            n = mir->num_instructions;
            i = j + 1;
        } else {
            i++;
        }
    }
}

/* Private field cache optimization (m8): detect consecutive accesses to the
 * same private field (#name) on the same object and convert to dedicated
 * MIR_OP_private_field_get/set ops.
 *
 * Strategy: scan for runs of contiguous MIR_OP_get_prop / MIR_OP_set_prop
 * that share the same src1 (object) and imm (pool name_idx pointing to a
 * "#..." string). For runs of 2+, replace with private_field ops. */
static void mir_optimize_private_field_cache(MIRProgram *mir, const BCProgram *prog) {
    if (!mir || !prog || mir->num_instructions < 2) return;
    if (prog->pool_count == 0) return;
    int n = mir->num_instructions;

    for (int i = 0; i < n; ) {
        MIRInstr *cur = &mir->instructions[i];
        int is_get = (cur->opcode == MIR_OP_get_prop);
        int is_set = (cur->opcode == MIR_OP_set_prop);
        if (!is_get && !is_set) { i++; continue; }

        int64_t imm = cur->imm;  /* pool name_idx */
        int src1 = cur->src1;

        /* Check if this is a private field access */
        if (imm < 0 || imm >= (int64_t)prog->pool_count) { i++; continue; }
        const char *name = prog->pool[(size_t)imm].u.str;
        if (!name || name[0] != '#') { i++; continue; }

        /* Look ahead for consecutive same-object private field accesses */
        int j = i + 1;
        while (j < n) {
            MIRInstr *next = &mir->instructions[j];
            int next_is_get = (next->opcode == MIR_OP_get_prop);
            int next_is_set = (next->opcode == MIR_OP_set_prop);
            if (!next_is_get && !next_is_set) break;
            if (next->src1 != src1 || next->imm != imm) break;
            /* Same private field on same object — skip if mixed read/write
             * between different dst slots (they might be independent) */
            if (is_get && next_is_set) break;
            if (is_set && next_is_get) break;
            j++;
        }

        if (j > i + 1) {
            int run_len = j - i;
            MIROpcode new_op = is_get ? MIR_OP_private_field_get
                                       : MIR_OP_private_field_set;
            /* Update all ops in the run */
            for (int k = i; k < j; k++) {
                mir->instructions[k].opcode = new_op;
            }
            n = mir->num_instructions;
            i = j;
        } else {
            i++;
        }
    }
}

/* Shape cache optimization (Priority 2 from V8 analysis): detect consecutive
 * identical MIR_OP_get_prop on the same object + property name and convert to
 * MIR_OP_get_prop_cached. The cached variant does a runtime shape check and
 * falls back to lr_jit_rt_get_prop on miss, but on hit reads the flat slot
 * directly — avoiding the full property lookup chain. */
static void mir_optimize_prop_shape_cache(MIRProgram *mir, const BCProgram *prog) {
    if (!mir || !prog || mir->num_instructions < 2) return;
    int n = mir->num_instructions;

    for (int i = 0; i < n; ) {
        MIRInstr *cur = &mir->instructions[i];
        if (cur->opcode != MIR_OP_get_prop) { i++; continue; }

        int64_t imm = cur->imm;  /* pool name_idx */
        int src1 = cur->src1;

        /* Only optimize if we have a valid name_idx in pool range. */
        if (imm < 0 || imm >= (int64_t)prog->pool_count) { i++; continue; }

        /* Look ahead for consecutive same-(obj, prop) get_prop ops.
         * Bail out if any intervening write op could invalidate the shape cache. */
        int j = i + 1;
        while (j < n) {
            MIRInstr *next = &mir->instructions[j];
            if (next->opcode != MIR_OP_get_prop) break;
            if (next->src1 != src1 || next->imm != imm) break;
            /* Check for write ops between current position j and the next candidate.
             * These could mutate the object's shape and invalidate caches. */
            int has_write = 0;
            for (int k = i + 1; k < j; k++) {
                MIRInstr *mid = &mir->instructions[k];
                if (mid->opcode == MIR_OP_set_prop || mid->opcode == MIR_OP_set_elem) {
                    has_write = 1;
                    break;
                }
            }
            if (has_write) break;
            j++;
        }

        if (j > i + 1) {
            /* Convert first to method_cache (populate), rest to call_cached */
            int run_len = j - i;
            /* For get_prop_cached we need a local cache area.
             * Use dst as cache_base and store obj pointer + shape ptr in it. */
            MIRInstr *first = &mir->instructions[i];
            first->opcode = MIR_OP_load_prop;
            /* MIR_OP_load_prop: (result, obj, name_idx) — same as get_prop but
             * the codegen will do an inline shape check before falling back.
             * We keep src1=obj, imm=name_idx, dst=result. */

            /* Convert remaining ops to cached versions.
             * MIR_OP_get_prop_cached: (result, obj, name_idx, cache_slot)
             * cache_slot is stored in src2. */
            for (int k = i + 1; k < j; k++) {
                MIRInstr *inst = &mir->instructions[k];
                inst->opcode = MIR_OP_get_prop_cached;
                inst->src2 = src1 + 1;  /* cache area starts after obj slot */
            }
            n = mir->num_instructions;
            i = j;
        } else {
            i++;
        }
    }
}

static void mir_mark_instr_reads(const MIRInstr *t, uint8_t *used, int max_slot) {
    if (t->src1 >= 0 && t->src1 <= max_slot) used[t->src1] = 1;
    if (t->src2 >= 0 && t->src2 <= max_slot) used[t->src2] = 1;
    if (t->src1 < 0) return;
    if (mir_op_is_variadic_read((MIROpcode)t->opcode)) {
        int argc = (int)(t->imm & 0xFFFF);
        int hi = t->src1 + argc + 2;
        if (hi > max_slot) hi = max_slot;
        for (int k = t->src1; k <= hi; k++) used[k] = 1;
    }
}

static void mir_eliminate_globally_dead(MIRProgram *mir) {
    if (!mir || mir->num_instructions <= 0) return;
    int n = mir->num_instructions;

    /* Slot space: bytecode locals occupy [0 .. MIR_VAR_BASE-1], JIT temps
     * start at MIR_VAR_BASE.  Pure ops only define temps (dst >= MIR_VAR_BASE),
     * so removing an unused temp can never clobber a local variable. */
    int max_slot = MIR_VAR_BASE * 4;
    for (int i = 0; i < n; i++) {
        const MIRInstr *t = &mir->instructions[i];
        if (t->dst  > max_slot) max_slot = t->dst;
        if (t->src1 > max_slot) max_slot = t->src1;
        if (t->src2 > max_slot) max_slot = t->src2;
        if (t->src1 >= 0 && mir_op_is_variadic_read((MIROpcode)t->opcode)) {
            int r = t->src1 + (int)(t->imm & 0xFFFF) + 2;
            if (r > max_slot) max_slot = r;
        }
    }

    uint8_t *used = (uint8_t *)calloc((size_t)max_slot + 1, 1);
    if (!used) return;

    for (int i = 0; i < n; i++)
        mir_mark_instr_reads(&mir->instructions[i], used, max_slot);

    int w = 0;
    for (int i = 0; i < n; i++) {
        MIRInstr *t = &mir->instructions[i];
        int keep = 1;
        if (mir_is_pure_no_side_effect((MIROpcode)t->opcode)
            && t->dst >= MIR_VAR_BASE
            && !used[t->dst]) {
            /* Result is never read: the instruction is globally dead. */
            keep = 0;
        }
        if (keep) {
            if (w != i) {
                mir->instructions[w] = mir->instructions[i];
                if (mir->bc_offsets) mir->bc_offsets[w] = mir->bc_offsets[i];
            }
            w++;
        }
    }
    mir->num_instructions = w;
    free(used);
}

/* Detect simple increment loops: for(let i=0; i<n; i++) pattern */
static int mir_detect_simple_loop(MIRProgram *mir) {
    if (!mir || mir->num_instructions < 4) return 0;
    int has_init = 0, has_inc = 0, has_cmp = 0, has_jump = 0;
    for (int i = 0; i < mir->num_instructions; i++) {
        MIROpcode op = mir->instructions[i].opcode;
        if (op == MIR_OP_const_i32 && mir->instructions[i].imm == 0) has_init = 1;
        else if (op == MIR_OP_add_i32 && mir->instructions[i].imm == 1) has_inc = 1;
        else if (op == MIR_OP_lt_i32 || op == MIR_OP_gt_i32) has_cmp = 1;
        else if (op == MIR_OP_jump) has_jump = 1;
    }
    return has_init && has_inc && has_cmp && has_jump;
}

/* ══════════════════════════════════════════════════════════════════════ *
 * Bytecode to MIR Compiler *
 * ══════════════════════════════════════════════════════════════════════ */
typedef struct {
    MIRProgram *mir;
    int *stack;
    int sp;
    int capacity;
    uint8_t *base_ip;  /* start of bytecode, for computing byte offsets */
    /* Per-var scope slot tracking (V8 per-call-frame style):
     * var_scope_slot[v] = scope slot index if v came from BC_LOAD_LOCAL,
     *                    or -1 if v was computed (arithmetic, const, etc.)
     * Used by BC_CALL to generate scope-aware calls for recursive JIT. */
    int *var_scope_slot;
    int var_scope_capacity;
    /* Name-to-slot mapping for BC_DECLARE_VAR / BC_LOAD_VAR / BC_STORE_VAR.
     * name_to_slot_pool[name_idx] = slot index (or -1 if not a local).
     * Built during first-pass scan over bytecode. */
    int *name_to_slot;
    int name_to_slot_cap;
    /* Per-var type tracking for speculative int32 optimization.
     * var_type[v]: 0 = unknown/other, 1 = int32, 2 = float64.
     * Used to detect float64 operands in arithmetic so we can bail out
     * to the interpreter instead of emitting wrong int32 code. */
    int *var_type;
    int var_type_capacity;
    /* Per-slot type tracking (bytecode local slots 0..63).
     * slot_type[slot]: 0 = unknown, 1 = int32, 2 = float64.
     * Propagated on BC_STORE_LOCAL / BC_LOAD_LOCAL so a float64 stored in
     * a local (e.g. `var pi = 3.14`) is still recognized when reloaded. */
    int slot_type[64];
    /* Type specialization hint from PGO runtime data.
     * 0 = no hint (default, use static analysis),
     * 1 = dominant int32 path preferred (aggressive int32 speculation),
     * 2 = dominant float64 path preferred. */
    int type_hint;
    /* Per-var pool name tracking: var_pool_name[v] = name_idx into prog->pool
     * if v was produced by a BC_LOAD_PROP (e.g. Atomics from global.Atomics),
     * or -1 if unknown/computed. Used for special-casing calls like Atomics.load. */
    int *var_pool_name;
    int var_pool_name_capacity;
    /* Loop stack management: when processing a forward BC_JUMP_IF_FALSE
     * (loop exit), we enter skip mode. All instructions between the
     * forward jump and the matching backward jump are skipped. This
     * avoids stack corruption from processing dead fall-through code. */
    int loop_skip_sp;
    int loop_skip_bc_off;
    int loop_skip_mode;
    /* When 1, allow btm_compile_instr to enter skip mode on forward jumps.
     * Set to 0 during post-merge processing in recovery loop to prevent
     * while-loop exits from re-entering skip mode. */
    int allow_skip_mode;
    /* Per-var function tracking: var_is_function[v] = 1 if the variable is
     * known to hold an LR_OBJ_FUNCTION value (loaded via BC_LOAD_VAR/LOCAL
     * of a named function declaration).  Used by BC_CALL to prefer
     * MIR_OP_inline_call over MIR_OP_runtime_call when the callee can be
     * dispatched directly to its JIT entry. */
    uint8_t *var_is_function;
    int var_is_function_capacity;
    /* Per-var function-self-reference tracking: var_func_self_ref[v] = 1 if
     * the variable was loaded via BC_LOAD_VAR of the function's own name.
     * These must use MIR_OP_load_var (scope chain lookup) at runtime, NOT
     * MIR_OP_load_local, because the function name lives in the parent scope,
     * not in the function's own spill area. */
    uint8_t *var_func_self_ref;
    int var_func_self_ref_capacity;
} BytecodeToMIR;

static BytecodeToMIR *btm_create(MIRProgram *mir, int max_stack) {
    BytecodeToMIR *ctx = (BytecodeToMIR *)calloc(1, sizeof(BytecodeToMIR));
    if (!ctx) return NULL;
    ctx->mir = mir;
    ctx->capacity = max_stack;
    ctx->stack = (int *)calloc(max_stack, sizeof(int));
    if (!ctx->stack) {
        free(ctx);
        return NULL;
    }
    ctx->sp = 0;
    ctx->base_ip = NULL;
    ctx->var_scope_slot = (int *)calloc(1024, sizeof(int));
    if (!ctx->var_scope_slot) {
        free(ctx->stack);
        free(ctx);
        return NULL;
    }
    ctx->var_scope_capacity = 1024;
    for (int i = 0; i < 1024; i++) ctx->var_scope_slot[i] = -1;
    ctx->name_to_slot = (int *)calloc(1024, sizeof(int));
    if (!ctx->name_to_slot) {
        free(ctx->stack);
        free(ctx->var_scope_slot);
        free(ctx);
        return NULL;
    }
    for (int i = 0; i < 1024; i++) ctx->name_to_slot[i] = -1;
    ctx->name_to_slot_cap = 1024;
    ctx->var_type = (int *)calloc(1024, sizeof(int));
    if (!ctx->var_type) {
        free(ctx->stack);
        free(ctx->var_scope_slot);
        free(ctx->name_to_slot);
        free(ctx);
        return NULL;
    }
    ctx->var_type_capacity = 1024;
    ctx->var_pool_name = (int *)calloc(1024, sizeof(int));
    if (!ctx->var_pool_name) {
        free(ctx->stack);
        free(ctx->var_scope_slot);
        free(ctx->name_to_slot);
        free(ctx->var_type);
        free(ctx);
        return NULL;
    }
    for (int i = 0; i < 1024; i++) ctx->var_pool_name[i] = -1;
    ctx->var_pool_name_capacity = 1024;
    ctx->var_is_function = (uint8_t *)calloc(1024, sizeof(uint8_t));
    if (!ctx->var_is_function) {
        free(ctx->stack);
        free(ctx->var_scope_slot);
        free(ctx->name_to_slot);
        free(ctx->var_type);
        free(ctx->var_pool_name);
        free(ctx);
        return NULL;
    }
    ctx->var_is_function_capacity = 1024;
    ctx->var_func_self_ref = (uint8_t *)calloc(1024, sizeof(uint8_t));
    if (!ctx->var_func_self_ref) {
        free(ctx->stack);
        free(ctx->var_scope_slot);
        free(ctx->name_to_slot);
        free(ctx->var_type);
        free(ctx->var_pool_name);
        free(ctx->var_is_function);
        free(ctx);
        return NULL;
    }
    ctx->var_func_self_ref_capacity = 1024;
    ctx->loop_skip_sp = 0;
    ctx->loop_skip_bc_off = 0;
    ctx->loop_skip_mode = 0;
    ctx->allow_skip_mode = 1;
    return ctx;
}

static void btm_free(BytecodeToMIR *ctx) {
    if (ctx) {
        free(ctx->stack);
        free(ctx->var_scope_slot);
        free(ctx->name_to_slot);
        free(ctx->var_type);
        free(ctx->var_pool_name);
        free(ctx->var_is_function);
        free(ctx->var_func_self_ref);
        free(ctx);
    }
}

static int btm_push(BytecodeToMIR *ctx, int var) {
    if (ctx->sp >= ctx->capacity) return -1;
    ctx->stack[ctx->sp++] = var;
    return 0;
}

static int btm_new_var(BytecodeToMIR *ctx);

static int btm_push_new(BytecodeToMIR *ctx) {
    if (ctx->sp >= ctx->capacity) return -1;
    int v = btm_new_var(ctx);
    if (v < 0) return -1;
    ctx->stack[ctx->sp++] = v;
    return v;
}

static int btm_pop(BytecodeToMIR *ctx) {
    if (ctx->sp <= 0) return -1;
    int v = ctx->stack[ctx->sp - 1];
    ctx->sp--;
    return v;
}

static int btm_peek(BytecodeToMIR *ctx, int offset) {
    if (ctx->sp <= offset) return -1;
    return ctx->stack[ctx->sp - 1 - offset];
}

static int btm_new_var(BytecodeToMIR *ctx) {
    /* Offset MIR temporaries above the bytecode-slot region so var indices
     * (used directly as JIT local-array offsets) never overwrite slots
     * still referenced by MIR_OP_load_local/store_local. */
    int v = mir_new_var(ctx->mir);
    if (v >= 0) {
        v += MIR_VAR_BASE;
    }
    return (v < 0) ? -1 : v;
}

/* Ensure var_pool_name can track var v. Returns 0 on success, -1 on OOM. */
static int btm_ensure_pool_name(BytecodeToMIR *ctx, int v) {
    if (v < 0) return 0;
    if (v < ctx->var_pool_name_capacity) return 0;
    int new_cap = ctx->var_pool_name_capacity;
    while (new_cap <= v) new_cap *= 2;
    int *new_arr = (int *)realloc(ctx->var_pool_name, sizeof(int) * new_cap);
    if (!new_arr) return -1;
    for (int i = ctx->var_pool_name_capacity; i < new_cap; i++)
        new_arr[i] = -1;
    ctx->var_pool_name = new_arr;
    ctx->var_pool_name_capacity = new_cap;
    return 0;
}

/* Record that var v was produced by loading pool[name_idx] (e.g. Atomics). */
static void btm_set_var_pool_name(BytecodeToMIR *ctx, int v, int name_idx) {
    if (v < 0) return;
    btm_ensure_pool_name(ctx, v);
    ctx->var_pool_name[v] = name_idx;
}

/* Record that var v came from scope slot s (for V8 per-call-frame scope calls).
 * Returns 0 on success, -1 if capacity exceeded. */
static int btm_record_scope_slot(BytecodeToMIR *ctx, int v, int s) {
    if (v < 0 || s < 0) return 0;
    if (v >= ctx->var_scope_capacity) {
        int new_cap = ctx->var_scope_capacity * 2;
        int *new_arr = (int *)realloc(ctx->var_scope_slot, sizeof(int) * new_cap);
        if (!new_arr) return -1;
        /* Initialize new entries to -1 */
        for (int i = ctx->var_scope_capacity; i < new_cap; i++)
            new_arr[i] = -1;
        ctx->var_scope_slot = new_arr;
        ctx->var_scope_capacity = new_cap;
    }
    ctx->var_scope_slot[v] = s;
    return 0;
}

/* Retrieve the recorded scope slot for a MIR var, or -1 if not tracked. */
static int btm_get_scope_slot(BytecodeToMIR *ctx, int v) {
    if (v < 0 || v >= ctx->var_scope_capacity) return -1;
    return ctx->var_scope_slot[v];
}

/* Mark that var v holds a function value (LR_OBJ_FUNCTION), enabling
 * inline JIT-to-JIT calls instead of going through the runtime bridge. */
static void btm_set_var_is_function(BytecodeToMIR *ctx, int v) {
    if (v < 0) return;
    if (v >= ctx->var_is_function_capacity) {
        int new_cap = ctx->var_is_function_capacity;
        while (new_cap <= v) new_cap *= 2;
        uint8_t *new_arr = (uint8_t *)realloc(ctx->var_is_function, sizeof(uint8_t) * new_cap);
        if (!new_arr) return;
        for (int i = ctx->var_is_function_capacity; i < new_cap; i++)
            new_arr[i] = 0;
        ctx->var_is_function = new_arr;
        ctx->var_is_function_capacity = new_cap;
    }
    ctx->var_is_function[v] = 1;
}

/* Mark that var v is a function self-reference (BC_LOAD_VAR of the
 * function's own name).  These must use load_var (scope chain) at
 * runtime, not load_local, because the name lives in the parent scope. */
static void btm_record_func_self_ref(BytecodeToMIR *ctx, int v) {
    if (v < 0) return;
    if (v >= ctx->var_func_self_ref_capacity) {
        int new_cap = ctx->var_func_self_ref_capacity;
        while (new_cap <= v) new_cap *= 2;
        uint8_t *new_arr = (uint8_t *)realloc(ctx->var_func_self_ref, sizeof(uint8_t) * new_cap);
        if (!new_arr) return;
        for (int i = ctx->var_func_self_ref_capacity; i < new_cap; i++)
            new_arr[i] = 0;
        ctx->var_func_self_ref = new_arr;
        ctx->var_func_self_ref_capacity = new_cap;
    }
    ctx->var_func_self_ref[v] = 1;
}

/* Check if var v is a function self-reference. */
static int btm_is_func_self_ref(BytecodeToMIR *ctx, int v) {
    if (v < 0 || v >= ctx->var_func_self_ref_capacity) return 0;
    return ctx->var_func_self_ref[v];
}

/* Grow name_to_slot array if needed, returning 0 on success or -1 on failure. */
static int btm_ensure_name_to_slot(BytecodeToMIR *ctx, int name_idx) {
    if (name_idx < ctx->name_to_slot_cap) return 0;
    int new_cap = ctx->name_to_slot_cap;
    while (new_cap <= name_idx) new_cap *= 2;
    int *new_arr = (int *)realloc(ctx->name_to_slot, sizeof(int) * new_cap);
    if (!new_arr) return -1;
    for (int i = ctx->name_to_slot_cap; i < new_cap; i++)
        new_arr[i] = -1;
    ctx->name_to_slot = new_arr;
    ctx->name_to_slot_cap = new_cap;
    return 0;
}

/* First-pass: scan bytecode to build name→slot mapping.
 * Walks the bytecode stream, tracking BC_DECLARE_VAR to record which
 * pool name index maps to which function-scope slot.  The slot index
 * is determined by counting how many distinct names have been declared
 * so far (mirroring the runtime scope's declaration order). */
static void btm_build_name_map(BytecodeToMIR *ctx, BCProgram *prog) {
    uint8_t *ip = prog->code;
    uint8_t *end = ip + prog->code_len;
    /* Track declared names to assign slots in declaration order.
     * We store name pointers here to detect duplicates. */
    const char **declared = (const char **)calloc(256, sizeof(const char *));
    if (!declared) return;
    /* Slot layout: slot 0 = "this", slots 1..nparams = params, then locals.
     * var/let declarations must start AFTER the parameter slots so they
     * never overwrite the function's arguments. */
    int decl_count = 1 + prog->nparams;

    while (ip < end) {
        uint8_t opcode = *ip++;
        switch (opcode) {
            case BC_NOP:
            case BC_STOP:
                break;

            case BC_DECLARE_VAR: {
                uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 3; /* skip name_idx + kind */
                const char *name = prog->pool[name_idx].u.str;
                if (name) {
                    /* Check if already declared (duplicate var) */
                    int already = 0;
                    for (int i = 0; i < decl_count; i++) {
                        if (declared[i] && strcmp(declared[i], name) == 0) {
                            already = 1;
                            break;
                        }
                    }
                    if (!already && decl_count < 256) {
                        declared[decl_count] = name;
                        if (btm_ensure_name_to_slot(ctx, name_idx) >= 0) {
                            ctx->name_to_slot[name_idx] = decl_count;
                        }
                        decl_count++;
                    }
                }
                break;
            }

            case BC_LOAD_LOCAL:
            case BC_STORE_LOCAL:
            case BC_INC_LOCAL:
            case BC_INC_LOCAL_DISCARD: {
                uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                (void)slot;
                break;
            }

            case BC_LOAD_VAR:
            case BC_STORE_VAR:
            case BC_TYPEOF_VAR:
            case BC_INC_VAR: {
                uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2;
                /* These use name-based lookup at runtime.
                 * If we already have a slot mapping, keep it.
                 * Otherwise mark as -1 (global or not-yet-declared). */
                if (btm_ensure_name_to_slot(ctx, name_idx) >= 0
                    && ctx->name_to_slot[name_idx] == -1) {
                    /* Name used but not declared in this function — leave as -1 */
                }
                break;
            }

            case BC_CALL: {
                uint16_t argc = (uint16_t)(ip[0] | (ip[1] << 8));
                ip += 2 + argc * 2;
                break;
            }

            case BC_PUSH_INT32:
                ip += 4;
                break;

            case BC_JUMP:
            case BC_JUMP_IF_FALSE:
            case BC_JUMP_IF_TRUE:
            case BC_JUMP_IF_FALSE_KEEP:
            case BC_JUMP_IF_TRUE_KEEP:
            case BC_JUMP_IF_NOT_NULLISH: {
                /* Linear scan: skip the 4-byte jump offset operand only.
                 * Do NOT follow the jump target — following back-edges of
                 * loops (negative offsets) would cause an infinite loop. */
                ip += 4;
                break;
            }

            case BC_JUMP_IF_LOCAL_LT_IMM: {
                ip += 2; /* skip u16 slot */
                ip += 8; /* skip i32 imm + i32 offset */
                break;
            }

            default:
                /* Unknown or variable-length opcode — skip conservatively.
                 * Most fixed-size opcodes are handled above. */
                break;
        }
    }
    free(declared);
}

/* Map the function's parameter names to their local slots (1..nparams).
 *
 * The bytecode references params via name (BC_LOAD_VAR), but when the
 * function is slot-safe (prog->can_inline == 1) the JIT prologue copies
 * current_scope->values[0..nparams] into local slots 0..nparams.  A param
 * name can therefore be resolved to a direct slot load, eliminating a
 * per-use lr_jit_rt_load_var() scope-chain walk — which dominated the hot
 * loops (countPrimes `i<=n`, hotLoop `i<iterations`).  Slots are 1-indexed
 * because slot 0 is `this`. */
static void btm_map_params_to_slots(BytecodeToMIR *ctx, BCProgram *prog) {
    if (!ctx || !prog || !prog->can_inline) return;
    int np = prog->nparams;
    if (np <= 0 || np > 255) return;
    if (!prog->cached_params_ast) return;
    for (int i = 0; i < np; i++) {
        ASTNode *p = prog->cached_params_ast[i];
        if (!p || p->type != AST_IDENTIFIER || !p->u.ident.name) continue;
        /* Find the pool name_idx matching this param name. */
        for (int k = 0; k < prog->pool_count; k++) {
            if (prog->pool[k].u.str && strcmp(prog->pool[k].u.str, p->u.ident.name) == 0) {
                if (btm_ensure_name_to_slot(ctx, k) >= 0)
                    ctx->name_to_slot[k] = i + 1;   /* slot 1..nparams */
                break;
            }
        }
    }
}

static int btm_add_instr(BytecodeToMIR *ctx, MIROpcode op, int dst, int src1, int src2,
                         int64_t imm, void *ptr, uint8_t *current_ip) {
    int idx = ctx->mir->num_instructions;
    if (idx < ctx->mir->capacity) {
        ctx->mir->bc_offsets[idx] = (int)(current_ip - ctx->base_ip);
    }
    return mir_add_instr(ctx->mir, op, dst, src1, src2, imm, ptr);
}

/* Record the tracked type of a MIR variable (0=unknown, 1=int32, 2=float64, 3=string). */
static void btm_set_var_type(BytecodeToMIR *ctx, int v, int type) {
    if (v >= MIR_VAR_BASE && v - MIR_VAR_BASE < ctx->var_type_capacity)
        ctx->var_type[v - MIR_VAR_BASE] = type;
}

/* Return 1 if the variable is known to be a float64 (from a const_f64 load). */
static int btm_var_is_float64(BytecodeToMIR *ctx, int v) {
    if (v >= MIR_VAR_BASE && v - MIR_VAR_BASE < ctx->var_type_capacity)
        return ctx->var_type[v - MIR_VAR_BASE] == 2;
    return 0;
}

/* Return 1 if the variable is known to be a string (from a const_string load). */
static int btm_var_is_string(BytecodeToMIR *ctx, int v) {
    if (v >= MIR_VAR_BASE && v - MIR_VAR_BASE < ctx->var_type_capacity)
        return ctx->var_type[v - MIR_VAR_BASE] == 3;
    return 0;
}

/* Return 1 if either operand of a binary op is a known float64, meaning the
 * int32 arithmetic instruction would produce wrong results. */
static int btm_binop_has_float64(BytecodeToMIR *ctx, int a, int b) {
    return btm_var_is_float64(ctx, a) || btm_var_is_float64(ctx, b);
}

/* With type_hint==2 (float64 dominant), treat unknown operands as f64
 * to avoid generating int32 code that would need runtime bailouts. */
static int btm_binop_may_need_f64(BytecodeToMIR *ctx, int a, int b) {
    if (btm_binop_has_float64(ctx, a, b)) return 1;
    /* If type_hint says float64 dominant, assume float64 for unknown ops. */
    if (ctx->type_hint == 2) return 1;
    return 0;
}

/* Return 1 if either operand of a binary op is a known string, meaning the
 * int32 arithmetic instruction would treat string pointers as integers and
 * produce garbage (especially for `+` concatenation). */
static int btm_binop_has_string(BytecodeToMIR *ctx, int a, int b) {
    return btm_var_is_string(ctx, a) || btm_var_is_string(ctx, b);
}

static int btm_add_imm(BytecodeToMIR *ctx, int64_t imm, uint8_t *current_ip) {
    int v = btm_new_var(ctx);
    if (v < 0) return -1;
    btm_add_instr(ctx, MIR_OP_const_i32, v, -1, -1, imm, NULL, current_ip);
    ctx->mir->const_values[v - MIR_VAR_BASE] = (int)imm;
    return v;
}

/* Compute the byte size of a bytecode instruction without executing it.
 * Used in skip mode to advance ip past dead fall-through code. */
static int btm_instr_size(const uint8_t *ip) {
    if (!ip) return 1;
    uint8_t op = ip[0];
    switch (op) {
        case BC_STOP:    return 1;
        case BC_NOP:     return 1;
        case BC_PUSH_UNDEFINED:
        case BC_PUSH_NULL:
        case BC_PUSH_TRUE:
        case BC_PUSH_FALSE:
        case BC_PUSH_THIS:
        case BC_POP:
        case BC_DUP:
        case BC_DUP2:
        case BC_SWAP:
        case BC_ROT3:
        case BC_NEG:
        case BC_POS:
        case BC_NOT:
        case BC_BIT_NOT:
        case BC_TYPEOF:
        case BC_VOID:
        case BC_TO_STRING:
        case BC_TO_NUMBER:
        case BC_TO_BOOL:
        case BC_NEW_OBJECT:
        case BC_SCOPE_ENTER:
        case BC_SCOPE_LEAVE:
        case BC_SET_RESULT:
        case BC_CLEAR_RESULT:
        case BC_LOOP_TICK:
            return 1;
        case BC_PUSH_INT32:
        case BC_ITER_NEXT:
            return 5;
        case BC_PUSH_FLOAT64:
        case BC_PUSH_STRING:
        case BC_GET_PROP:
        case BC_EVAL_NODE_POP:
        case BC_EVAL_NODE:
        case BC_DELETE_PROP:
        case BC_STORE_LOCAL:
        case BC_INC_LOCAL:
        case BC_INC_LOCAL_DISCARD:
        case BC_ADD_SELF:
        case BC_MUL_SELF:
        case BC_DEF_PROP:
        case BC_SET_PROP:
        case BC_NEW_ARRAY:
        case BC_ITER_INIT:
        case BC_ITER_CLOSE:
            return 3;
        case BC_LOAD_VAR:
        case BC_STORE_VAR:
        case BC_TYPEOF_VAR:
        case BC_INC_VAR:
        case BC_LOAD_LOCAL:
        case BC_GET_ELEM:
        case BC_SET_ELEM:
        case BC_DELETE_ELEM:
        case BC_LOAD_PROP:
        case BC_LOAD_PROP_ADD:
            return 3;
        case BC_CALL:
        case BC_CALL_ELEM:
        case BC_NEW:
        case BC_RETURN:
            return 3;
        case BC_CALL_METHOD: {
            /* u16 pool(name), u16 argc, then argc pairs of (obj key + arg) */
            uint16_t argc = (uint16_t)(ip[2]);
            return 3 + argc * 2;
        }
        case BC_JUMP:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_JUMP_IF_FALSE_KEEP:
        case BC_JUMP_IF_TRUE_KEEP:
        case BC_JUMP_IF_NOT_NULLISH:
            return 5;
        case BC_JUMP_IF_LOCAL_LT_IMM:
            return 10;
        case BC_DECLARE_VAR:
            return 4;
        case BC_THROW:
            return 1;
        default:
            /* Unknown opcode — assume 1 byte to avoid infinite loop */
            return 1;
    }
}

/* Emit MIR for a single bytecode instruction. Returns updated ip on success, NULL on bail */
static uint8_t *btm_compile_instr(BytecodeToMIR *ctx, Interpreter *interp,
                                  BCProgram *prog, uint8_t *ip, uint8_t *end) {
    if (!ip || ip >= end) return NULL;
    uint8_t opcode = *ip++;
    int a, b, result;
    int offset = (ip - 1) - prog->code;

    (void)prog;
    (void)interp;

    /* Skip mode: we are in dead fall-through code after a forward loop exit.
     * Advance ip past this instruction without emitting MIR or touching stack. */
    if (ctx->loop_skip_mode) {
        int sz = btm_instr_size(ip - 1);
        (void)ip;
        return ip + sz - 1;
    }

    switch (opcode) {
        /* NOP: skip padding/alignment bytes without emitting MIR */
        case BC_NOP:
            break;

        /* STOP: end of program */
        case BC_STOP:
            return NULL;

        case BC_PUSH_UNDEFINED:
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_push_undefined, result, -1, -1, 0, NULL, ip - 1);
            break;

        case BC_PUSH_NULL:
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_push_null, result, -1, -1, 0, NULL, ip - 1);
            break;

        case BC_PUSH_TRUE:
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_i32, result, -1, -1, 1, NULL, ip - 1);
            ctx->mir->const_values[result - MIR_VAR_BASE] = 1;
            break;

        case BC_PUSH_FALSE:
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_i32, result, -1, -1, 0, NULL, ip - 1);
            ctx->mir->const_values[result - MIR_VAR_BASE] = 0;
            break;

        case BC_PUSH_INT32: {
            int32_t val = (int32_t)((uint32_t)(ip[0]) |
                                    ((uint32_t)ip[1] << 8) |
                                    ((uint32_t)ip[2] << 16) |
                                    ((uint32_t)ip[3] << 24));
            ip += 4;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_i32, result, -1, -1, (int64_t)val, NULL, ip - 4);
            ctx->mir->const_values[result - MIR_VAR_BASE] = val;
            break;
        }

        /* Stack manipulation: track on the MIR operand stack so later
         * operand pops reference the correct values.  These only reorder
         * the var-id stack; no instructions are emitted. */
        case BC_POP:
            btm_pop(ctx);
            break;
        case BC_DUP: {
            int v = btm_peek(ctx, 0);
            if (v < 0) return NULL;
            btm_push(ctx, v);
            break;
        }
        case BC_DUP2: {
            int a = btm_peek(ctx, 1);
            int b = btm_peek(ctx, 0);
            if (a < 0 || b < 0) return NULL;
            btm_push(ctx, a);
            btm_push(ctx, b);
            break;
        }
        case BC_SWAP: {
            int a = btm_peek(ctx, 1);
            int b = btm_peek(ctx, 0);
            if (a < 0 || b < 0) return NULL;
            (void)btm_pop(ctx);
            (void)btm_pop(ctx);
            btm_push(ctx, b);
            btm_push(ctx, a);
            break;
        }
        case BC_ROT3: {
            int c = btm_pop(ctx);
            int b = btm_pop(ctx);
            int a = btm_pop(ctx);
            if (a < 0 || b < 0 || c < 0) return NULL;
            btm_push(ctx, b);
            btm_push(ctx, c);
            btm_push(ctx, a);
            break;
        }

        case BC_LOAD_LOCAL: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            /* Record scope slot mapping for V8 per-call-frame scope calls */
            int rc = btm_record_scope_slot(ctx, result, (int)slot);
            btm_add_instr(ctx, MIR_OP_load_local, result, (int)slot, -1, 0, NULL, ip - 3);
            /* Propagate the slot's tracked type to the loaded var. */
            if (slot < 64 && ctx->slot_type[slot] == 2)
                btm_set_var_type(ctx, result, 2);
            break;
        }

        case BC_STORE_LOCAL: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_store_local, -1, a, (int)slot, 0, NULL, ip - 3);
            /* Propagate the stored value's type to the slot. */
            if (slot < 64)
                ctx->slot_type[slot] = btm_var_is_float64(ctx, a) ? 2 : 0;
            break;
        }

        case BC_ADD:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_has_string(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_string_concat, result, a, b, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 3);
            } else if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_add_f64, result, a, b, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 2);
            } else {
                btm_add_instr(ctx, MIR_OP_add_i32, result, a, b, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 1);
            }
            break;

        case BC_SUB: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            if (btm_binop_has_string(ctx, a, b))
                return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_sub_f64, result, b, a, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 2);
            } else {
                btm_add_instr(ctx, MIR_OP_sub_i32, result, b, a, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 1);
            }
            break;
        }

        case BC_MUL:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            if (btm_binop_has_string(ctx, a, b))
                return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_mul_f64, result, a, b, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 2);
            } else {
                btm_add_instr(ctx, MIR_OP_mul_i32, result, a, b, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 1);
            }
            break;

        case BC_DIV: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            if (btm_binop_has_string(ctx, a, b))
                return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            /* JS `/` is always IEEE-754 float division (7/2 === 3.5), and
             * integer division would also crash / misbehave on 0 or INT_MIN/-1. */
            btm_add_instr(ctx, MIR_OP_div_f64, result, b, a, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 2);
            break;
        }

        case BC_MOD: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            if (btm_binop_has_string(ctx, a, b))
                return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            /* JS `%` is IEEE-754 float modulo, but for two int32 operands the
             * result is the same as the truncated integer remainder (and the
             * compiler's type tracking knows both are int32).  Emit a native
             * idiv mod for int32 operands to avoid a per-op lr_jit_rt_fmod()
             * C call (which dominated the countPrimes inner loop).  Fall back
             * to f64 when either operand is a float or a string-concat is not
             * possible. */
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_mod_f64, result, b, a, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 2);
            } else {
                btm_add_instr(ctx, MIR_OP_mod_i32, result, b, a, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 1);
            }
            break;
        }

        case BC_POW: {
            /* Stack: [..., x, y] on entry (y = top).  Semantics: x ** y → b ** a.
             * Use runtime callback since pow produces a float64 LRValue
             * and needs C library support. */
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_pow, result, b, a, 0, NULL, ip - 1);
            break;
        }

        case BC_LT: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_lt_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_lt_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;
        }

        case BC_GT: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_gt_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_gt_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;
        }

        case BC_LE: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_le_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_le_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;
        }

        case BC_GE: {
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_ge_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_ge_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;
        }

        case BC_EQ:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_eq_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_eq_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;

        case BC_NE:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_ne_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_ne_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;

        case BC_STRICT_EQ:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_eq_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_eq_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;

        case BC_STRICT_NE:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) break;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_binop_may_need_f64(ctx, a, b)) {
                btm_add_instr(ctx, MIR_OP_ne_f64, result, b, a, 0, NULL, ip - 1);
            } else {
                btm_add_instr(ctx, MIR_OP_ne_i32, result, b, a, 0, NULL, ip - 1);
            }
            break;

        case BC_NOT:
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_not_i32, result, a, -1, 0, NULL, ip - 1);
            break;

        case BC_BIT_NOT:
            /* ~x (bitwise NOT) is NOT the same as !x (logical NOT).
             * not_i32 implements logical NOT, so bail out to the interpreter
             * for bitwise NOT rather than emitting wrong code. */
            return NULL;

        case BC_BIT_AND:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_and_i32, result, a, b, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 1);
            break;

        case BC_BIT_OR:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_or_i32, result, a, b, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 1);
            break;

        case BC_BIT_XOR:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_xor_i32, result, a, b, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 1);
            break;

        case BC_SHL:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_shl_i32, result, a, b, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 1);
            break;

        case BC_SHR:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_shr_i32, result, a, b, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 1);
            break;

        case BC_SAR:
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_sar_i32, result, a, b, 0, NULL, ip - 1);
            btm_set_var_type(ctx, result, 1);
            break;

        case BC_NEG:
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            if (btm_var_is_float64(ctx, a)) {
                btm_add_instr(ctx, MIR_OP_neg_f64, result, a, -1, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 2);
            } else {
                btm_add_instr(ctx, MIR_OP_neg_i32, result, a, -1, 0, NULL, ip - 1);
                btm_set_var_type(ctx, result, 1);
            }
            break;

        case BC_POS: {
            /* Unary + is Number(x).  Route through runtime helper. */
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_pos, result, a, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_TYPEOF: {
            /* typeof value: route through runtime helper. */
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_typeof, result, a, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_VOID:
            btm_pop(ctx);
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_push_undefined, result, -1, -1, 0, NULL, ip - 1);
            break;

        case BC_IN: {
            /* Stack: [..., x, y] on entry (y = top).  Semantics: x in y → b in a. */
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_in, result, b, a, 0, NULL, ip - 1);
            break;
        }

        case BC_INSTANCEOF: {
            /* Stack: [..., x, y] on entry (y = top).  Semantics: x instanceof y. */
            a = btm_pop(ctx); b = btm_pop(ctx);
            if (a < 0 || b < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_instanceof, result, b, a, 0, NULL, ip - 1);
            break;
        }

        case BC_RETURN:
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_ret, -1, a, -1, 0, NULL, ip - 1);
            break;

        case BC_LOOP_TICK: {
            /* Loop guard: no-op for now. The interpreter-side
             * hot-loop detection in bc_execute handles compilation triggering. */
            break;
        }
        case BC_SET_RESULT:
        case BC_CLEAR_RESULT:
            /* No-op for JIT: just pop the result if any */
            if (opcode == BC_SET_RESULT) {
                btm_pop(ctx);
            }
            break;

        case BC_JUMP: {
            int32_t offset = (int32_t)(ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24));
            ip += 4;
            btm_add_instr(ctx, MIR_OP_jump, -1, -1, -1, (int64_t)offset, NULL, ip - 4);
            /* For backward jumps (loop re-entry), exit skip mode and
             * restore the pre-jump stack state. */
            if (offset < 0 && ctx->loop_skip_mode) {
                ctx->sp = ctx->loop_skip_sp;
                ctx->loop_skip_mode = 0;
            }
            break;
        }

        case BC_JUMP_IF_FALSE: {
            int32_t offset = (int32_t)(ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24));
            ip += 4;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_jump_if_false, -1, a, -1, (int64_t)offset, NULL, ip - 4);
            /* For forward jumps (loop exits), enter skip mode so subsequent
             * fall-through instructions are not processed as part of the loop.
             * Skip this during recovery loop post-merge phase. */
            if (offset > 0 && ctx->allow_skip_mode) {
                ctx->loop_skip_sp = ctx->sp;
                ctx->loop_skip_bc_off = (int)(ip - prog->code);
                ctx->loop_skip_mode = 1;
            }
            break;
        }

        case BC_JUMP_IF_TRUE: {
            int32_t offset = (int32_t)(ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24));
            ip += 4;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_jump_if_true, -1, a, -1, (int64_t)offset, NULL, ip - 4);
            break;
        }

        case BC_JUMP_IF_FALSE_KEEP: {
            int32_t offset = (int32_t)(ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24));
            ip += 4;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_jump_if_false, -1, a, -1, (int64_t)offset, NULL, ip - 4);
            btm_push(ctx, a);
            break;
        }

        case BC_JUMP_IF_TRUE_KEEP: {
            int32_t offset = (int32_t)(ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24));
            ip += 4;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_jump_if_true, -1, a, -1, (int64_t)offset, NULL, ip - 4);
            btm_push(ctx, a);
            break;
        }

        case BC_JUMP_IF_NOT_NULLISH: {
            /* Nullish coalescing (??): jump if value is NOT undefined and NOT null.
             * Inlined tag check in codegen — no runtime callback needed.
             * Value is kept on the stack (matching BC_JUMP_IF_NOT_NULLISH). */
            int32_t offset = (int32_t)(ip[0] | (ip[1] << 8) | (ip[2] << 16) | (ip[3] << 24));
            ip += 4;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_jump_if_not_nullish, -1, a, -1,
                          (int64_t)offset, NULL, ip - 4);
            btm_push(ctx, a);
            break;
        }

        case BC_JUMP_IF_LOCAL_LT_IMM: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            int32_t imm = (int32_t)((uint32_t)(ip[0]) | ((uint32_t)ip[1] << 8) |
                                    ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24));
            ip += 4;
            int32_t offset = (int32_t)((uint32_t)(ip[0]) | ((uint32_t)ip[1] << 8) |
                                        ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24));
            ip += 4;
            a = btm_new_var(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_load_local, a, (int)slot, -1, 0, NULL, ip - 10);
            int tmp = btm_new_var(ctx);
            if (tmp < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_i32, tmp, -1, -1, (int64_t)imm, NULL, ip - 6);
            int cond = btm_new_var(ctx);
            if (cond < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_lt_i32, cond, a, tmp, 0, NULL, ip - 6);
            btm_add_instr(ctx, MIR_OP_jump_if_false, -1, cond, -1, (int64_t)offset, NULL, ip - 6);
            break;
        }

        case BC_CALL: {
            uint16_t argc = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            /* Pop argc+1 items: args (top) then callee (bottom).
             * They may be non-contiguous in the spill area, so
             * allocate contiguous move slots and copy them. */
            int *popped = (int *)malloc(sizeof(int) * (argc + 1));
            if (!popped) return NULL;
            for (int i = argc; i >= 0; i--) {
                popped[i] = btm_pop(ctx);
                if (popped[i] < 0) { free(popped); return NULL; }
            }
            int callee_var = popped[0];
            /* V8 per-call-frame style: detect recursive calls.
             * A call is considered recursive only when the callee itself
             * is a function-scope variable (loaded via BC_LOAD_LOCAL),
             * meaning the function calls itself by name.  External
             * functions like print() use globals/closures, not scope slots,
             * so they must NOT be routed through scope_call. */
            int callee_slot = btm_get_scope_slot(ctx, callee_var);
            int is_self_ref = btm_is_func_self_ref(ctx, callee_var);
            int is_recursive = (callee_var >= 0 && prog != NULL &&
                                ((BCProgram *)prog)->prog_id != 0 &&
                                (callee_slot >= 0 || is_self_ref));
            if (is_recursive) {
                /* For recursive JIT calls, the callee was already resolved
                 * by the preceding BC_LOAD_VAR (compiled to MIR_OP_load_var
                 * which calls lr_jit_rt_load_var at runtime).  That value
                 * is already in spill area[callee_var].  We reuse it directly
                 * instead of reloading from scope — the function name lives
                 * in the parent scope, not in the function's own scope, so
                 * load_scope would read garbage.  Arguments are copied from
                 * the current spill area (they are already correct locals). */
                int arg_vars[32];
                int n_loaded = 0;
                for (int i = 1; i <= argc && i <= 32; i++) {
                    int v = btm_new_var(ctx);
                    if (v < 0) { free(popped); return NULL; }
                    arg_vars[i-1] = v;
                    btm_add_instr(ctx, MIR_OP_move, v, popped[i], -1, 0, NULL, ip - 3);
                    n_loaded++;
                }
                /* Build contiguous args_base: [callee, arg1, ..., argN] */
                int base_var = btm_new_var(ctx);
                if (base_var < 0) { free(popped); return NULL; }
                btm_add_instr(ctx, MIR_OP_move, base_var, callee_var, -1, 0, NULL, ip - 3);
                for (int i = 0; i < n_loaded; i++) {
                    btm_add_instr(ctx, MIR_OP_move, base_var + i + 1, arg_vars[i], -1,
                                  0, NULL, ip - 3);
                }
                result = btm_push_new(ctx);
                if (result < 0) { free(popped); return NULL; }
                btm_add_instr(ctx, MIR_OP_inline_call, result, base_var, -1,
                              (int64_t)argc, (void*)prog, ip - 3);
                free(popped);
                break;
            }
            /* Non-recursive path: prefer inline call when callee comes from
             * a scope slot (known function reference), otherwise fall back
             * to runtime call for dynamic callees. */
            int base_var = btm_new_var(ctx);
            if (base_var < 0) { free(popped); return NULL; }
            for (int i = 1; i <= argc; i++) {
                if (btm_new_var(ctx) < 0) { free(popped); return NULL; }
            }
            /* Emit moves: copy each popped var to its contiguous slot */
            for (int i = 0; i <= argc; i++) {
                btm_add_instr(ctx, MIR_OP_move, base_var + i, popped[i], -1,
                              0, NULL, ip - 3);
            }
            free(popped);
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            /* Use inline_call for known-function callees (from scope slot),
             * runtime_call for dynamic callees (globals, computed, etc.) */
            if (callee_slot >= 0) {
                btm_add_instr(ctx, MIR_OP_inline_call, result, base_var, -1,
                              (int64_t)argc, (void*)prog, ip - 3);
            } else {
                btm_add_instr(ctx, MIR_OP_runtime_call, result, base_var, -1,
                              (int64_t)argc, (void*)prog, ip - 3);
            }
            break;
        }

        case BC_CALL_METHOD: {
            /* Format: u16 name_idx, u16 argc.
             * Stack on entry: [..., this, arg1, ..., argN] (argc items above this).
             * Layout passed to runtime: args_base[0]=this, args_base[1..argc]=argv. */
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            uint16_t argc = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            int *popped = (int *)malloc(sizeof(int) * (argc + 2));
            if (!popped) return NULL;
            /* Pop args first (top of stack is last arg), then this. */
            for (int i = (int)argc; i >= 1; i--) {
                popped[i] = btm_pop(ctx);
                if (popped[i] < 0) { free(popped); return NULL; }
            }
            popped[0] = btm_pop(ctx);  /* this */
            if (popped[0] < 0) { free(popped); return NULL; }

            /* Special-case: Atomics.* static methods are called as
             * BC_CALL_METHOD with Atomics as "this".  Detect here so we
             * can emit MIR_OP_atomics_* directly instead of routing through
             * the full C-API call chain. */
            int atoomics_pool = -1;
            int method_op = -1;  /* which MIR_OP_atomics_* to emit */
            const char *method_name = prog->pool[name_idx].u.str;
            if (ctx->var_pool_name && popped[0] >= 0 &&
                popped[0] < ctx->var_pool_name_capacity &&
                ctx->var_pool_name[popped[0]] >= 0) {
                atoomics_pool = ctx->var_pool_name[popped[0]];
            }
            int is_atomics = (atoomics_pool >= 0 &&
                              strcmp(prog->pool[atoomics_pool].u.str, "Atomics") == 0);

            if (is_atomics && argc >= 1 && argc <= 4) {
                if      (strcmp(method_name, "load")            == 0) { method_op = MIR_OP_atomics_load;           }
                else if (strcmp(method_name, "store")           == 0) { method_op = MIR_OP_atomics_store;          }
                else if (strcmp(method_name, "add")             == 0) { method_op = MIR_OP_atomics_add;            }
                else if (strcmp(method_name, "sub")             == 0) { method_op = MIR_OP_atomics_sub;            }
                else if (strcmp(method_name, "and")             == 0) { method_op = MIR_OP_atomics_and;            }
                else if (strcmp(method_name, "or")              == 0) { method_op = MIR_OP_atomics_or;             }
                else if (strcmp(method_name, "xor")             == 0) { method_op = MIR_OP_atomics_xor;            }
                else if (strcmp(method_name, "exchange")        == 0) { method_op = MIR_OP_atomics_exchange;       }
                else if (strcmp(method_name, "compareExchange") == 0) { method_op = MIR_OP_atomics_compare_exchange; }
                else if (strcmp(method_name, "isLockFree")      == 0) { method_op = MIR_OP_atomics_is_lock_free;   }
            }

            if (method_op >= 0) {
                /* Atomics op: skip 'this' slot, lay out args directly at
                 * base_var+0..argc-1 so the JIT runtime sees the expected
                 * layout (args_base[0]=typedArray, args_base[1]=index, etc.). */
                int base_var = btm_new_var(ctx);
                if (base_var < 0) { free(popped); return NULL; }
                for (int i = 1; i <= argc; i++) {
                    if (btm_new_var(ctx) < 0) { free(popped); return NULL; }
                }
                for (int i = 0; i < (int)argc; i++) {
                    btm_add_instr(ctx, MIR_OP_move, base_var + i, popped[i + 1], -1,
                                  0, NULL, ip - 4);
                }
                free(popped);
                result = btm_push_new(ctx);
                if (result < 0) return NULL;
                btm_add_instr(ctx, (MIROpcode)method_op, result, base_var, -1,
                              (int64_t)argc, NULL, ip - 4);
            } else {
                int base_var = btm_new_var(ctx);
                if (base_var < 0) { free(popped); return NULL; }
                for (int i = 1; i <= argc; i++) {
                    if (btm_new_var(ctx) < 0) { free(popped); return NULL; }
                }
                for (int i = 0; i <= argc; i++) {
                    btm_add_instr(ctx, MIR_OP_move, base_var + i, popped[i], -1,
                                  0, NULL, ip - 4);
                }
                free(popped);
                result = btm_push_new(ctx);
                if (result < 0) return NULL;
                int64_t packed = ((int64_t)name_idx << 16) | (int64_t)argc;
                btm_add_instr(ctx, MIR_OP_call_method, result, base_var, -1,
                              packed, (void*)prog, ip - 4);
            }
            break;
        }

        case BC_CALL_ELEM: {
            /* Format: u16 argc.
             * Stack on entry: [..., this, key, arg1, ..., argN].
             * Layout passed to runtime: args_base[0]=this, args_base[1]=key,
             * args_base[2..argc+1]=argv. */
            uint16_t argc = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            int total = argc + 2;
            int *popped = (int *)malloc(sizeof(int) * total);
            if (!popped) return NULL;
            for (int i = total - 1; i >= 0; i--) {
                popped[i] = btm_pop(ctx);
                if (popped[i] < 0) { free(popped); return NULL; }
            }
            int base_var = btm_new_var(ctx);
            if (base_var < 0) { free(popped); return NULL; }
            for (int i = 1; i < total; i++) {
                if (btm_new_var(ctx) < 0) { free(popped); return NULL; }
            }
            for (int i = 0; i < total; i++) {
                btm_add_instr(ctx, MIR_OP_move, base_var + i, popped[i], -1,
                              0, NULL, ip - 3);
            }
            free(popped);
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_call_elem, result, base_var, -1,
                          (int64_t)argc, NULL, ip - 3);
            break;
        }

        case BC_NEW: {
            /* `new callee(args...)`: argc + 1 items on the stack
             * (callee at bottom, args above).  Pack as contiguous locals
             * [callee, arg1..argN] for the runtime helper. */
            uint16_t argc = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            int *popped = (int *)malloc(sizeof(int) * (argc + 1));
            if (!popped) return NULL;
            for (int i = argc; i >= 0; i--) {
                popped[i] = btm_pop(ctx);
                if (popped[i] < 0) { free(popped); return NULL; }
            }
            int base_var = btm_new_var(ctx);
            if (base_var < 0) { free(popped); return NULL; }
            for (int i = 1; i <= argc; i++) {
                if (btm_new_var(ctx) < 0) { free(popped); return NULL; }
            }
            for (int i = 0; i <= argc; i++) {
                btm_add_instr(ctx, MIR_OP_move, base_var + i, popped[i], -1,
                              0, NULL, ip - 3);
            }
            free(popped);
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_construct, result, base_var, -1,
                          (int64_t)argc, NULL, ip - 3);
            break;
        }

        case BC_THROW: {
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            /* Throw does not return control to the JIT — emit MIR_OP_throw
             * so the codegen can emit an unreachable after the call. */
            btm_add_instr(ctx, MIR_OP_throw, -1, a, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_TO_STRING: {
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_to_string, result, a, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_TO_NUMBER: {
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_to_number, result, a, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_TO_BOOL: {
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_to_bool, result, a, -1, 0, NULL, ip - 1);
            break;
        }

        /* In-place add: slot = slot + TOS (3 bytes: opcode + u16 slot) */
        case BC_ADD_SELF: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_new_var(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_load_local, result, (int)slot, -1, 0, NULL, ip - 3);
            int tmp = btm_new_var(ctx);
            if (tmp < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_add_i32, tmp, result, a, 0, NULL, ip - 3);
            btm_add_instr(ctx, MIR_OP_store_local, -1, tmp, (int)slot, 0, NULL, ip - 3);
            btm_push(ctx, tmp);
            break;
        }

        /* In-place mul: slot = slot * TOS (3 bytes: opcode + u16 slot) */
        case BC_MUL_SELF: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            result = btm_new_var(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_load_local, result, (int)slot, -1, 0, NULL, ip - 3);
            int tmp = btm_new_var(ctx);
            if (tmp < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_mul_i32, tmp, result, a, 0, NULL, ip - 3);
            btm_add_instr(ctx, MIR_OP_store_local, -1, tmp, (int)slot, 0, NULL, ip - 3);
            btm_push(ctx, tmp);
            break;
        }

        /* No-op for JIT: scope management is handled by the interpreter */
        case BC_SCOPE_ENTER:
        case BC_SCOPE_LEAVE:
            break;

        case BC_TYPEOF_VAR: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            int slot = (name_idx < (uint32_t)ctx->name_to_slot_cap)
                           ? ctx->name_to_slot[name_idx] : -1;
            if (slot >= 0) {
                /* Local slot: load the value from the spill area, then typeof.
                 * Avoids the name-based scope lookup (fails during JIT exec). */
                int val = btm_new_var(ctx);
                if (val < 0) return NULL;
                btm_add_instr(ctx, MIR_OP_load_local, val, slot, -1, 0, NULL, ip - 3);
                btm_add_instr(ctx, MIR_OP_typeof, result, val, -1, 0, NULL, ip - 3);
            } else {
                btm_add_instr(ctx, MIR_OP_typeof_var, result, -1, -1,
                              (int64_t)name_idx, (void*)prog, ip - 3);
            }
            break;
        }

        /* Variable declaration — skip (vars already allocated) */
        case BC_DECLARE_VAR: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            uint8_t kind = ip[2];
            ip += 3;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            (void)kind;
            /* Look up slot from pre-built name map. */
            int slot = -1;
            if (name_idx < ctx->name_to_slot_cap)
                slot = ctx->name_to_slot[name_idx];
            if (slot >= 0) {
                btm_add_instr(ctx, MIR_OP_store_local, -1, a, slot, 0, NULL, ip - 3);
                /* Propagate the stored value's type to the slot. */
                if (slot < 64)
                    ctx->slot_type[slot] = btm_var_is_float64(ctx, a) ? 2 : 0;
            } else {
                /* Not a function-local variable — fall back to name-based store.
                 * This should not happen for normal var declarations, but handle
                 * it gracefully to avoid losing the value. */
                LR_JIT_DBG("[MIR] BC_DECLARE_VAR: name_idx=%u slot=-1 (name-based fallback)\n", name_idx);
                btm_add_instr(ctx, MIR_OP_store_var, -1, a, -1,
                              (int64_t)name_idx, (void*)prog, ip - 3);
            }
            break;
        }

        /* Increment local: slot++, return old value */
        case BC_INC_LOCAL: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            a = btm_new_var(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_load_local, a, (int)slot, -1, 0, NULL, ip - 3);
            int one = btm_new_var(ctx);
            if (one < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_i32, one, -1, -1, 1, NULL, ip - 3);
            int inc = btm_new_var(ctx);
            if (inc < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_add_i32, inc, a, one, 0, NULL, ip - 3);
            btm_add_instr(ctx, MIR_OP_store_local, -1, inc, (int)slot, 0, NULL, ip - 3);
            btm_push(ctx, a);
            break;
        }

        case BC_INC_LOCAL_DISCARD: {
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            a = btm_new_var(ctx);
            if (a < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_load_local, a, (int)slot, -1, 0, NULL, ip - 3);
            int one = btm_new_var(ctx);
            if (one < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_i32, one, -1, -1, 1, NULL, ip - 3);
            int inc = btm_new_var(ctx);
            if (inc < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_add_i32, inc, a, one, 0, NULL, ip - 3);
            btm_add_instr(ctx, MIR_OP_store_local, -1, inc, (int)slot, 0, NULL, ip - 3);
            break;
        }

        case BC_LOAD_VAR: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            /* If this name maps to a function-scope local slot (recorded by
             * btm_build_name_map), emit a direct slot load instead of a
             * name-based scope-chain lookup.  This is essential: the JIT
             * prologue copies params+locals into the spill area, but
             * lr_jit_rt_load_var reads interp->current_scope, which during
             * JIT execution is the CALLER's scope — so a name-based lookup
             * would fail to find this function's own locals. */
            int slot = (name_idx < (uint32_t)ctx->name_to_slot_cap)
                           ? ctx->name_to_slot[name_idx] : -1;
            /* Function self-reference: the name is in the parent scope,
             * not the function's own spill area.  Must use load_var
             * (scope chain lookup) rather than load_local. */
            BCProgram *bprog = (BCProgram *)prog;
            int is_func_self = (bprog && bprog->func_name &&
                                name_idx < bprog->pool_count &&
                                bprog->pool[name_idx].u.str &&
                                strcmp(bprog->pool[name_idx].u.str, bprog->func_name) == 0);
            if (is_func_self) {
                btm_record_func_self_ref(ctx, result);
                btm_add_instr(ctx, MIR_OP_load_var, result, -1, -1,
                              (int64_t)name_idx, (void*)prog, ip - 3);
                btm_set_var_pool_name(ctx, result, name_idx);
            } else if (slot >= 0) {
                btm_record_scope_slot(ctx, result, slot);
                btm_add_instr(ctx, MIR_OP_load_local, result, slot, -1, 0, NULL, ip - 3);
            } else {
                btm_add_instr(ctx, MIR_OP_load_var, result, -1, -1,
                              (int64_t)name_idx, (void*)prog, ip - 3);
                btm_set_var_pool_name(ctx, result, name_idx);
            }
            break;
        }

        case BC_STORE_VAR: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            a = btm_pop(ctx);
            if (a < 0) return NULL;
            /* store_var pushes the assigned value back onto the stack
             * (matching the interpreter's BC_STORE_VAR semantics). */
            int slot = (name_idx < (uint32_t)ctx->name_to_slot_cap)
                           ? ctx->name_to_slot[name_idx] : -1;
            if (slot >= 0) {
                btm_add_instr(ctx, MIR_OP_store_local, -1, a, slot, 0, NULL, ip - 3);
            } else {
                btm_add_instr(ctx, MIR_OP_store_var, -1, a, -1,
                              (int64_t)name_idx, (void*)prog, ip - 3);
            }
            btm_push(ctx, a);
            break;
        }

        case BC_INC_VAR: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            /* Post-increment: returns old value, writes new value (old+1). */
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            int slot = (name_idx < (uint32_t)ctx->name_to_slot_cap)
                           ? ctx->name_to_slot[name_idx] : -1;
            if (slot >= 0) {
                /* Local slot: emit load_local + add + store_local so the
                 * increment operates on the JIT spill area directly, avoiding
                 * the name-based scope lookup (which fails during JIT exec). */
                btm_add_instr(ctx, MIR_OP_load_local, result, slot, -1, 0, NULL, ip - 3);
                int one = btm_new_var(ctx);
                if (one < 0) return NULL;
                btm_add_instr(ctx, MIR_OP_const_i32, one, -1, -1, 1, NULL, ip - 3);
                int inc = btm_new_var(ctx);
                if (inc < 0) return NULL;
                btm_add_instr(ctx, MIR_OP_add_i32, inc, result, one, 0, NULL, ip - 3);
                btm_add_instr(ctx, MIR_OP_store_local, -1, inc, slot, 0, NULL, ip - 3);
            } else {
                btm_add_instr(ctx, MIR_OP_inc_var, result, -1, -1,
                              (int64_t)name_idx, (void*)prog, ip - 3);
            }
            break;
        }

        case BC_PUSH_THIS: {
            /* Load `this` via runtime callback (resolves scope chain). */
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_load_this, result, -1, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_PUSH_FLOAT64: {
            uint16_t pool_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_f64, result, -1, -1,
                          (int64_t)pool_idx, (void*)prog, ip - 3);
            btm_set_var_type(ctx, result, 2);  /* float64 */
            /* Record f64 constant value immediately for folding */
            if (pool_idx < prog->pool_count &&
                prog->pool[pool_idx].kind == BC_POOL_FLOAT64) {
                ctx->mir->const_f64_values[result - MIR_VAR_BASE] =
                    prog->pool[pool_idx].u.f64;
            }
            break;
        }

        case BC_PUSH_STRING: {
            uint16_t pool_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_const_string, result, -1, -1,
                          (int64_t)pool_idx, (void*)prog, ip - 3);
            btm_set_var_type(ctx, result, 3);  /* string */
            break;
        }

        case BC_NEW_OBJECT: {
            /* Allocate a new empty object via runtime callback. */
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_new_object, result, -1, -1, 0, NULL, ip - 1);
            break;
        }

        case BC_NEW_ARRAY: {
            /* Stack layout on entry: v0 v1 ... vN-1 (N items).
             * Pop them into contiguous locals so the runtime helper can
             * iterate them as args_base[0..N-1]. */
            uint16_t n = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            int *popped = (int *)malloc(sizeof(int) * (n ? n : 1));
            if (!popped) return NULL;
            for (int i = (int)n - 1; i >= 0; i--) {
                popped[i] = btm_pop(ctx);
                if (popped[i] < 0) { free(popped); return NULL; }
            }
            int base_var = btm_new_var(ctx);
            if (base_var < 0) { free(popped); return NULL; }
            for (int i = 1; i < (int)n; i++) {
                if (btm_new_var(ctx) < 0) { free(popped); return NULL; }
            }
            for (int i = 0; i < (int)n; i++) {
                btm_add_instr(ctx, MIR_OP_move, base_var + i, popped[i], -1,
                              0, NULL, ip - 3);
            }
            free(popped);
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_new_array, result, base_var, -1,
                          (int64_t)n, NULL, ip - 3);
            break;
        }

        /* -- Property / element access and mutation --------------------
         * All route through runtime callbacks.  The JIT keeps native
         * code for the surrounding arithmetic / control flow, but
         * defers object model work (shape lookup, refcount, prototypes)
         * to the interpreter helpers. */
        case BC_DEF_PROP:
        case BC_SET_PROP: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            /* Stack: [..., obj, val].  Semantics: obj[name] = val; push val.
             * Pack into contiguous locals [obj_in, val_in, out] and pass
             * the base pointer to the runtime (4-arg ABI limit). */
            int val_v = btm_pop(ctx);
            int obj_v = btm_pop(ctx);
            if (val_v < 0 || obj_v < 0) return NULL;
            int base_var = btm_new_var(ctx);  /* obj slot */
            if (base_var < 0) return NULL;
            if (btm_new_var(ctx) < 0) return NULL;  /* val slot */
            int out_var = btm_new_var(ctx);  /* result slot */
            if (out_var < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_move, base_var + 0, obj_v, -1, 0, NULL, ip - 3);
            btm_add_instr(ctx, MIR_OP_move, base_var + 1, val_v, -1, 0, NULL, ip - 3);
            /* dst = out_var (informational: where result lands).
             * src1 = base_var (args_base pointer used by codegen). */
            btm_add_instr(ctx, MIR_OP_set_prop, out_var, base_var, -1,
                          (int64_t)name_idx, (void*)prog, ip - 3);
            /* BC_DEF_PROP (object literal) must push the OBJECT so that
             * chained property definitions ({a:1, b:2}) keep operating on
             * the same object.  BC_SET_PROP (assignment expr) pushes val.
             * The runtime helper writes val to args_base[2]; for DEF_PROP we
             * override the pushed result with the object itself. */
            if (opcode == BC_DEF_PROP) {
                btm_add_instr(ctx, MIR_OP_move, out_var, obj_v, -1, 0, NULL, ip - 3);
            }
            btm_push(ctx, out_var);
            break;
        }

        case BC_DEF_ELEM:
        case BC_SET_ELEM: {
            /* Stack: [..., obj, key, val] (val on top).
             * Use the contiguous-move pattern so the runtime helper can
             * read args_base[0..2] = [obj, key, val]. */
            int val_v = btm_pop(ctx);
            int key_v = btm_pop(ctx);
            int obj_v = btm_pop(ctx);
            if (val_v < 0 || key_v < 0 || obj_v < 0) return NULL;
            int base_var = btm_new_var(ctx);
            if (base_var < 0) return NULL;
            if (btm_new_var(ctx) < 0) return NULL;  /* key slot */
            if (btm_new_var(ctx) < 0) return NULL;  /* val slot */
            btm_add_instr(ctx, MIR_OP_move, base_var + 0, obj_v, -1, 0, NULL, ip - 1);
            btm_add_instr(ctx, MIR_OP_move, base_var + 1, key_v, -1, 0, NULL, ip - 1);
            btm_add_instr(ctx, MIR_OP_move, base_var + 2, val_v, -1, 0, NULL, ip - 1);
            result = btm_new_var(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_set_elem, result, base_var, -1, 0, NULL, ip - 1);
            btm_push(ctx, result);
            break;
        }

        case BC_GET_PROP:
        case BC_LOAD_PROP: {
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            /* BC_LOAD_PROP has an extra u16 slot operand that we discard:
             * the slot is a cache hint, not required for correctness. */
            if (opcode == BC_LOAD_PROP) {
                uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
                (void)slot;
                ip += 2;
            }
            int obj_v = btm_pop(ctx);
            if (obj_v < 0) return NULL;
            /* Runtime lr_jit_rt_get_prop expects contiguous args_base:
             *   args_base[0] = obj (input), args_base[1] = result (output).
             * Allocate [obj_in, out] contiguously and use base_var as src1. */
            int base_var = btm_new_var(ctx);   /* obj slot */
            if (base_var < 0) return NULL;
            int out_var = btm_new_var(ctx);    /* result slot (= base_var + 1) */
            if (out_var < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_move, base_var + 0, obj_v, -1, 0, NULL, ip - 3);
            btm_add_instr(ctx, MIR_OP_get_prop, out_var, base_var, -1,
                          (int64_t)name_idx, (void*)prog, ip - 3);
            btm_set_var_pool_name(ctx, out_var, name_idx);
            btm_push(ctx, out_var);
            break;
        }

        case BC_LOAD_PROP_ADD: {
            /* Fused load+get+add: u16 slot, u16 pool.  Slot is a cache hint.
             * Stack on entry: [..., obj, rhs]; result = obj[prop] + rhs.
             * Create 3 contiguous MIR vars: [obj_in, rhs_in, result].
             * src1=obj_in, src2=rhs_in, dst=result_in (==src1+2). */
            uint16_t slot = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            (void)slot;
            int rhs_v = btm_pop(ctx);
            int obj_v = btm_pop(ctx);
            if (rhs_v < 0 || obj_v < 0) return NULL;
            /* Copy obj and rhs into contiguous buffer. */
            int base_v = btm_new_var(ctx);
            if (base_v < 0) return NULL;
            int rhs_v2 = btm_new_var(ctx);
            if (rhs_v2 < 0) return NULL;
            int result_v = btm_new_var(ctx);
            if (result_v < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_move, base_v, obj_v, -1, 0, NULL, ip - 5);
            btm_add_instr(ctx, MIR_OP_move, rhs_v2, rhs_v, -1, 0, NULL, ip - 5);
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_add_prop, base_v, rhs_v2, result,
                          (int64_t)name_idx, (void*)prog, ip - 5);
            break;
        }

        case BC_GET_ELEM: {
            int key_v = btm_pop(ctx);
            int obj_v = btm_pop(ctx);
            if (key_v < 0 || obj_v < 0) return NULL;
            result = btm_push_new(ctx);
            if (result < 0) return NULL;
            btm_add_instr(ctx, MIR_OP_get_elem, result, obj_v, key_v, 0, NULL, ip - 1);
            break;
        }

        case BC_DELETE_PROP: {
            /* delete obj.name — push boolean.  We re-use set_prop's runtime
             * signature shape but it's delete semantics; route through
             * get_prop with a special flag is misleading, so bail. */
            uint16_t name_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            (void)name_idx;
            int obj_v = btm_pop(ctx);
            (void)obj_v;
            /* Bail: delete requires shape mutation, kept on interpreter. */
            return NULL;
        }

        case BC_DELETE_ELEM: {
            int key_v = btm_pop(ctx);
            int obj_v = btm_pop(ctx);
            (void)key_v; (void)obj_v;
            return NULL;
        }

        /* Iteration state lives in the interpreter — bail out. */
        case BC_ITER_INIT:
        case BC_ITER_NEXT:
        case BC_ITER_CLOSE:
            return NULL;

        case BC_EVAL_NODE_POP: {
            uint16_t pool_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            (void)pool_idx;
            break;
        }

        case BC_EVAL_NODE: {
            uint16_t pool_idx = (uint16_t)(ip[0] | (ip[1] << 8));
            ip += 2;
            int result = btm_push_new(ctx);
            if (result < 0) return NULL;
            (void)pool_idx;
            break;
        }

        default:
            /* Unsupported opcode — abort compilation, fall back to interpreter */
            LR_JIT_DBG("[MIR-BAILOUT] UNSUPPORTED opcode=%d (0x%02X) at byte_offset=%td ip=%p end=%p\n",
                    opcode, opcode, ip - 1 - prog->code, (void*)ip, (void*)end);
            return NULL;
    }
    return ip;
}

MIRProgram *mir_compile_bytecode(Interpreter *interp, LRProgram *prog) {
    if (!prog || !prog->code) return NULL;
    BCProgram *bc = (BCProgram *)prog;
    uint8_t *ip = bc->code;
    uint8_t *end = ip + bc->code_len;

    /* Reserve space for variable declarations (let/var)
     * Use larger multiplier for recursive functions and complex code */
    int reserve_vars = (int)bc->code_len * 4 + 256;
    if (reserve_vars < 1024) reserve_vars = 1024;
    MIRProgram *mir = mir_create(reserve_vars);
    if (!mir) {
        mir_log_error("[MIR-ERR] mir_create failed for prog#%d code_len=%d",
                    prog->prog_id, (int)bc->code_len);
        return NULL;
    }

    BytecodeToMIR *ctx = btm_create(mir, 64);
    if (!ctx) {
        mir_free(mir);
        mir_log_error("[MIR-ERR] btm_create failed for prog#%d", prog->prog_id);
        return NULL;
    }

    /* Set base_ip so all offset calculations are relative to bytecode start */
    ctx->base_ip = bc->code;

    /* Apply type specialization hint from PGO runtime data.
     * spec_dominant: 0=int32, 1=float64, 2=mixed, 3=other. */
    if (bc->spec_dominant == 0 && bc->spec_int32_count > 2)
        ctx->type_hint = 1;
    else if (bc->spec_dominant == 1 && bc->spec_float64_count > 2)
        ctx->type_hint = 2;

    /* First pass: build name→slot mapping from bytecode for BC_DECLARE_VAR.
     * This is needed because the MIR compiler doesn't have access to the
     * bytecode compiler's local_names table — we reconstruct it by scanning
     * the emitted bytecode. */
    btm_build_name_map(ctx, bc);
    /* Second pass: map function parameter names to slots 1..nparams so
     * BC_LOAD_VAR of a param becomes a direct load_local (no runtime load_var
     * scope-chain walk).  Only safe when the function is slot-safe. */
    btm_map_params_to_slots(ctx, bc);
    /* Note: we do NOT map the function's own name to name_to_slot here.
     * Function names live in the parent scope, not the function's own
     * spill area, so BC_LOAD_VAR of the function name must use load_var
     * (runtime scope chain lookup) rather than load_local.  Instead,
     * the function self-reference is detected and recorded directly in
     * the BC_LOAD_VAR handler below when func_name/func_name_slot is set. */

    /* Process bytecode instructions one at a time.
     * btm_compile_instr returns the updated ip on success, NULL on bail or STOP.
     * Bail (e.g., unsupported opcode like BC_LOAD_VAR) is expected and graceful —
     * return partial MIR so the caller can decide to skip JIT for this program. */
    while (ip < end) {
        LR_JIT_DBG("[MIR-PROG] prog#%d offset=%ld opcode=%d\n",
                    prog->prog_id, (long)(ip - bc->code), (int)*ip);
        uint8_t *new_ip = btm_compile_instr(ctx, interp, bc, ip, end);
        if (!new_ip) {
            /* STOP reached — normal termination */
            if (ip < end && *ip == BC_STOP) {
                break;
            }
            /* Bail-out: unsupported opcode — return partial MIR, not an error */
            if (lr_debug_jit_enabled()) {
                LR_JIT_DBG("[MIR-BAIL] Partial MIR at prog#%d: %d instructions, bailed at offset %ld\n",
                        prog->prog_id, mir->num_instructions, (long)(ip - bc->code));
                for (int i = 0; i < mir->num_instructions; i++) {
                    MIRInstr *inst = &mir->instructions[i];
                    LR_JIT_DBG("  [%2d] %-15s dst=%3d src1=%3d src2=%3d imm=%10lld bc=%4d\n",
                            i, mir_opcode_name(inst->opcode),
                            inst->dst, inst->src1, inst->src2,
                            (long long)inst->imm,
                            (mir->bc_offsets ? mir->bc_offsets[i] : -1));
                }
            }
            mir->is_partial = 1;
            btm_free(ctx);
            return mir;
        }
        ip = new_ip;
    }

    /* DEBUG: Dump MIR after main scan, before recovery loop */
    if (lr_debug_jit_enabled()) {
        LR_JIT_DBG("[MIR-MAIN] prog#%d: %d instructions after main scan\n", prog->prog_id, mir->num_instructions);
        for (int i = 0; i < mir->num_instructions; i++) {
            MIRInstr *inst = &mir->instructions[i];
            LR_JIT_DBG("  [%2d] %-15s dst=%3d src1=%3d src2=%3d imm=%10lld bc=%4d\n",
                    i, mir_opcode_name(inst->opcode),
                    inst->dst, inst->src1, inst->src2,
                    (long long)inst->imm,
                    (mir->bc_offsets ? mir->bc_offsets[i] : -1));
        }
        /* Dump bc_offsets array */
        LR_JIT_DBG("[MIR-MAIN] bc_offsets array:\n");
        for (int i = 0; i < mir->num_instructions && i < 50; i++) {
            if (mir->bc_offsets && i < mir->capacity) {
                LR_JIT_DBG("  bc_offsets[%d] = %d\n", i, mir->bc_offsets[i]);
            }
        }
    }

    /* If loop_skip_mode is still active at end-of-function, there were
     * forward jumps (if/else without else branch) whose skipped range
     * was never re-processed. Re-enter from loop_skip_bc_off in non-skip
     * mode to capture the live fall-through and return instructions. */
    if (ctx->loop_skip_mode) {
        uint8_t *skip_ip = bc->code + ctx->loop_skip_bc_off;
        int skipped_instrs = mir->num_instructions;
        /* Dump bytecode around skip region for debugging */
        LR_JIT_DBG("[MIR-SKIP] prog#%d: bc_code_len=%d loop_skip_bc_off=%d skip_ip=%p\n",
                   prog->prog_id, bc->code_len, ctx->loop_skip_bc_off, (void*)skip_ip);
        {
            int dump_start = ctx->loop_skip_bc_off - 15;
            if (dump_start < 0) dump_start = 0;
            int dump_len = 30;
            LR_JIT_DBG("[MIR-SKIP] prog#%d: bytecode[%d..%d]:\n", prog->prog_id, dump_start, dump_start + dump_len - 1);
            for (int i = dump_start; i < dump_start + dump_len && i < bc->code_len; i++) {
                if (i % 16 == 0) LR_JIT_DBG("  %04x: ", i);
                LR_JIT_DBG("%02x ", bc->code[i]);
                if (i % 16 == 15) LR_JIT_DBG("\n");
            }
            LR_JIT_DBG("\n");
        }
        LR_JIT_DBG("[MIR-SKIP] prog#%d: re-processing skipped range from bc_off=%d (had %d instrs)\n",
                   prog->prog_id, ctx->loop_skip_bc_off, skipped_instrs);
        LR_JIT_DBG("[MIR-SKIP] prog#%d: sp restored to %d\n", prog->prog_id, ctx->sp);
        ctx->loop_skip_mode = 0;
        ctx->sp = ctx->loop_skip_sp;
        /* Allow skip mode during true/false branch processing, but disable
         * it during post-merge to prevent while-loop exits from breaking. */
        ctx->allow_skip_mode = 1;
        /* Ternary detection: scan backward from skip_ip to find BC_JUMP_IF_FALSE,
         * then scan forward through true branch to find BC_JUMP.
         * skip_ip points to the first byte AFTER BC_JUMP_IF_FALSE (which is the
         * start of the true branch). */
        int is_ternary = 0;
        int true_branch_end = -1;
        int false_branch_start = -1;
        int merge_bc_off = -1;
        int jif_bc_off = -1;
        /* Scan backward to find BC_JUMP_IF_FALSE.
         * Since loop_skip_bc_off points exactly 5 bytes after JUMP_IF_FALSE
         * (the instruction is always 5 bytes: 1 opcode + 4 offset), we can
         * directly check the instruction at skip_ip - 5. We also validate
         * the opcode to handle any edge cases. */
        {
            uint8_t *jif_ip = skip_ip - 5;
            if (jif_ip >= bc->code && jif_ip < skip_ip) {
                uint8_t op = *jif_ip;
                if (op == BC_JUMP_IF_FALSE || op == BC_JUMP_IF_TRUE ||
                    op == BC_JUMP_IF_FALSE_KEEP || op == BC_JUMP_IF_TRUE_KEEP ||
                    op == BC_JUMP_IF_NOT_NULLISH) {
                    jif_bc_off = (int)(jif_ip - bc->code);
                    int32_t jif_offset = (int32_t)(jif_ip[1] | (jif_ip[2] << 8) |
                                                    (jif_ip[3] << 16) | (jif_ip[4] << 24));
                    if (jif_offset >= 0) {
                        false_branch_start = jif_bc_off + 5 + jif_offset;
                    }
                }
            }
        }
        LR_JIT_DBG("[MIR-SKIP] prog#%d: found BC_JUMP_IF_FALSE at bc_off=%d, false_start=%d\n",
                   prog->prog_id, jif_bc_off, false_branch_start);
        LR_JIT_DBG("[MIR-SKIP] prog#%d: end=%d true_branch_end=%d merge_bc_off=%d\n",
                   prog->prog_id, (int)(end - bc->code), true_branch_end, merge_bc_off);
        /* Scan true branch to find BC_JUMP and compute merge point.
         * If no explicit BC_JUMP is found, the true branch ends at the
         * false_branch_start (end of function or next control flow). */
        {
            uint8_t *scan = skip_ip;
            while (scan < end) {
                uint8_t op = *scan;
                if (op == BC_JUMP) {
                    int32_t off = (int32_t)(scan[1] | (scan[2] << 8) |
                                            (scan[3] << 16) | (scan[4] << 24));
                    if (off >= 0) {
                        merge_bc_off = (int)(scan - bc->code) + 5 + off;
                        true_branch_end = (int)(scan - bc->code) + 5;
                        is_ternary = 1;
                    }
                    break;
                }
                int sz = btm_instr_size(scan);
                if (sz <= 0) break;
                scan += sz;
            }
            /* If no BC_JUMP found but we have a false branch start,
             * treat the true branch as ending at the function end.
             * Do NOT set merge_bc_off to end (which would skip POST-MERGE).
             * Instead, leave merge_bc_off=-1 so POST-MERGE starts from skip_ip,
             * which is correct for if-without-else: the "false branch" is empty
             * and code after the if (while loops, return, etc.) falls through. */
            if (!is_ternary && false_branch_start >= 0) {
                true_branch_end = (int)(end - bc->code);
                /* Keep merge_bc_off = -1 for no-else-if: POST-MERGE starts
                 * at skip_ip, treating it as fall-through code with no false branch. */
                LR_JIT_DBG("[MIR-SKIP] prog#%d: NO_BC_JUMP: true_end=%d merge=KEEP(-1)\n",
                           prog->prog_id, true_branch_end);
            }
        }
        LR_JIT_DBG("[MIR-SKIP] prog#%d: is_ternary=%d true_end=%d false_start=%d merge=%d\n",
                   prog->prog_id, is_ternary, true_branch_end, false_branch_start, merge_bc_off);
        /* For if-without-else patterns (no BC_JUMP in true branch):
         * FALSE branch is empty, code after the if is fall-through (POST-MERGE).
         * Process POST-MERGE starting from skip_ip (not from merge_bc_off)
         * when merge_bc_off was not set by an explicit BC_JUMP. */
        if (!is_ternary && merge_bc_off < 0 && false_branch_start >= 0) {
            /* In NO-ELSE-IF fall-through processing, we must compile ALL subsequent
             * code without re-entering skip mode. The fall-through is continuation
             * of the program, not dead code to skip. Save and disable allow_skip_mode
             * to prevent forward jumps (e.g. while loop condition) from entering
             * skip mode and skipping the loop body. */
            int saved_allow_skip = ctx->allow_skip_mode;
            ctx->allow_skip_mode = 0;
            LR_JIT_DBG("[MIR-SKIP] prog#%d: NO-ELSE-IF: fall-through from skip_ip=%d (skip_bc_off=%d)\n",
                       prog->prog_id, (int)(skip_ip - bc->code), ctx->loop_skip_bc_off);
            uint8_t *fallthrough_ip = skip_ip;
            while (fallthrough_ip < end) {
                int bc_off_here = (int)(fallthrough_ip - bc->code);
                uint8_t op = *fallthrough_ip;
                LR_JIT_DBG("[MIR-SKIP] prog#%d: fall-through bc_off=%d opcode=%d mir_instrs_before=%d\n",
                           prog->prog_id, bc_off_here, op, mir->num_instructions);
                uint8_t *next = btm_compile_instr(ctx, interp, bc, fallthrough_ip, end);
                if (!next) {
                    LR_JIT_DBG("[MIR-SKIP] prog#%d: fall-through FAILED at bc_off=%d\n",
                               prog->prog_id, bc_off_here);
                    break;
                }
                int new_instrs = mir->num_instructions - skipped_instrs;
                LR_JIT_DBG("[MIR-SKIP] prog#%d: -> next=%d sp=%d (mir_instrs=%d new=%d)\n",
                           prog->prog_id, (int)(next - bc->code), ctx->sp, mir->num_instructions, new_instrs);
                /* Dump last few MIR instructions */
                if (new_instrs > 0 && lr_debug_jit_enabled()) {
                    for (int k = mir->num_instructions - new_instrs; k < mir->num_instructions; k++) {
                        MIRInstr *inst = &mir->instructions[k];
                        int bc_off = (mir->bc_offsets ? mir->bc_offsets[k] : -1);
                        LR_JIT_DBG("[MIR-SKIP]   [%2d] %-15s imm=%lld bc_off=%d\n",
                                   k, mir_opcode_name(inst->opcode), (long long)inst->imm, bc_off);
                    }
                }
                fallthrough_ip = next;
            }
            ctx->allow_skip_mode = saved_allow_skip;
        } else if (false_branch_start >= 0 && merge_bc_off >= 0) {
            /* Process TRUE branch (from skip_ip to true_branch_end).
             * Save sp before true branch so we can restore it for the false branch.
             * Both branches must produce the same number of stack values so that
             * the merge point has a single canonical result. */
            int sp_before_ternary = ctx->sp;
            LR_JIT_DBG("[MIR-SKIP] prog#%d: TRUE branch: [%d, %d), sp=%d\n",
                       prog->prog_id, (int)(skip_ip - bc->code), true_branch_end, sp_before_ternary);
        uint8_t *true_ip = skip_ip;
        while (true_ip < bc->code + true_branch_end) {
            int bc_off_here = (int)(true_ip - bc->code);
            LR_JIT_DBG("[MIR-SKIP] prog#%d: true-branch bc_off=%d opcode=%d\n",
                       prog->prog_id, bc_off_here, *true_ip);
            uint8_t *next = btm_compile_instr(ctx, interp, bc, true_ip, end);
            if (!next) {
                LR_JIT_DBG("[MIR-SKIP] prog#%d: true-branch FAILED at bc_off=%d\n",
                           prog->prog_id, bc_off_here);
                break;
            }
            LR_JIT_DBG("[MIR-SKIP] prog#%d: -> next=%d sp=%d\n",
                       prog->prog_id, (int)(next - bc->code), ctx->sp);
            true_ip = next;
        }
        /* Process FALSE branch (from false_branch_start to merge).
         * Restore sp to pre-ternary state so the false branch starts with
         * the same stack context as the true branch. */
        LR_JIT_DBG("[MIR-SKIP] prog#%d: FALSE branch: [%d, %d), sp_before=%d sp_now=%d\n",
                   prog->prog_id, false_branch_start, merge_bc_off, sp_before_ternary, ctx->sp);
        if (false_branch_start >= 0 && merge_bc_off >= 0) {
            ctx->sp = sp_before_ternary;
            uint8_t *false_ip = bc->code + false_branch_start;
            while (false_ip < bc->code + merge_bc_off) {
                int bc_off_here = (int)(false_ip - bc->code);
                LR_JIT_DBG("[MIR-SKIP] prog#%d: false-branch bc_off=%d opcode=%d\n",
                           prog->prog_id, bc_off_here, *false_ip);
                uint8_t *next = btm_compile_instr(ctx, interp, bc, false_ip, end);
                if (!next) {
                    LR_JIT_DBG("[MIR-SKIP] prog#%d: false-branch FAILED at bc_off=%d\n",
                               prog->prog_id, bc_off_here);
                    break;
                }
                LR_JIT_DBG("[MIR-SKIP] prog#%d: -> next=%d sp=%d\n",
                           prog->prog_id, (int)(next - bc->code), ctx->sp);
                false_ip = next;
            }
        }
        } /* end else-if (ternary true/false branch processing) */
        /* DEBUG: Dump MIR after recovery loop */
        if (lr_debug_jit_enabled()) {
            LR_JIT_DBG("[MIR-RECOVERY] prog#%d: %d instructions after recovery\n", prog->prog_id, mir->num_instructions);
            for (int i = 0; i < mir->num_instructions; i++) {
                MIRInstr *inst = &mir->instructions[i];
                LR_JIT_DBG("  [%2d] %-15s dst=%3d src1=%3d src2=%3d imm=%10lld bc=%4d\n",
                        i, mir_opcode_name(inst->opcode),
                        inst->dst, inst->src1, inst->src2,
                        (long long)inst->imm,
                        (mir->bc_offsets ? mir->bc_offsets[i] : -1));
            }
            /* Dump bc_offsets array after recovery */
            LR_JIT_DBG("[MIR-RECOVERY] bc_offsets array after recovery:\n");
            for (int i = 0; i < mir->num_instructions && i < 50; i++) {
                if (mir->bc_offsets && i < mir->capacity) {
                    LR_JIT_DBG("  bc_offsets[%d] = %d\n", i, mir->bc_offsets[i]);
                }
            }
        }
    }

    btm_free(ctx);

    /* Apply MIR optimizations (V8-inspired) */
    if (lr_debug_jit_enabled()) mir_dump(mir, "BEFORE_COND_MERGE");
    mir_optimize_conditional_merge(mir);
    if (lr_debug_jit_enabled()) mir_dump(mir, "AFTER_COND_MERGE");
    mir_optimize_constants(mir);
    if (lr_debug_jit_enabled()) mir_dump(mir, "AFTER_CONST_FOLD");
    mir_optimize_atomics_cache(mir);
    mir_optimize_method_cache(mir);
    mir_optimize_private_field_cache(mir, prog);
    mir_optimize_prop_shape_cache(mir, prog);
    mir_optimize_dead_code(mir);
    if (lr_debug_jit_enabled()) mir_dump(mir, "AFTER_DCE");

    return mir;
}
