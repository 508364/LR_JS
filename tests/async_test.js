// async function returns a Promise
async function f1() { return 42; }
const p1 = f1();
console.log("is promise:", typeof p1 === "object" && typeof p1.then === "function" ? "yes" : "no");
p1.then(v => console.log("then value:", v));

// await unwraps a resolved promise
async function f2() {
    const v = await Promise.resolve(7);
    return v + 1;
}
f2().then(v => console.log("await resolved:", v));

// await plain (non-promise) value
async function f3() {
    const v = await 5;
    return v * 2;
}
f3().then(v => console.log("await plain:", v));

// throw inside async -> rejected promise
async function f4() { throw "boom"; }
f4().catch(e => console.log("rejected with:", e));

// await a rejected promise -> try/catch catches it
async function f5() {
    try {
        await Promise.reject("bad");
        return "not reached";
    } catch (e) {
        return "caught:" + e;
    }
}
f5().then(v => console.log("await reject:", v));

// async arrow function
const f6 = async (x) => x + 100;
f6(1).then(v => console.log("async arrow:", v));

// chained awaits
async function f7() {
    const a = await f1();
    const b = await f6(a);
    return a + b;
}
f7().then(v => console.log("chained:", v));

console.log("ALL ASYNC TESTS QUEUED");
