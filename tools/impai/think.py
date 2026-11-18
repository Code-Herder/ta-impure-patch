#!/usr/bin/env python3
"""think.py -- what ImpAI held in mind at any tick of a recorded game, read from the recording alone.

    tools/impai/think.py state    REC [--seat S] --at T [--part mil,brain,job,known] [--fields PATH,...]
    tools/impai/think.py timeline REC [--seat S] --field PATH[,PATH...] [--from T] [--to T]
    tools/impai/think.py squads   REC [--seat S] [--from T] [--to T]
    tools/impai/think.py units    REC [--every N]
    tools/impai/think.py diff     REC_A REC_B [--seat S] [--part ...]
        each: [--tree DIR]... [--max M]
    tools/impai/think.py why      REC [--seat S] (--at T | --want NAME | both) [--from T] [--to T] [--log FILE]

A game's recording (`.imprec`) carries, exact at every tick, the four regions ImpAI's brain keeps its state in
(tagpu_impai.c): s_mil and s_brain, a row a seat (IMPAI_MIL, IMPAI_BRAIN), and s_job and s_known, a row a unit slot
(IMPAI_JOB; IMPAI_ROW, of which a seat reads its own IMPAI_SEEN). So "what was the AI doing at tick T" needs no
log line and no rebuilt DLL: one call here answers it from a game already played.

    state      the regions at GameTime T, after that tick's writes. A struct's scalars are packed on lines, an
               array of structs is a line an element that is not all zero, and of s_job and s_known the seat's
               rows, the commonest row once with its count. --fields prints the named fields alone, one a line
    timeline   the named fields' values at each tick where one changes (the tick's writes compared, so a value
               written again unchanged is no line); --from T starts from the values at T
    squads     each squad's tasks in the order they began: the ticks it held the task, its layer, its size and
               value over that time, its goal block and target slot
    units      the units each seat holds alive (frames under construction included), every N ticks and at the
               recording's last tick. A harness that ends a game by a scenario (bar.py's end) clears the level
               in the last ticks: a game's end is its result's end_tick, and the row there
    diff       the first tick at which the seat's state differs between two recordings of one game, and the
               fields that differ there with both values: equal at every tick before it. Exit 1 when they differ

    why        why a want of the economy's was or was not taken, from THE REASON TRACE (below), which the
               game's log holds and its recording does not. --at T: the gates up at tick T, the posture, and each
               of the six wants -- not needed; needed and taken; needed and VETOED, by which gates; needed, held
               by no gate and not taken (the term its rule reads last) -- each with the ticks it stands,
               from..until. --want NAME (`want_factory` or `factory`): that want's history through the
               game, change by change, under a line of its totals in ticks; with --at as well, the want's line
               at T first

A TICK is the recording's: tick T is the world as GameTime T finds it, so it holds what the step from GameTime
T - 1 wrote. A decision the log stamps `t=6960` therefore first shows at tick 6961 (MEASURED 2026-11-17 on the
exploiter games: the log's `mil ... t=6960 attack=1`, `plant ... gametime=1972` and `second ... gametime=8587`
against the ticks mil.wave_t and brain.nway change at, 6961, 1973 and 8588); `diff` names both.

A FIELD IS NAMED AS C NAMES IT, from its region: `mil.naval`, `mil.sq[3].task`, `brain.sea_refused`; a row of the
slot tables by its slot, `job[1512].kind`, `known[3002].how`. A path that names a struct or an array names every
scalar under it (`mil.sq[3]`, `brain.fmem`), `[*]` every element (`mil.sq[*].task`), and `job.kind` or
`job[*].kind` every row of the seat's. A value the header names is printed by that name: an enum's enumerators
less their common prefix (ENUM_OF), a flag byte's macros (BITS_OF).

A UNIT TYPE IS PRINTED BY ITS NAME with its number, `CORCOM(167)`, in the members that hold one (TYPE_OF), when
the content file predfit.py writes (CONTENT, .data/impai/fight-content.json) lists as many types as the
recording's header counts unit definitions: the count is all that ties the file to the recording, so a file of
another mod with the same count names wrongly, and the number printed beside the name is what to check by. No
file, or another count: the number alone.

THE LAYOUT IS THE COMPILER'S (thinklayout.py), made from the headers of a source tree and kept by their hash;
no offset is written here. A recording whose regions' row sizes are not that tree's structs' was made by
another layout and is REFUSED by name, never misread: --tree DIR names the source tree that made it (a worktree
at its commit), and may be given more than once, each recording read by the first tree it fits (this one
first). Sizes are what is compared: a layout of the same sizes with members moved is not seen, so every print's
first line carries the recording's build id for the reader to hold against the tree. `diff` across two layouts
compares the fields both name and lists those only one does.

RECORDED DATA IS DATA. Every span is held inside its table before it is applied, every row inside the table
before it is read, and a seat or a slot named on the command line inside the recording's own counts; a
recording that cannot be read, or a region it does not carry, is a message and exit 2.

THE EXTRACT. Reading a recording's stream costs seconds (tarec.py decodes every record of the game), so the
first call on a recording keeps what this tool reads -- every write to the four regions with its tick, and
each unit's birth and death -- under .data/impai/think-cache/ in the main checkout, by the SHA-256 of the
recording's bytes and of the reader's sources; later calls replay that. An entry that does not check out is
made again.

THE REASON TRACE (tagpu_impai.c, THE WHY LINE; impai.h, IMPAI_WHY). A decision's gates, its posture and what
it held of each want are scratch of the decision: no recorded struct holds them. The DLL writes them to the
game's log instead, `impai: why seat= t= gates= naval= pos= need= veto= take=`, at each economy decision whose
trace differs from the seat's last line, so the last line before a tick states every decision up to the next
line. `why` reads the log beside the recording, `<name>.tagpu.log` for `<name>.imprec` as the tools' result
folders keep the two, or --log FILE. Its ticks are the recording's: a line stamped `t=N` stands from tick N + 1.
A log whose GameTimes go back for the seat (a second level, a rewind) is refused by name, and a log with no
`why` line was written by a build before the trace. Under the wants `why --at T` quotes the seat's last
`impai: plantwalk` line at or before the tick, when the log holds one: what the plant rung met when it last
ordered no plant (THE PLANTWALK LINE).

--seat is the recording's one ImpAI seat when it has exactly one. The print is held to --max lines (default
60); its last line then says how many were cut. Exit 0, 1 (`diff`: the states differ), 2 (could not run)."""
import argparse
import array
import bisect
import difflib
import hashlib
import json
import os
import re
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import diag  # noqa: E402
import thinklayout  # noqa: E402

CACHE = diag.DATA / "impai" / "think-cache"
MAX_LINES = 60
PARTS = ("mil", "brain", "job", "known")
TABLES = {part: f"src/tagpu_impai.c:{name}" for part, name in thinklayout.REGIONS.items()}
BY_SEAT = ("mil", "brain")      # a row a seat; the others a row a unit slot
SEATS = 10
EXTRACT_MAGIC = b"THINKREC1\n"
TABLE_MAX = 1 << 28             # bytes a recorded table size may ask for
WIDTH = 118
# the header's names for a field's values, (struct, member): an enumerator of the enum it holds ...
ENUM_OF = {("IMPAI_SQUAD", "task"): "IMPAI_SQ_FREE", ("IMPAI_SQUAD", "layer"): "TASIM_LAYER_LAND",
           ("IMPAI_GOALFAIL", "layer"): "TASIM_LAYER_LAND", ("IMPAI_JOB", "kind"): "IMPAI_JOB_NONE",
           ("IMPAI_MIL", "level"): "IMPAI_LV_EASY", ("IMPAI_MIL", "est_seen"): "EST_START",
           ("IMPAI_PEND", "src"): "IMPAI_SRC_ECO"}
# ... or the macros of its bits
BITS_OF = {("IMPAI_SEEN", "how"): ("IMPAI_OWN", "IMPAI_LOS", "IMPAI_SHARED", "IMPAI_RADAR", "IMPAI_SONAR",
                                   "IMPAI_GHOST")}
# ... or a unit type's index in ImpAI's tables (A UNIT TYPE IS PRINTED BY ITS NAME)
TYPE_OF = {("IMPAI_SEEN", "type"), ("IMPAI_JOB", "type"), ("IMPAI_JOB", "build"), ("IMPAI_JOB", "far_type"),
           ("IMPAI_JOB", "help_type"), ("IMPAI_ENEMY", "type"), ("IMPAI_MIL", "radar_as"), ("IMPAI_PEND", "type"),
           ("IMPAI_WAYREC", "type"), ("IMPAI_WAYFAIL", "type"), ("IMPAI_BRAIN", "sea_none_type"),
           ("IMPAI_BRAIN", "sea_cur_type"), ("IMPAI_BRAIN", "unlock"), ("IMPAI_BRAIN", "unlock_plant")}
CONTENT = diag.DATA / "impai" / "fight-content.json"
FMT = {("u", 1): "<B", ("u", 2): "<H", ("u", 4): "<I", ("u", 8): "<Q", ("i", 1): "<b", ("i", 2): "<h",
       ("i", 4): "<i", ("i", 8): "<q", ("f", 4): "<f", ("f", 8): "<d", ("b", 1): "<B", ("p", 4): "<I"}
PATH = re.compile(r"^(\w+)(?:\[(\d+|\*)\])?(?:\.(.+))?$")


class ThinkError(Exception):
    pass


# ---- the layout ----

def short_names(group: dict) -> dict:
    """{value: name} of one C enum's {name: value}, each name less the prefix its enumerators share to an
    underscore; the largest, a count like IMPAI_SQ_TASKS, does not vote on the prefix."""
    names = sorted(group, key=lambda n: group[n])
    voters = names[:-1] if len(names) > 2 else names
    prefix = os.path.commonprefix(voters)
    prefix = prefix[:prefix.rfind("_") + 1]
    out = {}
    for n in names:
        out.setdefault(group[n], n[len(prefix):] if prefix and n.startswith(prefix) and n != prefix else n)
    return out


class Field:
    """One scalar of a region's row: its name from the row, where it is and how it is printed."""
    __slots__ = ("name", "off", "size", "st", "kind", "names", "bits")

    def __init__(self, name, off, node, names, bits):
        self.name, self.off, self.size, self.kind = name, off, node.get("size", 0), node["k"]
        fmt = FMT.get(("i" if node.get("signed") else "u", self.size) if self.kind == "e" else (self.kind, self.size))
        self.st = struct.Struct(fmt) if fmt else None
        self.names, self.bits = names, bits

    def value(self, raw: bytes):
        if self.st is None:
            raise ThinkError(f"{self.name}: a field no value can be read of ({self.kind}, {self.size} bytes)")
        return self.st.unpack(raw)[0]

    def show(self, raw: bytes) -> str:
        v = self.value(raw)
        if self.names is not None:
            return self.names.get(v, str(v))
        if self.bits is not None:
            on = [n for bit, n in self.bits if v & bit]
            rest = v & ~sum(bit for bit, _n in self.bits)
            return "|".join(on + ([f"{rest:#x}"] if rest else [])) or "0"
        if self.kind == "f":
            return f"{v:.6g}"
        return f"{v:#x}" if self.kind == "p" else str(v)


class Layout:
    """A source tree's layout of the four regions (thinklayout.load's dict), with each field's printing."""

    def __init__(self, lay: dict, tree):
        self.lay, self.tree = lay, tree
        self.types = lay["types"]
        self.size = {p: self.types[lay["roots"][p]]["size"] for p in PARTS}
        self.ctype = lay.get("ctypes", {})
        self.macros = lay.get("macros", {})
        self.unit_names = {}
        self._fields = {}

    def name_types(self, names: list):
        """Print the members of TYPE_OF by `names` (type_names), index by index; [] prints numbers. Index 0,
        the engine's reserved `None`, stays 0: it is what an unused member holds."""
        self.unit_names = {i: f"{n}({i})" for i, n in enumerate(names) if i}
        self._fields = {}

    def naming(self, owner, node):
        """(the {value: name} a field of `owner` (struct, member) prints by, its (bit, name) list): each None for
        a plain number."""
        if node["k"] == "e":
            group = next((g for g in self.lay["enums"] if set(g.values()) == {int(v) for v in node["names"]}
                          and set(g) == set(node["names"].values())), None)
            return (short_names(group) if group else {int(v): n for v, n in node["names"].items()}), None
        if owner in ENUM_OF:
            group = next((g for g in self.lay["enums"] if ENUM_OF[owner] in g), None)
            return (short_names(group) if group else None), None
        if owner in BITS_OF:
            have = [n for n in BITS_OF[owner] if n in self.macros]
            prefix = os.path.commonprefix(have) if len(have) > 1 else ""
            prefix = prefix[:prefix.rfind("_") + 1]
            return None, [(self.macros[n], n[len(prefix):]) for n in have]
        if owner in TYPE_OF and self.unit_names:
            return self.unit_names, None
        return None, None

    def fields(self, part: str) -> list:
        """Every scalar of `part`'s row, in offset order."""
        if part not in self._fields:
            self._fields[part] = sorted(fields_of(self, self.lay["roots"][part], "", 0, ("", "")),
                                        key=lambda f: f.off)
        return self._fields[part]

    def seat_fields(self, part: str, seat: int) -> list:
        """`part`'s scalars as the seat reads them: of s_known's row its own IMPAI_SEEN alone, named from it."""
        if part != "known":
            return self.fields(part)
        key = ("known", seat)
        if key not in self._fields:
            head = f"seat[{seat}]."
            out = []
            for f in self.fields("known"):
                if f.name.startswith(head):
                    g = Field.__new__(Field)
                    for s in Field.__slots__:
                        setattr(g, s, getattr(f, s))
                    g.name = f.name[len(head):]
                    out.append(g)
            if not out:
                raise ThinkError(f"{self.ctype.get('known', 's_known')} has no seat[{seat}]")
            self._fields[key] = out
        return self._fields[key]


def layouts(trees: list, cache=None) -> list:
    """The layouts a call reads by: this tree's, then each --tree's."""
    out = []
    for t in [TREE] + [Path(t) for t in trees or []]:
        try:
            out.append(Layout(thinklayout.load(t, CACHE if cache is None else cache), t))
        except thinklayout.LayoutError as e:
            raise ThinkError(str(e)) from None
    return out


def fit(ex, lays: list) -> Layout:
    """The first of `lays` whose structs are the sizes of `ex`'s recorded rows; ThinkError naming each region
    that fits none."""
    misses = []
    for lay in lays:
        bad = [(p, ex.stride[p], lay.size[p]) for p in PARTS if p in ex.stride and ex.stride[p] != lay.size[p]]
        if not bad:
            return lay
        misses.append("; ".join(f"its {thinklayout.REGIONS[p]} rows are {got} bytes, {lay.ctype.get(p, p)} is "
                                f"{want} in {lay.tree}" for p, got, want in bad))
    raise ThinkError(f"{ex.name} (build {ex.build}) was made by another layout and is refused: "
                     + " | ".join(misses) + " -- name the source tree that made it with --tree DIR")


# ---- the extract ----

class Spans:
    """One region's writes in stream order: the tick, the offset and the length of each, their bytes end to
    end in `blob`; a length of -1 is the table's new size, in `offs`."""

    def __init__(self):
        self.ticks, self.offs, self.lens = array.array("I"), array.array("I"), array.array("i")
        self.blob = bytearray()


class Extract:
    """What think.py reads of one recording (THE EXTRACT)."""

    def __init__(self, name: str, head: dict):
        self.name = name
        self.build, self.map = str(head.get("build", "?")), str(head.get("map", "?"))
        self.slots, self.each = int(head.get("slots") or 0), int(head.get("units_each") or 0)
        self.defs = int((head.get("unitdefs") or [0])[0] or 0)     # the unit definitions the header counts
        self.players = [{k: p.get(k) for k in ("index", "name", "ai", "side")} for p in head.get("players", [])]
        self.stride = {p: int(s) for p, s in (head.get("stride") or {}).items() if p in PARTS and int(s) > 0}
        self.stop = head.get("stop")
        self.end = 0
        self.parts = {p: Spans() for p in self.stride}
        self.uticks, self.uslots, self.utypes = array.array("I"), array.array("I"), array.array("H")
        self.uborn = bytearray()
        self._live = {}
        self._tick = 0

    # the sink a reading of the stream writes to (stream)
    def tick(self, t: int):
        self._tick = self.end = int(t)

    def size(self, part: str, n: int):
        s = self.parts.get(part)
        if s is not None:
            s.ticks.append(self._tick)
            s.offs.append(n)
            s.lens.append(-1)

    def span(self, part: str, off: int, data: bytes):
        s = self.parts.get(part)
        if s is not None and data:
            s.ticks.append(self._tick)
            s.offs.append(off)
            s.lens.append(len(data))
            s.blob += data

    def unit(self, slot: int, typ: int):
        if self._live.get(slot) != typ:
            self._live[slot] = typ
            self._unit(slot, typ, 1)

    def gone(self, slot: int):
        if self._live.pop(slot, None) is not None:
            self._unit(slot, 0, 0)

    def _unit(self, slot, typ, born):
        self.uticks.append(self._tick)
        self.uslots.append(slot)
        self.utypes.append(typ & 0xFFFF)
        self.uborn.append(born)

    def seat_of(self, slot: int):
        """The seat that owns unit slot `slot`: seat k's block is slots 1 + k x units_each on (MEASURED
        2026-11-17 on the exploiter games' recordings against the player records' unit blocks, +0x67 and +0x6B
        as IDs, an ID a slot + 1); None for slot 0 and past the seats."""
        if slot < 1 or self.each < 1 or (slot - 1) // self.each >= SEATS:
            return None
        return (slot - 1) // self.each

    def impai_seats(self) -> list:
        return [p["index"] for p in self.players if p.get("ai") == 1]

    def seat_label(self, seat: int) -> str:
        p = next((p for p in self.players if p["index"] == seat), None)
        if p is None:
            return f"seat {seat}"
        return f"seat {seat} ({p['name']}{', ImpAI' if p.get('ai') == 1 else ''})"

    def alive(self, every=None, ticks=None) -> tuple:
        """([tick, ...], {seat: [units alive at each tick's end, ...]}): at each multiple of `every` and at the
        last tick, or at `ticks`."""
        if ticks is None:
            ticks = sorted(set(range(every, self.end + 1, every)) | {self.end}) if every else [self.end]
        seats = sorted({p["index"] for p in self.players} | {s for s in map(self.seat_of, set(self.uslots))
                                                             if s is not None})
        count = {s: 0 for s in seats}
        out = {s: [] for s in seats}
        i, n = 0, len(self.uticks)
        for t in ticks:
            while i < n and self.uticks[i] <= t:
                s = self.seat_of(self.uslots[i])
                if s is not None:
                    count[s] += 1 if self.uborn[i] else -1
                i += 1
            for s in seats:
                out[s].append(count[s])
        return list(ticks), out

    def peak(self, seat: int) -> tuple:
        """(the most units `seat` held alive at once, the first tick it did)."""
        n = best = at = 0
        for i in range(len(self.uticks)):
            if self.seat_of(self.uslots[i]) == seat:
                n += 1 if self.uborn[i] else -1
                if n > best:
                    best, at = n, self.uticks[i]
        return best, at

    # the kept form: a JSON head, then each array's bytes in the head's order, deflated
    def pack(self) -> bytes:
        arrays = [("uticks", self.uticks), ("uslots", self.uslots), ("utypes", self.utypes)]
        blobs = [("uborn", self.uborn)]
        for p, s in self.parts.items():
            arrays += [(f"{p}.ticks", s.ticks), (f"{p}.offs", s.offs), (f"{p}.lens", s.lens)]
            blobs.append((f"{p}.blob", s.blob))
        head = {"byteorder": sys.byteorder, "build": self.build, "map": self.map, "slots": self.slots,
                "units_each": self.each, "unitdefs": [self.defs], "players": self.players, "stride": self.stride, "stop": self.stop,
                "end": self.end, "arrays": [[n, a.typecode, a.itemsize, len(a)] for n, a in arrays],
                "blobs": [[n, len(b)] for n, b in blobs]}
        js = json.dumps(head).encode()
        return zlib.compress(b"".join([EXTRACT_MAGIC, struct.pack("<I", len(js)), js]
                                      + [a.tobytes() for _n, a in arrays] + [bytes(b) for _n, b in blobs]), 1)

    @classmethod
    def unpack(cls, name: str, packed: bytes):
        """The extract `packed` holds; ValueError when it is not one whole extract in the reader's byte order."""
        raw = zlib.decompress(packed)
        if not raw.startswith(EXTRACT_MAGIC):
            raise ValueError("no extract")
        at = len(EXTRACT_MAGIC)
        (n,) = struct.unpack_from("<I", raw, at)
        head = json.loads(raw[at + 4:at + 4 + n])
        at += 4 + n
        if head["byteorder"] != sys.byteorder:
            raise ValueError("another byte order's")
        ex = cls(name, head)
        ex.end = int(head["end"])
        got = {}
        for aname, code, itemsize, count in head["arrays"]:
            a = array.array(code)
            if a.itemsize != itemsize or at + itemsize * count > len(raw):
                raise ValueError("an array past the extract")
            a.frombytes(raw[at:at + itemsize * count])
            at += itemsize * count
            got[aname] = a
        for bname, count in head["blobs"]:
            if at + count > len(raw):
                raise ValueError("a blob past the extract")
            got[bname] = bytearray(raw[at:at + count])
            at += count
        if at != len(raw):
            raise ValueError("bytes after the extract")
        ex.uticks, ex.uslots, ex.utypes, ex.uborn = got["uticks"], got["uslots"], got["utypes"], got["uborn"]
        if not len(ex.uticks) == len(ex.uslots) == len(ex.utypes) == len(ex.uborn):
            raise ValueError("unit arrays of different lengths")
        for p in ex.parts:
            s = ex.parts[p]
            s.ticks, s.offs, s.lens, s.blob = got[f"{p}.ticks"], got[f"{p}.offs"], got[f"{p}.lens"], got[f"{p}.blob"]
            if not len(s.ticks) == len(s.offs) == len(s.lens) or sum(n for n in s.lens if n > 0) != len(s.blob):
                raise ValueError("a region's arrays do not agree")
        return ex


def stream(path: Path, sink) -> dict:
    """A recording's stream through tarec.py, timeline 0: each tick, each write to a region of TABLES and each
    unit's image or death given to `sink` (an Extract's tick, size, span, unit, gone) in stream order. Returns
    the head an Extract is made of; called twice, first with no sink for the head alone."""
    sys.path.insert(0, str(TREE / "tools" / "replay"))
    import tarec
    if sink is None:
        h = tarec.header_only(str(path))
        stride = {p: t["stride"] for p in PARTS for t in h["tables"].values() if t["name"] == TABLES[p]}
        return dict(h, stride=stride)
    h, bl, stop = tarec.load(str(path))
    part_of = {ix: p for p in PARTS for ix, t in h["tables"].items() if t["name"] == TABLES[p]}
    head, tick_of, u16 = struct.Struct("<HxxI").unpack_from, struct.Struct("<I").unpack_from, tarec._U16
    K_TICK, K_SIZE, K_SPAN, K_UNIT, K_GONE = (tarec.RK_TICK, tarec.RK_TABLE_SIZE, tarec.RK_TABLE_SPAN,
                                              tarec.RK_UNIT, tarec.RK_UNIT_GONE)
    type_at = 4 + tarec.U_TYPE

    def on_record(_w, kind, b):
        if kind == K_SPAN:
            ix, off = head(b)
            if ix in part_of:
                sink.span(part_of[ix], off, bytes(b[8:]))
        elif kind == K_TICK:
            sink.tick(tick_of(b)[0])
        elif kind == K_UNIT:
            sink.unit(u16(b)[0], u16(b, type_at)[0])
        elif kind == K_GONE:
            sink.gone(u16(b)[0])
        elif kind == K_SIZE:
            ix, size = head(b)
            if ix in part_of:
                sink.size(part_of[ix], size)
    tarec.stream_to(h, bl, on_record=on_record)
    return {"stop": stop}


def reader_digest() -> str:
    """The SHA-256 of the sources an extract is a function of, beside the recording's bytes."""
    h = hashlib.sha256(EXTRACT_MAGIC)
    for f in (HERE / "think.py", TREE / "tools" / "replay" / "tarec.py", TREE / "tools" / "replay" / "reccodec.py"):
        try:
            h.update(f.read_bytes())
        except OSError:
            h.update(f.name.encode())
    return h.hexdigest()


def extract(path, cache=None) -> Extract:
    """`path`'s extract, from THE EXTRACT's cache when it holds it."""
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as e:
        raise ThinkError(f"{path}: {e.strerror or e}") from None
    cache = Path(CACHE if cache is None else cache)
    entry = cache / f"rec-{hashlib.sha256(reader_digest().encode() + data).hexdigest()[:24]}.bin"
    try:
        return Extract.unpack(path.name, entry.read_bytes())
    except (OSError, ValueError, KeyError, TypeError, zlib.error, struct.error):
        pass
    try:
        # a recording is data: whatever its bytes make the reader raise is a message, never a traceback
        ex = Extract(path.name, stream(path, None))
        ex.stop = stream(path, ex).get("stop")
    except Exception as e:                                      # noqa: BLE001
        raise ThinkError(f"{path}: not a recording this tool reads ({type(e).__name__}: {e})") from None
    try:
        cache.mkdir(parents=True, exist_ok=True)
        part = entry.with_name(f"{entry.name}.{os.getpid()}.part")
        part.write_bytes(ex.pack())
        os.replace(part, entry)
    except OSError:
        pass                    # an extract that cannot be kept costs the next call the reading again
    return ex


def type_names(ex: Extract, content=None) -> list:
    """The unit types' names by their index in ImpAI's tables (index 0 the engine's reserved `None`), from the
    content file when it lists as many as `ex`'s header counts unit definitions (A UNIT TYPE IS PRINTED BY ITS
    NAME); [] otherwise. A unit image's type ID is this index + 1."""
    try:
        names = json.loads(Path(CONTENT if content is None else content).read_text())["names"]
    except (OSError, ValueError, KeyError, TypeError):
        return []
    if not isinstance(names, list) or not ex.defs or len(names) != ex.defs:
        return []
    return [str(n) for n in names]


class Image:
    """One region's table, advanced through its writes a tick at a time."""

    def __init__(self, ex: Extract, part: str):
        if part not in ex.parts:
            raise ThinkError(f"{ex.name} (build {ex.build}) carries no region {TABLES[part]}")
        self.ex, self.part, self.s = ex, part, ex.parts[part]
        self.stride = ex.stride[part]
        self.img = bytearray()
        self.i = self.at = 0

    def peek(self):
        """The tick of the next writes, None past the last."""
        return self.s.ticks[self.i] if self.i < len(self.s.ticks) else None

    def step(self) -> list:
        """Apply the next tick's writes; [(offset, length), ...] of what they wrote."""
        s, i, t = self.s, self.i, self.s.ticks[self.i]
        out = []
        while i < len(s.ticks) and s.ticks[i] == t:
            off, n = s.offs[i], s.lens[i]
            if n < 0:
                if off > TABLE_MAX:
                    raise ThinkError(f"{self.ex.name}: {TABLES[self.part]} sized {off} bytes at tick {t}")
                if off < len(self.img):
                    del self.img[off:]
                else:
                    self.img.extend(bytes(off - len(self.img)))
            else:
                if off + n > len(self.img) or self.at + n > len(s.blob):
                    raise ThinkError(f"{self.ex.name}: a write past {TABLES[self.part]}'s {len(self.img)} bytes "
                                     f"at tick {t}")
                self.img[off:off + n] = s.blob[self.at:self.at + n]
                self.at += n
                out.append((off, n))
            i += 1
        self.i = i
        return out

    def to(self, tick: int):
        while self.peek() is not None and self.peek() <= tick:
            self.step()
        return self

    def rows(self) -> int:
        return len(self.img) // self.stride

    def raw(self, row: int, f: Field):
        """The bytes of field `f` in row `row`; None for a row the table does not hold."""
        at = row * self.stride + f.off
        return bytes(self.img[at:at + f.size]) if 0 <= row and at + f.size <= len(self.img) else None


def seat_rows(ex: Extract, part: str, seat: int) -> range:
    """The rows of `part` that are `seat`'s: its own row, its unit block's, or of s_known every slot's."""
    if part in BY_SEAT:
        return range(seat, seat + 1)
    if part == "job":
        return range(1 + seat * ex.each, 1 + (seat + 1) * ex.each)
    return range(0, max(ex.slots, 0))


def pick_seat(ex: Extract, seat):
    if seat is None:
        ai = ex.impai_seats()
        if len(ai) != 1:
            raise ThinkError(f"{ex.name}: --seat S, one of its seats "
                             + ", ".join(ex.seat_label(p["index"]) for p in ex.players)
                             + f" ({len(ai)} played by ImpAI)")
        return ai[0]
    if not 0 <= seat < SEATS or not any(p["index"] == seat for p in ex.players):
        raise ThinkError(f"{ex.name}: no seat {seat}; its seats: "
                         + ", ".join(ex.seat_label(p["index"]) for p in ex.players))
    return seat


def label(part: str, row: int, f: Field) -> str:
    return f"{part}.{f.name}" if part in BY_SEAT else f"{part}[{row}].{f.name}"


class Selection:
    """The fields of one region a call reads, of the rows that are the seat's (or the one row named)."""

    def __init__(self, ex, lay, part, seat, fields=None, row=None):
        self.part, self.stride = part, ex.stride.get(part, 0)
        self.fields = sorted(lay.seat_fields(part, seat) if fields is None else fields, key=lambda f: f.off)
        self.rows = seat_rows(ex, part, seat) if row is None else range(row, row + 1)
        self.starts = [f.off for f in self.fields]

    def hits(self, touched: list, out: set):
        """Add to `out` the (row, index of the field) of every selected field the writes `touched` reach."""
        st, fields, starts, rows = self.stride, self.fields, self.starts, self.rows
        for off, n in touched:
            for row in range(max(off // st, rows.start), min((off + n - 1) // st + 1, rows.stop)):
                lo, hi = max(off - row * st, 0), min(off + n - row * st, st)
                k = bisect.bisect_right(starts, lo) - 1
                if k < 0 or fields[k].off + fields[k].size <= lo:
                    k += 1
                while k < len(fields) and starts[k] < hi:
                    out.add((row, k))
                    k += 1


def select(ex, lay, seat, paths: list) -> list:
    """The Selections `paths` name (A FIELD IS NAMED AS C NAMES IT), one a region and row choice."""
    out = []
    for path in paths:
        m = PATH.match(path.strip())
        if not m or m.group(1) not in PARTS:
            raise ThinkError(f"no field {path!r}: a path starts with one of {', '.join(PARTS)}")
        part, row, rest = m.groups()
        if part not in ex.stride:
            raise ThinkError(f"{ex.name} (build {ex.build}) carries no region {TABLES[part]}")
        if row is not None and part in BY_SEAT:
            raise ThinkError(f"{path}: {part} is the seat's one row; a slot names a row of job and known")
        if row not in (None, "*") and not 0 <= int(row) < max(ex.slots, 1):
            raise ThinkError(f"{path}: the recording has slots 0..{ex.slots - 1}")
        fields = lay.seat_fields(part, seat)
        if rest:
            pat = re.compile("^" + re.escape(rest).replace(r"\[\*\]", r"\[\d+\]") + r"(?:$|[.\[])")
            fields = [f for f in fields if pat.match(f.name)]
            if not fields:
                known = sorted({re.sub(r"\[\d+\]", "", f.name) for f in lay.seat_fields(part, seat)})
                near = difflib.get_close_matches(re.sub(r"\[[\d*]+\]", "", rest), known, 4, 0.5)
                raise ThinkError(f"no field {path} in {lay.ctype.get(part, part)}"
                                 + (f"; near it: {', '.join(f'{part}.{n}' for n in near)}" if near else ""))
        out.append(Selection(ex, lay, part, seat, fields, None if row in (None, "*") else int(row)))
    return out


def changes(ex, sels: list, t0=None, t1=None) -> tuple:
    """(the selected fields' values at t0 as {(part, row, field): bytes}, their changes after it to t1 as
    [(tick, part, row, field, bytes before, bytes after), ...] in tick order). A field never written reads as
    zeros, which is what the level's start leaves."""
    start, events = {}, []
    for sel in sels:
        img = Image(ex, sel.part)
        last = {}
        while img.peek() is not None and (t1 is None or img.peek() <= t1):
            t = img.peek()
            hit = set()
            sel.hits(img.step(), hit)
            for row, k in sorted(hit):
                f = sel.fields[k]
                raw = img.raw(row, f)
                old = last.get((row, k), bytes(f.size))
                if raw is not None and raw != old:
                    last[(row, k)] = raw
                    if t0 is not None and t <= t0:
                        continue
                    events.append((t, sel.part, row, f, old, raw))
                    start.setdefault((sel.part, row, f), old)
        if t0 is not None:
            for (row, k), raw in last.items():
                start.setdefault((sel.part, row, sel.fields[k]), raw)
    events.sort(key=lambda e: (e[0], PARTS.index(e[1]), e[2], e[3].off))
    return (start if t0 is not None else {}), events


# ---- the verbs ----

def capped(lines: list, most: int, how="--fields, --part, --from/--to") -> list:
    if len(lines) <= most:
        return lines
    return lines[:most - 1] + [f"think: {len(lines) - (most - 1)} more lines cut at --max {most} (narrow with {how}, "
                               f"or raise --max)"]


def headline(verb: str, ex: Extract, seat=None) -> str:
    who = f", {ex.seat_label(seat)}" if seat is not None else ""
    cut = f"; the file stops early: {ex.stop}" if ex.stop else ""
    return f"{verb}: {ex.name} (build {ex.build}, {ex.map}, ticks to {ex.end}{cut}){who}"


def packed(prefix: str, items: list) -> list:
    """`items` on lines of WIDTH at most, the first after `prefix`, the rest under it."""
    lines, cur = [], prefix
    for it in items:
        if len(cur) + 1 + len(it) > WIDTH and cur.strip() != prefix.strip():
            lines.append(cur)
            cur = " " * len(prefix)
        cur += (" " if cur.strip() else "") + it if cur != prefix else it
    return lines + [cur] if cur.strip() else lines


def is_pad(name: str) -> bool:
    """A member the header names as padding: `pad`, `pad2`, `eyed_pad`."""
    last = name.rsplit(".", 1)[-1]
    return last.startswith("pad") or last.endswith("_pad")


def inline(fields: list, raw: bytes, base: int) -> list:
    """`fields` of one struct image, which starts at offset `base` of theirs, as name=value items: an array's
    scalars, whatever its dimensions, as one item, zero padding left out."""
    out, i = [], 0
    while i < len(fields):
        f = fields[i]
        cut = f.name.find("[", f.name.rfind(".") + 1)
        if cut >= 0:
            stem, j = f.name[:cut], i
            while j < len(fields) and fields[j].name.startswith(stem + "[") and "." not in fields[j].name[cut:]:
                j += 1
            vals = [g.show(raw[g.off - base:g.off - base + g.size]) for g in fields[i:j]]
            i = j
            if is_pad(stem) and all(v == "0" for v in vals):
                continue
            nz = [(k, v) for k, v in enumerate(vals) if v != "0"]
            if len(vals) <= 16:
                out.append(f"{stem}=[{','.join(vals)}]")
            else:
                shown = " ".join(f"[{k}]={v}" for k, v in nz[:8])
                out.append(f"{stem}[{len(vals)}]: {len(nz)} not 0" + (f": {shown}" if nz else "")
                           + (f" +{len(nz) - 8} more" if len(nz) > 8 else ""))
            continue
        v = f.show(raw[f.off - base:f.off - base + f.size])
        i += 1
        if not (is_pad(f.name) and v == "0"):
            out.append(f"{f.name}={v}")
    return out


def struct_lines(lay: Layout, tid: str, raw: bytes, name: str) -> list:
    """One struct image as `state` prints it: its scalars and scalar arrays packed, each struct member under its
    own name, an array of structs its commonest element once and a line each other one."""
    node = lay.types[tid]
    plain, later = [], []
    for member, off, mt in node["m"]:
        mn = lay.types[mt]
        elem = lay.types[mn["e"]] if mn["k"] == "a" else None
        if mn["k"] == "s":
            later.append((member, off, mt, None))
        elif elem is not None and elem["k"] == "s":
            later.append((member, off, mt, elem))
        else:
            plain.append((member, off, mt))
    out = []
    fields = []
    for member, off, mt in plain:
        fields += fields_of(lay, mt, member, off, (node["name"], member))
    out += packed(f"{name}: ", inline(fields, raw, 0))
    for member, off, mt, elem in later:
        mn = lay.types[mt]
        if elem is None:
            out += struct_lines(lay, mt, raw[off:off + mn["size"]], f"{name}.{member}")
            continue
        # an unused element is rarely all zero (a slot of WQ_NONE), so the commonest image is printed once
        fs = fields_of(lay, mn["e"], "", 0, (node["name"], member))
        images = [raw[off + i * elem["size"]:off + (i + 1) * elem["size"]] for i in range(mn["n"])]
        common = max(set(images), key=images.count) if images else b""
        if images.count(common) > 1:
            out += packed(f"{name}.{member}[*] x{images.count(common)}: ",
                          inline(fs, common, 0) if any(common) else ["all zero"])
        else:
            common = None
        for i, img in enumerate(images):
            if img != common:
                out += packed(f"{name}.{member}[{i}]: ", inline(fs, img, 0))
    return out


def fields_of(lay: Layout, tid: str, name: str, at: int, owner: tuple) -> list:
    """The scalars under type `tid` standing at `at` under `name`, as Fields."""
    out = []

    def walk(t, nm, off, own):
        n = lay.types[t]
        if n["k"] == "s":
            for member, moff, mt in n["m"]:
                walk(mt, f"{nm}.{member}" if nm else member, off + moff, (n["name"], member))
        elif n["k"] == "a":
            es = lay.types[n["e"]]["size"]
            for i in range(n["n"]):
                walk(n["e"], f"{nm}[{i}]", off + i * es, own)
        else:
            names, bits = lay.naming(own, n)
            out.append(Field(nm, off, n, names, bits))
    walk(tid, name, at, owner)
    return out


def state(ex, lay, seat, at, parts, paths) -> list:
    out = [headline("state", ex, seat) + f", at tick {at}"]
    if not 0 <= at <= ex.end:
        raise ThinkError(f"{ex.name}: --at {at}: its ticks are 0..{ex.end}")
    if paths:
        sels = select(ex, lay, seat, paths)
        rows, zeros = [], 0
        for sel in sels:
            img = Image(ex, sel.part).to(at)
            for row in sel.rows:
                for f in sel.fields:
                    raw = img.raw(row, f)
                    if raw is not None:
                        rows.append((label(sel.part, row, f), f.show(raw), any(raw)))
        many = len(rows) > 20
        for name, v, nonzero in rows:
            if many and not nonzero:
                zeros += 1
            else:
                out.append(f"  {name} = {v}")
        if zeros:
            out.append(f"  {zeros} more fields are 0")
        return out
    for part in parts:
        img = Image(ex, part).to(at)
        tid = lay.lay["roots"][part]
        rows = [r for r in seat_rows(ex, part, seat) if r < img.rows()]
        if part in BY_SEAT:
            if not rows:
                out.append(f"{part}: the table holds no row of seat {seat} at this tick")
                continue
            at0 = rows[0] * img.stride
            out += struct_lines(lay, tid, bytes(img.img[at0:at0 + img.stride]), part)
            continue
        fields = lay.seat_fields(part, seat)
        lo, hi = fields[0].off, fields[-1].off + fields[-1].size
        seen = {}
        for r in rows:
            seen.setdefault(bytes(img.img[r * img.stride + lo:r * img.stride + hi]), []).append(r)
        if not seen:
            out.append(f"{part}: the table holds no row of the seat's at this tick")
            continue
        common = max(seen, key=lambda k: len(seen[k]))
        if len(seen[common]) > 1:
            out += packed(f"{part}[*] x{len(seen[common])}: ",
                          inline(fields, common, lo) if any(common) else ["all zero"])
        else:
            common = None
        for r, image in sorted((r, image) for image, rs in seen.items() if image != common for r in rs):
            out += packed(f"{part}[{r}]: ", inline(fields, image, lo))
    return out


def timeline(ex, lay, seat, paths, t0, t1) -> list:
    sels = select(ex, lay, seat, paths)
    start, events = changes(ex, sels, t0, t1)
    out = [headline("timeline", ex, seat) + f", {', '.join(paths)}"]
    if t0 is not None:
        vals = [f"{label(p, r, f)}={f.show(raw)}" for (p, r, f), raw in
                sorted(start.items(), key=lambda kv: (PARTS.index(kv[0][0]), kv[0][1], kv[0][2].off)) if any(raw)]
        out += packed(f"  at {t0}: ", vals[:24] + ([f"+{len(vals) - 24} more not 0"] if len(vals) > 24 else [])
                      or ["every field 0"])
    by_tick = {}
    for t, part, row, f, old, raw in events:
        by_tick.setdefault(t, []).append(f"{label(part, row, f)} {f.show(old)} -> {f.show(raw)}")
    for t, items in by_tick.items():
        for k in range(0, len(items), 4):
            out.append(f"  {t if k == 0 else '':>6}  " + "; ".join(items[k:k + 4]))
    if not events:
        out.append("  no change of these fields" + (f" after {t0}" if t0 is not None else "")
                   + (f" to {t1}" if t1 is not None else ""))
    return out


SQUAD_MEMBERS = ("task", "layer", "n", "tx", "tz", "target", "value")
LEGS_SHOWN = 4


def squad_episodes(ex, lay, seat, t0=None, t1=None) -> list:
    """Each squad's tasks: {squad, task (its name), layer, from, to (the tick it ended, None while it holds at
    the end), n (least, most), value (most), legs [(tick, (tx, tz), target slot or None), ...]}, in the order
    they began. A squad's task is one of these from the tick it takes it to the tick it takes another; each
    goal block or target it has in that time is a leg. A FREE squad has none."""
    want = {}
    for f in lay.fields("mil"):
        m = re.match(r"^sq\[(\d+)\]\.(\w+)$", f.name)
        if m:
            want.setdefault(int(m.group(1)), {})[m.group(2)] = f
    if not want:
        raise ThinkError(f"IMPAI_MIL has no sq[] in {lay.tree}")
    missing = [m for m in SQUAD_MEMBERS if m not in want[0]]
    if missing:
        raise ThinkError(f"IMPAI_SQUAD has no member {', '.join(missing)} in {lay.tree}: `squads` reads "
                         + ", ".join(SQUAD_MEMBERS))
    task_names = want[0]["task"].names or {}
    free = next((v for v, n in task_names.items() if n == "FREE"), 0)
    none = lay.macros.get("WQ_NONE", 0xFFFF)
    by_field = {f: (k, m) for k, ms in want.items() for m, f in ms.items() if m in SQUAD_MEMBERS}
    _start, events = changes(ex, [Selection(ex, lay, "mil", seat, list(by_field))], None, t1)
    cur = {k: {m: 0 for m in SQUAD_MEMBERS} for k in want}
    open_, done = {}, []
    ticks = {}
    for t, _part, _row, f, _old, raw in events:
        k, member = by_field[f]
        ticks.setdefault(t, {}).setdefault(k, {})[member] = f.value(raw)
    for t in sorted(ticks):
        for k, new in sorted(ticks[t].items()):
            s = cur[k]
            s.update(new)
            leg = ((s["tx"], s["tz"]), None if s["target"] == none else s["target"])
            ep = open_.get(k)
            if ep is not None and ep["task_value"] == s["task"]:
                ep["n"] = (min(ep["n"][0], s["n"]), max(ep["n"][1], s["n"]))
                ep["value"] = max(ep["value"], s["value"])
                if ep["legs"][-1][1:] != leg:
                    ep["legs"].append((t, *leg))
                continue
            if ep is not None:
                ep["to"] = t
                done.append(open_.pop(k))
            if s["task"] != free:
                open_[k] = {"task_value": s["task"], "squad": k, "task": task_names.get(s["task"], str(s["task"])),
                            "layer": want[k]["layer"].show(bytes([s["layer"] & 0xFF])), "from": t, "to": None,
                            "n": (s["n"], s["n"]), "value": s["value"], "legs": [(t, *leg)]}
    done += list(open_.values())
    done.sort(key=lambda e: (e["from"], e["squad"]))
    return [e for e in done if t0 is None or e["to"] is None or e["to"] > t0]


def squads(ex, lay, seat, t0, t1) -> list:
    eps = squad_episodes(ex, lay, seat, t0, t1)
    count = {}
    for e in eps:
        count[e["task"]] = count.get(e["task"], 0) + 1
    out = [headline("squads", ex, seat),
           "  " + (", ".join(f"{n} {t}" for t, n in count.items()) or "no squad ever left FREE")
           + "; a line a task: ticks held, squad, task, layer, members least..most, value at most, then each "
             "goal block (and target slot) from its tick"]
    for e in eps:
        n = f"{e['n'][0]}" if e["n"][0] == e["n"][1] else f"{e['n'][0]}..{e['n'][1]}"
        legs = []
        for t, goal, target in e["legs"]:
            if not any(goal) and target is None:
                continue
            owner = ex.seat_of(target) if target is not None else None
            legs.append(f"({goal[0]},{goal[1]})" + (f" slot {target}" if target is not None else "")
                        + (f" of seat {owner}" if owner is not None else "")
                        + (f" from {t}" if t != e["from"] else ""))
        shown = "; ".join(legs[:LEGS_SHOWN])
        if len(legs) > LEGS_SHOWN:
            shown += f"; +{len(legs) - LEGS_SHOWN} more goals"
        end = e["to"] if e["to"] is not None else "end"
        out.append(f"  {e['from']:>6}-{end:<6} sq[{e['squad']}] {e['task']:<8} {e['layer']:<5} n={n:<5} "
                   f"v={e['value']:<6.0f}" + (f" goal {shown}" if legs else ""))
    return out


def units(ex, every) -> list:
    if every is None:
        every = max(1000, -(-ex.end // 20 // 1000) * 1000)
    if every < 1:
        raise ThinkError("--every: at least one tick")
    ticks, alive = ex.alive(every)
    seats = [s for s in alive if any(alive[s]) or any(p["index"] == s and s for p in ex.players)]
    out = [headline("units", ex), "  alive at the end of each tick, frames included; "
           + ", ".join(ex.seat_label(s) for s in seats),
           "    tick " + " ".join(f"{'seat ' + str(s):>7}" for s in seats)]
    for i, t in enumerate(ticks):
        out.append(f"  {t:>6} " + " ".join(f"{alive[s][i]:>7}" for s in seats))
    out.append("  most at once: " + ", ".join("seat {} {} at {}".format(s, *ex.peak(s)) for s in seats))
    return out


def first_difference(ea, la, eb, lb, seat, parts):
    """(tick, [(field's name, a's value, b's value), ...]) at the first tick `seat`'s state differs between two
    recordings in the fields both layouts name, None when it never does; and {part: [names]} only one names."""
    best, only = None, {}
    for part in parts:
        fa, fb = la.seat_fields(part, seat), lb.seat_fields(part, seat)
        na, nb = {f.name: f for f in fa}, {f.name: f for f in fb}
        for side, mine, other in (("A", na, nb), ("B", nb, na)):
            gone = sorted({re.sub(r"[.\[].*$", "", n) for n in mine if n not in other})
            if gone:
                only[f"{part}, {side} alone"] = [f"{part}.{n}" for n in gone]
        sa = Selection(ea, la, part, seat, [f for f in fa if f.name in nb])
        sb = Selection(eb, lb, part, seat, [f for f in fb if f.name in na])
        ia, ib = Image(ea, part), Image(eb, part)
        while ia.peek() is not None or ib.peek() is not None:
            t = min(x for x in (ia.peek(), ib.peek()) if x is not None)
            if best is not None and t > best[0]:
                break
            hit = set()
            for img, sel in ((ia, sa), (ib, sb)):
                if img.peek() == t:
                    mine = set()
                    sel.hits(img.step(), mine)
                    hit |= {(row, sel.fields[k].name) for row, k in mine}
            found = []
            for row, name in sorted(hit):
                x, y = na[name], nb[name]
                ra, rb = ia.raw(row, x), ib.raw(row, y)
                same = ra == rb if x.size == y.size and x.kind == y.kind else (
                    ra is not None and rb is not None and x.value(ra) == y.value(rb))
                if not same:
                    found.append((label(part, row, x), x.show(ra) if ra is not None else "(no row)",
                                  y.show(rb) if rb is not None else "(no row)"))
            if found:
                best = (t, (best[1] if best and best[0] == t else []) + found)
                break
    return best, only


def diff(ea, la, eb, lb, seat, parts) -> tuple:
    """(the lines `diff` prints, whether the states differ)."""
    parts = [p for p in parts if p in ea.stride and p in eb.stride]
    out = [headline("diff A", ea, seat), headline("diff B", eb)]
    best, only = first_difference(ea, la, eb, lb, seat, parts)
    for what, names in only.items():
        out.append(f"  not compared ({what}): {', '.join(names[:8])}"
                   + (f" +{len(names) - 8} more" if len(names) > 8 else ""))
    upto = min(ea.end, eb.end)
    if best is None or best[0] > upto:
        if ea.end == eb.end:
            out.append(f"  EQUAL at every tick to {upto}: {', '.join(parts)}")
            return out, False
        out.append(f"  equal at every tick to {upto}, where the shorter ends: A to {ea.end}, B to {eb.end}")
        return out, True
    t, found = best
    out.append(f"  FIRST DIFFERENCE at tick {t}, written by the step from GameTime {max(t - 1, 0)} "
               f"({', '.join(parts)} equal at every tick before it), {len(found)} fields:")
    width = max(len(n) for n, _a, _b in found)
    out += [f"    {n:<{width}}  A {a}  |  B {b}" for n, a, b in found]
    return out, True


def state_parting(rec_a, rec_b, trees=None, cache=None) -> tuple:
    """For a neutrality proof (session.py --state): (None, "equal to T") when every ImpAI seat's state is equal
    at every tick of two recordings of one game; else ((tick, field, a, b), what is said) of the first
    difference. ThinkError when either cannot be read."""
    ea, eb = extract(rec_a, cache), extract(rec_b, cache)
    lays = layouts(trees, cache)
    la, lb = fit(ea, lays), fit(eb, lays)
    seats = ea.impai_seats()
    if seats != eb.impai_seats() or not seats:
        raise ThinkError(f"{ea.name}: ImpAI's seats are {seats or 'none'} in one recording, "
                         f"{eb.impai_seats() or 'none'} in the other")
    first = None
    for seat in seats:
        best, _only = first_difference(ea, la, eb, lb, seat, [p for p in PARTS if p in ea.stride and p in eb.stride])
        if best is not None and (first is None or best[0] < first[0]):
            first = (best[0], *best[1][0], len(best[1]))
    if first is None:
        if ea.end != eb.end:
            return (min(ea.end, eb.end) + 1, "the recording's end", str(ea.end), str(eb.end)), \
                f"state equal to {min(ea.end, eb.end)}, but one game ends at {ea.end} and the other at {eb.end}"
        return None, f"state equal at every tick to {ea.end}"
    t, name, a, b, n = first
    return (t, name, a, b), (f"state parts in the step from GameTime {max(t - 1, 0)}: {name} {a} | {b}"
                             + (f" (+{n - 1} fields)" if n > 1 else ""))


# ---- why: THE REASON TRACE, read from the game's log ----
WHY_LINE = re.compile(r"^impai: why seat=(\d+) t=(\d+) gates=(\S+) naval=(-?\d+) pos=(\S+) need=(\S+) veto=(\S+) "
                      r"take=(\S+)\s*$")
WANTS = ("factory", "sea", "tech", "def", "aa", "sensor")       # impai_veto's rows, in its order
# the term a want's rule reads last, for a want nothing else refuses (impai.h, IMPAI_WHY)
LAST_TERM = {"sea": "no builder of the seat's offers a sea plant", "sensor": "the sensor rule names no kind or point"}
NAVAL = {1: "a naval map", 0: "not a naval map", -1: "not judged"}


class Why:
    """One `why` line: its GameTime, the gates up, the posture, and each want named as (needed, gates that veto
    it, taken)."""

    def __init__(self, m):
        def names(x):
            return () if x == "-" else tuple(x.split("+"))
        self.t = int(m.group(2))
        self.gates, self.naval, self.pos = names(m.group(3)), int(m.group(4)), names(m.group(5))
        need, take = names(m.group(6)), names(m.group(8))
        veto = dict((v.partition(":")[0], names(v.partition(":")[2])) for v in m.group(7).split(",")) \
            if m.group(7) != "-" else {}
        self.want = {w: (w in need, veto.get(w, ()), w in take)
                     for w in dict.fromkeys(WANTS + need + take + tuple(veto))}

    def of(self, want: str) -> tuple:
        return self.want.get(want, (False, (), False))


def why_status(want: str, st: tuple) -> str:
    need, veto, take = st
    if take:
        return "needed, taken" if need else "taken (not needed: the trace disagrees with itself)"
    if not need:
        return "not needed"
    if veto:
        return f"needed, VETOED by {'+'.join(veto)}"
    return f"needed, held by no gate, not taken: {LAST_TERM.get(want, 'the term its rule reads last')}"


def why_read(log: Path, seat: int, name: str) -> list:
    """The seat's `why` lines of `log`, in its order; refused when there is none or its GameTimes go back."""
    try:
        text = log.read_text(errors="replace")
    except OSError as e:
        raise ThinkError(f"{name}: no log to read the reason trace from ({log}: {e.strerror or e}); --log FILE "
                         f"names it")
    ws = [Why(m) for m in map(WHY_LINE.match, text.splitlines()) if m and int(m.group(1)) == seat]
    if not ws:
        raise ThinkError(f"{log.name}: no `impai: why` line for seat {seat}: the DLL that played the game writes no "
                         f"reason trace (a build before it), or the seat made no economy decision")
    for a, b in zip(ws, ws[1:]):
        if b.t <= a.t:
            raise ThinkError(f"{log.name}: seat {seat}'s `why` lines go back from GameTime {a.t} to {b.t} (a second "
                             f"level or a rewind in one log): not one game's trace")
    return ws


PLANTWALK_LINE = re.compile(r"^impai: plantwalk seat=(\d+) t=(\d+) (.*)$")


def plantwalk_at(log: Path, seat: int, at: int):
    """(the tick it stands from, its fields) of the seat's last `plantwalk` line at or before tick `at` -- what the
    plant rung last met when it ordered no plant (tagpu_impai.c, THE PLANTWALK LINE); None when there is none."""
    last = None
    for m in map(PLANTWALK_LINE.match, log.read_text(errors="replace").splitlines()):
        if m and int(m.group(1)) == seat and int(m.group(2)) + 1 <= at:
            last = (int(m.group(2)) + 1, m.group(3))
    return last


def why(ex, seat, log: Path, at, want, t0=None, t1=None) -> list:
    ws = why_read(log, seat, ex.name)
    ticks = [w.t + 1 for w in ws]           # a decision stamped t=N stands from tick N + 1
    if want is not None:
        want = want[5:] if want.startswith("want_") else want
        if not any(want in w.want for w in ws):
            raise ThinkError(f"--want {want}: the trace's wants are {', '.join(dict.fromkeys(x for w in ws for x in w.want))}")
    out = [headline("why", ex, seat)]

    def span(i, key):
        """(the tick ws[i]'s `key` has stood since, the tick it ends or None)"""
        a, b = i, i + 1
        while a > 0 and key(ws[a - 1]) == key(ws[i]):
            a -= 1
        while b < len(ws) and key(ws[b]) == key(ws[i]):
            b += 1
        return ticks[a], ticks[b] if b < len(ws) else None

    def stood(i, key):
        a, b = span(i, key)
        return f"{a}..{b if b is not None else 'end'}"

    if at is not None:
        if not 0 <= at <= ex.end:
            raise ThinkError(f"--at {at}: {ex.name}'s ticks are 0 to {ex.end}")
        i = bisect.bisect_right(ticks, at) - 1
        if i < 0:
            return out + [f"  tick {at}: no economy decision traced yet (the seat's first `why` line stands from tick "
                          f"{ticks[0]})"]
        w = ws[i]
        out.append(f"  tick {at}: the trace last changed at tick {ticks[i]} (the log's t={w.t}); {len(ws)} `why` lines "
                   f"in {log.name}; each row: the ticks it stands, from..until")
        rows = []
        if want is None:
            rows.append(("gates up", stood(i, lambda x: x.gates), ", ".join(w.gates) or "none"))
            rows.append(("posture", stood(i, lambda x: (x.naval, x.pos)),
                         f"naval {w.naval} ({NAVAL.get(w.naval, '?')}); {', '.join(w.pos) or 'no flag'}"))
        for n in ([want] if want is not None else list(w.want)):
            rows.append((f"want_{n}", stood(i, lambda x, n=n: x.of(n)), why_status(n, w.of(n))))
        width = max(len(r[1]) for r in rows)
        out += [f"  {a:<12} {b:<{width}}  {c}" for a, b, c in rows]
        walk = plantwalk_at(log, seat, at) if want is None else None
        if walk:
            out.append(f"  the plant rung last ordered nothing at tick {walk[0]}: {walk[1]}")
    if want is None:
        if at is None:
            raise ThinkError("why: --at T (the trace at a tick), --want NAME (a want's history), or both")
        return out
    # the want's history: its runs, each from the tick its line changed it to the next
    runs = [i for i in range(len(ws)) if i == 0 or ws[i].of(want) != ws[i - 1].of(want)]
    ends = [ticks[i] for i in runs[1:]] + [max(ex.end, ticks[-1])]
    tot = {"taken": 0, "vetoed": 0, "last": 0, "not": 0}
    by = {}
    for i, e in zip(runs, ends):
        need, veto, take = ws[i].of(want)
        d = e - ticks[i]
        tot["taken" if take else "not" if not need else "vetoed" if veto else "last"] += d
        for g in veto if not take else ():
            by[g] = by.get(g, 0) + d
    gates = f" ({', '.join(f'{g} {n}' for g, n in sorted(by.items(), key=lambda x: -x[1]))})" if by else ""
    out.append(f"  want_{want} over ticks {ticks[0]}..{ends[-1]}, {len(runs) - 1} changes: taken {tot['taken']} ticks, "
               f"vetoed {tot['vetoed']}{gates}, held by its last term {tot['last']}, not needed {tot['not']}")
    lo, hi = t0 if t0 is not None else ticks[0], t1 if t1 is not None else ends[-1]
    before = [i for i in runs if ticks[i] < lo]
    if before and t0 is not None:
        out.append(f"  at {lo}: {why_status(want, ws[before[-1]].of(want))} (from tick {ticks[before[-1]]})")
    out += [f"{ticks[i]:>8}  {why_status(want, ws[i].of(want))}" for i in runs if lo <= ticks[i] <= hi]
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)

    def common(q, seat=True):
        if seat:
            q.add_argument("--seat", type=int, default=None, help="the seat (default: the one ImpAI seat)")
        q.add_argument("--tree", action="append", default=[], metavar="DIR",
                       help="a source tree whose layout a recording may be read by (repeatable)")
        q.add_argument("--max", type=int, default=MAX_LINES, help="lines printed in all")
    s = sub.add_parser("state", help="the regions at one tick")
    s.add_argument("rec", type=Path)
    s.add_argument("--at", type=int, required=True, metavar="T")
    s.add_argument("--part", default=",".join(PARTS), help=f"a comma list of {', '.join(PARTS)}")
    s.add_argument("--fields", default=None, metavar="PATH,PATH", help="these fields alone, one a line")
    common(s)
    t = sub.add_parser("timeline", help="the ticks where a field changes")
    t.add_argument("rec", type=Path)
    t.add_argument("--field", required=True, metavar="PATH,PATH")
    t.add_argument("--from", dest="t0", type=int, default=None, metavar="T")
    t.add_argument("--to", dest="t1", type=int, default=None, metavar="T")
    common(t)
    q = sub.add_parser("squads", help="each squad's task history")
    q.add_argument("rec", type=Path)
    q.add_argument("--from", dest="t0", type=int, default=None, metavar="T")
    q.add_argument("--to", dest="t1", type=int, default=None, metavar="T")
    common(q)
    u = sub.add_parser("units", help="units alive a seat over time")
    u.add_argument("rec", type=Path)
    u.add_argument("--every", type=int, default=None, metavar="N")
    u.add_argument("--max", type=int, default=MAX_LINES)
    d = sub.add_parser("diff", help="the first tick two recordings' brain state differs")
    d.add_argument("rec", type=Path)
    d.add_argument("rec_b", type=Path)
    d.add_argument("--part", default=",".join(PARTS))
    common(d)
    w = sub.add_parser("why", help="why a want was or was not taken: the reason trace in the game's log")
    w.add_argument("rec", type=Path)
    w.add_argument("--at", type=int, default=None, metavar="T", help="the trace at this tick")
    w.add_argument("--want", default=None, metavar="NAME", help="this want's history (want_factory, or factory)")
    w.add_argument("--from", dest="t0", type=int, default=None, metavar="T")
    w.add_argument("--to", dest="t1", type=int, default=None, metavar="T")
    w.add_argument("--log", type=Path, default=None, metavar="FILE",
                   help="the game's log (default: <name>.tagpu.log beside <name>.imprec)")
    w.add_argument("--seat", type=int, default=None, help="the seat (default: the one ImpAI seat)")
    w.add_argument("--max", type=int, default=MAX_LINES, help="lines printed in all")
    a = p.parse_args(argv)
    try:
        ex = extract(a.rec)
        if a.verb == "units":
            print("\n".join(capped(units(ex, a.every), a.max, "--every")))
            return 0
        if a.verb == "why":
            log = a.log or a.rec.with_suffix(".tagpu.log")
            print("\n".join(capped(why(ex, pick_seat(ex, a.seat), log, a.at, a.want, a.t0, a.t1), a.max,
                                   "--want, --from/--to")))
            return 0
        lays = layouts(a.tree)
        lay = fit(ex, lays)
        for each in lays:
            each.name_types(type_names(ex))
        seat = pick_seat(ex, a.seat)
        parts = [x for x in getattr(a, "part", "").split(",") if x]
        bad = [x for x in parts if x not in PARTS]
        if bad:
            raise ThinkError(f"--part {','.join(bad)}: a comma list of {', '.join(PARTS)}")
        differ = False
        if a.verb == "state":
            lines = state(ex, lay, seat, a.at, parts, [x for x in (a.fields or "").split(",") if x])
        elif a.verb == "timeline":
            lines = timeline(ex, lay, seat, [x for x in a.field.split(",") if x], a.t0, a.t1)
        elif a.verb == "squads":
            lines = squads(ex, lay, seat, a.t0, a.t1)
        else:
            eb = extract(a.rec_b)
            if seat not in [p["index"] for p in eb.players]:
                raise ThinkError(f"{eb.name}: no seat {seat}")
            lines, differ = diff(ex, lay, eb, fit(eb, lays), seat, parts)
        print("\n".join(capped(lines, a.max)))
        return 1 if differ else 0
    except ThinkError as e:
        print(f"think: COULD NOT RUN: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
