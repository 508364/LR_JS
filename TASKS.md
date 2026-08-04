# L/R_JS 任务列表

纯 C ES2022 JS 引擎 v0.1.1+：字节码 VM + `--parallel N`

## 2026-08-04 最终状态

### 已完成
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

### 基准 (x64 MSVC, stress_run.js)

| | V8 | QuickJS | LR_JS | vs V8 |
|--|-----|---------|-------|-------|
| Class 5000x10deep | 8ms | 33ms | 242ms | 30× |
| Array 100k reduce | 13ms | 33ms | 159ms | 12× |
| String 10k concat | 1ms | 17ms | 82ms | 82× |
| **Total** | **56ms** | **117ms** | **595ms** | **11×** |

### 待办
- [ ] 完整事件循环 + 顶层await (.mjs模块)
- [ ] yield* 迭代栈替代递归深度限制
- [ ] Float64/NaN 算术修复 (closure/arrow NaN)
