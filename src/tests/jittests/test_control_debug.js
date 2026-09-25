/* This file is part of LR_JS.
 * Copyright (C) 2024-2025 by LR_JS authors */

function test_control(x) {
    var r = 0;
    if (x > 0) {
        r = 1;
    } else {
        r = 2;
    }
    while (x > 0) {
        r += 10;
        x = x - 1;
    }
    return r;
}

console.log("test_control(2):", test_control(2));
console.log("test_control(3):", test_control(3));
console.log("test_control(0):", test_control(0));
console.log("test_control(-1):", test_control(-1));

// Check values
var pass = true;
if (test_control(2) !== 22) { console.log("FAIL: test_control(2) expected 22"); pass = false; }
if (test_control(3) !== 32) { console.log("FAIL: test_control(3) expected 32"); pass = false; }
if (test_control(0) !== 2) { console.log("FAIL: test_control(0) expected 2"); pass = false; }
if (test_control(-1) !== 2) { console.log("FAIL: test_control(-1) expected 2"); pass = false; }
if (pass) console.log("PASS");
