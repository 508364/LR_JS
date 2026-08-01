// 提取 ES5-15.js 中未通过的特性（复用同样的检测逻辑）
const fails = [];
function addTest(name, version, testFn) {
  try { if (!testFn()) fails.push([version, name, '']); }
  catch (e) { fails.push([version, name, e.message]); }
}
function evalSyntax(code) {
  try { eval(code); return true; }
  catch (e) { return !(e instanceof SyntaxError); }
}
function evalResult(code) { try { return eval(code); } catch (e) { return undefined; } }
function regexFlagSupported(f) { try { new RegExp('.', f); return true; } catch (e) { return false; } }

addTest('严格模式', 'ES5', () => evalSyntax('"use strict"; function f(){return !this;}; f();'));
addTest('getter/setter 字面量', 'ES5', () => evalSyntax('var o={get x(){return 1;},set x(v){}};'));
addTest('Proxy', 'ES2015', () => typeof Proxy !== 'undefined');
addTest('Reflect', 'ES2015', () => typeof Reflect !== 'undefined');
addTest('迭代器 Symbol.iterator', 'ES2015', () => typeof Symbol.iterator === 'symbol');
addTest('二进制/八进制字面量', 'ES2015', () => evalSyntax('0b101; 0o10;'));
addTest('Unicode 码点转义', 'ES2015', () => evalSyntax('"\\u{1F600}"'));
addTest('SharedArrayBuffer', 'ES2017', () => typeof SharedArrayBuffer !== 'undefined');
addTest('Atomics', 'ES2017', () => typeof Atomics !== 'undefined');
addTest('函数参数尾逗号', 'ES2017', () => evalSyntax('function f(a,b,){}'));
addTest('for-await-of', 'ES2018', () => evalSyntax('async function f(){for await(let x of []){}}'));
addTest('正则命名捕获组', 'ES2018', () => { try { new RegExp('(?<year>\\d{4})'); return true; } catch (e) { return false; } });
addTest('正则后行断言', 'ES2018', () => { try { new RegExp('(?<=a)b'); return true; } catch (e) { return false; } });
addTest('正则 \\p{...}', 'ES2018', () => { try { new RegExp('\\p{Script=Greek}', 'u'); return true; } catch (e) { return false; } });
addTest('正则 s 标志', 'ES2018', () => regexFlagSupported('s'));
addTest('Symbol.description', 'ES2019', () => { try { return typeof Symbol('t').description === 'string'; } catch (e) { return false; } });
addTest('可选 catch 绑定', 'ES2019', () => evalSyntax('try{throw 0;}catch{}'));
addTest('JSON 超集', 'ES2019', () => evalSyntax('"\\u2028\\u2029"'));
addTest('BigInt', 'ES2020', () => typeof BigInt !== 'undefined');
addTest('动态 import()', 'ES2020', () => { try { eval('import("")'); return true; } catch (e) { return !(e instanceof SyntaxError); } });
addTest('?? 运算符', 'ES2020', () => evalResult('(null ?? 1) === 1 && (undefined ?? 2) === 2') === true);
addTest('可选链 ?.', 'ES2020', () => evalSyntax('var x={}; var y=x?.a;'));
addTest('Promise.allSettled', 'ES2020', () => typeof Promise.allSettled === 'function');
addTest('globalThis', 'ES2020', () => typeof globalThis !== 'undefined');
addTest('matchAll', 'ES2020', () => typeof String.prototype.matchAll === 'function');
addTest('replaceAll', 'ES2021', () => typeof String.prototype.replaceAll === 'function');
addTest('Promise.any', 'ES2021', () => typeof Promise.any === 'function');
addTest('WeakRef', 'ES2021', () => typeof WeakRef !== 'undefined');
addTest('逻辑赋值', 'ES2021', () => evalResult('let a; a ??= 1; a') === 1 && evalResult('let b=2; b &&= 4; b') === 4 && evalResult('let c=3; c ||= 5; c') === 3);
addTest('数字分隔符', 'ES2021', () => evalResult('1_000_000') === 1000000);
addTest('类公有字段', 'ES2022', () => evalSyntax('class A{x=1;}'));
addTest('类私有字段', 'ES2022', () => evalSyntax('class A{#x;}'));
addTest('类私有方法', 'ES2022', () => evalSyntax('class A{#m(){}}'));
addTest('static {}', 'ES2022', () => evalSyntax('class A{static {}}'));
addTest('Array.at', 'ES2022', () => typeof Array.prototype.at === 'function');
addTest('Object.hasOwn', 'ES2022', () => typeof Object.hasOwn === 'function');
addTest('Error.cause', 'ES2022', () => { try { return new Error('m', { cause: 't' }).cause === 't'; } catch (e) { return false; } });
addTest('正则 d 标志', 'ES2022', () => regexFlagSupported('d'));
addTest('findLast', 'ES2023', () => typeof Array.prototype.findLast === 'function');
addTest('findLastIndex', 'ES2023', () => typeof Array.prototype.findLastIndex === 'function');
addTest('toSorted', 'ES2023', () => typeof Array.prototype.toSorted === 'function');
addTest('toReversed', 'ES2023', () => typeof Array.prototype.toReversed === 'function');
addTest('toSpliced', 'ES2023', () => typeof Array.prototype.toSpliced === 'function');
addTest('Array.with', 'ES2023', () => typeof Array.prototype.with === 'function');
addTest('Symbol 作 WeakMap 键', 'ES2023', () => { try { var wm = new WeakMap(), s = Symbol('k'); wm.set(s, 1); return wm.get(s) === 1; } catch (e) { return false; } });
addTest('Object/Map.groupBy', 'ES2024', () => typeof Object.groupBy === 'function' || typeof Map.groupBy === 'function');
addTest('Promise.withResolvers', 'ES2024', () => typeof Promise.withResolvers === 'function');
addTest('正则 v 标志', 'ES2024', () => regexFlagSupported('v'));
addTest('ArrayBuffer.transfer', 'ES2024', () => typeof ArrayBuffer.prototype.transfer === 'function');
addTest('isWellFormed', 'ES2024', () => typeof String.prototype.isWellFormed === 'function');
addTest('toWellFormed', 'ES2024', () => typeof String.prototype.toWellFormed === 'function');

console.log('未通过 ' + fails.length + ' 项:');
for (const f of fails) console.log('  [' + f[0] + '] ' + f[1] + (f[2] ? '  <' + f[2] + '>' : ''));
