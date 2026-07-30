console.log("start");
const r4 = /(\d{4})/;
console.log("created:", r4.source);
console.log("test:", r4.test("2024"));
console.log("exec:", JSON.stringify(r4.exec("year 2024")));
console.log("end");
