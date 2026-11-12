#!/usr/bin/env python3
"""tasim_fixed_test.py -- build tests/tasim_fixed_test.c three ways and hold them to one another.

    tagpu/ddraw/tests/tasim_fixed_test.py [--velocity FILE]

The core's engine arithmetic (src/tasim_fixed.h) must agree bit for bit in every build the core is made in, so the
self-test is built as the wiki's simulation host builds it (tools/wiki/wikibuild.mjs): `host`, gcc -O2; `asan`, the
sanitizers; `wine`, the DLL's compiler i686-w64-mingw32-gcc on SSE2 as the Makefile compiles every src/tasim_*.c, run
under Wine in the wiki checks' prefix (tools/wiki/wineprefix.py). Each build runs the rules; the host compares the
transcendentals with glibc; every build prints the digest of 10^6 calls of each helper, and the three must be equal.
With --velocity, FILE holds "heading speed vx vz" lines the engine computed (tools/tasim/velocity_triples.py writes
them from a recording) and every build checks the ground velocity rule on them.

Exit 0 when every build passes and the digests agree, 1 when one does not, 2 when a build cannot be made or run."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
TREE = DDRAW.parents[1]
SRCS = ["tests/tasim_fixed_test.c", "src/tasim_fixed.c"]
DEPS = [DDRAW / s for s in SRCS] + [DDRAW / "src/tasim_fixed.h"]
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "host": (["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc"], ["-lm"], "tasim_fixed_test-host.exe"),
    "asan": (["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Isrc", "-fsanitize=address,undefined",
              "-fno-sanitize-recover=all"], ["-lm"], "tasim_fixed_test-asan.exe"),
    "wine": (["i686-w64-mingw32-gcc", "-std=c99", "-O2", "-Wall", "-Isrc", "-D__USE_MINGW_ANSI_STDIO=1", *SSE],
             ["-static"], "tasim_fixed_test-wine.exe"),
}


def build(name):
    flags, libs, exe = BUILDS[name]
    out = HERE / exe
    if out.exists() and all(out.stat().st_mtime >= d.stat().st_mtime for d in DEPS):
        return out
    print(f"tasim_fixed_test: building {exe}", flush=True)
    subprocess.run([*flags, "-o", str(out), *SRCS, *libs], cwd=DDRAW, check=True)
    return out


def wine_env():
    r = subprocess.run([sys.executable, str(TREE / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"no Wine prefix: {r.stderr.strip()}")
    return {**os.environ, "WINEPREFIX": r.stdout.strip().splitlines()[-1], "WINEDEBUG": "-all"}


def run(name, exe, args, stdin=None):
    cmd = ["wine", str(exe), *args] if name == "wine" else [str(exe), *args]
    env = wine_env() if name == "wine" else None
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, stdin=stdin)
    return r.returncode, r.stdout.replace("\r\n", "\n")


def main(argv):
    velocity = argv[argv.index("--velocity") + 1] if "--velocity" in argv else None
    try:
        exes = {n: build(n) for n in BUILDS}
    except (subprocess.CalledProcessError, OSError) as e:
        print(f"tasim_fixed_test: a build failed: {e}")
        return 2
    bad, digests = 0, {}
    try:
        for name, exe in exes.items():
            code, out = run(name, exe, [])
            print(f"{name}: {out.strip()}")
            bad |= code != 0
            if name == "host":
                code, out = run(name, exe, ["libm"])
                print(f"{name}: {out.strip()}")
                bad |= code != 0
            code, out = run(name, exe, ["digest"])
            digests[name] = out.strip()
            bad |= code != 0
            if velocity:
                with open(velocity) as f:
                    code, out = run(name, exe, ["velocity"], stdin=f)
                print(f"{name}: {out.strip()}")
                bad |= code != 0
    except (RuntimeError, OSError) as e:
        print(f"tasim_fixed_test: a build cannot run: {e}")
        return 2
    for name, d in digests.items():
        print(f"{name}: {d}")
    if len(set(digests.values())) != 1:
        print("tasim_fixed_test: the builds' digests differ")
        bad = 1
    print("tasim_fixed_test: FAILED" if bad else "tasim_fixed_test: ok, the three builds agree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
