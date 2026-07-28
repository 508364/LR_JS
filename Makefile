# L/R_JS - Lightweight/Runtime JavaScript Engine
# Pure C, ES2022-compatible
#
# Makefile
#
# Build targets:
#   make              Build for current platform (Linux/macOS/Windows-MSYS2)
#   make win CC=x86_64-w64-mingw32-gcc   Cross-compile for Windows 7+ (64-bit)
#   make win32 CC=i686-w64-mingw32-gcc   Cross-compile for Windows 7+ (32-bit)
#   make linux32      Build for Linux 32-bit (x86)
#   make test         Build and run tests

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
           -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-incompatible-pointer-types -Wno-discarded-qualifiers \
           -fPIC

# Architecture detection
UNAME_M := $(shell uname -m 2>/dev/null || echo "x86_64")
ifeq ($(UNAME_M),i386)
ARCH := x86
else ifeq ($(UNAME_M),i686)
ARCH := x86
else ifeq ($(UNAME_M),x86_64)
ARCH := x86_64
else ifeq ($(UNAME_M),aarch64)
ARCH := arm64
else ifeq ($(UNAME_M),armv7l)
ARCH := armv7
endif

# 32-bit Linux build (-m32 flags)
ifeq ($(ARCH32),1)
CFLAGS  += -m32 -D_FILE_OFFSET_BITS=64
LDFLAGS += -m32
endif

# Platform detection
ifeq ($(OS),Windows_NT)
    # Native Windows (MSYS2/MinGW)
    LDFLAGS = -lm -lpthread -lws2_32
    EXT     ?= .exe
else
    # Linux / macOS
    LDFLAGS = -lm -lpthread -ldl
    EXT     ?=
endif

# Engine source directory
ENGINE_DIR  = src/engine

# Project directories
SRC_DIR  = src
CLI_DIR  = cli
INC_DIR  = include
BUILD_DIR = build

# Engine sources (self-implemented)
ENGINE_SRCS = \
	$(ENGINE_DIR)/lr_engine.c \
	$(ENGINE_DIR)/lr_lexer.c \
	$(ENGINE_DIR)/lr_parser.c \
	$(ENGINE_DIR)/lr_interp.c

# L/R_JS sources (core + browser APIs)
LR_SRCS = \
	$(SRC_DIR)/lr_runtime.c \
	$(SRC_DIR)/lr_console.c \
	$(SRC_DIR)/lr_timers.c \
	$(SRC_DIR)/lr_url.c \
	$(SRC_DIR)/lr_encoding.c \
	$(SRC_DIR)/lr_event.c \
	$(SRC_DIR)/lr_performance.c \
	$(SRC_DIR)/lr_crypto.c \
	$(SRC_DIR)/lr_storage.c \
	$(SRC_DIR)/lr_fetch.c \
	$(SRC_DIR)/lr_ws.c \
	$(SRC_DIR)/lr_fs.c \
	$(SRC_DIR)/lr_terminal.c \
	$(SRC_DIR)/lr_sysinfo.c \
	$(SRC_DIR)/lr_thread_pool.c \
	$(SRC_DIR)/lr_sandbox.c \
	$(SRC_DIR)/lr_renderer.c \
	$(SRC_DIR)/lr_renderer_egl.c \
	$(SRC_DIR)/lr_canvas.c \
	$(SRC_DIR)/lr_canvas_webgl.c \
	$(SRC_DIR)/lr_scheduler.c \
	$(SRC_DIR)/lr_perf_opt.c \
	$(SRC_DIR)/lr_worker.c \
	$(SRC_DIR)/lr_gc.c \
	$(SRC_DIR)/lr_bytecode.c \
	$(SRC_DIR)/lr_compress.c \
	$(SRC_DIR)/lr_sandbox_log.c \
	$(SRC_DIR)/lr_promise.c \
	$(SRC_DIR)/lr_proxy.c \
	$(SRC_DIR)/lr_reflect.c \
	$(SRC_DIR)/lr_map.c \
	$(SRC_DIR)/lr_set.c \
	$(SRC_DIR)/lr_builtins_core.c \
	$(SRC_DIR)/lr_builtins_extra.c

# Object files
ENGINE_OBJS = $(patsubst $(ENGINE_DIR)/%.c, $(BUILD_DIR)/%.o, $(ENGINE_SRCS))
LR_OBJS  = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(LR_SRCS))
CLI_OBJ  = $(BUILD_DIR)/main.o

# Include paths
INCLUDES = -I$(INC_DIR) -I$(SRC_DIR) -I$(ENGINE_DIR)

# Target
TARGET = $(BUILD_DIR)/lr_js$(EXT)

# Library targets
LIB_TARGET   = $(BUILD_DIR)/liblr_js.so
LIB_A_TARGET = $(BUILD_DIR)/liblr_js.a

# Library objects (all .o except CLI main)
LIB_OBJS = $(ENGINE_OBJS) $(LR_OBJS)

# Default config for engine
ENGINE_CFLAGS = -D_GNU_SOURCE

# L/R_JS specific flags
LR_CFLAGS = -pthread

.PHONY: all clean run test win win32 linux32 repl help

all: $(TARGET)

# Windows cross-compilation targets (uses MinGW-w64)
win:
	$(MAKE) all CC=x86_64-w64-mingw32-gcc EXT=.exe LDFLAGS="-lm -lpthread -lws2_32" LR_CFLAGS="-pthread -D_WIN32_WINNT=0x0601"

win32:
	$(MAKE) all CC=i686-w64-mingw32-gcc EXT=.exe LDFLAGS="-lm -lpthread -lws2_32" LR_CFLAGS="-pthread -D_WIN32_WINNT=0x0601 -m32"

# Linux 32-bit build (requires gcc-multilib)
linux32:
	$(MAKE) all ARCH32=1

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile engine sources
$(BUILD_DIR)/%.o: $(ENGINE_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ENGINE_CFLAGS) $(INCLUDES) -c $< -o $@

# Compile L/R_JS sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LR_CFLAGS) $(ENGINE_CFLAGS) $(INCLUDES) -c $< -o $@

# Compile CLI
$(BUILD_DIR)/main.o: $(CLI_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(ENGINE_CFLAGS) $(INCLUDES) -c $< -o $@

# Link
$(TARGET): $(ENGINE_OBJS) $(LR_OBJS) $(CLI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"
	@echo "Run with: $(TARGET) [script.js]"

# Shared library (liblr_js.so)
$(LIB_TARGET): $(ENGINE_OBJS) $(LR_OBJS)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(LDFLAGS)
	@echo "Library built: $(LIB_TARGET)"

# Static library (liblr_js.a)
$(LIB_A_TARGET): $(ENGINE_OBJS) $(LR_OBJS)
	ar rcs $@ $^
	@echo "Static library built: $(LIB_A_TARGET)"

# Build both shared and static library
lib: $(LIB_TARGET) $(LIB_A_TARGET)
	@echo "Libraries built:"
	@ls -lh $(LIB_TARGET) $(LIB_A_TARGET)

# Build shared library only
lib-so: $(LIB_TARGET)

# Build static library only
lib-a: $(LIB_A_TARGET)

# Build example host program
EXAMPLE_BIN = $(BUILD_DIR)/example_host
EXAMPLE_SRC = examples/host_main.c
EXAMPLE_OBJ = $(BUILD_DIR)/host_main.o

$(EXAMPLE_OBJ): $(EXAMPLE_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(EXAMPLE_BIN): $(EXAMPLE_OBJ) $(LIB_A_TARGET)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Example built: $(EXAMPLE_BIN)"

example: $(EXAMPLE_BIN)
	@echo "Run with: $(EXAMPLE_BIN)"

# Clean
clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned."

# C test program
TEST_BIN = $(BUILD_DIR)/test_engine
TEST_OBJ = $(BUILD_DIR)/test_engine.o

$(TEST_OBJ): tests/test_engine.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iinclude -Isrc -Isrc/engine -c $< -o $@

$(TEST_BIN): $(ENGINE_OBJS) $(LR_OBJS) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Test build complete: $(TEST_BIN)"

test-c: $(TEST_BIN)
	@echo "=== Running C Engine Tests ==="
	$(TEST_BIN)

# Run with a test script
test: $(TARGET)
	@echo "=== Running test ==="
	$(TARGET) -e "console.log('L/R_JS is working!'); console.log('Array.from:', Array.from('hello'));"
	$(TARGET) -e "console.log('Promise test:'); Promise.resolve(42).then(v => console.log('Got:', v));"
	$(TARGET) -e "console.log('URL:', new URL('https://example.com/path?q=1').hostname);"
	$(TARGET) -e "console.log('TextEncoder:', new TextEncoder().encode('hello').length);"
	$(TARGET) -e "console.log('crypto.randomUUID():', crypto.randomUUID());"
	$(TARGET) -e "console.log('performance.now():', performance.now());"
	$(TARGET) -e "console.log('ES2022 features:'); console.log('  Object.hasOwn:', typeof Object.hasOwn); console.log('  Error.cause:', typeof Error.cause);"
	$(TARGET) -e "const arr = [1, 2, 3]; console.log('  .at():', arr.at(-1));"
	$(TARGET) -e "setTimeout(() => console.log('Timer fired!'), 10); console.log('Timer set');"

# Run with REPL
repl: $(TARGET)
	$(TARGET) -i

# Run with a specific script
run: $(TARGET)
	$(TARGET) $(SCRIPT)

# Help
help:
	@echo "L/R_JS Makefile targets:"
	@echo "  make                  Build lr_js (native)"
	@echo "  make linux32          Build 32-bit Linux (requires gcc-multilib)"
	@echo "  make win              Cross-compile for Windows 7+ 64-bit (MinGW-w64)"
	@echo "  make win32            Cross-compile for Windows 7+ 32-bit (MinGW-w64)"
	@echo "  make test             Build and run tests"
	@echo "  make repl             Build and start REPL"
	@echo "  make run SCRIPT=file  Build and run a script"
	@echo "  make clean            Remove build artifacts"
	@echo ""
	@echo "Environment variables:"
	@echo "  CC=clang              Use Clang instead of GCC"
	@echo "  CFLAGS=\"-O0 -g\"       Debug build"
	@echo "  ARCH32=1              32-bit build from 64-bit host"
	@echo "  EXT=.exe              Windows executable extension"