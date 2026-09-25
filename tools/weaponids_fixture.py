#!/usr/bin/env python3
"""weaponids_fixture — write the weapon-ID raise's test mod: weapons past stock's 256 and the
units that fire them, for research/notes/tadr-port/content-ids.md's A′3 tests.

Each weapon is a stock one under a new name and ID, and each unit a stock one under a new name
with its first weapon swapped. Both are read from the install at run time and written into one
archive where you point it: an instance's gamedir, never the repository (.gitignore refuses
.ufo). Stock's weapon IDs end at 246, so every ID here is free.

    tools/weaponids_fixture.py tagpu/instances/w1/gamedir/zzwid.ufo

Weapons, in weapons/weaponids.tdf:

    WID_MSL4000    ARMKBOT_MISSILE, the Rocko's, as ID 4000: a weapon drawn with a model
    WID_NOID       the same with no ID line, and WID_MSL5000 as ID 5000: both skipped at load
    WID_LAS253..255, WID_LAS3581..3583
                   ARM_LIGHTLASER as IDs 253..255 and 3581..3583, whose low bytes are the
                   feature sentinels' 0xFD..0xFF, with a blast wide enough to reach a feature's
                   cells: 1 damage below 256 and 5000 above, so a feature hit read as the wrong
                   weapon, or as a sentinel, ends differently
    WID_RKT250, WID_RKT3322
                   ARMTRUCK_ROCKET, the Merl's, as IDs 250 and 3322, one low byte, 0xFA: made
                   targetable, and slow, so an interceptor catches one long before its target and
                   the peer that fired it still has it in flight when the detonation arrives
    WID_AMD3000    AMD_ROCKET as ID 3000, a missile stocked every three seconds for little

Units: WIDAMD is an ARMAMD firing WID_AMD3000, with ARMAMD's build page, whose ARMMAKEANTI
queues a missile to stock (an interceptor fires only from stock). Every other is a Light Laser
Tower (ARMLLT) named for its weapon: WIDLLT4000, WIDLLTNOID, WIDLLT5000, WIDLAS253..255,
WIDLAS3581..3583, WIDRKT250, WIDRKT3322.

    --low          only the weapons below 256 and their towers: the control, for a build
                   without the raise, which would write the others past its array
"""

import argparse
import os
import re
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
hpipack = SourceFileLoader("hpipack", str(ROOT / "tools" / "hpipack.py")).load_module()


def gamedir():
    env = os.environ.get("TA_GAMEDIR")
    for cand in ([Path(env)] if env else []) + [
            ROOT / "tagpu" / "gamedir",
            Path.home() / ".local/share/Steam/steamapps/common/Total Annihilation"]:
        if (cand / "totala1.hpi").exists():
            return cand
    sys.exit("weaponids_fixture: no totala1.hpi found; set TA_GAMEDIR")


def section(text, name):
    """a [NAME] { ... } section with its nested [DAMAGE] block, as the TDF has it"""
    m = re.search(r"\[" + re.escape(name) + r"\]\s*\{", text, re.I)
    if not m:
        sys.exit(f"weaponids_fixture: no weapon {name} in the install")
    depth, i = 1, m.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def weapon(stock, name, wid, keys=None, damage=None):
    """stock's section as `name`, its ID replaced (None: no ID line), `keys` set or removed,
    and every entry of its [DAMAGE] block `damage` when given"""
    line = r"^[ \t]*{}[ \t]*=[^;\n]*;[^\n]*\n"
    body = re.sub(line.format("ID"), "", stock, count=1, flags=re.I | re.M)
    if damage is not None:
        head, block = body.split("[DAMAGE]", 1)
        body = head + "[DAMAGE]" + re.sub(r"=\s*\d+\s*;", f"={damage};", block)
    for k, v in (keys or {}).items():
        body = re.sub(line.format(k), "", body, flags=re.I | re.M)
        if v is not None:
            body = f"\r\n\t{k}={v};" + body
    if wid is not None:
        body = f"\r\n\tID={wid};" + body
    return f"[{name}]\r\n\t{{{body}}}\r\n"


def unit(fbi, name, weapon1):
    out, n = re.subn(r"UnitName\s*=[^;]*;", f"UnitName={name};", fbi, count=1, flags=re.I)
    out, m = re.subn(r"Weapon1\s*=[^;]*;", f"Weapon1={weapon1};", out, count=1, flags=re.I)
    if n != 1 or m != 1:
        sys.exit(f"weaponids_fixture: the stock FBI for {name} has no UnitName or Weapon1")
    return out.encode("latin-1")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--low", action="store_true",
                    help="only the weapons below 256 and their units: a control for a build without the raise")
    a = ap.parse_args()

    arc = hpipack.Archive(gamedir() / "totala1.hpi")
    text = "\n".join(arc.read(n).decode("latin-1") for n in arc.files if n.startswith("weapons/"))
    msl, las = section(text, "ARMKBOT_MISSILE"), section(text, "ARM_LIGHTLASER")
    rkt, amd = section(text, "ARMTRUCK_ROCKET"), section(text, "AMD_ROCKET")

    tdf = [weapon(msl, "WID_MSL4000", 4000), weapon(msl, "WID_NOID", None),
           weapon(msl, "WID_MSL5000", 5000)]
    tdf += [weapon(las, f"WID_LAS{i}", i, {"areaofeffect": 96}, damage=1 if i < 256 else 5000)
            for i in (253, 254, 255, 3581, 3582, 3583)]
    tdf += [weapon(rkt, f"WID_RKT{i}", i, {"targetable": 1, "weaponvelocity": 120,
                                              "weaponacceleration": 10, "flighttime": 20})
            for i in (250, 3322)]
    tdf += [weapon(amd, "WID_AMD3000", 3000, {"energypershot": 100, "metalpershot": 5, "reloadtime": 3})]

    llt, llt_cob = arc.read("units/armllt.fbi").decode("latin-1"), arc.read("scripts/armllt.cob")
    towers = {"WIDLLT4000": "WID_MSL4000", "WIDLLTNOID": "WID_NOID", "WIDLLT5000": "WID_MSL5000"}
    towers.update({f"WIDLAS{i}": f"WID_LAS{i}" for i in (253, 254, 255, 3581, 3582, 3583)})
    towers.update({f"WIDRKT{i}": f"WID_RKT{i}" for i in (250, 3322)})

    if a.low:
        tdf = [t for t in tdf if (m := re.search(r"\bID=(\d+);", t)) and int(m.group(1)) < 256]
        low = {re.match(r"\[(\w+)\]", t).group(1) for t in tdf}
        towers = {u: w for u, w in towers.items() if w in low}

    tree = {}
    hpipack.insert(tree, "weapons/weaponids.tdf", "\r\n".join(tdf).encode("latin-1"))
    for name, w in towers.items():
        hpipack.insert(tree, f"units/{name.lower()}.fbi", unit(llt, name, w))
        hpipack.insert(tree, f"scripts/{name.lower()}.cob", llt_cob)
    if not a.low:
        hpipack.insert(tree, "units/widamd.fbi",
                       unit(arc.read("units/armamd.fbi").decode("latin-1"), "WIDAMD", "WID_AMD3000"))
        hpipack.insert(tree, "scripts/widamd.cob", arc.read("scripts/armamd.cob"))
        # its build page, found by the unit's name: ARMMAKEANTI queues a missile to stock
        hpipack.insert(tree, "guis/widamd1.gui", arc.read("guis/armamd1.gui"))
        hpipack.insert(tree, "anims/widamd1.gaf", arc.read("anims/armamd1.gaf"))

    data = hpipack.build(tree)
    a.out.write_bytes(data)
    print(f"{a.out}: {len(tdf)} weapons, {len(towers) + (not a.low)} units, {len(data)} bytes")


if __name__ == "__main__":
    main()
