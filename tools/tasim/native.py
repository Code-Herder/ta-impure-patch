#!/usr/bin/env python3
"""native.py -- tasim_run and the role classifier's test, built with the host's gcc.

    tools/tasim/native.py                 build tools/tasim/tasim_run-native.exe (-O2)
    tools/tasim/native.py test            build and run tests/rolestest.c under ASan and UBSan
    tools/tasim/native.py roles [--gamedir DIR] [--language L]
                                          `tasim_run roles` on an install, its JSON on stdout

The classifier (tagpu/ddraw/src/tasim_roles.c) is engine-free and Win32-free, as are the file
modules the driver reads an install with (tagpu_vfs, tagpu_hpi, tagpu_tdf, tagpu_buildmenu), so
this needs no Wine. Ours compile with -Wall -Wextra -Werror; tagpu_hpi needs lodepng's inflate,
which is vendored and built without them. The outputs end in `.exe` so the tree's `*.exe` ignore
covers them, as tagpu/ddraw/tests/buildmenutest.py's does.
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
STRICT = ["-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror"]
OURS = ["src/tasim_roles.c", "src/tasim_keys.c", "src/tasim_fixed.c"]
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
    srcs = OURS + FILES + ["src/lodepng.c", str(HERE / "tasim_run.c"), "src/tasim_roles.h", "src/tasim_keys.h",
                           "src/tasim_fixed.h", "src/tagpu_tdf.h"]
    if not stale(EXE, srcs):
        return EXE
    print(f"native: building {EXE.name}", file=sys.stderr, flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        objs = objects(tmp, "-O2", [])
        subprocess.run(["gcc", *STRICT, "-O2", "-Iinc", "-Isrc", "-o", str(EXE),
                        str(HERE / "tasim_run.c"), *OURS, *objs, "-lm"], cwd=DDRAW, check=True)
    return EXE


def build_test():
    srcs = OURS + ["src/tasim_roles.h", str(HERE / "tests" / "rolestest.c")]
    if stale(TEST_EXE, srcs):
        print(f"native: building {TEST_EXE.name}", file=sys.stderr, flush=True)
        subprocess.run(["gcc", *STRICT, "-O1", "-g", "-Isrc", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-o", str(TEST_EXE),
                        str(HERE / "tests" / "rolestest.c"), *OURS, "-lm"], cwd=DDRAW, check=True)
    return TEST_EXE


def mount_list(gamedir=None):
    """(the game directory, a file of its archives' full paths in the engine's mount order)."""
    sys.path.insert(0, str(TREE / "tools" / "wiki"))
    import tavfs
    root = tavfs.find_install(gamedir)
    v = tavfs.Vfs(root)
    f = tempfile.NamedTemporaryFile("w", suffix=".mounts", delete=False)
    f.write("".join(str(Path(root) / name) + "\n" for name, _arc in v.archives))
    f.close()
    return Path(root), Path(f.name)


def roles(gamedir=None, language="english"):
    """`tasim_run roles` on the install: (its exit code, stdout, stderr)."""
    exe = build()
    root, mounts = mount_list(gamedir)
    try:
        r = subprocess.run([str(exe), "roles", "--cwd", str(root), "--archives", str(mounts),
                            "--language", language, "--json"], capture_output=True, text=True)
    finally:
        mounts.unlink()
    return r.returncode, r.stdout, r.stderr


def main(argv):
    if argv[:1] == ["test"]:
        return subprocess.run([str(build_test())]).returncode
    if argv[:1] == ["roles"]:
        import argparse
        p = argparse.ArgumentParser(prog="native.py roles")
        p.add_argument("--gamedir")
        p.add_argument("--language", default="english")
        a = p.parse_args(argv[1:])
        rc, out, err = roles(a.gamedir, a.language)
        sys.stdout.write(out)
        sys.stderr.write(err)
        return rc
    build()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
