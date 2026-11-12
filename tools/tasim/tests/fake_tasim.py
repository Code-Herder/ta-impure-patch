#!/usr/bin/env python3
"""fake_tasim.py -- a stand-in for `tasim_run play` that writes traces from a table, for the checker's tests.

    TASIM_FAKE_TABLE=table.json fake_tasim.py play --run-seed S --trace OUT ... --json

The table: {"exit": n} makes every call exit n; otherwise {"by_seed": {"<seed>": {"trace": [lines],
"t_end": T, "end": "limit"}}} gives each run its trace. It speaks the interface the checker expects
of tasim_run (research/notes/modules/tasim-calibration.md, *The tasim_run interface*): one JSON
object on stdout, exit 0 when it ran and 2 when it could not.
"""

import argparse
import json
import os
import sys


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("cmd", choices=("play",))
    for k in ("--setup", "--wire", "--exogenous", "--limit", "--end", "--cwd", "--archives"):
        p.add_argument(k)
    p.add_argument("--run-seed", required=True)
    p.add_argument("--trace", required=True)
    p.add_argument("--json", action="store_true")
    a = p.parse_args(argv)
    table = json.load(open(os.environ["TASIM_FAKE_TABLE"]))
    if "exit" in table:
        print(json.dumps({"schema": "tasim-run/1", "ok": False, "error": "told to fail"}))
        return table["exit"]
    row = table["by_seed"].get(a.run_seed)
    if row is None:
        print(json.dumps({"schema": "tasim-run/1", "ok": False, "error": f"no seed {a.run_seed}"}))
        return 2
    with open(a.trace, "w") as f:
        f.writelines(json.dumps(ln) + "\n" for ln in row["trace"])
    print(json.dumps({"schema": "tasim-run/1", "ok": True, "t_end": row["t_end"],
                      "end": row["end"], "trace": a.trace, "unmodelled": []}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
