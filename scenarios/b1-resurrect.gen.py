#!/usr/bin/env python3
"""Writes scenarios/b1-resurrect.json: B1's side measurement of Order_Resurrect's exits.

`python3 scenarios/b1-resurrect.gen.py GAMEDIR [variant]` -- GAMEDIR is a stock game folder (a
tacli instance's), read only; no variant writes the committed fixture; a variant (b, c, d, ...)
shuffles the wreck types and shifts the grid, and is written to stdout.

110 wrecks on the Two Continents plateau, each with a CORNECRO beside it ordered to `reclaim`
it, which the order resolver turns into RESURRECT for a unit that can resurrect
(0x43F5A5..0x43F612 -> 0x44004C). Ten of the wrecks get a second Necro, so two orders race for
one wreck. A feature's footprint runs +x/+z from its anchor, the cell at its placement
position, so every Necro stands up and to the left of its wreck, outside the footprint: a Necro
inside it keeps the resurrected structure from being created. The multi-cell wrecks are ordered
three ways: by handle (the order carries the anchor's position), at the footprint's centre, and
at its far corner cell, so a third of those orders name the anchor and the rest a cell that
0x4815F0 has to hop from.

THE FOOTPRINTS AND THE ROCKS COME FROM THE GAME. Each wreck's footprint is its FeatureDef's
`footprintx`/`footprintz`, read from the game's feature TDFs, so the centre and far-corner orders
land inside it whatever its shape (armlab_dead is 5x6, not square). The spawn 0x423C50 clears its
footprint first and abandons the spawn when a cell there holds an indestructible feature
(engine/map.md, the 0x423C50 row); Two Continents has indestructible metal rocks on the plateau
(RockMetal2 at cell 163,86 covers 165,88, the cell a regular grid gives w047). So every wreck
whose footprint would cover a cell of an indestructible map feature moves by whole cells, its
Necros with it, to the nearest clear place within SHIFT_MAX cells; none clear is an error, never a
wreck the applier refuses at run time.
"""
import json
import random
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import hpipack  # noqa: E402

MIX = (["armflea_dead"] * 25 + ["armaser_dead"] * 25 + ["armpw_dead"] * 15
       + ["armllt_dead"] * 10 + ["armmex_dead"] * 10 + ["armwin_dead"] * 5
       + ["armsolar_dead"] * 5 + ["armlab_dead"] * 5
       + ["armflea_dead"] * 5 + ["armpw_dead"] * 5)   # the last ten are shared
SHARED_FROM = len(MIX) - 10
COLS, STEP = 14, 176            # 11 cells a slot: a footprint of 6 shifted by 3 stays in its own
SHIFT_MAX = 3
MAP = "Two Continents"
ARCHIVES = (".hpi", ".ufo", ".ccx", ".gp3")
VARIANTS = {"": (1760, 880, 20260926), "b": (1800, 920, 2), "c": (1720, 960, 3), "d": (1780, 840, 4),
            "e": (1740, 900, 5), "f": (1820, 860, 6), "g": (1700, 940, 7), "h": (1790, 980, 8)}
TDF_BLOCK = re.compile(r"\[([^\]]+)\][ \t]*(?://[^\n]*)?\s*\{([^}]*)\}", re.S)
TDF_KEY = re.compile(r"(\w+)\s*=\s*([^;]*);")


def game(gamedir: Path):
    """(the map's TNT bytes, {feature name: [{key: value}, ...]}) from every archive of the folder,
    each definition of a name kept: which one the engine takes is never asked (fact())."""
    tnt, feats = None, {}
    for path in sorted(p for p in gamedir.iterdir() if p.suffix.lower() in ARCHIVES):
        a = hpipack.Archive(path)
        if f"maps/{MAP.lower()}.tnt" in a.files:
            tnt = a.read(f"maps/{MAP.lower()}.tnt")
        for name in a.files:
            if not (name.startswith("features/") and name.endswith(".tdf")):
                continue
            for m in TDF_BLOCK.finditer(a.read(name).decode("latin-1")):
                kv = {k.lower(): v.strip() for k, v in TDF_KEY.findall(m.group(2))}
                feats.setdefault(m.group(1).strip().lower(), []).append(kv)
    if tnt is None:
        sys.exit(f"{gamedir}: no map named {MAP}")
    return tnt, feats


def fact(feats, name, key, default):
    """One key of a feature, which every archive that defines the feature must agree on, so the
    archives' order cannot matter; a disagreement is an error."""
    seen = {d.get(key, default) for d in feats.get(name.lower(), [{}])}
    if len(seen) != 1:
        sys.exit(f"feature {name}: the archives disagree on {key} ({sorted(seen)})")
    return seen.pop()


def footprint(feats, name):
    return int(fact(feats, name, "footprintx", "1")), int(fact(feats, name, "footprintz", "1"))


def rock_cells(tnt, feats) -> set:
    """Every cell an indestructible map feature's footprint covers. The TNT header as
    tools/resource_fixture.py writes it: width and height in cells at +4/+8, the attribute array
    (height byte, feature word, one more byte per cell) at +16, the feature count at +28 and the
    names (a u32, then 128 bytes) at +32."""
    hdr = struct.unpack_from("<16i", tnt, 0)
    w, h, attr, nfeat, names_at = hdr[1], hdr[2], hdr[4], hdr[7], hdr[8]
    names = [tnt[names_at + i * 132 + 4:names_at + i * 132 + 132].split(b"\0")[0].decode("latin-1")
             for i in range(nfeat)]
    cells = set()
    for z in range(h):
        for x in range(w):
            f = struct.unpack_from("<H", tnt, attr + 4 * (z * w + x) + 1)[0]
            if f >= nfeat or fact(feats, names[f], "indestructible", "0") != "1":
                continue
            fx, fz = footprint(feats, names[f])
            cells |= {(x + i, z + j) for i in range(fx) for j in range(fz)}
    return cells


def clear_anchor(cx, cz, fx, fz, rocks):
    """The anchor cell nearest (cx, cz) whose footprint covers no rock cell."""
    shifts = sorted(((i, j) for i in range(-SHIFT_MAX, SHIFT_MAX + 1)
                     for j in range(-SHIFT_MAX, SHIFT_MAX + 1)),
                    key=lambda s: (abs(s[0]) + abs(s[1]), s[1], s[0]))
    for i, j in shifts:
        if not any((cx + i + a, cz + j + b) in rocks for a in range(fx) for b in range(fz)):
            return cx + i, cz + j
    sys.exit(f"no clear place within {SHIFT_MAX} cells of cell {cx},{cz} for a {fx}x{fz} wreck")


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.split("\n\n")[1])
    gamedir = Path(sys.argv[1])
    variant = sys.argv[2] if len(sys.argv) > 2 else ""
    X0, Y0, seed = VARIANTS[variant]
    tnt, feats = game(gamedir)
    rocks = rock_cells(tnt, feats)
    mix = list(MIX)
    if variant:
        head = mix[:SHARED_FROM]
        random.Random(seed).shuffle(head)
        mix = head + mix[SHARED_FROM:]

    features, units = [], []
    multi = 0
    for k, ftype in enumerate(mix):
        fx, fz = footprint(feats, ftype)
        cx, cz = clear_anchor((X0 + STEP * (k % COLS)) // 16, (Y0 + STEP * (k // COLS)) // 16,
                              fx, fz, rocks)
        x, y = 16 * cx, 16 * cz
        wid = f"w{k:03d}"
        features.append({"id": wid, "type": ftype, "pos": [x, y], "facing": 0})
        if fx * fz > 1 and k < SHARED_FROM:
            way = multi % 3
            multi += 1
            order = ({"cmd": "reclaim", "target": wid} if way == 0 else
                     {"cmd": "reclaim", "to": [x + 8 * fx, y + 8 * fz]} if way == 1 else
                     {"cmd": "reclaim", "to": [x + 16 * fx - 8, y + 16 * fz - 8]})
        else:
            order = {"cmd": "reclaim", "target": wid}
        units.append({"id": f"n{k:03d}", "type": "CORNECRO", "owner": 0, "pos": [x - 40, y - 40],
                      "orders": [order]})
        if k >= SHARED_FROM:
            units.append({"id": f"m{k:03d}", "type": "CORNECRO", "owner": 0,
                          "pos": [x + 16 * fx + 40, y - 40],
                          "orders": [{"cmd": "reclaim", "target": wid}]})
    units.append({"id": "ai_keepalive", "type": "ARMSOLAR", "owner": 1, "pos": [1400, 7300]})

    scn = {
        "format": "ta-scenario/1",
        "description": ("B1 side measurement: 110 wrecks (50 of 1x1, the rest 2x2 to 5x6), each "
                        "with a CORNECRO ordered to reclaim it -- the resolver makes that "
                        "RESURRECT. Ten wrecks have a second Necro; a third of the multi-cell "
                        "orders name the anchor, the others the footprint's centre or far corner. "
                        "A wreck whose footprint would cover an indestructible map feature sits "
                        "beside it. Written by b1-resurrect.gen.py"
                        + (f" (variant {variant})" if variant else "") + "."),
        "seed": seed,
        "on_error": "skip",
        "setup": {
            "map": MAP, "res": "1024x768", "clear_existing": True,
            "switches": {"noshake": True},
            "players": [
                {"slot": 0, "controller": "human", "side": "core", "color": 1,
                 "metal": 20000, "energy": 20000},
                {"slot": 1, "controller": "ai", "side": "arm", "color": 0},
            ],
        },
        "features": features,
        "units": units,
        "camera": {"at": [X0 + STEP * COLS // 2, Y0 + STEP * 4]},
    }
    text = json.dumps(scn, indent=1) + "\n"
    if variant:
        sys.stdout.write(text)
    else:
        Path(__file__).with_name("b1-resurrect.json").write_text(text)


if __name__ == "__main__":
    main()
