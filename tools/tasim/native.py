#!/usr/bin/env python3
"""native.py -- tasim_run and the role classifier's test, built with the host's gcc.

    tools/tasim/native.py                 build tools/tasim/tasim_run-native.exe (-O2)
    tools/tasim/native.py test            build and run tests/rolestest.c under ASan and UBSan
    tools/tasim/native.py roles [--gamedir DIR] [--language L]
                                          `tasim_run roles` on an install, its JSON on stdout
    tools/tasim/native.py defs [--gamedir DIR] [--language L] [--features]
                                          `tasim_run defs` on an install (--features: with
                                          tools/tasim/engine-features.tsv's rows), its JSON on stdout
    tools/tasim/native.py fightlib        build tools/impai/fightlib-native.so, ImpAI's fight predictor
                                          as a shared library for tools/impai/predfit.py

Every install is read through its profile (tools/wiki/tavfs.py, `profile`): the folders, patterns,
extensions, version threshold and mounts its running image reads, the exe with each imported Patch
Loader's list applied. A mod's own exe and a Patch Loader's renames are its own; nothing names a mod.
A folder tavfs refuses (a managed mod, a site whose code is not the one modelled) does not run.

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


class ProfileError(Exception):
    """tavfs could not read the install's profile: the message says why."""


def profile_file(gamedir=None, language=None):
    """(the game directory, a temporary `tasim-profile/1` file of its profile), which the caller unlinks. `language`
    replaces the image's default. ProfileError when tavfs refuses the folder."""
    sys.path.insert(0, str(TREE / "tools" / "wiki"))
    import tavfs
    root = Path(tavfs.find_install(gamedir))
    try:
        prof = tavfs.profile(root, language)
    except tavfs.VfsError as e:
        raise ProfileError(str(e)) from None
    f = tempfile.NamedTemporaryFile("w", suffix=".profile", delete=False)
    f.close()
    try:
        tavfs.write_profile(prof, Path(f.name))
    except tavfs.VfsError as e:
        Path(f.name).unlink()
        raise ProfileError(str(e)) from None
    return root, Path(f.name)


def run(cmd, gamedir=None, language=None, extra=()):
    """`tasim_run CMD` on the install: (its exit code, stdout, stderr); (2, "", "profile: ...") when its profile does
    not read."""
    exe = build()
    try:
        _root, prof = profile_file(gamedir, language)
    except ProfileError as e:
        return 2, "", f"profile: {e}"
    try:
        r = subprocess.run([str(exe), cmd, "--profile", str(prof), *extra, "--json"], capture_output=True, text=True)
    finally:
        prof.unlink()
    return r.returncode, r.stdout, r.stderr


def roles(gamedir=None, language=None):
    """`tasim_run roles` on the install: (its exit code, stdout, stderr)."""
    return run("roles", gamedir, language)


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
        p.add_argument("--language", help="the engine's language value, replacing the image's default")
        if argv[0] == "defs":
            p.add_argument("--features", action="store_true", help="with engine-features.tsv's rows")
        a = p.parse_args(argv[1:])
        extra = ["--features", str(FEATURES)] if argv[0] == "defs" and a.features else []
        rc, out, err = run(argv[0], a.gamedir, a.language, extra)
        sys.stdout.write(out)
        sys.stderr.write(err)
        return rc
    build()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
