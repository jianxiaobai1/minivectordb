#!/usr/bin/env python3
"""mysqltest-style driver for MiniVectorDB.

Reads a case file tests/t/<name>.test, feeds SQL statements one by one to a
minivectordb subprocess (fresh temporary --data-dir), captures stdout and
compares it line-by-line against tests/r/<name>.result.

Directive lines (start with '#', handled by the driver, never sent to the
engine; '# foo' where foo is not a directive name is a plain comment):
    # expect-error NAME     next statement must answer ERROR NAME: ...
    # restart [flags...]    clean shutdown + start again, same data dir
    # kill [flags...]       SIGKILL + start again, same data dir
    # mode ann|exact [fl]   restart with --vector-mode=<m> (plus extra flags)
    # gen-vectors T N D SEED  insert N rows into a 2-column (id BIGINT PK,
    #                        v VECTOR(D)) table; ids 1..N; uniform float32
    #                        components drawn from SEED
    # recall K THRESHOLD    run 16 K-way TopK queries in exact and ann mode,
    #                        assert Recall@K >= THRESHOLD (rule 16)
    # expect-ann-active K   after `# mode ann ...`, assert the CLI vector mode
    #                        observably changes search behaviour (rule 15)
    # timeout SEC           per-statement response deadline (default 10)

SQL accumulates until a line ends with ';' so multi-line statements work.
Responses are framed with --ready-marker, so the driver never guesses where a
reply ends. Live rule assertions (# expect-error, # recall, #
expect-ann-active) fail the run even in --record mode, and independently of
the baseline diff.

Usage:
    python3 mysqltest.py <case> [--record] [--binary PATH] [--keep]
Exit 0 iff actual output equals the baseline and no live assertion failed.
"""

import argparse
import difflib
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import queue
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BIN = os.environ.get("MVD_BIN", "/root/mvd-build/build/minivectordb")
MARKER = "__MVD_READY_9f3c7a__"
DEFAULT_TIMEOUT = 10.0
START_TIMEOUT = 60.0   # first answer after a (re)start may rebuild indexes

DIRECTIVES = ("expect-error", "restart", "kill", "mode", "gen-vectors",
              "recall", "expect-ann-active", "timeout")


class EngineTimeout(Exception):
    pass


class EngineDead(Exception):
    pass


class _GenAbort(Exception):
    pass


def decode(b):
    return b.decode("utf-8", errors="replace")


class Proc:
    """One minivectordb process speaking the stdin/stdout SQL protocol."""

    def __init__(self, binary, datadir, flags):
        cmd = [binary, "--data-dir=" + datadir, "--ready-marker=" + MARKER] + list(flags)
        self.p = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        self.outq = queue.Queue()
        self.errbuf = bytearray()
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._errdrain, daemon=True).start()
        self._next_timeout = START_TIMEOUT

    def _reader(self):
        try:
            for line in self.p.stdout:
                self.outq.put(line.rstrip(b"\n"))
        finally:
            self.outq.put(None)  # EOF sentinel

    def _errdrain(self):
        for chunk in iter(lambda: self.p.stderr.read(4096), b""):
            self.errbuf += chunk

    def stderr_tail(self, n=400):
        return decode(bytes(self.errbuf[-n:]))

    def request(self, stmt, timeout=None):
        """Send one complete statement, return response lines (marker excluded)."""
        if self.p.poll() is not None:
            raise EngineDead("engine exited (code %s) before answering; stderr: %s"
                             % (self.p.returncode, self.stderr_tail()))
        try:
            self.p.stdin.write((stmt + "\n").encode("utf-8"))
            self.p.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise EngineDead("write failed: %s; stderr: %s" % (exc, self.stderr_tail()))
        t = timeout if timeout is not None else self._next_timeout
        self._next_timeout = DEFAULT_TIMEOUT
        deadline = time.monotonic() + t
        lines = []
        marker = MARKER.encode()
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise EngineTimeout("no complete response within %ss for: %s" % (t, stmt[:120]))
            try:
                item = self.outq.get(timeout=left)
            except queue.Empty:
                raise EngineTimeout("no complete response within %ss for: %s" % (t, stmt[:120]))
            if item is None:
                raise EngineDead("engine closed stdout while answering [%s]; stderr: %s"
                                 % (stmt[:120], self.stderr_tail()))
            if item == marker:
                return lines
            lines.append(item)

    def shutdown_clean(self, timeout=20.0):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            return self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise EngineTimeout("engine did not shut down on stdin EOF within %ss" % timeout)

    def kill9(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()


class CaseRunner:
    def __init__(self, name, binary, timeout, keep, record):
        self.name = name
        self.binary = binary
        self.keep = keep
        self.record = record
        self.test_path = os.path.join(HERE, "t", name + ".test")
        self.result_path = os.path.join(HERE, "r", name + ".result")
        self.out = []           # recorded output lines (bytes)
        self.failures = []      # live rule violations (independent of baseline)
        self.timeout = timeout
        self.aborted = False
        self.proc = None
        self.mode_flags = []
        self.last_mode = "exact"
        self.gen_meta = None    # (table, n, dim, seed)
        self.base_dir = tempfile.mkdtemp(prefix="mvdcase_")
        self.datadir = os.path.join(self.base_dir, "data")
        os.makedirs(self.datadir, exist_ok=True)

    # -- process management --------------------------------------------------
    def start(self, extra_flags=None):
        flags = list(self.mode_flags) + list(extra_flags or [])
        self.proc = Proc(self.binary, self.datadir, flags)

    def stop_clean(self):
        try:
            rc = self.proc.shutdown_clean()
            if rc not in (0, -15):
                self.failures.append("engine exited with code %s on clean shutdown; stderr: %s"
                                     % (rc, self.proc.stderr_tail()))
        finally:
            self.proc = None

    def stop_kill(self):
        self.proc.kill9()
        self.proc = None

    def do_restart(self, extra=None, kill=False):
        if kill:
            self.stop_kill()
        else:
            try:
                self.stop_clean()
            except EngineTimeout as exc:
                self.failures.append("RESTART-HANG: %s" % exc)
                if self.proc:
                    self.stop_kill()
        self.start(extra)

    # -- statement plumbing ---------------------------------------------------
    def send(self, stmt, expect=None, record=True):
        try:
            lines = self.proc.request(stmt, timeout=self.timeout)
        except EngineTimeout as exc:
            self.failures.append("TIMEOUT: %s" % exc)
            self.aborted = True
            return None
        except EngineDead as exc:
            self.failures.append("DEAD: %s" % exc)
            self.aborted = True
            return None
        if record:
            self.out.extend(lines)
        if expect is not None:
            got = lines[0] if lines else b"<no output>"
            want = ("ERROR %s:" % expect).encode()
            if not got.startswith(want):
                self.failures.append("expect-error %s failed for [%s]: got '%s'"
                                     % (expect, stmt.strip()[:80], decode(got)))
        return lines

    # -- helpers for procedural directives ------------------------------------
    def query_ids(self, table, vec_col, comps, k):
        stmt = ("SELECT id, L2_DISTANCE(%s, '[%s]') AS d FROM %s ORDER BY d ASC LIMIT %d;"
                % (vec_col, ",".join("%.7g" % c for c in comps), table, k))
        lines = self.proc.request(stmt, timeout=self.timeout)
        ids = [int(ln.split(b"\t")[0]) for ln in lines]
        return lines, ids

    def generated_queries(self, m=16):
        table, n, dim, seed = self.gen_meta
        rng = random.Random(seed)
        for _ in range(n * dim):
            rng.random()  # replay the insertion stream
        return table, dim, [[rng.random() for _ in range(dim)] for _ in range(m)]

    def do_gen_vectors(self, table, n, dim, seed):
        rng = random.Random(seed)
        batch = []
        try:
            for i in range(1, n + 1):
                comps = ",".join("%.7g" % rng.random() for _ in range(dim))
                batch.append("(%d,[%s])" % (i, comps))
                if len(batch) == 40:
                    self._gen_insert("INSERT INTO %s VALUES %s;" % (table, ",".join(batch)))
                    batch = []
            if batch:
                self._gen_insert("INSERT INTO %s VALUES %s;" % (table, ",".join(batch)))
        except _GenAbort:
            self.aborted = True
            return
        except EngineTimeout as exc:
            self.failures.append("TIMEOUT during gen-vectors: %s" % exc)
            self.aborted = True
            return
        except EngineDead as exc:
            self.failures.append("DEAD during gen-vectors: %s" % exc)
            self.aborted = True
            return
        self.gen_meta = (table, n, dim, seed)

    def _gen_insert(self, stmt):
        lines = self.proc.request(stmt, timeout=self.timeout)
        if not lines or not re.match(rb"^OK \d+$", lines[0]):
            self.failures.append("gen-vectors insert rejected: %r"
                                 % decode(lines[0] if lines else "<no output>"))
            raise _GenAbort()

    def do_recall(self, k, threshold):
        if not self.gen_meta:
            self.failures.append("# recall without a preceding # gen-vectors")
            return
        table, dim, queries = self.generated_queries()
        try:
            self.proc.request("SET vector_mode = exact;", timeout=self.timeout)
            exact_sets = []
            for q in queries:
                _, ids = self.query_ids(table, "v", q, k)
                exact_sets.append(set(ids))
            self.proc.request("SET vector_mode = ann;", timeout=self.timeout)
            total = 0.0
            worst = 1.0
            valid = set(range(1, self.gen_meta[1] + 1))
            ann_dupes = ann_invalid = ann_toolong = 0
            for q, es in zip(queries, exact_sets):
                lines, ids = self.query_ids(table, "v", q, k)
                if len(lines) > k:
                    ann_toolong += 1
                if len(set(ids)) != len(ids):
                    ann_dupes += 1
                if not set(ids) <= valid:
                    ann_invalid += 1
                ratio = len(set(ids) & es) / float(k)
                total += ratio
                worst = min(worst, ratio)
            mean = total / len(queries)
            line = ("RECALL@%d queries=%d mean=%.3f min=%.3f dupes=%d invalid=%d overK=%d"
                    % (k, len(queries), mean, worst, ann_dupes, ann_invalid, ann_toolong))
            self.out.append(line.encode())
            if ann_dupes or ann_invalid or ann_toolong:
                self.failures.append("ann results violate rule 16 (unique/valid/<=K): %s" % line)
            if mean < threshold:
                self.failures.append("Recall@%d mean %.3f below threshold %.3f" % (k, mean, threshold))
            self.proc.request("SET vector_mode = exact;", timeout=self.timeout)
        except EngineTimeout as exc:
            self.failures.append("TIMEOUT during recall: %s" % exc)
            self.aborted = True
        except EngineDead as exc:
            self.failures.append("DEAD during recall: %s" % exc)
            self.aborted = True

    def do_expect_ann_active(self, k):
        # The engine was started with --vector-mode=ann and the process has not
        # received any `SET vector_mode` since that restart. If the CLI switch is
        # honoured, plain queries are approximate; flipping to exact via SET must
        # then change at least some answers (with the test's tiny nprobe).
        if self.last_mode != "ann":
            self.failures.append("# expect-ann-active needs a preceding `# mode ann`")
            return
        if not self.gen_meta:
            self.failures.append("# expect-ann-active without # gen-vectors")
            return
        table, dim, queries = self.generated_queries()
        try:
            p_sets = [self.query_ids(table, "v", q, k)[1] for q in queries]
            self.proc.request("SET vector_mode = exact;", timeout=self.timeout)
            e_sets = [self.query_ids(table, "v", q, k)[1] for q in queries]
            if p_sets == e_sets:
                self.failures.append(
                    "--vector-mode=ann had no observable effect: every query matched the "
                    "exact result set (the CLI flag is never applied to the engine)")
            self.proc.request("SET vector_mode = ann;", timeout=self.timeout)
        except EngineTimeout as exc:
            self.failures.append("TIMEOUT during expect-ann-active: %s" % exc)
            self.aborted = True
        except EngineDead as exc:
            self.failures.append("DEAD during expect-ann-active: %s" % exc)
            self.aborted = True

    # -- main loop --------------------------------------------------------------
    def run(self):
        with open(self.test_path, "rb") as fh:
            text = decode(fh.read())
        pending_err = None
        buf = []
        self.start()
        for raw in text.splitlines():
            if self.aborted:
                break
            line = raw.rstrip("\r")
            s = line.strip()
            if s.startswith("#"):
                if buf:
                    self.failures.append("directive inside unfinished statement: %s" % s)
                    break
                parts = s[1:].split()
                if not parts or parts[0] not in DIRECTIVES:
                    continue  # plain comment
                d, args = parts[0], parts[1:]
                # A '# restart ...' / '# kill ...' prose comment is not a
                # directive: real flags must start with '--'.
                if d in ("restart", "kill") and any(not a.startswith("--") for a in args):
                    continue
                if d == "mode" and (len(args) < 1 or args[0] not in ("ann", "exact")):
                    continue
                if d == "expect-error" and not (len(args) == 1 and re.match(r"^[A-Z_]+$", args[0])):
                    continue
                if d in ("gen-vectors", "recall", "expect-ann-active", "timeout") and not args:
                    continue
                if d == "expect-error":
                    pending_err = args[0]
                elif d == "restart":
                    self.do_restart(args, kill=False)
                    pending_err = None
                elif d == "kill":
                    self.do_restart(args, kill=True)
                    pending_err = None
                elif d == "mode":
                    m = args[0]
                    if m not in ("ann", "exact"):
                        self.failures.append("unknown mode: %s" % m)
                        continue
                    self.mode_flags = ["--vector-mode=" + m] + list(args[1:])
                    self.last_mode = m
                    self.do_restart()
                    pending_err = None
                elif d == "gen-vectors":
                    self.do_gen_vectors(args[0], int(args[1]), int(args[2]), int(args[3]))
                elif d == "recall":
                    self.do_recall(int(args[0]), float(args[1]))
                elif d == "expect-ann-active":
                    self.do_expect_ann_active(int(args[0]))
                elif d == "timeout":
                    self.timeout = float(args[0])
                continue
            if not buf and (s == "" or s.startswith("--")):
                continue  # blank / SQL comment line between statements
            buf.append(line)
            if s.endswith(";"):
                stmt = "\n".join(buf).strip()
                buf = []
                self.send(stmt, expect=pending_err)
                pending_err = None
        if buf:
            self.failures.append("file ends inside an unfinished statement")
        if not self.aborted and self.proc is not None:
            try:
                self.stop_clean()
            except EngineTimeout as exc:
                self.failures.append("SHUTDOWN: %s" % exc)

    # -- result handling ---------------------------------------------------------
    def finish(self):
        if self.record:
            os.makedirs(os.path.dirname(self.result_path), exist_ok=True)
            with open(self.result_path, "wb") as fh:
                for ln in self.out:
                    fh.write(ln + b"\n")
            if self.failures:
                print("CASE %s: recorded %d lines WITH live assertion failures:"
                      % (self.name, len(self.out)))
                for f in self.failures:
                    print("  ! %s" % f)
                return 1
            print("CASE %s: recorded %d lines -> %s" % (self.name, len(self.out), self.result_path))
            return 0
        if not os.path.exists(self.result_path):
            print("CASE %s: no baseline at %s (run with --record)" % (self.name, self.result_path))
            for f in self.failures:
                print("  ! %s" % f)
            return 2
        with open(self.result_path, "rb") as fh:
            expected = fh.read().splitlines()
        actual = self.out
        ok = expected == actual
        if not ok:
            diff = list(difflib.unified_diff(
                [decode(x) for x in expected],
                [decode(x) for x in actual],
                fromfile=self.result_path, tofile="<actual>", lineterm=""))
            print("CASE %s: OUTPUT DIFF (%d lines)" % (self.name, len(diff)))
            for ln in diff[:60]:
                print("  " + ln)
            if len(diff) > 60:
                print("  ... %d more diff lines" % (len(diff) - 60))
        if self.failures:
            print("CASE %s: %d ASSERTION FAILURE(S)" % (self.name, len(self.failures)))
            for f in self.failures:
                print("  ! %s" % f)
        if ok and not self.failures:
            print("CASE %s: PASS" % self.name)
            return 0
        print("CASE %s: FAIL" % self.name)
        return 1


def main():
    ap = argparse.ArgumentParser(description="MiniVectorDB mysqltest driver")
    ap.add_argument("case", help="case name without suffix (tests/t/<case>.test)")
    ap.add_argument("--record", action="store_true", help="write baseline instead of comparing")
    ap.add_argument("--binary", default=DEFAULT_BIN)
    ap.add_argument("--keep", action="store_true", help="keep the temporary data dir")
    ap.add_argument("--timeout", type=float, default=None)
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print("driver: binary not found: %s" % args.binary, file=sys.stderr)
        return 2
    if not os.path.exists(os.path.join(HERE, "t", args.case + ".test")):
        print("driver: test file not found for case %s" % args.case, file=sys.stderr)
        return 2

    runner = CaseRunner(args.case, args.binary, args.timeout or DEFAULT_TIMEOUT,
                        args.keep, args.record)
    try:
        runner.run()
        rc = runner.finish()
    finally:
        if runner.proc and runner.proc.p.poll() is None:
            runner.proc.kill9()
        if not args.keep:
            shutil.rmtree(runner.base_dir, ignore_errors=True)
        else:
            print("data dir kept: %s" % runner.datadir)
    return rc


if __name__ == "__main__":
    sys.exit(main())
