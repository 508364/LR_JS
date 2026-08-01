console.log("start");
class StressClass1 {
  #private1 = 42;
  #private2 = "secret";
  static #staticPrivate = 3.14;
  static staticField = "static";
  field1 = 100;
  field2 = { nested: { deep: [1, 2, 3, 4, 5] } };
  constructor(x) { this.x = x; }
  get computed() { return this.x * 2; }
  set computed(v) { this.x = v / 2; }
  #privateMethod() { return this.#private1; }
  publicMethod() { return this.#privateMethod() + this.field1; }
  *generatorMethod() { yield 1; yield 2; yield 3; }
  async asyncMethod() { return await Promise.resolve(42); }
  static staticMethod() { return StressClass1.#staticPrivate; }
}
console.log("class1 ok");
class StressClass2 extends StressClass1 {
  #extra = "extra";
  constructor(x, y) { super(x); this.y = y; }
  overrideMethod() { return super.publicMethod() + this.y; }
}
console.log("class2 ok");
class StressClass3 {
  static { console.log("Static block"); }
  field = [1,2,3].map(x => x*2);
}
console.log("class3 ok");
var arr = Array.from({length: 100}, (_,i) => new StressClass2(i,i*2));
console.log("instances:", arr.length);
