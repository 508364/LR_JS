try {
  let sum = 0;
  for (const x of [1, 2, 3, 4]) sum += x;
  console.log("array sum:", sum);

  let chars = "";
  for (const c of "abc") chars += c + ".";
  console.log("string:", chars);

  const myIterable = {
      [Symbol.iterator]() {
          let i = 0;
          return {
              next() {
                  if (i >= 3) return { value: undefined, done: true };
                  const v = i * 10;
                  i++;
                  return { value: v, done: false };
              }
          };
      }
  };
  let out = [];
  for (const v of myIterable) out.push(v);
  console.log("custom:", out.join(","));

  let b = [];
  for (const x of [1,2,3,4,5]) { if (x === 3) break; b.push(x); }
  console.log("break:", b.join(","));

  let c = [];
  for (const x of [1,2,3,4]) { if (x % 2 === 0) continue; c.push(x); }
  console.log("continue:", c.join(","));

  try { for (const x of 42) {} console.log("no error"); }
  catch (e) { console.log("not iterable caught:", e ? "yes" : "no"); }
} catch (e) {
  console.log("TOP CAUGHT:", e && e.message ? e.message : String(e));
}
