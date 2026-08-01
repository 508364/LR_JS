# L/R_JS API Reference

> Full-featured lightweight in-browser JS runner | Pure C | ES2022+ | Multithreaded | Async Sandbox

---

## 1. Overview

L/R_JS is a lightweight in-browser JavaScript runner implemented in pure C, supporting ES2022+ features, with a multithreaded, multitasking, and asynchronous sandbox execution environment and a built-in high-performance JS engine.

### 1.1 Supported Platforms

| Platform | Arch | Compiler | Min Version |
|----------|------|----------|-------------|
| **Linux** | x86_64, x86, aarch64, armv7 | GCC 9+, Clang 12+ | kernel 3.10+ |
| **macOS** | x86_64, arm64 (Apple Silicon) | Clang 14+ | macOS 11+ |
| **Windows** | x86_64, x86, aarch64 | MSVC 2022+, MinGW-w64 | Windows 7+ |
| **FreeBSD** | x86_64, aarch64 | Clang 14+ | FreeBSD 13+ |
| **OpenBSD** | x86_64, aarch64 | Clang 14+ | OpenBSD 7.0+ |
| **NetBSD** | x86_64, aarch64 | GCC 10+ | NetBSD 9.0+ |
| **Android** | aarch64, armv7, x86_64 | NDK r25+ | API 24+ |
| **iOS** | arm64 | Xcode 15+ | iOS 14+ |

### 1.2 Cross-Platform via Conditional Compilation

```c
// Memory detection
#ifdef __linux__
    // /proc/meminfo
#elif defined(__APPLE__)
    // sysctl
#elif defined(_WIN32)
    // GlobalMemoryStatusEx
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // sysctl
#endif

// UUID generation
#ifdef _WIN32
    // CryptGenRandom
#else
    // /dev/urandom
#endif

// Filesystem
#ifdef _WIN32
    #define lr_mkdir(p) _mkdir(p)
#else
    #define lr_mkdir(p) mkdir(p, 0755)
#endif
```

---

## 2. Quick Start

### 2.1 Command-Line Usage

```bash
# Basic usage
./lr_js script.js
./lr_js -e "console.log('Hello, L/R_JS!')"
./lr_js --interactive       # start REPL

# Full example
./lr_js \
    --no-memory-check \
    --gc-incremental \
    --gc-pause-target 5 \
    --iome586 ./cache \
    --sandbox-log ./logs \
    --gc-stats \
    --iome586-stats \
    script.js
```

### 2.2 Basic C API Usage

```c
#include "lr_js.h"

int main() {
    // 1. Configure the runtime
    LR_Config cfg;
    lr_config_default(&cfg);
    cfg.memory_limit = 128 * 1024 * 1024;  // 128MB heap limit
    cfg.gc_incremental = 1;                // enable incremental GC
    cfg.bytecode_cache_dir = "./cache";    // enable IOME586 result cache
    cfg.skip_memory_check = 1;             // skip system memory check

    // 2. Create the runtime
    LR_Runtime *rt = lr_runtime_new(&cfg);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    // 3. Run a script
    lr_eval(rt, "console.log('Hello!')", 20, "<eval>");
    // or run a file
    lr_eval_file(rt, "script.js");

    // 4. Cleanup
    lr_runtime_free(rt);
    return 0;
}
```

### 2.3 Compiling and Linking

```bash
# Linux / macOS / BSD
gcc -o myapp myapp.c -I/path/to/LR_JS/include -L/path/to/LR_JS/build -llr_js -lpthread -ldl -lm

# Windows (MSVC)
cl myapp.c /I path\to\LR_JS\include /link path\to\LR_JS\build\lr_js.lib

# Windows (MinGW)
gcc -o myapp.exe myapp.c -I/path/to/LR_JS/include -L/path/to/LR_JS/build -llr_js -lpthread -lws2_32
```

---

## 3. Core API

### 3.1 Runtime Management

| Function | Description |
|----------|-------------|
| `LR_Runtime *lr_runtime_new(LR_Config *cfg)` | Create a runtime |
| `void lr_runtime_free(LR_Runtime *rt)` | Destroy a runtime |
| `const char *lr_version(void)` | Get the version string |

### 3.2 Script Execution

| Function | Description |
|----------|-------------|
| `int lr_eval(rt, src, len, filename)` | Run a string script |
| `int lr_eval_file(rt, filename)` | Run a script file |
| `int lr_eval_module(rt, src, len, filename)` | Run an ES Module |
| `int lr_eval_module_file(rt, filename)` | Run an ES Module file |

### 3.3 Event Loop

| Function | Description |
|----------|-------------|
| `int lr_event_loop_pending(rt)` | Check for pending tasks |
| `void lr_event_loop_run(rt)` | Run the event loop |
| `void lr_event_loop_stop(rt)` | Stop the event loop |

### 3.4 Memory Management

| Function | Description |
|----------|-------------|
| `void lr_gc(rt)` | Manually trigger GC |
| `void lr_gc_print_stats(rt, fp)` | Print GC statistics |
| `void lr_gc_reset_stats(rt)` | Reset GC statistics |
| `void lr_compute_memory_usage(rt, usage)` | Get detailed memory usage |
| `void lr_dump_memory_usage(rt, fp)` | Print detailed memory usage |
| `int64_t lr_get_available_memory(void)` | Get available system memory |
| `int lr_check_system_memory(min_bytes)` | Check that system memory is sufficient |

### 3.5 IOME586 Result Cache

| Function | Description |
|----------|-------------|
| `void lr_bytecode_cache_stats(rt, fp)` | Print cache statistics (wraps `lr_iome586_stats`) |
| `void lr_bytecode_cache_clear(rt)` | Clear the cache (wraps `lr_iome586_clear`) |

### 3.6 Error Handling

| Function | Description |
|----------|-------------|
| `int lr_get_last_error(rt, buf, size)` | Get the last error message |
| `void lr_clear_last_error(rt)` | Clear the error state |

---

## 4. Configuration Structures

### 4.1 LR_Config

```c
typedef struct LR_Config {
    // Memory
    size_t  memory_limit;           // max heap memory (bytes), 0=unlimited
    size_t  gc_threshold;           // auto GC threshold, 0=default
    LR_GCMode gc_mode;             // GC mode

    // GC tuning
    int     gc_generational;        // enable generational GC (default 1)
    int     gc_incremental;         // enable incremental GC (default 1)
    size_t  gc_nursery_size;        // nursery size, 0=default 4MB
    int64_t gc_pause_target_ns;     // target max pause, 0=default 5ms

    // IOME586 result cache
    char   *bytecode_cache_dir;     // IOME586 cache dir (.lrfile.lz4 archive), NULL=disabled

    // Sandbox log
    char   *sandbox_log_dir;        // sandbox log dir, NULL=disabled

    // System memory
    size_t  min_system_memory;      // minimum system free memory, 0=disabled
    int     skip_memory_check;      // skip system memory check

    // Stack
    size_t  max_stack_size;         // max stack size, 0=default 1MB

    // Execution
    int     timeout_ms;             // execution timeout, 0=unlimited
    int     strict_mode;            // strict mode
    int     debug_mode;             // debug mode

    // Logging
    int     log_level;              // log level: 0=off, 1=error, 2=warn, 3=info, 4=debug
    FILE   *log_file;               // log output file, NULL=stderr

    // Compilation
    int     dump_bytecode;          // export bytecode
    int     strip_debug_info;       // strip debug info
} LR_Config;
```

### 4.2 GC Modes

```c
typedef enum {
    LR_GC_MODE_AUTO          = 0,  // automatic GC
    LR_GC_MODE_MANUAL        = 1,  // manual GC
    LR_GC_MODE_STRESS        = 2,  // stress test (GC after every allocation)
    LR_GC_MODE_GENERATIONAL  = 3,  // generational GC (nursery + old gen)
    LR_GC_MODE_INCREMENTAL   = 4,  // incremental GC (time-slicing + generational)
} LR_GCMode;
```

### 4.3 Memory Usage

```c
typedef struct LR_MemoryUsage {
    int64_t malloc_size;          // allocated memory
    int64_t malloc_limit;         // memory limit
    int64_t memory_used_size;     // used memory
    int64_t memory_used_count;    // number of live blocks
    int64_t atom_count;           // atom count
    int64_t atom_size;            // atom size
    int64_t str_count;            // string count
    int64_t str_size;             // string size
    int64_t obj_count;            // object count
    int64_t obj_size;             // object size
    int64_t prop_count;           // property count
    int64_t prop_size;            // property size
    int64_t shape_count;          // shape count
    int64_t shape_size;           // shape size
    int64_t js_func_count;        // JS function count
    int64_t js_func_size;         // JS function size
    int64_t js_func_code_size;    // JS function code size
    int64_t js_func_pc2line_count;// line-number mapping entries
    int64_t js_func_pc2line_size; // line-number mapping size
    int64_t c_func_count;         // C function count
    int64_t array_count;          // array count
    int64_t fast_array_count;     // fast array count
    int64_t fast_array_elements;  // fast array element count
    int64_t binary_object_count;  // binary object count
    int64_t binary_object_size;   // binary object size
} LR_MemoryUsage;
```

---

## 5. Sandbox API

### 5.1 Sandbox Manager

```c
#include "lr_sandbox.h"
#include "lr_sandbox_log.h"

// Create a sandbox manager
LR_SandboxManager *mgr = lr_sandbox_manager_create(16);  // up to 16 sandboxes

// Destroy the sandbox manager
lr_sandbox_manager_destroy(mgr);
```

### 5.2 Sandbox Configuration

```c
typedef struct LR_SandboxConfig {
    char   *name;              // sandbox name
    char   *log_dir;           // log dir (NULL=disabled)
    size_t  memory_limit;      // heap limit, 0=inherit
    size_t  stack_size;        // stack size, 0=default
    int     timeout_ms;        // timeout, 0=unlimited
    int     max_eval_depth;    // max nesting depth
    int     allow_network;     // allow network API
    int     allow_filesystem;  // allow file API
    int     allow_workers;     // allow Workers
    int     isolated_context;  // isolated context
} LR_SandboxConfig;
```

### 5.3 Sandbox Operations

```c
// Create a sandbox
LR_SandboxConfig cfg = {
    .name = "my-sandbox",
    .log_dir = "./sandbox_logs",     // enable logging
    .memory_limit = 64 * 1024 * 1024,
    .timeout_ms = 5000,
    .allow_network = 0,
    .isolated_context = 1,
};
LR_Sandbox *sb = lr_sandbox_create(mgr, &cfg);

// Get the UUID
printf("Sandbox UUID: %s\n", sb->uuid);
// Output: Sandbox UUID: a1b2c3d4-e5f6-47a8-b9c0-d1e2f3a4b5c6

// Run a script
lr_sandbox_eval(sb, "console.log('sandbox running')", 30, "<sandbox>");

// Get the state
LR_SandboxState state = lr_sandbox_get_state(sb);

// Destroy the sandbox
lr_sandbox_destroy(mgr, sb);
```

### 5.4 Sandbox Logging System

Each sandbox automatically gets its own log file:

```
{sandbox_log_dir}/{YYYY-MM-DD}-{run_count}-{uuid}.log
```

Example:
```
./sandbox_logs/2026-07-19-3-a1b2c3d4-e5f6-47a8-b9c0-d1e2f3a4b5c6.log
```

Log format:
```
[2026-07-19 14:30:25.123456] [INFO ] [eval:1] Sandbox created: my-sandbox (uuid=a1b2c3d4-...)
[2026-07-19 14:30:25.234567] [INFO ] [eval:1] Eval started: <sandbox>
[2026-07-19 14:30:25.245678] [INFO ] [eval:1] Eval completed in 11.11 ms
[2026-07-19 14:30:30.123456] [INFO ] [eval:2] Eval started: <sandbox>
[2026-07-19 14:30:30.234567] [ERROR] [eval:2] Eval failed after 111.11 ms: <sandbox>
```

Logging API:
```c
// Write a log (thread-safe, non-blocking)
lr_slog_info(sb->log, eval_id, "Custom message: %s", "value");
lr_slog_error(sb->log, eval_id, "Error: %d", error_code);
lr_slog_debug(sb->log, eval_id, "Debug info: %p", ptr);

// Force flush
lr_sandbox_log_flush(sb->log);

// Print statistics
lr_sandbox_log_stats(sb->log, stdout);
```

---

## 6. IOME586 Result Cache

### 6.1 Overview

**IOME586** is the formal name of the caching technology. It caches the interpreter's produced data at the granularity of the whole JS script (serialized AST, global variable binding snapshot, per-node results, run state, etc.), and caches **while running**: after parsing completes, it writes a WRITING-status archive to disk; once the script finishes executing, the archive becomes ARCHIVED.

The cache covers **full ES2022**, including both ordinary scripts and ES modules (`-m`/`--module` or `JS_EVAL_TYPE_MODULE`). Scripts and modules both go through `lr_exec_file_cached` and share the same cache path; the archive records the `LR_IOME586_FLAG_MODULE` flag, and the hot path re-runs it **as a module** accordingly (correctly rebuilding the `module_ns` namespace and `import`/`export` bindings). The `is_default` marker of a default import `import defFn from "mod"` round-trips completely through AST serialization/deserialization, so a cache-hit module behaves identically to a cold run.

**Hot path = static restore + dynamic re-run** (this is the key to covering full ES2022):
- **Static part restored directly**: `lr_iome586_restore_globals` restores the global variable binding snapshot, so primitive globals are in place immediately;
- **Dynamic part re-run**: re-execute the deserialized AST. The interpreter rebuilds function/class bindings (keeping them callable), re-runs I/O and side effects, and recomputes primitives, so results are always correct no matter how "dynamic" the script is.

**An archive is essentially an LZ4 compressed package**, written out as `<namehash>.lrfile` (the `.lrfile.lz4` spelling is also accepted when loading). The file name is a hash of the script **path**, so modifying a JS script auto-updates the same archive in place (the old version is moved to `.bak` for rollback). The payload is **LZ4-compressed only — no encryption**; integrity is guaranteed by `CRC32` and the content hash `SourceHash` (FNV-1a 64-bit). The self-keyed XOR was removed (legacy KEYED archives are still loaded compatibly). The file description area keeps a plaintext copy of the script name + time + version.

**Container format** (magic `"IOME586\0"`):
```
Magic          "IOME586\0"     8 bytes
ContainerVer   uint32          4 bytes
EngineVer      uint32          4 bytes   (version string FNV-1a32)
Status         uint32          4 bytes   (1=WRITING in-progress, 2=ARCHIVED)
Flags          uint32          4 bytes
CreatedAt      int64           8 bytes
SourceHash     FNV-1a 64-bit   8 bytes   (script content hash, for validation, not a key)
Mtime          int64           8 bytes
SrcSize        uint64          8 bytes
OptRatio       uint32          4 bytes   (optimization ratio ×1e6)
CRC32          uint32          4 bytes
PayloadStored  uint32          4 bytes   (after LZ4 compression)
PayloadRaw     uint32          4 bytes
DescLen+Desc   variable                  (plaintext description: script name/time/version)
Payload        variable                  (LZ4-compressed; no encryption, CRC32/SourceHash validated)
```

**Security model (v0.1.0 hardening)**:
- **Sensitive-value exclusion**: the snapshot skips any global binding whose name matches a sensitive token (token/secret/password/credential/apikey/auth/bearer/cookie/session/private, etc.), so tokens/keys are never persisted with the cache.
- **Strings are opt-out**: `snapshot_strings` is on by default (records string-literal bindings); `--iome586-no-strings` disables it so the archive contains no string-literal values at all.
- **Restore off by default**: `restore_globals` is off by default; a warm run only does "static restore + dynamic re-run" and does not write archived global bindings back onto the global object. Enable explicitly with `--iome586-restore-globals` when needed.
- **BOM baseline remap protection**: after builtins are registered, the runtime captures a baseline of pre-existing global property names and skips those engine/BOM names during restore, so cache recovery cannot pollute builtin APIs.


**The payload contains named entries** (`u16 name_len|name|u32 data_len|data`), equivalent to multiple binary files inside the package: `meta` (meta info), `path` (interpreter path/method), `config` (configuration), `init` (init content and result), `ast` (serialized AST), `nodes` (per-node results), `globals` (global variable binding snapshot), `state` (state machine / run state), `bytecode` (compiled bytecode, magic `LRBC`).

#### 6.1.0 What is cached, and can it be restored directly? (v0.1.1)

| Cached item | Location | Persisted | Directly restorable on a warm run |
|-------------|----------|-----------|------------------------------------|
| Script name | header/`meta` | ✅ | ✅ read directly |
| Script hash (`source_hash`) | header | ✅ | ✅ read directly (hit validation) |
| Status (writing / archived) | header `status` | ✅ | ✅ read directly; `writing` is treated as a dirty archive and discarded |
| Time (`created_at` / source `mtime`) | header | ✅ | ✅ read directly |
| Optimization ratio (`opt_ratio_x1e6`) | header | ✅ | ✅ read directly (15% rule) |
| Version (container + engine FNV-1a32) | header | ✅ | ✅ read directly; mismatch invalidates the whole archive |
| Checksum (`payload_crc32`) | header | ✅ | ✅ read and verified directly |
| Script interpretation path | `path` | ✅ | ✅ restored directly |
| Configuration | `config` | ✅ | ✅ restored directly |
| Init content and result | `init` | ✅ | ✅ restored directly |
| Per-node results | `nodes` | ✅ | ⚠️ restored as *records* (comparison/statistics), evaluation is not skipped |
| AST | `ast` (`LRA` v3) | ✅ | ✅ restored directly, skipping lexing/parsing |
| Bytecode | `bytecode` (`LRBC` v2) | ✅ | ⚠️ only when the program has **no AST node references**; otherwise deserialization returns `NULL` and it is recompiled from the AST (sub-millisecond) |
| State-machine state | `state` | ✅ | ✅ restored directly |
| Run state | `state` | ✅ | ⚠️ restored as metadata; execution still starts from the beginning (no resume) |
| Global variable binding object | `globals` | ✅ (`snapshot_strings` on by default) | ❌ **not restored** by default (`restore_globals=0`); requires explicit `--iome586-restore-globals` |

In short: **structural content (AST, bytecode, path, config, init, metadata, state-machine state) is directly restorable**, while **runtime semantic state (global bindings, execution progress) is not restored by default** and is recomputed by re-executing, to guarantee correct semantics (never observing stale/poisoned globals). A warm run therefore skips "file read + lexing + parsing + compilation", not "execution".

#### 6.1.1 AST Serialization Format (magic `LRA`)

The archive's `ast` entry stores the **serialized AST**, not bytecode. Its binary layout begins with the 4-byte magic `"LRA"`, followed by a 1-byte **format version number** (currently `3`).

On deserialization the magic and version are validated; if they do not match (invalid magic or incompatible version) it returns `NULL` directly, the archive is rejected and deleted, and the next run automatically performs a cold run and rebuilds the cache (see §6.4).

**Literal serialization (`AST_LITERAL`) uses an explicit type tag `ltag`** with 5 classes:

| `ltag` | Type | Deserialization handling |
|--------|------|--------------------------|
| 0 | bool (`true`/`false`) | read `u32` boolean value, write only `u.bool_val.val` |
| 1 | string | read the string payload |
| 2 | number (`double`) | read `f64` numeric value |
| 3 | `null` | set `u.number.num = 0.0`, mark `TOK_NULL_LIT` |
| 4 | `undefined` | set `u.number.num = -1.0`, mark `TOK_UNDEFINED_LIT` |

> **Known fix (v0.7.0 / `LRA` v3)**: Earlier versions, when deserializing a bool literal, wrote `u.bool_val.val` and then also wrote `u.number.num` (`double`). Since `bool_val` and `number` overlap in memory within the `ASTNode` union, the low 4 bytes of `double 1.0` are zero, which overwrote `true` into `false`. The consequence was that in a warm run `let ok = true` became `false`, causing `if(!ok) throw` to misfire and drop subsequent statements (e.g. the final `MODULE TEST OK` line was lost after a cache hit). v3 changes deserialization of bool to **write only `bool_val.val`**, matching the parse-time constant node (which only sets `bool_val.val`). This bug is fully fixed and verified (cold/warm outputs are identical).

### 6.2 API

```c
// Configure the cache directory
LR_Config cfg;
lr_config_default(&cfg);
cfg.bytecode_cache_dir = "./my_cache";  // absolute or relative path

// Or set at runtime
lr_iome586_set_dir(&rt->iome586, "./my_cache");

// View statistics / clear
lr_bytecode_cache_stats(rt, stdout);   // wraps lr_iome586_stats
lr_bytecode_cache_clear(rt);           // wraps lr_iome586_clear
```

### 6.3 Caching Strategy

- **15% rule**: if the parse-time saving (parse_us / total_us) is below 15%, the cache is not written and the archive is discarded on commit
- **Cache while running**: `lr_iome586_begin` (WRITING) → execute → `lr_iome586_commit` (ARCHIVED); on an execution exception, `lr_iome586_abort` rolls back
- **BOM support**: UTF-8 BOM is automatically stripped when loading a script, UTF-16 LE/BE is automatically transcoded to UTF-8

### 6.4 Cache Auto-Update, Invalidation, and Rollback

- **Auto-update on script change**: archives are named by script path; once the source changes (content hash / mtime changes), the old archive is moved to `.bak`, and the next run automatically re-caches to the same file
- After the engine version changes (EngineVer mismatch), it also auto-updates
- **AST format version mismatch**: if the `ast` entry's `LRA` magic or format version is incompatible, the archive is rejected and deleted, and the next run automatically rebuilds the cache (see §6.1.1)
- On CRC32 check failure or a WRITING status (leftover half-written), loading is refused and the file is deleted
- Manual invalidation: `lr_iome586_invalidate(&rt->iome586, "script.js")`
- **On-disk rollback**: a `.bak` backup is kept at begin time; `lr_iome586_revert` can roll back to the previous archive (CLI: `--iome586-revert <js>`)

---

## 7. Built-in Browser API

### 7.1 Console

```js
console.log("message");
console.error("error");
console.warn("warning");
console.info("info");
console.debug("debug");
console.trace("trace");
console.time("label");
console.timeEnd("label");
console.assert(condition, "message");
```

### 7.2 Timers

```js
setTimeout(() => console.log("delayed"), 1000);
setInterval(() => console.log("tick"), 100);
clearTimeout(id);
clearInterval(id);
```

### 7.3 URL

```js
const url = new URL("https://example.com/path?q=1#hash");
console.log(url.hostname);    // "example.com"
console.log(url.pathname);    // "/path"
console.log(url.searchParams.get("q"));  // "1"
```

### 7.4 Encoding

```js
const enc = new TextEncoder();
const bytes = enc.encode("Hello");     // Uint8Array

const dec = new TextDecoder();
const str = dec.decode(bytes);         // "Hello"

const b64 = btoa("Hello");             // "SGVsbG8="
const raw = atob(b64);                 // "Hello"
```

### 7.5 Crypto

```js
const uuid = crypto.randomUUID();
// "a1b2c3d4-e5f6-47a8-b9c0-d1e2f3a4b5c6"

const bytes = new Uint8Array(32);
crypto.getRandomValues(bytes);
```

### 7.6 Performance

```js
const now = performance.now();   // high-resolution timestamp
const then = performance.now();
console.log("Elapsed:", then - now, "ms");
```

### 7.7 Storage

```js
localStorage.setItem("key", "value");
const val = localStorage.getItem("key");
localStorage.removeItem("key");
localStorage.clear();
```

### 7.8 Fetch

> **Note:** L/R_JS **does not have any built-in networking**. The engine itself sends no network packets; `fetch()` delegates the request to the host application (browser, WebUI, etc.) through the `LR_HttpWrapper` interface. The host must call `lr_http_set_wrapper()` to register a wrapper, otherwise `fetch()` returns a rejected Promise. Network capabilities are all host-delegated: `fetch` (`LR_HttpWrapper`) and `WebSocket` (`LR_WsWrapper`, see §7.9).

```js
const resp = await fetch("https://api.example.com/data");
const json = await resp.json();
console.log(json);
```

### 7.9 WebSocket

> **Note:** Like `fetch`, L/R_JS **does not have a built-in WebSocket protocol**. Connections are delegated to the host through the `LR_WsWrapper` interface; the engine itself sends/receives no WebSocket frames. The host handles the real I/O in the `connect`/`send`/`close` callbacks, and pushes events back to JS via the engine-side `lr_ws_on_*` functions (**must be called on the engine thread**, e.g. inside the engine's event-loop I/O pump).

#### C API

```c
// Set the WebSocket wrapper (host owns ownership, pass NULL to clear)
void lr_ws_set_wrapper(LR_Runtime *rt, LR_WsWrapper *wrapper);

// Get the current WebSocket wrapper
LR_WsWrapper *lr_ws_get_wrapper(LR_Runtime *rt);
```

```c
// WebSocket wrapper interface
typedef struct LR_WsWrapper {
    void *user_data;  // opaque data passed through to callbacks

    // Initiate a connection. On success write the host connection handle to
    // *out_handle and return 0; the actual "open" event is reported later via
    // lr_ws_on_open(). On failure return -1.
    int (*connect)(void *user_data, const char *url, const char *protocols,
                   void **out_handle);

    // Send a text frame. Return 0 on success, -1 on failure.
    int (*send)(void *user_data, void *conn_handle,
                const void *data, size_t len);

    // Close the connection. code/reason may be 0/NULL. Return 0 on success.
    int (*close)(void *user_data, void *conn_handle, int code, const char *reason);
} LR_WsWrapper;
```

The host calls the engine-side callbacks when it receives data, pushing events to JS:

```c
void lr_ws_on_open(LR_Runtime *rt, void *conn_handle);
void lr_ws_on_message(LR_Runtime *rt, void *conn_handle, const void *data, size_t len);
void lr_ws_on_close(LR_Runtime *rt, void *conn_handle, int code, const char *reason);
void lr_ws_on_error(LR_Runtime *rt, void *conn_handle, const char *message);
```

#### Usage Example (host side)

```c
static int my_ws_connect(void *ud, const char *url, const char *protocols, void **out_handle) {
    my_conn *c = my_ws_lib_connect(url, protocols);
    if (!c) return -1;
    *out_handle = c;            // use the same handle to push events back later
    return 0;
}
static int my_ws_send(void *ud, void *h, const void *data, size_t len) {
    return my_ws_lib_send((my_conn *)h, data, len);
}
static int my_ws_close(void *ud, void *h, int code, const char *reason) {
    return my_ws_lib_close((my_conn *)h, code, reason);
}

LR_WsWrapper ws = { .user_data = NULL, .connect = my_ws_connect,
                    .send = my_ws_send, .close = my_ws_close };
lr_ws_set_wrapper(rt, &ws);

// When the connection opens / receives a message / closes / errors, call on the engine thread:
//   lr_ws_on_open(rt, conn);
//   lr_ws_on_message(rt, conn, buf, n);
//   lr_ws_on_close(rt, conn, code, reason);
//   lr_ws_on_error(rt, conn, "reason");
```

#### JS API

```js
const ws = new WebSocket("wss://example.com/socket");

ws.onopen    = () => console.log("connected");
ws.onmessage = (e) => console.log("recv:", e.data);
ws.onclose   = (e) => console.log("closed:", e.code, e.reason);
ws.onerror   = (e) => console.log("error:", e.message);

ws.addEventListener("message", (e) => console.log(e.data));

ws.send("hello");
// ...
ws.close();
```

`WebSocket` provides `readyState` (0 `CONNECTING` / 1 `OPEN` / 2 `CLOSING` / 3 `CLOSED`), `url`, `protocol`, `send()`, `close()`, and the `onopen` / `onmessage` / `onclose` / `onerror` event properties.

### 7.10 Event

```js
const emitter = new EventTarget();
emitter.addEventListener("custom", (e) => console.log(e.detail));
emitter.dispatchEvent(new CustomEvent("custom", { detail: 42 }));
```

### 7.11 Worker

```js
const worker = new Worker("worker.js");
worker.postMessage("Hello from main");
worker.onmessage = (e) => console.log("Worker says:", e.data);
worker.terminate();
```

---

## 8. Advanced Features

### 8.1 Generational GC

```bash
# Enable generational GC
./lr_js --gc-generational script.js

# Custom nursery size
./lr_js --gc-generational --gc-nursery-size 8 script.js
```

```c
cfg.gc_mode = LR_GC_MODE_GENERATIONAL;
cfg.gc_generational = 1;
cfg.gc_nursery_size = 8 * 1024 * 1024;  // 8MB nursery
```

### 8.2 Incremental GC

```bash
# Enable incremental GC (default)
./lr_js --gc-incremental script.js

# Custom pause target
./lr_js --gc-incremental --gc-pause-target 3 script.js
```

```c
cfg.gc_mode = LR_GC_MODE_INCREMENTAL;
cfg.gc_incremental = 1;
cfg.gc_pause_target_ns = 3000000;  // 3ms target pause
```

### 8.3 Memory Limit

```bash
# Require at least 2GB of system free memory
./lr_js --min-memory 2147483648 script.js

# Skip memory check
./lr_js --no-memory-check script.js
```

### 8.4 Renderer Bridge

```c
// Select a rendering backend
LR_RendererConfig rcfg = {
    .type = LR_RENDERER_SKIA,
    .socket_path = "/tmp/lr_render.sock",
    .width = 800,
    .height = 600,
};
lr_renderer_init(&rt->renderer, &rcfg);
```

### 8.5 Render Pipeline (External Renderer Output)

The render pipeline (`LR_RenderPipeline`) forwards Canvas 2D and WebGL render output to an external renderer. Each pipeline can mount multiple output sinks, supporting Socket, shared memory, callbacks, and file output.

#### C API

```c
// Create a pipeline
LR_RenderPipeline *pipe = lr_render_pipeline_create(800, 600);

// Add an output sink
LR_PipeSink sock = lr_render_pipe_sink_socket("/tmp/renderer.sock");
lr_render_pipeline_add_sink(pipe, &sock);

LR_PipeSink cb = lr_render_pipe_sink_callback(my_callback, my_data);
lr_render_pipeline_add_sink(pipe, &cb);

// Submit a frame (Canvas 2D framebuffer)
lr_render_pipeline_submit(pipe, pixels, width, height);

// Submit a GL frame (read back from the GLES framebuffer)
lr_render_pipeline_submit_gl(pipe, gl_ctx, width, height);

// Integrate into the renderer bridge
lr_renderer_set_pipeline(rb, pipe);
lr_renderer_pipeline_flush(rb);

// Cleanup
lr_render_pipeline_destroy(pipe);
```

#### JS API

```javascript
// Create a Canvas
const canvas = new Canvas(800, 600);

// Method 1: set pipeline via Canvas (Socket)
canvas.setPipeline('/tmp/renderer.sock');

// 2D context
const ctx = canvas.getContext('2d');
ctx.fillStyle = 'red';
ctx.fillRect(0, 0, 100, 100);
ctx.pipelineFlush();  // submit frame to pipeline

// Method 2: set pipeline via WebGL context
const gl = canvas.getContext('webgl');
gl.setPipeline('/tmp/renderer.sock');
gl.clear(gl.COLOR_BUFFER_BIT);
gl.pipelineFlush();  // read back GL framebuffer and submit

// Method 3: Canvas GL-specific method
canvas.pipelineFlushGL();
```

#### Socket Protocol

Per-frame format sent over the Socket:

```
FRAME <width> <height> <data_size>\n
<raw RGBA pixel data (data_size bytes)>
```

#### Pipe Types

| Type | Create Function | Description |
|------|-----------------|-------------|
| Socket | `lr_render_pipe_sink_socket(path)` | Unix domain socket |
| Shared Memory | `lr_render_pipe_sink_shm(ptr, size)` | Shared memory |
| Callback | `lr_render_pipe_sink_callback(fn, user)` | User callback |
| File | `lr_render_pipe_sink_file(pattern)` | PPM file (debug) |

### 8.6 HTTP Wrapper (External HTTP Delegation)

> L/R_JS itself has no built-in HTTP client. `fetch()` delegates HTTP requests to the host application (browser, WebUI, etc.) via the `LR_HttpWrapper` interface.

#### C API

```c
// Set the HTTP wrapper (host owns ownership, pass NULL to clear)
void lr_http_set_wrapper(LR_Runtime *rt, LR_HttpWrapper *wrapper);

// Get the current HTTP wrapper
LR_HttpWrapper *lr_http_get_wrapper(LR_Runtime *rt);

// Free allocated fields of LR_HttpResult
void lr_http_result_free(LR_HttpResult *result);
```

#### Type Definitions

```c
// HTTP request result
typedef struct LR_HttpResult {
    int         status_code;    // HTTP status code (e.g. 200), 0 on failure
    char       *status_text;    // status text (e.g. "OK"), strdup'd, freed by caller
    char       *headers;        // raw response headers, strdup'd, freed by caller
    char       *body;           // response body, malloc'd, freed by caller
    size_t      body_len;       // response body length (bytes)
    char       *error;          // error message (strdup'd), NULL on success
} LR_HttpResult;

// HTTP wrapper interface
typedef struct LR_HttpWrapper {
    void *user_data;  // opaque data passed through to callbacks

    // Perform an HTTP request. Fill the result struct. Return 0 on success, -1 on failure.
    int (*fetch)(void *user_data, const char *method, const char *url,
                 const char *headers, const void *body, size_t body_len,
                 LR_HttpResult *result);
} LR_HttpWrapper;
```

#### Usage Example

```c
// Host implements the fetch callback
static int my_fetch(void *user_data, const char *method, const char *url,
                    const char *headers, const void *body, size_t body_len,
                    LR_HttpResult *result)
{
    // Use the host's own HTTP library (libcurl, WinHTTP, browser fetch API, etc.)
    // Fill result->status_code, result->body, result->headers, etc.
    result->status_code = 200;
    result->status_text = strdup("OK");
    result->body = strdup("{\"message\":\"Hello from host\"}");
    result->body_len = strlen(result->body);
    result->headers = strdup("Content-Type: application/json\r\n");
    return 0;
}

// Register the wrapper
LR_HttpWrapper wrapper = { .user_data = NULL, .fetch = my_fetch };
lr_http_set_wrapper(rt, &wrapper);

// After that, fetch() can be used normally in JS:
// fetch("https://api.example.com/data").then(r => r.json()).then(console.log);
```

#### JS API

```js
// Once the host registers LR_HttpWrapper, fetch() works normally
const resp = await fetch("https://api.example.com/data");
const json = await resp.json();
console.log(json);

// When no wrapper is registered, fetch() returns a rejected Promise
try {
    await fetch("https://example.com");
} catch (e) {
    console.log(e.message);  // "fetch() is not available: no HTTP wrapper configured"
}
```

### 8.7 File System API (fs)

> L/R_JS provides basic file operation APIs. For operations requiring system privileges (such as writing to system directories), it delegates to the host program through `LR_FileWrapper`; the host is responsible for requesting OS-level privileges (Windows UAC, Linux polkit, etc.). Each batch of privileged operations requires re-requesting privileges.

#### JS API

```js
// Read a file (returns a string)
const data = fs.readFile("/path/to/file.txt");
console.log(data);

// Write a file (overwrite)
fs.writeFile("/path/to/file.txt", "Hello, World!");

// Append
fs.appendFile("/path/to/file.txt", "\nAppended line");

// Read a directory (returns an array of file names)
const entries = fs.readdir("/path/to/dir");
console.log(entries[0], entries[1], /* ... */);

// Create a directory
fs.mkdir("/path/to/newdir");

// Remove an empty directory
fs.rmdir("/path/to/emptydir");

// Delete a file
fs.unlink("/path/to/file.txt");

// Rename / move
fs.rename("/path/to/old.txt", "/path/to/new.txt");

// Get file info
const stat = fs.stat("/path/to/file.txt");
console.log(stat.size, stat.isFile, stat.isDirectory, stat.mtime);

// Check whether a file exists
if (fs.exists("/path/to/file.txt")) {
    console.log("File exists!");
}
```

#### Permission Model

| Operation | Ordinary File | Privileged File |
|----------|---------------|-----------------|
| `readFile` | read directly | fail → call wrapper |
| `writeFile` | write directly | fail → call wrapper |
| `appendFile` | append directly | fail → call wrapper |
| `readdir` | list directly | fail → call wrapper |
| `mkdir` | create directly | fail → call wrapper |
| `rmdir` | remove directly | fail → call wrapper |
| `unlink` | remove directly | fail → call wrapper |
| `rename` | rename directly | fail → call wrapper |
| `stat` | query directly | fail → call wrapper |
| `exists` | query directly | privileged not supported |

#### C API (Host Integration)

```c
// Set the file wrapper (host owns ownership, pass NULL to clear)
void lr_file_set_wrapper(LR_Runtime *rt, LR_FileWrapper *wrapper);

// Get the current file wrapper
LR_FileWrapper *lr_file_get_wrapper(LR_Runtime *rt);

// Free allocated fields of LR_FileResult
void lr_file_result_free(LR_FileResult *result);
```

#### Type Definitions

```c
// File operation result
typedef struct LR_FileResult {
    int         error_code;    // 0 success, errno on failure
    char       *error;         // error message (strdup'd), NULL on success
    char       *data;          // data for read operations, malloc'd
    size_t      data_len;      // data length
    int         is_dir;        // is a directory
    int         is_file;       // is a regular file
    size_t      file_size;     // file size
    char      **entries;       // directory entries (NULL-terminated)
    int         entry_count;   // entry count
} LR_FileResult;

// File system privileged wrapper
typedef struct LR_FileWrapper {
    void *user_data;  // opaque data passed through to callbacks

    // Perform a privileged file operation. Each call must re-request privileges; do not cache.
    // operation: "read_file", "write_file", "delete_file", "rename",
    //            "read_dir", "create_dir", "remove_dir", "stat"
    // Return 0 on success, -1 on failure (fill error_code/error).
    int (*execute)(void *user_data, const char *path, const char *operation,
                   const void *data, size_t data_len, const char *extra,
                   LR_FileResult *result);
} LR_FileWrapper;
```

#### Host Integration Example

```c
static int my_file_execute(void *user_data, const char *path,
                           const char *operation, const void *data,
                           size_t data_len, const char *extra,
                           LR_FileResult *result)
{
    // 1. Request OS privileges (UAC/polkit/...)
    if (!request_admin_privilege(operation, path))
        return -1;

    // 2. Perform the operation
    if (strcmp(operation, "read_file") == 0) {
        // read file, fill result->data/data_len
    } else if (strcmp(operation, "write_file") == 0) {
        // write file
    }
    // ...

    return 0;
}

// Register the wrapper
LR_FileWrapper wrapper = { .user_data = NULL, .execute = my_file_execute };
lr_file_set_wrapper(rt, &wrapper);
```

### 8.8 Terminal API (term)

> L/R_JS provides a terminal command execution API. For commands requiring system privileges (such as accessing system resources), it delegates to the host program through `LR_TerminalWrapper`; the host is responsible for requesting OS-level privileges (Windows UAC, Linux polkit, etc.). Each batch of privileged operations requires re-requesting privileges.

#### JS API

```js
// Run a single command, returns { exitCode, stdout, stderr }
const result = term.run("echo Hello, World!");
console.log(result.exitCode);  // 0
console.log(result.stdout);    // "Hello, World!\n"
console.log(result.stderr);    // ""

// Run a command and capture error output
const r = term.run("ls /nonexistent");
console.log(r.exitCode);       // 2
console.log(r.stderr);         // "ls: cannot access '/nonexistent': No such file or directory\n"

// Run multiple commands in a batch (re-authorized per batch)
const results = term.runBatch([
    "echo first",
    "echo second",
    "echo third"
]);
console.log(results.length);   // 3
console.log(results[0].stdout); // "first\n"

// Real-time output mode (line-by-line callback, supports stdout/stderr separation)
// The callback argument can be a function (shorthand for onStdout) or { onStdout, onStderr, onExit }
term.spawn("ping 127.0.0.1", function(line) {
    console.log("[stdout]", line.trim());
});

term.spawn("some-command", {
    onStdout: function(line) { console.log("OUT:", line); },
    onStderr: function(line) { console.error("ERR:", line); },
    onExit: function(code)  { console.log("EXIT:", code); }
});
```

#### Permission Model

| Operation | Ordinary Command | Privileged Command |
|----------|------------------|--------------------|
| `run` | execute directly (`popen`) | fail → call wrapper |
| `runBatch` | execute one by one directly | fail → call wrapper (re-authorized per batch) |
| `spawn` | execute directly, line-by-line callback | fail → call wrapper (whole-batch output) |

#### C API (Host Integration)

```c
// Set the terminal wrapper (host owns ownership, pass NULL to clear)
void lr_terminal_set_wrapper(LR_Runtime *rt, LR_TerminalWrapper *wrapper);

// Get the current terminal wrapper
LR_TerminalWrapper *lr_terminal_get_wrapper(LR_Runtime *rt);

// Free allocated fields of LR_TerminalResult
void lr_terminal_result_free(LR_TerminalResult *result);
```

#### Type Definitions

```c
// Command execution result
typedef struct LR_TerminalResult {
    int         error_code;    // 0 success, errno on failure
    char       *error;         // error message (strdup'd), NULL on success
    char       *stdout_data;   // standard output, malloc'd
    size_t      stdout_len;    // stdout length
    char       *stderr_data;   // standard error output, malloc'd
    size_t      stderr_len;    // stderr length
    int         exit_code;     // process exit code (0 = success)
} LR_TerminalResult;

// Terminal privileged wrapper
typedef struct LR_TerminalWrapper {
    void *user_data;  // opaque data passed through to callbacks

    // Perform a privileged command. Each call must re-request privileges; do not cache.
    // operation: "run"
    // Return 0 on success, -1 on failure (fill error_code/error).
    int (*execute)(void *user_data, const char *command, const char *operation,
                   const void *stdin_data, size_t stdin_len,
                   LR_TerminalResult *result);
} LR_TerminalWrapper;
```

#### Host Integration Example

```c
static int my_term_execute(void *user_data, const char *command,
                            const char *operation, const void *stdin_data,
                            size_t stdin_len, LR_TerminalResult *result)
{
    // 1. Request OS privileges (UAC/polkit/...)
    if (!request_admin_privilege("execute", command))
        return -1;

    // 2. Execute the command using system() or CreateProcess()
    //    fill result->stdout_data/stdout_len/stderr_data/stderr_len/exit_code
    FILE *fp = popen(command, "r");
    if (!fp) return -1;

    // read output...
    result->exit_code = 0;
    pclose(fp);
    return 0;
}

// Register the wrapper
LR_TerminalWrapper wrapper = {
    .user_data = NULL,
    .execute = my_term_execute
};
lr_terminal_set_wrapper(rt, &wrapper);
```

### 8.9 System Information API (system)

> L/R_JS provides a read-only system information API to obtain OS name, version, kernel version, CPU, GPU, RAM, etc. in JS. All data is read through standard Linux `/proc` and `/sys` filesystems, with no privileged operations required.

#### JS API

```js
// Get OS name
const name = system.name();
console.log(name);  // "Ubuntu", "Debian", "Fedora", etc.

// Get OS version
const version = system.version();
console.log(version);  // "22.04 LTS", "11", etc.

// Get kernel version
const kernel = system.kernel();
console.log(kernel);  // "6.2.0-35-generic"

// Get CPU architecture
const arch = system.arch();
console.log(arch);  // "x86_64", "aarch64", etc.

// Get CPU name
const cpu = system.cpu();
console.log(cpu);  // "Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz"

// Get CPU core count
const cpuCount = system.cpuCount();
console.log(cpuCount);  // 8

// Get GPU info
const gpu = system.gpu();
console.log(gpu);  // "PCI 0x10de:0x1f95" or "Unknown"

// Get RAM info (returns { total, used, free } in bytes)
const ram = system.ram();
console.log(ram.total);  // 17179869184 (16 GB)
console.log(ram.used);   // 8589934592 (8 GB)
console.log(ram.free);   // 8589934592 (8 GB)

// Get system uptime (seconds)
const uptime = system.uptime();
console.log(uptime);  // 123456.78 (seconds)

// Get hostname
const hostname = system.hostname();
console.log(hostname);  // "my-server"

// Get all system info at once
const info = system.info();
console.log(info);
// {
//   name: "Ubuntu",
//   version: "22.04 LTS",
//   kernel: "6.2.0-35-generic",
//   arch: "x86_64",
//   cpu: "Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz",
//   cpuCount: 8,
//   gpu: "PCI 0x10de:0x1f95",
//   hostname: "my-server",
//   uptime: 123456.78,
//   ram: { total: 17179869184, used: 8589934592, free: 8589934592 }
// }
```

#### Return Value Reference

| Function | Return Type | Description |
|----------|-------------|-------------|
| `system.name()` | `string` | OS name (e.g. "Ubuntu") |
| `system.version()` | `string` | OS version |
| `system.kernel()` | `string` | Linux kernel version |
| `system.arch()` | `string` | CPU architecture (e.g. "x86_64") |
| `system.cpu()` | `string` | CPU model name |
| `system.cpuCount()` | `number` | online CPU core count |
| `system.gpu()` | `string` | GPU device info (PCI vendor:device) |
| `system.ram()` | `{total, used, free}` | RAM info (in bytes) |
| `system.uptime()` | `number` | system uptime (seconds) |
| `system.hostname()` | `string` | hostname |
| `system.info()` | `object` | all system info at once |

#### Data Sources

| Info | Source |
|------|--------|
| OS name/version | `/etc/os-release` |
| Kernel version | `uname()` |
| CPU info | `/proc/cpuinfo` |
| GPU info | `/sys/class/drm/card*/device/` |
| RAM info | `/proc/meminfo` |
| Uptime | `/proc/uptime` |
| Hostname | `gethostname()` |

---

### 8.10 Thread Pool

```c
// Create a thread pool
LR_ThreadPoolConfig tcfg = {
    .thread_count = 4,
    .queue_size = 128,
};
lr_thread_pool_init(&rt->thread_pool, &tcfg);

// Submit a task
lr_thread_pool_submit(&rt->thread_pool, my_task, my_data);
```

---

## 9. CLI Reference

### 9.1 Full Argument List

| Argument | Description |
|----------|-------------|
| `-e <code>` | Execute string code |
| `-m <file>` | Execute as an ES Module |
| `-i, --interactive` | Start REPL interactive mode |
| `-h, --help` | Show help |
| `-v, --version` | Show version |
| `--strict` | Enable strict mode |
| `--debug` | Enable debug mode |
| `--memory-limit <bytes>` | Heap memory limit |
| `--gc-threshold <bytes>` | GC threshold |
| `--gc-stress` | GC stress test mode |
| `--gc-generational` | Enable generational GC |
| `--gc-incremental` | Enable incremental GC (default) |
| `--gc-manual` | Disable automatic GC |
| `--gc-nursery-size <mb>` | Set nursery size |
| `--gc-pause-target <ms>` | Set target max pause |
| `--gc-stats` | Print GC stats on exit |
| `--iome586 <dir>` | Enable IOME586 result cache (alias `--bytecode-cache`) |
| `--iome586-stats` | Print cache stats on exit (alias `--bytecode-stats`) |
| `--iome586-revert <js>` | Roll back a script's on-disk cache (to .bak) |
| `--iome586-no-strings` | Do not record any string-literal values in the cache snapshot (sensitive-value guard) |
| `--iome586-restore-globals` | Allow warm runs to restore archived global-variable bindings onto the global object (off by default; opt in as needed) |
| `--sandbox-log <dir>` | Enable sandbox logging |
| `--min-memory <bytes>` | Minimum system memory requirement |
| `--no-memory-check` | Skip system memory check |
| `--dump-bytecode` | Export compiled bytecode |
| `--strip-debug` | Strip debug info |
| `--timeout <ms>` | Execution timeout |
| `--log-level <0-4>` | Log level |
| `--stack-size <bytes>` | Stack size |

### 9.2 REPL Commands

| Command | Description |
|---------|-------------|
| `.exit`, `.quit` | Exit REPL |
| `.help` | Show help |
| `.clear` | Clear screen |
| `.gc` | Trigger GC |
| `.gc_stats` | Show GC stats |
| `.bc_stats` | Show IOME586 cache stats |
| `.memory` | Show memory usage |

---

## 10. Build Guide

### 10.1 Linux

```bash
# Dependencies
sudo apt install build-essential  # Debian/Ubuntu
sudo dnf install gcc make         # Fedora

# Build
cd LR_JS
make clean && make -j$(nproc)

# Output: build/lr_js, build/liblr_js.a
```

### 10.2 macOS

```bash
# Dependencies
xcode-select --install

# Build
cd LR_JS
make clean && make -j$(sysctl -n hw.logicalcpu)

# Output: build/lr_js, build/liblr_js.a
```

### 10.2.1 Cross-compilation (Linux → macOS, osxcross)

If you have no native macOS environment, you can cross-compile macOS `x86_64` / `arm64` binaries from Linux using [osxcross](https://github.com/tpoechtrager/osxcross):

```bash
# 1. Build and install osxcross, and prepare at least one macOS SDK (e.g. MacOSX12.sdk).
#    If your osxcross clang is older than LLVM 15, avoid SDKs of version 15.x
#    (its math.h uses the '_Float16' type, which older clang cannot compile).
#
# 2. Run the script, which auto-detects the toolchain and SDK and builds both architectures:
./build_macos.sh

# You can also specify the SDK explicitly:
LR_OSX_SDK=/path/to/MacOSX12.3.sdk ./build_macos.sh
```

The artifact is `releases/LR_JS-0.1.1-macos-{x86_64,arm64}.tar.gz`, containing `lib/liblr_js.a`, `lib/liblr_js.dylib`, `bin/lr_js`, and `lr_js.h`.

Implementation note: the script bypasses the `o64-clang`/`oa64-clang` launchers and calls the real architecture-targeted clang directly, extracting the precise `-target` triple (e.g. `x86_64-apple-darwin21.4`) from its file name to match the `x86_64-apple-darwin21.4-ld` linker; the SDK is auto-detected preferring the darwin version embedded in clang, falling back to the oldest available SDK.

### 10.3 Windows

```powershell
# MinGW-w64
pacman -S mingw-w64-x86_64-gcc make
cd LR_JS
make clean && make -j%NUMBER_OF_PROCESSORS%

# MSVC
cl /I include /I engine /O2 /Fe:build\lr_js.exe cli\main.c src\*.c engine\*.c
```

### 10.4 FreeBSD

```bash
pkg install gcc gmake
cd LR_JS
gmake clean && gmake -j$(sysctl -n hw.ncpu)
```

### 10.5 Mobile (Android NDK)

```bash
export ANDROID_NDK=/path/to/ndk
export TOOLCHAIN=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64

# arm64
export CC=$TOOLCHAIN/bin/aarch64-linux-android24-clang
export AR=$TOOLCHAIN/bin/llvm-ar

cd LR_JS
make CC=$CC AR=$AR clean && make CC=$CC AR=$AR
```

### 10.6 Mobile (iOS)

```bash
# arm64
export SDK=$(xcrun --sdk iphoneos --show-sdk-path)
export CC="xcrun --sdk iphoneos clang -arch arm64 -isysroot $SDK -miphoneos-version-min=14.0"

cd LR_JS
make CC="$CC" clean && make CC="$CC"
```

---

## 11. Performance Benchmarks

| Test | Time | Description |
|------|------|-------------|
| Empty script startup | < 1ms | runtime creation |
| console.log | < 1ms | basic I/O |
| 100K object loop | ~50ms | memory allocation |
| 50K objects (generational GC) | ~45ms | GC pause < 4ms |
| 50K objects (incremental GC) | ~48ms | GC pause < 2ms |
| IOME586 cache hit | < 5ms | skip lexing/parsing |

---

## 12. Error Codes

| Error Code | Description |
|------------|-------------|
| -1 | Generic error |
| -2 | Out of memory |
| -3 | Timeout |
| -4 | Syntax error |
| -5 | Runtime error |
| -6 | System out of memory |
| -7 | Sandbox restriction |
| -8 | Network error |

---

## 13. Version History

| Version | Date | Description |
|---------|------|-------------|
| 0.1.1 | 2026-07 | **Full bytecode VM**: a stack VM (`lr_bytecode.c`) covering literals, identifiers, all unary/binary/compound-assignment/`**`/bitwise/comparison/`in`/`instanceof`/`typeof`/`delete` operators, `&&`/`\|\|`/`??` short-circuiting, conditional expressions, template literals (concatenated in C), array/object literals, member and computed member get/set, function/method/constructor calls, `for`/`while`/`do-while`/`for-of` (native iteration protocol)/`switch`/labelled `break`/`continue`/`return`/`throw`/block scopes. All JS arithmetic and data handling run in C (int32 fast path, string concat, abstract/strict equality, relational comparison). Closures, classes, generators, async/await, try/catch, destructuring, modules, `for-in`, and `super` are lowered via escape analysis to `BC_EVAL_NODE`, falling back to the same interpreter state so semantics stay identical with no double execution. Bytecode serialization `LRBC` v2 is embedded in the IOME586 archive (marked non-restorable when AST node references exist; the warm path recompiles from the AST). Verified across MSVC/MinGW/GCC/Clang on Windows/macOS/Linux and x86/x64/ARM. Docs add §6.1.0 on cache contents and restorability |
| 0.1.0 | 2026-07 | Initial version: ES2022+ support, multithreaded sandbox, renderer bridge, system memory limit, generational/incremental GC, `.lrfile` bytecode cache and sandbox logging, IOME586 result cache (LZ4 archive, cache-while-running, 15% rule, BOM, rollback), AST serialization `LRA` v3 (explicit literal type tags); fixes the warm-run `true`→`false` bug. Added capabilities: top-level `var`/`function` bound to the global object in non-module Script mode (per `GlobalDeclarationInstantiation`; `let`/`const`/`class` are not mounted), `import.meta` inside modules, the `RegExp` `d` (match indices) flag, Windows console UTF-8 output; IOME586 security hardening (sensitive global-value exclusion, `snapshot_strings` on by default, self-keyed XOR removed, `restore_globals` off by default, BOM baseline remap protection), new CLI flags `--iome586-no-strings` / `--iome586-restore-globals` |

---

## 14. License

MIT License

---

## Reference Links

- FNV-1a: https://en.wikipedia.org/wiki/Fowler-Noll-Vo_hash_function

---

## 15. Promise API

L/R_JS implements the full Promise/A+ specification, supporting all ES2022 Promise static methods.

### 15.1 Constructor

```javascript
const promise = new Promise((resolve, reject) => {
    resolve(42);
    // or reject(new Error("failed"));
});
```

### 15.2 Static Methods

```javascript
// Promise.resolve(value)
Promise.resolve(42).then(v => console.log(v));

// Promise.reject(reason)
Promise.reject(new Error("fail")).catch(e => console.log(e.message));

// Promise.all(iterable) — returns an array of results if all succeed, rejects immediately if any fails
Promise.all([
    Promise.resolve(1),
    Promise.resolve(2),
    Promise.resolve(3),
]).then(results => console.log(results));  // [1, 2, 3]

// Promise.race(iterable) — returns the first settled Promise (resolve or reject)
Promise.race([
    new Promise(resolve => setTimeout(() => resolve("fast"), 10)),
    new Promise(resolve => setTimeout(() => resolve("slow"), 100)),
]).then(v => console.log(v));  // "fast"

// Promise.allSettled(iterable) — wait for all Promises to settle, return each result status
Promise.allSettled([
    Promise.resolve(1),
    Promise.reject("error"),
]).then(results => {
    results.forEach(r => console.log(r.status, r.value || r.reason));
});

// Promise.any(iterable) — returns the first fulfilled Promise, rejects AggregateError if all fail
Promise.any([
    Promise.reject("err1"),
    Promise.resolve("ok"),
    Promise.reject("err3"),
]).then(v => console.log(v));  // "ok"
```

### 15.3 Instance Methods

```javascript
const promise = new Promise(resolve => resolve(42));

// promise.then(onFulfilled, onRejected)
promise.then(
    value => console.log("Fulfilled:", value),
    reason => console.log("Rejected:", reason)
);

// promise.catch(onRejected)
promise.catch(reason => console.error("Error:", reason));

// promise.finally(callback) — runs regardless of success or failure
promise
    .then(v => v * 2)
    .catch(e => 0)
    .finally(() => console.log("Cleanup"));
```

### 15.4 Chaining

> **Note:** The `fetch()` in the following example requires the host to register `LR_HttpWrapper` to work.

```javascript
fetch("https://api.example.com/data")
    .then(resp => resp.json())
    .then(data => {
        console.log("Data:", data);
        return process(data);
    })
    .then(result => {
        console.log("Result:", result);
    })
    .catch(error => {
        console.error("Error:", error);
    });
```

---

## 16. Worker API

L/R_JS supports Web Worker multithreaded execution. Each Worker runs an isolated JS runtime in a separate thread and communicates with the main thread via message passing.

### 16.1 Constructor

```javascript
// Create a Worker that runs the specified script file
const worker = new Worker("worker.js");

// The second argument supports configuration options
const worker = new Worker("worker.js", {
    /* future extension */
});
```

### 16.2 Instance Methods

```javascript
// worker.postMessage(data) — send a message to the Worker
worker.postMessage("Hello from main");
worker.postMessage({cmd: "compute", data: [1, 2, 3]});
worker.postMessage(new Uint8Array([1, 2, 3]));  // Transferable supported

// worker.terminate() — terminate the Worker immediately
worker.terminate();
```

### 16.3 Event Handling

```javascript
// worker.onmessage — receive messages from the Worker
worker.onmessage = (event) => {
    console.log("Received from worker:", event.data);
};

// worker.onerror — handle errors in the Worker
worker.onerror = (event) => {
    console.error("Worker error:", event.message);
};
```

### 16.4 Worker-Side API

```javascript
// worker.js — runs in the Worker thread

// Listen for main-thread messages
self.onmessage = (event) => {
    const data = event.data;
    console.log("Worker received:", data);

    // Send the result back to the main thread
    self.postMessage({result: data.cmd + " done"});
};

// You can also use addEventListener
self.addEventListener("message", (event) => {
    self.postMessage("Processed: " + event.data);
});
```

### 16.5 Complete Example

```javascript
// main.js
const worker = new Worker("worker.js");

worker.onmessage = (e) => {
    console.log("Result:", e.data.result);
    worker.terminate();
};

worker.onerror = (e) => {
    console.error("Worker error:", e.message);
};

worker.postMessage({numbers: [10, 20, 30]});
```

```javascript
// worker.js
self.onmessage = (e) => {
    const {numbers} = e.data;
    const sum = numbers.reduce((a, b) => a + b, 0);
    self.postMessage({result: sum});
};
```

---

## 17. Lock-Free Queue (C API)

L/R_JS provides a CAS-based lock-free queue (MPSC: Multiple Producer, Single Consumer) using a linked-list + stub-node design to avoid the ABA problem.

### 17.1 Data Structures

```c
/* Queue node (embedded in the user's data structure) */
typedef struct LR_LFQNode {
    struct LR_LFQNode *next;
} LR_LFQNode;

/* Lock-free queue */
typedef struct LR_LFQueue {
    LR_LFQNode *head;        /* Consumer pops from head */
    LR_LFQNode *tail;        /* Producer pushes to tail */
    volatile int32_t count;  /* Approximate element count (diagnostic only) */
} LR_LFQueue;

/* Static initializer */
#define LR_LFQ_INIT { NULL, NULL, 0 }
```

### 17.2 API

```c
// Initialize the queue (must be called before use)
LR_LFQueue queue = LR_LFQ_INIT;
lr_lfq_init(&queue);

// Push a node (multi-producer safe)
MyData *node = malloc(sizeof(MyData));
node->value = 42;
lr_lfq_push(&queue, &node->lfq_node);

// Pop a node (single consumer)
LR_LFQNode *node = lr_lfq_pop(&queue);
if (node) {
    MyData *data = (MyData *)((char *)node - offsetof(MyData, lfq_node));
    printf("Value: %d\n", data->value);
    free(data);
}

// Get approximate queue length
int32_t count = lr_lfq_count(&queue);

// Check whether empty
int empty = lr_lfq_is_empty(&queue);

// Drain the queue
LR_LFQNode *all = lr_lfq_drain(&queue);

// Destroy the queue
lr_lfq_destroy(&queue, NULL);           // do not free data
lr_lfq_destroy(&queue, free_data_cb);   // free each node via callback
```

### 17.3 Thread-Safety Notes

| Operation | Safety |
|-----------|--------|
| `lr_lfq_push` | Multi-producer safe (CAS loop, bounded retries) |
| `lr_lfq_pop` | Single-consumer safe only (or external serialization) |
| `lr_lfq_peek` | Single-consumer safe only |
| `lr_lfq_count` | Approximate value, diagnostic only |
| `lr_lfq_drain` | Single-consumer safe only |

### 17.4 Embedding Example

```c
#include "lr_lockfree_queue.h"

typedef struct {
    LR_LFQNode lfq_node;  /* must be the first member or use offsetof */
    int   id;
    char  data[256];
} MyTask;

void producer(LR_LFQueue *queue) {
    for (int i = 0; i < 100; i++) {
        MyTask *task = malloc(sizeof(MyTask));
        task->id = i;
        lr_lfq_push(queue, &task->lfq_node);
    }
}

void consumer(LR_LFQueue *queue) {
    LR_LFQNode *node;
    while ((node = lr_lfq_pop(queue)) != NULL) {
        MyTask *task = (MyTask *)node;
        printf("Task %d processed\n", task->id);
        free(task);
    }
}
```

---

## 18. Cross-Platform Compatibility Layer

L/R_JS provides a unified cross-platform abstraction layer through `lr_platform.h`, supporting Linux, macOS, Windows 7+, FreeBSD, OpenBSD, and NetBSD.

### 18.1 Supported Platforms

| Macro | Platform |
|-------|----------|
| `LR_PLATFORM_WINDOWS` | Windows 7+ (MSVC/MinGW) |
| `LR_PLATFORM_LINUX` | Linux (GCC/Clang) |
| `LR_PLATFORM_MACOS` | macOS (Apple Clang) |
| `LR_PLATFORM_BSD` | FreeBSD / OpenBSD / NetBSD |

### 18.2 Compiler Detection

| Macro | Compiler |
|-------|----------|
| `LR_COMPILER_MSVC` | MSVC (cl.exe) |
| default | GCC / Clang |

### 18.3 Build Options

```bash
# MSVC (Windows)
cl /I include /I src /I src/engine /O2 /Fe:build\lr_js.exe cli\main.c src\*.c engine\*.c

# GCC (Linux/macOS)
gcc -O2 -g -Wall -Wextra -D_GNU_SOURCE -pthread \
    -I include -I src -I src/engine \
    -o build/lr_js cli/main.c src/*.c src/engine/*.c \
    -lm -lpthread -ldl

# MinGW-w64 (Windows cross-compile)
x86_64-w64-mingw32-gcc -O2 -g -Wall -Wextra -D_GNU_SOURCE -pthread \
    -I include -I src -I src/engine \
    -o build/lr_js.exe cli/main.c src/*.c src/engine/*.c \
    -lm -lpthread -lws2_32
```

### 18.4 Atomic Operations API

L/R_JS provides a unified atomic operations layer, using `Interlocked*` functions on Windows and GCC `__sync_*` builtins on POSIX systems.

```c
// 32-bit CAS
int32_t lr_atomic_cas_32(volatile int32_t *ptr, int32_t oldval, int32_t newval);

// Pointer CAS
void *lr_atomic_cas_ptr(volatile void **ptr, void *oldval, void *newval);

// Atomic exchange
int32_t lr_atomic_xchg_32(volatile int32_t *ptr, int32_t val);
void *lr_atomic_xchg_ptr(volatile void **ptr, void *val);

// Atomic fetch-and-add
int32_t lr_atomic_fetch_add_32(volatile int32_t *ptr, int32_t val);

// Atomic store/load
void  lr_atomic_store_32(volatile int32_t *ptr, int32_t val);
void  lr_atomic_store_ptr(volatile void **ptr, void *val);
int32_t lr_atomic_load_32(volatile int32_t *ptr);
void *lr_atomic_load_ptr(volatile void **ptr);

// 64-bit atomic operations
int64_t lr_atomic_cas_64(volatile int64_t *ptr, int64_t oldval, int64_t newval);
int64_t lr_atomic_load_64(volatile int64_t *ptr);

// Memory barriers
void lr_memory_barrier(void);
void lr_write_barrier(void);
void lr_read_barrier(void);
```

### 18.5 Convenience Macros

```c
// Atomic increment/decrement
#define LR_ATOMIC_INC(ptr)    lr_atomic_fetch_add_32((ptr), 1)
#define LR_ATOMIC_DEC(ptr)    lr_atomic_fetch_add_32((ptr), -1)

// Atomic test-and-set (returns 1 if already set, 0 if not)
#define LR_ATOMIC_TEST_AND_SET(ptr)  (lr_atomic_xchg_32((ptr), 1) != 0)

// Atomic clear flag
#define LR_ATOMIC_CLEAR(ptr)  lr_atomic_store_32((ptr), 0)
```

### 18.6 Cross-Platform API Mapping

| Feature | POSIX | Windows |
|---------|-------|---------|
| File ops | `open/close/read/write/lseek` | `_open/_close/_read/_write/_lseek` |
| Socket close | `close(s)` | `closesocket(s)` |
| Dynamic lib load | `dlopen/dlsym/dlclose` | `LoadLibrary/GetProcAddress/FreeLibrary` |
| Threads | `pthread` | `lr_pthread_win.h` compat layer |
| High-res time | `gettimeofday` | `QueryPerformanceCounter` |
| Random | `/dev/urandom` | `CryptGenRandom` |
| Memory info | `sysconf` | `GlobalMemoryStatusEx` |
| Socket init | no-op | `WSAStartup` |
| Path separator | `/` | `\\` |

### 18.7 Platform-Specific Headers

```c
// All source files include lr_platform.h instead of platform-specific headers
#include "lr_platform.h"   // auto-handles pthread, socket, dynamic lib, etc.

// Windows extra compat layer
#include "lr_pthread_win.h"  // MSVC pthread emulation
```

---

## 19. Task Scheduler (C API)

L/R_JS provides a priority-based asynchronous task scheduler, supporting one-shot, repeating, and timed tasks.

### 19.1 API

```c
// Create a scheduler
LR_Scheduler *sched = lr_scheduler_create(thread_pool);

// Schedule a repeating task
int task_id = lr_scheduler_schedule(sched, task,
    LR_TASK_PRIORITY_NORMAL, 1000000 /* 1s interval */, -1 /* repeat forever */);

// Schedule a delayed task
lr_scheduler_schedule_delayed(sched, task,
    LR_TASK_PRIORITY_HIGH, 500000 /* 500ms delay */);

// Schedule immediate execution
lr_scheduler_schedule_now(sched, task, LR_TASK_PRIORITY_CRITICAL);

// Cancel a task
lr_scheduler_cancel(sched, task_id);

// Run the event loop (blocking)
lr_scheduler_run(sched);

// Process pending tasks (non-blocking)
lr_scheduler_process(sched);

// Stop the scheduler
lr_scheduler_stop(sched);

// Destroy the scheduler
lr_scheduler_destroy(sched);
```

### 19.2 Task Priorities

```c
typedef enum {
    LR_TASK_PRIORITY_LOW      = 0,
    LR_TASK_PRIORITY_NORMAL   = 1,
    LR_TASK_PRIORITY_HIGH     = 2,
    LR_TASK_PRIORITY_CRITICAL = 3,
} LR_TaskPriority;
```

---

## 20. Map & Set API

L/R_JS implements the full ES2022 Map and Set objects, backed by a C hash table.

### 20.1 Map

```javascript
// Constructor
const map = new Map();
const map2 = new Map([['key1', 'val1'], ['key2', 'val2']]);

// Instance methods
map.set(key, value);          // set a key-value pair, returns the map itself (chainable)
map.get(key);                 // get value, returns undefined if absent
map.has(key);                 // check whether a key exists
map.delete(key);              // delete a key, returns true/false
map.clear();                  // clear all entries
map.size;                     // entry count (read-only getter)
map.forEach(callback, thisArg); // iterate all entries
```

### 20.2 Set

```javascript
// Constructor
const set = new Set();
const set2 = new Set([1, 2, 3]);

// Instance methods
set.add(value);               // add a value, returns the set itself (chainable)
set.has(value);               // check whether a value exists
set.delete(value);            // delete a value, returns true/false
set.clear();                  // clear all values
set.size;                     // entry count (read-only getter)
set.forEach(callback, thisArg); // iterate all values
```

---

## 21. Proxy & Reflect API

L/R_JS supports ES2022 Proxy and Reflect, implementing 7 core traps.

### 21.1 Proxy

```javascript
// Constructor
const proxy = new Proxy(target, handler);

// Supported traps
const handler = {
    get(target, prop, receiver) { return Reflect.get(target, prop, receiver); },
    set(target, prop, value, receiver) { return Reflect.set(target, prop, value, receiver); },
    has(target, prop) { return Reflect.has(target, prop); },
    deleteProperty(target, prop) { return Reflect.deleteProperty(target, prop); },
    ownKeys(target) { return Reflect.ownKeys(target); },
    apply(target, thisArg, args) { return Reflect.apply(target, thisArg, args); },
    construct(target, args, newTarget) { return Reflect.construct(target, args); },
};
```

### 21.2 Reflect

```javascript
// Static methods
Reflect.get(target, propertyKey, receiver);
Reflect.set(target, propertyKey, value, receiver);
Reflect.has(target, propertyKey);
Reflect.deleteProperty(target, propertyKey);
Reflect.ownKeys(target);
Reflect.apply(target, thisArg, args);
Reflect.construct(target, args);
Reflect.defineProperty(target, propertyKey, attributes);
Reflect.getPrototypeOf(target);
Reflect.setPrototypeOf(target, proto);
Reflect.isExtensible(target);
```

---

## 22. Errors and Stack Traces

L/R_JS supports full error stack traces, V8-compatible in format.

### 22.1 Error.prototype.stack

```javascript
try {
    throw new Error("something went wrong");
} catch (e) {
    console.log(e.stack);
    // Output:
    // Error: something went wrong
    //     at <eval> (input:1)
}
```

### 22.2 Error.captureStackTrace

```javascript
function MyError(message) {
    this.message = message;
    Error.captureStackTrace(this, MyError);
}

const err = new MyError("custom error");
console.log(err.stack);  // has stack info but omits the captureStackTrace call
```

### 22.3 Error.stackTraceLimit

```javascript
Error.stackTraceLimit = 20;  // control the max number of stack frames (default 10)
```

---

## 23. Performance Optimization

The L/R_JS engine includes several performance optimizations:

### 23.1 AST Node Pool
- The parser pre-allocates 4096 AST nodes to reduce `malloc` calls
- Constant nodes (0, 1, true, false, null, undefined) are globally cached

### 23.2 String Interning
- A 256-slot hash table atomizes identifiers and string literals
- Avoids duplicate `strdup` and string comparisons

### 23.3 Direct Threading
- The interpreter uses a function-pointer table instead of a `switch` statement
- Reduces branch-misprediction, improves execution efficiency

### 23.4 Inline Cache
- A 64-slot property-access cache caching recently accessed property offsets
- Reduces property-lookup overhead

### 23.5 Integer Fast Path
- `int32 + int32` fast path for binary operations (14 operations)
- Avoids object boxing and function-call overhead

### 23.6 Shape Cache
- A 128-slot shape cache caching (object, property) pairs for `get_property`/`set_property`
- Accelerates property access

### 23.7 Small String Cache
- A 128-slot cache for strings of length <= 32
- Avoids repeated allocation of short strings

### 23.8 GC Tuning
- Initial GC threshold: 1MB
- Young generation size: 8MB
- GC pause target: 10ms

---

## 24. ES2022 Core Built-in Objects

L/R_JS implements the full ES2022 core built-in objects, including Object, Array, String, Number, Boolean, Function, Math, JSON, Date, RegExp, Symbol, Error subclasses, WeakMap, and WeakSet.

### 24.1 Object

```javascript
// Static methods
Object.keys(obj);           // array of enumerable own property names
Object.values(obj);         // array of enumerable own property values
Object.entries(obj);        // array of [key, value] pairs
Object.fromEntries(entries); // create object from [key, value] pairs
Object.assign(target, ...sources); // copy properties
Object.create(proto);       // create object with given prototype
Object.defineProperty(obj, prop, desc); // define a property
Object.defineProperties(obj, props);    // define multiple properties
Object.freeze(obj);         // freeze object
Object.seal(obj);           // seal object
Object.isExtensible(obj);   // check extensibility
Object.preventExtensions(obj); // prevent extensions
Object.is(v1, v2);          // SameValue comparison
Object.hasOwn(obj, prop);   // ES2022 own-property check
Object.getPrototypeOf(obj); // get prototype
Object.setPrototypeOf(obj, proto); // set prototype
Object.getOwnPropertyNames(obj);   // get own property names
Object.getOwnPropertyDescriptor(obj, prop); // get property descriptor
Object.getOwnPropertyDescriptors(obj);      // get all property descriptors

// Prototype methods
Object.prototype.toString();
Object.prototype.hasOwnProperty(prop);
Object.prototype.isPrototypeOf(obj);
Object.prototype.propertyIsEnumerable(prop);
Object.prototype.valueOf();
```

### 24.2 Array

```javascript
// Static methods
Array.isArray(val);         // check whether it is an array
Array.from(arrayLike);      // create from array-like / iterable
Array.of(...items);         // create from arguments

// Prototype methods (ES2022+)
Array.prototype.at(index);       // ES2022 index access
Array.prototype.concat(...arrs);
Array.prototype.copyWithin(target, start, end);
Array.prototype.entries();
Array.prototype.every(callback);
Array.prototype.fill(value, start, end);
Array.prototype.filter(callback);
Array.prototype.find(callback);
Array.prototype.findIndex(callback);
Array.prototype.findLast(callback);      // ES2023
Array.prototype.findLastIndex(callback); // ES2023
Array.prototype.flat(depth);            // ES2019
Array.prototype.flatMap(callback);      // ES2019
Array.prototype.forEach(callback);
Array.prototype.includes(value);        // ES2016
Array.prototype.indexOf(value);
Array.prototype.join(separator);
Array.prototype.keys();
Array.prototype.lastIndexOf(value);
Array.prototype.map(callback);
Array.prototype.pop();
Array.prototype.push(...items);
Array.prototype.reduce(callback, initial);
Array.prototype.reduceRight(callback, initial);
Array.prototype.reverse();
Array.prototype.shift();
Array.prototype.slice(start, end);
Array.prototype.some(callback);
Array.prototype.sort(compareFn);
Array.prototype.splice(start, deleteCount, ...items);
Array.prototype.toReversed();           // ES2023
Array.prototype.toSorted(compareFn);    // ES2023
Array.prototype.toSpliced(start, delCount, ...items); // ES2023
Array.prototype.toString();
Array.prototype.unshift(...items);
Array.prototype.values();
Array.prototype.with(index, value);     // ES2023
```

### 24.3 String

```javascript
// Static methods
String.fromCharCode(...codes);
String.fromCodePoint(...points);

// Prototype methods
String.prototype.at(index);           // ES2022
String.prototype.charAt(index);
String.prototype.charCodeAt(index);
String.prototype.codePointAt(index);
String.prototype.concat(...strs);
String.prototype.includes(searchString, position);
String.prototype.indexOf(searchString, position);
String.prototype.lastIndexOf(searchString, position);
String.prototype.match(regexp);
String.prototype.matchAll(regexp);     // ES2020
String.prototype.replace(searchValue, replaceValue);
String.prototype.replaceAll(searchValue, replaceValue); // ES2021
String.prototype.search(regexp);
String.prototype.slice(start, end);
String.prototype.split(separator, limit);
String.prototype.substring(start, end);
String.prototype.toLowerCase();
String.prototype.toUpperCase();
String.prototype.trim();
String.prototype.trimStart();  // ES2019
String.prototype.trimEnd();    // ES2019
String.prototype.padStart(targetLength, padString); // ES2017
String.prototype.padEnd(targetLength, padString);   // ES2017
String.prototype.startsWith(searchString, position); // ES2015
String.prototype.endsWith(searchString, endPosition); // ES2015
String.prototype.repeat(count);   // ES2015
String.prototype.toString();
String.prototype.valueOf();
```

### 24.4 Number

```javascript
// Static properties
Number.MAX_VALUE;       // 1.7976931348623157e+308
Number.MIN_VALUE;       // 5e-324
Number.NaN;
Number.NEGATIVE_INFINITY;
Number.POSITIVE_INFINITY;
Number.EPSILON;         // 2.220446049250313e-16
Number.MAX_SAFE_INTEGER; // 9007199254740991
Number.MIN_SAFE_INTEGER; // -9007199254740991

// Static methods
Number.isNaN(value);
Number.isFinite(value);
Number.isInteger(value);
Number.isSafeInteger(value);
Number.parseFloat(string);
Number.parseInt(string, radix);

// Prototype methods
Number.prototype.toFixed(digits);
Number.prototype.toPrecision(precision);
Number.prototype.toString(radix);
Number.prototype.valueOf();
```

### 24.5 Boolean

```javascript
// Prototype methods
Boolean.prototype.toString();
Boolean.prototype.valueOf();
```

### 24.6 Function

```javascript
// Prototype methods
Function.prototype.bind(thisArg, ...args);
Function.prototype.call(thisArg, ...args);
Function.prototype.apply(thisArg, argsArray);
Function.prototype.toString();
```

### 24.7 Math

```javascript
// Constants
Math.E;     Math.LN2;    Math.LN10;   Math.LOG2E;
Math.LOG10E; Math.PI;    Math.SQRT1_2; Math.SQRT2;

// Methods
Math.abs(x);     Math.acos(x);    Math.acosh(x);    Math.asin(x);
Math.asinh(x);   Math.atan(x);    Math.atan2(y, x); Math.atanh(x);
Math.cbrt(x);    Math.ceil(x);    Math.clz32(x);    Math.cos(x);
Math.cosh(x);    Math.exp(x);     Math.expm1(x);    Math.floor(x);
Math.fround(x);  Math.hypot(...); Math.imul(a, b);  Math.log(x);
Math.log10(x);   Math.log1p(x);   Math.log2(x);     Math.max(...);
Math.min(...);   Math.pow(x, y);  Math.random();    Math.round(x);
Math.sign(x);    Math.sin(x);     Math.sinh(x);     Math.sqrt(x);
Math.tan(x);     Math.tanh(x);    Math.trunc(x);
```

### 24.8 JSON

```javascript
JSON.stringify(value, replacer, space);  // serialize
JSON.parse(text, reviver);               // parse
```

### 24.9 Date

```javascript
// Static methods
Date.now();
Date.parse(dateString);
Date.UTC(year, month, day, hour, min, sec, ms);

// Prototype methods
Date.prototype.getFullYear();
Date.prototype.getMonth();
Date.prototype.getDate();
Date.prototype.getDay();
Date.prototype.getHours();
Date.prototype.getMinutes();
Date.prototype.getSeconds();
Date.prototype.getMilliseconds();
Date.prototype.getTime();
Date.prototype.setFullYear(year);
Date.prototype.setMonth(month);
Date.prototype.setDate(day);
Date.prototype.setHours(hour);
Date.prototype.setMinutes(min);
Date.prototype.setSeconds(sec);
Date.prototype.setMilliseconds(ms);
Date.prototype.setTime(time);
Date.prototype.toString();
Date.prototype.toISOString();
Date.prototype.toJSON(key);
Date.prototype.toLocaleDateString();
Date.prototype.toLocaleTimeString();
Date.prototype.valueOf();
```

### 24.10 RegExp

```javascript
// Constructor
new RegExp(pattern, flags);

// Prototype methods
RegExp.prototype.exec(str);
RegExp.prototype.test(str);
RegExp.prototype.toString();

// Properties
reg.lastIndex;     reg.source;    reg.flags;
reg.global;        reg.ignoreCase; reg.multiline;
reg.dotAll;        reg.sticky;    reg.unicode;
```

### 24.11 Symbol

```javascript
// Constructor
Symbol(description);
Symbol.for(key);
Symbol.keyFor(sym);

// Well-known symbols
Symbol.iterator;         Symbol.asyncIterator;
Symbol.hasInstance;      Symbol.isConcatSpreadable;
Symbol.match;            Symbol.replace;
Symbol.search;           Symbol.split;
Symbol.toPrimitive;      Symbol.toStringTag;
Symbol.unscopables;      Symbol.species;
Symbol.matchAll;
```

### 24.12 Error Subclasses

```javascript
TypeError     // type error
RangeError    // range error
SyntaxError   // syntax error
ReferenceError // reference error
EvalError     // eval error
URIError      // URI error
```

### 24.13 WeakMap / WeakSet

```javascript
// WeakMap
const wm = new WeakMap();
wm.set(key, value);
wm.get(key);
wm.has(key);
wm.delete(key);

// WeakSet
const ws = new WeakSet();
ws.add(value);
ws.has(value);
ws.delete(value);
```

---

## 25. ES2022+ Syntax Features

### 25.1 globalThis and top-level declaration binding

```javascript
globalThis === window;  // true (the global object is `window` in browser semantics)
```

**Top-level `var` / `function` are bound to the global object (Script mode only).** Per
the ECMAScript `GlobalDeclarationInstantiation` spec, in a *non-module* Script, top-level
`var` and `function` declarations become properties of the global object; `let` / `const` /
`class` are declarative bindings and are *not* mirrored onto the global object. Under ES
modules (`.mjs` or `-m`) every top-level declaration lives in the module namespace and never
pollutes the global object.

```javascript
// Run as a script (default)
var topVar = 42;
function topFunc() { return "fn"; }
let topLet = 1;
const topConst = 2;
class TopClass {}

globalThis.topVar;            // 42
typeof globalThis.topFunc;   // "function"
"topLet"    in globalThis;   // false (declarative, not mounted)
"topConst"  in globalThis;   // false
"TopClass"  in globalThis;   // false

topVar = 200;
globalThis.topVar;           // 200 (assignment is mirrored onto the global object)

globalThis.topVar = 100;
topVar;                      // 100 (two-way binding: external write reflects back to bare)

// Run as a module (-m / .mjs)
//   'mVar' in globalThis  -> false
//   'mFunc' in globalThis -> false
```

### 25.2 for...of loop
```javascript
for (const v of [1, 2, 3]) {
    console.log(v);
}
```

### 25.3 Logical assignment operators
```javascript
x ||= y;  // x = x || y
x &&= y;  // x = x && y
x ??= y;  // x = x ?? y
```

### 25.4 Numeric separators
```javascript
const million = 1_000_000;  // 1000000
const bits = 0xFF_FF_FF;    // 16777215
const binary = 0b1010_0001; // 161
```

### 25.5 `import.meta`
Inside a module (`.mjs` or `-m`), `import.meta` exposes metadata about the current module:
```javascript
// module.mjs
import.meta.url;       // "file:///abs/path/module.mjs"
import.meta.filename;  // absolute path
import.meta.dirname;   // containing directory
console.log(import.meta.url);
```
Accessing `import.meta` in a non-module script is an error.
