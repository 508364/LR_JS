/* This file is part of LR_JS.
 * Copyright (C) 2024-2025 by LR_JS authors */

function test_ifelse(x) {
    var r = 0;
    if (x > 0) {
        r = 1;
    } else {
        r = 2;
    }
    return r;
}

console.log("test_ifelse(-1):", test_ifelse(-1));
console.log("test_ifelse(3):", test_ifelse(3));

// Check values
var pass = true;
if (test_ifelse(-1) !== 2) { console.log("FAIL: test_ifelse(-1) expected 2"); pass = false; }
if (test_ifelse(3) !== 1) { console.log("FAIL: test_ifelse(3) expected 1"); pass = false; }
if (pass) console.log("PASS");
