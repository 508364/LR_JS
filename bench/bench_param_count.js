// Test no-scope path (zero params)
function zero_arg() { return 42; }

var t = Date.now();
var r = 0;
for (var i = 0; i < 100000; i++) r = zero_arg();
print((Date.now() - t) + " ms, result=" + r);

// Test one-param function
function one_param(y) { return y * 2; }
t = Date.now();
r = 0;
for (var i = 0; i < 100000; i++) r = one_param(i);
print("one_param: " + (Date.now() - t) + " ms, result=" + r);

// Test two-param function
function two_param(a, b) { return a + b; }
t = Date.now();
r = 0;
for (var i = 0; i < 100000; i++) r = two_param(i, i+1);
print("two_param: " + (Date.now() - t) + " ms, result=" + r);
