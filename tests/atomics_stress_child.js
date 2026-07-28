/* Worker side: hammer Atomics.add on the shared counter, then signal done. */
self.onmessage = function (e) {
    var ta = new Int32Array(e.data.sab);
    var n = e.data.n;
    for (var i = 0; i < n; i++) {
        Atomics.add(ta, 0, 1);
    }
    Atomics.add(ta, 1, 1);  /* signal completion */
};
