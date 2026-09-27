#!/usr/bin/env python3
"""Writes scenarios/b1-resurrect.json: B1's side measurement of Order_Resurrect's exits.

`python3 scenarios/b1-resurrect.gen.py [variant]` — no variant writes the committed fixture;
a variant (b, c, d, ...) shuffles the wreck types and shifts the grid, and is written to stdout.

110 wrecks on the Two Continents plateau, each with a CORNECRO beside it ordered to `reclaim`
it, which the order resolver turns into RESURRECT for a unit that can resurrect
(0x43F5A5..0x43F612 -> 0x44004C). Ten of the wrecks get a second Necro, so two orders race for
one wreck. A feature's footprint runs +x/+z from its anchor, the cell at its placement
position, so every Necro stands up and to the left of its wreck, outside the footprint: a Necro
inside it keeps the resurrected structure from being created. The multi-cell wrecks are ordered
three ways: by handle (the order carries the anchor's position), at the footprint's centre, and
at its far corner cell, so a third of those orders name the anchor and the rest a cell that
0x4815F0 has to hop from.
"""
import json
import random
import sys
from pathlib import Path

MIX = ([("armflea_dead", 1)] * 25 + [("armaser_dead", 1)] * 25 + [("armpw_dead", 2)] * 15
       + [("armllt_dead", 2)] * 10 + [("armmex_dead", 3)] * 10 + [("armwin_dead", 4)] * 5
       + [("armsolar_dead", 5)] * 5 + [("armlab_dead", 6)] * 5
       + [("armflea_dead", 1)] * 5 + [("armpw_dead", 2)] * 5)   # the last ten are shared
SHARED_FROM = len(MIX) - 10
COLS, STEP = 14, 176
VARIANTS = {"": (1760, 880, 20260926), "b": (1800, 920, 2), "c": (1720, 960, 3), "d": (1780, 840, 4),
            "e": (1740, 900, 5), "f": (1820, 860, 6), "g": (1700, 940, 7), "h": (1790, 980, 8)}

variant = sys.argv[1] if len(sys.argv) > 1 else ""
X0, Y0, seed = VARIANTS[variant]
mix = list(MIX)
if variant:
    head = mix[:SHARED_FROM]
    random.Random(seed).shuffle(head)
    mix = head + mix[SHARED_FROM:]

features, units = [], []
multi = 0
for k, (ftype, foot) in enumerate(mix):
    x, y = X0 + STEP * (k % COLS), Y0 + STEP * (k // COLS)
    wid = f"w{k:03d}"
    features.append({"id": wid, "type": ftype, "pos": [x, y], "facing": 0})
    if foot > 1 and k < SHARED_FROM:
        way = multi % 3
        multi += 1
        order = ({"cmd": "reclaim", "target": wid} if way == 0 else
                 {"cmd": "reclaim", "to": [x + 8 * foot, y + 8 * foot]} if way == 1 else
                 {"cmd": "reclaim", "to": [x + 16 * foot - 8, y + 16 * foot - 8]})
    else:
        order = {"cmd": "reclaim", "target": wid}
    units.append({"id": f"n{k:03d}", "type": "CORNECRO", "owner": 0, "pos": [x - 40, y - 40],
                  "orders": [order]})
    if k >= SHARED_FROM:
        units.append({"id": f"m{k:03d}", "type": "CORNECRO", "owner": 0,
                      "pos": [x + 16 * foot + 40, y - 40],
                      "orders": [{"cmd": "reclaim", "target": wid}]})
units.append({"id": "ai_keepalive", "type": "ARMSOLAR", "owner": 1, "pos": [1400, 7300]})

scn = {
    "format": "ta-scenario/1",
    "description": ("B1 side measurement: 110 wrecks (50 of 1x1, the rest 2x2 to 5x6), each with a "
                    "CORNECRO ordered to reclaim it -- the resolver makes that RESURRECT. Ten wrecks "
                    "have a second Necro; a third of the multi-cell orders name the anchor, the "
                    "others the footprint's centre or far corner. Written by b1-resurrect.gen.py"
                    + (f" (variant {variant})" if variant else "") + "."),
    "seed": seed,
    "on_error": "skip",
    "setup": {
        "map": "Two Continents", "res": "1024x768", "clear_existing": True,
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
