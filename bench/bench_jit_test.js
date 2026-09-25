// Test JIT compilation
function inner(y) { return y * 2; }

// Force JIT compilation by calling 5 times with different args
for (var i = 0; i < 5; i++) inner(i);

// Now measure
var t = Date.now();
var r = 0;
for (var i = 0; i < 100000; i++) r = inner(i);
print("inner(100k): " + (Date.now() - t) + " ms, result=" + r);

// Also test what happens with same arg (memo hit)
t = Date.now();
r = 0;
for (var i = 0; i < 100000; i++) r = inner(5);
print("inner(same,100k): " + (Date.now() - t) + " ms, result=" + r);
