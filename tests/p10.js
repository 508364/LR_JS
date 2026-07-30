const r1 = /ab/;
console.log("1:", r1.source, "|", r1.test("xaby"));
const r2 = /\d/;
console.log("2:", r2.source, "|", r2.test("x5y"));
const r3 = /a{2}/;
console.log("3:", r3.source, "|", r3.test("xaay"));
const r4 = /(\d{4})/;
console.log("4:", r4.source, "|", r4.test("2024"));
