#!/usr/bin/env python3
"""recdifftest.py -- build tests/recdifftest.c with the host's gcc and run it.

    tagpu/ddraw/tests/recdifftest.py [bench]

inc/tagpu_recdiff.h is engine-free and Win32-free, so its self-test builds natively, with
AddressSanitizer and UndefinedBehaviorSanitizer, and needs no Wine (`make recdifftest` is the
Windows build of the same file, with the DLL's compiler and flags). `bench` times the comparison
against the one it replaced; the sanitizers slow it, so time the Windows build. Exit 0 when nothing
failed."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "recdifftest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "recdifftest.c", DDRAW / "inc/tagpu_recdiff.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("recdifftest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Iinc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/recdifftest.c"],
                   cwd=DDRAW, check=True)


def main(argv):
    build()
    return subprocess.run([str(EXE), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
