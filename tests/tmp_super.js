console.log("start");
class P {
  m() { return 42; }
}
class C extends P {
  n() { return super.m(); }
}
var o = new C();
console.log("super.m:", o.n());
