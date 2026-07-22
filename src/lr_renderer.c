/*
 * L/R_JS - Render Pipeline Implementation
 * Pure C, no built-in software renderer.
 * All rendering output is wrapped into LR_RenderFrame and dispatched
 * through LR_RendererWrapper to external renderers.
 *
 * Canvas 2D manages its own framebuffer and submits to pipeline on flush.
 * WebGL reads back GL framebuffer and submits to pipeline on flush.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "lr_platform.h"

#if !LR_PLATFORM_WINDOWS
#include <sys/un.h>
#endif

#include "lr_renderer.h"

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Get monotonic timestamp in microseconds */
static int64_t lr_now_us(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
#endif
}

/* Build an LR_RenderFrame from raw pixel data */
static void lr_build_frame(LR_RenderFrame *frame,
                           LR_RenderPipeline *pipe,
                           const uint32_t *pixels,
                           int w, int h,
                           const char *source)
{
    frame->width       = w;
    frame->height      = h;
    frame->channels    = 4;
    frame->pixel_format = LR_PIXEL_FORMAT_RGBA_U32;
    frame->pixels      = pixels;
    frame->timestamp_us = lr_now_us();
    frame->frame_id    = ++pipe->frame_counter;
    if (source) {
        strncpy(frame->source, source, LR_RENDER_FRAME_SOURCE_LEN - 1);
        frame->source[LR_RENDER_FRAME_SOURCE_LEN - 1] = '\0';
    } else {
        frame->source[0] = '\0';
    }
}

/* ── Create / Destroy ──────────────────────────────────────────────────── */

LR_RendererBridge *lr_renderer_create(void)
{
    LR_RendererBridge *rb = calloc(1, sizeof(LR_RendererBridge));
    if (!rb) return NULL;

    rb->shm_fd = -1;
    return rb;
}

void lr_renderer_destroy(LR_RendererBridge *rb)
{
    if (!rb) return;

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

    /* Note: pipeline is owned by the caller, not destroyed here */
    free(rb);
}

/* ── Pipeline integration with renderer bridge ────────────────────────── */

void lr_renderer_set_pipeline(LR_RendererBridge *rb,
                              LR_RenderPipeline *pipeline)
{
    if (!rb) return;
    rb->pipeline = pipeline;
}

LR_RenderPipeline *lr_renderer_get_pipeline(LR_RendererBridge *rb)
{
    return rb ? rb->pipeline : NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Render Pipeline Implementation
 *  Receives frames from Canvas 2D / WebGL, wraps them into LR_RenderFrame,
 *  and dispatches to the external renderer wrapper + side-output sinks.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Convenience: create sink configurations ──────────────────────────── */

LR_PipeSink lr_render_pipe_sink_socket(const char *socket_path)
{
    LR_PipeSink sink;
    memset(&sink, 0, sizeof(sink));
    sink.type = LR_PIPE_SINK_SOCKET;
    sink.active = 1;
    sink.fd = -1;

    if (!socket_path) return sink;

#if !LR_PLATFORM_WINDOWS
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return sink;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        sink.fd = fd;
    } else {
        lr_socket_close(fd);
    }
#else
    (void)socket_path;
#endif

    return sink;
}

LR_PipeSink lr_render_pipe_sink_shm(void *shm_ptr, size_t shm_size)
{
    LR_PipeSink sink;
    memset(&sink, 0, sizeof(sink));
    sink.type = LR_PIPE_SINK_SHM;
    sink.active = 1;
    sink.shm_ptr = shm_ptr;
    sink.shm_size = shm_size;
    return sink;
}

LR_PipeSink lr_render_pipe_sink_callback(
    void (*on_frame)(void *user, const uint32_t *pixels, int w, int h),
    void *user_data)
{
    LR_PipeSink sink;
    memset(&sink, 0, sizeof(sink));
    sink.type = LR_PIPE_SINK_CALLBACK;
    sink.active = 1;
    sink.on_frame = on_frame;
    sink.user_data = user_data;
    return sink;
}

LR_PipeSink lr_render_pipe_sink_file(const char *file_path)
{
    LR_PipeSink sink;
    memset(&sink, 0, sizeof(sink));
    sink.type = LR_PIPE_SINK_FILE;
    sink.active = 1;
    sink.frame_no = 0;
    if (file_path) {
        sink.file_path = strdup(file_path);
    }
    return sink;
}

/* ── Pipeline create / destroy ──────────────────────────────────────────── */

LR_RenderPipeline *lr_render_pipeline_create(int width, int height)
{
    LR_RenderPipeline *pipe = calloc(1, sizeof(LR_RenderPipeline));
    if (!pipe) return NULL;

    pipe->width = width > 0 ? width : 800;
    pipe->height = height > 0 ? height : 600;
    pipe->sink_capacity = 4;
    pipe->sinks = calloc((size_t)pipe->sink_capacity, sizeof(LR_PipeSink));
    if (!pipe->sinks) {
        free(pipe);
        return NULL;
    }

    /* Allocate internal frame buffer */
    size_t fb_size = (size_t)pipe->width * (size_t)pipe->height * sizeof(uint32_t);
    pipe->frame_buffer = calloc(1, fb_size);
    if (!pipe->frame_buffer) {
        free(pipe->sinks);
        free(pipe);
        return NULL;
    }

    /* Frame counter starts at 0 */
    pipe->frame_counter = 0;

    return pipe;
}

void lr_render_pipeline_destroy(LR_RenderPipeline *pipe)
{
    if (!pipe) return;

    /* Call the wrapper's destroy callback */
    if (pipe->wrapper && pipe->wrapper->destroy) {
        pipe->wrapper->destroy(pipe->wrapper->user_data);
    }

    /* Close all side-output sinks */
    for (int i = 0; i < pipe->sink_count; i++) {
        LR_PipeSink *sink = &pipe->sinks[i];
        if (sink->type == LR_PIPE_SINK_SOCKET && sink->fd >= 0) {
            lr_socket_close(sink->fd);
        }
        if (sink->type == LR_PIPE_SINK_FILE && sink->file) {
            fclose(sink->file);
        }
        free(sink->file_path);
    }

    free(pipe->sinks);
    free(pipe->frame_buffer);
    free(pipe);
}

/* ── Wrapper management ────────────────────────────────────────────────── */

void lr_render_pipeline_set_wrapper(LR_RenderPipeline *pipe,
                                    LR_RendererWrapper *wrapper)
{
    if (!pipe) return;
    pipe->wrapper = wrapper;
}

LR_RendererWrapper *lr_render_pipeline_get_wrapper(LR_RenderPipeline *pipe)
{
    return pipe ? pipe->wrapper : NULL;
}

/* ── Sink management ────────────────────────────────────────────────────── */

int lr_render_pipeline_add_sink(LR_RenderPipeline *pipe,
                                const LR_PipeSink *sink)
{
    if (!pipe || !sink) return -1;

    /* Grow array if needed */
    if (pipe->sink_count >= pipe->sink_capacity) {
        int new_cap = pipe->sink_capacity * 2;
        LR_PipeSink *new_sinks = realloc(pipe->sinks,
                                         (size_t)new_cap * sizeof(LR_PipeSink));
        if (!new_sinks) return -1;
        pipe->sinks = new_sinks;
        pipe->sink_capacity = new_cap;
    }

    pipe->sinks[pipe->sink_count] = *sink;
    return pipe->sink_count++;
}

int lr_render_pipeline_remove_sink(LR_RenderPipeline *pipe, int index)
{
    if (!pipe || index < 0 || index >= pipe->sink_count) return -1;

    LR_PipeSink *sink = &pipe->sinks[index];
    if (sink->type == LR_PIPE_SINK_SOCKET && sink->fd >= 0) {
        lr_socket_close(sink->fd);
    }
    if (sink->type == LR_PIPE_SINK_FILE && sink->file) {
        fclose(sink->file);
    }
    free(sink->file_path);

    /* Shift remaining sinks */
    int remaining = pipe->sink_count - index - 1;
    if (remaining > 0) {
        memmove(&pipe->sinks[index], &pipe->sinks[index + 1],
                (size_t)remaining * sizeof(LR_PipeSink));
    }
    pipe->sink_count--;
    return 0;
}

int lr_render_pipeline_set_sink_active(LR_RenderPipeline *pipe,
                                       int index, int active)
{
    if (!pipe || index < 0 || index >= pipe->sink_count) return -1;
    pipe->sinks[index].active = !!active;
    return 0;
}

/* ── Internal: send frame to a single side-output sink ──────────────────── */

static int pipe_sink_send_frame(LR_PipeSink *sink,
                                const uint32_t *pixels,
                                int width, int height)
{
    size_t data_size = (size_t)width * (size_t)height * sizeof(uint32_t);
    size_t header_size = 64;
    char header[64];

    switch (sink->type) {
    case LR_PIPE_SINK_SOCKET:
        if (sink->fd < 0) return -1;
        /* Send length-prefixed frame: "FRAME W H <size>\n" + raw pixel data */
        snprintf(header, sizeof(header), "FRAME %d %d %zu\n", width, height, data_size);
        if (send(sink->fd, header, strlen(header), MSG_NOSIGNAL) < 0) return -1;
        if (send(sink->fd, (const char *)pixels, data_size, MSG_NOSIGNAL) < 0) return -1;
        break;

    case LR_PIPE_SINK_SHM:
        if (!sink->shm_ptr || sink->shm_size < data_size + header_size) return -1;
        /* Write header + pixel data into shared memory */
        snprintf((char *)sink->shm_ptr, header_size, "FRAME %d %d %zu\n", width, height, data_size);
        memcpy((uint8_t *)sink->shm_ptr + header_size, pixels, data_size);
        break;

    case LR_PIPE_SINK_CALLBACK:
        if (sink->on_frame) {
            sink->on_frame(sink->user_data, pixels, width, height);
        }
        break;

    case LR_PIPE_SINK_FILE: {
        /* Write as PPM (frame_0001.ppm, frame_0002.ppm, ...) */
        char filename[512];
        if (sink->file_path) {
            snprintf(filename, sizeof(filename), sink->file_path, sink->frame_no + 1);
        } else {
            snprintf(filename, sizeof(filename), "frame_%04d.ppm", sink->frame_no + 1);
        }
        FILE *f = fopen(filename, "wb");
        if (!f) return -1;
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                uint32_t pixel = pixels[i * width + j];
                uint8_t rgb[3];
                rgb[0] = (pixel >> 16) & 0xFF;  /* R */
                rgb[1] = (pixel >> 8) & 0xFF;   /* G */
                rgb[2] = pixel & 0xFF;          /* B */
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        sink->frame_no++;
        break;
    }
    }

    return 0;
}

/* ── Internal: dispatch LR_RenderFrame to wrapper + sinks ──────────────── */

static int lr_dispatch_frame(LR_RenderPipeline *pipe,
                              const LR_RenderFrame *frame)
{
    int ret = 0;

    /* 1. Dispatch to the external renderer wrapper (primary output) */
    if (pipe->wrapper && pipe->wrapper->submit_frame) {
        /* Call init on first frame if available */
        if (pipe->frame_counter == 1 && pipe->wrapper->init) {
            if (pipe->wrapper->init(pipe->wrapper->user_data, frame) != 0) {
                /* Init failed - still try to submit */
            }
        }
        if (pipe->wrapper->submit_frame(pipe->wrapper->user_data, frame) != 0) {
            ret = -1;
        }
    }

    /* 2. Dispatch to side-output sinks (secondary output) */
    for (int i = 0; i < pipe->sink_count; i++) {
        if (pipe->sinks[i].active) {
            pipe_sink_send_frame(&pipe->sinks[i], frame->pixels,
                                 frame->width, frame->height);
        }
    }

    return ret;
}

/* ── Submit frame (from Canvas 2D) ──────────────────────────────────────── */

int lr_render_pipeline_submit(LR_RenderPipeline *pipe,
                              const uint32_t *pixels,
                              int w, int h,
                              const char *source)
{
    if (!pipe || !pixels) return -1;
    if (w != pipe->width || h != pipe->height) return -1;

    /* Copy to internal buffer */
    size_t fb_size = (size_t)w * (size_t)h * sizeof(uint32_t);

    /* Resize internal buffer if needed */
    size_t cur_size = (size_t)pipe->width * (size_t)pipe->height * sizeof(uint32_t);
    if (fb_size > cur_size) {
        uint32_t *new_buf = realloc(pipe->frame_buffer, fb_size);
        if (!new_buf) return -1;
        pipe->frame_buffer = new_buf;
        pipe->width = w;
        pipe->height = h;
    }

    memcpy(pipe->frame_buffer, pixels, fb_size);
    pipe->frame_ready = 1;

    /* Build LR_RenderFrame and dispatch */
    LR_RenderFrame frame;
    lr_build_frame(&frame, pipe, pipe->frame_buffer, w, h, source);
    return lr_dispatch_frame(pipe, &frame);
}

/* ── Submit GL frame (from WebGL) ───────────────────────────────────────── */

int lr_render_pipeline_submit_gl(LR_RenderPipeline *pipe,
                                 void *gl_ctx, int w, int h)
{
    if (!pipe || !gl_ctx) return -1;

#if LR_EGL_AVAILABLE
    /* Read back the GL framebuffer */
    size_t fb_size = (size_t)w * (size_t)h * sizeof(uint32_t);

    /* Ensure internal buffer is large enough */
    size_t cur_size = (size_t)pipe->width * (size_t)pipe->height * sizeof(uint32_t);
    if (fb_size > cur_size) {
        uint32_t *new_buf = realloc(pipe->frame_buffer, fb_size);
        if (!new_buf) return -1;
        pipe->frame_buffer = new_buf;
    }
    pipe->width = w;
    pipe->height = h;

    /* Read pixels from the current GL framebuffer */
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pipe->frame_buffer);
    pipe->frame_ready = 1;

    /* Build LR_RenderFrame and dispatch */
    LR_RenderFrame frame;
    lr_build_frame(&frame, pipe, pipe->frame_buffer, w, h, "webgl");
    return lr_dispatch_frame(pipe, &frame);
#else
    (void)gl_ctx;
    (void)w;
    (void)h;
    /* No GLES available - nothing to read back */
    return -1;
#endif
}