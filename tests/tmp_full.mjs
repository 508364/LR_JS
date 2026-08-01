try {
const START = performance.now();
console.log("START:", START);
const T1 = performance.now();
class StressClass1 {
  #private1 = 42;
  static #staticPrivate = 3.14;
  constructor(x) { this.x = x; }
  get computed() { return this.x * 2; }
  set computed(v) { this.x = v / 2; }
}
console.log("class1 ok");
const instances = Array.from({length: 5}, (_,i) => new StressClass1(i));
console.log("instances:", instances.length);
} catch(e) {
 console.log("ERROR:", e?.message || e);
}
