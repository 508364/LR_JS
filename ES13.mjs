// ES2022 特性支持检测脚本（模块版本）

// 由于 import.meta 和顶层 await 是静态语法，无法通过 eval 检测，
// 必须在模块顶层直接使用，这里提前检测并保存结果。
let importMetaSupported = false;
let topLevelAwaitSupported = false;

// 检测 import.meta
try {
  // 在模块中，import.meta 总是存在的，但为了兼容性，我们直接访问并赋值
  if (typeof import.meta !== 'undefined') {
    importMetaSupported = true;
  }
} catch {
  // 不会进入这里，因为模块中 import.meta 总是可用的
}

// 检测顶层 await：尝试执行一个顶层 await
try {
  // 注意：如果环境不支持顶层 await，这行会在解析阶段报错，所以用 try 无法捕获。
  // 但既然我们作为模块运行，支持环境一定支持，所以下面这行会正常执行。
  // 为了安全，我们将其放在一个立即执行的异步函数中？不，那样就不是顶层 await 了。
  // 正确方式：直接使用 await，如果支持，会执行并设置标志。
  // 由于这是顶层，如果环境不支持，整个脚本解析失败，所以无法捕获。
  // 因此，我们只能假设环境支持顶层 await，否则脚本运行不起来。
  // 但为了能输出结果，我们可以在支持时设置 true，不支持时脚本根本不会执行到这里。
  // 所以我们直接写：
  await 0;
  topLevelAwaitSupported = true;
} catch {
  // 如果环境不支持，这里的 catch 不会捕获解析错误，所以实际上不会执行。
  // 我们保留以作备用。
  topLevelAwaitSupported = false;
}

// 以下是原 IIFE，内部使用 testSyntax 和其他检测方式
(() => {
  const testSyntax = (code) => {
    try {
      new Function(code)();
      return true;
    } catch (e) {
      return false; // 简化
    }
  };

  const results = {};

  // 收集除 import.meta 和顶层 await 外的所有检测结果
  const tests = {
    '类字段 (Class fields)': () => testSyntax('class C { x = 1; }'),
    '私有字段 (Private fields)': () => testSyntax('class C { #x = 1; }'),
    '私有方法 (Private methods)': () => testSyntax('class C { #m() {} }'),
    '私有访问器 (Private accessors)': () => testSyntax('class C { get #x() {} }'),
    '静态类字段 (Static class fields)': () => testSyntax('class C { static x = 1; }'),
    '静态私有字段 (Static private fields)': () => testSyntax('class C { static #x = 1; }'),
    '静态初始化块 (Static initialization blocks)': () => testSyntax('class C { static { } }'),
    'Error cause': () => {
      try {
        const err = new Error('', { cause: 1 });
        return err.cause === 1;
      } catch {
        return false;
      }
    },
    'Array.prototype.at': () => typeof [].at === 'function',
    'String.prototype.at': () => typeof ''.at === 'function',
    'Object.hasOwn': () => typeof Object.hasOwn === 'function',
    'RegExp match indices (d flag)': () => {
      try {
        const result = /(.)/d.exec('a');
        return result && 'indices' in result;
      } catch {
        return false;
      }
    }
  };

  for (const [name, testFn] of Object.entries(tests)) {
    results[name] = testFn();
  }

  // 加入预先检测的 import.meta 和顶层 await
  results['import.meta'] = importMetaSupported;
  results['顶层 await (Top-level await)'] = topLevelAwaitSupported;

  // 输出
  console.log('=== ES2022 特性支持情况 ===');
  for (const [name, supported] of Object.entries(results)) {
    console.log(`${name}: ${supported ? 'YES' : 'NO'}`);
  }
})();