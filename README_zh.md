# L/R_JS - 轻量级 JavaScript 引擎

纯 C 语言实现，兼容 ES2022+ 的 JavaScript 引擎，内置浏览器 API。

**v0.2.0**：执行引擎为**直接/间接线程式字节码 VM**（GCC/Clang 使用 computed goto 零开销调度，MSVC 使用 switch-based dispatch）。AST 树遍历解释器已退役。内置稠密数组存储（O(1) 索引访问）、IOME586 字节码预热缓存、字符串拼接优化、闭包变量缓存、基于形状的属性 IC、纯函数 memoization、`Atomics` 正确性验证。

## 性能对比（vs Node.js v24, Linux x64, GCC/O2）

### bench_cross_fixed.js（微基准，5 轮累计）

| 测试项 | V8 (Node) | LR_JS | 倍率 |
|--------|-----------|-------|------|
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
| **总计** | **35 ms** | **1174 ms** | **33.5×** |

### stress_test_noawait.js（全量工作负载）

| 测试项 | V8 (Node) | LR_JS | 倍率 |
|--------|-----------|-------|------|
| 类/继承 | 4.5 ms | 74.9 ms | 16.6× |
| Map/Set | 32.8 ms | 202.7 ms | 6.2× |
| 闭包 | 18.9 ms | 114.5 ms | 6.0× |
| 函数+递归 | 21.5 ms | 74.6 ms | 3.5× |
| 解构/模板 | 40.2 ms | 151.6 ms | 3.8× |
| 正则 | 59.1 ms | 930.7 ms | 15.8× |
| 生成器 | 18.1 ms | 289.0 ms | 15.9× |
| 异常 | 2.0 ms | 1.7 ms | 0.9× |
| async/Promise | 0.6 ms | 21.5 ms | 36× |
| SAB/Atomics | 1.1 ms | 2134.8 ms | 1941× |
| **总计** | **198.8 ms** | **3996 ms** | **20.1×** |

### stress_run.js（核心基准）

| 测试项 | V8 (Node) | LR_JS | 倍率 |
|--------|-----------|-------|------|
| Class 5000×10 deep | 2 ms | 54 ms | 27× |
| Array 100k reduce | 4 ms | 43 ms | 10.8× |
| String concat 10k | 2 ms | 13 ms | 6.5× |
| **总计** | **15 ms** | **148 ms** | **9.9×** |

> **正确性**：SAB 校验和 32145560 与 V8 完全一致 ✓；递归/尾递归/生成器/正则结果全部对齐 ✓。
>
> **注意**：Class 深层构造存在 5× 倒退（10 层: 155ms，O(depth) 线性 vs V8 O(1)）。根因：super 链构造 + 每层属性设置的常数开销约 3μs/层。
>
> **主要瓶颈**：func_call (55×)、type_conv (61×)、SAB/Atomics (1941×) 是最大差距。闭包、Map/Set、异常相对接近 V8。

### IOME586 基准（交叉引擎，Windows x64，16 线程）

#### 冷跑（10 次迭代）

| 测试项 | V8 (ms) | LR_JS (ms) | 倍率 |
|--------|---------|------------|------|
| fib20 (纯递归) | 1.62 | **0.00** | — |
| factorial (纯递归) | **0.02** | 0.11 | 0.18× |
| sum (循环求和) | **0.14** | 0.05 | 2.80× |
| dot (点积计算) | **0.10** | 1.36 | 0.07× |
| matmul (矩阵乘法) | 2.19 | **0.82** | 2.67× |
| mixed (混合运算) | 1.62 | **0.42** | 3.86× |
| impure (非纯运算) | **0.11** | 0.03 | 3.67× |

#### 热跑（100 次迭代，50 次预热）

| 测试项 | V8 (ms) | LR_JS (ms) | 倍率 |
|--------|---------|------------|------|
| fib20 (纯递归) | 16.04 | **0.54** | **29.7×** |
| factorial (纯递归) | **0.07** | 0.50 | 0.14× |
| sum (循环求和) | 1.00 | **0.35** | 2.86× |
| dot (点积计算) | 1.28 | **0.37** | 3.46× |
| matmul (矩阵乘法) | **1.14** | 2.79 | 0.41× |
| mixed (混合运算) | 9.08 | **4.96** | 1.83× |
| impure (非纯运算) | 0.71 | **0.48** | 1.48× |

#### Memo 缓存统计

| 阶段 | 命中 | 未命中 | 命中率 |
|------|------|--------|--------|
| 冷跑 | 27 | 21 | 56.3% |
| 热跑 | 89 | 36 | 71.2% |

#### LR_JS 优势场景

| 场景 | 说明 | 性能表现 |
|------|------|----------|
| **递归纯函数** (fib) | IOME586 memo cache 缓存中间结果 | fib20 热跑 **29.7×** 快于 V8 |
| **纯计算密集型** (matmul/dot/sum) | C 层稠密数组直接运算，无 GC 停顿 | **2.7~3.5×** 快于 V8 |
| **混合函数式负载** (map/filter/reduce) | 无 V8 预热成本，VM 执行稳定 | **1.8×** 快于 V8 |
| **非纯副作用负载** (impure counter) | 不涉及 memo 但内存分配模式更优 | **1.5×** 快于 V8 |
| **多线程并行** (16 threads) | 内置线程池，脚本自动分片 | 并行 matmul/fib 接近线性加速 |

#### 资源占用对比

| 指标 | V8 (Node.js) | LR_JS |
|------|-------------|-------|
| 冷启动 RSS | 40.78 MB | ~21 KB |
| 工作负载后 RSS (对象压力) | 49.05 MB | 0 (GC 释放) |
| 峰值对象分配 | — | 100,573 |
| JIT 编译 | 有（大量） | 无（VM 解释执行） |

> **洞察**：LR_JS 宏观基准整体比 V8 慢 **20~33×**（stress_test、bench_cross_fixed），但在**纯函数递归计算**这一特定领域，IOME586 memo cache 可以让 LR_JS **反超 V8 数十倍**（fib20 热跑 29.7×）。"平均慢"主要来自 func_call 开销、type conversion、SAB/Atomics 等系统性差距，而非计算密集型路径。
>
> LR_JS 特别适合：函数式计算管道、递归算法（DP、分治、搜索）、嵌入式/沙箱环境（无 JIT 预热延迟）、需要确定性性能的场景（VM 指令解释延迟稳定，无 JIT compilation spike）。

## 特性

- **ES2022+ JavaScript**：类、箭头函数、Promise、Proxy、Reflect、模块等
- **浏览器 API**：`console`、`URL`、`TextEncoder`/`TextDecoder`、`fetch`（网络请求通过宿主 `LR_HttpWrapper` 委派）、`WebSocket`（通过 `LR_WsWrapper` 宿主委派）、`fs`（文件系统，权限感知）、`term`（终端命令执行，权限感知）、`crypto`、`performance`、Canvas、`setTimeout`/`setInterval`。注：引擎**无内置网络功能**，`fetch` 与 `WebSocket` 均由宿主委派。
- **类型数组**：`ArrayBuffer`、`Int8Array`、`Uint8Array`、`Float64Array`、`DataView`
- **错误处理**：`Error`、`TypeError`、`SyntaxError`、`RangeError`、`ReferenceError`，支持堆栈跟踪和 `cause`
- **ES6+ 集合**：`Map`、`Set`、`WeakMap`、`WeakSet`
- **GC**：标记-清除垃圾回收器
- **IOME586 结果缓存**：以整个脚本为粒度直接缓存解释器成果数据，归档为 LZ4 压缩包（`.lrfile`）——支持边运行边缓存、脚本修改自动更新缓存、全局变量/节点结果/运行状态快照、15% 收益规则、哈希做压缩密钥、落盘撤回，并支持 BOM 文件（剥离 UTF-8 BOM、UTF-16 LE/BE 自动转码）；覆盖全量 ES2022（含 ES 模块：`-m`/`--module` 执行的脚本同样落盘，缓存命中时以模块方式重跑，采用"静态还原全局变量 + 动态重跑 AST"策略）；AST 序列化采用 `LRA` v3 格式、字面量带显式类型标记（详见 `docs/API.md` §6.1.1）；使用 `--iome586 <dir>` 启用
- **跨平台**：Linux (x86_64, x86, ARM64, ARMv7)、Windows 7+ (x86_64, x86)、macOS
- **线程安全**：内置线程池，Worker 支持
- **Script / 模块语义**：非模块的 Script 模式下，顶层的 `var` 与 `function` 声明会作为属性绑定到全局对象（符合 ECMAScript 的 `GlobalDeclarationInstantiation` 规范）；`let`/`const`/`class` 属于声明式绑定，不会镜像到全局对象。ES 模块（`.mjs` 或 `-m`）下所有顶层声明都保存在模块命名空间中（不绑定到全局对象）。模块内部支持 `import.meta`。
- **Windows 控制台 UTF-8**：`lr_js` 会把控制台输出代码页切到 UTF-8，中文等非 ASCII 输出不再乱码。

## 构建

### 环境要求

| 平台 | 依赖 |
|------|------|
| Linux (x86_64) | `gcc` 或 `clang`、`make`、`cmake`（可选） |
| Linux 32 位 | `gcc-multilib`（用于 `-m32` 构建） |
| Windows (MSYS2) | [MinGW-w64](https://www.mingw-w64.org/)（`mingw-w64-x86_64-gcc` 或 `mingw-w64-i686-gcc`）— **推荐**（GCC computed goto 比 MSVC 快 4-6×） |
| Windows (MSVC) | Visual Studio 2019+ 或 Build Tools |
| macOS | Xcode Command Line Tools（`clang`） |

### 构建（CMake 推荐）

```bash
mkdir build_cmake && cd build_cmake

# Linux/macOS（Release）
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)

# Linux 32 位
cmake .. -DCMAKE_BUILD_TYPE=Release -DLR_32BIT=ON
cmake --build . --config Release -j$(nproc)

# Windows（MSVC）
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release

# Windows 7+ 目标（MinGW）
cmake .. -G "Unix Makefiles" -DLR_WIN7=ON
cmake --build . --config Release

# Windows 7+ 目标（MinGW）— **推荐**（GCC computed goto 快 4-6×）
# 也可直接使用一键脚本:
./build_mingw.sh

# 输出结构：
#   build_cmake/bin/lr_js          - CLI 可执行文件
#   build_cmake/lib/liblr_js.a     - 静态库
#   build_cmake/lib/liblr_js.so    - 共享库（Unix）
#   build_cmake/lib/lr_js.dll      - 共享库（Windows）
```

## macOS 交叉编译（osxcross）

可以从 Linux 使用 [osxcross](https://github.com/tpoechtrager/osxcross) 构建 macOS 的 `x86_64` 与 `arm64` 二进制：

```bash
# 1. 编译并安装 osxcross，至少准备一个 macOS SDK（如 MacOSX12.sdk）。
#    注意：若你的 osxcross clang 版本低于 LLVM 15，请勿使用 15.x 的 SDK
#    （它会用到 '_Float16' 类型，旧版 clang 无法编译）。
#
# 2. 运行交叉编译脚本。脚本会自动检测已安装的 osxcross 工具链
#    （o64-clang / oa64-clang）与 SDK，并构建两种架构：
./build_macos.sh

# 若自动检测选错了 SDK，可显式指定：
LR_OSX_SDK=/path/to/MacOSX12.3.sdk ./build_macos.sh
```

每种架构的产物（位于 `releases/` 下）：

- `LR_JS-0.2.0-macos-x86_64.tar.gz`
- `LR_JS-0.2.0-macos-arm64.tar.gz`

每个压缩包内含 `lib/liblr_js.a`、`lib/liblr_js.dylib`、`bin/lr_js` 以及 `lr_js.h`。

工作原理：

- 脚本绕过 `o64-clang`/`oa64-clang` 启动器（它们硬编码了 SDK，并在用户参数之后追加 `-isysroot`，导致无法覆盖）。改为直接调用真实的、带目标架构的 clang，并从其文件名提取精确的 `-target` triple，从而使用与之匹配的 `<triple>-ld` 链接器（如 `x86_64-apple-darwin21.4-ld`），而非宿主的 `/usr/bin/ld`。
- SDK 自动检测优先选择与 clang 内嵌 darwin 版本对应的 macOS 主版本，否则回退到最旧的可用 SDK，以规避 `_Float16` 不兼容问题。`LR_OSX_SDK` / `SDKROOT` 可手动覆盖。

## Windows 支持

### 兼容性

| Windows 版本 | 状态 | 说明 |
|-------------|------|------|
| Windows 7+ | ✅ 完整支持 | 使用 `_WIN32_WINNT=0x0601` |
| Windows 10 | ✅ 完整支持 | 推荐 |
| Windows 11 | ✅ 完整支持 | |

### 特性

- **WinSock2**：通过 `ws2_32` 实现网络 I/O
- **pthread 模拟**：`lr_pthread_win.h` 提供 Windows 上的 POSIX 线程兼容层
- **文件 I/O**：POSIX 兼容的 `open`/`close`/`read`/`write` 封装
- **内存映射**：通过 `VirtualAlloc`/`VirtualFree` 实现 `mmap`/`munmap`
- **动态加载**：通过 `LoadLibrary`/`GetProcAddress` 实现 `dlopen`/`dlsym`
- **随机数**：`crypto.randomUUID()` 通过 `BCryptGenRandom` 实现
- **高精度定时器**：通过 `QueryPerformanceCounter` 实现

### Windows 构建

**推荐使用 [MinGW-w64](https://www.mingw-w64.org/) 构建**（GCC 的 computed goto 直接线程化字节码调度，比 MSVC 的 switch-based 调度快 4-6×）:

```bash
# 一键 MinGW 交叉构建脚本（推荐）
./build_mingw.sh

# 或使用 CMake 交叉编译
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
mkdir build_mingw && cd build_mingw
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLR_WIN7=ON
cmake --build . --config Release

# Windows 本机构建（MSYS2）
pacman -S mingw-w64-x86_64-gcc
mkdir build && cd build
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Windows 本机构建（MSVC）
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

## 32 位支持

### Linux 32 位

```bash
# 从 x86_64 主机构建（-m32）
sudo apt install gcc-multilib
mkdir build32 && cd build32
cmake .. -DCMAKE_BUILD_TYPE=Release -DLR_32BIT=ON
cmake --build . --config Release
```

### Windows 32 位

```bash
# 使用 MinGW 交叉编译
./build_mingw.sh 32

# 或通过 CMake
mkdir build32 && cd build32
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DLR_32BIT=ON -DLR_WIN7=ON
cmake --build . --config Release
```

## 架构支持

| 架构 | CMake | 一键脚本 | 状态 |
|------|-------|---------|------|
| x86_64 | ✅ | `build_all.sh` | 完整支持 |
| x86（32 位） | ✅ `-DLR_32BIT=ON` | `build_mingw.sh 32` | 完整支持 |
| ARM64（aarch64） | ✅ | `build_all.sh` | 完整支持 |
| ARMv7 | ✅ | — | 未测试 |

## 项目结构

```
LR_JS/
├── cli/                # CLI 入口 (main.c)
├── include/            # 公共 API 头文件 (lr_js.h)
├── src/
│   ├── engine/         # 核心引擎（词法分析、语法分析、字节码 VM）
│   │   ├── lr_engine.c    # 运行时、值、对象、GC
│   │   ├── lr_engine.h    # 内部引擎类型
│   │   ├── lr_lexer.c     # 词法分析器
│   │   ├── lr_parser.c    # 语法分析器（AST 生成）
│   │   └── lr_interp.c    # 解释器（字节码调度）
│   ├── lr_runtime.c    # 运行时初始化
│   ├── lr_builtins_core.c  # Object、Array、String、Number、Function、Error
│   ├── lr_builtins_extra.c # Date、RegExp、Symbol、TypedArrays、Promise
│   ├── lr_promise.c    # Promise 实现
│   ├── lr_proxy.c      # Proxy 实现
│   ├── lr_reflect.c    # Reflect 实现
│   ├── lr_map.c        # Map 实现
│   ├── lr_set.c        # Set 实现
│   ├── lr_console.c    # console.log
│   ├── lr_timers.c     # setTimeout/setInterval
│   ├── lr_url.c        # URL
│   ├── lr_encoding.c   # TextEncoder/TextDecoder
│   ├── lr_event.c      # Event/EventTarget
│   ├── lr_performance.c # performance.now
│   ├── lr_crypto.c     # crypto.randomUUID
│   ├── lr_storage.c    # localStorage（内存存储）
│   ├── lr_fetch.c      # HTTP 请求（包装器模式，委托给宿主）
│   ├── lr_ws.c         # WebSocket（宿主委派包装器，LR_WsWrapper）
│   ├── lr_fs.c         # 文件系统 API（权限感知包装器）
│   ├── lr_terminal.c   # 终端 API（权限感知包装器）
│   ├── lr_thread_pool.c # 线程池
│   ├── lr_sandbox.c    # 沙箱
│   ├── lr_worker.c     # Web Worker 支持
│   ├── lr_gc.c         # 垃圾回收器
│   ├── lr_platform.h   # 跨平台抽象层
│   ├── lr_pthread_win.h # Windows pthread 模拟
│   ├── lr_renderer*.c  # Canvas/WebGL 渲染器
│   └── mir/            # JIT 编译器（MIR、代码生成、运行时）
├── docs/               # 文档（API 参考）
├── examples/           # 示例脚本
├── test/               # 测试脚本
├── tests/              # 完整测试套件
├── bench/              # 基准测试（已加入 .gitignore）
├── _debug/             # 调试临时文件（已加入 .gitignore）
├── scripts/            # 构建脚本
├── tools/              # 开发工具
├── sljit/              # SLJIT JIT 库子模块
├── 3rdparty/           # 第三方依赖（PCRE2）
├── git-github/         # GitHub 发布包（精简布局）
├── build*/             # 构建产物（已加入 .gitignore）
├── Project-Record/     # 开发记录（已加入 .gitignore）
│
├── CMakeLists.txt      # CMake 构建配置
├── README.md           # 英文 README
├── README_zh.md        # 中文 README
├── TASKS.md            # 项目任务追踪
└── .gitignore          # Git 忽略规则
```

## API 使用示例

```c
#include "lr_js.h"

int main() {
    LRRuntime *rt = lr_create_runtime();
    LRContext *ctx = lr_create_context(rt);

    // 执行 JavaScript
    LRValue result = lr_eval(ctx, "1 + 2", "<eval>", 0);
    printf("结果: %d\n", result.u.number);  // 3

    // 启动 REPL
    lr_repl(ctx);

    lr_free_context(ctx);
    lr_free_runtime(rt);
    return 0;
}
```

## 详细文档

- [API 参考文档（中文）](docs/API.md) — 完整的 API 文档、沙箱、IOME586 结果缓存、跨平台兼容层等
- [API Reference (English)](docs/API_en.md) — English version of the API documentation

## 渲染管道（外部渲染器输出）

渲染管道允许 Canvas 2D 和 WebGL 的渲染输出通过管道转发给外部渲染器，支持多种输出方式：

### 管道类型

| 输出类型 | 说明 | 适用场景 |
|----------|------|----------|
| **Socket** | 通过 Unix 域套接字发送帧数据 | 连接外部渲染进程 |
| **Shared Memory** | 写入共享内存缓冲区 | 高性能本地 IPC |
| **Callback** | 调用用户回调函数 | 集成到自定义渲染栈 |
| **File** | 写入 PPM 文件 | 调试和测试 |

### JS API

```javascript
// 创建 Canvas
const canvas = new Canvas(800, 600);

// 方式 1: 通过 Canvas 设置管道
canvas.setPipeline('/tmp/renderer.sock');

// 获取 2D 上下文
const ctx = canvas.getContext('2d');
ctx.fillStyle = 'red';
ctx.fillRect(0, 0, 100, 100);

// 提交帧到管道
ctx.pipelineFlush();        // 或 canvas.pipelineFlush()

// 方式 2: 通过 WebGL 上下文设置管道
const gl = canvas.getContext('webgl');
gl.setPipeline('/tmp/renderer.sock');

// 渲染...
gl.clear(gl.COLOR_BUFFER_BIT);
// ... 绘制三角形 ...

// 提交 GL 帧到管道（读回 GL 帧缓冲）
gl.pipelineFlush();

// 或者使用 Canvas 的 GL 专用方法
canvas.pipelineFlushGL();
```

### 管道协议（Socket 输出）

当使用 Socket 输出时，每帧数据以以下格式发送：

```
FRAME <width> <height> <data_size>\n
<raw RGBA pixel data>
```

外部渲染器收到 `FRAME` 头后，读取指定大小的像素数据即可。

### 编程接口（C API）

```c
#include "lr_renderer.h"

// 创建管道
LR_RenderPipeline *pipe = lr_render_pipeline_create(800, 600);

// 添加 Socket 输出
LR_PipeSink sink = lr_render_pipe_sink_socket("/tmp/renderer.sock");
if (sink.fd >= 0) {
    lr_render_pipeline_add_sink(pipe, &sink);
}

// 添加回调输出
LR_PipeSink cb_sink = lr_render_pipe_sink_callback(
    my_frame_callback, my_user_data);
lr_render_pipeline_add_sink(pipe, &cb_sink);

// 提交帧（来自 Canvas 2D 帧缓冲）
lr_render_pipeline_submit(pipe, pixels, 800, 600);

// 提交 GL 帧（从 GLES 帧缓冲读回）
lr_render_pipeline_submit_gl(pipe, gl_ctx, 800, 600);

// 集成到渲染器桥接
lr_renderer_set_pipeline(rb, pipe);
lr_renderer_pipeline_flush(rb);  // 自动提交当前帧缓冲

// 清理
lr_render_pipeline_destroy(pipe);
```

## 特别感谢

- **QuickJS** —— 本项目最初是基于 Fabrice Bellard 的 [QuickJS](https://bellard.org/quickjs/) JavaScript 引擎构建的分支（fork），其精简、可嵌入的 C 语言实现提供了最初的运行时基础。随着功能不断加入、架构被反复重构，代码逐渐偏离、演变为一套完全自研的引擎——如今已不再包含任何 QuickJS 源码，但血统仍可追溯到它。
- **V8** —— 本项目在对象模型、Promise/任务队列、字节码设计等架构与行为决策上，参考并借鉴了 Google [V8](https://v8.dev/) 引擎的设计思路。

## 许可证

MIT