#!/usr/bin/env bash
for f in /tmp/mvd-exact.json /tmp/mvd-ann.json /tmp/mvd-baseline.json; do
  echo "===== $f ====="
  if [ -f "$f" ]; then cat "$f"; else echo "(missing)"; fi
  echo
done
