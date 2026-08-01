console.log("start");
class S1 {
  #p1 = 42;
  constructor(x) { this.x = x; }
  get val() { return this.x * 2; }
  set val(v) { this.x = v / 2; }
  method() { return this.#p1; }
}
console.log("S1 defined");
class S2 extends S1 {
  constructor(x, y) { super(x); this.y = y; }
}
console.log("S2 defined");
var instances = Array.from({length: 3}, (_,i) => new S2(i, i*2));
console.log("instances:", instances.length, instances[0].val);
