#!/usr/bin/env bash
# Crash-recovery check. Needs no root; it only starts and SIGKILLs the engine.
# usage: run-crash.sh [rounds]
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$here")"
export BIN="${MVD_BIN:-$HOME/mvd-build/build/minivectordb}"
S=/tmp/mvd-crash_test.py
tr -d '\r' < "$ROOT/bench/crash_test.py" > "$S"
python3 "$S" "${1:-6}"
