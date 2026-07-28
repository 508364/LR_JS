/* Main side: verify whether SAB is physically shared with the worker. */
var sab = new SharedArrayBuffer(16);
var ta = new Int32Array(sab);
Atomics.store(ta, 0, 1);
Atomics.store(ta, 1, 7);

var w = new Worker("tests/sab_worker_child.js");
w.postMessage(sab);

/* Busy-wait until worker writes 12345 into ta[0], or give up */
var seen = 0;
var spins = 0;
while (spins < 200) {
    /* burn some cycles to give the worker thread time */
    var x = 0;
    for (var i = 0; i < 200000; i++) { x += i; }
    seen = Atomics.load(ta, 0);
    if (seen === 12345) break;
    spins++;
}

var v0 = Atomics.load(ta, 0);
var v1 = Atomics.load(ta, 1);
console.log("[main] after " + spins + " spins: ta[0]=" + v0 + " ta[1]=" + v1);
if (v0 === 12345 && v1 === 107) {
    console.log("RESULT: SHARED - worker writes are visible in main");
} else {
    console.log("RESULT: NOT SHARED - worker writes NOT visible");
}
w.terminate();
