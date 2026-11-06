#!/usr/bin/env python3
"""The fog expansion, the shaders' C mirror against the builder's window replication.

Builds tools/replay/fog_expand_test.c with tagpu/ddraw/src/tagpu_fogsrc.c as the DLL compiles it,
with the host's C compiler (both files are engine-free and Win32-free), and runs it: fixed checks,
then random source maps (random, all dark, all clear, one cell at a corner or on an edge, runs)
under random windows (inside, straddling each edge and corner, wholly off the map). It requires
`tagpu_fog_entry` -- what tagpu_fog_at and, through TAGPU_GLSL_FOG_FN, every pass's shader expand
the source with -- to equal `tagpu_fog_build_window` plus its edge fill on every entry the window
build defines, and requires each border completion to matter. The window build is what
tagpu_fogwide.c's oracle holds to the engine's own grid in a live game (`differ=0`); this is what
makes `mirror-differ=0` there mean the expansion and not a coincidence of the scene.

    python3 tools/replay/fog_expand_test.py [--cases N] [--seed S] [--out DIR]
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
    exe = out / "fog-expand-test"
    subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", TREE / "tagpu/ddraw/inc", "-o", exe, HERE / "fog_expand_test.c",
                    TREE / "tagpu/ddraw/src/tagpu_fogsrc.c"], check=True)
    got = subprocess.run([exe, str(seed), str(cases)], capture_output=True, text=True)
    print(got.stdout.strip())
    if got.returncode != 0 or not got.stdout.rstrip().endswith("every check holds"):
        raise RuntimeError("the C checks did not finish")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cases", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", type=Path, help="keep the program here")
    args = ap.parse_args()
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="fog-expand-test-"))
    out.mkdir(parents=True, exist_ok=True)
    try:
        run(out, args.cases, args.seed)
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if not args.out:
            shutil.rmtree(out, ignore_errors=True)
    print("PASS: the fog expansion equals the builder's window replication")
    return 0


if __name__ == "__main__":
    sys.exit(main())
