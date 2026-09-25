// Shape cache benchmark - heavier to trigger JIT compilation
const ITERATIONS = 10000;

function testShapeAccess(n) {
    var obj = { x: 1, y: 2, z: 3 };
    var sum = 0;
    for (var i = 0; i < n; i++) {
        sum += obj.x + obj.y + obj.z;
    }
    return sum;
}

function testArrayLike(n) {
    var arr = { length: 100, data: [1, 2, 3, 4, 5] };
    var sum = 0;
    for (var i = 0; i < n; i++) {
        sum += arr.length + arr.data[0] + arr.data[1];
    }
    return sum;
}

function Point() {
    this.x = 1;
    this.y = 2;
}

function testClassAccess(n) {
    var p = new Point();
    var sum = 0;
    for (var i = 0; i < n; i++) {
        sum += p.x + p.y;
    }
    return sum;
}

// Warmup
for (var w = 0; w < 200; w++) {
    testShapeAccess(50);
    testArrayLike(50);
    testClassAccess(50);
}

console.log("=== Shape Cache Benchmark ===");

var t0, t1, elapsed;

t0 = Date.now();
var r1 = testShapeAccess(ITERATIONS);
t1 = Date.now();
elapsed = t1 - t0;
console.log("Test 1 (shape access): " + elapsed + "ms, result=" + r1);

t0 = Date.now();
var r2 = testArrayLike(ITERATIONS);
t1 = Date.now();
elapsed = t1 - t0;
console.log("Test 2 (array-like): " + elapsed + "ms, result=" + r2);

t0 = Date.now();
var r3 = testClassAccess(ITERATIONS);
t1 = Date.now();
elapsed = t1 - t0;
console.log("Test 3 (class access): " + elapsed + "ms, result=" + r3);

console.log("=== Done ===");
