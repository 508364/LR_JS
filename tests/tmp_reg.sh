#!/bin/sh
cd /mnt/f/GitHub/LR_JS || exit 1
n=0
for f in tests/*.js tests/*.mjs ES5-15.js; do
  case "$f" in *tmp_*) continue;; esac
  new=$(/tmp/lrb/bin/lr_js "$f" 2>&1); nrc=$?
  old=$(/tmp/lrc/bin/lr_js "$f" 2>&1); orc=$?
  if [ "$new" != "$old" ] || [ "$nrc" != "$orc" ]; then
    n=$((n+1))
    echo "DIFF $f  (new rc=$nrc / old rc=$orc)"
  fi
done
echo "total_diff=$n"
