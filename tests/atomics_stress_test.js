/* Atomics concurrency stress test:
 * two workers each perform N Atomics.add(ta, 0, 1) on the SAME
 * SharedArrayBuffer. If the CAS-based Atomics are correct, the final
 * counter must be exactly 2*N (no lost updates). */
var N = 100000;
var sab = new SharedArrayBuffer(16);
var ta = new Int32Array(sab);

var w1 = new Worker("tests/atomics_stress_child.js");
var w2 = new Worker("tests/atomics_stress_child.js");
w1.postMessage({ sab: sab, n: N });
w2.postMessage({ sab: sab, n: N });

/* Wait until both workers signal completion via ta[1] (each adds 1).
 * Atomics.wait now really sleeps, so use it with a timeout as the poll. */
var rounds = 0;
while (Atomics.load(ta, 1) < 2 && rounds < 300) {
    Atomics.wait(ta, 2, 0, 50);  /* ta[2] never changes: 50ms sleep */
    rounds++;
}

var done = Atomics.load(ta, 1);
var total = Atomics.load(ta, 0);
console.log("[main] workers done=" + done + " counter=" + total +
            " expected=" + (2 * N));
if (done === 2 && total === 2 * N) {
    console.log("RESULT: ATOMIC OK - no lost updates under contention");
} else {
    console.log("RESULT: ATOMIC FAIL - lost updates detected");
}
w1.terminate();
w2.terminate();
