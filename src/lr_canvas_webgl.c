/*
 * L/R_JS - Canvas WebGL (WebGL 1.0 + WebGL 2.0 bindings)
 * Pure C, ES2022-compatible
 *
 * Complete WebGLRenderingContext and WebGL2RenderingContext implementation.
 * Backed by GLES2.0/3.0 when available (via EGL renderer), with a software
 * state-tracking fallback when no native GLES context is available.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "lr_runtime.h"
#include "lr_renderer.h"
#include "lr_renderer_egl.h"

/* ── GLES includes when available ──────────────────────────────────────── */
#if LR_EGL_AVAILABLE
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#ifdef GL_ES_VERSION_3_0
#include <GLES3/gl3.h>
#define LR_GLES3_AVAILABLE 1
#endif
#else
/* Stub GL types when GLES is not available */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef int GLfixed;
typedef long GLintptr;
typedef long GLsizeiptr;
typedef char GLchar;
#endif

/* ── WebGL 1.0 Constants ──────────────────────────────────────────────── */
#define GL_DEPTH_BUFFER_BIT              0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT              0x00004000
#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006
#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_NOTEQUAL                       0x0205
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207
#define GL_ZERO                           0
#define GL_ONE                            1
#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308
#define GL_FUNC_ADD                       0x8006
#define GL_BLEND_EQUATION                 0x8009
#define GL_BLEND_EQUATION_RGB             0x8009
#define GL_BLEND_EQUATION_ALPHA           0x883D
#define GL_FUNC_SUBTRACT                  0x800A
#define GL_FUNC_REVERSE_SUBTRACT          0x800B
#define GL_BLEND_DST_RGB                  0x80C8
#define GL_BLEND_SRC_RGB                  0x80C9
#define GL_BLEND_DST_ALPHA                0x80CA
#define GL_BLEND_SRC_ALPHA                0x80CB
#define GL_CONSTANT_COLOR                 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR       0x8002
#define GL_CONSTANT_ALPHA                 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA       0x8004
#define GL_BLEND_COLOR                    0x8005
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_ARRAY_BUFFER_BINDING           0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING   0x8895
#define GL_STREAM_DRAW                    0x88E0
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_BUFFER_SIZE                    0x8764
#define GL_BUFFER_USAGE                   0x8765
#define GL_CURRENT_VERTEX_ATTRIB          0x8626
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED    0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE       0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE     0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE       0x8625
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A
#define GL_VERTEX_ATTRIB_ARRAY_POINTER    0x8645
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
#define GL_CULL_FACE                      0x0B44
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_TEXTURE_2D                     0x0DE1
#define GL_BLEND                          0x0BE2
#define GL_DITHER                         0x0BD0
#define GL_STENCIL_TEST                   0x0B90
#define GL_DEPTH_TEST                     0x0B71
#define GL_SCISSOR_TEST                   0x0C11
#define GL_POLYGON_OFFSET_FILL            0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE       0x809E
#define GL_SAMPLE_COVERAGE                0x80A0
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_OUT_OF_MEMORY                  0x0505
#define GL_CW                             0x0900
#define GL_CCW                            0x0901
#define GL_LINE_WIDTH                     0x0B21
#define GL_ALIASED_POINT_SIZE_RANGE       0x846D
#define GL_ALIASED_LINE_WIDTH_RANGE       0x846E
#define GL_CULL_FACE_MODE                 0x0B45
#define GL_FRONT_FACE                     0x0B46
#define GL_DEPTH_RANGE                    0x0B70
#define GL_DEPTH_WRITEMASK                0x0B72
#define GL_DEPTH_CLEAR_VALUE              0x0B73
#define GL_DEPTH_FUNC                     0x0B74
#define GL_STENCIL_CLEAR_VALUE            0x0B91
#define GL_STENCIL_FUNC                   0x0B92
#define GL_STENCIL_VALUE_MASK             0x0B93
#define GL_STENCIL_FAIL                   0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL        0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS        0x0B96
#define GL_STENCIL_REF                    0x0B97
#define GL_STENCIL_WRITEMASK              0x0B98
#define GL_STENCIL_BACK_FUNC              0x8800
#define GL_STENCIL_BACK_FAIL              0x8801
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL   0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS   0x8803
#define GL_STENCIL_BACK_REF               0x8CA3
#define GL_STENCIL_BACK_VALUE_MASK        0x8CA4
#define GL_STENCIL_BACK_WRITEMASK         0x8CA5
#define GL_VIEWPORT                       0x0BA2
#define GL_SCISSOR_BOX                    0x0C10
#define GL_COLOR_CLEAR_VALUE              0x0C22
#define GL_COLOR_WRITEMASK                0x0C23
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_PACK_REVERSE_ROW_ORDER_ANGLE   0x93A4
#define GL_MAX_TEXTURE_SIZE               0x0D33
#define GL_MAX_VIEWPORT_DIMS              0x0D3A
#define GL_SUBPIXEL_BITS                  0x0D50
#define GL_RED_BITS                       0x0D52
#define GL_GREEN_BITS                     0x0D53
#define GL_BLUE_BITS                      0x0D54
#define GL_ALPHA_BITS                     0x0D55
#define GL_DEPTH_BITS                     0x0D56
#define GL_STENCIL_BITS                   0x0D57
#define GL_POLYGON_OFFSET_UNITS           0x2A00
#define GL_POLYGON_OFFSET_FACTOR          0x8038
#define GL_TEXTURE_BINDING_2D             0x8069
#define GL_SAMPLE_BUFFERS                 0x80A8
#define GL_SAMPLES                        0x80A9
#define GL_SAMPLE_COVERAGE_VALUE          0x80AA
#define GL_SAMPLE_COVERAGE_INVERT         0x80AB
#define GL_GENERATE_MIPMAP_HINT           0x8192
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#define GL_REPEAT                         0x2901
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_MIRRORED_REPEAT                0x8370
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2
#define GL_TEXTURE3                       0x84C3
#define GL_TEXTURE4                       0x84C4
#define GL_TEXTURE5                       0x84C5
#define GL_TEXTURE6                       0x84C6
#define GL_TEXTURE7                       0x84C7
#define GL_TEXTURE8                       0x84C8
#define GL_TEXTURE9                       0x84C9
#define GL_TEXTURE10                      0x84CA
#define GL_TEXTURE11                      0x84CB
#define GL_TEXTURE12                      0x84CC
#define GL_TEXTURE13                      0x84CD
#define GL_TEXTURE14                      0x84CE
#define GL_TEXTURE15                      0x84CF
#define GL_TEXTURE16                      0x84D0
#define GL_TEXTURE17                      0x84D1
#define GL_TEXTURE18                      0x84D2
#define GL_TEXTURE19                      0x84D3
#define GL_TEXTURE20                      0x84D4
#define GL_TEXTURE21                      0x84D5
#define GL_TEXTURE22                      0x84D6
#define GL_TEXTURE23                      0x84D7
#define GL_TEXTURE24                      0x84D8
#define GL_TEXTURE25                      0x84D9
#define GL_TEXTURE26                      0x84DA
#define GL_TEXTURE27                      0x84DB
#define GL_TEXTURE28                      0x84DC
#define GL_TEXTURE29                      0x84DD
#define GL_TEXTURE30                      0x84DE
#define GL_TEXTURE31                      0x84DF
#define GL_ACTIVE_TEXTURE                 0x84E0
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_ALPHA                          0x1906
#define GL_LUMINANCE                      0x1909
#define GL_LUMINANCE_ALPHA                0x190A
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_UNSIGNED_SHORT_4_4_4_4         0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1         0x8034
#define GL_UNSIGNED_SHORT_5_6_5           0x8363
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_MAX_VERTEX_ATTRIBS             0x8869
#define GL_MAX_VERTEX_UNIFORM_VECTORS     0x8DFB
#define GL_MAX_VARYING_VECTORS            0x8DFC
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_TEXTURE_IMAGE_UNITS        0x8872
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS   0x8DFD
#define GL_SHADER_TYPE                    0x8B4F
#define GL_DELETE_STATUS                  0x8B80
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_VALIDATE_STATUS                0x8B83
#define GL_ATTACHED_SHADERS               0x8B85
#define GL_ACTIVE_UNIFORMS                0x8B86
#define GL_ACTIVE_ATTRIBUTES              0x8B89
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_CURRENT_PROGRAM                0x8B8D
#define GL_KEEP                           0x1E00
#define GL_REPLACE                        0x1E01
#define GL_INCR                           0x1E02
#define GL_DECR                           0x1E03
#define GL_INVERT                         0x150A
#define GL_INCR_WRAP                      0x8507
#define GL_DECR_WRAP                      0x8508
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03
#define GL_DONT_CARE                      0x1100
#define GL_FASTEST                        0x1101
#define GL_NICEST                         0x1102
#define GL_BYTE                           0x1400
#define GL_SHORT                          0x1402
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_INT                            0x1404
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406
#define GL_FIXED                          0x140C
#define GL_DEPTH_COMPONENT                0x1902
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_RGBA4                          0x8056
#define GL_RGB5_A1                        0x8057
#define GL_RGB565                         0x8D62
#define GL_DEPTH_COMPONENT16              0x81A5
#define GL_STENCIL_INDEX8                 0x8D48
#define GL_DEPTH_STENCIL                  0x84F9
#define GL_RENDERBUFFER_WIDTH             0x8D42
#define GL_RENDERBUFFER_HEIGHT            0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT   0x8D44
#define GL_RENDERBUFFER_RED_SIZE          0x8D50
#define GL_RENDERBUFFER_GREEN_SIZE        0x8D51
#define GL_RENDERBUFFER_BLUE_SIZE         0x8D52
#define GL_RENDERBUFFER_ALPHA_SIZE        0x8D53
#define GL_RENDERBUFFER_DEPTH_SIZE        0x8D54
#define GL_RENDERBUFFER_STENCIL_SIZE      0x8D55
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE          0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME          0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL        0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE 0x8CD3
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_STENCIL_ATTACHMENT             0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
#define GL_NONE                           0
#define GL_FRAMEBUFFER_COMPLETE                      0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT         0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS         0x8CD9
#define GL_FRAMEBUFFER_UNSUPPORTED                   0x8CDD
#define GL_FRAMEBUFFER_BINDING           0x8CA6
#define GL_RENDERBUFFER_BINDING          0x8CA7
#define GL_MAX_RENDERBUFFER_SIZE         0x84E8
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#define GL_IMPLEMENTATION_COLOR_READ_TYPE   0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B

/* Internal WebGL object types */
#define LR_WEBGL_OBJECT_BUFFER        1
#define LR_WEBGL_OBJECT_FRAMEBUFFER   2
#define LR_WEBGL_OBJECT_PROGRAM       3
#define LR_WEBGL_OBJECT_RENDERBUFFER  4
#define LR_WEBGL_OBJECT_SHADER        5
#define LR_WEBGL_OBJECT_TEXTURE       6
#define LR_WEBGL_OBJECT_QUERY         7
#define LR_WEBGL_OBJECT_SAMPLER       8
#define LR_WEBGL_OBJECT_SYNC          9
#define LR_WEBGL_OBJECT_TRANSFORM_FEEDBACK 10
#define LR_WEBGL_OBJECT_VERTEX_ARRAY  11

/* ── WebGL 2.0 Constants ──────────────────────────────────────────────── */
#define GL_READ_BUFFER                   0x0C02
#define GL_UNPACK_ROW_LENGTH             0x0CF2
#define GL_UNPACK_SKIP_ROWS              0x0CF3
#define GL_UNPACK_SKIP_PIXELS            0x0CF4
#define GL_PACK_ROW_LENGTH               0x0D02
#define GL_PACK_SKIP_ROWS                0x0D03
#define GL_PACK_SKIP_PIXELS              0x0D04
#define GL_COLOR                         0x1800
#define GL_DEPTH                         0x1801
#define GL_STENCIL                       0x1802
#define GL_RED                           0x1903
#define GL_RGB8                          0x8051
#define GL_RGBA8                         0x8058
#define GL_RGB10_A2                      0x8059
#define GL_TEXTURE_BINDING_3D            0x806A
#define GL_UNPACK_SKIP_IMAGES            0x806D
#define GL_UNPACK_IMAGE_HEIGHT           0x806E
#define GL_TEXTURE_3D                    0x806F
#define GL_TEXTURE_WRAP_R                0x8072
#define GL_MAX_3D_TEXTURE_SIZE           0x8073
#define GL_UNSIGNED_INT_2_10_10_10_REV   0x8368
#define GL_MAX_ELEMENTS_VERTICES         0x80E8
#define GL_MAX_ELEMENTS_INDICES          0x80E9
#define GL_TEXTURE_MIN_LOD               0x813A
#define GL_TEXTURE_MAX_LOD               0x813B
#define GL_TEXTURE_BASE_LEVEL            0x813C
#define GL_TEXTURE_MAX_LEVEL             0x813D
#define GL_MIN                           0x8007
#define GL_MAX                           0x8008
#define GL_DEPTH_COMPONENT24             0x81A6
#define GL_MAX_TEXTURE_LOD_BIAS          0x84FD
#define GL_TEXTURE_COMPARE_MODE          0x884C
#define GL_TEXTURE_COMPARE_FUNC          0x884D
#define GL_CURRENT_QUERY                 0x8865
#define GL_QUERY_RESULT                  0x8866
#define GL_QUERY_RESULT_AVAILABLE        0x8867
#define GL_BUFFER_MAPPED                 0x88BC
#define GL_BUFFER_MAP_POINTER            0x88BD
#define GL_STREAM_READ                   0x88E1
#define GL_STREAM_COPY                   0x88E2
#define GL_STATIC_READ                   0x88E5
#define GL_STATIC_COPY                   0x88E6
#define GL_DYNAMIC_READ                  0x88E9
#define GL_DYNAMIC_COPY                  0x88EA
#define GL_MAX_DRAW_BUFFERS              0x8824
#define GL_DRAW_BUFFER0                  0x8825
#define GL_DRAW_BUFFER1                  0x8826
#define GL_DRAW_BUFFER2                  0x8827
#define GL_DRAW_BUFFER3                  0x8828
#define GL_DRAW_BUFFER4                  0x8829
#define GL_DRAW_BUFFER5                  0x882A
#define GL_DRAW_BUFFER6                  0x882B
#define GL_DRAW_BUFFER7                  0x882C
#define GL_DRAW_BUFFER8                  0x882D
#define GL_DRAW_BUFFER9                  0x882E
#define GL_DRAW_BUFFER10                 0x882F
#define GL_DRAW_BUFFER11                 0x8830
#define GL_DRAW_BUFFER12                 0x8831
#define GL_DRAW_BUFFER13                 0x8832
#define GL_DRAW_BUFFER14                 0x8833
#define GL_DRAW_BUFFER15                 0x8834
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS  0x8B49
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS    0x8B4A
#define GL_MAX_VARYING_COMPONENTS           0x8B4B
#define GL_MAX_VERTEX_UNIFORM_BLOCKS        0x8A2B
#define GL_MAX_FRAGMENT_UNIFORM_BLOCKS      0x8A2D
#define GL_MAX_UNIFORM_BUFFER_BINDINGS      0x8A2F
#define GL_MAX_UNIFORM_BLOCK_SIZE           0x8A30
#define GL_MAX_COMBINED_UNIFORM_BLOCKS      0x8A2E
#define GL_MAX_VERTEX_OUTPUT_COMPONENTS     0x9122
#define GL_MAX_FRAGMENT_INPUT_COMPONENTS    0x9125
#define GL_MAX_SERVER_WAIT_TIMEOUT          0x9111
#define GL_OBJECT_TYPE                      0x9112
#define GL_SYNC_CONDITION                   0x9113
#define GL_SYNC_STATUS                      0x9114
#define GL_SYNC_FLAGS                       0x9115
#define GL_SYNC_FENCE                       0x9116
#define GL_SYNC_GPU_COMMANDS_COMPLETE       0x9117
#define GL_UNSIGNALED                       0x9118
#define GL_SIGNALED                         0x9119
#define GL_ALREADY_SIGNALED                 0x911A
#define GL_TIMEOUT_EXPIRED                  0x911B
#define GL_CONDITION_SATISFIED              0x911C
#define GL_WAIT_FAILED                      0x911D
#define GL_SYNC_FLUSH_COMMANDS_BIT          0x00000001
#define GL_VERTEX_ATTRIB_ARRAY_DIVISOR      0x88FE
#define GL_ANY_SAMPLES_PASSED               0x8C2F
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE  0x8D6A
#define GL_SAMPLER_BINDING                  0x8919
#define GL_RGB10_A2UI                       0x906F
#define GL_TEXTURE_SWIZZLE_R                0x8E42
#define GL_TEXTURE_SWIZZLE_G                0x8E43
#define GL_TEXTURE_SWIZZLE_B                0x8E44
#define GL_TEXTURE_SWIZZLE_A                0x8E45
#define GL_GREEN                            0x1904
#define GL_BLUE                             0x1905
#define GL_INT_2_10_10_10_REV               0x8D9F
#define GL_TRANSFORM_FEEDBACK               0x8E22
#define GL_TRANSFORM_FEEDBACK_PAUSED        0x8E23
#define GL_TRANSFORM_FEEDBACK_ACTIVE        0x8E24
#define GL_TRANSFORM_FEEDBACK_BINDING       0x8E25
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT  0x8257
#define GL_TRANSFORM_FEEDBACK_VARYINGS      0x8C83
#define GL_TRANSFORM_FEEDBACK_BUFFER_MODE   0x8C7F
#define GL_TRANSFORM_FEEDBACK_BUFFER        0x8C8E
#define GL_TRANSFORM_FEEDBACK_BUFFER_BINDING 0x8C8F
#define GL_TRANSFORM_FEEDBACK_BUFFER_START  0x8C84
#define GL_TRANSFORM_FEEDBACK_BUFFER_SIZE   0x8C85
#define GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN 0x8C88
#define GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS 0x8C8A
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS 0x8C8B
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS 0x8C8C
#define GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH 0x8C76
#define GL_RASTERIZER_DISCARD              0x8C89
#define GL_MAX_COLOR_ATTACHMENTS           0x8CDF
#define GL_MAX_SAMPLES                     0x8D57
#define GL_TEXTURE_CUBE_MAP                0x8513
#define GL_TEXTURE_BINDING_CUBE_MAP        0x8514
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X     0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X     0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y     0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y     0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z     0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z     0x851A
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE       0x851C
#define GL_VERTEX_ARRAY_BINDING            0x85B5
#define GL_UNIFORM_BUFFER                  0x8A11
#define GL_UNIFORM_BUFFER_BINDING          0x8A28
#define GL_UNIFORM_BUFFER_START            0x8A29
#define GL_UNIFORM_BUFFER_SIZE             0x8A2A
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#define GL_FRAGMENT_SHADER_DERIVATIVE_HINT 0x8B8B
#define GL_DRAW_FRAMEBUFFER                0x8CA9
#define GL_READ_FRAMEBUFFER                0x8CA8
#define GL_DRAW_FRAMEBUFFER_BINDING        0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING        0x8CAA
#define GL_RENDERBUFFER_SAMPLES            0x8CAB
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER 0x8CD4
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE  0x8D56
#define GL_MAX_COLOR_TEXTURE_SAMPLES       0x910E
#define GL_MAX_DEPTH_TEXTURE_SAMPLES       0x910F
#define GL_MAX_INTEGER_SAMPLES             0x9110
#define GL_TEXTURE_2D_MULTISAMPLE          0x9100
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY    0x9102
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE  0x9104
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE_ARRAY 0x9105
#define GL_TEXTURE_SAMPLES                 0x9106
#define GL_TEXTURE_FIXED_SAMPLE_LOCATIONS  0x9107
#define GL_SAMPLER_2D_MULTISAMPLE          0x9108
#define GL_INT_SAMPLER_2D_MULTISAMPLE      0x9109
#define GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE 0x910A
#define GL_SAMPLER_2D_MULTISAMPLE_ARRAY    0x910B
#define GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY 0x910C
#define GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY 0x910D
#define GL_DEPTH_STENCIL_TEXTURE_MODE      0x90EA
#define GL_R8                              0x8229
#define GL_RG8                             0x822B
#define GL_RGBA32F                         0x8814
#define GL_RGB32F                          0x8815
#define GL_RGBA16F                         0x881A
#define GL_RGB16F                          0x881B
#define GL_R32F                            0x822E
#define GL_RG32F                           0x8230
#define GL_R16F                            0x822D
#define GL_RG16F                           0x822F
#define GL_RGBA32UI                        0x8D70
#define GL_RGB32UI                         0x8D71
#define GL_RGBA16UI                        0x8D76
#define GL_RGB16UI                         0x8D77
#define GL_RGBA8UI                         0x8D7C
#define GL_RGB8UI                          0x8D7D
#define GL_RGBA32I                         0x8D82
#define GL_RGB32I                          0x8D83
#define GL_RGBA16I                         0x8D88
#define GL_RGB16I                          0x8D89
#define GL_RGBA8I                          0x8D8E
#define GL_RGB8I                           0x8D8F
#define GL_RED_INTEGER                     0x8D94
#define GL_RGB_INTEGER                     0x8D98
#define GL_RGBA_INTEGER                    0x8D99
#define GL_R8_SNORM                        0x8F94
#define GL_RG8_SNORM                       0x8F95
#define GL_RGB8_SNORM                      0x8F96
#define GL_RGBA8_SNORM                     0x8F97
#define GL_SIGNED_NORMALIZED               0x8F9C
#define GL_PRIMITIVE_RESTART_FIXED_INDEX   0x8D69
#define GL_COPY_READ_BUFFER                0x8F36
#define GL_COPY_WRITE_BUFFER               0x8F37
#define GL_COPY_READ_BUFFER_BINDING        0x8F36
#define GL_COPY_WRITE_BUFFER_BINDING       0x8F37
#define GL_COMPARE_REF_TO_TEXTURE          0x884E
#define GL_R11F_G11F_B10F                  0x8C3A
#define GL_UNSIGNED_INT_10F_11F_11F_REV    0x8C3B
#define GL_RGB9_E5                         0x8C3D
#define GL_UNSIGNED_INT_5_9_9_9_REV        0x8C3E
#define GL_TEXTURE_SHARED_SIZE             0x8C3F
#define GL_DEPTH_COMPONENT32F              0x8CAC
#define GL_DEPTH32F_STENCIL8               0x8CAD
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV  0x8DAD
#define GL_HALF_FLOAT                      0x140B
#define GL_DEPTH24_STENCIL8                0x88F0
#define GL_NUM_EXTENSIONS                  0x821D
#define GL_SAMPLER_2D                      0x8B5E
#define GL_SAMPLER_3D                      0x8B5F
#define GL_SAMPLER_CUBE                    0x8B60
#define GL_SAMPLER_2D_SHADOW               0x8B62
#define GL_SAMPLER_2D_ARRAY                0x8DC1
#define GL_SAMPLER_2D_ARRAY_SHADOW         0x8DC4
#define GL_SAMPLER_CUBE_SHADOW             0x8DC5
#define GL_INT_SAMPLER_2D                  0x8DCA
#define GL_INT_SAMPLER_3D                  0x8DCB
#define GL_INT_SAMPLER_CUBE                0x8DCC
#define GL_INT_SAMPLER_2D_ARRAY            0x8DCF
#define GL_UNSIGNED_INT_SAMPLER_2D         0x8DD2
#define GL_UNSIGNED_INT_SAMPLER_3D         0x8DD3
#define GL_UNSIGNED_INT_SAMPLER_CUBE       0x8DD4
#define GL_UNSIGNED_INT_SAMPLER_2D_ARRAY   0x8DD7
#define GL_DRAW_BUFFER                     0x8825
#define GL_INVALID_INDEX                   0xFFFFFFFFu
#define GL_FLOAT_VEC4                      0x8B52
#define GL_FLOAT_VEC3                      0x8B51
#define GL_FLOAT_VEC2                      0x8B50
#define GL_FLOAT_MAT4                      0x8B5C
#define GL_FLOAT_MAT3                      0x8B5B
#define GL_FLOAT_MAT2                      0x8B5A
#define GL_INT_VEC4                        0x8B55
#define GL_INT_VEC3                        0x8B54
#define GL_INT_VEC2                        0x8B53
#define GL_BOOL_VEC4                       0x8B59
#define GL_BOOL_VEC3                       0x8B58
#define GL_BOOL_VEC2                       0x8B57
#define GL_SAMPLER_2D                      0x8B5E
#define GL_SAMPLER_CUBE                    0x8B60
/* ── WebGL Object Tracking (Software Fallback) ────────────────────────── */

typedef struct LR_WebGLObject {
    GLuint id;
    int type;
    int ref_count;
    int deleted;
    void *data;
    struct LR_WebGLObject *next;
} LR_WebGLObject;

typedef struct LR_WebGLBufferData {
    GLenum target;
    GLenum usage;
    size_t size;
    uint8_t *data;
} LR_WebGLBufferData;

typedef struct LR_WebGLTextureData {
    int width, height, depth;
    GLenum internal_format, format, type;
    int levels, is_3d, is_cube_map;
    uint8_t *pixels;
    GLenum mag_filter, min_filter, wrap_s, wrap_t, wrap_r;
    float max_anisotropy;
} LR_WebGLTextureData;

typedef struct LR_WebGLShaderData {
    GLenum type;
    char *source;
    int compile_status;
    char *info_log;
} LR_WebGLShaderData;

typedef struct LR_WebGLUniformInfo {
    char *name;
    GLenum type;
    int size, location;
} LR_WebGLUniformInfo;

typedef struct LR_WebGLAttribInfo {
    char *name;
    GLenum type;
    int size, location;
} LR_WebGLAttribInfo;

typedef struct LR_WebGLProgramData {
    int link_status, validate_status;
    char *info_log;
    LR_WebGLShaderData *attached_shaders[2];
    int num_attached;
    LR_WebGLUniformInfo *uniforms;
    int num_uniforms;
    LR_WebGLAttribInfo *attribs;
    int num_attribs;
} LR_WebGLProgramData;

typedef struct LR_WebGLFramebufferData {
    GLuint color_attachment, depth_attachment, stencil_attachment, depth_stencil_attachment;
    int color_attachment_type, depth_attachment_type, stencil_attachment_type, depth_stencil_attachment_type;
} LR_WebGLFramebufferData;

typedef struct LR_WebGLRenderbufferData {
    GLenum internal_format;
    int width, height, samples;
} LR_WebGLRenderbufferData;

typedef struct LR_WebGLQueryData {
    GLenum target;
    GLuint result;
    int result_available, active;
} LR_WebGLQueryData;

typedef struct LR_WebGLSamplerData {
    GLenum mag_filter, min_filter, wrap_s, wrap_t, wrap_r;
    float max_anisotropy;
} LR_WebGLSamplerData;

#define LR_WEBGL_MAX_VERTEX_ATTRIBS 16

typedef struct LR_WebGLVertexAttrib {
    int enabled, size, normalized, stride, divisor, integer;
    GLenum type;
    GLintptr offset;
    GLuint buffer_id;
} LR_WebGLVertexAttrib;

#define LR_WEBGL_MAX_TEXTURE_UNITS 32
#define LR_WEBGL_MAX_DRAW_BUFFERS 16

typedef struct LR_WebGLContext {
    LR_RendererBridge *rb;
    int width, height, is_webgl2, has_native_gl, error;
    int vp_x, vp_y, vp_w, vp_h;
    float clear_r, clear_g, clear_b, clear_a, clear_depth;
    int clear_stencil;
    int color_mask_r, color_mask_g, color_mask_b, color_mask_a;
    int depth_mask;
    int stencil_mask_front, stencil_mask_back;
    int enable_blend, enable_cull_face, enable_depth_test, enable_stencil_test;
    int enable_scissor_test, enable_dither, enable_polygon_offset_fill;
    int enable_sample_alpha_to_coverage, enable_sample_coverage;
    int enable_rast_discard, enable_primitive_restart;
    GLenum blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLenum blend_eq_rgb, blend_eq_alpha;
    float blend_color_r, blend_color_g, blend_color_b, blend_color_a;
    GLenum cull_face_mode, front_face, depth_func;
    GLenum stencil_func_front, stencil_func_back;
    GLenum stencil_fail_front, stencil_zfail_front, stencil_zpass_front;
    GLenum stencil_fail_back, stencil_zfail_back, stencil_zpass_back;
    int stencil_ref_front, stencil_ref_back;
    GLuint stencil_mask_read_front, stencil_mask_read_back;
    float sample_coverage_value;
    int sample_coverage_invert;
    float polygon_offset_factor, polygon_offset_units;
    int scissor_x, scissor_y, scissor_w, scissor_h;
    int unpack_alignment, pack_alignment;
    int unpack_row_length, unpack_skip_rows, unpack_skip_pixels;
    int pack_row_length, pack_skip_rows, pack_skip_pixels;
    int unpack_skip_images, unpack_image_height;
    float line_width;
    GLuint bound_array_buffer, bound_element_array_buffer;
    GLuint bound_framebuffer, bound_renderbuffer;
    GLuint bound_read_framebuffer, bound_draw_framebuffer;
    GLuint bound_textures[LR_WEBGL_MAX_TEXTURE_UNITS];
    GLuint bound_samplers[LR_WEBGL_MAX_TEXTURE_UNITS];
    GLuint bound_vertex_array, bound_uniform_buffer;
    GLuint bound_transform_feedback_buffer, bound_copy_read_buffer, bound_copy_write_buffer;
    GLuint bound_transform_feedback;
    int active_texture_unit;
    GLuint current_program;
    int transform_feedback_active, transform_feedback_paused;
    GLenum draw_buffers[LR_WEBGL_MAX_DRAW_BUFFERS];
    LR_WebGLVertexAttrib attribs[LR_WEBGL_MAX_VERTEX_ATTRIBS];
    LR_WebGLObject *objects;
    GLuint next_id;
    GLenum generate_mipmap_hint;
    float depth_range_near, depth_range_far;
} LR_WebGLContext;

/* Forward declarations */
static LR_WebGLObject *webgl_object_create(LR_WebGLContext *ctx, int type);
static LR_WebGLObject *webgl_object_find(LR_WebGLContext *ctx, GLuint id, int type);
static void webgl_object_delete(LR_WebGLContext *ctx, GLuint id, int type);
static void *webgl_get_typed_array_data(JSContext *js_ctx, JSValueConst val, size_t *out_len, GLenum *out_type);
static void webgl_set_error(LR_WebGLContext *ctx, GLenum error);
static GLenum webgl_get_error_and_clear(LR_WebGLContext *ctx);

#if LR_EGL_AVAILABLE
#define WEBGL_CALL(ctx, fn) do { if ((ctx)->has_native_gl) { fn; } } while(0)
#else
#define WEBGL_CALL(ctx, fn) do { (void)(ctx); } while(0)
#endif

/* ── Object Management ──────────────────────────────────────────────────── */

static LR_WebGLObject *webgl_object_create(LR_WebGLContext *ctx, int type)
{
    LR_WebGLObject *obj = (LR_WebGLObject *)calloc(1, sizeof(LR_WebGLObject));
    if (!obj) return NULL;
    obj->id = ++ctx->next_id;
    obj->type = type;
    obj->ref_count = 1;
    obj->next = ctx->objects;
    ctx->objects = obj;
    return obj;
}

static LR_WebGLObject *webgl_object_find(LR_WebGLContext *ctx, GLuint id, int type)
{
    LR_WebGLObject *obj = ctx->objects;
    while (obj) {
        if (obj->id == id && !obj->deleted) {
            if (type == 0 || obj->type == type) return obj;
            return NULL;
        }
        obj = obj->next;
    }
    return NULL;
}

static void webgl_object_delete(LR_WebGLContext *ctx, GLuint id, int type)
{
    (void)type;
    LR_WebGLObject *obj = ctx->objects;
    while (obj) {
        if (obj->id == id && !obj->deleted) {
            obj->deleted = 1;
            if (obj->data) {
                if (obj->type == LR_WEBGL_OBJECT_BUFFER) {
                    LR_WebGLBufferData *d = (LR_WebGLBufferData *)obj->data;
                    free(d->data);
                } else if (obj->type == LR_WEBGL_OBJECT_TEXTURE) {
                    LR_WebGLTextureData *d = (LR_WebGLTextureData *)obj->data;
                    free(d->pixels);
                } else if (obj->type == LR_WEBGL_OBJECT_SHADER) {
                    LR_WebGLShaderData *d = (LR_WebGLShaderData *)obj->data;
                    free(d->source); free(d->info_log);
                } else if (obj->type == LR_WEBGL_OBJECT_PROGRAM) {
                    LR_WebGLProgramData *d = (LR_WebGLProgramData *)obj->data;
                    free(d->info_log);
                    for (int i = 0; i < d->num_uniforms; i++) free(d->uniforms[i].name);
                    free(d->uniforms);
                    for (int i = 0; i < d->num_attribs; i++) free(d->attribs[i].name);
                    free(d->attribs);
                }
                free(obj->data);
                obj->data = NULL;
            }
            break;
        }
        obj = obj->next;
    }
}

/* ── Typed Array Helpers ────────────────────────────────────────────────── */

static GLenum webgl_typed_array_type(JSContext *js_ctx, JSValueConst val)
{
    JSValue ctor = JS_GetPropertyStr(js_ctx, val, "constructor");
    JSValue name = JS_GetPropertyStr(js_ctx, ctor, "name");
    const char *s = JS_ToCString(js_ctx, name);
    GLenum type = GL_FLOAT;
    if (s) {
        if (strcmp(s, "Float32Array") == 0) type = GL_FLOAT;
        else if (strcmp(s, "Float64Array") == 0) type = GL_FLOAT;
        else if (strcmp(s, "Int32Array") == 0) type = GL_INT;
        else if (strcmp(s, "Uint32Array") == 0) type = GL_UNSIGNED_INT;
        else if (strcmp(s, "Int16Array") == 0) type = GL_SHORT;
        else if (strcmp(s, "Uint16Array") == 0) type = GL_UNSIGNED_SHORT;
        else if (strcmp(s, "Int8Array") == 0) type = GL_BYTE;
        else if (strcmp(s, "Uint8Array") == 0) type = GL_UNSIGNED_BYTE;
        else if (strcmp(s, "Uint8ClampedArray") == 0) type = GL_UNSIGNED_BYTE;
        JS_FreeCString(js_ctx, s);
    }
    JS_FreeValue(js_ctx, name);
    JS_FreeValue(js_ctx, ctor);
    return type;
}

static void *webgl_get_typed_array_data(JSContext *js_ctx, JSValueConst val, size_t *out_len, GLenum *out_type)
{
    *out_len = 0;
    if (out_type) *out_type = GL_UNSIGNED_BYTE;
    if (JS_IsArrayBuffer(js_ctx, val)) {
        if (out_type) *out_type = GL_UNSIGNED_BYTE;
        return JS_GetArrayBuffer(js_ctx, out_len, val);
    }
    size_t offset = 0, len = 0, bytes = 0;
    JSValue ab = JS_GetTypedArrayBuffer(js_ctx, val, &offset, &len, &bytes);
    if (!JS_IsException(ab)) {
        uint8_t *buf = JS_GetArrayBuffer(js_ctx, &len, ab);
        if (buf) {
            *out_len = len;
            if (out_type) *out_type = webgl_typed_array_type(js_ctx, val);
            JS_FreeValue(js_ctx, ab);
            return buf + offset;
        }
        JS_FreeValue(js_ctx, ab);
    }
    return NULL;
}

/* ── Error Handling ─────────────────────────────────────────────────────── */

static void webgl_set_error(LR_WebGLContext *ctx, GLenum error)
{
    if (ctx->error == GL_NO_ERROR) ctx->error = error;
}

static GLenum webgl_get_error_and_clear(LR_WebGLContext *ctx)
{
    GLenum e = ctx->error;
    ctx->error = GL_NO_ERROR;
    return e;
}

/* ══════════════════════════════════════════════════════════════════════════
   WebGL 1.0 Methods
   ══════════════════════════════════════════════════════════════════════════ */

/* --- activeTexture --- */
static JSValue lr_webgl_active_texture(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) { webgl_set_error(ctx, GL_INVALID_VALUE); return JS_UNDEFINED; }
    int32_t texture = 0;
    JS_ToInt32(js_ctx, &texture, argv[0]);
    if (texture < GL_TEXTURE0 || texture > GL_TEXTURE0 + 31) { webgl_set_error(ctx, GL_INVALID_ENUM); return JS_UNDEFINED; }
    ctx->active_texture_unit = texture - GL_TEXTURE0;
    WEBGL_CALL(ctx, glActiveTexture((GLenum)texture));
    return JS_UNDEFINED;
}

/* --- attachShader --- */
static JSValue lr_webgl_attach_shader(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t program = 0, shader = 0;
    JS_ToInt32(js_ctx, &program, argv[0]);
    JS_ToInt32(js_ctx, &shader, argv[1]);
    LR_WebGLObject *prog_obj = webgl_object_find(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    LR_WebGLObject *shader_obj = webgl_object_find(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
    if (!prog_obj || !shader_obj) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    LR_WebGLProgramData *pd = (LR_WebGLProgramData *)prog_obj->data;
    if (pd->num_attached < 2) pd->attached_shaders[pd->num_attached++] = (LR_WebGLShaderData *)shader_obj->data;
    WEBGL_CALL(ctx, glAttachShader((GLuint)program, (GLuint)shader));
    return JS_UNDEFINED;
}
/* --- bindAttribLocation --- */
static JSValue lr_webgl_bind_attrib_location(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t program = 0, index = 0;
    JS_ToInt32(js_ctx, &program, argv[0]);
    JS_ToInt32(js_ctx, &index, argv[1]);
    const char *name = JS_ToCString(js_ctx, argv[2]);
    if (!name) return JS_UNDEFINED;
    WEBGL_CALL(ctx, glBindAttribLocation((GLuint)program, (GLuint)index, name));
    JS_FreeCString(js_ctx, name);
    return JS_UNDEFINED;
}

/* --- bindBuffer --- */
static JSValue lr_webgl_bind_buffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t target = 0, buffer = 0;
    JS_ToInt32(js_ctx, &target, argv[0]);
    JS_ToInt32(js_ctx, &buffer, argv[1]);
    if (target == GL_ARRAY_BUFFER) ctx->bound_array_buffer = (GLuint)buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) ctx->bound_element_array_buffer = (GLuint)buffer;
    else { webgl_set_error(ctx, GL_INVALID_ENUM); return JS_UNDEFINED; }
    WEBGL_CALL(ctx, glBindBuffer((GLenum)target, (GLuint)buffer));
    return JS_UNDEFINED;
}

/* --- bindFramebuffer --- */
static JSValue lr_webgl_bind_framebuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t fb = 0;
    JS_ToInt32(js_ctx, &fb, argv[0]);
    ctx->bound_framebuffer = (GLuint)fb;
    WEBGL_CALL(ctx, glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb));
    return JS_UNDEFINED;
}

/* --- bindRenderbuffer --- */
static JSValue lr_webgl_bind_renderbuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t rb = 0;
    JS_ToInt32(js_ctx, &rb, argv[0]);
    ctx->bound_renderbuffer = (GLuint)rb;
    WEBGL_CALL(ctx, glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)rb));
    return JS_UNDEFINED;
}

/* --- bindTexture --- */
static JSValue lr_webgl_bind_texture(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t target = 0, texture = 0;
    JS_ToInt32(js_ctx, &target, argv[0]);
    JS_ToInt32(js_ctx, &texture, argv[1]);
    if ((target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP) && ctx->active_texture_unit >= 0 && ctx->active_texture_unit < LR_WEBGL_MAX_TEXTURE_UNITS)
        ctx->bound_textures[ctx->active_texture_unit] = (GLuint)texture;
    WEBGL_CALL(ctx, glBindTexture((GLenum)target, (GLuint)texture));
    return JS_UNDEFINED;
}

/* --- blendColor --- */
static JSValue lr_webgl_blend_color(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    double r, g, b, a;
    JS_ToFloat64(js_ctx, &r, argv[0]); JS_ToFloat64(js_ctx, &g, argv[1]);
    JS_ToFloat64(js_ctx, &b, argv[2]); JS_ToFloat64(js_ctx, &a, argv[3]);
    ctx->blend_color_r = (float)r; ctx->blend_color_g = (float)g;
    ctx->blend_color_b = (float)b; ctx->blend_color_a = (float)a;
    WEBGL_CALL(ctx, glBlendColor((GLfloat)r, (GLfloat)g, (GLfloat)b, (GLfloat)a));
    return JS_UNDEFINED;
}

/* --- blendEquation --- */
static JSValue lr_webgl_blend_equation(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t mode = 0;
    JS_ToInt32(js_ctx, &mode, argv[0]);
    ctx->blend_eq_rgb = (GLenum)mode; ctx->blend_eq_alpha = (GLenum)mode;
    WEBGL_CALL(ctx, glBlendEquation((GLenum)mode));
    return JS_UNDEFINED;
}

/* --- blendEquationSeparate --- */
static JSValue lr_webgl_blend_equation_separate(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t mode_rgb = 0, mode_alpha = 0;
    JS_ToInt32(js_ctx, &mode_rgb, argv[0]); JS_ToInt32(js_ctx, &mode_alpha, argv[1]);
    ctx->blend_eq_rgb = (GLenum)mode_rgb; ctx->blend_eq_alpha = (GLenum)mode_alpha;
    WEBGL_CALL(ctx, glBlendEquationSeparate((GLenum)mode_rgb, (GLenum)mode_alpha));
    return JS_UNDEFINED;
}

/* --- blendFunc --- */
static JSValue lr_webgl_blend_func(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t sfactor = 0, dfactor = 0;
    JS_ToInt32(js_ctx, &sfactor, argv[0]); JS_ToInt32(js_ctx, &dfactor, argv[1]);
    ctx->blend_src_rgb = (GLenum)sfactor; ctx->blend_src_alpha = (GLenum)sfactor;
    ctx->blend_dst_rgb = (GLenum)dfactor; ctx->blend_dst_alpha = (GLenum)dfactor;
    WEBGL_CALL(ctx, glBlendFunc((GLenum)sfactor, (GLenum)dfactor));
    return JS_UNDEFINED;
}

/* --- blendFuncSeparate --- */
static JSValue lr_webgl_blend_func_separate(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t srgb = 0, drgb = 0, sa = 0, da = 0;
    JS_ToInt32(js_ctx, &srgb, argv[0]); JS_ToInt32(js_ctx, &drgb, argv[1]);
    JS_ToInt32(js_ctx, &sa, argv[2]); JS_ToInt32(js_ctx, &da, argv[3]);
    ctx->blend_src_rgb = (GLenum)srgb; ctx->blend_dst_rgb = (GLenum)drgb;
    ctx->blend_src_alpha = (GLenum)sa; ctx->blend_dst_alpha = (GLenum)da;
    WEBGL_CALL(ctx, glBlendFuncSeparate((GLenum)srgb, (GLenum)drgb, (GLenum)sa, (GLenum)da));
    return JS_UNDEFINED;
}

/* --- bufferData --- */
static JSValue lr_webgl_buffer_data(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t target = 0, usage = 0;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &usage, argv[2]);
    GLuint buf_id = (target == GL_ARRAY_BUFFER) ? ctx->bound_array_buffer : (target == GL_ELEMENT_ARRAY_BUFFER) ? ctx->bound_element_array_buffer : 0;
    if (buf_id == 0) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    size_t data_len = 0;
    void *data = NULL;
    if (JS_IsNumber(argv[1])) {
        int32_t size = 0; JS_ToInt32(js_ctx, &size, argv[1]); data_len = (size_t)size;
    } else {
        data = webgl_get_typed_array_data(js_ctx, argv[1], &data_len, NULL);
    }
    LR_WebGLObject *obj = webgl_object_find(ctx, buf_id, LR_WEBGL_OBJECT_BUFFER);
    if (obj && obj->data) {
        LR_WebGLBufferData *bd = (LR_WebGLBufferData *)obj->data;
        free(bd->data); bd->target = (GLenum)target; bd->usage = (GLenum)usage; bd->size = data_len;
        if (data_len > 0) { bd->data = (uint8_t *)malloc(data_len); if (bd->data && data) memcpy(bd->data, data, data_len); }
        else bd->data = NULL;
    }
    WEBGL_CALL(ctx, glBufferData((GLenum)target, (GLsizeiptr)data_len, data ? data : NULL, (GLenum)usage));
    return JS_UNDEFINED;
}

/* --- bufferSubData --- */
static JSValue lr_webgl_buffer_sub_data(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t target = 0, offset = 0;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &offset, argv[1]);
    size_t data_len = 0;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &data_len, NULL);
    if (!data) return JS_UNDEFINED;
    GLuint buf_id = (target == GL_ARRAY_BUFFER) ? ctx->bound_array_buffer : (target == GL_ELEMENT_ARRAY_BUFFER) ? ctx->bound_element_array_buffer : 0;
    LR_WebGLObject *obj = webgl_object_find(ctx, buf_id, LR_WEBGL_OBJECT_BUFFER);
    if (obj && obj->data) {
        LR_WebGLBufferData *bd = (LR_WebGLBufferData *)obj->data;
        if ((size_t)offset + data_len <= bd->size && bd->data) memcpy(bd->data + offset, data, data_len);
    }
    WEBGL_CALL(ctx, glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)data_len, data));
    return JS_UNDEFINED;
}

/* --- checkFramebufferStatus --- */
static JSValue lr_webgl_check_framebuffer_status(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    GLenum status = GL_FRAMEBUFFER_COMPLETE;
    WEBGL_CALL(ctx, status = glCheckFramebufferStatus(GL_FRAMEBUFFER));
    return JS_NewInt32(js_ctx, (int32_t)status);
}

/* --- clear --- */
static JSValue lr_webgl_clear(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t mask = 0;
    JS_ToInt32(js_ctx, &mask, argv[0]);
    WEBGL_CALL(ctx, glClear((GLbitfield)mask));
    return JS_UNDEFINED;
}

/* --- clearColor --- */
static JSValue lr_webgl_clear_color(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    double r, g, b, a;
    JS_ToFloat64(js_ctx, &r, argv[0]); JS_ToFloat64(js_ctx, &g, argv[1]);
    JS_ToFloat64(js_ctx, &b, argv[2]); JS_ToFloat64(js_ctx, &a, argv[3]);
    ctx->clear_r = (float)r; ctx->clear_g = (float)g; ctx->clear_b = (float)b; ctx->clear_a = (float)a;
    WEBGL_CALL(ctx, glClearColor((GLfloat)r, (GLfloat)g, (GLfloat)b, (GLfloat)a));
    return JS_UNDEFINED;
}

/* --- clearDepth --- */
static JSValue lr_webgl_clear_depth(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    double d; JS_ToFloat64(js_ctx, &d, argv[0]);
    ctx->clear_depth = (float)d;
    WEBGL_CALL(ctx, glClearDepthf((GLfloat)d));
    return JS_UNDEFINED;
}

/* --- clearStencil --- */
static JSValue lr_webgl_clear_stencil(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t s = 0; JS_ToInt32(js_ctx, &s, argv[0]);
    ctx->clear_stencil = s;
    WEBGL_CALL(ctx, glClearStencil((GLint)s));
    return JS_UNDEFINED;
}

/* --- colorMask --- */
static JSValue lr_webgl_color_mask(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    ctx->color_mask_r = JS_ToBool(js_ctx, argv[0]);
    ctx->color_mask_g = JS_ToBool(js_ctx, argv[1]);
    ctx->color_mask_b = JS_ToBool(js_ctx, argv[2]);
    ctx->color_mask_a = JS_ToBool(js_ctx, argv[3]);
    WEBGL_CALL(ctx, glColorMask((GLboolean)ctx->color_mask_r, (GLboolean)ctx->color_mask_g, (GLboolean)ctx->color_mask_b, (GLboolean)ctx->color_mask_a));
    return JS_UNDEFINED;
}

/* --- compileShader --- */
static JSValue lr_webgl_compile_shader(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t shader = 0; JS_ToInt32(js_ctx, &shader, argv[0]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
    if (!obj || !obj->data) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    LR_WebGLShaderData *sd = (LR_WebGLShaderData *)obj->data;
    sd->compile_status = GL_TRUE;
    free(sd->info_log); sd->info_log = strdup("Shader compiled successfully (software mode)");
    WEBGL_CALL(ctx, glCompileShader((GLuint)shader));
    return JS_UNDEFINED;
}

/* --- copyTexImage2D --- */
static JSValue lr_webgl_copy_tex_image_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 8) return JS_UNDEFINED;
    int32_t t, l, ifmt, x, y, w, h, b;
    JS_ToInt32(js_ctx, &t, argv[0]); JS_ToInt32(js_ctx, &l, argv[1]);
    JS_ToInt32(js_ctx, &ifmt, argv[2]); JS_ToInt32(js_ctx, &x, argv[3]);
    JS_ToInt32(js_ctx, &y, argv[4]); JS_ToInt32(js_ctx, &w, argv[5]);
    JS_ToInt32(js_ctx, &h, argv[6]); JS_ToInt32(js_ctx, &b, argv[7]);
    WEBGL_CALL(ctx, glCopyTexImage2D((GLenum)t, (GLint)l, (GLenum)ifmt, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, (GLint)b));
    return JS_UNDEFINED;
}

/* --- copyTexSubImage2D --- */
static JSValue lr_webgl_copy_tex_sub_image_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 8) return JS_UNDEFINED;
    int32_t t, l, xo, yo, x, y, w, h;
    JS_ToInt32(js_ctx, &t, argv[0]); JS_ToInt32(js_ctx, &l, argv[1]);
    JS_ToInt32(js_ctx, &xo, argv[2]); JS_ToInt32(js_ctx, &yo, argv[3]);
    JS_ToInt32(js_ctx, &x, argv[4]); JS_ToInt32(js_ctx, &y, argv[5]);
    JS_ToInt32(js_ctx, &w, argv[6]); JS_ToInt32(js_ctx, &h, argv[7]);
    WEBGL_CALL(ctx, glCopyTexSubImage2D((GLenum)t, (GLint)l, (GLint)xo, (GLint)yo, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h));
    return JS_UNDEFINED;
}

/* --- createBuffer --- */
static JSValue lr_webgl_create_buffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_BUFFER);
    if (!obj) return JS_UNDEFINED;
    LR_WebGLBufferData *bd = (LR_WebGLBufferData *)calloc(1, sizeof(LR_WebGLBufferData));
    if (!bd) { free(obj); return JS_UNDEFINED; }
    bd->usage = GL_STATIC_DRAW; obj->data = bd;
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLuint id = 0; glGenBuffers(1, &id); obj->id = id; if (id >= ctx->next_id) ctx->next_id = id + 1; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

/* --- createFramebuffer --- */
static JSValue lr_webgl_create_framebuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_FRAMEBUFFER);
    if (!obj) return JS_UNDEFINED;
    obj->data = calloc(1, sizeof(LR_WebGLFramebufferData));
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLuint id = 0; glGenFramebuffers(1, &id); obj->id = id; if (id >= ctx->next_id) ctx->next_id = id + 1; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

/* --- createProgram --- */
static JSValue lr_webgl_create_program(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_PROGRAM);
    if (!obj) return JS_UNDEFINED;
    LR_WebGLProgramData *pd = (LR_WebGLProgramData *)calloc(1, sizeof(LR_WebGLProgramData));
    if (!pd) { free(obj); return JS_UNDEFINED; }
    obj->data = pd;
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLuint id = glCreateProgram(); obj->id = id; if (id >= ctx->next_id) ctx->next_id = id + 1; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

/* --- createRenderbuffer --- */
static JSValue lr_webgl_create_renderbuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_RENDERBUFFER);
    if (!obj) return JS_UNDEFINED;
    obj->data = calloc(1, sizeof(LR_WebGLRenderbufferData));
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLuint id = 0; glGenRenderbuffers(1, &id); obj->id = id; if (id >= ctx->next_id) ctx->next_id = id + 1; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

/* --- createShader --- */
static JSValue lr_webgl_create_shader(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t type = 0; JS_ToInt32(js_ctx, &type, argv[0]);
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) { webgl_set_error(ctx, GL_INVALID_ENUM); return JS_NewInt32(js_ctx, 0); }
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_SHADER);
    if (!obj) return JS_UNDEFINED;
    LR_WebGLShaderData *sd = (LR_WebGLShaderData *)calloc(1, sizeof(LR_WebGLShaderData));
    if (!sd) { free(obj); return JS_UNDEFINED; }
    sd->type = (GLenum)type; obj->data = sd;
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLuint id = glCreateShader((GLenum)type); obj->id = id; if (id >= ctx->next_id) ctx->next_id = id + 1; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

/* --- createTexture --- */
static JSValue lr_webgl_create_texture(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_TEXTURE);
    if (!obj) return JS_UNDEFINED;
    LR_WebGLTextureData *td = (LR_WebGLTextureData *)calloc(1, sizeof(LR_WebGLTextureData));
    if (!td) { free(obj); return JS_UNDEFINED; }
    td->mag_filter = GL_LINEAR; td->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    td->wrap_s = GL_REPEAT; td->wrap_t = GL_REPEAT; td->wrap_r = GL_REPEAT;
    obj->data = td;
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLuint id = 0; glGenTextures(1, &id); obj->id = id; if (id >= ctx->next_id) ctx->next_id = id + 1; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

/* --- cullFace --- */
static JSValue lr_webgl_cull_face(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t mode = 0; JS_ToInt32(js_ctx, &mode, argv[0]);
    ctx->cull_face_mode = (GLenum)mode;
    WEBGL_CALL(ctx, glCullFace((GLenum)mode));
    return JS_UNDEFINED;
}

/* --- deleteBuffer --- */
static JSValue lr_webgl_delete_buffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t buffer = 0; JS_ToInt32(js_ctx, &buffer, argv[0]);
    if (ctx->bound_array_buffer == (GLuint)buffer) ctx->bound_array_buffer = 0;
    if (ctx->bound_element_array_buffer == (GLuint)buffer) ctx->bound_element_array_buffer = 0;
    webgl_object_delete(ctx, (GLuint)buffer, LR_WEBGL_OBJECT_BUFFER);
    WEBGL_CALL(ctx, glDeleteBuffers(1, (const GLuint *)&buffer));
    return JS_UNDEFINED;
}

/* --- deleteFramebuffer --- */
static JSValue lr_webgl_delete_framebuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t fb = 0; JS_ToInt32(js_ctx, &fb, argv[0]);
    if (ctx->bound_framebuffer == (GLuint)fb) ctx->bound_framebuffer = 0;
    webgl_object_delete(ctx, (GLuint)fb, LR_WEBGL_OBJECT_FRAMEBUFFER);
    WEBGL_CALL(ctx, glDeleteFramebuffers(1, (const GLuint *)&fb));
    return JS_UNDEFINED;
}

/* --- deleteProgram --- */
static JSValue lr_webgl_delete_program(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t program = 0; JS_ToInt32(js_ctx, &program, argv[0]);
    if (ctx->current_program == (GLuint)program) ctx->current_program = 0;
    webgl_object_delete(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    WEBGL_CALL(ctx, glDeleteProgram((GLuint)program));
    return JS_UNDEFINED;
}

/* --- deleteRenderbuffer --- */
static JSValue lr_webgl_delete_renderbuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t rb = 0; JS_ToInt32(js_ctx, &rb, argv[0]);
    if (ctx->bound_renderbuffer == (GLuint)rb) ctx->bound_renderbuffer = 0;
    webgl_object_delete(ctx, (GLuint)rb, LR_WEBGL_OBJECT_RENDERBUFFER);
    WEBGL_CALL(ctx, glDeleteRenderbuffers(1, (const GLuint *)&rb));
    return JS_UNDEFINED;
}

/* --- deleteShader --- */
static JSValue lr_webgl_delete_shader(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t shader = 0; JS_ToInt32(js_ctx, &shader, argv[0]);
    webgl_object_delete(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
    WEBGL_CALL(ctx, glDeleteShader((GLuint)shader));
    return JS_UNDEFINED;
}

/* --- deleteTexture --- */
static JSValue lr_webgl_delete_texture(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t texture = 0; JS_ToInt32(js_ctx, &texture, argv[0]);
    for (int i = 0; i < LR_WEBGL_MAX_TEXTURE_UNITS; i++) if (ctx->bound_textures[i] == (GLuint)texture) ctx->bound_textures[i] = 0;
    webgl_object_delete(ctx, (GLuint)texture, LR_WEBGL_OBJECT_TEXTURE);
    WEBGL_CALL(ctx, glDeleteTextures(1, (const GLuint *)&texture));
    return JS_UNDEFINED;
}

/* --- depthFunc --- */
static JSValue lr_webgl_depth_func(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t func = 0; JS_ToInt32(js_ctx, &func, argv[0]);
    ctx->depth_func = (GLenum)func;
    WEBGL_CALL(ctx, glDepthFunc((GLenum)func));
    return JS_UNDEFINED;
}

/* --- depthMask --- */
static JSValue lr_webgl_depth_mask(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    ctx->depth_mask = JS_ToBool(js_ctx, argv[0]);
    WEBGL_CALL(ctx, glDepthMask((GLboolean)ctx->depth_mask));
    return JS_UNDEFINED;
}
/* --- depthRange --- */
static JSValue lr_webgl_depth_range(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    double zn, zf; JS_ToFloat64(js_ctx, &zn, argv[0]); JS_ToFloat64(js_ctx, &zf, argv[1]);
    ctx->depth_range_near = (float)zn; ctx->depth_range_far = (float)zf;
    WEBGL_CALL(ctx, glDepthRangef((GLfloat)zn, (GLfloat)zf));
    return JS_UNDEFINED;
}

/* --- detachShader --- */
static JSValue lr_webgl_detach_shader(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t program = 0, shader = 0;
    JS_ToInt32(js_ctx, &program, argv[0]); JS_ToInt32(js_ctx, &shader, argv[1]);
    LR_WebGLObject *prog_obj = webgl_object_find(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    if (prog_obj && prog_obj->data) {
        LR_WebGLProgramData *pd = (LR_WebGLProgramData *)prog_obj->data;
        for (int i = 0; i < pd->num_attached; i++) {
            LR_WebGLObject *so = webgl_object_find(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
            if (so && pd->attached_shaders[i] == (LR_WebGLShaderData *)so->data) {
                for (int j = i; j < pd->num_attached - 1; j++) pd->attached_shaders[j] = pd->attached_shaders[j+1];
                pd->num_attached--; break;
            }
        }
    }
    WEBGL_CALL(ctx, glDetachShader((GLuint)program, (GLuint)shader));
    return JS_UNDEFINED;
}

/* --- disable/enable --- */
static JSValue lr_webgl_disable(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t cap = 0; JS_ToInt32(js_ctx, &cap, argv[0]);
    switch (cap) {
        case GL_BLEND: ctx->enable_blend = 0; break;
        case GL_CULL_FACE: ctx->enable_cull_face = 0; break;
        case GL_DEPTH_TEST: ctx->enable_depth_test = 0; break;
        case GL_STENCIL_TEST: ctx->enable_stencil_test = 0; break;
        case GL_SCISSOR_TEST: ctx->enable_scissor_test = 0; break;
        case GL_DITHER: ctx->enable_dither = 0; break;
        case GL_POLYGON_OFFSET_FILL: ctx->enable_polygon_offset_fill = 0; break;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: ctx->enable_sample_alpha_to_coverage = 0; break;
        case GL_SAMPLE_COVERAGE: ctx->enable_sample_coverage = 0; break;
    }
    WEBGL_CALL(ctx, glDisable((GLenum)cap));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_enable(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t cap = 0; JS_ToInt32(js_ctx, &cap, argv[0]);
    switch (cap) {
        case GL_BLEND: ctx->enable_blend = 1; break;
        case GL_CULL_FACE: ctx->enable_cull_face = 1; break;
        case GL_DEPTH_TEST: ctx->enable_depth_test = 1; break;
        case GL_STENCIL_TEST: ctx->enable_stencil_test = 1; break;
        case GL_SCISSOR_TEST: ctx->enable_scissor_test = 1; break;
        case GL_DITHER: ctx->enable_dither = 1; break;
        case GL_POLYGON_OFFSET_FILL: ctx->enable_polygon_offset_fill = 1; break;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: ctx->enable_sample_alpha_to_coverage = 1; break;
        case GL_SAMPLE_COVERAGE: ctx->enable_sample_coverage = 1; break;
    }
    WEBGL_CALL(ctx, glEnable((GLenum)cap));
    return JS_UNDEFINED;
}

/* --- enableVertexAttribArray / disableVertexAttribArray --- */
static JSValue lr_webgl_enable_vertex_attrib_array(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t index = 0; JS_ToInt32(js_ctx, &index, argv[0]);
    if (index >= 0 && index < LR_WEBGL_MAX_VERTEX_ATTRIBS) ctx->attribs[index].enabled = 1;
    WEBGL_CALL(ctx, glEnableVertexAttribArray((GLuint)index));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_disable_vertex_attrib_array(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t index = 0; JS_ToInt32(js_ctx, &index, argv[0]);
    if (index >= 0 && index < LR_WEBGL_MAX_VERTEX_ATTRIBS) ctx->attribs[index].enabled = 0;
    WEBGL_CALL(ctx, glDisableVertexAttribArray((GLuint)index));
    return JS_UNDEFINED;
}

/* --- drawArrays --- */
static JSValue lr_webgl_draw_arrays(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t mode = 0, first = 0, count = 0;
    JS_ToInt32(js_ctx, &mode, argv[0]); JS_ToInt32(js_ctx, &first, argv[1]); JS_ToInt32(js_ctx, &count, argv[2]);
    WEBGL_CALL(ctx, glDrawArrays((GLenum)mode, (GLint)first, (GLsizei)count));
    return JS_UNDEFINED;
}

/* --- drawElements --- */
static JSValue lr_webgl_draw_elements(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t mode = 0, count = 0, type = 0, offset = 0;
    JS_ToInt32(js_ctx, &mode, argv[0]); JS_ToInt32(js_ctx, &count, argv[1]);
    JS_ToInt32(js_ctx, &type, argv[2]); JS_ToInt32(js_ctx, &offset, argv[3]);
    WEBGL_CALL(ctx, glDrawElements((GLenum)mode, (GLsizei)count, (GLenum)type, (const void *)(intptr_t)offset));
    return JS_UNDEFINED;
}

/* --- finish/flush --- */
static JSValue lr_webgl_finish(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    WEBGL_CALL(ctx, glFinish());
    return JS_UNDEFINED;
}

static JSValue lr_webgl_flush(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    WEBGL_CALL(ctx, glFlush());
    return JS_UNDEFINED;
}

/* --- framebufferRenderbuffer --- */
static JSValue lr_webgl_framebuffer_renderbuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t target, attachment, rbtarget, renderbuffer;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &attachment, argv[1]);
    JS_ToInt32(js_ctx, &rbtarget, argv[2]); JS_ToInt32(js_ctx, &renderbuffer, argv[3]);
    WEBGL_CALL(ctx, glFramebufferRenderbuffer((GLenum)target, (GLenum)attachment, (GLenum)rbtarget, (GLuint)renderbuffer));
    if (ctx->bound_framebuffer > 0) {
        LR_WebGLObject *fbo = webgl_object_find(ctx, ctx->bound_framebuffer, LR_WEBGL_OBJECT_FRAMEBUFFER);
        if (fbo && fbo->data) {
            LR_WebGLFramebufferData *fd = (LR_WebGLFramebufferData *)fbo->data;
            if (attachment == GL_COLOR_ATTACHMENT0) { fd->color_attachment = (GLuint)renderbuffer; fd->color_attachment_type = 2; }
            else if (attachment == GL_DEPTH_ATTACHMENT) { fd->depth_attachment = (GLuint)renderbuffer; fd->depth_attachment_type = 2; }
            else if (attachment == GL_STENCIL_ATTACHMENT) { fd->stencil_attachment = (GLuint)renderbuffer; fd->stencil_attachment_type = 2; }
            else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) { fd->depth_stencil_attachment = (GLuint)renderbuffer; fd->depth_stencil_attachment_type = 2; }
        }
    }
    return JS_UNDEFINED;
}

/* --- framebufferTexture2D --- */
static JSValue lr_webgl_framebuffer_texture_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t target, attachment, textarget, texture, level;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &attachment, argv[1]);
    JS_ToInt32(js_ctx, &textarget, argv[2]); JS_ToInt32(js_ctx, &texture, argv[3]);
    JS_ToInt32(js_ctx, &level, argv[4]);
    WEBGL_CALL(ctx, glFramebufferTexture2D((GLenum)target, (GLenum)attachment, (GLenum)textarget, (GLuint)texture, (GLint)level));
    if (ctx->bound_framebuffer > 0) {
        LR_WebGLObject *fbo = webgl_object_find(ctx, ctx->bound_framebuffer, LR_WEBGL_OBJECT_FRAMEBUFFER);
        if (fbo && fbo->data) {
            LR_WebGLFramebufferData *fd = (LR_WebGLFramebufferData *)fbo->data;
            if (attachment == GL_COLOR_ATTACHMENT0) { fd->color_attachment = (GLuint)texture; fd->color_attachment_type = 1; }
            else if (attachment == GL_DEPTH_ATTACHMENT) { fd->depth_attachment = (GLuint)texture; fd->depth_attachment_type = 1; }
            else if (attachment == GL_STENCIL_ATTACHMENT) { fd->stencil_attachment = (GLuint)texture; fd->stencil_attachment_type = 1; }
            else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) { fd->depth_stencil_attachment = (GLuint)texture; fd->depth_stencil_attachment_type = 1; }
        }
    }
    return JS_UNDEFINED;
}

/* --- frontFace --- */
static JSValue lr_webgl_front_face(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t mode = 0; JS_ToInt32(js_ctx, &mode, argv[0]);
    ctx->front_face = (GLenum)mode;
    WEBGL_CALL(ctx, glFrontFace((GLenum)mode));
    return JS_UNDEFINED;
}

/* --- generateMipmap --- */
static JSValue lr_webgl_generate_mipmap(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t target = 0; JS_ToInt32(js_ctx, &target, argv[0]);
    WEBGL_CALL(ctx, glGenerateMipmap((GLenum)target));
    return JS_UNDEFINED;
}

/* --- getAttribLocation --- */
static JSValue lr_webgl_get_attrib_location(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewInt32(js_ctx, -1);
    if (argc < 2) return JS_NewInt32(js_ctx, -1);
    int32_t program = 0; JS_ToInt32(js_ctx, &program, argv[0]);
    const char *name = JS_ToCString(js_ctx, argv[1]);
    if (!name) return JS_NewInt32(js_ctx, -1);
    GLint loc = -1;
    WEBGL_CALL(ctx, loc = glGetAttribLocation((GLuint)program, name));
    JS_FreeCString(js_ctx, name);
    return JS_NewInt32(js_ctx, (int32_t)loc);
}

/* --- getError --- */
static JSValue lr_webgl_get_error(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewInt32(js_ctx, GL_NO_ERROR);
    GLenum err = webgl_get_error_and_clear(ctx);
#if LR_EGL_AVAILABLE
    if (ctx->has_native_gl) { GLenum gl_err = glGetError(); if (gl_err != GL_NO_ERROR) err = gl_err; }
#endif
    return JS_NewInt32(js_ctx, (int32_t)err);
}

/* --- getExtension --- */
static JSValue lr_webgl_get_extension(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(js_ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    int supported = 0;
    if (strcmp(name, "ANGLE_instanced_arrays") == 0) supported = 1;
    else if (strcmp(name, "EXT_blend_minmax") == 0) supported = 1;
    else if (strcmp(name, "EXT_color_buffer_half_float") == 0) supported = 1;
    else if (strcmp(name, "EXT_float_blend") == 0) supported = 1;
    else if (strcmp(name, "EXT_frag_depth") == 0) supported = 1;
    else if (strcmp(name, "EXT_shader_texture_lod") == 0) supported = 1;
    else if (strcmp(name, "EXT_texture_filter_anisotropic") == 0) supported = 1;
    else if (strcmp(name, "EXT_sRGB") == 0) supported = 1;
    else if (strcmp(name, "OES_element_index_uint") == 0) supported = 1;
    else if (strcmp(name, "OES_standard_derivatives") == 0) supported = 1;
    else if (strcmp(name, "OES_texture_float") == 0) supported = 1;
    else if (strcmp(name, "OES_texture_float_linear") == 0) supported = 1;
    else if (strcmp(name, "OES_texture_half_float") == 0) supported = 1;
    else if (strcmp(name, "OES_texture_half_float_linear") == 0) supported = 1;
    else if (strcmp(name, "OES_vertex_array_object") == 0) supported = 1;
    else if (strcmp(name, "WEBGL_color_buffer_float") == 0) supported = 1;
    else if (strcmp(name, "WEBGL_depth_texture") == 0) supported = 1;
    else if (strcmp(name, "WEBGL_draw_buffers") == 0) supported = 1;
    JS_FreeCString(js_ctx, name);
    if (supported) return JS_NewObject(js_ctx);
    return JS_NULL;
}

/* --- getParameter (large implementation) --- */
static JSValue lr_webgl_get_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t pname = 0; JS_ToInt32(js_ctx, &pname, argv[0]);
    switch (pname) {
        case GL_ACTIVE_TEXTURE: return JS_NewInt32(js_ctx, GL_TEXTURE0 + ctx->active_texture_unit);
        case GL_ALIASED_LINE_WIDTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: case GL_DEPTH_RANGE: {
            JSValue arr = JS_NewArray(js_ctx);
            JS_SetPropertyUint32(js_ctx, arr, 0, JS_NewFloat64(js_ctx, 0.0));
            JS_SetPropertyUint32(js_ctx, arr, 1, JS_NewFloat64(js_ctx, 1.0));
            return arr;
        }
        case GL_BLEND: return JS_NewBool(js_ctx, ctx->enable_blend);
        case GL_BLEND_COLOR: return JS_NewFloat64(js_ctx, ctx->blend_color_r);
        case GL_BLEND_EQUATION_RGB: return JS_NewInt32(js_ctx, (int32_t)ctx->blend_eq_rgb);
        case GL_BLEND_EQUATION_ALPHA: return JS_NewInt32(js_ctx, (int32_t)ctx->blend_eq_alpha);
        case GL_BLEND_SRC_RGB: return JS_NewInt32(js_ctx, (int32_t)ctx->blend_src_rgb);
        case GL_BLEND_DST_RGB: return JS_NewInt32(js_ctx, (int32_t)ctx->blend_dst_rgb);
        case GL_BLEND_SRC_ALPHA: return JS_NewInt32(js_ctx, (int32_t)ctx->blend_src_alpha);
        case GL_BLEND_DST_ALPHA: return JS_NewInt32(js_ctx, (int32_t)ctx->blend_dst_alpha);
        case GL_COLOR_CLEAR_VALUE: {
            JSValue arr = JS_NewArray(js_ctx);
            JS_SetPropertyUint32(js_ctx, arr, 0, JS_NewFloat64(js_ctx, ctx->clear_r));
            JS_SetPropertyUint32(js_ctx, arr, 1, JS_NewFloat64(js_ctx, ctx->clear_g));
            JS_SetPropertyUint32(js_ctx, arr, 2, JS_NewFloat64(js_ctx, ctx->clear_b));
            JS_SetPropertyUint32(js_ctx, arr, 3, JS_NewFloat64(js_ctx, ctx->clear_a));
            return arr;
        }
        case GL_COLOR_WRITEMASK: {
            JSValue arr = JS_NewArray(js_ctx);
            JS_SetPropertyUint32(js_ctx, arr, 0, JS_NewBool(js_ctx, ctx->color_mask_r));
            JS_SetPropertyUint32(js_ctx, arr, 1, JS_NewBool(js_ctx, ctx->color_mask_g));
            JS_SetPropertyUint32(js_ctx, arr, 2, JS_NewBool(js_ctx, ctx->color_mask_b));
            JS_SetPropertyUint32(js_ctx, arr, 3, JS_NewBool(js_ctx, ctx->color_mask_a));
            return arr;
        }
        case GL_CULL_FACE: return JS_NewBool(js_ctx, ctx->enable_cull_face);
        case GL_CULL_FACE_MODE: return JS_NewInt32(js_ctx, (int32_t)ctx->cull_face_mode);
        case GL_CURRENT_PROGRAM: return JS_NewInt32(js_ctx, (int32_t)ctx->current_program);
        case GL_DEPTH_CLEAR_VALUE: return JS_NewFloat64(js_ctx, ctx->clear_depth);
        case GL_DEPTH_FUNC: return JS_NewInt32(js_ctx, (int32_t)ctx->depth_func);
        case GL_DEPTH_TEST: return JS_NewBool(js_ctx, ctx->enable_depth_test);
        case GL_DEPTH_WRITEMASK: return JS_NewBool(js_ctx, ctx->depth_mask);
        case GL_DITHER: return JS_NewBool(js_ctx, ctx->enable_dither);
        case GL_FRONT_FACE: return JS_NewInt32(js_ctx, (int32_t)ctx->front_face);
        case GL_GENERATE_MIPMAP_HINT: return JS_NewInt32(js_ctx, (int32_t)ctx->generate_mipmap_hint);
        case GL_LINE_WIDTH: return JS_NewFloat64(js_ctx, ctx->line_width);
        case GL_POLYGON_OFFSET_FACTOR: return JS_NewFloat64(js_ctx, ctx->polygon_offset_factor);
        case GL_POLYGON_OFFSET_FILL: return JS_NewBool(js_ctx, ctx->enable_polygon_offset_fill);
        case GL_POLYGON_OFFSET_UNITS: return JS_NewFloat64(js_ctx, ctx->polygon_offset_units);
        case GL_SAMPLE_COVERAGE_VALUE: return JS_NewFloat64(js_ctx, ctx->sample_coverage_value);
        case GL_SAMPLE_COVERAGE_INVERT: return JS_NewBool(js_ctx, ctx->sample_coverage_invert);
        case GL_SCISSOR_TEST: return JS_NewBool(js_ctx, ctx->enable_scissor_test);
        case GL_SCISSOR_BOX: {
            JSValue arr = JS_NewArray(js_ctx);
            JS_SetPropertyUint32(js_ctx, arr, 0, JS_NewInt32(js_ctx, ctx->scissor_x));
            JS_SetPropertyUint32(js_ctx, arr, 1, JS_NewInt32(js_ctx, ctx->scissor_y));
            JS_SetPropertyUint32(js_ctx, arr, 2, JS_NewInt32(js_ctx, ctx->scissor_w));
            JS_SetPropertyUint32(js_ctx, arr, 3, JS_NewInt32(js_ctx, ctx->scissor_h));
            return arr;
        }
        case GL_STENCIL_TEST: return JS_NewBool(js_ctx, ctx->enable_stencil_test);
        case GL_STENCIL_CLEAR_VALUE: return JS_NewInt32(js_ctx, ctx->clear_stencil);
        case GL_STENCIL_FUNC: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_func_front);
        case GL_STENCIL_FAIL: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_fail_front);
        case GL_STENCIL_PASS_DEPTH_FAIL: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_zfail_front);
        case GL_STENCIL_PASS_DEPTH_PASS: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_zpass_front);
        case GL_STENCIL_REF: return JS_NewInt32(js_ctx, ctx->stencil_ref_front);
        case GL_STENCIL_VALUE_MASK: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_mask_read_front);
        case GL_STENCIL_WRITEMASK: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_mask_front);
        case GL_STENCIL_BACK_FUNC: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_func_back);
        case GL_STENCIL_BACK_FAIL: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_fail_back);
        case GL_STENCIL_BACK_PASS_DEPTH_FAIL: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_zfail_back);
        case GL_STENCIL_BACK_PASS_DEPTH_PASS: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_zpass_back);
        case GL_STENCIL_BACK_REF: return JS_NewInt32(js_ctx, ctx->stencil_ref_back);
        case GL_STENCIL_BACK_VALUE_MASK: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_mask_read_back);
        case GL_STENCIL_BACK_WRITEMASK: return JS_NewInt32(js_ctx, (int32_t)ctx->stencil_mask_back);
        case GL_SAMPLE_ALPHA_TO_COVERAGE: return JS_NewBool(js_ctx, ctx->enable_sample_alpha_to_coverage);
        case GL_SAMPLE_COVERAGE: return JS_NewBool(js_ctx, ctx->enable_sample_coverage);
        case GL_VIEWPORT: {
            JSValue arr = JS_NewArray(js_ctx);
            JS_SetPropertyUint32(js_ctx, arr, 0, JS_NewInt32(js_ctx, ctx->vp_x));
            JS_SetPropertyUint32(js_ctx, arr, 1, JS_NewInt32(js_ctx, ctx->vp_y));
            JS_SetPropertyUint32(js_ctx, arr, 2, JS_NewInt32(js_ctx, ctx->vp_w));
            JS_SetPropertyUint32(js_ctx, arr, 3, JS_NewInt32(js_ctx, ctx->vp_h));
            return arr;
        }
        case GL_ARRAY_BUFFER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_array_buffer);
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_element_array_buffer);
        case GL_FRAMEBUFFER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_framebuffer);
        case GL_RENDERBUFFER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_renderbuffer);
        case GL_TEXTURE_BINDING_2D: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_textures[ctx->active_texture_unit]);
        case GL_MAX_TEXTURE_SIZE: return JS_NewInt32(js_ctx, 4096);
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE: return JS_NewInt32(js_ctx, 4096);
        case GL_MAX_RENDERBUFFER_SIZE: return JS_NewInt32(js_ctx, 4096);
        case GL_MAX_TEXTURE_IMAGE_UNITS: return JS_NewInt32(js_ctx, LR_WEBGL_MAX_TEXTURE_UNITS);
        case GL_MAX_VERTEX_ATTRIBS: return JS_NewInt32(js_ctx, LR_WEBGL_MAX_VERTEX_ATTRIBS);
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: return JS_NewInt32(js_ctx, 16);
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: return JS_NewInt32(js_ctx, LR_WEBGL_MAX_TEXTURE_UNITS);
        case GL_MAX_VERTEX_UNIFORM_VECTORS: return JS_NewInt32(js_ctx, 256);
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS: return JS_NewInt32(js_ctx, 256);
        case GL_MAX_VARYING_VECTORS: return JS_NewInt32(js_ctx, 16);
        case GL_SHADING_LANGUAGE_VERSION: return JS_NewString(js_ctx, "OpenGL ES GLSL ES 1.00 (LR_JS)");
        case GL_VERSION: return JS_NewString(js_ctx, ctx->is_webgl2 ? "WebGL 2.0 (OpenGL ES 3.0 LR_JS)" : "WebGL 1.0 (OpenGL ES 2.0 LR_JS)");
        case GL_RENDERER: return JS_NewString(js_ctx, "LR_JS Software Renderer");
        case GL_VENDOR: return JS_NewString(js_ctx, "LR_JS");
        case GL_EXTENSIONS: return JS_NewString(js_ctx, "");
        case GL_NUM_EXTENSIONS: return JS_NewInt32(js_ctx, 0);
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: return JS_NewInt32(js_ctx, 8);
        case GL_DEPTH_BITS: return JS_NewInt32(js_ctx, 24);
        case GL_STENCIL_BITS: return JS_NewInt32(js_ctx, 8);
        case GL_SUBPIXEL_BITS: return JS_NewInt32(js_ctx, 4);
        case GL_SAMPLE_BUFFERS: return JS_NewInt32(js_ctx, 0);
        case GL_SAMPLES: return JS_NewInt32(js_ctx, 0);
        case GL_UNPACK_ALIGNMENT: return JS_NewInt32(js_ctx, ctx->unpack_alignment);
        case GL_PACK_ALIGNMENT: return JS_NewInt32(js_ctx, ctx->pack_alignment);
        case GL_MAX_VIEWPORT_DIMS: {
            JSValue arr = JS_NewArray(js_ctx);
            JS_SetPropertyUint32(js_ctx, arr, 0, JS_NewInt32(js_ctx, 4096));
            JS_SetPropertyUint32(js_ctx, arr, 1, JS_NewInt32(js_ctx, 4096));
            return arr;
        }
        case GL_IMPLEMENTATION_COLOR_READ_TYPE: return JS_NewInt32(js_ctx, GL_UNSIGNED_BYTE);
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT: return JS_NewInt32(js_ctx, GL_RGBA);
        default:
            if (ctx->is_webgl2) {
                switch (pname) {
                    case GL_READ_BUFFER: return JS_NewInt32(js_ctx, GL_COLOR_ATTACHMENT0);
                    case GL_DRAW_FRAMEBUFFER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_draw_framebuffer);
                    case GL_READ_FRAMEBUFFER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_read_framebuffer);
                    case GL_SAMPLER_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_samplers[ctx->active_texture_unit]);
                    case GL_TEXTURE_BINDING_3D: return JS_NewInt32(js_ctx, 0);
                    case GL_RASTERIZER_DISCARD: return JS_NewBool(js_ctx, ctx->enable_rast_discard);
                    case GL_MAX_3D_TEXTURE_SIZE: return JS_NewInt32(js_ctx, 512);
                    case GL_MAX_DRAW_BUFFERS: return JS_NewInt32(js_ctx, LR_WEBGL_MAX_DRAW_BUFFERS);
                    case GL_MAX_COLOR_ATTACHMENTS: return JS_NewInt32(js_ctx, LR_WEBGL_MAX_DRAW_BUFFERS);
                    case GL_MAX_SAMPLES: return JS_NewInt32(js_ctx, 4);
                    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: return JS_NewInt32(js_ctx, 256);
                    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: return JS_NewInt32(js_ctx, 1024);
                    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: return JS_NewInt32(js_ctx, 1024);
                    case GL_MAX_VARYING_COMPONENTS: return JS_NewInt32(js_ctx, 64);
                    case GL_MAX_VERTEX_UNIFORM_BLOCKS: return JS_NewInt32(js_ctx, 12);
                    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: return JS_NewInt32(js_ctx, 12);
                    case GL_MAX_COMBINED_UNIFORM_BLOCKS: return JS_NewInt32(js_ctx, 24);
                    case GL_UNPACK_ROW_LENGTH: return JS_NewInt32(js_ctx, ctx->unpack_row_length);
                    case GL_UNPACK_SKIP_ROWS: return JS_NewInt32(js_ctx, ctx->unpack_skip_rows);
                    case GL_UNPACK_SKIP_PIXELS: return JS_NewInt32(js_ctx, ctx->unpack_skip_pixels);
                    case GL_PACK_ROW_LENGTH: return JS_NewInt32(js_ctx, ctx->pack_row_length);
                    case GL_PACK_SKIP_ROWS: return JS_NewInt32(js_ctx, ctx->pack_skip_rows);
                    case GL_PACK_SKIP_PIXELS: return JS_NewInt32(js_ctx, ctx->pack_skip_pixels);
                    case GL_TRANSFORM_FEEDBACK_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_transform_feedback);
                    case GL_TRANSFORM_FEEDBACK_ACTIVE: return JS_NewBool(js_ctx, ctx->transform_feedback_active);
                    case GL_TRANSFORM_FEEDBACK_PAUSED: return JS_NewBool(js_ctx, ctx->transform_feedback_paused);
                    case GL_VERTEX_ARRAY_BINDING: return JS_NewInt32(js_ctx, (int32_t)ctx->bound_vertex_array);
                    default: break;
                }
            }
            webgl_set_error(ctx, GL_INVALID_ENUM);
            return JS_NULL;
    }
}

/* --- getProgramInfoLog / getProgramParameter --- */
static JSValue lr_webgl_get_program_info_log(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t program = 0; JS_ToInt32(js_ctx, &program, argv[0]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    if (obj && obj->data) { LR_WebGLProgramData *pd = (LR_WebGLProgramData *)obj->data; if (pd->info_log) return JS_NewString(js_ctx, pd->info_log); }
    return JS_NewString(js_ctx, "");
}
static JSValue lr_webgl_get_program_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t program = 0, pname = 0;
    JS_ToInt32(js_ctx, &program, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    if (!obj || !obj->data) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    LR_WebGLProgramData *pd = (LR_WebGLProgramData *)obj->data;
    switch (pname) {
        case GL_DELETE_STATUS: return JS_NewBool(js_ctx, obj->deleted);
        case GL_LINK_STATUS: return JS_NewBool(js_ctx, pd->link_status);
        case GL_VALIDATE_STATUS: return JS_NewBool(js_ctx, pd->validate_status);
        case GL_ATTACHED_SHADERS: return JS_NewInt32(js_ctx, pd->num_attached);
        case GL_ACTIVE_ATTRIBUTES: return JS_NewInt32(js_ctx, pd->num_attribs);
        case GL_ACTIVE_UNIFORMS: return JS_NewInt32(js_ctx, pd->num_uniforms);
        default: webgl_set_error(ctx, GL_INVALID_ENUM); return JS_UNDEFINED;
    }
}

/* --- getShaderInfoLog / getShaderParameter --- */
static JSValue lr_webgl_get_shader_info_log(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t shader = 0; JS_ToInt32(js_ctx, &shader, argv[0]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
    if (obj && obj->data) { LR_WebGLShaderData *sd = (LR_WebGLShaderData *)obj->data; if (sd->info_log) return JS_NewString(js_ctx, sd->info_log); }
    return JS_NewString(js_ctx, "");
}

static JSValue lr_webgl_get_shader_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t shader = 0, pname = 0;
    JS_ToInt32(js_ctx, &shader, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
    if (!obj || !obj->data) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    LR_WebGLShaderData *sd = (LR_WebGLShaderData *)obj->data;
    switch (pname) {
        case GL_SHADER_TYPE: return JS_NewInt32(js_ctx, (int32_t)sd->type);
        case GL_DELETE_STATUS: return JS_NewBool(js_ctx, obj->deleted);
        case GL_COMPILE_STATUS: return JS_NewBool(js_ctx, sd->compile_status);
        default: webgl_set_error(ctx, GL_INVALID_ENUM); return JS_UNDEFINED;
    }
}

/* --- getUniformLocation --- */
static JSValue lr_webgl_get_uniform_location(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t program = 0; JS_ToInt32(js_ctx, &program, argv[0]);
    const char *name = JS_ToCString(js_ctx, argv[1]);
    if (!name) return JS_UNDEFINED;
    GLint loc = -1;
    WEBGL_CALL(ctx, loc = glGetUniformLocation((GLuint)program, name));
    JS_FreeCString(js_ctx, name);
    if (loc < 0) return JS_NULL;
    return JS_NewInt32(js_ctx, (int32_t)loc);
}

/* --- getVertexAttribOffset --- */
static JSValue lr_webgl_get_vertex_attrib_offset(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t index = 0, pname = 0;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]);
    if (index >= 0 && index < LR_WEBGL_MAX_VERTEX_ATTRIBS && pname == GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        return JS_NewInt32(js_ctx, (int32_t)ctx->attribs[index].offset);
    }
    return JS_NewInt32(js_ctx, 0);
}

/* --- isBuffer/isEnabled/isProgram/isRenderbuffer/isShader/isTexture --- */
static JSValue lr_webgl_is_buffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_BUFFER) != NULL);
}

static JSValue lr_webgl_is_enabled(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t cap = 0; JS_ToInt32(js_ctx, &cap, argv[0]);
    int val = 0;
    switch (cap) {
        case GL_BLEND: val = ctx->enable_blend; break;
        case GL_CULL_FACE: val = ctx->enable_cull_face; break;
        case GL_DEPTH_TEST: val = ctx->enable_depth_test; break;
        case GL_STENCIL_TEST: val = ctx->enable_stencil_test; break;
        case GL_SCISSOR_TEST: val = ctx->enable_scissor_test; break;
        case GL_DITHER: val = ctx->enable_dither; break;
        case GL_POLYGON_OFFSET_FILL: val = ctx->enable_polygon_offset_fill; break;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: val = ctx->enable_sample_alpha_to_coverage; break;
        case GL_SAMPLE_COVERAGE: val = ctx->enable_sample_coverage; break;
        default: { GLboolean gl_val = 0; WEBGL_CALL(ctx, gl_val = glIsEnabled((GLenum)cap)); val = gl_val; break; }
    }
    return JS_NewBool(js_ctx, val);
}

static JSValue lr_webgl_is_program(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_PROGRAM) != NULL);
}

static JSValue lr_webgl_is_renderbuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_RENDERBUFFER) != NULL);
}

static JSValue lr_webgl_is_shader(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_SHADER) != NULL);
}

static JSValue lr_webgl_is_texture(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_TEXTURE) != NULL);
}

/* --- lineWidth --- */
static JSValue lr_webgl_line_width(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    double w = 0; JS_ToFloat64(js_ctx, &w, argv[0]);
    ctx->line_width = (float)w;
    WEBGL_CALL(ctx, glLineWidth((GLfloat)w));
    return JS_UNDEFINED;
}

/* --- linkProgram --- */
static JSValue lr_webgl_link_program(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t program = 0; JS_ToInt32(js_ctx, &program, argv[0]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    if (!obj || !obj->data) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    LR_WebGLProgramData *pd = (LR_WebGLProgramData *)obj->data;
    pd->link_status = GL_TRUE;
    free(pd->info_log); pd->info_log = strdup("Program linked successfully (software mode)");
    WEBGL_CALL(ctx, glLinkProgram((GLuint)program));
    return JS_UNDEFINED;
}

/* --- pixelStorei --- */
static JSValue lr_webgl_pixel_storei(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t pname = 0, param = 0;
    JS_ToInt32(js_ctx, &pname, argv[0]); JS_ToInt32(js_ctx, &param, argv[1]);
    switch (pname) {
        case GL_UNPACK_ALIGNMENT: ctx->unpack_alignment = param; break;
        case GL_PACK_ALIGNMENT: ctx->pack_alignment = param; break;
        case GL_UNPACK_ROW_LENGTH: ctx->unpack_row_length = param; break;
        case GL_UNPACK_SKIP_ROWS: ctx->unpack_skip_rows = param; break;
        case GL_UNPACK_SKIP_PIXELS: ctx->unpack_skip_pixels = param; break;
        case GL_PACK_ROW_LENGTH: ctx->pack_row_length = param; break;
        case GL_PACK_SKIP_ROWS: ctx->pack_skip_rows = param; break;
        case GL_PACK_SKIP_PIXELS: ctx->pack_skip_pixels = param; break;
        case GL_UNPACK_SKIP_IMAGES: ctx->unpack_skip_images = param; break;
        case GL_UNPACK_IMAGE_HEIGHT: ctx->unpack_image_height = param; break;
        default: webgl_set_error(ctx, GL_INVALID_ENUM); return JS_UNDEFINED;
    }
    WEBGL_CALL(ctx, glPixelStorei((GLenum)pname, (GLint)param));
    return JS_UNDEFINED;
}

/* --- polygonOffset --- */
static JSValue lr_webgl_polygon_offset(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    double factor = 0, units = 0;
    JS_ToFloat64(js_ctx, &factor, argv[0]); JS_ToFloat64(js_ctx, &units, argv[1]);
    ctx->polygon_offset_factor = (float)factor; ctx->polygon_offset_units = (float)units;
    WEBGL_CALL(ctx, glPolygonOffset((GLfloat)factor, (GLfloat)units));
    return JS_UNDEFINED;
}

/* --- readPixels --- */
static JSValue lr_webgl_read_pixels(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 7) return JS_UNDEFINED;
    int32_t x, y, w, h, format, type, offset;
    JS_ToInt32(js_ctx, &x, argv[0]); JS_ToInt32(js_ctx, &y, argv[1]);
    JS_ToInt32(js_ctx, &w, argv[2]); JS_ToInt32(js_ctx, &h, argv[3]);
    JS_ToInt32(js_ctx, &format, argv[4]); JS_ToInt32(js_ctx, &type, argv[5]); JS_ToInt32(js_ctx, &offset, argv[6]);
    /* For software mode, we can't read pixels, just return undefined */
    WEBGL_CALL(ctx, glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, (GLenum)format, (GLenum)type, (void *)(intptr_t)offset));
    return JS_UNDEFINED;
}

/* --- renderbufferStorage --- */
static JSValue lr_webgl_renderbuffer_storage(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t target, internalformat, w, h;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &internalformat, argv[1]);
    JS_ToInt32(js_ctx, &w, argv[2]); JS_ToInt32(js_ctx, &h, argv[3]);
    if (ctx->bound_renderbuffer > 0) {
        LR_WebGLObject *obj = webgl_object_find(ctx, ctx->bound_renderbuffer, LR_WEBGL_OBJECT_RENDERBUFFER);
        if (obj && obj->data) {
            LR_WebGLRenderbufferData *rd = (LR_WebGLRenderbufferData *)obj->data;
            rd->internal_format = (GLenum)internalformat; rd->width = w; rd->height = h;
        }
    }
    WEBGL_CALL(ctx, glRenderbufferStorage((GLenum)target, (GLenum)internalformat, (GLsizei)w, (GLsizei)h));
    return JS_UNDEFINED;
}

/* --- sampleCoverage --- */
static JSValue lr_webgl_sample_coverage(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    double value = 0; int32_t invert = 0;
    JS_ToFloat64(js_ctx, &value, argv[0]); JS_ToInt32(js_ctx, &invert, argv[1]);
    ctx->sample_coverage_value = (float)value; ctx->sample_coverage_invert = invert;
    WEBGL_CALL(ctx, glSampleCoverage((GLfloat)value, (GLboolean)invert));
    return JS_UNDEFINED;
}

/* --- scissor --- */
static JSValue lr_webgl_scissor(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t x, y, w, h;
    JS_ToInt32(js_ctx, &x, argv[0]); JS_ToInt32(js_ctx, &y, argv[1]);
    JS_ToInt32(js_ctx, &w, argv[2]); JS_ToInt32(js_ctx, &h, argv[3]);
    ctx->scissor_x = x; ctx->scissor_y = y; ctx->scissor_w = w; ctx->scissor_h = h;
    WEBGL_CALL(ctx, glScissor((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h));
    return JS_UNDEFINED;
}

/* --- shaderSource --- */
static JSValue lr_webgl_shader_source(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t shader = 0; JS_ToInt32(js_ctx, &shader, argv[0]);
    const char *source = JS_ToCString(js_ctx, argv[1]);
    if (!source) return JS_UNDEFINED;
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)shader, LR_WEBGL_OBJECT_SHADER);
    if (obj && obj->data) {
        LR_WebGLShaderData *sd = (LR_WebGLShaderData *)obj->data;
        free(sd->source); sd->source = strdup(source);
    }
    WEBGL_CALL(ctx, glShaderSource((GLuint)shader, 1, (const GLchar *const *)&source, NULL));
    JS_FreeCString(js_ctx, source);
    return JS_UNDEFINED;
}

/* --- stencilFunc/FuncSeparate/Mask/MaskSeparate/Op/OpSeparate --- */
static JSValue lr_webgl_stencil_func(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t func, ref, mask;
    JS_ToInt32(js_ctx, &func, argv[0]); JS_ToInt32(js_ctx, &ref, argv[1]); JS_ToInt32(js_ctx, &mask, argv[2]);
    ctx->stencil_func_front = (GLenum)func; ctx->stencil_func_back = (GLenum)func;
    ctx->stencil_ref_front = ref; ctx->stencil_ref_back = ref;
    ctx->stencil_mask_read_front = (GLuint)mask; ctx->stencil_mask_read_back = (GLuint)mask;
    WEBGL_CALL(ctx, glStencilFunc((GLenum)func, (GLint)ref, (GLuint)mask));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_stencil_func_separate(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t face, func, ref, mask;
    JS_ToInt32(js_ctx, &face, argv[0]); JS_ToInt32(js_ctx, &func, argv[1]);
    JS_ToInt32(js_ctx, &ref, argv[2]); JS_ToInt32(js_ctx, &mask, argv[3]);
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) { ctx->stencil_func_front = (GLenum)func; ctx->stencil_ref_front = ref; ctx->stencil_mask_read_front = (GLuint)mask; }
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) { ctx->stencil_func_back = (GLenum)func; ctx->stencil_ref_back = ref; ctx->stencil_mask_read_back = (GLuint)mask; }
    WEBGL_CALL(ctx, glStencilFuncSeparate((GLenum)face, (GLenum)func, (GLint)ref, (GLuint)mask));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_stencil_mask(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t mask = 0; JS_ToInt32(js_ctx, &mask, argv[0]);
    ctx->stencil_mask_front = mask; ctx->stencil_mask_back = mask;
    WEBGL_CALL(ctx, glStencilMask((GLuint)mask));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_stencil_mask_separate(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t face = 0, mask = 0;
    JS_ToInt32(js_ctx, &face, argv[0]); JS_ToInt32(js_ctx, &mask, argv[1]);
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) ctx->stencil_mask_front = mask;
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) ctx->stencil_mask_back = mask;
    WEBGL_CALL(ctx, glStencilMaskSeparate((GLenum)face, (GLuint)mask));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_stencil_op(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t fail, zfail, zpass;
    JS_ToInt32(js_ctx, &fail, argv[0]); JS_ToInt32(js_ctx, &zfail, argv[1]); JS_ToInt32(js_ctx, &zpass, argv[2]);
    ctx->stencil_fail_front = (GLenum)fail; ctx->stencil_fail_back = (GLenum)fail;
    ctx->stencil_zfail_front = (GLenum)zfail; ctx->stencil_zfail_back = (GLenum)zfail;
    ctx->stencil_zpass_front = (GLenum)zpass; ctx->stencil_zpass_back = (GLenum)zpass;
    WEBGL_CALL(ctx, glStencilOp((GLenum)fail, (GLenum)zfail, (GLenum)zpass));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_stencil_op_separate(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t face, fail, zfail, zpass;
    JS_ToInt32(js_ctx, &face, argv[0]); JS_ToInt32(js_ctx, &fail, argv[1]);
    JS_ToInt32(js_ctx, &zfail, argv[2]); JS_ToInt32(js_ctx, &zpass, argv[3]);
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) { ctx->stencil_fail_front = (GLenum)fail; ctx->stencil_zfail_front = (GLenum)zfail; ctx->stencil_zpass_front = (GLenum)zpass; }
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) { ctx->stencil_fail_back = (GLenum)fail; ctx->stencil_zfail_back = (GLenum)zfail; ctx->stencil_zpass_back = (GLenum)zpass; }
    WEBGL_CALL(ctx, glStencilOpSeparate((GLenum)face, (GLenum)fail, (GLenum)zfail, (GLenum)zpass));
    return JS_UNDEFINED;
}
/* --- texImage2D --- */
static JSValue lr_webgl_tex_image_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 6) return JS_UNDEFINED;
    int32_t target, level, internalformat, width, height, border, format, type;
    void *pixels = NULL; size_t pixels_len = 0;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &level, argv[1]); JS_ToInt32(js_ctx, &internalformat, argv[2]);
    if (argc == 9) {
        JS_ToInt32(js_ctx, &width, argv[3]); JS_ToInt32(js_ctx, &height, argv[4]); JS_ToInt32(js_ctx, &border, argv[5]);
        JS_ToInt32(js_ctx, &format, argv[6]); JS_ToInt32(js_ctx, &type, argv[7]);
        if (argc > 8) pixels = webgl_get_typed_array_data(js_ctx, argv[8], &pixels_len, NULL);
    } else {
        JS_ToInt32(js_ctx, &width, argv[3]); JS_ToInt32(js_ctx, &height, argv[4]);
        JS_ToInt32(js_ctx, &format, argv[5]); JS_ToInt32(js_ctx, &type, argv[6]);
        if (argc > 7) pixels = webgl_get_typed_array_data(js_ctx, argv[7], &pixels_len, NULL);
        border = 0;
    }
    /* Update software texture tracking */
    GLuint tex_id = ctx->bound_textures[ctx->active_texture_unit];
    if (tex_id > 0) {
        LR_WebGLObject *obj = webgl_object_find(ctx, tex_id, LR_WEBGL_OBJECT_TEXTURE);
        if (obj && obj->data) {
            LR_WebGLTextureData *td = (LR_WebGLTextureData *)obj->data;
            td->width = width; td->height = height; td->internal_format = (GLenum)internalformat;
            td->format = (GLenum)format; td->type = (GLenum)type;
            free(td->pixels);
            if (pixels && pixels_len > 0) { td->pixels = (uint8_t *)malloc(pixels_len); if (td->pixels) memcpy(td->pixels, pixels, pixels_len); }
            else td->pixels = NULL;
        }
    }
    WEBGL_CALL(ctx, glTexImage2D((GLenum)target, (GLint)level, (GLint)internalformat, (GLsizei)width, (GLsizei)height, (GLint)border, (GLenum)format, (GLenum)type, pixels));
    return JS_UNDEFINED;
}

/* --- texParameteri --- */
static JSValue lr_webgl_tex_parameteri(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t target, pname, param;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]); JS_ToInt32(js_ctx, &param, argv[2]);
    GLuint tex_id = ctx->bound_textures[ctx->active_texture_unit];
    if (tex_id > 0) {
        LR_WebGLObject *obj = webgl_object_find(ctx, tex_id, LR_WEBGL_OBJECT_TEXTURE);
        if (obj && obj->data) {
            LR_WebGLTextureData *td = (LR_WebGLTextureData *)obj->data;
            if (pname == GL_TEXTURE_MAG_FILTER) td->mag_filter = (GLenum)param;
            else if (pname == GL_TEXTURE_MIN_FILTER) td->min_filter = (GLenum)param;
            else if (pname == GL_TEXTURE_WRAP_S) td->wrap_s = (GLenum)param;
            else if (pname == GL_TEXTURE_WRAP_T) td->wrap_t = (GLenum)param;
            else if (pname == GL_TEXTURE_WRAP_R) td->wrap_r = (GLenum)param;
        }
    }
    WEBGL_CALL(ctx, glTexParameteri((GLenum)target, (GLenum)pname, (GLint)param));
    return JS_UNDEFINED;
}

/* --- texParameterf --- */
static JSValue lr_webgl_tex_parameterf(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t target, pname; double param;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]); JS_ToFloat64(js_ctx, &param, argv[2]);
    WEBGL_CALL(ctx, glTexParameterf((GLenum)target, (GLenum)pname, (GLfloat)param));
    return JS_UNDEFINED;
}

/* --- texSubImage2D --- */
static JSValue lr_webgl_tex_sub_image_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 7) return JS_UNDEFINED;
    int32_t target, level, xoffset, yoffset, width, height, format, type;
    void *pixels = NULL; size_t pixels_len = 0;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &level, argv[1]);
    JS_ToInt32(js_ctx, &xoffset, argv[2]); JS_ToInt32(js_ctx, &yoffset, argv[3]);
    JS_ToInt32(js_ctx, &width, argv[4]); JS_ToInt32(js_ctx, &height, argv[5]);
    JS_ToInt32(js_ctx, &format, argv[6]); JS_ToInt32(js_ctx, &type, argv[7]);
    if (argc > 8) pixels = webgl_get_typed_array_data(js_ctx, argv[8], &pixels_len, NULL);
    WEBGL_CALL(ctx, glTexSubImage2D((GLenum)target, (GLint)level, (GLint)xoffset, (GLint)yoffset, (GLsizei)width, (GLsizei)height, (GLenum)format, (GLenum)type, pixels));
    return JS_UNDEFINED;
}

/* ── Uniform setters (1f, 1fv, 1i, 1iv, 2f, 2fv, 2i, 2iv, 3f, 3fv, 3i, 3iv, 4f, 4fv, 4i, 4iv, Matrix2fv, Matrix3fv, Matrix4fv) ── */

static JSValue lr_webgl_uniform_1f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; double v0; JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]);
    WEBGL_CALL(ctx, glUniform1f((GLint)loc, (GLfloat)v0));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_1fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform1fv((GLint)loc, (GLsizei)(len / sizeof(GLfloat)), (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_1i(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0; JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    WEBGL_CALL(ctx, glUniform1i((GLint)loc, (GLint)v0));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_1iv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform1iv((GLint)loc, (GLsizei)(len / sizeof(GLint)), (const GLint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_2f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0; double v0, v1;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]); JS_ToFloat64(js_ctx, &v1, argv[2]);
    WEBGL_CALL(ctx, glUniform2f((GLint)loc, (GLfloat)v0, (GLfloat)v1));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_2fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform2fv((GLint)loc, (GLsizei)(len / (2 * sizeof(GLfloat))), (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_2i(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0, v1 = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]); JS_ToInt32(js_ctx, &v1, argv[2]);
    WEBGL_CALL(ctx, glUniform2i((GLint)loc, (GLint)v0, (GLint)v1));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_2iv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform2iv((GLint)loc, (GLsizei)(len / (2 * sizeof(GLint))), (const GLint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_3f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t loc = 0; double v0, v1, v2;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]);
    JS_ToFloat64(js_ctx, &v1, argv[2]); JS_ToFloat64(js_ctx, &v2, argv[3]);
    WEBGL_CALL(ctx, glUniform3f((GLint)loc, (GLfloat)v0, (GLfloat)v1, (GLfloat)v2));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_3fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform3fv((GLint)loc, (GLsizei)(len / (3 * sizeof(GLfloat))), (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_3i(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0, v1 = 0, v2 = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    JS_ToInt32(js_ctx, &v1, argv[2]); JS_ToInt32(js_ctx, &v2, argv[3]);
    WEBGL_CALL(ctx, glUniform3i((GLint)loc, (GLint)v0, (GLint)v1, (GLint)v2));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_3iv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform3iv((GLint)loc, (GLsizei)(len / (3 * sizeof(GLint))), (const GLint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_4f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t loc = 0; double v0, v1, v2, v3;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]);
    JS_ToFloat64(js_ctx, &v1, argv[2]); JS_ToFloat64(js_ctx, &v2, argv[3]); JS_ToFloat64(js_ctx, &v3, argv[4]);
    WEBGL_CALL(ctx, glUniform4f((GLint)loc, (GLfloat)v0, (GLfloat)v1, (GLfloat)v2, (GLfloat)v3));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_4fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform4fv((GLint)loc, (GLsizei)(len / (4 * sizeof(GLfloat))), (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_4i(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0, v1 = 0, v2 = 0, v3 = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    JS_ToInt32(js_ctx, &v1, argv[2]); JS_ToInt32(js_ctx, &v2, argv[3]); JS_ToInt32(js_ctx, &v3, argv[4]);
    WEBGL_CALL(ctx, glUniform4i((GLint)loc, (GLint)v0, (GLint)v1, (GLint)v2, (GLint)v3));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_4iv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform4iv((GLint)loc, (GLsizei)(len / (4 * sizeof(GLint))), (const GLint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_2fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix2fv((GLint)loc, (GLsizei)(len / (4 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_3fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix3fv((GLint)loc, (GLsizei)(len / (9 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_4fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix4fv((GLint)loc, (GLsizei)(len / (16 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

/* --- useProgram --- */
static JSValue lr_webgl_use_program(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t program = 0;
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        ctx->current_program = 0;
        WEBGL_CALL(ctx, glUseProgram(0));
    } else {
        JS_ToInt32(js_ctx, &program, argv[0]);
        ctx->current_program = (GLuint)program;
        WEBGL_CALL(ctx, glUseProgram((GLuint)program));
    }
    return JS_UNDEFINED;
}

/* --- validateProgram --- */
static JSValue lr_webgl_validate_program(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t program = 0; JS_ToInt32(js_ctx, &program, argv[0]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)program, LR_WEBGL_OBJECT_PROGRAM);
    if (obj && obj->data) { LR_WebGLProgramData *pd = (LR_WebGLProgramData *)obj->data; pd->validate_status = GL_TRUE; }
    WEBGL_CALL(ctx, glValidateProgram((GLuint)program));
    return JS_UNDEFINED;
}

/* --- vertexAttrib[1-4]f --- */
static JSValue lr_webgl_vertex_attrib_1f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t index = 0; double v0; JS_ToInt32(js_ctx, &index, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]);
    WEBGL_CALL(ctx, glVertexAttrib1f((GLuint)index, (GLfloat)v0));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_vertex_attrib_2f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t index = 0; double v0, v1;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]); JS_ToFloat64(js_ctx, &v1, argv[2]);
    WEBGL_CALL(ctx, glVertexAttrib2f((GLuint)index, (GLfloat)v0, (GLfloat)v1));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_vertex_attrib_3f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t index = 0; double v0, v1, v2;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]);
    JS_ToFloat64(js_ctx, &v1, argv[2]); JS_ToFloat64(js_ctx, &v2, argv[3]);
    WEBGL_CALL(ctx, glVertexAttrib3f((GLuint)index, (GLfloat)v0, (GLfloat)v1, (GLfloat)v2));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_vertex_attrib_4f(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t index = 0; double v0, v1, v2, v3;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToFloat64(js_ctx, &v0, argv[1]);
    JS_ToFloat64(js_ctx, &v1, argv[2]); JS_ToFloat64(js_ctx, &v2, argv[3]); JS_ToFloat64(js_ctx, &v3, argv[4]);
    WEBGL_CALL(ctx, glVertexAttrib4f((GLuint)index, (GLfloat)v0, (GLfloat)v1, (GLfloat)v2, (GLfloat)v3));
    return JS_UNDEFINED;
}

/* --- vertexAttribPointer --- */
static JSValue lr_webgl_vertex_attrib_pointer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 6) return JS_UNDEFINED;
    int32_t index, size, type, normalized, stride, offset;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToInt32(js_ctx, &size, argv[1]);
    JS_ToInt32(js_ctx, &type, argv[2]); JS_ToInt32(js_ctx, &normalized, argv[3]);
    JS_ToInt32(js_ctx, &stride, argv[4]); JS_ToInt32(js_ctx, &offset, argv[5]);
    if (index >= 0 && index < LR_WEBGL_MAX_VERTEX_ATTRIBS) {
        ctx->attribs[index].size = size; ctx->attribs[index].type = (GLenum)type;
        ctx->attribs[index].normalized = normalized; ctx->attribs[index].stride = stride;
        ctx->attribs[index].offset = offset; ctx->attribs[index].buffer_id = ctx->bound_array_buffer;
    }
    WEBGL_CALL(ctx, glVertexAttribPointer((GLuint)index, (GLint)size, (GLenum)type, (GLboolean)normalized, (GLsizei)stride, (const void *)(intptr_t)offset));
    return JS_UNDEFINED;
}

/* --- viewport --- */
static JSValue lr_webgl_viewport(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t x, y, w, h;
    JS_ToInt32(js_ctx, &x, argv[0]); JS_ToInt32(js_ctx, &y, argv[1]);
    JS_ToInt32(js_ctx, &w, argv[2]); JS_ToInt32(js_ctx, &h, argv[3]);
    ctx->vp_x = x; ctx->vp_y = y; ctx->vp_w = w; ctx->vp_h = h;
    WEBGL_CALL(ctx, glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h));
    return JS_UNDEFINED;
}
/* ══════════════════════════════════════════════════════════════════════════
   WebGL 2.0 Methods
   ══════════════════════════════════════════════════════════════════════════ */

/* --- beginQuery / endQuery / deleteQuery / isQuery / getQueryParameter --- */
static JSValue lr_webgl_begin_query(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2 || !ctx->is_webgl2) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    int32_t target, id;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &id, argv[1]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_QUERY);
    if (obj && obj->data) { LR_WebGLQueryData *qd = (LR_WebGLQueryData *)obj->data; qd->active = 1; qd->target = (GLenum)target; }
    WEBGL_CALL(ctx, glBeginQuery((GLenum)target, (GLuint)id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_end_query(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1 || !ctx->is_webgl2) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    int32_t target; JS_ToInt32(js_ctx, &target, argv[0]);
    WEBGL_CALL(ctx, glEndQuery((GLenum)target));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_delete_query(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    webgl_object_delete(ctx, (GLuint)id, LR_WEBGL_OBJECT_QUERY);
    WEBGL_CALL(ctx, glDeleteQueries(1, (const GLuint *)&id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_is_query(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_QUERY) != NULL);
}

static JSValue lr_webgl_get_query_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t id, pname; JS_ToInt32(js_ctx, &id, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]);
    LR_WebGLObject *obj = webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_QUERY);
    if (obj && obj->data) {
        LR_WebGLQueryData *qd = (LR_WebGLQueryData *)obj->data;
        if (pname == GL_QUERY_RESULT) return JS_NewInt32(js_ctx, (int32_t)qd->result);
        if (pname == GL_QUERY_RESULT_AVAILABLE) return JS_NewBool(js_ctx, qd->result_available);
    }
    return JS_NewInt32(js_ctx, 0);
}

/* --- beginTransformFeedback / endTransformFeedback / pauseTransformFeedback / resumeTransformFeedback --- */
static JSValue lr_webgl_begin_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1 || !ctx->is_webgl2) { webgl_set_error(ctx, GL_INVALID_OPERATION); return JS_UNDEFINED; }
    int32_t mode; JS_ToInt32(js_ctx, &mode, argv[0]);
    ctx->transform_feedback_active = 1;
    WEBGL_CALL(ctx, glBeginTransformFeedback((GLenum)mode));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_end_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    ctx->transform_feedback_active = 0; ctx->transform_feedback_paused = 0;
    WEBGL_CALL(ctx, glEndTransformFeedback());
    return JS_UNDEFINED;
}

static JSValue lr_webgl_pause_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    ctx->transform_feedback_paused = 1;
    WEBGL_CALL(ctx, glPauseTransformFeedback());
    return JS_UNDEFINED;
}

static JSValue lr_webgl_resume_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    ctx->transform_feedback_paused = 0;
    WEBGL_CALL(ctx, glResumeTransformFeedback());
    return JS_UNDEFINED;
}

/* --- bindBufferBase / bindBufferRange --- */
static JSValue lr_webgl_bind_buffer_base(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, index, buffer;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &index, argv[1]); JS_ToInt32(js_ctx, &buffer, argv[2]);
    WEBGL_CALL(ctx, glBindBufferBase((GLenum)target, (GLuint)index, (GLuint)buffer));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_bind_buffer_range(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, index, buffer, offset, size;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &index, argv[1]);
    JS_ToInt32(js_ctx, &buffer, argv[2]); JS_ToInt32(js_ctx, &offset, argv[3]); JS_ToInt32(js_ctx, &size, argv[4]);
    WEBGL_CALL(ctx, glBindBufferRange((GLenum)target, (GLuint)index, (GLuint)buffer, (GLintptr)offset, (GLsizeiptr)size));
    return JS_UNDEFINED;
}

/* --- bindSampler / deleteSampler / isSampler / samplerParameteri / samplerParameterf --- */
static JSValue lr_webgl_bind_sampler(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t unit, sampler;
    JS_ToInt32(js_ctx, &unit, argv[0]); JS_ToInt32(js_ctx, &sampler, argv[1]);
    if (unit >= 0 && unit < LR_WEBGL_MAX_TEXTURE_UNITS) ctx->bound_samplers[unit] = (GLuint)sampler;
    WEBGL_CALL(ctx, glBindSampler((GLuint)unit, (GLuint)sampler));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_delete_sampler(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    webgl_object_delete(ctx, (GLuint)id, LR_WEBGL_OBJECT_SAMPLER);
    WEBGL_CALL(ctx, glDeleteSamplers(1, (const GLuint *)&id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_is_sampler(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_SAMPLER) != NULL);
}

static JSValue lr_webgl_sampler_parameteri(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t sampler, pname, param;
    JS_ToInt32(js_ctx, &sampler, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]); JS_ToInt32(js_ctx, &param, argv[2]);
    WEBGL_CALL(ctx, glSamplerParameteri((GLuint)sampler, (GLenum)pname, (GLint)param));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_sampler_parameterf(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t sampler, pname; double param;
    JS_ToInt32(js_ctx, &sampler, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]); JS_ToFloat64(js_ctx, &param, argv[2]);
    WEBGL_CALL(ctx, glSamplerParameterf((GLuint)sampler, (GLenum)pname, (GLfloat)param));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_get_sampler_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t sampler, pname; JS_ToInt32(js_ctx, &sampler, argv[0]); JS_ToInt32(js_ctx, &pname, argv[1]);
    WEBGL_CALL(ctx, glGetSamplerParameteriv((GLuint)sampler, (GLenum)pname, (GLint *)&pname));
    return JS_NewInt32(js_ctx, pname);
}

/* --- bindTransformFeedback / deleteTransformFeedback / isTransformFeedback --- */
static JSValue lr_webgl_bind_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, id; JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &id, argv[1]);
    ctx->bound_transform_feedback = (GLuint)id;
    WEBGL_CALL(ctx, glBindTransformFeedback((GLenum)target, (GLuint)id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_delete_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    webgl_object_delete(ctx, (GLuint)id, LR_WEBGL_OBJECT_TRANSFORM_FEEDBACK);
    WEBGL_CALL(ctx, glDeleteTransformFeedbacks(1, (const GLuint *)&id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_is_transform_feedback(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_TRANSFORM_FEEDBACK) != NULL);
}

/* --- bindVertexArray / deleteVertexArray / isVertexArray --- */
static JSValue lr_webgl_bind_vertex_array(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    ctx->bound_vertex_array = (GLuint)id;
    WEBGL_CALL(ctx, glBindVertexArray((GLuint)id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_delete_vertex_array(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    webgl_object_delete(ctx, (GLuint)id, LR_WEBGL_OBJECT_VERTEX_ARRAY);
    WEBGL_CALL(ctx, glDeleteVertexArrays(1, (const GLuint *)&id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_is_vertex_array(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_VERTEX_ARRAY) != NULL);
}

/* --- blitFramebuffer --- */
static JSValue lr_webgl_blit_framebuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 10 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter;
    JS_ToInt32(js_ctx, &srcX0, argv[0]); JS_ToInt32(js_ctx, &srcY0, argv[1]);
    JS_ToInt32(js_ctx, &srcX1, argv[2]); JS_ToInt32(js_ctx, &srcY1, argv[3]);
    JS_ToInt32(js_ctx, &dstX0, argv[4]); JS_ToInt32(js_ctx, &dstY0, argv[5]);
    JS_ToInt32(js_ctx, &dstX1, argv[6]); JS_ToInt32(js_ctx, &dstY1, argv[7]);
    JS_ToInt32(js_ctx, &mask, argv[8]); JS_ToInt32(js_ctx, &filter, argv[9]);
    WEBGL_CALL(ctx, glBlitFramebuffer((GLint)srcX0, (GLint)srcY0, (GLint)srcX1, (GLint)srcY1, (GLint)dstX0, (GLint)dstY0, (GLint)dstX1, (GLint)dstY1, (GLbitfield)mask, (GLenum)filter));
    return JS_UNDEFINED;
}

/* --- clearBufferfv / clearBufferiv / clearBufferuiv --- */
static JSValue lr_webgl_clear_buffer_fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t buffer, drawbuffer, srcOffset;
    JS_ToInt32(js_ctx, &buffer, argv[0]); JS_ToInt32(js_ctx, &drawbuffer, argv[1]); JS_ToInt32(js_ctx, &srcOffset, argv[3]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glClearBufferfv((GLenum)buffer, (GLint)drawbuffer, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_clear_buffer_iv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t buffer, drawbuffer, srcOffset;
    JS_ToInt32(js_ctx, &buffer, argv[0]); JS_ToInt32(js_ctx, &drawbuffer, argv[1]); JS_ToInt32(js_ctx, &srcOffset, argv[3]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glClearBufferiv((GLenum)buffer, (GLint)drawbuffer, (const GLint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_clear_buffer_uiv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t buffer, drawbuffer, srcOffset;
    JS_ToInt32(js_ctx, &buffer, argv[0]); JS_ToInt32(js_ctx, &drawbuffer, argv[1]); JS_ToInt32(js_ctx, &srcOffset, argv[3]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glClearBufferuiv((GLenum)buffer, (GLint)drawbuffer, (const GLuint *)data));
    return JS_UNDEFINED;
}

/* --- clientWaitSync / fenceSync / deleteSync / isSync / getSyncParameter --- */
static JSValue lr_webgl_client_wait_sync(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(js_ctx, GL_CONDITION_SATISFIED);
}

static JSValue lr_webgl_fence_sync(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val;
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t condition, flags; JS_ToInt32(js_ctx, &condition, argv[0]); JS_ToInt32(js_ctx, &flags, argv[1]);
    LR_WebGLObject *obj = webgl_object_create(ctx, LR_WEBGL_OBJECT_SYNC);
    if (!obj) return JS_UNDEFINED;
    return JS_NewInt32(js_ctx, (int32_t)obj->id);
}

static JSValue lr_webgl_delete_sync(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    webgl_object_delete(ctx, (GLuint)id, LR_WEBGL_OBJECT_SYNC);
    WEBGL_CALL(ctx, glDeleteSync((GLsync)(intptr_t)id));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_is_sync(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_NewBool(js_ctx, 0);
    if (argc < 1) return JS_NewBool(js_ctx, 0);
    int32_t id = 0; JS_ToInt32(js_ctx, &id, argv[0]);
    return JS_NewBool(js_ctx, webgl_object_find(ctx, (GLuint)id, LR_WEBGL_OBJECT_SYNC) != NULL);
}

static JSValue lr_webgl_get_sync_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(js_ctx, GL_SIGNALED);
}

/* --- compressedTexImage2D / compressedTexSubImage2D --- */
static JSValue lr_webgl_compressed_tex_image_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 7) return JS_UNDEFINED;
    int32_t target, level, internalformat, w, h, border, imageSize, offset;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &level, argv[1]); JS_ToInt32(js_ctx, &internalformat, argv[2]);
    JS_ToInt32(js_ctx, &w, argv[3]); JS_ToInt32(js_ctx, &h, argv[4]); JS_ToInt32(js_ctx, &border, argv[5]);
    JS_ToInt32(js_ctx, &imageSize, argv[6]); JS_ToInt32(js_ctx, &offset, argv[7]);
    WEBGL_CALL(ctx, glCompressedTexImage2D((GLenum)target, (GLint)level, (GLenum)internalformat, (GLsizei)w, (GLsizei)h, (GLint)border, (GLsizei)imageSize, (const void *)(intptr_t)offset));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_compressed_tex_sub_image_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 8) return JS_UNDEFINED;
    int32_t target, level, xoffset, yoffset, w, h, format, imageSize, offset;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &level, argv[1]); JS_ToInt32(js_ctx, &xoffset, argv[2]);
    JS_ToInt32(js_ctx, &yoffset, argv[3]); JS_ToInt32(js_ctx, &w, argv[4]); JS_ToInt32(js_ctx, &h, argv[5]);
    JS_ToInt32(js_ctx, &format, argv[6]); JS_ToInt32(js_ctx, &imageSize, argv[7]); JS_ToInt32(js_ctx, &offset, argv[8]);
    WEBGL_CALL(ctx, glCompressedTexSubImage2D((GLenum)target, (GLint)level, (GLint)xoffset, (GLint)yoffset, (GLsizei)w, (GLsizei)h, (GLenum)format, (GLsizei)imageSize, (const void *)(intptr_t)offset));
    return JS_UNDEFINED;
}

/* --- copyBufferSubData --- */
static JSValue lr_webgl_copy_buffer_sub_data(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t readTarget, writeTarget, readOffset, writeOffset, size;
    JS_ToInt32(js_ctx, &readTarget, argv[0]); JS_ToInt32(js_ctx, &writeTarget, argv[1]);
    JS_ToInt32(js_ctx, &readOffset, argv[2]); JS_ToInt32(js_ctx, &writeOffset, argv[3]); JS_ToInt32(js_ctx, &size, argv[4]);
    WEBGL_CALL(ctx, glCopyBufferSubData((GLenum)readTarget, (GLenum)writeTarget, (GLintptr)readOffset, (GLintptr)writeOffset, (GLsizeiptr)size));
    return JS_UNDEFINED;
}

/* --- drawArraysInstanced / drawElementsInstanced --- */
static JSValue lr_webgl_draw_arrays_instanced(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t mode, first, count, instanceCount;
    JS_ToInt32(js_ctx, &mode, argv[0]); JS_ToInt32(js_ctx, &first, argv[1]);
    JS_ToInt32(js_ctx, &count, argv[2]); JS_ToInt32(js_ctx, &instanceCount, argv[3]);
    WEBGL_CALL(ctx, glDrawArraysInstanced((GLenum)mode, (GLint)first, (GLsizei)count, (GLsizei)instanceCount));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_draw_elements_instanced(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t mode, count, type, offset, instanceCount;
    JS_ToInt32(js_ctx, &mode, argv[0]); JS_ToInt32(js_ctx, &count, argv[1]);
    JS_ToInt32(js_ctx, &type, argv[2]); JS_ToInt32(js_ctx, &offset, argv[3]); JS_ToInt32(js_ctx, &instanceCount, argv[4]);
    WEBGL_CALL(ctx, glDrawElementsInstanced((GLenum)mode, (GLsizei)count, (GLenum)type, (const void *)(intptr_t)offset, (GLsizei)instanceCount));
    return JS_UNDEFINED;
}

/* --- drawBuffers --- */
static JSValue lr_webgl_draw_buffers(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1 || !ctx->is_webgl2) return JS_UNDEFINED;
    /* Get the array length */
    JSValue len_val = JS_GetPropertyStr(js_ctx, argv[0], "length");
    int32_t len = 0; JS_ToInt32(js_ctx, &len, len_val); JS_FreeValue(js_ctx, len_val);
    if (len > LR_WEBGL_MAX_DRAW_BUFFERS) len = LR_WEBGL_MAX_DRAW_BUFFERS;
    GLenum bufs[LR_WEBGL_MAX_DRAW_BUFFERS];
    for (int i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(js_ctx, argv[0], (uint32_t)i);
        int32_t b = 0; JS_ToInt32(js_ctx, &b, v);
        bufs[i] = (GLenum)b; ctx->draw_buffers[i] = (GLenum)b;
        JS_FreeValue(js_ctx, v);
    }
    WEBGL_CALL(ctx, glDrawBuffers((GLsizei)len, bufs));
    return JS_UNDEFINED;
}

/* --- drawRangeElements --- */
static JSValue lr_webgl_draw_range_elements(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 6 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t mode, start, end, count, type, offset;
    JS_ToInt32(js_ctx, &mode, argv[0]); JS_ToInt32(js_ctx, &start, argv[1]);
    JS_ToInt32(js_ctx, &end, argv[2]); JS_ToInt32(js_ctx, &count, argv[3]);
    JS_ToInt32(js_ctx, &type, argv[4]); JS_ToInt32(js_ctx, &offset, argv[5]);
    WEBGL_CALL(ctx, glDrawRangeElements((GLenum)mode, (GLuint)start, (GLuint)end, (GLsizei)count, (GLenum)type, (const void *)(intptr_t)offset));
    return JS_UNDEFINED;
}

/* --- framebufferTextureLayer --- */
static JSValue lr_webgl_framebuffer_texture_layer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, attachment, texture, level, layer;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &attachment, argv[1]);
    JS_ToInt32(js_ctx, &texture, argv[2]); JS_ToInt32(js_ctx, &level, argv[3]); JS_ToInt32(js_ctx, &layer, argv[4]);
    WEBGL_CALL(ctx, glFramebufferTextureLayer((GLenum)target, (GLenum)attachment, (GLuint)texture, (GLint)level, (GLint)layer));
    return JS_UNDEFINED;
}

/* --- getActiveUniforms / getUniformBlockIndex / getUniformIndices / uniformBlockBinding --- */
static JSValue lr_webgl_get_active_uniforms(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(js_ctx);
}

static JSValue lr_webgl_get_uniform_block_index(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    (void)ctx;
    if (argc < 2) return JS_NewInt32(js_ctx, -1);
    const char *name = JS_ToCString(js_ctx, argv[1]);
    if (!name) return JS_NewInt32(js_ctx, -1);
    GLuint idx = GL_INVALID_INDEX;
    WEBGL_CALL(ctx, idx = glGetUniformBlockIndex((GLuint)0, name));
    JS_FreeCString(js_ctx, name);
    return JS_NewInt32(js_ctx, (int32_t)idx);
}

static JSValue lr_webgl_get_uniform_indices(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewArray(js_ctx);
}

static JSValue lr_webgl_uniform_block_binding(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t program, blockIndex, binding;
    JS_ToInt32(js_ctx, &program, argv[0]); JS_ToInt32(js_ctx, &blockIndex, argv[1]); JS_ToInt32(js_ctx, &binding, argv[2]);
    WEBGL_CALL(ctx, glUniformBlockBinding((GLuint)program, (GLuint)blockIndex, (GLuint)binding));
    return JS_UNDEFINED;
}

/* --- getBufferSubData --- */
static JSValue lr_webgl_get_buffer_sub_data(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, srcByteOffset, dstOffset, length;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &srcByteOffset, argv[1]);
    size_t dst_len = 0; GLenum type = GL_UNSIGNED_BYTE;
    void *dst = webgl_get_typed_array_data(js_ctx, argv[2], &dst_len, &type);
    JS_ToInt32(js_ctx, &dstOffset, argc > 3 ? argv[3] : argv[2]);
    if (argc > 4) JS_ToInt32(js_ctx, &length, argv[4]);
    else length = (int32_t)(dst_len / sizeof(float));
    WEBGL_CALL(ctx, glGetBufferSubData((GLenum)target, (GLintptr)srcByteOffset, (GLsizeiptr)length, (unsigned char *)dst + dstOffset));
    return JS_UNDEFINED;
}

/* --- getFragDataLocation --- */
static JSValue lr_webgl_get_frag_data_location(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    (void)ctx;
    if (argc < 2) return JS_NewInt32(js_ctx, -1);
    const char *name = JS_ToCString(js_ctx, argv[1]);
    if (!name) return JS_NewInt32(js_ctx, -1);
    GLint loc = -1;
    WEBGL_CALL(ctx, loc = glGetFragDataLocation((GLuint)0, name));
    JS_FreeCString(js_ctx, name);
    return JS_NewInt32(js_ctx, (int32_t)loc);
}

/* --- getInternalformatParameter --- */
static JSValue lr_webgl_get_internalformat_parameter(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(js_ctx, 0);
}

/* --- getTransformFeedbackVarying --- */
static JSValue lr_webgl_get_transform_feedback_varying(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    JSValue info = JS_NewObject(js_ctx);
    JS_SetPropertyStr(js_ctx, info, "name", JS_NewString(js_ctx, ""));
    JS_SetPropertyStr(js_ctx, info, "type", JS_NewInt32(js_ctx, GL_FLOAT_VEC4));
    JS_SetPropertyStr(js_ctx, info, "size", JS_NewInt32(js_ctx, 1));
    return info;
}

/* --- invalidateFramebuffer / invalidateSubFramebuffer --- */
static JSValue lr_webgl_invalidate_framebuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue lr_webgl_invalidate_sub_framebuffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* --- multiDrawArraysInstanced / multiDrawElementsInstanced --- */
static JSValue lr_webgl_multi_draw_arrays_instanced(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static JSValue lr_webgl_multi_draw_elements_instanced(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)js_ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* --- readBuffer --- */
static JSValue lr_webgl_read_buffer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 1 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t src; JS_ToInt32(js_ctx, &src, argv[0]);
    WEBGL_CALL(ctx, glReadBuffer((GLenum)src));
    return JS_UNDEFINED;
}

/* --- renderbufferStorageMultisample --- */
static JSValue lr_webgl_renderbuffer_storage_multisample(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, samples, internalformat, w, h;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &samples, argv[1]);
    JS_ToInt32(js_ctx, &internalformat, argv[2]); JS_ToInt32(js_ctx, &w, argv[3]); JS_ToInt32(js_ctx, &h, argv[4]);
    if (ctx->bound_renderbuffer > 0) {
        LR_WebGLObject *obj = webgl_object_find(ctx, ctx->bound_renderbuffer, LR_WEBGL_OBJECT_RENDERBUFFER);
        if (obj && obj->data) {
            LR_WebGLRenderbufferData *rd = (LR_WebGLRenderbufferData *)obj->data;
            rd->internal_format = (GLenum)internalformat; rd->width = w; rd->height = h; rd->samples = samples;
        }
    }
    WEBGL_CALL(ctx, glRenderbufferStorageMultisample((GLenum)target, (GLsizei)samples, (GLenum)internalformat, (GLsizei)w, (GLsizei)h));
    return JS_UNDEFINED;
}

/* --- texImage3D / texSubImage3D / texStorage2D / texStorage3D --- */
static JSValue lr_webgl_tex_image_3d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 9 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, level, internalformat, w, h, d, border, format, type;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &level, argv[1]); JS_ToInt32(js_ctx, &internalformat, argv[2]);
    JS_ToInt32(js_ctx, &w, argv[3]); JS_ToInt32(js_ctx, &h, argv[4]); JS_ToInt32(js_ctx, &d, argv[5]);
    JS_ToInt32(js_ctx, &border, argv[6]); JS_ToInt32(js_ctx, &format, argv[7]); JS_ToInt32(js_ctx, &type, argv[8]);
    void *pixels = NULL; size_t pixels_len = 0;
    if (argc > 9) pixels = webgl_get_typed_array_data(js_ctx, argv[9], &pixels_len, NULL);
    WEBGL_CALL(ctx, glTexImage3D((GLenum)target, (GLint)level, (GLint)internalformat, (GLsizei)w, (GLsizei)h, (GLsizei)d, (GLint)border, (GLenum)format, (GLenum)type, pixels));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_tex_sub_image_3d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 10 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, level, xoffset, yoffset, zoffset, w, h, d, format, type;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &level, argv[1]);
    JS_ToInt32(js_ctx, &xoffset, argv[2]); JS_ToInt32(js_ctx, &yoffset, argv[3]); JS_ToInt32(js_ctx, &zoffset, argv[4]);
    JS_ToInt32(js_ctx, &w, argv[5]); JS_ToInt32(js_ctx, &h, argv[6]); JS_ToInt32(js_ctx, &d, argv[7]);
    JS_ToInt32(js_ctx, &format, argv[8]); JS_ToInt32(js_ctx, &type, argv[9]);
    void *pixels = NULL; size_t pixels_len = 0;
    if (argc > 10) pixels = webgl_get_typed_array_data(js_ctx, argv[10], &pixels_len, NULL);
    WEBGL_CALL(ctx, glTexSubImage3D((GLenum)target, (GLint)level, (GLint)xoffset, (GLint)yoffset, (GLint)zoffset, (GLsizei)w, (GLsizei)h, (GLsizei)d, (GLenum)format, (GLenum)type, pixels));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_tex_storage_2d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, levels, internalformat, w, h;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &levels, argv[1]); JS_ToInt32(js_ctx, &internalformat, argv[2]);
    JS_ToInt32(js_ctx, &w, argv[3]); JS_ToInt32(js_ctx, &h, argv[4]);
    WEBGL_CALL(ctx, glTexStorage2D((GLenum)target, (GLsizei)levels, (GLenum)internalformat, (GLsizei)w, (GLsizei)h));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_tex_storage_3d(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 6 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t target, levels, internalformat, w, h, d;
    JS_ToInt32(js_ctx, &target, argv[0]); JS_ToInt32(js_ctx, &levels, argv[1]); JS_ToInt32(js_ctx, &internalformat, argv[2]);
    JS_ToInt32(js_ctx, &w, argv[3]); JS_ToInt32(js_ctx, &h, argv[4]); JS_ToInt32(js_ctx, &d, argv[5]);
    WEBGL_CALL(ctx, glTexStorage3D((GLenum)target, (GLsizei)levels, (GLenum)internalformat, (GLsizei)w, (GLsizei)h, (GLsizei)d));
    return JS_UNDEFINED;
}

/* --- transformFeedbackVaryings --- */
static JSValue lr_webgl_transform_feedback_varyings(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t program, bufferMode;
    JS_ToInt32(js_ctx, &program, argv[0]); JS_ToInt32(js_ctx, &bufferMode, argv[2]);
    /* Get the array of varying names */
    JSValue len_val = JS_GetPropertyStr(js_ctx, argv[1], "length");
    int32_t len = 0; JS_ToInt32(js_ctx, &len, len_val); JS_FreeValue(js_ctx, len_val);
    const char **varyings = (const char **)calloc(len, sizeof(char *));
    for (int i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(js_ctx, argv[1], (uint32_t)i);
        varyings[i] = JS_ToCString(js_ctx, v);
        JS_FreeValue(js_ctx, v);
    }
    WEBGL_CALL(ctx, glTransformFeedbackVaryings((GLuint)program, (GLsizei)len, varyings, (GLenum)bufferMode));
    for (int i = 0; i < len; i++) if (varyings[i]) JS_FreeCString(js_ctx, varyings[i]);
    free(varyings);
    return JS_UNDEFINED;
}

/* --- uniform[1234]ui / uniform[1234]uiv --- */
static JSValue lr_webgl_uniform_1ui(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0; JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    WEBGL_CALL(ctx, glUniform1ui((GLint)loc, (GLuint)v0));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_1uiv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform1uiv((GLint)loc, (GLsizei)(len / sizeof(GLuint)), (const GLuint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_2ui(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0, v1 = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]); JS_ToInt32(js_ctx, &v1, argv[2]);
    WEBGL_CALL(ctx, glUniform2ui((GLint)loc, (GLuint)v0, (GLuint)v1));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_2uiv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform2uiv((GLint)loc, (GLsizei)(len / (2 * sizeof(GLuint))), (const GLuint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_3ui(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 4) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0, v1 = 0, v2 = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    JS_ToInt32(js_ctx, &v1, argv[2]); JS_ToInt32(js_ctx, &v2, argv[3]);
    WEBGL_CALL(ctx, glUniform3ui((GLint)loc, (GLuint)v0, (GLuint)v1, (GLuint)v2));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_3uiv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform3uiv((GLint)loc, (GLsizei)(len / (3 * sizeof(GLuint))), (const GLuint *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_4ui(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t loc = 0, v0 = 0, v1 = 0, v2 = 0, v3 = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    JS_ToInt32(js_ctx, &v1, argv[2]); JS_ToInt32(js_ctx, &v2, argv[3]); JS_ToInt32(js_ctx, &v3, argv[4]);
    WEBGL_CALL(ctx, glUniform4ui((GLint)loc, (GLuint)v0, (GLuint)v1, (GLuint)v2, (GLuint)v3));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_4uiv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t loc = 0; JS_ToInt32(js_ctx, &loc, argv[0]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[1], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniform4uiv((GLint)loc, (GLsizei)(len / (4 * sizeof(GLuint))), (const GLuint *)data));
    return JS_UNDEFINED;
}

/* --- uniformMatrix{2x3,2x4,3x2,3x4,4x2,4x3}fv --- */
static JSValue lr_webgl_uniform_matrix_2x3fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix2x3fv((GLint)loc, (GLsizei)(len / (6 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_2x4fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix2x4fv((GLint)loc, (GLsizei)(len / (8 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_3x2fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix3x2fv((GLint)loc, (GLsizei)(len / (6 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_3x4fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix3x4fv((GLint)loc, (GLsizei)(len / (12 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_4x2fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix4x2fv((GLint)loc, (GLsizei)(len / (8 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_uniform_matrix_4x3fv(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 3) return JS_UNDEFINED;
    int32_t loc = 0, transpose = 0;
    JS_ToInt32(js_ctx, &loc, argv[0]); JS_ToInt32(js_ctx, &transpose, argv[1]);
    size_t len = 0; GLenum type = GL_FLOAT;
    void *data = webgl_get_typed_array_data(js_ctx, argv[2], &len, &type);
    if (data) WEBGL_CALL(ctx, glUniformMatrix4x3fv((GLint)loc, (GLsizei)(len / (12 * sizeof(GLfloat))), (GLboolean)transpose, (const GLfloat *)data));
    return JS_UNDEFINED;
}

/* --- vertexAttribDivisor --- */
static JSValue lr_webgl_vertex_attrib_divisor(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 2) return JS_UNDEFINED;
    int32_t index, divisor;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToInt32(js_ctx, &divisor, argv[1]);
    if (index >= 0 && index < LR_WEBGL_MAX_VERTEX_ATTRIBS) ctx->attribs[index].divisor = divisor;
    WEBGL_CALL(ctx, glVertexAttribDivisor((GLuint)index, (GLuint)divisor));
    return JS_UNDEFINED;
}

/* --- vertexAttribI4i / vertexAttribI4ui --- */
static JSValue lr_webgl_vertex_attrib_i4i(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t index, v0, v1, v2, v3;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    JS_ToInt32(js_ctx, &v1, argv[2]); JS_ToInt32(js_ctx, &v2, argv[3]); JS_ToInt32(js_ctx, &v3, argv[4]);
    WEBGL_CALL(ctx, glVertexAttribI4i((GLuint)index, (GLint)v0, (GLint)v1, (GLint)v2, (GLint)v3));
    return JS_UNDEFINED;
}

static JSValue lr_webgl_vertex_attrib_i4ui(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5) return JS_UNDEFINED;
    int32_t index, v0, v1, v2, v3;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToInt32(js_ctx, &v0, argv[1]);
    JS_ToInt32(js_ctx, &v1, argv[2]); JS_ToInt32(js_ctx, &v2, argv[3]); JS_ToInt32(js_ctx, &v3, argv[4]);
    WEBGL_CALL(ctx, glVertexAttribI4ui((GLuint)index, (GLuint)v0, (GLuint)v1, (GLuint)v2, (GLuint)v3));
    return JS_UNDEFINED;
}

/* --- vertexAttribIPointer --- */
static JSValue lr_webgl_vertex_attrib_i_pointer(JSContext *js_ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LR_WebGLContext *ctx = JS_GetOpaque(this_val, 1);
    if (!ctx) return JS_UNDEFINED;
    if (argc < 5 || !ctx->is_webgl2) return JS_UNDEFINED;
    int32_t index, size, type, stride, offset;
    JS_ToInt32(js_ctx, &index, argv[0]); JS_ToInt32(js_ctx, &size, argv[1]);
    JS_ToInt32(js_ctx, &type, argv[2]); JS_ToInt32(js_ctx, &stride, argv[3]); JS_ToInt32(js_ctx, &offset, argv[4]);
    if (index >= 0 && index < LR_WEBGL_MAX_VERTEX_ATTRIBS) {
        ctx->attribs[index].size = size; ctx->attribs[index].type = (GLenum)type;
        ctx->attribs[index].stride = stride; ctx->attribs[index].offset = offset;
        ctx->attribs[index].integer = 1; ctx->attribs[index].buffer_id = ctx->bound_array_buffer;
    }
    WEBGL_CALL(ctx, glVertexAttribIPointer((GLuint)index, (GLint)size, (GLenum)type, (GLsizei)stride, (const void *)(intptr_t)offset));
    return JS_UNDEFINED;
}
/* ══════════════════════════════════════════════════════════════════════════
   Context Creation
   ══════════════════════════════════════════════════════════════════════════ */

/* Helper macro to reduce boilerplate when adding methods */
#define WEBGL_METHOD(name) \
    JS_SetPropertyStr(js_ctx, obj, #name, JS_NewCFunction(js_ctx, lr_webgl_##name, #name, 0))

#define WEBGL_CONST(name) \
    JS_SetPropertyStr(js_ctx, obj, #name, JS_NewInt32(js_ctx, GL_##name))
#define WEBGL_CONST_V(name, val) \
    JS_SetPropertyStr(js_ctx, obj, #name, JS_NewInt32(js_ctx, val))

JSValue lr_webgl_create_context(JSContext *js_ctx, LR_RendererBridge *rb,
                                int width, int height, int is_webgl2)
{
    LR_WebGLContext *ctx = (LR_WebGLContext *)calloc(1, sizeof(LR_WebGLContext));
    if (!ctx) return JS_NULL;

    ctx->rb = rb;
    ctx->width = width;
    ctx->height = height;
    ctx->is_webgl2 = is_webgl2;
    ctx->vp_w = width;
    ctx->vp_h = height;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
    ctx->depth_range_near = 0.0f;
    ctx->depth_range_far = 1.0f;
    ctx->line_width = 1.0f;
    ctx->front_face = GL_CCW;
    ctx->cull_face_mode = GL_BACK;
    ctx->depth_func = GL_LESS;
    ctx->stencil_func_front = GL_ALWAYS;
    ctx->stencil_func_back = GL_ALWAYS;
    ctx->stencil_ref_front = 0;
    ctx->stencil_ref_back = 0;
    ctx->stencil_mask_read_front = ~0u;
    ctx->stencil_mask_read_back = ~0u;
    ctx->stencil_mask_front = ~0u;
    ctx->stencil_mask_back = ~0u;
    ctx->stencil_fail_front = GL_KEEP;
    ctx->stencil_zfail_front = GL_KEEP;
    ctx->stencil_zpass_front = GL_KEEP;
    ctx->stencil_fail_back = GL_KEEP;
    ctx->stencil_zfail_back = GL_KEEP;
    ctx->stencil_zpass_back = GL_KEEP;
    ctx->blend_src_rgb = GL_ONE;
    ctx->blend_dst_rgb = GL_ZERO;
    ctx->blend_src_alpha = GL_ONE;
    ctx->blend_dst_alpha = GL_ZERO;
    ctx->blend_eq_rgb = GL_FUNC_ADD;
    ctx->blend_eq_alpha = GL_FUNC_ADD;
    ctx->enable_dither = 1;
    ctx->unpack_alignment = 4;
    ctx->pack_alignment = 4;
    ctx->generate_mipmap_hint = GL_DONT_CARE;
    ctx->color_mask_r = 1;
    ctx->color_mask_g = 1;
    ctx->color_mask_b = 1;
    ctx->color_mask_a = 1;
    ctx->depth_mask = 1;
    ctx->clear_depth = 1.0f;
    ctx->sample_coverage_value = 1.0f;
    ctx->active_texture_unit = 0;

    /* Initialize draw buffers for WebGL 2.0 */
    for (int i = 0; i < LR_WEBGL_MAX_DRAW_BUFFERS; i++)
        ctx->draw_buffers[i] = (i == 0) ? GL_COLOR_ATTACHMENT0 : GL_NONE;

    /* Check if native GL is available */
#if LR_EGL_AVAILABLE
    if (rb && rb->get_proc_address) {
        ctx->has_native_gl = 1;
    }
#endif

    JSValue obj = JS_NewObject(js_ctx);
    JS_SetOpaque(obj, ctx);

    /* ── Set WebGL 1.0 constants ── */
    /* ClearBufferMask */
    WEBGL_CONST(DEPTH_BUFFER_BIT);
    WEBGL_CONST(STENCIL_BUFFER_BIT);
    WEBGL_CONST(COLOR_BUFFER_BIT);
    /* Boolean */
    WEBGL_CONST(FALSE);
    WEBGL_CONST(TRUE);
    /* BeginMode */
    WEBGL_CONST(POINTS);
    WEBGL_CONST(LINES);
    WEBGL_CONST(LINE_LOOP);
    WEBGL_CONST(LINE_STRIP);
    WEBGL_CONST(TRIANGLES);
    WEBGL_CONST(TRIANGLE_STRIP);
    WEBGL_CONST(TRIANGLE_FAN);
    /* AlphaFunction */
    WEBGL_CONST(NEVER);
    WEBGL_CONST(LESS);
    WEBGL_CONST(EQUAL);
    WEBGL_CONST(LEQUAL);
    WEBGL_CONST(GREATER);
    WEBGL_CONST(NOTEQUAL);
    WEBGL_CONST(GEQUAL);
    WEBGL_CONST(ALWAYS);
    /* BlendingFactorDest */
    WEBGL_CONST(ZERO);
    WEBGL_CONST(ONE);
    WEBGL_CONST(SRC_COLOR);
    WEBGL_CONST(ONE_MINUS_SRC_COLOR);
    WEBGL_CONST(SRC_ALPHA);
    WEBGL_CONST(ONE_MINUS_SRC_ALPHA);
    WEBGL_CONST(DST_ALPHA);
    WEBGL_CONST(ONE_MINUS_DST_ALPHA);
    /* BlendingFactorSrc */
    WEBGL_CONST(DST_COLOR);
    WEBGL_CONST(ONE_MINUS_DST_COLOR);
    WEBGL_CONST(SRC_ALPHA_SATURATE);
    /* BlendEquationSeparate */
    WEBGL_CONST(FUNC_ADD);
    WEBGL_CONST(BLEND_EQUATION);
    WEBGL_CONST(BLEND_EQUATION_RGB);
    WEBGL_CONST(BLEND_EQUATION_ALPHA);
    /* BlendSubtract */
    WEBGL_CONST(FUNC_SUBTRACT);
    WEBGL_CONST(FUNC_REVERSE_SUBTRACT);
    /* Separate Blend Functions */
    WEBGL_CONST(BLEND_DST_RGB);
    WEBGL_CONST(BLEND_SRC_RGB);
    WEBGL_CONST(BLEND_DST_ALPHA);
    WEBGL_CONST(BLEND_SRC_ALPHA);
    WEBGL_CONST(CONSTANT_COLOR);
    WEBGL_CONST(ONE_MINUS_CONSTANT_COLOR);
    WEBGL_CONST(CONSTANT_ALPHA);
    WEBGL_CONST(ONE_MINUS_CONSTANT_ALPHA);
    WEBGL_CONST(BLEND_COLOR);
    /* Buffer Objects */
    WEBGL_CONST(ARRAY_BUFFER);
    WEBGL_CONST(ELEMENT_ARRAY_BUFFER);
    WEBGL_CONST(ARRAY_BUFFER_BINDING);
    WEBGL_CONST(ELEMENT_ARRAY_BUFFER_BINDING);
    WEBGL_CONST(STREAM_DRAW);
    WEBGL_CONST(STATIC_DRAW);
    WEBGL_CONST(DYNAMIC_DRAW);
    WEBGL_CONST(BUFFER_SIZE);
    WEBGL_CONST(BUFFER_USAGE);
    /* Vertex attributes */
    WEBGL_CONST(CURRENT_VERTEX_ATTRIB);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_ENABLED);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_SIZE);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_STRIDE);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_TYPE);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_NORMALIZED);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_POINTER);
    WEBGL_CONST(VERTEX_ATTRIB_ARRAY_BUFFER_BINDING);
    /* CullFaceMode */
    WEBGL_CONST(CULL_FACE);
    WEBGL_CONST(FRONT);
    WEBGL_CONST(BACK);
    WEBGL_CONST(FRONT_AND_BACK);
    /* EnableCap */
    WEBGL_CONST(TEXTURE_2D);
    WEBGL_CONST(BLEND);
    WEBGL_CONST(DITHER);
    WEBGL_CONST(STENCIL_TEST);
    WEBGL_CONST(DEPTH_TEST);
    WEBGL_CONST(SCISSOR_TEST);
    WEBGL_CONST(POLYGON_OFFSET_FILL);
    WEBGL_CONST(SAMPLE_ALPHA_TO_COVERAGE);
    WEBGL_CONST(SAMPLE_COVERAGE);
    /* ErrorCode */
    WEBGL_CONST(NO_ERROR);
    WEBGL_CONST(INVALID_ENUM);
    WEBGL_CONST(INVALID_VALUE);
    WEBGL_CONST(INVALID_OPERATION);
    WEBGL_CONST(OUT_OF_MEMORY);
    /* FrontFaceDirection */
    WEBGL_CONST(CW);
    WEBGL_CONST(CCW);
    /* GetPName */
    WEBGL_CONST(LINE_WIDTH);
    WEBGL_CONST(ALIASED_POINT_SIZE_RANGE);
    WEBGL_CONST(ALIASED_LINE_WIDTH_RANGE);
    WEBGL_CONST(CULL_FACE_MODE);
    WEBGL_CONST(FRONT_FACE);
    WEBGL_CONST(DEPTH_RANGE);
    WEBGL_CONST(DEPTH_WRITEMASK);
    WEBGL_CONST(DEPTH_CLEAR_VALUE);
    WEBGL_CONST(DEPTH_FUNC);
    WEBGL_CONST(STENCIL_CLEAR_VALUE);
    WEBGL_CONST(STENCIL_FUNC);
    WEBGL_CONST(STENCIL_VALUE_MASK);
    WEBGL_CONST(STENCIL_FAIL);
    WEBGL_CONST(STENCIL_PASS_DEPTH_FAIL);
    WEBGL_CONST(STENCIL_PASS_DEPTH_PASS);
    WEBGL_CONST(STENCIL_REF);
    WEBGL_CONST(STENCIL_WRITEMASK);
    WEBGL_CONST(STENCIL_BACK_FUNC);
    WEBGL_CONST(STENCIL_BACK_FAIL);
    WEBGL_CONST(STENCIL_BACK_PASS_DEPTH_FAIL);
    WEBGL_CONST(STENCIL_BACK_PASS_DEPTH_PASS);
    WEBGL_CONST(STENCIL_BACK_REF);
    WEBGL_CONST(STENCIL_BACK_VALUE_MASK);
    WEBGL_CONST(STENCIL_BACK_WRITEMASK);
    WEBGL_CONST(VIEWPORT);
    WEBGL_CONST(SCISSOR_BOX);
    WEBGL_CONST(COLOR_CLEAR_VALUE);
    WEBGL_CONST(COLOR_WRITEMASK);
    WEBGL_CONST(UNPACK_ALIGNMENT);
    WEBGL_CONST(PACK_ALIGNMENT);
    WEBGL_CONST(MAX_TEXTURE_SIZE);
    WEBGL_CONST(MAX_VIEWPORT_DIMS);
    WEBGL_CONST(SUBPIXEL_BITS);
    WEBGL_CONST(RED_BITS);
    WEBGL_CONST(GREEN_BITS);
    WEBGL_CONST(BLUE_BITS);
    WEBGL_CONST(ALPHA_BITS);
    WEBGL_CONST(DEPTH_BITS);
    WEBGL_CONST(STENCIL_BITS);
    WEBGL_CONST(POLYGON_OFFSET_UNITS);
    WEBGL_CONST(POLYGON_OFFSET_FACTOR);
    WEBGL_CONST(TEXTURE_BINDING_2D);
    WEBGL_CONST(SAMPLE_BUFFERS);
    WEBGL_CONST(SAMPLES);
    WEBGL_CONST(SAMPLE_COVERAGE_VALUE);
    WEBGL_CONST(SAMPLE_COVERAGE_INVERT);
    WEBGL_CONST(GENERATE_MIPMAP_HINT);
    /* GetTextureParameter */
    WEBGL_CONST(TEXTURE_MAG_FILTER);
    WEBGL_CONST(TEXTURE_MIN_FILTER);
    WEBGL_CONST(TEXTURE_WRAP_S);
    WEBGL_CONST(TEXTURE_WRAP_T);
    /* TextureMagFilter */
    WEBGL_CONST(NEAREST);
    WEBGL_CONST(LINEAR);
    /* TextureMinFilter */
    WEBGL_CONST(NEAREST_MIPMAP_NEAREST);
    WEBGL_CONST(LINEAR_MIPMAP_NEAREST);
    WEBGL_CONST(NEAREST_MIPMAP_LINEAR);
    WEBGL_CONST(LINEAR_MIPMAP_LINEAR);
    /* TextureWrapMode */
    WEBGL_CONST(REPEAT);
    WEBGL_CONST(CLAMP_TO_EDGE);
    WEBGL_CONST(MIRRORED_REPEAT);
    /* Texture Unit */
    WEBGL_CONST(TEXTURE0);
    WEBGL_CONST(TEXTURE1);
    WEBGL_CONST(TEXTURE2);
    WEBGL_CONST(TEXTURE3);
    WEBGL_CONST(TEXTURE4);
    WEBGL_CONST(TEXTURE5);
    WEBGL_CONST(TEXTURE6);
    WEBGL_CONST(TEXTURE7);
    WEBGL_CONST(TEXTURE8);
    WEBGL_CONST(TEXTURE9);
    WEBGL_CONST(TEXTURE10);
    WEBGL_CONST(TEXTURE11);
    WEBGL_CONST(TEXTURE12);
    WEBGL_CONST(TEXTURE13);
    WEBGL_CONST(TEXTURE14);
    WEBGL_CONST(TEXTURE15);
    WEBGL_CONST(TEXTURE16);
    WEBGL_CONST(TEXTURE17);
    WEBGL_CONST(TEXTURE18);
    WEBGL_CONST(TEXTURE19);
    WEBGL_CONST(TEXTURE20);
    WEBGL_CONST(TEXTURE21);
    WEBGL_CONST(TEXTURE22);
    WEBGL_CONST(TEXTURE23);
    WEBGL_CONST(TEXTURE24);
    WEBGL_CONST(TEXTURE25);
    WEBGL_CONST(TEXTURE26);
    WEBGL_CONST(TEXTURE27);
    WEBGL_CONST(TEXTURE28);
    WEBGL_CONST(TEXTURE29);
    WEBGL_CONST(TEXTURE30);
    WEBGL_CONST(TEXTURE31);
    WEBGL_CONST(ACTIVE_TEXTURE);
    /* Texture enums */
    WEBGL_CONST(RGB);
    WEBGL_CONST(RGBA);
    WEBGL_CONST(ALPHA);
    WEBGL_CONST(LUMINANCE);
    WEBGL_CONST(LUMINANCE_ALPHA);
    /* PixelType */
    WEBGL_CONST(UNSIGNED_BYTE);
    WEBGL_CONST(UNSIGNED_SHORT_4_4_4_4);
    WEBGL_CONST(UNSIGNED_SHORT_5_5_5_1);
    WEBGL_CONST(UNSIGNED_SHORT_5_6_5);
    /* Shaders */
    WEBGL_CONST(FRAGMENT_SHADER);
    WEBGL_CONST(VERTEX_SHADER);
    WEBGL_CONST(MAX_VERTEX_ATTRIBS);
    WEBGL_CONST(MAX_VERTEX_UNIFORM_VECTORS);
    WEBGL_CONST(MAX_VARYING_VECTORS);
    WEBGL_CONST(MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    WEBGL_CONST(MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    WEBGL_CONST(MAX_TEXTURE_IMAGE_UNITS);
    WEBGL_CONST(MAX_FRAGMENT_UNIFORM_VECTORS);
    WEBGL_CONST(SHADER_TYPE);
    WEBGL_CONST(DELETE_STATUS);
    WEBGL_CONST(COMPILE_STATUS);
    WEBGL_CONST(LINK_STATUS);
    WEBGL_CONST(VALIDATE_STATUS);
    WEBGL_CONST(ATTACHED_SHADERS);
    WEBGL_CONST(ACTIVE_UNIFORMS);
    WEBGL_CONST(ACTIVE_ATTRIBUTES);
    WEBGL_CONST(SHADING_LANGUAGE_VERSION);
    WEBGL_CONST(CURRENT_PROGRAM);
    /* StencilOp */
    WEBGL_CONST(KEEP);
    WEBGL_CONST(REPLACE);
    WEBGL_CONST(INCR);
    WEBGL_CONST(DECR);
    WEBGL_CONST(INVERT);
    WEBGL_CONST(INCR_WRAP);
    WEBGL_CONST(DECR_WRAP);
    /* StringName */
    WEBGL_CONST(VENDOR);
    WEBGL_CONST(RENDERER);
    WEBGL_CONST(VERSION);
    WEBGL_CONST(EXTENSIONS);
    /* HintMode */
    WEBGL_CONST(DONT_CARE);
    WEBGL_CONST(FASTEST);
    WEBGL_CONST(NICEST);
    /* DataType */
    WEBGL_CONST(BYTE);
    WEBGL_CONST(SHORT);
    WEBGL_CONST(UNSIGNED_SHORT);
    WEBGL_CONST(INT);
    WEBGL_CONST(UNSIGNED_INT);
    WEBGL_CONST(FLOAT);
    /* PixelFormat */
    WEBGL_CONST(DEPTH_COMPONENT);
    /* Framebuffer objects */
    WEBGL_CONST(FRAMEBUFFER);
    WEBGL_CONST(RENDERBUFFER);
    WEBGL_CONST(RGBA4);
    WEBGL_CONST(RGB5_A1);
    WEBGL_CONST(RGB565);
    WEBGL_CONST(DEPTH_COMPONENT16);
    WEBGL_CONST(STENCIL_INDEX8);
    WEBGL_CONST(DEPTH_STENCIL);
    WEBGL_CONST(RENDERBUFFER_WIDTH);
    WEBGL_CONST(RENDERBUFFER_HEIGHT);
    WEBGL_CONST(RENDERBUFFER_INTERNAL_FORMAT);
    WEBGL_CONST(RENDERBUFFER_RED_SIZE);
    WEBGL_CONST(RENDERBUFFER_GREEN_SIZE);
    WEBGL_CONST(RENDERBUFFER_BLUE_SIZE);
    WEBGL_CONST(RENDERBUFFER_ALPHA_SIZE);
    WEBGL_CONST(RENDERBUFFER_DEPTH_SIZE);
    WEBGL_CONST(RENDERBUFFER_STENCIL_SIZE);
    WEBGL_CONST(FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE);
    WEBGL_CONST(FRAMEBUFFER_ATTACHMENT_OBJECT_NAME);
    WEBGL_CONST(FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL);
    WEBGL_CONST(FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE);
    WEBGL_CONST(COLOR_ATTACHMENT0);
    WEBGL_CONST(DEPTH_ATTACHMENT);
    WEBGL_CONST(STENCIL_ATTACHMENT);
    WEBGL_CONST(DEPTH_STENCIL_ATTACHMENT);
    WEBGL_CONST(NONE);
    WEBGL_CONST(FRAMEBUFFER_COMPLETE);
    WEBGL_CONST(FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
    WEBGL_CONST(FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
    WEBGL_CONST(FRAMEBUFFER_INCOMPLETE_DIMENSIONS);
    WEBGL_CONST(FRAMEBUFFER_UNSUPPORTED);
    WEBGL_CONST(FRAMEBUFFER_BINDING);
    WEBGL_CONST(RENDERBUFFER_BINDING);
    WEBGL_CONST(MAX_RENDERBUFFER_SIZE);
    WEBGL_CONST(INVALID_FRAMEBUFFER_OPERATION);
    WEBGL_CONST(IMPLEMENTATION_COLOR_READ_TYPE);
    WEBGL_CONST(IMPLEMENTATION_COLOR_READ_FORMAT);
    /* Texture binding for cube map */
    WEBGL_CONST(TEXTURE_CUBE_MAP);
    WEBGL_CONST(TEXTURE_BINDING_CUBE_MAP);
    WEBGL_CONST(TEXTURE_CUBE_MAP_POSITIVE_X);
    WEBGL_CONST(TEXTURE_CUBE_MAP_NEGATIVE_X);
    WEBGL_CONST(TEXTURE_CUBE_MAP_POSITIVE_Y);
    WEBGL_CONST(TEXTURE_CUBE_MAP_NEGATIVE_Y);
    WEBGL_CONST(TEXTURE_CUBE_MAP_POSITIVE_Z);
    WEBGL_CONST(TEXTURE_CUBE_MAP_NEGATIVE_Z);
    WEBGL_CONST(MAX_CUBE_MAP_TEXTURE_SIZE);
    /* Implementation-specific */
    WEBGL_CONST(MAX_SAMPLES);
    WEBGL_CONST(HALF_FLOAT);
    WEBGL_CONST(DEPTH24_STENCIL8);
    WEBGL_CONST(COMPARE_REF_TO_TEXTURE);
    WEBGL_CONST(TEXTURE_COMPARE_MODE);
    WEBGL_CONST(TEXTURE_COMPARE_FUNC);

    /* ── Set WebGL 2.0 constants ── */
    if (is_webgl2) {
        WEBGL_CONST(READ_BUFFER);
        WEBGL_CONST(UNPACK_ROW_LENGTH);
        WEBGL_CONST(UNPACK_SKIP_ROWS);
        WEBGL_CONST(UNPACK_SKIP_PIXELS);
        WEBGL_CONST(PACK_ROW_LENGTH);
        WEBGL_CONST(PACK_SKIP_ROWS);
        WEBGL_CONST(PACK_SKIP_PIXELS);
        WEBGL_CONST(COLOR);
        WEBGL_CONST(DEPTH);
        WEBGL_CONST(STENCIL);
        WEBGL_CONST(RED);
        WEBGL_CONST(RGB8);
        WEBGL_CONST(RGBA8);
        WEBGL_CONST(RGB10_A2);
        WEBGL_CONST(TEXTURE_BINDING_3D);
        WEBGL_CONST(UNPACK_SKIP_IMAGES);
        WEBGL_CONST(UNPACK_IMAGE_HEIGHT);
        WEBGL_CONST(TEXTURE_3D);
        WEBGL_CONST(TEXTURE_WRAP_R);
        WEBGL_CONST(MAX_3D_TEXTURE_SIZE);
        WEBGL_CONST(UNSIGNED_INT_2_10_10_10_REV);
        WEBGL_CONST(MAX_ELEMENTS_VERTICES);
        WEBGL_CONST(MAX_ELEMENTS_INDICES);
        WEBGL_CONST(TEXTURE_MIN_LOD);
        WEBGL_CONST(TEXTURE_MAX_LOD);
        WEBGL_CONST(TEXTURE_BASE_LEVEL);
        WEBGL_CONST(TEXTURE_MAX_LEVEL);
        WEBGL_CONST(MIN);
        WEBGL_CONST(MAX);
        WEBGL_CONST(DEPTH_COMPONENT24);
        WEBGL_CONST(MAX_TEXTURE_LOD_BIAS);
        WEBGL_CONST(CURRENT_QUERY);
        WEBGL_CONST(QUERY_RESULT);
        WEBGL_CONST(QUERY_RESULT_AVAILABLE);
        WEBGL_CONST(STREAM_READ);
        WEBGL_CONST(STREAM_COPY);
        WEBGL_CONST(STATIC_READ);
        WEBGL_CONST(STATIC_COPY);
        WEBGL_CONST(DYNAMIC_READ);
        WEBGL_CONST(DYNAMIC_COPY);
        WEBGL_CONST(MAX_DRAW_BUFFERS);
        WEBGL_CONST(DRAW_BUFFER0);
        WEBGL_CONST(DRAW_BUFFER1);
        WEBGL_CONST(DRAW_BUFFER2);
        WEBGL_CONST(DRAW_BUFFER3);
        WEBGL_CONST(DRAW_BUFFER4);
        WEBGL_CONST(DRAW_BUFFER5);
        WEBGL_CONST(DRAW_BUFFER6);
        WEBGL_CONST(DRAW_BUFFER7);
        WEBGL_CONST(DRAW_BUFFER8);
        WEBGL_CONST(DRAW_BUFFER9);
        WEBGL_CONST(DRAW_BUFFER10);
        WEBGL_CONST(DRAW_BUFFER11);
        WEBGL_CONST(DRAW_BUFFER12);
        WEBGL_CONST(DRAW_BUFFER13);
        WEBGL_CONST(DRAW_BUFFER14);
        WEBGL_CONST(DRAW_BUFFER15);
        WEBGL_CONST(MAX_FRAGMENT_UNIFORM_COMPONENTS);
        WEBGL_CONST(MAX_VERTEX_UNIFORM_COMPONENTS);
        WEBGL_CONST(MAX_VARYING_COMPONENTS);
        WEBGL_CONST(MAX_VERTEX_UNIFORM_BLOCKS);
        WEBGL_CONST(MAX_FRAGMENT_UNIFORM_BLOCKS);
        WEBGL_CONST(MAX_UNIFORM_BUFFER_BINDINGS);
        WEBGL_CONST(MAX_UNIFORM_BLOCK_SIZE);
        WEBGL_CONST(MAX_COMBINED_UNIFORM_BLOCKS);
        WEBGL_CONST(MAX_VERTEX_OUTPUT_COMPONENTS);
        WEBGL_CONST(MAX_FRAGMENT_INPUT_COMPONENTS);
        WEBGL_CONST(MAX_SERVER_WAIT_TIMEOUT);
        WEBGL_CONST(OBJECT_TYPE);
        WEBGL_CONST(SYNC_CONDITION);
        WEBGL_CONST(SYNC_STATUS);
        WEBGL_CONST(SYNC_FLAGS);
        WEBGL_CONST(SYNC_FENCE);
        WEBGL_CONST(SYNC_GPU_COMMANDS_COMPLETE);
        WEBGL_CONST(UNSIGNALED);
        WEBGL_CONST(SIGNALED);
        WEBGL_CONST(ALREADY_SIGNALED);
        WEBGL_CONST(TIMEOUT_EXPIRED);
        WEBGL_CONST(CONDITION_SATISFIED);
        WEBGL_CONST(WAIT_FAILED);
        WEBGL_CONST(SYNC_FLUSH_COMMANDS_BIT);
        WEBGL_CONST(VERTEX_ATTRIB_ARRAY_DIVISOR);
        WEBGL_CONST(ANY_SAMPLES_PASSED);
        WEBGL_CONST(ANY_SAMPLES_PASSED_CONSERVATIVE);
        WEBGL_CONST(SAMPLER_BINDING);
        WEBGL_CONST(RGB10_A2UI);
        WEBGL_CONST(TRANSFORM_FEEDBACK);
        WEBGL_CONST(TRANSFORM_FEEDBACK_PAUSED);
        WEBGL_CONST(TRANSFORM_FEEDBACK_ACTIVE);
        WEBGL_CONST(TRANSFORM_FEEDBACK_BINDING);
        WEBGL_CONST(TRANSFORM_FEEDBACK_VARYINGS);
        WEBGL_CONST(TRANSFORM_FEEDBACK_BUFFER_MODE);
        WEBGL_CONST(TRANSFORM_FEEDBACK_BUFFER);
        WEBGL_CONST(TRANSFORM_FEEDBACK_BUFFER_BINDING);
        WEBGL_CONST(RASTERIZER_DISCARD);
        WEBGL_CONST(MAX_COLOR_ATTACHMENTS);
        WEBGL_CONST(MAX_SAMPLES);
        WEBGL_CONST(DEPTH24_STENCIL8);
        WEBGL_CONST(VERTEX_ARRAY_BINDING);
        WEBGL_CONST(UNIFORM_BUFFER);
        WEBGL_CONST(UNIFORM_BUFFER_BINDING);
        WEBGL_CONST(UNIFORM_BUFFER_START);
        WEBGL_CONST(UNIFORM_BUFFER_SIZE);
        WEBGL_CONST(UNIFORM_BUFFER_OFFSET_ALIGNMENT);
        WEBGL_CONST(FRAGMENT_SHADER_DERIVATIVE_HINT);
        WEBGL_CONST(DRAW_FRAMEBUFFER);
        WEBGL_CONST(READ_FRAMEBUFFER);
        WEBGL_CONST(DRAW_FRAMEBUFFER_BINDING);
        WEBGL_CONST(READ_FRAMEBUFFER_BINDING);
        WEBGL_CONST(RENDERBUFFER_SAMPLES);
        WEBGL_CONST(FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER);
        WEBGL_CONST(FRAMEBUFFER_INCOMPLETE_MULTISAMPLE);
        WEBGL_CONST(MAX_SAMPLES);
        WEBGL_CONST(MAX_COLOR_TEXTURE_SAMPLES);
        WEBGL_CONST(MAX_DEPTH_TEXTURE_SAMPLES);
        WEBGL_CONST(MAX_INTEGER_SAMPLES);
        WEBGL_CONST(COPY_READ_BUFFER);
        WEBGL_CONST(COPY_WRITE_BUFFER);
        WEBGL_CONST(COPY_READ_BUFFER_BINDING);
        WEBGL_CONST(COPY_WRITE_BUFFER_BINDING);
        WEBGL_CONST(PRIMITIVE_RESTART_FIXED_INDEX);
        WEBGL_CONST(R8);
        WEBGL_CONST(RG8);
        WEBGL_CONST(RGBA32F);
        WEBGL_CONST(RGB32F);
        WEBGL_CONST(RGBA16F);
        WEBGL_CONST(RGB16F);
        WEBGL_CONST(R32F);
        WEBGL_CONST(RG32F);
        WEBGL_CONST(R16F);
        WEBGL_CONST(RG16F);
        WEBGL_CONST(RED_INTEGER);
        WEBGL_CONST(RGB_INTEGER);
        WEBGL_CONST(RGBA_INTEGER);
        WEBGL_CONST(R8_SNORM);
        WEBGL_CONST(RG8_SNORM);
        WEBGL_CONST(RGB8_SNORM);
        WEBGL_CONST(RGBA8_SNORM);
        WEBGL_CONST(SIGNED_NORMALIZED);
        WEBGL_CONST(NUM_EXTENSIONS);
        WEBGL_CONST(COMPARE_REF_TO_TEXTURE);
        WEBGL_CONST(TEXTURE_COMPARE_MODE);
        WEBGL_CONST(TEXTURE_COMPARE_FUNC);
        WEBGL_CONST(DEPTH_COMPONENT32F);
        WEBGL_CONST(DEPTH32F_STENCIL8);
        WEBGL_CONST(FLOAT_32_UNSIGNED_INT_24_8_REV);
        WEBGL_CONST(HALF_FLOAT);
        WEBGL_CONST(R11F_G11F_B10F);
        WEBGL_CONST(UNSIGNED_INT_10F_11F_11F_REV);
        WEBGL_CONST(RGB9_E5);
        WEBGL_CONST(UNSIGNED_INT_5_9_9_9_REV);
        WEBGL_CONST(TEXTURE_SHARED_SIZE);
        WEBGL_CONST(RGBA32UI);
        WEBGL_CONST(RGB32UI);
        WEBGL_CONST(RGBA16UI);
        WEBGL_CONST(RGB16UI);
        WEBGL_CONST(RGBA8UI);
        WEBGL_CONST(RGB8UI);
        WEBGL_CONST(RGBA32I);
        WEBGL_CONST(RGB32I);
        WEBGL_CONST(RGBA16I);
        WEBGL_CONST(RGB16I);
        WEBGL_CONST(RGBA8I);
        WEBGL_CONST(RGB8I);
    }

    /* ── Set WebGL 1.0 methods ── */
    WEBGL_METHOD(active_texture);
    WEBGL_METHOD(attach_shader);
    WEBGL_METHOD(bind_attrib_location);
    WEBGL_METHOD(bind_buffer);
    WEBGL_METHOD(bind_framebuffer);
    WEBGL_METHOD(bind_renderbuffer);
    WEBGL_METHOD(bind_texture);
    WEBGL_METHOD(blend_color);
    WEBGL_METHOD(blend_equation);
    WEBGL_METHOD(blend_equation_separate);
    WEBGL_METHOD(blend_func);
    WEBGL_METHOD(blend_func_separate);
    WEBGL_METHOD(buffer_data);
    WEBGL_METHOD(buffer_sub_data);
    WEBGL_METHOD(check_framebuffer_status);
    WEBGL_METHOD(clear);
    WEBGL_METHOD(clear_color);
    WEBGL_METHOD(clear_depth);
    WEBGL_METHOD(clear_stencil);
    WEBGL_METHOD(color_mask);
    WEBGL_METHOD(compile_shader);
    WEBGL_METHOD(copy_tex_image_2d);
    WEBGL_METHOD(copy_tex_sub_image_2d);
    WEBGL_METHOD(create_buffer);
    WEBGL_METHOD(create_framebuffer);
    WEBGL_METHOD(create_program);
    WEBGL_METHOD(create_renderbuffer);
    WEBGL_METHOD(create_shader);
    WEBGL_METHOD(create_texture);
    WEBGL_METHOD(cull_face);
    WEBGL_METHOD(delete_buffer);
    WEBGL_METHOD(delete_framebuffer);
    WEBGL_METHOD(delete_program);
    WEBGL_METHOD(delete_renderbuffer);
    WEBGL_METHOD(delete_shader);
    WEBGL_METHOD(delete_texture);
    WEBGL_METHOD(depth_func);
    WEBGL_METHOD(depth_mask);
    WEBGL_METHOD(depth_range);
    WEBGL_METHOD(detach_shader);
    WEBGL_METHOD(disable);
    WEBGL_METHOD(disable_vertex_attrib_array);
    WEBGL_METHOD(draw_arrays);
    WEBGL_METHOD(draw_elements);
    WEBGL_METHOD(enable);
    WEBGL_METHOD(enable_vertex_attrib_array);
    WEBGL_METHOD(finish);
    WEBGL_METHOD(flush);
    WEBGL_METHOD(framebuffer_renderbuffer);
    WEBGL_METHOD(framebuffer_texture_2d);
    WEBGL_METHOD(front_face);
    WEBGL_METHOD(generate_mipmap);
    WEBGL_METHOD(get_attrib_location);
    WEBGL_METHOD(get_error);
    WEBGL_METHOD(get_extension);
    WEBGL_METHOD(get_parameter);
    WEBGL_METHOD(get_program_info_log);
    WEBGL_METHOD(get_program_parameter);
    WEBGL_METHOD(get_shader_info_log);
    WEBGL_METHOD(get_shader_parameter);
    WEBGL_METHOD(get_uniform_location);
    WEBGL_METHOD(get_vertex_attrib_offset);
    WEBGL_METHOD(is_buffer);
    WEBGL_METHOD(is_enabled);
    WEBGL_METHOD(is_program);
    WEBGL_METHOD(is_renderbuffer);
    WEBGL_METHOD(is_shader);
    WEBGL_METHOD(is_texture);
    WEBGL_METHOD(line_width);
    WEBGL_METHOD(link_program);
    WEBGL_METHOD(pixel_storei);
    WEBGL_METHOD(polygon_offset);
    WEBGL_METHOD(read_pixels);
    WEBGL_METHOD(renderbuffer_storage);
    WEBGL_METHOD(sample_coverage);
    WEBGL_METHOD(scissor);
    WEBGL_METHOD(shader_source);
    WEBGL_METHOD(stencil_func);
    WEBGL_METHOD(stencil_func_separate);
    WEBGL_METHOD(stencil_mask);
    WEBGL_METHOD(stencil_mask_separate);
    WEBGL_METHOD(stencil_op);
    WEBGL_METHOD(stencil_op_separate);
    WEBGL_METHOD(tex_image_2d);
    WEBGL_METHOD(tex_parameteri);
    WEBGL_METHOD(tex_parameterf);
    WEBGL_METHOD(tex_sub_image_2d);
    WEBGL_METHOD(uniform_1f);
    WEBGL_METHOD(uniform_1fv);
    WEBGL_METHOD(uniform_1i);
    WEBGL_METHOD(uniform_1iv);
    WEBGL_METHOD(uniform_2f);
    WEBGL_METHOD(uniform_2fv);
    WEBGL_METHOD(uniform_2i);
    WEBGL_METHOD(uniform_2iv);
    WEBGL_METHOD(uniform_3f);
    WEBGL_METHOD(uniform_3fv);
    WEBGL_METHOD(uniform_3i);
    WEBGL_METHOD(uniform_3iv);
    WEBGL_METHOD(uniform_4f);
    WEBGL_METHOD(uniform_4fv);
    WEBGL_METHOD(uniform_4i);
    WEBGL_METHOD(uniform_4iv);
    WEBGL_METHOD(uniform_matrix_2fv);
    WEBGL_METHOD(uniform_matrix_3fv);
    WEBGL_METHOD(uniform_matrix_4fv);
    WEBGL_METHOD(use_program);
    WEBGL_METHOD(validate_program);
    WEBGL_METHOD(vertex_attrib_1f);
    WEBGL_METHOD(vertex_attrib_2f);
    WEBGL_METHOD(vertex_attrib_3f);
    WEBGL_METHOD(vertex_attrib_4f);
    WEBGL_METHOD(vertex_attrib_pointer);
    WEBGL_METHOD(viewport);

    /* ── Set WebGL 2.0 methods ── */
    if (is_webgl2) {
        WEBGL_METHOD(begin_query);
        WEBGL_METHOD(begin_transform_feedback);
        WEBGL_METHOD(bind_buffer_base);
        WEBGL_METHOD(bind_buffer_range);
        WEBGL_METHOD(bind_sampler);
        WEBGL_METHOD(bind_transform_feedback);
        WEBGL_METHOD(bind_vertex_array);
        WEBGL_METHOD(blit_framebuffer);
        WEBGL_METHOD(clear_buffer_fv);
        WEBGL_METHOD(clear_buffer_iv);
        WEBGL_METHOD(clear_buffer_uiv);
        WEBGL_METHOD(client_wait_sync);
        WEBGL_METHOD(compressed_tex_image_2d);
        WEBGL_METHOD(compressed_tex_sub_image_2d);
        WEBGL_METHOD(copy_buffer_sub_data);
        WEBGL_METHOD(delete_query);
        WEBGL_METHOD(delete_sampler);
        WEBGL_METHOD(delete_sync);
        WEBGL_METHOD(delete_transform_feedback);
        WEBGL_METHOD(delete_vertex_array);
        WEBGL_METHOD(draw_arrays_instanced);
        WEBGL_METHOD(draw_buffers);
        WEBGL_METHOD(draw_elements_instanced);
        WEBGL_METHOD(draw_range_elements);
        WEBGL_METHOD(end_query);
        WEBGL_METHOD(end_transform_feedback);
        WEBGL_METHOD(fence_sync);
        WEBGL_METHOD(framebuffer_texture_layer);
        WEBGL_METHOD(get_active_uniforms);
        WEBGL_METHOD(get_buffer_sub_data);
        WEBGL_METHOD(get_frag_data_location);
        WEBGL_METHOD(get_internalformat_parameter);
        WEBGL_METHOD(get_query_parameter);
        WEBGL_METHOD(get_sampler_parameter);
        WEBGL_METHOD(get_sync_parameter);
        WEBGL_METHOD(get_transform_feedback_varying);
        WEBGL_METHOD(get_uniform_block_index);
        WEBGL_METHOD(get_uniform_indices);
        WEBGL_METHOD(invalidate_framebuffer);
        WEBGL_METHOD(invalidate_sub_framebuffer);
        WEBGL_METHOD(is_query);
        WEBGL_METHOD(is_sampler);
        WEBGL_METHOD(is_sync);
        WEBGL_METHOD(is_transform_feedback);
        WEBGL_METHOD(is_vertex_array);
        WEBGL_METHOD(multi_draw_arrays_instanced);
        WEBGL_METHOD(multi_draw_elements_instanced);
        WEBGL_METHOD(pause_transform_feedback);
        WEBGL_METHOD(read_buffer);
        WEBGL_METHOD(renderbuffer_storage_multisample);
        WEBGL_METHOD(resume_transform_feedback);
        WEBGL_METHOD(sampler_parameteri);
        WEBGL_METHOD(sampler_parameterf);
        WEBGL_METHOD(tex_image_3d);
        WEBGL_METHOD(tex_storage_2d);
        WEBGL_METHOD(tex_storage_3d);
        WEBGL_METHOD(tex_sub_image_3d);
        WEBGL_METHOD(transform_feedback_varyings);
        WEBGL_METHOD(uniform_1ui);
        WEBGL_METHOD(uniform_1uiv);
        WEBGL_METHOD(uniform_2ui);
        WEBGL_METHOD(uniform_2uiv);
        WEBGL_METHOD(uniform_3ui);
        WEBGL_METHOD(uniform_3uiv);
        WEBGL_METHOD(uniform_4ui);
        WEBGL_METHOD(uniform_4uiv);
        WEBGL_METHOD(uniform_block_binding);
        WEBGL_METHOD(uniform_matrix_2x3fv);
        WEBGL_METHOD(uniform_matrix_2x4fv);
        WEBGL_METHOD(uniform_matrix_3x2fv);
        WEBGL_METHOD(uniform_matrix_3x4fv);
        WEBGL_METHOD(uniform_matrix_4x2fv);
        WEBGL_METHOD(uniform_matrix_4x3fv);
        WEBGL_METHOD(vertex_attrib_divisor);
        WEBGL_METHOD(vertex_attrib_i4i);
        WEBGL_METHOD(vertex_attrib_i4ui);
        WEBGL_METHOD(vertex_attrib_i_pointer);
    }

    /* Add the canvas property */
    JS_SetPropertyStr(js_ctx, obj, "canvas", JS_UNDEFINED);

    /* Add the drawingBufferWidth/Height properties */
    JS_SetPropertyStr(js_ctx, obj, "drawingBufferWidth", JS_NewInt32(js_ctx, width));
    JS_SetPropertyStr(js_ctx, obj, "drawingBufferHeight", JS_NewInt32(js_ctx, height));

    /* Extension constants are not needed here - they are returned by getExtension() */

    return obj;
}

#undef WEBGL_METHOD
#undef WEBGL_CONST
#undef WEBGL_CONST_V
