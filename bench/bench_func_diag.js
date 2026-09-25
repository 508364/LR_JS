// Diagnostic benchmark to find where function call time is spent
var call_count = 0;
var outer_call_count = 0;
var inner_call_count = 0;

function inner(y) { return y * 2; }
function outer(x) {
    outer_call_count++;
    return x + inner(x);
}

var r = 0;
var t0 = Date.now();
for (var i = 0; i < 10000; i++) r = outer(i);
var t1 = Date.now();
console.log("outer:", (t1 - t0), "ms, result=", r, "calls=", outer_call_count);
console.log("inner calls should be:", outer_call_count);

// Test 2: direct inlining equivalent
var t2 = Date.now();
r = 0;
for (var i = 0; i < 10000; i++) r = i + i * 2;
var t3 = Date.now();
console.log("inline equivalent:", (t3 - t2), "ms, result=", r);

// Test 3: simple 1-arg function
var inner2_count = 0;
function inner2(y) { inner2_count++; return y * 2; }
var t4 = Date.now();
for (var i = 0; i < 100000; i++) inner2(i);
var t5 = Date.now();
console.log("simple_1arg (100k):", (t5 - t4), "ms, calls=", inner2_count);

// Test 4: 0-arg function (should be memoized)
var zero_count = 0;
function zero_arg() { zero_count++; return 42; }
var t6 = Date.now();
for (var i = 0; i < 100000; i++) zero_arg();
var t7 = Date.now();
console.log("zero_arg (100k):", (t7 - t6), "ms, calls=", zero_count);
