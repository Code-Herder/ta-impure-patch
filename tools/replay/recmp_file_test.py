#!/usr/bin/env python3
"""Run the real recording writer/readers at controlled contribution boundaries.

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
import zlib

import tarec
import tareplay
from recmp_net_test import command

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]


def encoded(blocks):
    out = bytearray(tarec.REC_MAGIC)
    for kind, first, last, raw in blocks:
        head = struct.pack("<IHHIIIII", tarec.REC_BLOCK_MAGIC, kind, 1, first, last,
                           len(raw), len(raw), zlib.crc32(raw))
        out += head + struct.pack("<I", zlib.crc32(head)) + raw
    return out


def run(output, read_file=None):
    root = tareplay.main_checkout()
    directory = None
    display = tareplay.start_xvfb()
    env = None
    try:
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-file-test.exe"
            sources = ("tagpu_recmp", "tagpu_recmp_auth", "tagpu_recmp_record", "tagpu_recmp_stream",
                       "tagpu_recmp_file", "tagpu_recmp_file_state", "tagpu_recwriter",
                       "tagpu_recreader", "tagpu_recfile", "lodepng", "crc32")
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2", "-Wall",
                     "-Wextra", "-Werror", "-std=c99", "-o", exe, HERE / "recmp_file_test.c",
                     *(TREE / f"tagpu/ddraw/src/{s}.c" for s in sources), "-lole32", "-luuid"], log)
            directory = Path(tempfile.mkdtemp(prefix="recmp-file-", dir=root / "tagpu/instances"))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display,
                       WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        for mode in (() if read_file else ("normal", "missing_cut", "missing_receipt", "journal_full", "local_stop")):
            path = output / f"{mode}.imprec"
            winpath = "Z:" + str(path).replace("/", "\\")
            with (output / f"{mode}.log").open("w") as log:
                command(["wine", exe, mode, winpath], log, env=env)
            blocks, stopped = tarec.blocks(path)
            if stopped:
                raise RuntimeError(stopped)
            segment = tarec.segments(blocks)[0]
            expected = 1 if mode == "normal" else None
            if segment["end_reason"] != expected:
                raise RuntimeError(f"{mode}: unexpected complete end")
            print(f"{mode}: real writer, C reader/summary/info and Python reader PASS", flush=True)
        original = read_file or output / "normal.imprec"
        blocks, stopped = tarec.blocks(original)
        if read_file:
            with (output / "normal-read.log").open("w") as log:
                command(["wine", exe, "read", "Z:" + str(original).replace("/", "\\")], log, env=env)
        contribution = next(i for i, b in enumerate(blocks) if b[0] == tarec.RB_CONTRIBUTIONS)
        raw = blocks[contribution][3]
        # Mutate logical contents and rebuild outer block CRCs. These are semantic
        # refusal checks, independent of the container's torn-block detection.
        variants = {}
        altered = bytearray(raw); altered[16+64] ^= 1
        variants["bad_data"] = bytes(altered)
        altered = bytearray(raw); struct.pack_into("<Q", altered, 0, 2)
        variants["bad_ordinal"] = bytes(altered)
        variants["missing_tail"] = raw[:-1]
        for name, altered in variants.items():
            changed = list(blocks)
            changed[contribution] = (*blocks[contribution][:3], altered)
            path = output / f"{name}.imprec"
            path.write_bytes(encoded(changed))
            try:
                tarec.segments(changed)
            except tarec.Unreadable:
                pass
            else:
                raise RuntimeError(f"Python reader accepted {name}")
            with (output / f"{name}.log").open("w") as log:
                command(["wine", exe, "refuse", "Z:" + str(path).replace("/", "\\")], log, env=env)
            print(f"{name}: C reader/summary/info and Python semantic refusal PASS", flush=True)
        end_index = next(i for i, b in enumerate(blocks) if b[0] == tarec.RB_END)
        end_entries = list(tarec.entries(blocks[end_index][3]))
        for name in ("missing_footer", "bad_cut"):
            payload = bytearray()
            for tag, body in end_entries:
                if tag == tarec.RT_MP_END:
                    if name == "missing_footer":
                        continue
                    body = bytearray(body)
                    # The first remote actor's frozen consumed sequence is 1;
                    # a proof-tail seal does not turn it into application 2.
                    struct.pack_into("<I", body, 16+40*2+4, 2)
                payload += struct.pack("<HH", tag, len(body)) + body + bytes((-len(body)) % 4)
            changed = list(blocks)
            changed[end_index] = (*blocks[end_index][:3], bytes(payload))
            path = output / f"{name}.imprec"
            path.write_bytes(encoded(changed))
            try:
                tarec.segments(changed)
            except tarec.Unreadable:
                pass
            else:
                raise RuntimeError(f"Python reader accepted {name}")
            with (output / f"{name}.log").open("w") as log:
                command(["wine", exe, "refuse", "Z:" + str(path).replace("/", "\\")], log, env=env)
            print(f"{name}: C reader/summary/info and Python semantic refusal PASS", flush=True)
        print("PASS: real .imprec contribution blocks, cut footer, writer hand-over and failure controls", flush=True)
    finally:
        if env:
            subprocess.run(["wineserver", "-k"], env=env, timeout=10, check=False)
        if directory:
            shutil.rmtree(directory)
        tareplay.stop_xvfb(display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--read-file", type=Path, help="verify new reader controls against an existing fixture file")
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(output, args.read_file.resolve() if args.read_file else None)
    except (OSError, ValueError, RuntimeError, tarec.Unreadable, subprocess.SubprocessError,
            tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
