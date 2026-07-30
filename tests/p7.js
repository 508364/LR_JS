const m1 = /(\d{4})/.exec("year 2024");
console.log("plain group:", m1 ? m1[1] : "null");
const r = new RegExp("(?<y>\\d{4})");
console.log("ctor named:", typeof r.exec);
const m2 = r.exec("year 2024");
console.log("named exec:", m2 ? m2[1] + "|" + (m2.groups ? m2.groups.y : "nog") : "null");
