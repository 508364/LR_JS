const START = performance.now();

// ============================================================
// 测试1: 大量类继承链 + 私有字段 + 静态块 (5000 个类实例)
// ============================================================
const T1 = performance.now();
const CLASS_COUNT = 5000;
const INHERIT_DEPTH = 10;

// 构建深层继承链
class Base { constructor(v) { this.base = v; } baseMethod() { return this.base * 2; } }
class L1 extends Base { #p1 = 1; constructor(v) { super(v); this.l1 = v + 1; } }
class L2 extends L1 { #p2 = 2; constructor(v) { super(v); this.l2 = v + 2; } }
class L3 extends L2 { #p3 = 3; constructor(v) { super(v); this.l3 = v + 3; } }
class L4 extends L3 { #p4 = 4; constructor(v) { super(v); this.l4 = v + 4; } }
class L5 extends L4 { #p5 = 5; constructor(v) { super(v); this.l5 = v + 5; } }
class L6 extends L5 { #p6 = 6; constructor(v) { super(v); this.l6 = v + 6; } }
class L7 extends L6 { #p7 = 7; constructor(v) { super(v); this.l7 = v + 7; } }
class L8 extends L7 { #p8 = 8; constructor(v) { super(v); this.l8 = v + 8; } }
class L9 extends L8 { #p9 = 9; constructor(v) { super(v); this.l9 = v + 9; } }
class L10 extends L9 { #p10 = 10; static { /* 静态块 */ } constructor(v) { super(v); this.l10 = v + 10; } }

const classInstances = [];
for (let i = 0; i < CLASS_COUNT; i++) {
  classInstances.push(new L10(i));
}
const T1_END = performance.now();

// ============================================================
// 测试2: 大型全局数据结构 (5000 个 Map/Set 条目 + 深层嵌套)
// ============================================================
const T2 = performance.now();
const MAP_SIZE = 5000;
const SET_SIZE = 5000;
const NESTED_DEPTH = 20;

function buildNested(depth) {
  let obj = { value: depth };
  for (let i = depth - 1; i >= 0; i--) {
    obj = { value: i, child: obj };
  }
  return obj;
}

const bigMap = new Map();
for (let i = 0; i < MAP_SIZE; i++) {
  bigMap.set(`key-${i}`, { 
    id: i, 
    payload: 'x'.repeat(100 + i % 50), 
    nested: buildNested(NESTED_DEPTH),
    meta: Array.from({ length: 20 }, (_, j) => j * i)
  });
}

const bigSet = new Set();
for (let i = 0; i < SET_SIZE; i++) {
  bigSet.add(`item-${i}-${'abcdefghijklmnopqrstuvwxyz'[i % 26].repeat(10)}`);
}

const bigArray = Array.from({ length: 2000 }, (_, i) => ({
  idx: i,
  sin: Math.sin(i),
  cos: Math.cos(i),
  rand: Math.random(),
  str: 'x'.repeat(i % 200)
}));

globalThis.__HEAVY_DATA__ = { bigMap, bigSet, bigArray, nested: buildNested(NESTED_DEPTH * 2) };
const T2_END = performance.now();

// ============================================================
// 测试3: 大量闭包 + 高阶函数 (5000 个闭包)
// ============================================================
const T3 = performance.now();
const CLOSURE_COUNT = 5000;

function makeComplexClosure(base) {
  let state = base;
  const arr = Array.from({ length: 50 }, (_, i) => i * base);
  return {
    inc: (n = 1) => { state += n; return state; },
    dec: (n = 1) => { state -= n; return state; },
    mul: (n) => state * n,
    div: (n) => state / n,
    getState: () => state,
    getArr: () => [...arr],
    transform: (fn) => arr.map(fn),
    compose: (fn1, fn2) => fn1(fn2(state)),
    memo: (() => { const c = new Map(); return (k) => { if (!c.has(k)) c.set(k, k * state); return c.get(k); }; })()
  };
}

const closures = [];
for (let i = 0; i < CLOSURE_COUNT; i++) {
  closures.push(makeComplexClosure(i % 100));
}

// 执行一些闭包操作
let closureSum = 0;
for (let i = 0; i < 1000; i++) {
  const c = closures[i % CLOSURE_COUNT];
  closureSum += c.inc(i % 5) + c.mul(i % 3) - c.div(i % 7 + 1);
}
const T3_END = performance.now();

// ============================================================
// 测试4: 大量函数定义 + 递归计算 (深度 100)
// ============================================================
const T4 = performance.now();
const FUNC_COUNT = 1000;

// 生成大量不同签名的函数
const funcs = [];
for (let i = 0; i < FUNC_COUNT; i++) {
  const a = i % 7, b = (i * 3) % 11, c = (i * 5) % 13;
  funcs.push(new Function('x', `return (x + ${a}) * ${b} - ${c};`));
  funcs.push(new Function('x', 'y', `return (x * ${a} + y * ${b}) % ${c + 1};`));
}

// 递归深度测试
let depthCounter = 0;
function deepRecurse(n, max) {
  depthCounter++;
  if (n >= max) return n;
  return deepRecurse(n + 1, max);
}
const recurseResult = deepRecurse(0, 200);

// 尾递归风格
function tailRecurse(n, acc = 0) {
  if (n <= 0) return acc;
  return tailRecurse(n - 1, acc + n);
}
const tailResult = tailRecurse(500);
const T4_END = performance.now();

// ============================================================
// 测试5: 大量解构 + 模板 + 可选链 + 逻辑赋值
// ============================================================
const T5 = performance.now();
const DATA_COUNT = 2000;
const dataArray = Array.from({ length: DATA_COUNT }, (_, i) => ({
  a: { b: { c: { d: { e: { f: { g: { h: { i: { j: `deep-${i}` } } } } } } } } },
  arr: Array.from({ length: 50 }, (_, j) => i * j),
  obj: { x: i, y: i * 2, z: i * 3 },
  maybe: i % 3 === 0 ? null : { value: i }
}));

let templateAccum = '';
let chainAccum = 0;
let logicalAccum = 0;
for (let i = 0; i < dataArray.length; i++) {
  const { a: { b: { c: { d: { e: { f: { g: { h: { i: { j } } } } } } } } } } = dataArray[i];
  const [first, second, third, ...rest] = dataArray[i].arr;
  const { x, y, z } = dataArray[i].obj;
  const sum = rest.reduce((a, b) => a + b, 0);
  templateAccum += `[${j}:${x},${y},${z}:${sum}]`;
  const chain = dataArray[i]?.a?.b?.c?.d?.e?.f?.g?.h?.i?.j ?? 'fallback';
  chainAccum += chain === j ? 1 : 0;
  let log = i;
  log ||= 100;
  log &&= 50;
  log ??= 999;
  logicalAccum += log;
}
const T5_END = performance.now();

// ============================================================
// 测试6: 大量正则表达式 + d 标志
// ============================================================
const T6 = performance.now();
const REGEX_COUNT = 100;
const regexPatterns = [];
for (let i = 0; i < REGEX_COUNT; i++) {
  const patterns = [
    `(?<g${i}>\\d{${i % 5 + 1}})`,
    `[a-z]{${i % 10 + 1}}`,
    `(\\w+)(?=\\s+\\1)`,
    `^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$`,
    `(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})`
  ];
  const p = patterns[i % patterns.length];
  regexPatterns.push(new RegExp(p, i % 2 === 0 ? 'd' : 'gd'));
}

let regexMatchCount = 0;
for (let i = 0; i < 500; i++) {
  const testStr = `test-${i}-${'abc'.repeat(i % 20)}-2026-07-30-127.0.0.1`;
  for (const re of regexPatterns) {
    const m = re.exec(testStr);
    if (m) regexMatchCount++;
  }
}
const T6_END = performance.now();

// ============================================================
// 测试7: 生成器 + 迭代器 (深度 500)
// ============================================================
const T7 = performance.now();
function* deepGen(level) {
  if (level <= 0) { yield 'done'; return; }
  yield `L${level}`;
  yield* deepGen(level - 1);
}

function* complexGen(max) {
  let i = 0;
  while (i < max) {
    const v = yield i;
    if (v !== undefined) {
      i += v;
    } else {
      i++;
    }
  }
}

const genResults = [];
const g = deepGen(500);
for (const val of g) { genResults.push(val); }

const genResults2 = [];
const g2 = complexGen(1000);
for (let i = 0; i < 500; i++) {
  const r = g2.next(i % 7 === 0 ? 3 : undefined);
  if (!r.done) genResults2.push(r.value);
}
const T7_END = performance.now();

// ============================================================
// 测试8: 错误 + 堆栈跟踪 (10 层嵌套)
// ============================================================
const T8 = performance.now();
let stackLines = 0;
for (let iter = 0; iter < 50; iter++) {
  try {
    (function l1() {
      (function l2() {
        (function l3() {
          (function l4() {
            (function l5() {
              (function l6() {
                (function l7() {
                  (function l8() {
                    (function l9() {
                      (function l10() {
                        throw new Error(`Stack depth test ${iter}`);
                      })();
                    })();
                  })();
                })();
              })();
            })();
          })();
        })();
      })();
    })();
  } catch (e) {
    stackLines += (e.stack || '').split('\n').length;
  }
}
const T8_END = performance.now();

// ============================================================
// 测试9: async/await + 大量 Promise (1000 个)
// ============================================================
const T9 = performance.now();
let asyncAccum = 0;
const asyncPromises = Array.from({ length: 1000 }, (_, i) => 
  Promise.resolve(i).then(v => v * v).then(v => v % 100)
);
const asyncResults = await Promise.all(asyncPromises);
asyncAccum = asyncResults.reduce((a, b) => a + b, 0);

// 额外的 Promise 链
const chainResults = await Promise.all(
  Array.from({ length: 200 }, (_, i) => 
    Promise.resolve(i)
      .then(v => v + 10)
      .then(v => v * 2)
      .then(v => v - 5)
      .catch(() => 0)
  )
);
const T9_END = performance.now();

// ============================================================
// 测试10: SharedArrayBuffer + Atomics (大量操作)
// ============================================================
const T10 = performance.now();
const SAB_SIZE = 65536;
const sab2 = new SharedArrayBuffer(SAB_SIZE);
const view2 = new Int32Array(sab2);
const view8 = new Uint8Array(sab2);

// 填充数据
for (let i = 0; i < view2.length; i++) {
  view2[i] = i % 1000;
  view8[i] = i % 256;
}

// Atomics 压力
let atomicsOps = 0;
for (let i = 0; i < 2000; i++) {
  const idx = i % (view2.length - 1);
  Atomics.add(view2, idx, 1);
  Atomics.sub(view2, idx, 1);
  Atomics.compareExchange(view2, idx, view2[idx], view2[idx] + 1);
  Atomics.and(view2, idx, 0xFFFF);
  Atomics.or(view2, idx, 0x0000);
  atomicsOps += 5;
}

// 非原子读验证
let sum = 0;
for (let i = 0; i < 1000; i++) {
  sum += view2[i % view2.length];
}
const T10_END = performance.now();

// ============================================================
// 汇总输出
// ============================================================
const END = performance.now();

console.log('=== 压力测试结果 (重载版) ===');
console.log(`总耗时: ${(END - START).toFixed(3)} ms`);

console.log('\n--- 各测试段耗时 ---');
console.log(`[1] 类/继承/私有/静态 (${CLASS_COUNT}类实例, 深度${INHERIT_DEPTH}):       ${(T1_END - T1).toFixed(3)} ms`);
console.log(`[2] 全局数据结构 (Map ${MAP_SIZE}/Set ${SET_SIZE}/嵌套深度${NESTED_DEPTH}):  ${(T2_END - T2).toFixed(3)} ms`);
console.log(`[3] 闭包/高阶函数 (${CLOSURE_COUNT}个闭包, 1000次操作):                ${(T3_END - T3).toFixed(3)} ms`);
console.log(`[4] 函数定义 (${FUNC_COUNT}个) + 递归(深度200) + 尾递归(500):         ${(T4_END - T4).toFixed(3)} ms`);
console.log(`[5] 解构/模板/可选链 (${DATA_COUNT}条数据):                           ${(T5_END - T5).toFixed(3)} ms`);
console.log(`[6] 正则表达式 (${REGEX_COUNT}个, 500次匹配):                          ${(T6_END - T6).toFixed(3)} ms`);
console.log(`[7] 生成器/迭代器 (深度500 + 1000次迭代):                            ${(T7_END - T7).toFixed(3)} ms`);
console.log(`[8] 异常/堆栈 (50次 × 10层嵌套):                                      ${(T8_END - T8).toFixed(3)} ms`);
console.log(`[9] async/await/Promise (1000+200个):                                 ${(T9_END - T9).toFixed(3)} ms`);
console.log(`[10] SharedArrayBuffer (64KB, 2000次Atomics):                         ${(T10_END - T10).toFixed(3)} ms`);

console.log('\n--- 测试数据摘要 ---');
console.log(`类实例数量: ${classInstances.length}`);
console.log(`Map 条目数: ${bigMap.size}`);
console.log(`Set 条目数: ${bigSet.size}`);
console.log(`数组长度: ${bigArray.length}`);
console.log(`闭包数量: ${closures.length}`);
console.log(`函数数量: ${funcs.length}`);
console.log(`递归结果: ${recurseResult} (预期 200)`);
console.log(`尾递归结果: ${tailResult} (预期 125250)`);
console.log(`生成器产出1: ${genResults.length} (预期 501)`);
console.log(`生成器产出2: ${genResults2.length}`);
console.log(`正则匹配总数: ${regexMatchCount}`);
console.log(`堆栈总行数: ${stackLines}`);
console.log(`异步累加值: ${asyncAccum} (预期 ~49500)`);
console.log(`Atomics 操作数: ${atomicsOps}`);
console.log(`SAB 校验和: ${sum}`);