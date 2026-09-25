// Manual bytecode dump for debugging
var fs = require('fs');
var data = fs.readFileSync('tests/test_simple_while_debug.js', 'utf8');
// Can't read binary bytecode directly, but we can see what the JS engine produces
print('Script loaded, data length:', data.length);
