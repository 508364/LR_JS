/*
 * L/R_JS - EGL Renderer Backend
 * Pure C, OpenGL ES 2.0+ via EGL for GPU-accelerated rendering.
 *
 * Supports:
 *   - Offscreen rendering (pbuffer / framebuffer)
 *   - On-screen rendering (native window via EGL surface)
 *   - Pixel readback for compositing
 *   - Pipeline integration (submit frames to external renderers)
 *
 * EGL is optional: compile with -DLR_HAS_EGL to enable.
 * When disabled, all EGL functions return error codes.
 */
#ifndef LR_RENDERER_EGL_H
#define LR_RENDERER_EGL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── EGL detection ─────────────────────────────────────────────────────── */

#if defined(LR_HAS_EGL)
  #include <EGL/egl.h>
  #include <GLES2/gl2.h>
  #define LR_EGL_AVAILABLE 1
#else
  #define LR_EGL_AVAILABLE 0
  /* Stub types when EGL not available */
  typedef void *EGLDisplay;
  typedef void *EGLSurface;
  typedef void *EGLContext;
  typedef void *EGLConfig;
  typedef int    EGLint;
  typedef int    EGLBoolean;
  typedef unsigned int GLuint;
  typedef unsigned int GLenum;
  #define EGL_NO_DISPLAY  ((EGLDisplay)0)
  #define EGL_NO_SURFACE  ((EGLSurface)0)
  #define EGL_NO_CONTEXT  ((EGLContext)0)
#endif

/* ── Forward declarations ──────────────────────────────────────────────── */

typedef struct LR_EGLRenderer    LR_EGLRenderer;

/* ── EGL Renderer (GPU-accelerated GLES2 rendering via EGL) ──────────────── */

struct LR_EGLRenderer {
    /* EGL state */
    EGLDisplay  display;
    EGLSurface  surface;
    EGLContext  context;
    EGLConfig   config;

    /* Dimensions */
    int         width;
    int         height;
    int         offscreen;

    /* Offscreen: pbuffer surface for rendering without a window */
    EGLSurface  pbuffer_surface;

    /* GLES Shader program for 2D drawing */
    GLuint      program_2d;
    GLuint      vbo;
    GLuint      attrib_pos;
    GLuint      uniform_color;
    GLuint      uniform_proj;

    /* Pixel readback buffer */
    uint32_t   *readback_buf;
    int         readback_w;
    int         readback_h;

    /* State stack for save/restore */
    float       transform_stack[16 * 8];  /* 8 levels of 4x4 matrices */
    int         transform_depth;
    float       current_color[4];
};

/* ── EGL Renderer API ─────────────────────────────────────────────────── */

#if LR_EGL_AVAILABLE

/*
 * Create an EGL renderer.
 *   width, height: initial surface dimensions
 *   offscreen: 1 = pbuffer (no window needed), 0 = on-screen window
 *   native_display: platform-native display handle (NULL = EGL_DEFAULT_DISPLAY)
 *   native_window:  platform-native window handle (NULL = offscreen only)
 *
 * Returns NULL on failure.
 */
LR_EGLRenderer *lr_egl_renderer_create(int width, int height, int offscreen,
                                       void *native_display, void *native_window);

/* Destroy the EGL renderer and free all resources. */
void lr_egl_renderer_destroy(LR_EGLRenderer *egl);

/* Make this renderer's context current on the calling thread. */
int  lr_egl_renderer_make_current(LR_EGLRenderer *egl);

/* Swap buffers (present rendered frame to display). */
int  lr_egl_renderer_swap_buffers(LR_EGLRenderer *egl);

/* Read pixel data from the current framebuffer into a user buffer.
 * Returns pointer to internal readback buffer (valid until next call). */
uint32_t *lr_egl_renderer_read_pixels(LR_EGLRenderer *egl, int *width, int *height);

/* Resize the rendering surface. */
int  lr_egl_renderer_resize(LR_EGLRenderer *egl, int width, int height);

/* Get the native EGLDisplay handle. */
void *lr_egl_renderer_get_display(LR_EGLRenderer *egl);
/* Get the native EGLContext handle. */
void *lr_egl_renderer_get_context(LR_EGLRenderer *egl);

/* Check if EGL is available at runtime. */
int  lr_egl_is_available(void);

#else /* !LR_EGL_AVAILABLE — stub implementations */

static inline LR_EGLRenderer *lr_egl_renderer_create(
    int width, int height, int offscreen,
    void *native_display, void *native_window)
{
    (void)width; (void)height; (void)offscreen;
    (void)native_display; (void)native_window;
    return NULL;
}

static inline void lr_egl_renderer_destroy(LR_EGLRenderer *egl) { (void)egl; }
static inline int  lr_egl_renderer_make_current(LR_EGLRenderer *egl) { (void)egl; return -1; }
static inline int  lr_egl_renderer_swap_buffers(LR_EGLRenderer *egl) { (void)egl; return -1; }
static inline uint32_t *lr_egl_renderer_read_pixels(LR_EGLRenderer *egl, int *w, int *h) { (void)egl; (void)w; (void)h; return NULL; }
static inline int  lr_egl_renderer_resize(LR_EGLRenderer *egl, int w, int h) { (void)egl; (void)w; (void)h; return -1; }
static inline void *lr_egl_renderer_get_display(LR_EGLRenderer *egl) { (void)egl; return NULL; }
static inline void *lr_egl_renderer_get_context(LR_EGLRenderer *egl) { (void)egl; return NULL; }
static inline int  lr_egl_is_available(void) { return 0; }

#endif /* LR_EGL_AVAILABLE */

#ifdef __cplusplus
}
#endif

#endif /* LR_RENDERER_EGL_H */