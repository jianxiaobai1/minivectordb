#!/usr/bin/env bash
# Builds MiniVectorDB on native Linux filesystem for fast I/O, from sources
# that live wherever the repo is checked out. Works under WSL and any Linux.
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$here")"
BUILD="${MVD_BUILD_DIR:-$HOME/mvd-build}"

mkdir -p "$BUILD"
# rsync rather than building in place: on WSL the repo is often a /mnt/c mount
# where every compile stat() crosses the 9P boundary.
rsync -a --delete --exclude 'build/' --exclude '.git/' "$ROOT/" "$BUILD/"
cd "$BUILD"
cmake -S . -B build -DCMAKE_BUILD_TYPE="${MVD_BUILD_TYPE:-Release}" > /dev/null
cmake --build build -j"$(nproc)" 2>&1 | grep -E "error:|Error [0-9]|undefined reference" | head -40
test -x build/minivectordb && echo "built: $BUILD/build/minivectordb"
