#!/usr/bin/env python3
"""unittypes_fixture — write a mod of N synthetic unit types, for the unit-type raise's tests.

Each type is SYN00001, SYN00002, ...: an FBI this tool writes, naming a stock model by
name (the Peewee's, `armpw`, unless --model says otherwise), and one small COB compiled
from the script below with tools/tacob. Each type's copy of the COB ends in the type's
number, which the engine loads with it and never runs, so each type has its own unit-sync
value (0x42A610 checksums the script) and a type the sync pairs with the wrong partner
reads as not synced. Nothing of the game's is copied into the
archive except where an option below says so, and the archive is written where you
point it — an instance's gamedir, never the repository (.gitignore refuses .ufo).

    tools/unittypes_fixture.py OUT.ufo --types 600
    tools/unittypes_fixture.py GAMEDIR/zzsyn.ufo --types 300 --canbuild 40 --download 12 --ai \
        --loose GAMEDIR

A .ufo cannot override a path a stock archive already has, and a loose file can
(research/notes/file-formats.md §5), so the two files --canbuild and --ai change are written
loose under --loose, never into the archive, and never through a symlink: an instance's
gamedir links into the install.

    --types N      N synthetic types. Stock has 278, so 234 reach ID 512, stock's break,
                   and 16 105 the build's ceiling of 16 383.
    --canbuild K   the Arm Commander's [CANBUILD] list gains the first K synthetic types:
                   <loose>/gamedata/sidedata.tdf is the install's own, read at run time, with
                   K `canbuild` keys added (the build list's growth past 30 entries).
    --download K   download/synmenu.tdf, written here: K menu entries for the builder
                   naming the last K synthetic types, six to a page from page 3 (a download
                   file past five entries, and the build-list appender).
    --builder U    the download entries' builder, ARMCOM unless given; ARMLAB, the Kbot
                   Lab, is the one that builds these kbots.
    --ai           every AI profile of the install (ai/*.txt, read at run time; a map names
                   its own, `AIPROFILE` in its .ota) written under <loose>/ai/ with a `Weight`
                   line naming the highest synthetic type added to every plan (the AI's
                   stack masks).
    --loose DIR    where the two above go; required with either.
    --ctrl L       the synthetic types carry the category CTRL_L, so Ctrl+L selects exactly
                   them. Ctrl+B and Ctrl+E..Y but S select by that category (the key table at
                   0x496694); stock units carry B, C, F, P, R, V and W, so the default is G.
    --pad KB       every script carries KB kilobytes of zeros after its code, which the engine
                   loads with it (a COB is read whole, at its file size) and never runs; the
                   archive is then zlib-compressed, so a mod too big for the address space is a
                   small file (the out-of-memory message's test).
    --raw-keys     every FBI ends in the same tag, so the unit sync's keys fall as the checksum
                   puts them and thousands collide (sync_state below): the engine fix's test,
                   which re-keys them at load.
    --part K/N     only every Nth type from the Kth, so one mod splits over N archives whose
                   types interleave; the keys are those of the whole mod. Two peers holding the
                   parts under swapped archive names load the types in a different order, which
                   the unit sync's re-key must not depend on. Types only: not with --canbuild,
                   --download, --ai or --pad.

The tests themselves are research/notes/tadr-port/content-ids.md's, "A′2".
"""

import argparse
import itertools
import os
import re
import subprocess
import sys
import tempfile
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
hpipack = SourceFileLoader("hpipack", str(TOOLS / "hpipack.py")).load_module()

# a kbot that does nothing of its own: the engine moves it, and it leaves no corpse
SCRIPT = """piece {root};

Create()
{{
}}

Killed(severity, corpsetype)
{{
\tcorpsetype = 3;
\treturn (0);
}}
"""

# The Copyright line is a key the engine requires, not a credit: the menu-time loader compares it
# with its own template (0x42B0E2..0x42B162, the year masked) and clears a def flag the game
# load needs (0x42D224), so a type without it is never loaded.
FBI = """[UNITINFO]
\t{{
\tUnitName={name};
\tVersion=1;
\tSide=ARM;
\tObjectname={model};
\tDesignation={name};
\tName=Synthetic {n};
\tDescription=Test type {n};
\tFootprintX=2;
\tFootprintZ=2;
\tBuildCostEnergy=100;
\tBuildCostMetal=10;
\tBuildTime=200;
\tMaxDamage=300;
\tMaxWaterDepth=12;
\tMaxSlope=14;
\tBMcode=1;
\tBuilder=0;
\tThreeD=1;
\tZBuffer=1;
\tSightDistance=200;
\tCategory=ARM KBOT LEVEL1 NOTAIR NOTSUB SYNTH CTRL_{ctrl};
\tTEDClass=KBOT;
\tCopyright=Copyright 1997 Humongous Entertainment. All rights reserved.;
\tUnitNumber={number};
\tcanmove=1;
\tcanpatrol=1;
\tcanstop=1;
\tcanguard=1;
\tmobilestandorders=1;
\tStandingMoveOrder=1;
\tMaxVelocity=1.2;
\tBrakeRate=0.2;
\tAcceleration=0.1;
\tTurnRate=800;
\tSteeringMode=2;
\tMovementClass=KBOT2;
\tUpright=1;
\tDefaultMissionType=Standby;
\tmaneuverleashlength=640;
"""
FBI_END = "\tSyncTag={tag};\n\t}}\n"

MENU = """[MENUENTRY{k}]
\t{{
\tUNITMENU={builder};
\tMENU={menu};
\tBUTTON={button};
\tUNITNAME={name};
\t}}

"""


def gamedir():
    env = os.environ.get("TA_GAMEDIR")
    cands = [Path(env)] if env else [ROOT / "tagpu" / "gamedir",
                                     Path.home() / ".local/share/Steam/steamapps/common/Total Annihilation"]
    for c in cands:
        if (c / "rev31.gp3").exists():
            return c
    sys.exit("unittypes_fixture: no rev31.gp3 found; set TA_GAMEDIR")


ARCHIVES = ("rev31.gp3", "ccdata.ccx", "btdata.ccx", "totala1.hpi")


def stock_text(name):
    """a text file of the install, from the first archive that has it"""
    for arc in ARCHIVES:
        a = hpipack.Archive(gamedir() / arc)
        if name in a.files:
            return a.read(name).decode("latin-1")
    sys.exit(f"unittypes_fixture: {name} is in no archive")


def stock_names(prefix, suffix):
    names = set()
    for arc in ARCHIVES:
        names |= {n for n in hpipack.Archive(gamedir() / arc).files
                  if n.startswith(prefix) and n.endswith(suffix)}
    return sorted(names)


def syn(i):
    return "SYN%05d" % i


def sync_state(data, state=(0, 0, 0, 0), start=0):
    """The network unit sync keys every type on this checksum of its FBI file (0x4B6BA0, kept at
    def+0x13E), not on a CRC: four 8-bit lanes, the bytes' sum and xor, and the sum of i^b and
    xor of i+b over the offset's low byte. Two types with one key are one entry in the sync's
    list, which then never reaches the type count, so the join never ends (the battle room
    shows SYNCHING for good). FBIs that differ only in their digits collide in their thousands,
    so every synthetic FBI ends in a tag chosen to make its key unique."""
    s, x, s2, x2 = state
    for i, c in enumerate(data, start):
        s = (s + c) & 255
        x ^= c
        s2 = (s2 + ((i & 255) ^ c)) & 255
        x2 ^= (i + c) & 255
    return s, x, s2, x2


def sync_key(state):
    s, x, s2, x2 = state
    return (x2 << 24) | (s2 << 16) | (x << 8) | s


def stock_sync_keys():
    """the keys of every FBI in every archive of the install, whichever copy the engine loads"""
    keys = set()
    for arc in sorted(gamedir().iterdir()):
        if arc.suffix.lower() not in (".hpi", ".ufo", ".ccx", ".gp3"):
            continue
        a = hpipack.Archive(arc)
        for n in a.files:
            if n.startswith("units/") and n.endswith(".fbi"):
                keys.add(sync_key(sync_state(a.read(n))))
    return keys


def unit_fbi(i, a, used):
    head = FBI.format(name=syn(i), model=a.model, n=i, number=20000 + i,
                      ctrl=a.ctrl.upper()).encode("latin-1")
    if a.raw_keys:
        return head + FBI_END.format(tag=0).encode("latin-1")
    state = sync_state(head)
    for tag in itertools.count():
        end = FBI_END.format(tag=tag).encode("latin-1")
        key = sync_key(sync_state(end, state, len(head)))
        if key not in used:
            used.add(key)
            return head + end


def write_loose(base, rel, text):
    """rel under base, refusing any existing symlink on the way: the gamedir mirrors the install"""
    path = base
    for part in Path(rel).parts:
        path = path / part
        if path.is_symlink():
            sys.exit(f"unittypes_fixture: {path} is a symlink; not writing through it")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("latin-1"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--types", type=int, required=True)
    ap.add_argument("--model", default="armpw")
    ap.add_argument("--root", default="ground", help="the model's root piece, which the script names")
    ap.add_argument("--canbuild", type=int, default=0)
    ap.add_argument("--download", type=int, default=0)
    ap.add_argument("--builder", default="ARMCOM")
    ap.add_argument("--ai", action="store_true")
    ap.add_argument("--ctrl", default="G")
    ap.add_argument("--loose", type=Path)
    ap.add_argument("--pad", type=int, default=0, metavar="KB")
    ap.add_argument("--raw-keys", action="store_true")
    ap.add_argument("--part", default="1/1", metavar="K/N")
    a = ap.parse_args()
    if (a.canbuild or a.ai) and not a.loose:
        sys.exit("unittypes_fixture: --canbuild and --ai write loose files: pass --loose GAMEDIR")
    if a.types < 1 or a.canbuild > a.types or a.download > a.types:
        sys.exit("unittypes_fixture: --canbuild and --download take from the --types made")
    part, parts = (int(x) for x in a.part.split("/"))
    if not 1 <= part <= parts:
        sys.exit("unittypes_fixture: --part is K/N with 1 <= K <= N")
    if parts > 1 and (a.canbuild or a.download or a.ai or a.pad):
        sys.exit("unittypes_fixture: --part writes types only")

    with tempfile.TemporaryDirectory() as tmp:
        bos, cob = Path(tmp) / "syn.bos", Path(tmp) / "syn.cob"
        bos.write_text(SCRIPT.format(root=a.root))
        subprocess.run([str(TOOLS / "tacob"), "compile", str(bos), "-o", str(cob)],
                       check=True, stdout=subprocess.DEVNULL)
        script = cob.read_bytes() + bytes(a.pad * 1024)

    tree = {}
    used = set() if a.raw_keys else stock_sync_keys()
    made = 0
    for i in range(1, a.types + 1):
        fbi = unit_fbi(i, a, used)               # every part's keys, so the parts agree
        if (i - part) % parts:
            continue
        name = syn(i)
        made += 1
        hpipack.insert(tree, f"units/{name.lower()}.fbi", fbi)
        hpipack.insert(tree, f"scripts/{name.lower()}.cob", script + i.to_bytes(4, "little"))

    if a.canbuild:
        side = stock_text("gamedata/sidedata.tdf")
        m = re.search(r"\[ARMCOM\]\s*\{(.*?)\}", side, re.S)
        have = [int(k) for k in re.findall(r"canbuild(\d+)\s*=", m.group(1), re.I)]
        extra = "".join(f"\t\tcanbuild{max(have) + k}={syn(k)};\n" for k in range(1, a.canbuild + 1))
        side = side[:m.end(1)] + extra + "\t\t" + side[m.end(1):]
        write_loose(a.loose, "gamedata/sidedata.tdf", side)

    if a.download:
        first = a.types - a.download + 1
        text = "".join(MENU.format(k=k + 1, menu=3 + k // 6, button=k % 6, name=syn(first + k),
                                   builder=a.builder.upper())
                       for k in range(a.download))
        hpipack.insert(tree, "download/synmenu.tdf", text.encode("latin-1"))

    if a.ai:
        for name in stock_names("ai/", ".txt"):
            ai = re.sub(r"^(plan\s+\S+.*)$", lambda m: m.group(1) + f"\r\n\r\nWeight {syn(a.types)} 5",
                        stock_text(name), flags=re.M | re.I)
            write_loose(a.loose, name, ai)

    data = hpipack.build(tree, method=2 if a.pad else 1)
    a.out.write_bytes(data)
    print(f"{a.out}: {made} types (part {part}/{parts} of {syn(1)}..{syn(a.types)}), {len(data)} bytes")


if __name__ == "__main__":
    main()
