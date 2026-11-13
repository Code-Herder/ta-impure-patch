#!/usr/bin/env python3
"""impai_fight_test.py -- build tests/impai_fight_test.c three ways, run each, and compare their transcripts.

    tagpu/ddraw/tests/impai_fight_test.py            the three builds, then the comparison
    tagpu/ddraw/tests/impai_fight_test.py native     one build alone: native, asan or wine

ImpAI's fight predictor is engine-free (src/impai_fight.h), so its test needs no game:
    native  the host's gcc -O2, as tools/impai/fightlib.c's shared library, which the fit calls, is built;
    asan    the same under AddressSanitizer and UndefinedBehaviorSanitizer, every finding fatal, a float converted
            to an integer it does not fit included;
    wine    the DLL's compiler and flags (Makefile), the predictor's sources on SSE2 as the Makefile compiles them,
            the test's own too, run under Wine in the wiki checks' prefix (tools/wiki/wineprefix.py).
The predictor computes in single-precision floats rounded once per operation in every build, so the three
transcripts are equal byte for byte; a difference is a failure, and it is what makes the fit's library and the
DLL give one answer. Exit 0 when every build passed and they agree."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
REPO = HERE.parents[2]
TEST = "tests/impai_fight_test.c"
# the sources the test links: the predictor and its coefficients
SRCS = ["src/impai_fight.c", "src/impai_fitparams.c"]
# the Makefile's IMPAI rule
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
WARN = ["-std=c99", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror", "-Isrc", "-Iinc"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "native": ("impai_fight_test-native.exe", "gcc", [*WARN, "-O2"], ["-lm"]),
    "asan": ("impai_fight_test-asan.exe", "gcc",
             [*WARN, "-O1", "-g", "-fsanitize=address,undefined", "-fsanitize=float-cast-overflow",
              "-fno-sanitize-recover=all"], ["-lm"]),
    "wine": ("impai_fight_test-wine.exe", "i686-w64-mingw32-gcc", [*WARN, "-O2", "-D__USE_MINGW_ANSI_STDIO=1"],
             ["-static", "-lm"]),
}


def build(kind):
    exe, cc, flags, libs = BUILDS[kind]
    out = HERE / exe
    inputs = [Path(__file__), DDRAW / TEST, *(DDRAW / s for s in SRCS), *(DDRAW / "src").glob("*.h")]
    if out.exists() and all(out.stat().st_mtime >= p.stat().st_mtime for p in inputs if p.exists()):
        return out
    print(f"impai_fight_test: building {exe}", flush=True)
    # the predictor's sources as the Makefile compiles them, and the test with them
    sse = [TEST, *SRCS] if kind == "wine" else []
    objs = []
    tmp = HERE / f".impai_fight_{kind}"
    tmp.mkdir(exist_ok=True)
    for src in [TEST, *SRCS]:
        obj = tmp / (Path(src).stem + ".o")
        subprocess.run([cc, *flags, *(SSE if src in sse else []), "-c", "-o", str(obj), src], cwd=DDRAW, check=True)
        objs.append(str(obj))
    subprocess.run([cc, *flags, "-o", str(out), *objs, *libs], cwd=DDRAW, check=True)
    return out


def wine_env():
    env = dict(os.environ)
    prefix = subprocess.run([sys.executable, str(REPO / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if prefix.returncode != 0:
        raise SystemExit(f"impai_fight_test: no Wine prefix: {prefix.stderr.strip()}")
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
        print(f"impai_fight_test {kind}: rc={rc} {last[0]}")
        if rc != 0:
            sys.stdout.write(out.decode(errors="replace"))
            return 1
    first = kinds[0]
    for kind in kinds[1:]:
        if results[kind] != results[first]:
            print(f"impai_fight_test: the {kind} transcript differs from the {first} one")
            return 1
    if len(kinds) > 1:
        print(f"impai_fight_test: {len(kinds)} builds, one transcript of {len(results[first].splitlines())} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
