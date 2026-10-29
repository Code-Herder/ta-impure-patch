#!/usr/bin/env python3
"""gfx.py -- the in-game wiki mockup's 3D stage packs, resolved from the player's own install.

    .venv-undither/bin/python tools/wiki/gfx.py                      # the install found as below
    .venv-undither/bin/python tools/wiki/gfx.py --gamedir <TA install> [--exe <its executable>]
    .venv-undither/bin/python tools/wiki/gfx.py --resolve-json <file>   # also the resolution, element by element

Writes tools/wiki/mockup/gfx/ -- gitignored, because everything in it is derived from the game's own files and
nothing derived from the game is ever committed (CLAUDE.md, *Nothing from the original game, ever*). The mockup's
`gfx=game` reads gfx/index.json and draws the stage with tools/tascene-render.js, tagpu's own shaders; without the
folder it stays on the whitebox. research/notes/ingame-wiki-3d-stage-plan.md section 9 is the design.

One tascene-format pack a stage, one stage per scene of a wiki entry, `gfx/<entry>.<scene>/scene.json` and its
binaries:

  terrain/   the stage description's tile set as tagpu_terr.c's R8 atlas (64 tiles a row, 34-texel pitch), its
             tile map (one u16 a 32-unit tile, an index into that set) and a zero height grid (the stage is flat)
  features/  the description's 2D features: their frames in an R8 atlas, the anchors, the definitions
  units/     every unit type the entry names: its model as the engine holds it, in tascene's mesh format, one mesh
             per owner the entry's scenes use (a team face takes that owner's logo frame), the structure shadow's
             slant casters, the nanoframe's wire edges, and the frames in an R8 atlas
  palette/   PALETTE.PAL and PALETTE.SHD
  sounds/    the WAVs the units' sound categories and the sidebar's buttons play, named by sounds.json
  shaders    gfx/shaders/, tagpu's GLSL extracted from the C sources (the repository's, not the game's)

and gfx/index.json, which names each stage's pack and carries the whole resolution report; and the game's cursors,
gfx/cursors.json and its sheet gfx/cursors.rgba, which the stage draws its pointer with in either look.

THE RULES ARE THE ENGINE'S, through tools/wiki/tavfs.py (loose first, then the first archive in the engine's mount
order; an enumeration lists each path once, from its first source), with the directory names read out of the
install's executable at their addresses, so a mod that renames `units` or `logos` is followed (the scout answers,
D1/D2). Each element is resolved or refused by name:

- **The ground** (section 4 of the plan): each source TNT opened, its tile count compared, each referenced tile
  hashed (FNV-1a 32) and compared; each feature section found in the first `features\\*.tdf` that holds it
  (LoadFeature 0x4224B0), its GAF `anims\\<filename>.GAF` loaded and its `seqname` (and `seqnameshad`) found, first
  sequence by name without case. ANY miss and the pack carries no terrain and no features (`ground.ok` false): the
  ground falls back whole, never a patchwork.
- **A unit** (A2, A3): `units\\<UnitName>.FBI` by path; its `Objectname`, else the UnitName, names
  `objects3d\\<name>.3DO`; `scripts\\<UnitName>.COB` gives Create()'s hides and dont-caches. A miss makes that type
  fall back alone (its whitebox box).
- **The model as the engine holds it** (A4): in every node whose selection primitive is not -1, that face and face 0
  swap, and faces 1.. are sorted ascending by their vertices' mean Y (a stable sort: the engine's bubble sort swaps on
  strictly greater), the truncated mean of the 16.16 values; face 0 is then the selection face, which the draws skip.
  The mesh stays in the file's axes; the X and Z negation the loader applies is a half turn, which gfx.js applies with
  the record's yaw (the stage's turn of the model as the engine holds it). Beside the mesh: each vertex's engine
  normal and its quad's four corners' (`*.normals.bin`, 15 floats a vertex), and per owner each vertex's quad frame
  (`*.<owner>.quad.bin`, 8 floats: tagpu_posebake.c `quad_frame`'s corner point, its (a, b) and the frame's rect).
- **The pieces** (COB L3b): every piece's faces are in the mesh, the slant casters and the wire, Create()'s hides
  included, because a script can show what Create hid. `*.pieces.bin` gives each vertex its piece (u16: the body's
  vertices, then the slant's, then the wire's), and the mesh's `pieces` table, one row a piece, its parent, its
  cumulative rest offset in the file's axes and whether Create() hides it or marks it dont-cache. Both are in the
  posed order (`script.order` maps it to the tree's pre-order walk) for a type with a script, in the walk's order
  for one without. The rest pose leaves out a hidden piece's vertices, and an uncached one's slant casters; a posed
  unit draws by its script's flags (tools/wiki/mockup/pose.js).
- **Textures** (A4, A5): the texture bank is `textures\\*.GAF` in enumeration order without `logos.GAF`; a face's
  name binds to the first GAF in bank order that has the entry; on a miss to `textures\\logos.GAF`, as a team face
  (the owner's frame) only when the entry has exactly 10 frames; when neither has it, the face is flat colour 0xD1.
  An animated face shows its frame 0, the binder's starting frame.
- **Language variants**: every engine open tries `<dir>-<language>\\` first (0x4290F0); `--language`, english by
  default as the registry's.

Colour is palette colour: no restorer runs (the plan's question 7: the stage is Classic).

Each stage pack also carries `minimap.bin`, the minimap picture the engine generates from the stage's tiles
(`minimap_base`), shrunk with this install's palettes\\PALETTE.ALP.

THE SIDEBAR PACK (`--side-only --mod <m>`; tools/wiki/sidepacks.py runs it once per compat setup) is the mod's left
sidebar as the stage draws it and each side's own units for the wiki's demos: `gfx/side/<m>/side.json` and its
binaries (research/notes/ingame-wiki.md, *The data format*, a mod's pack). side.json holds
  layout   exactly the object tools/wiki/mockup/sim.js `checkLayout` takes: every side that has MAIN2 and GEN screens,
           in sidedata's order, with the pages of its cast types (`sides[k].types`), and every screen's buttons with
           their roles and rects
  casting  index-aligned with layout.sides: the side's commander and, for every cast, the unit the cast rule picks
           (its type, display name, footprint, G cells and figures, and which casts its menu builds) or null, with
           the reason for each pick
  art      `sheet.bin` (u8 palette indices, 1024 wide; every frame [sx, sy, w, h, hx, hy] with its GAF key),
           `pal.bin` (PALETTE.PAL as 256 RGBA), PANELSIDE of each layout side, each screen's panel picture, one
           picture a button (`pics[i]` belongs to `layout.screens[S].gadgets[i]`) and its labels, `greyed.bin` (the
           tint over a greyed button, greyed_table), the minimap dots, and the minimap's colours:
           `unexplored` and `box` (GUI colours 0 and 0x0E), `seen.bin` (the grey remap) and one base per stage,
           `minimap/<entry>.<scene>.bin`, shrunk with that install's own PALETTE.ALP
  report   every element's source, and every refusal, by name
and with `--stages`, `gfx/side/<m>/stage/`: the stage packs above, laid out as in gfx/ (`<entry>.<scene>/scene.json`,
`palette/`, `shaders/`, `index.json`), resolved from the mod's install and holding, beside each entry's own unit
types, every side's type for each cast the entry's units name (`stage_types`). The mockup draws a demo bound to the
mod's casting from these, so each side shows its own models.
The rules are the engine's, each cited where it is applied: the .GUI files by position (tavfs.gui_sections), the page
probe and the download fill of the def load and the page loader, the greyed pass, the picture rule of decision 5,
the FBI's figures and display name. The cast rule (`side_casting`) is the wiki's own and names no mod.
"""

import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "mockup" / "gfx"
ENTRIES = REPO / "art" / "wiki" / "entries"
STAGE = REPO / "art" / "wiki" / "stage"
sys.path.insert(0, str(HERE))
import tavfs  # noqa: E402

tascene = tavfs.tascene
ta3do = tavfs.ta3do

FORMAT = 1
TILE_BYTES = 1024
FLAT_MISS = 0xD1           # the binder's colour for a texture name no bank holds (0x42A204)
LANGUAGE = "english"       # 0x51FB50 when neither a command-line argument nor the registry gives one
TEAM_FRAMES = 10           # a logos.GAF entry is a team face only with exactly this many frames (0x42A1F2)

# The executable's directory strings (the scout answers, D1); a mod renames them in place (D2)
VA = {"units": 0x503920, "objects3d": 0x5036E4, "scripts": 0x503740, "textures": 0x503720,
      "logos_gaf": 0x503704, "logos": 0x504734, "anims": 0x502E30, "features": 0x502C7C,
      "palettes": 0x5031BC, "palette": 0x5033A4,
      # the left sidebar's (MEASURED from the pristine exe; exe-reverse-engineering.md, *The left sidebar*)
      "gui": 0x502820, "gamedata": 0x503360, "unitpics": 0x505FE4, "download": 0x503730,
      "commongui": 0x50925C, "guipal": 0x5031C8}
# the folders --folders may override: the ones a mod's loader renames in memory, after the exe file was read
FOLDERS = ("gui", "unitpics", "gamedata", "download")


def fnv1a32(data: bytes) -> str:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f"{h:08x}"


class Miss(Exception):
    pass


# --------------------------------------------------------------------------- the engine's opens

class Engine:
    """The install as the engine asks it: tavfs's precedence, the executable's names, the language variant."""

    def __init__(self, root, exe=None, folders=None, archive=None, language=LANGUAGE):
        """`folders` ({name: folder}, FOLDERS only) and `archive` (the first mount pattern) replace what the
        executable's file says: a mod's loader rewrites those strings in memory at DllMain, before the engine reads
        them (the Q6 scout answer), and the offline pack reads the file. `language` stands for 0x51FB50, which names
        both the folder variant `open` tries first and the key a unit's display name is read under (`display_name`)."""
        self.vfs = tavfs.Vfs(root, exe, archive)
        self.language = language
        exe_path = Path(exe) if exe else self.vfs._loose_path(tavfs.EXE_NAME)
        blob = exe_path.read_bytes()
        self.dirs = {k: tavfs.exe_cstr(blob, va) for k, va in VA.items()}
        self.folders_from = "exe"
        for k, v in (folders or {}).items():
            if k not in FOLDERS:
                raise tavfs.VfsError(f"--folders: {k} is not one of {', '.join(FOLDERS)}")
            self.dirs[k] = v
            self.folders_from = "setup record"
        self._bank = None
        self._logos = None
        self._features = None

    def open(self, d, name, ext):
        """0x4290F0's path, `<d>-<language>\\<name>.<ext>` first: (bytes, path, where) or None."""
        name = name.rsplit(".", 1)[0] if "." in name else name      # it cuts at the last '.'
        for path in (f"{d}-{self.language}\\{name}.{ext}", f"{d}\\{name}.{ext}"):
            hit = self.vfs.open(path)
            if hit is not None:
                return hit[0], path, hit[1]
        return None

    def load(self, d, name, ext):
        """`open` as the engine's probes see it: a file of size 0 is missing (the open test 0x4BBC40 returns the
        size), so this gives None for one."""
        hit = self.open(d, name, ext)
        return hit if hit and hit[0] else None

    def gaf(self, d, name):
        hit = self.load(d, name, "GAF")
        if hit is None:
            return None
        try:
            return Gaf(hit[0]), hit[1], hit[2]
        except (struct.error, ValueError, IndexError):
            return None

    def bank(self):
        """The texture bank, in the engine's order: `textures\\*.GAF` enumerated, logos.GAF skipped (0x42A440)."""
        if self._bank is None:
            self._bank = []
            skip = self.dirs["logos_gaf"].lower()
            for path, where in self.vfs.enumerate(self.dirs["textures"], "*.gaf", False):
                base = path.rsplit("/", 1)[-1]
                if base.lower() == skip:
                    continue
                got = self.gaf(self.dirs["textures"], base)
                if got:
                    self._bank.append(got)
        return self._bank

    def logos(self):
        if self._logos is None:
            self._logos = self.gaf(self.dirs["textures"], self.dirs["logos"]) or False
        return self._logos or None

    def feature(self, name):
        """(section, tdf path, where) from the first `features\\*.tdf` holding it, in enumeration order."""
        if self._features is None:
            self._features = {}
            for path, where in self.vfs.enumerate(self.dirs["features"], "*.tdf", True):
                try:
                    text = self.vfs.read_from(path, where).decode("latin-1")
                except Exception:
                    continue
                for sec, body in tascene.parse_tdf(text).items():
                    if isinstance(body, dict) and body:
                        self._features.setdefault(sec, (body, path, where))
        return self._features.get(name.lower())


class Gaf:
    """A GAF's sequences in file order: name -> (frame count, [frame offsets]); the first of a name wins (0x4B8D40)."""

    def __init__(self, blob):
        sig, count, _ = struct.unpack_from("<III", blob, 0)
        if sig != 0x00010100:
            raise ValueError("not a GAF")
        self.blob = blob
        self.seqs, self.heads = {}, {}      # heads: each name's sequence header offset
        for off in struct.unpack_from(f"<{count}I", blob, 12):
            n = struct.unpack_from("<H", blob, off)[0]
            name = blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1").lower()
            frames = [struct.unpack_from("<I", blob, off + 40 + 8 * i)[0] for i in range(n)]
            self.seqs.setdefault(name, (n, frames))
            self.heads.setdefault(name, off)

    def frame(self, seq, k=0):
        """(GafFrame, hotspot x, hotspot y) of frame k of `seq` as the blit draws it, or None."""
        hit = self.drawn(seq, k)
        return hit[:3] if hit else None

    def drawn(self, seq, k=0):
        """(GafFrame, hotspot x, hotspot y, pixels outside the frame) of frame k of `seq` as CopyGafToContext
        0x4B7F90 draws it (ta3do.gaf_compose: sub-frames composed into the frame's own box), or None. The commongui
        order, tab and pager buttons are such stacks (a body and its lettering)."""
        hit = self.seqs.get(seq.lower())
        if not hit or not 0 <= k < len(hit[1]):
            return None
        off = hit[1][k]
        hx, hy = struct.unpack_from("<hh", self.blob, off + 4)
        fr, outside = ta3do.gaf_compose(self.blob, off)
        return fr, hx, hy, outside


# --------------------------------------------------------------------------- the game's cursors

CURSOR_SEQ = {"arrow": "cursornormal", "move": "cursormove", "build": "cursornormal", "reclaim": "cursorreclamate"}   # WIKI_CURSOR_*'s sequences
SPRITE_CELL, SPRITE_CELLS = 64, 16                       # tagpu_hudlay.h HUD_SPRITE_CELL, HUD_SPRITE_CELLS


def cursors(eng, palette, out_root):
    """The game's cursors as the stage draws them (tagpu_wikistate.c builds the same sheet): `<anims>\\cursors.GAF` by
    the engine's path rule, each shape's sequence (CURSOR_SEQ) decoded through PALETTE.PAL into one 64 x 64 cell a
    frame of an RGBA sheet, the frame's key transparent; `gfx/cursors.json` says where each frame is, its size, its
    hotspot and its 30 Hz ticks, and each shape's first cell, frame count and loop byte. A sequence that is missing,
    has a frame larger than a cell or would not fit the sheet leaves its shapes on the kit's icons (n 0). Returns the
    resolution: the GAF, its source and each sequence's frame count."""
    got = eng.gaf(eng.dirs["anims"], "cursors")
    rep = {"gaf": got[1].lower() if got else None, "from": got[2].lower() if got else None,
           "sequences": [{"name": CURSOR_SEQ[k], "frames": len(got[0].seqs[CURSOR_SEQ[k]][1])
                          if got and CURSOR_SEQ[k] in got[0].seqs else None} for k in ("arrow", "move", "build")]}
    sheet = bytearray(SPRITE_CELL * SPRITE_CELLS * SPRITE_CELL * 4)
    frames, seq, placed = [], {}, {}
    for shape, name in CURSOR_SEQ.items():
        if name in placed:
            seq[shape] = placed[name]
            continue
        seq[shape] = placed[name] = {"first": 0, "n": 0, "loop": 0}
        if not got or name not in got[0].seqs:
            continue
        gaf = got[0]
        n, offs = gaf.seqs[name]
        so = gaf.heads[name]
        loop, durs = gaf.blob[so + 2], [struct.unpack_from("<H", gaf.blob, so + 44 + 8 * i)[0] for i in range(n)]
        decoded = [(ta3do.gaf_frame(gaf.blob, o), *struct.unpack_from("<hh", gaf.blob, o + 4)) for o in offs]
        if not n or len(frames) + n > SPRITE_CELLS or any(f.width > SPRITE_CELL or f.height > SPRITE_CELL for f, _x, _y in decoded):
            continue
        placed[name].update(first=len(frames), n=n, loop=loop)
        for (f, hx, hy), dur in zip(decoded, durs):
            sx = len(frames) * SPRITE_CELL
            for y in range(f.height):
                for x in range(f.width):
                    i = f.pixels[y * f.width + x]
                    if i != f.transparent:
                        o = (y * SPRITE_CELL * SPRITE_CELLS + sx + x) * 4
                        sheet[o:o + 4] = bytes((*palette[i], 255))
            frames.append({"sx": sx, "w": f.width, "h": f.height, "hx": hx, "hy": hy, "dur": dur})
    (out_root / "cursors.rgba").write_bytes(bytes(sheet))
    (out_root / "cursors.json").write_text(json.dumps({
        "note": "derived from the local install by tools/wiki/gfx.py; gitignored, never committed",
        "sheet": [SPRITE_CELL * SPRITE_CELLS, SPRITE_CELL], "cell": SPRITE_CELL, "frames": frames, "seq": seq, **rep},
        indent=1) + "\n")
    return rep


# --------------------------------------------------------------------------- the ground

def tile_atlas(tiles):
    """tagpu_terr.c's R8 atlas of `tiles` (1024 bytes each): 34 x 34 guarded cells, 64 a row."""
    cols, pitch, border, tp = tascene.ATLAS_COLS, tascene.CELL_PITCH, tascene.CELL_BORDER, tascene.TILE_PX
    aw = tascene.ATLAS_W
    h = max(1, (len(tiles) + cols - 1) // cols) * pitch
    buf = bytearray(aw * h)
    for i, t in enumerate(tiles):
        ox, oy = (i % cols) * pitch + border, (i // cols) * pitch + border
        for r in range(tp):
            row = t[r * tp:(r + 1) * tp]
            o = (oy + r) * aw + ox
            buf[o - 1] = row[0]
            buf[o:o + tp] = row
            buf[o + tp] = row[tp - 1]
        top, first = (oy - 1) * aw + ox - 1, oy * aw + ox - 1
        buf[top:top + pitch] = buf[first:first + pitch]
        bot, last = (oy + tp) * aw + ox - 1, (oy + tp - 1) * aw + ox - 1
        buf[bot:bot + pitch] = buf[last:last + pitch]
    return aw, h, bytes(buf)


def resolve_ground(eng, desc, report):
    """The description's tiles and features, every one checked; raises Miss naming the first failure."""
    maps = {}
    for i, src in enumerate(desc["sources"]):
        hit = eng.vfs.open(src["map"])
        if hit is None:
            raise Miss(f"{src['map']}: no such file in the install")
        try:
            tnt = tascene.Tnt(hit[0], src["map"])
        except ValueError as e:
            raise Miss(f"{src['map']} ({hit[1]}): not a TNT the readers take: {e}")
        if tnt.tiles != src["tiles"]:
            raise Miss(f"{src['map']} ({hit[1]}): {tnt.tiles} tiles, the description says {src['tiles']}")
        maps[i] = tnt
        report.append({"element": "map", "path": src["map"], "from": hit[1], "tiles": tnt.tiles})
    tiles = []
    for k, (si, ti, want) in enumerate(desc["tileset"]):
        tnt = maps.get(si)
        if tnt is None or not 0 <= ti < tnt.tiles:
            raise Miss(f"tileset row {k}: source {si} tile {ti} is not in the map")
        o = tnt.p_tilegfx + ti * TILE_BYTES
        t = tnt.blob[o:o + TILE_BYTES]
        got = fnv1a32(t)
        if got != want:
            raise Miss(f"tileset row {k}: {desc['sources'][si]['map']} tile {ti} hashes {got}, "
                       f"the description says {want}")
        tiles.append(t)
        report.append({"element": "tile", "row": k, "source": si, "tile": ti, "hash": got})
    n = len(tiles)
    if any(not (isinstance(v, int) and 0 <= v < n) for v in desc["tilemap"]):
        raise Miss("the tile map names a tile the tile set does not have")
    feats = []
    for f in desc["features"]:
        hit = eng.feature(f["name"])
        if hit is None:
            raise Miss(f"feature {f['name']}: no section in any {eng.dirs['features']}\\*.tdf")
        body, tdf, where = hit
        rec = {"name": f["name"].lower(), "cell": f["cell"], "body": body, "tdf": tdf, "from": where}
        if (body.get("object") or "").strip():
            raise Miss(f"feature {f['name']}: a 3D feature ({body['object']}), which the stage's ground does not take")
        filename = (body.get("filename") or "").strip()
        got = eng.gaf(eng.dirs["anims"], filename) if filename else None
        if got is None:
            raise Miss(f"feature {f['name']}: no {eng.dirs['anims']}\\{filename}.GAF")
        gaf, gpath, gwhere = got
        for slot, key in (("bodyseq", "seqname"), ("shadowseq", "seqnameshad")):
            seq = (body.get(key) or "").strip()
            fr = gaf.frame(seq) if seq else None
            if seq and fr is None:
                raise Miss(f"feature {f['name']}: {key} {seq} is not in {gpath}")
            rec[slot] = (seq.upper(), fr) if fr else None
        if rec["bodyseq"] is None:
            raise Miss(f"feature {f['name']}: no seqname")
        feats.append(rec)
        report.append({"element": "feature", "name": rec["name"], "cell": f["cell"], "tdf": tdf, "from": where,
                       "gaf": gpath, "gafFrom": gwhere})
    return tiles, feats


def write_ground(out, desc, tiles, feats):
    """terrain/ and features/ in tascene's layout; returns the scene.json `map` and `features` blocks."""
    mw, mh = desc["cells"]
    (out / "terrain").mkdir(parents=True, exist_ok=True)
    aw, ah, atlas = tile_atlas(tiles)
    (out / "terrain/atlas.r8.bin").write_bytes(atlas)
    (out / "terrain/tilemap.u16.bin").write_bytes(struct.pack(f"<{len(desc['tilemap'])}H", *desc["tilemap"]))
    (out / "terrain/height.r8.bin").write_bytes(bytes(mw * mh))
    mapblock = {"name": desc["entry"], "w16": mw, "h16": mh,
                "tilemap": {"w": mw // 2, "h": mh // 2, "file": "terrain/tilemap.u16.bin"},
                "atlas": {"w": aw, "h": ah, "cols": tascene.ATLAS_COLS, "tiles": len(tiles),
                          "pitch": tascene.CELL_PITCH, "border": tascene.CELL_BORDER,
                          "file": "terrain/atlas.r8.bin", "rgba": None},
                "height": {"w": mw, "h": mh, "file": "terrain/height.r8.bin"}, "sealevel": 0}
    frames, hot, table, index = {}, {}, {}, {}
    for f in feats:
        if f["name"] not in index:
            index[f["name"]] = len(index)
            body = f["body"]
            ent = {"name": f["name"], "height": int(float(body.get("height", 0) or 0)),
                   "footx": int(float(body.get("footprintx", 1) or 1)),
                   "footz": int(float(body.get("footprintz", 1) or 1)),
                   "animtrans": 1 if (body.get("animtrans", "0") or "0").strip() == "1" else 0,
                   "shadtrans": 1 if (body.get("shadtrans", "0") or "0").strip() == "1" else 0}
            ent["flat"] = 1 if ent["height"] < 10 else 0
            for slot in ("bodyseq", "shadowseq"):
                if f[slot]:
                    seq, (frame, hx, hy) = f[slot]
                    frames[seq] = frame
                    hot[seq] = (hx, hy)
                    ent[slot[:-3]] = seq
            table[index[f["name"]]] = ent
    place, ah = tascene.shelf_pack(frames)
    aw, ah = (2048 if frames else 1), max(1, ah)
    atlas = bytearray(aw * ah)
    rects = {}
    for key, (x, y) in place.items():
        fr = frames[key]
        for r in range(fr.height):
            o = (y + r) * aw + x
            atlas[o:o + fr.width] = fr.pixels[r * fr.width:(r + 1) * fr.width]
        rects[key] = {"x": x, "y": y, "w": fr.width, "h": fr.height,
                      "hx": hot[key][0], "hy": hot[key][1], "ck": fr.transparent}
    (out / "features").mkdir(parents=True, exist_ok=True)
    (out / "features/atlas.r8.bin").write_bytes(bytes(atlas))
    inst = bytearray()
    for f in feats:
        inst += struct.pack("<3H", f["cell"][0], f["cell"][1], index[f["name"]])
    (out / "features/instances.bin").write_bytes(bytes(inst))
    defs = {str(i): {**{k: e[k] for k in ("name", "height", "flat", "footx", "footz", "animtrans", "shadtrans")},
                     "body": rects.get(e.get("body")), "shadow": rects.get(e.get("shadow"))}
            for i, e in table.items()}
    return mapblock, {"count": len(feats), "map": len(feats), "instances": "features/instances.bin",
                      "atlas": {"w": aw, "h": ah, "file": "features/atlas.r8.bin", "rgba": None}, "defs": defs}


# --------------------------------------------------------------------------- the units

def fbi_fields(text):
    import re
    return {m.group(1).lower(): m.group(2).strip()
            for m in re.finditer(r"^\s*([A-Za-z0-9_]+)\s*=\s*([^;]*);", text, re.M)}


def engine_faces(piece):
    """A node's faces in the order the loader leaves them (A4): the selection face swapped to 0, the rest sorted by
    their vertices' mean Y, truncated as `cdq; idiv` truncates. Returns (faces, skip0)."""
    faces = list(piece.primitives)
    sel = piece.sel_prim
    skip0 = sel != -1 and len(faces) > 0
    if skip0 and 0 <= sel < len(faces):
        faces[0], faces[sel] = faces[sel], faces[0]

    def key(prim):
        # a face with no vertices divides by zero in the engine; no stock face has one (MEASURED, A4)
        if not prim.indices:
            return 0
        s = sum(int(round(piece.vertices[i][1] * 65536)) for i in prim.indices if i < len(piece.vertices))
        q = abs(s) // len(prim.indices)
        return q if s >= 0 else -q
    faces[1:] = sorted(faces[1:], key=key)
    return faces, skip0


def vertex_normals(piece, faces, skip0):
    """The engine's vertex normal of every vertex of a node (tagpu_posebake.c `piece_vnormals`, 0x45A0FD..0x45A22A):
    each face's unit normal from its first three indices, against the winding ((V1-V0)x(V2-V0) negated), (0, 1, 0)
    for a face repeating one of them or of no length; added to every vertex the face names and divided by the count,
    not renormalised; (0, 0, 0) for a vertex no face names. Face 0 is left out when it is the selection face. In the
    file's axes: the loader's negation of X and Z is a rotation, which turns the normals with the vertices."""
    nv = len(piece.vertices)
    acc = [[0.0, 0.0, 0.0] for _ in range(nv)]
    cnt = [0] * nv
    for j, prim in enumerate(faces):
        idx = prim.indices
        if (j == 0 and skip0) or len(idx) < 3 or len(idx) > 32 or any(i >= nv for i in idx):
            continue
        f = (0.0, 1.0, 0.0)
        if idx[0] != idx[1] and idx[1] != idx[2] and idx[2] != idx[0]:
            a, b, c = (piece.vertices[i] for i in idx[:3])
            e1 = [b[k] - a[k] for k in range(3)]
            e2 = [c[k] - a[k] for k in range(3)]
            x = e1[1] * e2[2] - e1[2] * e2[1]
            y = e1[2] * e2[0] - e1[0] * e2[2]
            z = e1[0] * e2[1] - e1[1] * e2[0]
            nl = (x * x + y * y + z * z) ** 0.5
            if nl > 1e-6:
                f = (-x / nl, -y / nl, -z / nl)
        for i in idx:
            for k in range(3):
                acc[i][k] += f[k]
            cnt[i] += 1
    return [[v / cnt[i] for v in acc[i]] if cnt[i] else [0.0, 0.0, 0.0] for i in range(nv)]


def quad_frame(piece, idx):
    """A four-vertex face's frame (tagpu_posebake.c `quad_frame`): corner 2's point (a, b) in the frame of the edges
    to corners 1 and 3, the least-squares fit, or None where the face keeps its two triangles' mapping -- e and f all
    but parallel, or the quad not convex in the frame (a > 0, b > 0, a + b > 1, each with a 1/64 margin). An affine
    invariant, so the file's axes give the engine's numbers."""
    P = [piece.vertices[i] for i in idx]
    e = [P[1][r] - P[0][r] for r in range(3)]
    f = [P[3][r] - P[0][r] for r in range(3)]
    d = [P[2][r] - P[0][r] for r in range(3)]
    ee, ff, ef = sum(x * x for x in e), sum(x * x for x in f), sum(x * y for x, y in zip(e, f))
    de, df = sum(x * y for x, y in zip(d, e)), sum(x * y for x, y in zip(d, f))
    det = ee * ff - ef * ef
    if not det > 1e-6 * ee * ff:
        return None
    a, b = (de * ff - df * ef) / det, (df * ee - de * ef) / det
    if not (a > 1 / 64 and b > 1 / 64 and a + b > 1 + 1 / 64):
        return None
    return a, b


def resolve_unit(eng, name, report):
    """(label, root piece, hidden, uncached, facts, fbi path) for unit `name`, or raises Miss."""
    hit = eng.open(eng.dirs["units"], name, "FBI")
    if hit is None:
        raise Miss(f"{name}: no {eng.dirs['units']}\\{name}.FBI")
    fbi = fbi_fields(hit[0].decode("latin-1", "replace"))
    obj = (fbi.get("objectname") or name).strip()
    mhit = eng.open(eng.dirs["objects3d"], obj, "3DO")
    if mhit is None:
        raise Miss(f"{name}: no {eng.dirs['objects3d']}\\{obj}.3DO")
    try:
        root = ta3do.parse_3do(mhit[0])
    except (struct.error, ValueError, IndexError, RecursionError) as e:
        raise Miss(f"{name}: {mhit[1]} does not parse: {e}")
    shit = eng.open(eng.dirs["scripts"], name, "COB")
    hidden = {h.lower() for h in ta3do.hidden_at_create(shit[0])} if shit else set()
    uncached = {h.lower() for h in ta3do.dontcached_at_create(shit[0])} if shit else set()

    def flag(k):
        return (fbi.get(k, "0") or "0").strip() not in ("", "0")
    mobile = (fbi.get("bmcode", "1") or "1").strip() != "0"
    slant = not mobile and not flag("digger")
    facts = {"shadow": bool(not flag("noshadow") and (slant or (not flag("canhover") and not flag("floater")))),
             "slant": slant, "mobile": mobile}
    nodes = [p for p, _ in root.walk()]
    pose = posed_order(root, shit[0]) if shit else None
    report.append({"element": "unit", "name": name, "fbi": hit[1], "fbiFrom": hit[2], "model": mhit[1],
                   "modelFrom": mhit[2], "script": shit[1] if shit else None,
                   "scriptFrom": shit[2] if shit else None, "hidden": sorted(hidden),
                   "uncached": sorted(uncached), "facts": facts,
                   "posed": {"pieces": [nodes[k].name for k, _v in pose],
                             "visible": "".join("1" if v else "0" for _k, v in pose)} if pose else None})
    return obj.lower(), root, hidden, uncached, facts


def posed_order(root, cob):
    """The posed model's primitives as the engine lays them out for a script (exe-reverse-engineering.md, *The
    posed model the opcodes write into*): pre-order over the tree (0x45AEC0), then for each COB piece i below the
    primitive count the first primitive from index i up whose name equals piece i's without case swaps into place
    (0x45A950). [(walk index, visible)] in that order, visible when the node has three or more vertices
    (0x45AF1B). ta3do reads no sibling of the root, which no stock model has [INFERRED for mods]."""
    nodes = [p for p, _ in root.walk()]
    order = list(range(len(nodes)))
    got = ta3do._cob_read(cob)
    pieces = got[2] if got else []
    for i, want in enumerate(pieces[:len(order)]):
        for j in range(i, len(order)):
            if nodes[order[j]].name.lower() == want:
                order[i], order[j] = order[j], order[i]
                break
    return [(k, len(nodes[k].vertices) >= 3) for k in order]


def model_shape(root, order):
    """The model's shape the wiki's COB host takes (tagpu_wikicob.h WIKICOBTYPE), for the tree `root` whose posed
    order is `order` (walk indices, posed_order's): per primitive its node's offset from its parent's, three 16.16 words
    in the axes the engine holds a model in -- X and Z negated, as the loader 0x4CB590 negates them -- and its parent's
    primitive index, -1 for the root; and `top`, 0x4CB5F0's height: over a sibling chain, the most of 0 and, for each
    node, its vertices' y and its child chain's height, each plus the node's y offset (0x4CB5F0..0x4CB641,
    DISASSEMBLED). The DLL's 3D load computes the same from its arena (tagpu_wikiasset.c `unit_shape`)."""
    nodes = [p for p, _ in root.walk()]
    walk = {id(p): k for k, p in enumerate(nodes)}
    parent = {}
    for p in nodes:
        for c in p.children:
            parent[id(c)] = walk[id(p)]
    at = {k: i for i, k in enumerate(order)}

    def raw(v):
        return round(v * 65536)

    def i32(v):
        return (v + 2 ** 31) % 2 ** 32 - 2 ** 31

    def top(chain):
        m = 0
        for nd in chain:
            oy = raw(nd.offset[1])
            for v in nd.vertices:
                m = max(m, i32(raw(v[1]) + oy))
            if nd.children:
                m = max(m, i32(top(nd.children) + oy))
        return m

    offset = [[i32(-raw(nodes[k].offset[0])), i32(raw(nodes[k].offset[1])), i32(-raw(nodes[k].offset[2]))]
              for k in order]
    return {"offset": offset, "parent": [at.get(parent.get(id(nodes[k]), -1), -1) for k in order], "top": top([root])}


def bind(eng, tex, owner, binds):
    """The binder's rule for one face's texture name (0x42A140): (frame key, GafFrame) or None for flat 0xD1."""
    k = (tex.lower(), owner)
    if k in binds:
        return binds[k]
    got = None
    for gaf, path, _w in eng.bank():
        hit = gaf.seqs.get(tex.lower())
        if hit:
            fr = gaf.frame(tex, 0)
            got = (f"{path}:{tex.upper()}:0", fr[0]) if fr else None
            break
    else:
        logos = eng.logos()
        hit = logos[0].seqs.get(tex.lower()) if logos else None
        if hit:
            team = hit[0] == TEAM_FRAMES
            fi = owner if team else 0
            fr = logos[0].frame(tex, fi)
            got = (f"{logos[1]}:{tex.upper()}:{fi}", fr[0]) if fr else None
    binds[k] = got
    return got


def unit_meshes(eng, models, owners):
    """Every (type, owner)'s mesh in tascene's format, the slant casters and wire edges per type, and the atlas. Every
    piece is emitted, Create()'s hides too; `piece`, `slantPiece` and `wirePiece` give each vertex its piece's index in
    the tree's pre-order walk, and `walk` each piece's row of write_units' table, in that order."""
    binds, frames = {}, {}
    plan = {}
    for name, (label, root, hidden, uncached, facts) in models.items():
        for owner in owners:
            for piece, _d in root.walk():
                for prim in piece.primitives:
                    if prim.texture and len(prim.indices) >= 3:
                        got = bind(eng, prim.texture, owner, binds)
                        if got:
                            frames.setdefault(got[0], got[1])
    UNIT_PAD = tascene.UNIT_PAD
    place, ah = tascene.shelf_pack(frames, pad=UNIT_PAD, align=UNIT_PAD)
    aw, ah = (2048 if frames else 1), max(1, ah)
    atlas = bytearray(aw * ah)
    rects = {}
    for key, (x, y) in place.items():
        f = frames[key]
        tascene.blit_frame(atlas, aw, 1, x, y, f.width, f.height,
                           lambda r, f=f: f.pixels[r * f.width:(r + 1) * f.width], UNIT_PAD)
        rects[key] = (x / aw, y / ah, (x + f.width) / aw, (y + f.height) / ah, f.transparent)
    for name, (label, root, hidden, uncached, facts) in models.items():
        nodes = [p for p, _ in root.walk()]
        walk = {id(p): k for k, p in enumerate(nodes)}
        out = {"owners": {}, "quads": {}, "slant": [], "wire": [], "normals": [], "faces": 0, "flatMiss": 0,
               "piece": [], "slantPiece": [], "wirePiece": [], "walk": [None] * len(nodes)}
        for owner in owners:
            verts, quads = [], []
            stats = {"faces": 0, "flatMiss": 0}

            def emit(piece, ox, oy, oz, parent):
                px, py, pz = ox + piece.offset[0], oy + piece.offset[1], oz + piece.offset[2]
                k = walk[id(piece)]
                hid = piece.name.lower() in hidden
                first = owner == owners[0]
                if first:
                    out["walk"][k] = {"parent": parent, "off": [px, py, pz], "hidden": hid,
                                      "uncached": piece.name.lower() in uncached}
                faces, skip0 = engine_faces(piece)
                vn = vertex_normals(piece, faces, skip0) if first else None
                for i, prim in enumerate(faces):
                    if (i == 0 and skip0) or len(prim.indices) < 3:
                        continue
                    if any(ix >= len(piece.vertices) for ix in prim.indices):
                        continue
                    pts = [(px + piece.vertices[ix][0], py + piece.vertices[ix][1], pz + piece.vertices[ix][2])
                           for ix in prim.indices]
                    if first:
                        # the structure shadow's casters (0x45A610): every face, cast while its piece is visible
                        # and cached
                        for j in range(1, len(pts) - 1):
                            for c in (pts[0], pts[j], pts[j + 1]):
                                out["slant"].extend(c)
                                out["slantPiece"].append(k)
                        # the nanoframe's wire (0x458FA0): every face as a closed outline
                        for e in range(len(pts)):
                            out["wire"].extend(pts[e]); out["wire"].extend(pts[(e + 1) % len(pts)])
                            out["wirePiece"].extend((k, k))
                    rect = None
                    flat = (prim.color & 0xFF) / 255.0
                    if prim.texture:
                        got = bind(eng, prim.texture, owner, binds)
                        if got:
                            rect = rects[got[0]]
                        else:
                            flat = FLAT_MISS / 255.0
                            if not hid:
                                stats["flatMiss"] += 1
                    if not hid:
                        stats["faces"] += 1
                    n = len(pts)
                    qf = quad_frame(piece, prim.indices) if rect and n == 4 else None
                    for j in range(1, n - 1):        # fanned from vertex 0
                        for slot in (0, j, j + 1):
                            if vn is not None:
                                # the vertex's own normal, then the quad's four corners' (its own four times
                                # for any other face), as tagpu_posebake.c `geom_emit` carries them
                                out["normals"].extend(vn[prim.indices[slot]])
                                for c in range(4):
                                    out["normals"].extend(vn[prim.indices[c]] if n == 4 else vn[prim.indices[slot]])
                                out["piece"].append(k)
                            if qf:
                                a, b = qf
                                quads.extend((1.0 if slot == 1 else a if slot == 2 else 0.0,
                                              1.0 if slot == 3 else b if slot == 2 else 0.0,
                                              a, b, rect[0], rect[1], rect[2], rect[3]))
                            else:
                                quads.extend((0.0, 0.0, 1.0, 1.0, -1.0, -1.0, -1.0, -1.0))
                            vx, vy, vz = pts[slot]
                            if rect:
                                # the engine maps a whole GAF frame across the face's four corners; an n-gon
                                # collapses onto them, as tascene's parity lane does
                                c = slot if n <= 4 else slot * 4 // n
                                u = rect[2] if c in (1, 2) else rect[0]
                                v = rect[3] if c >= 2 else rect[1]
                                verts.extend((vx, vy, vz, u, v, 0.0, (rect[4] if rect[4] is not None else 0) / 255.0))
                            else:
                                verts.extend((vx, vy, vz, -1.0, -1.0, flat, -1.0))
                for child in piece.children:
                    emit(child, px, py, pz, k)

            emit(root, 0.0, 0.0, 0.0, -1)
            out["owners"][owner] = verts
            out["quads"][owner] = quads
            out["faces"], out["flatMiss"] = stats["faces"], stats["flatMiss"]
        plan[name] = out
    frame_rects = {key: {"x": x, "y": y, "w": frames[key].width, "h": frames[key].height,
                         "key": -1 if frames[key].transparent is None else frames[key].transparent}
                   for key, (x, y) in place.items()}
    return plan, (aw, ah, bytes(atlas), frame_rects)


def entry_owners(entry):
    """Every owner the entry's scenes give a unit, and 0, the player."""
    owners = {0}
    for sc in list(entry.get("scenes", {}).values()) + [d.get("scene", {}) for d in entry.get("demos", [])]:
        for u in sc.get("units", []) or []:
            if isinstance(u.get("owner"), int):
                owners.add(u["owner"])
    return sorted(owners)


def stage_types(entry, casting=None):
    """The unit types an entry's stage pack exports: the entry's own, then, for each cast its units name, every
    side's type for that cast in `casting` (a side pack's list), each name once. A bound demo's units carry the
    casting's type names (sim.js bindEntry) and an unbound cast keeps the entry's own type, so the pack holds a model
    for either."""
    names = list(entry["units"])
    casts = [u.get("cast") for u in entry["units"].values()]
    for side in casting or []:
        for c in casts:
            k = side["casts"].get(c)
            if k and k["type"] not in names:
                names.append(k["type"])
    return names


# THE SOUND KIND TABLE 0x5086D8's keys (+0xC) by kind, 1..23; the kinds the wiki says (tagpu_wikisim.h); the allsound
# names it plays (WIKI_SNDNAME_*, sim.js SND_NAMES)
SND_KEYS = [None, "select", "underattack", "activate", "deactivate", "ok", "arrived", "cant", "unitcomplete", "build",
            "repair", "working", "load", "unload", "cloak", "uncloak", "capture", "count5", "count4", "count3",
            "count2", "count1", "count0", "canceldestruct"]
SND_WIKI_KINDS = (1, 3, 4, 5, 6, 7, 8, 9, 11)
SND_NAMES = ("SelectMultipleUnits", "oktobuild", "notoktobuild", "addbuild", "buildbutton", "ordersbutton",
             "nextbuildmenu", "immediateorders", "Options")
SND_KINDS = 24


def sound_table(eng):
    """`<gamedata>\\sound.TDF` as 0x42F580 reads it, cached on `eng`: one record a top-level section in file order,
    {"name": its name cut to 63 characters (0x42F67E), "wav": per kind 0..23 the variants' WAV names}. A kind's
    variants are `<key>` when present, then `<key>1`, `<key>2`, ... up to the first one missing (0x42F694..0x42F6F1),
    each value cut to 63 characters (0x4C48C0's 0x40). A file missing or refused is no category at all."""
    if getattr(eng, "_sound", None) is None:
        hit = eng.load(eng.dirs["gamedata"], "sound", "TDF")
        recs = []
        try:
            secs = tavfs.tdf_sections(hit[0]) if hit else []
        except tavfs.TdfError:
            secs = []
        for sec in secs:
            keys, wav = sec["keys"], [[] for _ in range(SND_KINDS)]
            for k in range(1, SND_KINDS):
                key = SND_KEYS[k]
                if key in keys:
                    wav[k].append(keys[key][:63])
                i = 1
                while f"{key}{i}" in keys:
                    wav[k].append(keys[f"{key}{i}"][:63])
                    i += 1
            recs.append({"name": sec["_name"][:63], "wav": wav})
        eng._sound = recs
    return eng._sound


def unit_sound(eng, fbi):
    """{"cat", "count"} for a unit whose FBI's fields are `fbi` (`fbi_fields`): its category as def+0x20E holds it,
    none (-1) outside the table, and the category's variant count per kind (all 0 for none)."""
    table = sound_table(eng)
    v = fbi.get("soundcategory")
    if v is None:
        cat = 0
    else:
        v = v[:99]                                  # 0x42CC5A reads 100 bytes
        cat = next((i for i, r in enumerate(table) if r["name"].lower() == v.lower()), None)
        if cat is None:
            cat = atoi(v)
    if not 0 <= cat < len(table):
        return {"cat": -1, "count": [0] * SND_KINDS}
    return {"cat": cat, "count": [len(w) for w in table[cat]["wav"]]}


def wav_pcm(blob):
    """tagpu_wav.c's rule: a RIFF WAVE file whose `fmt ` chunk is PCM (tag 1), 1 or 2 channels, 8 or 16 bits, a rate
    of 1000..192000 Hz and a block align of channels x bits / 8, and a `data` chunk after it holding at least 4 bytes
    of whole frames within the file (DSBSIZE_MIN). None when it passes, else why not."""
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        return "not a RIFF WAVE file"
    end = min(len(blob), 8 + struct.unpack_from("<I", blob, 4)[0])
    at, fmt = 12, None
    while at + 8 <= end:
        cid, n = blob[at:at + 4], struct.unpack_from("<I", blob, at + 4)[0]
        body = at + 8
        if cid == b"fmt ":
            if n < 16 or body + 16 > end:
                return "a short fmt chunk"
            tag, ch, rate, _bps, align, bits = struct.unpack_from("<HHIIHH", blob, body)
            if tag != 1:
                return f"format tag {tag}, not PCM"
            if ch not in (1, 2) or bits not in (8, 16) or not 1000 <= rate <= 192000 or align != ch * bits // 8:
                return f"{ch} channels, {bits} bits, {rate} Hz, align {align}"
            fmt = (ch, bits)
        elif cid == b"data":
            if fmt is None:
                return "data before fmt"
            frames = min(n, end - body) // (fmt[0] * fmt[1] // 8)
            return None if frames * (fmt[0] * fmt[1] // 8) >= 4 else "under 4 bytes of whole frames"
        if n > end - body:
            break
        at = body + n + (n & 1)
    return "no data chunk"


def write_sounds(eng, out_root, types):
    """`<out_root>/sounds.json` and the WAVs it names, `sounds/<n>.wav`: each mesh type's `sound` (`types`, the first
    entry's of a name), which the mockup's sim reads as its sound source (sound.js); for each category they use its
    name and, per kind the wiki says, the variants' files in variant order, null for one missing or refused; the
    file of each button sound's allsound name, as 0x42F7E0 binds the names (each section's `sound`, the first
    section of a name winning, at most 255: 0x429470); and what was refused. A WAV opens as `sounds\\<wav>.WAV` by
    the built path (0x4290F0), the language folder first."""
    table = sound_table(eng)
    files, refused, n = {}, [], [0]
    (out_root / "sounds").mkdir(parents=True, exist_ok=True)
    for old in (out_root / "sounds").glob("*.wav"):
        old.unlink()

    def wav(name):
        if not name:
            return None
        if name.lower() not in files:
            hit = eng.load("sounds", name, "WAV")
            why = "no file" if hit is None else wav_pcm(hit[0])
            if why:
                refused.append({"wav": name, "why": why})
                files[name.lower()] = None
            else:
                f = f"sounds/{n[0]}.wav"
                n[0] += 1
                (out_root / f).write_bytes(hit[0])
                files[name.lower()] = f
        return files[name.lower()]

    allsound, hit = {}, eng.load(eng.dirs["gamedata"], "allsound", "TDF")
    try:
        for sec in tavfs.tdf_sections(hit[0]) if hit else []:
            name = sec["_name"][:31].lower()
            if name not in allsound and len(allsound) < 255:
                allsound[name] = sec["keys"].get("sound", "")[:255]
    except tavfs.TdfError:
        pass
    cats = {t["cat"] for t in types.values()}
    doc = {"categories": len(table), "types": types,
           "cats": {str(c): {"name": table[c]["name"],
                             "kinds": {str(k): [wav(w) for w in table[c]["wav"][k]] for k in SND_WIKI_KINDS}}
                    for c in sorted(cats) if 0 <= c < len(table)},
           "named": {nm: wav(allsound.get(nm.lower())) for nm in SND_NAMES}}
    doc["refused"] = refused
    (out_root / "sounds.json").write_text(json.dumps(doc, indent=1) + "\n")
    return doc


def write_units(out, eng, entry, report, types):
    models, refused = {}, {}
    for name in types:
        try:
            models[name] = resolve_unit(eng, name, report)
        except Miss as e:
            refused[name] = str(e)
            report.append({"element": "unit", "name": name, "refused": str(e)})
    owners = entry_owners(entry)
    (out / "units").mkdir(parents=True, exist_ok=True)
    if not models:
        return {"meshes": {}, "refused": refused, "owners": owners}
    plan, (aw, ah, atlas, frame_rects) = unit_meshes(eng, models, owners)
    (out / "units/atlas.r8.bin").write_bytes(atlas)
    meshes = {}
    for name, p in plan.items():
        label, root, hidden, uncached, facts = models[name]
        key = name.lower()
        rec = {"label": label, "files": {}, **facts, "faces": p["faces"], "flatMiss": p["flatMiss"]}
        fbi = eng.open(eng.dirs["units"], name, "FBI")
        rec["sound"] = unit_sound(eng, fbi_fields(fbi[0].decode("latin-1", "replace")))
        for owner, verts in p["owners"].items():
            f = f"units/{key}.{owner}.bin"
            (out / f).write_bytes(struct.pack(f"<{len(verts)}f", *verts))
            rec["files"][str(owner)] = f
            (out / f"units/{key}.{owner}.quad.bin").write_bytes(struct.pack(f"<{len(p['quads'][owner])}f",
                                                                            *p["quads"][owner]))
            rec.setdefault("quadFiles", {})[str(owner)] = f"units/{key}.{owner}.quad.bin"
            rec["verts"] = len(verts) // tascene.UNIT_VSTRIDE
        (out / f"units/{key}.slant.bin").write_bytes(struct.pack(f"<{len(p['slant'])}f", *p["slant"]))
        (out / f"units/{key}.wire.bin").write_bytes(struct.pack(f"<{len(p['wire'])}f", *p["wire"]))
        (out / f"units/{key}.normals.bin").write_bytes(struct.pack(f"<{len(p['normals'])}f", *p["normals"]))
        rec["normalsFile"] = f"units/{key}.normals.bin"
        rec["slantFile"], rec["slantVerts"] = f"units/{key}.slant.bin", len(p["slant"]) // 3
        rec["wireFile"], rec["wireVerts"] = f"units/{key}.wire.bin", len(p["wire"]) // 3
        # the script and its posed order, for the COB host the mockup's server runs (tools/wiki/serve.py)
        shit = eng.open(eng.dirs["scripts"], name, "COB")
        order = list(range(len(p["walk"])))
        if shit:
            (out / f"units/{key}.cob").write_bytes(shit[0])
            posed = posed_order(root, shit[0])
            order = [k for k, _ in posed]
            rec["script"] = {"file": f"units/{key}.cob", "order": order, "visible": [int(v) for _, v in posed],
                             **model_shape(root, order)}
        # the pieces in the order the host's posed model holds them, so a primitive's index is its row and its
        # vertices' piece (tools/wiki/mockup/pose.js composes the script's pose over this table)
        at = {k: i for i, k in enumerate(order)}
        rec["pieces"] = [{**p["walk"][k], "parent": at.get(p["walk"][k]["parent"], -1)} for k in order]
        ids = [at[k] for k in p["piece"] + p["slantPiece"] + p["wirePiece"]]
        (out / f"units/{key}.pieces.bin").write_bytes(struct.pack(f"<{len(ids)}H", *ids))
        rec["pieceFile"] = f"units/{key}.pieces.bin"
        meshes[name] = rec
    return {"atlas": {"w": aw, "h": ah, "file": "units/atlas.r8.bin", "rgba": None, "pad": tascene.UNIT_PAD,
                      "frames": frame_rects},
            "stride": tascene.UNIT_VSTRIDE, "meshes": meshes, "refused": refused, "owners": owners}


# --------------------------------------------------------------------------- the left sidebar

# The bounds the sim's layout holds (tools/wiki/mockup/sim.js checkLayout, tagpu_wikisim.h's WIKILAYOUT)
SIDE_W, SIDE_MIN_H, SIDE_ROWS = 128, 480, 1024
SIDE_SCREENS, SIDE_GADGETS, SIDE_PAGES, SIDE_TYPES = 48, 64, 7, 64
SHEET_W = 1024
MINIMAP_SPAN = 126         # 0x466780: the minimap picture's long side
GUI_UNEXPLORED, GUI_BOX = 0x00, 0x0E     # GUI colour table indices: fog 0x466C20, camera box 0x466B70
# GUI_ButtonDraw 0x4A5F40 and the stage pass's id-1 case 0x4A8B4B read these attribs bits
ATTR_LAST_GREY, ATTR_CHECKBOX, ATTR_SCROLL, ATTR_ONOFF = 0x100, 0x80, 0x1800, 0x4000


def atoi(v):
    """C's atoi, which is how the engine reads every number of a TDF (0x4C46C0): leading blanks, a sign, digits; 0
    when there are none. Retail ARMGEN.GUI's `attribs` value runs on to the next `;` and still reads 256."""
    m = re.match(r"[ \t\r\n]*([+-]?\d+)", v or "")
    return int(m.group(1)) if m else 0


def plain_int(v):
    return v is not None and re.fullmatch(r"[+-]?\d+", v.strip()) is not None


def read_pcx(blob):
    """An 8-bit, one-plane, RLE PCX as (w, h, pixels): the pixels are palette indices and the file's own palette is
    not read. Raises ValueError for any other kind."""
    if len(blob) < 128 or blob[0] != 10 or blob[2] != 1 or blob[3] != 8 or blob[65] != 1:
        raise ValueError("not an 8-bit one-plane RLE PCX")
    x0, y0, x1, y1 = struct.unpack_from("<4H", blob, 4)
    pitch = struct.unpack_from("<H", blob, 66)[0]
    w, h = x1 - x0 + 1, y1 - y0 + 1
    if not (0 < w <= pitch and 0 < h <= 4096):
        raise ValueError(f"a {w} x {h} PCX with a {pitch}-byte line")
    out, pos = bytearray(), 128
    while len(out) < pitch * h:
        if pos >= len(blob):
            raise ValueError("the PCX ends inside its pixels")
        b = blob[pos]
        pos += 1
        if b >= 0xC0:
            if pos >= len(blob):
                raise ValueError("the PCX ends inside a run")
            out += bytes([blob[pos]]) * (b & 0x3F)
            pos += 1
        else:
            out.append(b)
    return w, h, bytes(b for r in range(h) for b in out[r * pitch:r * pitch + w])


class Pixels:
    """A frame the sheet holds: the shape ta3do.gaf_frame gives (width, height, pixels, transparent)."""

    def __init__(self, w, h, pixels, key):
        self.width, self.height, self.pixels, self.transparent = w, h, pixels, key


# ---- the engine's colour tables (the Q2 scout answer; every formula DISASSEMBLED, the retail results MEASURED equal
# ---- to the shipped PALETTE.ALP and to a live read of the GUI table)

def pal_sorted(pal):
    """0x4BA920: each entry's R+G+B and its index, exchange-sorted by the sum with a strict `>` (not a stable order:
    the loop is copied as it is)."""
    s, idx = [sum(c) for c in pal], list(range(256))
    for i in range(256):
        for j in range(i + 1, 256):
            if s[i] > s[j]:
                s[i], s[j] = s[j], s[i]
                idx[i], idx[j] = idx[j], idx[i]
    return s, idx


def pal_nearest(pal, ordered, rgb):
    """0x4BA9D0: the nearest entry by squared RGB distance among those whose sum is within 40 of the colour's,
    walking the sorted list, the first strict minimum; with none in the window, the entry where the walk stopped."""
    s, idx = ordered
    want = sum(rgb)
    best, hit, k = 1000000000, None, 0
    while k < 256:
        if s[k] < want - 40:
            k += 1
            continue
        if s[k] > want + 40:
            break
        c = pal[idx[k]]
        d = (c[0] - rgb[0]) ** 2 + (c[1] - rgb[1]) ** 2 + (c[2] - rgb[2]) ** 2
        if d < best:
            best, hit = d, k
        k += 1
    return idx[hit] if hit is not None else idx[k & 0xFF]


def alp_generate(pal):
    """0x4BA750, which the engine runs only when the install has no palettes\\PALETTE.ALP: T[r][c] = r on the
    diagonal, else the nearest entry to the two colours' per-channel mean (floored)."""
    ordered = pal_sorted(pal)
    t = bytearray(65536)
    for r in range(256):
        for c in range(256):
            t[r * 256 + c] = r if r == c else pal_nearest(pal, ordered, tuple(
                (pal[r][k] + pal[c][k]) >> 1 for k in range(3)))
    return bytes(t)


def grey_table(pal):
    """0x4BAD30: the explored-but-unseen remap the minimap's fog 0x466C20 reads, each entry to the nearest entry to
    its grey, (R+G+B)/3 floored. No file feeds it."""
    ordered = pal_sorted(pal)
    return bytes(pal_nearest(pal, ordered, (v, v, v)) for v in (sum(c) // 3 for c in pal))


GREY_LEVEL = -0x14         # GUI_ButtonDraw's box-shader level over a greyed button (push 0xffffffec at 0x4A69B6)


def greyed_table(grey, shd):
    """The tint GUI_ButtonDraw 0x4A5F40 lays over a greyed button's whole rect once its frame is drawn
    (0x4A699B..0x4A69BA, DISASSEMBLED): the grey remap 0x4BFE10 through globals+0xCC (grey_table, the table the
    minimap's seen fog reads too), then the box shader 0x4BF4D0 at level -20, which reads globals+0xC4, PALETTE.SHD
    (32 rows of 256), at row level + 32 = 12 with a SIGNED index (movsx at 0x4BF5E8), so an index of 0x80..0xFF
    reads the row before. Two remaps of every pixel in a row are one table: T[p] = SHD[(12 - (g >= 128)) * 256 + g]
    with g = grey[p]."""
    row = GREY_LEVEL + 32
    return bytes(shd[(row - (g >= 128)) * 256 + g] for g in grey)


def gui_table(pal, guipal):
    """0x4AC7D0: guipal.PAL's 256 colours matched into the running palette, each to the first entry of least L1
    distance. main+0xDCB, the GUI colour table."""
    out = bytearray(256)
    for i in range(256):
        g = guipal[i]
        best, hit = 0x98967F, 0
        for j, c in enumerate(pal):
            d = abs(g[0] - c[0]) + abs(g[1] - c[1]) + abs(g[2] - c[2])
            if d < best:
                best, hit = d, j
        out[i] = hit
    return bytes(out)


def minimap_geometry(W, H):
    """0x466780: the minimap picture's size and its offset in the 128 x 128 box, for an extent W x H."""
    if W >= H:
        w, h = MINIMAP_SPAN, MINIMAP_SPAN * H // W
        return w, h, 0, (MINIMAP_SPAN - h) // 2
    w, h = MINIMAP_SPAN * W // H, MINIMAP_SPAN
    return w, h, (MINIMAP_SPAN - w) // 2, 0


def minimap_base(desc, tiles, alp):
    """The minimap picture the engine generates for a map with no TNT picture (0x466780): a 2w x 2h surface sampled
    from the tiles, shrunk 2:1 by 0x4B95A0 through the ALP table, out = T[T[a][b]][T[c][d]] with a, b the top pair
    and c, d the bottom pair (first index the row). The extent is the stage map's, not the engine's scroll extent,
    which would cut the stage's bottom rows off (research/notes/ingame-wiki.md, *How it is drawn*). Returns
    (w, h, offx, offy, w*h indices)."""
    mw, mh = desc["cells"]
    W, H = mw * 16, mh * 16
    w, h, offx, offy = minimap_geometry(W, H)
    cols, tmap, n = mw // 2, desc["tilemap"], len(tiles)
    sw, sh = 2 * w, 2 * h
    src = bytearray(sw * sh)
    for Y in range(sh):
        wy = H * Y // sh
        for X in range(sw):
            wx = W * X // sw
            t = tmap[(wy >> 5) * cols + (wx >> 5)]
            src[Y * sw + X] = tiles[t if t < n else 0][(wy & 31) * 32 + (wx & 31)]
    out = bytearray(w * h)
    for y in range(h):
        top, bot = 2 * y * sw, (2 * y + 1) * sw
        for x in range(w):
            a, b, c, d = src[top + 2 * x], src[top + 2 * x + 1], src[bot + 2 * x], src[bot + 2 * x + 1]
            out[y * w + x] = alp[alp[a * 256 + b] * 256 + alp[c * 256 + d]]
    return w, h, offx, offy, bytes(out)


def install_alp(eng, pal):
    """The 2:1 shrink's table as 0x42E140 gets it: palettes\\PALETTE.ALP by the engine's path rule, else the
    generator. The mods' files are not the generator's output (MEASURED, Q2: 508 to 53 378 cells differ), so the
    file is what the game uses. Returns (table, how)."""
    hit = eng.load(eng.dirs["palettes"], eng.dirs["palette"], "ALP")
    if hit and len(hit[0]) >= 65536:
        return hit[0][:65536], {"from": "file", "path": hit[1], "where": hit[2]}
    return alp_generate(pal), {"from": "generated (0x4BA750)", "path": None, "where": None}


# ---- the sidebar's files

class SideError(Exception):
    pass


def gadget_record(sec, index, where, report):
    """One .GUI section as the gadget reader 0x4AD350/0x4ADC70 fills its record: the COMMON keys and the button keys,
    every number read as the engine reads it (atoi) and reported when it is not a plain integer. None for a gadget
    whose size is negative, which the pack refuses."""
    com = tavfs.gadget_common(sec)
    ck, k = (com["keys"] if com else {}), sec["keys"]
    nums = {"id": ck.get("id"), "xpos": ck.get("xpos"), "ypos": ck.get("ypos"), "width": ck.get("width"),
            "height": ck.get("height"), "attribs": ck.get("attribs"), "commonattribs": ck.get("commonattribs"),
            "active": ck.get("active"), "gaffile": ck.get("gaffile"),
            # quickkey is a button key, the gadget's own section's beside grayedout, not a COMMON one (ARMGEN.GUI's
            # ARMRECLAIM: `quickkey=101;` after its [COMMON] block)
            "quickkey": k.get("quickkey"), "grayedout": k.get("grayedout"),
            "stages": k.get("stages")}
    for key, v in nums.items():
        if v is not None and not plain_int(v):
            report.append({"element": f"{where}#{index}", "note": f"{key} {v!r} is not an integer: read as "
                           f"{atoi(v)}, as the engine's atoi reads it"})
    n = {key: atoi(v) for key, v in nums.items()}
    # the name is read into the record's 16 bytes at +2 by 0x4C48C0(.., 0x10), which copies and ends the string at
    # its 16th byte (0x4AD39C, 0x4C4949..0x4C4951): 15 characters
    rec = {"index": index, "id": n["id"], "name": ck.get("name", "")[:NAME_GUI], "x": n["xpos"], "y": n["ypos"],
           "w": n["width"], "h": n["height"], "attribs": n["attribs"], "commonattribs": n["commonattribs"],
           "active": n["active"], "gaffile": n["gaffile"] & 1, "grey": n["grayedout"] & 1, "stages": n["stages"],
           # the record's u8 quickkey +0x13A, the key the button handler 0x4A6AE0 matches through tolower and toupper
           # (0x4A70FB..0x4A7129); the byte of the atoi [INFERRED: the store's width]
           "key": n["quickkey"] & 0xFF,
           "text": k.get("text", ""), "panel": k.get("panel", ""), "filled": False}
    if rec["w"] < 0 or rec["h"] < 0:
        report.append({"element": f"{where}#{index}", "refused": f"a negative size {rec['w']} x {rec['h']}"})
        return None
    return rec


def read_screen(eng, file, report):
    """`<gui>\\<file>.GUI` read by position: (path, where, [gadget record or None]). Raises SideError for a file the
    engine would not load (missing, 0 bytes) or would exit on, and for a first gadget that is no panel."""
    hit = eng.load(eng.dirs["gui"], file, "GUI")
    if hit is None:
        raise SideError(f"no {eng.dirs['gui']}\\{file}.GUI (or 0 bytes)")
    try:
        secs = tavfs.gui_sections(hit[0])
    except tavfs.TdfError as e:
        raise SideError(f"{hit[1]} ({hit[2]}): the engine's parser refuses it: {e}")
    recs = [gadget_record(s, i, file, report) for i, s in enumerate(secs)]
    if not recs or recs[0] is None or recs[0]["id"] != 0:
        raise SideError(f"{hit[1]}: its first gadget is no panel (id 0)")
    report.append({"element": f"screen {file}", "path": hit[1], "where": hit[2], "gadgets": len(recs)})
    return hit[1], hit[2], recs


def read_downloads(eng, report):
    """Every `<download>\\*.tdf` entry in the engine's order (0x42DCF0: the files as 0x4BCA30 lists them, each
    section k entry k): (UNITMENU, MENU, BUTTON, UNITNAME, file). A section without UNITMENU is skipped (0x42DE20);
    MENU and BUTTON are 0 when missing (0x4C46C0's default)."""
    out, empty = [], 0
    for path, where in eng.vfs.enumerate(eng.dirs["download"], "*.tdf", False):
        blob = eng.vfs.read_from(path, where)
        if not blob:
            empty += 1          # the engine's load fails on size 0 (0x4C2F9A) and the file adds nothing
            continue
        try:
            secs = tavfs.tdf_sections(blob)
        except tavfs.TdfError as e:
            report.append({"element": f"download {path}", "refused": f"the engine's parser refuses it: {e}"})
            continue
        for s in secs:
            k = s["keys"]
            if "unitmenu" not in k:
                continue
            # UNITNAME read by 0x4C48C0(.., 0x20) (0x42DED8): 31 characters
            out.append((k["unitmenu"], atoi(k.get("menu")), atoi(k.get("button")), k.get("unitname", "")[:NAME_DL], path))
    report.append({"element": "download", "folder": eng.dirs["download"], "entries": len(out), "empty files": empty})
    return out


def strtod(v):
    """C's strtod over a TDF value, which is how this pack reads the FBI's fractional figures: leading blanks, then
    the longest decimal prefix; 0.0 when there is none (or the key is missing)."""
    m = re.match(r"[ \t\r\n]*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)", v or "")
    return float(m.group(1)) if m else 0.0


# The yard codes of the unit-def loader's parse (0x42CF9D..0x42D06B: the byte map 0x42D198 into the jump table
# 0x42D16C, DISASSEMBLED). 0x8F, the G cell, is the only code with bit 0x80.
YARD_CODES = {".": 0x00, "C": 0x35, "G": 0x8F, "O": 0x2B, "Y": 0x31, "c": 0x2D, "f": 0x6F, "o": 0x2F, "w": 0x37,
              "y": 0x29}
YARD_G = 0x8F
YARD_BUF = 0x400           # GetString 0x4C48C0's buffer for the key (0x42CF3E..0x42CF59): 0x3FF chars and the NUL


def yard_cells(text, fx, fz):
    """The fx * fz yard bytes row by row, as the parse fills def+0x14E: a char of YARD_CODES fills one cell and moves
    on unless the next char ends the string, so a short string repeats its last char; any other char is skipped. Past
    the string every remaining cell takes the last code, or 'o' when the string had none: tagpu_patches.c
    `yard_parse`, which Impure installs at 0x42CF5E and which equals the stock parse for every string that stock
    parses inside its NUL."""
    t = (text or "")[:YARD_BUF - 1]
    cells, k, last = [], 0, None
    while len(cells) < fx * fz:
        if k >= len(t):
            cells.append(last if last is not None else YARD_CODES["o"])
            continue
        code = YARD_CODES.get(t[k])
        if code is None:
            k += 1
            continue
        cells.append(code)
        last = code
        if k + 1 < len(t):
            k += 1
    return cells


CP1252_UNMAPPED = b"\x81\x8d\x8f\x90\x9d"


def display_name(keys, language, unitname):
    """A unit's name as the game shows it: the FBI's `<language>name` when the key exists, else `name` (0x4C58A0 tries
    `<0x51FDC0><key>` first and the plain key second; `name` is read into def+0x00 at 0x42BFAC..0x42BFBB), cut to 31
    bytes because def+0x00 is 0x20 bytes (DISASSEMBLED). The bytes are the game's code page, taken to be Windows-1252
    [INFERRED], and given as UTF-8, a byte with no Windows-1252 character becoming '?'. An empty name gives the
    UnitName. The value comes without its blanks at either end, as tavfs.tdf_sections trims it and as the engine
    holds it: stock ARMCK's `FrenchName=Kbot de construction  ;` is def+0x00 "Kbot de construction" under
    language=french (MEASURED 2026-10-28 in game, M5)."""
    raw = keys.get(f"{language.lower()}name", keys.get("name", "")).encode("latin-1")[:0x1F]
    text = "".join("?" if bytes([b]) in CP1252_UNMAPPED else bytes([b]).decode("cp1252") for b in raw)
    return text or unitname


def unit_record(path, where, k, language):
    """One FBI's UNITINFO keys as the cast rule and the casting read them: integers as the engine's atoi, fractional
    figures as strtod, a missing key 0 except where said."""
    name = k["unitname"].strip()
    fx, fz = atoi(k.get("footprintx")), atoi(k.get("footprintz"))
    # BMcode: a missing key reads 0, a structure (esi zeroed at 0x42C0F0 is pushed as the default at 0x42C401, with
    # no write between; DISASSEMBLED)
    bmcode = atoi(k.get("bmcode"))
    # the yard is parsed only for a def whose BMcode is 0 (0x42CF30..0x42CF38)
    yard = yard_cells(k.get("yardmap", ""), fx, fz) if bmcode == 0 and fx > 0 and fz > 0 else []
    return {"name": name, "fbi": path, "where": where, "bmcode": bmcode, "builder": atoi(k.get("builder")),
            "workertime": strtod(k.get("workertime")), "maxvelocity": strtod(k.get("maxvelocity")),
            "canfly": atoi(k.get("canfly")),
            "minwaterdepth": strtod(k["minwaterdepth"]) if "minwaterdepth" in k else None,
            "fx": fx, "fz": fz, "g": [[i % fx, i // fx] for i, c in enumerate(yard) if c == YARD_G],
            "extractsmetal": strtod(k.get("extractsmetal")), "energymake": strtod(k.get("energymake")),
            "energyuse": strtod(k.get("energyuse")), "weapon1": bool((k.get("weapon1") or "").strip()),
            "fort": (k.get("tedclass") or "").strip().upper() == "FORT",
            "buildtime": strtod(k.get("buildtime")), "builddistance": strtod(k.get("builddistance")),
            # canstop is def+0x245 bit 3, 0 when the key is missing (0x42C90B..0x42C936; the Q4 scout answer)
            "canstop": atoi(k.get("canstop")) & 1,
            # canreclamate is def+0x245 bit 0x400, the key's `and 1` (0x42C9EF..0x42CA1A)
            "canreclamate": atoi(k.get("canreclamate")) & 1,
            # activatewhenbuilt is def+0x241 bit 18, the key's `and 1` (0x42C6B9..0x42C6DE)
            "activatewhenbuilt": atoi(k.get("activatewhenbuilt")) & 1,
            "title": display_name(k, language, name)}


def unit_index(eng, report):
    """Every FBI of the units folder by its UnitName without case (0x488B10 compares def+0x20, the FBI's
    `unitname`, with _stricmp): {name lowered: unit_record}. The FBI is read by the engine's TDF grammar, its first
    UNITINFO section; the first file listed wins a name."""
    out = {}
    for path, where in eng.vfs.enumerate(eng.dirs["units"], "*.fbi", False):
        try:
            blob = eng.vfs.read_from(path, where)
        except (ta3do.HpiError, tavfs.VfsError, struct.error, OSError):
            continue
        if not blob:
            continue
        try:
            secs = tavfs.tdf_sections(blob)
        except tavfs.TdfError as e:
            report.append({"element": f"fbi {path}", "refused": f"the engine's parser refuses it: {e}"})
            continue
        ui = next((sec for sec in secs if sec["_name"].lower() == "unitinfo"), None)
        if ui is None or not (ui["keys"].get("unitname") or "").strip():
            continue
        name = ui["keys"]["unitname"].strip()
        out.setdefault(name.lower(), unit_record(path, where, ui["keys"], eng.language))
    report.append({"element": "units", "folder": eng.dirs["units"], "types": len(out)})
    return out


def read_sides(eng, report):
    """`<gamedata>\\sidedata.tdf`'s [SIDE0], [SIDE1], ... up to the first missing one (0x431A60), the first section
    of a name winning as the engine's lookup 0x4C3410 does: [{name, prefix, intgaf, commander}]."""
    hit = eng.load(eng.dirs["gamedata"], "sidedata", "TDF")
    if hit is None:
        raise SideError(f"no {eng.dirs['gamedata']}\\sidedata.tdf")
    try:
        secs = tavfs.tdf_sections(hit[0])
    except tavfs.TdfError as e:
        raise SideError(f"{hit[1]}: the engine's parser refuses it: {e}")
    sides = []
    while True:
        sec = next((s for s in secs if s["_name"].lower() == f"side{len(sides)}"), None)
        if sec is None:
            break
        k = sec["keys"]
        sides.append({"name": k.get("name", ""), "prefix": k.get("nameprefix", ""), "intgaf": k.get("intgaf", ""),
                      "commander": k.get("commander", "").strip()})
    report.append({"element": "sidedata", "path": hit[1], "where": hit[2], "sides": [s["name"] for s in sides]})
    if not sides:
        raise SideError(f"{hit[1]} has no [SIDE0]")
    return sides


class SideArt:
    """The pictures: banks loaded once, frames collected under a key and packed into one u8 sheet at the end."""

    def __init__(self, eng, report):
        self.eng, self.report = eng, report
        self.banks, self.frames = {}, {}
        got = eng.gaf(eng.dirs["anims"], eng.dirs["commongui"])
        self.common = got
        if got is None:
            report.append({"element": "commongui", "refused": f"no {eng.dirs['anims']}\\{eng.dirs['commongui']}.GAF"})

    def bank(self, name):
        if name.lower() not in self.banks:
            self.banks[name.lower()] = self.eng.gaf(self.eng.dirs["anims"], name)
        return self.banks[name.lower()]

    def seq(self, got, name):
        """[(frame key, Pixels, hx, hy)] of sequence `name` in bank `got` (first of the name without case,
        0x4B8D40), each frame as the blit draws it (Gaf.drawn), or None."""
        if got is None or name.lower() not in got[0].seqs:
            return None
        gaf, path, where = got
        out = []
        for k in range(len(gaf.seqs[name.lower()][1])):
            key = f"{path.lower()}:{where}:{name.upper()}:{k}"
            fr, hx, hy, outside = gaf.drawn(name, k)
            if outside and key not in self.frames:
                self.report.append({"element": f"{path}:{name.upper()}#{k}", "note": f"{outside} subframe pixels "
                                    f"fall outside the frame's {fr.width} x {fr.height} box and are left out"})
            self.frames.setdefault(key, fr)
            out.append((key, fr, hx, hy))
        return out

    def add(self, key, pix):
        self.frames.setdefault(key, pix)
        return key

    def pack(self):
        place, h = tascene.shelf_pack(self.frames, width=SHEET_W)
        h = max(1, h)
        sheet = bytearray(SHEET_W * h)
        for key, (x, y) in place.items():
            f = self.frames[key]
            for r in range(f.height):
                o = (y + r) * SHEET_W + x
                sheet[o:o + f.width] = f.pixels[r * f.width:(r + 1) * f.width]
        return place, h, bytes(sheet)


def button_frames(n, base, attribs):
    """The frames GUI_ButtonDraw 0x4A5F40 shows at rest, pressed and greyed: base; base + status (1); attribs 0x100
    the last frame, else min(status + 2, n − 1) + base, under the greyed tint (greyed_table) unless attribs 0x100 or
    0x80. Clipped to the sequence. A one-frame sequence gives one: greyed, the engine draws it under the tint like
    any other; pressed, frame 1 is past the sequence, 0x4B7F30 returns no frame and the engine draws no picture
    (0x4A6172, DISASSEMBLED), where the stage keeps the section 2.3 interim (frame 0 moved one pixel)."""
    if n <= 1:
        return [0]
    last = n - 1
    grey = last if attribs & ATTR_LAST_GREY else min(min(2, last) + base, last)
    return [min(base, last), min(base + 1, last), grey]


def engine_picture(art, g, bank_name):
    """The stage pass's own resolution of a button (0x4A8B4B): its name in the screen's bank, then in commongui
    (base 0); else commongui's generic picture (CHECKBOX for attribs 0x80; stagebuttn1 for Off|On, one stage or
    attribs 0x4000; stagebuttn<min(stages, 4)> for other stages; BUTTONS0), whose base is the frame (0, 4, 8, ...)
    nearest the gadget's size in |dw| + |dh|. Returns (how, path, where, sequence, frames, base) or None."""
    for how, got in (("page", art.bank(bank_name)), ("commongui", art.common)):
        fr = art.seq(got, g["name"]) if g["name"] else None
        if fr:
            return how, got[1], got[2], g["name"].upper(), fr, 0
    if art.common is None:
        return None
    if g["attribs"] & ATTR_CHECKBOX:
        seq = "CHECKBOX"
    elif g["text"] == "Off|On" or g["stages"] == 1 or g["attribs"] & ATTR_ONOFF:
        seq = "stagebuttn1"
    elif g["stages"]:
        seq = f"stagebuttn{min(g['stages'], 4)}"
    else:
        seq = "BUTTONS0"
    fr = art.seq(art.common, seq)
    if not fr:
        return None
    best, base = None, 0
    for k in range(0, len(fr), 4):
        d = abs(g["w"] - fr[k][1].width) + abs(g["h"] - fr[k][1].height)
        if best is None or d < best:
            best, base = d, k
    return "fallback", art.common[1], art.common[2], seq.upper(), fr, base


def gadget_picture(art, g, role, unit, bank_name, where, report):
    """The picture of one button by the section 2.3 rule (decision 5), and the rect the engine gives it.

    Step 1 is the engine's: a slot the download fill wrote, or a gadget whose COMMON says `gaffile=1` (both set +0xB4
    bit 0: 0x41AEAE, 0x4AD4C1), takes `anims\\<NAME>_gadget.GAF`'s sequence named <NAME>, with no fallback
    (0x4A83E2..0x4A84D8, the Q3 scout answer); any other takes its name in the screen's bank, then in commongui. For
    an order, tab or pager the engine's generic commongui picture is also step 1 (it is what the game draws, with
    the gadget's text on it). For a build button it is not: decision 5 goes on to 2. the unit's download picture,
    3. `<unitpics>\\<UNIT>.PCX`, its pixels taken as palette indices as an 8-bit blit takes them [INFERRED; capture
    C2 settles it], 4. a labelled button.

    The rect: the engine resizes a button its stage pass pictured to that frame's size (0x4A8DCB..0x4A8DF3); a slot
    the download fill wrote keeps the size its first name was given; a `gaffile` gadget and a scroll arrow keep the
    file's. A build button that only the generic picture would have pictured keeps the file's size too: the stock
    engine would shrink it to a BUTTONS0 frame (96 x 20 for 64 x 64), which is not what any mod that draws its own
    build pictures shows [INFERRED; capture C2 settles it]."""
    rect = [g["x"], g["y"], g["w"], g["h"]]
    btn = lambda fr, base: [fr[i][0] for i in button_frames(len(fr), base, g["attribs"])]
    pic = None
    first = None if g["attribs"] & ATTR_SCROLL or g["gaffile"] else engine_picture(art, g, bank_name)
    if g["filled"]:
        orig = engine_picture(art, {**g, "name": g["orig"]}, bank_name) if not g["attribs"] & ATTR_SCROLL else None
        if orig and orig[0] != "fallback":
            f0 = orig[4][orig[5]][1]
            rect = [g["x"], g["y"], f0.width, f0.height]
        first = None
    if g["filled"] or g["gaffile"]:
        got = art.bank(f"{g['name']}_gadget")
        fr = art.seq(got, g["name"])
        if fr:
            pic = {"from": "download", "frames": btn(fr, 0), "path": got[1], "where": got[2], "seq": g["name"].upper()}
    elif first and (first[0] != "fallback" or role != "build"):
        how, path, w, seq, fr, base = first
        pic = {"from": how, "frames": btn(fr, base), "path": path, "where": w, "seq": seq}
        if how == "fallback":
            pic["text"] = g["text"]
        f0 = fr[base][1]
        rect = [g["x"], g["y"], f0.width, f0.height]
    if pic is None and role == "build" and unit:
        got = art.bank(f"{unit}_gadget")
        fr = art.seq(got, unit)
        if fr:
            pic = {"from": "download", "frames": btn(fr, 0), "path": got[1], "where": got[2], "seq": unit}
        else:
            hit = art.eng.load(art.eng.dirs["unitpics"], unit, "PCX")
            if hit:
                try:
                    w, h, px = read_pcx(hit[0])
                    key = art.add(f"{hit[1].lower()}:{hit[2]}:pcx", Pixels(w, h, px, None))
                    pic = {"from": "unitpics", "frames": [key], "path": hit[1], "where": hit[2]}
                except ValueError as e:
                    report.append({"element": f"{where}/{g['name']}", "note": f"{hit[1]}: {e}"})
    if pic is None:
        pic = {"from": "label", "text": unit or g["name"]}
    return pic, rect


# The names a page's buttons are read and pressed by (DISASSEMBLED): a .GUI gadget's name is 15 characters
# (0x4AD39C), a download entry's 31 (0x42DED8), and the fill copies the entry's whole name into the record (strcpy,
# 0x41AE84..0x41AEAC); a press reads the record's name cut to 16 characters (0x49FED0: strncpy 0x10, [0x10] = 0).
NAME_GUI, NAME_DL, NAME_PRESS = 15, 31, 16


def gadget_role(g, units):
    """The role the build pages' handler 0x41AA00 gives a press of the button, in its order: the name cut to 16
    characters (0x49FED0), then the case-sensitive substrings PREV, NEXT, ORDERS, BUILD of that cut name (strstr,
    0x41AA75..0x41AB30) -- a pager or tab whatever else it names -- then a unit type by that cut name (0x488B10 at
    0x41AA39, tested at 0x41AB5D; the press arms it, or queues a mobile one), whatever the commonattribs; then
    commonattribs bit 2 (a build button whose name is no type) and STOP; any other button is an order. Returns (role,
    unit or None)."""
    press = g["name"][:NAME_PRESS]
    for sub, role in (("PREV", "prev"), ("NEXT", "next"), ("ORDERS", "orders"), ("BUILD", "buildtab")):
        if sub in press:
            return role, None
    if press.lower() in units:
        return "build", units[press.lower()]["name"].upper()
    if g["commonattribs"] & 4:
        return "build", None
    return ("stop" if "STOP" in g["name"] else "order"), None


def button_state(g, units, page, hide_pagers, nrec):
    """A button as the layout holds it: (role, unit, grey, hidden). `page` is the build page number or None for a
    side's screen, `nrec` the page's record count (its sections); `hide_pagers` hides PREV and NEXT on a type with no
    numbered page. Its grey is the file's (after the download fill), but for the greyed pass 0x41B045..0x41B096,
    which runs on a numbered page only, over records 0 .. N-1 with N the header's +0xB6 = the sections less one
    (0x41B054, 0x41B094): a record of commonattribs bit 2 there is greyed exactly when its whole name is no type
    (0x488B10 at 0x41B07A, 0x4A1200 writes record i), so the last record keeps the file's grey."""
    role, unit = gadget_role(g, units)
    if page and g["index"] < nrec - 1 and g["commonattribs"] & 4:
        grey = g["name"].lower() not in units
    else:
        grey = bool(g["grey"])
    hidden = not g["active"] or (hide_pagers and role in ("prev", "next"))
    return role, unit, grey, hidden


def panel_picture(art, header, bank_name):
    """The panel's picture (the id-0 stage case 0x4A84F2..0x4A8658): `panel=` in the screen's bank, then commongui,
    drawn once at its origin minus its hotspot (0x4B0230 with one frame); else commongui BackTile with its hotspots
    zeroed, drawn once when it has one frame and as a 3 x 3 nine-slice over the panel when it has more (0x4B0344..
    0x4B0439), which is composed here into one frame. None when nothing resolves (the engine fills the panel with a
    GUI colour this pack does not carry)."""
    name = header["panel"]
    for how, got in (("page", art.bank(bank_name)), ("commongui", art.common)):
        fr = art.seq(got, name) if name else None
        if fr:
            key, f, hx, hy = fr[0]
            return {"f": key, "hot": [hx, hy], "key": f.transparent, "from": f"{how} {name.upper()}",
                    "path": got[1], "where": got[2]}
    tiles = art.seq(art.common, "BackTile")
    if not tiles:
        return None
    if len(tiles) == 1:
        key, f, _hx, _hy = tiles[0]
        return {"f": key, "hot": [0, 0], "key": f.transparent, "from": "commongui BACKTILE",
                "path": art.common[1], "where": art.common[2]}
    W, H = header["w"], header["h"]
    tw, th = tiles[0][1].width, tiles[0][1].height
    keyc = tiles[0][1].transparent if tiles[0][1].transparent is not None else 0
    px = bytearray([keyc]) * (W * H)
    rows = list(range(0, max(1, H - th) + 1, th)) if th else [0]
    if rows[-1] + th < H:
        rows.append(H - th)
    cols = list(range(0, max(1, W - tw) + 1, tw)) if tw else [0]
    if cols[-1] + tw < W:
        cols.append(W - tw)
    for ri, y in enumerate(rows):
        rc = 0 if ri == 0 else 6 if ri == len(rows) - 1 else 3
        for ci, x in enumerate(cols):
            cc = 0 if ci == 0 else 2 if ci == len(cols) - 1 else 1
            f = tiles[min(rc + cc, len(tiles) - 1)][1]
            for r in range(f.height):
                if not 0 <= y + r < H:
                    continue
                for c in range(f.width):
                    v = f.pixels[r * f.width + c]
                    if v != f.transparent and 0 <= x + c < W:
                        px[(y + r) * W + x + c] = v
    key = art.add(f"backtile:{W}x{H}", Pixels(W, H, bytes(px), keyc))
    return {"f": key, "hot": [0, 0], "key": keyc, "from": "commongui BACKTILE nine-slice",
            "path": art.common[1], "where": art.common[2]}


def check_panel(name, header):
    """Raises SideError for a screen whose panel the column cannot hold: the layout leaves such a screen out."""
    if not (1 <= header["w"] <= SIDE_W and header["h"] >= 1 and header["y"] >= 0 and header["y"] + header["h"] <= SIDE_ROWS):
        raise SideError(f"{name}: a {header['w']} x {header['h']} panel at row {header['y']} is outside the column")


def build_screen(art, name, file, path, where, recs, units, page, hide_pagers, report):
    """One screen of the layout and its art: (layout screen, art screen). Gadgets are the buttons (id 1) in file
    order; the panel (id 0), labels (id 5) and fonts (id 7) are the art's. `page` is the build page number (the
    greyed pass runs on page != 0) or None for a side's screen."""
    header = recs[0]
    if header["x"] != 0:
        report.append({"element": f"screen {name}", "note": f"the panel is at x {header['x']}; drawn at 0"})
    check_panel(name, header)
    gadgets, pics, labels = [], [], []
    for g in recs[1:]:
        if g is None:
            continue
        if g["id"] == 5:
            labels.append({"rect": [g["x"], g["y"], g["w"], g["h"]], "text": g["text"]})
            continue
        if g["id"] != 1:
            continue
        role, unit, grey, hidden = button_state(g, units, page, hide_pagers, len(recs))
        pic, rect = gadget_picture(art, g, role, unit, file, f"{name}#{g['index']}", report)
        x, y, w, h = rect
        cw, ch = min(x + w, header["w"]) - max(x, 0), min(y + h, header["h"]) - max(y, 0)
        if cw < 1 or ch < 1:
            report.append({"element": f"{name}/{g['name']}#{g['index']}", "refused": f"the rect {rect} is outside "
                           f"the {header['w']} x {header['h']} screen"})
            continue
        if [max(x, 0), max(y, 0), cw, ch] != rect:
            report.append({"element": f"{name}/{g['name']}#{g['index']}", "note": f"the rect {rect} is clipped "
                           f"to the screen"})
            rect = [max(x, 0), max(y, 0), cw, ch]
        # SIDE_GADGETS: the layout's per-screen gadget table; a button past it is left out, not the screen
        if len(gadgets) == SIDE_GADGETS:
            report.append({"element": f"{name}/{g['name']}#{g['index']}", "refused": f"past {SIDE_GADGETS} buttons"})
            continue
        lg = {"name": g["name"], "role": role, "rect": rect}
        if role == "build":
            lg["unit"] = unit
        lg["grey"] = grey
        if g["key"]:
            lg["key"] = g["key"]
        if hidden:
            lg["hidden"] = True
        gadgets.append(lg)
        pics.append({**pic, "attribs": g["attribs"]})
        report.append({"element": f"{name}/{g['name']}#{g['index']}", "role": role, "from": pic["from"],
                       "path": pic.get("path"), "where": pic.get("where"), "seq": pic.get("seq")})
    panel = panel_picture(art, header, file)
    report.append({"element": f"{name}/panel", "from": panel["from"] if panel else None,
                   "path": panel and panel["path"], "where": panel and panel["where"]})
    lay = {"y": header["y"], "w": header["w"], "h": header["h"], "gadgets": gadgets}
    return lay, {"file": file, "path": path, "where": where, "panel": panel, "pics": pics, "labels": labels}


def type_pages(eng, tname, downloads, player_prefix, report):
    """The def load's page count (0x42D800..0x42D8CB, 0x42DF60..0x42DFB4) and each page's file: [(page, file)]
    for pages 0..last, page 0 only when `<NAME>0.GUI` exists. def+0x22E is the first missing index of the `<NAME>1,
    2, ...` probe (or 1 with only `<NAME>0`), raised to every download entry's MENU, a byte; the numbered pages are
    1 .. def+0x22E − 1. A page whose file is missing is the PLAYER's side's `<prefix>DL` (0x41AD61..0x41AD94 takes
    the prefix through the unit's owner, PlayerInfo+0x95). Returns (pages, entries of this type) or None when the
    type has no page."""
    zero = eng.load(eng.dirs["gui"], f"{tname}0", "GUI") is not None
    n = 1
    while eng.load(eng.dirs["gui"], f"{tname}{n}", "GUI") is not None and n < 256:
        n += 1
    count = n if n > 1 else (1 if zero else 0)
    mine = [d for d in downloads if d[0].lower() == tname.lower()]
    for d in mine:
        count = max(count, d[1] & 0xFF)
    last = count - 1 if count >= 2 else 0
    if count == 0 or (last == 0 and not zero):
        return None
    if last > SIDE_PAGES:
        # the page is unit+0x110 bits 23..25: a type with more numbered pages than the field holds is refused here,
        # and the router then shows GEN for it
        report.append({"element": f"type {tname}", "refused": f"{last} numbered pages, more than {SIDE_PAGES}"})
        return None
    pages = [(0, f"{tname}0")] if zero else []
    for k in range(1, last + 1):
        own = eng.load(eng.dirs["gui"], f"{tname}{k}", "GUI") is not None
        pages.append((k, f"{tname}{k}" if own else f"{player_prefix}DL"))
    return pages, mine


def fill_downloads(recs, page, entries, name, report):
    """0x41ACE0's fill: each entry with MENU − 1 == page (MENU zero-extended from its byte, 0x41AE48) writes gadget
    BUTTON + 4 (BUTTON zero-extended from its byte too: stored at 0x42DEE7, read at 0x41AE57): the unit's name, commonattribs 4, not greyed, the `_gadget` picture. Later entries overwrite. The
    engine never bounds BUTTON + 4 against the gadget count (it writes past the screen's gadgets); here an entry
    whose slot is no gadget of the page is skipped and reported."""
    for unitmenu, menu, button, unitname, path in entries:
        if (menu & 0xFF) - 1 != page:
            continue
        slot = (button & 0xFF) + 4
        if not 0 <= slot < len(recs) or recs[slot] is None:
            report.append({"element": f"{name} download {unitname}", "refused": f"{path}: BUTTON {button} names "
                           f"gadget {slot}, which the page (of {len(recs)}) does not have"})
            continue
        g = recs[slot]
        g.update(orig=g["name"], name=unitname, commonattribs=4, grey=0, filled=True)
        report.append({"element": f"{name} download {unitname}", "slot": slot, "from": path})


# ---- the cast rule: each side's own units for the casts a wiki demo plays
#
# research/notes/ingame-wiki.md, the owner's decisions 14 and 20. A unit's place in a demo is its CAST; a side's answer
# to every cast is its CASTING. The rule names no mod, lists no unit and reads no unit's `Side=`: it walks a side's
# build menus breadth-first from the side's commander, so tech level ranks first and the mod's own menu order second,
# and it tests each candidate's own FBI figures. What pins it: on the stock game it gives ARM exactly the types the
# wiki's entries author, cast for cast and figure for figure (sidepacks.py `stock_identity`, run on every stock pack).
#
# Every cast's unit is distinct, since one unit in two casts binds two of an entry's defs to one type, which the
# binder refuses for every demo of the entry: a cast skips a unit already picked for an earlier cast in CASTS' order.
# tower, wall and solar, the last three, are drawn from one list and skip in that order (a tower that makes energy
# is the tower, not the solar too). The other casts' tests already exclude one another (a builder or the walker is
# mobile, an extractor, a geothermal plant and a plain building each fail the others' tests, the moho is stronger
# than the mex, the advbuilder is not the builder), and the last pass makes it a fact whatever the tests: a pick that
# repeats an earlier cast's is left unbound, with the reason. The DLL's loader (tagpu_wikiasset.c) does the same.

CASTS = ("builder", "advbuilder", "mex", "moho", "geo", "walker", "tower", "wall", "solar")
CAST_FP_MAX = 16           # a footprint's side on each axis that the casting holds (WIKI_FP_MAX: one G row a u16)


def is_mobile(u):
    return u["bmcode"] != 0 and u["maxvelocity"] > 0


def is_structure(u):
    return u["bmcode"] == 0 and u["fx"] > 0 and u["fz"] > 0


def is_builder(u):
    return bool(u["builder"] & 1) and u["workertime"] > 0


def is_land(u):
    """Neither a flyer nor bound to water; an FBI without MinWaterDepth is a land unit."""
    return not u["canfly"] and (u["minwaterdepth"] is None or u["minwaterdepth"] <= 0)


def is_extractor(u):
    return is_structure(u) and u["extractsmetal"] > 0


def is_geothermal(u):
    """A G cell in the yard is what ties a building to a vent: 0x47D2E0 refuses a yard holding G unless some G cell
    stands on a geothermal feature (0x47D684..0x47D711, refusal 0x47D80F; DISASSEMBLED)."""
    return is_structure(u) and not is_extractor(u) and bool(u["g"])


def is_plain(u):
    """A land building that is neither a resource building nor a builder: a tower, a wall or a power plant."""
    return is_structure(u) and is_land(u) and not is_extractor(u) and not is_geothermal(u) and not u["builder"] & 1


def net_energy(u):
    """What the building adds to the energy income: a solar collector's energy is a negative EnergyUse (stock ARMSOLAR
    energyuse -20, energymake 0)."""
    return u["energymake"] - u["energyuse"]


def menu_of(eng, units, downloads, tname, prefix, cache):
    """menu(T): the units of T's build buttons on its numbered pages as the layout reads them for the side whose
    prefix is `prefix` (type_pages' probe, else `<prefix>DL`, with the download fill and the greyed pass), in (page,
    gadget) order, a unit already listed keeping its first place; hidden and greyed buttons left out. No picture is
    composed. A page the layout would refuse leaves T with no menu, as the layout then holds no page of T. Returns
    [UnitName lowered], cached per (type, prefix)."""
    key = (tname.lower(), prefix.lower())
    if key in cache:
        return cache[key]
    out = []
    got = type_pages(eng, units[tname.lower()]["name"], downloads, prefix, [])
    if got:
        pages, mine = got
        try:
            for page, file in pages:
                if not page:
                    continue
                _path, _where, recs = read_screen(eng, file, [])
                check_panel(file, recs[0])
                fill_downloads(recs, page, mine, file, [])
                for g in recs[1:]:
                    if g is None or g["id"] != 1:
                        continue
                    role, unit, grey, hidden = button_state(g, units, page, False, len(recs))
                    if role == "build" and unit and not grey and not hidden and unit.lower() not in out:
                        out.append(unit.lower())
        except SideError:
            out = []
    cache[key] = out
    return out


def side_walk(root, menu):
    """Breadth-first from `root` through menu(): the units in the order they are first met (root first, at depth 0)
    and each one's depth."""
    order, depth = [root], {root: 0}
    for t in order:
        for x in menu(t):
            if x not in depth:
                depth[x] = depth[t] + 1
                order.append(x)
    return order, depth


def strongest(cands, value):
    """The candidate of the greatest value, the earlier one on an equal value."""
    best = None
    for t in cands:
        if best is None or value(t) > value(best):
            best = t
    return best


def side_casting(units, commander, menu):
    """One side's casting by the rule above: ({cast: UnitName lowered or None}, [{cast, type, why}]). `menu(t)` is
    menu_of for this side."""
    picks = dict.fromkeys(CASTS)
    why = {}
    root = commander.lower()
    if root not in units:
        for c in CASTS:
            why[c] = f"the side's commander {commander or '(none)'} has no FBI"
        return picks, [{"cast": c, "type": None, "why": why[c]} for c in CASTS]
    order, depth = side_walk(root, menu)
    U = units
    land_mex = lambda x: is_extractor(U[x]) and is_land(U[x])
    mobile_builder = lambda t: is_mobile(U[t]) and is_builder(U[t]) and is_land(U[t])

    b = next((t for t in order[1:] if mobile_builder(t) and any(land_mex(x) for x in menu(t))), None)
    picks["builder"] = b
    why["builder"] = (f"the first met (depth {depth[b]}) mobile land builder whose menu holds a land extractor" if b
                      else "no mobile land builder below the commander has a land extractor on its menu")
    if b:
        mb = menu(b)
        mx = strongest([x for x in mb if land_mex(x)], lambda x: U[x]["extractsmetal"])
        picks["mex"] = mx
        why["mex"] = f"the strongest land extractor on the builder's menu (extractsmetal {U[mx]['extractsmetal']:g})"
        geo = strongest([x for x in mb if is_geothermal(U[x]) and is_land(U[x])], lambda x: net_energy(U[x]))
        picks["geo"] = geo
        why["geo"] = (f"the land geothermal plant of the greatest net energy ({net_energy(U[geo]):g}) on the builder's "
                      "menu" if geo else "no land geothermal plant on the builder's menu")
        top = U[mx]["extractsmetal"]
        stronger = lambda x: land_mex(x) and U[x]["extractsmetal"] > top
        a = next((t for t in order[1:] if t != b and mobile_builder(t) and any(stronger(x) for x in menu(t))), None)
        picks["advbuilder"] = a
        why["advbuilder"] = (f"the first met (depth {depth[a]}) mobile land builder, after the builder, whose menu "
                             "holds a land extractor stronger than the mex" if a else
                             "no other mobile land builder has a land extractor stronger than the mex on its menu")
        if a:
            mo = strongest([x for x in menu(a) if stronger(x)], lambda x: U[x]["extractsmetal"])
            picks["moho"] = mo
            why["moho"] = f"the strongest land extractor on the advbuilder's menu (extractsmetal {U[mo]['extractsmetal']:g})"
        else:
            why["moho"] = "no advbuilder"
        maker = next((t for t in order if b in menu(t)), None)
        w = next((x for x in menu(maker) if is_mobile(U[x]) and not is_builder(U[x]) and is_land(U[x])), None)
        picks["walker"] = w
        why["walker"] = (f"the first mobile land non-builder on the menu of {U[maker]['name']}, the first met unit that "
                         "builds the builder" if w else
                         f"no mobile land non-builder on the menu of {U[maker]['name']}, the first met unit that builds "
                         "the builder")
        # the last three casts share one list: each skips a unit an earlier cast took
        plain = [x for x in mb if is_plain(U[x])]
        for c, test, what in (("tower", lambda x: U[x]["weapon1"], "with a Weapon1"),
                              ("wall", lambda x: not U[x]["weapon1"] and U[x]["fort"], "with no Weapon1 and TEDClass FORT"),
                              ("solar", lambda x: net_energy(U[x]) > 0, "of a positive net energy")):
            picks[c] = next((x for x in plain if test(x) and x not in picks.values()), None)
            why[c] = (f"the first plain land building on the builder's menu {what}" if picks[c] else
                      f"no plain land building on the builder's menu {what}")
    else:
        for c in CASTS[1:]:
            why[c] = "no builder"
    for i, c in enumerate(CASTS):
        first = next((d for d in CASTS[:i] if picks[c] and picks[d] == picks[c]), None)
        if first:
            why[c] = f"{U[picks[c]]['name']} is already the {first}"
            picks[c] = None
    for c in CASTS:
        t = picks[c]
        if t and not (1 <= U[t]["fx"] <= CAST_FP_MAX and 1 <= U[t]["fz"] <= CAST_FP_MAX):
            why[c] = f"{U[t]['name']}'s footprint {U[t]['fx']} x {U[t]['fz']} is not within 1..{CAST_FP_MAX} a side"
            picks[c] = None
    return picks, [{"cast": c, "type": U[picks[c]]["name"].upper() if picks[c] else None, "why": why[c]}
                   for c in CASTS]


def cast_record(u, builds):
    """A bound cast as the casting carries it: the entry's own keys (art/wiki/entries), the type in the case the
    layout's build buttons spell it, and `builds` naming casts, not types."""
    return {"type": u["name"].upper(), "title": u["title"], "footprint": [u["fx"], u["fz"]],
            "buildTime": u["buildtime"], "workerTime": u["workertime"], "buildDistance": u["builddistance"],
            "maxVelocity": u["maxvelocity"], "canstop": u["canstop"], "canreclamate": u["canreclamate"],
            "activatewhenbuilt": u["activatewhenbuilt"], "extracts": u["extractsmetal"],
            "energyMake": u["energymake"], "vent": bool(u["g"]), "g": u["g"], "builds": builds}


def side_pack(eng, mod, descs, out, pal, report):
    """The sidebar pack of one install (research/notes/ingame-wiki.md, *The data format*): side.json, sheet.bin,
    pal.bin, seen.bin, greyed.bin and the minimap bases. Every side with MAIN2 and GEN screens is a layout side, with
    its casting and the pages of its cast types. Returns the side.json object; raises SideError when no side has its
    screens."""
    units = unit_index(eng, report)
    sides = read_sides(eng, report)
    downloads = read_downloads(eng, report)
    art = SideArt(eng, report)
    screens, art_screens, lay_sides, kept = {}, {}, [], []

    def add_screen(name, file, page, hide, recs_fill=None):
        path, where, recs = read_screen(eng, file, report)
        if recs_fill:
            recs_fill(recs)
        lay, a = build_screen(art, name, file, path, where, recs, units, page, hide, report)
        screens[name], art_screens[name] = lay, a

    for i, s in enumerate(sides):
        try:
            add_screen(f"{s['prefix']}MAIN2", f"{s['prefix']}MAIN2", None, False)
            add_screen(f"{s['prefix']}GEN", f"{s['prefix']}GEN", None, False)
        except SideError as e:
            report.append({"element": f"side {s['name']}", "refused": str(e)})
            continue
        kept.append(s)
        lay_sides.append({"name": s["name"], "prefix": s["prefix"], "main2": f"{s['prefix']}MAIN2",
                          "gen": f"{s['prefix']}GEN", "types": {}})
    if not lay_sides:
        raise SideError("no side has MAIN2 and GEN screens")

    menus, casting = {}, []
    for s in kept:
        menu = lambda t, prefix=s["prefix"]: menu_of(eng, units, downloads, t, prefix, menus)
        picks, why = side_casting(units, s["commander"], menu)
        casts = {}
        for c in CASTS:
            t = picks[c]
            casts[c] = cast_record(units[t], [d for d in CASTS if picks[d] and picks[d] in menu(t)]) if t else None
        casting.append({"side": s["name"], "commander": s["commander"], "casts": casts, "report": why})

    # each side's types: its bound cast types that have a page, read with that side's prefix (the `<prefix>DL` page
    # follows the PLAYER's side, 0x41AD6C, and the tutorial plays the player's side)
    made, ntypes = {}, 0
    for k, (s, cs) in enumerate(zip(kept, casting)):
        want = []
        for T in dict.fromkeys(rec["type"] for rec in cs["casts"].values() if rec):
            u = units[T.lower()]
            got = type_pages(eng, u["name"], downloads, s["prefix"], report)
            if got is None:
                report.append({"element": f"type {T} ({s['name']})", "note": "no page: the router shows GEN for it"})
                continue
            want.append((T, u, got))
        # SIDE_TYPES: the layout's type table over all sides; a side whose types would pass it keeps none
        if ntypes + len(want) > SIDE_TYPES:
            report.append({"element": f"side {s['name']} types", "refused": f"its {len(want)} types pass "
                           f"{SIDE_TYPES} over all sides"})
            continue
        for T, u, (pages, mine) in want:
            numbered = [p for p, _f in pages if p]
            if made.get(T, (None,))[0] == pages:
                # the same files as another side's: the same screens
                lay_sides[k]["types"][T] = made[T][1]
                ntypes += 1
                continue
            tag = "" if T not in made else f"@{k}"
            names = {p: f"{T}{p}{tag}" for p, _f in pages}
            if any(len(n.encode()) >= 32 for n in names.values()):
                report.append({"element": f"type {T} ({s['name']})", "refused": "a page's screen name passes 31 bytes"})
                continue
            # SIDE_SCREENS: the layout's screen table; a type that would pass it is left out whole
            if len(screens) + len(pages) > SIDE_SCREENS:
                report.append({"element": f"type {T} ({s['name']})", "refused": f"its {len(pages)} pages pass "
                               f"{SIDE_SCREENS} screens"})
                continue
            done = []
            try:
                for p, file in pages:
                    add_screen(names[p], file, p, not numbered,
                               lambda recs, p=p, name=names[p]: fill_downloads(recs, p, mine, name, report))
                    done.append(names[p])
            except SideError as e:
                for name in done:
                    screens.pop(name)
                    art_screens.pop(name)
                report.append({"element": f"type {T} ({s['name']})", "refused": str(e)})
                continue
            entry = {"pages": [names[p] for p in numbered], "orders": names[0] if pages[0][0] == 0 else None}
            made.setdefault(T, (pages, entry))
            lay_sides[k]["types"][T] = entry
            ntypes += 1
            report.append({"element": f"type {T} ({s['name']})", "pages": [f for _p, f in pages]})
        # a cast the rule took from a menu that the layout shows no live button for would lock its demo's build trip
        for c in CASTS:
            rec = cs["casts"][c]
            entry = rec and lay_sides[k]["types"].get(rec["type"])
            if not entry:
                continue
            for d in rec["builds"]:
                unit = cs["casts"][d]["type"]
                if not any(g["role"] == "build" and g.get("unit") == unit and not g["grey"] and not g.get("hidden")
                           for n in entry["pages"] for g in screens[n]["gadgets"]):
                    report.append({"element": f"casting {s['name']}", "note": f"the {c} {rec['type']}'s pages show "
                                   f"no live button for the {d} {unit}"})
    height = max([SIDE_MIN_H] + [s["y"] + s["h"] for s in screens.values()])
    layout = {"format": 1, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, never committed",
              "width": SIDE_W, "height": height, "sides": lay_sides, "screens": screens}

    # PANELSIDE of every layout side, and the dots
    side_art = {"panelside": []}
    for s in kept:
        got = art.bank(s["intgaf"]) if s["intgaf"] else None
        fr = art.seq(got, "PANELSIDE")
        side_art["panelside"].append({"f": fr[0][0], "hot": [fr[0][2], fr[0][3]], "key": fr[0][1].transparent}
                                     if fr else None)
        report.append({"element": f"panelside {s['name']}", "from": got and got[1], "where": got and got[2],
                       **({} if fr else {"refused": f"no PANELSIDE in {eng.dirs['anims']}\\{s['intgaf']}.GAF"})})
    fx = art.bank("fx")
    rad, high = art.seq(fx, "radlogo"), art.seq(fx, "radlogohigh")
    side_art["dots"] = {"radlogo": [(k, hx, hy) for k, _f, hx, hy in rad] if rad else None,
                        "high": (high[0][0], high[0][2], high[0][3]) if high else None,
                        "key": (rad or high)[0][1].transparent if (rad or high) else 0}
    report.append({"element": "dots", "from": fx and fx[1], "where": fx and fx[2],
                   "radlogo": len(rad) if rad else None, "high": bool(high)})

    # the minimap's tables
    hit = eng.load(eng.dirs["palettes"], eng.dirs["guipal"], "PAL")
    if hit and len(hit[0]) >= 1024:
        gui = gui_table(pal, [(hit[0][i * 4], hit[0][i * 4 + 1], hit[0][i * 4 + 2]) for i in range(256)])
        gui_from = {"path": hit[1], "where": hit[2]}
    else:
        gui, gui_from = None, {"refused": f"no {eng.dirs['palettes']}\\{eng.dirs['guipal']}.PAL"}
    alp, alp_from = install_alp(eng, pal)
    seen = grey_table(pal)
    # PALETTE.SHD as 0x42E1D0 opens it (palettes\<palette>.SHD); without it the engine generates the table
    # (0x4BADF0), which this pack does not reproduce, and a greyed button is drawn untinted
    shd = eng.load(eng.dirs["palettes"], eng.dirs["palette"], "SHD")
    greyed = greyed_table(seen, shd[0]) if shd and len(shd[0]) >= 32 * 256 else None
    report.append({"element": "greyed tint", **({"path": shd[1], "where": shd[2]} if greyed else
                   {"refused": f"no {eng.dirs['palettes']}\\{eng.dirs['palette']}.SHD of 32 rows: greyed buttons "
                               "are drawn untinted"})})
    bases = {}
    for eid, desc in descs.items():
        if desc is None:
            continue
        sub = []
        try:
            tiles, _feats = resolve_ground(eng, desc, sub)
        except Miss as e:
            report.append({"element": f"minimap {eid}", "refused": str(e)})
            continue
        w, h, ox, oy, px = minimap_base(desc, tiles, alp)
        (out / "minimap").mkdir(parents=True, exist_ok=True)
        (out / f"minimap/{eid}.bin").write_bytes(px)
        bases[eid] = {"file": f"minimap/{eid}.bin", "w": w, "h": h, "offx": ox, "offy": oy, "cells": desc["cells"]}
    report.append({"element": "minimap", "gui": gui_from, "alp": alp_from, "bases": sorted(bases)})

    # the sheet: every frame key becomes [sx, sy, w, h, hx, hy]; a button's hotspot is 0 because GUI_ButtonDraw
    # adds it back before the blit subtracts it (0x4A6188..0x4A61BE: the frame's corner lands on the gadget's)
    place, sh, sheet = art.pack()

    def rect(key, hx=0, hy=0):
        f = art.frames[key]
        x, y = place[key]
        return [x, y, f.width, f.height, hx, hy]
    art_out = {"sheet": {"file": "sheet.bin", "w": SHEET_W, "h": sh}, "palette": "pal.bin"}
    art_out["panelside"] = [{"f": rect(ps["f"], *ps["hot"]), "key": ps["key"]} if ps else None
                            for ps in side_art["panelside"]]
    scr_out = {}
    for name, a in art_screens.items():
        pan = a["panel"]
        pics = []
        for p in a["pics"]:
            q = {k: v for k, v in p.items() if k != "frames"}
            if "frames" in p:
                q["frames"] = [rect(k) for k in p["frames"]]
                key = art.frames[p["frames"][0]].transparent
                q["key"] = -1 if key is None else key
            pics.append(q)
        scr_out[name] = {"file": a["file"], "path": a["path"], "where": a["where"],
                         "panel": {"f": rect(pan["f"], *pan["hot"]), "key": pan["key"], "from": pan["from"],
                                   "path": pan["path"], "where": pan["where"]} if pan else None,
                         "pics": pics, "labels": a["labels"]}
    art_out["screens"] = scr_out
    d = side_art["dots"]
    art_out["dots"] = {"radlogo": [rect(k, hx, hy) for k, hx, hy in d["radlogo"]] if d["radlogo"] else None,
                       "high": rect(*d["high"]) if d["high"] else None, "key": d["key"]}
    art_out["minimap"] = {"unexplored": gui[GUI_UNEXPLORED] if gui else None, "box": gui[GUI_BOX] if gui else None,
                          "seen": "seen.bin", "bases": bases}
    art_out["greyed"] = "greyed.bin" if greyed else None
    out.mkdir(parents=True, exist_ok=True)
    (out / "sheet.bin").write_bytes(sheet)
    (out / "pal.bin").write_bytes(bytes(b for c in pal for b in (*c, 255)))
    (out / "seen.bin").write_bytes(seen)
    if greyed:
        (out / "greyed.bin").write_bytes(greyed)
    pack = {"format": FORMAT, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, never "
            "committed", "mod": mod,
            "folders": {**{k: eng.dirs[k] for k in FOLDERS + ("anims", "commongui", "units")},
                        "archive": eng.vfs.patterns[0], "from": eng.folders_from},
            "layout": layout, "casting": casting, "art": art_out, "report": report}
    (out / "side.json").write_text(json.dumps(pack, indent=1) + "\n")
    return pack


# --------------------------------------------------------------------------- main

def build_entry(eng, entry, name, desc, out, palette_block, alp, types):
    """The stage pack of the entry's scene `name`."""
    report = []
    ground = {"ok": False, "miss": None}
    mapblock = features = minimap = None
    m = entry["scenes"][name]["map"]
    try:
        if desc is None:
            raise Miss(f"no stage description for {entry['id']}.{name}")
        if list(desc["cells"]) != [m["w"], m["h"]]:
            raise Miss(f"the description's cells {desc['cells']} are not the scene's map")
        tiles, feats = resolve_ground(eng, desc, report)
        mapblock, features = write_ground(out, desc, tiles, feats)
        ground["ok"] = True
        # the minimap's base with this install's shrink table; a mod's own is in its side pack (side_pack)
        w, h, ox, oy, px = minimap_base(desc, tiles, alp)
        (out / "minimap.bin").write_bytes(px)
        minimap = {"file": "minimap.bin", "w": w, "h": h, "offx": ox, "offy": oy}
    except Miss as e:
        ground["miss"] = str(e)
        report.append({"element": "ground", "refused": str(e)})
    units = write_units(out, eng, entry, report, types)
    scene = {"format": "ta-scene/1", "wiki": FORMAT, "entry": entry["id"], "scene": name, "cells": [m["w"], m["h"]],
             "ground": ground, "map": mapblock, "features": features, "units": units, "minimap": minimap,
             "palette": palette_block, "shaders": "../shaders/",
             "depth": {"terrain": 0.10, "scale": 512.0, "flat0": 0.40, "flatSpan": 0.10, "tall0": 3.0,
                       "rowStep": 4.0, "unit0": 1.0, "shadowTall": 0.30, "shadowFlat": 0.03},
             "report": report}
    (out / "scene.json").write_text(json.dumps(scene, indent=1) + "\n")
    return scene


def write_stages(eng, entries, stage_dir, out_root, palette, shdb, casting=None):
    """Every stage's pack under `out_root`, one per scene of each entry, with the palette and the shaders beside them:
    `<entry>.<scene>/scene.json`, `palette/`, `shaders/`. The units are stage_types(entry, casting): a mod's pack
    (`casting` its side pack's list) holds every side's units for the entry's casts as well as the entry's own.
    Returns (the index's `entries`, keyed `<entry>.<scene>`, and each stage's resolution report)."""
    alp, _how = install_alp(eng, palette)
    (out_root / "palette").mkdir(parents=True, exist_ok=True)
    (out_root / "palette/pal.bin").write_bytes(bytes(b for c in palette for b in (*c, 255)))
    if shdb:
        (out_root / "palette/shd.bin").write_bytes(shdb)
    neutral = tascene.shd_neutral(shdb, palette) if shdb else [15, 1]
    palette_block = {"file": "../palette/pal.bin", "shd": "../palette/shd.bin" if shdb else None,
                     "shdRows": len(shdb) // 256, "neutral": neutral}
    (out_root / "shaders").mkdir(exist_ok=True)
    for name, src in tascene.extract_shaders(tascene.SHADER_SET).items():
        (out_root / "shaders" / name).write_text(src)
    index, full, types = {}, {}, {}
    for entry, name in ((e, n) for e in entries for n in e["scenes"]):
        sid = f"{entry['id']}.{name}"
        dpath = stage_dir / f"{sid}.json"
        desc = json.loads(dpath.read_text()) if dpath.exists() else None
        out = out_root / sid
        out.mkdir(exist_ok=True)
        scene = build_entry(eng, entry, name, desc, out, palette_block, alp, stage_types(entry, casting))
        index[sid] = {"scene": f"{sid}/scene.json", "ground": scene["ground"]["ok"],
                      "units": sorted(scene["units"]["meshes"]), "refused": scene["units"]["refused"]}
        full[sid] = scene["report"]
        for t, m in scene["units"]["meshes"].items():
            types.setdefault(t, m["sound"])
        g = "ground ok" if scene["ground"]["ok"] else f"ground FALLS BACK: {scene['ground']['miss']}"
        print(f"  {sid}: {g}; units {', '.join(sorted(scene['units']['meshes'])) or 'none'}"
              + (f"; refused {', '.join(scene['units']['refused'])}" if scene["units"]["refused"] else ""))
    snd = write_sounds(eng, out_root, types)
    print(f"  sounds: {len(snd['cats'])} categories, {len(list((out_root / 'sounds').glob('*.wav')))} WAVs"
          + (f"; refused {', '.join(r['wav'] + ' (' + r['why'] + ')' for r in snd['refused'])}" if snd["refused"] else ""))
    return index, full


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gfx.py", description=__doc__.split("\n")[0])
    ap.add_argument("--gamedir", help="the Total Annihilation install (default: TA3DO_GAMEDIR, "
                                      "else Steam's default library)")
    ap.add_argument("--exe", help="the executable the archive patterns and directory names are read from "
                                  "(default: the install's TotalA.exe)")
    ap.add_argument("--stage", help="read the stage descriptions from this folder instead of art/wiki/stage/")
    ap.add_argument("--out", help=f"write here instead of {OUT.relative_to(REPO)}/")
    ap.add_argument("--resolve-json", dest="resolve_json",
                    help="also write the resolution report, element by element, to this file")
    ap.add_argument("--side-only", dest="side_only", action="store_true",
                    help="write only the mod's pack, gfx/side/<mod>/ (tools/wiki/sidepacks.py runs this per setup)")
    ap.add_argument("--stages", action="store_true",
                    help="with --side-only, also every entry's stage pack with the mod's units, gfx/side/<mod>/stage/")
    ap.add_argument("--mod", default="stock", help="the sidebar pack's name (default: stock)")
    ap.add_argument("--folders", default="",
                    help="gui=..,unitpics=..,gamedata=..,download=..: folder names that replace the executable's, "
                         "for a mod whose loader renames them in memory")
    ap.add_argument("--archive", help="the first archive pattern, replacing the executable's (same reason)")
    ap.add_argument("--language", default=LANGUAGE,
                    help=f"the registry's `language` value: the folder variant opened first and the key unit names "
                         f"are read under (default: {LANGUAGE})")
    args = ap.parse_args(argv)

    root = tavfs.find_install(args.gamedir)
    try:
        folders = dict(kv.split("=", 1) for kv in args.folders.split(",") if kv)
    except ValueError:
        print("gfx.py: --folders takes name=folder pairs separated by commas", file=sys.stderr)
        return 2
    try:
        eng = Engine(root, args.exe, folders, args.archive, args.language)
    except tavfs.VfsError as e:
        print(f"gfx.py: {e}", file=sys.stderr)
        return 2
    out_root = Path(args.out) if args.out else OUT
    stage_dir = Path(args.stage) if args.stage else STAGE
    out_root.mkdir(parents=True, exist_ok=True)

    pal = eng.open(eng.dirs["palettes"], eng.dirs["palette"], "PAL")
    shd = eng.open(eng.dirs["palettes"], eng.dirs["palette"], "SHD")
    if pal is None or len(pal[0]) < 1024:
        print("gfx.py: no palette in the install", file=sys.stderr)
        return 2
    palette = [(pal[0][i * 4], pal[0][i * 4 + 1], pal[0][i * 4 + 2]) for i in range(256)]
    entries = [json.loads(f.read_text()) for f in sorted(ENTRIES.glob("*.json"))]
    descs = {}
    for entry in entries:
        for name, sc in entry["scenes"].items():
            sid = f"{entry['id']}.{name}"
            dpath = stage_dir / f"{sid}.json"
            desc = json.loads(dpath.read_text()) if dpath.exists() else None
            if desc and list(desc["cells"]) != [sc["map"]["w"], sc["map"]["h"]]:
                desc = None
            descs[sid] = desc
    if args.side_only:
        out = out_root / "side" / args.mod
        report = []
        try:
            pack = side_pack(eng, args.mod, descs, out, palette, report)
        except SideError as e:
            print(f"gfx.py: no sidebar pack for {args.mod}: {e}", file=sys.stderr)
            return 1
        lay = pack["layout"]
        print(f"gfx.py: wrote {out}/: {len(lay['screens'])} screens; "
              + "; ".join(f"{s['name']}: casts {sum(1 for c in cs['casts'].values() if c)}/{len(CASTS)}, types "
                          f"{', '.join(f'{t} ({len(v['pages'])} pages)' for t, v in s['types'].items()) or 'none'}"
                          for s, cs in zip(lay["sides"], pack["casting"]))
              + f"; {lay['height']} rows, {sum(1 for r in report if 'refused' in r)} refused")
        if args.stages:
            index = {"format": FORMAT, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, "
                     "never committed", "mod": args.mod}
            index["entries"], _full = write_stages(eng, entries, stage_dir, out / "stage", palette,
                                                   shd[0] if shd else b"", pack["casting"])
            (out / "stage/index.json").write_text(json.dumps(index, indent=1) + "\n")
        return 0
    shdb = shd[0] if shd else b""
    index = {"format": FORMAT, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, "
             "never committed", "install": {"archives": [a for a, _ in eng.vfs.archives],
                                            "dirs": eng.dirs, "palette": pal[2]}}
    index["cursors"] = cursors(eng, palette, out_root)
    index["entries"], full = write_stages(eng, entries, stage_dir, out_root, palette, shdb)
    (out_root / "index.json").write_text(json.dumps(index, indent=1) + "\n")
    if args.resolve_json:
        Path(args.resolve_json).write_text(json.dumps({"install": index["install"], "cursors": index["cursors"], "entries": full}, indent=1) + "\n")
    print(f"gfx.py: wrote {out_root}/ ({len(index['entries'])} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
