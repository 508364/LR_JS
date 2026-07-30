// labeled break: exits both loops
let a = [];
outer1:
for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
        if (i === 1 && j === 1) break outer1;
        a.push(i + "" + j);
    }
}
console.log("lbl break:", a.join(","));

// labeled continue: skips to next outer iteration
let b = [];
outer2:
for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
        if (j === 1) continue outer2;
        b.push(i + "" + j);
    }
}
console.log("lbl continue:", b.join(","));

// unlabeled break/continue still only affect the innermost loop
let c = [];
for (let i = 0; i < 2; i++) {
    for (let j = 0; j < 4; j++) {
        if (j === 2) break;
        if (j === 0) continue;
        c.push(i + "" + j);
    }
}
console.log("plain:", c.join(","));

// labeled while + continue
let d = [];
let i2 = 0;
w1:
while (i2 < 3) {
    i2++;
    let k = 0;
    while (k < 3) {
        k++;
        if (k === 2) continue w1;
        d.push(i2 + ":" + k);
    }
}
console.log("lbl while:", d.join(","));

// labeled for-of break
let e = [];
outer3:
for (const x of [1, 2, 3]) {
    for (const y of [10, 20, 30]) {
        if (x === 2 && y === 20) break outer3;
        e.push(x + "-" + y);
    }
}
console.log("lbl forof:", e.join(","));

// labeled block: break out of a plain block
let f = [];
blk: {
    f.push("one");
    if (f.length === 1) break blk;
    f.push("never");
}
f.push("after");
console.log("lbl block:", f.join(","));

// break label targeting a labeled switch
let g = "";
sw: switch (1) {
    case 1:
        g += "case1";
        if (g) break sw;
        g += "unreachable";
    default:
        g += "def";
}
console.log("lbl switch:", g);

// labeled do-while
let h = [];
let n = 0;
dw: do {
    n++;
    for (let m = 0; m < 3; m++) {
        if (n === 2 && m === 1) break dw;
        h.push(n + "." + m);
    }
} while (n < 3);
console.log("lbl dowhile:", h.join(","));

console.log("ALL LABEL TESTS DONE");
