#!/usr/bin/env python3
"""stagemap.py -- writes and checks art/wiki/stage/<entry>.json, the in-game wiki stage's ground, as references.

    PY=.venv-undither/bin/python                       # the main checkout's venv
    $PY tools/wiki/stagemap.py tiles "maps\\Crossing Two Rivers.tnt"   # candidate ground tiles + a contact sheet
    $PY tools/wiki/stagemap.py build                   # every entry's description, from RECIPES below
    $PY tools/wiki/stagemap.py check                   # exit 0 when every description resolves and fits its scene

Every command reads the local install (--gamedir, else TA3DO_GAMEDIR, else Steam's default library) through
tools/wiki/tavfs.py, which finds each file where the running engine would: loose first, then the first archive in
the engine's mount order.

**What a description holds is our own**: which tile of which source map goes in each 32-unit square, a FNV-1a 32
hash of that tile's 1024 bytes and the source's tile count (identity checks: a map of the same name with other
tiles is refused, never drawn as a patchwork), and each feature by its TDF section name at its anchor cell (the
footprint's top-left cell, where the TNT and LoadFeature 0x4224B0 put it). No pixel, palette, TNT or GAF byte is
written, and nothing derived from one but the hash: CLAUDE.md, *Nothing from the original game, ever*. The format
and the fallback rule are research/notes/ingame-wiki-3d-stage-plan.md section 4; the data format section of
research/notes/ingame-wiki.md states the file.

`build` derives the description from the entry's scene and its recipe: the ground tile everywhere, the metal-ground
tile over each broad metal rect (one that touches both opposite map edges on an axis, which sim.js `distinct`
never takes for a deposit), a deposit feature centred on each deposit (the bounding box of the other metal rects,
joined eight-connected), and a vent feature with its anchor on each vent cell. `check` resolves every description
against the install and against its entry's scene, and compares it with what `build` writes now.

The contact sheet `tiles` draws goes to tools/wiki/mockup/gfx/, which is gitignored: it is the game's pixels.
"""

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ENTRIES = REPO / "art" / "wiki" / "entries"
STAGE = REPO / "art" / "wiki" / "stage"
SHEETS = HERE / "mockup" / "gfx"
sys.path.insert(0, str(HERE))
import tavfs  # noqa: E402

FORMAT = 1
CELL = 16          # world units a cell
TILE_CELLS = 2     # a TNT tile is 32 world units, two cells a side
TILE_BYTES = 1024

LUSH = "maps\\crossing two rivers.tnt"
METAL_WORLD = "maps\\metal heck.tnt"

# Per entry: the ground and metal-ground tiles (source map, tile index) and the deposit and vent features. Each tile
# is its map's most used tileable tile under flat, featureless ground, the first `tiles` lists. Metal Heck's stands
# for metal ground because the metal worlds' most used flat tiles are the dark void between their plates.
RECIPES = {
    "quickbuild": {"ground": (LUSH, 13), "metalground": (METAL_WORLD, 170),
                   "deposit": "lushmetal3", "vent": "lushvent01"},
    "buildshapes": {"ground": (LUSH, 13)},
}


def fnv1a32(data: bytes) -> str:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f"{h:08x}"


class Fail(Exception):
    pass


# --------------------------------------------------------------------------- the install

class Maps:
    """Each source TNT opened once by the engine's rule; the failure that kept one out, by name."""

    def __init__(self, vfs):
        self.vfs = vfs
        self.got = {}

    def get(self, path):
        if path not in self.got:
            hit = self.vfs.open(path)
            if hit is None:
                self.got[path] = (None, None, f"{path}: no such file in the install")
            else:
                blob, where = hit
                try:
                    self.got[path] = (tavfs.tascene.Tnt(blob, path), where, None)
                except ValueError as e:
                    self.got[path] = (None, where, f"{path} ({where}): not a TNT the readers take: {e}")
        return self.got[path]

    def tile(self, path, index):
        tnt, _where, err = self.get(path)
        if err:
            raise Fail(err)
        if not 0 <= index < tnt.tiles:
            raise Fail(f"{path}: tile {index} past its {tnt.tiles} tiles")
        o = tnt.p_tilegfx + index * TILE_BYTES
        return tnt.blob[o:o + TILE_BYTES]


def footprint(vfs, name):
    hit = vfs.feature(name)
    if hit is None:
        raise Fail(f"feature {name}: no section in any features\\*.tdf")
    body = hit[0]
    return int(float(body.get("footprintx", 1) or 1)), int(float(body.get("footprintz", 1) or 1))


# --------------------------------------------------------------------------- the scene

def broad(rect, w, h):
    """A rect that touches both opposite map edges on an axis: sim.js `distinct` never takes it for a deposit."""
    x, z, rw, rh = rect
    return (x <= 0 and x + rw >= w) or (z <= 0 and z + rh >= h)


def deposits(scene):
    """The bounding boxes of the non-broad metal rects, joined when their cells touch eight-connected."""
    w, h = scene["map"]["w"], scene["map"]["h"]
    rects = [m["rect"] for m in scene.get("metal", []) if m["v"] > scene["map"].get("surfaceMetal", 0)
             and not broad(m["rect"], w, h)]
    groups = [[r] for r in rects]
    touch = lambda a, b: (a[0] <= b[0] + b[2] and b[0] <= a[0] + a[2] and a[1] <= b[1] + b[3] and b[1] <= a[1] + a[3])
    merged = True
    while merged:
        merged = False
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                if any(touch(a, b) for a in groups[i] for b in groups[j]):
                    groups[i] += groups.pop(j)
                    merged = True
                    break
            if merged:
                break
    boxes = []
    for g in groups:
        x0, z0 = min(r[0] for r in g), min(r[1] for r in g)
        x1, z1 = max(r[0] + r[2] for r in g), max(r[1] + r[3] for r in g)
        boxes.append((x0, z0, x1 - x0, z1 - z0, g))
    return boxes


def vent_cells(scene):
    return [[int(x // CELL), int(z // CELL)] for x, z in scene.get("vents", [])]


def tile_aligned(rect):
    return all(v % TILE_CELLS == 0 for v in rect)


# --------------------------------------------------------------------------- build

def build_one(vfs, maps, entry):
    eid = entry["id"]
    recipe = RECIPES.get(eid)
    if recipe is None:
        raise Fail(f"{eid}: no recipe in stagemap.py RECIPES")
    scene = entry["scene"]
    w, h = scene["map"]["w"], scene["map"]["h"]
    if w % TILE_CELLS or h % TILE_CELLS:
        raise Fail(f"{eid}: the map's {w} x {h} cells are not whole tiles")
    tw, th = w // TILE_CELLS, h // TILE_CELLS
    sources, tileset = [], []

    def row(ref):
        path, index = ref
        if path not in sources:
            sources.append(path)
        key = [sources.index(path), index, fnv1a32(maps.tile(path, index))]
        if key not in tileset:
            tileset.append(key)
        return tileset.index(key)

    surface = scene["map"].get("surfaceMetal", 0)
    base = row(recipe["metalground"] if surface > 0 and "metalground" in recipe else recipe["ground"])
    tilemap = [base] * (tw * th)
    for m in scene.get("metal", []):
        if not broad(m["rect"], w, h):
            continue
        if "metalground" not in recipe:
            raise Fail(f"{eid}: broad metal {m['rect']} and no metalground tile in its recipe")
        mg = row(recipe["metalground"])
        x, z, rw, rh = m["rect"]
        for j in range(z // TILE_CELLS, (z + rh) // TILE_CELLS):
            for i in range(x // TILE_CELLS, (x + rw) // TILE_CELLS):
                tilemap[j * tw + i] = mg

    features = []
    boxes = deposits(scene)
    if boxes:
        name = recipe.get("deposit")
        if not name:
            raise Fail(f"{eid}: deposits and no deposit feature in its recipe")
        fx, fz = footprint(vfs, name)
        for x, z, bw, bh, _g in boxes:
            features.append({"name": name, "cell": [x + (bw - fx) // 2, z + (bh - fz) // 2]})
    vents = vent_cells(scene)
    if vents:
        name = recipe.get("vent")
        if not name:
            raise Fail(f"{eid}: vents and no vent feature in its recipe")
        for c in vents:
            features.append({"name": name, "cell": c})

    counts = []
    for path in sources:
        tnt, _where, err = maps.get(path)
        if err:
            raise Fail(err)
        counts.append({"map": path, "tiles": tnt.tiles})
    return {"format": FORMAT, "entry": eid, "cells": [w, h], "sources": counts,
            "tileset": tileset, "tilemap": tilemap, "features": features}


def dumps(desc):
    """One key a line; a tileset row, a tilemap row of tiles and a feature a line each."""
    tw = desc["cells"][0] // TILE_CELLS
    j = lambda v: json.dumps(v, separators=(", ", ": "))
    tm = desc["tilemap"]
    rows = [", ".join(str(v) for v in tm[k:k + tw]) for k in range(0, len(tm), tw)]
    lines = ["{", f'  "format": {desc["format"]},', f'  "entry": {j(desc["entry"])},',
             f'  "cells": {j(desc["cells"])},']
    lines.append('  "sources": [' + ",".join(f"\n    {j(s)}" for s in desc["sources"]) + "\n  ],")
    lines.append('  "tileset": [' + ",".join(f"\n    {j(t)}" for t in desc["tileset"]) + "\n  ],")
    lines.append('  "tilemap": [' + ",".join(f"\n    {r}" for r in rows) + "\n  ],")
    feats = ",".join(f"\n    {j(f)}" for f in desc["features"])
    lines.append('  "features": [' + (feats + "\n  " if feats else "") + "]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def load_entries():
    return [json.loads(p.read_text()) for p in sorted(ENTRIES.glob("*.json"))]


def cmd_build(vfs, args):
    maps = Maps(vfs)
    STAGE.mkdir(parents=True, exist_ok=True)
    bad = 0
    for entry in load_entries():
        try:
            desc = build_one(vfs, maps, entry)
        except Fail as e:
            print(f"FAIL {e}")
            bad += 1
            continue
        path = STAGE / f"{entry['id']}.json"
        path.write_text(dumps(desc))
        print(f"wrote {path.relative_to(REPO)}: {len(desc['tileset'])} tiles from "
              f"{len(desc['sources'])} maps, {len(desc['features'])} features")
    return 1 if bad else 0


# --------------------------------------------------------------------------- check

def check_one(vfs, maps, entry, fails):
    eid = entry["id"]
    say = lambda msg: fails.append(f"{eid}: {msg}")
    path = STAGE / f"{eid}.json"
    if not path.is_file():
        return say(f"no {path.relative_to(REPO)} (stagemap.py build)")
    try:
        desc = json.loads(path.read_text())
    except ValueError as e:
        return say(f"{path.relative_to(REPO)}: {e}")
    scene = entry["scene"]
    w, h = scene["map"]["w"], scene["map"]["h"]
    if desc.get("format") != FORMAT or desc.get("entry") != eid:
        say(f"format {desc.get('format')} entry {desc.get('entry')!r}, want {FORMAT} {eid!r}")
    if desc.get("cells") != [w, h]:
        say(f"cells {desc.get('cells')}, the scene's map is {[w, h]}")
    if w % TILE_CELLS or h % TILE_CELLS:
        say(f"the map's {w} x {h} cells are not whole tiles")

    # every rect the ground draws in tiles or the fog grid draws in blocks is whole tiles; a demo can add none
    for m in scene.get("metal", []):
        if broad(m["rect"], w, h) and not tile_aligned(m["rect"]):
            say(f"metal ground {m['rect']} is not whole tiles (even cells)")
    for f in scene.get("fog", []):
        if not tile_aligned(f["rect"]):
            say(f"{f['kind']} fog {f['rect']} is not whole tiles (even cells)")
    for d in entry.get("demos", []):
        add = d.get("scene") or {}
        for k in ("metal", "vents", "fog"):
            if add.get(k):
                say(f"demo {d['id']} adds {k}, which the stage's ground does not show")

    # every source and tile resolves, with its count and hash
    sources = desc.get("sources", [])
    for s in sources:
        tnt, where, err = maps.get(s.get("map", ""))
        if err:
            say(err)
        elif tnt.tiles != s.get("tiles"):
            say(f"{s['map']} ({where}): {tnt.tiles} tiles, the description says {s.get('tiles')}")
    tileset = desc.get("tileset", [])
    for k, t in enumerate(tileset):
        if not (isinstance(t, list) and len(t) == 3 and isinstance(t[0], int) and 0 <= t[0] < len(sources)
                and isinstance(t[1], int)):
            say(f"tileset row {k} {t}: not [source, tile, hash]")
            continue
        try:
            got = fnv1a32(maps.tile(sources[t[0]]["map"], t[1]))
        except Fail as e:
            say(f"tileset row {k}: {e}")
            continue
        if got != t[2]:
            say(f"tileset row {k}: {sources[t[0]]['map']} tile {t[1]} hashes {got}, the description says {t[2]}")
    tm = desc.get("tilemap", [])
    if len(tm) != (w // TILE_CELLS) * (h // TILE_CELLS):
        say(f"tilemap holds {len(tm)} tiles, the map {(w // TILE_CELLS) * (h // TILE_CELLS)}")
    if any(not isinstance(v, int) or not 0 <= v < len(tileset) for v in tm):
        say("a tilemap value is not a tileset row")

    # every feature resolves and sits inside the map; deposits and vents carry their rules' pictures
    rects = [m["rect"] for m in scene.get("metal", []) if not broad(m["rect"], w, h)]
    vents = vent_cells(scene)
    deps, vfeats = [], []
    for f in desc.get("features", []):
        name, cell = f.get("name", ""), f.get("cell")
        hit = vfs.feature(name)
        if hit is None:
            say(f"feature {name}: no section in any features\\*.tdf")
            continue
        body = hit[0]
        fx, fz = int(float(body.get("footprintx", 1) or 1)), int(float(body.get("footprintz", 1) or 1))
        if not (isinstance(cell, list) and len(cell) == 2 and 0 <= cell[0] and cell[0] + fx <= w
                and 0 <= cell[1] and cell[1] + fz <= h):
            say(f"feature {name} at {cell}: its {fx} x {fz} footprint leaves the map")
            continue
        if float(body.get("metal", 0) or 0) > 0:
            foot = [cell[0], cell[1], fx, fz]
            if not any(r[0] <= foot[0] and foot[0] + fx <= r[0] + r[2] and r[1] <= foot[1]
                       and foot[1] + fz <= r[1] + r[3] for r in rects):
                say(f"deposit feature {name} at {cell}: its footprint is over no metal rect of the scene")
            deps.append(foot)
        if str(body.get("geothermal", "0")).strip() == "1":
            if cell not in vents:
                say(f"vent feature {name} at {cell}: on no vent cell of the scene")
            vfeats.append(cell)
    for x, z, bw, bh, _g in deposits(scene):
        if not any(x <= d[0] and d[0] + d[2] <= x + bw and z <= d[1] and d[1] + d[3] <= z + bh for d in deps):
            say(f"the deposit at {[x, z, bw, bh]} has no deposit feature")
    for c in vents:
        if c not in vfeats:
            say(f"the vent at {c} has no vent feature")

    # and the file is what build writes from the scene and the recipe today
    try:
        want = dumps(build_one(vfs, maps, entry))
    except Fail as e:
        return say(f"build: {e}")
    if path.read_text() != want:
        say(f"{path.relative_to(REPO)} is not what stagemap.py build writes now")


def cmd_check(vfs, args):
    maps = Maps(vfs)
    fails = []
    entries = load_entries()
    for entry in entries:
        check_one(vfs, maps, entry, fails)
    known = {e["id"] for e in entries}
    for p in sorted(STAGE.glob("*.json")):
        if p.stem not in known:
            fails.append(f"{p.relative_to(REPO)}: no entry {p.stem}")
    for path, (tnt, where, err) in sorted(maps.got.items()):
        if not err:
            print(f"  {path}: {where}, {tnt.tiles} tiles")
    names = sorted({f.get("name", "") for p in STAGE.glob("*.json")
                    for f in json.loads(p.read_text()).get("features", [])})
    for name in names:
        hit = vfs.feature(name)
        if hit:
            print(f"  {name}: {hit[1]} ({hit[2]})")
    for f in fails:
        print(f"FAIL {f}")
    if fails:
        return 1
    print(f"stagemap: {len(entries)} descriptions resolve and fit their scenes")
    return 0


# --------------------------------------------------------------------------- tiles

def cmd_tiles(vfs, args):
    maps = Maps(vfs)
    tnt, where, err = maps.get(args.map)
    if err:
        print(f"FAIL {err}")
        return 1
    hit = vfs.open(tavfs.tascene.ta3do.PALETTE_PATH)
    if hit is None:
        print(f"FAIL {tavfs.tascene.ta3do.PALETTE_PATH}: no such file in the install")
        return 1
    raw = hit[0]
    palette = [(raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2]) for i in range(256)]
    attr = tnt.blob[tnt.p_mapattr:tnt.p_mapattr + tnt.w16 * tnt.h16 * 4]
    tiles = tnt.tilemap()
    use = Counter()
    for r in range(tnt.mh):
        for c in range(tnt.mw):
            cells = [((2 * r + j) * tnt.w16 + 2 * c + i) * 4 for j in (0, 1) for i in (0, 1)]
            if len({attr[o] for o in cells}) != 1:
                continue
            if any((attr[o + 1] | attr[o + 2] << 8) != 0xFFFF for o in cells):
                continue
            use[tiles[2 * (r * tnt.mw + c)] | tiles[2 * (r * tnt.mw + c) + 1] << 8] += 1
    picks = []
    for t, n in use.most_common():
        if t >= tnt.tiles:
            continue
        px = tnt.blob[tnt.p_tilegfx + t * TILE_BYTES:tnt.p_tilegfx + (t + 1) * TILE_BYTES]
        if tavfs.tascene._tileable(px, 32, 32, -1, palette):
            picks.append((t, n, px))
        if len(picks) >= args.count:
            break
    print(f"{args.map} ({where}): {tnt.tiles} tiles, {tnt.mw} x {tnt.mh} squares; "
          f"flat, featureless and tileable, most used first:")
    for t, n, px in picks:
        print(f"  tile {t:5}  used {n:5}  hash {fnv1a32(px)}")
    if picks:
        from PIL import Image, ImageDraw
        k, cols = 3, 8
        cw, chh = 32 * k + 8, 32 * k + 22
        sheet = Image.new("RGB", (cols * cw, ((len(picks) + cols - 1) // cols) * chh), (24, 24, 24))
        draw = ImageDraw.Draw(sheet)
        for i, (t, n, px) in enumerate(picks):
            img = Image.new("RGB", (32, 32))
            img.putdata([palette[b] for b in px])
            x, y = (i % cols) * cw + 4, (i // cols) * chh + 4
            sheet.paste(img.resize((32 * k, 32 * k), Image.NEAREST), (x, y))
            draw.text((x, y + 32 * k + 2), f"{t} x{n}", fill=(230, 230, 230))
        SHEETS.mkdir(parents=True, exist_ok=True)
        out = SHEETS / f"tiles-{Path(tavfs._key(args.map)).stem.replace(' ', '_')}.png"
        sheet.save(out)
        print(f"contact sheet: {out.relative_to(REPO)} (gitignored)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gamedir", help="the Total Annihilation install (default: TA3DO_GAMEDIR, "
                                      "else Steam's default library)")
    ap.add_argument("--exe", help=f"the executable whose archive pattern mounts first (default: the install's "
                                  f"{tavfs.EXE_NAME})")
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tiles", help="a map's flat, featureless, tileable tiles and a contact sheet")
    t.add_argument("map", help='the TNT path as the engine opens it, e.g. "maps\\\\Crossing Two Rivers.tnt"')
    t.add_argument("--count", type=int, default=40, help="how many tiles to list (default 40)")
    sub.add_parser("build", help="write art/wiki/stage/<entry>.json for every entry")
    sub.add_parser("check", help="resolve every description against the install and its entry's scene")
    args = ap.parse_args(argv)
    try:
        vfs = tavfs.Vfs(tavfs.find_install(args.gamedir), args.exe)
    except tavfs.VfsError as e:
        print(f"stagemap: {e}", file=sys.stderr)
        return 2
    return {"tiles": cmd_tiles, "build": cmd_build, "check": cmd_check}[args.cmd](vfs, args)


if __name__ == "__main__":
    sys.exit(main())
