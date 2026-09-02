#!/usr/bin/env python3
"""warlordex_content — build scenarios/content/warlordex.ufo, the WarlordEx unit.

WarlordEx (`CORBATSX`) is the Core Warlord with fifty model units of hull spliced
into its midbody and four Light Laser Tower turrets standing on the deck that
makes. It is the first unit in this repo whose *geometry* is generated rather
than reused, and it exists to give the extra-weapons module a unit that looks
like what it is: seven weapons, seven visible mounts.

    tools/warlordex_content.py                       # writes scenarios/content/warlordex.ufo
    TA_GAMEDIR=/path/to/steam/TA tools/warlordex_content.py

What goes in the archive:

  objects3d/CORBATSX.3do   `ta3domod`: one --stretch of the hull at z=6.5, then
      four --grafts of CORLLT's `stand` subtree (pedestal, gun, muzzle flare)
      onto the new deck as lasr1..4 / gun1..4 / flare1..4.
  scripts/corbatsx.cob     CORBATS.COB with those twelve pieces appended to its
      piece table, a Create that also hides the four new flares, and
      Aim/Fire/AimFrom/QueryWeapon4..7 written straight as bytecode — one
      turret each, one signal bit each, modelled on CORLLT's own aim script.
  units/corbatsx.fbi       CORBATS.FBI with Weapon4..7=CORE_LIGHTLASER, a longer
      footprint and a much bigger hull (capped at the int16 health field). Stats are otherwise the stock Warlord's — read through
      the merged archive view, so it is Core Contingency's CORBATS.FBI out of
      ccdata.ccx that gets inherited, not the older one in totala1.hpi.
  unitpics/CORBATSX.pcx    the Warlord's build picture, unchanged.
  units/armroyh.fbi        ARMROYH "Crusader (Hold)", the stock Crusader with
      `NoAutoFire=1` and nothing else changed — it keeps ARMROY's model and gets
      a copy of its script. A scenario `stance` only covers the movement half of
      TA's two order groups; this is the fire half, and it makes a ring of them
      into targets that neither shoot back nor run, which is what you want when
      the thing being watched is whether four new turrets track and fire.
  download/corbatsx.tdf    the build-menu registration Core Contingency uses for
      every unit it adds to a stock factory: a `[MENUENTRY]` naming the builder,
      the page and the button. WarlordEx takes CORASY page 3 button 3, the first
      slot the shipped `download/*.tdf` files leave free on the Core advanced
      shipyard.

One unit, one archive: drop `warlordex.ufo` in the game directory and the
Warlord's longer sibling appears in the advanced shipyard. Nothing outside the
archive is touched, and deleting it removes the unit.

Then, as with `extra_weapons_fixture.py`: `tacli arm <inst> weapons.on`, drop the
.ufo in the instance gamedir, and spawn CORBATSX.
"""

import os
import re
import struct
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
hpipack = SourceFileLoader("hpipack", str(TOOLS / "hpipack.py")).load_module()
cobalias = SourceFileLoader("cobalias", str(TOOLS / "cobalias.py")).load_module()
ta3domod = SourceFileLoader("ta3domod", str(TOOLS / "ta3domod")).load_module()

OUT = ROOT / "scenarios" / "content" / "warlordex.ufo"

# The splice: 50 model units inserted at z=6.5, which is aft of the forward deck
# houses (z<=5.27) and forward of the aft superstructure (z>=7.5), so the cut
# falls in the hull's parallel midbody and only the side and deck panels stretch.
CUT_Z = 6.5
INSERT = 50.0
# Where the four turrets stand on the deck it opens. y=11.09 is the deck plane.
MOUNTS = [(-11.0, 11.09, 20.0), (11.0, 11.09, 20.0),
          (-11.0, 11.09, 44.0), (11.0, 11.09, 44.0)]
# The same High Energy Laser the Warlord's own tri-barrel turret fires
# (weapons/lasers.tdf: range 810, 180 damage, 75 energy a shot). The Light
# Laser it used to carry only reached 300, which is inside the range every
# ship it meets opens fire from — the battery never got a shot off.
LASER = "CORE_BATSLASER"
FIRST_SLOT = 4
# The Warlord's 6140 is a fair fight against a few ships and a very short one
# against a fleet, so the demo unit is tougher. The ceiling is not a balance
# choice: a live unit's current health is `short Health` at unit+0x108
# (tamem_ghidra.h:1107), so a MaxDamage above 32767 spawns the ship with
# negative health and it dies on the frame it appears. x10 does exactly that.
HULL_MULTIPLIER = 5
HULL_CAP = 32767

# How AimWeaponN reports "aimed". The aim script's *return* is what the slot
# waits on before it will fire, and a script that waits holds one of the unit's
# COB threads for the whole slew. A unit gets EIGHT (COBEngine_AllocThread,
# 0x4B08C0, scans a fixed eight 0xA4-byte records at cob+0x1C and returns -1
# when they are all busy) and a warship already spends most of them: two stock
# aim scripts, their RestoreAfterDelay, SmokeUnit, the fire scripts. Four more
# waiting aim scripts do not fit, and the failure is silent — QueryScript on a
# full pool returns without touching its out-parameter, so AimFromWeaponN
# resolves to nothing, the slot aims from piece 0 and stops tracking. That is
# the "one turret won't move to aim" bug; see extra-weapons.md snag 10.
#
# So the turret slews from a script that returns inside its own tick, and the
# module supplies the guarantee the `wait-for-turn` used to: it re-solves every
# tick (a `turn` keeps running without a thread once issued) and holds fire
# until the muzzle piece actually points at the target. Measured on
# scenarios/warlordex-vs-fleet, shots on slots 4..7 over one run:
#
#   full      signal, turn, wait, return 1     0  2  0  6   thread-starved
#   track     turn, wait, return 1            32 32 34 23   starved, poisoned aim
#   snap      turn-now, return 1              48 10 10  5   on target, no slew
#   slew      turn, return 1                  19 18 25 19   even, and aligned
#
# `slew` reads lower than `track` only because it now declines the shots it used
# to take while the barrel was still swinging (the module counts those as
# `hold_fire`). It is the only one of the four that both keeps all four turrets
# alive and puts the beam out of a barrel that is pointing at the target.
AIM_STYLE = "slew"
# CORASY page 3: the shipped download/*.tdf take buttons 0-2 (CORSJAM, CORARCH,
# CORSSUB), so 3 is the first free slot on the Core advanced shipyard.
BUILD_MENU = ("CORASY", 3, 3)

# --------------------------------------------------------------------------- COB

OP = {
    "MOVE": 0x10001000, "TURN": 0x10002000, "SHOW": 0x10005000, "HIDE": 0x10006000,
    "SLEEP": 0x10013000, "WAIT_FOR_TURN": 0x10011000, "TURN_NOW": 0x1000C000, "PUSH_CONSTANT": 0x10021001,
    "PUSH_LOCAL_VAR": 0x10021002, "CREATE_LOCAL_VAR": 0x10022000,
    "POP_LOCAL_VAR": 0x10023002, "SUB": 0x10032000, "JUMP": 0x10064000,
    "RETURN": 0x10065000, "JUMP_NOT_EQUAL": 0x10066000,
    "SIGNAL": 0x10067000, "SET_SIGNAL_MASK": 0x10068000,
}
AXIS_X, AXIS_Y = 0, 1
# CORLLT aims its stand at 0xd555 and its gun at 0x8e38; keep the feel.
YAW_SPEED, PITCH_SPEED = 0xD555, 0x8E38
FLARE_FRAMES = 0x96          # CORLLT shows its muzzle flare for this long


def words(*items):
    return list(items)


def used_signal_bits(code):
    """Every constant this script pushes straight into SIGNAL/SET_SIGNAL_MASK."""
    ops = struct.unpack(f"<{len(code) // 4}I", code)
    bits = 0
    for i in range(len(ops) - 2):
        if ops[i] == OP["PUSH_CONSTANT"] and ops[i + 2] in (OP["SIGNAL"], OP["SET_SIGNAL_MASK"]):
            bits |= ops[i + 1]
    return bits


def free_signal_bits(code, count):
    taken = used_signal_bits(code)
    out = []
    bit = 1
    while len(out) < count:
        if not taken & bit:
            out.append(bit)
        bit <<= 1
        if bit > 0x40000000:
            raise SystemExit("warlordex_content: no free signal bits left")
    return out


def script_body(cob, start):
    """One script's words, entry point to its first RETURN.

    Only used for `Create`, which is a straight line — the copy is refused if a
    jump shows up, because a jump target is absolute and would not survive being
    moved to the end of the code array."""
    ops = list(struct.unpack(f"<{len(cob['code']) // 4}I", cob["code"]))
    operands = {OP["PUSH_CONSTANT"]: 1, OP["PUSH_LOCAL_VAR"]: 1, OP["POP_LOCAL_VAR"]: 1,
                OP["HIDE"]: 1, OP["SHOW"]: 1, OP["SLEEP"]: 0, OP["SUB"]: 0,
                OP["CREATE_LOCAL_VAR"]: 0, OP["SIGNAL"]: 0, OP["SET_SIGNAL_MASK"]: 0,
                OP["RETURN"]: 0, OP["TURN"]: 2, OP["WAIT_FOR_TURN"]: 2,
                0x10023004: 1, 0x10021004: 1, 0x10061000: 2}
    pc = start
    while pc < len(ops):
        op = ops[pc]
        if op in (OP["JUMP"], OP["JUMP_NOT_EQUAL"]):
            raise SystemExit(f"warlordex_content: Create jumps at {pc}; cannot relocate it")
        if op not in operands:
            raise SystemExit(f"warlordex_content: unknown opcode {op:#x} at {pc}")
        pc += 1 + operands[op]
        if op == OP["RETURN"]:
            return ops[start:pc]
    raise SystemExit("warlordex_content: Create has no RETURN")


def aim_script(stand, gun, signal, style="full"):
    """Turn the mount to the heading and the gun to the pitch, then report aimed.

    `style` exists because the aim script's *return* is what the extra-weapons
    module waits on before it will fire the slot. Measured against
    scenarios/warlordex-vs-fleet (shots on slots 4..7 in one 40-ship run):
      track     turn both axes at CORLLT's speeds, wait for both turns, return 1
                — Cavedog's shape minus the signal/set-signal-mask pair. 32/32/34/23,
                and the barrel is genuinely on target when the shot leaves it.
                This is what ships.
      slew      turn both and return 1 at once, no signal and no wait: 37/22/21/10,
                but the slot reports aimed while the turret is still slewing, so
                the beam leaves a barrel that is not yet pointing at the target.
      snap      turn-now both axes and return 1: 48/10/10/5 — always on target,
                but the turret teleports to the angle instead of slewing.
      full      Cavedog's own shape, signal and all: 0/2/0/6. The signal pair is
                what kills it; `track` is this minus those two words.
      nosignal  the old name for `track`, still accepted.
      nowait    the old name for `slew`, still accepted.
      instant   return 1 and turn nothing, the isolation test."""
    body = [OP["CREATE_LOCAL_VAR"], OP["CREATE_LOCAL_VAR"]]
    if style == "slew":
        return words(*body,
                     OP["PUSH_CONSTANT"], YAW_SPEED, OP["PUSH_LOCAL_VAR"], 0,
                     OP["TURN"], stand, AXIS_Y,
                     OP["PUSH_CONSTANT"], PITCH_SPEED,
                     OP["PUSH_CONSTANT"], 0, OP["PUSH_LOCAL_VAR"], 1, OP["SUB"],
                     OP["TURN"], gun, AXIS_X,
                     OP["PUSH_CONSTANT"], 1, OP["RETURN"])
    if style == "snap":
        # No signal/set-signal-mask: turn-now finishes inside the tick, so there
        # is no long-lived thread to protect — and the pair is precisely what
        # stops the slot ever reporting aimed (see AIM_STYLE).
        return words(*body,
                     OP["PUSH_LOCAL_VAR"], 0, OP["TURN_NOW"], stand, AXIS_Y,
                     OP["PUSH_CONSTANT"], 0, OP["PUSH_LOCAL_VAR"], 1, OP["SUB"],
                     OP["TURN_NOW"], gun, AXIS_X,
                     OP["PUSH_CONSTANT"], 1, OP["RETURN"])
    if style != "instant":
        if style not in ("nosignal", "track"):
            body += [OP["PUSH_CONSTANT"], signal, OP["SIGNAL"],
                     OP["PUSH_CONSTANT"], signal, OP["SET_SIGNAL_MASK"]]
        body += [OP["PUSH_CONSTANT"], YAW_SPEED, OP["PUSH_LOCAL_VAR"], 0,
                 OP["TURN"], stand, AXIS_Y,
                 OP["PUSH_CONSTANT"], PITCH_SPEED,
                 OP["PUSH_CONSTANT"], 0, OP["PUSH_LOCAL_VAR"], 1, OP["SUB"],
                 OP["TURN"], gun, AXIS_X]
    if style in ("full", "nosignal", "track"):
        body += [OP["WAIT_FOR_TURN"], stand, AXIS_Y,
                 OP["WAIT_FOR_TURN"], gun, AXIS_X]
    return words(*body, OP["PUSH_CONSTANT"], 1, OP["RETURN"])


def fire_script(flare):
    return words(OP["SHOW"], flare,
                 OP["PUSH_CONSTANT"], FLARE_FRAMES, OP["SLEEP"],
                 OP["HIDE"], flare,
                 OP["PUSH_CONSTANT"], 0, OP["RETURN"])


def piece_script(piece):
    """AimFrom/Query: hand the caller a piece index back in local var 0."""
    return words(OP["CREATE_LOCAL_VAR"],
                 OP["PUSH_CONSTANT"], piece, OP["POP_LOCAL_VAR"], 0,
                 OP["PUSH_CONSTANT"], 0, OP["RETURN"])


def build_cob(blob: bytes, turrets, aim_style="full") -> bytes:
    """CORBATS.COB + one aim/fire/query/aimfrom set per grafted turret.

    New code is appended, never inserted, so every absolute jump target already
    in the file still points where it did. `Create` is the one existing script
    that changes: its entry is repointed at a copy of itself with the four new
    flares hidden first, which is what stops them showing as cones on the deck."""
    cob = cobalias.parse(blob)
    index = {name.lower(): i for i, name in enumerate(cob["pieces"])}
    for turret in turrets:
        for piece in turret:
            if piece.lower() in index:
                raise SystemExit(f"warlordex_content: piece {piece} already in the COB")
            index[piece.lower()] = len(cob["pieces"])
            cob["pieces"].append(piece)

    code = list(struct.unpack(f"<{len(cob['code']) // 4}I", cob["code"]))
    scripts = {name.lower(): (n, entry) for n, (name, entry) in enumerate(cob["scripts"])}
    if "create" not in scripts:
        raise SystemExit("warlordex_content: CORBATS.COB has no Create")

    def append(name, body):
        entry = len(code)
        code.extend(body)
        cob["scripts"].append((name, entry))
        return entry

    hides = []
    for _, _, flare in turrets:
        hides += [OP["HIDE"], index[flare.lower()]]
    slot_n, create_entry = scripts["create"]
    cob["scripts"][slot_n] = ("Create", len(code))
    code.extend(hides + script_body(cob, create_entry))

    signals = free_signal_bits(cob["code"], len(turrets))
    for n, ((stand, gun, flare), signal) in enumerate(zip(turrets, signals)):
        slot = FIRST_SLOT + n
        s, g, f = index[stand.lower()], index[gun.lower()], index[flare.lower()]
        append(f"AimWeapon{slot}", aim_script(s, g, signal, aim_style))
        append(f"FireWeapon{slot}", fire_script(f))
        append(f"AimFromWeapon{slot}", piece_script(s))
        append(f"QueryWeapon{slot}", piece_script(f))

    cob["code"] = struct.pack(f"<{len(code)}I", *code)
    out = cobalias.build(cob)
    check = cobalias.parse(out)
    assert check["pieces"] == cob["pieces"] and check["scripts"] == cob["scripts"]
    assert check["code"] == cob["code"]
    return out


# --------------------------------------------------------------------------- FBI

def build_fbi(text: str) -> str:
    damage = re.search(r"(?im)^\s*MaxDamage=(\d+);", text)
    if not damage:
        raise SystemExit("warlordex_content: CORBATS.FBI has no MaxDamage")
    hull = int(damage.group(1)) * HULL_MULTIPLIER
    if hull > HULL_CAP:
        print(f"  note:  MaxDamage {hull} exceeds the int16 health field; clamped to {HULL_CAP}")
        hull = HULL_CAP
    tougher = f"MaxDamage={hull};"
    extra = "".join(f"\r\n\tWeapon{n}={LASER};\r\n\tw{n}_badTargetCategory=VTOL;"
                    for n in range(FIRST_SLOT, FIRST_SLOT + len(MOUNTS)))
    swaps = [
        ("UnitName=CORBATS;", "UnitName=CORBATSX;"),
        ("Objectname=CORBATS;", "Objectname=CORBATSX;"),
        ("Name=Warlord;", "Name=WarlordEx;"),
        ("Description=Battleship;", "Description=Battleship, seven mounts;"),
        # the hull is 191 model units long where the Warlord's is 141
        ("FootprintZ=6;", "FootprintZ=8;"),
        (damage.group(0).strip(), tougher),
        (f"Weapon2=COR_BATS;", f"Weapon2=COR_BATS;{extra}"),
    ]
    for old, new in swaps:
        if old not in text:
            raise SystemExit(f"warlordex_content: CORBATS.FBI has no {old!r}")
        text = text.replace(old, new, 1)
    return text


def build_holdfire_fbi(text: str) -> str:
    """The stock Crusader, told not to acquire targets on its own."""
    swaps = [("UnitName=ARMROY;", "UnitName=ARMROYH;"),
             ("Name=Crusader;", "Name=Crusader (Hold);"),
             ("NoAutoFire=0;", "NoAutoFire=1;")]
    for old, new in swaps:
        if old not in text:
            raise SystemExit(f"warlordex_content: ARMROY.FBI has no {old!r}")
        text = text.replace(old, new, 1)
    if "Objectname=ARMROY;" not in text:
        raise SystemExit("warlordex_content: ARMROY.FBI has no Objectname=ARMROY")
    return text


def build_menu_entry() -> str:
    builder, menu, button = BUILD_MENU
    return ("[MENUENTRY1]\r\n\t{\r\n"
            f"\tUNITMENU={builder};\r\n"
            f"\tMENU={menu};\r\n"
            f"\tBUTTON={button};\r\n"
            "\tUNITNAME=CORBATSX;\r\n\t}\r\n")


# --------------------------------------------------------------------------- main

def build_model(source: ta3domod.Source) -> bytes:
    root = ta3domod.parse_3do(source.read("corbats"))
    moved = ta3domod.stretch(root.find("base"), "z", CUT_Z, INSERT)
    donor = ta3domod.parse_3do(source.read("corllt")).find("stand")
    turrets = []
    for n, offset in enumerate(MOUNTS, start=1):
        clone = ta3domod.graft(root, "base", donor, offset, name=f"lasr{n}")
        turrets.append(tuple(piece.name for piece, _ in clone.walk()))
    print(f"  model: hull +{INSERT:g} at z={CUT_Z:g} ({moved[0]} vertices, {moved[1]} pieces "
          f"moved aft), {len(turrets)} turrets, {sum(1 for _ in root.walk())} pieces")
    return ta3domod.write_3do(root), turrets


def main():
    aim_style = AIM_STYLE
    for arg in sys.argv[1:]:
        if arg.startswith("--aim="):
            aim_style = arg.split("=", 1)[1]
    if aim_style not in ("track", "slew", "snap", "full", "nosignal", "nowait", "instant"):
        sys.exit("warlordex_content: --aim must be track, slew, snap, full, "
                 "nosignal, nowait or instant")
    # Read through the merged archive view, not totala1.hpi: Core Contingency
    # ships its own CORBATS.FBI in ccdata.ccx and that is the Warlord the game
    # actually loads, so it is the one WarlordEx should inherit from.
    source = ta3domod.Source()
    archive = source.assets
    model, turrets = build_model(source)
    cob = build_cob(archive.read("scripts/corbats.cob"), turrets, aim_style)
    fbi = build_fbi(archive.read("units/corbats.fbi").decode("latin-1"))
    print(f"  cob:   {len(cobalias.parse(cob)['scripts'])} scripts, "
          f"{len(cobalias.parse(cob)['pieces'])} pieces, aim={aim_style}")

    tree = {}
    for name, data in (("units/corbatsx.fbi", fbi.encode("latin-1")),
                       ("objects3d/CORBATSX.3do", model),
                       ("scripts/corbatsx.cob", cob),
                       ("unitpics/CORBATSX.pcx", archive.read("unitpics/corbats.pcx")),
                       ("download/corbatsx.tdf", build_menu_entry().encode("latin-1")),
                       ("units/armroyh.fbi",
                        build_holdfire_fbi(archive.read("units/armroy.fbi")
                                           .decode("latin-1")).encode("latin-1")),
                       ("scripts/armroyh.cob", archive.read("scripts/armroy.cob")),
                       ("unitpics/ARMROYH.pcx", archive.read("unitpics/armroy.pcx"))):
        hpipack.insert(tree, name, data)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(hpipack.build(tree))
    builder, menu, button = BUILD_MENU
    print(f"  menu:  {builder} page {menu} button {button}")
    print(f"{OUT} ({OUT.stat().st_size} bytes, {sum(1 for _ in hpipack.walk(tree))} files)")


if __name__ == "__main__":
    main()
