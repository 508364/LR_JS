// Pure Atomics hot loop - no string/Date so JIT can compile it.
// Warmup: call 4x to ensure JIT compiles (LR_JIT_HOT_FUNC_CALLS=3).
function atomics_loop(iter) {
    var buf = new Int32Array(16);
    Atomics.store(buf, 0, 99);
    var r = 0;
    for (var i = 0; i < iter; i++) {
        r = Atomics.load(buf, 0);
    }
    return r;
}

// Warmup (JIT threshold is 3)
var w1 = atomics_loop(10);
var w2 = atomics_loop(10);
var w3 = atomics_loop(10);
var w4 = atomics_loop(10);
print('warmup: ' + w1 + ' ' + w2 + ' ' + w3 + ' ' + w4);

// Timed runs on JIT-compiled version
var t0 = Date.now();
var r1 = atomics_loop(1000000);
var t1 = Date.now();
var r2 = atomics_loop(1000000);
var t2 = Date.now();
var r3 = atomics_loop(1000000);
var t3 = Date.now();

print('run1 ' + (t1 - t0) + ' ms result=' + r1);
print('run2 ' + (t2 - t1) + ' ms result=' + r2);
print('run3 ' + (t3 - t2) + ' ms result=' + r3);
