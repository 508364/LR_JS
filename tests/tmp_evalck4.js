console.log('typeof eval =', typeof eval);

function f1() { return eval('1+2'); }
try { console.log('in-func eval:', f1()); } catch (e) { console.log('in-func threw:', e.message); }

(function () {
  try { console.log('iife eval:', eval('1+2')); } catch (e) { console.log('iife threw:', e.message); }
  try { console.log('iife ??  :', eval('null ?? 1')); } catch (e) { console.log('iife ?? threw:', e.message); }
  try { console.log('iife num :', eval('1_000_000')); } catch (e) { console.log('iife num threw:', e.message); }
})();

try { console.log('top eval:', eval('1+2')); } catch (e) { console.log('top threw:', e.message); }
console.log('end');
