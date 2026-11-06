#!/usr/bin/env python3
"""iconsettest.py -- build tests/iconsettest.c with the host's gcc and run it.

    tagpu/ddraw/tests/iconsettest.py

src/tagpu_iconset.c, a mod icon set's PCX decoder, profile reader and atlas packer, is engine-free
and Win32-free, so its self-test builds natively, with AddressSanitizer and
UndefinedBehaviorSanitizer, and needs no Wine (`make iconsettest` is the Windows build of the same
files). Exit 0 when nothing failed."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "iconsettest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "iconsettest.c", DDRAW / "src/tagpu_iconset.c", DDRAW / "inc/tagpu_iconset.h",
        DDRAW / "inc/tagpu_icontype.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("iconsettest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Iinc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/iconsettest.c", "src/tagpu_iconset.c"],
                   cwd=DDRAW, check=True)


def main(argv):
    build()
    return subprocess.run([str(EXE), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
