#!/usr/bin/env python3
"""Check the production primary receive core with controlled SDK boundaries.

The native menu fixture exercises the installed wrapper. These controls use
the exact core functions, replacing only the engine receive/statistics targets
with deterministic fixtures. They cover buffer ownership and nested/concurrent
dispatch, not native primary transport or loading-chat admission.
"""
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


def core(output):
    source = (TREE / "tagpu/ddraw/src/tagpu_recorder.c").read_text()
    start = source.index("/* The queued receiver copies")
    end = source.index("/* DISASSEMBLED: only the host", start)
    copied = source[start:end]
    targets = {
        "*(const unsigned*)0x506DBC": "s_test_queued",
        "(size_t)0x4C9840": "(size_t)raw_receive",
        "(size_t)0x462F30": "(size_t)queued_receive",
        "(size_t)0x415EF0": "(size_t)message_stats",
        "(size_t)0x415F40": "(size_t)packet_stats",
    }
    for old, new in targets.items():
        if copied.count(old) != 1:
            raise ValueError(f"receive fixture boundary changed: {old}")
        copied = copied.replace(old, new)
    patches = (TREE / "tagpu/ddraw/src/tagpu_patches.c").read_text()
    for declaration in ("unsigned int tagpu_wire_received(",
                        "static int __cdecl wire_s2c_copy("):
        start = patches.index(declaration)
        end = patches.index("\n}\n", start) + 3
        copied += "\n" + patches[start:end]
    (output / "recmp-receive-core.inc").write_text(copied)


def run(output):
    directory = env = None
    display = tareplay.start_xvfb()
    try:
        core(output)
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-receive-test.exe"
            command(["i686-w64-mingw32-gcc", "-I", output, "-O2", "-Wall", "-Wextra",
                     "-Werror", "-std=c11", HERE / "recmp_receive_test.c", "-o", exe], log)
            directory = Path(tempfile.mkdtemp(prefix="recmp-receive-"))
            env = dict(os.environ, WINEPREFIX=str(directory), DISPLAY=display.display,
                       WINEDEBUG="-all")
        with (output / "receive.log").open("w") as log:
            command(["wine", exe], log, env=env)
        print("PASS: owned receive storage, exact spans and nested/concurrent dispatch", flush=True)
    finally:
        if env:
            subprocess.run(["wineserver", "-k"], env=env, timeout=10, check=False)
            subprocess.run(["wineserver", "-w"], env=env, timeout=10, check=False)
        if directory:
            shutil.rmtree(directory)
        tareplay.stop_xvfb(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    output = parser.parse_args().out.resolve()
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
