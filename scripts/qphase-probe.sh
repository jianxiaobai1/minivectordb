#!/usr/bin/env bash
# Loads N rows once, then times the query phase separately, and reports the
# distance kernel actually selected.
BIN=${BIN:-/root/mvd-build/build/minivectordb}
N=${N:-10000}
Q=${Q:-1000}
D=/tmp/mvd-qphase
rm -rf $D; mkdir -p $D

python3 - "$N" "$Q" <<'PY' > /tmp/mvd-qphase.sql
import random, sys
n, q = int(sys.argv[1]), int(sys.argv[2])
dim = 128
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
for i in range(q):
    v = "[" + ",".join("%.4f" % rng.uniform(-1, 1) for _ in range(dim)) + "]"
    where = " WHERE cat = 'cat%02d'" % (i % 20) if i % 2 == 0 else ""
    print("SELECT id, L2_DISTANCE(emb, %s) AS d FROM bench%s ORDER BY d LIMIT 10;" % (v, where))
PY

SQL=/tmp/mvd-qphase.sql
WRITE_PART=$(grep -n "^SELECT" "$SQL" | head -1 | cut -d: -f1)
head -n $((WRITE_PART - 1)) "$SQL" > /tmp/mvd-w.sql
tail -n +$WRITE_PART "$SQL" > /tmp/mvd-q.sql
echo "write sql bytes: $(stat -c %s /tmp/mvd-w.sql)   query sql bytes: $(stat -c %s /tmp/mvd-q.sql)"

start=$(date +%s.%N)
$BIN --data-dir=$D < /tmp/mvd-w.sql > /dev/null 2>/tmp/mvd-w.err
end=$(date +%s.%N)
printf "write phase : %.2fs   %s\n" "$(echo "$end - $start" | bc)" "$(head -1 /tmp/mvd-w.err)"

start=$(date +%s.%N)
$BIN --data-dir=$D < /tmp/mvd-q.sql > /tmp/mvd-q.out 2>/tmp/mvd-q.err
end=$(date +%s.%N)
printf "query phase : %.2fs for %s queries  (%s result lines)\n" \
  "$(echo "$end - $start" | bc)" "$Q" "$(wc -l < /tmp/mvd-q.out)"
head -1 /tmp/mvd-q.err
