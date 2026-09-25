/*
 * LR_JS — Bytecode VM (modular)
 *
 * Module: serialize
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
   SERIALIZATION  (IOME586 archive)
   ======================================================================= */

#define BC_SER_MAGIC   "LRBC"
#define BC_SER_VERSION 2u
#define BC_SER_FLAG_NODE_REFS 1u

static void put_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint8_t *bc_serialize(BCProgram *prog, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!prog || !prog->code || !out_len) return NULL;

    size_t total = 4 + 4 + 4 + 4 + 4 + (size_t)prog->code_len;
    for (int i = 0; i < prog->pool_count; i++) {
        total += 1;
        switch (prog->pool[i].kind) {
        case BC_POOL_INT32:   total += 4; break;
        case BC_POOL_FLOAT64: total += 8; break;
        case BC_POOL_STRING:  total += 4 + strlen(prog->pool[i].u.str) + 1; break;
        case BC_POOL_NODE:    break;
        }
    }
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    size_t pos = 0;
    memcpy(buf, BC_SER_MAGIC, 4); pos += 4;
    put_u32(buf + pos, BC_SER_VERSION); pos += 4;
    put_u32(buf + pos, prog->node_refs ? BC_SER_FLAG_NODE_REFS : 0u); pos += 4;
    put_u32(buf + pos, (uint32_t)prog->code_len); pos += 4;
    put_u32(buf + pos, (uint32_t)prog->pool_count); pos += 4;
    memcpy(buf + pos, prog->code, (size_t)prog->code_len);
    pos += (size_t)prog->code_len;

    for (int i = 0; i < prog->pool_count; i++) {
        buf[pos++] = (uint8_t)prog->pool[i].kind;
        switch (prog->pool[i].kind) {
        case BC_POOL_INT32:
            put_u32(buf + pos, (uint32_t)prog->pool[i].u.i32); pos += 4;
            break;
        case BC_POOL_FLOAT64:
            memcpy(buf + pos, &prog->pool[i].u.f64, 8); pos += 8;
            break;
        case BC_POOL_STRING: {
            size_t sl = strlen(prog->pool[i].u.str);
            put_u32(buf + pos, (uint32_t)sl); pos += 4;
            memcpy(buf + pos, prog->pool[i].u.str, sl + 1); pos += sl + 1;
            break;
        }
        case BC_POOL_NODE:
            break;
        }
    }
    *out_len = pos;
    return buf;
}

BCProgram *bc_deserialize(const uint8_t *data, size_t len)
{
    if (!data || len < 20 || memcmp(data, BC_SER_MAGIC, 4) != 0) return NULL;
    if (get_u32(data + 4) != BC_SER_VERSION) return NULL;
    uint32_t flags = get_u32(data + 8);
    if (flags & BC_SER_FLAG_NODE_REFS) return NULL;   /* needs the AST */

    uint32_t code_len = get_u32(data + 12);
    uint32_t pool_count = get_u32(data + 16);
    size_t pos = 20;
    if (pos + code_len > len) return NULL;

    BCProgram *p = bc_new_program();
    if (!p) return NULL;
    p->code_len = (int32_t)code_len;
    p->code_cap = (int32_t)code_len + 16;
    p->code = (uint8_t *)malloc((size_t)p->code_cap);
    if (!p->code) { bc_free_program(p); return NULL; }
    memcpy(p->code, data + pos, code_len);
    pos += code_len;

    p->pool_count = (int32_t)pool_count;
    p->pool_cap = (int32_t)pool_count + 8;
    p->pool = (BCConst *)calloc((size_t)p->pool_cap, sizeof(BCConst));
    if (!p->pool) { bc_free_program(p); return NULL; }

    for (uint32_t i = 0; i < pool_count; i++) {
        if (pos >= len) { bc_free_program(p); return NULL; }
        p->pool[i].kind = (BCPoolKind)data[pos++];
        switch (p->pool[i].kind) {
        case BC_POOL_INT32:
            if (pos + 4 > len) { bc_free_program(p); return NULL; }
            p->pool[i].u.i32 = (int32_t)get_u32(data + pos); pos += 4;
            break;
        case BC_POOL_FLOAT64:
            if (pos + 8 > len) { bc_free_program(p); return NULL; }
            memcpy(&p->pool[i].u.f64, data + pos, 8); pos += 8;
            break;
        case BC_POOL_STRING: {
            if (pos + 4 > len) { bc_free_program(p); return NULL; }
            uint32_t sl = get_u32(data + pos); pos += 4;
            if (pos + sl + 1 > len) { bc_free_program(p); return NULL; }
            p->pool[i].u.str = (char *)malloc(sl + 1);
            if (!p->pool[i].u.str) { bc_free_program(p); return NULL; }
            memcpy(p->pool[i].u.str, data + pos, sl + 1);
            p->pool[i].u.str[sl] = '\0';
            pos += sl + 1;
            break;
        }
        case BC_POOL_NODE:
            bc_free_program(p);
            return NULL;
        }
    }
    p->compiled = 1;
    p->max_stack = 64;
    return p;
}

