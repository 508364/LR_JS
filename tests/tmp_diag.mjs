try {
console.log("A");
const START = performance.now();
console.log("B");
class StressClass1 {
  #private1 = 42;
  #private2 = "secret";
  static #staticPrivate = 3.14;
  static staticField = "static";
  field1 = 100;
  field2 = { nested: { deep: [1, 2, 3, 4, 5] } };
  constructor(x) { this.x = x; }
}
console.log("C");
class StressClass2 extends StressClass1 {
  constructor(x, y) { super(x); this.y = y; }
}
console.log("D");
class StressClass3 {
  static { console.log("Static block"); }
  field = [1, 2, 3].map(function(x){return x*2});
}
console.log("E");
var instances = Array.from({length: 3}, function(_,i){return new StressClass2(i,i*2)});
console.log("F:", instances.length);
} catch(e) {
 console.log("ERROR:", e?.message);
}
