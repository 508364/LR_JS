/* Edge cases: class expressions, extends Error, accessor inheritance */

/* 1. class expression */
var X = class { m() { return "expr"; } };
var x = new X();
console.log("class expr:", x.m());          /* expr */

/* 2. named class expression */
var Y = class Named { n() { return 5; } };
console.log("named expr:", new Y().n());     /* 5 */

/* 3. extends Error (C-function base) */
class MyError extends Error {
    constructor(msg) {
        super(msg);
        this.code = 42;
    }
}
var err = new MyError("boom");
console.log("err msg:", err.message);        /* boom */
console.log("err code:", err.code);          /* 42 */
console.log("err instanceof:", err instanceof MyError); /* true */

/* 4. accessor inheritance through prototype chain */
class P {
    get val() { return this._v || 0; }
    set val(v) { this._v = v * 2; }
}
class Q extends P {}
var q = new Q();
q.val = 21;
console.log("inherited accessor:", q.val);   /* 42 */

/* 5. getter/setter pair merge order (set before get) */
var om = {
    set z(v) { this._z = v + 1; },
    get z() { return this._z; }
};
om.z = 9;
console.log("merged accessor:", om.z);       /* 10 */

/* 6. methods calling other methods via this */
class R {
    a() { return this.b() + 1; }
    b() { return 10; }
}
console.log("this chain:", new R().a());     /* 11 */

/* 7. constructor return object override */
class S { constructor() { return { custom: true }; } }
console.log("ctor return:", new S().custom); /* true */

/* 8. three-level inheritance */
class L1 { who() { return "L1"; } }
class L2 extends L1 { who() { return "L2>" + super.who(); } }
class L3 extends L2 { who() { return "L3>" + super.who(); } }
console.log("3-level:", new L3().who());     /* L3>L2>L1 */

console.log("ALL EDGE TESTS DONE");
