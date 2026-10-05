#!/usr/bin/env python3
"""Run copied named-root and bounded queue controls in an owned Wine prefix."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import tareplay
from recmp_net_test import command

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]


def run(output):
    root = tareplay.main_checkout()
    directory = env = None
    display = tareplay.start_xvfb()
    try:
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-named-test.exe"
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2", "-Wall",
                     "-Wextra", "-Werror", "-std=c99", "-o", exe, HERE / "recmp_named_test.c",
                     TREE / "tagpu/ddraw/src/tagpu_recmp_named.c",
                     TREE / "tagpu/ddraw/src/tagpu_recmp_players.c", "-lole32", "-luuid"], log)
            instances = root / "tagpu/instances"
            instances.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix="recmp-named-", dir=instances))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display,
                       WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        with (output / "named.log").open("w") as log:
            command(["wine", exe], log, env=env)
        print("PASS: copied named roots and bounded ordered staging controls", flush=True)
    finally:
        if env:
            subprocess.run(["wineserver", "-k"], env=env, timeout=10, check=False)
        if directory:
            shutil.rmtree(directory)
        tareplay.stop_xvfb(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(output)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
