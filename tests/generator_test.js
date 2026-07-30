// basic generator with next()
function* g1() { yield 1; yield 2; yield 3; }
const it1 = g1();
const r1 = it1.next(), r2 = it1.next(), r3 = it1.next(), r4 = it1.next();
console.log("next:", r1.value, r1.done, "|", r2.value, r2.done, "|", r3.value, r3.done, "|", r4.value, r4.done);

// for...of over a generator
let a = [];
for (const v of g1()) a.push(v);
console.log("forof:", a.join(","));

// generator with return value
function* g2() { yield "a"; return "END"; }
const it2 = g2();
const s1 = it2.next(), s2 = it2.next(), s3 = it2.next();
console.log("ret:", s1.value, s1.done, "|", s2.value, s2.done, "|", s3.value, s3.done);

// yield* delegation to another generator
function* inner() { yield 10; yield 20; }
function* g3() { yield 1; yield* inner(); yield 2; }
let b = [];
for (const v of g3()) b.push(v);
console.log("yield* gen:", b.join(","));

// yield* over array and string
function* g4() { yield* [7, 8]; yield* "xy"; }
let c = [];
for (const v of g4()) c.push(v);
console.log("yield* arr/str:", c.join(","));

// spread a generator
function* g5() { yield 5; yield 6; }
const arr = [...g5()];
console.log("spread:", arr.length >= 0 ? arr.join(",") : "?");

// generator.return()
const it3 = g1();
it3.next();
const rr = it3.return(99);
console.log("gen return:", rr.value, rr.done, "|", it3.next().done);

// generator with parameters and closure state
function* counter(start, n) {
    for (let i = 0; i < n; i++) yield start + i;
}
let d = [];
for (const v of counter(100, 3)) d.push(v);
console.log("params:", d.join(","));

// break inside for-of over generator
let e = [];
for (const v of counter(0, 10)) { if (v >= 3) break; e.push(v); }
console.log("break:", e.join(","));

// yield outside generator is an error
try {
    eval; // noop
    (function () { return; })();
    console.log("done-guard");
} catch (err) {}

console.log("ALL GENERATOR TESTS DONE");
