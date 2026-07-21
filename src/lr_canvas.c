/*
 * L/R_JS - Canvas API (canvas.getContext('2d'), canvas.getContext('webgl'))
 * Pure C, ES2022-compatible
 *
 * The Canvas API delegates rendering to the LR_RendererBridge,
 * which can be backed by EGL/GLES, internal framebuffer, or
 * a user-provided custom renderer plugin.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_runtime.h"
#include "lr_renderer.h"
#include "lr_renderer_egl.h"

/* ── Forward declarations ──────────────────────────────────────────────── */

typedef struct LR_Canvas2D LR_Canvas2D;

/* WebGL context creation - implemented in lr_canvas_webgl.c */
JSValue lr_webgl_create_context(JSContext *js_ctx, LR_RendererBridge *rb,
                                int width, int height, int is_webgl2);

struct LR_Canvas2D {
    LR_RendererBridge *rb;
    int                width;
    int                height;
    float              fill_r, fill_g, fill_b, fill_a;
    float              stroke_r, stroke_g, stroke_b, stroke_a;
    float              line_width;
    float              global_alpha;
};

/* ── Helper: get renderer bridge from runtime ──────────────────────────── */

static LR_RendererBridge *canvas_get_rb(LR_Runtime *rt)
{
    /* The renderer bridge is stored as opaque data on the context.
     * For now, we create/reuse a global one. In production, this
     * would be managed by the runtime lifecycle. */
    extern LR_RendererBridge *g_lr_renderer_bridge;  /* See lr_runtime.c */
    return g_lr_renderer_bridge;
}

/* ── Canvas 2D context ─────────────────────────────────────────────────── */

static LR_Canvas2D *canvas2d_create(LR_RendererBridge *rb, int w, int h)
{
    LR_Canvas2D *ctx = (LR_Canvas2D *)calloc(1, sizeof(LR_Canvas2D));
    if (!ctx) return NULL;
    ctx->rb = rb;
    ctx->width = w > 0 ? w : 800;
    ctx->height = h > 0 ? h : 600;
    ctx->fill_r = 0.0f; ctx->fill_g = 0.0f; ctx->fill_b = 0.0f; ctx->fill_a = 1.0f;
    ctx->stroke_r = 0.0f; ctx->stroke_g = 0.0f; ctx->stroke_b = 0.0f; ctx->stroke_a = 1.0f;
    ctx->line_width = 1.0f;
    ctx->global_alpha = 1.0f;
    return ctx;
}

static void canvas2d_destroy(LR_Canvas2D *ctx)
{
    (void)ctx;
    /* future cleanup */
}

/* ── JS: CanvasRenderingContext2D.clearRect(x, y, w, h) ────────────────── */

static JSValue lr_canvas_clear_rect(JSContext *js_ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx || !ctx->rb) return JS_UNDEFINED;

    double x = 0, y = 0, w = 0, h = 0;
    if (argc >= 4) {
        JS_ToFloat64(js_ctx, &x, argv[0]);
        JS_ToFloat64(js_ctx, &y, argv[1]);
        JS_ToFloat64(js_ctx, &w, argv[2]);
        JS_ToFloat64(js_ctx, &h, argv[3]);
    }
    lr_renderer_clear(ctx->rb, 0.0f, 0.0f, 0.0f, 0.0f);
    return JS_UNDEFINED;
}

/* ── JS: CanvasRenderingContext2D.fillRect(x, y, w, h) ─────────────────── */

static JSValue lr_canvas_fill_rect(JSContext *js_ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx || !ctx->rb) return JS_UNDEFINED;

    double x = 0, y = 0, w = 0, h = 0;
    if (argc >= 4) {
        JS_ToFloat64(js_ctx, &x, argv[0]);
        JS_ToFloat64(js_ctx, &y, argv[1]);
        JS_ToFloat64(js_ctx, &w, argv[2]);
        JS_ToFloat64(js_ctx, &h, argv[3]);
    }

    float a = ctx->fill_a * ctx->global_alpha;
    lr_renderer_draw_rect(ctx->rb, (float)x, (float)y, (float)w, (float)h,
                          ctx->fill_r, ctx->fill_g, ctx->fill_b, a);
    return JS_UNDEFINED;
}

/* ── JS: CanvasRenderingContext2D.fillStyle (get/set) ──────────────────── */

static JSValue lr_canvas_get_fill_style(JSContext *js_ctx, JSValueConst this_val)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;

    char buf[32];
    snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)",
             (int)(ctx->fill_r * 255), (int)(ctx->fill_g * 255),
             (int)(ctx->fill_b * 255), ctx->fill_a);
    return JS_NewString(js_ctx, buf);
}

static JSValue lr_canvas_set_fill_style(JSContext *js_ctx, JSValueConst this_val,
                                         JSValueConst val)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;

    const char *str = JS_ToCString(js_ctx, val);
    if (str) {
        /* Parse basic CSS color: named, #hex, rgb(), rgba() */
        int r = 0, g = 0, b = 0;
        float a = 1.0f;
        if (str[0] == '#' && strlen(str) == 7) {
            sscanf(str + 1, "%02x%02x%02x", &r, &g, &b);
        } else if (strncmp(str, "rgb(", 4) == 0) {
            sscanf(str, "rgb(%d,%d,%d)", &r, &g, &b);
        } else if (strncmp(str, "rgba(", 5) == 0) {
            sscanf(str, "rgba(%d,%d,%d,%f)", &r, &g, &b, &a);
        } else if (strcmp(str, "red") == 0) {
            r = 255; g = 0; b = 0;
        } else if (strcmp(str, "green") == 0) {
            r = 0; g = 128; b = 0;
        } else if (strcmp(str, "blue") == 0) {
            r = 0; g = 0; b = 255;
        } else if (strcmp(str, "white") == 0) {
            r = 255; g = 255; b = 255;
        } else if (strcmp(str, "black") == 0) {
            r = 0; g = 0; b = 0;
        }
        ctx->fill_r = (float)r / 255.0f;
        ctx->fill_g = (float)g / 255.0f;
        ctx->fill_b = (float)b / 255.0f;
        ctx->fill_a = a;
        JS_FreeCString(js_ctx, str);
    }
    return JS_UNDEFINED;
}

/* ── JS: CanvasRenderingContext2D.globalAlpha (get/set) ────────────────── */

static JSValue lr_canvas_get_global_alpha(JSContext *js_ctx, JSValueConst this_val)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    return JS_NewFloat64(js_ctx, ctx->global_alpha);
}

static JSValue lr_canvas_set_global_alpha(JSContext *js_ctx, JSValueConst this_val,
                                           JSValueConst val)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    double alpha = 1.0;
    JS_ToFloat64(js_ctx, &alpha, val);
    ctx->global_alpha = (float)(alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha));
    return JS_UNDEFINED;
}

/* ── JS: CanvasRenderingContext2D.getImageData(x, y, w, h) ─────────────── */

static JSValue lr_canvas_get_image_data(JSContext *js_ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    LR_Canvas2D *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx || !ctx->rb) return JS_UNDEFINED;

    int sx = 0, sy = 0, sw = 1, sh = 1;
    if (argc >= 4) {
        JS_ToInt32(js_ctx, &sx, argv[0]);
        JS_ToInt32(js_ctx, &sy, argv[1]);
        JS_ToInt32(js_ctx, &sw, argv[2]);
        JS_ToInt32(js_ctx, &sh, argv[3]);
    }

    /* Read pixels from renderer */
    size_t buf_size = (size_t)sw * (size_t)sh * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)malloc(buf_size);
    if (!pixels) return JS_UNDEFINED;

    if (lr_renderer_read_pixels(ctx->rb, pixels, sx, sy, sw, sh) != 0) {
        free(pixels);
        return JS_UNDEFINED;
    }

    /* Build ImageData object: { width, height, data: Uint8ClampedArray } */
    JSValue obj = JS_NewObject(js_ctx);
    JS_SetPropertyStr(js_ctx, obj, "width", JS_NewInt32(js_ctx, sw));
    JS_SetPropertyStr(js_ctx, obj, "height", JS_NewInt32(js_ctx, sh));

    /* Convert RGBA uint32 to Uint8ClampedArray */
    size_t byte_len = (size_t)sw * (size_t)sh * 4;
    uint8_t *bytes = (uint8_t *)malloc(byte_len);
    if (bytes) {
        for (int i = 0; i < sw * sh; i++) {
            uint32_t p = pixels[i];
            bytes[i * 4 + 0] = (uint8_t)(p & 0xFF);          /* R */
            bytes[i * 4 + 1] = (uint8_t)((p >> 8) & 0xFF);   /* G */
            bytes[i * 4 + 2] = (uint8_t)((p >> 16) & 0xFF);  /* B */
            bytes[i * 4 + 3] = (uint8_t)((p >> 24) & 0xFF);  /* A */
        }
        JSValue data = JS_NewArrayBufferCopy(js_ctx, bytes, byte_len);
        JS_SetPropertyStr(js_ctx, obj, "data", data);
        free(bytes);
    }

    free(pixels);
    return obj;
}

/* ── JS: Canvas constructor ────────────────────────────────────────────── */

static JSValue lr_canvas_constructor(JSContext *js_ctx,
                                      JSValueConst new_target,
                                      int argc, JSValueConst *argv)
{
    LR_Runtime *rt = JS_GetContextOpaque(js_ctx);
    LR_RendererBridge *rb = canvas_get_rb(rt);

    int w = 300, h = 150;
    if (argc >= 2) {
        JS_ToInt32(js_ctx, &w, argv[0]);
        JS_ToInt32(js_ctx, &h, argv[1]);
    }

    JSValue obj = JS_NewObject(js_ctx);

    JS_SetPropertyStr(js_ctx, obj, "width", JS_NewInt32(js_ctx, w));
    JS_SetPropertyStr(js_ctx, obj, "height", JS_NewInt32(js_ctx, h));

    /* Store the renderer bridge reference */
    JS_SetOpaque(obj, rb);

    return obj;
}

/* ── JS: canvas.getContext('2d') ───────────────────────────────────────── */

static JSValue lr_canvas_get_context(JSContext *js_ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    LR_RendererBridge *rb = JS_GetOpaque(this_val, 1);
    if (!rb) return JS_UNDEFINED;

    const char *type = "2d";
    if (argc >= 1) {
        type = JS_ToCString(js_ctx, argv[0]);
    }

    if (strcmp(type, "2d") == 0) {
        /* Get canvas dimensions */
        JSValue w_val = JS_GetPropertyStr(js_ctx, this_val, "width");
        JSValue h_val = JS_GetPropertyStr(js_ctx, this_val, "height");
        int w = 300, h = 150;
        JS_ToInt32(js_ctx, &w, w_val);
        JS_ToInt32(js_ctx, &h, h_val);
        JS_FreeValue(js_ctx, w_val);
        JS_FreeValue(js_ctx, h_val);

        LR_Canvas2D *ctx = canvas2d_create(rb, w, h);
        if (!ctx) {
            if (argc >= 1) JS_FreeCString(js_ctx, type);
            return JS_UNDEFINED;
        }

        /* Build 2D context object */
        JSValue ctx_obj = JS_NewObject(js_ctx);
        JS_SetOpaque(ctx_obj, ctx);

        /* Properties */
        JS_SetPropertyStr(js_ctx, ctx_obj, "canvas", JS_DupValue(js_ctx, this_val));

        /* Methods */
        JS_SetPropertyStr(js_ctx, ctx_obj, "clearRect",
                          JS_NewCFunction(js_ctx, lr_canvas_clear_rect, "clearRect", 4));
        JS_SetPropertyStr(js_ctx, ctx_obj, "fillRect",
                          JS_NewCFunction(js_ctx, lr_canvas_fill_rect, "fillRect", 4));
        JS_SetPropertyStr(js_ctx, ctx_obj, "getImageData",
                          JS_NewCFunction(js_ctx, lr_canvas_get_image_data, "getImageData", 4));

        /* fillStyle getter/setter */
        JS_DefinePropertyGetSet(js_ctx, ctx_obj, JS_NewAtom(js_ctx, "fillStyle"),
                                lr_canvas_get_fill_style, lr_canvas_set_fill_style, 0);
        JS_DefinePropertyGetSet(js_ctx, ctx_obj, JS_NewAtom(js_ctx, "globalAlpha"),
                                lr_canvas_get_global_alpha, lr_canvas_set_global_alpha, 0);

        if (argc >= 1) JS_FreeCString(js_ctx, type);
        return ctx_obj;
    }

    if (strcmp(type, "webgl") == 0 || strcmp(type, "experimental-webgl") == 0) {
        /* Get canvas dimensions */
        JSValue w_val = JS_GetPropertyStr(js_ctx, this_val, "width");
        JSValue h_val = JS_GetPropertyStr(js_ctx, this_val, "height");
        int w = 300, h = 150;
        JS_ToInt32(js_ctx, &w, w_val);
        JS_ToInt32(js_ctx, &h, h_val);
        JS_FreeValue(js_ctx, w_val);
        JS_FreeValue(js_ctx, h_val);

        if (argc >= 2) {
            /* Check WebGL context attributes */
            JSValue attrs = argv[1];
        }

        JSValue gl_obj = lr_webgl_create_context(js_ctx, rb, w, h, 0);
        if (JS_IsNull(gl_obj) || JS_IsUndefined(gl_obj)) {
            if (argc >= 1) JS_FreeCString(js_ctx, type);
            return JS_UNDEFINED;
        }

        JS_SetPropertyStr(js_ctx, gl_obj, "canvas", JS_DupValue(js_ctx, this_val));

        if (argc >= 1) JS_FreeCString(js_ctx, type);
        return gl_obj;
    }

    if (strcmp(type, "webgl2") == 0) {
        /* Get canvas dimensions */
        JSValue w_val = JS_GetPropertyStr(js_ctx, this_val, "width");
        JSValue h_val = JS_GetPropertyStr(js_ctx, this_val, "height");
        int w = 300, h = 150;
        JS_ToInt32(js_ctx, &w, w_val);
        JS_ToInt32(js_ctx, &h, h_val);
        JS_FreeValue(js_ctx, w_val);
        JS_FreeValue(js_ctx, h_val);

        JSValue gl_obj = lr_webgl_create_context(js_ctx, rb, w, h, 1);
        if (JS_IsNull(gl_obj) || JS_IsUndefined(gl_obj)) {
            if (argc >= 1) JS_FreeCString(js_ctx, type);
            return JS_UNDEFINED;
        }

        JS_SetPropertyStr(js_ctx, gl_obj, "canvas", JS_DupValue(js_ctx, this_val));

        if (argc >= 1) JS_FreeCString(js_ctx, type);
        return gl_obj;
    }

    if (argc >= 1) JS_FreeCString(js_ctx, type);
    return JS_UNDEFINED;
}

/* ── JS: canvas.toDataURL(type, quality) ───────────────────────────────── */

static JSValue lr_canvas_to_data_url(JSContext *js_ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    LR_RendererBridge *rb = JS_GetOpaque(this_val, 1);
    if (!rb) return JS_UNDEFINED;

    /* Read framebuffer pixels */
    int w = rb->fb_width, h = rb->fb_height;
    if (w <= 0 || h <= 0) return JS_UNDEFINED;

    size_t buf_size = (size_t)w * (size_t)h * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)malloc(buf_size);
    if (!pixels) return JS_UNDEFINED;

    if (lr_renderer_read_pixels(rb, pixels, 0, 0, w, h) != 0) {
        free(pixels);
        return JS_UNDEFINED;
    }

    /* Stub: return a placeholder data URL.
     * Full PNG/JPEG encoding would require a library like libpng/stb_image. */
    char url[256];
    snprintf(url, sizeof(url),
             "data:image/png;base64,LR_JS_CANVAS_%dx%d_PLACEHOLDER", w, h);

    free(pixels);
    return JS_NewString(js_ctx, url);
}

/* ── JS: canvas.toBuffer() → raw RGBA pixel buffer ─────────────────────── */

static JSValue lr_canvas_to_buffer(JSContext *js_ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_RendererBridge *rb = JS_GetOpaque(this_val, 1);
    if (!rb) return JS_UNDEFINED;

    int w = rb->fb_width, h = rb->fb_height;
    if (w <= 0 || h <= 0) return JS_UNDEFINED;

    size_t buf_size = (size_t)w * (size_t)h * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)malloc(buf_size);
    if (!pixels) return JS_UNDEFINED;

    if (lr_renderer_read_pixels(rb, pixels, 0, 0, w, h) != 0) {
        free(pixels);
        return JS_UNDEFINED;
    }

    JSValue buf = JS_NewArrayBufferCopy(js_ctx, (uint8_t *)pixels, buf_size);
    free(pixels);
    return buf;
}

/* ── Module init ───────────────────────────────────────────────────────── */

void lr_canvas_init(LR_Runtime *rt)
{
    JSContext *ctx = rt->lr_ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Create Canvas constructor */
    JSValue canvas_func = JS_NewCFunction2(ctx, lr_canvas_constructor,
                                            "Canvas", 2,
                                            JS_CFUNC_constructor, 0);

    /* Add getContext method to prototype */
    JSValue proto = JS_GetPropertyStr(ctx, canvas_func, "prototype");
    JS_SetPropertyStr(ctx, proto, "getContext",
                      JS_NewCFunction(ctx, lr_canvas_get_context, "getContext", 2));
    JS_SetPropertyStr(ctx, proto, "toDataURL",
                      JS_NewCFunction(ctx, lr_canvas_to_data_url, "toDataURL", 2));
    JS_SetPropertyStr(ctx, proto, "toBuffer",
                      JS_NewCFunction(ctx, lr_canvas_to_buffer, "toBuffer", 0));
    JS_FreeValue(ctx, proto);

    /* Expose to global */
    JS_SetPropertyStr(ctx, global, "Canvas", canvas_func);

    JS_FreeValue(ctx, global);
}