#!/bin/sh
cd /tmp || exit 1
printf 'throw new Error("boom");\n' > t1.js
printf 'function f(a,){}\n' > t2.js
printf 'console.log("ok");\n' > t3.js

/tmp/lrb/bin/lr_js t1.js; echo "uncaught_throw_exit=$?"
/tmp/lrb/bin/lr_js t2.js; echo "parse_error_exit=$?"
/tmp/lrb/bin/lr_js t3.js; echo "normal_exit=$?"
echo "--- raw bytes of a normal run ---"
/tmp/lrb/bin/lr_js t3.js 2>/dev/null | od -c
