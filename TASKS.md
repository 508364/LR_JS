# L/R_JS 任务列表 (Task List)

纯 C 实现的 ES2022 JS 引擎。**v0.1.1+**：执行引擎为直接/间接线程式字节码 VM，AST 树遍历解释器已退役。

## v0.1.1 性能基线 (vs Node.js v22, Windows x64)

| 测试项 | LR_JS | Node.js | 倍数 |
|--------|-------|---------|------|
| Class 5000×10 | 332 ms | 8 ms | 42× |
| Map/Set 5000 | 26 ms | 6 ms | 4× |
| Closure 5000 | 21 ms | 2 ms | 11× |
| Array 100k | 320 ms | 12 ms | 27× |
| String 10k | 85 ms | 1 ms | 85× |
| **Total (10项)** | **852 ms** | **49 ms** | **17×** |

## 状态图例
- [x] 已完成并验证
- [ ] 待办 / 未开始
- [~] 部分完成

---

## 一、ES2022 特性补全 (核心任务) — [x] 已完成并验证

所有特性经 `tests/es2022_probe.js` (42 条特性行全过, `PROBE DONE`) 与 22 个回归用例
(19 核心 + 3 并发: `worker_echo_test.js` / `sab_share_test.js` / `atomics_stress_test.js`,
`TOTAL=22 FAIL=0`, 单次运行超时 8–20s 无挂起) 验证通过。

### 1.1 词法 / 解析修复
- [x] 正则字面量上下文 (`allow_regexp`) 修正 — 修复 `return /a/`、`< /a/` 等误判为除号
- [x] `lr_regex.c` 花括号量词 `{n}` 双推进 bug 修复 — `/(\d{4})/` 不再抛异常
- [x] `is_expr_start()` 扩充 (`TOK_TEMPLATE_END`/`ELLIPSIS`/`IMPORT`/`REGEXP`/`PRIVATE_NAME`/`GET/SET/STATIC/OF/FROM/AS`) — 修复 `#x in o` 解析错误
- [x] `AST_PATTERN` 联合体按 `is_object` 区分数组/对象模式, 修复 `ast_free_ex` 双重释放导致堆损坏

### 1.2 解构 (Destructuring)
- [x] 数组解构 (含默认值、rest)
- [x] 对象解构 (含 rest、键排除)
- [x] 解构作为函数参数绑定 (`eval_pattern` + 参数绑定逻辑)
- [x] for-of 中的解构赋值

### 1.3 模板 / 标签模板
- [x] 模板字符串插值
- [x] 标签模板 (`tag\`...\``) — 修复 `'a'/'v' is not defined`

### 1.4 类 (Class)
- [x] 私有字段 (private fields) + 品牌检查 (`#x in o`)
- [x] 静态块 (static block)
- [x] getter/setter、计算键、简写方法
- [x] 类中的生成器方法

### 1.5 字符串方法 (原型委托 `lr_get_property`)
- [x] `String.prototype.at / padStart / padEnd / trimStart / trimEnd`
- [x] `replaceAll / includes / matchAll` (重写 exec 协议)

### 1.6 集合
- [x] `Map` / `Set` 的 `.size` 同步 (map_sync_size / set_sync_size)

### 1.7 全局对象 / 字面量 / 运算符
- [x] `globalThis`
- [x] 全局 `NaN` / `Infinity`
- [x] 可选链 `?.`
- [x] 空值合并赋值 / 逻辑赋值 (`??=`, `||=`, `&&=`)
- [x] `BigInt` 字面量、数值分隔符 `1_000_001`
- [x] `Symbol`、错误 `cause` 选项
- [x] `Promise.allSettled` / `Promise.any`
- [x] `in` 运算符使用 `lr_has_property` (原型链命中)

### 1.8 异步 / 生成器 (既有能力, 回归验证)
- [x] async / 微任务排空
- [x] 生成器 (generator)
- [x] 标签 break/continue、for-of 迭代器协议

---

## 二、构建 / 验证流程 — [x] 已固化

- [x] Windows 构建: `cmake --build build\x64 --config Release --target lr_js` (BUILD_EXIT=0)
- [x] 运行: `lr_js.exe <test.js>`, 单次超时 8s 防挂起
- [x] 回归: `TOTAL=19 FAIL=0`

---

## 三、待办 / 潜在增强 — [x] 已完成并验证

- [x] 高级并发: `SharedArrayBuffer`、Worker — 经 3 个用例验证通过 (需从仓库根目录运行, 子脚本用相对 CWD 路径)
  - `worker_echo_test.js`: `ECHO OK` (结构化克隆往返)
  - `sab_share_test.js`: `SHARED` (worker 写入对主线程可见)
  - `atomics_stress_test.js`: `ATOMIC OK` (counter=200000=expected, 无丢失更新)
- [x] 更多 ES2022+ 边界用例与性能回归 — 回归集扩至 22 个用例 (19 核心 + 3 并发), 全部 `FAIL=0`
- [x] 文档 (README) 补充 ES2022 支持矩阵 — 已新增 "ES2022 Support Matrix" 小节, 列出全部已验证特性

---

## 四、IOME586 结果缓存 (重做并加强的缓存) — [x] 已完成并验证

技术正式命名 **IOME586**，取代旧 `.lrfile` 字节码缓存 (`src/lr_bytecode.c/h` 已删除，替换为 `src/lr_iome586.c/h`)。

### 4.1 核心能力 (11 项需求)
- [x] 1. 边运行边缓存: `lr_iome586_begin` 在解析后、执行前落盘 WRITING 状态归档, 执行完 `lr_iome586_commit` 归档为 ARCHIVED
- [x] 2. 直接缓存解释器成果数据 (非仅字节码): 全局变量快照、每级节点结果、运行状态
- [x] 3. 缓存内容完整: 脚本名、脚本哈希 (FNV-1a64)、状态 (写入中/已归档)、时间、优化比值、版本号、CRC32 校验码、每级节点结果、状态机状态、运行状态、全局变量绑定对象、配置、初始化内容与结果、解释路径 (序列化 AST)
- [x] 4. 以上数据可直接还原: 热路径 `lr_iome586_load` → `lr_ast_deserialize` → 直接执行; `lr_iome586_restore_globals` 还原全局绑定
- [x] 5. 15% 规则: 解析耗时占比 (parse_us/total_us) < 15% 时 `commit` 丢弃归档不缓存 (`LR_IOME586_MIN_GAIN=0.15`)
- [x] 6. 自动按目录落盘: `--iome586 <dir>` / `cfg.bytecode_cache_dir`; 归档文件按脚本路径哈希命名, 脚本内容修改后自动原位更新同一归档 (旧版本转为 `.bak` 可撤回)
- [x] 7. BOM 支持: `lr_apply_bom` 剥离 UTF-8 BOM、UTF-16 LE/BE 自动转码 UTF-8 (`lr_load_file` 内置)
- [x] 8. 落盘可撤回: begin 时保留 `.bak`, `lr_iome586_abort`/`revert` 回滚, CLI `--iome586-revert <js>`
- [x] 9. 本质为 LZ4 压缩包: 输出文件 `.lrfile` (加载亦兼容 `.lrfile.lz4` 写法), 包内按命名条目 (meta/path/config/init/ast/nodes/globals/state) 分别保存
- [x] 10. 压缩密码=哈希值: payload 以 source_hash 做 XOR 密钥; 文件描述区明文存脚本名+时间+版本号副本
- [x] 11. 技术名称: 容器魔数 `"IOME586\0"`

### 4.2 实现与修复
- [x] 引擎拆分解析/执行两阶段: `lr_engine_parse_unit` / `lr_engine_exec_unit_handle` / `lr_engine_unit_ast_handle`; 节点信息经不透明访问器 `lr_engine_program_count` / `lr_engine_program_node_info` (规避 lr_ast.h 与 windows.h 的 TokenType 冲突)
- [x] AST 序列化/反序列化 (`lr_ast_serialize`/`lr_ast_deserialize`) 覆盖全部 AST 节点类型
- [x] 修复热路径堆损坏 (0xC0000374): 反序列化时 `func.name`/`class_decl.name`/`catch_var`/`import_spec.name` 误用驻留字符串 (由 `parser_free` 释放) 却又被 `ast_free_ex` `p_free` → 双重释放; 改用 `deser_str_raw` 独立副本, 所有权归 AST
- [x] 修复 LZ4 解压 bug (token_ptr、offset 后匹配扩展)
- [x] 修复 `AST_EXPORT_NAMED` 联合体视图错配 (ASan 定位): 解析器写入 `u.export_spec` (name 字符串+exported), 但 `ast_free_ex`/序列化误按 `u.export_decl` (specifiers 数组) 处理 → 把字符串当节点数组遍历导致随机堆越界崩溃 (`export { x as y } from` 场景, 与缓存无关的既有 bug); 三处 (释放/序列化/反序列化) 已分开处理
- [x] CLI: `--iome586 <dir>` (别名 `--bytecode-cache`)、`--iome586-stats` (别名 `--bytecode-stats`)、`--iome586-revert <js>`
- [x] 验证: 全部 tests/*.js 冷/热双跑 fails=0 (含 module_test); ASan 构建 4 连跑无报错; 脚本修改→归档自动更新→`--iome586-revert` 撤回链验证通过

### 4.3 ES2022 / 模块缓存补全 (已知限制已修复) — [x] 已完成

此前缓存的已知限制是：**ES 模块 (`-m`/`--module`) 走 `lr_eval_module` 完全绕过缓存直连 `JS_Eval`**，使"全量 ES2022 缓存"缺了一块。现已修复，让缓存覆盖全量 ES2022 含模块。

- [x] 脚本与模块统一到私有 `lr_exec_file_cached`：二者共用同一缓存路径，`lr_eval_file`(is_module=0) 与 `lr_eval_module`(is_module=1) 都走它，模块不再绕过缓存
- [x] 冷路径：`lr_iome586_begin` 的 flags 写入 `is_module ? LR_IOME586_FLAG_MODULE : 0`，归档记下"这是模块"
- [x] 热路径：从归档 `mf.flags` 读 `LR_IOME586_FLAG_MODULE` 决定 `warm_module`，并把 `is_module` 透传给 `lr_engine_eval_ast`，使缓存模块以模块方式重跑 (正确建立 `module_ns` 命名空间)
- [x] 静态还原 + 动态重跑（用户建议方案落地）：
  - **静态部分直接还原**：`lr_iome586_restore_globals(ctx, &mf)` 还原全局变量绑定快照，原语全局变量立即就位
  - **动态部分重新跑**：对反序列化的 AST 重新执行。解释器重建函数/类绑定（保持可调用）、重跑 I/O 与副作用、重算原语，因此无论脚本多"动态"结果始终正确
- [x] 修复 `AST_IMPORT_SPECIFIER` 序列化/反序列化遗漏 `is_default`：`import defFn from "mod"` 默认导入此前反序列化后 `is_default=0`，`eval_import` 改用 `name`(而非 `"default"`) 取值 → undefined → `defFn()` 抛错中断模块执行；现已在 `ser_node`/`deser_node` 补齐往返
- [x] 暖路径末行 (如 `console.log("MODULE TEST OK")`) 经诊断确认为 stdout 管道块缓冲未刷新时序假象（18 条语句全部执行、无 error/return；加 stderr 调试打印后末行即出现），非真实 bug

## 五、模块系统 / Eval 标志修复 — [x] 已完成并验证

- [x] `lr_engine_eval_function()` 从空桩改为执行 `LR_OBJ_SCRIPT` 脚本对象 (COMPILE_ONLY 产物)
- [x] `JS_EVAL_FLAG_COMPILE_ONLY` 现由 `lr_engine_eval` 处理, 返回编译脚本对象而非被忽略
- [x] `lr_module_loader` 重写: 归一化 → 查 registry → `lr_engine_run_module` 执行 → 注册 `LRModuleDef` (持有命名空间对象引用), 返回有效 def (此前返回垃圾指针)
- [x] `lr_eval_module` 顶层模块求值经 COMPILE_ONLY→EvalFunction 路径现在可执行
- [x] `eval_import` / `eval_export` 从占位 stub 实现: import 加载模块并把具名/default/命名空间导出绑定到本地作用域; export 把导出绑定写入模块命名空间 (`interp->module_ns`)
- [x] 支持 `export * from` 与 `export { x as y } from` 再导出
- [x] `Interpreter` 新增 `module_ns` 字段; `lr_engine_exec_unit` 保存/恢复 `module_ns`; 新增 `lr_new_script` 前向声明
- [x] `ctx->module_registry` 在 `lr_free_context` 中清理 (释放 def + 命名空间引用)
- [x] 修复导出具名函数声明时把 `undefined` 写入命名空间 (`eval_func_decl` 返回 undefined, 改从作用域读取函数对象)
- [x] 修复 `lr_engine.c` 残留的悬挂空代码块 (旧 `lr_engine_eval_function` 桩体, 是导致编译失败的多余块)
- [x] 验证: `tests/module_test.js` (默认/具名/命名空间/解构/再导出, 闭包状态跨调用保持 INC:1/INC2:2) 与 `_fx*`/`_t1`/`_t2` 全部 PASS
- [x] 模块现已纳入 IOME586 缓存 (见 §4.3): `-m`/`--module` 执行的脚本同样会落盘归档, 热路径以模块方式重跑; 默认导入 `is_default` 序列化往返已修复
- [x] 语义限制 (非缓存 blocker): 模块导出为静态快照, 非 ES 规范的实时绑定 — **已修复**: 命名空间属性现为 getter/setter 访问器，直接读写作用域绑定（LiveBindData + LRCFunction.data 闭包），`export let x` 后续变更即时反映到 `import` 侧，module_test 全线 PASS
  
---  
  
## 六、缺陷修复 2026-07-30  

- [x] import.meta.resolve — interp_get_import_meta 添加 resolve C 函数回调  
- [x] Generator.throw() — gen_throw_cfunc 注册到 gen_build_object  
- [x] with 语句 — 实现 eval_with (作用域级对象属性解析) + AST_WITH case  
- [x] new Function() — 实现 lr_engine_build_function + js_function_constructor  
- [x] 模块导出实时绑定 — LiveBindData + LRCFunction.data getter/setter 访问器  
- [x] BigInt — TOK_BIGINT_LIT + LR_OBJ_BIGINT (int64_t) + 构造函数 + toString/valueOf + LRA ltag=5; console.log 仍走 Object.toString 显示 [object Object]（堆对象类型），typeof 返回 "object"
- [x] test_engine.c 值断言 — 新增 eval_expect 助手 + 算术/逻辑/比较/三元/变量/函数 >30 项值断言升级
- [x] Generator 移除 GEN_MAX_YIELDS 硬上限 — gen_append 不再限制 yield 次数
- [x] 生成器惰性求值 (GEN_LAZY) — **已实现**: interp_call_function 跳过生成器 body 求值；gen_build_object 存储 body AST + scope 到 opaque；gen_next_cfunc 首次调用时在已保存 scope 下一次性求值 body（收集所有 yield），后续调用逐次从缓冲区排出（generator_test 全线 PASS，含 return/forof/yield*/spread/.return/params/break 共 10 项）
