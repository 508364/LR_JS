/**
 * ECMAScript 5 ~ ES2024 (ES15) 特性检测脚本
 * 用法: 使用 Node.js 运行 `node es-test.js` 或在浏览器控制台中执行
 * 输出: 控制台表格，展示每个特性的支持情况
 */
(function () {
  'use strict';

  // 测试结果收集
  const results = [];

  // 添加一个测试用例
  function addTest(name, version, testFn) {
    try {
      const passed = testFn();
      results.push({ name, version, supported: passed });
    } catch (e) {
      // 任何意外错误视为不支持
      results.push({ name, version, supported: false, error: e.message });
    }
  }

  // 辅助: 安全 eval 语法测试 (捕获 SyntaxError 表示不支持)
  function evalSyntax(code) {
    try {
      eval(code);
      return true;
    } catch (e) {
      if (e instanceof SyntaxError) return false;
      // 其他错误 (如运行时错误) 可能语法支持，返回 true
      return true;
    }
  }

  // 辅助: 安全 eval 执行并返回结果，异常时返回 undefined
  function evalResult(code) {
    try {
      return eval(code);
    } catch (e) {
      return undefined;
    }
  }

  // 辅助: 测试 RegExp 标志是否支持
  function regexFlagSupported(flag) {
    try {
      new RegExp('.', flag);
      return true;
    } catch (e) {
      return false;
    }
  }

  // ==================== ES5 ====================
  addTest('严格模式 "use strict"', 'ES5', () => {
    return evalSyntax('"use strict"; function f() { return !this; }; f();');
  });

  addTest('JSON 对象', 'ES5', () => typeof JSON === 'object' && JSON.parse && JSON.stringify);

  addTest('Object.create', 'ES5', () => typeof Object.create === 'function');

  addTest('Object.defineProperty', 'ES5', () => typeof Object.defineProperty === 'function');

  addTest('Array.isArray', 'ES5', () => typeof Array.isArray === 'function');

  addTest('Array.prototype.forEach', 'ES5', () => typeof Array.prototype.forEach === 'function');

  addTest('Array.prototype.map', 'ES5', () => typeof Array.prototype.map === 'function');

  addTest('Array.prototype.filter', 'ES5', () => typeof Array.prototype.filter === 'function');

  addTest('Function.prototype.bind', 'ES5', () => typeof Function.prototype.bind === 'function');

  addTest('String.prototype.trim', 'ES5', () => typeof String.prototype.trim === 'function');

  addTest('Date.now', 'ES5', () => typeof Date.now === 'function');

  addTest('getter/setter 字面量', 'ES5', () => {
    return evalSyntax('var o = { get x() { return 1; }, set x(v) {} };');
  });

  // ==================== ES2015 (ES6) ====================
  addTest('let / const', 'ES2015', () => evalSyntax('let a = 1; const b = 2;'));

  addTest('箭头函数', 'ES2015', () => evalSyntax('var f = () => 1;'));

  addTest('class 类', 'ES2015', () => evalSyntax('class A {}'));

  addTest('模板字面量', 'ES2015', () => evalSyntax('var s = `hello ${1}`;'));

  addTest('解构赋值', 'ES2015', () => evalSyntax('var [a, b] = [1, 2]; var {c, d} = {c:3, d:4};'));

  addTest('默认参数', 'ES2015', () => evalSyntax('function f(x = 1) {}'));

  addTest('rest 参数', 'ES2015', () => evalSyntax('function f(...args) {}'));

  addTest('spread 展开语法', 'ES2015', () => evalSyntax('var a = [1, ...[2, 3]];'));

  addTest('for...of 循环', 'ES2015', () => evalSyntax('for (var v of [1]) {}'));

  addTest('Promise', 'ES2015', () => typeof Promise !== 'undefined');

  addTest('Symbol', 'ES2015', () => typeof Symbol !== 'undefined');

  addTest('Map', 'ES2015', () => typeof Map !== 'undefined');

  addTest('Set', 'ES2015', () => typeof Set !== 'undefined');

  addTest('WeakMap', 'ES2015', () => typeof WeakMap !== 'undefined');

  addTest('WeakSet', 'ES2015', () => typeof WeakSet !== 'undefined');

  addTest('Proxy', 'ES2015', () => typeof Proxy !== 'undefined');

  addTest('Reflect', 'ES2015', () => typeof Reflect !== 'undefined');

  addTest('生成器 function*', 'ES2015', () => evalSyntax('function* g() { yield 1; }'));

  addTest('迭代器 Symbol.iterator', 'ES2015', () => typeof Symbol.iterator === 'symbol');

  addTest('二进制/八进制字面量', 'ES2015', () => evalSyntax('0b101; 0o10;'));

  addTest('Unicode 码点转义 \\u{...}', 'ES2015', () => evalSyntax('"\\u{1F600}"'));

  addTest('Math.sign', 'ES2015', () => typeof Math.sign === 'function');

  addTest('String.fromCodePoint', 'ES2015', () => typeof String.fromCodePoint === 'function');

  addTest('Array.from', 'ES2015', () => typeof Array.from === 'function');

  addTest('Array.of', 'ES2015', () => typeof Array.of === 'function');

  addTest('Object.assign', 'ES2015', () => typeof Object.assign === 'function');

  // ==================== ES2016 (ES7) ====================
  addTest('指数运算符 **', 'ES2016', () => evalSyntax('2 ** 3 === 8'));

  addTest('Array.prototype.includes', 'ES2016', () => typeof Array.prototype.includes === 'function');

  // ==================== ES2017 (ES8) ====================
  addTest('async / await', 'ES2017', () => evalSyntax('async function f() { await Promise.resolve(); }'));

  addTest('SharedArrayBuffer', 'ES2017', () => typeof SharedArrayBuffer !== 'undefined');

  addTest('Atomics', 'ES2017', () => typeof Atomics !== 'undefined');

  addTest('Object.values', 'ES2017', () => typeof Object.values === 'function');

  addTest('Object.entries', 'ES2017', () => typeof Object.entries === 'function');

  addTest('String.prototype.padStart', 'ES2017', () => typeof String.prototype.padStart === 'function');

  addTest('String.prototype.padEnd', 'ES2017', () => typeof String.prototype.padEnd === 'function');

  addTest('函数参数列表尾逗号', 'ES2017', () => evalSyntax('function f(a,b,){}'));

  // ==================== ES2018 (ES9) ====================
  addTest('for-await-of 异步迭代', 'ES2018', () => {
    return evalSyntax('async function f() { for await (let x of []) {} }');
  });

  addTest('Promise.prototype.finally', 'ES2018', () => typeof Promise.prototype.finally === 'function');

  addTest('对象 Rest/Spread 属性', 'ES2018', () => {
    return evalSyntax('var {a, ...rest} = {a:1, b:2}; var c = {...{x:1}};');
  });

  addTest('正则命名捕获组 (?<name>...)', 'ES2018', () => {
    try {
      new RegExp('(?<year>\\d{4})');
      return true;
    } catch (e) {
      return false;
    }
  });

  addTest('正则后行断言 (?<=...)', 'ES2018', () => {
    try {
      new RegExp('(?<=a)b');
      return true;
    } catch (e) {
      return false;
    }
  });

  addTest('正则 Unicode 属性转义 \\p{...}', 'ES2018', () => {
    try {
      new RegExp('\\p{Script=Greek}', 'u');
      return true;
    } catch (e) {
      return false;
    }
  });

  addTest('正则 s 标志 (dotAll)', 'ES2018', () => regexFlagSupported('s'));

  // ==================== ES2019 (ES10) ====================
  addTest('Array.prototype.flat', 'ES2019', () => typeof Array.prototype.flat === 'function');

  addTest('Array.prototype.flatMap', 'ES2019', () => typeof Array.prototype.flatMap === 'function');

  addTest('Object.fromEntries', 'ES2019', () => typeof Object.fromEntries === 'function');

  addTest('String.prototype.trimStart', 'ES2019', () => typeof String.prototype.trimStart === 'function');

  addTest('String.prototype.trimEnd', 'ES2019', () => typeof String.prototype.trimEnd === 'function');

  addTest('Symbol.prototype.description', 'ES2019', () => {
    try {
      return typeof Symbol('test').description === 'string';
    } catch (e) {
      return false;
    }
  });

  addTest('可选 catch 绑定 (省略参数)', 'ES2019', () => evalSyntax('try { throw 0; } catch { }'));

  addTest('JSON 超集 (\\u2028 \\u2029 合法)', 'ES2019', () => {
    return evalSyntax('"\\u2028\\u2029"');
  });

  // ==================== ES2020 (ES11) ====================
  addTest('BigInt', 'ES2020', () => typeof BigInt !== 'undefined');

  addTest('动态 import()', 'ES2020', () => {
    try {
      // 语法检测: import() 不报 SyntaxError 即视为语法支持
      eval('import("")');
      return true;
    } catch (e) {
      if (e instanceof SyntaxError) return false;
      // 运行时错误 (如 URL 无效) 表明语法正确
      return true;
    }
  });

  addTest('空值合并运算符 ??', 'ES2020', () => {
    return evalResult('(null ?? 1) === 1 && (undefined ?? 2) === 2') === true;
  });

  addTest('可选链 ?.', 'ES2020', () => {
    return evalSyntax('var x = {}; var y = x?.a;');
  });

  addTest('Promise.allSettled', 'ES2020', () => typeof Promise.allSettled === 'function');

  addTest('globalThis', 'ES2020', () => typeof globalThis !== 'undefined');

  addTest('String.prototype.matchAll', 'ES2020', () => typeof String.prototype.matchAll === 'function');

  // ==================== ES2021 (ES12) ====================
  addTest('String.prototype.replaceAll', 'ES2021', () => typeof String.prototype.replaceAll === 'function');

  addTest('Promise.any', 'ES2021', () => typeof Promise.any === 'function');

  addTest('WeakRef', 'ES2021', () => typeof WeakRef !== 'undefined');

  addTest('逻辑赋值 &&= ||= ??=', 'ES2021', () => {
    return evalResult('let a; a ??= 1; a') === 1 &&
           evalResult('let b = 2; b &&= 4; b') === 4 &&
           evalResult('let c = 3; c ||= 5; c') === 3;
  });

  addTest('数字分隔符 _', 'ES2021', () => evalResult('1_000_000') === 1000000);

  // ==================== ES2022 (ES13) ====================
  addTest('类公有字段', 'ES2022', () => evalSyntax('class A { x = 1; }'));

  addTest('类私有字段 #', 'ES2022', () => evalSyntax('class A { #x; }'));

  addTest('类私有方法 #', 'ES2022', () => evalSyntax('class A { #m(){} }'));

  addTest('类静态初始化块 static {}', 'ES2022', () => evalSyntax('class A { static {} }'));

  addTest('Array.prototype.at', 'ES2022', () => typeof Array.prototype.at === 'function');

  addTest('Object.hasOwn', 'ES2022', () => typeof Object.hasOwn === 'function');

  addTest('Error.cause', 'ES2022', () => {
    try {
      const err = new Error('msg', { cause: 'test' });
      return err.cause === 'test';
    } catch (e) {
      return false;
    }
  });

  addTest('正则 d 标志 (匹配索引)', 'ES2022', () => regexFlagSupported('d'));

  // ==================== ES2023 (ES14) ====================
  addTest('Array.prototype.findLast', 'ES2023', () => typeof Array.prototype.findLast === 'function');

  addTest('Array.prototype.findLastIndex', 'ES2023', () => typeof Array.prototype.findLastIndex === 'function');

  addTest('Array.prototype.toSorted', 'ES2023', () => typeof Array.prototype.toSorted === 'function');

  addTest('Array.prototype.toReversed', 'ES2023', () => typeof Array.prototype.toReversed === 'function');

  addTest('Array.prototype.toSpliced', 'ES2023', () => typeof Array.prototype.toSpliced === 'function');

  addTest('Array.prototype.with', 'ES2023', () => typeof Array.prototype.with === 'function');

  addTest('Symbol 作为 WeakMap 键', 'ES2023', () => {
    try {
      var wm = new WeakMap();
      var sym = Symbol('key');
      wm.set(sym, 1);
      return wm.get(sym) === 1;
    } catch (e) {
      return false;
    }
  });

  // ==================== ES2024 (ES15) ====================
  addTest('Object.groupBy / Map.groupBy', 'ES2024', () => {
    return typeof Object.groupBy === 'function' || typeof Map.groupBy === 'function';
  });

  addTest('Promise.withResolvers', 'ES2024', () => typeof Promise.withResolvers === 'function');

  addTest('正则 v 标志 (unicodeSets)', 'ES2024', () => regexFlagSupported('v'));

  addTest('ArrayBuffer.prototype.transfer', 'ES2024', () => {
    return typeof ArrayBuffer.prototype.transfer === 'function';
  });

  addTest('String.prototype.isWellFormed', 'ES2024', () => typeof String.prototype.isWellFormed === 'function');

  addTest('String.prototype.toWellFormed', 'ES2024', () => typeof String.prototype.toWellFormed === 'function');

  // ==================== 输出结果 ====================
  console.log('ECMAScript 特性支持检测结果:');
  console.table(results.map(r => ({
    '特性': r.name,
    '版本': r.version,
    '支持': r.supported ? '✅' : '❌',
    '错误': r.error || ''
  })));

  // 简要统计
  const passed = results.filter(r => r.supported).length;
  const total = results.length;
  console.log(`\n通过: ${passed}/${total} (${Math.round(passed/total*100)}%)`);
})();