#!/usr/bin/env python3
"""cob_ext_fixture -- the COB extensions as a unit, and the check of what its getters answered.

    tools/cob_ext_fixture.py <gamedir>                    write cobext.ufo and arm the trace
    tools/cob_ext_fixture.py check <instance> <gamedir>   judge the running game's trace

COBEXT is ARMSOLAR's model and FBI under a new name, with the script compiled from
research/notes/evidence/cobtrace/extension.bos: Create and seven sleepers fill records 0..7, so
every getter the prober calls runs in record 9, one of the two Impure's raised build adds. The
archive goes into a test instance's game folder (the replay group `cob-getters`, the tacob
fixture `extension`), never into the repository (.gitignore refuses .ufo) or a player's install.

`check` reads every COBEXT's R lines out of the cobtrace log and compares each getter's value
with the engine's own state, read through `tacli peek`: the unit array main+0x14357 (stride
0x118: the id at +0xA8, the kills at +0xB8, the build fraction at +0x104, the owner at +0xFF),
its slot count main+0x14351, and the player records from main+0x1B63 (stride 0x14B: the kind at
+0x73, the alliance bytes at +0x108). So the getter rule (tagpu_cob_getter) is judged against
the engine, not against a second copy of itself. Exit 1 names every difference.
"""
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
    (gamedir / "tagpu_cobtrace.on").write_text(UNIT)
    print(f"{gamedir / 'cobext.ufo'}: {UNIT}, {len(cob)} bytes of script; cobtrace armed for it")


def peek(instance, *specs):
    out = subprocess.run([str(TOOLS / "tacli"), "peek", instance, *specs], check=True,
                         capture_output=True, text=True).stdout
    got = {}
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] in specs:
            got[parts[0]] = int(parts[-2]) if parts[-1].startswith("(0x") else int(parts[-1])
    missing = [s for s in specs if s not in got]
    if missing:
        sys.exit(f"cob_ext_fixture: tacli peek gave nothing for {missing}")
    return [got[s] for s in specs]


def expected(instance, uid):
    """What the getter rule must answer for unit `uid`, from the engine's memory."""
    ta, = peek(instance, f"0x{MAIN:X}:4")
    slots, base = peek(instance, f"0x{ta + 0x14351:X}:2", f"0x{ta + 0x14357:X}:4")
    u = base + uid * 0x118
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


def check(instance, gamedir):
    talog = load("talog", TOOLS / "talog.py")
    trace = talog.run_text(gamedir, "tagpu_cobtrace")
    units = {int(f[2]) for f in (l.split("\t") for l in trace.splitlines())
             if len(f) > 4 and f[0] == "S" and f[3] == UNIT and f[4] == "Create"}
    if not units:
        sys.exit(f"cob_ext_fixture: no {UNIT} Create in the trace")
    seen = {}
    for line in trace.splitlines():
        f = line.split("\t")
        if len(f) == 6 and f[0] == "R" and int(f[2]) in units and f[4] in GETTERS:
            seen.setdefault((int(f[2]), f[4]), (int(f[3]), int(f[5])))
    bad = []
    for uid in sorted(units):
        want = expected(instance, uid)
        for name in GETTERS:
            if (uid, name) not in seen:
                bad.append(f"unit {uid}: {name} never returned")
                continue
            slot, value = seen[(uid, name)]
            if slot != 9:
                bad.append(f"unit {uid}: {name} ran in record {slot}, not 9")
            if value != want[name]:
                bad.append(f"unit {uid}: {name} = {value}, the engine's state says {want[name]}")
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
    if len(a) == 3 and a[0] == "check":
        return check(a[1], Path(a[2]))
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
