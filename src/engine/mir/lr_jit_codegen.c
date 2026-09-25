/*
 * LR_JS - JIT Code Generator: MIR -> native code via SLJIT
 *
 * Function signature: void (*)(Interpreter *, uint8_t *, LRValue *)
 *   Arg0 (RCX) = interp
 *   Arg1 (RDX) = ip (bytecode start for bailout)
 *   Arg2 (R8)  = result (LRValue * output)
 *
 * Local variables stored in a spill array at SP + SLJIT_LOCALS_OFFSET + slot*16.
 * Note: SLJIT's ADJUST_LOCAL_OFFSET auto-adds SLJIT_LOCALS_OFFSET to all
 *       SLJIT_MEM1(SLJIT_SP) accesses, so our offsets should be relative to
 *       the start of the local spill area (i.e., offset 0 = SLJIT_LOCALS_OFFSET).
 * Result pointer saved at SP offset past all locals.
 */

#include "lr_jit_mir.h"
#include "lr_interp.h"
#include <sljitLir.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* SLJIT_SKIP_CHECKS is not exported in public headers; redefine for our use. */
#ifndef SLJIT_SKIP_CHECKS
#define SLJIT_SKIP_CHECKS(compiler) (compiler)->skip_checks = 1
#endif

#define LRVALUE_SIZE 16
#define MAX_LOCALS LR_JIT_MAX_LOCALS
/* Need at least 4 scratch registers for sljit_emit_icall argument passing
 * (SLJIT_R0..R3 are used to set up 4 C-ABI call arguments). */
#define NUM_SCRATCH_REGS 4
/* Callee-saved registers we ask SLJIT to preserve across the generated
 * function.  We explicitly save the 3 incoming arg registers into S0..S2
 * (rbx, rsi, rdi on _WIN64) ourselves.  With 4 scratches + 4 saveds = 8,
 * well under SLJIT_NUMBER_OF_REGISTERS (12 on x64). */
#define NUM_SAVED_REGS 4
/* TMP_REG1 is a SLJIT internal temporary register (SLJIT_NUMBER_OF_REGISTERS+2),
 * mapped to R9 on x86_64 Windows. Defined in sljitNativeX86_64.c but not in
 * the public header, so we define it locally. */
#ifndef TMP_REG1
#define TMP_REG1 (SLJIT_NUMBER_OF_REGISTERS + 2)
#endif

#define CHECK_SLJIT_ERR(msg) do { \
    if ((compiler)->error != SLJIT_SUCCESS) { \
        LR_JIT_ERR("[JIT-ERR] %s: err=%d line=%d\n", msg, (compiler)->error, __LINE__); \
        sljit_free_compiler(compiler); return (compiler)->error; \
    } \
} while(0)

#define CHECK_SLJIT_NORC(msg) do { \
    if ((compiler)->error != SLJIT_SUCCESS) { \
        LR_JIT_ERR("[JIT-ERR] %s: err=%d line=%d\n", msg, (compiler)->error, __LINE__); \
        sljit_free_compiler(compiler); return NULL; \
    } \
} while(0)

#define CHECK_SLJIT_VOID(msg) do { \
    if ((compiler)->error != SLJIT_SUCCESS) { \
        LR_JIT_ERR("[JIT-ERR] %s: err=%d line=%d\n", msg, (compiler)->error, __LINE__); \
        sljit_free_compiler(compiler); return; \
    } \
} while(0)

/* MEM(offset): memory operand relative to SLJIT_LOCALS_OFFSET start.
 * SLJIT's ADJUST_LOCAL_OFFSET auto-adds SLJIT_LOCALS_OFFSET, so we pass
 * raw offsets (0, 16, 32, ...) and SLJIT handles the rest. */
#define MEM(off) SLJIT_MEM1(SLJIT_SP), (off)

/* Result pointer stored at SP offset past all locals.
 * SLJIT_ADJUST_LOCAL_OFFSET will add SLJIT_LOCALS_OFFSET automatically. */
#define RESULT_PTR_OFFSET (MAX_LOCALS * LRVALUE_SIZE)

/* Saved caller scope pointer — one pointer-sized slot after result ptr.
 * Used for V8-style per-call-frame scope pinning so that recursive JIT
 * calls see the outer function's scope, not the inner one. */
#define SAVED_SCOPE_OFFSET (RESULT_PTR_OFFSET + sizeof(void *))

/* Offsets within Interpreter struct (x64, 8-byte pointers): */
#define INTERP_CURRENT_SCOPE_OFF 16
#define INTERP_GLOBAL_SCOPE_OFF  8
#define INTERP_JIT_ARGS_BASE_OFF offsetof(Interpreter, jit_args_base)

/* Offsets within InterpScope struct: */
#define SCOPE_VALUES_OFF 16
#define SCOPE_COUNT_OFF  24

/* LRShape field offsets (for inlined shape validation):
 * ref_count(4) + pad(4) + prop_name(8) + prev(8) = 24 → next at 24
 * + slot_index(4) + hash(4) = 32 → version at 40 */
#define SHAPE_OFFSET_SHAPE  24   /* LRShape* next at offset 24 */
#define SHAPE_OFFSET_VERSION 40  /* uint32_t version at offset 40 */

/* LRObject field offsets (x64, 8-byte aligned):
 * ref_count(4) + type(4) = 8 → shape(ptr) at 8
 * shape(8) + class_def(8) = 16 → props(ptr) at 24
 * ... → is_exotic(uint8) at offset 104 */
#define LR_OBJECT_OFFSET_SHAPE      8   /* LRShape* shape */
#define LR_OBJECT_OFFSET_PROPS      24  /* LRValue* props */
#define LR_OBJECT_OFFSET_IS_EXOTIC  104 /* uint8_t is_exotic */

static void emit_set_local_tag(struct sljit_compiler *compiler, int slot,
                               sljit_sw local_base, int tag) {
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_set_local_tag ENTER pre_err=%d slot=%d tag=%d\n",
                   compiler->error, slot, tag);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)tag);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_set_local_tag mov32_imm err=%d slot=%d tag=%d op=%d\n",
                   compiler->error, slot, tag, SLJIT_MOV32);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + slot * LRVALUE_SIZE), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_set_local_tag mov32_mem err=%d slot=%d tag=%d\n",
                   compiler->error, slot, tag);
    }
}

/* Emit a boolean (0/1 with LR_TYPE_BOOL tag) result from a comparison.
 * Pattern: cmp flags �?conditional jump to true_case �?false path �? * jump done �?true_case label �?true path �?done label.
 *
 * IMPORTANT: We must NOT use sljit_emit_cmp() here �?that SLJIT function
 * internally calls sljit_emit_jump() and returns the jump struct, but we
 * discard the return value, leaving an unlabelled jump in the compiler's
 * jump list.  SLJIT's reduce_code_size then dereferences jump->u.label->size
 * on the unlabelled jump and segfaults.
 *
 * Instead we emit the compare manually via sljit_emit_op2u with the flag bit,
 * then control the jump emission ourselves. */
/* Emit a bailout sequence: write tag=-1 to *result and return.
 * Called by checked arithmetic ops when an int32 overflow is detected, so the
 * interpreter re-runs the function with IEEE-754 double semantics. */
static void emit_bailout(struct sljit_compiler *compiler, sljit_sw result_ptr_offset) {
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, MEM(result_ptr_offset));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_bailout mov_r1 err=%d\n", compiler->error);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, -1);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_bailout mov32_imm_-1 err=%d\n", compiler->error);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R1), 0, SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_bailout mov32_mem_tag err=%d\n", compiler->error);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_bailout mov32_imm_0 err=%d\n", compiler->error);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R1), 4, SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_bailout mov32_mem_val err=%d\n", compiler->error);
        return;
    }
    sljit_emit_return_void(compiler);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_bailout return_void err=%d\n", compiler->error);
    }
}

/* 32-bit binary arithmetic (ADD/SUB) with overflow detection.  On overflow
 * (result does not fit in int32, so the JS number would need double), bail
 * out to the interpreter instead of silently wrapping. */
static void emit_i32_bin_checked(struct sljit_compiler *compiler, sljit_s32 op32,
                                 int dst, int src1, int src2,
                                 sljit_sw local_base, sljit_sw result_ptr_offset) {
    (void)sljit_emit_op2(compiler, op32 | SLJIT_SET_OVERFLOW, SLJIT_R0, 0,
                   MEM(local_base + src1 * LRVALUE_SIZE + 8),
                   MEM(local_base + src2 * LRVALUE_SIZE + 8));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_i32_bin_checked op2 err=%d\n", compiler->error);
        return;
    }
    struct sljit_jump *ovf = sljit_emit_jump(compiler, SLJIT_OVERFLOW);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_i32_bin_checked jump_ovf err=%d\n", compiler->error);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    emit_set_local_tag(compiler, dst, local_base, LR_TYPE_INT32);
    struct sljit_jump *done = sljit_emit_jump(compiler, SLJIT_JUMP);
    struct sljit_label *ovf_lbl = sljit_emit_label(compiler);
    sljit_set_label(ovf, ovf_lbl);
    emit_bailout(compiler, result_ptr_offset);
    struct sljit_label *done_lbl = sljit_emit_label(compiler);
    sljit_set_label(done, done_lbl);
}

/* 32-bit multiply with overflow detection.  SLJIT_MUL32 does not expose an
 * overflow flag, so compute the 64-bit signed product and verify the upper 32
 * bits are the sign-extension of the low 32 bits (i.e. the result fits int32).
 * If not, bail out to the interpreter. */
static void emit_i32_mul_checked(struct sljit_compiler *compiler,
                                 int dst, int src1, int src2,
                                 sljit_sw local_base, sljit_sw result_ptr_offset) {
    /* sign-extend src1 -> R0 */
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src1 * LRVALUE_SIZE + 8));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked mov32_src1 err=%d src1=%d\n", compiler->error, src1);
        return;
    }
    (void)sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked shl_src1 err=%d\n", compiler->error);
        return;
    }
    (void)sljit_emit_op2(compiler, SLJIT_ASHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked ashr_src1 err=%d\n", compiler->error);
        return;
    }
    /* sign-extend src2 -> R1 */
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                   MEM(local_base + src2 * LRVALUE_SIZE + 8));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked mov32_src2 err=%d src2=%d\n", compiler->error, src2);
        return;
    }
    (void)sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked shl_src2 err=%d\n", compiler->error);
        return;
    }
    (void)sljit_emit_op2(compiler, SLJIT_ASHR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked ashr_src2 err=%d\n", compiler->error);
        return;
    }
    /* R0 = R0 * R1 (64-bit signed product) */
    (void)sljit_emit_op2(compiler, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked mul err=%d\n", compiler->error);
        return;
    }
    /* Re-sign-extend the low 32 bits of the product to 64 bits -> R1.
     * If R1 != R0, the product does not fit in int32, so bail out. */
    sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, 32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked shl_prod err=%d\n", compiler->error);
        return;
    }
    sljit_emit_op2(compiler, SLJIT_ASHR, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked ashr_prod err=%d\n", compiler->error);
        return;
    }
    SLJIT_SKIP_CHECKS(compiler);
    sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_R1, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked sub_z err=%d\n", compiler->error);
        return;
    }
    struct sljit_jump *ovf = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked jump_ne err=%d\n", compiler->error);
        return;
    }
    /* No overflow: store the int32 result. */
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked mov32_dst err=%d dst=%d\n", compiler->error, dst);
        return;
    }
    emit_set_local_tag(compiler, dst, local_base, LR_TYPE_INT32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked set_tag err=%d dst=%d\n", compiler->error, dst);
        return;
    }
    struct sljit_jump *done = sljit_emit_jump(compiler, SLJIT_JUMP);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked jump_done err=%d\n", compiler->error);
        return;
    }
    /* Overflow: bail out to the interpreter (IEEE-754 double semantics). */
    struct sljit_label *ovf_lbl = sljit_emit_label(compiler);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked label_ovf err=%d\n", compiler->error);
        return;
    }
    sljit_set_label(ovf, ovf_lbl);
    emit_bailout(compiler, result_ptr_offset);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked bailout err=%d\n", compiler->error);
        return;
    }
    struct sljit_label *done_lbl = sljit_emit_label(compiler);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] mul_checked label_done err=%d\n", compiler->error);
        return;
    }
    sljit_set_label(done, done_lbl);
}

static void emit_cmp_result(struct sljit_compiler *compiler, sljit_s32 cmp_op,
                            int src1, int src2, int dst, sljit_sw local_base) {
    /* Compute the SLJIT_SET_* flag corresponding to cmp_op.
     * cmp_op values: EQUAL=0, NOT_EQUAL=1, LESS=2, GREATER_EQUAL=3,
     *                GREATER=4, LESS_EQUAL=5.
     * SET flags start at bit 10: SLJIT_SET_LESS = 2<<10 = 0x800.
     * For EQUAL/NOT_EQUAL we use SLJIT_SET_Z = 0x200.
     *
     * IMPORTANT: int32 comparisons must be SIGNED.  The plain SLJIT_LESS /
     * SLJIT_GREATER / etc. are UNSIGNED comparisons, so a negative int32
     * (e.g. -1 = 0xFFFFFFFF) would be treated as a huge positive number.
     * Map to the SLJIT_SIG_* signed variants (EQUAL/NOT_EQUAL are the same
     * for signed and unsigned). */
    sljit_s32 sig_cmp = cmp_op;
    if (cmp_op == SLJIT_LESS) sig_cmp = SLJIT_SIG_LESS;
    else if (cmp_op == SLJIT_GREATER_EQUAL) sig_cmp = SLJIT_SIG_GREATER_EQUAL;
    else if (cmp_op == SLJIT_GREATER) sig_cmp = SLJIT_SIG_GREATER;
    else if (cmp_op == SLJIT_LESS_EQUAL) sig_cmp = SLJIT_SIG_LESS_EQUAL;

    sljit_s32 set_flag;
    if (sig_cmp == SLJIT_EQUAL || sig_cmp == SLJIT_NOT_EQUAL)
        set_flag = SLJIT_SET_Z;
    else if (sig_cmp == SLJIT_LESS)
        set_flag = SLJIT_SET_LESS;
    else if (sig_cmp == SLJIT_GREATER)
        set_flag = SLJIT_SET_GREATER;
    else if (sig_cmp == SLJIT_GREATER_EQUAL)
        set_flag = SLJIT_SET_GREATER_EQUAL;
    else if (sig_cmp == SLJIT_LESS_EQUAL)
        set_flag = SLJIT_SET_LESS_EQUAL;
    else if (sig_cmp == SLJIT_SIG_LESS)
        set_flag = SLJIT_SET_SIG_LESS;
    else if (sig_cmp == SLJIT_SIG_GREATER)
        set_flag = SLJIT_SET_SIG_GREATER;
    else if (sig_cmp == SLJIT_SIG_GREATER_EQUAL)
        set_flag = SLJIT_SET_SIG_GREATER_EQUAL;
    else if (sig_cmp == SLJIT_SIG_LESS_EQUAL)
        set_flag = SLJIT_SET_SIG_LESS_EQUAL;
    else
        set_flag = SLJIT_SET_LESS;

    if (src1 < 0 || src2 < 0) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result SKIP: invalid src src1=%d src2=%d cmp_op=%d\n", src1, src2, cmp_op);
        compiler->error = SLJIT_SUCCESS;
        return;
    }
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result SKIP: pre_err=%d cmp_op=%d sig_cmp=%d src1=%d src2=%d\n",
                   compiler->error, cmp_op, sig_cmp, src1, src2);
        compiler->error = SLJIT_SUCCESS;
        return;
    }
    SLJIT_SKIP_CHECKS(compiler);
    (void)sljit_emit_op2u(compiler, SLJIT_SUB32 | set_flag,
                    MEM(local_base + src1 * LRVALUE_SIZE + 8),
                    MEM(local_base + src2 * LRVALUE_SIZE + 8));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result op2u err=%d cmp_op=%d sig_cmp=%d set_flag=0x%x src1=%d src2=%d\n",
                   compiler->error, cmp_op, sig_cmp, set_flag, src1, src2);
    }
    if (dst < 0) dst = 0;
    /* If comparison true ? jump to true_case label */
    struct sljit_jump *to_true = sljit_emit_jump(compiler, sig_cmp);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result jump1 err=%d sig_cmp=%d\n", compiler->error, sig_cmp);
    }
    /* False path: set dst = false (0) */
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result mov_imm_0 err=%d\n", compiler->error);
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result mov_mem_0 err=%d\n", compiler->error);
    }
    emit_set_local_tag(compiler, dst, local_base, LR_TYPE_BOOL);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result set_tag err=%d\n", compiler->error);
    }
    struct sljit_jump *skip_true = sljit_emit_jump(compiler, SLJIT_JUMP);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result jump_skip err=%d\n", compiler->error);
    }
    /* True path: set dst = true (1) */
    struct sljit_label *true_lbl = sljit_emit_label(compiler);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result label_true err=%d\n", compiler->error);
    }
    sljit_set_label(to_true, true_lbl);
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result mov_imm_1 err=%d\n", compiler->error);
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result mov_mem_1 err=%d\n", compiler->error);
    }
    emit_set_local_tag(compiler, dst, local_base, LR_TYPE_BOOL);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result set_tag_1 err=%d\n", compiler->error);
    }
    /* Done: skip_true targets here */
    struct sljit_label *done_lbl = sljit_emit_label(compiler);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_cmp_result label_done err=%d\n", compiler->error);
    }
    sljit_set_label(skip_true, done_lbl);
    (void)true_lbl;
}

/* Load the numeric payload of a local slot into float register freg,
 * converting an int32 slot to its f64 value.
 *
 * LRValue layout: [tag:4][pad:4][value:8].  An int32 only fills the low
 * 4 bytes of value (+8); the upper 4 bytes (+12) are stale garbage.  Reading
 * those 8 bytes as a raw double would yield a wrong (often denormal/NaN)
 * number, so we inspect the tag first and sign-extend int32 -> f64.  This is
 * required whenever a slot may be either int32 or float64 (mixed-type f64
 * ops and comparisons). */
static void emit_load_num_to_freg(struct sljit_compiler *compiler,
                                  int slot, sljit_sw local_base,
                                  sljit_s32 freg) {
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_load_num_to_freg ENTER pre_err=%d slot=%d freg=%d\n",
                   compiler->error, slot, freg);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + slot * LRVALUE_SIZE));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_load_num_to_freg mov32_tag err=%d slot=%d\n",
                   compiler->error, slot);
        return;
    }
    SLJIT_SKIP_CHECKS(compiler);
    sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                    SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_INT32);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_load_num_to_freg sub32_z err=%d slot=%d\n",
                   compiler->error, slot);
        return;
    }
    struct sljit_jump *not_int = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_load_num_to_freg jump_not_eq err=%d slot=%d\n",
                   compiler->error, slot);
        return;
    }
    /* int32: sign-extend the 32-bit value to f64. */
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + slot * LRVALUE_SIZE + 8));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] emit_load_num_to_freg mov32_val err=%d slot=%d\n",
                   compiler->error, slot);
        return;
    }
    sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_S32, freg, 0, SLJIT_R0, 0);
    struct sljit_jump *done = sljit_emit_jump(compiler, SLJIT_JUMP);
    struct sljit_label *not_int_lbl = sljit_emit_label(compiler);
    sljit_set_label(not_int, not_int_lbl);
    /* default (float64/other): raw 8-byte value. */
    sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, freg,
                    MEM(local_base + slot * LRVALUE_SIZE + 8));
    struct sljit_label *done_lbl = sljit_emit_label(compiler);
    sljit_set_label(done, done_lbl);
}

static void emit_cmp_result_f64(struct sljit_compiler *compiler, sljit_s32 cmp_type,
                                int src1, int src2, int dst, sljit_sw local_base) {
    /* Load src1 and src2 into float registers, converting int32 -> f64. */
    emit_load_num_to_freg(compiler, src1, local_base, SLJIT_FR0);
    emit_load_num_to_freg(compiler, src2, local_base, SLJIT_FR1);

    /* sljit_emit_fcmp emits a floating-point comparison and returns a jump
     * that fires when the condition is TRUE (same semantics as sljit_emit_jump
     * after sljit_emit_op2u | SET_FLAG).  The returned jump is labeled with
     * true_lbl so that the fall-through path becomes the FALSE case. */
    struct sljit_jump *to_true = sljit_emit_fcmp(compiler, cmp_type,
                                                  SLJIT_FR0, 0, SLJIT_FR1, 0);
    if (dst < 0) dst = 0;
    /* False path: set dst = 0 */
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    emit_set_local_tag(compiler, dst, local_base, LR_TYPE_BOOL);
    struct sljit_jump *skip_true = sljit_emit_jump(compiler, SLJIT_JUMP);
    /* True path: set dst = 1 */
    struct sljit_label *true_lbl = sljit_emit_label(compiler);
    sljit_set_label(to_true, true_lbl);
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    emit_set_local_tag(compiler, dst, local_base, LR_TYPE_BOOL);
    /* Done: skip_true targets here */
    struct sljit_label *done_lbl = sljit_emit_label(compiler);
    sljit_set_label(skip_true, done_lbl);
    (void)true_lbl;
}

/* Find the MIR instruction index whose bytecode offset matches a jump target.
 * bc_offsets[] has two recording bases: some opcodes record the opcode byte
 * position, others record the imm32 start position (opcode+1).  A jump target
 * is always an absolute opcode position, so accept both (off == target and
 * off == target+1).  Sequential scan (targets are monotonic, lists are short). */
static int find_mir_idx_for_bc_off(const int *bc_offsets, int num_instrs,
                                   int target_bc_off) {
    /* First pass: exact match or +1 (original logic for jump targets that
     * land directly on an opcode). */
    for (int k = 0; k < num_instrs; k++) {
        int off = bc_offsets[k];
        if (off == target_bc_off || off == target_bc_off + 1)
            return k;
    }
    /* Second pass: fallback — find the first instruction whose bc offset is
     * greater than the target. This handles cases where the jump target lands
     * in the middle of a multi-byte opcode (e.g. the operand of a following
     * instruction), which occurs when unconditional jumps in bytecode skip
     * over the false branch of a ternary and land on a store_local whose
     * opcode starts before the target byte. */
    for (int k = 0; k < num_instrs; k++) {
        int off = bc_offsets[k];
        if (off > target_bc_off)
            return k;
    }
    /* Third pass: the target is at or past the end of the instruction stream
     * (e.g. an unconditional jump to the implicit fall-through / epilogue after
     * the last instruction). Jump to the end label at index num_instrs. */
    if (num_instrs > 0 && target_bc_off >= bc_offsets[num_instrs - 1])
        return num_instrs;
    return -1;
}

static void emit_copy_local(struct sljit_compiler *compiler,
                            int dst, int src, sljit_sw local_base) {
    /* Copy full 16-byte LRValue from src slot to dst slot.
     * Uses 4x MOV32 for aligned 16-byte copy (optimal on x64).
     * Layout: [tag:4] [padding:4] [value:8] */
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local ENTER pre_err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_src_tag err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_dst_tag err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE + 4));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_src_pad err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 4), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_dst_pad err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE + 8));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_src_val err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_dst_val err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE + 12));
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_src_ext err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 12), SLJIT_R0, 0);
    if (compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] copy_local mov_dst_ext err=%d src=%d dst=%d\n", compiler->error, src, dst);
        return;
    }
}

/* Copy a full LRValue (16 bytes) using 4x MOV32 for 16-byte alignment. */
static void emit_copy_lrvalue(struct sljit_compiler *compiler,
                              int src, int dst, sljit_sw local_base) {
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE));
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE + 4));
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 4), SLJIT_R0, 0);
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE + 8));
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
    sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                   MEM(local_base + src * LRVALUE_SIZE + 12));
    sljit_emit_op1(compiler, SLJIT_MOV32,
                   MEM(local_base + dst * LRVALUE_SIZE + 12), SLJIT_R0, 0);
}

static sljit_s32 emit_mir_instr(struct sljit_compiler *compiler,
                                const MIRInstr *instr,
                                int instr_idx,
                                sljit_sw local_base,
                                sljit_sw result_ptr_offset,
                                struct sljit_label **label_arr,
                                int num_instrs,
                                const int *bc_offsets,
                                int cur_bc_off,
                                void *prog,
                                struct sljit_jump ***jump_stack,
                                int **jump_target_stack,
                                int *jump_stack_size,
                                int *jump_stack_cap) {
    LR_JIT_DBG("[CGEN] instr[%d] op=%d dst=%d src1=%d src2=%d imm=%lld\n",
               instr_idx, (int)instr->opcode, instr->dst, instr->src1, instr->src2, (long long)instr->imm);
    /* Reset any stale error from the previous instruction so that valid
     * instructions following a bailout/invalid-MIR emission can still compile. */
    compiler->error = SLJIT_SUCCESS;
    switch (instr->opcode) {
        case MIR_OP_const_i32: {
            int dst = instr->dst;
            if (dst >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_INT32);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)instr->imm);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            }
            break;
        }

        case MIR_OP_load_local: {
            int slot = (int)instr->src1;
            int dst = instr->dst;
            if (dst >= 0 && slot >= 0)
                emit_copy_local(compiler, dst, slot, local_base);
            break;
        }

        case MIR_OP_store_local: {
            int val_var = instr->src1;
            int slot = (int)instr->src2;
            if (val_var >= 0 && slot >= 0)
                emit_copy_local(compiler, slot, val_var, local_base);
            break;
        }

        case MIR_OP_add_i32:
            emit_i32_bin_checked(compiler, SLJIT_ADD32, instr->dst,
                                 instr->src1, instr->src2,
                                 local_base, result_ptr_offset);
            break;

        case MIR_OP_sub_i32:
            emit_i32_bin_checked(compiler, SLJIT_SUB32, instr->dst,
                                 instr->src1, instr->src2,
                                 local_base, result_ptr_offset);
            break;

        case MIR_OP_mul_i32:
            emit_i32_mul_checked(compiler, instr->dst,
                                 instr->src1, instr->src2,
                                 local_base, result_ptr_offset);
            break;

        case MIR_OP_div_i32:
            /* Load operands into registers first.  SLJIT's SLJIT_DIV_S32
             * (x86 idiv) requires the dividend in R0 and divisor in R1;
             * passing memory operands directly to sljit_emit_op2 can
             * produce incorrect code, so mirror the mod_i32 pattern. */
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_R1, 0,
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8));
            sljit_emit_op0(compiler, SLJIT_DIVMOD_S32);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_mod_i32: {
            /* Native idiv for int32 operands.  Guard the two x86 idiv fault
             * cases so JS semantics hold:
             *   - divisor == 0      -> JS yields NaN (bail out to interpreter).
             *   - INT_MIN % -1      -> x86 raises #DE, but JS gives 0. */
            int dst = instr->dst;
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8)); /* dividend */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8)); /* divisor */
            /* if divisor == 0 -> bailout */
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z, SLJIT_R1, 0, SLJIT_IMM, 0);
            struct sljit_jump *div0 = sljit_emit_jump(compiler, SLJIT_EQUAL);
            /* if dividend != INT_MIN -> normal idiv path */
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)(int32_t)0x80000000);
            struct sljit_jump *not_intmin = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);
            /* dividend == INT_MIN: if divisor == -1 -> result 0, else normal */
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z, SLJIT_R1, 0, SLJIT_IMM, -1);
            struct sljit_jump *intmin_m1 = sljit_emit_jump(compiler, SLJIT_EQUAL);
            /* fallthrough -> normal idiv */
            struct sljit_label *normal_lbl = sljit_emit_label(compiler);
            sljit_set_label(not_intmin, normal_lbl);
            sljit_emit_op0(compiler, SLJIT_DIVMOD_S32);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R1, 0);
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_INT32);
            struct sljit_jump *done = sljit_emit_jump(compiler, SLJIT_JUMP);
            /* INT_MIN % -1 -> result 0 */
            struct sljit_label *m1_lbl = sljit_emit_label(compiler);
            sljit_set_label(intmin_m1, m1_lbl);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R1, 0);
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_INT32);
            struct sljit_jump *done2 = sljit_emit_jump(compiler, SLJIT_JUMP);
            /* divisor == 0 -> bailout (NaN via re-execution) */
            struct sljit_label *div0_lbl = sljit_emit_label(compiler);
            sljit_set_label(div0, div0_lbl);
            emit_bailout(compiler, result_ptr_offset);
            struct sljit_label *done_lbl = sljit_emit_label(compiler);
            sljit_set_label(done, done_lbl);
            sljit_set_label(done2, done_lbl);
            break;
        }

        case MIR_OP_neg_i32: {
            int dst = instr->dst;
            /* 0 - src, with overflow detection (negating INT32_MIN overflows). */
            (void)sljit_emit_op2(compiler, SLJIT_SUB32 | SLJIT_SET_OVERFLOW,
                           SLJIT_R0, 0,
                           SLJIT_IMM, 0,
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8));
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 op2 err=%d\n", compiler->error);
                break;
            }
            struct sljit_jump *ovf = sljit_emit_jump(compiler, SLJIT_OVERFLOW);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 jump_ovf err=%d\n", compiler->error);
                break;
            }
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 mov32_result err=%d\n", compiler->error);
                break;
            }
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_INT32);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 set_tag err=%d\n", compiler->error);
                break;
            }
            struct sljit_jump *done = sljit_emit_jump(compiler, SLJIT_JUMP);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 jump_done err=%d\n", compiler->error);
                break;
            }
            struct sljit_label *ovf_lbl = sljit_emit_label(compiler);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 label_ovf err=%d\n", compiler->error);
                break;
            }
            sljit_set_label(ovf, ovf_lbl);
            emit_bailout(compiler, result_ptr_offset);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 bailout err=%d\n", compiler->error);
                break;
            }
            struct sljit_label *done_lbl = sljit_emit_label(compiler);
            if (compiler->error != SLJIT_SUCCESS) {
                LR_JIT_ERR("[JIT-ERR] neg_i32 label_done err=%d\n", compiler->error);
                break;
            }
            sljit_set_label(done, done_lbl);
            break;
        }

        case MIR_OP_neg_f64: {
            int dst = instr->dst;
            emit_load_num_to_freg(compiler, instr->src1, local_base, SLJIT_FR0);
            sljit_emit_fop1(compiler, SLJIT_NEG_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
            sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, SLJIT_FR0,
                            MEM(local_base + dst * LRVALUE_SIZE + 8));
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_FLOAT64);
            break;
        }

        case MIR_OP_add_f64: {
            int dst = instr->dst;
            /* Register allocation: load src1 into FR0, src2 into FR1
             * (converting int32 -> f64), then FR0 = FR0 + FR1.  Using two float
             * registers avoids a memory operand in the fop2 (faster on x64). */
            emit_load_num_to_freg(compiler, instr->src1, local_base, SLJIT_FR0);
            emit_load_num_to_freg(compiler, instr->src2, local_base, SLJIT_FR1);
            sljit_emit_fop2(compiler, SLJIT_ADD_F64,
                            SLJIT_FR0, 0,
                            SLJIT_FR0, 0,
                            SLJIT_FR1, 0);
            sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, SLJIT_FR0,
                            MEM(local_base + dst * LRVALUE_SIZE + 8));
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_FLOAT64);
            break;
        }

        case MIR_OP_sub_f64: {
            int dst = instr->dst;
            emit_load_num_to_freg(compiler, instr->src1, local_base, SLJIT_FR0);
            emit_load_num_to_freg(compiler, instr->src2, local_base, SLJIT_FR1);
            sljit_emit_fop2(compiler, SLJIT_SUB_F64,
                            SLJIT_FR0, 0,
                            SLJIT_FR0, 0,
                            SLJIT_FR1, 0);
            sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, SLJIT_FR0,
                            MEM(local_base + dst * LRVALUE_SIZE + 8));
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_FLOAT64);
            break;
        }

        case MIR_OP_mul_f64: {
            int dst = instr->dst;
            emit_load_num_to_freg(compiler, instr->src1, local_base, SLJIT_FR0);
            emit_load_num_to_freg(compiler, instr->src2, local_base, SLJIT_FR1);
            sljit_emit_fop2(compiler, SLJIT_MUL_F64,
                            SLJIT_FR0, 0,
                            SLJIT_FR0, 0,
                            SLJIT_FR1, 0);
            sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, SLJIT_FR0,
                            MEM(local_base + dst * LRVALUE_SIZE + 8));
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_FLOAT64);
            break;
        }

        case MIR_OP_div_f64: {
            int dst = instr->dst;
            emit_load_num_to_freg(compiler, instr->src1, local_base, SLJIT_FR0);
            emit_load_num_to_freg(compiler, instr->src2, local_base, SLJIT_FR1);
            sljit_emit_fop2(compiler, SLJIT_DIV_F64,
                            SLJIT_FR0, 0,
                            SLJIT_FR0, 0,
                            SLJIT_FR1, 0);
            sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, SLJIT_FR0,
                            MEM(local_base + dst * LRVALUE_SIZE + 8));
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_FLOAT64);
            break;
        }

        case MIR_OP_mod_f64: {
            /* fmod(a, b) via runtime helper since SLJIT has no native mod.
             * lr_jit_rt_fmod(interp, const LRValue *a, const LRValue *b, LRValue *out)
             * expects the FULL LRValue slot addresses (slot start), NOT the
             * .value sub-field.  Passing the +8 value address would make the
             * helper read the int operand as the LRValue tag (e.g. 5 = STRING,
             * 6 = OBJECT) and dereference a garbage pointer inside
             * lr_to_float64 -> access violation. */
            {
                int dst = instr->dst;
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + instr->src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + instr->src2 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_fmod);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        case MIR_OP_shl_i32:
            LR_JIT_DBG("[CGEN-TRACE] shl_i32: dst=%d src1=%d(count) src2=%d(value) bc=%d\n",
                       instr->dst, instr->src1, instr->src2, cur_bc_off);
            /* value << count (MIR frontend puts count in src1). */
            (void)sljit_emit_op2(compiler, SLJIT_SHL32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8),
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8));
            CHECK_SLJIT_ERR("op2(shl_i32)");
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_shr_i32:
            LR_JIT_DBG("[CGEN-TRACE] shr_i32: dst=%d src1=%d(count) src2=%d(value) bc=%d\n",
                       instr->dst, instr->src1, instr->src2, cur_bc_off);
            /* value >>> count (logical right shift, MIR src2=value). */
            (void)sljit_emit_op2(compiler, SLJIT_LSHR32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8),
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8));
            CHECK_SLJIT_ERR("op2(shr_i32)");
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_sar_i32:
            LR_JIT_DBG("[CGEN-TRACE] sar_i32: dst=%d src1=%d(count) src2=%d(value) bc=%d\n",
                       instr->dst, instr->src1, instr->src2, cur_bc_off);
            /* value >> count (arithmetic right shift, MIR src2=value). */
            (void)sljit_emit_op2(compiler, SLJIT_ASHR32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8),
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8));
            CHECK_SLJIT_ERR("op2(sar_i32)");
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_not_i32: {
            /* Logical NOT (!x): result = (x == 0) ? 1 : 0.
             * Use SUB32 | SET_Z to set flags, then conditional jump.
             * No SLJIT_SKIP_CHECKS to prevent SLJIT from folding/omitting the sub. */
            int dst = instr->dst;
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            MEM(local_base + instr->src1 * LRVALUE_SIZE + 8),
                            SLJIT_IMM, 0);
            struct sljit_jump *to_true = sljit_emit_jump(compiler, SLJIT_EQUAL);
            /* false path: dst = 0 */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_BOOL);
            struct sljit_jump *skip_true = sljit_emit_jump(compiler, SLJIT_JUMP);
            /* true path: dst = 1 */
            struct sljit_label *true_lbl = sljit_emit_label(compiler);
            sljit_set_label(to_true, true_lbl);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, dst, local_base, LR_TYPE_BOOL);
            struct sljit_label *done_lbl = sljit_emit_label(compiler);
            sljit_set_label(skip_true, done_lbl);
            break;
        }

        case MIR_OP_and_i32:
            LR_JIT_DBG("[CGEN-TRACE] and_i32: dst=%d src1=%d src2=%d bc=%d\n",
                       instr->dst, instr->src1, instr->src2, cur_bc_off);
            CHECK_SLJIT_ERR("op2(and_i32)");
            (void)sljit_emit_op2(compiler, SLJIT_AND32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8),
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_or_i32:
            LR_JIT_DBG("[CGEN-TRACE] or_i32: dst=%d src1=%d src2=%d bc=%d\n",
                       instr->dst, instr->src1, instr->src2, cur_bc_off);
            CHECK_SLJIT_ERR("op2(or_i32)");
            (void)sljit_emit_op2(compiler, SLJIT_OR32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8),
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_xor_i32:
            LR_JIT_DBG("[CGEN-TRACE] xor_i32: dst=%d src1=%d src2=%d bc=%d\n",
                       instr->dst, instr->src1, instr->src2, cur_bc_off);
            CHECK_SLJIT_ERR("op2(xor_i32)");
            (void)sljit_emit_op2(compiler, SLJIT_XOR32,
                           SLJIT_R0, 0,
                           MEM(local_base + instr->src1 * LRVALUE_SIZE + 8),
                           MEM(local_base + instr->src2 * LRVALUE_SIZE + 8));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + instr->dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            emit_set_local_tag(compiler, instr->dst, local_base, LR_TYPE_INT32);
            break;

        case MIR_OP_lt_i32:
            emit_cmp_result(compiler, SLJIT_LESS,
                            instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_gt_i32:
            emit_cmp_result(compiler, SLJIT_GREATER,
                            instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_le_i32:
            emit_cmp_result(compiler, SLJIT_LESS_EQUAL,
                            instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_ge_i32:
            emit_cmp_result(compiler, SLJIT_GREATER_EQUAL,
                            instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_eq_i32:
            emit_cmp_result(compiler, SLJIT_EQUAL,
                            instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_ne_i32:
            emit_cmp_result(compiler, SLJIT_NOT_EQUAL,
                            instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_lt_f64:
            emit_cmp_result_f64(compiler, SLJIT_F_LESS,
                                instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_gt_f64:
            emit_cmp_result_f64(compiler, SLJIT_F_GREATER,
                                instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_le_f64:
            emit_cmp_result_f64(compiler, SLJIT_F_LESS_EQUAL,
                                instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_ge_f64:
            emit_cmp_result_f64(compiler, SLJIT_F_GREATER_EQUAL,
                                instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_eq_f64:
            emit_cmp_result_f64(compiler, SLJIT_F_EQUAL,
                                instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_ne_f64:
            emit_cmp_result_f64(compiler, SLJIT_F_NOT_EQUAL,
                                instr->src1, instr->src2, instr->dst, local_base);
            break;

        case MIR_OP_push_true: {
            int dst = instr->dst;
            if (dst >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_BOOL);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            }
            break;
        }

        case MIR_OP_push_false: {
            int dst = instr->dst;
            if (dst >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_BOOL);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            }
            break;
        }

        case MIR_OP_push_undefined: {
            int dst = instr->dst;
            if (dst >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_UNDEFINED);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            }
            break;
        }

        case MIR_OP_push_null: {
            int dst = instr->dst;
            if (dst >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_NULL);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                               MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            }
            break;
        }

        case MIR_OP_ret: {
            int val_var = instr->src1;
            /* V8 per-call-frame: restore caller's current_scope before returning. */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, MEM(SAVED_SCOPE_OFFSET));
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), INTERP_CURRENT_SCOPE_OFF, SLJIT_R0, 0);
            /* Load result pointer from saved location */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                           MEM(result_ptr_offset));
            /* Layout: [tag:4] [padding:4] [value:8]
             * Local slot layout: [tag@0] [pad@4] [value@8] */
            /* Write tag (offset 0) */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + val_var * LRVALUE_SIZE));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 0, SLJIT_R0, 0);
            /* Write padding (offset 4) */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + val_var * LRVALUE_SIZE + 4));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 4, SLJIT_R0, 0);
            /* Write value (offset 8 - first 4 bytes) */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + val_var * LRVALUE_SIZE + 8));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 8, SLJIT_R0, 0);
            /* Write value (offset 12 - last 4 bytes) */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + val_var * LRVALUE_SIZE + 12));
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 12, SLJIT_R0, 0);
            sljit_emit_return_void(compiler);
            break;
        }

        case MIR_OP_move: {
            /* Copy 16-byte LRValue from src1 var to dst var in spill area */
            if (instr->dst >= 0 && instr->src1 >= 0)
                emit_copy_local(compiler, instr->dst, instr->src1, local_base);
            break;
        }

        case MIR_OP_load_var: {
            /* Runtime callback: lr_jit_rt_load_var(interp, prog, name_idx, out)
             *   arg0 = interp (SLJIT_S0)
             *   arg1 = prog  (immediate)
             *   arg2 = name_idx (immediate from instr->imm)
             *   arg3 = out ptr (address of dst local slot)
             *
             * Use SLJIT_CALL_REG_ARG so call_with_args is skipped.
             * All args are already in scratch regs (R0..R3) —
             * SLJIT will emit a direct CALL rel32 without reordering. */
            int dst = instr->dst;
            if (dst >= 0) {
                /* Set up call args in scratch regs R0..R3.
                 * Under _WIN64_ABI: R3=RCX(arg1), R1=RDX(arg2), R2=R8(arg3), TMP_REG1=R9(arg4). */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);       /* RCX = interp */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);  /* RDX = prog  */
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);  /* R8 = name_idx */
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));                     /* R9 = out ptr */
                /* Call helper — SLJIT_CALL_REG_ARG skips call_with_args */
                sljit_s32 icall_rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_load_var);
                if (icall_rc != SLJIT_SUCCESS) return icall_rc;
            }
            break;
        }

        case MIR_OP_runtime_call: {
            /* Runtime callback: lr_jit_rt_call(interp, argc, args_base, out)
             *   arg0 = interp (SLJIT_S0)
             *   arg1 = argc  (immediate from instr->imm)
             *   arg2 = args_base (pointer to callee+args in spill area)
             *   arg3 = out ptr (address of dst local slot)
             * The callee and args are MIR local vars. src1 = callee var.
             *
             * Use SLJIT_CALL_REG_ARG so call_with_args is skipped.
             * Args are pre-setup in _WIN64_ABI order: R3=RCX, R1=RDX, R2=R8, TMP_REG1=R9. */
            int dst = instr->dst;
            int callee_var = instr->src1;
            if (dst >= 0 && callee_var >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);           /* RCX = interp */
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);  /* RDX = argc */
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + callee_var * LRVALUE_SIZE));                   /* R8 = args_base */
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));                           /* R9 = out ptr */
                sljit_s32 icall_rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, 32, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_call);
                if (icall_rc != SLJIT_SUCCESS) return icall_rc;
            }
            break;
        }

        case MIR_OP_scope_call: {
            /* V8 per-call-frame style: same as MIR_OP_runtime_call.
             * Args were pre-loaded from scope via MIR_OP_load_scope +
             * MIR_OP_move into contiguous spill slots [base_var..base_var+argc].
             * Identical arg layout to runtime_call:
             *   arg0 = interp, arg1 = argc, arg2 = args_base, arg3 = out ptr.
             *
             * Use SLJIT_CALL_REG_ARG so call_with_args is skipped.
             * Args are pre-setup in _WIN64_ABI order: R3=RCX, R1=RDX, R2=R8, TMP_REG1=R9. */
            int dst = instr->dst;
            int callee_var = instr->src1;
            if (dst >= 0 && callee_var >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);           /* RCX = interp */
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);  /* RDX = argc */
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + callee_var * LRVALUE_SIZE));                   /* R8 = args_base */
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));                           /* R9 = out ptr */
                sljit_s32 icall_rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, 32, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_call);
                if (icall_rc != SLJIT_SUCCESS) return icall_rc;
            }
            break;
        }

        case MIR_OP_inline_call: {
            /* Direct JIT-to-JIT call: lr_jit_rt_inline_call(interp, argc,
             * args_base, out).  Same 4-param ABI as lr_jit_rt_call but the
             * helper attempts direct native dispatch before falling back.
             *
             * ABI (Windows x64 / SLJIT_CALL_REG_ARG, 4 params):
             *   R3(CX)=interp, R1(DX)=argc, R2(R8)=args_base, TMP_REG1(R9)=out */
            int dst = instr->dst;
            int callee_var = instr->src1;
            if (dst >= 0 && callee_var >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);           /* RCX = interp */
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM,
                    (sljit_sw)(uint32_t)instr->imm);                                     /* RDX = argc */
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + callee_var * LRVALUE_SIZE));                   /* R8 = args_base */
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));                           /* R9 = out ptr */
                sljit_s32 icall_rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, 32, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_inline_call);
                if (icall_rc != SLJIT_SUCCESS) return icall_rc;
            }
            break;
        }

        case MIR_OP_load_scope: {
            /* V8 per-call-frame: load values[slot] from the PINNED
             * caller scope (saved in SAVED_SCOPE_OFFSET during prologue)
             * into the JIT spill area at dst.
             *
             * We use the saved scope pointer instead of interp->current_scope
             * because recursive JIT calls change current_scope to the callee's
             * scope, but load_scope must read from the OUTER function's scope.
             *
             * R0 = saved_scope = *(SAVED_SCOPE_OFFSET)
             * R1 = *(R0 + SCOPE_VALUES_OFF)          // values[] pointer
             * Copy 16 bytes from R1 + slot*16 to spill dst */
            int slot = (int)instr->src1;
            int dst = instr->dst;
            if (dst >= 0 && slot >= 0) {
                /* R0 = saved_scope = *(SP + SAVED_SCOPE_OFFSET) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                    MEM(SAVED_SCOPE_OFFSET));
                /* R1 = values[] = *(saved_scope + SCOPE_VALUES_OFF) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                    SLJIT_MEM1(SLJIT_R0), SCOPE_VALUES_OFF);
                /* Copy 16 bytes: values[slot] -> spill[dst] */
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_MEM1(SLJIT_R1), slot * LRVALUE_SIZE);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                    MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R2, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_MEM1(SLJIT_R1), slot * LRVALUE_SIZE + 4);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                    MEM(local_base + dst * LRVALUE_SIZE + 4), SLJIT_R2, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_MEM1(SLJIT_R1), slot * LRVALUE_SIZE + 8);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                    MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R2, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_MEM1(SLJIT_R1), slot * LRVALUE_SIZE + 12);
                sljit_emit_op1(compiler, SLJIT_MOV32,
                    MEM(local_base + dst * LRVALUE_SIZE + 12), SLJIT_R2, 0);
            }
            break;
        }

        case MIR_OP_bailout:
            /* Write tag=-1 to *result */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                           MEM(result_ptr_offset));
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, -1);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 0, SLJIT_R0, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 4, SLJIT_R0, 0);
            sljit_emit_return_void(compiler);
            break;

        case MIR_OP_call: {
            /* Function call: args already on scope, callee at argv[-1] in scope.
             * For now, bail out since call requires resolving callee and pushing args. */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                           MEM(result_ptr_offset));
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, -1);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 0, SLJIT_R0, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_R1), 4, SLJIT_R0, 0);
            sljit_emit_return_void(compiler);
            break;
        }

        case MIR_OP_jump: {
            int32_t byte_off = (int32_t)instr->imm;
            if (byte_off == 0) break;
            if (!bc_offsets) break;
            /* bc_offsets[] for jump opcodes is the imm32 start position (opcode+1),
             * and patch_jump_to computes rel = target - (pos + 4).  So:
             *   target = cur_bc_off + 4 + byte_off */
            int target_bc_off = cur_bc_off + 4 + byte_off;
            LR_JIT_DBG("[JIT] jump: cur_bc_off=%d byte_off=%d -> target_bc_off=%d\n",
                           cur_bc_off, byte_off, target_bc_off);
            int target_mir_idx = find_mir_idx_for_bc_off(bc_offsets, num_instrs, target_bc_off);
            if (target_mir_idx < 0 || target_mir_idx > num_instrs) break;
            struct sljit_jump *j = sljit_emit_jump(compiler, SLJIT_JUMP);
            struct sljit_label *target = label_arr[target_mir_idx];
            if (target) {
                sljit_set_label(j, target);
            } else if (jump_stack) {
                /* Label not yet created (target is ahead in the instruction
                 * stream). Defer patching until after the loop. */
                int cap = *jump_stack_size + 1;
                if (cap > *jump_stack_cap) {
                    int new_cap = *jump_stack_cap * 2;
                    if (new_cap < cap) new_cap = cap;
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)new_cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)new_cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; *jump_stack_cap = new_cap; }
                } else {
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; }
                }
                if (*jump_stack) { (*jump_stack)[*jump_stack_size] = j; (*jump_target_stack)[*jump_stack_size] = target_mir_idx; (*jump_stack_size)++; }
            }
            break;
        }

        case MIR_OP_jump_if_false: {
            int32_t byte_off = (int32_t)instr->imm;
            if (byte_off == 0) break;
            if (!bc_offsets) break;
            /* imm32-start based, see MIR_OP_jump comment */
            int target_bc_off = cur_bc_off + 4 + byte_off;
            int target_mir_idx = find_mir_idx_for_bc_off(bc_offsets, num_instrs, target_bc_off);
            LR_JIT_DBG("[JIT] jump_if_false: cur_bc_off=%d byte_off=%d -> target_bc_off=%d target_mir_idx=%d (bc_offsets=[",
                           cur_bc_off, byte_off, target_bc_off, target_mir_idx);
            for (int k = 0; k < num_instrs && k < 20; k++) {
                if (k > 0) LR_JIT_DBG(",");
                LR_JIT_DBG("%d", bc_offsets[k]);
            }
            LR_JIT_DBG("...]) cond=%d\n", instr->src1);
            if (target_mir_idx < 0 || target_mir_idx > num_instrs) {
                LR_JIT_DBG("[JIT] jump_if_false: target_bc_off=%d NOT FOUND (instr %d, cond=%d)\n",
                               target_bc_off, cur_bc_off, instr->src1);
                break;
            }
            LR_JIT_DBG("[JIT] jump_if_false: cur_bc_off=%d byte_off=%d -> target_bc_off=%d target_mir_idx=%d label=%p\n",
                           cur_bc_off, byte_off, target_bc_off, target_mir_idx, (void*)label_arr[target_mir_idx]);
            int cond = instr->src1;
            /* Use SUB32 | SET_Z to set flags for the jump. No SLJIT_SKIP_CHECKS. */
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            MEM(local_base + cond * LRVALUE_SIZE + 8),
                            SLJIT_IMM, 0);
            struct sljit_jump *j = sljit_emit_jump(compiler, SLJIT_EQUAL);
            struct sljit_label *target = label_arr[target_mir_idx];
            if (target) {
                sljit_set_label(j, target);
            } else if (jump_stack) {
                int cap = *jump_stack_size + 1;
                if (cap > *jump_stack_cap) {
                    int new_cap = *jump_stack_cap * 2;
                    if (new_cap < cap) new_cap = cap;
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)new_cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)new_cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; *jump_stack_cap = new_cap; }
                } else {
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; }
                }
                if (*jump_stack) { (*jump_stack)[*jump_stack_size] = j; (*jump_target_stack)[*jump_stack_size] = target_mir_idx; (*jump_stack_size)++; }
            }
            break;
        }

        case MIR_OP_jump_if_true: {
            int32_t byte_off = (int32_t)instr->imm;
            if (byte_off == 0) break;
            if (!bc_offsets) break;
            int target_bc_off = cur_bc_off + 4 + byte_off;
            int target_mir_idx = find_mir_idx_for_bc_off(bc_offsets, num_instrs, target_bc_off);
            if (target_mir_idx < 0 || target_mir_idx > num_instrs) break;
            int cond = instr->src1;
            /* Use SUB32 | SET_Z to set flags for the jump. No SLJIT_SKIP_CHECKS. */
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            MEM(local_base + cond * LRVALUE_SIZE + 8),
                            SLJIT_IMM, 0);
            struct sljit_jump *j = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);
            struct sljit_label *target = label_arr[target_mir_idx];
            if (target) {
                sljit_set_label(j, target);
            } else if (jump_stack) {
                int cap = *jump_stack_size + 1;
                if (cap > *jump_stack_cap) {
                    int new_cap = *jump_stack_cap * 2;
                    if (new_cap < cap) new_cap = cap;
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)new_cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)new_cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; *jump_stack_cap = new_cap; }
                } else {
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; }
                }
                if (*jump_stack) { (*jump_stack)[*jump_stack_size] = j; (*jump_target_stack)[*jump_stack_size] = target_mir_idx; (*jump_stack_size)++; }
            }
            break;
        }

        /* ── Extended core MIR instructions ───────────────────────────────
         * Each routes through a runtime callback (lr_jit_rt_*) so the JIT
         * can keep emitting native code for surrounding arithmetic and
         * control flow.  All callbacks expect the JIT spill area pointers
         * to be 16-byte aligned LRValue slots. */

        /* Constant pool loads: BC_PUSH_FLOAT64 / BC_PUSH_STRING.
         * Pattern: lr_jit_rt_load_const_*(interp, prog, pool_idx, out*)
         *   arg0 = interp (S0)
         *   arg1 = prog  (immediate)
         *   arg2 = pool_idx (imm32)
         *   arg3 = &dst   (spill slot pointer) */
        case MIR_OP_const_f64: {
            int dst = instr->dst;
            if (dst >= 0) {
                /* load_const_f64: lr_jit_rt_load_const_f64(interp, prog, name_idx, out*)
                 *   ABI: R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(out) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_load_const_f64);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        case MIR_OP_const_string: {
            int dst = instr->dst;
            if (dst >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(pool_idx), TMP_REG1=R9(out) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_load_const_string);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* `this` binding: BC_PUSH_THIS.
         * Pattern: lr_jit_rt_load_this(interp, out*)
         *   arg0 = interp, arg1 = &dst */
        case MIR_OP_load_this: {
            int dst = instr->dst;
            if (dst >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(out) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS2V(W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_load_this);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Variable write-back: BC_STORE_VAR / BC_INC_VAR.
         * store_var: lr_jit_rt_store_var(interp, prog, name_idx, val*)
         *   arg0 = interp, arg1 = prog, arg2 = name_idx (imm32), arg3 = &src1 */
        case MIR_OP_store_var: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(val*).
                 * Do NOT reuse R1 for the val pointer: R1 must keep prog. */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_store_var);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* inc_var: lr_jit_rt_inc_var(interp, prog, name_idx, out*)
         *   arg0 = interp, arg1 = prog, arg2 = name_idx (imm32), arg3 = &dst */
        case MIR_OP_inc_var: {
            int dst = instr->dst;
            if (dst >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(out) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_inc_var);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* typeof family: BC_TYPEOF / BC_TYPEOF_VAR.
         * typeof: lr_jit_rt_typeof(interp, val*, out*)
         *   arg0 = interp, arg1 = &src1, arg2 = &dst */
        case MIR_OP_typeof: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(val*), R2=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_typeof);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* typeof_var: lr_jit_rt_typeof_var(interp, prog, name_idx, out*)
         *   arg0 = interp, arg1 = prog, arg2 = name_idx (imm32), arg3 = &dst */
        case MIR_OP_typeof_var: {
            int dst = instr->dst;
            if (dst >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(out) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_typeof_var);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Inline coercions: BC_TO_STRING / BC_TO_NUMBER / BC_TO_BOOL / BC_POS.
         * Pattern: lr_jit_rt_*(interp, val*, out*)
         *   arg0 = interp, arg1 = &src1, arg2 = &dst */
        case MIR_OP_to_string: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* to_string: lr_jit_rt_to_string(interp, val*, out*)
                 *   ABI: R3=RCX(interp), R1=RDX(val*), R2=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_to_string);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        case MIR_OP_to_number: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(val*), R2=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_to_number);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        case MIR_OP_string_concat: {
            int dst = instr->dst, src1 = instr->src1, src2 = instr->src2;
            if (dst >= 0 && src1 >= 0 && src2 >= 0) {
                /* string_concat: lr_jit_rt_string_concat(interp, a*, b*, out*)
                 *   ABI: R3=RCX(interp), R1=RDX(a*), R2=R8(b*), R0=R9(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src2 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R0, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_string_concat);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        case MIR_OP_to_bool: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* to_bool: lr_jit_rt_to_bool(interp, val*, out*)
                 *   ABI: R3=RCX(interp), R1=RDX(val*), R2=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_to_bool);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        case MIR_OP_pos: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(val*), R2=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_pos);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Arithmetic runtime: BC_POW.
         * lr_jit_rt_pow(interp, base*, exp*, out*)
         *   arg0 = interp, arg1 = &src1, arg2 = &src2, arg3 = &dst */
        case MIR_OP_pow: {
            int dst = instr->dst, src1 = instr->src1, src2 = instr->src2;
            if (dst >= 0 && src1 >= 0 && src2 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(base*), R2=R8(exp*), TMP_REG1=R9(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src2 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_pow);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Object/array allocation: BC_NEW_OBJECT / BC_NEW_ARRAY.
         * new_object: lr_jit_rt_new_object(interp, out*)
         *   arg0 = interp, arg1 = &dst */
        case MIR_OP_new_object: {
            int dst = instr->dst;
            if (dst >= 0) {
                /* new_object: lr_jit_rt_new_object(interp, out*)
                 *   ABI: R3=RCX(interp), R1=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS2V(W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_new_object);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* new_array: lr_jit_rt_new_array(interp, n, elems*, out*)
         *   arg0 = interp, arg1 = n (imm32), arg2 = &src1 (elems base), arg3 = &dst */
        case MIR_OP_new_array: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(n), R2=R8(elems*), TMP_REG1=R9(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, 32, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_new_array);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Property/element access.
         * get_prop: lr_jit_rt_get_prop(interp, prog, name_idx, args_base*)
         *   args_base[0] = obj (input), args_base[1] = result (output).
         *   arg0 = interp, arg1 = prog, arg2 = name_idx (imm32), arg3 = &src1 (base_var) */
        case MIR_OP_get_prop: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(base*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_get_prop);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* MIR_OP_load_prop: shape-cached property load via runtime helper. */
        case MIR_OP_load_prop: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_get_prop_cached);
                if (rc != SLJIT_SUCCESS) return rc;
                if (dst != src1 + 1) {
                    emit_copy_lrvalue(compiler, src1 + 1, dst, local_base);
                }
            }
            break;
        }

        /* MIR_OP_get_prop_cached: inline shape+version check with direct
         * slot read on hit, fallback to runtime helper on miss.
         *
         * Layout:
         *   src1 (base_var): [base_var]=obj LRValue, [base_var+1]=result LRValue
         *   src2 (cache_base): [cache_base]=shape LRValue(.u.ptr),
         *                      [cache_base+1]=version LRValue(.u.int32),
         *                      [cache_base+2]=slot LRValue(.u.int32)
         *   imm = pool name_idx
         *   dst = result slot (= base_var+1, but may differ if MIR optimizer
         *         renumbered vars; we always write to dst).
         *
         * Hit path (no runtime call):
         *   1. Load tag from obj[base_var], cmp == LR_TYPE_OBJECT, bail if not
         *   2. Load obj ptr from obj+8 (value field), null check, bail if null
         *   3. Load shape ptr from obj_ptr + SHAPE_OFFSET_SHAPE (24), null check, bail
         *   4. Load version from shape_ptr + SHAPE_OFFSET_VERSION (40), cmp == cached
         *   5. Load slot from cache_base+2, compute props base + slot*16, load result
         *   6. Copy result to dst
         *
         * Miss path: call lr_jit_rt_get_prop_cached to re-resolve and update cache */
        case MIR_OP_get_prop_cached: {
            int dst = instr->dst, src1 = instr->src1, src2 = instr->src2;
            if (dst < 0 || src1 < 0 || src2 < 0) break;
            if (dst != src1 + 1) {
                /* Result slot is not contiguous with obj — must use runtime
                 * helper for correctness (it manages the full LRValue copy). */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_get_prop_cached);
                if (rc != SLJIT_SUCCESS) return rc;
                if (dst != src1 + 1) {
                    emit_copy_lrvalue(compiler, src1 + 1, dst, local_base);
                }
                break;
            }
            /* Fast path: inline shape+version check + direct slot read.
             * ABI preserved across the inline block: no callee calls here. */

            /* --- 1. Load object LRValue, check tag --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + src1 * LRVALUE_SIZE));
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_OBJECT);
            struct sljit_jump *not_obj = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* --- 2. Load object pointer (value field at +8) --- */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                           MEM(local_base + src1 * LRVALUE_SIZE + 8));
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z,
                            SLJIT_R0, 0, SLJIT_IMM, 0);
            struct sljit_jump *null_obj = sljit_emit_jump(compiler, SLJIT_EQUAL);

            /* --- 3. Load shape ptr from obj + SHAPE_OFFSET_SHAPE (24) --- */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_R0), SHAPE_OFFSET_SHAPE);
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z,
                            SLJIT_R1, 0, SLJIT_IMM, 0);
            struct sljit_jump *null_shape = sljit_emit_jump(compiler, SLJIT_EQUAL);

            /* --- 3b. Check is_exotic at obj + LR_OBJECT_OFFSET_IS_EXOTIC (104) --- */
            sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R0), LR_OBJECT_OFFSET_IS_EXOTIC);
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z,
                            SLJIT_R2, 0, SLJIT_IMM, 0);
            struct sljit_jump *exotic = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* --- 4. Load shape version from shape + SHAPE_OFFSET_VERSION (40) --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R1), SHAPE_OFFSET_VERSION);

            /* --- 5. Compare with cached version at cache_base+1 --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, TMP_REG1, 0,
                           MEM(local_base + (src2 + 1) * LRVALUE_SIZE));
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R2, 0, TMP_REG1, 0);
            struct sljit_jump *ver_miss = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* --- HIT: load cached slot from cache_base+2, read props[slot] --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                           MEM(local_base + (src2 + 2) * LRVALUE_SIZE));
            /* props array base = obj_ptr + offset_of_props (from LRObject struct) */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_R0), LR_OBJECT_OFFSET_PROPS);
            /* result_addr = R3 + R2 * 16 (props[slot]) */
            sljit_emit_op2(compiler, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 16);
            sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R3, 0, SLJIT_R2, 0);
            /* Copy 16 bytes from props[slot] to result at base_var+1 (=dst) */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R2), 0);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R0, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R2), 4);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 4), SLJIT_R0, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R2), 8);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R2), 12);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 12), SLJIT_R0, 0);
            struct sljit_jump *done_cached = sljit_emit_jump(compiler, SLJIT_JUMP);

            /* --- Miss / bailout labels --- */
            {
                struct sljit_label *miss_lbl = sljit_emit_label(compiler);
                sljit_set_label(ver_miss, miss_lbl);
                struct sljit_label *exotic_lbl = sljit_emit_label(compiler);
                sljit_set_label(exotic, exotic_lbl);
                struct sljit_label *shape_null_lbl = sljit_emit_label(compiler);
                sljit_set_label(null_shape, shape_null_lbl);
                struct sljit_label *obj_null_lbl = sljit_emit_label(compiler);
                sljit_set_label(null_obj, obj_null_lbl);
                struct sljit_label *not_obj_lbl = sljit_emit_label(compiler);
                sljit_set_label(not_obj, not_obj_lbl);
                /* All miss paths fall through to the runtime helper call below */
            }

            /* --- Fallback: call runtime helper, then copy result --- */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
            sljit_get_local_base(compiler, TMP_REG1, 0,
                (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
            sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                SLJIT_ARGS4V(W, W, 32, W),
                SLJIT_IMM, (sljit_sw)&lr_jit_rt_get_prop_cached);
            if (rc != SLJIT_SUCCESS) return rc;
            /* Helper writes result to args_base[1] = src1+1; dst == src1+1, so done. */
            break;
        }

        /* MIR_OP_add_prop: fused shape check + property read + i32 add.
         *   imm=name_idx, ptr=prog, src1=obj, src2=rhs, dst=prop_out (= src1+1)
         *   Layout: [src1]=obj LRValue, [src1+1]=result LRValue
         *   Hit path (no runtime call):
         *     1. Load tag from obj[src1], cmp == LR_TYPE_OBJECT, bail if not
         *     2. Load obj ptr from obj+8, null check, bail if null
         *     3. Load shape ptr from obj_ptr+SHAPE_OFFSET_SHAPE(24), bail if null
         *     4. Check is_exotic at obj_ptr+104, bail if exotic
         *     5. Load version from shape+SHAPE_OFFSET_VERSION(40)
         *     6. Load cached slot from [src1+1] (prop_out LRValue .u.int32)
         *     7. Read props[slot] -> prop LRValue
         *     8. Check prop.tag == LR_TYPE_INT32, bail if not
         *     9. Load rhs from src2, check tag == LR_TYPE_INT32, bail if not
         *    10. Add prop.u.int32 + rhs.u.int32, overflow check -> dst
         *   Miss path: call lr_jit_rt_add_prop, helper writes to args_base[1] */
        case MIR_OP_add_prop: {
            int src1 = instr->src1, src2 = instr->src2;
            int dst = instr->dst;
            if (dst < 0 || src1 < 0 || src2 < 0) break;

            /* --- 1. Load object LRValue tag --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           MEM(local_base + src1 * LRVALUE_SIZE));
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_OBJECT);
            struct sljit_jump *not_obj = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* --- 2. Load object pointer (value field at +8) --- */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                           MEM(local_base + src1 * LRVALUE_SIZE + 8));
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z,
                            SLJIT_R0, 0, SLJIT_IMM, 0);
            struct sljit_jump *null_obj = sljit_emit_jump(compiler, SLJIT_EQUAL);

            /* --- 3. Load shape ptr from obj + SHAPE_OFFSET_SHAPE (24) --- */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_R0), SHAPE_OFFSET_SHAPE);
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z,
                            SLJIT_R1, 0, SLJIT_IMM, 0);
            struct sljit_jump *null_shape = sljit_emit_jump(compiler, SLJIT_EQUAL);

            /* --- 4. Check is_exotic --- */
            sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R0), LR_OBJECT_OFFSET_IS_EXOTIC);
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB | SLJIT_SET_Z,
                            SLJIT_R2, 0, SLJIT_IMM, 0);
            struct sljit_jump *exotic = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* --- 5. Load shape version --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R1), SHAPE_OFFSET_VERSION);

            /* --- 6. Load cached slot from [dst] (.u.int32) --- */
            sljit_emit_op1(compiler, SLJIT_MOV32, TMP_REG1, 0,
                           MEM(local_base + dst * LRVALUE_SIZE));

            /* --- 7. Compare version --- */
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R2, 0, TMP_REG1, 0);
            struct sljit_jump *ver_miss = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* --- HIT path: read props[slot] --- */
            /* props base = R0 + LR_OBJECT_OFFSET_PROPS (24) */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_R0), LR_OBJECT_OFFSET_PROPS);
            /* prop_addr = R3 + R2 * 16 */
            sljit_emit_op2(compiler, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 16);
            sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R3, 0, SLJIT_R2, 0);

            /* Load prop tag, check == INT32 */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R2), 0);
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R0, 0, SLJIT_IMM, LR_TYPE_INT32);
            struct sljit_jump *prop_not_int = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* Load prop value */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R2), 8);

            /* Check rhs tag == INT32 */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                           MEM(local_base + src2 * LRVALUE_SIZE));
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_Z,
                            SLJIT_R1, 0, SLJIT_IMM, LR_TYPE_INT32);
            struct sljit_jump *rhs_not_int = sljit_emit_jump(compiler, SLJIT_NOT_EQUAL);

            /* Load rhs value into R1 */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                           MEM(local_base + src2 * LRVALUE_SIZE + 8));

            /* R0 = R0 + R1 with overflow check */
            (void)sljit_emit_op2(compiler, SLJIT_ADD32 | SLJIT_SET_OVERFLOW,
                           SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            CHECK_SLJIT_ERR("op2(add_prop)");
            struct sljit_jump *ovf = sljit_emit_jump(compiler, SLJIT_OVERFLOW);

            /* Store result: tag = INT32, value = R0 at dst */
            sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)LR_TYPE_INT32);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE), SLJIT_R2, 0);
            sljit_emit_op1(compiler, SLJIT_MOV32,
                           MEM(local_base + dst * LRVALUE_SIZE + 8), SLJIT_R0, 0);

            struct sljit_jump *done_add_prop = sljit_emit_jump(compiler, SLJIT_JUMP);

            /* --- MISS / bailout labels (all fall through to runtime call) --- */
            {
                struct sljit_label *miss_lbl = sljit_emit_label(compiler);
                sljit_set_label(ver_miss, miss_lbl);
                sljit_set_label(exotic, miss_lbl);
                sljit_set_label(null_shape, miss_lbl);
                sljit_set_label(null_obj, miss_lbl);
                sljit_set_label(not_obj, miss_lbl);
                sljit_set_label(prop_not_int, miss_lbl);
                sljit_set_label(rhs_not_int, miss_lbl);
                sljit_set_label(ovf, miss_lbl);
            }

            /* Fallback: call runtime helper for full semantics.
             * Need contiguous [obj, rhs, result] in spill area.
             * Use src1 as the base (3 LRValue slots starting at src1). */
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);     /* RCX = interp */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                               SLJIT_IMM, (sljit_sw)prog);                       /* RDX = prog */
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);                 /* R8 = name_idx */
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));              /* R9 = args_base */
                /* Copy obj from src1 to args_base[0] */
                emit_copy_lrvalue(compiler, src1, src1, local_base);
                /* Copy rhs from src2 to args_base[1] */
                emit_copy_lrvalue(compiler, src2, src1 + 1, local_base);
                /* Helper writes result to args_base[2], then copy to dst */
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_add_prop);
                if (rc != SLJIT_SUCCESS) return rc;
                if (dst != src1 + 2) {
                    emit_copy_lrvalue(compiler, src1 + 2, dst, local_base);
                }
                sljit_emit_label(compiler);
            }
            break;
        }

        /* set_prop: lr_jit_rt_set_prop(interp, prog, name_idx, args_base*)
         *   args_base[0] = obj, args_base[1] = val, args_base[2] = result.
         *   arg0 = interp, arg1 = prog, arg2 = name_idx (imm32), arg3 = &src1 (base_var) */
        case MIR_OP_set_prop: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(base*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_set_prop);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* get_elem: lr_jit_rt_get_elem(interp, obj*, key*, out*)
         *   arg0 = interp, arg1 = &src1 (obj), arg2 = &src2 (key), arg3 = &dst */
        case MIR_OP_get_elem: {
            int dst = instr->dst, src1 = instr->src1, src2 = instr->src2;
            if (dst >= 0 && src1 >= 0 && src2 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(obj*), R2=R8(key*), TMP_REG1=R9(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src2 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_get_elem);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* set_elem: lr_jit_rt_set_elem(interp, args_base*, out*)
         *   args_base[obj, key, val]; out gets the assigned val.
         *   arg0 = interp, arg1 = &src1 (base_var), arg2 = &dst */
        case MIR_OP_set_elem: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(base*), R2=R8(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_set_elem);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Operators: BC_IN / BC_INSTANCEOF.
         * in: lr_jit_rt_in(interp, obj*, key*, out*)
         *   arg0 = interp, arg1 = &src1 (obj), arg2 = &src2 (key), arg3 = &dst */
        case MIR_OP_in: {
            int dst = instr->dst, src1 = instr->src1, src2 = instr->src2;
            if (dst >= 0 && src1 >= 0 && src2 >= 0) {
                /* in: lr_jit_rt_in(interp, obj*, prop, out*)
                 *   ABI: R3=RCX(interp), R1=RDX(obj*), R2=R8(prop), TMP_REG1=R9(out) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src2 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_in);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* instanceof: lr_jit_rt_instanceof(interp, obj*, ctor*, out*)
         *   arg0 = interp, arg1 = &src1 (obj), arg2 = &src2 (ctor), arg3 = &dst */
        case MIR_OP_instanceof: {
            int dst = instr->dst, src1 = instr->src1, src2 = instr->src2;
            if (dst >= 0 && src1 >= 0 && src2 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(obj*), R2=R8(ctor*), TMP_REG1=R9(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src2 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_instanceof);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Exception: BC_THROW.
         * lr_jit_rt_throw(interp, val*) — does not return.
         *   arg0 = interp, arg1 = &src1
         * Follow with an unreachable trap so the JIT epilogue stays sane. */
        case MIR_OP_throw: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(val*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS2V(W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_throw);
                if (rc != SLJIT_SUCCESS) return rc;
                /* Throw should not return; emit breakpoint as a safety net. */
                sljit_emit_op0(compiler, SLJIT_BREAKPOINT);
            }
            break;
        }

        /* Nullish coalescing jump: BC_JUMP_IF_NOT_NULLISH.
         * Jump when src1.tag > LR_TYPE_NULL (i.e., not undefined and not null).
         * Value is kept on the MIR operand stack (matches bytecode semantics). */
        case MIR_OP_jump_if_not_nullish: {
            int32_t byte_off = (int32_t)instr->imm;
            if (byte_off == 0) break;
            if (!bc_offsets) break;
            int target_bc_off = cur_bc_off + 4 + byte_off;
            int target_mir_idx = find_mir_idx_for_bc_off(bc_offsets, num_instrs, target_bc_off);
            if (target_mir_idx < 0 || target_mir_idx > num_instrs) break;
            int cond = instr->src1;
            SLJIT_SKIP_CHECKS(compiler);
            sljit_emit_op2u(compiler, SLJIT_SUB32 | SLJIT_SET_GREATER_EQUAL,
                            MEM(local_base + cond * LRVALUE_SIZE),
                            SLJIT_IMM, 2);
            struct sljit_jump *j = sljit_emit_jump(compiler, SLJIT_GREATER);
            struct sljit_label *target = label_arr[target_mir_idx];
            if (target) {
                sljit_set_label(j, target);
            } else if (jump_stack) {
                int cap = *jump_stack_size + 1;
                if (cap > *jump_stack_cap) {
                    int new_cap = *jump_stack_cap * 2;
                    if (new_cap < cap) new_cap = cap;
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)new_cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)new_cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; *jump_stack_cap = new_cap; }
                } else {
                    struct sljit_jump **ns = (struct sljit_jump **)realloc(*jump_stack, (size_t)cap * sizeof(struct sljit_jump *));
                    int *nt = (int *)realloc(*jump_target_stack, (size_t)cap * sizeof(int));
                    if (ns && nt) { *jump_stack = ns; *jump_target_stack = nt; }
                }
                if (*jump_stack) { (*jump_stack)[*jump_stack_size] = j; (*jump_target_stack)[*jump_stack_size] = target_mir_idx; (*jump_stack_size)++; }
            }
            break;
        }

        /* Method/element calls.
         * call_method: lr_jit_rt_call_method(interp, prog, packed, args_base*)
         *   packed = (name_idx << 16) | argc.
         *   args_base[0..argc] = [this, arg1..argN] (inputs),
         *   args_base[argc+1]   = result (output).
         *   arg0 = interp, arg1 = prog, arg2 = packed (imm32), arg3 = &src1 (base_var) */
        case MIR_OP_call_method: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(prog), R2=R8(packed), TMP_REG1=R9(base*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_call_method);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Atomics.* ops: lr_jit_rt_atomics_* (interp, argc, args_base*, out*)
         *   args_base layout: [typedArray, index, value...] (argc contiguous).
         *   out = args_base[argc] (last slot).
         *   arg0=RCX(interp), arg1=RDX(argc), arg2=R8(args_base*), arg3=R9(out*). */
        case MIR_OP_atomics_load:
        case MIR_OP_atomics_store:
        case MIR_OP_atomics_add:
        case MIR_OP_atomics_sub:
        case MIR_OP_atomics_and:
        case MIR_OP_atomics_or:
        case MIR_OP_atomics_xor:
        case MIR_OP_atomics_exchange:
        case MIR_OP_atomics_compare_exchange:
        case MIR_OP_atomics_is_lock_free: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                uint32_t argc = (uint32_t)(instr->imm & 0xFFFF);
                void (*fn)(void*) = NULL;
                switch ((MIROpcode)instr->opcode) {
                    case MIR_OP_atomics_load:            fn = (void(*)(void*))&lr_jit_rt_atomics_load;            break;
                    case MIR_OP_atomics_store:           fn = (void(*)(void*))&lr_jit_rt_atomics_store;           break;
                    case MIR_OP_atomics_add:             fn = (void(*)(void*))&lr_jit_rt_atomics_add;             break;
                    case MIR_OP_atomics_sub:             fn = (void(*)(void*))&lr_jit_rt_atomics_sub;             break;
                    case MIR_OP_atomics_and:             fn = (void(*)(void*))&lr_jit_rt_atomics_and;             break;
                    case MIR_OP_atomics_or:              fn = (void(*)(void*))&lr_jit_rt_atomics_or;              break;
                    case MIR_OP_atomics_xor:             fn = (void(*)(void*))&lr_jit_rt_atomics_xor;             break;
                    case MIR_OP_atomics_exchange:        fn = (void(*)(void*))&lr_jit_rt_atomics_exchange;        break;
                    case MIR_OP_atomics_compare_exchange:fn = (void(*)(void*))&lr_jit_rt_atomics_compare_exchange;break;
                    case MIR_OP_atomics_is_lock_free:    fn = (void(*)(void*))&lr_jit_rt_atomics_is_lock_free;    break;
                }
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                               SLJIT_IMM, (sljit_sw)argc);
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)fn);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* MIR_OP_atomics_cache: extract TypedArray buffer info into
         * 4 consecutive local slots starting at dst:
         *   dst[0] = buffer_base (uint64, LRValue.ptr)
         *   dst[1] = byte_offset (uint64, LRValue.float64 or int32)
         *   dst[2] = element_size (uint64)
         *   dst[3] = magic (uint64)
         * Reads typed array from src1 (the args_base var whose slot 0 is the TA). */
        case MIR_OP_atomics_cache: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* R3=RCX(interp), R1=RDX(typedArray*), R2=R8(out_base*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                /* Load typedArray LRValue pointer from args_base[0] */
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                /* out_base points to dst (4 slots = 64 bytes) */
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS3V(W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_atomics_cache);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* MIR_OP_atomics_inline_*: uses pre-cached TypedArray info.
         * Cache is stored at src1 (5 LRValue slots = 80 bytes):
         *   src1[0]: ptr=base_ptr, float64=buf_size
         *   src1[1]: float64=byte_offset
         *   src1[2]: float64=element_size
         *   src1[3]: float64=magic
         *   src1[4]: float64=valid (1.0)
         *
         * ABI (Windows x64 SLJIT_CALL_REG_ARG, 4 params):
         *   R3(CX)=interp, R1(DX)=cache_ptr(src1), R2(R8)=args_base_ptr(src1),
         *   TMP_REG1(R9)=out_ptr(dst)
         */
        case MIR_OP_atomics_inline_load:
        case MIR_OP_atomics_inline_store:
        case MIR_OP_atomics_inline_add:
        case MIR_OP_atomics_inline_sub:
        case MIR_OP_atomics_inline_and:
        case MIR_OP_atomics_inline_or:
        case MIR_OP_atomics_inline_xor:
        case MIR_OP_atomics_inline_exchange:
        case MIR_OP_atomics_inline_compare_exchange: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                void (*fn)(void*) = NULL;
                switch ((MIROpcode)instr->opcode) {
                    case MIR_OP_atomics_inline_load:            fn = (void(*)(void*))&lr_jit_rt_atomics_inline_load;            break;
                    case MIR_OP_atomics_inline_store:           fn = (void(*)(void*))&lr_jit_rt_atomics_inline_store;           break;
                    case MIR_OP_atomics_inline_add:             fn = (void(*)(void*))&lr_jit_rt_atomics_inline_add;             break;
                    case MIR_OP_atomics_inline_sub:             fn = (void(*)(void*))&lr_jit_rt_atomics_inline_sub;             break;
                    case MIR_OP_atomics_inline_and:             fn = (void(*)(void*))&lr_jit_rt_atomics_inline_and;             break;
                    case MIR_OP_atomics_inline_or:              fn = (void(*)(void*))&lr_jit_rt_atomics_inline_or;              break;
                    case MIR_OP_atomics_inline_xor:             fn = (void(*)(void*))&lr_jit_rt_atomics_inline_xor;             break;
                    case MIR_OP_atomics_inline_exchange:        fn = (void(*)(void*))&lr_jit_rt_atomics_inline_exchange;        break;
                    case MIR_OP_atomics_inline_compare_exchange:fn = (void(*)(void*))&lr_jit_rt_atomics_inline_compare_exchange;break;
                    default: break;
                }
                /* R3 = interp */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                /* R1 = cache_ptr (points to src1 LRValue array, where cache was written) */
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                /* R2 = args_base_ptr (also points to src1 LRValue array) */
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                /* TMP_REG1(R9) = out_ptr (points to dst) */
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)fn);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* MIR_OP_method_cache: lr_jit_rt_method_cache(interp, src1_base, dst_base, method_name)
         *   src1_base = args_base for the call (contains this at [0])
         *   dst_base  = cache area (5 LRValue slots)
         *   method_name = instr->ptr (pre-resolved const char* from pool)
         *   imm = argc (stored in cache[2] for call_cached_method) */
        case MIR_OP_method_cache: {
            int src1 = instr->src1, dst = instr->dst;
            if (src1 >= 0 && dst >= 0 && instr->ptr) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_emit_op1(compiler, SLJIT_MOV, TMP_REG1, 0, SLJIT_IMM,
                    (sljit_sw)(uintptr_t)instr->ptr);
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_method_cache);
                 if (rc != SLJIT_SUCCESS) return rc;
                 /* Store argc into cache[2] so call_cached_method can read it.
                  * cache[2] is at dst_base + 32 bytes (3rd LRValue slot).
                  * We need to store as float64 with proper tag. */
                 int32_t argc = (int32_t)(instr->imm >> 16);
                 sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
                     (sljit_sw)argc);
                 sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0,
                     SLJIT_R1, 0);
                 sljit_emit_fmem(compiler, SLJIT_MOV_F64 | SLJIT_MEM_ALIGNED_32, SLJIT_FR0,
                     MEM(local_base + dst * LRVALUE_SIZE + 8 + 32));
                 emit_set_local_tag(compiler, dst + 2, local_base, LR_TYPE_FLOAT64);
            }
            break;
        }

        /* MIR_OP_call_cached_method: lr_jit_rt_call_cached_method(interp, cache, args_base, out)
         *   cache = src1 (5-LRValue cache from method_cache)
         *   args_base = same as original call_method's src1 (contains this + args)
         *   out = dst */
        case MIR_OP_call_cached_method: {
            int src1 = instr->src1, dst = instr->dst;
            if (src1 >= 0 && dst >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_get_local_base(compiler, SLJIT_R1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_call_cached_method);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* MIR_OP_private_field_get: same ABI as get_prop
         *   R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(base*)
         *   base[0]=obj, base[1]=result */
        case MIR_OP_private_field_get: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_private_field_get);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* MIR_OP_private_field_set: same ABI as set_prop
         *   R3=RCX(interp), R1=RDX(prog), R2=R8(name_idx), TMP_REG1=R9(base*)
         *   base[0]=obj, base[1]=val, base[2]=result */
        case MIR_OP_private_field_set: {
            int src1 = instr->src1;
            if (src1 >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R2, 0,
                    SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, W, 32, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_private_field_set);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* call_elem: lr_jit_rt_call_elem(interp, argc, args_base*, out*)
         *   args_base layout: [this, key, arg1, ..., argN] (argc+2 contiguous).
         *   arg0 = interp, arg1 = argc (imm32), arg2 = &src1 (base), arg3 = &dst */
        case MIR_OP_call_elem: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, 32, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_call_elem);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* construct: lr_jit_rt_construct(interp, argc, args_base*, out*)
         *   args_base layout: [callee, arg1, ..., argN] (argc+1 contiguous).
         *   arg0 = interp, arg1 = argc (imm32), arg2 = &src1 (base), arg3 = &dst */
        case MIR_OP_construct: {
            int dst = instr->dst, src1 = instr->src1;
            if (dst >= 0 && src1 >= 0) {
                /* ABI order: R3=RCX(interp), R1=RDX(argc), R2=R8(args_base*), TMP_REG1=R9(out*) */
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0,
                               SLJIT_IMM, (sljit_sw)(uint32_t)instr->imm);
                sljit_get_local_base(compiler, SLJIT_R2, 0,
                    (sljit_sw)(local_base + src1 * LRVALUE_SIZE));
                sljit_get_local_base(compiler, TMP_REG1, 0,
                    (sljit_sw)(local_base + dst * LRVALUE_SIZE));
                sljit_s32 rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                    SLJIT_ARGS4V(W, 32, W, W),
                    SLJIT_IMM, (sljit_sw)&lr_jit_rt_construct);
                if (rc != SLJIT_SUCCESS) return rc;
            }
            break;
        }

        /* Stack ops and loop markers — no native code emitted (handled in
         * MIR compiler as stack/operand reordering).  Defined here so the
         * codegen switch is exhaustive and produces no "unhandled opcode"
         * noise when these slip through. */
        case MIR_OP_push_int32:
        case MIR_OP_pop:
        case MIR_OP_dup:
        case MIR_OP_swap:
        case MIR_OP_loop_start:
        case MIR_OP_loop_end:
            break;

        case MIR_OP_loop_guard: {
            /* Call lr_jit_rt_notify_loop(interp, prog).
             * Returns 1 if compilation was triggered -> bailout to interpreter.
             * Returns 0 -> continue loop normally. */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S0, 0);       /* RCX = interp */
            sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)prog);  /* RDX = prog */
            sljit_s32 icall_rc = sljit_emit_icall(compiler, SLJIT_CALL_REG_ARG,
                SLJIT_ARGS2V(W, W),
                SLJIT_IMM, (sljit_sw)&lr_jit_rt_notify_loop);
            if (icall_rc != SLJIT_SUCCESS) return icall_rc;
            /* R0 = 1 if triggered, 0 otherwise */
            struct sljit_jump *ok = sljit_emit_jump(compiler, SLJIT_EQUAL);
            /* Bailout path: signal interpreter to re-run with new JIT code */
            emit_bailout(compiler, result_ptr_offset);
            /* Fall-through path: continue loop */
            struct sljit_label *ok_lbl = sljit_emit_label(compiler);
            (void)ok_lbl;
            (void)ok;
            break;
        }

        default:
            break;
    }

    return SLJIT_SUCCESS;
}

uint8_t *codegen_emit(MIRProgram *mir, size_t *out_size, int nparams, void *prog) {
    if (!mir || !out_size) return NULL;

    /* Validate all variable indices are within bounds before emitting any code.
     * Out-of-bounds indices would produce load/store offsets beyond the fixed
     * stack frame, causing access violations at runtime. */
    for (int i = 0; i < mir->num_instructions; i++) {
        MIRInstr *instr = &mir->instructions[i];
        if ((instr->dst >= MAX_LOCALS) ||
            (instr->src1 >= 0 && instr->src1 >= MAX_LOCALS) ||
            (instr->src2 >= 0 && instr->src2 >= MAX_LOCALS)) {
            LR_JIT_ERR("[JIT-ERR] codegen: var index out of bounds (%d/%d/%d >= MAX_LOCALS=%d) at instr %d\n",
                    instr->dst, instr->src1, instr->src2, MAX_LOCALS, i);
            *out_size = 0;
            return NULL;
        }
    }

    int max_vars = mir->num_vars;
    if (max_vars <= 0) max_vars = 1;
    if (max_vars > MAX_LOCALS) max_vars = MAX_LOCALS;

    /* Stack space: all locals + result pointer + alignment + shadow space */
    int stack_bytes = MAX_LOCALS * LRVALUE_SIZE + 64;
    stack_bytes = (stack_bytes + 15) & ~15;

    struct sljit_compiler *compiler = sljit_create_compiler(NULL);
    if (!compiler) return NULL;
    sljit_compiler_verbose(compiler, stderr);

    /*
     * Function: void (*)(Interpreter *, uint8_t *, LRValue *)
     * Windows x64 Microsoft ABI: RCX=arg0(interp), RDX=arg1(ip), R8=arg2(result)
     *
     * IMPORTANT: We deliberately do NOT declare the three pointer arguments to
     * sljit_emit_enter (i.e. only SLJIT_ARG_RETURN here).  Under _WIN64_ABI with
     * 3 declared args, SLJIT has occasionally emitted a spurious stack-slot load
     * (e.g. `mov r9, [rsp - local_size]`) BEFORE the `sub rsp, local_size`
     * instruction, causing a read from unallocated stack memory.  Instead we
     * manually copy the three incoming argument registers into S0..S2 right
     * after the prologue.
     *
     * The SLJIT virtual �?physical register mapping under _WIN64_ABI is:
     *   SLJIT_R3  �? RCX   (1st arg)
     *   SLJIT_R1  �? RDX   (2nd arg)
     *   SLJIT_R2  �? R8    (3rd arg)
     *   SLJIT_S0  �? RBX   (callee-saved slot for interp*)
     *   SLJIT_S1  �? RSI   (callee-saved slot for ip)
     *   SLJIT_S2  �? RDI   (callee-saved slot, not strictly needed but mirrors
     *                        the old layout)
     */
    sljit_s32 arg_types = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
                      | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1)
                      | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 2)
                      | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 3);
    sljit_s32 options = 0;
    /* Encode both general and float register counts into the combined parameter.
     * Bits 0-7  = general scratches, bits 8-15 = float scratches.
     * We need at least 4 float regs for f64 ops (neg_f64, add_f64, etc.). */
    sljit_s32 scratches = NUM_SCRATCH_REGS | (NUM_SCRATCH_REGS << 8);
    sljit_s32 saveds = NUM_SAVED_REGS | (NUM_SAVED_REGS << 8);

    int enter_rc = sljit_emit_enter(compiler, options, arg_types, scratches, saveds, stack_bytes);
    if (enter_rc != SLJIT_SUCCESS || compiler->error != SLJIT_SUCCESS) {
        LR_JIT_ERR("[JIT-ERR] sljit_emit_enter failed: rc=%d compiler->error=%d\n",
                enter_rc, compiler->error);
        sljit_free_compiler(compiler);
        return NULL;
    }

    /* Local spill area starts at SLJIT_LOCALS_OFFSET (auto-added by SLJIT's ADJUST_LOCAL_OFFSET). */
    sljit_sw local_base = 0;

    /* RCX→S0, RDX→S1, R8→S2 are auto-saved by SLJIT from the declared arg_types above
     * (all three are SLJIT_ARG_TYPE_P, so SLJIT saves them to S0, S1, S2). */

    /* Prologue: copy function arguments from current_scope->values[] to locals.
     * IMPORTANT: max_init counts bytecode LOCAL SLOTS ONLY (load_local's src1
     * and store_local's src2).  MIR temporaries (ret/src, arithmetic src/dst)
     * live above MIR_VAR_BASE and are written by their own instructions, so
     * they must NOT extend the scope copy — scope->values only holds
     * "this" + nparams + declared locals. */
    if (nparams > 0) {
        int max_init = nparams + 1;  /* slot 0 = this, slots 1..nparams = params */
        for (int i = 0; i < mir->num_instructions; i++) {
            MIRInstr *instr = &mir->instructions[i];
            if (instr->opcode == MIR_OP_load_local) {
                if (instr->src1 >= 0 && instr->src1 >= max_init) max_init = instr->src1 + 1;
            } else if (instr->opcode == MIR_OP_store_local) {
                if (instr->src2 >= 0 && instr->src2 >= max_init) max_init = instr->src2 + 1;
            }
        }
        if (max_init > MAX_LOCALS) max_init = MAX_LOCALS;
        LR_JIT_DBG("[JIT]   prologue: nparams=%d max_init=%d (copy slots 0..%d)\n",
                    nparams, max_init, max_init - 1);

        /* R0 = interp = S0 (auto-saved by SLJIT from RCX) */
        { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue line %d: rc=%d err=%d\n", __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
        /* Check jit_args_base: *(interp + INTERP_JIT_ARGS_BASE_OFF).
         * If non-NULL, load from it directly (recursive JIT call path);
         * otherwise load from current_scope->values (normal path). */
        struct sljit_jump *skip_to_scope;
        { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), INTERP_JIT_ARGS_BASE_OFF); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue line %d: rc=%d err=%d\n", __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
        { skip_to_scope = sljit_emit_cmp(compiler, SLJIT_EQUAL, SLJIT_R2, 0, SLJIT_IMM, 0); if (!skip_to_scope) { LR_JIT_ERR("[JIT-ERR] prologue line %d\n", __LINE__); sljit_free_compiler(compiler); return NULL; } }
        /* Load from jit_args_base when set (recursive JIT call path) */
        { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_R2, 0); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue line %d: rc=%d err=%d\n", __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
        struct sljit_jump *prologue_done;
        { prologue_done = sljit_emit_jump(compiler, SLJIT_JUMP); if (!prologue_done) { LR_JIT_ERR("[JIT-ERR] prologue line %d\n", __LINE__); sljit_free_compiler(compiler); return NULL; } }
        sljit_set_label(skip_to_scope, sljit_emit_label(compiler));
        /* Load from current_scope->values (normal path) */
        /* R0 = current_scope = *(interp + 16) */
        { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0), INTERP_CURRENT_SCOPE_OFF); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue line %d: rc=%d err=%d\n", __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
        /* R1 = values[] = *(current_scope + 16) */
        { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), SCOPE_VALUES_OFF); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue line %d: rc=%d err=%d\n", __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
        sljit_set_label(prologue_done, sljit_emit_label(compiler));

        /* Copy each argument slot from scope to locals.
         * SLJIT's ADJUST_LOCAL_OFFSET auto-adds SLJIT_LOCALS_OFFSET to all
         * SLJIT_MEM1(SLJIT_SP) accesses, so we pass raw offsets (off, off+4, etc.)
         * and SLJIT handles the rest. */
        for (int slot = 0; slot < max_init; slot++) {
            int off = slot * LRVALUE_SIZE;

            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), off); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d tag line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), off, SLJIT_R0, 0); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d tag store line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), off + 4); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d i32 line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), off + 4, SLJIT_R0, 0); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d i32 store line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), off + 8); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d val line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), off + 8, SLJIT_R0, 0); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d val store line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), off + 12); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d pad2 line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
            { sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), off + 12, SLJIT_R0, 0); if (rc != SLJIT_SUCCESS) { LR_JIT_ERR("[JIT-ERR] prologue slot %d pad2 store line %d: rc=%d err=%d\n", slot, __LINE__, rc, compiler->error); sljit_free_compiler(compiler); return NULL; } }
        }
        sljit_emit_label(compiler);
    }

    /* Save result ptr (R8 = SLJIT_R2, auto-saved to S2 by SLJIT prologue
     * because arg_types declares all 3 args as SLJIT_ARG_TYPE_P). */
    {
        sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV,
                                      MEM(RESULT_PTR_OFFSET), SLJIT_S2, 0);
        CHECK_SLJIT_NORC("manual save arg3 R8->S2 ->result_ptr slot");
    }

    /* V8 per-call-frame scope pinning: save the caller's current_scope.
     * This is needed because recursive JIT calls change interp->current_scope
     * to the callee's scope, but load_scope must read from the OUTER
     * function's scope (the one that was current when this JIT function
     * was entered).  We restore it on function exit.
     *
     * NOTE: R0 (RAX) was clobbered by the arg-copy loop above (it is used as
     * a scratch for the 32-bit copies), so we must RELOAD current_scope here
     * instead of trusting a stale register. */
    {
        sljit_s32 rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
        if (rc != SLJIT_SUCCESS) {
            LR_JIT_ERR("[JIT-ERR] prologue reload interp line %d: rc=%d\n", __LINE__, rc);
            sljit_free_compiler(compiler); return NULL;
        }
        rc = sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                            SLJIT_MEM1(SLJIT_R0), INTERP_CURRENT_SCOPE_OFF);
        if (rc != SLJIT_SUCCESS) {
            LR_JIT_ERR("[JIT-ERR] prologue reload scope line %d: rc=%d\n", __LINE__, rc);
            sljit_free_compiler(compiler); return NULL;
        }
        rc = sljit_emit_op1(compiler, SLJIT_MOV,
                            MEM(SAVED_SCOPE_OFFSET), SLJIT_R0, 0);
        if (rc != SLJIT_SUCCESS) {
            LR_JIT_ERR("[JIT-ERR] prologue save_scope line %d: rc=%d\n", __LINE__, rc);
            sljit_free_compiler(compiler); return NULL;
        }
    }

    if (compiler->error != SLJIT_SUCCESS) {
#if (defined SLJIT_ARGUMENT_CHECKS && SLJIT_ARGUMENT_CHECKS) || (defined SLJIT_DEBUG && SLJIT_DEBUG)
        LR_JIT_ERR("[JIT-ERR] SLJIT prologue error: %d (scratches=%d saveds=%d local_size=%d)\n",
                compiler->error, compiler->scratches, compiler->saveds, compiler->logical_local_size);
#else
        LR_JIT_ERR("[JIT-ERR] SLJIT prologue error: %d (scratches=%d saveds=%d)\n",
                compiler->error, compiler->scratches, compiler->saveds);
#endif
        sljit_free_compiler(compiler);
        return NULL;
    }

    /* Emit labels inline with code.  First label goes before any code,
     * then each subsequent label is created AFTER its instruction so that
     * SLJIT records the correct code position.  This avoids the bug where
     * a pre-pass creates all labels at size=0, causing forward jumps to
     * target the start of the function. */
    struct sljit_label **label_arr = NULL;
    if (mir->num_instructions > 0) {
        label_arr = (struct sljit_label **)calloc((size_t)(mir->num_instructions + 1), sizeof(struct sljit_label *));
        if (!label_arr) {
            sljit_free_compiler(compiler);
            return NULL;
        }
    }

    struct sljit_jump **jump_stack = NULL;
    int *jump_target_stack = NULL;
    int jump_stack_size = 0;
    int jump_stack_cap = 0;
    int had_error = 0;

    /* Pre-allocate jump stack with a reasonable initial capacity.
     * Without this, deferred jumps (where the target label hasn't been
     * created yet) are silently dropped because `else if (jump_stack)` is
     * always false when jump_stack is NULL. */
    jump_stack_cap = 64;
    jump_stack = (struct sljit_jump **)malloc((size_t)jump_stack_cap * sizeof(struct sljit_jump *));
    jump_target_stack = (int *)malloc((size_t)jump_stack_cap * sizeof(int));
    if (!jump_stack || !jump_target_stack) {
        free(jump_stack); free(jump_target_stack);
        free(label_arr);
        sljit_free_compiler(compiler);
        return NULL;
    }

    /* Create the first label before any code */
    if (mir->num_instructions > 0) {
        label_arr[0] = sljit_emit_label(compiler);
        if (!label_arr[0]) {
            LR_JIT_ERR("[JIT-ERR] sljit_emit_label failed at instr 0\n");
            free(label_arr);
            sljit_free_compiler(compiler);
            return NULL;
        }
    }

    for (int i = 0; i < mir->num_instructions; i++) {
        int cur_bc_off = (mir->bc_offsets ? mir->bc_offsets[i] : -1);
        if (lr_debug_jit_enabled()) {
            MIRInstr *cur_instr = &mir->instructions[i];
            if (cur_instr->opcode == MIR_OP_load_var || cur_instr->opcode == MIR_OP_runtime_call ||
                 cur_instr->opcode == MIR_OP_scope_call ||
                 cur_instr->opcode == MIR_OP_sub_i32 || cur_instr->opcode == MIR_OP_add_i32 ||
                 cur_instr->opcode == MIR_OP_const_i32 || cur_instr->opcode == MIR_OP_move ||
                 cur_instr->opcode == MIR_OP_load_local || cur_instr->opcode == MIR_OP_load_scope ||
                 cur_instr->opcode == MIR_OP_inline_call)
                 LR_JIT_DBG("[CGEN-TRACE] instr[%d] op=%d dst=%d src1=%d src2=%d imm=%lld bc=%d\n",
                         i, (int)cur_instr->opcode,
                         cur_instr->dst, cur_instr->src1, cur_instr->src2,
                         (long long)cur_instr->imm, cur_bc_off);
        }
        sljit_s32 rc = emit_mir_instr(compiler, &mir->instructions[i], i, local_base,
                                       RESULT_PTR_OFFSET,
                                       label_arr, mir->num_instructions,
                                       mir->bc_offsets, cur_bc_off, prog,
                                       &jump_stack, &jump_target_stack, &jump_stack_size,
                                       &jump_stack_cap);
        if (rc != SLJIT_SUCCESS) {
            LR_JIT_ERR("[JIT-ERR] SLJIT error at instruction %d opcode=%d: %d\n",
                    i, mir->instructions[i].opcode, rc);
            free(jump_stack); free(jump_target_stack); free(label_arr);
            sljit_free_compiler(compiler);
            return NULL;
        }
        if (compiler->error != SLJIT_SUCCESS) {
            LR_JIT_ERR("[JIT-ERR] SLJIT compiler error at instr %d opcode=%d err=%d\n",
                    i, mir->instructions[i].opcode, compiler->error);
#if (defined SLJIT_ARGUMENT_CHECKS && SLJIT_ARGUMENT_CHECKS) || (defined SLJIT_DEBUG && SLJIT_DEBUG)
            LR_JIT_ERR("[JIT-ERR]   last_flags=0x%x", compiler->last_flags);
            LR_JIT_ERR(" last_return=%d", compiler->last_return);
#endif
            LR_JIT_ERR(" (stale from earlier instr)\n");
            for (int j = i - 1; j >= 0 && j >= i - 5; j--) {
                LR_JIT_ERR("[JIT-ERR]   prev_instr[%d] op=%d dst=%d src1=%d src2=%d imm=%lld\n",
                        j, mir->instructions[j].opcode,
                        mir->instructions[j].dst, mir->instructions[j].src1,
                        mir->instructions[j].src2, (long long)mir->instructions[j].imm);
            }
            free(jump_stack); free(jump_target_stack); free(label_arr);
            sljit_free_compiler(compiler);
            return NULL;
        }
        if (compiler->error != SLJIT_SUCCESS && !had_error) {
            had_error = 1;
            LR_JIT_ERR("[JIT-ERR] ERROR SET at instr %d opcode=%d err=%d\n",
                    i, mir->instructions[i].opcode, compiler->error);
        }
        /* Create next label AFTER current instruction code has been emitted */
        if (i + 1 < mir->num_instructions) {
            label_arr[i + 1] = sljit_emit_label(compiler);
            if (!label_arr[i + 1]) {
                LR_JIT_ERR("[JIT-ERR] sljit_emit_label failed at instr %d\n", i + 1);
                free(label_arr);
                sljit_free_compiler(compiler);
                return NULL;
            }
        }
    }

    /* Create the end-of-function label at index num_instrs.  Unconditional
     * jumps to the implicit fall-through (epilogue) target this label, so it
     * must exist or those jumps would be dropped. */
    if (mir->num_instructions > 0) {
        label_arr[mir->num_instructions] = sljit_emit_label(compiler);
        if (!label_arr[mir->num_instructions]) {
            LR_JIT_ERR("[JIT-ERR] sljit_emit_label failed at end label\n");
            free(label_arr);
            sljit_free_compiler(compiler);
            return NULL;
        }
    }

    /* Patch all saved jumps to their target labels. */
    LR_JIT_DBG("[JIT] Patching %d deferred jumps...\n", jump_stack_size);
    for (int k = 0; k < jump_stack_size; k++) {
        int target_idx = jump_target_stack[k];
        if (target_idx >= 0 && target_idx <= mir->num_instructions) {
            struct sljit_label *target = label_arr[target_idx];
            LR_JIT_DBG("[JIT]   patch jump[%d] -> label[%d] = %p (label->size=%d)\n",
                        k, target_idx, (void*)target, target ? (int)target->size : -1);
            if (target) sljit_set_label(jump_stack[k], target);
        }
    }
    free(jump_stack);
    free(jump_target_stack);

    /* If no return was emitted, fall through to implicit bailout */
    int saw_ret = 0;
    for (int i = 0; i < mir->num_instructions; i++) {
        if (mir->instructions[i].opcode == MIR_OP_ret) { saw_ret = 1; break; }
    }
    if (!saw_ret) {
        /* V8 per-call-frame: restore caller's current_scope before implicit return. */
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, MEM(SAVED_SCOPE_OFFSET));
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), INTERP_CURRENT_SCOPE_OFF, SLJIT_R0, 0);
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0,
                       MEM(RESULT_PTR_OFFSET));
        /* Falling off the end of a function is an implicit `return undefined`.
         * Emit LR_VALUE_UNDEFINED (tag=0, value=0) instead of the -1 bailout
         * sentinel: returning -1 makes the interpreter re-run the whole body,
         * double-executing side effects (global inc, string alloc) and
         * corrupting the heap. */
        sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32,
                       SLJIT_MEM1(SLJIT_R1), 0, SLJIT_R0, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32,
                       SLJIT_MEM1(SLJIT_R1), 4, SLJIT_R0, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32,
                       SLJIT_MEM1(SLJIT_R1), 8, SLJIT_R0, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
        sljit_emit_op1(compiler, SLJIT_MOV32,
                       SLJIT_MEM1(SLJIT_R1), 12, SLJIT_R0, 0);
        sljit_emit_return_void(compiler);
    }

    if (lr_debug_jit_enabled()) {
        struct sljit_jump *j = compiler->jumps;
        int jump_count = 0;
        while (j) {
            int has_addr = (j->flags & 0x1) ? 1 : 0;
            int has_label = (j->u.label != NULL) ? 1 : 0;
            int type = (int)(j->flags >> 17);
            LR_JIT_DBG("[JIT]   jump[%d] addr=%p flags=0x%x type=%d has_JUMP_ADDR=%d has_label=%d\n",
                    jump_count, (void*)j, (unsigned)j->flags, type, has_addr, has_label);
            jump_count++;
            j = j->next;
        }
        LR_JIT_DBG("[JIT]   total jumps: %d, compiler->size=%d\n", jump_count, (int)compiler->size);
    }

    uint8_t *code = (uint8_t *)sljit_generate_code(compiler, 0, NULL);
    size_t code_size = sljit_get_generated_code_size(compiler);
    /* Save error BEFORE freeing the compiler (sljit_free_compiler frees the
     * compiler struct itself, so reading compiler->error afterwards is
     * use-after-free). */
    int gen_error = compiler->error;

    if (lr_debug_jit_enabled()) {
        LR_JIT_DBG("[JIT] Generated %zu bytes of native code\n", code_size);
        LR_JIT_DBG("[JIT] codegen status=%d (%s)\n", gen_error,
                   (gen_error == SLJIT_ERR_COMPILED) ? "SLJIT_ERR_COMPILED" : "FAILED");
        LR_JIT_DBG("[JIT] Machine code dump:\n");
        for (size_t i = 0; i < code_size; i += 16) {
            LR_JIT_DBG("  [%03zx]: ", i);
            for (size_t j = 0; j < 16 && i + j < code_size; j++)
                LR_JIT_DBG("%02x ", (unsigned char)code[i + j]);
            LR_JIT_DBG("\n");
        }
    }

    sljit_free_compiler(compiler);
    free(label_arr);

    if (!code || code_size == 0 || gen_error != SLJIT_ERR_COMPILED) {
        LR_JIT_ERR("[JIT-ERR] ERROR: code=%p code_size=%zu error=%d\n",
                (void*)code, code_size, gen_error);
        return NULL;
    }

    *out_size = code_size;
    return code;
}
