#!/usr/bin/env bash
# Runs one .sql file through a fresh MiniVectorDB instance.
# usage: run-sql.sh <file.sql> [extra minivectordb args...]
set -e
BIN="${MVD_BIN:-$HOME/mvd-build/build/minivectordb}"
F="$1"; shift || true
D=$(mktemp -d /tmp/mvd-run.XXXXXX)
tr -d '\r' < "$F" > "$D/input.sql"
"$BIN" --data-dir="$D" "$@" < "$D/input.sql"
echo "# data dir kept for inspection: $D"
