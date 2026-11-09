#!/usr/bin/env bash
# Proves the deliverable is self-contained: clone the branch into a fresh
# directory, build it there, and run the functional suite against that binary.
set -u
# The repo is passed in rather than derived from this file's location, so the
# script keeps working when it is copied or piped somewhere else.
SRC="${1:-}"
if [ -z "$SRC" ]; then
  here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  SRC="$(dirname "$here")"
fi
DEST=/tmp/mvd-clean-clone
rm -rf "$DEST"
git clone -q --branch 2026-competition-round2 "$SRC" "$DEST"
echo "cloned HEAD: $(git -C "$DEST" rev-parse HEAD)"

cd "$DEST" || exit 1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /tmp/clone-cmake.log 2>&1 || {
  tail -20 /tmp/clone-cmake.log; exit 1; }
cmake --build build -j"$(nproc)" > /tmp/clone-build.log 2>&1 || {
  grep -E 'error:' /tmp/clone-build.log | head -20; exit 1; }
echo "built from clean clone: $(ls -l build/minivectordb | awk '{print $5" bytes"}')"

echo "=== functional suite against the cloned build ==="
MVD_BIN="$DEST/build/minivectordb" bash tests/run_all.sh 2>&1 | tail -3
echo "=== crash recovery against the cloned build ==="
MVD_BIN="$DEST/build/minivectordb" bash scripts/run-crash.sh 3 2>&1 | tail -2
