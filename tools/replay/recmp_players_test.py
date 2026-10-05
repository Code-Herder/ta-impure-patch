#!/usr/bin/env python3
"""Check bounded owner player images and their persisted contribution history.

One owned Wine prefix/private display; no game launch or primary session.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

import tareplay
from recmp_file import Contributions
from recmp_net_test import command

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]


def persisted(raw):
    ledger = Contributions(raw[:112])
    ledger.block(raw[112:])
    if ledger.records != 12 or ledger.consumed != 6 or any(not s["ended"] for s in ledger.source):
        raise ValueError("player journal final prefixes missing")
    states, at = {}, 112
    while at < len(raw):
        ordinal, application_tick, phase = struct.unpack_from("<QII", raw, at)
        at += 16
        span, origin, tick, sequence, kind = struct.unpack_from("<IIIII", raw, at+12)
        if kind == 6:
            body = raw[at+64:at+span]
            actor, controller, full = struct.unpack_from("<III", body)
            if actor != origin or controller not in (1, 2) or full not in (0, 1) or body[29:32] != bytes(3):
                raise ValueError("player delta prefix refused")
            bits = int.from_bytes(body[12:29], "little")
            if bits >> 130 or full and bits != (1 << 130)-1 or not full and origin not in states:
                raise ValueError("player delta mask/baseline refused")
            image = bytearray(520) if full else bytearray(states[origin][0])
            pos = 32
            for word in range(130):
                if bits >> word & 1:
                    if pos+4 > len(body):
                        raise ValueError("player delta cut short")
                    image[word*4:word*4+4] = body[pos:pos+4]
                    pos += 4
            if pos != len(body):
                raise ValueError("player delta trailing bytes")
            states[origin] = image, controller, tick, sequence, application_tick
        at += span
    for rank, origin in enumerate((11, 22, 55, 66)):
        expected = bytearray((i*37+origin) & 255 for i in range(520))
        struct.pack_into("<II", expected, 0x67, 2+2*rank, 3+2*rank)
        expected[0] ^= 1
        if states.get(origin) != (expected, 1+(rank & 1), 31, 2, 101):
            raise ValueError("player journal image/provenance mismatch")


def run(output):
    root = tareplay.main_checkout()
    directory = env = None
    display = tareplay.start_xvfb()
    try:
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-players-test.exe"
            sources = ("tagpu_recmp_players", "tagpu_recmp_record", "tagpu_recmp_stream",
                       "tagpu_recmp_file_state", "crc32")
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2", "-Wall",
                     "-Wextra", "-Werror", "-std=c99", "-o", exe, HERE / "recmp_players_test.c",
                     *(TREE / f"tagpu/ddraw/src/{s}.c" for s in sources), "-lole32", "-luuid"], log)
            instances = root / "tagpu/instances"
            instances.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix="recmp-players-", dir=instances))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display,
                       WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        path = output / "players.journal"
        with (output / "players.log").open("w") as log:
            command(["wine", exe, "Z:" + str(path).replace("/", "\\")], log, env=env)
        raw = path.read_bytes()
        persisted(raw)
        for altered in (raw[:-1], raw[:112+16+64] + bytes([raw[112+16+64] ^ 1]) + raw[112+16+65:]):
            try:
                persisted(altered)
            except ValueError:
                pass
            else:
                raise RuntimeError("altered player journal accepted")
        print("PASS: C player bounds and journal round trip; independent Python image/provenance and corrupt/missing-tail refusal", flush=True)
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
