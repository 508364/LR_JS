// Comprehensive float64 JIT test
var t0 = Date.now();

// Test 1: Basic f64 arithmetic
var sum = 0;
for (var i = 0; i < 1000000; i++) sum += (i * 1.5 + i * 0.5);
var t1 = Date.now();
console.log("f64_arith_loop:", (t1 - t0), "ms, sum=", sum);

// Test 2: f64 comparisons
var count = 0;
for (var i = 0; i < 1000000; i++) {
    if ((i * 1.1) > (i * 0.9)) count++;
}
var t2 = Date.now();
console.log("f64_cmp_loop:", (t2 - t1), "ms, count=", count);

// Test 3: f64 negation
var neg = 0;
for (var i = 0; i < 1000000; i++) neg -= i * 1.0;
var t3 = Date.now();
console.log("f64_neg_loop:", (t3 - t2), "ms, neg=", neg);

// Test 4: Mixed int32/f64 arithmetic
var mixed = 0;
for (var i = 0; i < 1000000; i++) mixed += i + 1.5;
var t4 = Date.now();
console.log("f64_mixed_loop:", (t4 - t3), "ms, mixed=", mixed);

// Test 5: f64 equality
var eq_count = 0;
for (var i = 0; i < 1000000; i++) {
    if ((i * 1.0) === (i + 0.0)) eq_count++;
}
var t5 = Date.now();
console.log("f64_eq_loop:", (t5 - t4), "ms, eq_count=", eq_count);

// Test 6: Large float values
var big = 1e100;
for (var i = 0; i < 100000; i++) big *= 2.0;
var t6 = Date.now();
console.log("f64_big_loop:", (t6 - t5), "ms, big=", big);

// Test 7: Function with f64 params
function f64_add(a, b) { return a + b; }
var fn_sum = 0;
for (var i = 0; i < 100000; i++) fn_sum += f64_add(i * 1.5, i * 0.5);
var t7 = Date.now();
console.log("f64_fn_loop:", (t7 - t6), "ms, fn_sum=", fn_sum);

console.log("total:", (t7 - t0), "ms");
