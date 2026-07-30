// In module mode, top-level var/function must NOT be bound onto the global
// object (they live in the module's declarative record).
var mVar = 7;
function mFunc() {}
let mLet = 8;

console.log("'mVar' in globalThis ?", "mVar" in globalThis);
console.log("'mFunc' in globalThis ?", "mFunc" in globalThis);
console.log("'mLet' in globalThis ?", "mLet" in globalThis);
console.log("MODULE BINDING OK");
