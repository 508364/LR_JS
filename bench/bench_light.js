// 综合性能测试：IOME586 + JIT缓存（精简版，避免崩溃）

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

    console.log('Engine : ' + ENGINE_NAME);
    console.log('');

    // 纯函数
    function pure_fib(n) {
        if (n <= 1) return n;
        return pure_fib(n - 1) + pure_fib(n - 2);
    }

    function pure_factorial(n) {
        if (n <= 1) return 1;
        return n * pure_factorial(n - 1);
    }

    // JIT热点函数
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

    var RESULTS = [];

    function bench(name, fn, iterations) {
        for (var w = 0; w < 3; w++) fn();

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
    console.log('═══ PART 1: IOME586 COLD RUN ═══');
    var COLD_ITERS = IS_LRJS ? 20 : 200;

    bench('cold_factorial', function() { return pure_factorial(15); }, COLD_ITERS);

    if (IS_LRJS) {
        var ms1 = bc_memo_stats();
        console.log('IOME586 memo hits=' + ms1.hits + ' misses=' + ms1.misses);
    }

    // ========== PART 2: IOME586 热跑 ==========
    console.log('');
    console.log('═══ PART 2: IOME586 HOT RUN ═══');

    // Pre-warm
    for (var pw = 0; pw < 10; pw++) {
        pure_factorial(15);
    }

    var HOT_ITERS = IS_LRJS ? 100 : 1000;

    bench('hot_factorial', function() { return pure_factorial(15); }, HOT_ITERS);

    if (IS_LRJS) {
        var ms2 = bc_memo_stats();
        console.log('IOME586 memo hits=' + ms2.hits + ' misses=' + ms2.misses);
        var totalOps = ms2.hits + ms2.misses;
        var hitRate = totalOps > 0 ? ((ms2.hits / totalOps) * 100).toFixed(1) : 'N/A';
        console.log('Hit rate: ' + hitRate + '%');
    }

    // ========== PART 3: JIT缓存 ==========
    console.log('');
    console.log('═══ PART 3: JIT CACHE (Cold vs Hot) ═══');

    console.log('');
    console.log('--- JIT Cold Run ---');
    bench('jit_cold_loop', function() { return jit_hot_loop(50); }, 30);
    bench('jit_cold_ifwhile', function() { return jit_hot_ifwhile(50); }, 30);
    bench('jit_cold_nested', function() { return jit_hot_nested(50); }, 30);

    console.log('');
    console.log('--- JIT Hot Run ---');
    bench('jit_hot_loop', function() { return jit_hot_loop(50); }, 300);
    bench('jit_hot_ifwhile', function() { return jit_hot_ifwhile(50); }, 300);
    bench('jit_hot_nested', function() { return jit_hot_nested(50); }, 300);

    // Correctness
    console.log('');
    console.log('--- Correctness ---');
    console.log('loop(50) = ' + jit_hot_loop(50) + ' (expect 500)');
    console.log('ifwhile(50) = ' + jit_hot_ifwhile(50) + ' (expect 501)');
    console.log('nested(50) = ' + jit_hot_nested(50) + ' (expect 150)');

    // ========== Summary ==========
    console.log('');
    console.log('═══ SUMMARY ═══');
    console.log('');
    console.log('  %-20s | %8s | %8s', 'Benchmark', 'ms', 'μs/iter');
    console.log('  ' + '─'.repeat(45));

    for (var i = 0; i < RESULTS.length; i++) {
        var r = RESULTS[i];
        var usPerIter = r.iterations > 0 && r.elapsed > 0
            ? (r.elapsed / r.iterations * 1000).toFixed(1) : 'N/A';
        console.log('  %-20s | %8.2f | %8s',
            r.name, r.elapsed, usPerIter + ' μs');
    }

    console.log('');
    console.log(ENGINE_NAME + ': benchmark done.');
})();
