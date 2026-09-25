// Benchmark with timing instrumentation to find where time is spent
var call_count = 0;
var total_time = 0;
var inner_time = 0;

function inner(y) { return y * 2; }
function outer(x) {
    call_count++;
    var t1 = Date.now();
    var r = x + inner(x);
    var t2 = Date.now();
    total_time += (t2 - t1);
    inner_time += /* we can't easily measure inner separately */ 0;
    return r;
}

var r = 0;
var t0 = Date.now();
for (var i = 0; i < 10000; i++) r = outer(i);
var t1 = Date.now();

console.log("outer calls:", call_count);
console.log("outer total:", (t1 - t0), "ms");
console.log("result:", r);

// Also test a simpler nested pattern
var simple_count = 0;
function simple_inner(n) { return n + 1; }
function simple_outer(n) { return simple_inner(n) * 2; }

var t2 = Date.now();
for (var i = 0; i < 100000; i++) simple_outer(i);
var t3 = Date.now();
console.log("simple_nested (100k):", (t3 - t2), "ms");
