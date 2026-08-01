console.log("start");
class S1 {
  #p1 = 42;
  constructor(x) { this.x = x; }
}
console.log("S1 defined");
var instances = Array.from({length: 3}, (_,i) => new S1(i));
console.log("instances created:", instances.length);
