#!/usr/bin/env python3
"""Execute the original retail unit allocator with copied multiplayer block ordering.

Its verified absolute operands are relocated and only its three allocation calls
are replaced. This launches no game, never calls
the exe entry point, and proves neither native replay playback nor its loader route.
"""
import argparse
import hashlib
import json
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
    original = root / "pristine/TotalA.exe.pristine"
    directory = env = None
    display = tareplay.start_xvfb()
    try:
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-arena-test.exe"
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-I", TREE / "tagpu/ddraw/src",
                     "-O2", "-Wall", "-Wextra", "-Werror", "-std=c99", "-Wl,--image-base,0x18000000",
                     "-o", exe, HERE / "recmp_arena_test.c", TREE / "tagpu/ddraw/src/tagpu_recmp_arena.c",
                     TREE / "tagpu/ddraw/src/tagpu_recmp_players.c", TREE / "tagpu/ddraw/src/tagpu_detour.c",
                     "-lole32", "-luuid"], log)
            instances = root / "tagpu/instances"
            instances.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix="recmp-arena-", dir=instances))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display, WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        with (output / "arena.log").open("w") as log:
            command(["wine", exe, "Z:" + str(original).replace("/", "\\")], log, env=env)
        (output / "evidence.json").write_text(json.dumps(dict(
            original="pristine/TotalA.exe.pristine", original_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
            fixture_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), copied_permutations=640,
            native_reordered_allocations=206, native_original_ordering_controls=2,
            engine_range="0x4854A0..0x48593B", boundary="0x48587E", engine_dependencies=["0x435100", "0x485940", "0x488920"],
            absolute_operands_relocated=24, substituted_dependency="0x4D83B0 heap allocation only",
            full_game=False, saved_playback=False), indent=2) + "\n")
        print("PASS: relocated retail allocator and copied owner block order; full game/playback untested", flush=True)
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
