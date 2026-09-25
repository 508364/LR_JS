class Point {
    #x = 0;
    #y = 0;
    constructor(x, y) {
        this.#x = x;
        this.#y = y;
    }
    getX() { return this.#x; }
    getTotals() { return this.#x + this.#y; }
}

function bench(n) {
    var p = new Point(1, 2);
    var r = 0;
    for (var i = 0; i < n; i++) {
        r = p.getTotals();
    }
    return r;
}

var w1 = bench(100);
var w2 = bench(100);
var w3 = bench(100);
var w4 = bench(100);
print('warmup: ' + w1 + ' ' + w2 + ' ' + w3 + ' ' + w4);

var t0 = Date.now();
var r1 = bench(100000);
var t1 = Date.now();
print('bench: ' + (t1 - t0) + 'ms result=' + r1);
