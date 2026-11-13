#!/usr/bin/env python3
"""tasim_link_test.py -- link the shared simulation core without the in-game wiki's front end and play it.

    tagpu/ddraw/tests/tasim_link_test.py

tests/tasim_link_test.c is linked with every src/tasim_*.c and what the core calls -- the COB host and its VM, the
entry loader the sight table comes from, the shapes' geometry -- and never with src/tagpu_wikisim.c, so a core file
that reached the front end would fail the link. It is built three ways, as tests/tasim_fixed_test.py builds its own:
`host`, gcc -O2; `asan`, AddressSanitizer and UBSan, every finding fatal; `wine`, the DLL's compiler with the
core's SSE2 flags, run under Wine in the wiki checks' prefix (tools/wiki/wineprefix.py). Each must pass its claims
and the three must print the same lines, the core's state hash among them.

Exit 0 when they do, 1 when not, 2 when a build cannot be made or run."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
TREE = DDRAW.parents[1]
FRONT = "src/tagpu_wikisim.c"
CORE = sorted(f"src/{p.name}" for p in (DDRAW / "src").glob("tasim_*.c"))
DEPS = ["src/tagpu_wikicob.c", "src/tagpu_cobvm.c", "src/tagpu_cob_safe.c", "src/tagpu_wikidata.c",
        "src/tagpu_placegeo.c", "src/tagpu_sweepgeo.c", "src/tagpu_json.c", "src/tagpu_pak.c"]
SRCS = ["tests/tasim_link_test.c", *CORE, *DEPS]
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
BUILDS = {
    "host": ["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Isrc", "-Iinc"],
    "asan": ["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Isrc", "-Iinc", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all"],
    "wine": ["i686-w64-mingw32-gcc", "-std=c99", "-O2", "-Wall", "-Isrc", "-Iinc", "-D__USE_MINGW_ANSI_STDIO=1", *SSE,
             "-static"],
}


def wine_env():
    r = subprocess.run([sys.executable, str(TREE / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"no Wine prefix: {r.stderr.strip()}")
    return {**os.environ, "WINEPREFIX": r.stdout.strip().splitlines()[-1], "WINEDEBUG": "-all"}


def main():
    assert FRONT not in SRCS
    outs = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, flags in BUILDS.items():
            exe = Path(tmp) / f"tasim_link_test-{name}.exe"
            r = subprocess.run([*flags, "-o", str(exe), *SRCS, "-lm"], cwd=DDRAW, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"tasim_link_test: the {name} build failed:\n{r.stderr.strip()}")
                return 2
            try:
                cmd, env = (["wine", str(exe)], wine_env()) if name == "wine" else ([str(exe)], None)
                r = subprocess.run(cmd, capture_output=True, text=True, env=env)
            except (RuntimeError, OSError) as e:
                print(f"tasim_link_test: the {name} build cannot run: {e}")
                return 2
            outs[name] = r.stdout.replace("\r\n", "\n")
            print(f"== {name}: exit {r.returncode}\n{outs[name]}{r.stderr.strip()}")
            if r.returncode != 0:
                return 1
    bad = [n for n, o in outs.items() if o != outs["host"]]
    for n in bad:
        print(f"tasim_link_test: {n} prints otherwise than host")
    print("tasim_link_test: FAILED" if bad else "tasim_link_test: ok, the core links alone and the three builds agree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
