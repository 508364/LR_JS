var v = 1;
let l = 2;
const c = 3;
class C {}
var found = [];
for (var k in globalThis) {
    if (k === "topVar" || k === "topLet" || k === "topConst" || k === "TopClass" ||
        k === "v" || k === "l" || k === "c" || k === "C") {
        found.push(k);
    }
}
console.log("enumerated:", JSON.stringify(found));
console.log("globalThis.v is", globalThis.v);
console.log("globalThis.l is", globalThis.l);
console.log("globalThis.c is", globalThis.c);
console.log("typeof globalThis.C is", typeof globalThis.C);
console.log("typeof C bare is", typeof C);
console.log("typeof l bare is", typeof l);
