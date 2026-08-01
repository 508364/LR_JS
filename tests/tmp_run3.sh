#!/bin/sh
cd /mnt/f/GitHub/LR_JS || exit 1
for bin in /tmp/lrb/bin/lr_js /tmp/lrc/bin/lr_js; do
  for i in 1 2 3; do
    echo "--- $bin run $i ---"
    "$bin" tests/atomics_stress_test.js > /tmp/o.txt 2>&1
    echo "exit=$?"
    tail -4 /tmp/o.txt
  done
done
