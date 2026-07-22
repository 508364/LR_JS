# L/R_JS - 轻量级 JavaScript 引擎

纯 C 语言实现，兼容 ES2022+ 的 JavaScript 引擎，内置浏览器 API。

## 特性

- **ES2022+ JavaScript**：类、箭头函数、Promise、Proxy、Reflect、模块等
- **浏览器 API**：`console`、`URL`、`TextEncoder`/`TextDecoder`、`fetch`（包装器模式，委托给宿主）、`fs`（文件系统，权限感知）、`term`（终端命令执行，权限感知）、`crypto`、`performance`、`WebSocket`、Canvas、`setTimeout`/`setInterval`
- **类型数组**：`ArrayBuffer`、`Int8Array`、`Uint8Array`、`Float64Array`、`DataView`
- **错误处理**：`Error`、`TypeError`、`SyntaxError`、`RangeError`、`ReferenceError`，支持堆栈跟踪和 `cause`
- **ES6+ 集合**：`Map`、`Set`、`WeakMap`、`WeakSet`
- **GC**：标记-清除垃圾回收器
- **跨平台**：Linux (x86_64, x86, ARM64, ARMv7)、Windows 7+ (x86_64, x86)、macOS
- **线程安全**：内置线程池，Worker 支持

## 构建

### 环境要求

| 平台 | 依赖 |
|------|------|
| Linux (x86_64) | `gcc` 或 `clang`、`make`、`cmake`（可选） |
| Linux 32 位 | `gcc-multilib`（用于 `-m32` 构建） |
| Windows (MSYS2) | `mingw-w64-x86_64-gcc` 或 `mingw-w64-i686-gcc` |
| Windows (MSVC) | Visual Studio 2019+ 或 Build Tools |
| macOS | Xcode Command Line Tools（`clang`） |

### Makefile

```bash
# 本机构建（Linux/macOS/Windows-MSYS2）
make

# Linux 32 位（需要 gcc-multilib）
make linux32

# 交叉编译 Windows 7+ 64 位（MinGW-w64）
make win

# 交叉编译 Windows 7+ 32 位（MinGW-w64）
make win32

# 调试构建
make CFLAGS="-O0 -g -fsanitize=address"

# 运行测试
make test

# 启动 REPL
make repl
```

### CMake

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

# 输出结构：
#   build_cmake/bin/lr_js          - CLI 可执行文件
#   build_cmake/lib/liblr_js.a     - 静态库
#   build_cmake/lib/liblr_js.so    - 共享库（Unix）
#   build_cmake/lib/lr_js.dll      - 共享库（Windows）
```

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

```bash
# 从 Linux 交叉编译（MinGW-w64）
sudo apt install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
make win        # 64 位
make win32      # 32 位

# Windows 本机构建（MSYS2）
pacman -S mingw-w64-x86_64-gcc
make

# Windows 本机构建（MSVC）
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

## 32 位支持

### Linux 32 位

```bash
# 从 x86_64 主机构建（-m32）
sudo apt install gcc-multilib
make linux32

# 或通过 CMake
cmake .. -DLR_32BIT=ON
```

### Windows 32 位

```bash
# 交叉编译
make win32

# 或通过 CMake（MSVC）
cmake -B build -G "Visual Studio 17 2022" -A Win32
```

## 架构支持

| 架构 | Makefile | CMake | 状态 |
|------|----------|-------|------|
| x86_64 | ✅ | ✅ | 完整支持 |
| x86（32 位） | ✅ `linux32`/`win32` | ✅ `-DLR_32BIT=ON` | 完整支持 |
| ARM64（aarch64） | ✅ | ✅ | 完整支持 |
| ARMv7 | ✅ | ✅ | 未测试 |

## 项目结构

```
LR_JS/
├── cli/main.c          # CLI 入口
├── include/
│   └── lr_js.h         # 公共 API 头文件
├── src/
│   ├── engine/         # 核心引擎（词法分析、语法分析、解释执行）
│   │   ├── lr_engine.c    # 运行时、值、对象、GC
│   │   ├── lr_engine.h    # 内部引擎类型
│   │   ├── lr_lexer.c     # 词法分析器
│   │   ├── lr_parser.c    # 语法分析器（AST 生成）
│   │   └── lr_interp.c    # 解释器（字节码 + AST 遍历）
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
├── lr_fs.c         # 文件系统 API（权限感知包装器）
├── lr_terminal.c   # 终端 API（权限感知包装器）
│   ├── lr_thread_pool.c # 线程池
│   ├── lr_sandbox.c    # 沙箱
│   ├── lr_worker.c     # Web Worker 支持
│   ├── lr_gc.c         # 垃圾回收器
│   ├── lr_platform.h   # 跨平台抽象层
│   ├── lr_pthread_win.h # Windows pthread 模拟
│   └── lr_renderer*.c  # Canvas/WebGL 渲染器
└── Project-Record/     # 开发记录（不纳入 git 跟踪）
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

- [API 参考文档（中文）](docs/API.md) — 完整的 API 文档、沙箱、字节码缓存、跨平台兼容层等

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

## 许可证

MIT