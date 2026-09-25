# L/R_JS - CMake Build System
# Supports: MSVC (cl.exe), GCC, Clang
# Outputs: static library, shared/DLL library, CLI executable
#
# Usage:
#   mkdir build && cd build
#   cmake .. -G "Visual Studio 17 2022"    # MSVC
#   cmake .. -G "Unix Makefiles"            # Linux/macOS
#   cmake --build . --config Release
#
# Output structure:
#   build/lib/  → lr_js_static.lib / liblr_js.a
#                 lr_js.dll / liblr_js.so
#   build/bin/  → lr_js.exe / lr_js

cmake_minimum_required(VERSION 3.14)
project(lr_js VERSION 0.2.0 LANGUAGES C)

# ── C Standard ────────────────────────────────────────────────────────────

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Platform detection ────────────────────────────────────────────────────

if(MSVC)
    set(LR_PLATFORM_WINDOWS ON)
    set(LR_COMPILER_MSVC ON)
    # MSVC C11 mode
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /std:c11")
    # MSVC does not define NDEBUG automatically in Release
    add_compile_definitions(NDEBUG)
elseif(WIN32)
    set(LR_PLATFORM_WINDOWS ON)
    set(LR_COMPILER_MSVC OFF)
    # Define NOMINMAX to avoid Windows.h min/max macros
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # For MinGW Makefiles on Windows, default to Release if CMAKE_BUILD_TYPE is empty
    # This ensures NDEBUG is always defined unless user explicitly passes -D CMAKE_BUILD_TYPE=Debug
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        add_compile_definitions(NDEBUG)
        # Disable JIT debug logging in Release builds
        add_compile_definitions(LR_DEBUG_JIT=0)
    endif()
else()
    set(LR_PLATFORM_WINDOWS OFF)
    set(LR_COMPILER_MSVC OFF)
    # Enable LTO for cross-TU inlining (hot path functions like scope_new_inline_fast)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -flto")
    endif()
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -flto")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} -flto")
    # Suppress warnings on GCC/Clang
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-format -Wno-sign-compare)
    endif()
    # GCC/Clang flags
    # The following noisy categories are suppressed to keep the build log clean.
    # This mirrors the lenient policy already applied on MSVC (which disables a
    # long list of warnings). None of them indicate real defects in this engine:
    #   -Wno-unused-parameter   : callback APIs (ctx, this_val, argc, argv) leave
    #                             some params unused by design.
    #   -Wno-unused-function     : many internal helpers (*_data_free finalizers,
    #                             URL accessors) are compiled but only referenced
    #                             in certain build configurations.
    #   -Wno-cast-function-type  : the engine's property accessor macro
    #                             (JS_CGETSET_DEF) deliberately casts 2/3-arg getter
    #                             and setter signatures to the 4-arg LRCFunctionFunc.
    #   -Wno-unused-variable /
    #   -Wno-unused-but-set-variable : a handful of leftover locals.
    #
    # -Wno-cast-function-type and -Wno-unused-but-set-variable do not exist on
    # older Clang / AppleClang, which then emits an -Wunknown-warning-option
    # note for *every* translation unit. Probe each flag before using it.
    # Note: we probe the *positive* form (-Wfoo) with -Werror, because Clang
    # accepts an unknown -Wno-foo silently and only complains later, which would
    # make the probe useless.
    include(CheckCCompilerFlag)
    add_compile_options(-Wall -Wextra -D_GNU_SOURCE)
    set(_lr_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
    foreach(_lr_warn
            unused-parameter
            unused-function
            cast-function-type
            unused-variable
            unused-but-set-variable)
        string(MAKE_C_IDENTIFIER "LR_HAS_W_${_lr_warn}" _lr_flag_var)
        check_c_compiler_flag("-W${_lr_warn}" ${_lr_flag_var})
        if(${_lr_flag_var})
            add_compile_options("-Wno-${_lr_warn}")
        endif()
    endforeach()
    set(CMAKE_REQUIRED_FLAGS "${_lr_saved_required_flags}")
endif()

# ── Output directories ────────────────────────────────────────────────────

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ── Include directories ───────────────────────────────────────────────────

set(LR_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/pcre2
    ${CMAKE_SOURCE_DIR}/sljit/sljit_src
)

# ── Engine Sources ────────────────────────────────────────────────────────

set(LR_ENGINE_SRCS
    src/engine/lr_engine.c
    src/engine/lr_lexer.c
    src/engine/lr_parser.c
    src/engine/lr_interp.c
    src/engine/lr_jit_v2.c
    src/engine/mir/lr_jit_mir.c
    src/engine/mir/lr_jit_codegen.c
    # SLJIT JIT backend (sljitLir.c includes all needed allocators and utils)
    sljit/sljit_src/sljitLir.c
)

# ── PCRE2 Embedded Sources ────────────────────────────────────────────────

set(LR_PCRE2_SRCS
    src/pcre2/pcre2_auto_possess.c
    src/pcre2/pcre2_chartables.c
    src/pcre2/pcre2_chkdint.c
    src/pcre2/pcre2_compile.c
    src/pcre2/pcre2_compile_cgroup.c
    src/pcre2/pcre2_compile_class.c
    src/pcre2/pcre2_config.c
    src/pcre2/pcre2_context.c
    src/pcre2/pcre2_convert.c
    src/pcre2/pcre2_dfa_match.c
    src/pcre2/pcre2_error.c
    src/pcre2/pcre2_extuni.c
    src/pcre2/pcre2_find_bracket.c
    src/pcre2/pcre2_maketables.c
    src/pcre2/pcre2_match.c
    src/pcre2/pcre2_match_data.c
    src/pcre2/pcre2_match_next.c
    src/pcre2/pcre2_newline.c
    src/pcre2/pcre2_ord2utf.c
    src/pcre2/pcre2_pattern_info.c
    src/pcre2/pcre2_script_run.c
    src/pcre2/pcre2_string_utils.c
    src/pcre2/pcre2_study.c
    src/pcre2/pcre2_substitute.c
    src/pcre2/pcre2_substring.c
    src/pcre2/pcre2_tables.c
    src/pcre2/pcre2_ucd.c
    src/pcre2/pcre2_valid_utf.c
    src/pcre2/pcre2_xclass.c
    src/pcre2/pcre2_jit_compile.c
    src/pcre2/pcre2_serialize.c
    src/pcre2/pcre2posix.c
)

# ── Runtime Sources ───────────────────────────────────────────────────────

set(LR_RUNTIME_SRCS
    src/lr_runtime.c
    src/lr_console.c
    src/lr_timers.c
    src/lr_url.c
    src/lr_encoding.c
    src/lr_event.c
    src/lr_performance.c
    src/lr_crypto.c
    src/lr_storage.c
    src/lr_fetch.c
    src/lr_ws.c
    src/lr_thread_pool.c
    src/lr_sandbox.c
    src/lr_renderer.c
    src/lr_renderer_egl.c
    src/lr_canvas.c
    src/lr_canvas_webgl.c
    src/lr_dom.c
    src/lr_scheduler.c
    src/lr_perf_opt.c
    src/engine/lr_bytecode_cache.c
    src/engine/lr_bytecode_emit.c
    src/engine/lr_bytecode_compile.c
    src/engine/lr_bytecode_value.c
    src/engine/lr_bytecode_exec.c
    src/engine/lr_bytecode_serialize.c
    src/engine/lr_bytecode_disasm.c
    src/lr_worker.c
    src/lr_gc.c
    src/lr_iome586.c
    src/lr_compress.c
    src/lr_sandbox_log.c
    src/lr_promise.c
    src/lr_proxy.c
    src/lr_reflect.c
    src/lr_map.c
    src/lr_set.c
    src/lr_builtins_core.c
    src/lr_builtins_extra.c
)

# Built-in PCRE2-based regex engine (replaces system <regex.h> on all
# platforms for full compatibility with modern regex features).
list(APPEND LR_RUNTIME_SRCS src/lr_regex.c)

# Windows POSIX relay: provides dirent/utsname/sysinfo/popen wrappers so
# the Unix-oriented modules below link under MSVC and MinGW.
if(WIN32)
    list(APPEND LR_RUNTIME_SRCS src/lr_posix_win.c)
endif()

# Filesystem / terminal / sysinfo modules use POSIX APIs (dirent, utsname,
# sysinfo, popen). On MSVC they rely on the lr_posix_win.c relay above; on
# Unix they use the system libc directly. They MUST be compiled on every
# platform, otherwise lr_fs_init / lr_terminal_init / lr_sysinfo_init (called
# unconditionally from lr_runtime.c) are left undefined at link time.
list(APPEND LR_RUNTIME_SRCS
    src/lr_fs.c
    src/lr_terminal.c
    src/lr_sysinfo.c)

# ── CLI Sources ───────────────────────────────────────────────────────────

set(LR_CLI_SRCS
    cli/main.c
)

# ── Internal static library: lr_engine ────────────────────────────────────

add_library(lr_engine STATIC ${LR_ENGINE_SRCS})
target_include_directories(lr_engine PUBLIC ${LR_INCLUDE_DIRS})
target_compile_definitions(lr_engine PRIVATE LR_JS_BUILD_STATIC)
target_compile_definitions(lr_engine PRIVATE SLJIT_DEBUG=0 SLJIT_VERBOSE=1 SLJIT_ARGUMENT_CHECKS=0)
target_compile_definitions(lr_engine PRIVATE LR_JIT_ENABLED=1)

# ── Internal static library: lr_js_core ───────────────────────────────────

add_library(lr_js_core STATIC
    ${LR_RUNTIME_SRCS}
    ${LR_PCRE2_SRCS}
)
target_include_directories(lr_js_core PUBLIC ${LR_INCLUDE_DIRS})
target_link_libraries(lr_js_core PUBLIC lr_engine)
target_compile_definitions(lr_js_core PRIVATE LR_JS_BUILD_STATIC)
# PCRE2 sources need HAVE_CONFIG_H to include config.h
# and PCRE2_CODE_UNIT_WIDTH=8 for the 8-bit library
target_compile_definitions(lr_js_core PRIVATE HAVE_CONFIG_H)
target_compile_definitions(lr_js_core PRIVATE PCRE2_CODE_UNIT_WIDTH=8)

# ── Public static library: lr_js_static ───────────────────────────────────

add_library(lr_js_static STATIC
    ${LR_ENGINE_SRCS}
    ${LR_RUNTIME_SRCS}
    ${LR_PCRE2_SRCS}
)
target_include_directories(lr_js_static PUBLIC ${LR_INCLUDE_DIRS})
target_compile_definitions(lr_js_static PUBLIC LR_JS_BUILD_STATIC)
target_compile_definitions(lr_js_static PRIVATE HAVE_CONFIG_H)
target_compile_definitions(lr_js_static PRIVATE PCRE2_CODE_UNIT_WIDTH=8)
target_compile_definitions(lr_js_static PRIVATE PCRE2_STATIC)
target_compile_definitions(lr_js_static PRIVATE SLJIT_DEBUG=0 SLJIT_VERBOSE=1 SLJIT_ARGUMENT_CHECKS=0)
# On MSVC, link ws2_32 for sockets
if(LR_PLATFORM_WINDOWS)
    target_link_libraries(lr_js_static PUBLIC ws2_32 ntdll)
    if(NOT LR_COMPILER_MSVC)
        target_link_libraries(lr_js_static PUBLIC pthread m)
    endif()
    message(STATUS "JIT enabled: MIR frontend + SLJIT backend (Windows)")
else()
    target_link_libraries(lr_js_static PUBLIC m)
    message(STATUS "JIT enabled: MIR frontend + SLJIT backend (Unix)")
endif()

# ── Public shared/DLL library: lr_js_shared ───────────────────────────────

add_library(lr_js_shared SHARED
    ${LR_ENGINE_SRCS}
    ${LR_RUNTIME_SRCS}
    ${LR_PCRE2_SRCS}
)
target_include_directories(lr_js_shared PUBLIC ${LR_INCLUDE_DIRS})
target_compile_definitions(lr_js_shared PRIVATE LR_JS_BUILD_DLL)
target_compile_definitions(lr_js_shared PUBLIC LR_JS_USE_DLL)
target_compile_definitions(lr_js_shared PRIVATE HAVE_CONFIG_H)
target_compile_definitions(lr_js_shared PRIVATE PCRE2_CODE_UNIT_WIDTH=8)
target_compile_definitions(lr_js_shared PRIVATE PCRE2_STATIC)
target_compile_definitions(lr_js_shared PRIVATE SLJIT_DEBUG=0 SLJIT_VERBOSE=1 SLJIT_ARGUMENT_CHECKS=0)
target_compile_definitions(lr_js_shared PRIVATE LR_JIT_ENABLED=1)
if(LR_PLATFORM_WINDOWS)
    target_link_libraries(lr_js_shared PUBLIC ws2_32 ntdll)
    if(NOT LR_COMPILER_MSVC)
        target_link_libraries(lr_js_shared PUBLIC pthread m)
    endif()
endif()

# Set output name
set_target_properties(lr_js_shared PROPERTIES
    OUTPUT_NAME lr_js
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# ── CLI Executable ────────────────────────────────────────────────────────

add_executable(lr_js ${LR_CLI_SRCS})
target_include_directories(lr_js PRIVATE ${LR_INCLUDE_DIRS})
target_link_libraries(lr_js PRIVATE lr_js_static)
if(LR_PLATFORM_WINDOWS)
    target_link_libraries(lr_js PRIVATE ws2_32 ntdll)
    if(NOT LR_COMPILER_MSVC)
        target_link_libraries(lr_js PRIVATE pthread m)
    endif()
    # Deep JS recursion (e.g. depth-500 tail recursion in the stress tests)
    # C-recurses through interp_bc_call_function/bc_execute with ~4KB+ alloca per frame.
    # The default 1MB PE stack overflows at ~250 frames; give the CLI a 16MB stack.
    if(LR_COMPILER_MSVC)
        target_link_options(lr_js PRIVATE "/STACK:16777216")
    else()
        target_link_options(lr_js PRIVATE "-Wl,--stack,16777216")
    endif()
endif()

# On MSVC, set the executable output name
set_target_properties(lr_js PROPERTIES OUTPUT_NAME lr_js)

# ── Threading library ─────────────────────────────────────────────────────

if(NOT LR_PLATFORM_WINDOWS)
    find_package(Threads REQUIRED)
    target_link_libraries(lr_js_static PUBLIC Threads::Threads)
    target_link_libraries(lr_js_shared PUBLIC Threads::Threads)
    target_link_libraries(lr_js PRIVATE Threads::Threads)
endif()

# On Unix, need math library
if(UNIX)
    target_link_libraries(lr_js_static PUBLIC m)
    target_link_libraries(lr_js_shared PUBLIC m)
    target_link_libraries(lr_js PRIVATE m)
endif()

# ── EGL / OpenGL ES (optional) ────────────────────────────────────────────

option(LR_ENABLE_EGL "Enable EGL/GLES2 GPU-accelerated renderer" OFF)

if(LR_ENABLE_EGL)
    find_package(EGL QUIET)
    if(EGL_FOUND)
        target_compile_definitions(lr_js_core PRIVATE LR_HAS_EGL)
        target_compile_definitions(lr_js_static PUBLIC LR_HAS_EGL)
        target_compile_definitions(lr_js_shared PRIVATE LR_HAS_EGL)
        target_link_libraries(lr_js_static PUBLIC EGL::EGL)
        target_link_libraries(lr_js_shared PUBLIC EGL::EGL)
        target_link_libraries(lr_js PRIVATE EGL::EGL)
        find_package(OpenGLES2 QUIET)
        if(OpenGLES2_FOUND)
            target_link_libraries(lr_js_static PUBLIC OpenGLES2::OpenGLES2)
            target_link_libraries(lr_js_shared PUBLIC OpenGLES2::OpenGLES2)
            target_link_libraries(lr_js PRIVATE OpenGLES2::OpenGLES2)
        endif()
        message(STATUS "EGL/GLES2 GPU renderer: ENABLED")
    else()
        message(STATUS "EGL not found; GPU renderer disabled")
    endif()
else()
    message(STATUS "EGL/GLES2 GPU renderer: disabled (use -DLR_ENABLE_EGL=ON)")
endif()

# On Unix, need dl library for dynamic loading
if(UNIX)
    target_link_libraries(lr_js_static PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(lr_js_shared PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(lr_js PRIVATE ${CMAKE_DL_LIBS})
endif()

# ── Install ────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS lr_js_static lr_js_shared lr_js
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES include/lr_js.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lr_js)

# ── Platform-specific Options ──────────────────────────────────────────────

option(LR_32BIT "Build 32-bit (add -m32 flag)" OFF)
if(LR_32BIT)
    if(MSVC)
        # MSVC: use x86 toolchain
        set(CMAKE_GENERATOR_PLATFORM "Win32" CACHE STRING "Force Win32" FORCE)
    else()
        add_compile_options(-m32 -D_FILE_OFFSET_BITS=64)
        link_libraries(-m32)
    endif()
    message(STATUS "LR_JS: 32-bit build enabled")
endif()

option(LR_WIN7 "Target Windows 7+ (MinGW only, adds _WIN32_WINNT=0x0601)" OFF)
if(LR_WIN7 AND WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601)
    message(STATUS "LR_JS: Windows 7+ target enabled")
endif()

# ── Platform-specific notes ───────────────────────────────────────────────

if(LR_PLATFORM_WINDOWS)
    message(STATUS "LR_JS building for Windows")
    if(LR_COMPILER_MSVC)
        message(STATUS "  Compiler: MSVC (cl.exe)")
        message(STATUS "  Using lr_pthread_win.h compatibility layer")
    else()
        message(STATUS "  Compiler: MinGW/GCC")
    endif()
    message(STATUS "  Output: build/lib/lr_js_static.lib + build/lib/lr_js.dll + build/bin/lr_js.exe")
else()
    message(STATUS "LR_JS building for Unix (Linux/macOS)")
    message(STATUS "  Output: build/lib/liblr_js.a + build/lib/liblr_js.so + build/bin/lr_js")
endif()