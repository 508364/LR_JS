/* Worker side: receive SAB, write a magic value via Atomics. */
self.onmessage = function (e) {
    try {
        var sab = e.data;
        console.log("[worker] typeof data=" + typeof sab);
        console.log("[worker] byteLength=" + sab.byteLength);
        var ta = new Int32Array(sab);
        console.log("[worker] ta.length=" + ta.length);
        var before = Atomics.load(ta, 0);
        console.log("[worker] before=" + before);
        Atomics.store(ta, 0, 12345);
        Atomics.add(ta, 1, 100);
        console.log("[worker] wrote ta[0]=12345, ta[1]+=100");
    } catch (err) {
        console.log("[worker] THREW: " + (err && err.message ? err.message : err));
    }
};
console.log("[worker] ready");
