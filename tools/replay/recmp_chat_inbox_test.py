#!/usr/bin/env python3
"""Exercise the production copied chat inbox and game text queue under Wine."""
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
    display = tareplay.start_xvfb()
    directory = env = None
    try:
        source = (TREE / "tagpu/ddraw/src/tagpu_recorder.c").read_text()
        start = source.index("static int __cdecl mp_chat_receive(")
        end = source.index("\n}\n", start) + 3
        (output / "recmp-chat-receive.inc").write_text(source[start:end])
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-chat-inbox-test.exe"
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2",
                     "-I", output, "-Wall", "-Wextra", "-Werror", "-std=c99", "-o", exe,
                     HERE / "recmp_chat_inbox_test.c",
                     *(TREE / f"tagpu/ddraw/src/{name}.c" for name in
                       ("tagpu_recmp_text", "tagpu_recmp_players",
                        "tagpu_recmp_chat_wire", "crc32")), "-lole32", "-luuid"], log)
            directory = Path(tempfile.mkdtemp(prefix="recmp-chat-inbox-"))
            env = dict(os.environ, WINEPREFIX=str(directory), DISPLAY=display.display,
                       WINEDEBUG="-all")
        with (output / "inbox.log").open("w") as log:
            command(["wine", exe], log, env=env)
        print("PASS: copied loading inbox, truthful clocks and producer handoff", flush=True)
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
