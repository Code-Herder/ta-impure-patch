#!/usr/bin/env python3
"""defcheck.py -- the shared core's definitions against the engine's own, byte for byte, from one running game.

    tools/tasim/defcheck.py <instance> [--scenario FILE] [--gamedir DIR] [--no-launch] [--show N]

tasim_run defs (tools/tasim/native.py builds it) reads the instance's install through the key table
(tagpu/ddraw/src/tasim_keys.h) and prints, for every unit, weapon, feature and move class, its engine image: each field
at the engine's offset, in the engine's type, with the bits the loader writes. Then, under the run lock
(tools/runlock.py), one launch: `tacli scenario load <instance> FILE --defaults --restart` (default
scenarios/calib/arena.json, a skirmish, so the loader body 0x497180 has run the unit-data load 0x42D2E0 and the weapon
load 0x42E310), and `tacli peek` reads the engine's tables:

  - the UnitDefs, *(main+0x1439B), 0x249 bytes each, *(main+0x1438F) of them, matched to the core's by unitname (+0x20);
  - each def's references: the weapon records +0x1EE/+0x1F2/+0x1F6/+0x220/+0x224 point at (their section name +0), the
    corpse +0x1BC (the FeatureDef it indexes, its name +0), the move class +0x1B6 (its index from 0x512358, 0x20 each),
    the yard map +0x14E (footx x footz bytes);
  - the weapon array, wherever Impure's raised build put it: its base is a known weapon's pointer less its ID x 0x115,
    and each of the core's weapons is the record at its ID;
  - the FeatureDefs the level loaded, *(main+0x1426F), 0x100 bytes each, *(main+0x14253) of them, matched by name;
  - the 32 move class records at 0x512358, and the name each points at.

Every field of every image row is compared under its mask; a mismatch prints the definition, the engine offset, the
core's bytes and the engine's. --no-launch peeks the level already running. --show N prints at most N mismatches a kind.
Exit 0 when every field equals, 1 when one differs, 2 when the defs, the launch or a peek cannot be had.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import native  # noqa: E402
import runlock  # noqa: E402
from resource_regression import tacli_api  # noqa: E402

TACLI = TREE / "tools" / "tacli"
SCENARIO = TREE / "scenarios" / "calib" / "arena.json"
LOAD_S = 900                    # a launch to a live map, a cold prefix included
PEEK_S = 120
BATCH = 200                     # specs a peek trigger carries
MAIN = "*0x511DE8"
UDEF, UCOUNT = 0x1439B, 0x1438F
FDEF, FCOUNT = 0x1426F, 0x14253
UDEF_SIZE, WREC_SIZE, FDEF_SIZE = 0x249, 0x115, 0x100
CLASSES, CLASS_SIZE = 0x512358, 0x20


class CouldNotRun(Exception):
    pass


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


def read_block(instance, addrs, size):
    """`size` bytes at each address, as bytes (None where unreadable)."""
    specs = []
    for a in addrs:
        for off in range(0, size, 256):
            specs.append(f"0x{a + off:X}:x{min(256, size - off):X}")
    vals = peek(instance, specs)
    per = (size + 255) // 256
    out = []
    for i in range(len(addrs)):
        chunks = [as_bytes(v) for v in vals[i * per:(i + 1) * per]]
        out.append(None if any(c is None for c in chunks) else b"".join(chunks))
    return out


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


def compare_image(t, kind, name, image, rec):
    for off, width, hexv, mask in image:
        ours = bytes.fromhex(hexv)
        theirs = rec[off:off + width]
        if mask != -1:
            m = mask & ((1 << (8 * width)) - 1)
            a = int.from_bytes(ours, "little") & m
            b = int.from_bytes(theirs, "little") & m
            t.field(kind, a == b, f"{name} +0x{off:X} core {a:0{2 * width}x} engine {b:0{2 * width}x} "
                                  f"(mask {m:x})")
        else:
            t.field(kind, ours == theirs, f"{name} +0x{off:X} core {ours.hex()} engine {theirs.hex()}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("instance")
    p.add_argument("--scenario", default=str(SCENARIO))
    p.add_argument("--gamedir", help="the install tasim_run reads (default: the instance's game folder)")
    p.add_argument("--no-launch", action="store_true", help="peek the level already running")
    p.add_argument("--show", type=int, default=20)
    a = p.parse_args()
    gamedir = Path(a.gamedir) if a.gamedir else tacli_api().Instance(a.instance).gamedir
    if not gamedir.is_dir():
        print(f"defcheck: no install at {gamedir}")
        return 2
    exe = native.build()
    root, mounts = native.mount_list(str(gamedir))
    try:
        r = subprocess.run([str(exe), "defs", "--cwd", str(root), "--archives", str(mounts), "--json"],
                           capture_output=True, text=True)
    finally:
        mounts.unlink()
    if r.returncode != 0:
        print(f"defcheck: tasim_run defs: {r.stderr.strip()}")
        return 2
    core = json.loads(r.stdout)
    print(f"defcheck: the core read {core['counts']} from {root}")

    try:
        if not a.no_launch:
            runlock.hold(TREE, f"defcheck on {a.instance}")
            rc, o, e = tacli("scenario", "load", a.instance, a.scenario, "--defaults", "--restart", timeout=LOAD_S)
            if rc != 0:
                raise CouldNotRun(f"scenario load: {(o + e).strip()[-400:]}")
        m = peek(a.instance, [f"{MAIN}+0x{UDEF:X}:4", f"{MAIN}+0x{UCOUNT:X}:4", f"{MAIN}+0x{FDEF:X}:4",
                              f"{MAIN}+0x{FCOUNT:X}:4"])
        ubase, ucount, fbase, fcount = (as_int(v) for v in m)
        print(f"defcheck: the engine holds {ucount} unit defs at 0x{ubase:08X}, {fcount} feature defs at "
              f"0x{fbase:08X}")
        t = Tally(a.show)

        # ---- the move classes
        recs = read_block(a.instance, [CLASSES + CLASS_SIZE * i for i in range(32)], CLASS_SIZE)
        names = peek(a.instance, [f"*0x{CLASSES + CLASS_SIZE * i:X}:s{64:X}" for i in range(32)])
        for c in core["classes"]:
            rec, nm = recs[c["index"]], as_str(names[c["index"]])
            t.field("class", rec is not None and nm is not None and nm == c["name"],
                    f"CLASS{c['index']} name core {c['name']!r} engine {nm!r}")
            if rec is not None:
                compare_image(t, "class", f"CLASS{c['index']}", c["image"], rec)

        # ---- the unit defs, matched by unitname
        urecs = read_block(a.instance, [ubase + UDEF_SIZE * i for i in range(ucount)], UDEF_SIZE)
        engine_units = {}
        for rec in urecs:
            if rec is not None:
                engine_units.setdefault(cstr(rec[0x20:0x40]).lower(), rec)
        fdefs = read_block(a.instance, [fbase + FDEF_SIZE * i for i in range(fcount)], FDEF_SIZE)
        fnames = [cstr(f[0:0x80]) if f is not None else None for f in fdefs]
        weapon_ptrs = set()
        for u in core["units"]:
            rec = engine_units.get(u["unitname"].lower())
            t.field("unit", rec is not None, f"{u['unitname']} is not in the engine's def table")
            if rec is None:
                continue
            compare_image(t, "unit", u["unitname"], u["image"], rec)
            for off, s in u["strings"].items():
                o = int(off, 16)
                width = {0x0: 0x20, 0x40: 0x40, 0x80: 0x20}[o]
                t.field("unit string", cstr(rec[o:o + width]) == s,
                        f"{u['unitname']} +0x{o:X} core {s!r} engine {cstr(rec[o:o + width])!r}")
            for off in u["weapons"]:
                weapon_ptrs.add(int.from_bytes(rec[int(off, 16):int(off, 16) + 4], "little"))
            cls = int.from_bytes(rec[0x1B6:0x1BA], "little")
            ecls = -1 if cls == 0 else (cls - CLASSES) // CLASS_SIZE
            t.field("unit ref", ecls == u["class"], f"{u['unitname']} class core {u['class']} engine {ecls}")
            corpse = int.from_bytes(rec[0x1BC:0x1BE], "little")
            ecorpse = "" if corpse == 0xFFFF else (fnames[corpse] if corpse < len(fnames) else None)
            t.field("unit ref", ecorpse is not None and ieq(ecorpse, u["corpse"]),
                    f"{u['unitname']} corpse core {u['corpse']!r} engine {ecorpse!r}")
        # the weapon array's base, from a pointer whose section the core knows
        ptr_name = dict(zip(sorted(weapon_ptrs), (as_str(v) for v in
                                                  peek(a.instance, [f"0x{q:X}:s{32:X}" for q in sorted(weapon_ptrs)]))))
        by_section = {w["section"].lower(): w for w in core["weapons"]}
        wbase = next((q - by_section[n.lower()]["id"] * WREC_SIZE for q, n in sorted(ptr_name.items())
                      if n and n.lower() in by_section), None)
        if wbase is None:
            raise CouldNotRun("no unit def points at a weapon the core knows: the weapon array is not found")
        print(f"defcheck: the weapon array's slot 0 is at 0x{wbase:08X}")
        for u in core["units"]:
            rec = engine_units.get(u["unitname"].lower())
            if rec is None:
                continue
            for off, sec in u["weapons"].items():
                q = int.from_bytes(rec[int(off, 16):int(off, 16) + 4], "little")
                theirs = "" if q == wbase else ptr_name.get(q)
                t.field("unit ref", theirs is not None and ieq(theirs, sec),
                        f"{u['unitname']} {off} core {sec!r} engine {theirs!r}")
        yards = [u for u in core["units"] if u["yard"] and u["unitname"].lower() in engine_units]
        if yards:
            specs = [f"0x{int.from_bytes(engine_units[u['unitname'].lower()][0x14E:0x152], 'little'):X}:"
                     f"x{min(256, len(u['yard']) // 2):X}" for u in yards]
            for u, v in zip(yards, peek(a.instance, specs)):
                n = min(256, len(u["yard"]) // 2)
                theirs = as_bytes(v)
                t.field("unit yard", theirs is not None and theirs.hex() == u["yard"][:2 * n],
                        f"{u['unitname']} yard core {u['yard'][:2 * n]} engine {v[:2 * n]}")

        # ---- the weapons, each at its ID
        wrecs = read_block(a.instance, [wbase + WREC_SIZE * w["id"] for w in core["weapons"]], WREC_SIZE)
        for w, rec in zip(core["weapons"], wrecs):
            ok = rec is not None and ieq(cstr(rec[0:0x20]), w["section"])
            t.field("weapon", ok, f"ID {w['id']} core {w['section']!r} engine "
                                  f"{cstr(rec[0:0x20]) if rec else None!r}")
            if ok:
                compare_image(t, "weapon", w["section"], w["image"], rec)

        # ---- the features the level loaded
        core_f = {f["name"].lower(): f for f in core["features"]}
        for name, rec in zip(fnames, fdefs):
            f = core_f.get((name or "").lower())
            t.field("feature", f is not None, f"the engine's {name!r} is not among the core's")
            if f is not None:
                compare_image(t, "feature", name, f["image"], rec)
    except (CouldNotRun, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as e:
        print(f"defcheck: COULD NOT RUN: {e}")
        return 2

    for kind in sorted(t.fields):
        print(f"defcheck: {kind}: {t.fields[kind] - t.bad.get(kind, 0)} of {t.fields[kind]} equal")
    print("\n".join(t.lines))
    bad = sum(t.bad.values())
    print(f"defcheck: {'FAIL' if bad else 'PASS'}, {bad} field(s) differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
