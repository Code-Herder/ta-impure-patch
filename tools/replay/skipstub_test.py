#!/usr/bin/env python3
"""The skippable before-call stub's bytes: NO DRAW's frame-visit stub, against the literal.

Builds tools/replay/skipstub_test.c over tagpu/ddraw/inc/tagpu_skipstub.h, the emitter
tagpu_replay.c's before_call_skippable writes its stub with, with the host's C compiler (the
header is engine-free and Win32-free), and runs it. It requires the stub for the frame visit
0x4969CD -- the visit, its skip flag, DrawGameScreen 0x468CF0 and the draw's own `ret 8` -- to
equal the 26 bytes written out by hand, and every rel32 and the jne of random placements to
decode back to their targets.

    python3 tools/replay/skipstub_test.py [--cases N] [--seed S] [--out DIR]
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]


def run(out, cases, seed):
    exe = out / "skipstub-test"
    subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", TREE / "tagpu/ddraw/inc", "-o", exe, HERE / "skipstub_test.c"],
                   check=True)
    got = subprocess.run([exe, str(seed), str(cases)], capture_output=True, text=True)
    print(got.stdout.strip())
    if got.returncode != 0 or not got.stdout.rstrip().endswith("every check holds"):
        raise RuntimeError("the C checks did not finish")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cases", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", type=Path, help="keep the program here")
    args = ap.parse_args()
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="skipstub-test-"))
    out.mkdir(parents=True, exist_ok=True)
    try:
        run(out, args.cases, args.seed)
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if not args.out:
            shutil.rmtree(out, ignore_errors=True)
    print("PASS: the skippable stub's bytes hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
