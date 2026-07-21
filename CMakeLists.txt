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
project(lr_js VERSION 1.0.0 LANGUAGES C)

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
    # Disable noisy warnings
    add_compile_options(/wd4996 /wd4244 /wd4267 /wd4100 /wd4127)
    # Define NOMINMAX to avoid Windows.h min/max macros
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(WIN32)
    set(LR_PLATFORM_WINDOWS ON)
    set(LR_COMPILER_MSVC OFF)
else()
    set(LR_PLATFORM_WINDOWS OFF)
    set(LR_COMPILER_MSVC OFF)
    # GCC/Clang flags
    add_compile_options(-Wall -Wextra -D_GNU_SOURCE)
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
)

# ── Engine Sources ────────────────────────────────────────────────────────

set(LR_ENGINE_SRCS
    src/engine/lr_engine.c
    src/engine/lr_lexer.c
    src/engine/lr_parser.c
    src/engine/lr_interp.c
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
    src/lr_thread_pool.c
    src/lr_sandbox.c
    src/lr_renderer.c
    src/lr_renderer_egl.c
    src/lr_canvas.c
    src/lr_canvas_webgl.c
    src/lr_scheduler.c
    src/lr_perf_opt.c
    src/lr_worker.c
    src/lr_gc.c
    src/lr_bytecode.c
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

# ── CLI Sources ───────────────────────────────────────────────────────────

set(LR_CLI_SRCS
    cli/main.c
)

# ── Internal static library: lr_engine ────────────────────────────────────

add_library(lr_engine STATIC ${LR_ENGINE_SRCS})
target_include_directories(lr_engine PUBLIC ${LR_INCLUDE_DIRS})
target_compile_definitions(lr_engine PRIVATE LR_JS_BUILD_STATIC)

# ── Internal static library: lr_js_core ───────────────────────────────────

add_library(lr_js_core STATIC ${LR_RUNTIME_SRCS})
target_include_directories(lr_js_core PUBLIC ${LR_INCLUDE_DIRS})
target_link_libraries(lr_js_core PUBLIC lr_engine)
target_compile_definitions(lr_js_core PRIVATE LR_JS_BUILD_STATIC)

# ── Public static library: lr_js_static ───────────────────────────────────

add_library(lr_js_static STATIC
    ${LR_ENGINE_SRCS}
    ${LR_RUNTIME_SRCS}
)
target_include_directories(lr_js_static PUBLIC ${LR_INCLUDE_DIRS})
target_compile_definitions(lr_js_static PUBLIC LR_JS_BUILD_STATIC)
# On MSVC, link ws2_32 for sockets
if(LR_PLATFORM_WINDOWS)
    target_link_libraries(lr_js_static PUBLIC ws2_32)
endif()

# ── Public shared/DLL library: lr_js_shared ───────────────────────────────

add_library(lr_js_shared SHARED
    ${LR_ENGINE_SRCS}
    ${LR_RUNTIME_SRCS}
)
target_include_directories(lr_js_shared PUBLIC ${LR_INCLUDE_DIRS})
target_compile_definitions(lr_js_shared PRIVATE LR_JS_BUILD_DLL)
target_compile_definitions(lr_js_shared PUBLIC LR_JS_USE_DLL)
if(LR_PLATFORM_WINDOWS)
    target_link_libraries(lr_js_shared PUBLIC ws2_32)
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
    target_link_libraries(lr_js PRIVATE ws2_32)
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