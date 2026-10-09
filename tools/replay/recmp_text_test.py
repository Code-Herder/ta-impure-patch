#!/usr/bin/env python3
"""Run copied multiplayer text identity, audience and queue controls."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import struct
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
            exe = output / "recmp-text-test.exe"
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2", "-Wall",
                     "-Wextra", "-Werror", "-std=c99", "-o", exe, HERE / "recmp_text_test.c",
                     TREE / "tagpu/ddraw/src/tagpu_recmp_text.c",
                     *(TREE / f"tagpu/ddraw/src/{name}.c" for name in
                       ("tagpu_recmp_players", "tagpu_recmp_history", "tagpu_recmp_file_state",
                        "tagpu_recmp_stream", "tagpu_recmp_units", "tagpu_recmp_named",
                        "tagpu_recmp_roots", "tagpu_recmp_orders", "tagpu_recmp_explored", "tagpu_recmp_pov",
                        "crc32")),
                     "-lole32", "-luuid"], log)
            instances = root / "tagpu/instances"
            instances.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix="recmp-text-", dir=instances))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display,
                       WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        with (output / "text.log").open("w") as log:
            command(["wine", exe], log, env=env, cwd=output)
        check_history(output)
        print("PASS: copied text identities, actual audiences, bounded joins and ordered queue", flush=True)
    finally:
        if env:
            subprocess.run(["wineserver", "-k"], env=env, timeout=10, check=False)
        if directory:
            shutil.rmtree(directory)
        tareplay.stop_xvfb(display)


def check_history(output):
    from recmp_file import History
    for original in (False, True):
        stem = "text-history" if original else "text-history-missing"
        raw = (output / f"{stem}.bin").read_bytes()
        end = (output / f"{stem}-end.bin").read_bytes()
        rows = [struct.unpack_from("<IIII", raw, 112+16+16*i) for i in range(10)]
        header = dict(multiplayer=raw[:112], mp_seats=raw[112:288], slots=21, units_each=2,
                      features=(2, 2), players=[dict(index=r[1], controller=1 if i == 0 else 3)
                                               for i, r in enumerate(rows)])
        history = History(header)
        history.block(raw[288:])
        try:
            history.footer(end)
        except ValueError as error:
            if original or "lacks its byte-exact original" not in str(error):
                raise
        else:
            if not original:
                raise ValueError("missing original accepted by Python history")
        if history.at(200).text:
            raise ValueError("receipt published before its application tick")
        for tick in (201, 202, 1000, 201):
            state = history.at(tick)
            if set(state.text) != {11}:
                raise ValueError("proof original fabricated application text")
            text = state.text[11]
            event = text["events"][0]
            if (text["source_tick"], text["applied_tick"], event["origin"], event["recipient"],
                    event["event"], event["source_tick"], event["stock"]) != \
                    (1, 201, 55, 11, 123, 0, b"\x05"+64*b"T"):
                raise ValueError("receipt identity/bytes/independent clocks changed through seek")
        if original and (55 not in history.source_text or 55 in history.text):
            raise ValueError("proof source validation/publication isolation")
    print("PASS: C/Python persisted text joins, absent-original refusal and seek/proof isolation", flush=True)


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
