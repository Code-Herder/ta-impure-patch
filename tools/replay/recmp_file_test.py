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
from recmp_file import History
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


def network_create_controls(blocks, output, exe, log, env):
    expected = struct.pack("<IHBBIiiifHH", 6, 1, 2, 4, 0x80000011, 123456, -654321, 123, 0.25, 65535, 0)
    segment = tarec.segments(blocks)[0]
    observed = []
    tarec.stream_to(segment["header"], segment["blocks"], on_record=lambda w, k, b:
                    observed.append(bytes(b)) if k == tarec.RK_EV_NET_CREATE else None)
    if observed != [expected]:
        raise RuntimeError("Python network creation arguments/incarnation changed")

    def changed(body, legacy=False, drop=False):
        result = []
        for kind, first, last, raw in blocks:
            if legacy and kind == tarec.RB_HEADER:
                raw = b"".join(struct.pack("<HH", tag, len(b)) + b + bytes((-len(b)) % 4)
                               for tag, b in ((tag, struct.pack("<I", 5) if tag == tarec.RT_FORMAT else b)
                                              for tag, b in tarec.entries(raw)))
            if kind == tarec.RB_STREAM:
                raw = b"".join(struct.pack("<HH", rk, len(b)) + b + bytes((-len(b)) % 4)
                               for rk, b in ((rk, body if rk == tarec.RK_EV_NET_CREATE else b)
                                             for rk, b in tarec.records(raw))
                               if not (drop and rk == tarec.RK_EV_NET_CREATE))
            result.append((kind, first, last, raw))
        return result

    variants = {"short": expected[:-1], "long": expected+b"x"}
    for name, offset, fmt, value in (("null_unit", 0, "I", 0), ("slot_zero", 0, "I", 1),
                                    ("past_slots", 0, "I", 22), ("wrong_block", 6, "B", 0),
                                    ("past_owner", 6, "B", 10), ("past_water", 7, "B", 5),
                                    ("null_type", 4, "H", 0), ("past_type", 4, "H", 8),
                                    ("reserved", 30, "H", 1), ("nan_build", 24, "I", 0x7fc00000),
                                    ("negative_build", 24, "f", -0.25), ("past_build", 24, "f", 1.25)):
        b = bytearray(expected)
        struct.pack_into("<"+fmt, b, offset, value)
        variants[name] = bytes(b)
    for name, body in variants.items():
        altered = changed(body)
        seg = tarec.segments(altered)[0]
        try:
            tarec.stream_to(seg["header"], seg["blocks"])
        except tarec.Unreadable:
            pass
        else:
            raise RuntimeError("Python accepted network create "+name)
        path = output / ("network-create-"+name+".imprec")
        path.write_bytes(encoded(altered))
        command(["wine", exe, "world-refuse", "Z:"+str(path).replace("/", "\\")], log, env=env)
    altered = changed(expected, legacy=True)
    seg = tarec.segments(altered)[0]
    try:
        tarec.stream_to(seg["header"], seg["blocks"])
    except tarec.Unreadable:
        pass
    else:
        raise RuntimeError("Python accepted format-5 network create")
    path = output / "network-create-format5.imprec"
    path.write_bytes(encoded(altered))
    command(["wine", exe, "world-refuse", "Z:"+str(path).replace("/", "\\")], log, env=env)
    legacy = changed(expected, legacy=True, drop=True)
    seg = tarec.segments(legacy)[0]
    tarec.stream_to(seg["header"], seg["blocks"])
    path = output / "legacy-format5.imprec"
    path.write_bytes(encoded(legacy))
    command(["wine", exe, "read", "Z:"+str(path).replace("/", "\\")], log, env=env)
    print("network create: exact arguments/token/carried state, 15 primary semantic refusals and legacy format-5 reading PASS", flush=True)


def resealed(blocks):
    """Rebuild source DATA prefix seals too, so malformed bodies reach semantic checks."""
    sources, result = {}, []
    for kind, first, last, payload in blocks:
        if kind == tarec.RB_CONTRIBUTIONS:
            changed, at = bytearray(payload), 0
            while at < len(changed):
                at += 16
                span, origin, tick, sequence, source_kind = struct.unpack_from("<IIIII", changed, at+12)
                size, crc = sources.get(origin, (0, 0))
                if source_kind == 4:
                    final = struct.unpack_from("<I", changed, at+64+20)[0]
                    struct.pack_into("<IIQII", changed, at+64, sequence-1, tick, size, crc, final)
                else:
                    size += span
                    crc = zlib.crc32(changed[at:at+span], crc)
                    sources[origin] = size, crc
                at += span
            payload = bytes(changed)
        result.append((kind, first, last, payload))
    return result


def semantic_states(segment):
    history = History(segment["header"])
    for kind, _, _, payload in segment["blocks"]:
        if kind == tarec.RB_CONTRIBUTIONS:
            history.block(payload)
    if (history.at(99).players or history.at(99).orders or history.at(99).units or
            history.at(99).named or history.at(99).roots):
        raise RuntimeError("unknown authority was fabricated before first application")
    for tick in (100, 101):
        state = history.at(tick)
        for i, origin in enumerate((11, 22, 55, 66)):
            p = state.players.get(origin)
            if not p or (struct.unpack_from("<I", p["image"])[0], p["controller"], p["source_tick"],
                         p["sequence"], p["applied_tick"]) != (origin, 1+(i & 1), 100 if i < 2 else 8, 2, 100):
                raise RuntimeError("player authority/provenance changed across proof tail")
            sender = range(11) if i < 2 else (7, 4, 0, 2, 1, 3, 5, 6, 8, 9, 10)
            if p["image"][0x146] != i or any(p["image"][0x108+11*v+row] != 20*v+source+1
                                            for v in range(5) for row, source in enumerate(sender)):
                raise RuntimeError("source-local player vectors/self row were not mapped")
        for origin in (11, 22, 55):
            n = state.named.get(origin)
            if (not n or (n["source_tick"], n["sequence"], n["applied_tick"]) !=
                    (100 if origin < 55 else 8, 4, 100) or
                    [e["name"] for e in n["events"]] != [b"TakeDamage", b"TargetCleared"] or
                    [(e["birth"], e["before_capture"], struct.unpack_from("<I", e["args"])[0])
                     for e in n["events"]] != [(17, 0, origin), (17, 1, origin+1)]):
                raise RuntimeError("named root bytes/phase/provenance changed across proof tail")
            roots = state.roots.get(origin)
            unit = 2 if origin == 11 else 4 if origin == 22 else 6
            later = tick == 101 and origin < 55
            variant = (2 if origin == 11 else 1) if later else 0
            images = [struct.pack("<IBBBBIIII4s", unit, 1, 0, 0, 0, origin+variant, 0, 0, 0, b"Move"),
                      struct.pack("<IIBBBB2s", unit, 23, 1, 1, 0, 0, b"NO" if later else b"OK"),
                      struct.pack("<III", unit, 1, origin+variant) + b"\xA5"*32 + struct.pack("<I", 0xABCD0004),
                      struct.pack("<IHBBIIiiB3x", unit, 1, 0 if origin == 55 else origin//11-1,
                                  3, 0x80000001, origin+variant, -0x80000000, 0x7FFFFFFF, 4)]
            if (not roots or (roots["source_tick"], roots["sequence"], roots["applied_tick"]) !=
                    (101 if later else 100 if origin < 55 else 8,
                     (8 if origin == 11 else 7) if later else 5, 101 if later else 100) or
                    [(e["kind"], e["unit"], e["birth"], e["before_capture"], e["image"])
                     for e in roots["events"]] !=
                    [(kind, unit, 17, i & 1, image) for i, (kind, image) in enumerate(zip(
                        (tarec.RK_EV_START_NAMED, tarec.RK_EV_REPLY, tarec.RK_EV_SPRAY,
                         tarec.RK_EV_CREATE), images))]):
                raise RuntimeError("mixed root bytes/order/phase/provenance changed across proof tail")
        if 66 in state.named or 66 in state.roots:
            raise RuntimeError("unknown named roots fabricated")
        o = state.orders.get((7, 0))
        if not o or (o["count"], o["birth"], o["source_tick"], o["sequence"], o["applied_tick"]) != (0, 17, 8, 4, 100):
            raise RuntimeError("partial/proof order transaction replaced complete authority")
        if (8, 0) in state.orders:
            raise RuntimeError("unknown order list was fabricated empty")
        local = state.orders.get((1, 1))
        if not local or (local["origin"], local["birth"], local["count"], local["source_tick"],
                         local["sequence"], local["applied_tick"], local["image"]) != (
                11, 21, 1, 100, 3, 100, bytes((i*7+3) & 255 for i in range(88))):
            raise RuntimeError("recorded nonempty order bytes/provenance were not reconstructed")
        for slot, origin, row, birth, source_tick, attacker in ((3, 22, 1, 23, 100, 0), (7, 66, 3, 17, 8, 2)):
            u = state.units.get(slot)
            if (not u or (u["live"], u["origin"], u["birth"], u["source_tick"], u["sequence"], u["applied_tick"]) !=
                    (1, origin, birth, source_tick, 3, 100) or struct.unpack_from("<I", u["image"], 0x68)[0] != origin or
                    struct.unpack_from("<I", u["image"], 0x96)[0] != row+1 or
                    struct.unpack_from("<I", u["image"], 0xEC)[0] != row+1 or
                    u["image"][0xFF] != row or u["image"][0xF4] != attacker or u["stream"] != state.players[origin]["stream"]):
                raise RuntimeError("mapped unit authority/provenance changed across proof tail")
            empty = state.units.get(slot+1)
            if not empty or empty["live"] or empty["image"] != bytes(328) or empty["sequence"] != 3:
                raise RuntimeError("authoritative unit absence was not retained")
        if 5 in state.units:
            raise RuntimeError("unknown unit was fabricated absent")
    if (history.source_orders[7, 0]["count"] != 46 or
            struct.unpack_from("<I", history.source_players[66]["image"])[0] != 67 or
            struct.unpack_from("<I", history.source_units[7]["image"], 0x68)[0] != 67):
        raise RuntimeError("proof tail was not semantically validated")
    if (history.source_named[55]["sequence"] != 6 or
            struct.unpack_from("<I", history.source_named[55]["events"][0]["args"])[0] != 56 or
            history.named[55]["sequence"] != 4):
        raise RuntimeError("named proof tail replaced application authority")
    if (history.source_roots[55]["sequence"] != 7 or
            history.source_roots[55]["events"][1]["image"][12:] != b"NO" or
            history.roots[55]["sequence"] != 5):
        raise RuntimeError("mixed root proof tail replaced application authority")
    proof = History(segment["header"])
    for block_kind, _, _, payload in segment["blocks"]:
        if block_kind != tarec.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(payload):
            span, origin, _, _, kind = struct.unpack_from("<IIIII", payload, at+16+12)
            phase = struct.unpack_from("<I", payload, at+12)[0]
            if origin == 66 and kind in (6, 7, 8) and phase != 3:
                proof.block(struct.pack("<QII", 0, 0, 3) + payload[at+16:at+16+span])
            at += 16+span
    if (66 not in proof.source_maps or 66 not in proof.source_players or 7 not in proof.source_units or
            proof.maps or proof.players or proof.units or proof.orders or proof.at(1000).players or proof.at(1000).units):
        raise RuntimeError("proof-only roster fabricated application authority")
    named_proof = History(segment["header"])
    for block_kind, _, _, payload in segment["blocks"]:
        if block_kind != tarec.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(payload):
            span, origin, _, _, _ = struct.unpack_from("<IIIII", payload, at+16+12)
            phase = struct.unpack_from("<I", payload, at+12)[0]
            if origin == 55 and phase != 3:
                named_proof.block(struct.pack("<QII", 0, 0, 3) + payload[at+16:at+16+span])
            at += 16+span
    if (55 not in named_proof.source_named or 55 not in named_proof.source_roots or
            named_proof.named or named_proof.roots or named_proof.maps or named_proof.players or
            named_proof.orders or named_proof.at(1000).named or named_proof.at(1000).roots):
        raise RuntimeError("proof-only named roots fabricated application authority")
    if history.source_players[66]["image"][0x146] != 2 or history.at(101).players[66]["image"][0x146] != 3:
        raise RuntimeError("mapped application image replaced the raw source baseline")
    print("typed history: C and Python mapped player/unit/order/script/reply/spray provenance, mixed call order, absence versus unknown, partial-list retention and proof isolation PASS", flush=True)


def application_trace(segment, log):
    history = History(segment["header"])
    for kind, _, _, raw in segment["blocks"]:
        if kind == tarec.RB_CONTRIBUTIONS:
            history.block(raw)
    expected, roots = [], []
    for tick, raw in history.applications:
        ordinal, application_tick, phase = struct.unpack_from("<QII", raw)
        span, origin, source_tick, sequence, kind = struct.unpack_from("<IIIII", raw, 28)
        if application_tick != tick:
            raise RuntimeError("independent application clock changed")
        expected.append((ordinal, tick, phase, origin, source_tick, sequence, kind, span,
                         zlib.crc32(raw[16:])))
        if kind == 10:
            roots.append((origin, source_tick, sequence, tick))
    observed = [tuple(int(v, 16 if i == 8 else 10) for i, v in enumerate(line.split()[1:]))
                for line in log.read_text().splitlines() if line.startswith("APPLICATION ")]
    if observed != expected or len(expected) != 27 or roots != [
            (55, 8, 5, 100), (11, 100, 5, 100), (22, 100, 5, 100),
            (11, 101, 7, 101), (22, 101, 7, 101), (11, 101, 8, 101)]:
        raise RuntimeError("C ordered application cursor differs from independent Python ledger")
    print("application cursor: all 27 envelopes/6 mixed root batches match Python bytes and both clocks; A/B/A, query isolation, resumed ordinals, duplicate polls and seek PASS", flush=True)


def semantic_mutation(blocks, origin, kind, phase, offset, fmt, value, envelope=False):
    result, found = [], False
    for block_kind, first, last, raw in blocks:
        if block_kind == tarec.RB_CONTRIBUTIONS and not found:
            raw, at = bytearray(raw), 0
            while at < len(raw):
                record_phase = struct.unpack_from("<I", raw, at+12)[0]
                span, source, _, _, source_kind = struct.unpack_from("<IIIII", raw, at+16+12)
                if (source, source_kind, record_phase) == (origin, kind, phase):
                    values = value if isinstance(value, tuple) else (value,)
                    struct.pack_into(fmt, raw, at+16+(0 if envelope else 64)+offset, *values)
                    found = True
                    break
                at += 16+span
            raw = bytes(raw)
        result.append((block_kind, first, last, raw))
    if not found:
        raise RuntimeError("semantic control could not find its typed source record")
    return resealed(result)


def run(output, read_file=None, skirmish_file=None, native_file=None):
    root = tareplay.main_checkout()
    directory = None
    display = tareplay.start_xvfb()
    env = None
    try:
        with (output / "setup.log").open("w") as log:
            exe = output / "recmp-file-test.exe"
            sources = ("tagpu_recmp", "tagpu_recmp_auth", "tagpu_recmp_record", "tagpu_recmp_stream", "tagpu_recmp_players",
                       "tagpu_recmp_file", "tagpu_recmp_file_state", "tagpu_recmp_history", "tagpu_recmp_units", "tagpu_recmp_named", "tagpu_recmp_roots", "tagpu_recmp_orders", "tagpu_recwriter",
                       "tagpu_recreader", "tagpu_recfile", "lodepng", "crc32")
            command(["i686-w64-mingw32-gcc", "-I", TREE / "tagpu/ddraw/inc", "-O2", "-Wall",
                     "-Wextra", "-Werror", "-std=c99", "-o", exe, HERE / "recmp_file_test.c",
                     *(TREE / f"tagpu/ddraw/src/{s}.c" for s in sources), "-lole32", "-luuid"], log)
            directory = Path(tempfile.mkdtemp(prefix="recmp-file-", dir=root / "tagpu/instances"))
            env = dict(os.environ, WINEPREFIX=str(directory / "prefix"), DISPLAY=display.display,
                       WINEDEBUG="-all")
            command([sys.executable, root / "tools/tacli", "create", directory.name,
                     "--private-registry", "--display", display.display, "--res", "640x480"], log)
        if skirmish_file:
            with (output / "skirmish-read.log").open("w") as log:
                command(["wine", exe, "skirmish", "Z:" + str(skirmish_file).replace("/", "\\")], log, env=env)
            print("actual skirmish file: C reader/summary/full-header info PASS", flush=True)
        if native_file:
            log_path = output / "native-read.log"
            with log_path.open("w") as log:
                command(["wine", exe, "native", "Z:" + str(native_file).replace("/", "\\")], log, env=env)
            blocks, stopped = tarec.blocks(native_file)
            if stopped:
                raise RuntimeError(stopped)
            segment = tarec.segments(blocks)[0]
            history = History(segment["header"])
            for kind, _, _, raw in blocks:
                if kind == tarec.RB_CONTRIBUTIONS:
                    history.block(raw)
            crc, size, roots = 0, 0, 0
            for _, raw in history.applications:
                crc = zlib.crc32(raw, crc)
                size += len(raw)
                roots += struct.unpack_from("<I", raw, 44)[0] == 10
            rows = [line.split()[1:] for line in log_path.read_text().splitlines()
                    if line.startswith("NATIVE_APPLICATIONS ")]
            expected = [str(history.ledger.records), str(size), str(roots), f"{crc:08X}",
                        str(segment["first"]), str(segment["last"])]
            if rows != [expected] or set(history.players) != {a[0] for a in history.ledger.actors}:
                raise RuntimeError("native C/Python application bytes, clocks or applied players disagree")
            creates, create_crc = [], 0
            def create_record(world, kind, body):
                nonlocal create_crc
                if kind == tarec.RK_EV_NET_CREATE:
                    creates.append(bytes(body))
                    create_crc = zlib.crc32(struct.pack("<I", world.tick)+body, create_crc)
            tarec.stream_to(segment["header"], segment["blocks"], on_record=create_record)
            rows = [line.split()[1:] for line in log_path.read_text().splitlines()
                    if line.startswith("NATIVE_CREATES ")]
            if rows != [[str(len(creates)), f"{create_crc:08X}"]]:
                raise RuntimeError("native C/Python network creation arguments/ticks/token disagree")
            print(f"native creates: {len(creates)} exact C/Python roots retained through world seek PASS", flush=True)
            print("native file: C/Python complete application bytes/order/provenance, all owner players/unit-block bounds and world seek PASS", flush=True)
            return
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
        semantic_states(tarec.segments(blocks)[0])
        if not read_file:
            with (output / "network-create-controls.log").open("w") as log:
                network_create_controls(blocks, output, exe, log, env)
        if read_file:
            with (output / "normal-read.log").open("w") as log:
                command(["wine", exe, "read", "Z:" + str(original).replace("/", "\\")], log, env=env)
        application_trace(tarec.segments(blocks)[0], output / ("normal-read.log" if read_file else "normal.log"))
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
                    # A proof-tail seal cannot advance the frozen application cut.
                    offset = 16+40*2+4
                    struct.pack_into("<I", body, offset, struct.unpack_from("<I", body, offset)[0]+1)
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
        header_index = next(i for i, b in enumerate(blocks) if b[0] == tarec.RB_HEADER)
        header_entries = list(tarec.entries(blocks[header_index][3]))
        roster = next(b for tag, b in header_entries if tag == tarec.RT_MP_SEATS)
        mappings = {"short_mapping": roster[:-1]}
        for name, offset, value in (
                ("mapping_version", 0, 0), ("mapping_count", 4, 3),
                ("mapping_slots", 8, 31), ("mapping_each", 12, 3),
                ("mapping_origin", 16, 99), ("mapping_alias_origin", 32, 11),
                ("mapping_alias_seat", 36, 0), ("mapping_bad_seat", 20, 10),
                ("mapping_unused", 16+16*4, 99)):
            altered = bytearray(roster); struct.pack_into("<I", altered, offset, value)
            mappings[name] = bytes(altered)
        for name, first, last in (("mapping_alias_block", 1, 2), ("mapping_alignment", 4, 5),
                                  ("mapping_outside", 21, 22), ("mapping_span", 3, 3)):
            altered = bytearray(roster); struct.pack_into("<II", altered, 40, first, last)
            mappings[name] = bytes(altered)
        entries_by_name = {name: [(tag, altered if tag == tarec.RT_MP_SEATS else body)
                                  for tag, body in header_entries] for name, altered in mappings.items()}
        entries_by_name["missing_mapping"] = [(tag, body) for tag, body in header_entries if tag != tarec.RT_MP_SEATS]
        entries_by_name["duplicate_mapping"] = header_entries + [(tarec.RT_MP_SEATS, roster)]
        entries_by_name["missing_seat"] = [(tag, body) for tag, body in header_entries
                                           if tag != tarec.RT_PLAYER or body[0] != 3]
        first_seat = next(body for tag, body in header_entries if tag == tarec.RT_PLAYER)
        entries_by_name["duplicate_seat"] = header_entries + [(tarec.RT_PLAYER, first_seat)]
        for name, tag, value in (("world_slots", tarec.RT_SLOTS, 22),
                                 ("world_each", tarec.RT_UNITS_EACH, 1), ("nonnetwork_mapping", tarec.RT_MODE, 2)):
            entries_by_name[name] = [(t, struct.pack("<I", value) if t == tag else body)
                                     for t, body in header_entries]
        for name in ("seat_ownership", "short_seat"):
            entries_by_name[name] = []
            for tag, body in header_entries:
                if tag == tarec.RT_PLAYER and body[0] == 0:
                    body = body[:2] if name == "short_seat" else body[:1] + bytes([3]) + body[2:]
                entries_by_name[name].append((tag, body))
        for name, altered in entries_by_name.items():
            payload = bytearray()
            for tag, body in altered:
                payload += struct.pack("<HH", tag, len(body)) + body + bytes((-len(body)) % 4)
            changed = list(blocks)
            changed[header_index] = (*blocks[header_index][:3], bytes(payload))
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
            print(f"{name}: C reader/summary/info and Python mapping refusal PASS", flush=True)
        for name, origin, kind, phase, offset, fmt, value, envelope in (
                ("player_origin", 55, 6, 2, 0, "<I", 66, False),
                ("player_controller", 55, 6, 2, 4, "<I", 3, False),
                ("player_missing_baseline", 55, 6, 2, 8, "<I", 0, False),
                ("player_reserved", 55, 6, 2, 29, "<B", 1, False),
                ("player_full_mask", 55, 6, 2, 12, "<B", 254, False),
                ("player_mask_overflow", 55, 6, 2, 28, "<B", 7, False),
                ("player_block_ids", 55, 6, 2, 32+0x67, "<I", 8, False),
                ("player_self_row", 55, 6, 2, 32+0x146, "<B", 2, False),
                ("source_roster_version", 55, 7, 2, 0, "<I", 0, False),
                ("source_roster_alias", 55, 7, 2, 36, "<I", 7, False),
                ("source_roster_origin", 55, 7, 2, 16, "<I", 99, False),
                ("source_roster_block", 55, 7, 2, 24, "<I", 9, False),
                ("source_roster_changed_block", 55, 7, 2, 24, "<II", (9, 10), False),
                ("source_roster_dummy", 55, 7, 2, 20, "<I", 10, False),
                ("source_roster_unused", 55, 7, 2, 80, "<I", 99, False),
                ("missing_source_roster", 55, 7, 2, 28, "<I", 1, True),
                ("repeated_source_roster", 66, 6, 3, 28, "<I", 7, True),
                ("proof_player_origin", 66, 6, 3, 0, "<I", 55, False),
                ("proof_player_mask", 66, 6, 3, 28, "<B", 4, False),
                ("unit_batch_count", 66, 8, 2, 0, "<I", 0, False),
                ("unit_owner_block", 66, 8, 2, 4, "<H", 5, False),
                ("unit_live_flag", 66, 8, 2, 6, "<B", 2, False),
                ("unit_full_flag", 66, 8, 2, 7, "<B", 0, False),
                ("unit_birth", 66, 8, 2, 8, "<I", 0x80000000, False),
                ("unit_reserved", 66, 8, 2, 23, "<B", 1, False),
                ("unit_unused_bits", 66, 8, 2, 22, "<B", 7, False),
                ("unit_owner_id", 66, 8, 2, 24+0x96, "<I", 1, False),
                ("unit_store_id", 66, 8, 2, 24+0xEC, "<I", 12, False),
                ("unit_attacker_row", 66, 8, 2, 24+0xF4, "<B", 11, False),
                ("unit_owner_row", 66, 8, 2, 24+0xFF, "<B", 255, False),
                ("unit_missing_live_bit", 66, 8, 2, 24+0x110, "<I", 0, False),
                ("unit_duplicate_slot", 66, 8, 2, 24+328, "<H", 7, False),
                ("proof_unit_birth", 66, 8, 3, 8, "<I", 18, False),
                ("proof_unit_mask", 66, 8, 3, 22, "<B", 4, False),
                ("order_owner_block", 66, 2, 2, 0, "<H", 5, False),
                ("order_reserved", 66, 2, 2, 3, "<B", 1, False),
                ("named_count", 55, 9, 2, 0, "<I", 0, False),
                ("named_span", 55, 9, 2, 4, "<H", 24, False),
                ("named_phase", 55, 9, 2, 6, "<B", 2, False),
                ("named_reserved", 55, 9, 2, 7, "<B", 1, False),
                ("named_birth", 55, 9, 2, 8, "<I", 0x80000000, False),
                ("named_owner", 55, 9, 2, 12, "<I", 8, False),
                ("named_argc", 55, 9, 2, 16, "<B", 5, False),
                ("named_now", 55, 9, 2, 17, "<B", 2, False),
                ("named_arg_reserved", 55, 9, 2, 18, "<B", 1, False),
                ("named_name", 55, 9, 2, 36, "<B", 0, False),
                ("proof_named_argc", 55, 9, 3, 16, "<B", 5, False),
                ("roots_count", 55, 10, 2, 0, "<I", 0, False),
                ("roots_kind", 55, 10, 2, 4, "<H", 0, False),
                ("roots_span", 55, 10, 2, 6, "<H", 11, False),
                ("roots_phase", 55, 10, 2, 8, "<B", 2, False),
                ("roots_reserved", 55, 10, 2, 10, "<B", 1, False),
                ("roots_birth", 55, 10, 2, 12, "<I", 0x80000000, False),
                ("roots_owner", 55, 10, 2, 16, "<I", 8, False),
                ("roots_script_argc", 55, 10, 2, 20, "<B", 5, False),
                ("roots_script_name", 55, 10, 2, 40, "<B", 0, False),
                ("roots_reply_kind", 55, 10, 2, 60, "<I", 24, False),
                ("roots_reply_root", 55, 10, 2, 64, "<B", 2, False),
                ("roots_reply_absent_text", 55, 10, 2, 65, "<B", 0, False),
                ("roots_reply_reserved", 55, 10, 2, 67, "<B", 1, False),
                ("roots_reply_text", 55, 10, 2, 68, "<B", 0, False),
                ("roots_spray_reverse", 55, 10, 2, 86, "<I", 2, False),
                ("roots_spray_layer", 55, 10, 2, 126, "<h", 10, False),
                ("roots_spray_negative_layer", 55, 10, 2, 126, "<h", -1, False),
                ("roots_create_short", 55, 10, 2, 132, "<H", 27, False),
                ("roots_create_long", 55, 10, 2, 132, "<H", 29, False),
                ("roots_create_type", 55, 10, 2, 146, "<H", 0, False),
                ("roots_create_owner_bound", 55, 10, 2, 148, "<B", 10, False),
                ("roots_create_owner_row", 55, 10, 2, 148, "<B", 2, False),
                ("roots_create_layer", 55, 10, 2, 149, "<B", 4, False),
                ("roots_create_water", 55, 10, 2, 166, "<B", 5, False),
                ("roots_create_reserved0", 55, 10, 2, 167, "<B", 1, False),
                ("roots_create_reserved1", 55, 10, 2, 168, "<B", 1, False),
                ("roots_create_reserved2", 55, 10, 2, 169, "<B", 1, False),
                ("roots_create_birth", 55, 10, 2, 138, "<I", 0x80000000, False),
                ("roots_create_phase", 55, 10, 2, 134, "<B", 2, False),
                ("proof_roots_kind", 55, 10, 3, 4, "<H", 0, False),
                ("proof_roots_reply_kind", 55, 10, 3, 60, "<I", 24, False),
                ("proof_roots_spray_layer", 55, 10, 3, 126, "<h", 10, False),
                ("proof_roots_create_owner", 55, 10, 3, 148, "<B", 2, False),
                ("order_total", 66, 2, 2, 8, "<H", 4097, False),
                ("order_first_chunk", 66, 2, 2, 10, "<H", 1, False),
                ("order_chunk_count", 66, 2, 2, 12, "<H", 46, False),
                ("proof_order_offset", 66, 2, 3, 10, "<H", 44, False),
                ("proof_order_birth", 66, 2, 3, 4, "<I", 18, False),
                ("proof_order_tick", 66, 2, 3, 20, "<I", 10, True),
                ("seal_partial_transaction", 66, 2, 3, 28, "<I", 1, True)):
            changed = semantic_mutation(blocks, origin, kind, phase, offset, fmt, value, envelope)
            # Rebuilt source seals and cut validation must pass before semantic refusal.
            from recmp_file import Contributions
            ledger = Contributions(tarec.header(changed[0][3])["multiplayer"])
            for block_kind, _, _, raw in changed:
                if block_kind == tarec.RB_CONTRIBUTIONS:
                    ledger.block(raw)
                elif block_kind == tarec.RB_END:
                    ledger.footer(next(body for tag, body in tarec.entries(raw) if tag == tarec.RT_MP_END))
            path = output / f"{name}.imprec"
            path.write_bytes(encoded(changed))
            try:
                tarec.segments(changed)
            except tarec.Unreadable:
                pass
            else:
                raise RuntimeError(f"Python reader accepted semantic body {name}")
            with (output / f"{name}.log").open("w") as log:
                command(["wine", exe, "refuse", "Z:" + str(path).replace("/", "\\")], log, env=env)
            print(f"{name}: raw seals/cuts valid; C reader/summary/info and Python semantic refusal PASS", flush=True)
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
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--read-file", type=Path, help="verify new reader controls against an existing fixture file")
    source.add_argument("--native-file", type=Path, help="verify a game-produced multiplayer file and its ordered authority through seek")
    parser.add_argument("--skirmish-file", type=Path, help="also validate a real skirmish recording's header/info")
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(output, args.read_file.resolve() if args.read_file else None,
            args.skirmish_file.resolve() if args.skirmish_file else None,
            args.native_file.resolve() if args.native_file else None)
    except (OSError, ValueError, RuntimeError, tarec.Unreadable, subprocess.SubprocessError,
            tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
