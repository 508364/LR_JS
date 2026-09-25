# L/R_JS 任务列表

纯 C ES2022 JS 引擎 v0.2.0：字节码 VM + `--parallel N` + 多线程线程池 + 渲染器桥接 + 跨平台 (Linux/macOS/Windows/BSD/Android/iOS)

## 2026-08-24 最新状态 (v0.2.0)

### 已完成
- [x] 大脚本支持：SCOPE_FUNC_CAP 24→256 + packed_alloc 溢出自动降级
- [x] 大循环优化：内联 int32 快速路径 (BC_ADD/SUB/MUL/LT/GT/LE/GE)
- [x] 循环 tick 优化：timeout=0 时跳过 clock() 调用
- [x] 条件跳转优化：int32(bool) 快速路径避免 lr_to_bool 调用
- [x] 修复 int32 溢出：BC_ADD/BC_SUB 使用 int64_t 中间计算
- [x] 修复 var 作用域：移除 for 循环周围的 BC_SCOPE_ENTER/LEAVE
- [x] arguments 惰性创建：-25% (2796→1861ms)
- [x] BC_PUSH_THIS + bc_body_cache MRU O(1)
- [x] Array.reduce int64 直读：溢出修复, 361→159ms
- [x] Scope 打包分配：function scope 1×calloc(曾5×), stress 636→595ms
- [x] new Function eager compile + eval unit释放
- [x] GC: double-free, extra=NULL, shutdown order
- [x] `--parallel N` (1-16) AST拆分+沙箱
- [x] IOME586 warm CAS预编译
- [x] LR_SharedResults 16沙箱共享内存池
- [x] yield* 深度限制256
- [x] 闭包变量缓存 P0 修复：scope reachability 校验 + 修复 slot 错位
- [x] 空函数调用优化：debug flag 缓存 + BC_LOAD_VAR 路径优化 + memoization 改进 (116ms→21ms)
- [x] 对象属性 IC 正确性验证 (5-property 48ms, 与 local var 46ms 持平)
- [x] 字节码融合修复：instruction boundary 跟踪避免 BC_CALL_METHOD 操作数误改写
- [x] for-of 块级作用域修复：local_scope_depth 跟踪 (BC_SCOPE_ENTER/LEAVE)
- [x] 解构模式 slot 对齐修复：register_pattern_names 预注册 destructured 标识符
- [x] try/catch slot 对齐修复：预声明 catch 变量 + 编译器预注册
- [x] Array.push length 更新修复：shape flat slot 同步写入
- [x] Atomics.compareExchange 语义修复：移除 fake old=0 预过滤，8 字节对齐 CAS 路径修复
- [x] SAB 校验和与 V8 完全对齐 (32145560)
- [x] Memoization 缓存修复：BC_CALL/BC_LOAD_VAR 标记为纯操作，fib(35) 15000ms→1ms
- [x] Class 深层继承性能倒退诊断 (10-level 155ms, O(depth) 线性放大)
- [x] **type_conv 内联优化**：String/Number/Boolean 构造函数调用内联为 BC_TO_STRING/BC_TO_NUMBER/BC_TO_BOOL 单指令，避免完整函数调用开销 (428ms→**59ms**, 7.3×)
- [x] **v0.2.0**: 直接/间接线程式字节码 VM (computed goto / switch dispatch)
- [x] **v0.2.0**: IOME586 memo cache 纯函数结果缓存 (冷跑 56.3% 命中率，热跑 71.2%)
- [x] **v0.2.0**: fib20 热跑 29.7× 快于 V8
- [x] **v0.2.0**: dot/sum/mixed/impure 等纯计算负载 1.5~3.5× 快于 V8
- [x] **v0.2.0**: `lr_memory_usage()` JS 全局函数暴露内存统计
- [x] **v0.2.0**: `lr_js_version` / `__LR_PARALLEL_THREADS__` 全局变量
- [x] **v0.2.0**: 内存使用负值修复
- [x] **v0.2.0**: 16 线程并行执行
- [x] **v0.2.0**: AST 树遍历解释器退役
- [x] **v0.2.0**: 线程池、渲染器桥接、多平台支持 (FreeBSD/OpenBSD/NetBSD/Android/iOS)
- [x] **v0.2.0**: LR_Config 新增字段: `enable_thread_pool`/`thread_pool_size`/`enable_sandbox`/`enable_perf_optimizations`/`enable_renderer`/`module_paths`/I/O 重定向
- [x] **v0.2.0**: Infinity/NaN 格式化修复
- [x] **v0.2.0**: 生成器返回值修复
- [x] **v0.2.0**: string replace 全局匹配修复
- [x] **v0.2.0**: Symbol.iterator / Array Iterator 修复
- [x] **v0.2.0**: 左移 UB 修复
- [x] **v0.2.0**: INT32_MIN 字符串转换修复
- [x] **v0.2.0**: 清理 135 测试文件、5 temp 文件、3 空文件、67 jit-debug benchmark 文件
- [x] **v0.2.1-dev**: Release 版本输出清理：将所有 JIT/MIR/BTM 调试输出从编译期宏改为运行时开关 `g_lr_debug`，默认 Release 运行 stdout 干净、stderr 0 行，`--debug` 模式产生完整诊断信息（550+ 行）
- [x] **v0.2.1-dev**: 目录结构重构：参考 git-github 仓库样式，创建 `_debug/`（调试临时文件 117 个）和 `bench/`（基准测试 3 个）目录，更新 `.gitignore` 和 README
- [x] **v0.2.1-dev**: f64 JIT 完整支持：12 个新 MIR opcodes (add_f64/sub_f64/mul_f64/div_f64/mod_f64/neg_f64/lt_f64/gt_f64/le_f64/ge_f64/eq_f64/ne_f64)，前端不再 float64 bail-out，后端用 SLJIT 原生 f64 指令编译，纯 f64 计算性能与 QuickJS 持平（10-16× 差距来自 VM 派发而非 JIT 缺失）

### 版本历史基准 (v0.2.0, x64 Linux GCC)

| 测试项 | LR_JS v0.2.0 | V8 (Node v24) | vs V8 |
|--------|-------------|--------------|-------|
| stress_test_noawait.js | 3996ms (5轮) | 198ms (1轮) | 20.1× (单轮 ~800ms vs 199ms) |
| stress_run.js | 148ms | 15ms | 9.9× |
| fib20 (warm) | — | — | 29.7× 快于 V8 |
| SAB 校验和 | 32145560 | 32145560 | 完全一致 ✓ |

### v0.2.0 新增 API

```c
// 统一创建 API（取代旧 lr_create_runtime/lr_create_context）
LR_Runtime *lr_runtime_new(const LR_Config *cfg);
void        lr_runtime_free(LR_Runtime *rt);
int         lr_eval(LR_Runtime *rt, const char *source, size_t source_len, const char *filename);
int         lr_eval_file(LR_Runtime *rt, const char *filename);
int         lr_eval_module(LR_Runtime *rt, const char *source, size_t source_len, const char *filename);

// 事件循环
int  lr_event_loop_run(LR_Runtime *rt);
int  lr_event_loop_run_timeout(LR_Runtime *rt, int timeout_ms);
int  lr_event_loop_pending(LR_Runtime *rt);
void lr_event_loop_stop(LR_Runtime *rt);

// 内存
void lr_gc(LR_Runtime *rt);
void lr_gc_print_stats(LR_Runtime *rt, FILE *fp);
void lr_gc_reset_stats(LR_Runtime *rt);
void lr_bytecode_cache_stats(LR_Runtime *rt, FILE *fp);
void lr_bytecode_cache_clear(LR_Runtime *rt);
int  lr_check_system_memory(size_t min_bytes);
int64_t lr_get_available_memory(void);
const char *lr_version(void);
```

### 待办
- [ ] **Class 深层继承构造优化**：当前 O(depth) 线性放大（10层 155ms），目标接近 O(1)/实例
- [ ] **函数调用优化**：func_call vs V8 仍有差距，P2 目标空函数调用 <20ms
- [ ] 正则性能：PCRE2 vs V8 原生引擎
- [ ] 生成器/迭代器性能优化
- [ ] 循环体指令精简
- [ ] 尾调用优化
- [ ] 完整事件循环 + 顶层 await (.mjs模块)
- [ ] yield* 迭代栈替代递归深度限制

### 已知瓶颈 / 缺点 / 漏洞点

#### A. 性能瓶颈

| 瓶颈 | 现状 | 根因 | 可能的修复方向 | 来源 |
|------|------|------|---------------|------|
| func_call 函数调用 | vs V8 有差距 | 函数调用帧/作用域分配开销大、var_cache_gen++ 缓存失效 | 帧池 + 寄存器式调用 | round5/7/8/9/11/13/16 |
| closures 闭包 | vs V8 有差距 | 闭包单元分配 + 捕获变量访问开销 | 闭包单元池化/复用 | round9/11/16 |
| 正则表达式 | PCRE2 vs V8 原生 | PCRE2 JIT 未启用（SLJIT 依赖未嵌入） | 嵌入 SLJIT | round2/4/16 |
| 生成器/迭代器 | vs V8 有差距 | 迭代器机制开销 | 待分析 | round2/16 |
| obj_access 属性访问 | shape cache miss | shape 首次访问开销 | BC_LOAD_LOCAL+BC_GET_PROP 融合 | round7/8/9/11/16 |
| string_concat 字符串拼接 | 每步 malloc | GC 安全的长串缓冲策略 | 环形缓冲池 | round9/11/12/16 |
| Class 深层继承 | 54ms vs V8 2ms (10层 155ms) | super 链 O(depth) | super 链 O(1) | round16/17 |
| empty_loop 循环派发 | 架构性开销 | 字节码 VM 派发 | 循环体指令精简 | round7/16 |

#### B. 缺点 / 漏洞点

| 问题点 | 说明 | 修复方向 | 来源 |
|--------|------|---------|------|
| LR_EVAL_MAX_DEPTH_DEFAULT=256 | 深层嵌套表达式解析受限 | 已提高至 256 | round2/20 |
| ASAN 栈溢出 | M4 深度200递归+尾递归500 | `ulimit -s 65536` 规避 | round15 |
| JIT bailout 与 memoization 冲突 | JIT 对已 memoization 函数触发 bailout | bailout 检查/标记集成 | round12/13 |
| memoization 纯函数分析放宽 | BC_CALL/BC_LOAD_VAR 被放宽为纯操作 | 更精细分析 | round13 |
| PCRE2 JIT 未启用 | SLJIT 依赖未嵌入 | 如需 JIT 嵌入 SLJIT | round4 |
| DOM 绑定不完整 | 事件处理、CSSOM、表单控件待完善 | 后续功能开发 | round12 |
| 顶层 await / .mjs 模块 | 完整事件循环未完全实现 | 事件循环开发 | round2/待办 |
| 尾调用优化未做 | 递归深函数无 TCO | 尾递归消除 | 待办 |
| yield* 迭代栈未替换 | 仍用深度限制（已 256→8192） | 迭代栈替代递归 | round2/待办 |

#### C. 安全 / 黑名单状态

- 各轮已核查：无硬编码 API key/token/secret（round9/10/11）。
- `Project-Record/` 已在 `.gitignore` 黑名单，不会上传 GitHub（round9/10/11 确认）。
- `docs/API.en.md` 已统一重命名为 `docs/API_en.md`（round24 命名统一）。
