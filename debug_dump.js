var bytes = (function() {
    var buf = new Uint8Array(62);
    var str = require('fs').readFileSync('tests/test_simple_while_debug.bc');
    for (var i = 0; i < str.length && i < 62; i++) buf[i] = str.charCodeAt(i);
    return buf;
})();
for (var i = 0; i < bytes.length; i++) {
    if (i % 16 === 0) process.stdout.write((i).toString(16).padStart(4,'0') + ': ');
    process.stdout.write(bytes[i].toString(16).padStart(2,'0') + ' ');
    if (i % 16 === 15) process.stdout.write('\n');
}
