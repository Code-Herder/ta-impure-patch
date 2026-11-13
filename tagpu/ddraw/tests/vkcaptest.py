#!/usr/bin/env python3
"""vkcaptest.py -- build tests/vkcaptest.c with the host's gcc and run it.

    tagpu/ddraw/tests/vkcaptest.py [steps]

inc/tagpu_vkcap.h, the GPU memory cap's ledger, is engine-free and Win32-free, so its self-test
builds natively, with AddressSanitizer and UndefinedBehaviorSanitizer, and needs no Wine. `steps`
is the random walk's length (200000). Exit 0 when nothing failed."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "vkcaptest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "vkcaptest.c", DDRAW / "inc/tagpu_vkcap.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("vkcaptest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Iinc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/vkcaptest.c"],
                   cwd=DDRAW, check=True)


def main(argv):
    build()
    return subprocess.run([str(EXE), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
