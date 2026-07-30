// ES2022 feature probe - each test isolated with try/catch
function t(name, fn) {
    try { console.log(name + ": " + fn()); }
    catch (e) { console.log(name + ": THROW " + e); }
}

// optional chaining & nullish
t("optchain", () => { const o = { a: { b: 1 } }; return o?.a?.b + "," + (o.x?.y ?? "def"); });
t("nullish-assign", () => { let x = null; x ??= 5; let y = 1; y ??= 9; return x + "," + y; });
t("logical-assign", () => { let a = 0; a ||= 3; let b = 2; b &&= 7; return a + "," + b; });

// destructuring
t("destr-arr", () => { const [a, b = 9, ...r] = [1, undefined, 3, 4]; return a + "," + b + "," + r.join("/"); });
t("destr-obj", () => { const { x, y: z = 5, ...rest } = { x: 1, w: 2, v: 3 }; return x + "," + z + "," + JSON.stringify(rest); });
t("destr-param", () => { const f = ({ a, b = 2 }, [c]) => a + b + c; return f({ a: 1 }, [10]); });
t("destr-swap", () => { let a = 1, b = 2; [a, b] = [b, a]; return a + "," + b; });

// template literals
t("template", () => { const n = 7; return `n=${n} sq=${n * n}`; });
t("tagged", () => { const tag = (s, ...v) => s.join("|") + "#" + v.join(","); return tag`a${1}b${2}c`; });

// classes
t("class-priv", () => { class C { #p = 3; getP() { return this.#p; } static s = 10; } return new C().getP() + "," + C.s; });
t("class-static-block", () => { class C { static v; static { C.v = 42; } } return C.v; });
t("class-in", () => { class C { #x; static has(o) { return #x in o; } } return C.has(new C()) + "," + C.has({}); });
t("getter-setter", () => { const o = { _v: 1, get v() { return this._v; }, set v(n) { this._v = n * 2; } }; o.v = 5; return o.v; });

// object/array features
t("computed-key", () => { const k = "ab"; const o = { [k + "c"]: 9 }; return o.abc; });
t("shorthand-method", () => { const o = { m(x) { return x * 3; } }; return o.m(4); });
t("at-method", () => { return [1, 2, 3].at(-1) + "," + "xyz".at(-1); });
t("array-includes", () => [1, 2, NaN].includes(NaN));
t("array-flat", () => [1, [2, [3]]].flat(2).join(","));
t("array-findlast", () => [1, 2, 3, 4].findLast(x => x % 2 === 1) + "," + [1, 2, 3].findLastIndex(x => x === 2));
t("object-entries", () => JSON.stringify(Object.entries({ a: 1 })) + JSON.stringify(Object.fromEntries([["b", 2]])));
t("object-hasown", () => Object.hasOwn({ a: 1 }, "a") + "," + Object.hasOwn({}, "a"));

// strings
t("str-replaceall", () => "a-b-c".replaceAll("-", "+"));
t("str-pad", () => "5".padStart(3, "0") + "," + "5".padEnd(3, "x"));
t("str-trim", () => "[" + "  hi  ".trimStart() + "][" + "  hi  ".trimEnd() + "]");
t("str-matchall", () => { let r = []; for (const m of "a1b2".matchAll(/\d/g)) r.push(m[0]); return r.join(","); });

// numbers / misc
t("exponent", () => 2 ** 10);
t("bigint", () => { const b = 10n ** 3n; return b + "," + typeof b; });
t("numeric-sep", () => 1_000_000 + 1);
t("symbol", () => { const s = Symbol("k"); const o = {}; o[s] = 5; return o[s] + "," + typeof s; });

// Map/Set/WeakMap
t("map", () => { const m = new Map([["a", 1]]); m.set("b", 2); return m.get("a") + m.get("b") + "," + m.size; });
t("set", () => { const s = new Set([1, 1, 2]); return s.size + "," + s.has(2); });

// error cause / try
t("error-cause", () => { const e = new Error("m", { cause: "root" }); return e.message + "," + e.cause; });
t("try-noparam", () => { try { throw 1; } catch { return "ok"; } });

// regexp
t("regexp-d", () => { const m = /a(b)/d.exec("ab"); return m ? (m.indices ? "indices" : "no-indices") : "no-match"; });
t("regexp-named", () => { const m = /(?<y>\d{4})/.exec("year 2024"); return m.groups.y; });

// iterators / generators interplay
t("gen-in-class", () => { class C { *g() { yield 1; yield 2; } } return [...new C().g()].join(","); });
t("destr-forof", () => { let r = []; for (const [k, v] of [["a", 1], ["b", 2]]) r.push(k + v); return r.join(","); });

// top-level await (module-ish)
t("promise-allsettled", () => typeof Promise.allSettled);
t("promise-any", () => typeof Promise.any);
t("globalthis", () => typeof globalThis);
t("structured-misc", () => JSON.stringify({ a: [1, { b: 2 }] }));

console.log("PROBE DONE");
