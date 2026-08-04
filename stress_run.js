const START = Date.now();
let t;

// 1: Class deep inheritance (5000 instances)
t = Date.now();
class Base { constructor(v) { this.base = v; } }
class L1 extends Base { #p1 = 1; constructor(v) { super(v); this.l1 = v + 1; } }
class L2 extends L1 { #p2 = 2; constructor(v) { super(v); this.l2 = v + 2; } }
class L3 extends L2 { constructor(v) { super(v); this.l3 = v + 3; } }
class L4 extends L3 { constructor(v) { super(v); this.l4 = v + 4; } }
class L5 extends L4 { constructor(v) { super(v); this.l5 = v + 5; } }
class L6 extends L5 { constructor(v) { super(v); this.l6 = v + 6; } }
class L7 extends L6 { constructor(v) { super(v); this.l7 = v + 7; } }
class L8 extends L7 { constructor(v) { super(v); this.l8 = v + 8; } }
class L9 extends L8 { constructor(v) { super(v); this.l9 = v + 9; } }
class L10 extends L9 { static { } constructor(v) { super(v); this.l10 = v + 10; } }
let a1 = [];
for (let i = 0; i < 5000; i++) a1.push(new L10(i));
console.log("[1] Class 5000x10deep:", Date.now() - t, "ms");

// 2: Map/Set (5000 entries)
t = Date.now();
let bigMap = new Map();
for (let i = 0; i < 5000; i++) bigMap.set("k" + i, { id: i });
let bigSet = new Set();
for (let i = 0; i < 5000; i++) bigSet.add("s" + i);
console.log("[2] Map/Set 5000:", Date.now() - t, "ms, size:", bigMap.size, bigSet.size);

// 3: Closures (5000)
t = Date.now();
let closures = [];
for (let i = 0; i < 5000; i++) {
  let x = i % 100 + 1;
  closures.push(() => x * 2);
}
let cs = 0;
for (let i = 0; i < 1000; i++) cs += closures[i % 5000]();
console.log("[3] Closure 5000:", Date.now() - t, "ms, sum =", cs);

// 4: new Function + recursion
t = Date.now();
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }
let fr = fact(200);
console.log("[4] Recursion fact(200):", Date.now() - t, "ms, result =", fr % 10000);

// 5: Destructuring
t = Date.now();
let data = [];
for (let i = 0; i < 2000; i++) data.push({ x: i, y: i * 2, z: i * 3 });
let sum = 0;
for (let i = 0; i < 2000; i++) {
  let { x, y, z } = data[i];
  sum += x + y + z;
}
console.log("[5] Destructuring 2000:", Date.now() - t, "ms, sum =", sum);

// 6: Regex
t = Date.now();
let re = /\d{4}/;
let matches = 0;
for (let i = 0; i < 500; i++) {
  let s = "test-" + i + "-2026";
  if (re.exec(s)) matches++;
}
console.log("[6] Regex 500:", Date.now() - t, "ms, matches =", matches);

// 7: Try/catch
t = Date.now();
for (let i = 0; i < 50; i++) {
  try {
    (function a() { (function b() { (function c() {
      (function d() { (function e() { throw new Error("X" + i); })(); })();
    })(); })(); })();
  } catch(e) { /* swallow */ }
}
console.log("[7] Try/catch 50x5deep:", Date.now() - t, "ms");

// 8: Array operations
t = Date.now();
let arr = [];
for (let i = 0; i < 100000; i++) arr.push(i);
let s = arr.reduce((a, b) => a + b, 0);
console.log("[8] Array 100k:", Date.now() - t, "ms, sum =", s);

// 9: Arrow functions
t = Date.now();
let fn = (n = 1) => n * 2;
let r = 0;
for (let i = 0; i < 10000; i++) r += fn(i % 3 === 0 ? undefined : i);
console.log("[9] Arrow 10k:", Date.now() - t, "ms, r =", r);

// 10: Template literals
t = Date.now();
let str = "";
for (let i = 0; i < 10000; i++) str += "item" + i;
console.log("[10] String concat 10k:", Date.now() - t, "ms, len =", str.length);

console.log("\nTotal:", Date.now() - START, "ms");
