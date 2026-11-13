#!/usr/bin/env python3
"""tasim_keys_test.py -- build tests/tasim_keys_test.c three ways and hold them to the engine's number getters.

    tagpu/ddraw/tests/tasim_keys_test.py [--gamedir DIR] [--no-install]

The key table (src/tasim_keys.h) turns every FBI and TDF value into the field the engine holds, in every build the
core is made in, so the self-test is built as tests/tasim_fixed_test.py builds it: `host`, gcc -O2; `asan`, the
sanitizers; `wine`, the DLL's compiler i686-w64-mingw32-gcc on SSE2, run under Wine in the wiki checks' prefix.

THE SAMPLE is every value of every key in the install's unit FBIs, weapon TDFs, feature TDFs and moveinfo.tdf, read at
run time by the repository's own readers (`tasim_run values`, tools/tasim/native.py's mount list) into a temporary
file that is deleted afterwards: nothing from the game is kept. Each build runs the rules; the host holds the double
getter to glibc's correctly rounded strtod on the sample's plain decimals, on 10^6 generated ones and on exact ties
(the decimal midpoints of two doubles, written out here); the i686 build runs the engine's own 16.16 sequence on the
x87 over MSVCRT's atof for every sample value and requires ts_tdf_fx equal, and the doubles and the ints equal too;
every build prints the digest of the three getters on the sample, and the three must agree.

Exit 0 when every build passes and the digests agree, 1 when one does not, 2 when a build, Wine or the install
cannot be had (--no-install skips the sample)."""
import os
import random
import re
import subprocess
import sys
import tempfile
from decimal import Decimal, getcontext
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
TREE = DDRAW.parents[1]
SRCS = ["tests/tasim_keys_test.c", "src/tasim_keys.c", "src/tasim_fixed.c"]
DEPS = [DDRAW / s for s in SRCS] + [DDRAW / "src/tasim_keys.h", DDRAW / "src/tasim_fixed.h"]
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "host": (["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc"], ["-lm"], "tasim_keys_test-host.exe"),
    "asan": (["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Isrc", "-fsanitize=address,undefined",
              "-fno-sanitize-recover=all"], ["-lm"], "tasim_keys_test-asan.exe"),
    "wine": (["i686-w64-mingw32-gcc", "-std=c99", "-O2", "-Wall", "-Isrc", "-D__USE_MINGW_ANSI_STDIO=1", *SSE],
             ["-static"], "tasim_keys_test-wine.exe"),
}
PLAIN = re.compile(r"^[ \t]*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$")


def build(name):
    flags, libs, exe = BUILDS[name]
    out = HERE / exe
    if out.exists() and all(out.stat().st_mtime >= d.stat().st_mtime for d in DEPS):
        return out
    print(f"tasim_keys_test: building {exe}", flush=True)
    subprocess.run([*flags, "-o", str(out), *SRCS, *libs], cwd=DDRAW, check=True)
    return out


def wine_env():
    r = subprocess.run([sys.executable, str(TREE / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"no Wine prefix: {r.stderr.strip()}")
    return {**os.environ, "WINEPREFIX": r.stdout.strip().splitlines()[-1], "WINEDEBUG": "-all"}


def run(name, exe, args, stdin_path=None):
    cmd = ["wine", str(exe), *args] if name == "wine" else [str(exe), *args]
    env = wine_env() if name == "wine" else None
    with open(stdin_path or os.devnull, "rb") as f:
        r = subprocess.run(cmd, capture_output=True, env=env, stdin=f)
    return r.returncode, r.stdout.decode("latin-1").replace("\r\n", "\n")


def install_sample(gamedir, path):
    """Every value of the install's definitions, one a line, into `path`; how many."""
    sys.path.insert(0, str(TREE / "tools/tasim"))
    import native
    exe = native.build()
    root, mounts = native.mount_list(gamedir)
    try:
        with open(path, "wb") as out:
            r = subprocess.run([str(exe), "values", "--cwd", str(root), "--archives", str(mounts)], stdout=out,
                               stderr=subprocess.PIPE)
    finally:
        mounts.unlink()
    if r.returncode != 0:
        raise RuntimeError(f"tasim_run values: {r.stderr.decode(errors='replace').strip()}")
    return sum(1 for _ in open(path, "rb"))


def ties(n):
    """Exact decimal midpoints between two neighbouring doubles, m * 2^k with m odd and 54 bits, short enough
    (|k| <= 30) that the core keeps every digit: round half to even decides each."""
    getcontext().prec = 200
    rnd = random.Random(20261112)
    out = []
    for _ in range(n):
        m = (1 << 53) | rnd.getrandbits(53) | 1
        k = rnd.randint(-30, 30)
        out.append(format(Decimal(m) * (Decimal(2) ** k), "f"))
    return out


def main(argv):
    gamedir = argv[argv.index("--gamedir") + 1] if "--gamedir" in argv else None
    try:
        exes = {n: build(n) for n in BUILDS}
    except (subprocess.CalledProcessError, OSError) as e:
        print(f"tasim_keys_test: a build failed: {e}")
        return 2
    tmp = tempfile.TemporaryDirectory()
    sample = Path(tmp.name) / "values.txt"
    plain = Path(tmp.name) / "plain.txt"
    try:
        if "--no-install" in argv:
            sample.write_text("")
            print("tasim_keys_test: no install sample (--no-install)")
        else:
            print(f"tasim_keys_test: {install_sample(gamedir, sample)} values read from the install")
        lines = sample.read_bytes().decode("latin-1").splitlines()
        decimals = sorted({s for s in lines if PLAIN.match(s)})
        plain.write_text("\n".join(decimals + ties(20000)) + "\n", encoding="latin-1")
        print(f"tasim_keys_test: {len(set(lines))} distinct values, {len(decimals)} of them plain decimals")
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        print(f"tasim_keys_test: the install cannot be read: {e}")
        return 2
    bad, digests = 0, {}
    try:
        for name, exe in exes.items():
            code, out = run(name, exe, [])
            print(f"{name}: {out.strip()}")
            bad |= code != 0
            if name == "host":
                code, out = run(name, exe, ["libc"], plain)
                print(f"{name}: {out.strip()}")
                bad |= code != 0
            if name == "wine":
                code, out = run(name, exe, ["x87"], sample)
                print(f"{name}: {out.strip()}")
                bad |= code != 0
            code, out = run(name, exe, ["digest"], sample)
            digests[name] = out.strip()
            bad |= code != 0
    except (RuntimeError, OSError) as e:
        print(f"tasim_keys_test: a build cannot run: {e}")
        return 2
    finally:
        tmp.cleanup()
    for name, d in digests.items():
        print(f"{name}: {d}")
    if len(set(digests.values())) != 1:
        print("tasim_keys_test: the builds' digests differ")
        bad = 1
    print("tasim_keys_test: FAILED" if bad else "tasim_keys_test: ok, the three builds agree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
