#!/usr/bin/env bash
# Sweeps IVF nprobe / nlist and reports recall@10 and query time, so the index
# configuration is chosen against the 0.95 gate rather than guessed.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S=/tmp/mvd-bench.py
tr -d '\r' < "$(dirname "$here")/bench/bench.py" > "$S"
export BIN="${MVD_BIN:-$HOME/mvd-build/build/minivectordb}"

printf "%-7s %-7s %-10s %-12s %-12s %-9s\n" nprobe nlist T_query recall_un recall_filt E
for cfg in "8 0" "16 0" "24 0" "32 0" "48 0" "64 0" "96 0" "128 0" "32 50" "48 50" "32 100" "64 100"; do
  set -- $cfg
  np=$1; nl=$2
  line=$(python3 -u "$S" --rounds 1 --mode ann --baseline /tmp/mvd-baseline.json \
           --extra "--ivf-nprobe=$np --ivf-nlist=$nl" 2>&1 | grep 'T_query=')
  t=$(echo "$line" | sed -n 's/.*T_query=\([0-9.]*\)s.*/\1/p')
  r1=$(echo "$line" | sed -n 's/.*recall \([0-9.]*\)\/\([0-9.]*\).*/\1/p')
  r2=$(echo "$line" | sed -n 's/.*recall \([0-9.]*\)\/\([0-9.]*\).*/\2/p')
  e=$(echo "$line" | sed -n 's/.*E=\([0-9.]*\).*/\1/p')
  printf "%-7s %-7s %-10s %-12s %-12s %-9s\n" "$np" "$nl" "$t" "$r1" "$r2" "$e"
done
