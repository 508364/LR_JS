/*
 * L/R_JS - Renderer Bridge Implementation
 * Pure C, IPC-based rendering delegation + custom renderer plugin support.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "lr_platform.h"

#if !LR_PLATFORM_WINDOWS
#include <sys/un.h>
#endif

#include "lr_renderer.h"
#include "lr_renderer_egl.h"

/* ── Helpers ───────────────────────────────────────────────────────────── */

const char *lr_renderer_type_str(LR_RendererType type)
{
    switch (type) {
    case LR_RENDERER_NONE:     return "none";
    case LR_RENDERER_SKIA:     return "skia";
    case LR_RENDERER_HEADLESS: return "headless";
    case LR_RENDERER_WEBGPU:   return "webgpu";
    case LR_RENDERER_CUSTOM:   return "custom";
    case LR_RENDERER_INTERNAL: return "internal";
    case LR_RENDERER_EGL:      return "egl";
    case LR_RENDERER_PLUGIN:   return "plugin";
    default:                   return "unknown";
    }
}

void lr_renderer_config_default(LR_RendererConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->type          = LR_RENDERER_INTERNAL;
    cfg->socket_path   = NULL;
    cfg->exec_path     = NULL;
    cfg->width         = 800;
    cfg->height        = 600;
    cfg->fps           = 60;
    cfg->double_buffer = 1;
    cfg->use_shm       = 0;
    cfg->shm_size      = 0;
}

/* ── Create / Destroy ──────────────────────────────────────────────────── */

LR_RendererBridge *lr_renderer_create(const LR_RendererConfig *config)
{
    LR_RendererBridge *rb = calloc(1, sizeof(LR_RendererBridge));
    if (!rb) return NULL;

    rb->config = *config;
    rb->socket_fd = -1;
    rb->shm_fd = -1;
    rb->connected = 0;
    rb->running = 0;
    rb->custom_renderer_active = 0;

    /* Allocate internal framebuffer */
    if (config->type == LR_RENDERER_INTERNAL) {
        rb->fb_width = config->width;
        rb->fb_height = config->height;
        size_t fb_size = (size_t)config->width * (size_t)config->height * sizeof(uint32_t);
        rb->framebuffer = calloc(1, fb_size);
        if (!rb->framebuffer) {
            free(rb);
            return NULL;
        }
    }

    return rb;
}

void lr_renderer_destroy(LR_RendererBridge *rb)
{
    if (!rb) return;

    lr_renderer_disconnect(rb);

    /* Destroy custom renderer if active */
    if (rb->custom_renderer_active && rb->custom_renderer.destroy) {
        rb->custom_renderer.destroy(rb->custom_renderer.user_data);
        rb->custom_renderer_active = 0;
    }

    if (rb->shm_buf && rb->shm_buf != LR_MMAP_FAILED) {
#if !LR_PLATFORM_WINDOWS
        munmap(rb->shm_buf, rb->shm_size);
#endif
    }
    if (rb->shm_fd >= 0) {
#if !LR_PLATFORM_WINDOWS
        close(rb->shm_fd);
#endif
    }

    free(rb->framebuffer);
    free(rb);
}

/* ── Connection ────────────────────────────────────────────────────────── */

int lr_renderer_connect(LR_RendererBridge *rb)
{
    if (!rb || rb->connected) return -1;

    if (rb->config.type == LR_RENDERER_INTERNAL) {
        /* Internal renderer doesn't need connection */
        rb->connected = 1;
        rb->running = 1;
        return 0;
    }

    if (!rb->config.socket_path) return -1;

#if !LR_PLATFORM_WINDOWS
    /* Create Unix domain socket */
    rb->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (rb->socket_fd < 0) return -1;

    /* Set non-blocking */
    lr_socket_set_nonblock(rb->socket_fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, rb->config.socket_path, sizeof(addr.sun_path) - 1);

    if (connect(rb->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (!lr_socket_einprog()) {
            lr_socket_close(rb->socket_fd);
            rb->socket_fd = -1;
            return -1;
        }
    }

    rb->connected = 1;
    rb->running = 1;

    /* Setup shared memory if configured */
    if (rb->config.use_shm && rb->config.shm_size > 0) {
        rb->shm_fd = shm_open("/lr_js_render_shm", O_CREAT | O_RDWR, 0600);
        if (rb->shm_fd >= 0) {
            if (ftruncate(rb->shm_fd, (off_t)rb->config.shm_size) != 0) {
                /* ignore truncate error, mmap will fail if needed */
            }
            rb->shm_buf = mmap(NULL, rb->config.shm_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, rb->shm_fd, 0);
            rb->shm_size = rb->config.shm_size;
        }
    }
#else
    /* Windows: external renderer via TCP or internal fallback */
    /* For now, fall back to internal renderer */
    rb->connected = 1;
    rb->running = 1;
#endif

    return 0;
}

void lr_renderer_disconnect(LR_RendererBridge *rb)
{
    if (!rb) return;

    rb->running = 0;
    rb->connected = 0;

    if (rb->socket_fd >= 0) {
        lr_socket_close(rb->socket_fd);
        rb->socket_fd = -1;
    }
}

/* ── Send / Receive ────────────────────────────────────────────────────── */

int lr_renderer_send_command(LR_RendererBridge *rb, LR_RenderCommand cmd,
                             const char *json_params)
{
    if (!rb || !rb->connected) return -1;

    /* Build length-prefixed message */
    char header[64];
    int header_len = 0;
    if (json_params) {
        header_len = snprintf(header, sizeof(header), "%d %zu\n", (int)cmd, strlen(json_params));
    } else {
        header_len = snprintf(header, sizeof(header), "%d 0\n", (int)cmd);
    }

    if (rb->config.type == LR_RENDERER_INTERNAL) {
        /* Internal renderer processes commands directly */
        return 0;
    }

    /* Send header */
    if (send(rb->socket_fd, header, (size_t)header_len, MSG_NOSIGNAL) < 0) {
        return -1;
    }

    /* Send JSON body */
    if (json_params && strlen(json_params) > 0) {
        if (send(rb->socket_fd, json_params, strlen(json_params), MSG_NOSIGNAL) < 0) {
            return -1;
        }
    }

    return 0;
}

int lr_renderer_send_raw(LR_RendererBridge *rb, const uint8_t *data,
                         size_t len)
{
    if (!rb || !rb->connected || !data) return -1;

    if (rb->config.type == LR_RENDERER_INTERNAL) {
        return 0;
    }

    if (send(rb->socket_fd, data, len, MSG_NOSIGNAL) < 0) {
        return -1;
    }

    return 0;
}

int lr_renderer_recv_response(LR_RendererBridge *rb, char *buf, size_t buf_size)
{
    if (!rb || !rb->connected || !buf) return -1;

    if (rb->config.type == LR_RENDERER_INTERNAL) {
        return 0;
    }

    ssize_t n = recv(rb->socket_fd, buf, buf_size - 1, MSG_DONTWAIT);
    if (n < 0) {
        if (lr_socket_eagain()) return 0;
        return -1;
    }
    if (n == 0) {
        rb->connected = 0;
        return -1;
    }

    buf[n] = '\0';
    return (int)n;
}

/* ── Internal framebuffer ──────────────────────────────────────────────── */

int lr_renderer_paint_internal(LR_RendererBridge *rb,
                               const uint32_t *pixels,
                               int width, int height)
{
    if (!rb || !pixels) return -1;

    if (rb->config.type != LR_RENDERER_INTERNAL) {
        /* Forward to external renderer */
        return lr_renderer_send_command(rb, LR_RCMD_PAINT, NULL);
    }

    /* Resize framebuffer if needed */
    if (width != rb->fb_width || height != rb->fb_height) {
        size_t new_size = (size_t)width * (size_t)height * sizeof(uint32_t);
        uint32_t *new_fb = realloc(rb->framebuffer, new_size);
        if (!new_fb) return -1;
        rb->framebuffer = new_fb;
        rb->fb_width = width;
        rb->fb_height = height;
    }

    /* Copy pixels */
    size_t copy_size = (size_t)width * (size_t)height * sizeof(uint32_t);
    memcpy(rb->framebuffer, pixels, copy_size);

    rb->frames_rendered++;
    return 0;
}

uint32_t *lr_renderer_get_framebuffer(LR_RendererBridge *rb,
                                      int *width, int *height)
{
    if (!rb || !rb->framebuffer) return NULL;
    if (width) *width = rb->fb_width;
    if (height) *height = rb->fb_height;
    return rb->framebuffer;
}

int lr_renderer_save_ppm(LR_RendererBridge *rb, const char *filename)
{
    if (!rb || !rb->framebuffer || !filename) return -1;

    FILE *f = fopen(filename, "wb");
    if (!f) return -1;

    fprintf(f, "P6\n%d %d\n255\n", rb->fb_width, rb->fb_height);

    for (int i = 0; i < rb->fb_height; i++) {
        for (int j = 0; j < rb->fb_width; j++) {
            uint32_t pixel = rb->framebuffer[i * rb->fb_width + j];
            uint8_t rgb[3];
            rgb[0] = (pixel >> 16) & 0xFF;  /* R */
            rgb[1] = (pixel >> 8) & 0xFF;   /* G */
            rgb[2] = pixel & 0xFF;          /* B */
            fwrite(rgb, 1, 3, f);
        }
    }

    fclose(f);
    return 0;
}

/* ── Custom Renderer Plugin API ────────────────────────────────────────── */

int lr_renderer_set_custom_renderer(LR_RendererBridge *rb,
                                    const LR_CustomRenderer *renderer)
{
    if (!rb) return -1;

    /* Destroy existing custom renderer if active */
    if (rb->custom_renderer_active && rb->custom_renderer.destroy) {
        rb->custom_renderer.destroy(rb->custom_renderer.user_data);
    }
    rb->custom_renderer_active = 0;
    memset(&rb->custom_renderer, 0, sizeof(rb->custom_renderer));

    if (renderer) {
        rb->custom_renderer = *renderer;
        rb->custom_renderer_active = 1;

        /* Initialize the custom renderer */
        if (rb->custom_renderer.init) {
            int w = rb->fb_width > 0 ? rb->fb_width : rb->config.width;
            int h = rb->fb_height > 0 ? rb->fb_height : rb->config.height;
            if (rb->custom_renderer.init(rb->custom_renderer.user_data, w, h) != 0) {
                rb->custom_renderer_active = 0;
                memset(&rb->custom_renderer, 0, sizeof(rb->custom_renderer));
                return -1;
            }
        }
    }

    return 0;
}

const LR_CustomRenderer *lr_renderer_get_custom_renderer(LR_RendererBridge *rb)
{
    if (!rb || !rb->custom_renderer_active) return NULL;
    return &rb->custom_renderer;
}

int lr_renderer_init_egl(LR_RendererBridge *rb,
                         int width, int height, int offscreen,
                         void *native_display, void *native_window)
{
    if (!rb) return -1;

    if (!lr_egl_is_available()) {
        fprintf(stderr, "[LR_JS] EGL is not available (compile with -DLR_HAS_EGL and link -lEGL -lGLESv2)\n");
        return -1;
    }

    LR_EGLRenderer *egl = lr_egl_renderer_create(width, height, offscreen,
                                                  native_display, native_window);
    if (!egl) {
        fprintf(stderr, "[LR_JS] Failed to create EGL renderer\n");
        return -1;
    }

    const LR_CustomRenderer *iface = lr_egl_renderer_get_interface(egl);
    if (!iface) {
        lr_egl_renderer_destroy(egl);
        return -1;
    }

    if (lr_renderer_set_custom_renderer(rb, iface) != 0) {
        lr_egl_renderer_destroy(egl);
        return -1;
    }

    rb->fb_width = width;
    rb->fb_height = height;

    return 0;
}

/* ── Delegate drawing calls to custom renderer ─────────────────────────── */

int lr_renderer_begin_frame(LR_RendererBridge *rb)
{
    if (!rb) return -1;
    if (rb->custom_renderer_active && rb->custom_renderer.begin_frame) {
        return rb->custom_renderer.begin_frame(rb->custom_renderer.user_data);
    }
    return 0;
}

int lr_renderer_end_frame(LR_RendererBridge *rb)
{
    if (!rb) return -1;
    if (rb->custom_renderer_active && rb->custom_renderer.end_frame) {
        return rb->custom_renderer.end_frame(rb->custom_renderer.user_data);
    }
    return 0;
}

int lr_renderer_present(LR_RendererBridge *rb)
{
    if (!rb) return -1;
    if (rb->custom_renderer_active && rb->custom_renderer.present) {
        return rb->custom_renderer.present(rb->custom_renderer.user_data);
    }
    return 0;
}

int lr_renderer_clear(LR_RendererBridge *rb, float r, float g, float b, float a)
{
    if (!rb) return -1;
    if (rb->custom_renderer_active && rb->custom_renderer.clear) {
        return rb->custom_renderer.clear(rb->custom_renderer.user_data, r, g, b, a);
    }
    /* Fallback: clear internal framebuffer */
    if (rb->framebuffer) {
        uint32_t color = ((uint32_t)(r * 255.0f) << 16)
                       | ((uint32_t)(g * 255.0f) << 8)
                       | ((uint32_t)(b * 255.0f))
                       | ((uint32_t)(a * 255.0f) << 24);
        size_t count = (size_t)rb->fb_width * (size_t)rb->fb_height;
        for (size_t i = 0; i < count; i++) {
            rb->framebuffer[i] = color;
        }
    }
    return 0;
}

int lr_renderer_draw_rect(LR_RendererBridge *rb,
                          float x, float y, float w, float h,
                          float r, float g, float b, float a)
{
    if (!rb) return -1;
    if (rb->custom_renderer_active && rb->custom_renderer.draw_rect) {
        return rb->custom_renderer.draw_rect(rb->custom_renderer.user_data,
                                              x, y, w, h, r, g, b, a);
    }
    /* Fallback: draw to internal framebuffer */
    if (rb->framebuffer) {
        uint32_t color = ((uint32_t)(r * 255.0f) << 16)
                       | ((uint32_t)(g * 255.0f) << 8)
                       | ((uint32_t)(b * 255.0f))
                       | ((uint32_t)(a * 255.0f) << 24);
        int ix = (int)x, iy = (int)y;
        int iw = (int)w, ih = (int)h;
        if (ix < 0) { iw += ix; ix = 0; }
        if (iy < 0) { ih += iy; iy = 0; }
        if (ix + iw > rb->fb_width)  iw = rb->fb_width - ix;
        if (iy + ih > rb->fb_height) ih = rb->fb_height - iy;
        if (iw <= 0 || ih <= 0) return 0;
        for (int row = iy; row < iy + ih; row++) {
            for (int col = ix; col < ix + iw; col++) {
                rb->framebuffer[row * rb->fb_width + col] = color;
            }
        }
    }
    return 0;
}

int lr_renderer_read_pixels(LR_RendererBridge *rb,
                            uint32_t *out_buf, int x, int y, int w, int h)
{
    if (!rb || !out_buf) return -1;
    if (rb->custom_renderer_active && rb->custom_renderer.read_pixels) {
        return rb->custom_renderer.read_pixels(rb->custom_renderer.user_data,
                                                out_buf, x, y, w, h);
    }
    /* Fallback: read from internal framebuffer */
    if (rb->framebuffer) {
        int fw = rb->fb_width, fh = rb->fb_height;
        if (x < 0 || y < 0 || x + w > fw || y + h > fh) return -1;
        for (int row = 0; row < h; row++) {
            memcpy(out_buf + row * w,
                   rb->framebuffer + (y + row) * fw + x,
                   (size_t)w * sizeof(uint32_t));
        }
        return 0;
    }
    return -1;
}

void *lr_renderer_get_native_gl(LR_RendererBridge *rb)
{
    if (!rb || !rb->custom_renderer_active) return NULL;
    if (rb->custom_renderer.get_native_gl) {
        return rb->custom_renderer.get_native_gl(rb->custom_renderer.user_data);
    }
    return NULL;
}