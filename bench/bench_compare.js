// Cross-engine benchmark: LR_JS vs V8 vs QuickJS
// Usage: node bench_compare.js
//        .\build-debug\bin\lr_js.exe bench_compare.js
//        .\qjs.exe bench_compare.js

console.log("=== Cross-Engine Benchmark ===");
console.log("LR_JS v0.2.0 | V8 (Node.js) | QuickJS\n");

const results = {};

// Test 1: Simple loop
(function test1() {
    const t = performance.now ? performance.now() : Date.now();
    let sum = 0;
    for (let i = 0; i < 100000; i++) sum += i;
    results.test1 = { ms: (performance.now ? performance.now() : Date.now()) - t, sum };
    console.log(`[1] Simple loop (100k iterations): ${results.test1.ms.toFixed(3)} ms, sum=${results.test1.sum}`);
})();

// Test 2: Nested function calls
(function test2() {
    const t = performance.now ? performance.now() : Date.now();

    function outer(x) {
        function inner(y) { return y * 2; }
        return x + inner(x);
    }

    let result = 0;
    for (let i = 0; i < 10000; i++) result = outer(result);
    results.test2 = { ms: (performance.now ? performance.now() : Date.now()) - t, result };
    console.log(`[2] Nested functions (10k calls): ${results.test2.ms.toFixed(3)} ms, result=${results.test2.result}`);
})();

// Test 3: Arithmetic operations
(function test3() {
    const t = performance.now ? performance.now() : Date.now();
    let result = 0;
    for (let i = 0; i < 100000; i++) {
        result = i + i * 2 - i / 2;
    }
    results.test3 = { ms: (performance.now ? performance.now() : Date.now()) - t, result };
    console.log(`[3] Arithmetic ops (100k): ${results.test3.ms.toFixed(3)} ms, result=${results.test3.result.toFixed(2)}`);
})();

// Test 4: Array operations
(function test4() {
    const t = performance.now ? performance.now() : Date.now();
    const arr = Array.from({ length: 10000 }, (_, i) => i * 2);
    const mapped = arr.map(v => v * 3);
    const filtered = mapped.filter(v => v > 100);
    const reduced = filtered.reduce((a, b) => a + b, 0);
    results.test4 = { ms: (performance.now ? performance.now() : Date.now()) - t, reduced };
    console.log(`[4] Array ops (10k): ${results.test4.ms.toFixed(3)} ms, reduced=${results.test4.reduced}`);
})();

// Test 5: Fibonacci (pure recursive)
(function test5() {
    const t = performance.now ? performance.now() : Date.now();

    function fib(n) {
        if (n <= 1) return n;
        return fib(n - 1) + fib(n - 2);
    }

    const result = fib(25);
    results.test5 = { ms: (performance.now ? performance.now() : Date.now()) - t, result };
    console.log(`[5] Fibonacci(25): ${results.test5.ms.toFixed(3)} ms, result=${results.test5.result}`);
})();

// Test 6: Factorial
(function test6() {
    const t = performance.now ? performance.now() : Date.now();

    function factorial(n) {
        let result = 1;
        for (let i = 2; i <= n; i++) result *= i;
        return result;
    }

    const result = factorial(15);
    results.test6 = { ms: (performance.now ? performance.now() : Date.now()) - t, result };
    console.log(`[6] Factorial(15): ${results.test6.ms.toFixed(3)} ms, result=${results.test6.result}`);
})();

// Summary
console.log("\n=== Summary ===");
console.log(`Total time: ${Object.values(results).reduce((sum, r) => sum + r.ms, 0).toFixed(3)} ms`);
console.log("\nExpected values for verification:");
console.log("  test1.sum = 4999950000");
console.log("  test2.result = 0");
console.log("  test4.reduced = correct sum");
console.log("  test5.result = 75025");
console.log("  test6.result = 1307674368000");
