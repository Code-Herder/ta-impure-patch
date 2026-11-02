#!/usr/bin/env python3
"""huddrawtest.py -- build tests/huddrawtest.c with the host's gcc and run it.

    tagpu/ddraw/tests/huddrawtest.py

tagpu_huddraw.c builds its instances without Vulkan, Win32 or engine state, so its self-test
builds natively, with AddressSanitizer and UndefinedBehaviorSanitizer, and needs no Wine. It
checks the CPU half of the replay HUD's draw: a HOP_TRI op's instance, and the two chart slots'
images, bands and generations kept apart. Exit 0 when nothing failed."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "huddrawtest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = ["tests/huddrawtest.c", "src/tagpu_huddraw.c", "src/tagpu_hudlay.c", "src/tagpu_hudtext.c", "src/tagpu_json.c"]
DEPS = SRCS + ["src/tagpu_huddraw.h", "src/tagpu_hudlay.h", "src/tagpu_hudart.h", "src/tagpu_hudtext.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= (DDRAW / s).stat().st_mtime for s in DEPS):
        return
    print("huddrawtest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-Iinc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), *SRCS, "-lm"],
                   cwd=DDRAW, check=True)


def main(argv):
    build()
    return subprocess.run([str(EXE), *argv]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
