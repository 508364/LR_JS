/* Class & object-literal feature regression test */

/* 1. constructor + new */
class A {
    constructor(v) { this.v = v; }
    m() { return this.v + 1; }
}
var a = new A(41);
console.log("ctor:", a.v);            /* 41 */
console.log("m:", a.m());             /* 42 */
console.log("instanceof:", a instanceof A); /* true */

/* 2. static methods & fields */
class B {
    static f() { return 9; }
    static count = 7;
}
console.log("static f:", B.f());      /* 9 */
console.log("static field:", B.count);/* 7 */

/* 3. getters/setters in class */
class C {
    constructor() { this._x = 1; }
    get x() { return this._x * 10; }
    set x(v) { this._x = v + 1; }
}
var c = new C();
console.log("get:", c.x);             /* 10 */
c.x = 4;
console.log("set+get:", c.x);         /* 50 */

/* 4. instance fields */
class D {
    a = 5;
    b = this.a * 2;
}
var d = new D();
console.log("fields:", d.a, d.b);     /* 5 10 */

/* 5. extends + super */
class Base {
    constructor(n) { this.n = n; }
    hello() { return "base-" + this.n; }
    static sm() { return "S"; }
}
class Derived extends Base {
    constructor() { super(3); this.extra = 1; }
    hello() { return "derived:" + super.hello(); }
}
var e = new Derived();
console.log("super ctor:", e.n, e.extra);   /* 3 1 */
console.log("super method:", e.hello());    /* derived:base-3 */
console.log("static inherit:", Derived.sm());/* S */

/* 6. implicit constructor in derived class */
class Derived2 extends Base {}
var e2 = new Derived2(8);
console.log("implicit super:", e2.n);        /* 8 */
console.log("proto chain:", e2 instanceof Base, e2 instanceof Derived2); /* true true */

/* 7. object literal getters/setters */
var o = {
    _v: 2,
    get x() { return 7; },
    get v() { return this._v; },
    set v(nv) { this._v = nv * 3; }
};
console.log("obj get:", o.x);          /* 7 */
o.v = 5;
console.log("obj set+get:", o.v);      /* 15 */

/* 8. get/set as ordinary names still work */
var p = { get: 1, set: 2, static: 3 };
console.log("plain names:", p.get, p.set, p.static); /* 1 2 3 */

/* 9. method shorthand + computed keys in class */
var key = "dyn";
class E {
    ["c" + "omp"]() { return "computed"; }
    static [key]() { return "sdyn"; }
}
var ee = new E();
console.log("computed method:", ee.comp());  /* computed */
console.log("computed static:", E.dyn());    /* sdyn */

/* 10. new on plain function */
function F(v) { this.w = v; }
var f = new F(6);
console.log("func new:", f.w, f instanceof F); /* 6 true */

/* 11. semicolons in class body */
class G { ; m() { return 1; } ; }
console.log("semi class:", new G().m());     /* 1 */

console.log("ALL CLASS TESTS DONE");
