#!/usr/bin/env python3
"""mutate.py -- run a brain batch's mutations: each one a single text replacement in one engine-free source, which
the brain's and the table's native tests must then fail.

    tools/impai/mutate.py tools/impai/mutations/<name>.py [FILTER...]

A mutation list is a Python file defining M = [(name, file under tagpu/ddraw/src, old text, new text), ...]; the old
text must occur exactly once (else the mutation is NOT RUN and counts against the total). FILTER keeps the
mutations whose name holds any of them. The clean tests run first and must pass; each source is restored after its
mutation whatever happens. Exit 0 when every mutation run was caught."""
import runpy
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tagpu/ddraw/src"
TESTS = ("tagpu/ddraw/tests/impai_brain_test.py", "tagpu/ddraw/tests/impai_table_test.py")


def run_tests():
    """(rc, the FAIL and error lines) of the native brain and table tests"""
    fails, rc = [], 0
    for t in TESTS:
        r = subprocess.run([sys.executable, str(ROOT / t), "native"], capture_output=True, text=True, cwd=ROOT)
        rc |= r.returncode
        fails += [line for line in (r.stdout + r.stderr).splitlines() if "FAIL" in line or "error:" in line]
    return rc, fails


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    muts = runpy.run_path(argv[0])["M"]
    if argv[1:]:
        muts = [m for m in muts if any(a in m[0] for a in argv[1:])]
    rc, fails = run_tests()
    print(f"clean: rc={rc} {fails[:3]}", flush=True)
    if rc:
        return 1
    bad = 0
    for name, f, old, new in muts:
        p = SRC / f
        keep = p.read_text()
        if keep.count(old) != 1:
            print(f"{name}: {keep.count(old)} matches -- NOT RUN", flush=True)
            bad += 1
            continue
        try:
            p.write_text(keep.replace(old, new))
            rc, fails = run_tests()
        finally:
            p.write_text(keep)
        print(f"{name}: {'caught' if rc else 'SURVIVED'} ({len(fails)} FAIL lines{': ' + fails[0] if fails else ''})",
              flush=True)
        bad += not rc
    print(f"mutate: {len(muts) - bad} of {len(muts)} caught")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
