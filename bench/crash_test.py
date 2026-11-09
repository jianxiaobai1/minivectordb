#!/usr/bin/env python3
"""Crash-recovery check for MiniVectorDB.

Reproduces the three cases the task statement fixes:
  * a transaction that already returned COMMIT success must survive;
  * a transaction that never sent COMMIT must not be visible;
  * a transaction whose COMMIT was sent but whose response was never read may
    either fully apply or fully not apply, but must never be partial.

Run inside WSL:  python3 crash_test.py [rounds]
"""
import os
import random
import shutil
import signal
import subprocess
import sys
import time

BIN = os.environ.get("BIN", "/root/mvd-build/build/minivectordb")
READY = b"###READY###"


class Db:
    def __init__(self, data_dir, mode="exact"):
        self.data_dir = data_dir
        self.mode = mode
        self.p = None

    def start(self):
        self.p = subprocess.Popen(
            [BIN, "--data-dir=" + self.data_dir, "--ready-marker=###READY###",
             "--vector-mode=" + self.mode],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            bufsize=0)

    def send(self, sql):
        self.p.stdin.write((sql if sql.endswith(";") else sql + ";").encode() + b"\n")
        self.p.stdin.flush()

    def call(self, sql, timeout=30.0):
        """Sends one statement and returns its response lines (excluding READY)."""
        t0 = time.time()
        self.send(sql)
        out = []
        while True:
            if time.time() - t0 > timeout:
                raise TimeoutError("no response to: " + sql)
            line = self.p.stdout.readline()
            if not line:
                raise EOFError("process died on: " + sql)
            line = line.rstrip(b"\n")
            if line == READY:
                return out
            out.append(line.decode("utf-8", "replace"))

    def kill(self):
        if self.p and self.p.poll() is None:
            os.kill(self.p.pid, signal.SIGKILL)
            self.p.wait()

    def stop(self):
        if self.p and self.p.poll() is None:
            try:
                self.p.stdin.close()
                self.p.wait(timeout=10)
            except Exception:
                self.p.kill()
                self.p.wait()


def read_all_ids(db, table="t"):
    rows = db.call("SELECT id FROM %s ORDER BY id;" % table)
    ids = []
    for r in rows:
        if r.startswith("ERROR"):
            raise RuntimeError("select failed: " + r)
        ids.append(int(r.split("\t")[0]))
    return ids


def check(label, ids):
    """A surviving set must be a prefix of 1..N with no gaps and no dupes."""
    problems = []
    if sorted(ids) != ids:
        problems.append("not ordered")
    if len(set(ids)) != len(ids):
        problems.append("duplicate ids: %r" % (sorted(x for x in set(ids) if ids.count(x) > 1),))
    expect = list(range(1, len(ids) + 1))
    if ids != expect:
        missing = sorted(set(expect) - set(ids))
        extra = sorted(set(ids) - set(expect))
        problems.append("not a contiguous prefix (missing=%r extra=%r)" % (missing[:10], extra[:10]))
    print("  %-28s rows=%-6d %s" % (label, len(ids), "OK" if not problems else "FAIL " + "; ".join(problems)))
    return not problems


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    base = "/tmp/mvd-crash"
    shutil.rmtree(base, ignore_errors=True)
    ok_all = True

    for rnd in range(rounds):
        d = os.path.join(base, "r%d" % rnd)
        os.makedirs(d, exist_ok=True)
        db = Db(d)
        db.start()
        db.call("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(32));")

        confirmed = 0
        n = 400
        # Randomly decide where to pull the plug: after a confirmed commit, or
        # in the window between sending COMMIT and reading its response.
        crash_at = random.randint(1, n)
        abrupt = rnd % 2 == 0  # even rounds die mid-ACK, odd rounds die mid-txn

        for i in range(1, n + 1):
            if i == crash_at:
                if abrupt:
                    db.send("BEGIN;")
                    db.send("INSERT INTO t VALUES (%d, 'x%d');" % (i, i))
                    db.send("COMMIT;")
                    # Do not read the response: this is the "提交请求已发出但未
                    # 收到成功响应" window.
                    time.sleep(random.choice([0.0, 0.001, 0.01]))
                    db.kill()
                else:
                    # Die inside an open transaction: nothing of it may survive.
                    db.call("BEGIN;")
                    for j in range(5):
                        db.call("INSERT INTO t VALUES (%d, 'y%d');" % (i * 100 + j, j))
                    db.kill()
                break
            db.call("BEGIN;")
            db.call("INSERT INTO t VALUES (%d, 'x%d');" % (i, i))
            db.call("COMMIT;")
            confirmed += 1

        db2 = Db(d)
        db2.start()
        ids = read_all_ids(db2)
        print("round %d  crash_at=%d  mode=%s  confirmed_before_crash=%d" %
              (rnd, crash_at, "mid-ack" if abrupt else "mid-txn", confirmed))

        good = check("after recovery", ids)
        if good:
            # Anything confirmed before the crash must still be there.
            if abrupt and len(ids) < confirmed:
                print("  FAIL lost confirmed commits: %d < %d" % (len(ids), confirmed))
                good = False
            if not abrupt and len(ids) > confirmed:
                print("  FAIL uncommitted rows survived: %d > %d" % (len(ids), confirmed))
                good = False
        # Keep writing after recovery: the redo pass must leave a usable log.
        nxt = (max(ids) + 1) if ids else 1
        db2.call("BEGIN;")
        db2.call("INSERT INTO t VALUES (%d, 'z');" % nxt)
        db2.call("COMMIT;")
        ids2 = read_all_ids(db2)
        if len(ids2) != len(ids) + 1:
            print("  FAIL post-recovery insert changed the row count unexpectedly")
            good = False
        db2.stop()
        ok_all = ok_all and good

    print("\nRESULT:", "PASS" if ok_all else "FAIL")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
