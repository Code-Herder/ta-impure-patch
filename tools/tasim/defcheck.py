#!/usr/bin/env python3
"""defcheck.py -- the shared core's definitions against the engine's own, byte for byte, from one running game.

    tools/tasim/defcheck.py <instance> [--scenario FILE] [--gamedir DIR] [--no-launch] [--show N]
    tools/tasim/defcheck.py --setup NAME [--dll PATH] [--scenario FILE] [--show N]
    tools/tasim/defcheck.py --catalogue RESULTS --setup NAME [--show N]

tasim_run defs (tools/tasim/native.py builds it) reads the install through its profile (tools/wiki/tavfs.py: the
names and constants the folder's running image reads) and the key table (tagpu/ddraw/src/tasim_keys.h), and prints,
for every unit, weapon, feature and move class, its engine image: each field at the engine's offset, in the engine's
type, with the bits the loader writes. Then, under the run lock (tools/runlock.py), one launch: `tacli scenario load
<instance> FILE --defaults --restart` (default scenarios/calib/arena.json, a skirmish, so the loader body 0x497180 has
run the unit-data load 0x42D2E0 and the weapon load 0x42E310), and `tacli peek` reads the engine's tables:

  - the UnitDefs, *(main+0x1439B), 0x249 bytes each, *(main+0x1438F) of them: type 0 `None` and the core's rows as
    types 1.. (0x42D2E0 sorts [1, count) by name), so the count is the core's units and one, every name has as many
    types as rows and they are the same indices, and every engine type is one of the core's;
  - each def's references: the weapon records +0x1EE/+0x1F2/+0x1F6/+0x220/+0x224 point at (their section name +0), the
    corpse +0x1BC (the FeatureDef it indexes, its name +0), the move class +0x1B6 (its index from 0x512358, 0x20 each),
    the yard map +0x14E (footx x footz bytes, whole), and the key +0x13E, the checksum of the FBI the pre-scan read
    (0x4B6BA0 at 0x42ABB3; engine/network.md *The key*);
  - the weapon array, wherever Impure's raised build put it: its base is a known weapon's pointer less its ID x 0x115,
    each of the core's weapons is the record at its ID, and every slot whose section name (+0) is not empty is one
    of the core's;
  - each weapon's [DAMAGE] table, +0x64: NULL for none (0x499CED), else an object whose dwords +0x05 and +0x09 are the
    begin and end (0x499CFF, 0x499D02) of 8-byte rows {char* name, i32 damage} (engine/weapons.md, *The [DAMAGE]
    table's layout*);
  - the FeatureDefs the level loaded, *(main+0x1426F), 0x100 bytes each, *(main+0x14253) of them, matched by name;
  - the 32 move class records at 0x512358 and the name each points at, and every record whose name pointer is set is
    one of the core's classes.

TWO TYPES OF ONE NAME. Mayhem's tronmakenuke.fbi names EMPMAKENUKE, so its engine holds two EMPMAKENUKE types, and the
CRT qsort's tie order of the two is not reproducible offline. It is unobservable in every compared field: the level
defines both types from the one file named after them (0x42D6E9..0x42D722, loader-spec F8), so each core row of the
name is paired with the first unpaired engine type of the name whose image, strings, references and yard compare
equal, and the leftovers are compared in order and fail. The fields that do differ between the two -- `side`, which is
not in the image, and the key -- are compared as a multiset per name. No field goes uncompared and nothing is
tolerated.

THE [DAMAGE] NAMES are compared after the ASCII fold A-Z to a-z, as `_stricmp` folds them in the C locale. Every reader
of the table compares names with `_stricmp` -- the lower bound and the equality test (0x499CF9..0x499D60) and the
fill's insert (0x42F035..0x42F0B5) -- and the engine's TDF parser keeps one key a name without case, so a name's case
is unobservable. The rows are sorted by the folded name on both sides and compared name for name and damage for
damage, exactly; the rows of 32 characters or more, which no def +0x20 can hold, are compared as a count.

--setup NAME (the live check on a compat setup): the compatibility suite's folder for NAME (tools/compat/tacompat.py
prepare_wine) in a fresh instance, rendered with GDI, one lane of the run lock with no GPU memory, the scenario loaded
with --any-map and the instance removed at the end. The tables do not depend on the map: 0x42D2E0 loads every kept
type and 0x42E310 every weapon file, and only the features the level loaded are compared; and under a moved settings
root the game reads its own map whatever tacli names.

--catalogue RESULTS --setup NAME (offline): the core's reading of the setup's folder (tacompat.setup_tree: the retail
install, the setup's files on top) against RESULTS/NAME/verdict.json, the compat suite's own reads of the engine in a
player's launch (tacompat.py level_tables and mod_content: `tacli units` and `features`). It proves which file won
each type (the type list in order and multiplicity, against `level.types`), which copy of each FBI the pre-scan read
(each name's key, against `content.keys`), and which section holds each weapon ID (tagpu_recorder.c's names_hash over
the 4096 slots, against `level.weapons`). No game runs.

Every field of every image row is compared under its mask; a mismatch prints the definition, the engine offset, the
core's bytes and the engine's; a definition one side has and the other lacks prints its name. --no-launch peeks the
level already running. --show N prints at most N mismatches a kind.
Exit 0 when every field equals, 1 when one differs, 2 when the defs, the launch, a peek or the catalogue cannot be had.
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import native  # noqa: E402
from resource_regression import cap_failure, hold_instance, tacli_api  # noqa: E402

TACLI = TREE / "tools" / "tacli"
SCENARIO = TREE / "scenarios" / "calib" / "arena.json"
DLL = TREE / "tagpu" / "ddraw" / "ddraw.dll"
SUFFIX = "-dc"                  # the instance of a --setup run (tacompat.inst_name, within its SUFFIX_ROOM)
LOAD_S = 900                    # a launch to a live map, a cold prefix included
PEEK_S = 120
BATCH = 200                     # specs a peek trigger carries
MAIN = "*0x511DE8"
UDEF, UCOUNT = 0x1439B, 0x1438F
FDEF, FCOUNT = 0x1426F, 0x14253
UDEF_SIZE, WREC_SIZE, FDEF_SIZE = 0x249, 0x115, 0x100
CLASSES, CLASS_SIZE = 0x512358, 0x20
U_KEY = 0x13E                   # u32, the FBI's checksum (0x42ABB3)
U_WEAPONS = (0x1EE, 0x1F2, 0x1F6, 0x220, 0x224)
W_DMG = 0x64                    # the [DAMAGE] table object, NULL for none (0x499CED)
DMG_MAX = 65536                 # rows a table may hold: tagpu_worldq.c's bound on the same table
DMG_NAME = 0x20                 # a row name's bytes read; one with no NUL in them is 32 characters or more
WEAPON_IDS = 4096               # the weapon array's slots, the recorder's fingerprint's count

FOLD = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


class CouldNotRun(Exception):
    pass


def fold(s):
    """`s` with A-Z folded to a-z and nothing else, as _stricmp folds in the C locale (0x4F8A88..0x4F8AC1)."""
    return s.translate(FOLD)


def tacli(*args, timeout=PEEK_S):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True, text=True,
                       timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def peek(instance, specs):
    """Each spec's raw value text, in order, read in batches. A size is hex (tagpu_peek.h's grammar)."""
    out = []
    for i in range(0, len(specs), BATCH):
        part = specs[i:i + BATCH]
        rc, o, e = tacli("peek", instance, *part, "--json")
        if rc != 0:
            raise CouldNotRun(f"peek: {(o + e).strip()[-300:]}")
        rows = json.loads(o)["peeks"]
        if [r["spec"] for r in rows] != part:
            raise CouldNotRun(f"peek answered {len(rows)} of {len(part)} specs")
        out += [r["value"] for r in rows]
    return out


def as_int(v):
    return int(v.split()[0])


def as_str(v):
    return v[1:-1] if v.startswith('"') and v.endswith('"') else None


def as_bytes(v):
    try:
        return bytes.fromhex(v.replace(" ", ""))
    except ValueError:
        return None


def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def read_spans(instance, spans):
    """The bytes of each (address, size), read 256 at a time, as bytes (None where unreadable)."""
    specs, per = [], []
    for a, size in spans:
        per.append(0)
        for off in range(0, size, 256):
            specs.append(f"0x{a + off:X}:x{min(256, size - off):X}")
            per[-1] += 1
    vals = peek(instance, specs)
    out, at = [], 0
    for n in per:
        chunks = [as_bytes(v) for v in vals[at:at + n]]
        at += n
        out.append(None if any(c is None for c in chunks) else b"".join(chunks))
    return out


def read_block(instance, addrs, size):
    """`size` bytes at each address, as bytes (None where unreadable)."""
    return read_spans(instance, [(a, size) for a in addrs])


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def ieq(a, b):
    return a.lower() == b.lower()


class Tally:
    def __init__(self, show):
        self.show, self.fields, self.bad, self.lines = show, {}, {}, []

    def field(self, kind, ok, text):
        self.fields[kind] = self.fields.get(kind, 0) + 1
        if not ok:
            self.bad[kind] = self.bad.get(kind, 0) + 1
            if self.bad[kind] <= self.show:
                self.lines.append(f"  {kind}: {text}")

    def add(self, fields):
        for kind, ok, text in fields:
            self.field(kind, ok, text)


def image_fields(kind, name, image, rec):
    """[(kind, ok, text)]: each image row of the core's against the engine's record, under its mask."""
    out = []
    for off, width, hexv, mask in image:
        ours = bytes.fromhex(hexv)
        theirs = rec[off:off + width]
        if mask != -1:
            m = mask & ((1 << (8 * width)) - 1)
            a = int.from_bytes(ours, "little") & m
            b = int.from_bytes(theirs, "little") & m
            out.append((kind, a == b, f"{name} +0x{off:X} core {a:0{2 * width}x} engine {b:0{2 * width}x} "
                                      f"(mask {m:x})"))
        else:
            out.append((kind, ours == theirs, f"{name} +0x{off:X} core {ours.hex()} engine {theirs.hex()}"))
    return out


# ---- the pure comparisons (tools/tasim/tests/test_defcheck.py) ----------------------------------------------------

class Pairing:
    """pair_duplicates' answer: `pairs` [(core row, engine type)], the core rows and engine types left with no partner,
    the per-name failures of the type order, and the names held by more than one row or type."""

    def __init__(self):
        self.pairs, self.rows_alone, self.types_alone, self.problems, self.shared = [], [], [], [], []


def pair_duplicates(core_names, engine_names, same):
    """The core's rows paired with the engine's types by name (folded as _stricmp folds). `core_names` are the core's
    unitnames in row order, `engine_names` {type index: unitname} of types 1.., `same(row, type)` whether the two
    compare equal in every field. A name must have as many types as rows, and the types must be its rows + 1 (0x42D2E0
    sorts [1, count) by name, row r is type r + 1): each name that does not is a problem. Within a name of more than
    one row, each row in order takes the first unpaired type that compares equal; the rest are paired in order, which
    the caller's comparison then fails (the module docstring, TWO TYPES OF ONE NAME)."""
    p = Pairing()
    rows, types = {}, {}
    for r, n in enumerate(core_names):
        rows.setdefault(fold(n), []).append(r)
    for k in sorted(engine_names):
        types.setdefault(fold(engine_names[k]), []).append(k)
    for name in list(rows) + [n for n in types if n not in rows]:
        rs, ks = rows.get(name, []), types.get(name, [])
        if not rs or not ks:
            p.rows_alone += rs
            p.types_alone += ks
            continue
        if len(rs) != len(ks):
            p.problems.append(f"{name!r}: the core has {len(rs)} row(s), the engine {len(ks)} type(s)")
        elif sorted(r + 1 for r in rs) != ks:
            p.problems.append(f"{name!r}: the core's rows are types {[r + 1 for r in rs]}, the engine's {ks}")
        free, left = list(ks), []
        if len(rs) > 1 or len(ks) > 1:
            p.shared.append(name)
            for r in rs:
                k = next((k for k in free if same(r, k)), None)
                if k is None:
                    left.append(r)
                else:
                    free.remove(k)
                    p.pairs.append((r, k))
        else:
            left = rs
        p.pairs += zip(left, free)
        p.rows_alone += left[len(free):]
        p.types_alone += free[len(left):]
    p.pairs.sort()
    return p


def damage_extent(begin, end):
    """The rows of a [DAMAGE] table from its begin and end, or None when the pair cannot be one: end below begin, a
    span that is not whole 8-byte rows, or more than DMG_MAX rows."""
    if end < begin or (end - begin) % 8 or (end - begin) // 8 > DMG_MAX:
        return None
    return (end - begin) // 8


def compare_damage(section, engine, engine_long, core, core_long):
    """[(ok, text)]: weapon `section`'s [DAMAGE] rows, the engine's [(name, damage)] and its count of names of 32
    characters or more, against the core's. Both sides sorted by the folded name (the damage breaking a tie), then
    compared row for row: the folded names and the damages exactly (the module docstring, THE [DAMAGE] NAMES)."""
    out = [(engine_long == core_long,
            f"{section}: rows of 32 characters or more: core {core_long} engine {engine_long}")]
    a = sorted(core, key=lambda r: (fold(r[0]), r[1]))
    b = sorted(engine, key=lambda r: (fold(r[0]), r[1]))
    for i in range(max(len(a), len(b))):
        x = tuple(a[i]) if i < len(a) else None
        y = tuple(b[i]) if i < len(b) else None
        ok = x is not None and y is not None and fold(x[0]) == fold(y[0]) and x[1] == y[1]
        out.append((ok, f"{section}: row {i} of {len(a)}/{len(b)} by name: core {x} engine {y}"))
    return out


def names_hash(names):
    """tagpu_recorder.c's names_hash: FNV-1a 32 over each name's bytes up to 32 or its end, then one multiply by the
    prime a name, so that an empty slot still moves the hash."""
    h = 2166136261
    for n in names:
        for c in n.encode("latin-1")[:32]:
            h = ((h ^ c) * 16777619) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def weapon_slots(core):
    """The core's section name in each of the WEAPON_IDS slots, "" for an empty one. CouldNotRun when the core's slot
    count is another: the fingerprint would be over another table."""
    if core["counts"]["weapon_slots"] != WEAPON_IDS:
        raise CouldNotRun(f"the core has {core['counts']['weapon_slots']} weapon slots, the recorder's fingerprint "
                          f"{WEAPON_IDS}")
    slots = [""] * WEAPON_IDS
    for w in core["weapons"]:
        slots[w["id"]] = w["section"]
    return slots


def compare_catalogue(core, verdict):
    """The differences between the core's defs and the compat suite's reads of the engine (verdict.json's `level` and
    `content`), each a line; empty when every comparison holds (the module docstring, --catalogue)."""
    bad = []
    level, content = verdict["level"], verdict["content"]
    want = ["NONE"] + [u["unitname"].upper() for u in core["units"]]
    got = list(level["types"])
    if got != want:
        i = next((i for i, (x, y) in enumerate(zip(got, want)) if x != y), min(len(got), len(want)))
        bad.append(f"types: the engine has {len(got)}, the core {len(want)} with None; first difference at type {i}: "
                   f"engine {got[i:i + 3]}, core {want[i:i + 3]}")
    keys = {}
    for u in core["units"]:
        keys.setdefault(u["unitname"].upper(), []).append(u["key"])
    for name, key in content["keys"].items():
        if name == "NONE" and name not in keys:
            continue            # type 0, the engine's own record, which no FBI defines: its place is `types`' first
        mine = keys.get(name)
        if mine is None:
            bad.append(f"key: {name} is not among the core's units")
        elif key is None or not any(k is not None and int(k, 16) == int(key, 16) for k in mine):
            bad.append(f"key: {name} engine {key}, core {', '.join(str(k) for k in mine)}")
    slots = weapon_slots(core)
    want_w = [WEAPON_IDS, "0x%08X" % names_hash(slots)]
    if list(level["weapons"]) != want_w:
        bad.append(f"weapons: the engine's fingerprint {level['weapons']}, the core's {want_w} "
                   f"({sum(1 for s in slots if s)} sections)")
    return bad


# ---- the core, and the live comparison ----------------------------------------------------------------------------

def core_defs(gamedir):
    """`tasim_run defs` on the folder, through its profile. CouldNotRun when the profile or the load is refused."""
    rc, out, err = native.run("defs", str(gamedir))
    if rc != 0:
        raise CouldNotRun(f"tasim_run defs: {err.strip()}")
    return json.loads(out)


class Engine:
    """What the unit comparison needs of the running level beyond each type's record."""

    def __init__(self, fnames, wbase, ptr_name, yards):
        self.fnames, self.wbase, self.ptr_name, self.yards = fnames, wbase, ptr_name, yards


def unit_fields(u, rec, k, eng):
    """[(kind, ok, text)]: every field of the core's unit `u` against the engine's type `k`, its record `rec`; the key
    is not among them (compared per name, `key_fields`)."""
    name = u["unitname"]
    out = image_fields("unit", name, u["image"], rec)
    for off, s in u["strings"].items():
        o = int(off, 16)
        width = {0x0: 0x20, 0x40: 0x40, 0x80: 0x20}[o]
        out.append(("unit string", cstr(rec[o:o + width]) == s,
                    f"{name} +0x{o:X} core {s!r} engine {cstr(rec[o:o + width])!r}"))
    cls = u32(rec, 0x1B6)
    ecls = -1 if cls == 0 else (cls - CLASSES) // CLASS_SIZE
    out.append(("unit ref", ecls == u["class"], f"{name} class core {u['class']} engine {ecls}"))
    corpse = int.from_bytes(rec[0x1BC:0x1BE], "little")
    ecorpse = "" if corpse == 0xFFFF else (eng.fnames[corpse] if corpse < len(eng.fnames) else None)
    out.append(("unit ref", ecorpse is not None and ieq(ecorpse, u["corpse"]),
                f"{name} corpse core {u['corpse']!r} engine {ecorpse!r}"))
    for off, sec in u["weapons"].items():
        q = u32(rec, int(off, 16))
        theirs = "" if q == eng.wbase else eng.ptr_name.get(q)
        out.append(("unit ref", theirs is not None and ieq(theirs, sec), f"{name} {off} core {sec!r} engine {theirs!r}"))
    if u["yard"]:
        ours = bytes.fromhex(u["yard"])
        b = eng.yards.get((k, len(ours)))
        if b is None:
            out.append(("unit yard", False, f"{name} yard unreadable"))
        else:
            i = next((i for i in range(len(ours)) if ours[i] != b[i]), None)
            out.append(("unit yard", i is None, "" if i is None else
                        f"{name} yard cell {i} of {len(ours)}: core {ours[i]:02x} engine {b[i]:02x}"))
    return out


def key_fields(core_units, engine_names, urecs):
    """[(kind, ok, text)]: per name, the engine's keys (+0x13E) as a multiset against the core's, and the count of the
    core's null keys (a .OVR beside the FBI, whose key the core does not model), which no engine key equals."""
    engine, core = {}, {}
    for k, n in engine_names.items():
        engine.setdefault(fold(n), []).append(u32(urecs[k], U_KEY))
    for u in core_units:
        core.setdefault(fold(u["unitname"]), []).append(u["key"])
    out, nulls = [], 0
    for name, mine in core.items():
        nulls += mine.count(None)
        theirs = sorted(engine.get(name, []))
        ours = sorted(int(k, 16) for k in mine if k is not None)
        out.append(("unit key", theirs == ours,
                    f"{name}: engine {['0x%08X' % k for k in theirs]} core {['0x%08X' % k for k in ours]}"
                    + (f" and {mine.count(None)} null" if None in mine else "")))
    return out, nulls


def damage_tables(t, instance, wtables):
    """The [DAMAGE] table of each (core weapon, engine record) whose core rows are not none, compared (compare_damage)."""
    start = time.monotonic()
    have = []
    for w, rec in wtables:
        tab = u32(rec, W_DMG)
        t.field("weapon damage", tab != 0, f"{w['section']}: the engine has no table, the core {w['damage_rows']} rows")
        if tab:
            have.append((w, tab))
    ends = peek(instance, [f"0x{tab + 5:X}:x8" for _w, tab in have])
    spans = []
    for (w, _tab), v in zip(have, ends):
        b = as_bytes(v)
        n = damage_extent(u32(b, 0), u32(b, 4)) if b is not None and len(b) == 8 else None
        t.field("weapon damage", n is not None, f"{w['section']}: the table's begin and end are unreadable or bound "
                                                f"no whole rows ({v})")
        if n is not None:
            spans.append((w, u32(b, 0), n))
    rows = read_spans(instance, [(begin, 8 * n) for _w, begin, n in spans])
    names = peek(instance, [f"*0x{begin + 8 * i:X}:s{DMG_NAME:X}" for _w, begin, n in spans for i in range(n)])
    at, total = 0, 0
    for (w, _begin, n), body in zip(spans, rows):
        texts = [as_str(v) for v in names[at:at + n]]
        at += n
        total += n
        if body is None or None in texts:
            t.field("weapon damage", False, f"{w['section']}: a row of its {n} is unreadable")
            continue
        dmg = [int.from_bytes(body[8 * i + 4:8 * i + 8], "little", signed=True) for i in range(n)]
        engine = [(s, d) for s, d in zip(texts, dmg) if len(s) < DMG_NAME]
        ours = [tuple(r) for r in w["damage"]]
        for ok, text in compare_damage(w["section"], engine, n - len(engine), ours, w["damage_rows"] - len(ours)):
            t.field("weapon damage", ok, text)
    print(f"defcheck: the [DAMAGE] tables: {total} rows of {len(spans)} weapons read in "
          f"{time.monotonic() - start:.0f} s")


def compare_live(instance, core, show):
    """Every definition of `core` against the running level of `instance`: the Tally."""
    m = peek(instance, [f"{MAIN}+0x{UDEF:X}:4", f"{MAIN}+0x{UCOUNT:X}:4", f"{MAIN}+0x{FDEF:X}:4",
                        f"{MAIN}+0x{FCOUNT:X}:4"])
    ubase, ucount, fbase, fcount = (as_int(v) for v in m)
    print(f"defcheck: the engine holds {ucount} unit defs at 0x{ubase:08X}, {fcount} feature defs at 0x{fbase:08X}")
    t = Tally(show)

    # ---- the move classes, and every class the engine holds that the core does not
    recs = read_block(instance, [CLASSES + CLASS_SIZE * i for i in range(32)], CLASS_SIZE)
    names = peek(instance, [f"*0x{CLASSES + CLASS_SIZE * i:X}:s{64:X}" for i in range(32)])
    for c in core["classes"]:
        rec, nm = recs[c["index"]], as_str(names[c["index"]])
        t.field("class", rec is not None and nm is not None and nm == c["name"],
                f"CLASS{c['index']} name core {c['name']!r} engine {nm!r}")
        if rec is not None:
            t.add(image_fields("class", f"CLASS{c['index']}", c["image"], rec))
    core_classes = {c["index"] for c in core["classes"]}
    for i, rec in enumerate(recs):
        # a class is present while its record's +0 points at its name: 0x42D392 sets it, 0x440A00 zeroes it
        if rec is None:
            t.field("class", False, f"the engine's CLASS{i} record is unreadable")
        elif u32(rec, 0):
            t.field("class", i in core_classes, f"the engine's CLASS{i} ({as_str(names[i])!r}) is not among the core's")

    # ---- the unit defs: type 0 is None, the rest the core's rows by name (pair_duplicates)
    units = core["units"]
    urecs = read_block(instance, [ubase + UDEF_SIZE * i for i in range(ucount)], UDEF_SIZE)
    t.field("unit row", ucount == len(units) + 1,
            f"the engine holds {ucount} types and the core {len(units)} units and None")
    t.field("unit row", bool(urecs) and urecs[0] is not None and cstr(urecs[0][0x20:0x40]) == "None",
            f"type 0 is {cstr(urecs[0][0x20:0x40]) if urecs and urecs[0] else None!r}, not 'None'")
    engine_names = {}
    for k, rec in enumerate(urecs):
        t.field("unit row", rec is not None, f"type {k}'s record is unreadable")
        if rec is not None and k > 0:
            engine_names[k] = cstr(rec[0x20:0x40])
    fdefs = read_block(instance, [fbase + FDEF_SIZE * i for i in range(fcount)], FDEF_SIZE)
    fnames = [cstr(f[0:0x80]) if f is not None else None for f in fdefs]
    # the weapon array's base, from a pointer whose section the core knows
    pointers = sorted({u32(urecs[k], off) for k in engine_names for off in U_WEAPONS} - {0})
    ptr_name = dict(zip(pointers, (as_str(v) for v in peek(instance, [f"0x{q:X}:s{32:X}" for q in pointers]))))
    by_section = {w["section"].lower(): w for w in core["weapons"]}
    wbase = next((q - by_section[n.lower()]["id"] * WREC_SIZE for q, n in sorted(ptr_name.items())
                  if n and n.lower() in by_section), None)
    if wbase is None:
        raise CouldNotRun("no unit def points at a weapon the core knows: the weapon array is not found")
    print(f"defcheck: the weapon array's slot 0 is at 0x{wbase:08X}")
    # each yard map whole, footx x footz bytes (at most 64 x 64, tasim_run's buffer), of every type a row may pair with
    spans = sorted({(k, len(u["yard"]) // 2) for u in units if u["yard"]
                    for k, n in engine_names.items() if fold(n) == fold(u["unitname"])})
    yards = dict(zip(spans, read_spans(instance, [(u32(urecs[k], 0x14E), size) for k, size in spans])))
    eng = Engine(fnames, wbase, ptr_name, yards)

    pairing = pair_duplicates([u["unitname"] for u in units], engine_names,
                              lambda r, k: all(ok for _kind, ok, _text in unit_fields(units[r], urecs[k], k, eng)))
    for text in pairing.problems:
        t.field("unit row", False, text)
    for r, k in pairing.pairs:
        t.add(unit_fields(units[r], urecs[k], k, eng))
    for r in pairing.rows_alone:
        t.field("unit", False, f"{units[r]['unitname']} is not in the engine's def table")
    for k in pairing.types_alone:
        t.field("unit", False, f"the engine's type {k}, {engine_names[k]!r}, is not among the core's units")
    if pairing.shared:
        print(f"defcheck: held by more than one type, paired by their fields: {', '.join(pairing.shared)}")
    keys, nulls = key_fields(units, engine_names, urecs)
    t.add(keys)
    if nulls:
        print(f"defcheck: {nulls} of the core's keys are null (a .OVR beside the FBI)")

    # ---- the weapons, each at its ID, and every slot the engine fills that the core does not
    wrecs = read_block(instance, [wbase + WREC_SIZE * w["id"] for w in core["weapons"]], WREC_SIZE)
    wtables = []
    for w, rec in zip(core["weapons"], wrecs):
        ok = rec is not None and ieq(cstr(rec[0:0x20]), w["section"])
        t.field("weapon", ok, f"ID {w['id']} core {w['section']!r} engine {cstr(rec[0:0x20]) if rec else None!r}")
        if ok:
            t.add(image_fields("weapon", w["section"], w["image"], rec))
            if w["damage_rows"]:
                wtables.append((w, rec))
    damage_tables(t, instance, wtables)
    core_ids = {w["id"] for w in core["weapons"]}
    slot_names = peek(instance, [f"0x{wbase + WREC_SIZE * i:X}:s{32:X}" for i in range(core["counts"]["weapon_slots"])])
    for i, v in enumerate(slot_names):
        name = as_str(v)
        if name is None:
            t.field("weapon", False, f"the engine's weapon slot {i} is unreadable")
        elif name:
            t.field("weapon", i in core_ids, f"the engine's weapon slot {i}, {name!r}, is not among the core's")

    # ---- the features the level loaded
    core_f = {f["name"].lower(): f for f in core["features"]}
    for name, rec in zip(fnames, fdefs):
        f = core_f.get((name or "").lower())
        t.field("feature", f is not None, f"the engine's {name!r} is not among the core's")
        if f is not None:
            t.add(image_fields("feature", name, f["image"], rec))
    return t


def report(t, refused):
    for kind in sorted(t.fields):
        print(f"defcheck: {kind}: {t.fields[kind] - t.bad.get(kind, 0)} of {t.fields[kind]} equal")
    print("\n".join(t.lines))
    bad = sum(t.bad.values())
    if refused:
        print(f"defcheck: {refused}")
    print(f"defcheck: {'FAIL' if bad or refused else 'PASS'}, {bad} field(s) differ")
    return 1 if bad or refused else 0


# ---- the three ways in -------------------------------------------------------------------------------------------

def run_instance(a):
    gamedir = Path(a.gamedir) if a.gamedir else tacli_api().Instance(a.instance).gamedir
    if not gamedir.is_dir():
        print(f"defcheck: no install at {gamedir}")
        return 2
    try:
        core = core_defs(gamedir)
        print(f"defcheck: the core read {core['counts']} from {core['profile']['root']}")
        if not a.no_launch:
            hold_instance(f"defcheck on {a.instance}", a.instance)
            rc, o, e = tacli("scenario", "load", a.instance, a.scenario, "--defaults", "--restart", timeout=LOAD_S)
            if rc != 0:
                raise CouldNotRun(f"scenario load: {(o + e).strip()[-400:]}")
        t = compare_live(a.instance, core, a.show)
    except (CouldNotRun, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as e:
        print(f"defcheck: COULD NOT RUN: {e}")
        return 2
    # the game it launched, as far as it has logged: a refusal of the GPU memory cap fails the
    # check (resource_regression, THE CAP'S REFUSALS)
    return report(t, cap_failure(a.instance))


def compat():
    sys.path.insert(0, str(TREE / "tools" / "compat"))
    import tacompat
    return tacompat


def find_setup(tc, name):
    setup = {s["name"]: s for s in tc.SETUPS}.get(name)
    if setup is None:
        raise CouldNotRun(f"no setup {name} (tools/compat/tacompat.py list)")
    missing = sorted({a["fixture"] for a in setup["add"] if tc.fixture_problems(a["fixture"])})
    if missing:
        raise CouldNotRun(f"setup {name} needs fixtures {', '.join(missing)} (tacompat.py fetch)")
    return setup


def run_setup(a):
    """The live check in a fresh instance of the setup's folder, on GDI, one lane and no GPU memory of the run lock."""
    import runlock
    import talog
    if not a.dll.is_file():
        print(f"defcheck: no DLL at {a.dll}")
        return 2
    tc = compat()
    try:
        setup = find_setup(tc, a.setup)
    except CouldNotRun as e:
        print(f"defcheck: COULD NOT RUN: {e}")
        return 2
    runlock.hold(TREE, f"defcheck on {a.setup}", 1, gpu=False)
    xv = tc.start_xvfb()
    try:
        inst = tc.prepare_wine(setup, a.dll, xv.tacompat_display, suffix=SUFFIX)
        gamedir = inst["gamedir"]
        # tacompat parks tacli's store for a player's launch; this one is tacli's own
        parked = gamedir / "tacli-state.off"
        if parked.is_dir():
            parked.rename(gamedir / "tacli-state")
        gdi = gamedir / "tagpu_gdi.on"
        gdi.unlink(missing_ok=True)         # unlinked first: a link is never written through
        gdi.write_text("")
        caps = talog.CapWatch(gamedir)
        try:
            core = core_defs(gamedir)
            print(f"defcheck: the core read {core['counts']} from {core['profile']['root']}")
            rc, o, e = tacli("scenario", "load", inst["name"], a.scenario, "--defaults", "--restart", "--any-map",
                             timeout=LOAD_S)
            if rc != 0:
                raise CouldNotRun(f"scenario load: {(o + e).strip()[-400:]}")
            t = compare_live(inst["name"], core, a.show)
        except (CouldNotRun, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as e:
            print(f"defcheck: COULD NOT RUN: {e}")
            return 2
        return report(t, caps.failure(inst["name"]))
    finally:
        tc.tacli("rm", tc.inst_name(a.setup, SUFFIX), "--force")
        tc.stop_xvfb(xv)


def run_catalogue(a):
    """The offline check of the setup's folder against the compat suite's verdict (the module docstring)."""
    sys.path.insert(0, str(TREE / "tools" / "wiki"))
    import tavfs
    tc = compat()
    try:
        setup = find_setup(tc, a.setup)
        path = Path(a.catalogue) / a.setup / "verdict.json"
        try:
            verdict = json.loads(path.read_text())
        except (OSError, ValueError) as e:
            raise CouldNotRun(f"{path}: {e}")
        level, content = verdict.get("level") or {}, verdict.get("content") or {}
        if verdict.get("setup") != a.setup or not level.get("types") or not level.get("weapons") or \
                not content.get("keys"):
            raise CouldNotRun(f"{path} is not setup {a.setup}'s with its level's types, weapons and keys")
        with tempfile.TemporaryDirectory(prefix="defcheck-") as tmp:
            tree = Path(tmp) / "game"
            tc.setup_tree(setup, tavfs.find_install(None), tree)
            core = core_defs(tree)
        bad = compare_catalogue(core, verdict)
    except (CouldNotRun, OSError, ValueError, KeyError) as e:
        print(f"defcheck: COULD NOT RUN: {e}")
        return 2
    sections = sum(1 for s in weapon_slots(core) if s)
    print(f"defcheck: {a.setup}: the core's {len(core['units']) + 1} types with None, "
          f"{sum(1 for u in core['units'] if u['key'] is not None)} keys, {sections} weapon sections "
          f"(0x{names_hash(weapon_slots(core)):08X}); the engine's {len(level['types'])} types, "
          f"{len(content['keys'])} keys, weapons {level['weapons']}")
    for line in bad[:a.show]:
        print(f"  {line}")
    print(f"defcheck: {'FAIL' if bad else 'PASS'}, {len(bad)} difference(s)")
    return 1 if bad else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("instance", nargs="?", help="a running instance's name (not with --setup)")
    p.add_argument("--setup", help="a compat setup (tools/compat/setups.json): the live check in its own instance, "
                                   "or with --catalogue the offline one")
    p.add_argument("--catalogue", metavar="RESULTS", help="a compat run's results folder: compare offline")
    p.add_argument("--dll", type=Path, default=DLL, help="the ddraw.dll a --setup run plays")
    p.add_argument("--scenario", default=str(SCENARIO))
    p.add_argument("--gamedir", help="the install tasim_run reads (default: the instance's game folder)")
    p.add_argument("--no-launch", action="store_true", help="peek the level already running")
    p.add_argument("--show", type=int, default=20)
    a = p.parse_args()
    if bool(a.instance) == bool(a.setup):
        p.error("name an instance or --setup, one of them")
    if a.catalogue and not a.setup:
        p.error("--catalogue needs --setup")
    if a.setup and (a.gamedir or a.no_launch):
        p.error("--gamedir and --no-launch are an instance's")
    if a.catalogue:
        return run_catalogue(a)
    if a.setup:
        return run_setup(a)
    return run_instance(a)


if __name__ == "__main__":
    sys.exit(main())
