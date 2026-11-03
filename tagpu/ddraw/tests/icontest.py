#!/usr/bin/env python3
"""icontest.py -- build tests/icontest.c with the host's gcc and run it.

    tagpu/ddraw/tests/icontest.py [dump]

inc/tagpu_icon_rule.h is engine-free and Win32-free, so its self-test builds natively, with
AddressSanitizer and UndefinedBehaviorSanitizer, and needs no Wine (`make icontest` is the
Windows build of the same file). `dump` prints the 16-px shape rasters. Exit 0 when nothing
failed."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "icontest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "icontest.c", DDRAW / "inc/tagpu_icon_rule.h", DDRAW / "inc/tagpu_icontype.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("icontest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Iinc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/icontest.c", "-lm"],
                   cwd=DDRAW, check=True)


def main(argv):
    build()
    return subprocess.run([str(EXE), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
