#!/usr/bin/env python3
"""placetest.py -- build tests/placetest.c with the host's gcc and run it.

    tagpu/ddraw/tests/placetest.py

tagpu_placegeo.c is engine-free and Win32-free, so its self-test builds natively, with
AddressSanitizer and UndefinedBehaviorSanitizer, and needs no Wine. Extra arguments are passed
to the test (`dump` reads cases on stdin, see placetest.c). Exit 0 when nothing failed."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "placetest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "placetest.c", DDRAW / "src/tagpu_placegeo.c", DDRAW / "src/tagpu_placegeo.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("placetest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/placetest.c", "src/tagpu_placegeo.c"],
                   cwd=DDRAW, check=True)


def main(argv):
    build()
    return subprocess.run([str(EXE), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
