// Debug: Force JIT compilation by calling function multiple times
function testAtomicsOnce() {
    var buf = new Int32Array(16);
    Atomics.store(buf, 0, 99);
    return Atomics.load(buf, 0);
}

// Call 5 times to trigger JIT (threshold is 3)
var r1 = testAtomicsOnce();
var r2 = testAtomicsOnce();
var r3 = testAtomicsOnce();
var r4 = testAtomicsOnce();
var r5 = testAtomicsOnce();

print('warmup done, results: ' + r1 + ' ' + r2 + ' ' + r3 + ' ' + r4 + ' ' + r5);

// Now time the JIT-compiled version.
// NOTE: keep the hot function free of string concatenation / Date.now()
// so the MIR frontend does NOT bail out and the whole body gets JIT'd.
function testAtomicsBench() {
    var buf = new Int32Array(16);
    Atomics.store(buf, 0, 99);
    var r = 0;
    for (var i = 0; i < 100000; i++) {
        r = Atomics.load(buf, 0);
    }
    return r;
}

// Call 5 times to trigger JIT on bench function
var x1 = testAtomicsBench();
var x2 = testAtomicsBench();
var x3 = testAtomicsBench();
var x4 = testAtomicsBench();
var x5 = testAtomicsBench();

print('bench results: ' + x1 + ' ' + x2 + ' ' + x3 + ' ' + x4 + ' ' + x5);
