// Simple benchmark compatible with LR_JS, V8 (Node.js), and QuickJS
var t0 = Date.now();
var sum = 0;
for (var i = 0; i < 100000; i++) sum += i;
var t1 = Date.now();
(typeof console !== 'undefined' ? console.log : print)("loop:", (t1-t0), "ms, sum=", sum);

function inner(y) { return y * 2; }
function outer(x) { return x + inner(x); }
var r = 0;
for (var i = 0; i < 10000; i++) r = outer(i);
var t2 = Date.now();
(typeof console !== 'undefined' ? console.log : print)("nested_fns:", (t2-t1), "ms, result=", r);

var fib_result;
function fib(n) { if (n <= 1) return n; return fib(n-1) + fib(n-2); }
var t3 = Date.now();
fib_result = fib(25);
var t4 = Date.now();
(typeof console !== 'undefined' ? console.log : print)("fib25:", (t4-t3), "ms, result=", fib_result);

(typeof console !== 'undefined' ? console.log : print)("total:", (t4-t0), "ms");
