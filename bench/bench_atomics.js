// Atomics JIT performance test
const ITERS = 200000;

// Test 1: Atomics.load
{
    const buf = new Int32Array(16);
    Atomics.store(buf, 0, 42);

    let t = Date.now();
    let r = 0;
    for (let i = 0; i < ITERS; i++) {
        r = Atomics.load(buf, 0);
    }
    print('Atomics.load ' + ITERS + ': ' + (Date.now() - t) + ' ms, result=' + r);
}

// Test 2: Atomics.store
{
    const buf2 = new Int32Array(16);

    let t = Date.now();
    for (let i = 0; i < ITERS; i++) {
        Atomics.store(buf2, 0, i & 0xFF);
    }
    print('Atomics.store ' + ITERS + ': ' + (Date.now() - t) + ' ms');
}

// Test 3: Atomics.add
{
    const buf3 = new Int32Array(16);
    Atomics.store(buf3, 0, 0);

    let t = Date.now();
    for (let i = 0; i < ITERS; i++) {
        Atomics.add(buf3, 0, 1);
    }
    print('Atomics.add ' + ITERS + ': ' + (Date.now() - t) + ' ms, result=' + Atomics.load(buf3, 0));
}

// Test 4: Atomics.isLockFree
{
    let t = Date.now();
    let r = true;
    for (let i = 0; i < ITERS; i++) {
        r = Atomics.isLockFree(4);
    }
    print('Atomics.isLockFree ' + ITERS + ': ' + (Date.now() - t) + ' ms, result=' + r);
}
