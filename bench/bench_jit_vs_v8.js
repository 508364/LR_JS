// JIT性能对比测试：LR_JS (JIT) vs V8
// 测试覆盖了if-while组合、循环、函数调用等JIT热点模式

const ITERATIONS = 10000;
const WARMUP = 1000;

console.log("=== JIT 性能对比测试 ===");
console.log(`LR_JS JIT vs V8 (Node.js)`);
console.log(`迭代次数: ${ITERATIONS}, 预热: ${WARMUP}\n`);

// ========== 测试1: If-While组合（本轮修复的模式） ==========
function testIfWhile(x) {
    var r = 0;
    if (x > 0) { r = 1; }
    while (x > 0) { r = r + 10; x = x - 1; }
    return r;
}

(function test1() {
    // Warmup
    for (let i = 0; i < WARMUP; i++) {
        testIfWhile(3);
    }

    const t = Date.now();
    let result = 0;
    for (let i = 0; i < ITERATIONS; i++) {
        result = testIfWhile(3);
    }
    const elapsed = Date.now() - t;

    console.log("[Test 1] If-While组合");
    console.log(`  结果: ${result} (期望: 31)`);
    console.log(`  耗时: ${elapsed}ms`);
    console.log(`  平均: ${(elapsed / ITERATIONS * 1000).toFixed(3)}μs/call\n`);
})();

// ========== 测试2: Pure Loop ==========
function testLoop(n) {
    var r = 0;
    while (n > 0) { r = r + 10; n = n - 1; }
    return r;
}

(function test2() {
    for (let i = 0; i < WARMUP; i++) {
        testLoop(3);
    }

    const t = Date.now();
    let result = 0;
    for (let i = 0; i < ITERATIONS; i++) {
        result = testLoop(3);
    }
    const elapsed = Date.now() - t;

    console.log("[Test 2] Pure Loop");
    console.log(`  结果: ${result} (期望: 30)`);
    console.log(`  耗时: ${elapsed}ms`);
    console.log(`  平均: ${(elapsed / ITERATIONS * 1000).toFixed(3)}μs/call\n`);
})();

// ========== 测试3: Nested Function Calls ==========
function inner(y) { return y * 2; }
function outer(x) { return x + inner(x); }

(function test3() {
    let result = 0;
    for (let i = 0; i < WARMUP; i++) {
        result = outer(result);
    }

    const t = Date.now();
    result = 0;
    for (let i = 0; i < ITERATIONS; i++) {
        result = outer(result);
    }
    const elapsed = Date.now() - t;

    console.log("[Test 3] Nested Functions");
    console.log(`  结果: ${result} (期望: 0)`);
    console.log(`  耗时: ${elapsed}ms`);
    console.log(`  平均: ${(elapsed / ITERATIONS * 1000).toFixed(3)}μs/call\n`);
})();

// ========== 测试4: Simple Loop ==========
(function test4() {
    let sum = 0;
    for (let i = 0; i < WARMUP * 100; i++) sum += i;

    const t = Date.now();
    sum = 0;
    for (let i = 0; i < ITERATIONS; i++) {
        for (let j = 0; j < 100; j++) sum += j;
    }
    const elapsed = Date.now() - t;

    console.log("[Test 4] Simple Loop (100k ops)");
    console.log(`  结果: ${sum}`);
    console.log(`  耗时: ${elapsed}ms`);
    console.log(`  平均: ${(elapsed / ITERATIONS * 1000).toFixed(3)}μs/call\n`);
})();

// ========== 测试5: Factorial ==========
function factorial(n) {
    let result = 1;
    for (let i = 2; i <= n; i++) result *= i;
    return result;
}

(function test5() {
    for (let i = 0; i < WARMUP; i++) {
        factorial(15);
    }

    const t = Date.now();
    let result = 0;
    for (let i = 0; i < ITERATIONS; i++) {
        result = factorial(15);
    }
    const elapsed = Date.now() - t;

    console.log("[Test 5] Factorial(15)");
    console.log(`  结果: ${result} (期望: 1307674368000)`);
    console.log(`  耗时: ${elapsed}ms`);
    console.log(`  平均: ${(elapsed / ITERATIONS * 1000).toFixed(3)}μs/call\n`);
})();

console.log("=== 测试完成 ===");
