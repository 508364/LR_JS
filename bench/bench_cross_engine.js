// Cross-engine benchmark comparing LR_JS, V8 (Node.js), and QuickJS
const ROUNDS = 3;
const LR_JS_CMD = process.platform === 'win32' ? 'build-release/bin/lr_js.exe' : './build-release/bin/lr_js';
const QUICKJS_CMD = process.platform === 'win32' ? 'build-release/bin/quickjs.exe' : './build-release/bin/quickjs';

function spawn(cmd, args, timeout = 30000) {
  return new Promise((resolve, reject) => {
    const { spawn } = require('child_process');
    const proc = spawn(cmd, args, { timeout });
    let stdout = '', stderr = '';
    proc.stdout.on('data', d => stdout += d);
    proc.stderr.on('data', d => stderr += d);
    proc.on('error', e => resolve({ error: e.message, stdout, stderr }));
    proc.on('close', code => resolve({ code, stdout, stderr }));
  });
}

async function runTest(name, code, engines) {
  console.log(`\n[${name}]`);
  const results = {};
  for (const [engine, cmd] of Object.entries(engines)) {
    const times = [];
    for (let r = 0; r < ROUNDS; r++) {
      const start = performance.now();
      const { stdout, stderr, error } = await spawn(cmd, ['--eval', code], 30000);
      const elapsed = performance.now() - start;
      if (error || stderr.includes('error')) {
        console.log(`  ${engine}: ERROR - ${stderr || error}`);
        results[engine] = null;
        break;
      }
      times.push(elapsed);
    }
    if (results[engine] !== undefined) {
      const avg = times.reduce((a,b) => a+b, 0) / times.length;
      console.log(`  ${engine}: ${avg.toFixed(3)} ms (avg of ${times.length})`);
      results[engine] = avg;
    }
  }
  return results;
}

async function main() {
  console.log('=== Cross-Engine Benchmark ===');
  console.log('LR_JS | V8 (Node.js) | QuickJS\n');

  // Test 1: Simple loop
  await runTest('Simple loop (100k iterations)', `
var sum = 0; for (var i = 0; i < 100000; i++) sum += i;
`, {
    'V8': 'node',
    'QuickJS': QUICKJS_CMD,
    'LR_JS': LR_JS_CMD
  });

  // Test 2: Nested functions (JIT stress test)
  await runTest('Nested functions (10k calls)', `
function inner(y) { return y * 2; }
function outer(x) { return x + inner(x); }
var r = 0; for (var i = 0; i < 10000; i++) r = outer(i);
print(r);
`, {
    'V8': 'node',
    'QuickJS': QUICKJS_CMD,
    'LR_JS': LR_JS_CMD
  });

  // Test 3: Arithmetic ops
  await runTest('Arithmetic ops (100k)', `
var x = 0; for (var i = 0; i < 100000; i++) x += i * 0.5;
print(x);
`, {
    'V8': 'node',
    'QuickJS': QUICKJS_CMD,
    'LR_JS': LR_JS_CMD
  });

  // Test 4: Array operations
  await runTest('Array ops (10k)', `
var arr = []; for (var i = 0; i < 10000; i++) arr.push(i);
var sum = arr.reduce(function(a, b) { return a + b; }, 0);
print(sum);
`, {
    'V8': 'node',
    'QuickJS': QUICKJS_CMD,
    'LR_JS': LR_JS_CMD
  });

  // Test 5: Fibonacci (recursive)
  await runTest('Fibonacci(25)', `
function fib(n) { if (n <= 1) return n; return fib(n-1) + fib(n-2); }
print(fib(25));
`, {
    'V8': 'node',
    'QuickJS': QUICKJS_CMD,
    'LR_JS': LR_JS_CMD
  });

  console.log('\n=== Summary ===');
  console.log('Expected results:');
  console.log('  Fibonacci(25) = 75025');
  console.log('  Array reduce sum(0..9999) = 49995000');
}

main().catch(console.error);
