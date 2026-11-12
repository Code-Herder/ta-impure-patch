#!/usr/bin/env python3
"""fake_tasim.py -- a stand-in for `tasim_run play` that writes traces from a table, for the checker's tests.

    TASIM_FAKE_TABLE=table.json fake_tasim.py play --run-seed S --trace OUT ... --json
    fake_tasim.py table RUNS_DIR OUT.json [--j N]

The table: {"exit": n} makes every call exit n; otherwise {"by_seed": {"<seed>": {"trace": [lines],
"t_end": T, "end": "limit"}}} gives each run its trace. It speaks the interface the checker expects
of tasim_run (research/notes/modules/tasim-calibration.md, *The tasim_run interface*): one JSON
object on stdout, exit 0 when it ran and 2 when it could not -- without a table, or without the
run's seed in it.

`table` builds a table from an engine run's lane manifests (what `calib.py refs build` reads):
every run's engine trace under its seed, which perturb.run_seed derives from the scenario's hash and
the run's index, so one table serves every scenario of the run. `calib.py check --tasim` on it,
against the references built from the same directory, grades every metric in: the checker's own
end-to-end test, with the engine standing in for tasim.
"""

import argparse
import json
import os
import sys
from pathlib import Path


def could_not(why):
    print(json.dumps({"schema": "tasim-run/1", "ok": False, "error": why}))
    return 2


def play(a):
    path = os.environ.get("TASIM_FAKE_TABLE")
    if not path:
        return could_not("TASIM_FAKE_TABLE names no table (fake_tasim.py table builds one)")
    try:
        table = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as e:
        return could_not(f"the table {path}: {e}")
    if "exit" in table:
        print(json.dumps({"schema": "tasim-run/1", "ok": False, "error": "told to fail"}))
        return table["exit"]
    row = table.get("by_seed", {}).get(a.run_seed)
    if row is None:
        return could_not(f"no seed {a.run_seed}")
    with open(a.trace, "w") as f:
        f.writelines(json.dumps(ln) + "\n" for ln in row["trace"])
    print(json.dumps({"schema": "tasim-run/1", "ok": True, "t_end": row["t_end"],
                      "end": row["end"], "trace": a.trace, "unmodelled": []}))
    return 0


def table(a):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import calib
    import calibref
    import devpy                        # on calib's path: the recordings need the shared venv
    devpy.ensure()
    try:
        _lanes, got = calib.lane_jobs(Path(a.runs_dir), a.j, all_traces=True)
    except calibref.CalibError as e:
        print(f"fake_tasim table: could not run: {e}")
        return 2
    by_seed, errors = {}, []
    for g in got:
        if "error" in g:
            errors.append(f"{g['lane']}: {g['error']}")
            continue
        for r in g["runs"]:
            row = r["row"]
            by_seed[str(row["seed"])] = {"trace": r["trace"], "t_end": row["t_end"] - row["t_apply"],
                                         "end": row["end"]}
    Path(a.out).write_text(json.dumps({"by_seed": by_seed}) + "\n")
    print(f"wrote {a.out}: {len(by_seed)} runs")
    for e in errors:
        print(f"could not read {e}")
    return 2 if errors else 0


def main(argv=None):
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("play")
    for k in ("--setup", "--wire", "--exogenous", "--limit", "--end", "--cwd", "--archives"):
        pl.add_argument(k)
    pl.add_argument("--run-seed", required=True)
    pl.add_argument("--trace", required=True)
    pl.add_argument("--json", action="store_true")
    tb = sub.add_parser("table")
    tb.add_argument("runs_dir")
    tb.add_argument("out")
    tb.add_argument("--j", type=int, default=8)
    a = p.parse_args(argv)
    return play(a) if a.cmd == "play" else table(a)


if __name__ == "__main__":
    sys.exit(main())
