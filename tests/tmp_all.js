console.log("A");
class C {
  #pm() { return 1; }         // private method
  m() { return this.#pm(); }  // public calling private
  *gen() { yield 1; }         // generator
  async as() { return 2; }    // async
  static sm() { return 3; }   // static
}
console.log("B");
var o = new C();
console.log("m:", o.m());
console.log("sm:", C.sm());
