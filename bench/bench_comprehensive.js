// 综合性能测试：IOME586 + JIT缓存
// 测试内容：
//   1. IOME586 冷跑（首次执行，无缓存）
//   2. IOME586 热跑（缓存命中，重复执行）
//   3. JIT中间表示缓存验证（冷热对比）

(function() {
    var ENGINE_NAME = 'Unknown';
    var IS_LRJS = false;
    var IS_V8 = false;

    if (typeof process !== 'undefined' && process.versions && process.versions.node) {
        ENGINE_NAME = 'Node.js(V8) v' + process.versions.node;
        IS_V8 = true;
    } else if (typeof lr_js_version !== 'undefined') {
        ENGINE_NAME = 'LR_JS v' + lr_js_version;
        IS_LRJS = true;
    }

    var now;
    if (typeof performance !== 'undefined' && performance.now) {
        now = function() { return performance.now(); };
    } else {
        now = Date.now;
    }

    console.log('╔══════════════════════════════════════════════════════════════╗');
    console.log('║  综合性能测试：IOME586 + JIT缓存                            ║');
    console.log('╚══════════════════════════════════════════════════════════════╝');
    console.log('Engine : ' + ENGINE_NAME);
    console.log('');

    // ========== 测试函数定义 ==========

    // 纯函数（支持IOME586 memoization）
    function pure_fib(n) {
        if (n <= 1) return n;
        return pure_fib(n - 1) + pure_fib(n - 2);
    }

    function pure_factorial(n) {
        if (n <= 1) return 1;
        return n * pure_factorial(n - 1);
    }

    function pure_sum(arr) {
        var s = 0;
        for (var i = 0; i < arr.length; i++) s += arr[i];
        return s;
    }

    // JIT热点函数（用于缓存测试）
    function jit_hot_loop(n) {
        var r = 0;
        while (n > 0) { r = r + 10; n = n - 1; }
        return r;
    }

    function jit_hot_ifwhile(x) {
        var r = 0;
        if (x > 0) { r = 1; }
        while (x > 0) { r = r + 10; x = x - 1; }
        return r;
    }

    function jit_hot_nested(x) {
        function inner(y) { return y * 2; }
        return x + inner(x);
    }

    // ========== 测试运行器 ==========
    var RESULTS = [];

    function bench(name, fn, iterations) {
        // Warmup
        for (var w = 0; w < 5; w++) fn();

        var t0 = now();
        var result = fn();
        for (var i = 1; i < iterations; i++) {
            result = fn();
        }
        var t1 = now();
        var elapsed = t1 - t0;

        RESULTS.push({ name: name, elapsed: elapsed, iterations: iterations, result: result });
        console.log(name + ': ' + elapsed.toFixed(2) + ' ms (' + iterations + ' iters, result=' + result + ')');
        return { name: name, elapsed: elapsed, iterations: iterations, result: result };
    }

    // ========== PART 1: IOME586 冷跑 ==========
    console.log('═══════════════════════════════════════════════════════════════');
    console.log(' PART 1: IOME586 COLD RUN (首次执行，无缓存)');
    console.log('═══════════════════════════════════════════════════════════════');

    var COLD_ITERS = IS_LRJS ? 50 : 500;

    bench('cold_fib20', function() { return pure_fib(20); }, COLD_ITERS);
    bench('cold_factorial', function() { return pure_factorial(15); }, COLD_ITERS);
    bench('cold_sum', function() {
        var a = []; for (var i = 0; i < 100; i++) a.push(i);
        return pure_sum(a);
    }, COLD_ITERS);

    if (IS_LRJS) {
        var ms1 = bc_memo_stats();
        console.log('IOME586 memo hits=' + ms1.hits + ' misses=' + ms1.misses);
    }

    // ========== PART 2: IOME586 热跑 ==========
    console.log('');
    console.log('═══════════════════════════════════════════════════════════════');
    console.log(' PART 2: IOME586 HOT RUN (缓存命中，重复执行)');
    console.log('═══════════════════════════════════════════════════════════════');

    // Pre-warm memo cache
    for (var pw = 0; pw < 20; pw++) {
        pure_fib(20);
        pure_factorial(15);
        var wa = []; for (var wi = 0; wi < 100; wi++) wa.push(wi);
        pure_sum(wa);
    }

    var HOT_ITERS = IS_LRJS ? 200 : 2000;

    bench('hot_fib20', function() { return pure_fib(20); }, HOT_ITERS);
    bench('hot_factorial', function() { return pure_factorial(15); }, HOT_ITERS);
    bench('hot_sum', function() {
        var a = []; for (var i = 0; i < 100; i++) a.push(i);
        return pure_sum(a);
    }, HOT_ITERS);

    if (IS_LRJS) {
        var ms2 = bc_memo_stats();
        console.log('IOME586 memo hits=' + ms2.hits + ' misses=' + ms2.misses);
        var totalOps = ms2.hits + ms2.misses;
        var hitRate = totalOps > 0 ? ((ms2.hits / totalOps) * 100).toFixed(1) : 'N/A';
        console.log('Hit rate: ' + hitRate + '%');
    }

    // ========== PART 3: JIT缓存性能（冷热对比） ==========
    console.log('');
    console.log('═══════════════════════════════════════════════════════════════');
    console.log(' PART 3: JIT INTERMEDIATE REPRESENTATION CACHE');
    console.log('═══════════════════════════════════════════════════════════════');

    // 冷跑：首次JIT编译
    console.log('');
    console.log('--- JIT Cold Run (首次编译) ---');
    bench('jit_cold_ifwhile', function() { return jit_hot_ifwhile(50); }, 50);
    bench('jit_cold_loop', function() { return jit_hot_loop(50); }, 50);
    bench('jit_cold_nested', function() { return jit_hot_nested(50); }, 50);

    // 热跑：使用缓存
    console.log('');
    console.log('--- JIT Hot Run (缓存命中) ---');
    bench('jit_hot_ifwhile', function() { return jit_hot_ifwhile(50); }, 500);
    bench('jit_hot_loop', function() { return jit_hot_loop(50); }, 500);
    bench('jit_hot_nested', function() { return jit_hot_nested(50); }, 500);

    // 验证正确性
    console.log('');
    console.log('--- Correctness Verification ---');
    console.log('ifwhile(50) = ' + jit_hot_ifwhile(50) + ' (expect 501)');
    console.log('loop(50) = ' + jit_hot_loop(50) + ' (expect 500)');
    console.log('nested(50) = ' + jit_hot_nested(50) + ' (expect 150)');

    // ========== 结果汇总 ==========
    console.log('');
    console.log('═══════════════════════════════════════════════════════════════');
    console.log(' BENCHMARK SUMMARY');
    console.log('═══════════════════════════════════════════════════════════════');

    console.log('');
    console.log('  %-25s | %8s | %8s | %10s', 'Benchmark', 'ms', 'iters', 'μs/iter');
    console.log('  ' + '─'.repeat(60));

    for (var i = 0; i < RESULTS.length; i++) {
        var r = RESULTS[i];
        var usPerIter = r.iterations > 0 && r.elapsed > 0
            ? (r.elapsed / r.iterations * 1000).toFixed(3) : 'N/A';
        console.log('  %-25s | %8.2f | %8d | %10s',
            r.name, r.elapsed, r.iterations, usPerIter + ' μs');
    }

    console.log('');
    console.log(ENGINE_NAME + ': comprehensive benchmark done.');
})();
