/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: disasm
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

/* =======================================================================
   DISASSEMBLER (debugging aid)
   ======================================================================= */

typedef struct { const char *name; int operands; } BCInfo;

/* bc_info is a regular function (not part of the threaded-code executor),
   so we must use `case op:` here regardless of LR_THREADED_CODE mode. */
#undef BC_CASE
#define BC_CASE(lbl, op) case op:

/* operands: 0 none, 1 = u16, 2 = i32, 3 = u16+u16, 4 = u16+u8 */
static BCInfo bc_info(uint8_t op)
{
    BCInfo t;
    t.name = "?"; t.operands = 0;
    switch (op) {
    BC_CASE(stop, BC_STOP) t.name = "STOP"; break;
    BC_CASE(nop, BC_NOP) t.name = "NOP"; break;
    BC_CASE(push_undefined, BC_PUSH_UNDEFINED) t.name = "PUSH_UNDEFINED"; break;
    BC_CASE(push_null, BC_PUSH_NULL) t.name = "PUSH_NULL"; break;
    BC_CASE(push_true, BC_PUSH_TRUE) t.name = "PUSH_TRUE"; break;
    BC_CASE(push_false, BC_PUSH_FALSE) t.name = "PUSH_FALSE"; break;
    BC_CASE(push_this, BC_PUSH_THIS) t.name = "PUSH_THIS"; break;
    BC_CASE(push_int32, BC_PUSH_INT32) t.name = "PUSH_INT32"; t.operands = 2; break;
    BC_CASE(push_float64, BC_PUSH_FLOAT64) t.name = "PUSH_FLOAT64"; t.operands = 1; break;
    BC_CASE(push_string, BC_PUSH_STRING) t.name = "PUSH_STRING"; t.operands = 1; break;
    BC_CASE(pop, BC_POP) t.name = "POP"; break;
    BC_CASE(dup, BC_DUP) t.name = "DUP"; break;
    BC_CASE(dup2, BC_DUP2) t.name = "DUP2"; break;
    BC_CASE(swap, BC_SWAP) t.name = "SWAP"; break;
    BC_CASE(rot3, BC_ROT3) t.name = "ROT3"; break;
    BC_CASE(load_var, BC_LOAD_VAR) t.name = "LOAD_VAR"; t.operands = 1; break;
    BC_CASE(store_var, BC_STORE_VAR) t.name = "STORE_VAR"; t.operands = 1; break;
    BC_CASE(load_local, BC_LOAD_LOCAL) t.name = "LOAD_LOCAL"; t.operands = 1; break;
    BC_CASE(store_local, BC_STORE_LOCAL) t.name = "STORE_LOCAL"; t.operands = 1; break;
    BC_CASE(inc_local, BC_INC_LOCAL) t.name = "INC_LOCAL"; t.operands = 1; break;
    BC_CASE(inc_local_discard, BC_INC_LOCAL_DISCARD) t.name = "INC_LOCAL_DISCARD"; t.operands = 1; break;
    BC_CASE(inc_var, BC_INC_VAR) t.name = "INC_VAR"; t.operands = 1; break;
    BC_CASE(declare_var, BC_DECLARE_VAR) t.name = "DECLARE_VAR"; t.operands = 4; break;
    BC_CASE(typeof_var, BC_TYPEOF_VAR) t.name = "TYPEOF_VAR"; t.operands = 1; break;
    BC_CASE(add_self, BC_ADD_SELF) t.name = "ADD_SELF"; t.operands = 1; break;
    BC_CASE(mul_self, BC_MUL_SELF) t.name = "MUL_SELF"; t.operands = 1; break;
    BC_CASE(add, BC_ADD) t.name = "ADD"; break;
    BC_CASE(sub, BC_SUB) t.name = "SUB"; break;
    BC_CASE(mul, BC_MUL) t.name = "MUL"; break;
    BC_CASE(div, BC_DIV) t.name = "DIV"; break;
    BC_CASE(mod, BC_MOD) t.name = "MOD"; break;
    BC_CASE(pow, BC_POW) t.name = "POW"; break;
    BC_CASE(lt, BC_LT) t.name = "LT"; break;
    BC_CASE(gt, BC_GT) t.name = "GT"; break;
    BC_CASE(le, BC_LE) t.name = "LE"; break;
    BC_CASE(ge, BC_GE) t.name = "GE"; break;
    BC_CASE(eq, BC_EQ) t.name = "EQ"; break;
    BC_CASE(ne, BC_NE) t.name = "NE"; break;
    BC_CASE(strict_eq, BC_STRICT_EQ) t.name = "STRICT_EQ"; break;
    BC_CASE(strict_ne, BC_STRICT_NE) t.name = "STRICT_NE"; break;
    BC_CASE(shl, BC_SHL) t.name = "SHL"; break;
    BC_CASE(shr, BC_SHR) t.name = "SHR"; break;
    BC_CASE(sar, BC_SAR) t.name = "SAR"; break;
    BC_CASE(bit_and, BC_BIT_AND) t.name = "BIT_AND"; break;
    BC_CASE(bit_or, BC_BIT_OR) t.name = "BIT_OR"; break;
    BC_CASE(bit_xor, BC_BIT_XOR) t.name = "BIT_XOR"; break;
    BC_CASE(in, BC_IN) t.name = "IN"; break;
    BC_CASE(instanceof, BC_INSTANCEOF) t.name = "INSTANCEOF"; break;
    BC_CASE(neg, BC_NEG) t.name = "NEG"; break;
    BC_CASE(pos, BC_POS) t.name = "POS"; break;
    BC_CASE(not, BC_NOT) t.name = "NOT"; break;
    BC_CASE(bit_not, BC_BIT_NOT) t.name = "BIT_NOT"; break;
    BC_CASE(typeof, BC_TYPEOF) t.name = "TYPEOF"; break;
    BC_CASE(void, BC_VOID) t.name = "VOID"; break;
    BC_CASE(jump, BC_JUMP) t.name = "JUMP"; t.operands = 2; break;
    BC_CASE(jump_if_false, BC_JUMP_IF_FALSE) t.name = "JUMP_IF_FALSE"; t.operands = 2; break;
    BC_CASE(jump_if_true, BC_JUMP_IF_TRUE) t.name = "JUMP_IF_TRUE"; t.operands = 2; break;
    BC_CASE(jump_if_false_keep, BC_JUMP_IF_FALSE_KEEP) t.name = "JUMP_IF_FALSE_KEEP"; t.operands = 2; break;
    BC_CASE(jump_if_true_keep, BC_JUMP_IF_TRUE_KEEP) t.name = "JUMP_IF_TRUE_KEEP"; t.operands = 2; break;
    BC_CASE(jump_if_not_nullish, BC_JUMP_IF_NOT_NULLISH) t.name = "JUMP_IF_NOT_NULLISH"; t.operands = 2; break;
    BC_CASE(jump_if_local_lt_imm, BC_JUMP_IF_LOCAL_LT_IMM) t.name = "JUMP_IF_LOCAL_LT_IMM"; t.operands = 5; break;
    BC_CASE(loop_tick, BC_LOOP_TICK) t.name = "LOOP_TICK"; break;
    BC_CASE(call, BC_CALL) t.name = "CALL"; t.operands = 1; break;
    BC_CASE(call_method, BC_CALL_METHOD) t.name = "CALL_METHOD"; t.operands = 3; break;
    BC_CASE(call_elem, BC_CALL_ELEM) t.name = "CALL_ELEM"; t.operands = 1; break;
    BC_CASE(new, BC_NEW) t.name = "NEW"; t.operands = 1; break;
    BC_CASE(return, BC_RETURN) t.name = "RETURN"; break;
    BC_CASE(new_object, BC_NEW_OBJECT) t.name = "NEW_OBJECT"; break;
    BC_CASE(new_array, BC_NEW_ARRAY) t.name = "NEW_ARRAY"; t.operands = 1; break;
    BC_CASE(def_prop, BC_DEF_PROP) t.name = "DEF_PROP"; t.operands = 1; break;
    BC_CASE(def_elem, BC_DEF_ELEM) t.name = "DEF_ELEM"; break;
    BC_CASE(get_prop, BC_GET_PROP) t.name = "GET_PROP"; t.operands = 1; break;
    BC_CASE(load_prop, BC_LOAD_PROP) t.name = "LOAD_PROP"; t.operands = 3; break;
    BC_CASE(set_prop, BC_SET_PROP) t.name = "SET_PROP"; t.operands = 1; break;
    BC_CASE(get_elem, BC_GET_ELEM) t.name = "GET_ELEM"; break;
    BC_CASE(set_elem, BC_SET_ELEM) t.name = "SET_ELEM"; break;
    BC_CASE(delete_prop, BC_DELETE_PROP) t.name = "DELETE_PROP"; t.operands = 1; break;
    BC_CASE(delete_elem, BC_DELETE_ELEM) t.name = "DELETE_ELEM"; break;
    BC_CASE(iter_init, BC_ITER_INIT) t.name = "ITER_INIT"; break;
    BC_CASE(iter_next, BC_ITER_NEXT) t.name = "ITER_NEXT"; t.operands = 2; break;
    BC_CASE(iter_close, BC_ITER_CLOSE) t.name = "ITER_CLOSE"; break;
    BC_CASE(scope_enter, BC_SCOPE_ENTER) t.name = "SCOPE_ENTER"; break;
    BC_CASE(scope_leave, BC_SCOPE_LEAVE) t.name = "SCOPE_LEAVE"; break;
    BC_CASE(eval_node, BC_EVAL_NODE) t.name = "EVAL_NODE"; t.operands = 1; break;
    BC_CASE(eval_node_pop, BC_EVAL_NODE_POP) t.name = "EVAL_NODE_POP"; t.operands = 1; break;
    BC_CASE(set_result, BC_SET_RESULT) t.name = "SET_RESULT"; break;
    BC_CASE(clear_result, BC_CLEAR_RESULT) t.name = "CLEAR_RESULT"; break;
    BC_CASE(throw, BC_THROW) t.name = "THROW"; break;
    BC_CASE(to_string, BC_TO_STRING) t.name = "TO_STRING"; break;
    BC_CASE(to_number, BC_TO_NUMBER) t.name = "TO_NUMBER"; break;
    BC_CASE(to_bool, BC_TO_BOOL) t.name = "TO_BOOL"; break;
    default: break;
    }
    return t;
}

/* Map bc_info's operand encoding to the instruction's total byte length:
 * 0=none(1B), 1=u16(3B), 2=i32(5B), 3=u16+u16(5B), 4=u16+u8(4B),
 * 5=u16+i32+i32(11B, JUMP_IF_LOCAL_LT_IMM).  Used by the emitter to track
 * instruction boundaries so the LOAD_PROP+ADD fusion never corrupts an
 * operand byte. */
int bc_op_total_len(uint8_t op)
{
    switch (bc_info(op).operands) {
    case 0:  return 1;
    case 1:  return 3;
    case 2:  return 5;
    case 3:  return 5;
    case 4:  return 4;
    case 5:  return 11;
    default: return 1;
    }
}

char *bc_disassemble(BCProgram *prog)
{
    if (!prog || !prog->code) return NULL;
    size_t cap = 4096, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    char line[256];
    int pc = 0;
    while (pc < prog->code_len) {
        uint8_t op = prog->code[pc];
        BCInfo info = bc_info(op);
        int start = pc++;
        int n = 0;
        switch (info.operands) {
        case 1: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            pc += 2;
            n = snprintf(line, sizeof(line), "%05d  %-20s %u\n", start, info.name, a);
            break;
        }
        case 2: {
            int32_t a = (int32_t)((uint32_t)prog->code[pc] |
                                  ((uint32_t)prog->code[pc + 1] << 8) |
                                  ((uint32_t)prog->code[pc + 2] << 16) |
                                  ((uint32_t)prog->code[pc + 3] << 24));
            pc += 4;
            n = snprintf(line, sizeof(line), "%05d  %-20s %d (→%d)\n",
                         start, info.name, a, pc + a);
            break;
        }
        case 3: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            uint16_t b = (uint16_t)(prog->code[pc + 2] | (prog->code[pc + 3] << 8));
            pc += 4;
            n = snprintf(line, sizeof(line), "%05d  %-20s %u, %u\n", start, info.name, a, b);
            break;
        }
        case 4: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            uint8_t b = prog->code[pc + 2];
            pc += 3;
            n = snprintf(line, sizeof(line), "%05d  %-20s %u, kind=%u\n",
                         start, info.name, a, b);
            break;
        }
        case 5: {
            uint16_t a = (uint16_t)(prog->code[pc] | (prog->code[pc + 1] << 8));
            int32_t b = (int32_t)((uint32_t)prog->code[pc + 2] |
                                  ((uint32_t)prog->code[pc + 3] << 8) |
                                  ((uint32_t)prog->code[pc + 4] << 16) |
                                  ((uint32_t)prog->code[pc + 5] << 24));
            int32_t c = (int32_t)((uint32_t)prog->code[pc + 6] |
                                  ((uint32_t)prog->code[pc + 7] << 8) |
                                  ((uint32_t)prog->code[pc + 8] << 16) |
                                  ((uint32_t)prog->code[pc + 9] << 24));
            pc += 10;
            n = snprintf(line, sizeof(line), "%05d  %-20s slot=%u, imm=%d, offset=%d (→%d)\n",
                         start, info.name, a, b, c, pc + c);
            break;
        }
        default:
            n = snprintf(line, sizeof(line), "%05d  %s\n", start, info.name);
            break;
        }
        if (n < 0) break;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *no = (char *)realloc(out, cap);
            if (!no) break;
            out = no;
        }
        memcpy(out + len, line, (size_t)n);
        len += (size_t)n;
        out[len] = '\0';
    }
    return out;
}
