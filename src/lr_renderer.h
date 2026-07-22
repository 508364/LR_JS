/*
 * L/R_JS - Render Pipeline Module
 * Pure C, all rendering output is forwarded through the render pipeline
 * to external renderers. No built-in software renderer.
 *
 * All rendering (Canvas 2D, WebGL) is wrapped into LR_RenderFrame and
 * dispatched through LR_RendererWrapper to arbitrary external renderers.
 * The wrapper is the unified interface — the pipeline does not know
 * what the external renderer is, it only calls wrapper->submit_frame().
 *
 * Built-in sinks (socket, shared memory, callback, file) are available
 * as additional side-outputs alongside the wrapper.
 */
#ifndef LR_RENDERER_H
#define LR_RENDERER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ── Renderer bridge (thin container for pipeline + shared memory) ──────── */

typedef struct LR_RendererBridge LR_RendererBridge;

struct LR_RendererBridge {
    /* Shared memory for pixel buffers (optional, for external renderers) */
    int                shm_fd;
    uint8_t           *shm_buf;
    size_t             shm_size;

    /* Render pipeline (external renderer output) */
    struct LR_RenderPipeline *pipeline;

    /* Stats */
    int64_t            frames_rendered;
    int64_t            total_render_time_us;
};

/* Create a renderer bridge (initially no pipeline attached). */
LR_RendererBridge *lr_renderer_create(void);

/* Destroy renderer bridge. */
void lr_renderer_destroy(LR_RendererBridge *rb);

/* ── Pipeline integration with renderer bridge ────────────────────────── */

/* Forward declaration (defined below in "Render Pipeline" section) */
typedef struct LR_RenderPipeline LR_RenderPipeline;

/* Attach a render pipeline to the bridge. Any existing pipeline is detached. */
void lr_renderer_set_pipeline(LR_RendererBridge *rb,
                              LR_RenderPipeline *pipeline);

/* Get the currently attached pipeline, or NULL. */
LR_RenderPipeline *lr_renderer_get_pipeline(LR_RendererBridge *rb);

/* ══════════════════════════════════════════════════════════════════════════
 *  LR_RenderFrame — Unified Frame Wrapper
 *
 *  Every rendered frame (Canvas 2D, WebGL, etc.) is wrapped into this
 *  structure before being dispatched to the external renderer.
 *  This is the universal container that the renderer wrapper receives.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Pixel format identifiers */
#define LR_PIXEL_FORMAT_RGBA_U32  0   /* uint32_t ARGB: 0xAARRGGBB */

/* Maximum source identifier length */
#define LR_RENDER_FRAME_SOURCE_LEN  32

typedef struct LR_RenderFrame {
    /* Frame dimensions */
    int                width;
    int                height;

    /* Pixel format and channel count */
    int                channels;       /* 4 for RGBA */
    int                pixel_format;   /* LR_PIXEL_FORMAT_* */

    /* Raw pixel data (RGBA 32-bit, width*height*4 bytes) */
    const uint32_t    *pixels;

    /* Frame metadata */
    int64_t            timestamp_us;   /* Monotonic timestamp at submission */
    uint32_t           frame_id;       /* Auto-incrementing frame counter */
    char               source[LR_RENDER_FRAME_SOURCE_LEN];  /* "canvas2d", "webgl", etc. */
} LR_RenderFrame;

/* ══════════════════════════════════════════════════════════════════════════
 *  LR_RendererWrapper — External Renderer Interface
 *
 *  The pipeline does NOT know what the external renderer is.
 *  Users implement this interface to connect any renderer backend.
 *
 *  Lifecycle:
 *    1. Call lr_render_pipeline_set_wrapper(pipe, &my_wrapper) to attach.
 *    2. On first frame, the pipeline calls wrapper->init() (if non-NULL).
 *    3. Each frame is wrapped into LR_RenderFrame and dispatched via
 *       wrapper->submit_frame().
 *    4. On pipeline destroy, wrapper->destroy() is called (if non-NULL).
 *
 *  The user_data pointer is passed back to all callbacks.
 *  All callbacks are optional (may be NULL).
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct LR_RendererWrapper {
    /* Opaque user data passed to all callbacks */
    void *user_data;

    /*
     * Initialize the external renderer.
     * Called once on the first frame submission.
     * The first frame is provided so the renderer knows the initial format.
     * Return 0 on success, -1 on error.
     */
    int (*init)(void *user_data, const LR_RenderFrame *frame);

    /*
     * Submit a rendered frame to the external renderer.
     * Called for every frame. The frame data is valid only during this call.
     * Return 0 on success, -1 on error.
     */
    int (*submit_frame)(void *user_data, const LR_RenderFrame *frame);

    /*
     * Destroy the external renderer.
     * Called when the pipeline is destroyed.
     * Clean up any resources allocated by init().
     */
    void (*destroy)(void *user_data);
} LR_RendererWrapper;

/* ── Render Pipeline (external renderer output) ────────────────────────── */

/*
 * The render pipeline receives rendered frames from Canvas 2D and WebGL,
 * wraps them into LR_RenderFrame, and dispatches them through the
 * LR_RendererWrapper to the external renderer.
 *
 * Additionally, built-in side-output sinks (socket, shared memory, callback,
 * file) can be attached for debugging or parallel output.
 *
 * Usage:
 *   1. Create a pipeline:         lr_render_pipeline_create(w, h)
 *   2. (Optional) Set a wrapper:  lr_render_pipeline_set_wrapper(pipe, &w)
 *   3. (Optional) Add side-sinks: lr_render_pipeline_add_sink(pipe, ...)
 *   4. Canvas 2D submits:         lr_render_pipeline_submit(pipe, pixels, w, h, "canvas2d")
 *   5. WebGL submits:             lr_render_pipeline_submit_gl(pipe, gl_ctx, w, h)
 *   6. Destroy:                   lr_render_pipeline_destroy(pipe)
 */

/* Pipeline sink types (side-outputs alongside the wrapper) */
typedef enum {
    LR_PIPE_SINK_SOCKET,    /* Forward via Unix socket / TCP */
    LR_PIPE_SINK_SHM,       /* Write to shared memory buffer */
    LR_PIPE_SINK_CALLBACK,  /* Invoke user callback with frame data */
    LR_PIPE_SINK_FILE,      /* Write frame as PPM (debug) */
} LR_PipeSinkType;

/* Pipeline output sink configuration */
typedef struct LR_PipeSink {
    LR_PipeSinkType type;
    int              active;     /* 1 = enabled, 0 = disabled */

    /* Socket sink */
    int              fd;         /* Connected socket fd */

    /* Shared memory sink */
    void            *shm_ptr;    /* Shared memory buffer pointer */
    size_t           shm_size;   /* Buffer size */

    /* Callback sink */
    void            *user_data;  /* Opaque user pointer */
    void (*on_frame)(void *user_data, const uint32_t *pixels,
                     int width, int height);  /* Frame callback */

    /* File sink */
    char            *file_path;  /* Output file path */
    FILE            *file;       /* Opened file handle */
    int              frame_no;   /* Frame counter for unique filenames */
} LR_PipeSink;

/* Render pipeline - receives rendered frames and distributes to wrapper + sinks */
typedef struct LR_RenderPipeline {
    int              width;          /* Frame width */
    int              height;         /* Frame height */

    /* External renderer wrapper (primary output) */
    LR_RendererWrapper *wrapper;     /* Pluggable external renderer interface */

    /* Side-output sinks (secondary output, alongside wrapper) */
    LR_PipeSink     *sinks;          /* Array of output sinks */
    int              sink_count;     /* Number of sinks */
    int              sink_capacity;  /* Allocated sink capacity */

    /* Internal working buffer */
    uint32_t        *frame_buffer;   /* Internal working buffer */
    int              frame_ready;    /* Whether a frame has been submitted */

    /* Auto-incrementing frame counter */
    uint32_t         frame_counter;
} LR_RenderPipeline;

/* Create a render pipeline with the given frame dimensions. */
LR_RenderPipeline *lr_render_pipeline_create(int width, int height);

/* Destroy a render pipeline and all its resources (wrapper + sinks). */
void lr_render_pipeline_destroy(LR_RenderPipeline *pipe);

/* ── Wrapper management (primary output) ───────────────────────────────── */

/*
 * Set the external renderer wrapper.
 * The wrapper is owned by the caller and must remain valid until
 * the pipeline is destroyed or the wrapper is replaced.
 * Pass NULL to clear the wrapper.
 */
void lr_render_pipeline_set_wrapper(LR_RenderPipeline *pipe,
                                    LR_RendererWrapper *wrapper);

/* Get the currently attached wrapper, or NULL. */
LR_RendererWrapper *lr_render_pipeline_get_wrapper(LR_RenderPipeline *pipe);

/* ── Side-output sink management ───────────────────────────────────────── */

/* Add an output sink to the pipeline. Returns sink index, or -1 on error. */
int lr_render_pipeline_add_sink(LR_RenderPipeline *pipe,
                                const LR_PipeSink *sink);

/* Remove an output sink by index. */
int lr_render_pipeline_remove_sink(LR_RenderPipeline *pipe, int index);

/* Enable/disable a sink. */
int lr_render_pipeline_set_sink_active(LR_RenderPipeline *pipe,
                                       int index, int active);

/* ── Frame submission ──────────────────────────────────────────────────── */

/*
 * Submit a rendered frame from Canvas 2D to the pipeline.
 * The frame is wrapped into LR_RenderFrame and dispatched to:
 *   1. The external renderer wrapper (if set)
 *   2. All active side-output sinks (if any)
 *
 *   pixels: RGBA 32-bit pixel data (width*height*4 bytes)
 *   w, h:   frame dimensions
 *   source: frame source identifier (e.g. "canvas2d"), max 31 chars
 *
 * Returns 0 on success, -1 on error.
 */
int lr_render_pipeline_submit(LR_RenderPipeline *pipe,
                              const uint32_t *pixels,
                              int w, int h,
                              const char *source);

/*
 * Submit a rendered frame from WebGL/GLES context.
 * Reads back the current GL framebuffer and dispatches to:
 *   1. The external renderer wrapper (if set)
 *   2. All active side-output sinks (if any)
 *
 *   gl_ctx:   opaque pointer to native GLES context
 *   w, h:     viewport dimensions
 *
 * Returns 0 on success, -1 on error.
 */
int lr_render_pipeline_submit_gl(LR_RenderPipeline *pipe,
                                 void *gl_ctx, int w, int h);

/* ── Convenience: sink constructors ────────────────────────────────────── */

/* Create a socket sink connected to the given path. */
LR_PipeSink lr_render_pipe_sink_socket(const char *socket_path);

/* Create a shared memory sink. */
LR_PipeSink lr_render_pipe_sink_shm(void *shm_ptr, size_t shm_size);

/* Create a callback sink. */
LR_PipeSink lr_render_pipe_sink_callback(
    void (*on_frame)(void *user, const uint32_t *pixels, int w, int h),
    void *user_data);

/* Create a file sink (writes PPM frames). */
LR_PipeSink lr_render_pipe_sink_file(const char *file_pattern);

#endif /* LR_RENDERER_H */