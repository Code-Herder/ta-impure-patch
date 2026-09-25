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
    sys.exit("datakeys_fixture: no totala1.hpi found; set TA_GAMEDIR")


def unit(fbi, name, keys):
    """the stock FBI renamed, with `keys` added at the end of its [UNITINFO] section"""
    out, n = re.subn(r"UnitName\s*=[^;]*;", f"UnitName={name};", fbi, count=1, flags=re.I)
    out, m = re.subn(r"\}\s*$", "".join(f"\t{k}={v};\r\n" for k, v in keys.items()) + "\t}\r\n",
                     out.rstrip(), count=1)
    if n != 1 or m != 1:
        sys.exit(f"datakeys_fixture: the stock FBI for {name} has no UnitName or closing brace")
    return out.encode("latin-1")


def menu(builder, page, button, name):
    return (f"[MENUENTRY1]\r\n\t{{\r\n\tUNITMENU={builder};\r\n\tMENU={page};\r\n"
            f"\tBUTTON={button};\r\n\tUNITNAME={name};\r\n\t}}\r\n").encode("latin-1")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", type=Path)
    a = ap.parse_args()

    arc = hpipack.Archive(gamedir() / "totala1.hpi")
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

    data = hpipack.build(tree)
    a.out.write_bytes(data)
    print(f"{a.out}: {len(clones)} units, {len(data)} bytes")


if __name__ == "__main__":
    main()
