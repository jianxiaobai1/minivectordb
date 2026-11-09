#!/usr/bin/env bash
# Performance benchmark. Needs root and cgroup v2 with io+memory controllers,
# because T/B/M are measured the way the platform measures them.
# usage: run-bench.sh [bench.py options...]
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$here")"
export BIN="${MVD_BIN:-$HOME/mvd-build/build/minivectordb}"
S=/tmp/mvd-bench.py
tr -d '\r' < "$ROOT/bench/bench.py" > "$S"
# -u so progress is visible while a round is still running.
python3 -u "$S" "$@"
