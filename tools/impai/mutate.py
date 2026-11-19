#!/usr/bin/env python3
"""mutate.py -- run a brain batch's mutations: each one a single text replacement in one engine-free source, which
the brain's and the table's native tests must then fail.

    tools/impai/mutate.py tools/impai/mutations/<name>.py [FILTER...]

A mutation list is a Python file defining M = [(name, file under tagpu/ddraw/src, old text, new text), ...]; the old
text must occur exactly once (else the mutation is NOT RUN and counts against the total; an old text of None, a list
that could not find its anchor, is NOT RUN too). FILTER keeps the mutations whose name holds any of them. The clean
tests run first and must pass; each source is restored after its mutation whatever happens.

A mutation is CAUGHT only when the tests build and then fail. One whose build fails -- a test runner's compile step
raising -- is NOT COMPILED and counts against the total like a survivor: a compile error proves nothing about the
fixtures (a mutation that leaves a function unused fails -Werror and only looks caught); rewrite it with `&& 0` or
`if (0)`. Exit 0 when every mutation was run and caught."""
import runpy
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tagpu/ddraw/src"
TESTS = ("tagpu/ddraw/tests/impai_brain_test.py", "tagpu/ddraw/tests/impai_table_test.py")


BUILD_FAILED = "CalledProcessError"     # the test runners' build steps run with check=True


def run_tests():
    """(rc, the FAIL and error lines, 1 when a build failed) of the native brain and table tests"""
    fails, rc, unbuilt = [], 0, 0
    for t in TESTS:
        r = subprocess.run([sys.executable, str(ROOT / t), "native"], capture_output=True, text=True, cwd=ROOT)
        rc |= r.returncode
        unbuilt |= r.returncode != 0 and BUILD_FAILED in r.stderr
        fails += [line for line in (r.stdout + r.stderr).splitlines() if "FAIL" in line or "error:" in line]
    return rc, fails, unbuilt


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    muts = runpy.run_path(argv[0])["M"]
    if argv[1:]:
        muts = [m for m in muts if any(a in m[0] for a in argv[1:])]
    rc, fails, _ = run_tests()
    print(f"clean: rc={rc} {fails[:3]}", flush=True)
    if rc:
        return 1
    bad = 0
    for name, f, old, new in muts:
        p = SRC / f
        keep = p.read_text()
        if old is None or keep.count(old) != 1:
            print(f"{name}: {0 if old is None else keep.count(old)} matches -- NOT RUN", flush=True)
            bad += 1
            continue
        try:
            p.write_text(keep.replace(old, new))
            rc, fails, unbuilt = run_tests()
        finally:
            p.write_text(keep)
        verdict = "NOT COMPILED" if unbuilt else "caught" if rc else "SURVIVED"
        print(f"{name}: {verdict} ({len(fails)} FAIL lines{': ' + fails[0] if fails else ''})", flush=True)
        bad += unbuilt or not rc
    print(f"mutate: {len(muts) - bad} of {len(muts)} caught")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
