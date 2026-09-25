# 性能测试结果 2026-08-29

## 环境
- Windows x64
- Node.js v22.15.0 (V8)
- QuickJS 0.16.1
- LR_JS v0.2.0 (Release, build-release/bin/lr_js.exe)

---

## 基准测试 1: bench_simple_cross.js (跨引擎兼容测试)

| 测试项 | LR_JS | V8 (Node.js) | QuickJS |
|--------|-------|-------------|---------|
| loop 100k | 11 ms | 6 ms | 4 ms |
| nested_fns 10k | 1942 ms | 1 ms | 1 ms |
| fib25 | 11 ms | 4 ms | 22 ms |
| array_reduce 10k | — | 2 ms | — |
| **total** | **~1964 ms** | **~12 ms** | **~23 ms** |

> LR_JS 的 nested_fns 显著慢于 V8/QJS，是函数调用开销的典型瓶颈。

---

## 基准测试 2: tests/stress_run.js (10项压力测试)

| 测试项 | LR_JS | V8 (Node.js) | QuickJS |
|--------|-------|-------------|---------|
| [1] Class 5000x10deep | 1146 ms | 11 ms | 30 ms |
| [2] Map/Set 5000 | 15 ms | — | 5 ms |
| [3] Closure 5000 | 47 ms | — | 2 ms |
| [4] Recursion fact(200) | 17 ms | — | 0 ms |
| [5] Destructuring 2000 | 16 ms | — | 1 ms |
| [6] Regex 500 | 10 ms | — | 0 ms |
| [7] Try/catch 50x5deep | 7 ms | — | 3 ms |
| [8] Array 100k | 22 ms | — | 15 ms |
| [9] Arrow 10k | 260 ms | — | 261 ms |
| [10] String concat 10k | 5 ms | — | 2 ms |
| **Total** | **1546 ms** | **58 ms** | **87 ms** |

> LR_JS vs V8 慢约 **26.7×**，vs QuickJS 慢约 **17.8×**。主要差距在类继承和函数调用。

---

## 基准测试 3: tests/test_es2022_comprehensive.js (ES2022 全功能测试)

| 测试项 | LR_JS | V8 (Node.js) | QuickJS |
|--------|-------|-------------|---------|
| [1] Core Syntax & Operators | 40.6 ms | 6.4 ms | 22.2 ms |
| [2] Functions & Closures | 19937.2 ms | 54.6 ms | 160.7 ms |
| [3] Classes & Inheritance (5000×5) | 1291.1 ms | 18.6 ms | 18.8 ms |
| [4] Objects/Destructuring/Spread | 394.2 ms | 75.2 ms | 37.5 ms |
| [5] Arrays & Iteration (20k) | 794.5 ms | 10.7 ms | 18.4 ms |
| [10] async/await & Promises | — | 59.9 ms | 61.8 ms |
| **Total (excl async)** | **~23255 ms** | **~216 ms** | **~359 ms** |

> LR_JS vs V8 慢约 **107.7×**，vs QuickJS 慢约 **64.8×**。
> 主要瓶颈是 `[2] Functions & Closures`（19937ms vs V8 54.6ms）。

---

## 关键发现

1. **函数调用是最大瓶颈**: nested_fns 10k 测试中，LR_JS 1942ms vs V8/QJS 1ms，差距近 2000×
2. **类继承**: 5000个10层类实例，LR_JS 1146ms vs V8 11ms，差距 104×
3. **Functions & Closures 段**: 19937ms vs V8 54.6ms，差距 365× — 这是整体差距的主要来源
4. **基础运算** (loop、array) 差距相对较小，LR_JS 约 2-4×

## 对比参考 (TASKS.md 已有数据)

| 指标 | LR_JS v0.2.0 | V8 | vs V8 |
|------|-------------|----|-------|
| stress_test_noawait | 1535 ms | — | — |
| stress_run | 1546 ms | 58 ms | 26.7× |
| fib20 热跑 | — | — | 29.7× 快于 V8 |
| SAB 校验和 | 32145560 | 32145560 | 完全一致 ✓ |
