// Debug: Atomics JIT detection test with function wrapper
function testAtomics() {
    var buf = new Int32Array(16);
    Atomics.store(buf, 0, 99);

    var t = Date.now();
    var r = 0;
    for (var i = 0; i < 100000; i++) {
        r = Atomics.load(buf, 0);
    }
    return (Date.now() - t) + ' ms, result=' + r;
}

print(testAtomics());
