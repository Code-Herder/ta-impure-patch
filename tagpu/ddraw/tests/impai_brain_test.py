#!/usr/bin/env python3
"""impai_brain_test.py -- build tests/impai_brain_test.c three ways, run each, and compare their transcripts.

    tagpu/ddraw/tests/impai_brain_test.py            the three builds, then the comparison
    tagpu/ddraw/tests/impai_brain_test.py native     one build alone: native, asan or wine

ImpAI's brain is engine-free (src/impai.h), so its test needs no game:
    native  the host's gcc -O2;
    asan    the same under AddressSanitizer and UndefinedBehaviorSanitizer, every finding fatal, a float converted
            to an integer it does not fit included;
    wine    the DLL's compiler and flags (Makefile), the brain's sources and the shared core's on SSE2 as the Makefile
            compiles them, the test's own too, run under Wine in the wiki checks' prefix (tools/wiki/wineprefix.py).
The brain decides in single-precision floats rounded once per operation on SSE2 in every build, so the three
transcripts are equal byte for byte; a difference is a failure. Exit 0 when every build passed and they agree."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
REPO = HERE.parents[2]
TEST = "tests/impai_brain_test.c"
# the sources the test links: the brain's, the knowledge tick's radar publish, and the shared rules they call
SRCS = ["src/impai_know.c", "src/impai_tinfo.c", "src/impai_brain.c", "src/impai_econ.c", "src/impai_build.c",
        "src/impai_maps.c", "src/impai_table.c", "src/impai_mil.c", "src/impai_fight.c", "src/impai_fitparams.c",
        "src/tasim_snap.c", "src/tagpu_placegeo.c"]
# the Makefile's SSE2 rule: every src/tasim_*.c and src/impai_*.c
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
WARN = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-Iinc"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "native": ("impai_brain_test-native.exe", "gcc", [*WARN, "-O2"], ["-lm"]),
    "asan": ("impai_brain_test-asan.exe", "gcc",
             [*WARN, "-O1", "-g", "-fsanitize=address,undefined", "-fsanitize=float-cast-overflow",
              "-fno-sanitize-recover=all"], ["-lm"]),
    "wine": ("impai_brain_test-wine.exe", "i686-w64-mingw32-gcc", [*WARN, "-O2", "-D__USE_MINGW_ANSI_STDIO=1"],
             ["-static", "-lm"]),
}


def build(kind):
    exe, cc, flags, libs = BUILDS[kind]
    out = HERE / exe
    inputs = [Path(__file__), DDRAW / TEST, *(DDRAW / s for s in SRCS), *(DDRAW / "src").glob("*.h")]
    if out.exists() and all(out.stat().st_mtime >= p.stat().st_mtime for p in inputs if p.exists()):
        return out
    print(f"impai_brain_test: building {exe}", flush=True)
    # the brain's and the core's sources as the Makefile compiles them, and the test with them: its expected
    # values are single-precision expressions that must round as the brain's do
    sse = [TEST, *(s for s in SRCS if Path(s).name.startswith(("impai_", "tasim_")))] if kind == "wine" else []
    objs = []
    tmp = HERE / f".impai_brain_{kind}"
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
        raise SystemExit(f"impai_brain_test: no Wine prefix: {prefix.stderr.strip()}")
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
        print(f"impai_brain_test {kind}: rc={rc} {last[0]}")
        if rc != 0:
            sys.stdout.write(out.decode(errors="replace"))
            return 1
    first = kinds[0]
    for kind in kinds[1:]:
        if results[kind] != results[first]:
            print(f"impai_brain_test: the {kind} transcript differs from the {first} one")
            return 1
    if len(kinds) > 1:
        print(f"impai_brain_test: {len(kinds)} builds, one transcript of {len(results[first].splitlines())} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
