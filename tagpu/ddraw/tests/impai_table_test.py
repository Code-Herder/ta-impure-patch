#!/usr/bin/env python3
"""impai_table_test.py -- build tests/impai_table_test.c three ways, run each, compare their transcripts, and hold
the tools' table files (tools/impai/tablefile.py) to the C loader.

    tagpu/ddraw/tests/impai_table_test.py            the three builds, the comparison, the file check
    tagpu/ddraw/tests/impai_table_test.py native     one build alone: native, asan or wine

The classes, the response table and the buy (src/impai_table.h) are engine-free, so their test needs no game:
    native  the host's gcc -O2;
    asan    the same under AddressSanitizer and UndefinedBehaviorSanitizer, every finding fatal;
    wine    the DLL's compiler and flags, the sources on SSE2 as the Makefile compiles them, run under Wine in the
            wiki checks' prefix (tools/wiki/wineprefix.py).
Every share and score is a single-precision operation rounded once on SSE2 in every build, so the three
transcripts are equal byte for byte; a difference is a failure.

THE FILE CHECK: the native build writes IMPAI_TABLE_HAND as a file, which tablefile.py must read with the same
hash and numbers; tablefile.py writes a changed table, which the C loader must accept with tablefile.py's hash;
and a file whose trailer tablefile.py did not recompute must be refused. Exit 0 when everything passed."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
REPO = HERE.parents[2]
TEST = "tests/impai_table_test.c"
SRCS = ["src/impai_table.c", "src/tasim_roles.c"]
# the Makefile's SSE2 rule for both sources
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
WARN = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-Iinc"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "native": ("impai_table_test-native.exe", "gcc", [*WARN, "-O2"], ["-lm"]),
    "asan": ("impai_table_test-asan.exe", "gcc",
             [*WARN, "-O1", "-g", "-fsanitize=address,undefined", "-fsanitize=float-cast-overflow",
              "-fno-sanitize-recover=all"], ["-lm"]),
    "wine": ("impai_table_test-wine.exe", "i686-w64-mingw32-gcc", [*WARN, "-O2", "-D__USE_MINGW_ANSI_STDIO=1"],
             ["-static", "-lm"]),
}
sys.path.insert(0, str(REPO / "tools" / "impai"))
import tablefile  # noqa: E402


def build(kind):
    exe, cc, flags, libs = BUILDS[kind]
    out = HERE / exe
    inputs = [Path(__file__), DDRAW / TEST, *(DDRAW / s for s in SRCS), *(DDRAW / "src").glob("*.h")]
    if out.exists() and all(out.stat().st_mtime >= p.stat().st_mtime for p in inputs if p.exists()):
        return out
    print(f"impai_table_test: building {exe}", flush=True)
    # the test's expected values are single-precision expressions that must round as the sources' do
    sse = SSE if kind == "wine" else []
    objs = []
    tmp = HERE / f".impai_table_{kind}"
    tmp.mkdir(exist_ok=True)
    for src in [TEST, *SRCS]:
        obj = tmp / (Path(src).stem + ".o")
        subprocess.run([cc, *flags, *sse, "-c", "-o", str(obj), src], cwd=DDRAW, check=True)
        objs.append(str(obj))
    subprocess.run([cc, *flags, "-o", str(out), *objs, *libs], cwd=DDRAW, check=True)
    return out


def wine_env():
    env = dict(os.environ)
    prefix = subprocess.run([sys.executable, str(REPO / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if prefix.returncode != 0:
        raise SystemExit(f"impai_table_test: no Wine prefix: {prefix.stderr.strip()}")
    env["WINEPREFIX"] = prefix.stdout.strip()
    env.setdefault("WINEDEBUG", "-all")
    return env


def run(kind, *args):
    exe = build(kind)
    if kind == "wine":
        r = subprocess.run(["wine", str(exe), *args], env=wine_env(), stdout=subprocess.PIPE)
        return r.returncode, r.stdout.replace(b"\r\n", b"\n")
    r = subprocess.run([str(exe), *args], stdout=subprocess.PIPE)
    return r.returncode, r.stdout


def file_check():
    """The C loader and tablefile.py read and write one format. A list of what failed."""
    bad = []
    with tempfile.TemporaryDirectory() as d:
        hand = Path(d) / "hand.tbl"
        rc, out = run("native", "hand", str(hand))
        c_hash = out.decode().split()[1] if rc == 0 else None
        t = tablefile.read(hand)
        if t.hash_hex() != c_hash:
            bad.append(f"the hand table: C wrote hash {c_hash}, tablefile.py reads {t.hash_hex()}")
        t.gains[tablefile.ROWS.index("aa_air")] = 0.75
        t.counter = 2.5
        t.base[tablefile.CLASSES.index("bomber")] = 0.125
        changed = Path(d) / "changed.tbl"
        tablefile.write(changed, t)
        rc, out = run("native", "load", str(changed))
        text = out.decode()
        if rc != 0 or not text.startswith(f"ok {t.hash_hex()}") or " 12:14:0.75" not in text or \
                "counter 2.5 " not in text:
            bad.append(f"a table tablefile.py wrote: the C loader says {text.strip()!r}")
        raw = bytearray(changed.read_bytes())
        raw[0] ^= 1
        changed.write_bytes(bytes(raw))
        rc, out = run("native", "load", str(changed))
        if rc != 1 or not out.startswith(b"refused -2"):
            bad.append(f"a file with a stale trailer: the C loader says {out.decode().strip()!r}")
    return bad


def main(argv):
    kinds = argv or list(BUILDS)
    results = {}
    for kind in kinds:
        rc, out = run(kind)
        results[kind] = out
        last = out.decode(errors="replace").strip().splitlines()[-1:] or ["(no output)"]
        print(f"impai_table_test {kind}: rc={rc} {last[0]}")
        if rc != 0:
            sys.stdout.write(out.decode(errors="replace"))
            return 1
    first = kinds[0]
    for kind in kinds[1:]:
        if results[kind] != results[first]:
            print(f"impai_table_test: the {kind} transcript differs from the {first} one")
            return 1
    if len(kinds) > 1:
        print(f"impai_table_test: {len(kinds)} builds, one transcript of {len(results[first].splitlines())} lines")
    if not argv:
        bad = file_check()
        for b in bad:
            print(f"impai_table_test: FILE {b}")
        if bad:
            return 1
        print("impai_table_test: tablefile.py and the C loader read and write one format")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
