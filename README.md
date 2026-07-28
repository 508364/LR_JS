# L/R_JS - Lightweight/Runtime JavaScript Engine

Pure C, ES2022-compatible JavaScript engine with browser APIs.

## Features

- **ES2022+ JavaScript**: Classes, arrow functions, Promises, Proxies, Reflect, modules, etc.
- **Browser APIs**: `console`, `URL`, `TextEncoder`/`TextDecoder`, `fetch` (network requests delegated to the host via `LR_HttpWrapper`), `WebSocket` (host-delegated via `LR_WsWrapper`), `fs` (file system, privilege-aware), `term` (terminal command execution, privilege-aware), `crypto`, `performance`, Canvas, `setTimeout`/`setInterval`. Note: the engine has **no built-in networking** — both `fetch` and `WebSocket` are delegated to the host.
- **Typed Arrays**: `ArrayBuffer`, `Int8Array`, `Uint8Array`, `Float64Array`, `DataView`
- **Error Handling**: `Error`, `TypeError`, `SyntaxError`, `RangeError`, `ReferenceError` with stack traces and `cause` support
- **ES6+ Collections**: `Map`, `Set`, `WeakMap`, `WeakSet`
- **GC**: Mark-and-sweep garbage collector
- **Cross-platform**: Linux (x86_64, x86, ARM64, ARMv7), Windows 7+ (x86_64, x86), macOS
- **Thread-safe**: Built-in thread pool, Worker support

## Build

### Prerequisites

| Platform | Requirements |
|----------|-------------|
| Linux (x86_64) | `gcc` or `clang`, `make`, `cmake` (optional) |
| Linux 32-bit | `gcc-multilib` (for `-m32` builds) |
| Windows (MSYS2) | `mingw-w64-x86_64-gcc` or `mingw-w64-i686-gcc` |
| Windows (MSVC) | Visual Studio 2019+ or Build Tools |
| macOS | Xcode Command Line Tools (`clang`) |

### Makefile

```bash
# Native build (Linux/macOS/Windows-MSYS2)
make

# Linux 32-bit (requires gcc-multilib)
make linux32

# Cross-compile for Windows 7+ 64-bit (MinGW-w64)
make win

# Cross-compile for Windows 7+ 32-bit (MinGW-w64)
make win32

# Debug build
make CFLAGS="-O0 -g -fsanitize=address"

# Run tests
make test

# Start REPL
make repl
```

### CMake

```bash
mkdir build_cmake && cd build_cmake

# Linux/macOS (Release)
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)

# Linux 32-bit
cmake .. -DCMAKE_BUILD_TYPE=Release -DLR_32BIT=ON
cmake --build . --config Release -j$(nproc)

# Windows (MSVC)
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release

# Windows 7+ target (MinGW)
cmake .. -G "Unix Makefiles" -DLR_WIN7=ON
cmake --build . --config Release

# Output structure:
#   build_cmake/bin/lr_js          - CLI executable
#   build_cmake/lib/liblr_js.a     - Static library
#   build_cmake/lib/liblr_js.so    - Shared library (Unix)
#   build_cmake/lib/lr_js.dll      - Shared library (Windows)
```

## macOS Cross-Compile (osxcross)

You can build macOS `x86_64` and `arm64` binaries from Linux using
[osxcross](https://github.com/tpoechtrager/osxcross):

```bash
# 1. Build & install osxcross with at least one macOS SDK (e.g. MacOSX12.sdk).
#    NOTE: if your osxcross clang is older than LLVM 15, avoid the 15.x SDK
#    (it uses the '_Float16' type, which those clang versions cannot compile).
#
# 2. Run the cross-build script. It auto-detects the installed osxcross
#    toolchains (o64-clang / oa64-clang) and SDKs and builds both archs:
./build_macos.sh

# Override the SDK explicitly if auto-detection picks the wrong one:
LR_OSX_SDK=/path/to/MacOSX12.3.sdk ./build_macos.sh
```

Per-architecture output (created under `releases/`):

- `LR_JS-0.1.0-macos-x86_64.tar.gz`
- `LR_JS-0.1.0-macos-arm64.tar.gz`

Each archive contains `lib/liblr_js.a`, `lib/liblr_js.dylib`,
`bin/lr_js` and `lr_js.h`.

How it works:

- The script bypasses the `o64-clang`/`oa64-clang` launchers (which hardcode an
  SDK and append `-isysroot` after the user's flags, preventing override). It
  instead invokes the real target-specific clang directly and derives the exact
  `-target` triple from that binary's filename, so the matching `<triple>-ld`
  linker (e.g. `x86_64-apple-darwin21.4-ld`) is used instead of the host
  `/usr/bin/ld`.
- SDK auto-detection prefers the SDK whose macOS major version matches the
  clang's baked darwin version, and falls back to the oldest available SDK to
  avoid the `_Float16` incompatibility. `LR_OSX_SDK` / `SDKROOT` can override it.

## Windows Support

### Compatibility

| Windows Version | Status | Notes |
|----------------|--------|-------|
| Windows 7+ | ✅ Full | Uses `_WIN32_WINNT=0x0601` |
| Windows 10 | ✅ Full | Recommended |
| Windows 11 | ✅ Full | |

### Features

- **WinSock2**: Network I/O via `ws2_32`
- **pthread emulation**: `lr_pthread_win.h` provides POSIX threads on Windows
- **File I/O**: POSIX-compatible `open`/`close`/`read`/`write` wrappers
- **Memory mapping**: `mmap`/`munmap` via `VirtualAlloc`/`VirtualFree`
- **Dynamic loading**: `dlopen`/`dlsym` via `LoadLibrary`/`GetProcAddress`
- **Random**: `crypto.randomUUID()` via `BCryptGenRandom`
- **Timers**: High-resolution via `QueryPerformanceCounter`

### Build for Windows

```bash
# Cross-compile from Linux (MinGW-w64)
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
make win        # 64-bit
make win32      # 32-bit

# Native on Windows (MSYS2)
pacman -S mingw-w64-x86_64-gcc
make

# Native on Windows (MSVC)
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

## 32-bit Support

### Linux 32-bit

```bash
# Build from x86_64 host (-m32)
sudo apt install gcc-multilib
make linux32

# Or via CMake
cmake .. -DLR_32BIT=ON
```

### Windows 32-bit

```bash
# Cross-compile
make win32

# Or via CMake (MSVC)
cmake -B build -G "Visual Studio 17 2022" -A Win32
```

## Architecture Support

| Architecture | Makefile | CMake | Status |
|-------------|----------|-------|--------|
| x86_64 | ✅ | ✅ | Full |
| x86 (32-bit) | ✅ `linux32` | ✅ `-DLR_32BIT=ON` | Full |
| ARM64 (aarch64) | ✅ | ✅ | Full |
| ARMv7 | ✅ | ✅ | Untested |

## Project Structure

```
LR_JS/
├── cli/main.c          # CLI entry point
├── include/
│   └── lr_js.h         # Public API header
├── src/
│   ├── engine/         # Core engine (lexer, parser, interpreter)
│   │   ├── lr_engine.c    # Runtime, values, objects, GC
│   │   ├── lr_engine.h    # Internal engine types
│   │   ├── lr_lexer.c     # Lexer/tokenizer
│   │   ├── lr_parser.c    # Parser (AST generation)
│   │   └── lr_interp.c    # Interpreter (bytecode + AST walk)
│   ├── lr_runtime.c    # Runtime initialization
│   ├── lr_builtins_core.c  # Object, Array, String, Number, Function, Error
│   ├── lr_builtins_extra.c # Date, RegExp, Symbol, TypedArrays, Promise
│   ├── lr_promise.c    # Promise implementation
│   ├── lr_proxy.c      # Proxy implementation
│   ├── lr_reflect.c    # Reflect implementation
│   ├── lr_map.c        # Map implementation
│   ├── lr_set.c        # Set implementation
│   ├── lr_console.c    # console.log
│   ├── lr_timers.c     # setTimeout/setInterval
│   ├── lr_url.c        # URL
│   ├── lr_encoding.c   # TextEncoder/TextDecoder
│   ├── lr_event.c      # Event/EventTarget
│   ├── lr_performance.c # performance.now
│   ├── lr_crypto.c     # crypto.randomUUID
│   ├── lr_storage.c    # localStorage (in-memory)
│   ├── lr_fetch.c      # HTTP fetch (wrapper-based, delegates to host)
│   ├── lr_ws.c         # WebSocket (host-delegated wrapper, LR_WsWrapper)
├── lr_fs.c         # File system API (privilege-aware wrapper)
├── lr_terminal.c   # Terminal API (privilege-aware wrapper)
│   ├── lr_thread_pool.c # Thread pool for workers
│   ├── lr_sandbox.c    # Sandboxing
│   ├── lr_worker.c     # Web Worker support
│   ├── lr_gc.c         # Garbage collector
│   ├── lr_platform.h   # Platform abstraction layer
│   ├── lr_pthread_win.h # Windows pthread emulation
│   └── lr_renderer*.c  # Canvas/WebGL renderer
└── Project-Record/     # Development records (not tracked by git)
```

## API Usage

```c
#include "lr_js.h"

int main() {
    LRRuntime *rt = lr_create_runtime();
    LRContext *ctx = lr_create_context(rt);

    // Evaluate JavaScript
    LRValue result = lr_eval(ctx, "1 + 2", "<eval>", 0);
    printf("Result: %d\n", result.u.number);  // 3

    // Run REPL
    lr_repl(ctx);

    lr_free_context(ctx);
    lr_free_runtime(rt);
    return 0;
}
```

## Special Thanks

- **QuickJS** — This project originally started as a fork built upon the [QuickJS](https://bellard.org/quickjs/) JavaScript engine by Fabrice Bellard, whose compact, embeddable C implementation provided the initial runtime foundation. Over time, as features were added and the architecture was reworked, the codebase diverged so heavily that it became a fully self-implemented engine of its own — today it no longer shares QuickJS source code, but its lineage traces back to it.
- **V8** — Many architectural and behavioral decisions (object model, Promise/job queues, bytecode design, etc.) are informed by and reference the design of Google's [V8](https://v8.dev/) engine.

## License

MIT