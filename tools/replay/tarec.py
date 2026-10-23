#!/usr/bin/env python3
"""tarec -- reads an Impure demo recording (research/notes/tadr-port/demo-recorder-skirmish-slice.md).

  tools/replay/tarec.py info  FILE           the header, the blocks, the events and how the
                                             recording ended; its timelines and how each ended
  tools/replay/tarec.py check FILE           rebuild the world from the stream and require it to
                                             equal every snapshot the file carries, and every
                                             change of the world between two iterations to have
                                             the events that explain it
  tools/replay/tarec.py compare FILE DUMP... rebuild the world to each dump's GameTime and require
                                             it to equal the dump, byte for byte
  tools/replay/tarec.py lossy-check FILE [--to T]
                                             the stream against its exact records (RB_EXACT, the
                                             test lever tagpu_rec.exact), iteration by iteration:
                                             every shown byte exact, every held byte the stream's
                                             last value or the exact one, the two equal at every
                                             refresh, and the refreshes on their grid
                                             (cmd_lossy_check); to GameTime T when given
  tools/replay/tarec.py segments FILE        each timeline's segment of the file and its bytes,
                                             then the SHA-256 of the original's
  tools/replay/tarec.py compare-fork FILE N  timeline N's first iteration against its parent's
                                             chain at fork_at, less what the takeover's rebuild
                                             makes its own (fork_view)
  tools/replay/tarec.py compare --puppet FILE DUMP...
                                             the same for dumps a playing of FILE took, less what
                                             a puppet host keeps as its own (puppet_view), with
                                             the order nodes' puppet rows as it holds them
                                             (puppet_hold), the globals' `hold` rows and the
                                             viewer's bit of the explored map masked;
                                             the radar picture is compared too where the dump's
                                             viewed player is the recording's. On a file that
                                             holds its cells the unit slots and yard bit expected
                                             are the exact stream's (cells_exact); with --seek,
                                             the dumps a seek's, they are checked by THE
                                             OCCUPANCY RULE instead, the contest marks with them
  tools/replay/tarec.py compare --images DUMP OTHERDUMP
                                             a report, not a check: the bytes of the main block
                                             and .data that differ between two dumps and that no
                                             row of replay-globals.tsv covers, in runs, grouped by
                                             what the notes map there (exit 0 whatever it finds)
  tools/replay/tarec.py compare-dumps FILE DUMP OTHERDUMP [--takeover] [--seek]
                                             two dumps of playings of FILE at one GameTime with
                                             the same player viewed (a seek in a fresh process
                                             and one in a running playback) equal in every
                                             record, the players' sight counters included; --takeover
                                             applies the existing fork_view comparison policy;
                                             --seek, OTHERDUMP a seek's, checks its unit slots,
                                             yard bits and contest marks by THE OCCUPANCY RULE in
                                             a segment that holds its cells; the rules are those
                                             of the segment that holds the dumps' GameTime
  tools/replay/tarec.py counts FILE DUMP     a dump's counts of roots, sinks and observed sites
  tools/replay/tarec.py compare-counts FILE LIVE_A LIVE_B PUPPET_A PUPPET_B [--absent TSV]
                                             [--rewind | --cob-exact]
                                             a live game's counts against a playing's, each the
                                             change between its two dumps: roots per site
                                             exactly, the script slice per placement exactly,
                                             sinks per (site, caller, aux) within
                                             replay-absent.tsv's band less its rows, and no COB
                                             engine write on the puppet; --cob-exact compares
                                             the VM's START and CALL children by script name
                                             exactly, with no band and no row
  tools/replay/tarec.py images-gate FILE L_T1 L_TLAST TWIN1 TWIN2 P_T1 P_TLAST R_T1 Q_T1 [VIEW...]
                                             the null twin: every bit of the two images no row
                                             covers where the live game and a playing or the
                                             seek differ at T1, classified presentation,
                                             host-variant or load-constant, or a FAIL
  tools/replay/tarec.py scan FILE DUMP       every plain value of the dump's tables, unit,
                                             player and projectile images and globals image
                                             that lies inside the main block, an engine pool or
                                             Impure's DLL: an address recorded as a value

  tools/replay/tarec.py pov FILE [--check]  the human seat's perspective: every camera and cursor
                                             sample, click and selection, by GameTime; with
                                             --check, require each record well formed, samples
                                             at least their `every` apart and each one differing
                                             from the last, every selection ascending inside its
                                             seat's block, and the stream's perspective to equal
                                             the one every snapshot rebuilds
  tools/replay/tarec.py pov FILE --eye T --viewer W,H [--seat P]
                                             what the playback's Follow puts on a W x H view at
                                             displayed GameTime T: the sample the perspective
                                             clock takes, the eye before the range clamp and the
                                             level x 65536 (tagpu_persp.c, mirrored)

  check, compare, compare --puppet, compare-dumps and lossy-check take --timeline N: the chain of
  timeline N, read across each fork it descends from. check, compare and compare --puppet take --exact: the
  world built from the exact records instead of the stream (exact_blocks).

  images-gate alone takes --stock-secondary-cosmetics: the owner-approved native cosmetic-pool
  known gap for stock-secondary-orders at the native 300-projectile cap. Other bytes stay strict.

  exit 0 when every check holds, 1 when one fails, 2 when a file cannot be read.

THE FORMAT is tagpu/ddraw/inc/tagpu_recfmt.h, which this file mirrors: a change there is a change
here, and REC_FORMAT says which; this tool reads REC_FORMAT alone. A recording is REC_MAGIC, then
blocks; a block is a 32-byte header and its payload, one zstd frame unless the block is stored (the
header, end and timelines blocks always are), each checked by a CRC-32 -- but for a sectioned stream
block, whose payload is a chunk of its run's frame, and a chained snapshot (RBF_CHAINED), whose
frame takes the segment's snapshot before it as its prefix (tagpu_recfmt.h, THE RUN AND THE
CHAIN). A stream block may be a
sectioned container (RBF_SECTIONS) that decodes to exactly the records it was made from: its unit
deltas are made by the unit frame from the header's RT_CODEC, RT_PHYSICS, RT_AIR and RT_TYPES, its
projectile records by the pool frame from the pool of RT_PROJECTILES records, and the bodies of
its cells, orders, events and state records by the four record frames. The walk over
the blocks, their unpacking and that decoding are tools/replay/reccodec.py's (iter_blocks), which
refuses what the DLL's reader refuses; every command here reads the decoded records.
A skirmish recording's stream is LOSSY (tagpu/ddraw/src/tagpu_reclossy.c): between two refreshes
it may hold the bytes its header's RT_HOLD names at a value the engine produced earlier, and at
each refresh -- the segment's first two iterations and then every GameTime 1 modulo RT_HOLD's
interval (is_refresh), the last iteration of a stream block flagged RBF_REFRESH -- its world is
the writer's exact copy. With RT_HOLD's RECL_SPRAYS it leaves out the nano sprays its spray
sessions (RK_SPRAY_SESSION) imply, which every reader makes again at the end of their iteration
(Sprays; World.iteration_end). A snapshot follows a refresh block only. A recording made under the test lever tagpu_rec.exact
carries each stream block's exact records after it, as an RB_EXACT block. A file ends at
its last complete block: a block cut short or torn ends the reading, and the file is reported as
read to there. A dump (`dump <name>` in tagpu_replay.trigger) is REC_MAGIC, a stored header with the
format, the build, the globals list's hash and the tables, and one stored snapshot, which carries
every cell and, after the world, the players' sight counters (RK_SIGHT) and the main block and
.data whole (RK_IMAGE). A recording's snapshot
carries only the cells and the wreck records that differ from its segment's first iteration's grid
and pool, the base and the wreck base, which a reader takes from the stream, and none of a table's
spans that are all zero. A table is paired with the
recording's by its name: each build numbers its tables in its own registry order, and a recording
may be played by another build than the one that made it. The header gives each table's layout
hash as well (`info` shows it), which the replay's seek compares with its own declaration's. The
globals image is replay-globals.tsv's fields, read through tools/replay-globals-gen.py where a
command needs their names or classes. The explored map is carried whole at the first iteration,
in every snapshot and in every dump, and as runs of changed bytes between.

THE SEGMENTS: a file is the magic and one or more segments, each an RB_HEADER and its blocks, every
one after the first right after an RB_END and naming its timeline (RT_TIMELINE): a takeover's fork,
appended to the file it was played from. A chain of timelines is read as the DLL's reader reads it
(tagpu_recreader.c): each segment to the GameTime before its child's fork_at, where the child's
first iteration replaces the world whole.

Needs numpy, numba and zstandard, imported (through reccodec.py) only by the functions that read
a file's blocks: main() re-executes under the shared venv when they are missing (devpy.py), and
the build's `tareplay.py features --check` imports this file under a bare python3. header_only
reads the stored header with the standard library alone.

TAREC_CACHE=DIR keeps each recording's decoded blocks in DIR, keyed by the file's bytes and the
decoder's sources, so a second command on the same file does not decode it again (reccache.py).
"""
import os
import re
import struct
import sys
import zlib
from datetime import datetime, timedelta, timezone
from pathlib import Path
import reccache
from recmp_file import Contributions, Seats, History

REC_FORMAT = 25                           # the start's offset from UTC; the unit frame range-coded; transport and pad queries, BeginTransport and setSFXoccupy as owner starts; sprays against the last four, order nodes matched, projectile lanes; the unit listing from state; script slices, owner's queries, yards; the unit frame's type rules (RT_TYPES); kinds as patterns, ticks once a block; zstd runs and chains, hold rows, sprays as sessions, cells held
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
# REC_VIEWER: the record a playing seats the viewer in, the engine's no-player record 10, which no
# recording carries; and its bit of the explored map, its +0x146, 10 by the seat activation
# (0x46434D) (tagpu_replay.c, THE VIEWER'S SEAT)
VIEWER = VIEWER_BIT = 10
REC_UNIT_BITS = (REC_UNIT_DWORDS + 7) // 8
REC_PLAYER_BITS = (REC_PLAYER_DWORDS + 7) // 8
REC_UNIT_DELTA_HEAD = (2 + REC_UNIT_BITS + 3) & ~3
REC_PLAYER_DELTA_HEAD = (2 + REC_PLAYER_BITS + 3) & ~3
U_TYPE, U_BUILT, U_STATEBYTE, U_YARD = 0x92, 0x104, 0x10E, 0x10F   # the image's type ID, build fraction, state
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
REC_INFO_AT = 0x14C                       # PlayerInfo's bytes in a player image
INFO_TYPE = 0x94                          # PlayerInfo's copy of the record's type (0x463C6F)


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
 RK_VERIFY_PROJ, RK_IMAGE, RK_MAPPED, RK_MAPPED_RUNS, RK_SIGHT, RK_COUNTS,
 RK_COUNT_NAME, RK_MINIMAP, RK_EV_NET_CREATE, RK_POV, RK_CLICK, RK_SELECT, RK_POV_AT,
 RK_SPRAY_SESSION, RK_EV_SLICE, RK_EV_QUERY, RK_EV_YARD) = range(60)
REC_QUERIES = 4                           # RK_EV_QUERY's queries: QueryBuildInfo, QueryNanoPiece, QueryTransport, QueryLandingPad

# THE PERSPECTIVE's payloads (tagpu_recpov.h)
REC_POV = struct.Struct("<BBBBHHIiiiii")   # player, every, flags, zero, view_w, view_h, zoom,
                                           # cam_x, cam_y, cur_x, cur_h, cur_z: 32 bytes
REC_CLICK = struct.Struct("<BBBBBBHiii")   # player, button, order, cursor, region, itype, keys,
                                           # x, h, z: 20 bytes
REC_SEL = struct.Struct("<BBHHH")          # player, flags, total, first, count: 8 bytes
REC_SEL_ENTRY = struct.Struct("<HI")       # slot, birth: 6 bytes
REC_POV_EVERY_MAX = 30
REC_POV_WORLD, REC_POV_MINIMAP = 1, 2
REC_POV_ZOOM_MIN, REC_POV_ZOOM_MAX = 16384, 524288
REC_POV_VIEW_MAX = 16384
REC_SEL_BEGIN, REC_SEL_END = 1, 2
REC_SEL_MAX = 1500
REC_CLICK_KEYS = 0x000C                           # MK_SHIFT | MK_CONTROL
PL_BLOCKFIRST, PL_BLOCKLAST = 0x67, 0x6B          # a player image's unit block, as IDs
REC_COUNT_DLL = 0x80000000           # RK_COUNTS: a caller inside Impure's DLL, as its offset
RK_NAMES = {RK_UNIT: "unit", RK_PLAYER: "player", RK_TABLE_SPAN: "table", RK_ORDERS: "order list",
            RK_GLOBALS: "globals image", RK_PROJ: "projectile record",
            RK_PROJ_HEAD: "projectile count (+0x0) and extent (+0x4)"}
EVENTS = {RK_EV_CREATE: "create", RK_EV_NET_CREATE: "network create", RK_EV_DEATH: "death", RK_EV_SHOT: "shot",
          RK_EV_INTERCEPT: "interception", RK_EV_START: "broadcast start",
          RK_EV_START_NAMED: "owner's start", RK_EV_BUILT: "build done", RK_EV_STATE: "state byte",
          RK_EV_REPLY: "reply", RK_EV_SPRAY: "nano spray", RK_EV_FEATURE: "feature call",
          RK_EV_FEATURE_END: "feature call's end", RK_EV_DRAW: "feature draw",
          RK_EV_DEATH_END: "death's end", RK_EV_SLICE: "script slice", RK_EV_QUERY: "owner's query",
          RK_EV_YARD: "yard"}
FR_TICK, FR_SPAWN, FR_DESTROY, FR_DIE, FR_BURN, FR_DAMAGE, FR_SWAP, FR_CORPSE = range(8)
REC_PHASE_LATE = 2                        # RK_EV_FEATURE's phase byte at most (tagpu_recfmt.h)
ROOT_ARGS = (0, 28, 8, 12, 12, 16, 12, 32)     # an RK_EV_FEATURE's arguments after its site
FS_SOURCE = 6           # reproduction's source test: the tick's input where the slots are the engine's
FS_SITES = 7

RB_HEADER, RB_STREAM, RB_SNAPSHOT, RB_END, RB_MINIMAP, RB_TIMELINES = 1, 2, 3, 4, 5, 6
RB_CONTRIBUTIONS = 7
RB_EXACT = 8                # the exact records of the stream block before it (tagpu_rec.exact)
RB_BOUNDARY = -1            # load()'s marker of a fork in a chain's blocks: no block of the file
RECR_TIMELINES = 64         # the segments a file may hold (tagpu_recreader.h)
RB_NAMES = {RB_HEADER: "header", RB_STREAM: "stream", RB_SNAPSHOT: "snapshot", RB_END: "end",
            RB_MINIMAP: "minimap", RB_TIMELINES: "timelines", RB_CONTRIBUTIONS: "contributions",
            RB_EXACT: "exact stream"}
RBF_STORED, RBF_SECTIONS, RBF_FRAME_RESET, RBF_REFRESH, RBF_CHAINED = 1, 2, 4, 8, 16

(RT_FORMAT, RT_BUILD, RT_MODE, RT_MAP, RT_START, RT_WALLCLOCK, RT_SLOTS, RT_UNITS_EACH,
 RT_UNITDEFS, RT_PLAYER, RT_LEVER, RT_LAYOUT, RT_TABLE, RT_OPTIONS, RT_SETTINGS_MAP,
 RT_WEAPONS, RT_FEATURES, RT_GLOBALS, RT_PROJECTILES, RT_FRAME, RT_DLL, RT_FIELDS,
 RT_GAME_VERSION, RT_ARCHIVE, RT_MODPRINT, RT_TIMELINE) = range(1, 27)
RT_MULTIPLAYER = 27
RT_MP_SEATS = 28
RT_CODEC, RT_PHYSICS, RT_AIR = 29, 30, 31   # the unit frame's lanes, physics and aircraft
(RT_END_REASON, RT_LAST, RT_ITERATIONS, RT_CORRECTIONS, RT_BAD_IDS, RT_RING_HIGH, RT_OFF_THREAD,
 RT_UNCREDITED, RT_UNOBSERVED, RT_GAME_END, RT_SEAT_END) = range(32, 43)
RT_MP_END = 43
RT_CODEC_DECLINED, RT_CODEC_MISMATCH = 44, 45   # the writer's stream blocks written as records
RT_HOLD = 46                # what a skirmish stream holds between refreshes, and how often
RT_TYPES = 47               # the unit frame's per-type constants
REC_HOLD_BYTES = 8 + 32 + REC_UNIT_IMAGE + REC_ORDER_IMAGE
REC_REFRESH_TICKS, REC_REFRESH_MIN = 300, 30
RECL_CELLS, RECL_SPRAYS = 1, 2
REC_HOLD_POS = range(0x6A, 0x76)          # the unit position, which a spray session reads
REC_SPRAY_FOLLOW = 8                      # RK_SPRAY_SESSION's mode: the point follows the unit
RESULTS = {0: "unknown", 1: "won", 2: "lost"}

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
               10: "INCOMPLETE: the explored map was not the one it began with",
               11: "INCOMPLETE: a later timeline ended it at its last complete block",
               12: "pruned: a deleted ancestor's retained prefix",
               13: "complete: continued from a live rewind"}


class Unreadable(Exception):
    pass


# ---- blocks ----------------------------------------------------------------------------------

def _codec():
    """reccodec.py, imported on the first read of a file's blocks: it needs numpy (numba for a
    sectioned block and zstandard for a packed one), which the build's bare python3 may lack."""
    try:
        import reccodec
    except ImportError as e:
        raise Unreadable(f"reading a recording's blocks needs numpy, numba and zstandard ({e.name} "
                         f"is missing): the shared venv (CLAUDE.md, Python tooling)") from None
    return reccodec


def blocks(path, sizes=None, offsets=None, flags=None, decode=True):
    """(kind, first, last, payload) for each complete block, a stream block's payload its records,
    and why the reading stopped (None at a clean end of the file); with `sizes`, (kind, first,
    last, raw, packed) of each block appended to it (raw the block header's: a sectioned block's
    container), with `offsets` its bytes [start, end) in the file and with `flags` its flags. With
    decode=False the stream, snapshot and minimap payloads are None and nothing is decoded. A
    decoding walk is read from TAREC_CACHE's entry for the file when there is one (reccache.py)."""
    codec = _codec()
    walked, stop = (reccache.walk(path, codec) if decode else
                    reccache.collect(codec.iter_blocks(path, decode=False)))
    out = []
    for b in walked:
        out.append((b.kind, b.first, b.last, b.payload))
        if sizes is not None:
            sizes.append((b.kind, b.first, b.last, b.raw, b.packed))
        if offsets is not None:
            offsets.append((b.offset, b.offset + 32 + b.packed))
        if flags is not None:
            flags.append(b.flags)
    return out, stop


def header_only(path):
    """A recording's header, read from its first block alone, which is always stored: the rest of
    the file is neither read nor unpacked."""
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
        if flags != RBF_STORED or raw != packed:
            raise Unreadable(f"{path}: a header block with flags {flags:#x}, {packed} bytes for "
                             f"{raw}: a header is always stored")
        body = f.read(packed)
    if len(body) < packed or zlib.crc32(body) != crc_body:
        raise Unreadable(f"{path}: its header block is cut short or fails its CRC")
    return header(body)


_REC_HEAD = struct.Struct("<HH")
_U16 = struct.Struct("<H").unpack_from


def records(payload):
    at, end, head = 0, len(payload), _REC_HEAD.unpack_from
    while at + 4 <= end:
        kind, n = head(payload, at)
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
        elif tag == RT_WALLCLOCK and len(b) == 12:
            h["wallclock"], h["utc_offset"] = struct.unpack_from("<Qi", b)
        elif tag == RT_SLOTS: h["slots"] = u32
        elif tag == RT_UNITS_EACH: h["units_each"] = u32
        elif tag == RT_UNITDEFS and len(b) == 8: h["unitdefs"] = struct.unpack_from("<II", b)
        elif tag == RT_PLAYER:
            if len(b) < 16+0xB9 or b[0] >= 10 or any(p["index"] == b[0] for p in h["players"]):
                raise Unreadable("malformed or duplicate seat in the header")
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
        elif tag == RT_FRAME: h["frame"] = u32
        elif tag == RT_DLL: h["dll"] = struct.unpack_from("<II", b)
        elif tag == RT_GAME_VERSION: h["version"] = b.decode("latin-1")
        elif tag == RT_ARCHIVE:
            size, mtime = struct.unpack_from("<IQ", b)
            h.setdefault("archives", []).append((b[12:].decode("latin-1"), size, mtime))
        elif tag == RT_MODPRINT:
            h["modprint"] = u32
            if len(b) >= 8:
                h["archives_mounted"] = struct.unpack_from("<I", b, 4)[0]
        elif tag == RT_TIMELINE and len(b) == 12:
            h["timeline"] = struct.unpack_from("<III", b)
        elif tag == RT_MULTIPLAYER:
            if "multiplayer" in h:
                raise Unreadable("duplicate contribution roster")
            try:
                Contributions(b)
            except ValueError as error:
                raise Unreadable(str(error)) from error
            h["multiplayer"] = b
        elif tag == RT_MP_SEATS:
            if "mp_seats" in h:
                raise Unreadable("duplicate contribution seat mapping")
            h["mp_seats"] = b
        elif tag == RT_FIELDS:
            ix, n = struct.unpack_from("<HH", b)
            h.setdefault("fields", {})[ix] = [struct.unpack_from("<HHHHBB", b, 4 + 12 * k)
                                              for k in range(n)]
        elif tag == RT_HOLD:
            if "hold" in h:
                raise Unreadable(f"header tag {tag} twice")
            h["hold"] = hold_parse(b)
        elif tag in (RT_CODEC, RT_PHYSICS, RT_AIR, RT_TYPES):
            # kept whole for reccodec.params, which bounds them (the block walk already has)
            if tag in h.setdefault("codec", {}):
                raise Unreadable(f"header tag {tag} twice")
            h["codec"][tag] = bytes(b)
    if h.get("format") != REC_FORMAT:
        raise Unreadable(f"format {h.get('format')}, this tool reads {REC_FORMAT}")
    if (h.get("mode") == 3) != ("multiplayer" in h) or ("multiplayer" in h) != ("mp_seats" in h):
        raise Unreadable("network mode and contribution roster disagree")
    if "multiplayer" in h and "hold" in h:
        raise Unreadable("a multiplayer header with RT_HOLD: its stream holds nothing")
    if "multiplayer" in h:
        try:
            Seats(h["mp_seats"], Contributions(h["multiplayer"]).actors,
                  h.get("slots"), h.get("units_each"), h["players"])
        except ValueError as error:
            raise Unreadable(str(error)) from error
    return h


def hold_parse(b):
    """RT_HOLD's bytes, bounded as tagpu_reclossy.c's reclossy_hold_parse bounds them."""
    if len(b) != REC_HOLD_BYTES:
        raise Unreadable(f"an RT_HOLD of {len(b)} bytes, not {REC_HOLD_BYTES}")
    refresh, flags = struct.unpack_from("<II", b)
    unit = bytes(b[40:40 + REC_UNIT_IMAGE])
    node = bytes(b[40 + REC_UNIT_IMAGE:])
    if flags & ~(RECL_CELLS | RECL_SPRAYS):
        raise Unreadable(f"RT_HOLD's flags {flags:#x}")
    if not REC_REFRESH_MIN <= refresh <= REC_REFRESH_TICKS:
        raise Unreadable(f"RT_HOLD's refresh interval {refresh}")
    if any(unit[i] for i in REC_HOLD_POS):
        raise Unreadable("RT_HOLD holds a byte of the unit position +0x6A..+0x75")
    return {"refresh": refresh, "flags": flags, "sha": bytes(b[8:40]), "unit": unit, "node": node}


def holds_cells(h):
    """1 when a header's stream holds its cells' unit slots and yard bit (RT_HOLD's RECL_CELLS):
    its playing keeps the engine's own (tagpu_recreader.h, recr_holds_cells)."""
    return bool(h) and bool(h.get("hold", {}).get("flags", 0) & RECL_CELLS)


def holds_nothing(h):
    """True when a header's stream holds nothing, as tagpu_recreader.h's recr_holds_nothing: no
    RT_HOLD, or one with no flag and no held bit. Its stream's world is then the writer's exact
    copy at every iteration, not only at a refresh."""
    hold = h.get("hold")
    return not hold or (not hold["flags"] and not any(hold["unit"]) and not any(hold["node"]))


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
    e = {"corrections": [], "seats": []}
    for tag, b in entries(payload):
        if tag == RT_CORRECTIONS:
            kind, table, off, count = struct.unpack_from("<BxHII", b)
            e["corrections"].append((kind, table, off, count))
        elif tag == RT_SEAT_END:
            e["seats"].append(struct.unpack_from("<BBxxII", b))
        elif len(b) >= 4:
            e[tag] = struct.unpack_from("<I", b)[0]
    return e


# ---- the world -------------------------------------------------------------------------------

def pov_fields(b):
    """A RecPov's fields, each bounded as tagpu_recpov.h's rec_pov_ok bounds it."""
    if len(b) != REC_POV.size:
        raise Unreadable(f"a sample of {len(b)} bytes")
    f = REC_POV.unpack(b)
    player, every, flags, zero, view_w, view_h, zoom = f[:7]
    if player >= REC_PLAYERS or not 1 <= every <= REC_POV_EVERY_MAX or \
            flags & ~(REC_POV_WORLD | REC_POV_MINIMAP) or zero or \
            not REC_POV_ZOOM_MIN <= zoom <= REC_POV_ZOOM_MAX or \
            not 1 <= view_w <= REC_POV_VIEW_MAX or not 1 <= view_h <= REC_POV_VIEW_MAX:
        raise Unreadable(f"a malformed sample {f}")
    return f


def click_fields(b):
    """A RecClick's fields, each bounded as rec_click_ok bounds it."""
    if len(b) != REC_CLICK.size:
        raise Unreadable(f"a click of {len(b)} bytes")
    f = REC_CLICK.unpack(b)
    player, button, _, _, region, itype, keys = f[:7]
    if player >= REC_PLAYERS or button > 1 or region > 3 or itype > 1 or keys & ~REC_CLICK_KEYS:
        raise Unreadable(f"a malformed click {f}")
    return f


def sel_block(player_image, slots):
    """The seat's block of slots, inclusive, from its player image's two unit IDs (rec_sel_block)."""
    first, last = struct.unpack_from("<I", player_image, PL_BLOCKFIRST)[0], \
        struct.unpack_from("<I", player_image, PL_BLOCKLAST)[0]
    if first < 2 or last < first or last > slots:
        raise Unreadable(f"a selection's seat has no unit block (IDs {first}, {last})")
    return first - 1, last - 1


# ---- the playback's perspective arithmetic (tagpu_persp.c, mirrored line for line) -----------

PERSP_NONE, PERSP_LATEST, PERSP_BETWEEN, PERSP_EARLIER = range(4)
PERSP_HOW = ("none", "latest", "between", "earlier")


def _tdiv(a, b):
    """a / b truncated toward zero, as C99's integer division is (b > 0)."""
    q = abs(a) // b
    return q if a >= 0 else -q


def _sat32(v):
    return max(-0x80000000, min(0x7FFFFFFF, v))


def persp_sample(samples, n):
    """THE PERSPECTIVE CLOCK (tagpu_persp.h, persp_sample) for the displayed GameTime n over a
    seat's held samples, World.pov's [(GameTime, RecPov bytes)], the earlier first: (how, fields)
    with fields REC_POV's tuple, or (PERSP_NONE, None)."""
    if not 1 <= len(samples) <= 2:
        return PERSP_NONE, None
    tb, bb = samples[-1]
    b = REC_POV.unpack(bb)
    t = n - b[1]
    if tb <= t:
        return PERSP_LATEST, b
    if len(samples) == 1:
        return PERSP_EARLIER, b
    ta, ab = samples[0]
    a = REC_POV.unpack(ab)
    if not (ta <= t and tb - ta <= 2 * b[1]):
        return PERSP_EARLIER, a
    num, den = t - ta, tb - ta
    out = list(a)
    fields = [4, 5, 6, 7, 8]                    # view_w, view_h, zoom, cam_x, cam_y
    if a[2] & REC_POV_WORLD and b[2] & REC_POV_WORLD:
        fields += [9, 10, 11]                   # the cursor
    for i in fields:
        out[i] = a[i] + _tdiv((b[i] - a[i]) * num, den)
    return PERSP_BETWEEN, tuple(out)


def persp_follow(f, view_w, view_h):
    """FOLLOW'S FIT (tagpu_persp.h, persp_follow) of a sample's fields to a view_w x view_h view:
    (eye_x, eye_y, level x 65536), or None for a view of no size."""
    if view_w <= 0 or view_h <= 0 or not f[4] or not f[5]:
        return None
    z = min(f[6] * view_w // f[4], f[6] * view_h // f[5])
    z = max(REC_POV_ZOOM_MIN, min(REC_POV_ZOOM_MAX, z))
    return _sat32(f[7] - view_w // 2), _sat32(f[8] - view_h // 2), z


_RUNS = {}


def _delta_runs(bits, dwords):
    """A delta mask's runs of set bits among its first `dwords`, as ((image byte, its end, values
    byte), ...), and the bytes of values they take. A recording repeats its masks -- Town &
    Country's 3.65 million deltas carry 12,445 (MEASURED 2026-10-08) -- so each is worked out
    once, and the cache is emptied when it reaches 65,536."""
    got = _RUNS.get((bits, dwords))
    if got is not None:
        return got
    if len(bits) < (dwords + 7) // 8:
        raise Unreadable(f"a delta's mask of {len(bits)} bytes, for {dwords} dwords")
    runs, k, i = [], 0, 0
    while i < dwords:
        if bits[i >> 3] >> (i & 7) & 1:
            j = i
            while j < dwords and bits[j >> 3] >> (j & 7) & 1:
                j += 1
            runs.append((4 * i, 4 * j, k))
            k += 4 * (j - i)
            i = j
        else:
            i += 1
    if len(_RUNS) >= 1 << 16:
        _RUNS.clear()
    got = _RUNS[(bits, dwords)] = (tuple(runs), k)
    return got


class Sprays:
    """THE SPRAY SESSIONS' TABLE every reader of a stream with RT_HOLD's RECL_SPRAYS keeps, as
    tagpu_reclossy.c's RecSprays: each unit slot's session (RK_SPRAY_SESSION: the period p, 1, 2
    or 4, and FOLLOW) and its sprays of the last four GameTimes -- one spray, or a mark of several
    -- with the unit's liveness and position +0x6A then. At the end of each iteration a slot with
    a session and no spray of its own gets the one its session implies: its spray of T - p, with
    FOLLOW its point moved by the unit's position change since, each axis modulo 2^32; one that
    would predict from no single spray, or follow a unit not live then and now, is malformed (the
    writer never leaves one open). `unit_of(slot)` gives (live, (x, y, z)) at the iteration."""
    ONE, SEVERAL = 1, 2

    def __init__(self, slots):
        self.slots = slots
        self.mode = {}                  # slot -> its session's mode, open sessions only
        self.at = {}                    # (slot, T % 4) -> (T, mark, live, pos, body)

    def empty(self):
        self.mode.clear()
        self.at.clear()

    def slot(self, b):
        """the unit slot a spray's or a session's ID names, or 0"""
        s = struct.unpack_from("<I", b)[0] - 1
        return s if 1 <= s < self.slots else 0

    def session(self, b):
        m = b[4] if len(b) == 8 else 0
        p = m & 7
        if len(b) != 8 or any(b[5:8]) or m & ~(7 | REC_SPRAY_FOLLOW) or \
                (p not in (1, 2, 4) if p else m) or not self.slot(b):
            raise Unreadable(f"a malformed spray session ({bytes(b).hex()})")
        if m:
            self.mode[self.slot(b)] = m
        else:
            self.mode.pop(self.slot(b), None)

    def note(self, s, T, body, live, pos):
        e = self.at.get((s, T % 4))
        if e and e[0] == T:
            self.at[(s, T % 4)] = (T, self.SEVERAL) + e[2:]
        else:
            self.at[(s, T % 4)] = (T, self.ONE, live, pos, bytes(body))

    def spray(self, T, b, unit_of):
        if len(b) != 48:
            raise Unreadable(f"a nano spray of {len(b)} bytes")
        s = self.slot(b)
        if s:
            self.note(s, T, b[4:], *unit_of(s))

    def predict(self, s, T, m, unit_of):
        p = m & 7
        e = self.at.get((s, (T - p) % 4))
        if not p or e is None or e[0] != T - p or e[1] != self.ONE:
            return None
        body = e[4]
        if m & REC_SPRAY_FOLLOW:
            live, now = unit_of(s)
            if not e[2] or not live:
                return None
            pt = struct.unpack_from("<III", body, 4)
            pt = [(v + n - o) & 0xFFFFFFFF for v, n, o in zip(pt, now, e[3])]
            body = body[:4] + struct.pack("<III", *pt) + body[16:]
        return body

    def imply(self, T, unit_of):
        """The sprays the sessions imply at the end of iteration T, 48 bytes each, slots
        ascending, each taken into the table."""
        out = []
        for s in sorted(self.mode):
            e = self.at.get((s, T % 4))
            if e and e[0] == T:
                continue                # a spray of its own in T
            body = self.predict(s, T, self.mode[s], unit_of)
            if body is None:
                raise Unreadable(f"the spray session of unit {s + 1} at GameTime {T} predicts "
                                 f"from no single spray of it, or follows it not live")
            out.append(struct.pack("<I", s + 1) + body)
            self.note(s, T, body, *unit_of(s))
        return out


class World:
    def __init__(self, slots, tables, header=None, dims=None):
        self.slots = slots
        self.header = header
        if dims is None:
            dims = feature_dims(header)
        self.gw, self.gh, self.nrec, self.ncap = dims
        self.grid = bytearray(self.gw * self.gh * REC_CELL_BYTES)     # every cell's image
        self.pool = bytearray(self.nrec * REC_WRECK_BYTES)             # every record's image
        self.heads = None               # the pool's three heads and the cursor, once sent
        self.base = None                # the grid at the end of the first iteration (stream_to)
        self.wbase = None               # ... and the wreck pool, taken with it
        self.globals = None             # the globals image, once sent
        self.proj = bytearray(self.ncap * REC_PROJ_IMAGE)              # the projectile pool's
        self.phead = None               # (count, hw), once sent: records [hw, ncap) are zero
        self.mapped = bytearray(mapped_bytes(self.gw, self.gh))    # the explored map
        self.mnext = 0                  # RK_MAPPED's next offset: len(mapped) once it is whole
        self.images = {}                # a dump's main block (0) and .data (1)
        self.sight = {}                 # a dump's sight counters, by player record
        self.counts = None              # a dump's counts: {(site, caller, aux): n}
        self.cmeta = None               # their (overflow, sinks counted, counting)
        self.cnames = {}                # the scripts' names by their FNV-1a
        self.fbracket = 0               # an RK_EV_FEATURE's root + 1, until its end
        self.deaths = []                # the IDs of the deaths whose end is still to come
        self.ystep = {}                 # the step's creates and deaths, slot -> "made"/"gone",
        self.yheld = set()              # and the slots whose yard waits for their create
        self.evented = False            # an event has come since the iteration's RK_TICK
        self.session = None             # the unit ID of the iteration's last RK_SPRAY_SESSION
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
        self.snap = False       # the records applied are a snapshot's (snapshot_world)
        self.pov = [[] for _ in range(REC_PLAYERS)]   # a seat's last two samples, the earlier
                                                      # first: (GameTime, its RecPov's bytes)
        self.sel = [None] * REC_PLAYERS     # a seat's selection, [(slot, birth)], once it has one
        self.clicks = []                    # the iteration's RK_CLICK bodies, in order
        self.sprays = Sprays(slots)         # the spray sessions' table (RECL_SPRAYS)
        self.ended = True                   # the iteration's implied sprays have been made

    def sprays_on(self):
        """the stream writes its nano sprays as sessions (RT_HOLD's RECL_SPRAYS)"""
        hold = self.header.get("hold") if self.header else None
        return bool(hold and hold["flags"] & RECL_SPRAYS)

    def unit_of(self, s):
        img = self.units.get(s)
        return (True, struct.unpack_from("<iii", img, 0x6A)) if img is not None else (False, (0, 0, 0))

    def iteration_end(self):
        """The sprays the iteration's spray sessions imply, (RK_EV_SPRAY, body) each, slots
        ascending: what follows its events in every reader's world (tagpu_recreader.c, tick_end).
        Once an iteration; a snapshot's world makes none."""
        if self.ended or self.snap:
            return []
        self.ended = True
        if not self.sprays_on():
            return []
        return [(RK_EV_SPRAY, b) for b in self.sprays.imply(self.tick, self.unit_of)]

    def yard_rule(self, kind, b):
        """THE YARD RULE (tagpu_recfmt.h), as the writer and the DLL's reader have it: a unit is
        live at an event when the images hold it and no death of it came since, or a create of it
        came since; a yard of a unit live there is its yard at its place, one of a unit not live
        is its create's (Create's first slice runs inside the create 0x485F50, which is recorded
        at its exit) and waits for that create in the step, and a yard naming the unit of a death
        still open is malformed, as is a death of a unit whose yard waits."""
        slot = struct.unpack_from("<I", b)[0] - 1
        if not 1 <= slot < self.slots:
            return
        if kind == RK_EV_CREATE:
            self.yheld.discard(slot)
            self.ystep[slot] = "made"
        elif kind == RK_EV_DEATH:
            if slot in self.yheld:
                raise Unreadable(f"the death of slot {slot}, whose create's yard waits for it")
            self.ystep[slot] = "gone"
        elif slot + 1 in self.deaths:
            raise Unreadable(f"a yard of slot {slot}, inside its own death")
        elif not (self.ystep.get(slot) == "made" or
                  (slot not in self.ystep and slot in self.units)):
            self.yheld.add(slot)

    def yard_end(self):
        """The step's end: every yard that waited met its create."""
        if self.yheld:
            raise Unreadable(f"a yard of slot {min(self.yheld)}, not live, whose create does not "
                             f"follow in its step")
        self.ystep.clear()

    def bracket(self, kind, b):
        """The brackets' grammar, as the DLL's reader has it (tagpu_recreader.c, event_take): a
        feature call's holds no other and cells only when it is the tick's, which is inside none;
        a death's holds its explosion's feature calls, in a multiplayer recording its corpse's
        (FR_CORPSE, inside a death only), and may hold deaths; a draw is inside one, FS_SOURCE inside
        the tick only, of a recording whose playing keeps the engine's slots: a multiplayer one, or
        one that holds its cells (RECL_CELLS)."""
        network = bool(self.header) and self.header.get("mode") == 3
        if kind == RK_EV_FEATURE:
            if self.fbracket or (b[0] == FR_TICK and self.deaths) or b[0] >= len(ROOT_ARGS) or \
                    (b[0] == FR_CORPSE and (not self.deaths or not network)) or \
                    b[1] > REC_PHASE_LATE or b[2] or b[3] or len(b) != 8 + ROOT_ARGS[b[0]]:
                raise Unreadable("a malformed feature call")
            self.fbracket = b[0] + 1
        elif kind == RK_EV_FEATURE_END:
            if len(b) != 8 or self.fbracket != b[0] + 1 or b[1] or b[2] or b[3]:
                raise Unreadable("a feature call's end with no call open")
            self.fbracket = 0
        elif kind == RK_EV_DEATH:
            if self.fbracket or len(self.deaths) == REC_DEATHS_MAX:
                raise Unreadable("a death inside a feature call, or nested too deep")
            self.yard_rule(kind, b)
            self.deaths.append(struct.unpack_from("<I", b)[0])
        elif kind == RK_EV_DEATH_END:
            if len(b) != 4 or not self.deaths or self.deaths[-1] != struct.unpack_from("<I", b)[0]:
                raise Unreadable("a death's end that closes no death")
            self.deaths.pop()
        elif kind == RK_EV_DRAW:
            if (not self.fbracket and not self.deaths) or len(b) != 12 or b[0] >= FS_SITES:
                raise Unreadable("a draw outside a feature call or a death")
            if b[0] == FS_SOURCE and (self.fbracket != FR_TICK + 1 or
                                      not (network or holds_cells(self.header))):
                raise Unreadable("a source test outside the tick of a recording that keeps the "
                                 "engine's slots")

    def perspective(self, kind, b):
        """THE PERSPECTIVE, as the DLL's reader has it (tagpu_recreader.c, pov_record): a skirmish
        file's, RK_POV and RK_CLICK a stream's and RK_POV_AT a snapshot's, a selection's slots
        inside its seat's block as the player image applied before it names it."""
        h = self.header
        if h is not None and h.get("mode") == 3:
            raise Unreadable(f"perspective record kind {kind} outside a skirmish file")
        if kind in (RK_POV, RK_POV_AT):
            if (kind == RK_POV_AT) != self.snap:
                raise Unreadable(f"perspective record kind {kind} in a "
                                 f"{'snapshot' if self.snap else 'stream'}")
            if kind == RK_POV:
                tick, body = self.tick, bytes(b)
            else:
                if len(b) != 4 + REC_POV.size:
                    raise Unreadable(f"a dated sample of {len(b)} bytes")
                tick, body = struct.unpack_from("<I", b)[0], bytes(b[4:])
            f = pov_fields(body)
            seat = self.pov[f[0]]
            if self.tick is None or tick > self.tick or (seat and seat[-1][0] >= tick):
                raise Unreadable(f"player {f[0]}'s sample at GameTime {tick} is not after its "
                                 f"last one and at most the iteration's")
            seat.append((tick, body))
            del seat[:-2]
        elif kind == RK_CLICK:
            if self.snap:
                raise Unreadable("a click in a snapshot")
            click_fields(b)
            self.clicks.append(bytes(b))
        else:
            if len(b) < REC_SEL.size:
                raise Unreadable(f"a selection of {len(b)} bytes")
            p, flags, total, first, count = REC_SEL.unpack_from(b)
            limit = REC_SEL_MAX if h is None else min(REC_SEL_MAX, h.get("units_each", 0))
            if len(b) != REC_SEL.size + count * REC_SEL_ENTRY.size or p >= REC_PLAYERS or \
                    flags != REC_SEL_BEGIN | REC_SEL_END or first or count != total or \
                    total > limit:
                raise Unreadable(f"a malformed selection of player {p} ({count} entries)")
            entries_ = [REC_SEL_ENTRY.unpack_from(b, REC_SEL.size + REC_SEL_ENTRY.size * i)
                        for i in range(count)]
            if entries_:
                lo, hi = sel_block(self.players[p], self.slots)
                if any(not lo <= slot <= hi for slot, _ in entries_) or \
                        any(a[0] >= c[0] for a, c in zip(entries_, entries_[1:])):
                    raise Unreadable(f"player {p}'s selection is not ascending inside its block "
                                     f"[{lo}, {hi}]")
            self.sel[p] = entries_

    def cells(self, b):
        x, z, n, zero = struct.unpack_from("<HHHH", b)
        if not n or n > REC_CELLS_MAX or zero or len(b) != 8 + n * REC_CELL_BYTES or \
                x + n > self.gw or z >= self.gh:
            raise Unreadable(f"a run of cells past the {self.gw} x {self.gh} grid")
        return (z * self.gw + x) * REC_CELL_BYTES

    def apply(self, kind, b):
        """One record; an event changes nothing here and is returned as (kind, body) -- the
        cells inside the tick's feature call too, which are its input. An RK_CELLS is an image
        only before the iteration's first event and tick input after it, which only the tick's
        feature call may hold: the readers' rule (tagpu_recreader.c, apply), which the writer's
        copy and so every snapshot follow (tagpu_recwriter.c, feature_record). An order list's
        records come together and in order, as the DLL's reader has them (tagpu_recreader.c,
        list_record)."""
        if kind == RK_UNIT_DELTA and self.pend is None and not self.fbracket and not self.deaths \
                and self.session is None:
            # the stream's most common record: with no order list, feature call or death open,
            # nothing below refuses it
            self._delta(self.units[_U16(b)[0]], b[2:2 + REC_UNIT_BITS], REC_UNIT_DWORDS,
                        b[REC_UNIT_DELTA_HEAD:])
            return None
        if self.pend is not None and kind != RK_ORDERS:
            raise Unreadable(f"slot {self.pend[0]}'s order list cut short by another record")
        if self.session is not None and kind not in (RK_SPRAY_SESSION, RK_TICK):
            raise Unreadable(f"record kind {kind} after the iteration's spray sessions")
        if kind in EVENTS:              # RCS_EVENTS's kinds (tagpu_recfmt.h, rec_section_of)
            self.evented = True
        if kind == RK_EV_NET_CREATE:
            h = self.header
            if not h or h.get("mode") != 3 or len(b) != 32 or \
                    "mp_seats" not in h or "unitdefs" not in h:
                raise Unreadable("a network create outside a native file")
            unit, unit_type, owner, water = struct.unpack_from("<IHBB", b)
            unfinished, _, reserved = struct.unpack_from("<fHH", b, 24)
            slot = unit - 1
            roster = h["mp_seats"]
            count = struct.unpack_from("<I", roster, 4)[0]
            if not 1 <= slot < self.slots or not 1 <= unit_type < h["unitdefs"][0] or \
                    owner >= REC_PLAYERS or water > 4 or reserved or not 0 <= unfinished <= 1 or not any(
                        row == owner and first <= slot <= last
                        for _, row, first, last in (struct.unpack_from("<IIII", roster, 16 + 16*i)
                                                    for i in range(count))):
                raise Unreadable("a network create outside its owner block/type/water bounds")
            return kind, b
        if kind in (RK_EV_FEATURE, RK_EV_FEATURE_END, RK_EV_DEATH, RK_EV_DEATH_END, RK_EV_DRAW):
            self.bracket(kind, b)
            return kind, b
        if kind == RK_EV_SLICE:
            # the unit tick's marker: inside no bracket, a slot of the level's (tagpu_recreader.c,
            # event_take)
            if self.fbracket or self.deaths:
                raise Unreadable("a script slice inside a feature call or a death")
            if len(b) != 4:
                raise Unreadable(f"a script slice of {len(b)} bytes")
            slot = struct.unpack_from("<I", b)[0] - 1
            if not 1 <= slot < self.slots:
                raise Unreadable(f"a script slice of slot {slot}, outside the array")
            return kind, b
        if kind == RK_EV_QUERY:
            # an order handler's query, in the unit tick: inside no bracket, a slot of the level's
            # (tagpu_recreader.c, event_take)
            if self.fbracket or self.deaths:
                raise Unreadable("an owner's query inside a feature call or a death")
            if len(b) != 8 or b[4] >= REC_QUERIES or any(b[5:8]):
                raise Unreadable(f"an owner's query of {len(b)} bytes, query {b[4] if len(b) > 4 else '-'}")
            slot = struct.unpack_from("<I", b)[0] - 1
            if not 1 <= slot < self.slots:
                raise Unreadable(f"an owner's query of slot {slot}, outside the array")
            return kind, b
        if kind == RK_EV_YARD:
            # YARD_OPEN's pass, outside every feature call, in a death or not, a slot of the
            # level's, where THE YARD RULE lets it stand (tagpu_recreader.c, event_take)
            if self.fbracket:
                raise Unreadable("a yard inside a feature call")
            if len(b) != 8 or b[4] > 1 or any(b[5:8]):
                raise Unreadable(f"a yard of {len(b)} bytes, yard {b[4] if len(b) > 4 else '-'}")
            slot = struct.unpack_from("<I", b)[0] - 1
            if not 1 <= slot < self.slots:
                raise Unreadable(f"a yard of slot {slot}, outside the array")
            self.yard_rule(kind, b)
            return kind, b
        if kind == RK_CELLS and (self.fbracket or self.deaths):
            if self.fbracket != FR_TICK + 1:
                raise Unreadable("cells inside a feature call that is not the tick's")
            self.cells(b)
            return kind, b
        if (self.fbracket or self.deaths) and kind not in EVENTS:
            raise Unreadable(f"record kind {kind} inside a feature call or a death")
        if kind in (RK_POV, RK_CLICK, RK_SELECT, RK_POV_AT):
            self.perspective(kind, b)
        elif kind == RK_TICK:
            self.yard_end()
            self.tick = struct.unpack_from("<I", b)[0]
            self.clicks = []
            self.evented = False
            self.ended = False
            self.session = None
        elif kind == RK_SPRAY_SESSION:
            # after the iteration's last other record, slots ascending (tagpu_recfmt.h)
            if self.snap or not self.sprays_on():
                raise Unreadable("a spray session outside a stream that writes its sprays as "
                                 "sessions")
            unit = struct.unpack_from("<I", b)[0] if len(b) >= 4 else 0
            if self.session is not None and unit <= self.session:
                raise Unreadable(f"the spray session of unit {unit} after unit {self.session}'s")
            self.sprays.session(b)
            self.session = unit
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
            if self.evented:
                raise Unreadable("cells after an event of their iteration, outside the tick's "
                                 "feature call")
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
        elif kind == RK_COUNTS:
            over, sinks, on, n = struct.unpack_from("<IIII", b)
            if len(b) != 16 + 16 * n or (self.cmeta and self.cmeta != (over, sinks, on)):
                raise Unreadable("a counts record cut short or disagreeing with the last")
            self.cmeta = (over, sinks, on)
            if self.counts is None:
                self.counts = {}
            for i in range(n):
                va, caller, aux, c = struct.unpack_from("<IIII", b, 16 + 16 * i)
                self.counts[(va, caller, aux)] = self.counts.get((va, caller, aux), 0) + c
        elif kind == RK_COUNT_NAME:
            aux = struct.unpack_from("<I", b)[0]
            self.cnames.setdefault(aux, b[4:].split(b"\0")[0].decode("latin-1"))
        elif kind == RK_IMAGE:
            which, off = b[0], struct.unpack_from("<I", b, 4)[0]
            size = (REC_MAIN_BYTES, REC_DATA_BYTES)[which] if which < 2 else 0
            if not size or off + len(b) - 8 > size:
                raise Unreadable(f"an image record past its image ({which}, +0x{off:X})")
            img = self.images.setdefault(which, bytearray(size))
            img[off:off + len(b) - 8] = b[8:]
        elif kind in EVENTS:
            # the images come before the events: the units are this iteration's
            if kind == RK_EV_CREATE:
                self.yard_rule(kind, b)
            if kind == RK_EV_SPRAY and self.sprays_on():
                self.sprays.spray(self.tick, b, self.unit_of)
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
        if self.yheld:
            raise Unreadable(f"a yard of slot {min(self.yheld)}, not live, whose create does not "
                             f"follow in its iteration")

    def pov_differences(self, other):
        """Lines naming each seat whose last two samples, with their GameTimes, or whose selection
        differ between the two worlds; a seat that never had a selection holds none."""
        out = []
        for p in range(REC_PLAYERS):
            a, b = [t for t, _ in self.pov[p]], [t for t, _ in other.pov[p]]
            if self.pov[p] != other.pov[p]:
                out.append(f"player {p}'s samples: recording at GameTime {a}, dump at {b}"
                           + (", their fields differ" if a == b else ""))
            a, b = self.sel[p] or [], other.sel[p] or []
            if a != b:
                out.append(f"player {p}'s selection: {len(a)} units in the recording, {len(b)} "
                           f"in the dump" + (", their slots or births differ"
                                             if len(a) == len(b) else ""))
        return out

    @staticmethod
    def _delta(img, bits, dwords, vals):
        """img's dwords whose bit is set in `bits`, in order, from `vals`."""
        runs, n = _delta_runs(bytes(bits), dwords)
        if len(vals) < n:
            raise Unreadable(f"a delta's {len(vals)} bytes of values, for {n // 4} dwords")
        for at, end, k in runs:
            img[at:end] = vals[k:k + end - at]

    def differences(self, other, names, other_names=None, limit=20, gnames=None, sight=False,
                    pov=False):
        """Lines naming each difference, the first `limit` of them, and their count. `names` are
        this world's tables and `other_names` the other's, when another build numbered them;
        `gnames` the globals image's dwords by name, when the list is known (globals_list); with
        `sight`, the players' sight counters too, which only dumps carry; with `pov`, the
        perspective, which only a recording's stream and snapshots carry."""
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
        for line in self.pov_differences(other) if pov else ():
            note(line)
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


def snapshot_world(payload, slots, tables, dims, base=None, header=None, wbase=None):
    """A snapshot or a dump builds the world from nothing, its seats and viewer included; a
    snapshot's cells over its segment's base grid and its wreck records over its wreck base, a
    dump's over nothing (it carries every cell and every record that is not zero). With the
    recording's `header`, its perspective is checked against it as the stream's is."""
    w = World(slots, tables, dims=dims)
    w.header = header
    w.snap = True
    if base is not None:
        w.grid[:] = base
    if wbase is not None:
        w.pool[:] = wbase
    for kind, b in records(payload):
        w.apply(kind, b)
    w.whole()
    w.snap = False
    return w


def timeline_states(payload):
    """The latest RB_TIMELINES table: validated 16-byte rows, matching recmeta_parse."""
    if not payload or len(payload) % 16 or len(payload) > 65 * 16:
        raise Unreadable("invalid timeline metadata length")
    out = {}
    for at in range(0, len(payload), 16):
        tid, state, used = struct.unpack_from("<IB3xQ", payload, at)
        if tid == 0xFFFFFFFF or state not in (1, 2) or any(payload[at + 5:at + 8]) or tid in out:
            raise Unreadable("invalid timeline metadata row")
        out[tid] = (state, used)
    return out


def fork_prediction(segs, selected=0, at=None):
    """The LRU fork ID a new fork would delete, or None; tie broken by file order."""
    ch = chain(segs, selected)
    keep = segs
    if len(segs) > 1 and not any(b[0] in (RB_STREAM, RB_END) for b in segs[-1]["blocks"]):
        keep = segs[:-1]
    if len(keep) >= 64:
        raise Unreadable("the recording holds 64 timelines, the most a file may")
    if segs[0]["max_id"] >= 0xFFFFFFFE:
        raise Unreadable("timeline ID exhausted")
    if at is not None:
        if ch[0]["first"] is None or ch[-1]["last"] is None or not ch[0]["first"] <= at <= ch[-1]["last"]:
            raise Unreadable("takeover GameTime is outside the timeline")
        # a takeover at a timeline's first iteration would append after its parent's iteration,
        # or after none (tagpu_replay.c, takeover_point)
        for seg in ch:
            if at == seg["first"]:
                raise Unreadable(f"GameTime {at} is timeline {seg['id']}'s first iteration, "
                                 f"which no takeover starts at; the first GameTime one can is "
                                 f"{at + 1}")
    live = [s for s in keep if s["id"] and s["state"] == 1]
    if len(live) < 3:
        return None
    if len(live) > 3:
        raise Unreadable("the live fork set cannot be bounded to 3")
    candidates = [s for s in live if s["id"] != selected]
    if not candidates:
        raise Unreadable("no fork can be evicted")
    return min(candidates, key=lambda s: s["last_used"])["id"]


def segments(bl, offsets=None):
    """The file's segments, as the DLL's scan finds them (tagpu_recreader.c, segs_scan): a new one
    at each RB_HEADER that follows an RB_END, to the first block that is neither -- an end that no
    header follows ends the file there. Each a dict: id, parent and fork_at (RT_TIMELINE; 0 for the
    first), its header, its blocks (its header block first), first and last (its stream's, None
    for none), end_reason (None without an end block) and, with `offsets`, its bytes [start, end).
    Refused by name: more than RECR_TIMELINES segments, a fork that names no timeline, an id used
    twice, or a parent that is not an earlier segment with a lower id."""
    segs, after_end, states, covered = [], True, {}, set()
    for i, blk in enumerate(bl):
        kind, first, last, payload = blk
        if kind == RB_TIMELINES:
            if not segs:
                raise Unreadable("timeline metadata before any header")
            states = timeline_states(payload)
            covered = {s["id"] for s in segs}
            continue
        if after_end:
            if kind != RB_HEADER:
                break
            h = header(payload)
            if segs and "timeline" not in h:
                raise Unreadable(f"the segment at block {i} names no timeline")
            tid, parent, fork_at = h["timeline"] if segs else (0, 0, 0)
            if len(segs) == RECR_TIMELINES:
                raise Unreadable(f"the file holds more than {RECR_TIMELINES} timelines")
            seg = {"id": tid, "parent": parent, "fork_at": fork_at, "header": h, "blocks": [],
                   "first": None, "last": None, "end_reason": None,
                   "range": list(offsets[i]) if offsets else None}
            segs.append(seg)
            contributions = History(h) if "multiplayer" in h else None
            after_end = False
        elif kind == RB_HEADER:
            raise Unreadable(f"a header block (block {i}) inside timeline {seg['id']}")
        seg["blocks"].append(blk)
        if offsets:
            seg["range"][1] = offsets[i][1]
        if kind == RB_STREAM:
            if seg["first"] is None:
                seg["first"] = first
            seg["last"] = last
        elif kind == RB_CONTRIBUTIONS:
            if contributions is None:
                raise Unreadable("contribution block without network roster")
            try:
                contributions.block(payload)
            except ValueError as error:
                raise Unreadable(str(error)) from error
        elif kind == RB_END:
            footers = [b for tag, b in entries(payload) if tag == RT_MP_END]
            if contributions is not None:
                if len(footers) != 1:
                    raise Unreadable("missing or duplicate contribution end proof")
                try:
                    contributions.footer(footers[0])
                except ValueError as error:
                    raise Unreadable(str(error)) from error
            elif footers:
                raise Unreadable("contribution end proof without network roster")
            seg["end_reason"] = end(payload).get(RT_END_REASON)
            after_end = True
    seen = set()
    for k, seg in enumerate(segs):
        if seg["id"] in seen:
            raise Unreadable(f"timeline {seg['id']} twice in the file")
        if k and (seg["parent"] not in seen or seg["parent"] >= seg["id"]):
            raise Unreadable(f"timeline {seg['id']} names parent {seg['parent']}, which is not an "
                             f"earlier timeline")
        if seg["id"] == 0xFFFFFFFF:
            raise Unreadable("timeline ID exhausted")
        seen.add(seg["id"])
        seg["state"], seg["last_used"] = states.get(seg["id"],
                                                   (1, seg["header"].get("wallclock", 0)))
    for tid, (state, _) in states.items():
        if state == 1 and tid not in seen:
            raise Unreadable(f"live timeline {tid} has no segment")
    if covered - set(states):
        raise Unreadable("timeline missing from its metadata table")
    maximum = max(seen | set(states), default=0)
    for seg in segs:
        seg["max_id"] = maximum
    return segs


def shape_differs(a, b):
    """The first field in which a segment's header `a` differs from the first header `b`, or None:
    the reader's bound that keeps the world sized from the first header for every segment, each
    rule the DLL's reader's (tagpu_recreader.c, shape_differs)."""
    if "multiplayer" in a and "multiplayer" in b and (a["multiplayer"] != b["multiplayer"] or a["mp_seats"] != b["mp_seats"]):
        return "multiplayer source/seat mapping"
    if "multiplayer" in a and "multiplayer" not in b:
        return "multiplayer roster"
    if "multiplayer" not in a and "multiplayer" not in b and holds_cells(a) != holds_cells(b):
        return "held cells"
    for key in ("slots", "units_each", "unitdefs", "weapons", "features", "projectiles", "layout",
                "globals", "settings_map"):
        if a.get(key) != b.get(key):
            return key
    if [p["index"] for p in a["players"]] != [p["index"] for p in b["players"]]:
        return "seats"
    if a["tables"] != b["tables"]:
        return "declared tables"
    return None


def chain(segs, n):
    """Timeline n and its ancestors, root first, each fork inside its parent's iterations and
    every segment's header the first's in shape; Unreadable naming the pair or the field."""
    by = {s["id"]: s for s in segs}
    if n not in by:
        raise Unreadable(f"the recording has no timeline {n}")
    if by[n].get("state", 1) == 2:
        raise Unreadable(f"timeline {n} is deleted")
    out = [by[n]]
    while out[-1]["id"]:
        out.append(by[out[-1]["parent"]])
    out.reverse()
    for p, c in zip(out, out[1:]):
        if (p["first"] is None or c["first"] is None or not p["first"] < c["fork_at"]
                or c["fork_at"] - 1 > p["last"] or c["first"] != c["fork_at"]):
            raise Unreadable(f"timeline {c['id']}, from GameTime {c['first']}, forks from "
                             f"timeline {p['id']} at GameTime {c['fork_at']}, outside its GameTime "
                             f"{p['first']} to {p['last']}")
        f = shape_differs(c["header"], out[0]["header"])
        if f:
            raise Unreadable(f"timeline {c['id']} differs from the recording in its {f}")
    return out


def exact_blocks(bl, offsets):
    """The blocks with each stream block's records replaced by the exact records of the RB_EXACT
    that follows it (the test lever tagpu_rec.exact), and the RB_EXACT blocks dropped; Unreadable
    for a stream block with no exact block after it."""
    out, offs, k = [], [], 0
    while k < len(bl):
        kind, first, last, payload = bl[k]
        if kind == RB_EXACT:
            raise Unreadable(f"an exact block of GameTime {first} to {last} after no stream block")
        if kind == RB_STREAM:
            ex = bl[k + 1] if k + 1 < len(bl) else None
            if ex is None or ex[0] != RB_EXACT or ex[1:3] != (first, last):
                raise Unreadable(f"the stream block of GameTime {first} to {last} has no exact "
                                 f"stream after it: recorded without tagpu_rec.exact")
            out.append((RB_STREAM, first, last, ex[3]))
            offs.append((offsets[k][0], offsets[k + 1][1]))
            k += 2
            continue
        out.append(bl[k])
        offs.append(offsets[k])
        k += 1
    return out, offs


def load(path, sizes=None, timeline=0, exact=False):
    """(the first header, the chain's blocks, why the reading stopped). The blocks of timeline 0
    are the first segment's alone, as they were written, whatever follows; timeline N's are each
    chain segment's to the GameTime before its child's fork_at, the child's header replaced by a
    marker (RB_BOUNDARY, fork_at, fork_at, the child's header), then the next. `stop` is the
    file's when the chain's last segment is the file's last, else None. With `exact`, the stream
    is the exact one (exact_blocks), and `sizes` is not taken."""
    offsets = []
    bl, stop = blocks(path, None if exact else sizes, offsets)
    if exact:
        bl, offsets = exact_blocks(bl, offsets)
    if not bl or bl[0][0] != RB_HEADER:
        raise Unreadable(f"{path}: no header block" + (f" ({stop})" if stop else ""))
    segs = segments(bl, offsets)
    ch = chain(segs, timeline)
    if ch[-1] is not segs[-1]:
        stop = None
    if len(ch) == 1:
        if sizes is not None:
            del sizes[len(ch[0]["blocks"]):]
        return segs[0]["header"], ch[0]["blocks"], stop
    out = []
    for i, seg in enumerate(ch):
        cut = ch[i + 1]["fork_at"] - 1 if i + 1 < len(ch) else None
        if i:
            out.append((RB_BOUNDARY, seg["fork_at"], seg["fork_at"], seg["header"]))
        for blk in seg["blocks"][1 if i else 0:]:
            if cut is None or blk[0] == RB_HEADER or blk[1] <= cut:
                out.append(blk)
    return segs[0]["header"], out, stop


def stream_to(h, bl, want=None, on_snapshot=None, on_record=None):
    """The world the stream builds, to the end of iteration `want` (or of the blocks). Calls
    on_snapshot(world, snapshot payload) at each snapshot block on the way, and
    on_record(world, kind, body) after each record is applied -- and with kind None before a TICK
    is, while the world is still the iteration's before it. The world's `base` and `wbase` are its
    grid and its wreck pool at the end of its segment's first iteration, which a snapshot's cells
    and wreck records differ from: the DLL's writer and reader take theirs at that moment too
    (tagpu_recwriter.c, iteration_done; tagpu_recreader.c, tick_take), each from the pool that
    segment's first iteration built from zero.
    Across a chain (load's RB_BOUNDARY markers) each segment is read to the GameTime before the
    next fork_at, its snapshots past that unread, and at the marker a fresh world, the child's
    header's seats and viewer, takes the child's first iteration whole (the reader's invariant,
    tagpu_recreader.c); on_record(world, RB_BOUNDARY, None) says so first, with the parent's.
    An iteration's implied sprays (World.iteration_end) come to on_record as RK_EV_SPRAY after its
    last record, and a snapshot block passed on the way empties the spray sessions' table, as the
    DLL's sequential reader does."""
    w = World(h["slots"], h["tables"], h)
    marks = [blk[1] for blk in bl if blk[0] == RB_BOUNDARY]
    cut = marks.pop(0) - 1 if marks else None
    skip = False

    def ended():
        for rk, b in w.iteration_end():
            if on_record:
                on_record(w, rk, b)
    for kind, first, last, payload in bl[1:]:
        if kind == RB_BOUNDARY:
            w.whole(True)
            if want is not None and w.tick is not None and w.tick >= want:
                return w
            if on_record:
                on_record(w, RB_BOUNDARY, None)
            w = World(h["slots"], h["tables"], payload)
            cut = marks.pop(0) - 1 if marks else None
            skip = False
            continue
        if skip:
            continue
        if kind == RB_SNAPSHOT and (cut is None or last <= cut):
            if on_snapshot:
                on_snapshot(w, payload)
            w.sprays.empty()
        if kind != RB_STREAM:
            continue
        for rk, b in records(payload):
            if rk == RK_TICK:
                ended()
                if cut is not None and struct.unpack_from("<I", b)[0] > cut:
                    skip = True
                    break
                if w.tick is not None and w.base is None:
                    w.whole(True)
                    w.base = bytes(w.grid)
                    w.wbase = bytes(w.pool)
                if want is not None and w.tick is not None and w.tick >= want:
                    w.whole(True)
                    return w
                if on_record:
                    on_record(w, None, b)
            w.apply(rk, b)
            if on_record:
                on_record(w, rk, b)
        ended()
        w.whole(True)           # a stream block ends where an iteration does
    return w


# ---- the events against the images -----------------------------------------------------------

class EventCheck:
    """Each iteration's records are its images, then the events of the step that follows them
    (tagpu_recfmt.h): the images end at the iteration's first event, as the readers have them
    (tagpu_recreader.c, apply), and World.apply refuses cells past it that are not the tick's
    feature call's, so the world summarised here is every reader's. Between the images of two
    iterations, a unit that appears must have its create, one that disappears its death, one whose
    state byte +0x10E changed a state event with the new byte, one whose build fraction +0x104
    reached 0.0 its build done, and, with `yards` (a skirmish's: its recorder writes them; None,
    the default, takes the rule per segment from the world's header, since a multiplayer file's
    takeover timeline is a skirmish's, whose recorder writes them; a bool fixes it), one
    whose yard, +0x10F bit 2, changed a yard event whose last one for it is the new yard, since
    YARD_OPEN 0x47DAC0 is that bit's one writer (0x47DAF1) -- unless the slot died and was made
    again in the step, which the create and the death both name; and a unit made in the step has
    the yard of its last yard event, its create's or a later one, or with none the closed yard its
    create's init leaves. Within an iteration's events a script slice and an owner's
    query name a unit live at its point -- in the iteration's images, or created earlier in its
    events and not dead since -- and a script slice a slot at most once (tagpu_recfmt.h,
    RK_EV_SLICE, RK_EV_QUERY)."""

    def __init__(self, limit=20, yards=None):
        self.fixed = yards
        self.yards = bool(yards)
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

    # a unit's (type ID, build fraction, state byte, +0x10F), in the image's order
    _FIELDS = struct.Struct(f"<{U_TYPE}x4s{U_BUILT - U_TYPE - 4}x4s{U_STATEBYTE - U_BUILT - 4}xBB")

    @staticmethod
    def summary(w):
        fields = EventCheck._FIELDS.unpack_from
        return {slot: fields(img) for slot, img in w.units.items()}

    def images_done(self, w):
        self.summary_now = self.summary(w)
        if self.pending is not None:
            self.transition(self.pending[0], self.pending[1], self.summary_now, w.tick)
            self.pending = None
        self.in_events = True

    def on_record(self, w, kind, b):
        if self.fixed is None and w.header is not None:
            self.yards = w.header.get("mode") != 3
        if kind is None:
            self.before_tick(w)
            return
        if kind == RB_BOUNDARY:
            # the child's first iteration replaces the world: no step leads from the parent's
            self.finish(w)
            self.open, self.in_events, self.events, self.pending = False, False, [], None
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
        elif self.open:
            # the last iteration's events lead to no images: its slices are checked here
            self.slices(self.summary_now, self.events, w.tick)

    def slices(self, was, events, tick):
        live, sliced = set(was), set()
        for kind, b in events:
            if kind in (RK_EV_CREATE, RK_EV_NET_CREATE):
                live.add(struct.unpack_from("<I", b)[0] - 1)
            elif kind == RK_EV_DEATH:
                live.discard(struct.unpack_from("<I", b)[0] - 1)
            elif kind == RK_EV_SLICE:
                slot = struct.unpack_from("<I", b)[0] - 1
                if slot not in live:
                    self.note(f"the events of GameTime {tick}: a script slice of slot {slot}, "
                              f"which holds no live unit there")
                if slot in sliced:
                    self.note(f"the events of GameTime {tick}: slot {slot}'s script slice twice")
                sliced.add(slot)
            elif kind == RK_EV_QUERY:
                slot = struct.unpack_from("<I", b)[0] - 1
                if slot not in live:
                    self.note(f"the events of GameTime {tick}: an owner's query of slot {slot}, "
                              f"which holds no live unit there")

    def transition(self, was, events, now, tick):
        self.checked += 1
        self.slices(was, events, tick - 1)
        made, died, state, built, yard = set(), set(), {}, set(), {}
        for kind, b in events:
            if kind in (RK_EV_CREATE, RK_EV_NET_CREATE):
                made.add(struct.unpack_from("<I", b)[0] - 1)
            elif kind == RK_EV_DEATH:
                died.add(struct.unpack_from("<I", b)[0] - 1)
            elif kind == RK_EV_STATE:
                state[struct.unpack_from("<I", b)[0] - 1] = b[4]
            elif kind == RK_EV_BUILT:
                built.add(struct.unpack_from("<I", b, 4)[0] - 1)
            elif kind == RK_EV_YARD:
                yard[struct.unpack_from("<I", b)[0] - 1] = b[4]
        at = f"the step to GameTime {tick}"
        # a slot whose fields stayed and that no create or death names has nothing to explain
        changed = {slot for slot, _ in was.items() ^ now.items()}
        for slot in sorted(changed | ((made | died) & (was.keys() | now.keys()))):
            a, b = was.get(slot), now.get(slot)
            if a is None:
                if slot not in made:
                    self.note(f"{at}: unit {slot} appears with no create")
                elif self.yards and slot not in died and (b[3] >> 2 & 1) != yard.get(slot, 0):
                    # the create's init closes the yard (0x485B3E..0x485B52), and only YARD_OPEN
                    # opens it: in the create's Create slice or after it, an event each
                    self.note(f"{at}: unit {slot} appears with its yard {b[3] >> 2 & 1} and "
                              + (f"its last yard event at {yard[slot]}" if slot in yard
                                 else "no yard event"))
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
            if a[2] != b[2] and state.get(slot) != b[2]:
                self.note(f"{at}: unit {slot}'s +0x10E went {a[2]:#x} -> {b[2]:#x} with "
                          + (f"its last state event at {state[slot]:#x}" if slot in state
                             else "no state event"))
            if a[1] != bytes(4) and b[1] == bytes(4) and slot not in built:
                self.note(f"{at}: unit {slot}'s +0x104 reached 0.0 with no build done")
            ya, yb = a[3] >> 2 & 1, b[3] >> 2 & 1
            if self.yards and ya != yb and yard.get(slot) != yb:
                self.note(f"{at}: unit {slot}'s yard went {ya} -> {yb} with "
                          + (f"its last yard event at {yard[slot]}" if slot in yard
                             else "no yard event"))


# ---- the commands ----------------------------------------------------------------------------

def codec_text(h):
    """The header's unit frame constants in a line, bounded by reccodec.params."""
    c = h.get("codec", {})
    if RT_CODEC not in c or RT_PHYSICS not in c:
        return "none (no RT_CODEC and RT_PHYSICS): every stream block is records"
    gw, gh, wrecks = (h.get("features") or (0, 0, 0))[:3]
    prm = _codec().params(list(c.items()), h.get("slots") or 0, h.get("projectiles") or 0, gw, gh,
                          wrecks, (h.get("unitdefs") or (0, 0))[0])
    if not prm.usable:
        return (f"off: the grid of {gw} x {gh} cells or the wreck pool of {wrecks} records is past "
                f"the codec's bounds, so every stream block is records")
    return (f"{len(prm.lanes)} lanes, gravity {prm.gravity}, sea level {prm.sea}, sight rule "
            f"{prm.sight}, bucket columns {prm.bcols}, {len(prm.air)} aircraft types, "
            f"{len(prm.types)} unit types")


def started(h):
    """The header's recorded start (RT_WALLCLOCK) as its local time with the offset from UTC,
    "2026-10-23 14:05:09 +02:00"; None when the header has none, and the raw FILETIME and
    minutes when they make no date (an offset of a day or more, or a time past datetime's)."""
    if "wallclock" not in h:
        return None
    ft, off = h["wallclock"], h["utc_offset"]
    try:
        if abs(off) >= 1440:
            raise ValueError(off)
        utc = datetime(1601, 1, 1, tzinfo=timezone.utc) + timedelta(microseconds=ft // 10)
        local = utc.astimezone(timezone(timedelta(minutes=off)))
    except (ValueError, OverflowError):
        return f"FILETIME {ft}, offset {off} min"
    return (f"{local:%Y-%m-%d %H:%M:%S} {'-' if off < 0 else '+'}"
            f"{abs(off) // 60:02d}:{abs(off) % 60:02d}")


def cmd_info(path):
    sizes, flags = [], []
    all_blocks, stop = blocks(path, sizes, flags=flags)
    segs = segments(all_blocks)
    if not segs:
        raise Unreadable("no header block")
    h, bl = segs[0]["header"], segs[0]["blocks"]
    print(f"{path}")
    print(f"  map {h.get('map')!r}, mode {h.get('mode')}, build {h.get('build')}, "
          f"from GameTime {h.get('start')}, started {started(h)}")
    print(f"  {h.get('slots')} unit slots, {h.get('units_each')} units each, "
          f"unit defs {h.get('unitdefs')}, layout {h.get('layout', '')[:12]}")
    o = h.get("options", {})
    print(f"  options: LosType {o.get('lostype')}, commander death {o.get('cmdrdeath')}, "
          f"difficulty {o.get('difficulty')}, fixed starts {o.get('fixed_starts')}, "
          f"{o.get('players')} players, controlled {o.get('controlled')}, viewed {o.get('viewed')}")
    print(f"  settings map {h.get('settings_map')!r}, weapons {h.get('weapons')}")
    print(f"  version {h.get('version')!r}, mod fingerprint "
          f"{'0x%08x' % h['modprint'] if 'modprint' in h else None}, "
          f"{h.get('archives_mounted', len(h.get('archives', [])))} archives mounted: "
          + ", ".join(f"{n} ({s} bytes)" for n, s, _ in h.get("archives", [])))
    for kind, first, last, payload in bl:
        if kind == RB_MINIMAP:
            w, hh, bc, br = struct.unpack_from("<HHHH", payload)
            print(f"  minimap {w} x {hh}, shown {2 * bc} x {2 * br}, "
                  f"{len(payload) - 8} indices{'' if len(payload) - 8 == w * hh else ' (WRONG)'}")
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
    stream = [(s, f) for s, f in zip(sizes, flags) if s[0] == RB_STREAM]
    srec = sum(len(p) for k, a, b, p in all_blocks if k == RB_STREAM)
    sraw = sum(s[3] for s, f in stream)
    spk = sum(s[4] for s, f in stream)
    print(f"  the stream: {srec} bytes of records, {sraw} as blocks hold them, {spk} packed")
    print(f"  the unit frame: {codec_text(h)}")
    nsec = sum(1 for s, f in stream if f & RBF_SECTIONS)
    print(f"  stream blocks: {len(stream)}, {nsec} sectioned "
          f"({sum(1 for s, f in stream if f & RBF_FRAME_RESET)} restarting the frame), "
          f"{len(stream) - nsec} records; {sum(1 for s, f in stream if f & RBF_REFRESH)} end at a "
          f"refresh")
    hold = h.get("hold")
    if hold:
        print(f"  the stream holds {sum(bin(b).count('1') for b in hold['unit'])} bits of a unit "
              f"image and {sum(bin(b).count('1') for b in hold['node'])} of a main list node, "
              f"flags {hold['flags']:#x}, between refreshes every {hold['refresh']} GameTimes "
              f"(table {hold['sha'].hex()[:12]})")
    else:
        print("  the stream holds nothing: no RT_HOLD")
    for (k, first, last, r, pk), f in zip(sizes, flags):
        if k == RB_SNAPSHOT:
            print(f"  snapshot at GameTime {last}: {r} bytes raw, {pk} packed "
                  f"({'chained' if f & RBF_CHAINED else 'a key'})")
    ev, evb = {}, {}
    for kind, first, last, payload in bl:
        if kind != RB_STREAM:
            continue
        for rk, b in records(payload):
            if rk in EVENTS or rk in (RK_ORDERS, RK_CONTROLLER, RK_VIEWER, RK_CELLS, RK_WRECK,
                                      RK_GLOBALS, RK_GLOBALS_DELTA, RK_PROJ_HEAD, RK_PROJ,
                                      RK_PROJ_DELTA, RK_MAPPED, RK_MAPPED_RUNS, RK_POV, RK_CLICK,
                                      RK_SELECT, RK_SPRAY_SESSION):
                ev[rk] = ev.get(rk, 0) + 1
                evb[rk] = evb.get(rk, 0) + 4 + ((len(b) + 3) & ~3)
    names = {**EVENTS, RK_ORDERS: "order list", RK_CONTROLLER: "controller", RK_VIEWER: "viewer",
             RK_CELLS: "run of cells", RK_WRECK: "wreck record", RK_GLOBALS: "globals image",
             RK_GLOBALS_DELTA: "globals delta", RK_PROJ_HEAD: "projectile head",
             RK_PROJ: "projectile record", RK_PROJ_DELTA: "projectile delta",
             RK_MAPPED: "explored map record", RK_MAPPED_RUNS: "explored map runs",
             RK_POV: "camera and cursor sample", RK_CLICK: "click", RK_SELECT: "selection",
             # the stream's own: the sprays its sessions imply are made by the readers, not counted
             RK_EV_SPRAY: "nano spray written", RK_SPRAY_SESSION: "spray session"}
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
        print(f"  the writer's codec: {e.get(RT_CODEC_DECLINED, 'absent')} blocks declined, "
              f"{e.get(RT_CODEC_MISMATCH, 'absent')} mismatched")
        if RT_GAME_END in e:
            print(f"  the game's end flags 0x{e[RT_GAME_END]:02x}")
        for seat, res, cmdr, defeat in e["seats"]:
            print(f"  seat {seat}: {RESULTS.get(res, res)}, commander died at GameTime "
                  f"{cmdr or '-'}, defeated at {defeat or '-'}")
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
    segs, _ = file_segments(path)
    print(f"  timelines: {len(segs)}")
    for seg in segs:
        ends = [b for b in seg["blocks"] if b[0] == RB_END]
        how = (END_REASONS.get(seg["end_reason"], seg["end_reason"]) if ends
               else "INCOMPLETE: no end block")
        print(f"  timeline {seg['id']}" + (f", parent {seg['parent']}, fork_at {seg['fork_at']}"
                                           if seg["id"] else ", the original")
              + f": {'live' if seg['state'] == 1 else 'deleted'}, last_used {seg['last_used']}; "
                f"GameTime {seg['first']} to {seg['last']}, bytes {seg['range'][0]} to "
                f"{seg['range'][1]}; {how}")
        if ends:
            e = end(ends[0][3])
            if RT_GAME_END in e:
                print(f"    the game's end flags 0x{e[RT_GAME_END]:02x}")
            for seat, res, cmdr, defeat in e["seats"]:
                print(f"    seat {seat}: {RESULTS.get(res, res)}, commander died at GameTime "
                      f"{cmdr or '-'}, defeated at {defeat or '-'}")
    return 0


def file_segments(path, decode=False):
    """The file's segments with their byte ranges, and why the reading stopped. Without `decode`
    their stream, snapshot and minimap payloads are None, and zstandard is needed only for a
    multiplayer file's contribution blocks."""
    offsets = []
    bl, stop = blocks(path, None, offsets, decode=decode)
    return segments(bl, offsets), stop


def _segment_at(bl, offsets, timeline, T):
    ch = chain(segments(bl, offsets), timeline)
    for i, s in enumerate(ch):
        end = ch[i + 1]["fork_at"] - 1 if i + 1 < len(ch) else s["last"]
        if s["first"] is not None and s["first"] <= T <= end:
            return s
    raise Unreadable(f"GameTime {T} is outside timeline {timeline}'s chain")


def segment_at(path, timeline, T):
    """The segment of `timeline`'s chain whose part holds T -- each segment's to its child's
    fork_at - 1 -- as tagpu_recreader.c's chain_hold finds it, its header among its keys.
    Unreadable when T is outside the chain."""
    offsets = []
    bl, _ = blocks(path, None, offsets, decode=False)
    return _segment_at(bl, offsets, timeline, T)


def refresh_at(path, timeline, T):
    """The last refresh at or before T in the chain's segment that holds it, as tagpu_recreader.c's
    recr_refresh_at finds it: in the chain's segment whose part holds T, the largest `last` <= T
    of a stream block flagged RBF_REFRESH, or the segment's first iteration. Unreadable when T is
    outside the chain."""
    offsets, flags = [], []
    bl, _ = blocks(path, None, offsets, flags=flags, decode=False)
    fl = {id(b): f for b, f in zip(bl, flags)}
    s = _segment_at(bl, offsets, timeline, T)
    return max([s["first"]] + [b[2] for b in s["blocks"] if b[0] == RB_STREAM and
                               fl[id(b)] & RBF_REFRESH and b[2] <= T])


def takeover_at(path, timeline, T):
    """The GameTime a takeover of `timeline` asked for at T starts from, as tagpu_replay.c's
    takeover_point finds it: T itself where the chain's segment holding T holds nothing
    (holds_nothing), else refresh_at. Unreadable, as takeover_point refuses it, when that start
    is its segment's first iteration, after which no fork can be appended."""
    s = segment_at(path, timeline, T)
    R = T if holds_nothing(s["header"]) else refresh_at(path, timeline, T)
    if R == s["first"]:
        raise Unreadable(f"GameTime {R} is timeline {s['id']}'s first iteration, which no "
                         f"takeover starts at; the first GameTime one can is {R + 1}")
    return R


def seek_start(path, timeline, T, lead):
    """Where the DLL's reader builds a seek to T of `timeline` from, as recr_seek
    (tagpu_recreader.c) chooses it from the file's index: (S, from_snapshot). The last snapshot of
    the chain's part -- each segment's to its child's fork_at - 1 -- with S + lead < T, else the
    first iteration of the chain's first segment whose part reaches T - lead. Unreadable when T is
    outside the chain, as the reader refuses it."""
    ch = chain(file_segments(path)[0], timeline)

    def cover_end(i):
        return ch[i + 1]["fork_at"] - 1 if i + 1 < len(ch) else ch[i]["last"]
    hold = next((i for i, s in enumerate(ch) if s["first"] is not None and
                 s["first"] <= T <= cover_end(i)), None)
    if hold is None:
        raise Unreadable(f"GameTime {T} is outside timeline {timeline}'s chain")
    snap = None
    for i in range(hold + 1):
        for kind, _first, last, _payload in ch[i]["blocks"]:
            if kind == RB_SNAPSHOT and last <= cover_end(i) and last + lead < T:
                snap = last
    if snap is not None:
        return snap, True
    lo = T - lead if T > lead else 0
    i = 0
    while i < hold and cover_end(i) < lo:
        i += 1
    return ch[i]["first"], False


def seek_blocks(path, timeline, T, lead, base):
    """The blocks the DLL's reader decodes for a seek to T of `timeline`, as recr_seek
    (tagpu_recreader.c) reads them, from the file's own blocks: (count, their bytes in the file,
    their bytes unpacked, base), each size a block header's. From the snapshot seek_start chooses
    -- after the first stream block of that snapshot's segment when `base`, the reader not holding
    the segment's base -- or from the first iteration seek_start chooses; then, in each segment of
    the chain up to T, every stream block holding an iteration it reads: a block is decoded whole,
    and one ends where an iteration does (tagpu_recwriter.c, iteration_done), so the reader opens
    no block past the last iteration it reads. The contribution, exact-stream and timeline blocks
    it passes, and the snapshots between its iterations, are skipped unread. `base` None is a
    reader that has read its chain's first iteration and nothing since, as a playing in a fresh
    process holds after its first step: it holds the root's base, so it reads a base only for a
    snapshot in a later segment of the chain.
    THE CHAIN: a chained snapshot (RBF_CHAINED) is decoded with every snapshot of its segment
    before it back to its key, each a block of the count (snapshot_chain); more than CHAIN_MAX
    links, or a chain with no key in its segment, is Unreadable, as the reader refuses it.
    THE RUN: a sectioned block is a chunk of its run's one zstd frame and counts as its block, its
    header's packed and raw -- the chunk and the container it unpacks to, not the run's window.
    The reader decodes a chunk only after its run's chunks before it (sections_ready): its codec
    is invalid where a seek starts, at a crossing and after a records block, and only an
    RBF_FRAME_RESET block starts it again. A seek starts only where a run does -- the stream
    after a snapshot, a segment's first iteration -- so it decodes each run it reads from its
    start; a file where it would not is Unreadable, as the reader refuses it."""
    sizes, offsets, flags = [], [], []
    bl, _ = blocks(path, sizes, offsets, flags=flags, decode=False)
    size = {id(b): sz for b, sz in zip(bl, sizes)}
    flag = {id(b): f for b, f in zip(bl, flags)}
    ch = chain(segments(bl, offsets), timeline)

    def cover_end(i):
        return ch[i + 1]["fork_at"] - 1 if i + 1 < len(ch) else ch[i]["last"]
    hold = next((i for i, s in enumerate(ch) if s["first"] is not None and
                 s["first"] <= T <= cover_end(i)), None)
    if hold is None:
        raise Unreadable(f"GameTime {T} is outside timeline {timeline}'s chain")
    snap = frm = None
    for i in range(hold + 1):
        for b in ch[i]["blocks"]:
            if b[0] == RB_SNAPSHOT and b[2] <= cover_end(i) and b[2] + lead < T:
                snap, frm = b, i
    read = []

    def runs(stream):
        """`stream`, the blocks one span of the seek decodes in order, after the codec was made
        invalid: each sectioned block must restart the frame or continue one it decoded"""
        valid = False
        for b in stream:
            if not flag[id(b)] & RBF_SECTIONS:
                valid = False
            elif flag[id(b)] & RBF_FRAME_RESET:
                valid = True
            elif not valid:
                raise Unreadable(f"the stream block of GameTime {b[1]} to {b[2]} continues a run "
                                 f"the seek to {T} does not start")
        read.extend(stream)

    if base is None:
        base = snap is not None and frm > 0
    if snap is not None:
        if base:
            runs([next(b for b in ch[frm]["blocks"] if b[0] == RB_STREAM)])
        snaps = [b for b in ch[frm]["blocks"] if b[0] == RB_SNAPSHOT]
        k = next(j for j, b in enumerate(snaps) if b is snap)
        links = [snap]
        while flag[id(links[0])] & RBF_CHAINED:
            if k == 0:
                raise Unreadable(f"the chained snapshot of GameTime {links[0][2]} has no snapshot "
                                 f"before it in its segment")
            if len(links) == _codec().CHAIN_MAX + 1:
                raise Unreadable(f"the snapshot of GameTime {snap[2]} chains past "
                                 f"{_codec().CHAIN_MAX} snapshots to its key")
            k -= 1
            links.insert(0, snaps[k])
        read += links
        start = snap[2] + 1
    else:
        lo = T - lead if T > lead else 0
        frm = 0
        while frm < hold and cover_end(frm) < lo:
            frm += 1
        start = ch[frm]["first"]
    for i in range(frm, hold + 1):
        end = T if i == hold else cover_end(i)
        runs([b for b in ch[i]["blocks"] if b[0] == RB_STREAM and b[1] <= end and b[2] >= start])
        if i < hold:
            start = ch[i + 1]["fork_at"]
    return (len(read), sum(size[id(b)][4] for b in read), sum(size[id(b)][3] for b in read),
            int(bool(base) and snap is not None))


def cmd_segments(path):
    """One line per segment, then the SHA-256 of the original's bytes: what a fork's writer must
    leave as it was."""
    import hashlib
    segs, stop = file_segments(path)
    for seg in segs:
        print(f"timeline {seg['id']}: bytes {seg['range'][0]} to {seg['range'][1]}")
    if stop:
        print(f"read to its last complete block: {stop}")
    with open(path, "rb") as f:
        data = f.read(segs[0]["range"][1])
    print(f"original sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


def cmd_check(path, timeline=0, exact=False):
    h, bl, stop = load(path, timeline=timeline, exact=exact)
    bad, seen = 0, 0
    gnames = known_names(h)

    def at_snapshot(w, payload):
        nonlocal bad, seen
        seen += 1
        s = snapshot_world(payload, h["slots"], h["tables"], feature_dims(h), w.base, w.header,
                           w.wbase)
        lines, n = w.differences(s, h["tables"], gnames=gnames, pov=True)
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


# ---- the lossy stream against the exact one ---------------------------------------------------

def iterations(payload):
    """(GameTime, its records' bytes) for each iteration of a stream block's records: an
    RK_TICK and the records up to the next. Unreadable for a block that begins otherwise."""
    head = _REC_HEAD.unpack_from
    at, n, start, tick = 0, len(payload), None, None
    while at + 4 <= n:
        kind, ln = head(payload, at)
        if kind == RK_TICK:
            if ln != 4:
                raise Unreadable(f"an RK_TICK of {ln} bytes")
            if start is not None:
                yield tick, payload[start:at]
            start, tick = at, struct.unpack_from("<I", payload, at + 4)[0]
        elif start is None:
            raise Unreadable("a stream block that does not begin with an RK_TICK")
        at += (4 + ln + 3) & ~3
    if at != n:
        raise Unreadable("a record cut short at the end of a stream block")
    if start is not None:
        yield tick, payload[start:n]


# a cell's bytes a stream that holds its cells (RECL_CELLS) carries at a value it wrote before:
# the unit slots +0x00 and +0x02 and the yard bit, +0x0C bit 1 (tagpu_reclossy.c)
CELL_HELD = bytes([0xFF] * 4 + [0] * 8 + [0x02])


class Held:
    """The units, their liveness and their order lists as one stream builds them -- the lossy
    stream's or the exact one's -- with the slots and lists each iteration touched, the bytes of
    the iteration's other records in order but its nano sprays and spray sessions, those bodies
    in order apart, and each touched thing as it was before. With `grid`, (columns, rows) of a
    segment that holds its cells, also the grid from the cells its images carry, the cells each
    iteration's images touched as they were before (prev_cells), and the tick's input runs apart
    from the other records (inputs, their bodies in order)."""

    def __init__(self, slots, grid=None):
        import numpy as np
        self.np = np
        self.units = np.zeros((slots, REC_UNIT_DWORDS), np.uint32)
        self.live = np.zeros(slots, bool)
        self.lists = {}
        self.slots = slots
        self.gw, self.gh = grid or (0, 0)
        self.grid = np.zeros((self.gw * self.gh, REC_CELL_BYTES), np.uint8) if grid else None
        self.prev_cells, self.inputs = {}, []

    def _cells(self, recs, at, ln, evented):
        x, z, n, zero = struct.unpack_from("<HHHH", recs, at + 4)
        if not n or n > REC_CELLS_MAX or zero or ln != 8 + n * REC_CELL_BYTES or \
                x + n > self.gw or z >= self.gh:
            raise Unreadable(f"a run of cells past the {self.gw} x {self.gh} grid")
        if evented:
            self.inputs.append(bytes(recs[at + 4:at + 4 + ln]))
            return
        i = z * self.gw + x
        for c in range(i, i + n):
            if c not in self.prev_cells:
                self.prev_cells[c] = self.grid[c].copy()
        self.grid[i:i + n] = self.np.frombuffer(recs, self.np.uint8, n * REC_CELL_BYTES,
                                                at + 12).reshape(n, REC_CELL_BYTES)

    def iteration(self, recs):
        np = self.np
        head = _REC_HEAD.unpack_from
        prev, prev_lists, other, sprays, sessions = {}, {}, [], [], []
        pend = None
        evented = False
        self.prev_cells, self.inputs = {}, []
        at, n = 0, len(recs)
        while at < n:
            kind, ln = head(recs, at)
            size = (4 + ln + 3) & ~3
            if kind in EVENTS:
                evented = True
            if kind == RK_CELLS and self.grid is not None:
                if pend is not None:
                    raise Unreadable(f"slot {pend[0]}'s list unfinished")
                self._cells(recs, at, ln, evented)
            elif kind in (RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE, RK_ORDERS):
                if ln < 2:
                    raise Unreadable(f"a {kind} record of {ln} bytes")
                s = _U16(recs, at + 4)[0]
                if not 1 <= s < self.slots:
                    raise Unreadable(f"slot {s} of {self.slots}")
                if s not in prev:
                    prev[s] = (bool(self.live[s]), self.units[s].copy())
                if kind == RK_UNIT:
                    if ln != 4 + REC_UNIT_IMAGE:
                        raise Unreadable(f"an RK_UNIT of {ln} bytes")
                    self.units[s] = np.frombuffer(recs, "<u4", REC_UNIT_DWORDS, at + 8)
                    self.live[s] = True
                elif kind == RK_UNIT_GONE:
                    self.live[s] = False
                    for k in (0, 1):
                        prev_lists.setdefault((s, k), self.lists.get((s, k), b""))
                        self.lists.pop((s, k), None)
                elif kind == RK_UNIT_DELTA:
                    if not self.live[s] or ln < REC_UNIT_DELTA_HEAD:
                        raise Unreadable(f"a delta of slot {s}, not live, or short")
                    bits = int.from_bytes(recs[at + 6:at + 6 + REC_UNIT_BITS], "little")
                    sel = [d for d in range(REC_UNIT_DWORDS) if bits >> d & 1]
                    if bits >> REC_UNIT_DWORDS or ln != REC_UNIT_DELTA_HEAD + 4 * len(sel):
                        raise Unreadable(f"a malformed delta of slot {s}")
                    self.units[s, sel] = np.frombuffer(recs, "<u4", len(sel),
                                                       at + 4 + REC_UNIT_DELTA_HEAD)
                else:
                    lst, zero, total, first = struct.unpack_from("<BBHH", recs, at + 6)
                    body = bytes(recs[at + 12:at + 4 + ln])
                    if lst > 1 or zero or (ln - 8) % REC_ORDER_IMAGE or not self.live[s]:
                        raise Unreadable(f"a malformed order list of slot {s}")
                    if first == 0:
                        if pend is not None:
                            raise Unreadable(f"slot {pend[0]}'s list unfinished")
                        pend = [s, lst, total, 0, []]
                    elif pend is None or pend[:4] != [s, lst, total, first]:
                        raise Unreadable(f"an order list of slot {s} out of order")
                    pend[4].append(body)
                    pend[3] = first + len(body) // REC_ORDER_IMAGE
                    if pend[3] > total:
                        raise Unreadable(f"an order list of slot {s} past its length")
                    if pend[3] == total:
                        prev_lists.setdefault((s, lst), self.lists.get((s, lst), b""))
                        img = b"".join(pend[4])
                        if img:
                            self.lists[(s, lst)] = img
                        else:
                            self.lists.pop((s, lst), None)
                        pend = None
            else:
                if pend is not None:
                    raise Unreadable(f"slot {pend[0]}'s list unfinished")
                if kind == RK_EV_SPRAY:
                    sprays.append(bytes(recs[at + 4:at + 4 + ln]))
                elif kind == RK_SPRAY_SESSION:
                    sessions.append(bytes(recs[at + 4:at + 4 + ln]))
                else:
                    other.append(recs[at:at + size])
            at += size
        if pend is not None:
            raise Unreadable(f"slot {pend[0]}'s list unfinished")
        return prev, prev_lists, b"".join(other), sprays, sessions

    def unit_of(self, s):
        """(live, position +0x6A) of slot s, as a spray session reads it"""
        return bool(self.live[s]), struct.unpack_from("<iii", self.units[s].tobytes(), 0x6A)


def is_refresh(n, t, prev, interval):
    """Whether a segment's iteration at GameTime t is a refresh, n its ordinal in the segment from
    1 and prev the GameTime of the segment's last refresh, as the writer decides it
    (tagpu_reclossy.h, reclossy_is_refresh): the first two, and then the first in each interval
    of `interval` GameTimes (1 to interval, interval + 1 to 2 * interval, ...) other than the last
    refresh's -- on consecutive GameTimes those 1 modulo the interval."""
    return n <= 2 or (interval > 0 and (t + interval - 1) // interval !=
                      (prev + interval - 1) // interval)


def lossy_chain(path, timeline):
    """(the chain's segments, each block's flags by the block, why the reading stopped)."""
    offsets, flags = [], []
    bl, stop = blocks(path, None, offsets, flags=flags)
    fl = {id(b): f for b, f in zip(bl, flags)}
    if not bl or bl[0][0] != RB_HEADER:
        raise Unreadable(f"{path}: no header block" + (f" ({stop})" if stop else ""))
    return chain(segments(bl, offsets), timeline), fl, stop


def cmd_lossy_check(path, timeline=0, to=None):
    """The lossy stream against the exact one (RB_EXACT, the test lever tagpu_rec.exact), an
    iteration at a time, as tagpu_reclossy.c's invariants state them:
      (a) every shown byte exact: each unit's liveness, each live unit's image outside RT_HOLD's
          unit mask, each main order list outside the node mask and each sub list whole; every
          other record -- the players, tables, globals, projectiles, wrecks, cells, explored map,
          perspective, events and corrections -- byte for byte, in order; with RECL_CELLS, each
          cell an image touched outside CELL_HELD, and the tick's input runs those of the exact
          stream that differ from the stream's grid outside CELL_HELD, in order;
      (b) every held byte a value the engine produced: a unit dword's held bits the stream's
          previous value or the exact one, an order list the stream's previous or the exact one,
          a cell's CELL_HELD bits the stream's previous value or the exact one;
      (c) the two worlds equal byte for byte at the end of every RBF_REFRESH block, every
          snapshot after a refresh block, and a refresh block ending at each refresh of the
          writer's grid (is_refresh, on RT_HOLD's interval) and nowhere else, as the takeover's
          rounding reads them -- the grid with them, with RECL_CELLS;
      (d) with RT_HOLD's RECL_SPRAYS, each iteration's nano sprays -- the stream's and the ones
          its spray sessions imply (Sprays, over the stream's units) -- the exact stream's as a
          multiset of (ID, body), and each ID's in the exact stream's order: the order across
          IDs inside one iteration is the one thing not kept, a loosening the owner approved
          (tagpu_reclossy.c, THE SPRAY SESSIONS); every other event is in (a);
      (e) RT_HOLD in every skirmish segment's header, and the exact stream's blocks and
          iterations the stream's.
    """
    import numpy as np
    ch, fl, stop = lossy_chain(path, timeline)
    lines, bad = [], 0
    n_iter = n_refresh = n_snap = n_written = n_implied = 0
    last = None

    def fail(msg):
        nonlocal bad
        bad += 1
        if len(lines) < 20:
            lines.append(msg)

    for i, seg in enumerate(ch):
        h = seg["header"]
        cut = ch[i + 1]["fork_at"] - 1 if i + 1 < len(ch) else None
        if to is not None:
            cut = to if cut is None else min(cut, to)
        hold = h.get("hold")
        if h.get("mode") == 3:
            interval, hu, hn = REC_REFRESH_TICKS, bytes(REC_UNIT_IMAGE), bytes(REC_ORDER_IMAGE)
        elif hold is None:
            fail(f"timeline {seg['id']}: a skirmish header without RT_HOLD")
            break
        else:
            if hold["flags"] & ~(RECL_CELLS | RECL_SPRAYS):
                raise Unreadable(f"RT_HOLD's flags {hold['flags']:#x}: a flag this check does not "
                                 f"know")
            interval, hu, hn = hold["refresh"], hold["unit"], hold["node"]
        sessions_on = bool(hold and hold["flags"] & RECL_SPRAYS)
        H = np.frombuffer(hu, "<u4")
        HN = np.frombuffer(hn, "<u4")
        node_held = HN.any()
        grid = feature_dims(h)[:2] if holds_cells(h) else None
        CH = np.frombuffer(CELL_HELD, np.uint8)
        D, W = Held(h["slots"], grid), Held(h["slots"], grid)
        S = Sprays(h["slots"])
        bl = seg["blocks"]
        n_seg, prev_refresh = 0, 0
        k = 1
        done = False
        prev_stream = None
        while k < len(bl) and not done:
            kind, first, lastb, payload = bl[k]
            if kind == RB_SNAPSHOT:
                if cut is None or lastb <= cut:
                    n_snap += 1
                    S.empty()               # every reader's, where the writer emptied its own
                    if prev_stream is None or prev_stream[1] != lastb or \
                            not fl[id(prev_stream[0])] & RBF_REFRESH:
                        fail(f"the snapshot at GameTime {lastb} does not follow a refresh block")
                k += 1
                continue
            if kind == RB_EXACT:
                fail(f"an exact block of GameTime {first} to {lastb} after no stream block")
                k += 1
                continue
            if kind != RB_STREAM:
                k += 1
                continue
            ex = bl[k + 1] if k + 1 < len(bl) else None
            if ex is None or ex[0] != RB_EXACT or ex[1:3] != (first, lastb):
                raise Unreadable(f"the stream block of GameTime {first} to {lastb} has no exact "
                                 f"stream after it: recorded without tagpu_rec.exact")
            refresh_block = fl[id(bl[k])] & RBF_REFRESH
            prev_stream = (bl[k], lastb)
            its, xits = list(iterations(payload)), list(iterations(ex[3]))
            if len(its) != len(xits):
                fail(f"the block of GameTime {first} to {lastb}: {len(its)} iterations, the "
                     f"exact stream's {len(xits)}")
            pairs = list(zip(its, xits))
            for j, ((t, recs), (tx, xrecs)) in enumerate(pairs):
                if t != tx:
                    fail(f"iteration GameTime {t} against the exact stream's {tx}")
                    done = True
                    break
                if cut is not None and t > cut:
                    done = True
                    break
                n_iter += 1
                last = t
                n_seg += 1
                refresh = is_refresh(n_seg, t, prev_refresh, interval)
                if refresh:
                    prev_refresh = t
                ends = j == len(pairs) - 1
                if refresh != (ends and bool(refresh_block)):
                    fail(f"GameTime {t}: " + ("a refresh iteration that does not end a refresh "
                                              "block" if refresh else
                                              "a refresh block ending off the grid"))
                pd, pdl, od, dspray, dsess = D.iteration(recs)
                pw, pwl, ow, wspray, wsess = W.iteration(xrecs)
                if od != ow:
                    fail(f"GameTime {t}: the records other than units, order lists and sprays "
                         f"differ")
                if grid:
                    cells = sorted(set(D.prev_cells) | set(W.prev_cells))
                    if cells:
                        dg, wg = D.grid[cells], W.grid[cells]
                        pg = np.array([D.prev_cells.get(c, D.grid[c]) for c in cells])
                        for r in np.nonzero(((dg ^ wg) & ~CH).any(axis=1))[0]:
                            fail(f"GameTime {t}: cell {cells[r]}'s shown bytes differ")
                        ok = ((dg & CH) == (wg & CH)) | ((dg & CH) == (pg & CH))
                        for r in np.nonzero(~ok.all(axis=1))[0]:
                            fail(f"GameTime {t}: cell {cells[r]}'s held bytes are neither the "
                                 f"stream's last nor the exact ones")
                    want = []
                    for b in W.inputs:
                        x, z, nc = struct.unpack_from("<HHH", b)
                        i = z * W.gw + x
                        run = np.frombuffer(b, np.uint8, nc * REC_CELL_BYTES, 8)
                        if ((run.reshape(nc, REC_CELL_BYTES) ^ D.grid[i:i + nc]) & ~CH).any():
                            want.append(b)
                    if D.inputs != want:
                        fail(f"GameTime {t}: the tick's input runs are not the exact stream's "
                             f"that differ from the stream's grid in a shown byte")
                if wsess:
                    fail(f"GameTime {t}: a spray session in the exact stream")
                if dsess and not sessions_on:
                    fail(f"GameTime {t}: a spray session in a stream without RECL_SPRAYS")
                if sessions_on:
                    try:
                        for b in dspray:
                            S.spray(t, b, D.unit_of)
                        for b in dsess:
                            S.session(b)
                        implied = S.imply(t, D.unit_of)
                        n_written += len(dspray)
                        n_implied += len(implied)
                        dspray = dspray + implied
                    except Unreadable as e:
                        fail(f"GameTime {t}: {e}")
                if sorted(dspray) != sorted(wspray):
                    fail(f"GameTime {t}: the nano sprays, {len(dspray)} with the implied ones, "
                         f"are not the exact stream's {len(wspray)}")
                else:
                    ids = {b[:4] for b in wspray}
                    for i in sorted(ids):
                        if [b for b in dspray if b[:4] == i] != [b for b in wspray if b[:4] == i]:
                            fail(f"GameTime {t}: unit ID {struct.unpack('<I', i)[0]}'s sprays "
                                 f"are not in the exact stream's order")
                slots = sorted(set(pd) | set(pw))
                if slots:
                    sl = np.array(slots)
                    dl, wl = D.live[sl], W.live[sl]
                    for s in sl[dl != wl]:
                        fail(f"GameTime {t}: slot {s} live {bool(D.live[s])} against "
                             f"{bool(W.live[s])}")
                    lv = sl[dl & wl]
                    du, wu = D.units[lv], W.units[lv]
                    for r in np.nonzero(((du ^ wu) & ~H).any(axis=1))[0]:
                        fail(f"GameTime {t}: slot {lv[r]}'s shown bytes differ")
                    if H.any() and len(lv):
                        pv = np.array([pd[s][1] if s in pd else D.units[s] for s in lv])
                        pl = np.array([pd[s][0] if s in pd else True for s in lv])
                        ok = ((du & H) == (wu & H)) | (((du & H) == (pv & H)) & pl[:, None])
                        for r in np.nonzero(~ok.all(axis=1))[0]:
                            fail(f"GameTime {t}: slot {lv[r]}'s held bytes are neither the "
                                 f"stream's last nor the exact ones")
                for key in sorted(set(pdl) | set(pwl)):
                    a, b = D.lists.get(key, b""), W.lists.get(key, b"")
                    if a == b:
                        continue
                    shown = key[1] == 0 and node_held and len(a) == len(b) and not (
                        (np.frombuffer(a, "<u4").reshape(-1, REC_ORDER_IMAGE // 4) ^
                         np.frombuffer(b, "<u4").reshape(-1, REC_ORDER_IMAGE // 4)) & ~HN).any()
                    if not shown:
                        fail(f"GameTime {t}: slot {key[0]}'s {'sub' if key[1] else 'main'} "
                             f"list's shown bytes differ")
                    elif a != pdl.get(key, a):
                        fail(f"GameTime {t}: slot {key[0]}'s main list is neither the stream's "
                             f"last nor the exact one")
                if refresh:
                    n_refresh += 1
                    if not (np.array_equal(D.live, W.live) and
                            np.array_equal(D.units[D.live], W.units[W.live]) and
                            D.lists == W.lists and
                            (grid is None or np.array_equal(D.grid, W.grid))):
                        fail(f"GameTime {t}: the stream's world is not the exact copy at a "
                             f"refresh")
            k += 2
    print(f"{path}: lossy-check of timeline {timeline}: {n_iter} iterations to GameTime {last}, "
          f"{n_refresh} refreshes, {n_snap} snapshots, sprays {n_written} written and "
          f"{n_implied} implied; "
          + ("every shown byte exact, every held byte the stream's last or the exact one, "
             "every iteration's sprays the exact ones, and the stream's world the exact copy at "
             "every refresh" if not bad else
             f"{bad} FAILURES") + (f"; stopped at {stop}" if stop else ""))
    for line in lines:
        print("  " + line)
    return 1 if bad or not n_iter else 0


def cmd_pov(path, check):
    """The perspective records in stream order, and with `check` what the plan's V1 requires of
    them (demo-recorder-perspective-streams.md): each well formed and bounded (World.perspective),
    each seat's samples at least their `every` apart and each differing from its last, and the
    stream's perspective equal to the one each snapshot rebuilds."""
    h, bl, stop = load(path)
    bad, counts, snaps, last = [], {RK_POV: 0, RK_CLICK: 0, RK_SELECT: 0}, [0], {}

    def on_record(w, kind, b):
        if kind == RK_POV:
            f = pov_fields(b)
            where = "".join(n for bit, n in ((REC_POV_WORLD, " world"), (REC_POV_MINIMAP, " minimap"))
                            if f[2] & bit) or " outside"
            print(f"GameTime {w.tick} player {f[0]}: camera ({f[7]}, {f[8]}), view {f[4]}x{f[5]}, "
                  f"zoom {f[6]}/65536, cursor ({f[9]}, {f[10]}, {f[11]}){where}, every {f[1]}")
            was = last.get(f[0])
            if was and w.tick - was[0] < f[1]:
                bad.append(f"GameTime {w.tick} player {f[0]}: a sample {w.tick - was[0]} ticks "
                           f"after the last, under its every {f[1]}")
            if was and was[1] == bytes(b):
                bad.append(f"GameTime {w.tick} player {f[0]}: a sample equal to the last one")
            last[f[0]] = (w.tick, bytes(b))
        elif kind == RK_CLICK:
            f = click_fields(b)
            print(f"GameTime {w.tick} player {f[0]}: {('left', 'right')[f[1]]} click at "
                  f"({f[7]}, {f[8]}, {f[9]}), order {f[2]}, cursor {f[3]}, region {f[4]}, "
                  f"interface {f[5]}, keys 0x{f[6]:X}")
        elif kind == RK_SELECT:
            p, _, total = REC_SEL.unpack_from(b)[:3]
            units = " ".join(f"{slot}:{birth:08X}" for slot, birth in w.sel[p][:8])
            print(f"GameTime {w.tick} player {p}: selection of {total}"
                  + (f": {units}" + (" ..." if total > 8 else "") if total else ""))
        if kind in counts:
            counts[kind] += 1

    def at_snapshot(w, payload):
        snaps[0] += 1
        s = snapshot_world(payload, h["slots"], h["tables"], feature_dims(h), w.base, w.header,
                           w.wbase)
        bad.extend(f"the snapshot at GameTime {s.tick}: {line.replace('dump', 'snapshot')}"
                   for line in w.pov_differences(s))

    try:
        stream_to(h, bl, on_snapshot=at_snapshot, on_record=on_record)
    except Unreadable as e:
        if not check:
            raise
        bad.append(f"unreadable: {e}")
    print(f"{path}: {counts[RK_POV]} samples, {counts[RK_CLICK]} clicks, {counts[RK_SELECT]} "
          f"selections; {snaps[0]} snapshots" + (f"; stopped at {stop}" if stop else ""))
    if not check:
        return 0
    if h.get("mode") == 3:
        bad.append("a multiplayer file carries its perspectives as contributions, which this "
                   "command does not read")
    elif not counts[RK_POV]:
        bad.append("no camera and cursor sample is recorded")
    for line in bad[:40]:
        print("  FAIL " + line)
    print("  " + (f"{len(bad)} FAILED" if bad else "every perspective check holds"))
    return 1 if bad else 0


def cmd_pov_eye(path, n, view, seat):
    """Follow's answer for the displayed GameTime n on a view_w x view_h screen, as the playback
    computes it at its frame visit (tagpu_replay.c, FOLLOW, JUMP AND FREE CAM): the world at the
    end of iteration n -- the reader's RecWorld there, W->tick == n -- the seat's held samples, the
    perspective clock and Follow's fit (persp_sample, persp_follow). `seat` None takes the only seat
    with samples. Prints the sample and `eye (x, y), level L/65536`; the eye is before the camera's
    range clamp, which the playback applies with the map and the range in force."""
    h, bl, stop = load(path)
    w = stream_to(h, bl, want=n)
    if w.tick != n:
        print(f"{path}: GameTime {n} is no iteration of the stream (it reached {w.tick}"
              + (f"; stopped at {stop}" if stop else "") + ")")
        return 1
    held = [p for p in range(REC_PLAYERS) if w.pov[p]]
    if seat is None:
        if len(held) != 1:
            print(f"{path}: at GameTime {n} the seats with samples are {held or 'none'}: "
                  f"name one with --seat")
            return 1
        seat = held[0]
    how, f = persp_sample(w.pov[seat], n)
    fit = persp_follow(f, *view) if f else None
    if not fit:
        print(f"{path}: GameTime {n} player {seat}: no sample to follow")
        return 1
    ticks = ", ".join(str(t) for t, _ in w.pov[seat])
    print(f"GameTime {n} player {seat}: sample {PERSP_HOW[how]} of the samples at [{ticks}]: "
          f"camera ({f[7]}, {f[8]}), view {f[4]}x{f[5]}, zoom {f[6]}/65536")
    print(f"  on {view[0]}x{view[1]}: eye ({fit[0]}, {fit[1]}), level {fit[2]}/65536 "
          f"({fit[2] / 65536:.4f})")
    return 0


def dump_world(path, h):
    """A dump's world, read with its own header's tables and the recording's dimensions."""
    dbl, dstop = blocks(path)
    if len(dbl) != 2 or dbl[0][0] != RB_HEADER or dbl[1][0] != RB_SNAPSHOT:
        raise Unreadable(f"{path}: not a dump (a header and one snapshot block)" +
                         (f" ({dstop})" if dstop else ""))
    dh = header(dbl[0][3])
    return dh, snapshot_world(dbl[1][3], h["slots"], dh["tables"], feature_dims(h))


def mapped_hold(w, bit=VIEWER_BIT):
    """The explored map less the viewer's bit, which a puppet host keeps set in every entry
    (tagpu_replay.c, recorded_put): masked on both sides of a puppet compare."""
    if bit >= 16:
        return
    for i in range(0, len(w.mapped) - 1, 2):
        v = struct.unpack_from("<H", w.mapped, i)[0] & ~(1 << bit)
        struct.pack_into("<H", w.mapped, i, v)


def puppet_view(w, radar=False):
    """A world less what a puppet host keeps as its own (demo-recorder-s2-host.md §1.1, §2.3): the
    contact bits +0x110 8-10 and 12, which its radar pass computes for its own view (D1) -- bit 12
    only, with `radar`, when the host views the player the recording viewed, whose radar picture
    it computes from the same inputs, and loop 4 of the pass, which sets bit 12, runs only for a
    type 1 or 2 owner; and the controllers and the controlled and viewed players, which are the
    host's seating. Every recorded record is compared: the viewer's is record 10, which neither a
    recording nor a dump carries."""
    mask = 0x1000 if radar else 0x1700
    for img in w.units.values():
        struct.pack_into("<I", img, U_STATE, struct.unpack_from("<I", img, U_STATE)[0] & ~mask)
    w.ctl = [0] * REC_PLAYERS
    w.view = [0, 0]


FORK_NODE = ((0x05, 1), (0x06, 4), (0x0A, 4), (0x4E, 4), (0x52, 4), (0x32, 4))
FORK_NODE_FLAGS = 0x600000          # +0x42 bits 21 (seen) and 22 (stopbuilding)


def fork_view(a, b, ha, hb):
    """A parent's world at fork_at and its fork's first iteration less what the takeover's rebuild
    makes its own, decision D2's list and nothing else: (a) every order node's handler
    state, which the rebuild restarts -- +0x05, +0x06, +0x0A, +0x4E, +0x52, +0x32 and +0x42's bits
    21 and 22; (b) the player images and sight counters of every record that is not a seat of the fork's header, the
    viewer's and the deactivated records alike; (c) the controllers and the controlled and viewed
    players, and PlayerInfo +0x94 of each seat whose controller the two headers give differently;
    (d) the contact bits by puppet_view's rule, bit 12 always and bits 8-10 too unless the two
    worlds view the same player."""
    seats = {p["index"] for p in hb["players"]}
    ctl_a = {p["index"]: p["controller"] for p in ha["players"]}
    ctl_b = {p["index"]: p["controller"] for p in hb["players"]}
    mask = 0x1000 if a.view[1] == b.view[1] else 0x1700
    for w in (a, b):
        for img in w.lists.values():
            for at in range(0, len(img), REC_ORDER_IMAGE):
                for off, size in FORK_NODE:
                    img[at + off:at + off + size] = bytes(size)
                v = struct.unpack_from("<I", img, at + 0x42)[0] & ~FORK_NODE_FLAGS
                struct.pack_into("<I", img, at + 0x42, v)
        for i in range(REC_PLAYERS):
            if i not in seats:
                w.players[i] = bytearray(REC_PLAYER_IMAGE)
                w.sight.pop(i, None)
            elif ctl_a.get(i) != ctl_b.get(i):
                w.players[i][REC_INFO_AT + INFO_TYPE] = 0
        w.ctl = [0] * REC_PLAYERS
        w.view = [0, 0]
        for img in w.units.values():
            struct.pack_into("<I", img, U_STATE,
                             struct.unpack_from("<I", img, U_STATE)[0] & ~mask)


def cmd_compare_fork(path, n):
    """Timeline n's first iteration, the world the takeover's rebuild made, against its parent's
    chain at fork_at, through fork_view with the header of the parent chain's segment that holds
    fork_at (a fork of a fork takes its parent segment's controllers, not the root's)."""
    segs, _ = file_segments(path, decode=True)
    c = chain(segs, n)[-1]
    if not c["id"]:
        raise Unreadable("timeline 0 is the original: it forks from nothing")
    T = c["fork_at"]
    hp, blp, _ = load(path, timeline=c["parent"])
    a = stream_to(hp, blp, want=T)
    if a.tick != T:
        print(f"{path}: timeline {c['parent']}'s chain reads to GameTime {a.tick}, not to "
              f"timeline {n}'s fork_at {T}")
        return 2
    b = stream_to(c["header"], c["blocks"], want=T)
    if b.tick != T:
        print(f"{path}: timeline {n}'s first iteration is GameTime {b.tick}, not {T}")
        return 2
    fork_view(a, b, a.header, c["header"])
    lines, k = a.differences(b, hp["tables"], c["header"]["tables"], gnames=known_names(hp))
    print(f"{path}: timeline {n} at GameTime {T} against timeline {c['parent']}'s chain, "
          f"{len(b.units)} live units: " + ("EQUAL" if not k else f"{k} DIFFERENCES"))
    for line in lines:
        print("  " + line.replace("dump", f"timeline {n}"))
    return 1 if k else 0


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


def globals_opaque(h):
    """The globals image's dwords of `opaque` slots, when this checkout's list is the recording's;
    with another list, none (every dword is scanned)."""
    if not h.get("globals"):
        return set()
    try:
        slots = globals_list(h["globals"])[2]
    except Unreadable:
        return set()
    return {sl["dword"] + k for sl in slots if sl["opaque"] for k in range((sl["size"] + 3) // 4)}


def known_names(h):
    """The globals' names when this checkout's list is the recording's, else None (dword numbers)."""
    if not h.get("globals"):
        return None
    try:
        return globals_names(globals_list(h["globals"])[2])
    except Unreadable:
        return None


# THE OCCUPANCY RULE: where a playing's grid is the engine's, its unit slots and yard bit are not
# compared but checked, and then taken from the dump. The owner's decision for a multiplayer
# recording (its units follow their owners, tagpu_puppet.c, THE FOOTPRINT WRITERS), and on
# 2026-10-12 for a skirmish recording that holds its cells after a seek (`contest`): a seek's
# creates and moves stamp in slot order, and a contested cell goes to whichever came first there,
# not to the unit that came first in the game (demo-recorder-completeness.md, Decisions taken).
STRUCTURE = 1 << 29     # unit +0x110: a structure, whose yard bits a footprint writer sets
U_LOSER = 1 << 27       # unit +0x110: the contest's loser, which a holder's clear re-claims
U_HOLDER = 1 << 26      # unit +0x110: the holder of a cell a loser contests
U_MARKS = U_LOSER | U_HOLDER    # the footprint writers' alone


def rect(u):
    """The cells a unit's footprint covers: +0x76/+0x78 by +0x7E/+0x80, i16 each."""
    x, z = struct.unpack_from("<hh", u, 0x76)
    w, h = struct.unpack_from("<hh", u, 0x7E)
    return {(cx, cz) for cx in range(x, x + w) for cz in range(z, z + h)}


def structure(u):
    return struct.unpack_from("<I", u, U_STATE)[0] & STRUCTURE


def footprints(dw):
    """Every cell a live structure of the dump covers."""
    return set().union(*(rect(u) for u in dw.units.values() if structure(u)))


def stamped(u, gw, gh):
    """Whether the stamp 0x47CC30 writes a mobile unit's footprint at all: it parks one with x or
    z below 0, or x + w or z + h past the grid's columns or rows, in the off-map bucket and writes
    no cell (DISASSEMBLED 0x47CC57..0x47CCA3, with Impure's `jg` at 0x47CC8B and 0x47CCA3:
    tagpu_patches.c, fix_last_cell), and it files the rest by the layer, +0x110 & 3: 1 in slot A,
    2 in slot B, any other value in no cell (0x47CEBC..0x47CEC2) -- a carried unit's, 0."""
    x, z = struct.unpack_from("<hh", u, 0x76)
    w, h = struct.unpack_from("<hh", u, 0x7E)
    layer = struct.unpack_from("<I", u, U_STATE)[0] & 3
    return layer in (1, 2) and x >= 0 and z >= 0 and x + w <= gw and z + h <= gh


def layer(u):
    """The layer the stamp files a unit by, +0x110 & 3: 1 slot A, 2 slot B (stamped)."""
    return struct.unpack_from("<I", u, U_STATE)[0] & 3


U_BUCKET, U_CARRIER, U_NEXT = 0x82, 0x86, 0x8E    # a unit's sort bucket, carrier and next


def chain_faults(w):
    """What breaks the engine's invariants of world `w`'s sort buckets: every live unit that is
    not carried and has a bucket is in that bucket's one chain -- a next is a unit of the same
    bucket, no unit is named twice, each bucket has one head, and the walks from the heads reach
    every unit."""
    out, member, named, heads = [], {}, {}, {}

    def nxt(slot):
        return struct.unpack_from("<I", w.units[slot], U_NEXT)[0]
    for slot, u in w.units.items():
        bucket, carrier = struct.unpack_from("<II", u, U_BUCKET)
        if bucket and not carrier:
            member[slot] = bucket
    for slot, bucket in sorted(member.items()):
        n = nxt(slot)
        if not n:
            continue
        if member.get(n - 1) != bucket:
            out.append(f"sort chains: unit {slot}'s next, ID {n}, is no unit of its bucket {bucket}")
        elif n - 1 in named:
            out.append(f"sort chains: units {named[n - 1]} and {slot} both name unit {n - 1} next")
        else:
            named[n - 1] = slot
    for slot, bucket in sorted(member.items()):
        if slot in named:
            continue
        if bucket in heads:
            out.append(f"sort chains: units {heads[bucket]} and {slot} both head bucket {bucket}")
        else:
            heads[bucket] = slot
    walked = 0
    for bucket, slot in heads.items():
        seen = set()
        while slot is not None and member.get(slot) == bucket and slot not in seen:
            seen.add(slot)
            slot = nxt(slot) - 1 if nxt(slot) else None
        walked += len(seen)
    if not out and walked != len(member):
        out.append(f"sort chains: the walks from the heads reach {walked} of the {len(member)} "
                   "units in a bucket")
    return out


def sort_chains(w, dw):
    """A multiplayer level's sort buckets: `dw`'s checked by the engine's invariants
    (chain_faults) instead of compared, and then taken into `w` (tagpu_replay.c, THE SORT
    BUCKETS ON A MULTIPLAYER LEVEL). Each owner's image carries its own machine's +0x8E, so the
    composed images need not form chains, and a host's order is its own -- a seek's is a
    function of T's composed world, a playing's of its history. So +0x8E of every unit not
    carried is cleared in both worlds; the bucket +0x82 and a carried unit's +0x8E, its
    carrier's chain, are compared. The differences the check finds."""
    out = chain_faults(dw)
    for world in (w, dw):
        for u in world.units.values():
            if not struct.unpack_from("<I", u, U_CARRIER)[0]:
                struct.pack_into("<I", u, U_NEXT, 0)
    return out


def occupancy(w, dw, contest=False):
    """The dump's unit slots and yard bit checked by the engine's invariants instead of compared,
    and then taken into the expected world `w`: each footprint cell of a live mobile unit names it;
    each slot a cell names is a live unit whose footprint holds the cell; a yard bit lies inside a
    live structure's footprint. With `contest`, a skirmish recording's after a seek, the checks are
    the stamp's own, layer by layer: a mobile unit the stamp does not write (stamped) has no cell
    to name it; a stamped unit's footprint cell names it in its layer's slot (A for layer 1, B for
    layer 2), or, when it carries the loser's mark, bit 27, names there another live unit of the
    same layer -- or a structure, for a layer-1 unit: the stamp files a structure in slot A
    whatever its layer (0x47CD5A -> 0x47CDA5) -- whose footprint holds the cell and which carries
    the holder's mark, bit 26 -- the engine's contest kept the cell for that one (the stamp's
    contest, 0x47CDDB..0x47CE1D, sets 26 on the unit that keeps the cell and 27 on the other; a
    unit's clear, 0x47D286..0x47D2C4, drops its 27 and, holding 26, drops that and re-claims for
    the bit-27 units: DISASSEMBLED); a slot names a mobile unit of its own layer, or a structure
    in slot A; and the contest's marks, +0x110 bits 26 and 27, are taken from the dump too. The
    differences those checks find. Not checked: which cells of a structure's footprint name it --
    the stamp files a structure only at the cells its yardmap sets under the current mask
    (0x47CD82..0x47CD9F), which a dump does not carry -- nor, the marks being per unit, which
    cell a mark was set for."""
    out = []
    cb = REC_CELL_BYTES
    yards = footprints(dw)
    rects = {slot: rect(u) for slot, u in dw.units.items()}
    state = {slot: struct.unpack_from("<I", u, U_STATE)[0] for slot, u in dw.units.items()}
    for slot, u in sorted(dw.units.items()):
        if structure(u) or (contest and not stamped(u, w.gw, w.gh)):
            continue
        lost = contest and state[slot] & U_LOSER
        for cx, cz in sorted(rects[slot]):
            i = (cz * w.gw + cx) * cb
            if not (0 <= cx < w.gw and 0 <= i < len(dw.grid)):
                out.append(f"occupancy: cell {(cx, cz)} of mobile unit {slot}'s footprint "
                           "does not name it")
                continue
            named = struct.unpack_from("<HH", dw.grid, i)
            if not contest:
                if slot in named:
                    continue
                out.append(f"occupancy: cell {(cx, cz)} of mobile unit {slot}'s footprint "
                           "does not name it")
                continue
            want = named[layer(u) - 1]
            if want == slot:
                continue
            if lost and want in dw.units and (cx, cz) in rects[want] and \
                    (layer(dw.units[want]) == layer(u) or
                     structure(dw.units[want]) and layer(u) == 1):
                if not state[want] & U_HOLDER:
                    out.append(f"occupancy: cell {(cx, cz)} of mobile unit {slot}'s footprint "
                               f"names unit {want}, which holds it without the holder's mark")
                continue
            out.append(f"occupancy: cell {(cx, cz)} of mobile unit {slot}'s footprint does not "
                       f"name it in slot {'AB'[layer(u) - 1]}, and no unit of its layer that "
                       "holds it took it from this one")
    for i in range(0, len(dw.grid), cb):
        c = i // cb
        xz = (c % w.gw, c // w.gw)
        w.grid[i:i + 4] = dw.grid[i:i + 4]
        w.grid[i + 0x0C] = (w.grid[i + 0x0C] & ~2) | (dw.grid[i + 0x0C] & 2)
        for k, slot in enumerate(struct.unpack_from("<HH", dw.grid, i)):
            if slot and slot not in dw.units:
                out.append(f"occupancy: cell {xz} names slot {slot}, not live")
            elif slot and xz not in rects[slot]:
                out.append(f"occupancy: cell {xz} names slot {slot}, outside its footprint")
            elif slot and contest and structure(dw.units[slot]) and k:
                out.append(f"occupancy: cell {xz} names structure {slot} in slot B")
            elif slot and contest and not structure(dw.units[slot]) and \
                    layer(dw.units[slot]) != k + 1:
                out.append(f"occupancy: cell {xz} names slot {slot} in slot {'AB'[k]}, "
                           f"a unit of layer {layer(dw.units[slot])}")
        if dw.grid[i + 0x0C] & 2 and xz not in yards:
            out.append(f"occupancy: cell {xz} holds a yard bit outside every live structure")
    if contest:
        for slot, img in w.units.items():
            if slot in dw.units:
                v = struct.unpack_from("<I", img, U_STATE)[0] & ~U_MARKS
                struct.pack_into("<I", img, U_STATE, v | state[slot] & U_MARKS)
    return out


def cells_exact(path, timeline, w, cache):
    """The expected world's unit slots and yard bit where the stream holds them (holds_cells) and
    the dump's grid is the engine's, the live game's or a playing's: the exact stream's at w's
    GameTime (RB_EXACT, the test lever tagpu_rec.exact), which a playing's engine stamps from the
    same positions; without one,
    the stream's own at a refresh, where it is exact (tagpu_reclossy.c). `cache` keeps the exact
    stream's blocks between dumps. Unreadable anywhere else."""
    if "x" not in cache:
        try:
            cache["x"] = load(path, timeline=timeline, exact=True)
        except Unreadable as e:
            cache["x"] = e
    if isinstance(cache["x"], Unreadable):
        if refresh_at(path, timeline, w.tick) == w.tick:
            return
        raise Unreadable(f"GameTime {w.tick}: the stream holds the cells' slots there, and "
                         f"{cache['x']}")
    xh, xbl, _ = cache["x"]
    x = stream_to(xh, xbl, want=w.tick)
    if x.tick != w.tick:
        raise Unreadable(f"the exact stream reads to GameTime {x.tick}, not {w.tick}")
    for i in range(0, len(w.grid), REC_CELL_BYTES):
        w.grid[i:i + 4] = x.grid[i:i + 4]
        w.grid[i + 0x0C] = (w.grid[i + 0x0C] & ~2) | (x.grid[i + 0x0C] & 2)


def cmd_compare(path, dumps, puppet=False, timeline=0, exact=False, seek=False):
    h, bl, stop = load(path, timeline=timeline, exact=exact)
    status = 0
    cache = {}
    held = puppet_rows() if puppet else None
    gslots = globals_list(h.get("globals"))[2] if puppet else None
    gnames = globals_names(gslots) if puppet else known_names(h)
    for d in dumps:
        dh, dw = dump_world(d, h)
        if "frame" in dh:
            # a frame dump's images hold what the rest of a frame writes: no capture point's
            print(f"{d}: a frame dump at GameTime {dh['frame']}: compare it with compare-dumps")
            status = 2
            continue
        w = stream_to(h, bl, want=dw.tick)
        if w.tick != dw.tick:
            print(f"{d}: GameTime {dw.tick}, but the recording reads to {w.tick}")
            status = 1
            continue
        radar = False
        stale = []
        if puppet:
            radar = dw.view[1] == w.view[1]
            mapped_hold(w)
            mapped_hold(dw)
            puppet_view(w, radar)
            puppet_view(dw, radar)
            puppet_hold(w, held)
            globals_hold(w, gslots)
            globals_hold(dw, gslots)
            if holds_cells(w.header) and seek:
                stale = occupancy(w, dw, contest=True)
        # the live game's grid and a straight playing's are the exact slots: the stream's held
        # ones are behind them between refreshes
        if holds_cells(w.header) and not exact and not (puppet and seek):
            cells_exact(path, timeline, w, cache)
        lines, n = w.differences(dw, h["tables"], dh["tables"], gnames=gnames)
        lines, n = lines + stale, n + len(stale)
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
        (0x531, 0x535, "the active GUI stack pointer"),
        (0x561, 0x565, "the GUI mouse-event clock"),
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
        (0x10A40, 0x10DE0, "the network statistics and their sampler"),
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
        (0x1E4B6, 0x1E4BE, "the cursor's position: the mouse thread 0x4C2990 writes it"),
        (0x1E4D6, 0x1E4DE, "the cursor's drawn position: the mouse thread 0x4C2990 writes it"),
        (0x1E524, 0x1E528, "the current bitmap-font pointer"),
        (0x1EB50, 0x1EBD4, "a registry value, checksums, the graphics globals' pointer"),
        (0x1EBE0, 0x1EC88, "timers"),
        (0x1EC88, 0x1EC8C, "the simulation RNG"),
        (0x27994, 0x28EC4, "the Cavedog library: the engine allocator's statistics, the debug and "
                           "performance objects"),
        (0x28EC4, 0x2A65C, "the C runtime"),
        (0x294E8, 0x294F4, "a tagged lock, its owner's tag and its event: the game thread and the "
                           "mouse thread 0x4C2990 take it"),
        (0x294F4, 0x29500, "a second tagged lock: tag, event, word (0x4D0F60, 0x4D1480)")],
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


def cmd_compare_dumps(path, a_path, b_path, takeover=False, seek=False, timeline=0):
    """Two dumps of playings of FILE at the same GameTime, the same player viewed: every record
    they carry, the players' sight counters included, equal byte for byte. FILE gives the
    dimensions and the globals' names from its first header; the rules below come from the
    header of `timeline`'s chain segment that holds the dumps' GameTime (segment_at), since a
    multiplayer file's takeover timeline is a skirmish segment. With `seek`, B a seek's, in a
    segment that holds its cells: B's unit slots, yard bits and contest marks by THE OCCUPANCY
    RULE, and A's, the straight playing's, by the same rule on their own, since B's then stand
    in for them in the comparison. With `seek` in a multiplayer segment, both dumps' sort buckets
    by the engine's invariants and +0x8E of a unit not carried left out (sort_chains): a seek's
    order there is a function of T's composed world, a playing's of its history."""
    h = header_only(path)
    ah, a = dump_world(a_path, h)
    bh, b = dump_world(b_path, h)
    if a.tick != b.tick or ah.get("frame") != bh.get("frame"):
        print(f"{a_path} is at GameTime {a.tick}, {b_path} at {b.tick}"
              + ("" if ah.get("frame") == bh.get("frame") else
                 ": one is a frame dump and the other is not, or their frames differ"))
        return 1
    hs = segment_at(path, timeline, a.tick)["header"]
    if takeover:
        fork_view(a, b, hs, hs)
    stale = []
    if seek and holds_cells(hs):
        # occupancy(a, a) writes A's own cells and marks over themselves
        stale = ["A's " + line for line in occupancy(a, a, contest=True)]
        stale += occupancy(a, b, contest=True)
    if seek and hs.get("mode") == 3:
        stale += ["A's " + line for line in chain_faults(a)] + sort_chains(a, b)
    lines, n = a.differences(b, ah["tables"], bh["tables"], gnames=known_names(h), sight=True)
    lines, n = lines + stale, n + len(stale)
    when = (f"frame dump at GameTime {ah['frame']} (iteration {a.tick})" if "frame" in ah
            else f"GameTime {a.tick}")
    print(f"{a_path} against {b_path}: {when}: "
          + ("EQUAL" if not n else f"{n} DIFFERENCES") + " (A is the first, B the second)")
    for line in lines:
        print("  " + line.replace("the recording", "A").replace("recording", "A")
              .replace("the dump", "B").replace("dump", "B"))
    return 1 if n else 0


# THE COUNTS (tagpu_recorder.c, THE COUNTS): what compare-counts compares
COUNT_ROOTS = {
    0x4861B3: "the create 0x485F50 (its exit)", 0x4866D0: "the destructor 0x4866D0",
    0x41B8D0: "build done 0x41B8D0", 0x48B090: "the state setter 0x48B090 (a change)",
    0x47F780: "the reply 0x47F780", 0x47F850: "the reply 0x47F850",
    0x4720D0: "the nano emitter 0x4720D0", 0x472200: "the nano emitter 0x472200",
    0x424050: "the feature tick 0x424050", 0x423C50: "the feature spawn 0x423C50",
    0x4246B0: "the feature destroy 0x4246B0", 0x423550: "the feature death 0x423550",
    0x4233A0: "the feature burn 0x4233A0", 0x4244B0: "the feature damage 0x4244B0",
    0x423710: "the feature swap 0x423710"}
# in the recorder's CNT_SINKS order: bit k of the sinks counted
COUNT_SINKS = [0x420A30, 0x4B08C0, 0x47F0C0, 0x47F1A0, 0x47F210, 0x47F300, 0x49C740, 0x49CDE0,
               0x49CC20, 0x49C9C0, 0x499EB0, 0x471FD0, 0x472330, 0x472430, 0x472530, 0x472630,
               0x472810, 0x4728F0, 0x4729D0, 0x472AB0, 0x472C50]
# The script slice: the unit tick's DoScriptsNow(1), counted where it is given, by aux
# (tagpu_patches.c, THE SCRIPT SLICE; tagpu_rec_slice_given). A live game gives every slice in its
# unit tick or at the place the recording names for it; a playing gives a slice at the step's end
# only when its marker did not.
SLICE_ROOT = 0x48ADEB
SLICE_WHERE = {0: "in the unit tick", 1: "at its recorded place", 2: "at the step's end"}
# the COB thread allocation's callers inside a slice: the VM's START and CALL (the engine map,
# *COB child allocation caller keys*), each key's aux the child script's name hashed
COB_CHILD_ALLOC = 0x4B08C0
COB_CHILDREN = {0x4B18C0: "START", 0x4B192D: "CALL"}
COB_WRITES = {0x480B57: "SET ACTIVATION", 0x480BF1: "SET ARMORED", 0x480BB3: "SET YARD_OPEN",
              0x4813AB: "ATTACH", 0x481427: "DROP"}
# the fifteen owner-only start calls (tagpu_recorder.c, START_SITES), counted at the call: on a
# puppet level only the replay starts these scripts, through 0x4B0A70 itself, so a count here is
# the engine starting one natively (the slice note, *Exactly-once private children*)
OWNER_STARTS = {0x489F43: "HitByWeapon", 0x489F8E: "TakeDamage", 0x406834: "TransportPickup",
                0x4069BF: "TransportDrop", 0x489898: "TargetCleared (0x489800)",
                0x489948: "TargetCleared (0x4898B0)", 0x48A149: "TargetCleared (0x48A0F0)",
                0x48A2E0: "TargetCleared (0x48A1E0)", 0x40F433: "EndTransport", 0x41148D: "EndTransport",
                0x411794: "EndTransport", 0x411DA1: "EndTransport", 0x411E2B: "EndTransport",
                0x4113EF: "BeginTransport", 0x43DBE8: "setSFXoccupy"}
# the six owner's query calls (tagpu_recorder.c, QUERY_SITES), counted at the call: on a puppet
# level only the replay asks these, through 0x4B0BC0 itself, so a count here is the engine's
# order handler asking one natively
OWNER_QUERIES = {0x4027FB: "QueryBuildInfo", 0x4113B1: "QueryTransport",
                 0x411A35: "QueryLandingPad", 0x411AA6: "QueryLandingPad",
                 0x411BF5: "QueryLandingPad", 0x411CF1: "QueryLandingPad"}


def counts_of(path, h):
    """A dump's GameTime, counts, (overflow, sinks counted, counting) and script names."""
    _, w = dump_world(path, h)
    if w.counts is None:
        raise Unreadable(f"{path}: a dump without counts (RK_COUNTS): built before S3c-2")
    return w.tick, w.counts, w.cmeta, w.cnames


def caller_text(c):
    return f"dll+0x{c & ~REC_COUNT_DLL:X}" if c & REC_COUNT_DLL else f"0x{c:06X}"


def absent_rows(path):
    """replay-absent.tsv: the band ((percent, floor)) and the rows, each (site, callers, aux, side)
    with callers a tuple of the caller column's comma-separated entries."""
    band, rows = (0, 0), []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        m = re.match(r"#\s*band\t(\d+)%\t(\d+)\s*$", line)
        if m:
            band = (int(m.group(1)), int(m.group(2)))
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) < 6 or f[3] not in ("absent", "extra", "same"):
            raise Unreadable(f"{path}: a row without six fields or with side {f[3:4]}: {line}")
        rows.append((int(f[0], 16), tuple(c.strip() for c in f[1].split(",")), f[2], f[3]))
    return band, rows


def caller_matches(entry, caller):
    if entry == "*":
        return True
    if entry == "dll":
        return bool(caller & REC_COUNT_DLL)
    return entry == caller_text(caller) or entry.lower() == f"0x{caller:x}"


def aux_matches(entry, aux, name):
    return entry == "*" or entry == f'"{name}"' or entry.lower() == f"0x{aux:x}"


def absent_covers(rows, site, caller, aux, name, side):
    """Whether an absent or extra row covers the key; caller is a VA, or a `same` row's label."""
    for rs, rc, ra, rside in rows:
        if rs != site or rside != side:
            continue
        if isinstance(caller, str):
            if rc != ("*",) and ",".join(rc) != caller:
                continue
        elif not any(caller_matches(e, caller) for e in rc):
            continue
        if aux_matches(ra, aux, name):
            return True
    return False


def same_class(rows, site, caller, aux, name):
    """The `same` row's callers that the key belongs to, joined, or None: one effect the engine
    reaches through another path on a puppet host, so its callers' counts are compared summed."""
    for rs, rc, ra, rside in rows:
        if rside == "same" and rs == site and aux_matches(ra, aux, name) \
                and any(caller_matches(e, caller) for e in rc):
            return ",".join(rc)
    return None


def within_band(band, a, b):
    """A sink's counts agree when they differ by at most the band's percent of the larger, or by
    its floor, whichever is more."""
    pct, floor = band
    return abs(a - b) <= max(floor, pct * max(a, b) // 100)


def bounded_cob_flare(key, live, puppet, sinks, band):
    """The named COB explosion can emit at most one flare (0x420AE1).
    Its accepted parent difference bounds this allowance; it does not identify
    which individual parent calls account for the child difference."""
    if key != (0x472630, 0x420AE6, 0x4812D3) or not sinks & (1 << COUNT_SINKS.index(0x420A30)):
        return None
    parent = (0x420A30, 0x4812D3, 0)
    a, b = live.get(key, 0), puppet.get(key, 0)
    pa, pb = live.get(parent, 0), puppet.get(parent, 0)
    if min(a, b, pa, pb) < 0 or a > pa or b > pb or not within_band(band, pa, pb):
        return None
    if (a - b) * (pa - pb) > 0 and abs(a - b) <= abs(pa - pb):
        return pa, pb
    return None


def cmd_compare_counts(path, la, lb, pa, pb, absent, rewind=False, cob_exact=False):
    """The counts of a live game (L, dumps la and lb) against a playing's (P, pa and pb), each as
    the change between its two dumps: every root per site exactly; the script slice per placement
    exactly (SLICE_WHERE); every sink per (site, caller, aux), a `same` row's callers summed,
    within replay-absent.tsv's band, less its absent and extra rows; the named COB flare can
    inherit an accepted parent shortfall, reported with both counts; and no COB engine write,
    native owner-only start or native owner's query on the puppet.
    Rewind permits only VM START/CALL child counts to differ and reports each pair. cob_exact
    compares every VM START/CALL child key with a script name (aux not 0) exactly instead: no band,
    no flare, no row; aux 0, a refused index, stays with the rows. The two exclude each other.
    Both dumps of a pair are taken at a capture point before that step's replay step, so both
    windows hold the steps [first GameTime, second GameTime)."""
    if rewind and cob_exact:
        print("compare-counts: --rewind and --cob-exact exclude each other")
        return 2
    h = header_only(path)
    (ta, ca, ma, na), (tb, cb, mb, nb) = counts_of(la, h), counts_of(lb, h)
    (tpa, cpa, mpa, npa), (tpb, cpb, mpb, npb) = counts_of(pa, h), counts_of(pb, h)
    names = {**na, **nb, **npa, **npb}
    if (ta, tb) != (tpa, tpb) or ta >= tb:
        print(f"the windows differ: live [{ta}, {tb}), puppet [{tpa}, {tpb})")
        return 2
    bad = [p for p, m in ((la, ma), (lb, mb), (pa, mpa), (pb, mpb)) if not m[2]]
    if bad:
        print("counted nothing (the level neither recorded nor played): " + ", ".join(bad))
        return 2
    lines, unrun, accepted = [], [], []
    for p, m in ((la, ma), (lb, mb), (pa, mpa), (pb, mpb)):
        if m[0]:
            lines.append(f"{p}: {m[0]} keys found no row (the table full)")
    sinks = ma[1] & mb[1] & mpa[1] & mpb[1]
    for k, va in enumerate(COUNT_SINKS):
        if not sinks >> k & 1:
            unrun.append(f"sink 0x{va:06X}: not counted in every process")
    band, rows = absent_rows(absent)
    live = {k: cb.get(k, 0) - ca.get(k, 0) for k in set(ca) | set(cb)}
    pup = {k: cpb.get(k, 0) - cpa.get(k, 0) for k in set(cpa) | set(cpb)}
    for va, what in COUNT_ROOTS.items():
        a = sum(n for (s, _, _), n in live.items() if s == va)
        b = sum(n for (s, _, _), n in pup.items() if s == va)
        if a != b:
            lines.append(f"root {what}: live {a}, puppet {b}")
    for aux, where in SLICE_WHERE.items():
        a = sum(n for (s, _, x), n in live.items() if s == SLICE_ROOT and x == aux)
        b = sum(n for (s, _, x), n in pup.items() if s == SLICE_ROOT and x == aux)
        if a != b:
            lines.append(f"root the script slice 0x{SLICE_ROOT:06X} {where}: live {a}, puppet {b}")
    def grouped(counts):
        out = {}
        for (site, caller, aux), n in counts.items():
            if site in COUNT_SINKS:
                cls = same_class(rows, site, caller, aux, names.get(aux, ""))
                key = (site, cls if cls else caller, aux)
                out[key] = out.get(key, 0) + n
        return out
    glive, gpup = grouped(live), grouped(pup)
    nsink = 0
    for key in sorted(set(glive) | set(gpup), key=lambda k: (k[0], str(k[1]), k[2])):
        site, caller, aux = key
        if not sinks >> COUNT_SINKS.index(site) & 1:
            continue
        a, b = glive.get(key, 0), gpup.get(key, 0)
        nsink += 1
        if rewind and site == COB_CHILD_ALLOC and caller in COB_CHILDREN:
            accepted.append(f'accepted rewind COB child {caller_text(caller)} '
                            f'"{names.get(aux, "")}": live {a}, puppet {b}')
            continue
        if cob_exact and site == COB_CHILD_ALLOC and caller in COB_CHILDREN and aux:
            if a != b:
                lines.append(f'COB child {COB_CHILDREN[caller]} "{names.get(aux, "")}": '
                             f'live {a}, puppet {b}')
            continue
        if within_band(band, a, b):
            continue
        parent = bounded_cob_flare(key, live, pup, sinks, band)
        if parent is not None:
            accepted.append(f"accepted bounded COB flare 0x4812D3: "
                            f"parent live {parent[0]}, puppet {parent[1]}; "
                            f"flare live {a}, puppet {b}")
            continue
        name = names.get(aux, "")
        if absent_covers(rows, site, caller, aux, name, "absent" if b < a else "extra"):
            continue
        label = f' "{name}"' if name else (f" aux 0x{aux:X}" if aux else "")
        who = caller if isinstance(caller, str) else caller_text(caller)
        lines.append(f"sink 0x{site:06X} from {who}{label}: live {a}, puppet {b}")
    for va, what in COB_WRITES.items():
        n = sum(c for (s, _, _), c in cpb.items() if s == va)
        if n:
            lines.append(f"COB engine write on a puppet, {what}: {n} since the level began")
    for va, what in OWNER_STARTS.items():
        n = sum(c for (s, _, _), c in cpb.items() if s == va)
        if n:
            lines.append(f"owner-only start on a puppet, {what} at 0x{va:06X}: {n} since the level "
                         f"began")
    for va, what in OWNER_QUERIES.items():
        n = sum(c for (s, _, _), c in cpb.items() if s == va)
        if n:
            lines.append(f"owner's query on a puppet, {what} at 0x{va:06X}: {n} since the level "
                         f"began")
    print(f"counts, live {la}..{lb} against puppet {pa}..{pb}, GameTime [{ta}, {tb}): "
          f"{len(COUNT_ROOTS)} roots, the script slice, {nsink} sink keys, band {band[0]}% or "
          f"{band[1]}, {len(rows)} rows" + (", the COB children exact" if cob_exact else "") + ": "
          + ("EQUAL" if not lines else f"{len(lines)} DIFFERENCES"))
    for line in lines + unrun + accepted:
        print("  " + line)
    return 1 if lines else 2 if unrun else 0


def cmd_counts(path, dump):
    """A dump's counts, by site."""
    t, c, m, names = counts_of(dump, header_only(path))
    print(f"{dump}: GameTime {t}: {len(c)} keys, {m[0]} found no row, sinks counted "
          f"0x{m[1]:X}, counting {m[2]}")
    for (site, caller, aux), n in sorted(c.items()):
        kind = ("root" if site in COUNT_ROOTS else "script slice" if site == SLICE_ROOT else
                "sink" if site in COUNT_SINKS else
                "COB write" if site in COB_WRITES else
                "owner-only start" if site in OWNER_STARTS else
                "owner's query" if site in OWNER_QUERIES else "site")
        name = names.get(aux)
        print(f"  {kind} 0x{site:06X} from {caller_text(caller)}"
              + (f' "{name}"' if name else f" aux 0x{aux:X}" if aux else "") + f": {n}")
    return 0


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


def image_group(which, at):
    best = None
    for lo, hi, label in IMAGE_MAP[which]:
        if lo <= at < hi and (best is None or hi - lo < best[1] - best[0]):
            best = (lo, hi, label)
    return best[2] if best else "not mapped"


GATE_CLASSES = ("presentation", "host-variant", "load-constant", "known-gap", "FAIL")
STOCK_SECONDARY_COSMETICS = ((0, 0x1491B, 0x1AB8F), (0, 0x1AB9F, 0x1E88F),
                           (1, 0x511DF0 - REC_DATA_BEGIN, 0x511F80 - REC_DATA_BEGIN))


def cmd_images_gate(path, live, twin, playing, second, seek, views=(),
                    stock_secondary_cosmetics=False):
    """THE IMAGES GATE (the null twin): every bit of the main block and .data that no row of
    replay-globals.tsv covers and where the live game at T1 differs from the playing at T1 or from
    the fresh seek to T1 is, in this order:
      presentation   the paused twin's dumps differ there (the live game, paused, then the
                     camera, the zoom, the menu and the cursor moved): the viewer's own
      host-variant   two playings on one host differ there (P against R at T1)
      load-constant  unchanged across [T1, Tlast] on the recording machine and on the puppet host
      known-gap      only the explicit stock-secondary-orders native cosmetic-pool exception
      FAIL           anything else, labelled by who wrote it in play.
    A bit equal at both ends of [T1, Tlast] on each machine counts as unwritten in play: the window
    cannot see a write that was undone inside it. Exit 1 on any FAIL."""
    h = header_only(path)
    if stock_secondary_cosmetics and (Path(path).stem != "stock-secondary-orders" or
                                      h["projectiles"] != 300):
        print("native cosmetic-pool exception requires stock-secondary-orders at the stock cap")
        return 2
    (l1, lT), (n1, n2), (p1, pT) = live, twin, playing
    got = {}
    for name, d in (("l1", l1), ("lT", lT), ("n1", n1), ("n2", n2), ("p1", p1), ("pT", pT),
                    ("r1", second), ("q1", seek), *[(f"v{i}", d) for i, d in enumerate(views)]):
        got[name] = dump_images(d)
    frames = [header(blocks(d)[0][0][3]).get("frame") for d in (n1, n2, *views)]
    if frames[0] is None or len(set(frames)) != 1:
        print(f"the twin's dumps are not two frame dumps of one GameTime: {frames}")
        return 2
    t1 = {got[k][0] for k in ("l1", "p1", "r1", "q1")}
    if len(t1) != 1 or got["lT"][0] != got["pT"][0]:
        print("the dumps are not at T1 and Tlast alike: " +
              ", ".join(f"{k} {got[k][0]}" for k in got))
        return 2
    if len({got[k][1] for k in got}) != 1:
        print("the dumps were made with different replay-globals.tsv")
        return 2
    gen, rows, slots, digest = globals_list(got["l1"][1])
    cov = gen.coverage(rows)
    im = {k: v[2] for k, v in got.items()}
    tally, fails, accepted = {}, [], 0
    for which, where in ((0, "main"), (1, "data")):
        L1, LT, N1, N2 = im["l1"][which], im["lT"][which], im["n1"][which], im["n2"][which]
        P1, PT, R1, Q1 = im["p1"][which], im["pT"][which], im["r1"][which], im["q1"][which]
        for i in range(len(L1)):
            free = ~cov.get((where, i), 0) & 0xFF
            bits = ((L1[i] ^ P1[i]) | (L1[i] ^ Q1[i])) & free
            if not bits:
                continue
            changed = N1[i] ^ N2[i]
            for k in range(len(views)):
                changed |= N1[i] ^ im[f"v{k}"][which][i]
            pres = bits & changed
            host = bits & ~pres & (P1[i] ^ R1[i])
            const = bits & ~pres & ~host & ~((L1[i] ^ LT[i]) | (P1[i] ^ PT[i])) & 0xFF
            fail = bits & ~pres & ~host & ~const
            # Seek clears these native pools; they have no snapshot layout yet. The owner's
            # exact fixture/range exception leaves all other image bits and all record/effect
            # comparisons intact. See demo-recorder-completeness.md, native cosmetic pools.
            gap = fail if stock_secondary_cosmetics and any(
                which == pool and lo <= i < hi for pool, lo, hi in STOCK_SECONDARY_COSMETICS) else 0
            fail &= ~gap
            accepted += bool(gap)
            group = image_group(which, i)
            t = tally.setdefault((which, group), [0, 0, 0, 0, 0])
            for k, m in enumerate((pres, host, const, gap, fail)):
                if m:
                    t[k] += 1
            if fail:
                lw, pw = (L1[i] ^ LT[i]) & fail, (P1[i] ^ PT[i]) & fail
                if lw and not pw:
                    why = "written during play on the recording machine only"
                elif pw and not lw:
                    why = "written on the puppet host only"
                elif not (L1[i] ^ P1[i]) & fail:
                    why = "the seek leaves it otherwise"
                else:
                    why = "both write it, differently"
                fails.append((which, i, group, why, L1[i], P1[i], Q1[i]))
    print(f"the images gate at GameTime {got['l1'][0]} (window to {got['lT'][0]}): "
          + ("PASS" if not fails else f"{len(fails)} bytes FAIL"))
    for (which, group), t in sorted(tally.items()):
        print(f"  {'main' if which == 0 else '.data'} {group}: "
              + ", ".join(f"{c} {n}" for c, n in zip(GATE_CLASSES, t) if n))
    runs = []
    for f in fails:
        if runs and runs[-1][0][:2] == (f[0], f[1] - len(runs[-1])) and runs[-1][0][3] == f[3]:
            runs[-1].append(f)
        else:
            runs.append([f])
    for r in runs[:200]:
        which, at, group, why = r[0][:4]
        name = f"main+0x{at:X}" if which == 0 else f"0x{REC_DATA_BEGIN + at:06X}"
        hexs = lambda k: bytes(x[k] for x in r).hex()
        print(f"  FAIL {name}, {len(r)} byte{'s' if len(r) != 1 else ''} ({group}): {why}: live "
              f"{hexs(4)}, playing {hexs(5)}, seek {hexs(6)}")
    if len(runs) > 200:
        print(f"  ... and {len(runs) - 200} runs more")
    if stock_secondary_cosmetics:
        print(f"  accepted {accepted} native cosmetic-pool bytes for stock-secondary-orders; "
              "all other bytes checked")
    return 1 if fails else 0


# SIMF kinds whose bytes a recorded table's image does not hold as a plain value
(SIMF_ID, SIMF_ADDR, SIMF_OWNED, SIMF_LOCAL, SIMF_COUNT, SIMF_COLUMN, SIMF_TARGET, SIMF_PINNED,
 SIMF_OPAQUE) = range(9)
SCAN_NOT_PLAIN = (SIMF_ID, SIMF_ADDR, SIMF_OWNED, SIMF_LOCAL, SIMF_COLUMN, SIMF_OPAQUE)
# the image records' places in their image (tagpu_recfmt.h)
SCAN_AT = {"unit": ("unit", 0), "mover": ("unit", 0x118), "player": ("player", 0),
           "info": ("player", 0x14C), "proj": ("proj", 0)}


def scan_ranges(h, img, dll=True):
    """The address ranges a plain value must not fall in, from a dump's own images: (lo, hi, what).
    Without `dll`, the engine's own ranges only."""
    main_img, data = img[0], img[1]
    d32 = lambda at: struct.unpack_from("<I", data, at - REC_DATA_BEGIN)[0]
    m32 = lambda off: struct.unpack_from("<I", main_img, off)[0]
    base = d32(0x511DE8)
    r = [(base, base + REC_MAIN_BYTES, "the main block", 1)]
    first, last = m32(0x14357), m32(0x1435B)
    if first and last >= first:
        r.append((first, last + 0x118, "the unit array", 0x118))
    defs, ndefs = m32(0x1439B), m32(0x1438F)
    if defs and ndefs < 0x10000:
        r.append((defs, defs + ndefs * 0x249, "the unit defs", 0x249))
    proj = m32(0x141F7)
    if proj and "projectiles" in h:
        r.append((proj, proj + h["projectiles"] * 0x6B, "the projectile pool", 0x6B))
    gw, gh = m32(0x14233), m32(0x14237)
    grid = m32(0x14287)
    if grid and gw < 0x10000 and gh < 0x10000:
        r.append((grid, grid + gw * gh * REC_CELL_BYTES, "the feature grid", REC_CELL_BYTES))
    wrecks = m32(0x1420B)
    if wrecks and "features" in h:
        r.append((wrecks, wrecks + h["features"][2] * REC_WRECK_BYTES, "the wreck pool",
                  REC_WRECK_BYTES))
    mapped = m32(0x14273)
    if mapped and gw < 0x10000 and gh < 0x10000:
        r.append((mapped, mapped + mapped_bytes(gw, gh), "the explored map", 2))
    if dll and "dll" in h:
        r.append((h["dll"][0], h["dll"][0] + h["dll"][1], "Impure's DLL", 1))
    return [x for x in r if x[0] < x[1]]


def scan_hit(ranges, v):
    """Where `v` points, or None. Into a pool of records only at a record's start, which is where
    the engine's pointers to its records point: an aligned dword that straddles two u16 fields
    (an angle and a position's half) lands inside a pool's range far more often than at a
    record's first byte."""
    for lo, hi, what, stride in ranges:
        if lo <= v < hi:
            if stride > 1:
                return None if (v - lo) % stride else f"at {what}'s row {(v - lo) // stride}"
            return f"inside {what} (+0x{v - lo:X})"
    return None


def plain_dwords(size, excluded):
    """The aligned dwords of an image `size` bytes long that hold no excluded byte."""
    return [at for at in range(0, size - 3, 4) if not any(b in excluded for b in range(at, at + 4))]


def cmd_scan(path, dump):
    """THE ADDRESS-SHAPED-VALUE SCAN: a plain value in a dump that lies inside the main block, the
    unit array, the unit defs, the projectile pool, the feature grid, the wreck pool, the explored
    map or Impure's DLL is an address recorded as a value -- a recorded table's fields but its ID,
    ADDR, OWNED, LOCAL, COLUMN and OPAQUE ones (each aligned dword of a table without a layout),
    and the globals image against the engine's ranges only, less its `opaque` slots (a dump made
    with another replay-globals.tsv is scanned whole). A value that merely looks like one
    fails too: the line says where it points. The unit, player and projectile images' bytes no
    replay-layout.tsv row names are tested against the engine's ranges and reported."""
    h = header_only(path)
    dh, w = dump_world(dump, h)
    tick, _, img = dump_images(dump)
    ranges = scan_ranges(dh, img)
    engine = scan_ranges(dh, img, dll=False)
    lines = []
    for ix, t in sorted(w.tables.items()):
        info = dh["tables"].get(ix, {})
        stride, name = info.get("stride"), info.get("name", f"table {ix}")
        if not stride:
            continue
        excluded = set()
        for off, size, rep, step, kind, _ in dh.get("fields", {}).get(ix, []):
            if kind in SCAN_NOT_PLAIN:
                for k in range(rep or 1):
                    excluded.update(range(off + k * step, off + k * step + size))
        dw = plain_dwords(stride, excluded)
        for r in range(len(t) // stride):
            for at in dw:
                v = struct.unpack_from("<I", t, r * stride + at)[0]
                hit = v and scan_hit(ranges, v)
                if hit:
                    lines.append(f"table {name} row {r} +0x{at:02X} holds 0x{v:08X}, {hit}")
    gen = _generator("replay-layout-gen")
    with open(gen.TSV, encoding="utf-8") as f:
        lrows = gen.parse(f.read())
    excl = {"unit": set(), "player": set(), "proj": set()}
    report = []
    for r in lrows:
        if r["record"] in SCAN_AT:
            img_name, at = SCAN_AT[r["record"]]
            excl[img_name].update(range(at + r["offset"], at + r["offset"] + r["size"]))
    for what, recs, size in (("unit", w.units, REC_UNIT_IMAGE),
                             ("player", dict(enumerate(w.players)), REC_PLAYER_IMAGE),
                             ("projectile", {i: w.proj[i * REC_PROJ_IMAGE:(i + 1) * REC_PROJ_IMAGE]
                                             for i in range((w.phead[1] if w.phead else 0))},
                              REC_PROJ_IMAGE)):
        dw = plain_dwords(size, excl["proj" if what == "projectile" else what])
        for k, b in sorted(recs.items()):
            for at in dw:
                if at + 4 > len(b):
                    continue
                v = struct.unpack_from("<I", b, at)[0]
                hit = v and scan_hit(engine, v)
                if hit:
                    report.append(f"{what} {k} +0x{at:03X} holds 0x{v:08X}, {hit}")
    if w.globals is not None:
        gnames = known_names(h) or {}
        opaque = globals_opaque(h)
        for i in range(0, len(w.globals) - 3, 4):
            if i // 4 in opaque:
                continue
            v = struct.unpack_from("<I", w.globals, i)[0]
            hit = v and scan_hit(engine, v)
            if hit:
                lines.append(f"globals {gnames.get(i // 4, f'dword {i // 4}')} holds 0x{v:08X}, {hit}")
    print(f"{dump}: GameTime {tick}: {len(ranges)} address ranges: "
          + ("no address recorded as a value" if not lines else
             f"{len(lines)} VALUES THAT ARE ADDRESSES")
          + f"; {len(report)} engine-record values that point somewhere (reported)")
    for line in lines[:100]:
        print("  " + line + ": an address recorded as a value")
    if len(lines) > 100:
        print(f"  ... and {len(lines) - 100} more")
    # The engine's records and the globals image are tested against the engine's own ranges: stock
    # code stores no address of Impure's DLL there, and their aligned dwords straddle pairs of u16
    # (angles, a position's halves) whose value lands in the DLL's 77 MB about 1.8 % of the time.
    # The records are reported, not failed: replay-layout.tsv's rows are their audited pointers.
    for line in report[:20]:
        print("  (report) " + line)
    return 1 if lines else 0


def main(argv):
    import devpy
    devpy.ensure()
    timeline = 0
    stock_secondary_cosmetics = "--stock-secondary-cosmetics" in argv
    if stock_secondary_cosmetics:
        if argv[1:2] != ["images-gate"]:
            print("--stock-secondary-cosmetics requires images-gate", file=sys.stderr)
            return 2
        argv = [a for a in argv if a != "--stock-secondary-cosmetics"]
    rewind = argv[1:2] == ["compare-counts"] and "--rewind" in argv
    if rewind:
        argv = [a for a in argv if a != "--rewind"]
    cob_exact = argv[1:2] == ["compare-counts"] and "--cob-exact" in argv
    if cob_exact:
        argv = [a for a in argv if a != "--cob-exact"]
    takeover = argv[1:2] == ["compare-dumps"] and "--takeover" in argv
    if takeover:
        argv = [a for a in argv if a != "--takeover"]
    seek = "--seek" in argv
    if seek:
        if argv[1:2] != ["compare-dumps"] and argv[1:3] != ["compare", "--puppet"]:
            print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
            return 2
        argv = [a for a in argv if a != "--seek"]
    exact = "--exact" in argv
    if exact:
        if argv[1:2] not in (["check"], ["compare"]):
            print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
            return 2
        argv = [a for a in argv if a != "--exact"]
    to = None
    if "--to" in argv:
        k = argv.index("--to")
        if argv[1:2] != ["lossy-check"] or k + 1 >= len(argv) or not argv[k + 1].isdigit():
            print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
            return 2
        to = int(argv[k + 1])
        argv = argv[:k] + argv[k + 2:]
    if "--timeline" in argv:
        k = argv.index("--timeline")
        if argv[1:2] not in (["check"], ["compare"], ["lossy-check"], ["compare-dumps"]) or \
                k + 1 >= len(argv) or \
                not argv[k + 1].isdigit():
            print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
            return 2
        timeline = int(argv[k + 1])
        argv = argv[:k] + argv[k + 2:]
    puppet = argv[2:3] == ["--puppet"] and argv[1:2] == ["compare"]
    images = argv[2:3] == ["--images"] and argv[1:2] == ["compare"]
    if puppet or images:
        argv = argv[:2] + argv[3:]
    absent = None
    if argv[1:2] == ["compare-counts"] and len(argv) == 9 and argv[7] == "--absent":
        absent, argv = argv[8], argv[:7]
    pov_check = argv[1:2] == ["pov"] and "--check" in argv
    if pov_check:
        argv = [a for a in argv if a != "--check"]
    pov_eye = None
    if argv[1:2] == ["pov"] and "--eye" in argv:
        k = argv.index("--eye")
        try:
            n = int(argv[k + 1])
            if argv[k + 2] != "--viewer":
                raise ValueError
            view = tuple(int(v) for v in argv[k + 3].split(","))
            rest = argv[k + 4:]
            seat = None
            if rest[:1] == ["--seat"]:
                seat, rest = int(rest[1]), rest[2:]
            if rest or len(view) != 2 or n < 0 or seat is not None and not 0 <= seat < REC_PLAYERS:
                raise ValueError
        except (ValueError, IndexError):
            print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
            return 2
        pov_eye, argv = (n, view, seat), argv[:k]
    if len(argv) < 3 or argv[1] not in ("info", "check", "compare", "compare-dumps", "counts",
                                        "compare-counts", "images-gate", "scan", "segments",
                                        "compare-fork", "pov", "lossy-check") or \
            (argv[1] in ("pov", "lossy-check") and len(argv) != 3) or \
            (argv[1] == "segments" and len(argv) != 3) or \
            (argv[1] == "compare-fork" and (len(argv) != 4 or not argv[3].isdigit())) or \
            (argv[1] == "compare" and len(argv) < 4) or (images and len(argv) != 4) or \
            (argv[1] == "compare-dumps" and len(argv) != 5) or \
            (argv[1] in ("counts", "scan") and len(argv) != 4) or \
            (argv[1] == "images-gate" and len(argv) < 11) or \
            (argv[1] == "compare-counts" and len(argv) != 7):
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        if argv[1] == "info":
            return cmd_info(argv[2])
        if argv[1] == "check":
            return cmd_check(argv[2], timeline, exact)
        if argv[1] == "lossy-check":
            return cmd_lossy_check(argv[2], timeline, to)
        if argv[1] == "segments":
            return cmd_segments(argv[2])
        if argv[1] == "pov" and pov_eye:
            if pov_check:
                print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
                return 2
            return cmd_pov_eye(argv[2], *pov_eye)
        if argv[1] == "pov":
            return cmd_pov(argv[2], pov_check)
        if argv[1] == "compare-fork":
            return cmd_compare_fork(argv[2], int(argv[3]))
        if images:
            return cmd_images(argv[2], argv[3])
        if argv[1] == "compare-dumps":
            return cmd_compare_dumps(argv[2], argv[3], argv[4], takeover, seek, timeline)
        if argv[1] == "images-gate":
            return cmd_images_gate(argv[2], (argv[3], argv[4]), (argv[5], argv[6]),
                                   (argv[7], argv[8]), argv[9], argv[10], argv[11:],
                                   stock_secondary_cosmetics)
        if argv[1] == "scan":
            return cmd_scan(argv[2], argv[3])
        if argv[1] == "counts":
            return cmd_counts(argv[2], argv[3])
        if argv[1] == "compare-counts":
            return cmd_compare_counts(argv[2], *argv[3:7], absent or str(
                Path(__file__).resolve().parents[2] / "tagpu" / "ddraw" / "replay-absent.tsv"), rewind,
                cob_exact)
        return cmd_compare(argv[2], argv[3:], puppet, timeline, exact, seek)
    except (Unreadable, OSError, struct.error, KeyError) as e:
        print(f"tarec: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    # reccodec imports this module as `tarec`: run as a script it is __main__, and a second copy
    # would raise an Unreadable that main() does not catch
    sys.modules.setdefault("tarec", sys.modules[__name__])
    sys.exit(main(sys.argv))
