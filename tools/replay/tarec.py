#!/usr/bin/env python3
"""tarec -- reads an Impure demo recording (research/notes/tadr-port/demo-recorder-skirmish-slice.md).

  tools/replay/tarec.py info  FILE           the header, the blocks and how the recording ended
  tools/replay/tarec.py check FILE           rebuild the world from the stream and require it to
                                             equal every snapshot the file carries
  tools/replay/tarec.py compare FILE DUMP... rebuild the world to each dump's GameTime and require
                                             it to equal the dump, byte for byte

  exit 0 when every check holds, 1 when one fails, 2 when a file cannot be read.

THE FORMAT is tagpu/ddraw/inc/tagpu_recfmt.h, which this file mirrors: a change there is a change
here, and REC_FORMAT says which. A recording is REC_MAGIC, then blocks; a block is a 32-byte header
and its payload, zlib-deflated unless the block is stored, each checked by a CRC-32. A file ends at
its last complete block: a block cut short or torn ends the reading, and the file is reported as
read to there. A dump (`dump <name>` in tagpu_replay.trigger) is REC_MAGIC and one stored snapshot.

Standard library only.
"""
import os
import struct
import sys
import zlib

REC_FORMAT = 1
REC_MAGIC = b"IMPREC\r\n"
REC_BLOCK_MAGIC = 0x4B4C4249
REC_UNIT_BYTES = 0x118
REC_UNIT_DWORDS = REC_UNIT_BYTES // 4
REC_PLAYER_DWORDS = (0x14B + 3) // 4
REC_PLAYER_IMG = REC_PLAYER_DWORDS * 4
REC_PLAYERS = 10
REC_UNIT_BITS = (REC_UNIT_DWORDS + 7) // 8
REC_PLAYER_BITS = (REC_PLAYER_DWORDS + 7) // 8

(RK_PAD, RK_TICK, RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE, RK_PLAYER, RK_PLAYER_DELTA, RK_TABLE_SIZE,
 RK_TABLE_SPAN, RK_CORRECT, RK_VERIFY_UNIT, RK_VERIFY_PLAYER, RK_VERIFY_TABLE, RK_HEADER,
 RK_END) = range(15)
RK_NAMES = {RK_UNIT: "unit", RK_PLAYER: "player", RK_TABLE_SPAN: "table"}

RB_HEADER, RB_STREAM, RB_SNAPSHOT, RB_END = 1, 2, 3, 4
RB_NAMES = {RB_HEADER: "header", RB_STREAM: "stream", RB_SNAPSHOT: "snapshot", RB_END: "end"}
RBF_STORED = 1

(RT_FORMAT, RT_BUILD, RT_MODE, RT_MAP, RT_START, RT_WALLCLOCK, RT_SLOTS, RT_UNITS_EACH,
 RT_UNITDEFS, RT_PLAYER, RT_LEVER, RT_LAYOUT, RT_TABLE) = range(1, 14)
(RT_END_REASON, RT_LAST, RT_ITERATIONS, RT_CORRECTIONS, RT_BAD_IDS, RT_RING_HIGH) = range(32, 38)

END_REASONS = {1: "complete: the level ended", 2: "INCOMPLETE: the ring was full",
               3: "INCOMPLETE: the file could not be written",
               4: "INCOMPLETE: no memory for a declared table's growth",
               5: "INCOMPLETE: the level changed without a teardown",
               6: "INCOMPLETE: the game stopped being a skirmish"}


class Unreadable(Exception):
    pass


# ---- blocks ----------------------------------------------------------------------------------

def blocks(path):
    """(kind, first, last, raw payload) for each complete block, and why the reading stopped
    (None at a clean end of the file)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != REC_MAGIC:
        raise Unreadable(f"{path}: not a recording (no {REC_MAGIC!r})")
    out, at = [], 8
    while at < len(data):
        head = data[at:at + 32]
        if len(head) < 32:
            return out, f"a block header cut short at byte {at}"
        magic, kind, flags, first, last, raw, packed, crc_body, crc_head = \
            struct.unpack("<IHHIIIIII", head)
        if magic != REC_BLOCK_MAGIC or zlib.crc32(head[:28]) != crc_head:
            return out, f"a torn block header at byte {at}"
        body = data[at + 32:at + 32 + packed]
        if len(body) < packed:
            return out, f"a block cut short at byte {at} ({len(body)} of {packed} bytes)"
        if zlib.crc32(body) != crc_body:
            return out, f"a block whose payload fails its CRC at byte {at}"
        payload = body if flags & RBF_STORED else zlib.decompress(body)
        if len(payload) != raw:
            return out, f"a block at byte {at} that inflates to {len(payload)} bytes, not {raw}"
        out.append((kind, first, last, payload))
        at += 32 + packed
    return out, None


def records(payload):
    at = 0
    while at + 4 <= len(payload):
        kind, n = struct.unpack_from("<HH", payload, at)
        body = payload[at + 4:at + 4 + n]
        if len(body) < n:
            raise Unreadable(f"a record cut short at payload byte {at}")
        yield kind, body
        at += (4 + n + 3) & ~3


def entries(payload):
    at = 0
    while at + 4 <= len(payload):
        tag, n = struct.unpack_from("<HH", payload, at)
        yield tag, payload[at + 4:at + 4 + n]
        at += (4 + n + 3) & ~3


# ---- the header and the end ------------------------------------------------------------------

def header(payload):
    h = {"players": [], "levers": [], "tables": {}}
    for tag, b in entries(payload):
        u32 = struct.unpack_from("<I", b)[0] if len(b) >= 4 else None
        if tag == RT_FORMAT: h["format"] = u32
        elif tag == RT_BUILD: h["build"] = b.decode("latin-1")
        elif tag == RT_MODE: h["mode"] = u32
        elif tag == RT_MAP: h["map"] = b.decode("latin-1")
        elif tag == RT_START: h["start"] = u32
        elif tag == RT_WALLCLOCK: h["wallclock"] = struct.unpack_from("<Q", b)[0]
        elif tag == RT_SLOTS: h["slots"] = u32
        elif tag == RT_UNITS_EACH: h["units_each"] = u32
        elif tag == RT_UNITDEFS: h["unitdefs"] = struct.unpack_from("<II", b)
        elif tag == RT_PLAYER:
            h["players"].append({"index": b[0], "controller": b[1], "side": b[2], "logo": b[3],
                                 "name": b[4:].decode("latin-1")})
        elif tag == RT_LEVER:
            nf = struct.unpack_from("<H", b, 2)[0]
            h["levers"].append({"set": b[0], "read": b[1], "file": b[4:4 + nf].decode("latin-1"),
                                "text": b[4 + nf:].decode("latin-1")})
        elif tag == RT_LAYOUT: h["layout"] = b.decode("latin-1")
        elif tag == RT_TABLE:
            ix, cls, key, stride, flags = struct.unpack_from("<HBBIH", b)
            h["tables"][ix] = {"class": cls, "key": key, "stride": stride, "flags": flags,
                               "name": b[12:].decode("latin-1")}
    if h.get("format") != REC_FORMAT:
        raise Unreadable(f"format {h.get('format')}, this tool reads {REC_FORMAT}")
    return h


def end(payload):
    e = {"corrections": []}
    for tag, b in entries(payload):
        if tag == RT_CORRECTIONS:
            kind, table, off, count = struct.unpack_from("<BxHII", b)
            e["corrections"].append((kind, table, off, count))
        elif len(b) >= 4:
            e[tag] = struct.unpack_from("<I", b)[0]
    return e


# ---- the world -------------------------------------------------------------------------------

class World:
    def __init__(self, slots, tables):
        self.slots = slots
        self.units = {}                                  # slot -> bytearray image, live only
        self.players = [bytearray(REC_PLAYER_IMG) for _ in range(REC_PLAYERS)]
        self.tables = {ix: bytearray() for ix in tables}
        self.tick = None
        self.corrections = 0

    def apply(self, kind, b):
        if kind == RK_TICK:
            self.tick = struct.unpack_from("<I", b)[0]
        elif kind == RK_UNIT:
            self.units[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + REC_UNIT_BYTES])
        elif kind == RK_UNIT_DELTA:
            slot = struct.unpack_from("<H", b)[0]
            self._delta(self.units[slot], b[2:2 + REC_UNIT_BITS], REC_UNIT_DWORDS, b[12:])
        elif kind == RK_UNIT_GONE:
            self.units.pop(struct.unpack_from("<H", b)[0], None)
        elif kind == RK_PLAYER:
            self.players[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + REC_PLAYER_IMG])
        elif kind == RK_PLAYER_DELTA:
            i = struct.unpack_from("<H", b)[0]
            self._delta(self.players[i], b[2:2 + REC_PLAYER_BITS], REC_PLAYER_DWORDS, b[16:])
        elif kind == RK_TABLE_SIZE:
            ix, size = struct.unpack_from("<HxxI", b)
            t = self.tables.setdefault(ix, bytearray())
            if size < len(t):
                del t[size:]
            else:
                t.extend(bytes(size - len(t)))
        elif kind == RK_TABLE_SPAN:
            ix, off = struct.unpack_from("<HxxI", b)
            t, n = self.tables[ix], len(b) - 8
            if off + n > len(t):
                raise Unreadable(f"a span past table {ix}'s {len(t)} bytes")
            t[off:off + n] = b[8:]
        elif kind == RK_CORRECT:
            self.corrections += 1
        else:
            raise Unreadable(f"record kind {kind} in a block")

    @staticmethod
    def _delta(img, bits, dwords, vals):
        k = 0
        for i in range(dwords):
            if bits[i >> 3] >> (i & 7) & 1:
                img[4 * i:4 * i + 4] = vals[4 * k:4 * k + 4]
                k += 1

    def differences(self, other, names, limit=20):
        """Lines naming each difference, the first `limit` of them, and their count."""
        out, n = [], 0

        def note(line):
            nonlocal n
            n += 1
            if len(out) < limit:
                out.append(line)

        for slot in sorted(set(self.units) | set(other.units)):
            a, b = self.units.get(slot), other.units.get(slot)
            if a is None or b is None:
                note(f"unit {slot}: live in {'the dump' if a is None else 'the recording'} only")
            elif a != b:
                for i in range(0, REC_UNIT_BYTES, 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        note(f"unit {slot} +0x{i:03X}: recording {a[i:i + 4].hex()}, "
                             f"dump {b[i:i + 4].hex()}")
        for p in range(REC_PLAYERS):
            a, b = self.players[p], other.players[p]
            for i in range(0, REC_PLAYER_IMG, 4):
                if a[i:i + 4] != b[i:i + 4]:
                    note(f"player {p} +0x{i:03X}: recording {a[i:i + 4].hex()}, "
                         f"dump {b[i:i + 4].hex()}")
        for ix in sorted(set(self.tables) | set(other.tables)):
            a, b = self.tables.get(ix, bytearray()), other.tables.get(ix, bytearray())
            name = names.get(ix, {}).get("name", f"table {ix}")
            if len(a) != len(b):
                note(f"{name}: {len(a)} bytes in the recording, {len(b)} in the dump")
                continue
            if a != b:
                for i in range(0, len(a), 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        note(f"{name} +0x{i:X}: recording {a[i:i + 4].hex()}, "
                             f"dump {b[i:i + 4].hex()}")
        return out, n


def snapshot_world(payload, slots, tables):
    w = World(slots, tables)
    for kind, b in records(payload):
        w.apply(kind, b)
    return w


def load(path):
    bl, stop = blocks(path)
    if not bl or bl[0][0] != RB_HEADER:
        raise Unreadable(f"{path}: no header block" + (f" ({stop})" if stop else ""))
    return header(bl[0][3]), bl, stop


def stream_to(h, bl, want=None, on_snapshot=None):
    """The world the stream builds, to the end of iteration `want` (or of the file). Calls
    on_snapshot(world, snapshot payload) at each snapshot block on the way."""
    w = World(h["slots"], h["tables"])
    for kind, first, last, payload in bl[1:]:
        if kind == RB_SNAPSHOT and on_snapshot:
            on_snapshot(w, payload)
        if kind != RB_STREAM:
            continue
        for rk, b in records(payload):
            if rk == RK_TICK and want is not None and w.tick is not None and w.tick >= want:
                return w
            w.apply(rk, b)
    return w


# ---- the commands ----------------------------------------------------------------------------

def cmd_info(path):
    h, bl, stop = load(path)
    print(f"{path}")
    print(f"  map {h.get('map')!r}, mode {h.get('mode')}, build {h.get('build')}, "
          f"from GameTime {h.get('start')}")
    print(f"  {h.get('slots')} unit slots, {h.get('units_each')} units each, "
          f"unit defs {h.get('unitdefs')}, layout {h.get('layout', '')[:12]}")
    for p in h["players"]:
        print(f"  player {p['index']}: {p['name']!r}, controller {p['controller']}, "
              f"side {p['side']}, logo {p['logo']}")
    for lv in h["levers"]:
        print(f"  lever {lv['file']}: {'set' if lv['set'] else 'absent'}"
              + (f", {lv['text'][:40]!r}" if lv["read"] else ""))
    for ix, t in sorted(h["tables"].items()):
        print(f"  table {ix}: {t['name']}, {t['stride']} bytes a row")
    counts, raw = {}, {}
    for kind, first, last, payload in bl:
        counts[kind] = counts.get(kind, 0) + 1
        raw[kind] = raw.get(kind, 0) + len(payload)
    size = os.path.getsize(path)
    print(f"  {size} bytes: " + ", ".join(f"{counts[k]} {RB_NAMES.get(k, k)} ({raw[k] >> 10} KB raw)"
                                          for k in sorted(counts)))
    ends = [b for b in bl if b[0] == RB_END]
    if ends:
        e = end(ends[-1][3])
        print(f"  ended: {END_REASONS.get(e.get(RT_END_REASON), e.get(RT_END_REASON))}, "
              f"last GameTime {e.get(RT_LAST)}, {e.get(RT_ITERATIONS)} iterations, "
              f"{e.get(RT_BAD_IDS)} id rows outside their tables, "
              f"the ring held {e.get(RT_RING_HIGH, 0) >> 10} KB at most")
        for kind, table, off, count in e["corrections"]:
            if kind == RK_TABLE_SPAN:
                t = h["tables"].get(table, {"name": f"#{table}", "stride": 0})
                where = (f"table {t['name']} row {off // t['stride']} +0x{off % t['stride']:X}"
                         if t["stride"] else f"table {t['name']} +0x{off:X}")
            elif kind == 0:
                where = "more offsets than the end block counts one by one"
            else:
                where = f"{RK_NAMES.get(kind, kind)} +0x{off:X}" + \
                        (" (its liveness)" if kind == RK_UNIT and off == REC_UNIT_BYTES else "")
            print(f"  CORRECTED {count}x: {where}")
    else:
        print("  INCOMPLETE: no end block" + (f" ({stop})" if stop else ""))
    if stop:
        print(f"  read to its last complete block: {stop}")
    return 0


def cmd_check(path):
    h, bl, stop = load(path)
    bad, seen = 0, 0

    def at_snapshot(w, payload):
        nonlocal bad, seen
        seen += 1
        s = snapshot_world(payload, h["slots"], h["tables"])
        lines, n = w.differences(s, h["tables"])
        if s.tick != w.tick:
            lines.insert(0, f"the snapshot is at GameTime {s.tick}, the stream at {w.tick}")
            n += 1
        if n:
            bad += 1
            print(f"snapshot at GameTime {s.tick}: {n} differences from the stream")
            for line in lines:
                print("  " + line.replace("dump", "snapshot"))

    w = stream_to(h, bl, on_snapshot=at_snapshot)
    print(f"{path}: the stream reads to GameTime {w.tick} ({w.corrections} corrections in it); "
          f"{seen} snapshots, {bad} differing" + (f"; stopped at {stop}" if stop else ""))
    return 1 if bad or w.corrections else 0


def cmd_compare(path, dumps):
    h, bl, stop = load(path)
    status = 0
    for d in dumps:
        dbl, dstop = blocks(d)
        if len(dbl) != 1 or dbl[0][0] != RB_SNAPSHOT:
            raise Unreadable(f"{d}: not a dump (one snapshot block)")
        dw = snapshot_world(dbl[0][3], h["slots"], h["tables"])
        w = stream_to(h, bl, want=dw.tick)
        if w.tick != dw.tick:
            print(f"{d}: GameTime {dw.tick}, but the recording reads to {w.tick}")
            status = 1
            continue
        lines, n = w.differences(dw, h["tables"])
        print(f"{d}: GameTime {dw.tick}, {len(dw.units)} live units: "
              + ("EQUAL" if not n else f"{n} DIFFERENCES"))
        for line in lines:
            print("  " + line)
        if n:
            status = 1
    return status


def main(argv):
    if len(argv) < 3 or argv[1] not in ("info", "check", "compare") or \
            (argv[1] == "compare" and len(argv) < 4):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        if argv[1] == "info":
            return cmd_info(argv[2])
        if argv[1] == "check":
            return cmd_check(argv[2])
        return cmd_compare(argv[2], argv[3:])
    except (Unreadable, OSError, zlib.error, struct.error, KeyError) as e:
        print(f"tarec: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
