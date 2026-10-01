#!/usr/bin/env python3
"""tarec -- reads an Impure demo recording (research/notes/tadr-port/demo-recorder-skirmish-slice.md).

  tools/replay/tarec.py info  FILE           the header, the blocks, the events and how the
                                             recording ended
  tools/replay/tarec.py check FILE           rebuild the world from the stream and require it to
                                             equal every snapshot the file carries, and every
                                             change of the world between two iterations to have
                                             the events that explain it
  tools/replay/tarec.py compare FILE DUMP... rebuild the world to each dump's GameTime and require
                                             it to equal the dump, byte for byte
  tools/replay/tarec.py compare --puppet FILE DUMP...
                                             the same for dumps a playing of FILE took, less what
                                             a puppet host keeps as its own (puppet_view), with
                                             the order nodes' puppet rows as it holds them
                                             (puppet_hold), the globals' `hold` rows and the
                                             viewer's bit of the explored map masked;
                                             the radar picture is compared too where the dump's
                                             viewed player is the recording's
  tools/replay/tarec.py compare --images DUMP OTHERDUMP
                                             a report, not a check: the bytes of the main block
                                             and .data that differ between two dumps and that no
                                             row of replay-globals.tsv covers, in runs, grouped by
                                             what the notes map there (exit 0 whatever it finds)
  tools/replay/tarec.py compare-dumps FILE DUMP OTHERDUMP
                                             two dumps of playings of FILE at one GameTime with
                                             the same player viewed (a seek in a fresh process
                                             and one in a running playback) equal in every
                                             record, the players' sight counters included

  exit 0 when every check holds, 1 when one fails, 2 when a file cannot be read.

THE FORMAT is tagpu/ddraw/inc/tagpu_recfmt.h, which this file mirrors: a change there is a change
here, and REC_FORMAT says which. A recording is REC_MAGIC, then blocks; a block is a 32-byte header
and its payload, zlib-deflated unless the block is stored, each checked by a CRC-32. A file ends at
its last complete block: a block cut short or torn ends the reading, and the file is reported as
read to there. A dump (`dump <name>` in tagpu_replay.trigger) is REC_MAGIC, a stored header with the
format, the build, the globals list's hash and the tables, and one stored snapshot, which carries
every cell and, after the world, the players' sight counters (RK_SIGHT) and the main block and
.data whole (RK_IMAGE). A recording's snapshot
carries only the cells that differ from the first iteration's grid, the base, which a reader takes
from the stream, and none of a table's spans that are all zero. A table is paired with the
recording's by its name: each build numbers its tables in its own registry order, and a recording
may be played by another build than the one that made it. The header gives each table's layout
hash as well (`info` shows it), which the replay's seek compares with its own declaration's. The
globals image is replay-globals.tsv's fields, read through tools/replay-globals-gen.py where a
command needs their names or classes. The explored map is carried whole at the first iteration,
in every snapshot and in every dump, and as runs of changed bytes between.

Standard library only.
"""
import os
import struct
import sys
import zlib

REC_FORMAT = 4
REC_MAGIC = b"IMPREC\r\n"
REC_BLOCK_MAGIC = 0x4B4C4249
REC_UNIT_IMAGE = 0x148                    # a unit's record, then its mover's (tagpu_recfmt.h)
REC_UNIT_DWORDS = REC_UNIT_IMAGE // 4
REC_PLAYER_IMAGE = 0x208                  # a player's record, then its PlayerInfo's
REC_PLAYER_DWORDS = REC_PLAYER_IMAGE // 4
REC_ORDER_IMAGE = 0x58
REC_CELL_BYTES = 0x0D                     # a feature-grid cell's image
REC_CELLS_MAX = 4096
REC_WRECK_BYTES = 0x30                    # a wreck-pool record's image
REC_DEATHS_MAX = 32
REC_ORDERS_LENGTH = 0xFFFFFFFE
REC_PLAYERS = 10
REC_UNIT_BITS = (REC_UNIT_DWORDS + 7) // 8
REC_PLAYER_BITS = (REC_PLAYER_DWORDS + 7) // 8
REC_UNIT_DELTA_HEAD = (2 + REC_UNIT_BITS + 3) & ~3
REC_PLAYER_DELTA_HEAD = (2 + REC_PLAYER_BITS + 3) & ~3
U_TYPE, U_BUILT, U_STATEBYTE = 0x92, 0x104, 0x10E   # the image's type ID, build fraction, state
U_STATE = 0x110
REC_PROJ_IMAGE = 0x6C                     # a projectile record's 0x6B bytes, to a dword
REC_PROJ_DWORDS = REC_PROJ_IMAGE // 4
REC_PROJ_BITS = (REC_PROJ_DWORDS + 7) // 8
REC_PROJ_DELTA_HEAD = (2 + REC_PROJ_BITS + 3) & ~3
REC_MAIN_BYTES = 0x3924D                  # the main block, from main
REC_DATA_BEGIN, REC_DATA_BYTES = 0x501000, 0x2A65C   # .data
REC_IMAGE_CHUNK = 0x8000
REC_MAPPED_CHUNK = 0x8000
REC_MAPPED_RUNS = 0x7000
PL_MAPBIT = 0x146                         # a player record's bit in the explored map (0x48194B)


def mapped_bytes(w, h):
    """The explored map's bytes for a grid of w x h cells, in LoadMap's 32-bit arithmetic."""
    return ((w * h * 2) & 0xFFFFFFFF) >> 2


(RK_PAD, RK_TICK, RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE, RK_PLAYER, RK_PLAYER_DELTA, RK_TABLE_SIZE,
 RK_TABLE_SPAN, RK_CORRECT, RK_VERIFY_UNIT, RK_VERIFY_PLAYER, RK_VERIFY_TABLE, RK_HEADER,
 RK_END, RK_ORDERS, RK_VERIFY_ORDERS, RK_CONTROLLER, RK_VIEWER,
 RK_EV_CREATE, RK_EV_DEATH, RK_EV_SHOT, RK_EV_INTERCEPT, RK_EV_START, RK_EV_START_NAMED,
 RK_EV_BUILT, RK_EV_STATE, RK_EV_REPLY, RK_EV_SPRAY, RK_EV_FEATURE, RK_EV_FEATURE_END, RK_EV_DRAW,
 RK_EV_DEATH_END, RK_CELLS, RK_WRECK, RK_FEATURES, RK_GRID,
 RK_GLOBALS, RK_GLOBALS_DELTA, RK_PROJ_HEAD, RK_PROJ, RK_PROJ_DELTA, RK_VERIFY_GLOBALS,
 RK_VERIFY_PROJ, RK_IMAGE, RK_MAPPED, RK_MAPPED_RUNS, RK_SIGHT) = range(48)
RK_NAMES = {RK_UNIT: "unit", RK_PLAYER: "player", RK_TABLE_SPAN: "table", RK_ORDERS: "order list",
            RK_GLOBALS: "globals image", RK_PROJ: "projectile record",
            RK_PROJ_HEAD: "projectile count (+0x0) and extent (+0x4)"}
EVENTS = {RK_EV_CREATE: "create", RK_EV_DEATH: "death", RK_EV_SHOT: "shot",
          RK_EV_INTERCEPT: "interception", RK_EV_START: "broadcast start",
          RK_EV_START_NAMED: "owner's start", RK_EV_BUILT: "build done", RK_EV_STATE: "state byte",
          RK_EV_REPLY: "reply", RK_EV_SPRAY: "nano spray", RK_EV_FEATURE: "feature call",
          RK_EV_FEATURE_END: "feature call's end", RK_EV_DRAW: "feature draw",
          RK_EV_DEATH_END: "death's end"}
FR_TICK, FR_SPAWN, FR_DESTROY, FR_DIE, FR_BURN, FR_DAMAGE, FR_SWAP = range(7)
ROOT_ARGS = (0, 28, 8, 12, 12, 16, 12)     # an RK_EV_FEATURE's arguments after its site
FS_SITES = 6

RB_HEADER, RB_STREAM, RB_SNAPSHOT, RB_END = 1, 2, 3, 4
RB_NAMES = {RB_HEADER: "header", RB_STREAM: "stream", RB_SNAPSHOT: "snapshot", RB_END: "end"}
RBF_STORED = 1

(RT_FORMAT, RT_BUILD, RT_MODE, RT_MAP, RT_START, RT_WALLCLOCK, RT_SLOTS, RT_UNITS_EACH,
 RT_UNITDEFS, RT_PLAYER, RT_LEVER, RT_LAYOUT, RT_TABLE, RT_OPTIONS, RT_SETTINGS_MAP,
 RT_WEAPONS, RT_FEATURES, RT_GLOBALS, RT_PROJECTILES) = range(1, 20)
(RT_END_REASON, RT_LAST, RT_ITERATIONS, RT_CORRECTIONS, RT_BAD_IDS, RT_RING_HIGH, RT_OFF_THREAD,
 RT_UNCREDITED, RT_UNOBSERVED) = range(32, 41)

END_REASONS = {1: "complete: the level ended", 2: "INCOMPLETE: the ring was full",
               3: "INCOMPLETE: the file could not be written",
               4: "INCOMPLETE: no memory for a declared table's growth",
               5: "INCOMPLETE: the level changed without a teardown",
               6: "INCOMPLETE: the game stopped being a skirmish",
               7: "INCOMPLETE: an order list could not be recorded",
               8: "INCOMPLETE: the features were not the ones it began with, or their calls "
                  "nested deeper than the capture tracks",
               9: "INCOMPLETE: the projectile pool was not the one it began with, or its count "
                  "was past its records",
               10: "INCOMPLETE: the explored map was not the one it began with"}


class Unreadable(Exception):
    pass


# ---- blocks ----------------------------------------------------------------------------------

def blocks(path, sizes=None):
    """(kind, first, last, raw payload) for each complete block, and why the reading stopped
    (None at a clean end of the file); with `sizes`, (kind, first, last, raw, packed) of each
    block appended to it."""
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
        if sizes is not None:
            sizes.append((kind, first, last, raw, packed))
        at += 32 + packed
    return out, None


def header_only(path):
    """A recording's header, read from its first block alone: the rest of the file is neither
    read nor inflated."""
    with open(path, "rb") as f:
        if f.read(8) != REC_MAGIC:
            raise Unreadable(f"{path}: not a recording (no {REC_MAGIC!r})")
        head = f.read(32)
        if len(head) < 32:
            raise Unreadable(f"{path}: its header block is cut short")
        magic, kind, flags, first, last, raw, packed, crc_body, crc_head = \
            struct.unpack("<IHHIIIIII", head)
        if magic != REC_BLOCK_MAGIC or zlib.crc32(head[:28]) != crc_head or kind != RB_HEADER:
            raise Unreadable(f"{path}: no header block")
        body = f.read(packed)
    if len(body) < packed or zlib.crc32(body) != crc_body:
        raise Unreadable(f"{path}: its header block is cut short or fails its CRC")
    payload = body if flags & RBF_STORED else zlib.decompress(body)
    if len(payload) != raw:
        raise Unreadable(f"{path}: its header block inflates to {len(payload)} bytes, not {raw}")
    return header(payload)


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
                                 "alliances": bytes(b[4:15]), "info": bytes(b[16:16 + 0xB9]),
                                 "name": b[16 + 0xB9:].decode("latin-1")})
        elif tag == RT_LEVER:
            nf = struct.unpack_from("<H", b, 2)[0]
            h["levers"].append({"set": b[0], "read": b[1], "file": b[4:4 + nf].decode("latin-1"),
                                "text": b[4 + nf:].decode("latin-1")})
        elif tag == RT_LAYOUT: h["layout"] = b.decode("latin-1")
        elif tag == RT_TABLE:
            ix, cls, key, stride, flags, _, lhash = struct.unpack_from("<HBBIHHI", b)
            h["tables"][ix] = {"class": cls, "key": key, "stride": stride, "flags": flags,
                               "layout": lhash, "name": b[16:].decode("latin-1")}
        elif tag == RT_OPTIONS:
            los, cmdr, diff, fixed, np, ctl, view = struct.unpack_from("<IIIIHBB", b)
            h["options"] = {"lostype": los, "cmdrdeath": cmdr, "difficulty": diff,
                            "fixed_starts": fixed, "players": np, "controlled": ctl,
                            "viewed": view}
        elif tag == RT_SETTINGS_MAP: h["settings_map"] = b.decode("latin-1")
        elif tag == RT_WEAPONS: h["weapons"] = struct.unpack_from("<II", b)
        elif tag == RT_FEATURES: h["features"] = struct.unpack_from("<IIIII", b)
        elif tag == RT_GLOBALS: h["globals"] = b.decode("latin-1")
        elif tag == RT_PROJECTILES: h["projectiles"] = u32
    if h.get("format") != REC_FORMAT:
        raise Unreadable(f"format {h.get('format')}, this tool reads {REC_FORMAT}")
    return h


def feature_dims(h):
    """The grid's columns and rows, the wreck pool's records and the projectile pool's records a
    recording's header states."""
    if "features" not in h:
        raise Unreadable("a header without its features (RT_FEATURES)")
    if "projectiles" not in h:
        raise Unreadable("a header without its projectile pool (RT_PROJECTILES)")
    w, rows, wrecks = h["features"][:3]
    return w, rows, wrecks, h["projectiles"]


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
    def __init__(self, slots, tables, header=None, dims=None):
        self.slots = slots
        if dims is None:
            dims = feature_dims(header)
        self.gw, self.gh, self.nrec, self.ncap = dims
        self.grid = bytearray(self.gw * self.gh * REC_CELL_BYTES)     # every cell's image
        self.pool = bytearray(self.nrec * REC_WRECK_BYTES)             # every record's image
        self.heads = None               # the pool's three heads and the cursor, once sent
        self.base = None                # the grid at the end of the first iteration (stream_to)
        self.globals = None             # the globals image, once sent
        self.proj = bytearray(self.ncap * REC_PROJ_IMAGE)              # the projectile pool's
        self.phead = None               # (count, hw), once sent: records [hw, ncap) are zero
        self.mapped = bytearray(mapped_bytes(self.gw, self.gh))    # the explored map
        self.mnext = 0                  # RK_MAPPED's next offset: len(mapped) once it is whole
        self.images = {}                # a dump's main block (0) and .data (1)
        self.sight = {}                 # a dump's sight counters, by player record
        self.fbracket = 0               # an RK_EV_FEATURE's root + 1, until its end
        self.deaths = []                # the IDs of the deaths whose end is still to come
        self.units = {}                                  # slot -> bytearray image, live only
        self.lists = {}                                  # (slot, list) -> bytearray, not empty
        self.players = [bytearray(REC_PLAYER_IMAGE) for _ in range(REC_PLAYERS)]
        self.ctl = [0] * REC_PLAYERS
        self.view = [0, 0]
        if header:
            for p in header["players"]:
                self.ctl[p["index"]] = p["controller"]
            o = header.get("options")
            if o:
                self.view = [o["controlled"], o["viewed"]]
        self.tables = {ix: bytearray() for ix in tables}
        self.tick = None
        self.corrections = 0
        self.pend = None        # (slot, list, total, next) of an order list still owed records

    def bracket(self, kind, b):
        """The brackets' grammar, as the DLL's reader has it (tagpu_recreader.c, event_take): a
        feature call's holds no other and cells only when it is the tick's, which is inside none;
        a death's holds its explosion's feature calls and may hold deaths; a draw is inside one."""
        if kind == RK_EV_FEATURE:
            if self.fbracket or (b[0] == FR_TICK and self.deaths) or b[0] >= len(ROOT_ARGS) or \
                    len(b) != 8 + ROOT_ARGS[b[0]]:
                raise Unreadable("a malformed feature call")
            self.fbracket = b[0] + 1
        elif kind == RK_EV_FEATURE_END:
            if len(b) != 8 or self.fbracket != b[0] + 1:
                raise Unreadable("a feature call's end with no call open")
            self.fbracket = 0
        elif kind == RK_EV_DEATH:
            if self.fbracket or len(self.deaths) == REC_DEATHS_MAX:
                raise Unreadable("a death inside a feature call, or nested too deep")
            self.deaths.append(struct.unpack_from("<I", b)[0])
        elif kind == RK_EV_DEATH_END:
            if len(b) != 4 or not self.deaths or self.deaths[-1] != struct.unpack_from("<I", b)[0]:
                raise Unreadable("a death's end that closes no death")
            self.deaths.pop()
        elif kind == RK_EV_DRAW:
            if (not self.fbracket and not self.deaths) or len(b) != 12 or b[0] >= FS_SITES:
                raise Unreadable("a draw outside a feature call or a death")

    def cells(self, b):
        x, z, n, zero = struct.unpack_from("<HHHH", b)
        if not n or n > REC_CELLS_MAX or zero or len(b) != 8 + n * REC_CELL_BYTES or \
                x + n > self.gw or z >= self.gh:
            raise Unreadable(f"a run of cells past the {self.gw} x {self.gh} grid")
        return (z * self.gw + x) * REC_CELL_BYTES

    def apply(self, kind, b):
        """One record; an event changes nothing here and is returned as (kind, body) -- the
        cells inside the tick's feature call too, which are its input. An order list's records
        come together and in order, as the DLL's reader has them (tagpu_recreader.c,
        list_record)."""
        if self.pend is not None and kind != RK_ORDERS:
            raise Unreadable(f"slot {self.pend[0]}'s order list cut short by another record")
        if kind in (RK_EV_FEATURE, RK_EV_FEATURE_END, RK_EV_DEATH, RK_EV_DEATH_END, RK_EV_DRAW):
            self.bracket(kind, b)
            return kind, b
        if kind == RK_CELLS and (self.fbracket or self.deaths):
            if self.fbracket != FR_TICK + 1:
                raise Unreadable("cells inside a feature call that is not the tick's")
            self.cells(b)
            return kind, b
        if (self.fbracket or self.deaths) and kind not in EVENTS:
            raise Unreadable(f"record kind {kind} inside a feature call or a death")
        if kind == RK_TICK:
            self.tick = struct.unpack_from("<I", b)[0]
        elif kind == RK_UNIT:
            self.units[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + REC_UNIT_IMAGE])
        elif kind == RK_UNIT_DELTA:
            slot = struct.unpack_from("<H", b)[0]
            self._delta(self.units[slot], b[2:2 + REC_UNIT_BITS], REC_UNIT_DWORDS,
                        b[REC_UNIT_DELTA_HEAD:])
        elif kind == RK_UNIT_GONE:
            slot = struct.unpack_from("<H", b)[0]
            self.units.pop(slot, None)
            self.lists.pop((slot, 0), None)
            self.lists.pop((slot, 1), None)
        elif kind == RK_PLAYER:
            self.players[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + REC_PLAYER_IMAGE])
        elif kind == RK_PLAYER_DELTA:
            i = struct.unpack_from("<H", b)[0]
            self._delta(self.players[i], b[2:2 + REC_PLAYER_BITS], REC_PLAYER_DWORDS,
                        b[REC_PLAYER_DELTA_HEAD:])
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
        elif kind == RK_ORDERS:
            slot, lst, total, first = struct.unpack_from("<HBxHH", b)
            key = (slot, lst)
            k = (len(b) - 8) // REC_ORDER_IMAGE
            if first == 0 and self.pend is not None:
                raise Unreadable(f"slot {slot}'s order list begun inside slot {self.pend[0]}'s")
            if first != 0 and self.pend != (slot, lst, total, first):
                raise Unreadable(f"an order list record for slot {slot} out of order")
            self.pend = (slot, lst, total, first + k) if first + k < total else None
            if first == 0:
                if total:
                    self.lists[key] = bytearray(total * REC_ORDER_IMAGE)
                else:
                    self.lists.pop(key, None)
            imgs = b[8:]
            if total:
                l = self.lists.get(key)
                if l is None or len(l) != total * REC_ORDER_IMAGE or \
                        first * REC_ORDER_IMAGE + len(imgs) > len(l):
                    raise Unreadable(f"an order list record for slot {slot} out of its list")
                l[first * REC_ORDER_IMAGE:first * REC_ORDER_IMAGE + len(imgs)] = imgs
        elif kind == RK_CONTROLLER:
            self.ctl[b[0]] = b[1]
        elif kind == RK_VIEWER:
            self.view = [b[0], b[1]]
        elif kind == RK_CORRECT:
            self.corrections += 1
        elif kind == RK_CELLS:
            at = self.cells(b)
            self.grid[at:at + len(b) - 8] = b[8:]
        elif kind == RK_WRECK:
            i = struct.unpack_from("<H", b)[0]
            if len(b) != 4 + REC_WRECK_BYTES or i >= self.nrec:
                raise Unreadable(f"wreck record {i} past the pool's {self.nrec}")
            self.pool[i * REC_WRECK_BYTES:(i + 1) * REC_WRECK_BYTES] = b[4:]
        elif kind == RK_FEATURES:
            self.heads = struct.unpack_from("<iiiI", b)
        elif kind == RK_GLOBALS:
            if not b or len(b) % 4:
                raise Unreadable(f"a globals image of {len(b)} bytes")
            self.globals = bytearray(b)
        elif kind == RK_GLOBALS_DELTA:
            if self.globals is None:
                raise Unreadable("a globals delta before the globals image")
            n = len(self.globals) // 4
            nbits = (n + 7) // 8
            self._delta(self.globals, b[:nbits], n, b[(nbits + 3) & ~3:])
        elif kind == RK_PROJ_HEAD:
            if len(b) != 8:
                raise Unreadable(f"a projectile head of {len(b)} bytes")
            count, hw = struct.unpack_from("<II", b)
            if count > hw or hw > self.ncap:
                raise Unreadable(f"a projectile count {count} and extent {hw} past the pool's "
                                 f"{self.ncap} records")
            if self.phead and hw < self.phead[1]:
                self.proj[hw * REC_PROJ_IMAGE:self.phead[1] * REC_PROJ_IMAGE] = \
                    bytes((self.phead[1] - hw) * REC_PROJ_IMAGE)
            self.phead = (count, hw)
        elif kind in (RK_PROJ, RK_PROJ_DELTA):
            i = struct.unpack_from("<H", b)[0]
            if self.phead is None or i >= self.phead[1] or \
                    (kind == RK_PROJ and len(b) != 4 + REC_PROJ_IMAGE):
                raise Unreadable(f"projectile record {i} outside the pool's written extent")
            img = memoryview(self.proj)[i * REC_PROJ_IMAGE:(i + 1) * REC_PROJ_IMAGE]
            if kind == RK_PROJ:
                img[:] = b[4:]
            else:
                rec = bytearray(img)
                self._delta(rec, b[2:2 + REC_PROJ_BITS], REC_PROJ_DWORDS, b[REC_PROJ_DELTA_HEAD:])
                img[:] = rec
        elif kind == RK_MAPPED:
            off = struct.unpack_from("<I", b)[0]
            if len(b) < 5 or off != self.mnext or off + len(b) - 4 > len(self.mapped):
                raise Unreadable(f"an explored map record at +0x{off:X}, out of order or past "
                                 f"its {len(self.mapped)} bytes")
            self.mapped[off:off + len(b) - 4] = b[4:]
            self.mnext += len(b) - 4
        elif kind == RK_MAPPED_RUNS:
            if self.mnext != len(self.mapped):
                raise Unreadable("explored map runs before the map is whole")
            at = 0
            while at < len(b):
                if len(b) - at < 8:
                    raise Unreadable("an explored map run cut short")
                off, n = struct.unpack_from("<II", b, at)
                if not n or n > len(b) - at - 8 or off + n > len(self.mapped):
                    raise Unreadable(f"an explored map run at +0x{off:X} past the map")
                self.mapped[off:off + n] = b[at + 8:at + 8 + n]
                at += (8 + n + 3) & ~3
        elif kind == RK_SIGHT:
            p, off = b[0], struct.unpack_from("<I", b, 4)[0]
            cnt = self.sight.setdefault(p, bytearray())
            if p >= REC_PLAYERS or len(b) <= 8 or off != len(cnt):
                raise Unreadable(f"record {p}'s sight counters out of order at +0x{off:X}")
            cnt += b[8:]
        elif kind == RK_IMAGE:
            which, off = b[0], struct.unpack_from("<I", b, 4)[0]
            size = (REC_MAIN_BYTES, REC_DATA_BYTES)[which] if which < 2 else 0
            if not size or off + len(b) - 8 > size:
                raise Unreadable(f"an image record past its image ({which}, +0x{off:X})")
            img = self.images.setdefault(which, bytearray(size))
            img[off:off + len(b) - 8] = b[8:]
        elif kind in EVENTS:
            return kind, b
        else:
            raise Unreadable(f"record kind {kind} in a block")
        return None

    def whole(self, stream=False):
        """An iteration, a snapshot or a dump ends with no order list owed records, no feature
        call or death open and the explored map whole or, but in the stream, absent."""
        if self.mnext != len(self.mapped) and (stream or self.mnext):
            raise Unreadable(f"an iteration with {self.mnext} of the explored map's "
                             f"{len(self.mapped)} bytes")
        if self.pend is not None:
            raise Unreadable(f"slot {self.pend[0]}'s order list cut short at the end of its "
                             f"iteration")
        if self.fbracket or self.deaths:
            raise Unreadable("a feature call's or a death's records cut short at the end of its "
                             "iteration")

    @staticmethod
    def _delta(img, bits, dwords, vals):
        k = 0
        for i in range(dwords):
            if bits[i >> 3] >> (i & 7) & 1:
                img[4 * i:4 * i + 4] = vals[4 * k:4 * k + 4]
                k += 1

    def differences(self, other, names, other_names=None, limit=20, gnames=None, sight=False):
        """Lines naming each difference, the first `limit` of them, and their count. `names` are
        this world's tables and `other_names` the other's, when another build numbered them;
        `gnames` the globals image's dwords by name, when the list is known (globals_list); with
        `sight`, the players' sight counters too, which only dumps carry."""
        out, n = [], 0

        def note(line):
            nonlocal n
            n += 1
            if len(out) < limit:
                out.append(line)

        def dwords(what, a, b):
            for i in range(0, max(len(a), len(b)), 4):
                if a[i:i + 4] != b[i:i + 4]:
                    note(f"{what} +0x{i:03X}: recording {a[i:i + 4].hex()}, dump {b[i:i + 4].hex()}")

        for slot in sorted(set(self.units) | set(other.units)):
            a, b = self.units.get(slot), other.units.get(slot)
            if a is None or b is None:
                note(f"unit {slot}: live in {'the dump' if a is None else 'the recording'} only")
            elif a != b:
                dwords(f"unit {slot}", a, b)
        for key in sorted(set(self.lists) | set(other.lists)):
            a, b = self.lists.get(key, bytearray()), other.lists.get(key, bytearray())
            what = f"unit {key[0]}'s {'sub' if key[1] else 'main'} list"
            if len(a) != len(b):
                note(f"{what}: {len(a) // REC_ORDER_IMAGE} nodes in the recording, "
                     f"{len(b) // REC_ORDER_IMAGE} in the dump")
            elif a != b:
                for i in range(0, len(a), 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        note(f"{what} node {i // REC_ORDER_IMAGE} +0x{i % REC_ORDER_IMAGE:02X}: "
                             f"recording {a[i:i + 4].hex()}, dump {b[i:i + 4].hex()}")
        for p in range(REC_PLAYERS):
            dwords(f"player {p}", self.players[p], other.players[p])
            if self.ctl[p] != other.ctl[p]:
                note(f"player {p}'s controller: recording {self.ctl[p]}, dump {other.ctl[p]}")
        if self.view != other.view:
            note(f"the controlled and viewed players: recording {self.view}, dump {other.view}")
        def by_name(w, tabs):
            return {tabs.get(ix, {}).get("name", f"table {ix}"): (tabs.get(ix, {}).get("stride"), t)
                    for ix, t in w.tables.items()}

        if (self.gw, self.gh, self.nrec) != (other.gw, other.gh, other.nrec):
            note(f"the features: a {self.gw} x {self.gh} grid and {self.nrec} wreck records in "
                 f"the recording, {other.gw} x {other.gh} and {other.nrec} in the dump")
        else:
            for at in range(0, len(self.grid), REC_CELL_BYTES):
                a, b = self.grid[at:at + REC_CELL_BYTES], other.grid[at:at + REC_CELL_BYTES]
                if a != b:
                    c = at // REC_CELL_BYTES
                    note(f"cell ({c % self.gw}, {c // self.gw}): recording {a.hex()}, "
                         f"dump {b.hex()}")
            for at in range(0, len(self.pool), REC_WRECK_BYTES):
                a, b = self.pool[at:at + REC_WRECK_BYTES], other.pool[at:at + REC_WRECK_BYTES]
                if a != b:
                    dwords(f"wreck record {at // REC_WRECK_BYTES}", a, b)
        if self.heads != other.heads:
            note(f"the wreck pool's heads and the cursor: recording {self.heads}, "
                 f"dump {other.heads}")
        if self.globals is None or other.globals is None:
            if self.globals != other.globals:
                note(f"the globals image: in {'the dump' if self.globals is None else 'the recording'}"
                     f" only")
        elif len(self.globals) != len(other.globals):
            note(f"the globals image: {len(self.globals)} bytes in the recording, "
                 f"{len(other.globals)} in the dump")
        else:
            for i in range(0, len(self.globals), 4):
                a, b = self.globals[i:i + 4], other.globals[i:i + 4]
                if a != b:
                    name = (gnames or {}).get(i // 4, f"dword {i // 4}")
                    note(f"globals {name}: recording {a.hex()}, dump {b.hex()}")
        if self.ncap != other.ncap:
            note(f"the projectile pool: {self.ncap} records in the recording, {other.ncap} in "
                 f"the dump")
        else:
            if self.phead != other.phead:
                note(f"the projectile count and extent: recording {self.phead}, "
                     f"dump {other.phead}")
            hw = max(self.phead[1] if self.phead else 0, other.phead[1] if other.phead else 0)
            for at in range(0, hw * REC_PROJ_IMAGE, REC_PROJ_IMAGE):
                a, b = self.proj[at:at + REC_PROJ_IMAGE], other.proj[at:at + REC_PROJ_IMAGE]
                if a != b:
                    dwords(f"projectile {at // REC_PROJ_IMAGE}", a, b)
        if len(self.mapped) != len(other.mapped):
            pass                        # the grid's sizes differ, named above
        elif (self.mnext == len(self.mapped)) != (other.mnext == len(other.mapped)):
            note(f"the explored map: in {'the recording' if self.mnext else 'the dump'} only")
        else:
            for i in range(0, len(self.mapped) - 1, 2):
                a, b = self.mapped[i:i + 2], other.mapped[i:i + 2]
                if a != b:
                    note(f"explored map entry {i // 2} (+0x{i:X}): recording {a.hex()}, "
                         f"dump {b.hex()}")
        for p in sorted(set(self.sight) | set(other.sight)) if sight else ():
            a, b = self.sight.get(p), other.sight.get(p)
            if a is None or b is None or len(a) != len(b):
                note(f"record {p}'s sight counters: {len(a) if a is not None else 'none'} in the "
                     f"recording, {len(b) if b is not None else 'none'} in the dump")
                continue
            diff = [i for i in range(len(a)) if a[i] != b[i]]
            if diff:
                note(f"record {p}'s sight counters: {len(diff)} cells differ, the first +0x"
                     f"{diff[0]:X}: recording {a[diff[0]]}, dump {b[diff[0]]}")
        mine = by_name(self, names)
        theirs = by_name(other, names if other_names is None else other_names)
        for name in sorted(set(mine) | set(theirs)):
            if name not in mine or name not in theirs:
                note(f"{name}: declared by the {'recording' if name in mine else 'dump'}'s build only")
                continue
            (sa, a), (sb, b) = mine[name], theirs[name]
            if sa != sb:
                note(f"{name}: rows of {sa} bytes in the recording, {sb} in the dump")
                continue
            if len(a) != len(b):
                note(f"{name}: {len(a)} bytes in the recording, {len(b)} in the dump")
                continue
            if a != b:
                for i in range(0, len(a), 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        note(f"{name} +0x{i:X}: recording {a[i:i + 4].hex()}, "
                             f"dump {b[i:i + 4].hex()}")
        return out, n


def snapshot_world(payload, slots, tables, dims, base=None):
    """A snapshot or a dump builds the world from nothing, its seats and viewer included; a
    snapshot's cells over the recording's base grid, a dump's over nothing (it carries them all)."""
    w = World(slots, tables, dims=dims)
    if base is not None:
        w.grid[:] = base
    for kind, b in records(payload):
        w.apply(kind, b)
    w.whole()
    return w


def load(path, sizes=None):
    bl, stop = blocks(path, sizes)
    if not bl or bl[0][0] != RB_HEADER:
        raise Unreadable(f"{path}: no header block" + (f" ({stop})" if stop else ""))
    return header(bl[0][3]), bl, stop


def stream_to(h, bl, want=None, on_snapshot=None, on_record=None):
    """The world the stream builds, to the end of iteration `want` (or of the file). Calls
    on_snapshot(world, snapshot payload) at each snapshot block on the way, and
    on_record(world, kind, body) after each record is applied -- and with kind None before a TICK
    is, while the world is still the iteration's before it. The world's `base` is its grid at the
    end of the first iteration, which a snapshot's cells differ from."""
    w = World(h["slots"], h["tables"], h)
    for kind, first, last, payload in bl[1:]:
        if kind == RB_SNAPSHOT and on_snapshot:
            on_snapshot(w, payload)
        if kind != RB_STREAM:
            continue
        for rk, b in records(payload):
            if rk == RK_TICK and w.tick is not None and w.base is None:
                w.whole(True)
                w.base = bytes(w.grid)
            if rk == RK_TICK and want is not None and w.tick is not None and w.tick >= want:
                w.whole(True)
                return w
            if rk == RK_TICK and on_record:
                on_record(w, None, b)
            w.apply(rk, b)
            if on_record:
                on_record(w, rk, b)
        w.whole(True)           # a stream block ends where an iteration does
    return w


# ---- the events against the images -----------------------------------------------------------

class EventCheck:
    """Each iteration's records are its images, then the events of the step that follows them
    (tagpu_recfmt.h). Between the images of two iterations, a unit that appears must have its
    create, one that disappears its death, one whose state byte +0x10E changed a state event with
    the new byte, and one whose build fraction +0x104 reached 0.0 its build done -- unless the slot
    died and was made again in the step, which the create and the death both name."""

    def __init__(self, limit=20):
        self.open = False           # an iteration has begun
        self.in_events = False      # ... and its images are behind us
        self.summary_now = None     # its world, once its images are applied
        self.events = []            # its events so far
        self.pending = None         # (world, events) of the iteration before, until this one's
        self.checked = 0
        self.lines, self.n, self.limit = [], 0, limit
        self.counts = {}

    def note(self, line):
        self.n += 1
        if len(self.lines) < self.limit:
            self.lines.append(line)

    @staticmethod
    def summary(w):
        return {slot: (bytes(img[U_TYPE:U_TYPE + 4]), img[U_STATEBYTE],
                       bytes(img[U_BUILT:U_BUILT + 4]))
                for slot, img in w.units.items()}

    def images_done(self, w):
        self.summary_now = self.summary(w)
        if self.pending is not None:
            self.transition(self.pending[0], self.pending[1], self.summary_now, w.tick)
            self.pending = None
        self.in_events = True

    def on_record(self, w, kind, b):
        if kind is None:
            self.before_tick(w)
            return
        if kind == RK_TICK:
            # apply() has already moved w.tick on; the iteration before ended with the last record
            if self.open:                     # before_tick has summarised it
                self.pending = (self.summary_now, self.events)
            self.open, self.in_events, self.events = True, False, []
            return
        if kind in EVENTS:
            self.counts[kind] = self.counts.get(kind, 0) + 1
            if not self.in_events:
                self.images_done(w)
            self.events.append((kind, b))

    def before_tick(self, w):
        """Called before a TICK is applied: an iteration with no events ends its images here."""
        if self.open and not self.in_events:
            self.images_done(w)

    def finish(self, w):
        if self.open and not self.in_events:
            self.images_done(w)

    def transition(self, was, events, now, tick):
        self.checked += 1
        made, died, state, built = set(), set(), {}, set()
        for kind, b in events:
            if kind == RK_EV_CREATE:
                made.add(struct.unpack_from("<I", b)[0] - 1)
            elif kind == RK_EV_DEATH:
                died.add(struct.unpack_from("<I", b)[0] - 1)
            elif kind == RK_EV_STATE:
                state[struct.unpack_from("<I", b)[0] - 1] = b[4]
            elif kind == RK_EV_BUILT:
                built.add(struct.unpack_from("<I", b, 4)[0] - 1)
        at = f"the step to GameTime {tick}"
        for slot in sorted(set(was) | set(now)):
            a, b = was.get(slot), now.get(slot)
            if a is None:
                if slot not in made:
                    self.note(f"{at}: unit {slot} appears with no create")
                continue
            if b is None:
                if slot not in died:
                    self.note(f"{at}: unit {slot} disappears with no death")
                continue
            if slot in made or slot in died:
                if not (slot in made and slot in died):
                    self.note(f"{at}: unit {slot} stays live across a "
                              f"{'create' if slot in made else 'death'} alone")
                continue
            if a[1] != b[1] and state.get(slot) != b[1]:
                self.note(f"{at}: unit {slot}'s +0x10E went {a[1]:#x} -> {b[1]:#x} with "
                          + (f"its last state event at {state[slot]:#x}" if slot in state
                             else "no state event"))
            if a[2] != bytes(4) and b[2] == bytes(4) and slot not in built:
                self.note(f"{at}: unit {slot}'s +0x104 reached 0.0 with no build done")


# ---- the commands ----------------------------------------------------------------------------

def cmd_info(path):
    sizes = []
    h, bl, stop = load(path, sizes)
    print(f"{path}")
    print(f"  map {h.get('map')!r}, mode {h.get('mode')}, build {h.get('build')}, "
          f"from GameTime {h.get('start')}")
    print(f"  {h.get('slots')} unit slots, {h.get('units_each')} units each, "
          f"unit defs {h.get('unitdefs')}, layout {h.get('layout', '')[:12]}")
    o = h.get("options", {})
    print(f"  options: LosType {o.get('lostype')}, commander death {o.get('cmdrdeath')}, "
          f"difficulty {o.get('difficulty')}, fixed starts {o.get('fixed_starts')}, "
          f"{o.get('players')} players, controlled {o.get('controlled')}, viewed {o.get('viewed')}")
    print(f"  settings map {h.get('settings_map')!r}, weapons {h.get('weapons')}")
    f = h.get("features")
    if f:
        print(f"  features: a {f[0]} x {f[1]} grid, {f[2]} wreck records, {f[3]} feature defs "
              f"(names 0x{f[4]:08x})")
    print(f"  globals list {h.get('globals', '')[:12]}, a projectile pool of "
          f"{h.get('projectiles')} records")
    for p in h["players"]:
        print(f"  player {p['index']}: {p['name']!r}, controller {p['controller']}, "
              f"side {p['side']}, logo {p['logo']}, alliances {p['alliances'].hex()}")
    for lv in h["levers"]:
        print(f"  lever {lv['file']}: {'set' if lv['set'] else 'absent'}"
              + (f", {lv['text'][:40]!r}" if lv["read"] else ""))
    for ix, t in sorted(h["tables"].items()):
        print(f"  table {ix}: {t['name']}, {t['stride']} bytes a row, layout {t['layout']:08x}")
    counts, raw = {}, {}
    for kind, first, last, payload in bl:
        counts[kind] = counts.get(kind, 0) + 1
        raw[kind] = raw.get(kind, 0) + len(payload)
    size = os.path.getsize(path)
    print(f"  {size} bytes: " + ", ".join(f"{counts[k]} {RB_NAMES.get(k, k)} ({raw[k] >> 10} KB raw)"
                                          for k in sorted(counts)))
    sraw = sum(r for k, a, b, r, pk in sizes if k == RB_STREAM)
    spk = sum(pk for k, a, b, r, pk in sizes if k == RB_STREAM)
    print(f"  the stream: {sraw} bytes raw, {spk} packed")
    for k, first, last, r, pk in sizes:
        if k == RB_SNAPSHOT:
            print(f"  snapshot at GameTime {last}: {r} bytes raw, {pk} packed")
    ev, evb = {}, {}
    for kind, first, last, payload in bl:
        if kind != RB_STREAM:
            continue
        for rk, b in records(payload):
            if rk in EVENTS or rk in (RK_ORDERS, RK_CONTROLLER, RK_VIEWER, RK_CELLS, RK_WRECK,
                                      RK_GLOBALS, RK_GLOBALS_DELTA, RK_PROJ_HEAD, RK_PROJ,
                                      RK_PROJ_DELTA, RK_MAPPED, RK_MAPPED_RUNS):
                ev[rk] = ev.get(rk, 0) + 1
                evb[rk] = evb.get(rk, 0) + 4 + ((len(b) + 3) & ~3)
    names = {**EVENTS, RK_ORDERS: "order list", RK_CONTROLLER: "controller", RK_VIEWER: "viewer",
             RK_CELLS: "run of cells", RK_WRECK: "wreck record", RK_GLOBALS: "globals image",
             RK_GLOBALS_DELTA: "globals delta", RK_PROJ_HEAD: "projectile head",
             RK_PROJ: "projectile record", RK_PROJ_DELTA: "projectile delta",
             RK_MAPPED: "explored map record", RK_MAPPED_RUNS: "explored map runs"}
    print("  records: " + (", ".join(f"{ev[k]} {names[k]} ({evb[k] >> 10} KB)" for k in sorted(ev))
                           or "no order lists, seats or events"))
    ends = [b for b in bl if b[0] == RB_END]
    if ends:
        e = end(ends[-1][3])
        print(f"  ended: {END_REASONS.get(e.get(RT_END_REASON), e.get(RT_END_REASON))}, "
              f"last GameTime {e.get(RT_LAST)}, {e.get(RT_ITERATIONS)} iterations, "
              f"{e.get(RT_BAD_IDS)} id rows outside their tables, "
              f"the ring held {e.get(RT_RING_HIGH, 0) >> 10} KB at most, "
              f"{e.get(RT_OFF_THREAD)} events refused off the game thread, "
              f"{e.get(RT_UNCREDITED)} nano sprays uncredited, "
              f"{e.get(RT_UNOBSERVED)} grid cells changed by no observed writer")
        for kind, table, off, count in e["corrections"]:
            if kind == RK_TABLE_SPAN:
                t = h["tables"].get(table, {"name": f"#{table}", "stride": 0})
                where = (f"table {t['name']} row {off // t['stride']} +0x{off % t['stride']:X}"
                         if t["stride"] else f"table {t['name']} +0x{off:X}")
            elif kind == 0:
                where = "more offsets than the end block counts one by one"
            elif kind == RK_ORDERS:
                where = (f"{'sub' if table else 'main'} order list"
                         + (" (its length)" if off == REC_ORDERS_LENGTH else f" node +0x{off:X}"))
            else:
                where = f"{RK_NAMES.get(kind, kind)} +0x{off:X}" + \
                        (" (its liveness)" if kind == RK_UNIT and off == REC_UNIT_IMAGE else "")
            print(f"  CORRECTED {count}x: {where}")
    else:
        print("  INCOMPLETE: no end block" + (f" ({stop})" if stop else ""))
    if stop:
        print(f"  read to its last complete block: {stop}")
    return 0


def cmd_check(path):
    h, bl, stop = load(path)
    bad, seen = 0, 0
    gnames = known_names(h)

    def at_snapshot(w, payload):
        nonlocal bad, seen
        seen += 1
        s = snapshot_world(payload, h["slots"], h["tables"], feature_dims(h), w.base)
        lines, n = w.differences(s, h["tables"], gnames=gnames)
        if s.tick != w.tick:
            lines.insert(0, f"the snapshot is at GameTime {s.tick}, the stream at {w.tick}")
            n += 1
        if n:
            bad += 1
            print(f"snapshot at GameTime {s.tick}: {n} differences from the stream")
            for line in lines:
                print("  " + line.replace("dump", "snapshot"))

    ec = EventCheck()
    w = stream_to(h, bl, on_snapshot=at_snapshot, on_record=ec.on_record)
    ec.finish(w)
    print(f"{path}: the stream reads to GameTime {w.tick} ({w.corrections} corrections in it); "
          f"{seen} snapshots, {bad} differing" + (f"; stopped at {stop}" if stop else ""))
    print(f"  the events against the images: {ec.checked} steps, "
          + ("every change explained" if not ec.n else f"{ec.n} UNEXPLAINED") + "; "
          + (", ".join(f"{ec.counts[k]} {EVENTS[k]}" for k in sorted(ec.counts)) or "no events"))
    for line in ec.lines:
        print("  " + line)
    return 1 if bad or w.corrections or ec.n else 0


def viewer_of(h):
    """V, the record a playing seats the viewer in: the lowest the recording does not seat
    (tagpu_replay.c, validate)."""
    seated = {p["index"] for p in h["players"]}
    free = [i for i in range(REC_PLAYERS) if i not in seated]
    if not free:
        raise Unreadable("the recording seats all ten records, so a playing has no seat for the "
                         "viewer and refuses it")
    return free[0]


def dump_world(path, h):
    """A dump's world, read with its own header's tables and the recording's dimensions."""
    dbl, dstop = blocks(path)
    if len(dbl) != 2 or dbl[0][0] != RB_HEADER or dbl[1][0] != RB_SNAPSHOT:
        raise Unreadable(f"{path}: not a dump (a header and one snapshot block)" +
                         (f" ({dstop})" if dstop else ""))
    dh = header(dbl[0][3])
    return dh, snapshot_world(dbl[1][3], h["slots"], dh["tables"], feature_dims(h))


def mapped_hold(w, bit):
    """The explored map less the viewer's bit, which a puppet host keeps set in every entry
    (tagpu_replay.c, recorded_put): masked on both sides of a puppet compare."""
    if bit >= 16:
        return
    for i in range(0, len(w.mapped) - 1, 2):
        v = struct.unpack_from("<H", w.mapped, i)[0] & ~(1 << bit)
        struct.pack_into("<H", w.mapped, i, v)


def puppet_view(w, viewer, radar=False):
    """A world less what a puppet host keeps as its own (demo-recorder-s2-host.md §1.1, §2.3): the
    contact bits +0x110 8-10 and 12, which its radar pass computes for its own view (D1) -- bit 12
    only, with `radar`, when the host views the player the recording viewed, whose radar picture
    it computes from the same inputs, and loop 4 of the pass, which sets bit 12, runs only for a
    type 1 or 2 owner; the viewer's record V; and the controllers and the controlled and viewed
    players, which are the host's seating."""
    mask = 0x1000 if radar else 0x1700
    for img in w.units.values():
        struct.pack_into("<I", img, U_STATE, struct.unpack_from("<I", img, U_STATE)[0] & ~mask)
    w.players[viewer] = bytearray(REC_PLAYER_IMAGE)
    w.ctl = [0] * REC_PLAYERS
    w.view = [0, 0]


def _generator(name):
    """One of tools/'s generators, as a module, for its parser."""
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"),
                                                  os.path.join(here, "..", name + ".py"))
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    return gen


def globals_list(want=None):
    """replay-globals.tsv through its own parser (tools/replay-globals-gen.py): (the generator, its
    rows, the image's slots, the file's SHA-256). With `want`, a recording's or a dump's RT_GLOBALS,
    the file must be the one it was made with: the classes and the coverage are this checkout's."""
    import hashlib
    gen = _generator("replay-globals-gen")
    with open(gen.TSV, "rb") as f:
        raw = f.read()
    digest = hashlib.sha256(raw).hexdigest()
    if want is not None and want != digest:
        raise Unreadable(f"made with another replay-globals.tsv ({want[:12]}) than this checkout's "
                         f"({digest[:12]}): its classes and coverage would be the wrong ones")
    rows = gen.parse(raw.decode("utf-8"))
    return gen, rows, gen.slots(rows)[0], digest


def globals_names(slots):
    """The globals image's dwords by name: a slot's, and `+n` for the dwords after its first."""
    out = {}
    for sl in slots:
        for k in range((sl["size"] + 3) // 4):
            out[sl["dword"] + k] = sl["names"] + (f"+{4 * k}" if k else "")
    return out


def globals_hold(w, slots):
    """The globals image less its `hold` bits, which a puppet host never writes (the slice note's
    S3b): masked on both sides of a puppet compare."""
    if w.globals is None:
        return
    for sl in slots:
        if not sl["hold"]:
            continue
        at = 4 * sl["dword"]
        if sl["size"] > 4:
            w.globals[at:at + ((sl["size"] + 3) & ~3)] = bytes((sl["size"] + 3) & ~3)
        else:
            v = struct.unpack_from("<I", w.globals, at)[0] & ~sl["hold"]
            struct.pack_into("<I", w.globals, at, v & 0xFFFFFFFF)


def puppet_rows():
    """The order node's puppet rows, from the exclusion file through its own parser
    (tools/replay-layout-gen.py): (offset, size, mask, held set) for each."""
    gen = _generator("replay-layout-gen")
    with open(gen.TSV, encoding="utf-8") as f:
        rows = gen.parse(f.read())
    return [(r["offset"], r["size"], r["mask"], r["kind"] == "puppet1") for r in rows
            if r["record"] == "order" and r["kind"] in ("puppet0", "puppet1")]


def puppet_hold(w, rows):
    """The recording's order nodes as a puppet host holds them (the slice note's *Decided
    2026-09-30*): each puppet row at 0 (puppet0) or set (puppet1). Applied to the recording only,
    so the compare checks that the host holds them."""
    for img in w.lists.values():
        for at in range(0, len(img), REC_ORDER_IMAGE):
            for off, size, mask, held in rows:
                v = int.from_bytes(img[at + off:at + off + size], "little")
                v = v | mask if held else v & ~mask
                img[at + off:at + off + size] = v.to_bytes(size, "little")


def known_names(h):
    """The globals' names when this checkout's list is the recording's, else None (dword numbers)."""
    if not h.get("globals"):
        return None
    try:
        return globals_names(globals_list(h["globals"])[2])
    except Unreadable:
        return None


def cmd_compare(path, dumps, puppet=False):
    h, bl, stop = load(path)
    status = 0
    held = puppet_rows() if puppet else None
    gslots = globals_list(h.get("globals"))[2] if puppet else None
    gnames = globals_names(gslots) if puppet else known_names(h)
    for d in dumps:
        dh, dw = dump_world(d, h)
        w = stream_to(h, bl, want=dw.tick)
        if w.tick != dw.tick:
            print(f"{d}: GameTime {dw.tick}, but the recording reads to {w.tick}")
            status = 1
            continue
        radar = False
        if puppet:
            radar = dw.view[1] == w.view[1]
            # V's bit is the host's record's, as recorded_put takes it (the dump is the host's)
            bit = dw.players[viewer_of(h)][PL_MAPBIT]
            mapped_hold(w, bit)
            mapped_hold(dw, bit)
            puppet_view(w, viewer_of(h), radar)
            puppet_view(dw, viewer_of(h), radar)
            puppet_hold(w, held)
            globals_hold(w, gslots)
            globals_hold(dw, gslots)
        lines, n = w.differences(dw, h["tables"], dh["tables"], gnames=gnames)
        print(f"{d}: GameTime {dw.tick}, {len(dw.units)} live units"
              + (", the radar picture too" if radar else "") + ": "
              + ("EQUAL" if not n else f"{n} DIFFERENCES"))
        for line in lines:
            print("  " + line)
        if n:
            status = 1
    return status


# THE COARSE MAP of the two images, for `compare --images`: what the notes say lies in each range
# (the engine map; tools/tamem_ghidra.h's TAdynmemStruct with this project's corrections). A run is
# reported under the narrowest range that holds its first byte, or as not mapped. Offsets are from
# main, and for .data from its start, 0x501000.
IMAGE_MAP = {
    0: [(0x0, 0xC, "two pointers into .data (+4, +8)"),
        (0xC, 0x14, "the program and DirectSound pointers"),
        (0x14, 0x4DE, "HAPINET: the network state"),
        (0x511, 0x519, "the pacing gate's record and lag, rewritten every frame"),
        (0x519, 0x1B63, "the desktop GUI"),
        (0x1B63, 0x299C, "the eleven player records"),
        (0x299C, 0x29A4, "the branch selector, the skirmish settings' pointer"),
        (0x29D0, 0x2A25, "the start positions"),
        (0x2A30, 0x2A3C, "the restrict list, the packet buffer"),
        (0x2A3C, 0x2A45, "the player count, the controlled and viewed players, the network flags"),
        (0x2A45, 0x2CF3, "UI and input: mouse, build placement, cursor, order mode"),
        (0x2CF3, 0x141F3, "Weapons[256], the weapon defs"),
        (0x141F3, 0x141FB, "the projectile count and the pool's pointer"),
        (0x141FB, 0x1428F, "the map: the wreck pool and its heads, the feature grid, the map's "
                           "size, the wind's limits, gravity, sea level, LosType"),
        (0x1428F, 0x142BB, "the LOS grid and the sort buckets"),
        (0x142CB, 0x1434F, "the minimap, the radar pictures, the camera, the eye, the scroll"),
        (0x1434F, 0x1436F, "the unit counter, slots, the unit array's pointers, the hot units"),
        (0x14377, 0x1439F, "the model pointers, UNITINFOCount, the unit defs"),
        (0x147DF, 0x148DF, "GAF sequence pointers: render assets"),
        (0x1491B, 0x1AB90, "the explosions (stock's pool)"),
        (0x1AB9B, 0x1AB9F, "a 3D object built at level load"),
        (0x1AB9F, 0x1E88F, "the debris pool"),
        (0x33A0F, 0x37E13, "the sound table"),
        (0x37E13, 0x37E1F, "a category table, the back buffer"),
        (0x37E1F, 0x37EC4, "the screen, the game-screen rectangle, the GUI state"),
        (0x37EC4, 0x37EE6, "the wind"),
        (0x37EE6, 0x37EFE, "units each, the limits, difficulty, side, ActiveCommanderDeath"),
        (0x37F06, 0x37F31, "options, sound flags, SoftwareDebugMode"),
        (0x37F31, 0x38A37, "the network timeout, the race and side data"),
        (0x38A37, 0x38A53, "the clock and pacing: DeltaTime, GameTime, the speeds, the pause"),
        (0x38A53, 0x38C53, "the screenshot and movie folders"),
        (0x38C53, 0x391C0, "mostly not mapped: the particle layers, the loader's flags, the "
                           "profile counters, the end state"),
        (0x391C3, 0x3922D, "show ranges, GameingState, the objective, the GUI callback, fonts, "
                           "DirectPlay, game options"),
        (0x39239, 0x3923D, "the game end's countdown and flags"),
        (0x3923D, 0x3924D, "a .data string's address")],
    1: [(0x0, 0x10A00, "file-backed constants and tables"),
        (0x109B8, 0x10A3C, "an object, the pathfinder's budget"),
        (0x10A58, 0x10DC8, "the GAF and animation state"),
        (0x10A60, 0x10C65, "a table"),
        (0x10DE8, 0x10DEC, "the main pointer"),
        (0x10DF0, 0x10F80, "stock's flying-piece slots"),
        (0x10F80, 0x10FB0, "a pool allocator, GAF frame counts"),
        (0x112A0, 0x11339, "the weapon parse contexts, the meteor state"),
        (0x11344, 0x1134C, "the order descriptors' vector"),
        (0x11358, 0x11758, "the movement classes"),
        (0x11AD8, 0x11DD4, "the network state"),
        (0x11EF8, 0x12000, "the GUI panels and slides"),
        (0x1D53C, 0x1D6CC, "the key poll and shell, a pool, the build-site test, sound flags, "
                           "the objective's timer"),
        (0x1D548, 0x1D598, "DLL entry points"),
        (0x1D810, 0x1D814, "an object"),
        (0x1EB50, 0x1EBD4, "a registry value, checksums, the graphics globals' pointer"),
        (0x1EBE0, 0x1EC88, "timers"),
        (0x1EC88, 0x1EC8C, "the simulation RNG"),
        (0x279A4, 0x2A65C, "the C runtime")],
}


def dump_images(path):
    """A dump's GameTime, its RT_GLOBALS and its two images."""
    dbl, stop = blocks(path)
    if len(dbl) != 2 or dbl[0][0] != RB_HEADER or dbl[1][0] != RB_SNAPSHOT:
        raise Unreadable(f"{path}: not a dump (a header and one snapshot block)" +
                         (f" ({stop})" if stop else ""))
    dh = header(dbl[0][3])
    images, tick = {}, None
    for kind, b in records(dbl[1][3]):
        if kind == RK_TICK:
            tick = struct.unpack_from("<I", b)[0]
        elif kind == RK_IMAGE:
            which, off = b[0], struct.unpack_from("<I", b, 4)[0]
            size = (REC_MAIN_BYTES, REC_DATA_BYTES)[which] if which < 2 else 0
            if not size or off + len(b) - 8 > size:
                raise Unreadable(f"{path}: an image record past its image ({which}, +0x{off:X})")
            images.setdefault(which, bytearray(size))[off:off + len(b) - 8] = b[8:]
    if set(images) != {0, 1}:
        raise Unreadable(f"{path}: a dump without the main block and .data (RK_IMAGE)")
    return tick, dh.get("globals"), images


def cmd_compare_dumps(path, a_path, b_path):
    """Two dumps of playings of FILE at the same GameTime, the same player viewed: every record
    they carry, the players' sight counters included, equal byte for byte. FILE gives the
    dimensions and the globals' names; only its header is read."""
    h = header_only(path)
    ah, a = dump_world(a_path, h)
    bh, b = dump_world(b_path, h)
    if a.tick != b.tick:
        print(f"{a_path} is at GameTime {a.tick}, {b_path} at {b.tick}")
        return 1
    lines, n = a.differences(b, ah["tables"], bh["tables"], gnames=known_names(h), sight=True)
    print(f"{a_path} against {b_path}: GameTime {a.tick}: "
          + ("EQUAL" if not n else f"{n} DIFFERENCES") + " (A is the first, B the second)")
    for line in lines:
        print("  " + line.replace("the recording", "A").replace("recording", "A")
              .replace("the dump", "B").replace("dump", "B"))
    return 1 if n else 0


def cmd_images(a_path, b_path):
    """The differing bytes no row of replay-globals.tsv covers, as runs, grouped by IMAGE_MAP. A
    byte is reported when it differs in a bit no row names."""
    ta, ga, ia = dump_images(a_path)
    tb, gb, ib = dump_images(b_path)
    if ga != gb:
        raise Unreadable(f"the two dumps were made with different replay-globals.tsv "
                         f"({(ga or '')[:12]}, {(gb or '')[:12]})")
    gen, rows, slots, digest = globals_list(ga)
    cov = gen.coverage(rows)
    print(f"{a_path} (GameTime {ta}) against {b_path} (GameTime {tb})"
          + ("" if ta == tb else ": NOT THE SAME GameTime"))
    total = 0
    for which, what in ((0, "main"), (1, ".data")):
        a, b = ia[which], ib[which]
        where = "main" if which == 0 else "data"
        runs, start = [], None
        for i in range(len(a) + 1):
            hit = False
            if i < len(a) and a[i] != b[i]:
                hit = bool((a[i] ^ b[i]) & ~cov.get((where, i), 0) & 0xFF)
            if hit and start is None:
                start = i
            elif not hit and start is not None:
                runs.append((start, i - start))
                start = None
        groups = {}
        for at, n in runs:
            best = None
            for lo, hi, label in IMAGE_MAP[which]:
                if lo <= at < hi and (best is None or hi - lo < best[1] - best[0]):
                    best = (lo, hi, label)
            groups.setdefault(best[2] if best else "not mapped", []).append((at, n))
        nbytes = sum(n for at, n in runs)
        total += nbytes
        print(f"  {what}: {len(runs)} runs, {nbytes} bytes outside the list")
        for label in sorted(groups, key=lambda k: groups[k][0][0]):
            print(f"    {label}:")
            for at, n in groups[label]:
                name = f"main+0x{at:X}" if which == 0 else f"0x{REC_DATA_BEGIN + at:06X}"
                print(f"      {name}, {n} byte{'s' if n != 1 else ''}")
    print(f"  {total} bytes differ outside replay-globals.tsv (a report: S3c's null twin "
          f"classifies them)")
    return 0


def main(argv):
    puppet = argv[2:3] == ["--puppet"] and argv[1:2] == ["compare"]
    images = argv[2:3] == ["--images"] and argv[1:2] == ["compare"]
    if puppet or images:
        argv = argv[:2] + argv[3:]
    if len(argv) < 3 or argv[1] not in ("info", "check", "compare", "compare-dumps") or \
            (argv[1] == "compare" and len(argv) < 4) or (images and len(argv) != 4) or \
            (argv[1] == "compare-dumps" and len(argv) != 5):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        if argv[1] == "info":
            return cmd_info(argv[2])
        if argv[1] == "check":
            return cmd_check(argv[2])
        if images:
            return cmd_images(argv[2], argv[3])
        if argv[1] == "compare-dumps":
            return cmd_compare_dumps(argv[2], argv[3], argv[4])
        return cmd_compare(argv[2], argv[3:], puppet)
    except (Unreadable, OSError, zlib.error, struct.error, KeyError) as e:
        print(f"tarec: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
