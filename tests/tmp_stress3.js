console.log("start");
class StressClass1 {
  #private1 = 42;
  static #staticPrivate = 3.14;
  constructor(x) { this.x = x; }
  get computed() { return this.x * 2; }
  set computed(v) { this.x = v / 2; }
  *gen() { yield 1; yield 2; }
  async am() { return await Promise.resolve(42); }
  static sm() { return StressClass1.#staticPrivate; }
}
console.log("class defined");
console.log("static sm:", StressClass1.sm());
var o = new StressClass1(10);
console.log("computed:", o.computed);
o.computed = 40;
console.log("x:", o.x);
console.log("gen:", [...o.gen()].join(","));
