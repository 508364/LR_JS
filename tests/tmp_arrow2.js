console.log("start");
class S {
  #p = 42;
  constructor(x) { this.x = x; }
}
console.log("class ok");
var arr = Array.from({length: 2}, (_, i) => new S(i));
console.log("arr:", arr.length);
