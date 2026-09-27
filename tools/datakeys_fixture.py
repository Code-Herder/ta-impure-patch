#!/usr/bin/env python3
"""datakeys_fixture — write section C's test mod: stock units under new names carrying the new
data keys, for research/notes/tadr-port/data-keys.md's tests.

Each unit is a stock one read from the install at run time, renamed and given one key, and
written into one archive where you point it: an instance's gamedir, never the repository
(.gitignore refuses .ufo).

    tools/datakeys_fixture.py tagpu/instances/c1/gamedir/zzkeys.ufo

C1, the build ghost's PreviewPieces= (ARMLLT's tree is base > turret > sleeve > barrel > flare).
All four are on the ARM Commander's fourth build page (ARMCOM4), in the four empty slots after
ARMTL, so their ghosts can be placed; a download TDF's MENU=n names page n-1:

    PPLLT     PreviewPieces=base: the ghost shows the base alone
    PPSEP     PreviewPieces= Base ,<tab>TURRET: case folds and every separator splits, so the
              ghost shows the base and the turret
    PPNONE    PreviewPieces=nosuchpiece: names no piece of the model, so it is ignored and
              the ghost takes Create()'s hides (the flare)
    PPLONG    a 64-character name, which no piece name can equal: the list is refused at load

C2, the weapon keys: stock weapons under new names and IDs (230..239, free in every archive and
below 256, so the stock-limits build loads them too), each with one key, in weapons/datakeys.tdf,
and the units that carry them. Spawned by scenarios/c2-weapon-keys.json, not built:

    WKLLTNA   ARMLLT, ARM_LIGHTLASER + nottoair              never engages a flying unit
    WKLLTNL   ARMLLT, ARM_LIGHTLASER + notoverland           on land: never fires
    WKLLTNW   ARMLLT, ARM_LIGHTLASER + notoverwater          on land: fires as stock
    WKLLTWK   ARMLLT, ARM_LIGHTLASER + nottoair, damage 1    holds a landed aircraft alive, so
                                                             it can take off under fire
    WKTLNU    ARMTL, COAX_TORPEDO + nottounderwater          never engages a submerged unit
    WKTLSF    ARMTL, COAX_TORPEDO + surfacefire              engages a hovercraft on the water
    WKFHLNW   ARMFHLT, ARMFHLT_LASER + notoverwater          on the water: never fires
    WKFHLNL   ARMFHLT, ARMFHLT_LASER + notoverland           on the water: fires as stock
    WKLLT5    ARMLLT, Weapon1 and Weapon4 WK_LAS_1 (the laser, no key, damage 1, so a target
              lives), Weapon5 the nottoair laser: the extra-weapons module's slots (tacli arm
              <i> weapons.on), COB from cobclone
    WKSUB     ARMSUB with no weapon: a submerged target that does not shoot back
    WKSHSF    ARMSH, a hovercraft, with WK_TORP_SF: the order cursor's surfacefire mirror
    WKSHTP    ARMSH with COAX_TORPEDO: its control
    WKAIR     ARMATLAS at MaxVelocity 1 carrying nothing: an aircraft its AI owner flies off slowly, for the
              order cursor's nottoair mirror (scenarios/c2-order-cursor.json)
    WKCOMSF   ARMCOM whose D-gun (Weapon3) is WK_DGUN_SF: ARM_DISINTEGRATOR made a water beam, with
              surfacefire and nottoair -- the shape of Escalation's DGUN_ARM
    WKCOMW    ARMCOM with WK_DGUN_W, the same beam without surfacefire: its control
    WKSUBVL   ARMSUB firing WK_VL_SF: ARMTRUCK_ROCKET, the Merl's vertical-launch rocket, made a water
              weapon with surfacefire -- Escalation's VLAUNCH_SUB_ARM shape. Its flight is the Merl's:
              Escalation's own numbers (weaponvelocity=-10, startvelocity=690) never reach a target
              on the stock engine, steering or not. ARMSUB's script has no
              AimPrimary, which a torpedo never starts; a vertical-launch weapon starts it and fires
              only on its result (0x49DB70 tests the slot's +8), so the clone's COB gets one that
              returns 1
    WKSUBVW   ARMSUB with WK_VL_W, the same missile without surfacefire: its control
              (these four: scenarios/c2-surfacefire.json)

and two weapons no unit carries, for the load's diagnostics: WK_LAS_SF, surfacefire without
waterweapon, and WK_LAS_NB, both notoverwater and notoverland (IDs 238, 239; WK_LAS_1 is 240,
the D-guns 241 and 242, the missiles 243 and 244).

C2's nomapweaponalert: HAILSTORM (the patch archive's meteors.tdf) as two weapons with default
damage 0 and 5 against ARMMSTOR, and two maps that rain them -- Show Down's terrain, small and
nearly all land, so a shower's random centre still covers most of it, under a new name each:

    WK_HAIL_Q  (247)  nomapweaponalert=1   the map "WK Hail"
    WK_HAIL_L  (248)  no key               the map "WK Hail C", its control

(245 and 246 are TREEBURN and SHRUBBURN, the burning features' weapons.)

B7's radar owner test: WK_HAIL_T (251), HAILSTORM with targetable=1 and default damage 0,
rained on the map "WK Hail T" -- its stones take the radar's marker branch, which reads a
shooter's owner out of sight.

C3, veterancy: ARMLLT under new names with VT_LAS (249), the light laser at damage 100
against every type, so a hit's amount reads straight off the target's health. Spawned by
scenarios/c3-*.json, with their kills set by the scenario:

    VTLLT0    no key: stock's level, min(kills / 5, 5)
    VTLLT1    VeterancyThresholds=1 2 3 ... 30 (a level a kill) and VeterancyAccuracyBuffRate=1
    VTRATE0   VeterancyAccuracyBuffRate=0 alone: no accuracy buff, stock's levels
    VTBAD1 .. VTBAD8, each refused at load and played as stock: thresholds "0", "5 5",
              "10 5", "-3", "1.5", 33 of them; rate "-3", rate "1.5"

Unarmed, so a victim or a capturer never fires back, each with VTLLT1's thresholds (1) or no key
(0): VTTGT0/1 (ARMLLT), VTCOM0/1 (ARMCOM, the capturers) and VTKROG0/1 (CORKROG, 29 918 HP to
count hits on). B7HUGE is ARMLLT with
WK_HUGE (250): the light laser at damage 65 000 and edge effectiveness 1, so its area pass deals
every hit whole -- past the HP word, where stock's wrap reads it as -536 and the victim GAINS 536.

Both maps set MeteorRadius=2800, MeteorDensity=400, MeteorDuration=35 (Flooded Glaciers' shower)
and MeteorInterval=20, and drop Show Down's useonlyunits. Spawned by scenarios/c2-nomapalert.json.

C4, transported explosions: unarmed ARM Commanders under new names, carried by the stock ARMATLAS,
with blasts that read straight off a CORKROG's health (COMMANDER_BLAST's look at area 2000 and edge
effectiveness 1, so every unit within 2000 takes the whole amount wherever the transport falls;
stock's COMMANDER_BLAST is 9999 over 950 at edge effectiveness 0.75):

    TXCOM1    TransportedExplodeAs=TX_BLAST_E (252, 111) and TransportedSelfDestructAs=TX_BLAST_S
              (253, 222)
    TXCOME    TransportedExplodeAs=TX_BLAST_E alone: a carried self-destruct is stock's
    TXNONE    TransportedExplodeAs=TX_NO_SUCH, which names no weapon: refused at load
    TXBIG     TXCOM1's keys at MaxDamage 32000, so it lives through its transport's 30 000

and three towers for the AI, which fire at what is flown or placed near them (a tower of the
player's own takes no `attack unit` against the player's ATLAS; an aircraft is hit well past a
weapon's range): TXRL with TX_AA
(254), ARMRL_MISSILE dealing 1000 but 1 to the commanders, so the ATLAS dies and its passenger
lives until its transport's cargo loop kills it; TXRL2 with TX_AA2 (255), dealing 1 but 65 000
to the commanders, so the passenger dies in a transport that lives; TXLLT2 with TX_LAS2 (229),
ARM_LIGHTLASER at range 200 with TX_AA2's damage, for a commander on the ground. VTCOM0 is the unkeyed
control. `--tx-damage N` writes TX_BLAST_E with N instead of 111: a second peer's TDF
for the same name, for the sync fold (def+0x146 of TXCOM1 differs, VTCOM0's does not).
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
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
    sys.exit("datakeys_fixture: no totala1.hpi found; set TA_GAMEDIR")


def unit(fbi, name, keys):
    """the stock FBI renamed, with `keys` added at the end of its [UNITINFO] section"""
    out, n = re.subn(r"UnitName\s*=[^;]*;", f"UnitName={name};", fbi, count=1, flags=re.I)
    out, m = re.subn(r"\}\s*$", "".join(f"\t{k}={v};\r\n" for k, v in keys.items()) + "\t}\r\n",
                     out.rstrip(), count=1)
    if n != 1 or m != 1:
        sys.exit(f"datakeys_fixture: the stock FBI for {name} has no UnitName or closing brace")
    return out.encode("latin-1")


def section(text, name):
    """a weapon's [NAME] { ... } body with its nested [DAMAGE] block"""
    m = re.search(r"\[" + re.escape(name) + r"\]\s*\{", text, re.I)
    if not m:
        sys.exit(f"datakeys_fixture: no weapon {name} in the install")
    depth, i = 1, m.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def weapon(stock, name, wid, keys, damage=None):
    """stock's body as `name` with ID `wid`, `keys` set, and every [DAMAGE] entry `damage`"""
    line = r"^[ \t]*{}[ \t]*=[^;\n]*;[^\n]*\n"
    body = stock
    if damage is not None:
        head, block = body.split("[DAMAGE]", 1)
        body = head + "[DAMAGE]" + re.sub(r"=\s*\d+\s*;", f"={damage};", block)
    for k, v in dict(keys, ID=wid).items():
        body = re.sub(line.format(k), "", body, flags=re.I | re.M)
        body = f"\r\n\t{k}={v};" + body
    return f"[{name}]\r\n\t{{{body}}}\r\n"


def armed(fbi, name, weapons, keys=None):
    """the stock FBI renamed, its WeaponN lines replaced by `weapons` ({n: name}; an empty
    dict leaves the unit unarmed), and `keys` set in place of the stock values"""
    out, n = re.subn(r"UnitName\s*=[^;]*;", f"UnitName={name};", fbi, count=1, flags=re.I)
    out = re.sub(r"^[ \t]*Weapon\d+\s*=[^;]*;[^\n]*\n", "", out, flags=re.I | re.M)
    for k in keys or {}:
        out = re.sub(r"^[ \t]*" + k + r"\s*=[^;]*;[^\n]*\n", "", out, flags=re.I | re.M)
    lines = "".join(f"\tWeapon{k}={v};\r\n" for k, v in sorted(weapons.items()))
    lines += "".join(f"\t{k}={v};\r\n" for k, v in (keys or {}).items())
    out, m = re.subn(r"\}\s*$", lines + "\t}\r\n", out.rstrip(), count=1)
    if n != 1 or m != 1:
        sys.exit(f"datakeys_fixture: the stock FBI for {name} has no UnitName or closing brace")
    return out.encode("latin-1")


def with_aim(cob):
    """the COB with an AimPrimary that returns 1 appended (PUSH_CONSTANT 1; RETURN)"""
    cobalias = SourceFileLoader("cobalias", str(ROOT / "tools" / "cobalias.py")).load_module()
    c = cobalias.parse(cob)
    words = list(struct.unpack(f"<{len(c['code']) // 4}I", c["code"]))
    c["scripts"].append(("AimPrimary", len(words)))
    words += [0x10021001, 1, 0x10065000]
    c["code"] = struct.pack(f"<{len(words)}I", *words)
    return cobalias.build(c)


def weapon_keys(tree, gd):
    """C2's weapons and units into `tree`"""
    ta, cc = hpipack.Archive(gd / "totala1.hpi"), hpipack.Archive(gd / "ccdata.ccx")
    text = "\n".join(ta.read(n).decode("latin-1") for n in ta.files if n.startswith("weapons/"))
    las, torp = section(text, "ARM_LIGHTLASER"), section(text, "COAX_TORPEDO")
    fhl = section(cc.read("weapons/armfhlt_weapon.tdf").decode("latin-1"), "ARMFHLT_LASER")
    tdf = [weapon(las, "WK_LAS_NA", 230, {"nottoair": 1}),
           weapon(las, "WK_LAS_NL", 231, {"notoverland": 1}),
           weapon(las, "WK_LAS_NW", 232, {"notoverwater": 1}),
           weapon(las, "WK_LAS_WK", 233, {"nottoair": 1}, damage=1),
           weapon(torp, "WK_TORP_NU", 234, {"nottounderwater": 1}),
           weapon(torp, "WK_TORP_SF", 235, {"surfacefire": 1}),
           weapon(fhl, "WK_FHL_NW", 236, {"notoverwater": 1}),
           weapon(fhl, "WK_FHL_NL", 237, {"notoverland": 1}),
           weapon(las, "WK_LAS_SF", 238, {"surfacefire": 1}),
           weapon(las, "WK_LAS_NB", 239, {"notoverwater": 1, "notoverland": 1}),
           weapon(las, "WK_LAS_1", 240, {}, damage=1)]
    dgun, rkt = section(text, "ARM_DISINTEGRATOR"), section(text, "ARMTRUCK_ROCKET")
    beam = {"waterweapon": 1, "beamweapon": 1, "nottoair": 1}
    flight = {"waterweapon": 1, "nottoair": 1}
    tdf += [weapon(dgun, "WK_DGUN_SF", 241, dict(beam, surfacefire=1)),
            weapon(dgun, "WK_DGUN_W", 242, beam),
            weapon(rkt, "WK_VL_SF", 243, dict(flight, surfacefire=1)),
            weapon(rkt, "WK_VL_W", 244, flight)]
    hpipack.insert(tree, "weapons/datakeys.tdf", "\r\n".join(tdf).encode("latin-1"))

    base = {"armllt": ta, "armtl": ta, "armsub": ta, "armatlas": ta, "armcom": ta, "armfhlt": cc,
            "armsh": cc}
    fbi = {u: arc.read(f"units/{u}.fbi").decode("latin-1") for u, arc in base.items()}
    cob = {u: arc.read(f"scripts/{u}.cob") for u, arc in base.items()}
    units = {"WKLLTNA": ("armllt", {1: "WK_LAS_NA"}), "WKLLTNL": ("armllt", {1: "WK_LAS_NL"}),
             "WKLLTNW": ("armllt", {1: "WK_LAS_NW"}), "WKLLTWK": ("armllt", {1: "WK_LAS_WK"}),
             "WKTLNU": ("armtl", {1: "WK_TORP_NU"}), "WKTLSF": ("armtl", {1: "WK_TORP_SF"}),
             "WKFHLNW": ("armfhlt", {1: "WK_FHL_NW"}), "WKFHLNL": ("armfhlt", {1: "WK_FHL_NL"}),
             "WKLLT5": ("armllt", {1: "WK_LAS_1", 4: "WK_LAS_1", 5: "WK_LAS_NA"}),
             "WKSUB": ("armsub", {}),
             "WKSHSF": ("armsh", {1: "WK_TORP_SF"}), "WKSHTP": ("armsh", {1: "COAX_TORPEDO"}),
             "WKAIR": ("armatlas", {}, {"MaxVelocity": 1, "TransportCapacity": 0, "transportsize": 0}),
             "WKCOMSF": ("armcom", {1: "ARMCOMLASER", 3: "WK_DGUN_SF"}),
             "WKCOMW": ("armcom", {1: "ARMCOMLASER", 3: "WK_DGUN_W"}),
             "WKSUBVL": ("armsub", {1: "WK_VL_SF"}), "WKSUBVW": ("armsub", {1: "WK_VL_W"})}
    with tempfile.TemporaryDirectory() as tmp:
        src, five = Path(tmp) / "armllt.cob", Path(tmp) / "wkllt5.cob"
        src.write_bytes(cob["armllt"])
        subprocess.run([sys.executable, str(ROOT / "tools" / "cobclone.py"), str(src), str(five),
                        "--weapons", "5"], check=True, stdout=subprocess.DEVNULL)
        extended = five.read_bytes()
    for name, (stock, weapons, *keys) in units.items():
        low = name.lower()
        hpipack.insert(tree, f"units/{low}.fbi", armed(fbi[stock], name, weapons, *keys))
        script = extended if name == "WKLLT5" else cob[stock]
        if name in ("WKSUBVL", "WKSUBVW"):
            script = with_aim(script)
        hpipack.insert(tree, f"scripts/{low}.cob", script)
    return len(tdf), len(units)


def weather(tree, gd):
    """nomapweaponalert's two hail weapons and the two maps that rain them"""
    hail = section(hpipack.Archive(gd / "rev31.gp3").read("weapons/meteors.tdf").decode("latin-1"),
                   "HAILSTORM")
    per_type = r"\1\r\n\t\tARMMSTOR=5;"
    tdf = [re.sub(r"(\[DAMAGE\]\s*\{)", per_type, weapon(hail, name, wid, keys, damage=0), count=1)
           for name, wid, keys in (("WK_HAIL_Q", 247, {"nomapweaponalert": 1}),
                                   ("WK_HAIL_L", 248, {}))]
    tdf.append(weapon(hail, "WK_HAIL_T", 251, {"targetable": 1}, damage=0))
    hpipack.insert(tree, "weapons/datakeys_hail.tdf", "\r\n".join(tdf).encode("latin-1"))
    cc = hpipack.Archive(gd / "ccmaps.ccx")
    ota, tnt = cc.read("maps/show down.ota").decode("latin-1"), cc.read("maps/show down.tnt")
    shower = {"MeteorRadius": 2800, "MeteorDensity": 400, "MeteorDuration": 35, "MeteorInterval": 20}
    for name, w in (("WK Hail", "WK_HAIL_Q"), ("WK Hail C", "WK_HAIL_L"), ("WK Hail T", "WK_HAIL_T")):
        o = re.sub(r"missionname=[^;]*;", f"missionname={name};", ota, count=1)
        o = re.sub(r"^[ \t]*useonlyunits=[^;]*;[^\n]*\n", "", o, flags=re.M | re.I)
        for k, v in dict(shower, MeteorWeapon=w).items():
            o, n = re.subn(k + r"=[^;]*;", f"{k}={v};", o, count=1)
            if n != 1:
                sys.exit(f"datakeys_fixture: Show Down's OTA has no {k}")
        hpipack.insert(tree, f"maps/{name.lower()}.ota", o.encode("latin-1"))
        hpipack.insert(tree, f"maps/{name.lower()}.tnt", tnt)
    return len(tdf)


def veterancy(tree, gd):
    """C3's keyed and malformed types, their unarmed targets and capturers, and B7's damage
    past the word"""
    ta, cc = hpipack.Archive(gd / "totala1.hpi"), hpipack.Archive(gd / "ccdata.ccx")
    text = "\n".join(ta.read(n).decode("latin-1") for n in ta.files if n.startswith("weapons/"))
    las = section(text, "ARM_LIGHTLASER")
    tdf = [weapon(las, "VT_LAS", 249, {}, damage=100),
           weapon(las, "WK_HUGE", 250, {"edgeeffectiveness": 1}, damage=65000)]
    hpipack.insert(tree, "weapons/datakeys_vet.tdf", "\r\n".join(tdf).encode("latin-1"))
    levels = " ".join(str(k) for k in range(1, 31))
    vet = {"VeterancyThresholds": levels}
    keyed = {"VTLLT0": {}, "VTLLT1": dict(vet, VeterancyAccuracyBuffRate=1),
             "VTRATE0": {"VeterancyAccuracyBuffRate": 0},
             "VTBAD1": {"VeterancyThresholds": "0"}, "VTBAD2": {"VeterancyThresholds": "5 5"},
             "VTBAD3": {"VeterancyThresholds": "10 5"}, "VTBAD4": {"VeterancyThresholds": "-3"},
             "VTBAD5": {"VeterancyThresholds": "1.5"},
             "VTBAD6": {"VeterancyThresholds": " ".join(str(k) for k in range(1, 34))},
             "VTBAD7": {"VeterancyAccuracyBuffRate": "-3"}, "VTBAD8": {"VeterancyAccuracyBuffRate": "1.5"},
             "B7HUGE": {}}
    unarmed = {"VTTGT0": ("armllt", {}), "VTTGT1": ("armllt", vet),
               "VTCOM0": ("armcom", {}), "VTCOM1": ("armcom", vet),
               "VTKROG0": ("corkrog", {}), "VTKROG1": ("corkrog", vet)}
    fbi, cob = ta.read("units/armllt.fbi").decode("latin-1"), ta.read("scripts/armllt.cob")
    for name, keys in keyed.items():
        gun = "WK_HUGE" if name == "B7HUGE" else "VT_LAS"
        hpipack.insert(tree, f"units/{name.lower()}.fbi", armed(fbi, name, {1: gun}, keys))
        hpipack.insert(tree, f"scripts/{name.lower()}.cob", cob)
    for name, (stock, keys) in unarmed.items():
        arc = cc if stock == "corkrog" else ta
        body = arc.read(f"units/{stock}.fbi").decode("latin-1")
        hpipack.insert(tree, f"units/{name.lower()}.fbi", armed(body, name, {}, keys))
        hpipack.insert(tree, f"scripts/{name.lower()}.cob", arc.read(f"scripts/{stock}.cob"))
    return len(tdf), len(keyed) + len(unarmed)


def damage(body, default, per):
    """the weapon body with its [DAMAGE] block replaced: `default` and the {unit: amount} entries"""
    head, rest = body.split("[DAMAGE]", 1)
    tail = rest[rest.index("}") + 1:]
    lines = "".join(f"\t\t{k}={v};\r\n" for k, v in dict(default=default, **per).items())
    return f"{head}[DAMAGE]\r\n\t\t{{\r\n{lines}\t\t}}{tail}"


def transported(tree, gd, tx_damage):
    """C4's keyed commanders, their blasts and the two towers that bring a transport down"""
    ta = hpipack.Archive(gd / "totala1.hpi")
    text = "\n".join(ta.read(n).decode("latin-1") for n in ta.files if n.startswith("weapons/"))
    blast, aa = section(text, "COMMANDER_BLAST"), section(text, "ARMRL_MISSILE")
    las = section(text, "ARM_LIGHTLASER")
    area = {"areaofeffect": 2000, "edgeeffectiveness": 1}
    coms = ("TXCOM1", "TXCOME", "TXNONE", "TXBIG", "VTCOM0")
    tdf = [weapon(blast, "TX_BLAST_E", 252, area, damage=tx_damage),
           weapon(blast, "TX_BLAST_S", 253, area, damage=222),
           damage(weapon(aa, "TX_AA", 254, {}), 1000, {c: 1 for c in coms}),
           damage(weapon(aa, "TX_AA2", 255, {}), 1, {c: 65000 for c in coms}),
           damage(weapon(las, "TX_LAS2", 229, {"range": 200}), 1, {c: 65000 for c in coms})]
    hpipack.insert(tree, "weapons/datakeys_tx.tdf", "\r\n".join(tdf).encode("latin-1"))
    com, rl, llt = (ta.read(f"units/{u}.fbi").decode("latin-1") for u in ("armcom", "armrl", "armllt"))
    units = {"TXCOM1": (com, {}, {"TransportedExplodeAs": "TX_BLAST_E",
                                  "TransportedSelfDestructAs": "TX_BLAST_S"}),
             "TXCOME": (com, {}, {"TransportedExplodeAs": "TX_BLAST_E"}),
             "TXNONE": (com, {}, {"TransportedExplodeAs": "TX_NO_SUCH"}),
             "TXBIG": (com, {}, {"TransportedExplodeAs": "TX_BLAST_E",
                                 "TransportedSelfDestructAs": "TX_BLAST_S", "MaxDamage": 32000}),
             "TXRL": (rl, {1: "TX_AA"}, {}), "TXRL2": (rl, {1: "TX_AA2"}, {}),
             "TXLLT2": (llt, {1: "TX_LAS2"}, {})}
    for name, (fbi, weapons, keys) in units.items():
        stock = {id(com): "armcom", id(rl): "armrl", id(llt): "armllt"}[id(fbi)]
        hpipack.insert(tree, f"units/{name.lower()}.fbi", armed(fbi, name, weapons, keys))
        hpipack.insert(tree, f"scripts/{name.lower()}.cob", ta.read(f"scripts/{stock}.cob"))
    return len(tdf), len(units)


def menu(builder, page, button, name):
    return (f"[MENUENTRY1]\r\n\t{{\r\n\tUNITMENU={builder};\r\n\tMENU={page};\r\n"
            f"\tBUTTON={button};\r\n\tUNITNAME={name};\r\n\t}}\r\n").encode("latin-1")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--tx-damage", type=int, default=111,
                    help="TX_BLAST_E's damage (111): another value is another peer's TDF")
    a = ap.parse_args()

    gd = gamedir()
    arc = hpipack.Archive(gd / "totala1.hpi")
    fbi = arc.read("units/armllt.fbi").decode("latin-1")
    cob, pic = arc.read("scripts/armllt.cob"), arc.read("unitpics/armllt.pcx")
    clones = {
        "PPLLT":  "base",
        "PPSEP":  " Base ,\tTURRET",
        "PPNONE": "nosuchpiece",
        "PPLONG": "p" * 64,
    }
    tree = {}
    for button, (name, value) in enumerate(clones.items()):
        low = name.lower()
        hpipack.insert(tree, f"units/{low}.fbi", unit(fbi, name, {"PreviewPieces": value}))
        hpipack.insert(tree, f"scripts/{low}.cob", cob)
        hpipack.insert(tree, f"unitpics/{low}.pcx", pic)
        hpipack.insert(tree, f"download/{low}.tdf", menu("ARMCOM", 5, button + 1, name))

    nw, nu = weapon_keys(tree, gd)
    nw += weather(tree, gd)
    vw, vu = veterancy(tree, gd)
    tw, tu = transported(tree, gd, a.tx_damage)

    data = hpipack.build(tree)
    a.out.write_bytes(data)
    print(f"{a.out}: {len(clones) + nu + vu + tu} units, {nw + vw + tw} weapons, 3 maps, "
          f"{len(data)} bytes")


if __name__ == "__main__":
    main()
