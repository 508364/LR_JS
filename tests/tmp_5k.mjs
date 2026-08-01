console.log("start");
class B { constructor(v) { this.b = v; } }
class L1 extends B { #p=1; constructor(v) { super(v); this.l1=v+1; } }
class L2 extends L1 { #p=2; constructor(v) { super(v); this.l2=v+2; } }
class L3 extends L2 { #p=3; constructor(v) { super(v); this.l3=v+3; } }
class L4 extends L3 { #p=4; constructor(v) { super(v); this.l4=v+4; } }
class L5 extends L4 { #p=5; constructor(v) { super(v); this.l5=v+5; } }
class L6 extends L5 { #p=6; constructor(v) { super(v); this.l6=v+6; } }
class L7 extends L6 { #p=7; constructor(v) { super(v); this.l7=v+7; } }
class L8 extends L7 { #p=8; constructor(v) { super(v); this.l8=v+8; } }
class L9 extends L8 { #p=9; constructor(v) { super(v); this.l9=v+9; } }
class L10 extends L9 { #p=10; static {} constructor(v) { super(v); this.l10=v+10; } }
console.log("classes ok");
var arr = [];
for (let i = 0; i < 5000; i++) arr.push(new L10(i));
console.log("instances:", arr.length, arr[0].l10);
