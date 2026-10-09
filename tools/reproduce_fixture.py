#!/usr/bin/env python3
"""reproduce_fixture — write a test archive whose trees and shrubs reproduce, for the demo
recorder's feature-reproduction tests (research/notes/tadr-port/demo-recorder-multiplayer-slice.md).

No stock feature reproduces (every stock definition has reproduce=0), so the engine's reproduction
path -- the cursor's source test at 0x424189 and the spawn at 0x4241A3 -- runs only under a mod
that sets it. This archive is that mod, for a test: the stock sections of Tree1..Tree6 and
Shrub1..Shrub3, read from the install at run time, with reproduce=100 (every visit of the cursor
draws past the chance) and reproducearea=3 (a 3x3 window, two draws per spawn). Written where
you point it -- an instance's gamedir, never the repository (.gitignore refuses .ufo):

    tools/reproduce_fixture.py tagpu/instances/x/gamedir/zzrepro.ufo

A feature is defined by the first file enumerated that holds its section, and every .ufo is
enumerated before every .hpi (exe-reverse-engineering.md, LoadFeature 0x4224B0), so a new path
here takes those names over from totala1.hpi. Every peer of a game and the puppet host playing
its recording need the same archive.
"""
import argparse
import re
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
hpipack = SourceFileLoader("hpipack", str(ROOT / "tools" / "hpipack.py")).load_module()
datakeys = SourceFileLoader("datakeys_fixture", str(ROOT / "tools" / "datakeys_fixture.py")).load_module()

NAMES = {"features/green/trees.tdf": [f"Tree{i}" for i in range(1, 7)],
         "features/green/shrubs.tdf": [f"Shrub{i}" for i in range(1, 4)]}
KEYS = {"reproduce": 100, "reproducearea": 3}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", type=Path)
    a = ap.parse_args()
    ta = hpipack.Archive(datakeys.gamedir() / "totala1.hpi")
    tdf = []
    for path, names in NAMES.items():
        text = ta.read(path).decode("latin-1")
        for name in names:
            body = re.sub(r"\s*(reproduce|reproducearea)\s*=[^;]*;", "",
                          datakeys.section(text, name), flags=re.I)
            tdf += [f"[{name}]", "\t{" + body.rstrip()]
            tdf += [f"\t{k}={v};" for k, v in KEYS.items()] + ["\t}"]
    tree = {}
    hpipack.insert(tree, "features/zzrepro/reproduce.tdf", "\r\n".join(tdf).encode("latin-1"))
    data = hpipack.build(tree)
    a.out.write_bytes(data)
    print(f"{a.out}: {sum(map(len, NAMES.values()))} features reproduce, {len(data)} bytes")


if __name__ == "__main__":
    sys.exit(main())
