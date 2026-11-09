# MiniVectorDB test suite

mysqltest-style suite that feeds SQL over stdin to the real `minivectordb`
binary and diffs stdout against recorded baselines. Every case runs against
its own temporary `--data-dir`, so cases are fully independent.

## Running

From a shell on Linux (or inside WSL), with the repo at `$REPO`:

```sh
cd "$REPO"

# build first
bash scripts/build.sh                       # -> $HOME/mvd-build/build/minivectordb

# everything, with a pass/fail summary (exit code != 0 on any failure)
bash tests/run_all.sh

# one case
python3 tests/mysqltest.py basic_sql
```

From Windows, prefix any of the above with `wsl.exe -d Ubuntu-D -- bash -c "..."`.
Avoid putting `$` variables inside that prefix: the Windows argument layer eats them,
so call a script file instead.

The binary defaults to `/root/mvd-build/build/minivectordb`; override with
`--binary PATH` or `MVD_BIN`. Useful driver flags: `--record` (rewrite the
baseline), `--keep` (keep the temp data dir), `--timeout SEC`.

If a script fails with `\r` errors, run `sed -i 's/\r//g' <file>` first.
`.gitattributes` forces `eol=lf` for these files so a fresh clone is clean;
the problem only shows up if a Windows editor rewrites a working-tree file.

## Layout

- `t/<name>.test`  — SQL plus `#` directives
- `r/<name>.result` — baseline stdout (one line per engine output line)
- `mysqltest.py`   — driver
- `run_all.sh`     — runs every `t/*.test` and summarizes

## Test file syntax

Plain lines are SQL; a statement ends at a line whose trailing token is `;`
(multi-line statements, `--` and `/* */` comments work). Lines starting with
`#` are driver directives or comments:

| Directive | Meaning |
|---|---|
| `# expect-error NAME` | next statement must answer `ERROR NAME: ...` (live assertion, survives re-record) |
| `# restart [flags]` | clean shutdown + restart, same data dir (persistence) |
| `# kill [flags]` | SIGKILL + restart, same data dir (crash recovery) |
| `# mode ann --ivf-nprobe=N` | restart with `--vector-mode=ann` + extra flags |
| `# gen-vectors T N D SEED` | insert N deterministic rows into 2-col table `(id BIGINT PK, v VECTOR(D))` |
| `# recall K THRESHOLD` | 16 TopK-K queries exact vs ann; assert mean Recall@K ≥ THRESHOLD, ids valid/unique |
| `# expect-ann-active K` | assert CLI `--vector-mode=ann` observably changes results (rule 15) |
| `# timeout SEC` | per-statement deadline (default 10 s; timeouts fail the case) |

The driver talks to the engine with `--ready-marker`, so each statement is
fully round-tripped before the next is written; output lines containing the
marker are never recorded.

## Adding a case

1. Write `t/<name>.test`.
2. `python3 mysqltest.py <name> --record` to generate `r/<name>.result`.
3. Review the baseline by hand. If the engine output violates the task rules
   (tie order, filtered TopK, txn/crash semantics, error contract), edit the
   affected baseline lines to the correct expected content and leave the case
   failing — that is a bug report, not a test bug. Do not adjust the engine
   or relax assertions to make a case pass.

## Updating baselines intentionally

Re-run `mysqltest.py <name> --record`. Live assertions (`# expect-error`,
`# recall`, `# expect-ann-active`) are re-checked at compare time too, so an
engine regression that accidentally matches a re-recorded baseline still
fails the run.
