#!/usr/bin/env python3
"""The mip chains' C side: the reference reduction, the fragment stage's lookup and the level rule.

Builds tools/replay/chain_test.c with tagpu/ddraw/src/tagpu_chain_ref.c as the DLL compiles it,
with the host's C compiler (both files are engine-free and Win32-free), and runs it. It requires
`tagpu_chain_ref` -- what the terrain oracle (tagpu_chaincheck.on) holds the device's chains to --
to equal an emulation of the reducer's own order of work (src/tagpu_chain_comp.h REDUCE_CS) on
random tiles, opaque, transparent, keyed and clipped; `tagpu_chain_texel`, the mirror of the
terrain fragment stage's taChain, to stay inside its cell's block and to hold the texel level 0
samples; `tagpu_chain_level` to give each level the spec names; the features' table form to reduce
a frame's tiles from the frame alone; and `tagpu_chain_ftexel`, the mirror of the feature fragment
stage's taFChain, to find each texel's own cell and block. So a live `cpu-differ=0` means the
device's arithmetic is this one, and this one is the shader's.

    python3 tools/replay/chain_test.py [--cases N] [--seed S] [--out DIR]
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
    exe = out / "chain-test"
    subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", TREE / "tagpu/ddraw/inc", "-o", exe, HERE / "chain_test.c",
                    TREE / "tagpu/ddraw/src/tagpu_chain_ref.c", "-lm"], check=True)
    got = subprocess.run([exe, str(seed), str(cases)], capture_output=True, text=True)
    print(got.stdout.strip())
    if got.returncode != 0 or not got.stdout.rstrip().endswith("every check holds"):
        raise RuntimeError("the C checks did not finish")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cases", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", type=Path, help="keep the program here")
    args = ap.parse_args()
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="chain-test-"))
    out.mkdir(parents=True, exist_ok=True)
    try:
        run(out, args.cases, args.seed)
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if not args.out:
            shutil.rmtree(out, ignore_errors=True)
    print("PASS: the chains' reference, lookup and level rule hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
