let x = null; x ??= 5; console.log("nullish-assign:", x);
let a = 0; a ||= 3; let b = 2; b &&= 7; console.log("logic-assign:", a, b);
