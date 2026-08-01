const rows = [
  ['typeof Symbol.iterator', typeof Symbol.iterator],
  ['Symbol("t").description', String(Symbol('t').description)],
  ['typeof Object.groupBy', typeof Object.groupBy],
  ['typeof Map.groupBy', typeof Map.groupBy],
  ['typeof Promise.withResolvers', typeof Promise.withResolvers],
  ['typeof ArrayBuffer.prototype.transfer', typeof ArrayBuffer.prototype.transfer],
  ['typeof String.prototype.isWellFormed', typeof String.prototype.isWellFormed],
  ['typeof String.prototype.toWellFormed', typeof String.prototype.toWellFormed],
  ['typeof eval', typeof eval],
  ['typeof Function', typeof Function],
];
for (const [k, v] of rows) console.log(k.padEnd(38), '=', v);

// 这些"被 evalSyntax 误判为通过"的特性，直接写是否真的支持？
function chk(name, fn) {
  try { console.log('  ' + name.padEnd(28), fn()); }
  catch (e) { console.log('  ' + name.padEnd(28), 'THREW: ' + e.message); }
}
console.log('-- 直接语法验证 --');
chk('0b101 / 0o10', () => 0b101 + ',' + 0o10);
chk('"\\u{1F600}"', () => "\u{1F600}".length);

chk('可选 catch 绑定', () => { try { throw 0; } catch { return 'ok'; } });
chk('可选链 ?.', () => { const x = {}; return String(x?.a); });
chk('static {}', () => { class A { static v; static { A.v = 9; } } return A.v; });
chk('#私有字段/方法', () => { class A { #x = 1; #m() { return this.#x; } get() { return this.#m(); } } return new A().get(); });
chk('for await of', () => 'syntax-only');
console.log('END');
