#!/bin/sh
cd /mnt/f/GitHub/LR_JS || exit 1
for f in tests/*.js tests/*.mjs; do
  case "$f" in *tmp_*) continue;; esac
  for bin in /tmp/lrb/bin/lr_js /tmp/lrc/bin/lr_js; do
    "$bin" "$f" > /dev/null 2>&1
    rc=$?
    [ $rc -ge 128 ] || [ $rc -eq 134 ] && echo "ABORT rc=$rc $bin $f"
  done
done
echo SCAN_DONE
