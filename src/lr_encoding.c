/*
 * L/R_JS - Encoding API (TextEncoder, TextDecoder)
 * Pure C, ES2022-compatible
 */
#include <stdlib.h>
#include <string.h>
#include "lr_runtime.h"

/* ── TextEncoder ──────────────────────────────────────────────────────── */

static JSValue lr_text_encoder_constructor(JSContext *ctx, JSValueConst new_target,
                                            int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    /* new_target is the newly created object with the correct prototype
     * already set by lr_call_constructor. Dup the value so the caller
     * can free the original without freeing the returned object. */
    return lr_dup_value(ctx, new_target);
}

static JSValue lr_text_encoder_encode(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    const char *input = "";
    if (argc >= 1) {
        input = JS_ToCString(ctx, argv[0]);
        if (!input) return JS_EXCEPTION;
    }

    size_t len = strlen(input);
    JSValue result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)input, len);
    if (argc >= 1) JS_FreeCString(ctx, input);

    if (!JS_IsException(result)) {
        /* Set the 'length' property so JS code can access encoded.length */
        JS_SetPropertyStr(ctx, result, "length", JS_NewInt64(ctx, (int64_t)len));
    }

    return result;
}

static JSValue lr_text_encoder_encodeInto(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "TextEncoder.encodeInto requires 2 arguments");
    }

    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;

    size_t len = strlen(input);
    uint8_t *dest = JS_GetArrayBuffer(ctx, NULL, argv[1]);
    if (!dest) {
        JS_FreeCString(ctx, input);
        return JS_ThrowTypeError(ctx, "encodeInto: destination must be Uint8Array");
    }

    size_t dest_size = 0;
    JS_GetArrayBuffer(ctx, &dest_size, argv[1]);

    size_t written = len < dest_size ? len : dest_size;
    memcpy(dest, input, written);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "read", JS_NewInt64(ctx, (int64_t)written));
    JS_SetPropertyStr(ctx, result, "written", JS_NewInt64(ctx, (int64_t)written));

    JS_FreeCString(ctx, input);
    return result;
}

static JSValue lr_text_encoder_encoding(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewString(ctx, "utf-8");
}

static const JSCFunctionListEntry lr_text_encoder_funcs[] = {
    JS_CFUNC_DEF("encode",     1, lr_text_encoder_encode),
    JS_CFUNC_DEF("encodeInto", 2, lr_text_encoder_encodeInto),
    JS_CGETSET_DEF("encoding", lr_text_encoder_encoding, NULL),
};

/* ── TextDecoder ──────────────────────────────────────────────────────── */

static JSValue lr_text_decoder_constructor(JSContext *ctx, JSValueConst new_target,
                                            int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    /* new_target is the newly created object with the correct prototype
     * already set by lr_call_constructor. Dup the value so the caller
     * can free the original without freeing the returned object. */
    return lr_dup_value(ctx, new_target);
}

static JSValue lr_text_decoder_decode(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NewString(ctx, "");

    size_t buf_len = 0;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &buf_len, argv[0]);
    if (!buf) {
        return JS_ThrowTypeError(ctx, "TextDecoder.decode: argument must be a BufferSource");
    }

    /* Handle stream option */
    if (argc >= 2) {
        JSValue options = argv[1];
        if (JS_IsObject(options)) {
            JSValue stream_val = JS_GetPropertyStr(ctx, options, "stream");
            JS_FreeValue(ctx, stream_val);
        }
    }

    /* Simple UTF-8 decode */
    JSValue result = JS_NewStringLen(ctx, (const char *)buf, buf_len);

    return result;
}

static JSValue lr_text_decoder_encoding(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewString(ctx, "utf-8");
}

static JSValue lr_text_decoder_fatal(JSContext *ctx, JSValueConst this_val)
{
    return JS_FALSE;
}

static JSValue lr_text_decoder_ignoreBOM(JSContext *ctx, JSValueConst this_val)
{
    return JS_FALSE;
}

static const JSCFunctionListEntry lr_text_decoder_funcs[] = {
    JS_CFUNC_DEF("decode",   1, lr_text_decoder_decode),
    JS_CGETSET_DEF("encoding", lr_text_decoder_encoding, NULL),
    JS_CGETSET_DEF("fatal", lr_text_decoder_fatal, NULL),
    JS_CGETSET_DEF("ignoreBOM", lr_text_decoder_ignoreBOM, NULL),
};

/* ── atob / btoa ──────────────────────────────────────────────────────── */

static JSValue lr_btoa(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "btoa requires 1 argument");
    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;

    static const char base64_table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t len = strlen(input);
    size_t out_len = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(out_len);
    if (!out) {
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t triple = 0;
        int remaining = (int)(len - i);
        
        triple |= ((unsigned char)input[i]) << 16;
        triple |= (remaining > 1) ? ((unsigned char)input[i + 1]) << 8 : 0;
        triple |= (remaining > 2) ? ((unsigned char)input[i + 2]) : 0;

        out[j++] = base64_table[(triple >> 18) & 0x3F];
        out[j++] = base64_table[(triple >> 12) & 0x3F];
        out[j++] = (remaining > 1) ? base64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (remaining > 2) ? base64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';

    JSValue result = JS_NewString(ctx, out);
    free(out);
    JS_FreeCString(ctx, input);
    return result;
}

static JSValue lr_atob(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "atob requires 1 argument");
    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;

    static const int decode_table[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
    };

    size_t len = strlen(input);
    size_t out_len = (len / 4) * 3 + 1;
    char *out = malloc(out_len);
    if (!out) {
        JS_FreeCString(ctx, input);
        return JS_ThrowOutOfMemory(ctx);
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = decode_table[(unsigned char)input[i]];
        int b = decode_table[(unsigned char)input[i+1]];
        int c = (input[i+2] == '=') ? 0 : decode_table[(unsigned char)input[i+2]];
        int d = (input[i+3] == '=') ? 0 : decode_table[(unsigned char)input[i+3]];

        out[j++] = (char)((a << 2) | (b >> 4));
        if (input[i+2] != '=') out[j++] = (char)((b << 4) | (c >> 2));
        if (input[i+3] != '=') out[j++] = (char)((c << 6) | d);
    }
    out[j] = '\0';

    JSValue result = JS_NewStringLen(ctx, out, j);
    free(out);
    JS_FreeCString(ctx, input);
    return result;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void lr_encoding_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* TextEncoder */
    JSValue encoder_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, encoder_proto, lr_text_encoder_funcs,
                                sizeof(lr_text_encoder_funcs) / sizeof(lr_text_encoder_funcs[0]));
    JSValue encoder_ctor = JS_NewCFunction2(ctx, lr_text_encoder_constructor, "TextEncoder", 0,
                                             JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, encoder_ctor, "prototype", JS_DupValue(ctx, encoder_proto));
    JS_FreeValue(ctx, encoder_proto);
    JS_SetPropertyStr(ctx, global, "TextEncoder", encoder_ctor);

    /* TextDecoder */
    JSValue decoder_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, decoder_proto, lr_text_decoder_funcs,
                                sizeof(lr_text_decoder_funcs) / sizeof(lr_text_decoder_funcs[0]));
    JSValue decoder_ctor = JS_NewCFunction2(ctx, lr_text_decoder_constructor, "TextDecoder", 0,
                                             JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, decoder_ctor, "prototype", JS_DupValue(ctx, decoder_proto));
    JS_FreeValue(ctx, decoder_proto);
    JS_SetPropertyStr(ctx, global, "TextDecoder", decoder_ctor);

    /* btoa / atob */
    JS_SetPropertyStr(ctx, global, "btoa",
                      JS_NewCFunction(ctx, lr_btoa, "btoa", 1));
    JS_SetPropertyStr(ctx, global, "atob",
                      JS_NewCFunction(ctx, lr_atob, "atob", 1));

    JS_FreeValue(ctx, global);
    lr_log(rt, LR_LOG_DEBUG, "Encoding API initialized");
}