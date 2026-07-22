/*
 * L/R_JS - EGL Renderer Backend Implementation
 * Pure C, OpenGL ES 2.0 via EGL for GPU-accelerated 2D rendering.
 *
 * Compile with -DLR_HAS_EGL and link against -lEGL -lGLESv2
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "lr_platform.h"
#include "lr_renderer_egl.h"

#if LR_EGL_AVAILABLE

/* ── GLES2 Shader sources ──────────────────────────────────────────────── */

static const char *g_vert_shader_2d =
    "attribute vec2 a_position;\n"
    "uniform mat4 u_projection;\n"
    "void main() {\n"
    "    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *g_frag_shader_2d =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "    gl_FragColor = u_color;\n"
    "}\n";

/* ── Internal helpers ──────────────────────────────────────────────────── */

static GLuint lr_egl_compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info = (char *)malloc((size_t)info_len);
            if (info) {
                glGetShaderInfoLog(shader, info_len, NULL, info);
                fprintf(stderr, "[EGL] Shader compile error: %s\n", info);
                free(info);
            }
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint lr_egl_create_program_2d(void)
{
    GLuint vert = lr_egl_compile_shader(GL_VERTEX_SHADER, g_vert_shader_2d);
    GLuint frag = lr_egl_compile_shader(GL_FRAGMENT_SHADER, g_frag_shader_2d);
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info = (char *)malloc((size_t)info_len);
            if (info) {
                glGetProgramInfoLog(prog, info_len, NULL, info);
                fprintf(stderr, "[EGL] Program link error: %s\n", info);
                free(info);
            }
        }
        glDeleteProgram(prog);
        glDeleteShader(vert);
        glDeleteShader(frag);
        return 0;
    }

    /* Shaders can be detached and deleted after linking */
    glDetachShader(prog, vert);
    glDetachShader(prog, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    return prog;
}

static void lr_egl_build_ortho(float *m, float left, float right,
                                float bottom, float top,
                                float near_val, float far_val)
{
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / (right - left);
    m[5]  = 2.0f / (top - bottom);
    m[10] = -2.0f / (far_val - near_val);
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[14] = -(far_val + near_val) / (far_val - near_val);
    m[15] = 1.0f;
}

static void lr_egl_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* ── Native handle getters ──────────────────────────────────────────────── */

void *lr_egl_renderer_get_display(LR_EGLRenderer *egl)
{
    return egl ? (void *)egl->display : NULL;
}

void *lr_egl_renderer_get_context(LR_EGLRenderer *egl)
{
    return egl ? (void *)egl->context : NULL;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int lr_egl_is_available(void)
{
    return 1;
}

LR_EGLRenderer *lr_egl_renderer_create(int width, int height, int offscreen,
                                       void *native_display, void *native_window)
{
    LR_EGLRenderer *egl = (LR_EGLRenderer *)calloc(1, sizeof(LR_EGLRenderer));
    if (!egl) return NULL;

    egl->width     = width > 0 ? width : 800;
    egl->height    = height > 0 ? height : 600;
    egl->offscreen = offscreen;
    egl->display   = EGL_NO_DISPLAY;
    egl->surface   = EGL_NO_SURFACE;
    egl->context   = EGL_NO_CONTEXT;
    egl->pbuffer_surface = EGL_NO_SURFACE;

    /* ── Get EGL display ───────────────────────────────────────────────── */
    EGLNativeDisplayType dpy = native_display
        ? (EGLNativeDisplayType)native_display
        : EGL_DEFAULT_DISPLAY;

    egl->display = eglGetDisplay(dpy);
    if (egl->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "[EGL] eglGetDisplay failed\n");
        goto fail;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl->display, &major, &minor)) {
        fprintf(stderr, "[EGL] eglInitialize failed\n");
        goto fail;
    }

    /* ── Choose config ─────────────────────────────────────────────────── */
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE,    offscreen ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_NONE
    };

    EGLint num_configs = 0;
    if (!eglChooseConfig(egl->display, config_attrs, &egl->config, 1, &num_configs)
        || num_configs < 1) {
        /* Fallback: try without alpha */
        config_attrs[11] = 0; /* ALPHA_SIZE = 0 */
        if (!eglChooseConfig(egl->display, config_attrs, &egl->config, 1, &num_configs)
            || num_configs < 1) {
            fprintf(stderr, "[EGL] eglChooseConfig failed\n");
            goto fail;
        }
    }

    /* ── Create surface ────────────────────────────────────────────────── */
    if (offscreen) {
        EGLint pbuffer_attrs[] = {
            EGL_WIDTH,  egl->width,
            EGL_HEIGHT, egl->height,
            EGL_NONE
        };
        egl->pbuffer_surface = eglCreatePbufferSurface(egl->display, egl->config,
                                                        pbuffer_attrs);
        if (egl->pbuffer_surface == EGL_NO_SURFACE) {
            fprintf(stderr, "[EGL] eglCreatePbufferSurface failed\n");
            goto fail;
        }
        egl->surface = egl->pbuffer_surface;
    } else {
        if (native_window) {
            egl->surface = eglCreateWindowSurface(egl->display, egl->config,
                                                   (EGLNativeWindowType)native_window, NULL);
            if (egl->surface == EGL_NO_SURFACE) {
                fprintf(stderr, "[EGL] eglCreateWindowSurface failed\n");
                goto fail;
            }
        } else {
            fprintf(stderr, "[EGL] On-screen mode requires native_window\n");
            goto fail;
        }
    }

    /* ── Create context ────────────────────────────────────────────────── */
    EGLint context_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    egl->context = eglCreateContext(egl->display, egl->config,
                                     EGL_NO_CONTEXT, context_attrs);
    if (egl->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "[EGL] eglCreateContext failed\n");
        goto fail;
    }

    /* ── Make current ──────────────────────────────────────────────────── */
    if (!eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context)) {
        fprintf(stderr, "[EGL] eglMakeCurrent failed\n");
        goto fail;
    }

    /* ── Create shader program ─────────────────────────────────────────── */
    egl->program_2d = lr_egl_create_program_2d();
    if (!egl->program_2d) {
        fprintf(stderr, "[EGL] Failed to create shader program\n");
        goto fail;
    }

    egl->attrib_pos   = glGetAttribLocation(egl->program_2d, "a_position");
    egl->uniform_color = glGetUniformLocation(egl->program_2d, "u_color");
    egl->uniform_proj  = glGetUniformLocation(egl->program_2d, "u_projection");

    /* Create VBO */
    glGenBuffers(1, &egl->vbo);

    /* Default state */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glViewport(0, 0, egl->width, egl->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Allocate readback buffer */
    egl->readback_buf = (uint32_t *)malloc(
        (size_t)egl->width * (size_t)egl->height * sizeof(uint32_t));
    egl->readback_w = egl->width;
    egl->readback_h = egl->height;

    return egl;

fail:
    lr_egl_renderer_destroy(egl);
    return NULL;
}

void lr_egl_renderer_destroy(LR_EGLRenderer *egl)
{
    if (!egl) return;

    if (egl->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (egl->vbo) {
            glDeleteBuffers(1, &egl->vbo);
            egl->vbo = 0;
        }
        if (egl->program_2d) {
            glDeleteProgram(egl->program_2d);
            egl->program_2d = 0;
        }
        if (egl->context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl->display, egl->context);
            egl->context = EGL_NO_CONTEXT;
        }
        if (egl->pbuffer_surface != EGL_NO_SURFACE) {
            eglDestroySurface(egl->display, egl->pbuffer_surface);
            egl->pbuffer_surface = EGL_NO_SURFACE;
        }
        if (egl->surface != EGL_NO_SURFACE && egl->surface != egl->pbuffer_surface) {
            eglDestroySurface(egl->display, egl->surface);
        }
        egl->surface = EGL_NO_SURFACE;
        eglTerminate(egl->display);
        egl->display = EGL_NO_DISPLAY;
    }

    free(egl->readback_buf);
    free(egl);
}

int lr_egl_renderer_make_current(LR_EGLRenderer *egl)
{
    if (!egl || egl->display == EGL_NO_DISPLAY) return -1;
    if (!eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context))
        return -1;
    return 0;
}

int lr_egl_renderer_swap_buffers(LR_EGLRenderer *egl)
{
    if (!egl) return -1;
    glFlush();
    if (!eglSwapBuffers(egl->display, egl->surface))
        return -1;
    return 0;
}

uint32_t *lr_egl_renderer_read_pixels(LR_EGLRenderer *egl, int *width, int *height)
{
    if (!egl || !egl->readback_buf) return NULL;

    /* Reallocate if size changed */
    int need = (int)((size_t)egl->width * (size_t)egl->height * sizeof(uint32_t));
    if (egl->readback_w != egl->width || egl->readback_h != egl->height) {
        uint32_t *nb = (uint32_t *)realloc(egl->readback_buf, (size_t)need);
        if (!nb) return NULL;
        egl->readback_buf = nb;
        egl->readback_w = egl->width;
        egl->readback_h = egl->height;
    }

    glReadPixels(0, 0, egl->width, egl->height, GL_RGBA, GL_UNSIGNED_BYTE,
                 egl->readback_buf);

    if (width)  *width  = egl->width;
    if (height) *height = egl->height;
    return egl->readback_buf;
}

int lr_egl_renderer_resize(LR_EGLRenderer *egl, int width, int height)
{
    if (!egl) return -1;
    if (width <= 0 || height <= 0) return -1;

    egl->width  = width;
    egl->height = height;
    glViewport(0, 0, width, height);

    /* Reallocate readback */
    uint32_t *nb = (uint32_t *)realloc(egl->readback_buf,
        (size_t)width * (size_t)height * sizeof(uint32_t));
    if (nb) {
        egl->readback_buf = nb;
        egl->readback_w = width;
        egl->readback_h = height;
    }

    return 0;
}

#endif /* LR_EGL_AVAILABLE */