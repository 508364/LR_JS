/*
 * L/R_JS - Renderer Bridge Module
 * Pure C, IPC-based rendering delegation to external renderers,
 * plus a plugin-based custom renderer interface (EGL, Vulkan, etc.).
 * Uses Unix domain sockets for high-performance local IPC.
 * Protocol: length-prefixed JSON messages.
 */
#ifndef LR_RENDERER_H
#define LR_RENDERER_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct LR_RendererBridge LR_RendererBridge;
typedef struct LR_RendererConfig LR_RendererConfig;
typedef struct LR_CustomRenderer LR_CustomRenderer;

/* ── Renderer types ────────────────────────────────────────────────────── */

typedef enum {
    LR_RENDERER_NONE       = 0,
    LR_RENDERER_SKIA       = 1,   /* Skia graphics library */
    LR_RENDERER_HEADLESS   = 2,   /* Headless browser (Chromium/WebKit) */
    LR_RENDERER_WEBGPU     = 3,   /* WebGPU/Dawn renderer */
    LR_RENDERER_CUSTOM     = 4,   /* Custom external renderer */
    LR_RENDERER_INTERNAL   = 5,   /* Internal framebuffer renderer */
    LR_RENDERER_EGL        = 6,   /* EGL/GLES2 GPU-accelerated renderer */
    LR_RENDERER_PLUGIN     = 7,   /* User-provided LR_CustomRenderer plugin */
} LR_RendererType;

/* ── Render command ────────────────────────────────────────────────────── */

typedef enum {
    LR_RCMD_PAINT           = 0,   /* Full paint request */
    LR_RCMD_SET_VIEWPORT    = 1,   /* Set viewport size */
    LR_RCMD_DRAW_RECT       = 2,   /* Draw rectangle */
    LR_RCMD_DRAW_TEXT       = 3,   /* Draw text */
    LR_RCMD_DRAW_IMAGE      = 4,   /* Draw image */
    LR_RCMD_CLEAR           = 5,   /* Clear canvas */
    LR_RCMD_FLUSH           = 6,   /* Flush render buffer */
    LR_RCMD_SNAPSHOT        = 7,   /* Take snapshot */
    LR_RCMD_SET_STYLE       = 8,   /* Set paint style */
    LR_RCMD_COMPOSITE       = 9,   /* Composite layers */
    LR_RCMD_CUSTOM          = 99,  /* Custom command */
} LR_RenderCommand;

/* ── Renderer config ───────────────────────────────────────────────────── */

struct LR_RendererConfig {
    LR_RendererType  type;
    char            *socket_path;     /* Unix socket path for IPC */
    char            *exec_path;       /* External renderer executable */
    int              width;           /* Default viewport width */
    int              height;          /* Default viewport height */
    int              fps;             /* Target FPS */
    int              double_buffer;   /* Use double buffering */
    int              use_shm;         /* Use shared memory for buffers */
    int              shm_size;        /* Shared memory buffer size */
};

/* ── Custom renderer plugin (vtable) ───────────────────────────────────── */

/*
 * LR_CustomRenderer is a vtable-based plugin interface.
 * Implement this struct and pass it to lr_renderer_set_custom_renderer()
 * to replace the default rendering pipeline with your own.
 *
 * All callbacks receive `user_data` as their first argument.
 * Return 0 on success, non-zero on error.
 */
struct LR_CustomRenderer {
    void *user_data;

    /* Lifecycle */
    int  (*init)         (void *user_data, int width, int height);
    void (*destroy)      (void *user_data);

    /* Frame lifecycle */
    int  (*begin_frame)  (void *user_data);
    int  (*end_frame)    (void *user_data);
    int  (*present)      (void *user_data);      /* swap/present to display */

    /* Viewport / state */
    int  (*resize)       (void *user_data, int width, int height);
    int  (*set_viewport) (void *user_data, int x, int y, int w, int h);

    /* Clear */
    int  (*clear)        (void *user_data, float r, float g, float b, float a);

    /* 2D drawing primitives */
    int  (*draw_rect)    (void *user_data, float x, float y, float w, float h,
                          float r, float g, float b, float a);
    int  (*draw_text)    (void *user_data, const char *text,
                          float x, float y, float size,
                          float r, float g, float b, float a);
    int  (*draw_image)   (void *user_data, const uint8_t *pixels,
                          int pw, int ph, float x, float y, float w, float h);

    /* Path / shape */
    int  (*begin_path)   (void *user_data);
    int  (*move_to)      (void *user_data, float x, float y);
    int  (*line_to)      (void *user_data, float x, float y);
    int  (*arc)          (void *user_data, float cx, float cy, float r,
                          float start_angle, float end_angle, int counter_clockwise);
    int  (*fill)         (void *user_data);
    int  (*stroke)       (void *user_data);

    /* Transform */
    int  (*save)         (void *user_data);
    int  (*restore)      (void *user_data);
    int  (*translate)    (void *user_data, float tx, float ty);
    int  (*scale)        (void *user_data, float sx, float sy);
    int  (*rotate)       (void *user_data, float angle_rad);

    /* Pixel readback (for offscreen rendering) */
    int  (*read_pixels)  (void *user_data, uint32_t *out_buf,
                          int x, int y, int w, int h);

    /* WebGL / GLES direct access */
    void *(*get_native_gl) (void *user_data);  /* returns GLES context handle */
};

/* ── Renderer bridge ───────────────────────────────────────────────────── */

struct LR_RendererBridge {
    LR_RendererConfig  config;
    int                socket_fd;     /* Unix domain socket fd */
    int                connected;
    int                running;

    /* Shared memory for pixel buffers */
    int                shm_fd;
    uint8_t           *shm_buf;
    size_t             shm_size;

    /* Frame buffer (internal renderer) */
    uint32_t          *framebuffer;
    int                fb_width;
    int                fb_height;

    /* Custom renderer plugin (EGL, Vulkan, or user-provided) */
    LR_CustomRenderer  custom_renderer;
    int                custom_renderer_active;

    /* Stats */
    int64_t            frames_rendered;
    int64_t            total_render_time_us;
};

/* ── API ───────────────────────────────────────────────────────────────── */

/* Initialize renderer bridge with config. */
LR_RendererBridge *lr_renderer_create(const LR_RendererConfig *config);

/* Connect to external renderer. */
int lr_renderer_connect(LR_RendererBridge *rb);

/* Disconnect from renderer. */
void lr_renderer_disconnect(LR_RendererBridge *rb);

/* Send a render command. */
int lr_renderer_send_command(LR_RendererBridge *rb, LR_RenderCommand cmd,
                             const char *json_params);

/* Send raw data to renderer. */
int lr_renderer_send_raw(LR_RendererBridge *rb, const uint8_t *data,
                         size_t len);

/* Receive response from renderer (non-blocking). */
int lr_renderer_recv_response(LR_RendererBridge *rb, char *buf, size_t buf_size);

/* Render a frame using internal framebuffer (no external renderer needed). */
int lr_renderer_paint_internal(LR_RendererBridge *rb,
                               const uint32_t *pixels,
                               int width, int height);

/* Get framebuffer pointer for direct pixel access. */
uint32_t *lr_renderer_get_framebuffer(LR_RendererBridge *rb,
                                      int *width, int *height);

/* Output framebuffer as PPM to file. */
int lr_renderer_save_ppm(LR_RendererBridge *rb, const char *filename);

/* Destroy renderer bridge. */
void lr_renderer_destroy(LR_RendererBridge *rb);

/* Default config. */
void lr_renderer_config_default(LR_RendererConfig *cfg);

/* Get renderer type string. */
const char *lr_renderer_type_str(LR_RendererType type);

/* ── Custom renderer plugin API ────────────────────────────────────────── */

/*
 * Set a custom renderer plugin on the bridge.
 * This replaces the default rendering pipeline with the provided vtable.
 * The custom renderer takes ownership of the rendering process;
 * IPC and internal framebuffer are bypassed when a custom renderer is active.
 *
 * Pass NULL to deactivate the custom renderer and return to default.
 */
int lr_renderer_set_custom_renderer(LR_RendererBridge *rb,
                                    const LR_CustomRenderer *renderer);

/*
 * Get the currently active custom renderer, or NULL if none.
 */
const LR_CustomRenderer *lr_renderer_get_custom_renderer(LR_RendererBridge *rb);

/*
 * Initialize an EGL-based custom renderer and attach it to the bridge.
 * This is a convenience wrapper that creates an LR_EGLRenderer internally.
 *
 *   offscreen: 1 = headless pbuffer, 0 = on-screen window
 *   native_display: platform display handle (NULL = default)
 *   native_window:  platform window handle (NULL = offscreen only)
 *
 * Returns 0 on success, -1 on error.
 */
int lr_renderer_init_egl(LR_RendererBridge *rb,
                         int width, int height, int offscreen,
                         void *native_display, void *native_window);

/*
 * Delegate a draw call to the custom renderer (if active).
 * Falls back to internal framebuffer if no custom renderer is set.
 */

/* Begin a new frame using the custom renderer. */
int lr_renderer_begin_frame(LR_RendererBridge *rb);

/* End the current frame. */
int lr_renderer_end_frame(LR_RendererBridge *rb);

/* Present the rendered frame (swap buffers for on-screen, no-op for offscreen). */
int lr_renderer_present(LR_RendererBridge *rb);

/* Clear the canvas. */
int lr_renderer_clear(LR_RendererBridge *rb, float r, float g, float b, float a);

/* Draw a filled rectangle. */
int lr_renderer_draw_rect(LR_RendererBridge *rb,
                          float x, float y, float w, float h,
                          float r, float g, float b, float a);

/* Read back pixels from the renderer. */
int lr_renderer_read_pixels(LR_RendererBridge *rb,
                            uint32_t *out_buf, int x, int y, int w, int h);

/* Get native GL context handle (for WebGL interop). Returns NULL if not available. */
void *lr_renderer_get_native_gl(LR_RendererBridge *rb);

#endif /* LR_RENDERER_H */