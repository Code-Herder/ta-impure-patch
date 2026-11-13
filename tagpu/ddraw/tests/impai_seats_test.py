#!/usr/bin/env python3
"""impai_seats_test.py -- build tests/impai_seats_test.c three ways, run each, and compare their transcripts.

    tagpu/ddraw/tests/impai_seats_test.py            the three builds, then the comparison
    tagpu/ddraw/tests/impai_seats_test.py native     one build alone: native, asan or wine

The level init's seat choice (src/impai_seats.h) is engine-free, so its test needs no game:
    native  the host's gcc -O2;
    asan    the same under AddressSanitizer and UndefinedBehaviorSanitizer, every finding fatal;
    wine    the DLL's compiler and flags, run under Wine in the wiki checks' prefix (tools/wiki/wineprefix.py).
The choice is integer logic, so the three transcripts are equal byte for byte; a difference is a failure. Exit 0
when every build passed and they agree."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
REPO = HERE.parents[2]
TEST = "tests/impai_seats_test.c"
SRCS = ["src/impai_seats.c"]
WARN = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-Iinc"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "native": ("impai_seats_test-native.exe", "gcc", [*WARN, "-O2"], []),
    "asan": ("impai_seats_test-asan.exe", "gcc",
             [*WARN, "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"], []),
    "wine": ("impai_seats_test-wine.exe", "i686-w64-mingw32-gcc", [*WARN, "-O2", "-D__USE_MINGW_ANSI_STDIO=1"],
             ["-static"]),
}


def build(kind):
    exe, cc, flags, libs = BUILDS[kind]
    out = HERE / exe
    inputs = [Path(__file__), DDRAW / TEST, *(DDRAW / s for s in SRCS), DDRAW / "src/impai_seats.h"]
    if out.exists() and all(out.stat().st_mtime >= p.stat().st_mtime for p in inputs):
        return out
    print(f"impai_seats_test: building {exe}", flush=True)
    subprocess.run([cc, *flags, "-o", str(out), TEST, *SRCS, *libs], cwd=DDRAW, check=True)
    return out


def wine_env():
    env = dict(os.environ)
    prefix = subprocess.run([sys.executable, str(REPO / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if prefix.returncode != 0:
        raise SystemExit(f"impai_seats_test: no Wine prefix: {prefix.stderr.strip()}")
    env["WINEPREFIX"] = prefix.stdout.strip()
    env.setdefault("WINEDEBUG", "-all")
    return env


def run(kind):
    exe = build(kind)
    if kind == "wine":
        r = subprocess.run(["wine", str(exe)], env=wine_env(), stdout=subprocess.PIPE)
        return r.returncode, r.stdout.replace(b"\r\n", b"\n")
    r = subprocess.run([str(exe)], stdout=subprocess.PIPE)
    return r.returncode, r.stdout


def main(argv):
    kinds = argv or list(BUILDS)
    results = {}
    for kind in kinds:
        rc, out = run(kind)
        results[kind] = out
        last = out.decode(errors="replace").strip().splitlines()[-1:] or ["(no output)"]
        print(f"impai_seats_test {kind}: rc={rc} {last[0]}")
        if rc != 0:
            sys.stdout.write(out.decode(errors="replace"))
            return 1
    first = kinds[0]
    for kind in kinds[1:]:
        if results[kind] != results[first]:
            print(f"impai_seats_test: the {kind} transcript differs from the {first} one")
            return 1
    if len(kinds) > 1:
        print(f"impai_seats_test: {len(kinds)} builds, one transcript of {len(results[first].splitlines())} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
