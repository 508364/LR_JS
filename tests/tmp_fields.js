console.log("start");
class C {
  #a = 1;
  #b = "two";
  static #c = 3.14;
  static d = "static field";
  e = 100;
  f = { x: { y: [1,2,3] } };
  constructor(v) { this.v = v; }
  get g() { return this.v * 2; }
  set g(val) { this.v = val / 2; }
  #h() { return this.#a + this.#b; }
  i() { return this.#h() + this.e; }
}
console.log("class ok");
var o = new C(10);
console.log("v:", o.v, "g:", o.g, "i:", o.i());
