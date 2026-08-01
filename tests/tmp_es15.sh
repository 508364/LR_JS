#!/bin/sh
cd /mnt/f/GitHub/LR_JS || exit 1
for bin in /tmp/lrb/bin/lr_js /tmp/lrc/bin/lr_js; do
  echo "=== $bin ==="
  "$bin" ES5-15.js > "/tmp/$(basename $(dirname $(dirname $bin))).out" 2>&1
  echo "exit=$?"
done
echo "=== DIFF gcc vs clang ==="
diff /tmp/lrb.out /tmp/lrc.out && echo IDENTICAL
echo "=== TAIL ==="
tail -60 /tmp/lrb.out
