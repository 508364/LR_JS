// Test callee inlining and method cache
function add(a, b) { return a + b; }
function multiply(a, b) { return a * b; }

// Warm up JIT for add
for (var i = 0; i < 5; i++) add(i, i+1);

// Measure inline_call
var t = Date.now();
var r = 0;
for (var i = 0; i < 100000; i++) r = add(i, i+1);
print("inline_add(100k): " + (Date.now() - t) + "ms result=" + r);

// Test method cache with class
class Counter {
    constructor(start) { this.val = start; }
    inc() { this.val++; return this.val; }
    getVal() { return this.val; }
}

var c = new Counter(0);
for (var i = 0; i < 5; i++) c.inc();

t = Date.now();
r = 0;
for (var i = 0; i < 100000; i++) r = c.inc();
print("method_cache_inc(100k): " + (Date.now() - t) + "ms result=" + r);

t = Date.now();
r = 0;
for (var i = 0; i < 100000; i++) r = c.getVal();
print("method_cache_get(100k): " + (Date.now() - t) + "ms result=" + r);
