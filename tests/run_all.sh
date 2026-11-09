#!/usr/bin/env bash
# Runs every MiniVectorDB test case and prints a summary.
# Usage (from Windows):
#   wsl.exe -d Ubuntu-D -- bash <abs path>/tests/run_all.sh [--record]
# Self-cleans CRLF line endings (Windows-side edits) while keeping the
# original directory for t/ r/ lookups.
set -u
if [ -z "${MVD_TESTS_DIR:-}" ]; then
  MVD_TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
  export MVD_TESTS_DIR
  if grep -q $'\r' "$0" 2>/dev/null; then
    tr -d '\r' < "$0" > /tmp/mvd_run_all_$$.sh
    exec bash /tmp/mvd_run_all_$$.sh "$@"
  fi
fi
cd "$MVD_TESTS_DIR"
PY="${PYTHON:-python3}"
BIN="${MVD_BIN:-/root/mvd-build/build/minivectordb}"
MODE="${1:-}"

if [ ! -f "$BIN" ]; then
  echo "run_all: engine binary not found: $BIN" >&2
  exit 2
fi

pass=0
fail=0
skip=0
failed=""
skipped=""
for tf in t/*.test; do
  name="$(basename "$tf" .test)"
  echo "=== $name ==="
  if [ "$MODE" = "--record" ]; then
    "$PY" "$MVD_TESTS_DIR/mysqltest.py" "$name" --record --binary "$BIN"
  else
    "$PY" "$MVD_TESTS_DIR/mysqltest.py" "$name" --binary "$BIN"
  fi
  rc=$?
  case "$rc" in
    0) pass=$((pass + 1)) ;;
    1) fail=$((fail + 1)); failed="$failed $name" ;;
    2) skip=$((skip + 1)); skipped="$skipped $name" ;;
    *) fail=$((fail + 1)); failed="$failed $name(rc=$rc)" ;;
  esac
  echo
done

total=$((pass + fail + skip))
echo "SUMMARY: $total cases, $pass passed, $fail failed, $skip missing-baseline"
[ -n "$failed" ] && echo "failed:$failed"
[ -n "$skipped" ] && echo "missing baseline:$skipped"
[ "$fail" -eq 0 ] && [ "$skip" -eq 0 ] && exit 0
exit 1
