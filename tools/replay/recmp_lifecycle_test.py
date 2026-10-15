#!/usr/bin/env python3
"""Check owner lifetime dispatch against a saved native application prefix.

Owns one Wine prefix/private display, without launching a game. The production
consumer emits copied engine actions; actual engine application remains a
separate gate. The Python audit derives actions independently from file bytes.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import devpy
import tarec
import tareplay
from recmp_file import History, RECMP_ROOT_CREATE_BEGIN
from recmp_net_test import command

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]


def audit(path, output):
    blocks, stopped = tarec.blocks(path)
    if stopped:
        raise ValueError(stopped)
    segment = tarec.segments(blocks)[0]
    history = History(segment["header"])
    records, footer = [], None
    for kind, _, _, raw in blocks:
        if kind == tarec.RB_CONTRIBUTIONS:
            at = 0
            while at < len(raw):
                span = struct.unpack_from("<I", raw, at+28)[0]
                records.append(bytes(raw[at:at+16+span]))
                at += 16+span
            if at != len(raw):
                raise ValueError("contribution block span")
        elif kind == tarec.RB_END:
            footers = [b for tag, b in tarec.entries(raw) if tag == tarec.RT_MP_END]
            if len(footers) != 1 or footer is not None:
                raise ValueError("missing/duplicate multiplayer footer")
            footer = bytes(footers[0])
    if footer is None or segment["end_reason"] != 1:
        raise ValueError("complete native recording required")
    slots, constructor, deaths = {}, {}, {}
    counts = [0]*9
    crc, size, constructor_children, death_children = 0, 0, 0, 0
    proof, observations = [], []

    def emit(raw, action, index, slot, root, birth, before, image):
        nonlocal crc, size
        event = raw[:80]+struct.pack("<7I", action, index, slot, root, birth, before, len(image))+image
        crc, size = zlib.crc32(event, crc), size+len(event)
        counts[action] += 1

    for raw in records:
        history.block(raw)
        ordinal, applied, phase = struct.unpack_from("<QII", raw)
        _, origin, tick, _, kind = struct.unpack_from("<5I", raw, 28)
        if phase == 3:
            proof.append(raw)
            continue
        body = raw[80:]
        if kind == 8:
            at = 4
            for index in range(struct.unpack_from("<I", body)[0]):
                slot, live, full, birth, bits, _ = struct.unpack_from("<HBBI11sB", body, at)
                at += 20+4*int.from_bytes(bits, "little").bit_count()
                current = history.units[slot]
                image = current["image"] if live else b""
                unit_type = struct.unpack_from("<H", image, 0xA6)[0] if live else 0
                old = slots.get(slot)
                if old is None:
                    if not full:
                        raise ValueError("missing first unit baseline")
                    action = 1 if live else 7
                else:
                    if (old["birth"] != birth or old["phase"] != ("live" if live else "absent") or
                            live and old["type"] != unit_type):
                        raise ValueError(f"owner image lacks completed lifetime {origin}/{slot}/{birth}")
                    action = 6 if live else 7
                slots[slot] = dict(phase="live" if live else "absent", birth=birth, type=unit_type)
                emit(raw, action, index, slot, 0, birth, 0, bytes(image))
        elif kind == 10:
            source = history.maps[origin]["roster"]
            mapping = source.mapping(history.roster)
            at = 4
            for index in range(struct.unpack_from("<I", body)[0]):
                root, length, before, _, birth = struct.unpack_from("<HHB3sI", body, at)
                at += 12
                image = bytearray(body[at:at+length]); at += length
                slot = struct.unpack_from("<I", image)[0]-1
                old = slots.get(slot)
                if root in (RECMP_ROOT_CREATE_BEGIN, tarec.RK_EV_CREATE):
                    image[6] = mapping[image[6]]
                    unit_type = struct.unpack_from("<H", image, 4)[0]
                    if root == RECMP_ROOT_CREATE_BEGIN:
                        if old and (old["phase"] != "absent" or birth <= old["birth"]):
                            raise ValueError("occupied or repeated constructor")
                        constructor[slot] = (tick, before, bytes(image[:24]))
                        slots[slot] = dict(phase="creating", birth=birth, type=unit_type)
                        action = 2
                    else:
                        if (not old or old["phase"] != "creating" or old["birth"] != birth or
                                constructor.pop(slot) != (tick, before, bytes(image[:24]))):
                            raise ValueError("constructor success mismatch")
                        old["phase"] = "live"
                        action = 3
                else:
                    if not old or old["birth"] != birth or old["phase"] not in ("creating", "live", "dying"):
                        raise ValueError("private root outside its exact incarnation")
                    if root == tarec.RK_EV_DEATH:
                        image[8] = mapping[image[8]]
                        image[12:] = source.unit(history.roster, origin, slot, image[12:])
                        if old["phase"] != "live" or struct.unpack_from("<H", image, 12+0xA6)[0] != old["type"]:
                            raise ValueError("death image mismatch")
                        deaths[slot] = (tick, before, bytes(image))
                        old["phase"] = "dying"
                        action = 4
                    elif root == tarec.RK_EV_DEATH_END:
                        start_tick, start_before, entry = deaths.pop(slot)
                        if old["phase"] != "dying" or (tick, before) != (start_tick, start_before):
                            raise ValueError("death return mismatch")
                        image = entry
                        old["phase"], old["type"] = "absent", 0
                        action = 5
                    else:
                        constructor_children += old["phase"] == "creating"
                        death_children += old["phase"] == "dying"
                        observations.append(dict(ordinal=ordinal, applied=applied, origin=origin,
                                                 slot=slot, birth=birth, source=tick, phase=old["phase"],
                                                 kind=root))
                        action = 8
                emit(raw, action, index, slot, root, birth, before, bytes(image))
    history.footer(footer)
    if constructor or deaths or any(slots.get(slot, {}).get("phase") not in ("live", "absent")
                                    for _, _, first, last in history.roster.actors
                                    for slot in range(first, last+1)):
        raise ValueError("unclosed lifetime or missing initial owned slot")
    summary = dict(application_records=history.ledger.records, action_bytes=size, action_crc=f"{crc:08X}",
                   constructor_children=constructor_children, death_children=death_children,
                   actions=counts[1:], owned_slots=len(slots), proof_records=len(proof))
    (output / "tail.bin").write_bytes(footer+struct.pack("<I", sum(map(len, proof)))+b"".join(proof))
    (output / "expected.json").write_text(json.dumps(summary, indent=2)+"\n")
    (output / "private-roots.json").write_text(json.dumps(observations, indent=2)+"\n")
    return summary


def run(output, native_file):
    root = tareplay.main_checkout()
    directory = env = None
    display = tareplay.start_xvfb()
    try:
        with (output / "setup.log").open("w") as log:
            sources = ("tagpu_recmp", "tagpu_recmp_auth", "tagpu_recmp_record", "tagpu_recmp_stream",
                       "tagpu_recmp_players", "tagpu_recmp_file", "tagpu_recmp_file_state",
                       "tagpu_recmp_history", "tagpu_recmp_units", "tagpu_recmp_named", "tagpu_recmp_roots",
                       "tagpu_recmp_explored", "tagpu_recmp_text", "tagpu_recmp_pov", "tagpu_recmp_orders", "tagpu_recmp_lifecycle",
                       "tagpu_recwriter", "tagpu_recreader", "tagpu_recfile", "tagpu_reccodec", "tagpu_reclossy",
                       "tagpu_recframe_cells", "tagpu_recframe_orders", "tagpu_recframe_events",
                       "tagpu_recframe_state", "zstd", "crc32")
            exe = output / "recmp-lifecycle-test.exe"
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2", "-Wall",
                     "-Wextra", "-Werror", "-std=c99", "-o", exe, HERE / "recmp_lifecycle_test.c",
                     *(TREE / f"tagpu/ddraw/src/{s}.c" for s in sources), "-lole32", "-luuid"], log)
            directory = Path(tempfile.mkdtemp(prefix="recmp-life-", dir=root / "tagpu/instances"))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display, WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        with (output / "lifecycle.log").open("w") as log:
            args = ["wine", exe]
            expected = None
            if native_file:
                expected = audit(native_file, output)
                args += ["native", "Z:"+str(native_file).replace("/", "\\"),
                         "Z:"+str(output / "tail.bin").replace("/", "\\")]
            command(args, log, env=env)
        if expected:
            rows = [line.split()[1:] for line in (output / "lifecycle.log").read_text().splitlines()
                    if line.startswith("NATIVE_LIFECYCLE ")]
            values = [str(expected["action_bytes"]), expected["action_crc"],
                      str(expected["constructor_children"]), str(expected["death_children"]),
                      *map(str, expected["actions"])]
            if rows != [values]:
                raise RuntimeError("native C/Python lifetime actions disagree")
            print("PASS: native C/Python owner lifetime action bytes/order through fresh reader seek", expected, flush=True)
        print("PASS: copied owner lifetime controls; native engine application remains untested", flush=True)
    finally:
        if env:
            subprocess.run(["wineserver", "-k"], env=env, timeout=10, check=False)
        if directory:
            shutil.rmtree(directory)
        tareplay.stop_xvfb(display)


def main():
    devpy.ensure()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--native-file", type=Path)
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(output, args.native_file.resolve() if args.native_file else None)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
