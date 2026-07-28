/* Worker echo test: verifies bidirectional postMessage with structured
 * clone (object/array/string/number) and parent-side onmessage delivery
 * through the event loop (lr_worker_poll). */
var w = new Worker("tests/worker_echo_child.js");
var got = null;

w.onmessage = function (e) {
    got = e.data;
    console.log("[main] echo: " + JSON.stringify(got));
    var ok = got && got.msg === "hello" && got.num === 42 &&
             got.arr && got.arr.length === 3 && got.arr[2] === 3 &&
             got.nested && got.nested.flag === true;
    console.log(ok ? "RESULT: ECHO OK - structured clone roundtrip works"
                   : "RESULT: ECHO FAIL");
    w.terminate();
};
w.onerror = function (e) {
    console.log("RESULT: ECHO FAIL - worker error: " + e.data.message);
    w.terminate();
};

w.postMessage({ msg: "hello", num: 42, arr: [1, 2, 3],
                nested: { flag: true } });
