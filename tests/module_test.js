import defFn, { PI, inc, Greeter, a, b } from "./tests/mod_fixture.js";
import * as ns from "./tests/mod_fixture.js";
import * as re from "./tests/mod_reexport.js";

let ok = true;
function check(name, cond) {
  if (!cond) { ok = false; console.log("FAIL:", name); }
  else console.log("PASS:", name);
}

check("PI", PI === 3.14159);
check("inc return", inc() === 1);
check("default export", defFn() === "default-export");
check("class export", new Greeter().hello() === "hi");
check("destructured export a", a === 1);
check("destructured export b", b === 2);
check("namespace PI", ns.PI === 3.14159);
check("namespace inc is fn", typeof ns.inc === "function");

/* re-export via `export { x as y } from` */
check("reexport PI2", re.PI2 === 3.14159);
/* re-export via `export * from` (default is intentionally NOT re-exported) */
check("reexport star PI", re.PI === 3.14159);
check("reexport star no default", re.default === undefined);

if (!ok) throw new Error("module test failed");
console.log("MODULE TEST OK");
