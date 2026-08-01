console.log('A', eval('1+2'));
console.log('B', eval('"x"'));
console.log('C', eval('let b = 2; b'));
console.log('D', eval('null ?? 1'));
console.log('E', eval('1_000_000'));
console.log('F', typeof Symbol.iterator, String(Symbol.iterator));
console.log('G', Symbol('t').description, typeof Symbol('t').description);
console.log('END');
