#!/usr/bin/env python3
"""formtest.py -- build tests/formtest.c and run it.

    tagpu/ddraw/tests/formtest.py                 the checks, native, sanitized
    tagpu/ddraw/tests/formtest.py dump < cases    the dump (see formtest.c), native, sanitized
    tagpu/ddraw/tests/formtest.py bench           the timings, native -O2
    tagpu/ddraw/tests/formtest.py --wine [mode]   any of the above built as the DLL is built
                                                  and run under Wine

tagpu_formgeo.c is engine-free and Win32-free, so its self-test builds natively, with
AddressSanitizer and UndefinedBehaviorSanitizer, and needs no Wine. The sanitizers slow it, so the
native bench builds a plain -O2 copy. The number that matters for the game is the one `--wine`
gives: the DLL's compiler and flags (Makefile: i686-w64-mingw32-gcc -O2, 32-bit, its default
-march), so every int64 product is the 32-bit code the game runs. WINEPREFIX defaults to the main
checkout's `wineprefix/`. Exit 0 when nothing failed."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
REPO = HERE.parents[2]
SRCS = [HERE / "formtest.c", DDRAW / "src/tagpu_formgeo.c", DDRAW / "src/tagpu_formgeo.h"]
WARN = ["-std=c99", "-Wall", "-Wextra", "-Werror"]
# `.exe` so the tree's *.exe ignore covers each
BUILDS = {
    "native": (HERE / "formtest-native.exe",
               ["gcc", *WARN, "-O1", "-g", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all"],
               ["-lm"]),
    "bench": (HERE / "formtest-bench.exe", ["gcc", *WARN, "-O2"], ["-lm"]),
    # the DLL's CC and CFLAGS (Makefile), static as the Makefile's own test executables are
    "wine": (HERE / "formtest-wine.exe", ["i686-w64-mingw32-gcc", "-Iinc", "-O2", *WARN, "-static"],
             ["-lm"]),
}


def build(kind):
    exe, cc, libs = BUILDS[kind]
    if exe.exists() and all(exe.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return exe
    print("formtest: building", exe.name, flush=True)
    subprocess.run([*cc, "-o", str(exe), "tests/formtest.c", *libs], cwd=DDRAW, check=True)
    return exe


def wine_env():
    env = dict(os.environ)
    common = Path(subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "--git-common-dir"],
                                          text=True).strip())
    env.setdefault("WINEPREFIX", str((REPO / common).resolve().parent / "wineprefix"))
    env.setdefault("WINEDEBUG", "-all")
    return env


def main(argv):
    wine = "--wine" in argv
    args = [a for a in argv if a != "--wine"]
    if wine:
        exe = build("wine")
        # Wine's console writes CRLF; the dump is compared line for line
        r = subprocess.run(["wine", str(exe), *args], env=wine_env(), stdout=subprocess.PIPE)
        sys.stdout.buffer.write(r.stdout.replace(b"\r\n", b"\n"))
        sys.stdout.flush()
        return r.returncode
    exe = build("bench" if args[:1] == ["bench"] else "native")
    return subprocess.run([str(exe), *args]).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
