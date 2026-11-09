#!/usr/bin/env bash
# Validates the Dockerfile's build steps without needing a registry.
#
# The image build itself could not be verified where Docker Hub is unreachable,
# so this runs each instruction's command on the host (also Ubuntu 24.04) and
# checks the two things most likely to break the graded run: that the install
# layout the ENTRYPOINT points at really exists, and that the binary's shared
# library needs are satisfiable by a runtime image that installs nothing.
set -u
SRC="${1:-/mnt/c/Users/ZhuanZ1/Documents/Qoder/2026-11-09/5e42768a/minivectordb}"
STAGE=/tmp/mvd-stage
rm -rf "$STAGE"; mkdir -p "$STAGE"
cd "$SRC" || exit 1

echo "=== build stage commands ==="
command -v cmake >/dev/null || { echo "cmake missing"; exit 1; }
cmake -S . -B /tmp/mvd-dockerbuild -DCMAKE_BUILD_TYPE=Release > /tmp/mvd-dockerbuild.log 2>&1 || { tail -20 /tmp/mvd-dockerbuild.log; exit 1; }
cmake --build /tmp/mvd-dockerbuild -j"$(nproc)" >> /tmp/mvd-dockerbuild.log 2>&1 || { grep -E 'error:' /tmp/mvd-dockerbuild.log | head; exit 1; }
DESTDIR=$STAGE cmake --install /tmp/mvd-dockerbuild --prefix /usr/local >> /tmp/mvd-dockerbuild.log 2>&1 || { tail -10 /tmp/mvd-dockerbuild.log; exit 1; }
echo "installed tree:"
find "$STAGE" -type f | sed "s|$STAGE||"

echo
echo "=== runtime stage: does the ENTRYPOINT path exist? ==="
BINPATH="$STAGE/usr/local/bin/minivectordb"
if [ -x "$BINPATH" ]; then echo "  ok $BINPATH"; else echo "  FAIL missing"; exit 1; fi

echo
echo "=== shared libraries the runtime image must provide ==="
ldd "$BINPATH"

echo
echo "=== the copy the runtime stage would make, then run it ==="
# Mirrors `COPY --from=build /stage/usr/local /usr/local`: an explicit
# destination, which is not the same as `cp -a src dst/` (that would land the
# contents in dst/local).
rm -rf /tmp/mvd-runtime-root /tmp/mvd-runtime-data
mkdir -p /tmp/mvd-runtime-root/usr/local
cp -a "$STAGE/usr/local/." /tmp/mvd-runtime-root/usr/local/
printf 'CREATE TABLE d (id BIGINT PRIMARY KEY, v VECTOR(2));\nINSERT INTO d VALUES (1,[0,0]),(2,[1,0]);\nSELECT id, L2_DISTANCE(v, [1,0]) AS dist FROM d ORDER BY dist LIMIT 2;\n' \
  | /tmp/mvd-runtime-root/usr/local/bin/minivectordb --data-dir=/tmp/mvd-runtime-data 2>&1 | tail -6
echo "--- second process on the same data dir (the /data volume contract) ---"
printf 'SELECT id FROM d ORDER BY id;\n' \
  | /tmp/mvd-runtime-root/usr/local/bin/minivectordb --data-dir=/tmp/mvd-runtime-data 2>&1 | tail -4

echo
echo "=== ldd must not need libstdc++/libgcc (runtime stage installs nothing) ==="
if ldd "$BINPATH" | grep -E 'libstdc\+\+|libgcc_s' ; then
  echo "  FAIL: runtime stage would need apt packages that the Dockerfile does not install"
  exit 1
else
  echo "  ok: statically linked, only glibc remains"
fi
