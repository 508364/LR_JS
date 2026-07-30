try { throw 1; } catch { console.log("catch-noparam ok"); }
const e = new Error("m", { cause: "root" });
console.log("cause:", e.message, e.cause);
