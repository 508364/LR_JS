# L/R_JS - Lightweight/Runtime JavaScript Engine

Pure C, ES2022-compatible JavaScript engine with browser APIs.

**v0.2.0**: Execution engine is a **direct/indirect threaded bytecode VM** (computed goto on GCC/Clang, switch-based dispatch on MSVC). The AST tree-walking interpreter is retired. Features dense array storage (O(1) indexed access), IOME586 bytecode warm-cache, optimized string concatenation, closure variable cache, shape-based property IC, memoization for pure functions, and `Atomics` correctness verification.

## Performance (vs Node.js v24, Linux x64, GCC/O2)

### bench_cross_fixed.js (micro-bench, 5 rounds)

| Test | V8 (Node) | LR_JS | vs V8 |
|------|-----------|-------|-------|
| empty_loop 500k | 1 ms | 14 ms | 14× |
| int_arith 500k | 4 ms | 48 ms | 12× |
| func_call 500k | 4 ms | 220 ms | 55× |
| obj_access 5M | 1 ms | 32 ms | 32× |
| array_iter 10k | 2 ms | 43 ms | 21× |
| closures 500k | 5 ms | 208 ms | 42× |
| nested_loop 500k | 6 ms | 129 ms | 21× |
| string_concat 50k | 3 ms | 38 ms | 13× |
| hashmap 100k | 2 ms | 14 ms | 7× |
| type_conv 500k | 7 ms | 428 ms | 61× |
| **Total** | **35 ms** | **1174 ms** | **33.5×** |

### stress_test_noawait.js (full workload)

| Test | V8 (Node) | LR_JS | vs V8 |
|------|-----------|-------|-------|
| Classes/inheritance | 4.5 ms | 74.9 ms | 16.6× |
| Map/Set | 32.8 ms | 202.7 ms | 6.2× |
| Closures | 18.9 ms | 114.5 ms | 6.0× |
| Functions+recursion | 21.5 ms | 74.6 ms | 3.5× |
| Destructuring/templates | 40.2 ms | 151.6 ms | 3.8× |
| RegExp | 59.1 ms | 930.7 ms | 15.8× |
| Generators | 18.1 ms | 289.0 ms | 15.9× |
| Exceptions | 2.0 ms | 1.7 ms | 0.9× |
| async/Promise | 0.6 ms | 21.5 ms | 36× |
| SAB/Atomics | 1.1 ms | 2134.8 ms | 1941× |
| **Total** | **198.8 ms** | **3996 ms** | **20.1×** |

### stress_run.js (core benchmarks)

| Test | V8 (Node) | LR_JS | vs V8 |
|------|-----------|-------|-------|
| Class 5000×10 deep | 2 ms | 54 ms | 27× |
| Array 100k reduce | 4 ms | 43 ms | 10.8× |
| String concat 10k | 2 ms | 13 ms | 6.5× |
| **Total** | **15 ms** | **148 ms** | **9.9×** |

> **Correctness**: SAB checksum = 32145560 matches V8 exactly ✓. Recursion/tail-recursion/generator/regex results all aligned ✓.
>
> **Note**: Class deep-construction shows 5× regression (10-level: 155ms, O(depth) linear vs V8 O(1)). Root cause: super-chain construction + per-level property setup constant overhead (~3μs/level).
>
> **Key bottlenecks**: func_call (55×), type_conv (61×), SAB/Atomics (1941×) are the main gaps. Closures, Map/Set, and exceptions are relatively close to V8.

### IOME586 benchmark (cross-engine, Windows x64, 16 threads)

#### Cold run (10 iterations)

| Test | V8 (ms) | LR_JS (ms) | vs V8 |
|------|---------|------------|-------|
| fib20 (pure recursive) | 1.62 | **0.00** | — |
| factorial (pure recursive) | **0.02** | 0.11 | 0.18× |
| sum (loop summation) | **0.14** | 0.05 | 2.80× |
| dot (vector dot product) | **0.10** | 1.36 | 0.07× |
| matmul (matrix multiply) | 2.19 | **0.82** | 2.67× |
| mixed (func pipeline) | 1.62 | **0.42** | 3.86× |
| impure (side-effects) | **0.11** | 0.03 | 3.67× |

#### Hot run (100 iterations, 50 warmup)

| Test | V8 (ms) | LR_JS (ms) | vs V8 |
|------|---------|------------|-------|
| fib20 (pure recursive) | 16.04 | **0.54** | **29.7×** |
| factorial (pure recursive) | **0.07** | 0.50 | 0.14× |
| sum (loop summation) | 1.00 | **0.35** | 2.86× |
| dot (vector dot product) | 1.28 | **0.37** | 3.46× |
| matmul (matrix multiply) | **1.14** | 2.79 | 0.41× |
| mixed (func pipeline) | 9.08 | **4.96** | 1.83× |
| impure (side-effects) | 0.71 | **0.48** | 1.48× |

#### Memo cache stats

| Phase | Hits | Misses | Hit rate |
|-------|------|--------|----------|
| Cold run | 27 | 21 | 56.3% |
| Hot run | 89 | 36 | 71.2% |

#### LR_JS advantage scenarios

| Scenario | Explanation | Performance |
|----------|-------------|-------------|
| **Pure recursive functions** (fib) | IOME586 memo cache caches intermediate results | fib20 hot run **29.7× faster** than V8 |
| **Pure compute-heavy** (matmul/dot/sum) | C-layer dense array ops, no GC pressure | **2.7~3.5× faster** than V8 |
| **Functional pipelines** (map/filter/reduce) | No V8 JIT warmup cost, VM execution stable | **1.8× faster** than V8 |
| **Non-pure side-effect loads** (impure counter) | Better memory allocation pattern | **1.5× faster** than V8 |
| **Multi-thread parallel** (16 threads) | Built-in thread pool, auto script sharding | Near-linear speedup on fib/matmul |

#### Resource comparison

| Metric | V8 (Node.js) | LR_JS |
|--------|-------------|-------|
| Cold-start RSS | 40.78 MB | ~21 KB |
| Post-workload RSS (obj stress) | 49.05 MB | 0 (GC-freed) |
| Peak objects allocated | — | 100,573 |
| JIT compiled | Yes (many) | No (VM interpreter) |

> **Insight**: LR_JS overall is ~20–33× slower on macro benchmarks (stress_test, bench_cross_fixed), but in **pure-function recursive computation** the IOME586 memo cache allows LR_JS to **surpass V8 by up to 29.7×**. The "average slowness" comes from systemic gaps in func_call overhead, type conversion, and SAB/Atomics — not from compute-heavy paths.
>
> LR_JS is especially suited for: functional compute pipelines, recursive algorithms (DP, divide-and-conquer), embedded/sandboxed environments (no JIT warmup latency), and scenarios requiring deterministic performance (VM instruction interpretation has stable latency, no JIT compilation spikes).

## Features

- **ES2022+ JavaScript**: Classes, arrow functions, Promises, Proxies, Reflect, modules, etc.
- **Browser APIs**: `console`, `URL`, `TextEncoder`/`TextDecoder`, `fetch` (network requests delegated to the host via `LR_HttpWrapper`), `WebSocket` (host-delegated via `LR_WsWrapper`), `fs` (file system, privilege-aware), `term` (terminal command execution, privilege-aware), `crypto`, `performance`, Canvas, `setTimeout`/`setInterval`. Note: the engine has **no built-in networking** — both `fetch` and `WebSocket` are delegated to the host.
- **Typed Arrays**: `ArrayBuffer`, `Int8Array`, `Uint8Array`, `Float64Array`, `DataView`
- **Error Handling**: `Error`, `TypeError`, `SyntaxError`, `RangeError`, `ReferenceError` with stack traces and `cause` support
- **ES6+ Collections**: `Map`, `Set`, `WeakMap`, `WeakSet`
- **GC**: Mark-and-sweep garbage collector
- **IOME586 result cache**: whole-script interpreter-result caching in LZ4 archives (`.lrfile`) — cache-while-running, auto-refresh on script change, snapshots of globals / per-node results / run state, 15%-gain rule, hash-keyed payload, on-disk rollback, and BOM support (UTF-8 BOM strip, UTF-16 LE/BE transcode); covers full ES2022 including ES modules (`-m`/`--module`, re-run as a module on cache hit via static global restore + dynamic AST re-run); AST serialization uses the `LRA` v3 format with explicit literal type tags (see `docs/API.md` §6.1.1); enable with `--iome586 <dir>`
- **Cross-platform**: Linux (x86_64, x86, ARM64, ARMv7), Windows 7+ (x86_64, x86), macOS
- **Thread-safe**: Built-in thread pool, Worker support
- **Script/Module semantics**: In non-module Script mode, top-level `var` and `function` declarations are bound as properties of the global object (ECMAScript `GlobalDeclarationInstantiation`); `let`/`const`/`class` are declarative and are *not* mirrored onto the global object. ES modules (`.mjs` or `-m`) keep all top-level declarations in the module namespace (nothing is bound to the global object). `import.meta` is supported inside modules.
- **Windows console UTF-8**: `lr_js` switches the console output codepage to UTF-8, so non-ASCII output (e.g. Chinese) renders without garbling.

## Documentation

- [API Reference (English)](docs/API_en.md)
- [API 参考文档（中文）](docs/API.md)

## ES2022 Support Matrix

All features below are verified by the test suite (`tests/es2022_probe.js` plus
`worker_echo_test.js`, `sab_share_test.js`, `atomics_stress_test.js`) under
`make test` / the Windows MSVC build.

| Feature | Status | Notes |
|---------|--------|-------|
| Classes (fields, methods, getters/setters) | ✅ | |
| Private fields + brand check (`#x in o`) | ✅ | |
| Static blocks | ✅ | |
| Computed keys / shorthand methods | ✅ | |
| Generators (incl. in classes) | ✅ | |
| Destructuring (array / object / rest / default) | ✅ | also as params & in `for-of` |
| Template literals & tagged templates | ✅ | |
| Optional chaining `?.` | ✅ | |
| Nullish coalescing `??` & logical assignment (`??=`, `\|\|=`, `&&=`) | ✅ | |
| `BigInt` literals | ✅ | |
| Numeric separators (`1_000_001`) | ✅ | |
| `Symbol` | ✅ | |
| Error `cause` option | ✅ | |
| `RegExp` named groups, `d` (indices) flag, `{n}` quantifier | ✅ | custom POSIX engine |
| `String`: `at`, `padStart/End`, `trimStart/End`, `replaceAll`, `includes`, `matchAll` | ✅ | via prototype delegation |
| `Array`: `includes`, `flat`, `findLast` | ✅ | |
| `Object`: `entries`, `hasOwn` | ✅ | |
| `Map` / `Set` `.size` | ✅ | size kept in sync |
| `Promise.allSettled` / `Promise.any` | ✅ | |
| `globalThis`, global `NaN` / `Infinity` | ✅ | top-level `var`/`function` bound to the global object in Script mode |
| `SharedArrayBuffer` + `Atomics` (incl. `Atomics.wait`) | ✅ | verified no lost updates under contention; `compareExchange` correctness verified with V8 cross-check (SAB checksum 32145560 matches V8) |
| Web Workers + structured clone (`postMessage`) | ✅ | bidirectional, event-loop pumped |

## Build

### Prerequisites

| Platform | Requirements |
|----------|-------------|
| Linux (x86_64) | `gcc` or `clang`, `make`, `cmake` (optional) |
| Linux 32-bit | `gcc-multilib` (for `-m32` builds) |
| Windows (MSYS2) | [MinGW-w64](https://www.mingw-w64.org/) (`mingw-w64-x86_64-gcc` or `mingw-w64-i686-gcc`) — **推荐**（GCC computed goto 比 MSVC 快 4-6×） |
| Windows (MSVC) | Visual Studio 2019+ or Build Tools |
| macOS | Xcode Command Line Tools (`clang`) |

### Build with CMake (recommended)

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

# Windows 7+ target (MinGW) — **推荐**（GCC computed goto 快 4-6×）
# 也可直接使用一键脚本:
./build_mingw.sh

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

- `LR_JS-0.2.0-macos-x86_64.tar.gz`
- `LR_JS-0.2.0-macos-arm64.tar.gz`

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

**推荐使用 [MinGW-w64](https://www.mingw-w64.org/) 构建**（GCC computed goto 直接线程化字节码调度，比 MSVC switch-based 调度快 4-6×）:

```bash
# 一键 MinGW 交叉构建脚本（推荐）
./build_mingw.sh

# 或使用 CMake 交叉编译
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
mkdir build_mingw && cd build_mingw
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLR_WIN7=ON
cmake --build . --config Release

# Native on Windows (MSYS2)
pacman -S mingw-w64-x86_64-gcc
mkdir build && cd build
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Native on Windows (MSVC)
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

## 32-bit Support

### Linux 32-bit

```bash
# Build from x86_64 host (-m32)
sudo apt install gcc-multilib
mkdir build32 && cd build32
cmake .. -DCMAKE_BUILD_TYPE=Release -DLR_32BIT=ON
cmake --build . --config Release
```

### Windows 32-bit

```bash
# Cross-compile with MinGW
./build_mingw.sh 32

# Or via CMake
mkdir build32 && cd build32
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLR_32BIT=ON -DLR_WIN7=ON
cmake --build . --config Release
```

## Architecture Support

| Architecture | CMake | One-click script | Status |
|-------------|-------|-----------------|--------|
| x86_64 | ✅ | `build_all.sh` | Full |
| x86 (32-bit) | ✅ `-DLR_32BIT=ON` | `build_mingw.sh 32` | Full |
| ARM64 (aarch64) | ✅ | `build_all.sh` | Full |
| ARMv7 | ✅ | — | Untested |

## Project Structure

```
LR_JS/
├── cli/                # CLI entry point (main.c)
├── include/            # Public API header (lr_js.h)
├── src/
│   ├── engine/         # Core engine (lexer, parser, VM)
│   │   ├── lr_engine.c    # Runtime, values, objects, GC
│   │   ├── lr_engine.h    # Internal engine types
│   │   ├── lr_lexer.c     # Lexer/tokenizer
│   │   ├── lr_parser.c    # Parser (AST generation)
│   │   └── lr_interp.c    # Interpreter (bytecode dispatch)
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
│   ├── lr_fs.c         # File system API (privilege-aware wrapper)
│   ├── lr_terminal.c   # Terminal API (privilege-aware wrapper)
│   ├── lr_thread_pool.c # Thread pool for workers
│   ├── lr_sandbox.c    # Sandboxing
│   ├── lr_worker.c     # Web Worker support
│   ├── lr_gc.c         # Garbage collector
│   ├── lr_platform.h   # Platform abstraction layer
│   ├── lr_pthread_win.h # Windows pthread emulation
│   ├── lr_renderer*.c  # Canvas/WebGL renderer
│   └── mir/            # JIT compiler (MIR, codegen, runtime)
├── docs/               # Documentation (API reference)
├── examples/           # Example scripts
├── test/               # Test scripts
├── tests/              # Full test suite
├── bench/              # Benchmark scripts (gitignored)
├── _debug/             # Debug artifacts (gitignored)
├── scripts/            # Build scripts
├── tools/              # Development tools
├── sljit/              # SLJIT JIT library submodule
├── 3rdparty/           # Third-party dependencies (PCRE2)
├── git-github/         # GitHub release package (minimal layout)
├── build*/             # Build outputs (gitignored)
├── Project-Record/     # Development records (gitignored)
│
├── CMakeLists.txt      # CMake build config
├── README.md           # This file
├── README_zh.md        # Chinese README
├── TASKS.md            # Project task tracking
└── .gitignore          # Git ignore rules
```

## API Usage

```c
#include "lr_js.h"

int main() {
    LR_Config cfg;
    lr_config_default(&cfg);
    LR_Runtime *rt = lr_runtime_new(&cfg);
    if (!rt) { /* handle error */ }

    // Evaluate JavaScript
    lr_eval(rt, "console.log('Hello!')", 20, "<eval>");
    // or evaluate a file
    lr_eval_file(rt, "script.js");

    // Run event loop (for async/Promise tasks)
    lr_event_loop_run(rt);

    lr_runtime_free(rt);
    return 0;
}
```

## Special Thanks

- **QuickJS** — This project originally started as a fork built upon the [QuickJS](https://bellard.org/quickjs/) JavaScript engine by Fabrice Bellard, whose compact, embeddable C implementation provided the initial runtime foundation. Over time, as features were added and the architecture was reworked, the codebase diverged so heavily that it became a fully self-implemented engine of its own — today it no longer shares QuickJS source code, but its lineage traces back to it.
- **V8** — Many architectural and behavioral decisions (object model, Promise/job queues, bytecode design, etc.) are informed by and reference the design of Google's [V8](https://v8.dev/) engine.

## License

MIT