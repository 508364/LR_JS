class Counter {
    #val = 0;
    constructor() { this.#val = 0; }
    add(n) {
        for (var i = 0; i < n; i++) this.#val++;
        return this.#val;
    }
}

function bench(n) {
    var c = new Counter();
    for (var i = 0; i < n; i++) {
        c.add(100);
    }
    return c.add(0);
}

var w1 = bench(100);
var w2 = bench(100);
var w3 = bench(100);
var w4 = bench(100);
print('warmup: ' + w1 + ' ' + w2 + ' ' + w3 + ' ' + w4);

var t0 = Date.now();
var r1 = bench(10000);
var t1 = Date.now();
print('bench: ' + (t1 - t0) + 'ms result=' + r1);
