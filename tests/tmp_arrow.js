console.log("A");
class C {
  field = [1,2,3].map(x => x * 2);
}
console.log("B");
var o = new C();
console.log("C:", o.field);
