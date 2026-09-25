// Simplified benchmark to isolate function call overhead
function inner(y) { return y * 2; }
function outer(x) { return x + inner(x); }

// Warm up JIT
for (var i = 0; i < 100; i++) outer(i);

var r = 0;
var t0 = Date.now();
for (var i = 0; i < 10000; i++) r = outer(i);
var t1 = Date.now();
console.log("nested_fns:", (t1-t0), "ms, result=", r);

// Test simple loop
sum = 0;
t0 = Date.now();
for (var i = 0; i < 100000; i++) sum += i;
t1 = Date.now();
console.log("loop:", (t1-t0), "ms, sum=", sum);
