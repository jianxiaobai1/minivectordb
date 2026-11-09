#!/usr/bin/env python3
"""MiniVectorDB performance benchmark, measured the way the competition measures it.

Reproduces section 3.3 of the task statement:

  * 10,000 records, 128 dimensions, batches of 100 rows committed individually
  * 1,000 TopK queries with K = 10, half of them with an equality predicate on
    a scalar column
  * T = wall clock, B = cgroup v2 io.stat rbytes+wbytes, M = cgroup v2
    memory.peak (the whole container, page cache included)
  * three rounds, the median of the composite metric E is used for ranking

E is computed against a baseline file so the number is comparable across
optimisation steps; the official baseline is published with the test package.

Usage (inside WSL, as root):
    python3 bench.py [--rounds 3] [--mode ann|exact] [--baseline base.json]
                     [--record-baseline base.json] [--out results.json]
"""
import argparse
import json
import os
import queue
import random
import shutil
import statistics
import subprocess
import sys
import threading
import time

CGROOT = "/sys/fs/cgroup"
CG = CGROOT + "/mvdbench"
BIN = os.environ.get("BIN", "/root/mvd-build/build/minivectordb")

# Scale can be dialed down for a quick sanity run; the competition numbers are
# 10,000 x 128 with 1,000 queries, which is also the default.
N_ROWS = int(os.environ.get("MVD_BENCH_ROWS", 10000))
DIM = int(os.environ.get("MVD_BENCH_DIM", 128))
BATCH = int(os.environ.get("MVD_BENCH_BATCH", 100))
N_QUERIES = int(os.environ.get("MVD_BENCH_QUERIES", 1000))
TOPK = int(os.environ.get("MVD_BENCH_TOPK", 10))
N_CATS = int(os.environ.get("MVD_BENCH_CATS", 20))
WARMUP = int(os.environ.get("MVD_BENCH_WARMUP", 100))

EPS_T = 0.001       # 1 ms expressed in seconds
EPS_B = 1 << 20     # 1 MiB
EPS_M = 1 << 20     # 1 MiB


# ---------------------------------------------------------------------------
# cgroup helpers
# ---------------------------------------------------------------------------

def cgroup_available():
    try:
        with open(CGROOT + "/cgroup.controllers") as f:
            ctrls = f.read().split()
        return "io" in ctrls and "memory" in ctrls
    except OSError:
        return False


def cgroup_setup(mem_bytes, cpu_quota_us, cpu_period_us):
    if os.path.exists(CG + "/cgroup.procs") and os.listdir(CGROOT) and False:
        pass
    try:
        with open(CGROOT + "/cgroup.subtree_control", "w") as f:
            f.write("+io +memory")
    except OSError as e:
        raise SystemExit("cannot enable controllers (need root, cgroup v2): %s" % e)
    os.makedirs(CG, exist_ok=True)
    with open(CG + "/memory.max", "w") as f:
        f.write(str(mem_bytes))
    with open(CG + "/cpu.max", "w") as f:
        f.write("%d %d" % (cpu_quota_us, cpu_period_us))
    # swap must stay off, as the task requires
    try:
        with open(CG + "/memory.swap.max", "w") as f:
            f.write("0")
    except OSError:
        pass


def cgroup_teardown():
    try:
        for pid in open(CG + "/cgroup.procs").read().split():
            os.kill(int(pid), 9)
    except (OSError, ValueError):
        pass
    try:
        os.rmdir(CG)
    except OSError:
        pass


def io_bytes():
    total = 0
    try:
        with open(CG + "/io.stat") as f:
            for line in f:
                parts = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
                total += int(parts.get("rbytes", 0)) + int(parts.get("wbytes", 0))
    except OSError:
        pass
    return total


def mem_peak():
    try:
        with open(CG + "/memory.peak") as f:
            v = f.read().strip()
        return 0 if v == "max" else int(v)
    except (OSError, ValueError):
        return 0


def mem_peak_reset():
    try:
        with open(CG + "/memory.peak", "w") as f:
            f.write("0")
    except OSError:
        pass


# ---------------------------------------------------------------------------
# workload
# ---------------------------------------------------------------------------

def read_line(proc, why, timeout=180.0):
    """readline with a deadline, so a stall names the phase instead of hanging."""
    import select
    r, _, _ = select.select([proc.stdout], [], [], timeout)
    if not r:
        raise RuntimeError("timed out waiting for output during %s" % why)
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("process died during %s" % why)
    return line


class Feeder:
    """Writes SQL to the child's stdin from a background thread.

    The write phase is ~10 MB of SQL and the query phase ~700 KB, both far
    larger than a pipe buffer. Writing them from the thread that is also
    waiting for the child's stdout would deadlock: the child blocks filling its
    output pipe while the parent blocks filling the input one.
    """

    def __init__(self, stream):
        self.stream = stream
        self.q = queue.Queue()
        self.err = None
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    def _loop(self):
        while True:
            chunk = self.q.get()
            try:
                if chunk is None:
                    return
                # A raw (bufsize=0) pipe write may accept only part of a large
                # buffer and return the count; ignoring that silently drops
                # everything past the 64 KiB pipe capacity, which left the
                # engine waiting on a half-received statement.
                off = 0
                while off < len(chunk):
                    n = self.stream.write(chunk[off:])
                    if not n:
                        raise RuntimeError("stdin write stalled")
                    off += n
                self.stream.flush()
            except (BrokenPipeError, ValueError, RuntimeError) as e:
                self.err = e
                return
            finally:
                self.q.task_done()

    def send(self, text):
        self.q.put(text.encode())

    def wait(self):
        self.q.join()
        if self.err:
            raise RuntimeError("stdin feeder failed: %s" % self.err)

    def close(self):
        self.q.put(None)
        self.t.join(timeout=5)


def gen_data(seed):
    rng = random.Random(seed)
    cats = ["cat%02d" % i for i in range(N_CATS)]
    rows = []
    for i in range(1, N_ROWS + 1):
        v = [round(rng.uniform(-1.0, 1.0), 4) for _ in range(DIM)]
        rows.append((i, rng.choice(cats), v))
    return rows


def gen_queries(seed):
    rng = random.Random(seed ^ 0x5EED)
    qs = []
    for i in range(N_QUERIES):
        v = [round(rng.uniform(-1.0, 1.0), 4) for _ in range(DIM)]
        filtered = (i % 2 == 0)
        cat = rng.choice(["cat%02d" % j for j in range(N_CATS)]) if filtered else None
        qs.append((cat, v))
    return qs


def vec_text(v):
    """Bare vector literal: [0.6,0.7]."""
    return "[" + ",".join(repr(x) for x in v) + "]"


def vec_text_quoted(v):
    """The string form the task also accepts: '[0.6,0.7]'.

    Writes use the bare form and queries use the quoted one so both syntaxes
    are exercised by the same benchmark run.
    """
    return "'[" + ",".join(repr(x) for x in v) + "]'"


def write_phase(rows):
    out = ["CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));" % DIM]
    for s in range(0, len(rows), BATCH):
        chunk = rows[s:s + BATCH]
        out.append("BEGIN;")
        vals = ",".join("(%d, '%s', %s)" % (r[0], r[1], vec_text(r[2])) for r in chunk)
        out.append("INSERT INTO bench VALUES " + vals + ";")
        out.append("COMMIT;")
    return "\n".join(out) + "\n"


def query_phase(queries, count):
    out = []
    for cat, v in queries[:count]:
        where = (" WHERE cat = '%s'" % cat) if cat else ""
        out.append("SELECT id, L2_DISTANCE(emb, %s) AS d FROM bench%s ORDER BY d LIMIT %d;"
                   % (vec_text_quoted(v), where, TOPK))
    return "\n".join(out) + "\n"


def run_one(data_dir, rows, queries, mode, extra_args, index=True):
    """One measured round. Returns the raw T/B/M triples."""
    shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(data_dir)
    cgroup_teardown()
    cgroup_setup(MEM_LIMIT, CPU_QUOTA, CPU_PERIOD)
    os.makedirs(data_dir, exist_ok=True)

    args = [BIN, "--data-dir=" + data_dir, "--vector-mode=" + mode] + extra_args
    env = dict(os.environ)
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, env=env, bufsize=0,
                            start_new_session=True)
    with open(CG + "/cgroup.procs", "w") as f:
        f.write(str(proc.pid))
    feed = Feeder(proc.stdin)

    # --- write phase ---
    b0 = io_bytes()
    t0 = time.monotonic()
    feed.send(write_phase(rows))
    # The last line of the write phase is a COMMIT; wait until its OK arrives so
    # the timer covers durability, not just submission.
    # BEGIN, INSERT and COMMIT each answer with an OK line, so a batch is three
    # of them; the timer must not stop before the last COMMIT is acknowledged.
    expect_ok = 1 + (len(rows) // BATCH) * 3
    seen = 0
    while seen < expect_ok:
        line = read_line(proc, "phase")
        if not line:
            raise RuntimeError("process died during write phase")
        if line.startswith(b"OK"):
            seen += 1
    t_write = time.monotonic() - t0
    b_write = io_bytes() - b0
    m_write = mem_peak()

    # --- index preparation + warmup (not in T, but in B and M) ---
    if index:
        feed.send("CREATE VECTOR INDEX vi ON bench(emb);\n")
        while True:
            line = read_line(proc, "phase")
            if not line or line.startswith(b"OK"):
                break
    feed.send(query_phase(queries, WARMUP))
    want = WARMUP * TOPK
    got = 0
    while got < want:
        line = read_line(proc, "phase")
        if not line:
            raise RuntimeError("process died during warmup")
        if line.startswith(b"ERROR"):
            raise RuntimeError("warmup error: " + line.decode())
        got += 1
    m_after_warm = mem_peak()

    # --- query phase ---
    b1 = io_bytes()
    t1 = time.monotonic()
    feed.send(query_phase(queries, len(queries)))
    results = []
    cur = []
    want = len(queries) * TOPK
    got = 0
    while got < want:
        line = read_line(proc, "phase")
        if not line:
            raise RuntimeError("process died during query phase")
        s = line.decode().rstrip("\n")
        if s.startswith("ERROR"):
            raise RuntimeError("query error: " + s)
        cur.append(int(s.split("\t")[0]))
        got += 1
        if len(cur) == TOPK:
            results.append(cur)
            cur = []
    t_query = time.monotonic() - t1
    b_query = io_bytes() - b1
    m_query = max(mem_peak(), m_after_warm)

    feed.close()
    try:
        proc.stdin.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    return dict(t_write=t_write, b_write=b_write, m_write=m_write,
                t_query=t_query, b_query=b_query, m_query=m_query,
                results=results)


# ---------------------------------------------------------------------------
# metric
# ---------------------------------------------------------------------------

def f_of_r(r):
    return r / (1.0 + r)


def composite(item, base):
    rt = (base["t"] + EPS_T) / (item["t"] + EPS_T)
    rb = (base["b"] + EPS_B) / (item["b"] + EPS_B)
    rm = (base["m"] + EPS_M) / (item["m"] + EPS_M)
    return 0.6 * f_of_r(rt) + 0.2 * f_of_r(rb) + 0.2 * f_of_r(rm)


def overall(e_write, e_query):
    return (e_write + 2.0 * e_query) / 3.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--mode", default="ann", choices=["ann", "exact"])
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--record-baseline", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-index", action="store_true")
    ap.add_argument("--mem-gib", type=float, default=2.0)
    ap.add_argument("--cpu", type=float, default=2.0)
    ap.add_argument("--extra", default="")
    args = ap.parse_args()

    global MEM_LIMIT, CPU_QUOTA, CPU_PERIOD
    MEM_LIMIT = int(args.mem_gib * (1 << 30))
    CPU_PERIOD = 100000
    CPU_QUOTA = int(args.cpu * CPU_PERIOD)

    if not cgroup_available():
        raise SystemExit("cgroup v2 with io+memory controllers is required; run as root")

    rows = gen_data(20261109)
    queries = gen_queries(20261109)
    extra = args.extra.split() if args.extra else []

    print("generating ground truth with the exact executor ...")
    truth_dir = "/tmp/mvd-truth"
    shutil.rmtree(truth_dir, ignore_errors=True)
    os.makedirs(truth_dir)
    tr = run_one(truth_dir, rows, queries, "exact", extra, index=False)
    truth = tr["results"]
    unfiltered = [i for i, (c, _) in enumerate(queries) if c is None]
    filtered = [i for i, (c, _) in enumerate(queries) if c is not None]

    base_path = args.baseline
    baseline = None
    if base_path and os.path.exists(base_path):
        baseline = json.load(open(base_path))

    rounds = []
    for r in range(args.rounds):
        d = "/tmp/mvd-bench-r%d" % r
        print("round %d (mode=%s) ..." % (r, args.mode))
        res = run_one(d, rows, queries, args.mode, extra, index=not args.no_index)
        rec = dict(t_write=res["t_write"], b_write=res["b_write"], m_write=res["m_write"],
                   t_query=res["t_query"], b_query=res["b_query"], m_query=res["m_query"])
        # recall against the exact ground truth, per group
        def recall(idx_list):
            hits = 0
            tot = 0
            for i in idx_list:
                a = set(res["results"][i][:TOPK])
                t = set(truth[i][:TOPK])
                hits += len(a & t)
                tot += len(t)
            return hits / max(1, tot)
        rec["recall_unfiltered"] = recall(unfiltered)
        rec["recall_filtered"] = recall(filtered)
        if baseline:
            rec["e_write"] = composite({"t": rec["t_write"], "b": rec["b_write"], "m": rec["m_write"]},
                                       baseline["write"])
            rec["e_query"] = composite({"t": rec["t_query"], "b": rec["b_query"], "m": rec["m_query"]},
                                       baseline["query"])
            rec["e"] = overall(rec["e_write"], rec["e_query"])
        for k in ("t_write", "t_query"):
            rec[k] = round(rec[k], 4)
        for k in ("b_write", "m_write", "b_query", "m_query"):
            rec[k] = int(rec[k])
        for k in ("recall_unfiltered", "recall_filtered"):
            rec[k] = round(rec[k], 4)
        rounds.append(rec)
        print("  T_write=%.3fs B_write=%.1fMiB M_write=%.1fMiB | T_query=%.3fs B_query=%.1fMiB "
              "M_query=%.1fMiB | recall %.4f/%.4f%s"
              % (rec["t_write"], rec["b_write"] / 2**20, rec["m_write"] / 2**20,
                 rec["t_query"], rec["b_query"] / 2**20, rec["m_query"] / 2**20,
                 rec["recall_unfiltered"], rec["recall_filtered"],
                 (" | E=%.6f" % rec["e"]) if "e" in rec else ""))

    summary = {}
    if baseline:
        for key in ("e_write", "e_query", "e"):
            vals = sorted(round(x[key], 6) for x in rounds)
            summary[key] = statistics.median(vals)
        summary["rank_metric_6dp"] = round(summary["e"], 6)
    for key in ("t_write", "t_query", "b_query", "m_query", "recall_unfiltered", "recall_filtered"):
        summary[key + "_median"] = statistics.median([x[key] for x in rounds])

    if args.record_baseline:
        json.dump({"write": {"t": summary["t_write_median"], "b": rounds[0]["b_write"],
                             "m": rounds[0]["m_write"]},
                   "query": {"t": summary["t_query_median"], "b": rounds[0]["b_query"],
                             "m": rounds[0]["m_query"]}},
                  open(args.record_baseline, "w"), indent=2)
        print("baseline recorded to", args.record_baseline)

    print(json.dumps(summary, indent=2))
    if args.out:
        json.dump({"rounds": rounds, "summary": summary, "args": vars(args)},
                  open(args.out, "w"), indent=2)
        print("wrote", args.out)
    cgroup_teardown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
