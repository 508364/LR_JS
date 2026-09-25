// Simple loop test
var t = Date.now();
var s = 0;
for (var i = 0; i < 100000; i++) s += i;
print((Date.now() - t) + " ms, sum=" + s);
