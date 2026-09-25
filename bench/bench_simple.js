// Simple benchmark: runs on LR_JS and prints results for comparison
var t0 = performance.now();
var sum = 0;
for (var i = 0; i < 100000; i++) sum += i;
var t1 = performance.now();
print("loop:", (t1-t0).toFixed(3), "ms, sum=", sum);

function inner(y) { return y * 2; }
function outer(x) { return x + inner(x); }
var r = 0;
for (var i = 0; i < 10000; i++) r = outer(i);
var t2 = performance.now();
print("nested_fns:", (t2-t1).toFixed(3), "ms, result=", r);

var fib_result;
function fib(n) { if (n <= 1) return n; return fib(n-1) + fib(n-2); }
var t3 = performance.now();
fib_result = fib(25);
var t4 = performance.now();
print("fib25:", (t4-t3).toFixed(3), "ms, result=", fib_result);

print("total:", (t4-t0).toFixed(3), "ms");
