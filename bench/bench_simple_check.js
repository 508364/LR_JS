// Check if simple loop is affected
function identity(x) { return x; }
var sum = 0;
for (var i = 0; i < 1000000; i++) sum += identity(i);
console.log("identity_loop:", sum);

// Check simple int32 loop
sum = 0;
for (var i = 0; i < 1000000; i++) sum += i;
console.log("simple_loop:", sum);
