#!/usr/bin/env python3
"""native.py -- tasim_run and the role classifier's test, built with the host's gcc.

    tools/tasim/native.py                 build tools/tasim/tasim_run-native.exe (-O2)
    tools/tasim/native.py test            build and run tests/rolestest.c under ASan and UBSan
    tools/tasim/native.py roles [--gamedir DIR] [--language L] [--archive A] [--folders F]
                                          `tasim_run roles` on an install, its JSON on stdout
    tools/tasim/native.py defs [--gamedir DIR] [--language L] [--archive A] [--folders F] [--features]
                                          `tasim_run defs` on an install (--features: with
                                          tools/tasim/engine-features.tsv's rows), its JSON on stdout
    tools/tasim/native.py fightlib        build tools/impai/fightlib-native.so, ImpAI's fight predictor
                                          as a shared library for tools/impai/predfit.py

A mod whose loader renames things in memory is read as the game reads it with --archive, the first
mount pattern (ProTA's `ProTA.gp3`), and --folders, the renamed folders
(`gui=guiP,gamedata=gamedatP,download=downloadP`): tools/wiki/sidepacks.py's RECORDS hold both for
each mod of the compatibility suite.

The classifier (tagpu/ddraw/src/tasim_roles.c) and the key table (tasim_keys.c) are engine-free and
Win32-free, as are the file modules the install loader (tools/tasim/tasim_load.c) reads an install
with (tagpu_vfs, tagpu_hpi, tagpu_tdf, tagpu_buildmenu), so this needs no Wine. Ours compile with
-Wall -Wextra -Werror; tagpu_hpi needs lodepng's inflate, which is vendored and built without them.
The outputs end in `.exe` so the tree's `*.exe` ignore covers them, as
tagpu/ddraw/tests/buildmenutest.py's does.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
DDRAW = TREE / "tagpu" / "ddraw"
EXE = HERE / "tasim_run-native.exe"
TEST_EXE = HERE / "tests" / "rolestest-native.exe"
FEATURES = HERE / "engine-features.tsv"
STRICT = ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror"]
OURS = ["src/tasim_roles.c", "src/tasim_keys.c", "src/tasim_fixed.c"]
DRIVER = [str(HERE / "tasim_run.c"), str(HERE / "tasim_load.c")]
FILES = ["src/tagpu_vfs.c", "src/tagpu_hpi.c", "src/tagpu_tdf.c", "src/tagpu_buildmenu.c",
         "src/tagpu_guifile.c"]
# the file modules are built as the wiki's asset check builds them (tools/wiki/assetcheck.py)
FILE_FLAGS = ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wno-format-truncation", "-Wno-restrict"]


def stale(out, srcs):
    return not out.exists() or any(out.stat().st_mtime < (DDRAW / s if not Path(s).is_absolute()
                                                          else Path(s)).stat().st_mtime
                                   for s in srcs)


def objects(tmp, opt, san):
    objs = []
    lode = Path(tmp) / "lodepng.o"
    subprocess.run(["gcc", "-std=c99", opt, "-Iinc", "-Isrc", *san, "-c", "-o", str(lode),
                    "src/lodepng.c"], cwd=DDRAW, check=True)
    objs.append(str(lode))
    for src in FILES:
        o = Path(tmp) / (Path(src).stem + ".o")
        subprocess.run(["gcc", *FILE_FLAGS, opt, "-Iinc", "-Isrc", *san, "-c", "-o", str(o), src],
                       cwd=DDRAW, check=True)
        objs.append(str(o))
    return objs


def build():
    srcs = OURS + FILES + DRIVER + ["src/lodepng.c", str(HERE / "tasim_load.h"), "src/tasim_roles.h",
                                    "src/tasim_keys.h", "src/tasim_fixed.h", "src/tagpu_tdf.h",
                                    "src/tagpu_vfs.h", "src/tagpu_buildmenu.h"]
    if not stale(EXE, srcs):
        return EXE
    print(f"native: building {EXE.name}", file=sys.stderr, flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        objs = objects(tmp, "-O2", [])
        subprocess.run(["gcc", *STRICT, "-O2", "-Iinc", "-Isrc", f"-I{HERE}", "-o", str(EXE),
                        *DRIVER, *OURS, *objs, "-lm"], cwd=DDRAW, check=True)
    return EXE


def build_test():
    srcs = OURS + ["src/tasim_roles.h", str(HERE / "tests" / "rolestest.c")]
    if stale(TEST_EXE, srcs):
        print(f"native: building {TEST_EXE.name}", file=sys.stderr, flush=True)
        subprocess.run(["gcc", *STRICT, "-O1", "-g", "-Isrc", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-o", str(TEST_EXE),
                        str(HERE / "tests" / "rolestest.c"), *OURS, "-lm"], cwd=DDRAW, check=True)
    return TEST_EXE


FIGHTLIB = TREE / "tools" / "impai" / "fightlib-native.so"
FIGHT_SRCS = ["src/impai_fight.c", "src/impai_fitparams.c", "src/impai_table.c"]


def fightlib():
    """ImpAI's fight predictor and its army buy as a native shared library: the DLL's own sources with the
    Makefile's IMPAI rules' rounding (no contraction; SSE2 is the x86-64 default), so the fit calls the C the DLL runs and
    tagpu/ddraw/tests/impai_fight_test.py holds the two to one transcript. The path."""
    glue = TREE / "tools" / "impai" / "fightlib.c"
    if stale(FIGHTLIB, FIGHT_SRCS + ["src/impai_fight.h", "src/impai_table.h", "src/tasim_roles.h", str(glue)]):
        print(f"native: building {FIGHTLIB.name}", file=sys.stderr, flush=True)
        subprocess.run(["gcc", *STRICT, "-ffp-contract=off", "-O2", "-fPIC", "-shared", "-Isrc", "-Iinc",
                        "-o", str(FIGHTLIB), str(glue), *FIGHT_SRCS, "-lm"], cwd=DDRAW, check=True)
    return FIGHTLIB


def mount_list(gamedir=None, archive=None):
    """(the game directory, a file of its archives' full paths in the engine's mount order). `archive`
    replaces the first mount pattern the executable spells, as a mod's loader does in memory."""
    sys.path.insert(0, str(TREE / "tools" / "wiki"))
    import tavfs
    root = tavfs.find_install(gamedir)
    v = tavfs.Vfs(root, None, archive)
    f = tempfile.NamedTemporaryFile("w", suffix=".mounts", delete=False)
    f.write("".join(str(Path(root) / name) + "\n" for name, _arc in v.archives))
    f.close()
    return Path(root), Path(f.name)


def run(cmd, gamedir=None, language="english", archive=None, folders=None, extra=()):
    """`tasim_run CMD` on the install: (its exit code, stdout, stderr)."""
    exe = build()
    root, mounts = mount_list(gamedir, archive)
    args = [str(exe), cmd, "--cwd", str(root), "--archives", str(mounts), "--language", language]
    if folders:
        args += ["--folders", folders]
    try:
        r = subprocess.run([*args, *extra, "--json"], capture_output=True, text=True)
    finally:
        mounts.unlink()
    return r.returncode, r.stdout, r.stderr


def roles(gamedir=None, language="english", archive=None, folders=None):
    """`tasim_run roles` on the install: (its exit code, stdout, stderr)."""
    return run("roles", gamedir, language, archive, folders)


def main(argv):
    if argv[:1] == ["test"]:
        return subprocess.run([str(build_test())]).returncode
    if argv[:1] == ["fightlib"]:
        print(fightlib())
        return 0
    if argv[:1] in (["roles"], ["defs"]):
        import argparse
        p = argparse.ArgumentParser(prog=f"native.py {argv[0]}")
        p.add_argument("--gamedir")
        p.add_argument("--language", default="english")
        p.add_argument("--archive", help="the first mount pattern, replacing the executable's")
        p.add_argument("--folders", help="gui=..,gamedata=..,download=..: folders replacing the executable's")
        if argv[0] == "defs":
            p.add_argument("--features", action="store_true", help="with engine-features.tsv's rows")
        a = p.parse_args(argv[1:])
        extra = ["--features", str(FEATURES)] if argv[0] == "defs" and a.features else []
        rc, out, err = run(argv[0], a.gamedir, a.language, a.archive, a.folders, extra)
        sys.stdout.write(out)
        sys.stderr.write(err)
        return rc
    build()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
