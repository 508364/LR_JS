console.log("L1");
class C1 {
  #p = 1;
  constructor(x) { this.x = x; }
}
console.log("L2");
class C2 extends C1 {
  constructor(x, y) { super(x); this.y = y; }
}
console.log("L3");
class C3 { static { console.log("static3"); } }
console.log("L4");
var arr = Array.from({length: 2}, (_,i) => new C2(i,i*2));
console.log("L5:", arr.length);
