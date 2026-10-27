#!/usr/bin/env python3
"""cob_ext_fixture -- the COB extensions as a unit, and the check of what its getters answered.

    tools/cob_ext_fixture.py <gamedir>               write cobext.ufo
    tools/cob_ext_fixture.py check <instance>        judge the running game's answers

COBEXT is ARMSOLAR's model and FBI under a new name, with the script compiled from
research/notes/evidence/cobtrace/extension.bos: Create and seven sleepers fill records 0..7, so
every getter the prober calls runs in record 9, one of the two Impure's raised build adds. The
archive goes into a test instance's game folder (the replay group `cob-getters`, the tacob
fixture `extension`), never into the repository (.gitignore refuses .ufo) or a player's install.

`check` reads every COBEXT's answers out of its script's statics (the getters keep them there)
and compares each with the engine's own state, read through `tacli peek`: the unit array
main+0x14357 (stride 0x118: the type at +0x92 with its name at +0x20, the COB object at +0x9A
with its statics at +0x10, the id at +0xA8, the kills at +0xB8, the build fraction at +0x104,
the alive bit 0x10000000 of +0x110, the owner at +0xFF), its slot count main+0x14351, and the
player records from main+0x1B63 (stride 0x14B: the kind at +0x73, the alliance bytes at +0x108).
So the getter rule (tagpu_cob_getter) is judged against the engine, not against a second copy of
itself. It reads no trace: the cobtrace hooks the thread allocator 0x4B08C0, where the replay
harness counts. Exit 1 names every difference.

The units are found, not assumed: a seat's units take the lowest free slots of its block of
`unit_limit` slots (seat 0 from slot 1, seat 1 from slot unit_limit + 1; MEASURED 2026-10-23,
scenarios/replay-teleport.json), so the first SCAN slots of each of the scenario's two seats are
read, and every live unit of the type named COBEXT among them is checked. Fewer than two fails.
"""
import json
import re
import struct
import subprocess
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
BOS = ROOT / "research/notes/evidence/cobtrace/extension.bos"
UNIT = "COBEXT"
SCAN = 8
UNIT_LIMIT = 500                    # tools/replay/fixtures.json's default, which cob-getters keeps
GETTERS = ("min_id", "max_id", "my_id", "my_owner", "my_build_left", "nobodys_build_left",
           "allied_with_me", "allied_past_the_array", "mine_is_local", "my_kills")
MAIN = 0x511DE8


def load(name, path):
    return SourceFileLoader(name, str(path)).load_module()


def build(gamedir):
    hpipack = load("hpipack", TOOLS / "hpipack.py")
    tacob = load("tacob", TOOLS / "tacob")
    ta = hpipack.Archive(gamedir / "totala1.hpi")
    fbi, n = re.subn(r"UnitName\s*=[^;]*;", f"UnitName={UNIT};",
                     ta.read("units/armsolar.fbi").decode("latin-1"), count=1, flags=re.I)
    if n != 1:
        sys.exit("cob_ext_fixture: ARMSOLAR's FBI has no UnitName")
    cob = tacob.write_cob(tacob.compile_bos(BOS.read_text(encoding="latin-1"), str(BOS)))
    tree = {}
    hpipack.insert(tree, f"units/{UNIT.lower()}.fbi", fbi.encode("latin-1"))
    hpipack.insert(tree, f"scripts/{UNIT.lower()}.cob", cob)
    (gamedir / "cobext.ufo").write_bytes(hpipack.build(tree))
    print(f"{gamedir / 'cobext.ufo'}: {UNIT}, {len(cob)} bytes of script")


def peek(instance, *specs):
    """The values of `specs` (tacli peek's grammar): an int for a number, a str for s<N>."""
    out = subprocess.run([str(TOOLS / "tacli"), "peek", instance, "--json", *specs], check=True,
                         capture_output=True, text=True).stdout
    rows = json.loads(out)["peeks"]
    if [r["spec"] for r in rows] != list(specs):
        sys.exit(f"cob_ext_fixture: tacli peek answered {len(rows)} of {len(specs)} specs")
    got = []
    for r in rows:
        v = r["value"]
        if v.startswith('"'):
            got.append(v[1:v.rindex('"')])          # the DLL stops at the first NUL
        elif re.match(r"^\d+", v):
            got.append(int(re.match(r"^\d+", v).group(0)))
        else:
            sys.exit(f"cob_ext_fixture: {r['spec']} reads {v}")
    return got


def i32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


def cobext_units(instance, ta, base):
    """[(slot, unit address)] of every live COBEXT in the first SCAN slots of seats 0 and 1."""
    slots = [first + k for first in (1, UNIT_LIMIT + 1) for k in range(SCAN)]
    words = peek(instance, *[f"0x{base + s * 0x118 + off:X}:4" for s in slots for off in (0x92, 0x110)])
    found = []
    types = {}
    for i, s in enumerate(slots):
        kind, mask = words[2 * i], words[2 * i + 1]
        if not mask & 0x10000000 or not kind:
            continue
        if kind not in types:
            types[kind], = peek(instance, f"0x{kind + 0x20:X}:s32")
        if types[kind].upper() == UNIT:
            found.append((s, base + s * 0x118))
    return found


def expected(instance, ta, slots, u):
    """What the getter rule must answer for the unit at `u`, from the engine's memory."""
    uid, = peek(instance, f"0x{u + 0xA8:X}:2")
    owner, kills, left = peek(instance, f"0x{u + 0xFF:X}:1", f"0x{u + 0xB8:X}:2", f"0x{u + 0x104:X}:4")
    player = ta + 0x1B63 + owner * 0x14B
    kind, allied = peek(instance, f"0x{player + 0x73:X}:1", f"0x{player + 0x108 + owner:X}:1")
    remaining = struct.unpack("<f", struct.pack("<I", left & 0xFFFFFFFF))[0]
    if remaining != 0.0:
        sys.exit(f"cob_ext_fixture: {UNIT} {uid} is not finished ({remaining}); the scenario spawns it whole")
    return {"min_id": 1, "max_id": slots - 1, "my_id": uid, "my_owner": owner,
            "my_build_left": 0, "nobodys_build_left": -1, "allied_with_me": int(allied != 0),
            "allied_past_the_array": 0, "mine_is_local": int(kind in (1, 2)),
            "my_kills": kills * 100}


def check(instance):
    ta, = peek(instance, f"0x{MAIN:X}:4")
    slots, base = peek(instance, f"0x{ta + 0x14351:X}:2", f"0x{ta + 0x14357:X}:4")
    units = cobext_units(instance, ta, base)
    bad = [] if len(units) >= 2 else [f"{len(units)} live {UNIT} in the scanned slots, not 2"]
    for slot, u in units:
        cob, = peek(instance, f"0x{u + 0x9A:X}:4")
        statics, = peek(instance, f"0x{cob + 0x10:X}:4")
        # me, done, then r0..r9 in GETTERS order (extension.bos)
        me, done, *answers = [i32(v) for v in peek(instance, *[f"0x{statics + 4 * k:X}:4"
                                                              for k in range(2 + len(GETTERS))])]
        want = expected(instance, ta, slots, u)
        if done != 1:
            bad.append(f"slot {slot}: the prober has not finished (done = {done})")
            continue
        if me != want["my_id"]:
            bad.append(f"slot {slot}: me = {me}, the unit's id is {want['my_id']}")
        for name, value in zip(GETTERS, answers):
            if value != want[name]:
                bad.append(f"slot {slot}: {name} = {value}, the engine's state says {want[name]}")
    for b in bad:
        print(f"cob_ext_fixture: {b}")
    print(f"cob_ext_fixture: {len(units)} {UNIT} x {len(GETTERS)} getters: "
          f"{'ALL MATCH' if not bad else f'{len(bad)} DIFFER'}")
    return 1 if bad else 0


def main():
    a = sys.argv[1:]
    if len(a) == 1:
        build(Path(a[0]))
        return 0
    if len(a) == 2 and a[0] == "check":
        return check(a[1])
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
