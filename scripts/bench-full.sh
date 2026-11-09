#!/usr/bin/env bash
# Full-scale benchmark: measure the exact executor to get a baseline, then
# measure ann against it, then confirm ann degrades gracefully with no index.
# Needs root (cgroups).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Match the running engine process specifically. A bare "minivectordb" pattern
# also matches this shell, whose path contains that word, and kills the run.
pkill -9 -f 'minivectordb --data-dir' 2>/dev/null
sleep 1
rm -rf /sys/fs/cgroup/mvdbench 2>/dev/null

echo "########## exact (baseline) ##########"
bash "$here/run-bench.sh" --rounds 3 --mode exact --record-baseline /tmp/mvd-baseline.json --out /tmp/mvd-exact.json
echo
echo "########## ann vs baseline ##########"
bash "$here/run-bench.sh" --rounds 3 --mode ann --baseline /tmp/mvd-baseline.json --out /tmp/mvd-ann.json
echo
echo "########## ann with no index ##########"
bash "$here/run-bench.sh" --rounds 1 --mode ann --baseline /tmp/mvd-baseline.json --no-index
