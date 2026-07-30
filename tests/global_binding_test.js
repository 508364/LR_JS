// Verifies ES-spec global-object binding for non-module Script top-level
// var/function declarations (and that let/const/class are NOT mirrored).
var topVar = 42;
function topFunc() { return "fn"; }
let topLet = 1;
const topConst = 2;
class TopClass {}

console.log("globalThis.topVar =", globalThis.topVar);
console.log("typeof globalThis.topFunc =", typeof globalThis.topFunc);
console.log("bare topVar === globalThis.topVar ?", topVar === globalThis.topVar);
console.log("bare topFunc === globalThis.topFunc ?", topFunc === globalThis.topFunc);

// let/const/class must NOT be properties of the global object
console.log("'topLet' in globalThis ?", "topLet" in globalThis);
console.log("'topConst' in globalThis ?", "topConst" in globalThis);
console.log("'TopClass' in globalThis ?", "TopClass" in globalThis);

// One-way mirror: assignment to a top-level var updates the global object.
topVar = 200;
console.log("after topVar=200, globalThis.topVar =", globalThis.topVar);

// function identity preserved
console.log("typeof topFunc === 'function' ?", typeof topFunc === "function");
console.log("GLOBAL BINDING OK");
