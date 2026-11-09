#!/usr/bin/env bash
# Times the write phase at increasing scales to find a complexity cliff.
BIN=${BIN:-/root/mvd-build/build/minivectordb}
gen() {
  local rows=$1
  python3 - "$rows" <<'PY'
import random, sys
n = int(sys.argv[1]); dim = 128
rng = random.Random(1)
print("CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(128));")
for s in range(0, n, 100):
    vals = []
    for i in range(s, min(s + 100, n)):
        v = "[" + ",".join("%.4f" % rng.uniform(-1, 1) for _ in range(dim)) + "]"
        vals.append("(%d,'cat%02d',%s)" % (i + 1, i % 20, v))
    print("BEGIN;")
    print("INSERT INTO bench VALUES " + ",".join(vals) + ";")
    print("COMMIT;")
PY
}
for n in 500 1000 2000 4000; do
  D=/tmp/mvd-scale-$n; rm -rf $D; mkdir -p $D
  gen $n > /tmp/mvd-scale-$n.sql
  sz=$(stat -c %s /tmp/mvd-scale-$n.sql)
  start=$(date +%s.%N)
  $BIN --data-dir=$D < /tmp/mvd-scale-$n.sql > /dev/null 2>&1
  end=$(date +%s.%N)
  printf "rows=%-6s sql=%-9sB elapsed=%.2fs  heap=%sB wal=%sB\n" "$n" "$sz" \
    "$(echo "$end - $start" | bc)" "$(stat -c %s $D/t1.heap 2>/dev/null)" "$(stat -c %s $D/mvd.wal 2>/dev/null)"
done
