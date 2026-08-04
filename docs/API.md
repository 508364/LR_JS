# L/R_JS API Reference

> 全功能轻量级浏览器 JS 运行器 | Pure C | ES2022+ | Multithreaded | Async Sandbox

---

## 1. 概述

L/R_JS 是一个用纯 C 语言实现的轻量级浏览器 JavaScript 运行器，支持 ES2022+ 特性，提供多线程、多任务、异步沙箱执行环境，内建高性能 JS 引擎。

**v0.1.1+**：执行引擎为直接/间接线程式字节码 VM，AST 树遍历已退役。函数体字节码执行（bc_body_cache MRU O(1)），arguments 惰性创建，Array reduce C 直读，BC_PUSH_THIS opcode，AST 依赖分析并行拆分（`--parallel N` 1-16），IOME586 字节码预热缓存 + CAS 预编译。基准 2796→1861ms (-33%)，vs V8 45×。

### 1.1 支持平台

| 平台 | 架构 | 编译器 | 最低版本 |
|------|------|--------|----------|
| **Linux** | x86_64, x86, aarch64, armv7 | GCC 9+, Clang 12+ | kernel 3.10+ |
| **macOS** | x86_64, arm64 (Apple Silicon) | Clang 14+ | macOS 11+ |
| **Windows** | x86_64, x86, aarch64 | MSVC 2022+, MinGW-w64 | Windows 7+ |
| **FreeBSD** | x86_64, aarch64 | Clang 14+ | FreeBSD 13+ |
| **OpenBSD** | x86_64, aarch64 | Clang 14+ | OpenBSD 7.0+ |
| **NetBSD** | x86_64, aarch64 | GCC 10+ | NetBSD 9.0+ |
| **Android** | aarch64, armv7, x86_64 | NDK r25+ | API 24+ |
| **iOS** | arm64 | Xcode 15+ | iOS 14+ |

### 1.2 通过条件编译支持跨平台

```c
// 内存检测
#ifdef __linux__
    // /proc/meminfo
#elif defined(__APPLE__)
    // sysctl
#elif defined(_WIN32)
    // GlobalMemoryStatusEx
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    // sysctl
#endif

// UUID 生成
#ifdef _WIN32
    // CryptGenRandom
#else
    // /dev/urandom
#endif

// 文件系统
#ifdef _WIN32
    #define lr_mkdir(p) _mkdir(p)
#else
    #define lr_mkdir(p) mkdir(p, 0755)
#endif
```

---

## 2. 快速开始

### 2.1 命令行使用

```bash
# 基本用法
./lr_js script.js
./lr_js -e "console.log('Hello, L/R_JS!')"
./lr_js --interactive       # 启动 REPL

# 完整示例
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

### 2.2 C API 基本使用

```c
#include "lr_js.h"

int main() {
    // 1. 配置运行时
    LR_Config cfg;
    lr_config_default(&cfg);
    cfg.memory_limit = 128 * 1024 * 1024;  // 128MB 堆限制
    cfg.gc_incremental = 1;                // 启用增量 GC
    cfg.bytecode_cache_dir = "./cache";    // 启用 IOME586 结果缓存
    cfg.skip_memory_check = 1;             // 跳过系统内存检查

    // 2. 创建运行时
    LR_Runtime *rt = lr_runtime_new(&cfg);
    if (!rt) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    // 3. 执行脚本
    lr_eval(rt, "console.log('Hello!')", 20, "<eval>");
    // 或执行文件
    lr_eval_file(rt, "script.js");

    // 4. 清理
    lr_runtime_free(rt);
    return 0;
}
```

### 2.3 编译链接

```bash
# Linux / macOS / BSD
gcc -o myapp myapp.c -I/path/to/LR_JS/include -L/path/to/LR_JS/build -llr_js -lpthread -ldl -lm

# Windows (MSVC)
cl myapp.c /I path\to\LR_JS\include /link path\to\LR_JS\build\lr_js.lib

# Windows (MinGW)
gcc -o myapp.exe myapp.c -I/path/to/LR_JS/include -L/path/to/LR_JS/build -llr_js -lpthread -lws2_32
```

---

## 3. 核心 API

### 3.1 运行时管理

| 函数 | 说明 |
|------|------|
| `LR_Runtime *lr_runtime_new(LR_Config *cfg)` | 创建运行时 |
| `void lr_runtime_free(LR_Runtime *rt)` | 销毁运行时 |
| `const char *lr_version(void)` | 获取版本号 |

### 3.2 脚本执行

| 函数 | 说明 |
|------|------|
| `int lr_eval(rt, src, len, filename)` | 执行字符串脚本 |
| `int lr_eval_file(rt, filename)` | 执行文件脚本 |
| `int lr_eval_module(rt, src, len, filename)` | 执行 ES Module |
| `int lr_eval_module_file(rt, filename)` | 执行 ES Module 文件 |

### 3.3 事件循环

| 函数 | 说明 |
|------|------|
| `int lr_event_loop_pending(rt)` | 检查是否有待处理任务 |
| `void lr_event_loop_run(rt)` | 运行事件循环 |
| `void lr_event_loop_stop(rt)` | 停止事件循环 |

### 3.4 内存管理

| 函数 | 说明 |
|------|------|
| `void lr_gc(rt)` | 手动触发 GC |
| `void lr_gc_print_stats(rt, fp)` | 打印 GC 统计 |
| `void lr_gc_reset_stats(rt)` | 重置 GC 统计 |
| `void lr_compute_memory_usage(rt, usage)` | 获取内存使用详情 |
| `void lr_dump_memory_usage(rt, fp)` | 打印内存使用详情 |
| `int64_t lr_get_available_memory(void)` | 获取系统可用内存 |
| `int lr_check_system_memory(min_bytes)` | 检查系统内存是否充足 |

### 3.5 IOME586 结果缓存

| 函数 | 说明 |
|------|------|
| `void lr_bytecode_cache_stats(rt, fp)` | 打印缓存统计（内部封装 `lr_iome586_stats`） |
| `void lr_bytecode_cache_clear(rt)` | 清空缓存（内部封装 `lr_iome586_clear`） |

### 3.6 错误处理

| 函数 | 说明 |
|------|------|
| `int lr_get_last_error(rt, buf, size)` | 获取最后的错误信息 |
| `void lr_clear_last_error(rt)` | 清除错误状态 |

---

## 4. 配置结构

### 4.1 LR_Config

```c
typedef struct LR_Config {
    // 内存
    size_t  memory_limit;           // 最大堆内存 (bytes), 0=无限制
    size_t  gc_threshold;           // 自动 GC 阈值, 0=默认
    LR_GCMode gc_mode;             // GC 模式

    // GC 调优
    int     gc_generational;        // 启用分代 GC (默认 1)
    int     gc_incremental;         // 启用增量 GC (默认 1)
    size_t  gc_nursery_size;        // nursery 大小, 0=默认 4MB
    int64_t gc_pause_target_ns;     // 目标最大暂停, 0=默认 5ms

    // IOME586 结果缓存
    char   *bytecode_cache_dir;     // IOME586 缓存目录 (.lrfile.lz4 归档), NULL=禁用

    // 沙箱日志
    char   *sandbox_log_dir;        // 沙箱日志目录, NULL=禁用

    // 系统内存
    size_t  min_system_memory;      // 最小系统可用内存, 0=禁用
    int     skip_memory_check;      // 跳过系统内存检查

    // 栈
    size_t  max_stack_size;         // 最大栈大小, 0=默认 1MB

    // 执行
    int     timeout_ms;             // 执行超时, 0=无限制
    int     strict_mode;            // 严格模式
    int     debug_mode;             // 调试模式

    // 日志
    int     log_level;              // 日志级别: 0=关闭, 1=错误, 2=警告, 3=信息, 4=调试
    FILE   *log_file;               // 日志输出文件, NULL=stderr

    // 编译
    int     dump_bytecode;          // 导出字节码
    int     strip_debug_info;       // 剥离调试信息
} LR_Config;
```

### 4.2 GC 模式

```c
typedef enum {
    LR_GC_MODE_AUTO          = 0,  // 自动 GC
    LR_GC_MODE_MANUAL        = 1,  // 手动 GC
    LR_GC_MODE_STRESS        = 2,  // 压力测试 (每次分配后 GC)
    LR_GC_MODE_GENERATIONAL  = 3,  // 分代 GC (nursery + old gen)
    LR_GC_MODE_INCREMENTAL   = 4,  // 增量 GC (时间切片 + 分代)
} LR_GCMode;
```

### 4.3 内存使用

```c
typedef struct LR_MemoryUsage {
    int64_t malloc_size;          // 已分配内存
    int64_t malloc_limit;         // 内存限制
    int64_t memory_used_size;     // 已使用内存
    int64_t memory_used_count;    // 使用中的块数
    int64_t atom_count;           // 原子数
    int64_t atom_size;            // 原子大小
    int64_t str_count;            // 字符串数
    int64_t str_size;             // 字符串大小
    int64_t obj_count;            // 对象数
    int64_t obj_size;             // 对象大小
    int64_t prop_count;           // 属性数
    int64_t prop_size;            // 属性大小
    int64_t shape_count;          // 形状数
    int64_t shape_size;           // 形状大小
    int64_t js_func_count;        // JS 函数数
    int64_t js_func_size;         // JS 函数大小
    int64_t js_func_code_size;    // JS 函数代码大小
    int64_t js_func_pc2line_count;// 行号映射条目
    int64_t js_func_pc2line_size; // 行号映射大小
    int64_t c_func_count;         // C 函数数
    int64_t array_count;          // 数组数
    int64_t fast_array_count;     // 快速数组数
    int64_t fast_array_elements;  // 快速数组元素数
    int64_t binary_object_count;  // 二进制对象数
    int64_t binary_object_size;   // 二进制对象大小
} LR_MemoryUsage;
```

---

## 5. 沙箱 API

### 5.1 沙箱管理器

```c
#include "lr_sandbox.h"
#include "lr_sandbox_log.h"

// 创建沙箱管理器
LR_SandboxManager *mgr = lr_sandbox_manager_create(16);  // 最多 16 个沙箱

// 销毁沙箱管理器
lr_sandbox_manager_destroy(mgr);
```

### 5.2 沙箱配置

```c
typedef struct LR_SandboxConfig {
    char   *name;              // 沙箱名称
    char   *log_dir;           // 日志目录 (NULL=禁用)
    size_t  memory_limit;      // 堆限制, 0=继承
    size_t  stack_size;        // 栈大小, 0=默认
    int     timeout_ms;        // 超时, 0=无限制
    int     max_eval_depth;    // 最大嵌套深度
    int     allow_network;     // 允许网络 API
    int     allow_filesystem;  // 允许文件 API
    int     allow_workers;     // 允许 Worker
    int     isolated_context;  // 隔离上下文
} LR_SandboxConfig;
```

### 5.3 沙箱操作

```c
// 创建沙箱
LR_SandboxConfig cfg = {
    .name = "my-sandbox",
    .log_dir = "./sandbox_logs",     // 启用日志
    .memory_limit = 64 * 1024 * 1024,
    .timeout_ms = 5000,
    .allow_network = 0,
    .isolated_context = 1,
};
LR_Sandbox *sb = lr_sandbox_create(mgr, &cfg);

// 获取 UUID
printf("Sandbox UUID: %s\n", sb->uuid);
// 输出: Sandbox UUID: a1b2c3d4-e5f6-47a8-b9c0-d1e2f3a4b5c6

// 执行脚本
lr_sandbox_eval(sb, "console.log('sandbox running')", 30, "<sandbox>");

// 获取状态
LR_SandboxState state = lr_sandbox_get_state(sb);

// 销毁沙箱
lr_sandbox_destroy(mgr, sb);
```

### 5.4 沙箱日志系统

每个沙箱自动获得独立的日志文件：

```
{sandbox_log_dir}/{YYYY-MM-DD}-{run_count}-{uuid}.log
```

示例：
```
./sandbox_logs/2026-07-19-3-a1b2c3d4-e5f6-47a8-b9c0-d1e2f3a4b5c6.log
```

日志格式：
```
[2026-07-19 14:30:25.123456] [INFO ] [eval:1] Sandbox created: my-sandbox (uuid=a1b2c3d4-...)
[2026-07-19 14:30:25.234567] [INFO ] [eval:1] Eval started: <sandbox>
[2026-07-19 14:30:25.245678] [INFO ] [eval:1] Eval completed in 11.11 ms
[2026-07-19 14:30:30.123456] [INFO ] [eval:2] Eval started: <sandbox>
[2026-07-19 14:30:30.234567] [ERROR] [eval:2] Eval failed after 111.11 ms: <sandbox>
```

日志 API：
```c
// 写入日志 (线程安全, 非阻塞)
lr_slog_info(sb->log, eval_id, "Custom message: %s", "value");
lr_slog_error(sb->log, eval_id, "Error: %d", error_code);
lr_slog_debug(sb->log, eval_id, "Debug info: %p", ptr);

// 强制刷新
lr_sandbox_log_flush(sb->log);

// 打印统计
lr_sandbox_log_stats(sb->log, stdout);
```

---

## 6. IOME586 结果缓存

### 6.1 概述

**IOME586** 是缓存技术的正式名称。它以整个 JS 脚本为粒度，直接缓存解释器的成果数据
（序列化 AST、全局变量绑定快照、每级节点结果、运行状态等），并在**边运行边缓存**：
解析完成后先落盘 WRITING 状态归档，脚本执行结束后归档为 ARCHIVED。

缓存覆盖**全量 ES2022**，包括普通脚本与 ES 模块（`-m`/`--module` 或
`JS_EVAL_TYPE_MODULE`）。脚本与模块统一走 `lr_exec_file_cached`，共用同一缓存路径；
归档会记下 `LR_IOME586_FLAG_MODULE` 标志，热路径据此以**模块方式**重跑（正确重建
`module_ns` 命名空间与 `import`/`export` 绑定）。默认导入
`import defFn from "mod"` 的 `is_default` 标记在 AST 序列化/反序列化中完整往返，因此
缓存命中的模块行为与冷跑完全一致。

**热路径 = 静态还原 + 动态重跑**（这是缓存能覆盖全量 ES2022 的关键）：
- **静态部分直接还原**：`lr_iome586_restore_globals` 还原全局变量绑定快照，原语全局变量立即就位；
- **动态部分重新跑**：对反序列化的 AST 重新执行。解释器重建函数/类绑定（保持可调用）、
  重跑 I/O 与副作用、重算原语，因此无论脚本多"动态"，结果始终正确。

**归档文件本质是一个 LZ4 压缩包**，输出文件为 `<namehash>.lrfile`（加载时也兼容
`.lrfile.lz4` 写法）。文件名取脚本**路径**哈希，因此 JS 脚本修改后会自动原位更新
同一归档（旧版本转为 `.bak` 可撤回）。payload 仅做 **LZ4 压缩，不做任何加密**；
完整性由 `CRC32` 与内容哈希 `SourceHash`（FNV-1a 64-bit）保证，不再使用任何自密钥
XOR（历史版本的 KEYED 归档仍可被兼容加载）。文件描述区以明文保存脚本名 + 时间 +
版本号的副本。

**容器格式**（魔数 `"IOME586\0"`）：
```
Magic          "IOME586\0"     8 bytes
ContainerVer   uint32          4 bytes
EngineVer      uint32          4 bytes   (版本串 FNV-1a32)
Status         uint32          4 bytes   (1=WRITING 写入中, 2=ARCHIVED 已归档)
Flags          uint32          4 bytes
CreatedAt      int64           8 bytes
SourceHash     FNV-1a 64-bit   8 bytes   (脚本内容哈希，用于校验，非密钥)
Mtime          int64           8 bytes
SrcSize        uint64          8 bytes
OptRatio       uint32          4 bytes   (优化比值 ×1e6)
CRC32          uint32          4 bytes
PayloadStored  uint32          4 bytes   (LZ4 压缩后)
PayloadRaw     uint32          4 bytes
DescLen+Desc   variable                  (明文描述: 脚本名/时间/版本)
Payload        variable                  (LZ4 压缩；无加密，CRC32/SourceHash 校验)
```

**安全模型（v0.1.0 加固）**：
- **敏感值排除**：快照跳过名称命中敏感词（token/secret/password/credential/apikey/
  auth/bearer/cookie/session/private 等）的全局绑定，避免令牌/密钥随缓存落盘泄露。
- **字符串可控**：`snapshot_strings` 默认开启（记录字符串字面量绑定）；`--iome586-no-strings`
  可关闭，使归档中完全不出现任何字符串字面量值。
- **还原默认关闭**：`restore_globals` 默认关闭，暖跑只做"静态还原 + 动态重跑"，不把
  归档里的全局变量绑定写回全局对象；需要时通过 `--iome586-restore-globals` 显式开启。
- **BOM 基线重映射保护**：运行时在注册完内建 API 后捕获一份"已存在全局属性"基线，
  还原时跳过这些属于引擎/BOM 的名称，避免缓存恢复污染内建 API。


**Payload 内为命名条目**（`u16 name_len|name|u32 data_len|data`），相当于压缩包内的
多个二进制文件：`meta`（元信息）、`path`（解释路径/方法）、`config`（配置）、
`init`（初始化内容与结果）、`ast`（序列化 AST）、`nodes`（每级节点结果）、
`globals`（全局变量绑定快照）、`state`（状态机/运行状态）、`bytecode`（编译后的字节码，魔数 `LRBC`）。

#### 6.1.0 缓存了哪些内容？能否直接恢复？（v0.1.1）

| 缓存项 | 存放位置 | 是否持久化 | 暖跑能否直接恢复 |
|--------|----------|-----------|------------------|
| JS 脚本名称 | header/`meta` | ✅ | ✅ 直接读取 |
| JS 脚本哈希（`source_hash`） | header | ✅ | ✅ 直接读取（用于命中校验） |
| 状态（写入中 / 已归档） | header `status` | ✅ | ✅ 直接读取；`writing` 视为脏归档并丢弃 |
| 时间（`created_at` / 源文件 `mtime`） | header | ✅ | ✅ 直接读取 |
| 优化比值（`opt_ratio_x1e6`） | header | ✅ | ✅ 直接读取（15% 规则判定） |
| 版本号（容器版本 + 引擎版本 FNV-1a32） | header | ✅ | ✅ 直接读取；不匹配即整包作废 |
| 校验码（`payload_crc32`） | header | ✅ | ✅ 直接读取校验 |
| JS 脚本的解释路径 | `path` | ✅ | ✅ 直接恢复 |
| 配置 | `config` | ✅ | ✅ 直接恢复 |
| 初始化内容与结果 | `init` | ✅ | ✅ 直接恢复 |
| 每一级节点的结果 | `nodes` | ✅ | ⚠️ 恢复为**记录**（用于比对/统计），不直接跳过求值 |
| AST | `ast`（`LRA` v3） | ✅ | ✅ 直接恢复，跳过词法/语法分析 |
| 字节码 | `bytecode`（`LRBC` v2） | ✅ | ⚠️ 仅当程序**不含 AST 节点引用**时可直接恢复；否则反序列化返回 `NULL`，由 AST 重新编译（毫秒级） |
| 状态机状态 | `state` | ✅ | ✅ 直接恢复 |
| 运行状态 | `state` | ✅ | ⚠️ 恢复为元信息；执行仍从头开始，不做“断点续跑” |
| 全局变量绑定对象 | `globals` | ✅（`snapshot_strings` 默认开） | ❌ 默认**不恢复**（`restore_globals=0`）；需显式 `--iome586-restore-globals` 才注入 |

结论：**结构性内容（AST、字节码、路径、配置、初始化、元信息、状态机状态）可直接恢复**；
**运行期语义状态（全局变量绑定、执行进度）默认不恢复**，而是重新执行以保证语义正确
（避免观测到过期/被污染的全局值）。因此暖跑省掉的是「读文件 + 词法 + 语法 + 编译」，
而非「执行」。

#### 6.1.1 AST 序列化格式（魔数 `LRA`）

归档的 `ast` 条目保存的是**序列化后的 AST**，而非字节码。其二进制布局以 4 字节魔数
`"LRA"` 开头，紧随 1 字节**格式版本号**（当前为 `3`）。

反序列化时校验魔数与版本号，若不匹配（魔数非法或版本不兼容）直接返回 `NULL`，
归档被拒绝加载并删除，下次运行自动重新冷跑并重建缓存（见 §6.4）。

**字面量序列化（`AST_LITERAL`）采用显式类型标记 `ltag`**，取值 5 类：

| `ltag` | 类型 | 反序列化处理 |
|--------|------|--------------|
| 0 | bool（`true`/`false`） | 读 `u32` 布尔值，仅写入 `u.bool_val.val` |
| 1 | string | 读字符串载荷 |
| 2 | number（`double`） | 读 `f64` 数值 |
| 3 | `null` | 置 `u.number.num = 0.0`，标记 `TOK_NULL_LIT` |
| 4 | `undefined` | 置 `u.number.num = -1.0`，标记 `TOK_UNDEFINED_LIT` |

> **已知修复（v0.7.0 / `LRA` v3）**：早期版本在反序列化 bool 字面量时，写入
> `u.bool_val.val` 后又写入 `u.number.num`（`double`）。由于 `bool_val` 与 `number`
> 在 `ASTNode` 的 union 中内存重叠，`double 1.0` 的低 4 字节为零，会把 `true`
> 覆盖成 `false`。后果是暖跑中 `let ok = true` 变成 `false`，导致 `if(!ok) throw`
> 误触发、丢弃后续语句（如缓存命中后末句 `MODULE TEST OK` 丢失）。v3 改为反序列化
> bool 时**只写 `bool_val.val`**，与解析期的常量节点（仅设 `bool_val.val`）保持一致，
> 该 bug 已彻底修复并验证（冷/暖跑输出一致）。

### 6.2 API

```c
// 配置缓存目录
LR_Config cfg;
lr_config_default(&cfg);
cfg.bytecode_cache_dir = "./my_cache";  // 绝对或相对路径

// 或运行时设置
lr_iome586_set_dir(&rt->iome586, "./my_cache");

// 查看统计 / 清空
lr_bytecode_cache_stats(rt, stdout);   // 封装 lr_iome586_stats
lr_bytecode_cache_clear(rt);           // 封装 lr_iome586_clear
```

### 6.3 缓存策略

- **15% 规则**：解析耗时收益 (parse_us / total_us) 低于 15% 时不缓存，commit 时自动丢弃归档
- **边运行边缓存**：`lr_iome586_begin`（WRITING）→ 执行 → `lr_iome586_commit`（ARCHIVED）；执行抛异常时 `lr_iome586_abort` 回滚
- **BOM 支持**：加载脚本时自动剥离 UTF-8 BOM，UTF-16 LE/BE 自动转码为 UTF-8

### 6.4 缓存自动更新、失效与撤回

- **脚本修改自动更新**：归档按脚本路径命名，源文件修改后（内容 hash / mtime 变化），旧归档转为 `.bak`，下一次运行自动重新缓存到同一文件
- 引擎版本变化后（EngineVer 不匹配），同样自动更新
- **AST 格式版本不匹配**：`ast` 条目的 `LRA` 魔数或格式版本号不兼容时，归档被拒绝加载并删除，下次运行自动重建缓存（见 §6.1.1）
- CRC32 校验失败或状态为 WRITING（残留半成品）时拒绝加载并删除
- 手动失效：`lr_iome586_invalidate(&rt->iome586, "script.js")`
- **落盘撤回**：begin 时保留 `.bak` 备份，`lr_iome586_revert` 可回滚到上一版归档（CLI：`--iome586-revert <js>`）

---

## 7. 内置浏览器 API

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
const now = performance.now();   // 高精度时间戳
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

> **注意：** L/R_JS **不内置任何网络功能**。引擎本身不发任何网络包；`fetch()` 通过 `LR_HttpWrapper` 接口将请求委托给宿主应用程序（浏览器、WebUI 等）。
> 宿主必须调用 `lr_http_set_wrapper()` 注册包装器，否则 `fetch()` 返回 rejected Promise。
> 网络相关能力均通过宿主委派实现：`fetch`（`LR_HttpWrapper`）与 `WebSocket`（`LR_WsWrapper`，见 §7.9）。

```js
const resp = await fetch("https://api.example.com/data");
const json = await resp.json();
console.log(json);
```

### 7.9 WebSocket

> **注意：** 与 `fetch` 一样，L/R_JS **不内置 WebSocket 协议**。连接通过 `LR_WsWrapper` 接口委托给宿主；引擎本身不收发任何 WebSocket 帧。宿主在 `connect`/`send`/`close` 回调中处理真实 I/O，并通过引擎侧 `lr_ws_on_*` 函数把事件回灌给 JS（**必须在引擎线程调用**，例如宿主接入引擎事件循环的 I/O 泵中）。

#### C API

```c
// 设置 WebSocket 包装器（宿主拥有所有权，传 NULL 清除）
void lr_ws_set_wrapper(LR_Runtime *rt, LR_WsWrapper *wrapper);

// 获取当前 WebSocket 包装器
LR_WsWrapper *lr_ws_get_wrapper(LR_Runtime *rt);
```

```c
// WebSocket 包装器接口
typedef struct LR_WsWrapper {
    void *user_data;  // 透传给回调的 opaque 数据

    // 发起连接。成功时把宿主连接句柄写入 *out_handle 并返回 0；
    // 实际的 "open" 事件稍后通过 lr_ws_on_open() 上报。失败返回 -1。
    int (*connect)(void *user_data, const char *url, const char *protocols,
                   void **out_handle);

    // 发送文本帧。成功返回 0，失败返回 -1。
    int (*send)(void *user_data, void *conn_handle,
                const void *data, size_t len);

    // 关闭连接。code/reason 可为 0/NULL。成功返回 0。
    int (*close)(void *user_data, void *conn_handle, int code, const char *reason);
} LR_WsWrapper;
```

宿主在收到数据时调用引擎侧回调，把事件推送给 JS：

```c
void lr_ws_on_open(LR_Runtime *rt, void *conn_handle);
void lr_ws_on_message(LR_Runtime *rt, void *conn_handle, const void *data, size_t len);
void lr_ws_on_close(LR_Runtime *rt, void *conn_handle, int code, const char *reason);
void lr_ws_on_error(LR_Runtime *rt, void *conn_handle, const char *message);
```

#### 使用示例（宿主侧）

```c
static int my_ws_connect(void *ud, const char *url, const char *protocols, void **out_handle) {
    my_conn *c = my_ws_lib_connect(url, protocols);
    if (!c) return -1;
    *out_handle = c;            // 之后用同一句柄回灌事件
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

// 连接打开/收到消息/关闭/出错时，在引擎线程调用：
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

`WebSocket` 提供 `readyState`（0 `CONNECTING` / 1 `OPEN` / 2 `CLOSING` / 3 `CLOSED`）、`url`、`protocol`、`send()`、`close()`，以及 `onopen` / `onmessage` / `onclose` / `onerror` 事件属性。

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

## 8. 高级特性

### 8.1 分代 GC

```bash
# 启用分代 GC
./lr_js --gc-generational script.js

# 自定义 nursery 大小
./lr_js --gc-generational --gc-nursery-size 8 script.js
```

```c
cfg.gc_mode = LR_GC_MODE_GENERATIONAL;
cfg.gc_generational = 1;
cfg.gc_nursery_size = 8 * 1024 * 1024;  // 8MB nursery
```

### 8.2 增量 GC

```bash
# 启用增量 GC (默认)
./lr_js --gc-incremental script.js

# 自定义暂停目标
./lr_js --gc-incremental --gc-pause-target 3 script.js
```

```c
cfg.gc_mode = LR_GC_MODE_INCREMENTAL;
cfg.gc_incremental = 1;
cfg.gc_pause_target_ns = 3000000;  // 3ms 目标暂停
```

### 8.3 内存限制

```bash
# 要求至少 2GB 系统可用内存
./lr_js --min-memory 2147483648 script.js

# 跳过内存检查
./lr_js --no-memory-check script.js
```

### 8.4 渲染器桥接

```c
// 选择渲染后端
LR_RendererConfig rcfg = {
    .type = LR_RENDERER_SKIA,
    .socket_path = "/tmp/lr_render.sock",
    .width = 800,
    .height = 600,
};
lr_renderer_init(&rt->renderer, &rcfg);
```

### 8.5 渲染管道（外部渲染器输出）

渲染管道（`LR_RenderPipeline`）允许 Canvas 2D 和 WebGL 的渲染输出转发到外部渲染器。每个管道可以挂载多个输出接收器（Sink），支持 Socket、共享内存、回调和文件输出。

#### C API

```c
// 创建管道
LR_RenderPipeline *pipe = lr_render_pipeline_create(800, 600);

// 添加输出接收器
LR_PipeSink sock = lr_render_pipe_sink_socket("/tmp/renderer.sock");
lr_render_pipeline_add_sink(pipe, &sock);

LR_PipeSink cb = lr_render_pipe_sink_callback(my_callback, my_data);
lr_render_pipeline_add_sink(pipe, &cb);

// 提交帧（Canvas 2D 帧缓冲）
lr_render_pipeline_submit(pipe, pixels, width, height);

// 提交 GL 帧（从 GLES 帧缓冲读回）
lr_render_pipeline_submit_gl(pipe, gl_ctx, width, height);

// 集成到渲染器桥接
lr_renderer_set_pipeline(rb, pipe);
lr_renderer_pipeline_flush(rb);

// 清理
lr_render_pipeline_destroy(pipe);
```

#### JS API

```javascript
// 创建 Canvas
const canvas = new Canvas(800, 600);

// 方法 1: 通过 Canvas 设置管道（Socket）
canvas.setPipeline('/tmp/renderer.sock');

// 2D 上下文
const ctx = canvas.getContext('2d');
ctx.fillStyle = 'red';
ctx.fillRect(0, 0, 100, 100);
ctx.pipelineFlush();  // 提交帧到管道

// 方法 2: 通过 WebGL 上下文设置管道
const gl = canvas.getContext('webgl');
gl.setPipeline('/tmp/renderer.sock');
gl.clear(gl.COLOR_BUFFER_BIT);
gl.pipelineFlush();  // 读回 GL 帧缓冲并提交

// 方法 3: 使用 Canvas 的 GL 专用方法
canvas.pipelineFlushGL();
```

#### Socket 协议

每帧通过 Socket 发送的格式：

```
FRAME <width> <height> <data_size>\n
<raw RGBA pixel data (data_size bytes)>
```

#### 管道类型

| 类型 | 创建函数 | 说明 |
|------|----------|------|
| Socket | `lr_render_pipe_sink_socket(path)` | Unix 域套接字 |
| Shared Memory | `lr_render_pipe_sink_shm(ptr, size)` | 共享内存 |
| Callback | `lr_render_pipe_sink_callback(fn, user)` | 用户回调 |
| File | `lr_render_pipe_sink_file(pattern)` | PPM 文件（调试） |

### 8.6 HTTP 包装器（外部 HTTP 委派）

> L/R_JS 本身不内置 HTTP 客户端。`fetch()` 通过 `LR_HttpWrapper` 接口将 HTTP 请求委托给宿主应用程序（浏览器、WebUI 等）。

#### C API

```c
// 设置 HTTP 包装器（宿主拥有所有权，传 NULL 清除）
void lr_http_set_wrapper(LR_Runtime *rt, LR_HttpWrapper *wrapper);

// 获取当前 HTTP 包装器
LR_HttpWrapper *lr_http_get_wrapper(LR_Runtime *rt);

// 释放 LR_HttpResult 的分配字段
void lr_http_result_free(LR_HttpResult *result);
```

#### 类型定义

```c
// HTTP 请求结果
typedef struct LR_HttpResult {
    int         status_code;    // HTTP 状态码（如 200），失败时为 0
    char       *status_text;    // 状态文本（如 "OK"），strdup'd，由调用者 free
    char       *headers;        // 原始响应头，strdup'd，由调用者 free
    char       *body;           // 响应体，malloc'd，由调用者 free
    size_t      body_len;       // 响应体长度（字节）
    char       *error;          // 错误消息（strdup'd），成功时为 NULL
} LR_HttpResult;

// HTTP 包装器接口
typedef struct LR_HttpWrapper {
    void *user_data;  // 透传给回调的 opaque 数据

    // 执行 HTTP 请求。填充 result 结构体。成功返回 0，失败返回 -1。
    int (*fetch)(void *user_data, const char *method, const char *url,
                 const char *headers, const void *body, size_t body_len,
                 LR_HttpResult *result);
} LR_HttpWrapper;
```

#### 使用示例

```c
// 宿主实现 fetch 回调
static int my_fetch(void *user_data, const char *method, const char *url,
                    const char *headers, const void *body, size_t body_len,
                    LR_HttpResult *result)
{
    // 使用宿主自己的 HTTP 库（如 libcurl、WinHTTP、浏览器 fetch API 等）
    // 填充 result->status_code, result->body, result->headers 等
    result->status_code = 200;
    result->status_text = strdup("OK");
    result->body = strdup("{\"message\":\"Hello from host\"}");
    result->body_len = strlen(result->body);
    result->headers = strdup("Content-Type: application/json\r\n");
    return 0;
}

// 注册包装器
LR_HttpWrapper wrapper = { .user_data = NULL, .fetch = my_fetch };
lr_http_set_wrapper(rt, &wrapper);

// 之后 JS 中即可正常调用 fetch()
// fetch("https://api.example.com/data").then(r => r.json()).then(console.log);
```

#### JS API

```js
// 当宿主注册了 LR_HttpWrapper 后，可正常使用 fetch()
const resp = await fetch("https://api.example.com/data");
const json = await resp.json();
console.log(json);

// 未注册包装器时，fetch() 返回 rejected Promise
try {
    await fetch("https://example.com");
} catch (e) {
    console.log(e.message);  // "fetch() is not available: no HTTP wrapper configured"
}
```

### 8.7 文件系统 API（fs）

> L/R_JS 提供基本的文件操作 API。对于需要系统权限的操作（如写入系统目录），通过 `LR_FileWrapper` 委托给宿主程序，
> 宿主负责申请 OS 级权限（Windows UAC、Linux polkit 等）。每批次特权操作需要重新申请权限。

#### JS API

```js
// 读取文件（返回字符串）
const data = fs.readFile("/path/to/file.txt");
console.log(data);

// 写入文件（覆盖）
fs.writeFile("/path/to/file.txt", "Hello, World!");

// 追加写入
fs.appendFile("/path/to/file.txt", "\nAppended line");

// 读取目录（返回文件名数组）
const entries = fs.readdir("/path/to/dir");
console.log(entries[0], entries[1], /* ... */);

// 创建目录
fs.mkdir("/path/to/newdir");

// 删除空目录
fs.rmdir("/path/to/emptydir");

// 删除文件
fs.unlink("/path/to/file.txt");

// 重命名/移动
fs.rename("/path/to/old.txt", "/path/to/new.txt");

// 获取文件信息
const stat = fs.stat("/path/to/file.txt");
console.log(stat.size, stat.isFile, stat.isDirectory, stat.mtime);

// 检查文件是否存在
if (fs.exists("/path/to/file.txt")) {
    console.log("File exists!");
}
```

#### 权限模型

| 操作 | 普通文件 | 需要权限的文件 |
|------|---------|--------------|
| `readFile` | 直接读取 | 失败 → 调用 wrapper |
| `writeFile` | 直接写入 | 失败 → 调用 wrapper |
| `appendFile` | 直接追加 | 失败 → 调用 wrapper |
| `readdir` | 直接列出 | 失败 → 调用 wrapper |
| `mkdir` | 直接创建 | 失败 → 调用 wrapper |
| `rmdir` | 直接删除 | 失败 → 调用 wrapper |
| `unlink` | 直接删除 | 失败 → 调用 wrapper |
| `rename` | 直接重命名 | 失败 → 调用 wrapper |
| `stat` | 直接查询 | 失败 → 调用 wrapper |
| `exists` | 直接查询 | 不支持特权 |

#### C API（宿主集成）

```c
// 设置文件包装器（宿主拥有所有权，传 NULL 清除）
void lr_file_set_wrapper(LR_Runtime *rt, LR_FileWrapper *wrapper);

// 获取当前文件包装器
LR_FileWrapper *lr_file_get_wrapper(LR_Runtime *rt);

// 释放 LR_FileResult 的分配字段
void lr_file_result_free(LR_FileResult *result);
```

#### 类型定义

```c
// 文件操作结果
typedef struct LR_FileResult {
    int         error_code;    // 0 成功，errno 失败
    char       *error;         // 错误消息（strdup'd），成功时 NULL
    char       *data;          // 读取操作的数据，malloc'd
    size_t      data_len;      // 数据长度
    int         is_dir;        // 是否为目录
    int         is_file;       // 是否为普通文件
    size_t      file_size;     // 文件大小
    char      **entries;       // 目录条目（NULL 结尾）
    int         entry_count;   // 条目数量
} LR_FileResult;

// 文件系统特权包装器
typedef struct LR_FileWrapper {
    void *user_data;  // 透传给回调的 opaque 数据

    // 执行特权文件操作。每次调用都需要重新申请权限，不得缓存。
    // operation: "read_file", "write_file", "delete_file", "rename",
    //            "read_dir", "create_dir", "remove_dir", "stat"
    // 成功返回 0，失败返回 -1（填充 error_code/error）。
    int (*execute)(void *user_data, const char *path, const char *operation,
                   const void *data, size_t data_len, const char *extra,
                   LR_FileResult *result);
} LR_FileWrapper;
```

#### 宿主集成示例

```c
static int my_file_execute(void *user_data, const char *path,
                           const char *operation, const void *data,
                           size_t data_len, const char *extra,
                           LR_FileResult *result)
{
    // 1. 请求 OS 权限（UAC/polkit/…）
    if (!request_admin_privilege(operation, path))
        return -1;

    // 2. 执行操作
    if (strcmp(operation, "read_file") == 0) {
        // 读取文件，填充 result->data/data_len
    } else if (strcmp(operation, "write_file") == 0) {
        // 写入文件
    }
    // ...

    return 0;
}

// 注册包装器
LR_FileWrapper wrapper = { .user_data = NULL, .execute = my_file_execute };
lr_file_set_wrapper(rt, &wrapper);
```

### 8.8 终端 API（term）

> L/R_JS 提供终端命令执行 API。对于需要系统权限的命令（如访问系统资源），通过 `LR_TerminalWrapper` 委托给宿主程序，
> 宿主负责申请 OS 级权限（Windows UAC、Linux polkit 等）。每批次特权操作需要重新申请权限。

#### JS API

```js
// 运行单条命令，返回 { exitCode, stdout, stderr }
const result = term.run("echo Hello, World!");
console.log(result.exitCode);  // 0
console.log(result.stdout);    // "Hello, World!\n"
console.log(result.stderr);    // ""

// 运行命令并捕获错误输出
const r = term.run("ls /nonexistent");
console.log(r.exitCode);       // 2
console.log(r.stderr);         // "ls: cannot access '/nonexistent': No such file or directory\n"

// 批量运行多条命令（每批次重新授权）
const results = term.runBatch([
    "echo first",
    "echo second",
    "echo third"
]);
console.log(results.length);   // 3
console.log(results[0].stdout); // "first\n"

// 实时输出模式（逐行回调，支持 stdout/stderr 分离）
// 回调参数可以是函数（简写为 onStdout）或 { onStdout, onStderr, onExit }
term.spawn("ping 127.0.0.1", function(line) {
    console.log("[stdout]", line.trim());
});

term.spawn("some-command", {
    onStdout: function(line) { console.log("OUT:", line); },
    onStderr: function(line) { console.error("ERR:", line); },
    onExit: function(code)  { console.log("EXIT:", code); }
});
```

#### 权限模型

| 操作 | 普通命令 | 需要权限的命令 |
|------|---------|--------------|
| `run` | 直接执行 (`popen`) | 失败 → 调用 wrapper |
| `runBatch` | 逐个直接执行 | 失败 → 调用 wrapper（每批次重新授权） |
| `spawn` | 直接执行，逐行回调 | 失败 → 调用 wrapper（整批输出） |

#### C API（宿主集成）

```c
// 设置终端包装器（宿主拥有所有权，传 NULL 清除）
void lr_terminal_set_wrapper(LR_Runtime *rt, LR_TerminalWrapper *wrapper);

// 获取当前终端包装器
LR_TerminalWrapper *lr_terminal_get_wrapper(LR_Runtime *rt);

// 释放 LR_TerminalResult 的分配字段
void lr_terminal_result_free(LR_TerminalResult *result);
```

#### 类型定义

```c
// 命令执行结果
typedef struct LR_TerminalResult {
    int         error_code;    // 0 成功，errno 失败
    char       *error;         // 错误消息（strdup'd），成功时 NULL
    char       *stdout_data;   // 标准输出，malloc'd
    size_t      stdout_len;    // stdout 长度
    char       *stderr_data;   // 标准错误输出，malloc'd
    size_t      stderr_len;    // stderr 长度
    int         exit_code;     // 进程退出码（0 = 成功）
} LR_TerminalResult;

// 终端特权包装器
typedef struct LR_TerminalWrapper {
    void *user_data;  // 透传给回调的 opaque 数据

    // 执行特权命令。每次调用都需要重新申请权限，不得缓存。
    // operation: "run"
    // 成功返回 0，失败返回 -1（填充 error_code/error）。
    int (*execute)(void *user_data, const char *command, const char *operation,
                   const void *stdin_data, size_t stdin_len,
                   LR_TerminalResult *result);
} LR_TerminalWrapper;
```

#### 宿主集成示例

```c
static int my_term_execute(void *user_data, const char *command,
                            const char *operation, const void *stdin_data,
                            size_t stdin_len, LR_TerminalResult *result)
{
    // 1. 请求 OS 权限（UAC/polkit/…）
    if (!request_admin_privilege("execute", command))
        return -1;

    // 2. 使用 system() 或 CreateProcess() 执行命令
    //    填充 result->stdout_data/stdout_len/stderr_data/stderr_len/exit_code
    FILE *fp = popen(command, "r");
    if (!fp) return -1;

    // 读取输出...
    result->exit_code = 0;
    pclose(fp);
    return 0;
}

// 注册包装器
LR_TerminalWrapper wrapper = {
    .user_data = NULL,
    .execute = my_term_execute
};
lr_terminal_set_wrapper(rt, &wrapper);
```

### 8.9 系统信息 API（system）

> L/R_JS 提供只读的系统信息 API，可在 JS 中获取操作系统名称、版本号、内核版本、CPU、GPU、RAM 等信息。
> 所有数据通过标准 Linux `/proc` 和 `/sys` 文件系统读取，无需特权操作。

#### JS API

```js
// 获取操作系统名称
const name = system.name();
console.log(name);  // "Ubuntu", "Debian", "Fedora" 等

// 获取操作系统版本
const version = system.version();
console.log(version);  // "22.04 LTS", "11" 等

// 获取内核版本
const kernel = system.kernel();
console.log(kernel);  // "6.2.0-35-generic"

// 获取 CPU 架构
const arch = system.arch();
console.log(arch);  // "x86_64", "aarch64" 等

// 获取 CPU 名称
const cpu = system.cpu();
console.log(cpu);  // "Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz"

// 获取 CPU 核心数
const cpuCount = system.cpuCount();
console.log(cpuCount);  // 8

// 获取 GPU 信息
const gpu = system.gpu();
console.log(gpu);  // "PCI 0x10de:0x1f95" 或 "Unknown"

// 获取 RAM 信息（返回 { total, used, free } 字节）
const ram = system.ram();
console.log(ram.total);  // 17179869184 (16 GB)
console.log(ram.used);   // 8589934592 (8 GB)
console.log(ram.free);   // 8589934592 (8 GB)

// 获取系统运行时间（秒）
const uptime = system.uptime();
console.log(uptime);  // 123456.78 (秒)

// 获取主机名
const hostname = system.hostname();
console.log(hostname);  // "my-server"

// 一次性获取所有系统信息
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

#### 返回值说明

| 函数 | 返回值类型 | 说明 |
|------|-----------|------|
| `system.name()` | `string` | 操作系统名称（如 "Ubuntu"） |
| `system.version()` | `string` | 操作系统版本号 |
| `system.kernel()` | `string` | Linux 内核版本 |
| `system.arch()` | `string` | CPU 架构（如 "x86_64"） |
| `system.cpu()` | `string` | CPU 型号名称 |
| `system.cpuCount()` | `number` | 在线 CPU 核心数 |
| `system.gpu()` | `string` | GPU 设备信息（PCI vendor:device） |
| `system.ram()` | `{total, used, free}` | RAM 信息（字节为单位） |
| `system.uptime()` | `number` | 系统运行时间（秒） |
| `system.hostname()` | `string` | 主机名 |
| `system.info()` | `object` | 一次性返回所有系统信息 |

#### 数据来源

| 信息 | 来源 |
|------|------|
| OS 名称/版本 | `/etc/os-release` |
| 内核版本 | `uname()` |
| CPU 信息 | `/proc/cpuinfo` |
| GPU 信息 | `/sys/class/drm/card*/device/` |
| RAM 信息 | `/proc/meminfo` |
| 运行时间 | `/proc/uptime` |
| 主机名 | `gethostname()` |

---

### 8.10 线程池

```c
// 创建线程池
LR_ThreadPoolConfig tcfg = {
    .thread_count = 4,
    .queue_size = 128,
};
lr_thread_pool_init(&rt->thread_pool, &tcfg);

// 提交任务
lr_thread_pool_submit(&rt->thread_pool, my_task, my_data);
```

---

## 9. CLI 参考

### 9.1 完整参数列表

| 参数 | 说明 |
|------|------|
| `-e <code>` | 执行字符串代码 |
| `-m <file>` | 作为 ES Module 执行 |
| `-i, --interactive` | 启动 REPL 交互模式 |
| `-h, --help` | 显示帮助 |
| `-v, --version` | 显示版本 |
| `--strict` | 启用严格模式 |
| `--debug` | 启用调试模式 |
| `--memory-limit <bytes>` | 堆内存限制 |
| `--gc-threshold <bytes>` | GC 阈值 |
| `--gc-stress` | GC 压力测试模式 |
| `--gc-generational` | 启用分代 GC |
| `--gc-incremental` | 启用增量 GC（默认） |
| `--gc-manual` | 禁用自动 GC |
| `--gc-nursery-size <mb>` | 设置 nursery 大小 |
| `--gc-pause-target <ms>` | 设置目标最大暂停 |
| `--gc-stats` | 退出时打印 GC 统计 |
| `--iome586 <dir>` | 启用 IOME586 结果缓存（别名 `--bytecode-cache`） |
| `--iome586-stats` | 退出时打印缓存统计（别名 `--bytecode-stats`） |
| `--iome586-revert <js>` | 撤回指定脚本的缓存落盘（回滚到 .bak） |
| `--iome586-no-strings` | 缓存快照中不记录任何字符串字面量值（敏感值防护） |
| `--iome586-restore-globals` | 允许暖跑时把归档中的全局变量绑定还原回全局对象（默认关闭，按需开启） |
| `--sandbox-log <dir>` | 启用沙箱日志 |
| `--min-memory <bytes>` | 最小系统内存要求 |
| `--no-memory-check` | 跳过系统内存检查 |
| `--dump-bytecode` | 导出编译字节码 |
| `--strip-debug` | 剥离调试信息 |
| `--timeout <ms>` | 执行超时 |
| `--log-level <0-4>` | 日志级别 |
| `--stack-size <bytes>` | 栈大小 |

### 9.2 REPL 命令

| 命令 | 说明 |
|------|------|
| `.exit`, `.quit` | 退出 REPL |
| `.help` | 显示帮助 |
| `.clear` | 清屏 |
| `.gc` | 触发 GC |
| `.gc_stats` | 显示 GC 统计 |
| `.bc_stats` | 显示 IOME586 缓存统计 |
| `.memory` | 显示内存使用 |

---

## 10. 构建指南

### 10.1 Linux

```bash
# 依赖
sudo apt install build-essential  # Debian/Ubuntu
sudo dnf install gcc make         # Fedora

# 构建
cd LR_JS
make clean && make -j$(nproc)

# 输出: build/lr_js, build/liblr_js.a
```

### 10.2 macOS

```bash
# 依赖
xcode-select --install

# 构建
cd LR_JS
make clean && make -j$(sysctl -n hw.logicalcpu)

# 输出: build/lr_js, build/liblr_js.a
```

### 10.2.1 交叉编译（Linux → macOS，osxcross）

若没有 macOS 本机环境，可从 Linux 使用 [osxcross](https://github.com/tpoechtrager/osxcross) 交叉编译 macOS `x86_64` / `arm64` 二进制：

```bash
# 1. 编译安装 osxcross，并准备至少一个 macOS SDK（如 MacOSX12.sdk）。
#    若 osxcross clang 版本低于 LLVM 15，避免使用 15.x 的 SDK
#   （其 math.h 使用了 '_Float16' 类型，旧版 clang 无法编译）。
#
# 2. 运行脚本，自动检测工具链与 SDK 并构建两种架构：
./build_macos.sh

# 也可显式指定 SDK：
LR_OSX_SDK=/path/to/MacOSX12.3.sdk ./build_macos.sh
```

产物为 `releases/LR_JS-0.1.1-macos-{x86_64,arm64}.tar.gz`，内含 `lib/liblr_js.a`、`lib/liblr_js.dylib`、`bin/lr_js` 与 `lr_js.h`。

实现要点：脚本绕过 `o64-clang`/`oa64-clang` 启动器，直接调用真实的、带目标架构的 clang，并从其文件名提取精确 `-target` triple（如 `x86_64-apple-darwin21.4`），以匹配 `x86_64-apple-darwin21.4-ld` 链接器；SDK 自动检测优先匹配 clang 内嵌的 darwin 版本，否则回退到最旧的可用 SDK。

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

### 10.5 移动端 (Android NDK)

```bash
export ANDROID_NDK=/path/to/ndk
export TOOLCHAIN=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64

# arm64
export CC=$TOOLCHAIN/bin/aarch64-linux-android24-clang
export AR=$TOOLCHAIN/bin/llvm-ar

cd LR_JS
make CC=$CC AR=$AR clean && make CC=$CC AR=$AR
```

### 10.6 移动端 (iOS)

```bash
# arm64
export SDK=$(xcrun --sdk iphoneos --show-sdk-path)
export CC="xcrun --sdk iphoneos clang -arch arm64 -isysroot $SDK -miphoneos-version-min=14.0"

cd LR_JS
make CC="$CC" clean && make CC="$CC"
```

---

## 11. 性能基准

| 测试项目 | 耗时 | 说明 |
|----------|------|------|
| 空脚本启动 | < 1ms | 运行时创建 |
| console.log | < 1ms | 基本 I/O |
| 100K 对象循环 | ~50ms | 内存分配 |
| 50K 对象 (分代 GC) | ~45ms | GC 暂停 < 4ms |
| 50K 对象 (增量 GC) | ~48ms | GC 暂停 < 2ms |
| IOME586 缓存命中 | < 5ms | 跳过词法/解析 |

---

## 12. 错误码

| 错误码 | 说明 |
|--------|------|
| -1 | 通用错误 |
| -2 | 内存不足 |
| -3 | 超时 |
| -4 | 语法错误 |
| -5 | 运行时错误 |
| -6 | 系统内存不足 |
| -7 | 沙箱限制 |
| -8 | 网络错误 |

---

## 13. 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| 0.1.1 | 2026-07 | **全量字节码 VM**：栈式 VM（`lr_bytecode.c`）覆盖字面量、标识符、全部一元/二元/复合赋值/`**`/位运算/比较/`in`/`instanceof`/`typeof`/`delete`、`&&`/`\|\|`/`??` 短路、条件表达式、模板字符串（C 层拼接）、数组/对象字面量、成员与计算成员读写、函数/方法/构造调用、`for`/`while`/`do-while`/`for-of`（原生迭代协议）/`switch`/带标签 `break`/`continue`/`return`/`throw`/块级作用域。JS 运算与数据解析全部在 C 层完成（int32 快路径、字符串拼接、抽象/严格相等、关系比较）。闭包、类、生成器、async/await、try/catch、解构、模块、`for-in`、`super` 等语义经逃逸分析后以 `BC_EVAL_NODE` 回落到同一解释器状态，保证语义一致且无双重执行。字节码序列化 `LRBC` v2 并入 IOME586 归档（含 AST 节点引用时标记为不可直接恢复，暖跑由 AST 重新编译）。跨平台验证：MSVC/MinGW/GCC/Clang，Windows/macOS/Linux，x86/x64/ARM。文档新增 §6.1.0 缓存内容与可恢复性说明 |
| 0.1.0 | 2026-07 | 初始版本：ES2022+ 支持、多线程沙箱、渲染器桥接、系统内存限制、分代/增量 GC、`.lrfile` 字节码缓存与沙箱日志、IOME586 结果缓存（LZ4 归档、边运行边缓存、15% 规则、BOM、撤回）、AST 序列化 `LRA` v3（字面量显式类型标记），修复暖跑中 `true`→`false` 的 bug；追加能力：顶层 `var`/`function` 在非模块 Script 下绑定到全局对象（符合 `GlobalDeclarationInstantiation`，`let`/`const`/`class` 不挂载），模块内 `import.meta`，`RegExp` 的 `d`（match indices）标志，Windows 控制台 UTF-8 输出；IOME586 安全加固（敏感全局值排除、`snapshot_strings` 默认开启、去除自密钥 XOR、`restore_globals` 默认关闭、BOM 基线重映射保护），新增 CLI `--iome586-no-strings` / `--iome586-restore-globals` |

---

## 14. 许可证

MIT License

---

## 参考链接

- FNV-1a: https://en.wikipedia.org/wiki/Fowler-Noll-Vo_hash_function

---

## 15. Promise API

L/R_JS 实现完整的 Promise/A+ 规范，支持 ES2022 所有 Promise 静态方法。

### 15.1 构造函数

```javascript
const promise = new Promise((resolve, reject) => {
    resolve(42);
    // 或 reject(new Error("failed"));
});
```

### 15.2 静态方法

```javascript
// Promise.resolve(value)
Promise.resolve(42).then(v => console.log(v));

// Promise.reject(reason)
Promise.reject(new Error("fail")).catch(e => console.log(e.message));

// Promise.all(iterable) — 所有成功则返回结果数组，任一失败则立即 reject
Promise.all([
    Promise.resolve(1),
    Promise.resolve(2),
    Promise.resolve(3),
]).then(results => console.log(results));  // [1, 2, 3]

// Promise.race(iterable) — 返回第一个完成的 Promise（不论 resolve 或 reject）
Promise.race([
    new Promise(resolve => setTimeout(() => resolve("fast"), 10)),
    new Promise(resolve => setTimeout(() => resolve("slow"), 100)),
]).then(v => console.log(v));  // "fast"

// Promise.allSettled(iterable) — 等待所有 Promise 完成，返回每个的结果状态
Promise.allSettled([
    Promise.resolve(1),
    Promise.reject("error"),
]).then(results => {
    results.forEach(r => console.log(r.status, r.value || r.reason));
});

// Promise.any(iterable) — 返回第一个成功的 Promise，全部失败则 reject AggregateError
Promise.any([
    Promise.reject("err1"),
    Promise.resolve("ok"),
    Promise.reject("err3"),
]).then(v => console.log(v));  // "ok"
```

### 15.3 实例方法

```javascript
const promise = new Promise(resolve => resolve(42));

// promise.then(onFulfilled, onRejected)
promise.then(
    value => console.log("Fulfilled:", value),
    reason => console.log("Rejected:", reason)
);

// promise.catch(onRejected)
promise.catch(reason => console.error("Error:", reason));

// promise.finally(callback) — 无论成功或失败都会执行
promise
    .then(v => v * 2)
    .catch(e => 0)
    .finally(() => console.log("Cleanup"));
```

### 15.4 链式调用

> **注意：** 以下示例中的 `fetch()` 需要宿主注册 `LR_HttpWrapper` 才能正常工作。

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

L/R_JS 支持 Web Worker 多线程执行，每个 Worker 在独立线程中运行隔离的 JS 运行时，通过消息传递与主线程通信。

### 16.1 构造函数

```javascript
// 创建一个 Worker，执行指定脚本文件
const worker = new Worker("worker.js");

// 第二个参数支持配置选项
const worker = new Worker("worker.js", {
    /* 未来扩展 */
});
```

### 16.2 实例方法

```javascript
// worker.postMessage(data) — 向 Worker 发送消息
worker.postMessage("Hello from main");
worker.postMessage({cmd: "compute", data: [1, 2, 3]});
worker.postMessage(new Uint8Array([1, 2, 3]));  // 支持 Transferable

// worker.terminate() — 立即终止 Worker
worker.terminate();
```

### 16.3 事件处理

```javascript
// worker.onmessage — 接收 Worker 发来的消息
worker.onmessage = (event) => {
    console.log("Received from worker:", event.data);
};

// worker.onerror — 处理 Worker 中的错误
worker.onerror = (event) => {
    console.error("Worker error:", event.message);
};
```

### 16.4 Worker 端 API

```javascript
// worker.js — 在 Worker 线程中执行

// 监听主线程消息
self.onmessage = (event) => {
    const data = event.data;
    console.log("Worker received:", data);

    // 发送结果回主线程
    self.postMessage({result: data.cmd + " done"});
};

// 也可以使用 addEventListener
self.addEventListener("message", (event) => {
    self.postMessage("Processed: " + event.data);
});
```

### 16.5 完整示例

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

## 17. 无锁队列（C API）

L/R_JS 提供基于 CAS 的无锁队列（MPSC：Multiple Producer, Single Consumer），使用链表 + stub 节点设计避免 ABA 问题。

### 17.1 数据结构

```c
/* 队列节点（嵌入到用户数据结构中） */
typedef struct LR_LFQNode {
    struct LR_LFQNode *next;
} LR_LFQNode;

/* 无锁队列 */
typedef struct LR_LFQueue {
    LR_LFQNode *head;        /* Consumer 从 head 弹出 */
    LR_LFQNode *tail;        /* Producer 向 tail 推入 */
    volatile int32_t count;  /* 近似元素数量（仅诊断用途） */
} LR_LFQueue;

/* 静态初始化器 */
#define LR_LFQ_INIT { NULL, NULL, 0 }
```

### 17.2 API

```c
// 初始化队列（必须在使用前调用）
LR_LFQueue queue = LR_LFQ_INIT;
lr_lfq_init(&queue);

// 推入节点（多生产者安全）
MyData *node = malloc(sizeof(MyData));
node->value = 42;
lr_lfq_push(&queue, &node->lfq_node);

// 弹出节点（单消费者）
LR_LFQNode *node = lr_lfq_pop(&queue);
if (node) {
    MyData *data = (MyData *)((char *)node - offsetof(MyData, lfq_node));
    printf("Value: %d\n", data->value);
    free(data);
}

// 获取近似队列长度
int32_t count = lr_lfq_count(&queue);

// 检查是否为空
int empty = lr_lfq_is_empty(&queue);

// 排空队列
LR_LFQNode *all = lr_lfq_drain(&queue);

// 销毁队列
lr_lfq_destroy(&queue, NULL);           // 不释放数据
lr_lfq_destroy(&queue, free_data_cb);   // 回调释放每个节点
```

### 17.3 线程安全说明

| 操作 | 安全性 |
|------|--------|
| `lr_lfq_push` | 多生产者安全（CAS 循环，有限重试） |
| `lr_lfq_pop` | 仅单消费者安全（或外部序列化） |
| `lr_lfq_peek` | 仅单消费者安全 |
| `lr_lfq_count` | 近似值，仅诊断用途 |
| `lr_lfq_drain` | 仅单消费者安全 |

### 17.4 嵌入示例

```c
#include "lr_lockfree_queue.h"

typedef struct {
    LR_LFQNode lfq_node;  /* 必须为第一个成员或使用 offsetof */
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

## 18. 跨平台兼容层

L/R_JS 通过 `lr_platform.h` 提供统一的跨平台抽象层，支持 Linux、macOS、Windows 7+、FreeBSD、OpenBSD、NetBSD。

### 18.1 支持的平台

| 宏 | 平台 |
|-----|------|
| `LR_PLATFORM_WINDOWS` | Windows 7+ (MSVC/MinGW) |
| `LR_PLATFORM_LINUX` | Linux (GCC/Clang) |
| `LR_PLATFORM_MACOS` | macOS (Apple Clang) |
| `LR_PLATFORM_BSD` | FreeBSD / OpenBSD / NetBSD |

### 18.2 编译器检测

| 宏 | 编译器 |
|-----|---------|
| `LR_COMPILER_MSVC` | MSVC (cl.exe) |
| 默认 | GCC / Clang |

### 18.3 编译选项

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

### 18.4 原子操作 API

L/R_JS 提供统一的原子操作层，在 Windows 上使用 `Interlocked*` 系列函数，在 POSIX 系统上使用 GCC `__sync_*` 内置函数。

```c
// 32 位 CAS
int32_t lr_atomic_cas_32(volatile int32_t *ptr, int32_t oldval, int32_t newval);

// 指针 CAS
void *lr_atomic_cas_ptr(volatile void **ptr, void *oldval, void *newval);

// 原子交换
int32_t lr_atomic_xchg_32(volatile int32_t *ptr, int32_t val);
void *lr_atomic_xchg_ptr(volatile void **ptr, void *val);

// 原子 fetch-and-add
int32_t lr_atomic_fetch_add_32(volatile int32_t *ptr, int32_t val);

// 原子存储/加载
void  lr_atomic_store_32(volatile int32_t *ptr, int32_t val);
void  lr_atomic_store_ptr(volatile void **ptr, void *val);
int32_t lr_atomic_load_32(volatile int32_t *ptr);
void *lr_atomic_load_ptr(volatile void **ptr);

// 64 位原子操作
int64_t lr_atomic_cas_64(volatile int64_t *ptr, int64_t oldval, int64_t newval);
int64_t lr_atomic_load_64(volatile int64_t *ptr);

// 内存屏障
void lr_memory_barrier(void);
void lr_write_barrier(void);
void lr_read_barrier(void);
```

### 18.5 便捷宏

```c
// 原子自增/自减
#define LR_ATOMIC_INC(ptr)    lr_atomic_fetch_add_32((ptr), 1)
#define LR_ATOMIC_DEC(ptr)    lr_atomic_fetch_add_32((ptr), -1)

// 原子 test-and-set（返回 1 表示已设置，0 表示未设置）
#define LR_ATOMIC_TEST_AND_SET(ptr)  (lr_atomic_xchg_32((ptr), 1) != 0)

// 原子清除标志
#define LR_ATOMIC_CLEAR(ptr)  lr_atomic_store_32((ptr), 0)
```

### 18.6 跨平台 API 映射

| 功能 | POSIX | Windows |
|------|-------|---------|
| 文件操作 | `open/close/read/write/lseek` | `_open/_close/_read/_write/_lseek` |
| Socket 关闭 | `close(s)` | `closesocket(s)` |
| 动态库加载 | `dlopen/dlsym/dlclose` | `LoadLibrary/GetProcAddress/FreeLibrary` |
| 线程 | `pthread` | `lr_pthread_win.h` 兼容层 |
| 高精度时间 | `gettimeofday` | `QueryPerformanceCounter` |
| 随机数 | `/dev/urandom` | `CryptGenRandom` |
| 内存信息 | `sysconf` | `GlobalMemoryStatusEx` |
| Socket 初始化 | 无需操作 | `WSAStartup` |
| 目录分隔符 | `/` | `\\` |

### 18.7 平台特定头文件

```c
// 所有源文件包含 lr_platform.h 替代平台特定头文件
#include "lr_platform.h"   // 自动处理 pthread、socket、动态库等

// Windows 额外兼容层
#include "lr_pthread_win.h"  // MSVC 的 pthread 模拟
```

---

## 19. 任务调度器（C API）

L/R_JS 提供基于优先级的异步任务调度器，支持一次性、重复和定时任务。

### 19.1 API

```c
// 创建调度器
LR_Scheduler *sched = lr_scheduler_create(thread_pool);

// 调度重复任务
int task_id = lr_scheduler_schedule(sched, task,
    LR_TASK_PRIORITY_NORMAL, 1000000 /* 1秒间隔 */, -1 /* 无限重复 */);

// 调度延迟任务
lr_scheduler_schedule_delayed(sched, task,
    LR_TASK_PRIORITY_HIGH, 500000 /* 500ms 延迟 */);

// 调度立即执行
lr_scheduler_schedule_now(sched, task, LR_TASK_PRIORITY_CRITICAL);

// 取消任务
lr_scheduler_cancel(sched, task_id);

// 运行事件循环（阻塞）
lr_scheduler_run(sched);

// 处理待处理任务（非阻塞）
lr_scheduler_process(sched);

// 停止调度器
lr_scheduler_stop(sched);

// 销毁调度器
lr_scheduler_destroy(sched);
```

### 19.2 任务优先级

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

L/R_JS 实现完整的 ES2022 Map 和 Set 对象，使用 C 哈希表作为底层存储。

### 20.1 Map

```javascript
// 构造函数
const map = new Map();
const map2 = new Map([['key1', 'val1'], ['key2', 'val2']]);

// 实例方法
map.set(key, value);          // 设置键值对，返回 map 自身（支持链式）
map.get(key);                 // 获取值，不存在返回 undefined
map.has(key);                 // 检查键是否存在
map.delete(key);              // 删除键，返回 true/false
map.clear();                  // 清空所有条目
map.size;                     // 条目数（只读 getter）
map.forEach(callback, thisArg); // 遍历所有条目
```

### 20.2 Set

```javascript
// 构造函数
const set = new Set();
const set2 = new Set([1, 2, 3]);

// 实例方法
set.add(value);               // 添加值，返回 set 自身（支持链式）
set.has(value);               // 检查值是否存在
set.delete(value);            // 删除值，返回 true/false
set.clear();                  // 清空所有值
set.size;                     // 条目数（只读 getter）
set.forEach(callback, thisArg); // 遍历所有值
```

---

## 21. Proxy & Reflect API

L/R_JS 支持 ES2022 Proxy 和 Reflect，实现 7 个核心 trap。

### 21.1 Proxy

```javascript
// 构造函数
const proxy = new Proxy(target, handler);

// 支持的 trap
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
// 静态方法
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

## 22. 错误与堆栈跟踪

L/R_JS 支持完整的错误堆栈跟踪，格式兼容 V8。

### 22.1 Error.prototype.stack

```javascript
try {
    throw new Error("something went wrong");
} catch (e) {
    console.log(e.stack);
    // 输出:
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
console.log(err.stack);  // 带有堆栈信息，但不包含 captureStackTrace 调用
```

### 22.3 Error.stackTraceLimit

```javascript
Error.stackTraceLimit = 20;  // 控制堆栈帧数上限（默认 10）
```

---

## 23. 性能优化

L/R_JS 引擎包含多项性能优化：

### 23.1 AST 节点池
- 解析器预分配 4096 个 AST 节点，减少 `malloc` 调用
- 全局缓存常量节点（0、1、true、false、null、undefined）

### 23.2 字符串驻留
- 256 槽哈希表对标识符和字符串字面量进行原子化
- 避免重复的 `strdup` 和字符串比较

### 23.3 直接线程化
- 解释器使用函数指针表替代 `switch` 语句
- 减少分支预测失败，提高执行效率

### 23.4 内联缓存
- 64 槽属性访问缓存，缓存最近访问的属性偏移
- 减少属性查找的开销

### 23.5 整数快速路径
- 二元运算的 `int32 + int32` 快速路径（14 种运算）
- 避免对象装箱和函数调用开销

### 23.6 形状缓存
- 128 槽形状缓存，缓存 `get_property`/`set_property` 的 (对象, 属性) 对
- 加速属性访问

### 23.7 小字符串缓存
- 128 槽缓存长度 <= 32 的字符串
- 避免短字符串的重复分配

### 23.8 GC 调优
- 初始 GC 阈值: 1MB
- 新生代大小: 8MB
- GC 暂停目标: 10ms

---

## 24. ES2022 核心内置对象

L/R_JS 实现完整的 ES2022 核心内置对象，包括 Object、Array、String、Number、Boolean、Function、Math、JSON、Date、RegExp、Symbol、Error 子类、WeakMap、WeakSet。

### 24.1 Object

```javascript
// 静态方法
Object.keys(obj);           // 返回可枚举自有属性名数组
Object.values(obj);         // 返回可枚举自有属性值数组
Object.entries(obj);        // 返回 [key, value] 对数组
Object.fromEntries(entries); // 从 [key, value] 对创建对象
Object.assign(target, ...sources); // 复制属性
Object.create(proto);       // 创建指定原型的对象
Object.defineProperty(obj, prop, desc); // 定义属性
Object.defineProperties(obj, props);    // 定义多个属性
Object.freeze(obj);         // 冻结对象
Object.seal(obj);           // 密封对象
Object.isExtensible(obj);   // 检查是否可扩展
Object.preventExtensions(obj); // 阻止扩展
Object.is(v1, v2);          // SameValue 比较
Object.hasOwn(obj, prop);   // ES2022 自有属性检查
Object.getPrototypeOf(obj); // 获取原型
Object.setPrototypeOf(obj, proto); // 设置原型
Object.getOwnPropertyNames(obj);   // 获取自有属性名
Object.getOwnPropertyDescriptor(obj, prop); // 获取属性描述符
Object.getOwnPropertyDescriptors(obj);      // 获取所有属性描述符

// 原型方法
Object.prototype.toString();
Object.prototype.hasOwnProperty(prop);
Object.prototype.isPrototypeOf(obj);
Object.prototype.propertyIsEnumerable(prop);
Object.prototype.valueOf();
```

### 24.2 Array

```javascript
// 静态方法
Array.isArray(val);         // 检查是否为数组
Array.from(arrayLike);      // 从类数组/可迭代对象创建
Array.of(...items);         // 从参数创建

// 原型方法 (ES2022+)
Array.prototype.at(index);       // ES2022 索引访问
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
// 静态方法
String.fromCharCode(...codes);
String.fromCodePoint(...points);

// 原型方法
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
// 静态属性
Number.MAX_VALUE;       // 1.7976931348623157e+308
Number.MIN_VALUE;       // 5e-324
Number.NaN;
Number.NEGATIVE_INFINITY;
Number.POSITIVE_INFINITY;
Number.EPSILON;         // 2.220446049250313e-16
Number.MAX_SAFE_INTEGER; // 9007199254740991
Number.MIN_SAFE_INTEGER; // -9007199254740991

// 静态方法
Number.isNaN(value);
Number.isFinite(value);
Number.isInteger(value);
Number.isSafeInteger(value);
Number.parseFloat(string);
Number.parseInt(string, radix);

// 原型方法
Number.prototype.toFixed(digits);
Number.prototype.toPrecision(precision);
Number.prototype.toString(radix);
Number.prototype.valueOf();
```

### 24.5 Boolean

```javascript
// 原型方法
Boolean.prototype.toString();
Boolean.prototype.valueOf();
```

### 24.6 Function

```javascript
// 原型方法
Function.prototype.bind(thisArg, ...args);
Function.prototype.call(thisArg, ...args);
Function.prototype.apply(thisArg, argsArray);
Function.prototype.toString();
```

### 24.7 Math

```javascript
// 常量
Math.E;     Math.LN2;    Math.LN10;   Math.LOG2E;
Math.LOG10E; Math.PI;    Math.SQRT1_2; Math.SQRT2;

// 方法
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
JSON.stringify(value, replacer, space);  // 序列化
JSON.parse(text, reviver);               // 解析
```

### 24.9 Date

```javascript
// 静态方法
Date.now();
Date.parse(dateString);
Date.UTC(year, month, day, hour, min, sec, ms);

// 原型方法
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
// 构造函数
new RegExp(pattern, flags);

// 原型方法
RegExp.prototype.exec(str);
RegExp.prototype.test(str);
RegExp.prototype.toString();

// 属性
reg.lastIndex;     reg.source;    reg.flags;
reg.global;        reg.ignoreCase; reg.multiline;
reg.dotAll;        reg.sticky;    reg.unicode;
```

### 24.11 Symbol

```javascript
// 构造函数
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

### 24.12 Error 子类

```javascript
TypeError     // 类型错误
RangeError    // 范围错误
SyntaxError   // 语法错误
ReferenceError // 引用错误
EvalError     // 求值错误
URIError      // URI 错误
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

## 25. ES2022+ 语法特性

### 25.1 globalThis 与顶层声明绑定

```javascript
globalThis === window;  // true（浏览器语义下全局对象即 window）
```

**顶层 `var` / `function` 绑定到全局对象（仅非模块 Script 模式）。** 依据 ECMAScript
`GlobalDeclarationInstantiation` 规范：在非模块的 Script 中，顶层的 `var` 和 `function`
声明会成为全局对象的属性；而 `let` / `const` / `class` 属于声明式绑定，不会挂载到全局对象。
ES 模块（`.mjs` 或 `-m`）下所有顶层声明都保存在模块命名空间，绝不污染全局对象。

```javascript
// 以脚本方式运行（默认）
var topVar = 42;
function topFunc() { return "fn"; }
let topLet = 1;
const topConst = 2;
class TopClass {}

globalThis.topVar;            // 42
typeof globalThis.topFunc;   // "function"
"topLet"    in globalThis;   // false（声明式，不挂载）
"topConst"  in globalThis;   // false
"TopClass"  in globalThis;   // false

topVar = 200;
globalThis.topVar;           // 200（裸赋值经镜像同步到全局对象）

globalThis.topVar = 100;
topVar;                      // 100（双向绑定：外部写全局对象反射回裸变量）

// 以模块方式运行（-m / .mjs）
//   'mVar' in globalThis  -> false
//   'mFunc' in globalThis -> false
```

### 25.2 for...of 循环
```javascript
for (const v of [1, 2, 3]) {
    console.log(v);
}
```

### 25.3 逻辑赋值运算符
```javascript
x ||= y;  // x = x || y
x &&= y;  // x = x && y
x ??= y;  // x = x ?? y
```

### 25.4 数字分隔符
```javascript
const million = 1_000_000;  // 1000000
const bits = 0xFF_FF_FF;    // 16777215
const binary = 0b1010_0001; // 161
```

### 25.5 `import.meta`
模块（`.mjs` 或 `-m`）内部可访问 `import.meta`，提供当前模块元信息：
```javascript
// module.mjs
import.meta.url;       // "file:///abs/path/module.mjs"
import.meta.filename;  // 绝对路径
import.meta.dirname;   // 所在目录
console.log(import.meta.url);
```
非模块脚本中访问 `import.meta` 会报错。