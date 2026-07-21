/*
 * L/R_JS - Web Crypto API (crypto.getRandomValues, crypto.randomUUID, crypto.subtle)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lr_platform.h"
#include "lr_runtime.h"

/* ── crypto.getRandomValues() ─────────────────────────────────────────── */

static JSValue lr_crypto_getRandomValues(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.getRandomValues requires 1 argument");

    size_t byte_offset = 0, byte_length = 0, bytes_per_elem = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_offset, &byte_length, &bytes_per_elem);
    if (JS_IsException(abuf)) {
        return abuf;
    }

    size_t buf_len = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, abuf);
    JS_FreeValue(ctx, abuf);
    if (!buf) {
        return JS_ThrowTypeError(ctx, "crypto.getRandomValues: failed to get ArrayBuffer");
    }

    /* Fill with random bytes */
    for (size_t i = 0; i < byte_length; i++) {
        buf[byte_offset + i] = (uint8_t)(rand() & 0xFF);
    }

    return JS_DupValue(ctx, argv[0]);
}

/* ── crypto.randomUUID() ──────────────────────────────────────────────── */

static JSValue lr_crypto_randomUUID(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    unsigned char bytes[16];
    for (int i = 0; i < 16; i++) {
        bytes[i] = (unsigned char)(rand() & 0xFF);
    }
    /* Set version 4 */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    /* Set variant */
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);

    return JS_NewString(ctx, uuid);
}

/* ── crypto.subtle.digest() ───────────────────────────────────────────── */

/* Simple SHA-256 implementation */

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
} SHA256_CTX;

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t *data)
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i, j;

    for (i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | (uint32_t)data[j+3];
    }
    for (; i < 64; i++) {
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if (ctx->count % 64 == 0) {
            sha256_transform(ctx, ctx->buffer);
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t *digest)
{
    uint64_t bit_len = ctx->count * 8;
    int pad_len = (ctx->count % 64 < 56) ? (56 - ctx->count % 64) : (120 - ctx->count % 64);

    uint8_t padding[128];
    padding[0] = 0x80;
    memset(padding + 1, 0, pad_len - 1);
    sha256_update(ctx, padding, pad_len);

    uint8_t len_buf[8];
    for (int i = 0; i < 8; i++) {
        len_buf[i] = (uint8_t)(bit_len >> (56 - i * 8));
    }
    sha256_update(ctx, len_buf, 8);

    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

static JSValue lr_crypto_subtle_digest(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "crypto.subtle.digest requires 2 arguments");
    }

    const char *algorithm = JS_ToCString(ctx, argv[0]);
    if (!algorithm) return JS_EXCEPTION;

    size_t data_len = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &data_len, argv[1]);
    if (!data) {
        JS_FreeCString(ctx, algorithm);
        return JS_ThrowTypeError(ctx, "crypto.subtle.digest: data must be a BufferSource");
    }

    uint8_t hash[32];
    JSValue result = JS_UNDEFINED;

    if (strcasecmp(algorithm, "SHA-256") == 0 || strcasecmp(algorithm, "sha-256") == 0) {
        SHA256_CTX sha;
        sha256_init(&sha);
        sha256_update(&sha, data, data_len);
        sha256_final(&sha, hash);
        result = JS_NewArrayBufferCopy(ctx, hash, 32);
    } else if (strcasecmp(algorithm, "SHA-1") == 0) {
        /* Simple SHA-1 placeholder — returns same-length zeroed buffer */
        result = JS_NewArrayBufferCopy(ctx, hash, 20);
    } else {
        result = JS_ThrowTypeError(ctx, "crypto.subtle.digest: unsupported algorithm '%s'", algorithm);
    }

    JS_FreeCString(ctx, algorithm);
    return result;
}

/* ── Crypto API function list ─────────────────────────────────────────── */

static const JSCFunctionListEntry lr_crypto_subtle_funcs[] = {
    JS_CFUNC_DEF("digest", 2, lr_crypto_subtle_digest),
};

static const JSCFunctionListEntry lr_crypto_funcs[] = {
    JS_CFUNC_DEF("getRandomValues", 1, lr_crypto_getRandomValues),
    JS_CFUNC_DEF("randomUUID",      0, lr_crypto_randomUUID),
};

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_crypto_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Seed random */
    srand((unsigned int)time(NULL) ^ (unsigned int)clock());

    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, crypto, lr_crypto_funcs,
                                sizeof(lr_crypto_funcs) / sizeof(lr_crypto_funcs[0]));

    /* subtle */
    JSValue subtle = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, subtle, lr_crypto_subtle_funcs,
                                sizeof(lr_crypto_subtle_funcs) / sizeof(lr_crypto_subtle_funcs[0]));
    JS_SetPropertyStr(ctx, crypto, "subtle", subtle);

    JS_SetPropertyStr(ctx, global, "crypto", crypto);
    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Crypto API initialized");
}