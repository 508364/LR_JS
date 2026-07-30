function tryRe(pat) {
    try { const r = new RegExp(pat); return "ok " + (r.exec ? "e" : "-"); }
    catch (err) { return "FAIL " + err; }
}
console.log("a(b):", tryRe("a(b)"));
console.log("\\d:", tryRe("\\d"));
console.log("a{2}:", tryRe("a{2}"));
console.log("\\d{4}:", tryRe("\\d{4}"));
console.log("(\\d{4}):", tryRe("(\\d{4})"));
console.log("(?<y>\\d{4}):", tryRe("(?<y>\\d{4})"));
