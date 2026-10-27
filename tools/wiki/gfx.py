#!/usr/bin/env python3
"""gfx.py -- the in-game wiki mockup's 3D stage packs, resolved from the player's own install.

    .venv-undither/bin/python tools/wiki/gfx.py                      # the install found as below
    .venv-undither/bin/python tools/wiki/gfx.py --gamedir <TA install> [--exe <its executable>]
    .venv-undither/bin/python tools/wiki/gfx.py --resolve-json <file>   # also the resolution, element by element

Writes tools/wiki/mockup/gfx/ -- gitignored, because everything in it is derived from the game's own files and
nothing derived from the game is ever committed (CLAUDE.md, *Nothing from the original game, ever*). The mockup's
`gfx=game` reads gfx/index.json and draws the stage with tools/tascene-render.js, tagpu's own shaders; without the
folder it stays on the whitebox. research/notes/ingame-wiki-3d-stage-plan.md section 9 is the design.

One tascene-format pack a wiki entry, `gfx/<entry>/scene.json` and its binaries:

  terrain/   the stage description's tile set as tagpu_terr.c's R8 atlas (64 tiles a row, 34-texel pitch), its
             tile map (one u16 a 32-unit tile, an index into that set) and a zero height grid (the stage is flat)
  features/  the description's 2D features: their frames in an R8 atlas, the anchors, the definitions
  units/     every unit type the entry names: its model as the engine holds it, in tascene's mesh format, one mesh
             per owner the entry's scenes use (a team face takes that owner's logo frame), the structure shadow's
             slant casters, the nanoframe's wire edges, and the frames in an R8 atlas
  palette/   PALETTE.PAL and PALETTE.SHD
  shaders    gfx/shaders/, tagpu's GLSL extracted from the C sources (the repository's, not the game's)

and gfx/index.json, which names each entry's pack and carries the whole resolution report; and the game's cursors,
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
- **Textures** (A4, A5): the texture bank is `textures\\*.GAF` in enumeration order without `logos.GAF`; a face's
  name binds to the first GAF in bank order that has the entry; on a miss to `textures\\logos.GAF`, as a team face
  (the owner's frame) only when the entry has exactly 10 frames; when neither has it, the face is flat colour 0xD1.
  An animated face shows its frame 0, the binder's starting frame.
- **Language variants**: every engine open tries `<dir>-english\\` first (0x4290F0), the registry's default.

Colour is palette colour: no restorer runs (the plan's question 7: the stage is Classic).

Each entry pack also carries `minimap.bin`, the minimap picture the engine generates from the stage's tiles
(`minimap_base`), shrunk with this install's palettes\\PALETTE.ALP.

THE SIDEBAR PACK (`--side-only --mod <m>`; tools/wiki/sidepacks.py runs it once per compat setup) is the mod's left
sidebar as the stage draws it: `gfx/side/<m>/side.json` and its binaries (spec: .data/wiki-sidebar/spec.md section
2.4). side.json holds
  layout   exactly the object tools/wiki/mockup/sim.js `checkLayout` takes: the sides (MAIN2 and GEN screens), the
           player's side, each player type's pages, and every screen's buttons with their roles and rects
  art      `sheet.bin` (u8 palette indices, 1024 wide; every frame [sx, sy, w, h, hx, hy] with its GAF key),
           `pal.bin` (PALETTE.PAL as 256 RGBA), PANELSIDE, each screen's panel picture, one picture a button (`pics[i]`
           belongs to `layout.screens[S].gadgets[i]`) and its labels, the minimap dots, and the minimap's colours:
           `unexplored` and `box` (GUI colours 0 and 0x0E), `seen.bin` (the grey remap) and one base per entry,
           `minimap/<entry>.bin`, shrunk with that install's own PALETTE.ALP
  defs     each player type's `canstop` (STOP's greyed state)
  report   every element's source, and every refusal, by name
The rules are the engine's, each cited where it is applied: the .GUI files by position (tavfs.gui_sections), the page
probe and the download fill of the def load and the page loader, the greyed pass, the picture rule of decision 5.
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
LANGUAGE = "english"       # 0x51FB50 when the registry's value is empty
TEAM_FRAMES = 10           # a logos.GAF entry is a team face only with exactly this many frames (0x42A1F2)

# The executable's directory strings (the scout answers, D1); a mod renames them in place (D2)
VA = {"units": 0x503920, "objects3d": 0x5036E4, "scripts": 0x503740, "textures": 0x503720,
      "logos_gaf": 0x503704, "logos": 0x504734, "anims": 0x502E30, "features": 0x502C7C,
      "palettes": 0x5031BC, "palette": 0x5033A4,
      # the left sidebar's (MEASURED from the pristine exe; spec section 1, *Folders a mod renames*)
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

    def __init__(self, root, exe=None, folders=None, archive=None):
        """`folders` ({name: folder}, FOLDERS only) and `archive` (the first mount pattern) replace what the
        executable's file says: a mod's loader rewrites those strings in memory at DllMain, before the engine reads
        them (the Q6 scout answer), and the offline pack reads the file."""
        self.vfs = tavfs.Vfs(root, exe, archive)
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
        """0x4290F0's path, `<d>-english\\<name>.<ext>` first: (bytes, path, where) or None."""
        name = name.rsplit(".", 1)[0] if "." in name else name      # it cuts at the last '.'
        for path in (f"{d}-{LANGUAGE}\\{name}.{ext}", f"{d}\\{name}.{ext}"):
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
        """(GafFrame, hotspot x, hotspot y) of frame k of `seq`, or None."""
        hit = self.seqs.get(seq.lower())
        if not hit or not 0 <= k < len(hit[1]):
            return None
        off = hit[1][k]
        hx, hy = struct.unpack_from("<hh", self.blob, off + 4)
        return ta3do.gaf_frame(self.blob, off), hx, hy


# --------------------------------------------------------------------------- the game's cursors

CURSOR_SEQ = {"arrow": "cursornormal", "move": "cursormove", "build": "cursornormal"}   # WIKI_CURSOR_*'s sequences
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
    report.append({"element": "unit", "name": name, "fbi": hit[1], "fbiFrom": hit[2], "model": mhit[1],
                   "modelFrom": mhit[2], "script": shit[1] if shit else None,
                   "scriptFrom": shit[2] if shit else None, "hidden": sorted(hidden),
                   "uncached": sorted(uncached), "facts": facts})
    return obj.lower(), root, hidden, uncached, facts


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
    """Every (type, owner)'s mesh in tascene's format, the slant casters and wire edges per type, and the atlas."""
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
        out = {"owners": {}, "quads": {}, "slant": [], "wire": [], "normals": [], "faces": 0, "flatMiss": 0}
        for owner in owners:
            verts, quads = [], []
            stats = {"faces": 0, "flatMiss": 0}

            def emit(piece, ox, oy, oz):
                px, py, pz = ox + piece.offset[0], oy + piece.offset[1], oz + piece.offset[2]
                if piece.name.lower() not in hidden:
                    faces, skip0 = engine_faces(piece)
                    vn = vertex_normals(piece, faces, skip0) if owner == owners[0] else None
                    for i, prim in enumerate(faces):
                        if (i == 0 and skip0) or len(prim.indices) < 3:
                            continue
                        if any(ix >= len(piece.vertices) for ix in prim.indices):
                            continue
                        pts = [(px + piece.vertices[ix][0], py + piece.vertices[ix][1], pz + piece.vertices[ix][2])
                               for ix in prim.indices]
                        if owner == owners[0]:
                            # the structure shadow's casters (0x45A610): every face of a cached piece
                            if piece.name.lower() not in uncached:
                                for k in range(1, len(pts) - 1):
                                    for c in (pts[0], pts[k], pts[k + 1]):
                                        out["slant"].extend(c)
                            # the nanoframe's wire (0x458FA0): every face as a closed outline
                            for e in range(len(pts)):
                                out["wire"].extend(pts[e]); out["wire"].extend(pts[(e + 1) % len(pts)])
                        rect = None
                        flat = (prim.color & 0xFF) / 255.0
                        if prim.texture:
                            got = bind(eng, prim.texture, owner, binds)
                            if got:
                                rect = rects[got[0]]
                            else:
                                flat = FLAT_MISS / 255.0
                                stats["flatMiss"] += 1
                        stats["faces"] += 1
                        n = len(pts)
                        qf = quad_frame(piece, prim.indices) if rect and n == 4 else None
                        for k in range(1, n - 1):        # fanned from vertex 0
                            for slot in (0, k, k + 1):
                                if vn is not None:
                                    # the vertex's own normal, then the quad's four corners' (its own four times
                                    # for any other face), as tagpu_posebake.c `geom_emit` carries them
                                    out["normals"].extend(vn[prim.indices[slot]])
                                    for c in range(4):
                                        out["normals"].extend(vn[prim.indices[c]] if n == 4 else vn[prim.indices[slot]])
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
                    emit(child, px, py, pz)

            emit(root, 0.0, 0.0, 0.0)
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
    for sc in [entry.get("scene", {})] + [d.get("scene", {}) for d in entry.get("demos", [])]:
        for u in sc.get("units", []) or []:
            if isinstance(u.get("owner"), int):
                owners.add(u["owner"])
    return sorted(owners)


def write_units(out, eng, entry, report):
    models, refused = {}, {}
    for name in entry["units"]:
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
    which would cut the stage's bottom rows off (spec section 5.2). Returns (w, h, offx, offy, w*h indices)."""
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
            "active": ck.get("active"), "gaffile": ck.get("gaffile"), "grayedout": k.get("grayedout"),
            "stages": k.get("stages")}
    for key, v in nums.items():
        if v is not None and not plain_int(v):
            report.append({"element": f"{where}#{index}", "note": f"{key} {v!r} is not an integer: read as "
                           f"{atoi(v)}, as the engine's atoi reads it"})
    n = {key: atoi(v) for key, v in nums.items()}
    rec = {"index": index, "id": n["id"], "name": ck.get("name", ""), "x": n["xpos"], "y": n["ypos"],
           "w": n["width"], "h": n["height"], "attribs": n["attribs"], "commonattribs": n["commonattribs"],
           "active": n["active"], "gaffile": n["gaffile"] & 1, "grey": n["grayedout"] & 1, "stages": n["stages"],
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
            out.append((k["unitmenu"], atoi(k.get("menu")), atoi(k.get("button")), k.get("unitname", ""), path))
    report.append({"element": "download", "folder": eng.dirs["download"], "entries": len(out), "empty files": empty})
    return out


def unit_index(eng, report):
    """Every FBI of the units folder by its UnitName without case (0x488B10 compares def+0x20, the FBI's
    `unitname`, with _stricmp): {name: (UnitName, Side, canstop, path, where)}. The first file listed wins a name."""
    out = {}
    for path, where in eng.vfs.enumerate(eng.dirs["units"], "*.fbi", False):
        try:
            f = fbi_fields(eng.vfs.read_from(path, where).decode("latin-1", "replace"))
        except (ta3do.HpiError, tavfs.VfsError, struct.error, OSError):
            continue
        name = (f.get("unitname") or "").strip()
        if name:
            # canstop is def+0x245 bit 3, 0 when the key is missing (0x42C90B..0x42C936; the Q4 scout answer)
            out.setdefault(name.lower(), (name, (f.get("side") or "").strip(), atoi(f.get("canstop")) & 1,
                                          path, where))
    report.append({"element": "units", "folder": eng.dirs["units"], "types": len(out)})
    return out


def read_sides(eng, report):
    """`<gamedata>\\sidedata.tdf`'s [SIDE0], [SIDE1], ... up to the first missing one (0x431A60), the first section
    of a name winning as the engine's lookup 0x4C3410 does: [{name, prefix, intgaf}]."""
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
        sides.append({"name": k.get("name", ""), "prefix": k.get("nameprefix", ""), "intgaf": k.get("intgaf", "")})
    report.append({"element": "sidedata", "path": hit[1], "where": hit[2], "sides": [s["name"] for s in sides]})
    if not sides:
        raise SideError(f"{hit[1]} has no [SIDE0]")
    return sides


def player_units(entries):
    """The types of the player's (owner 0) units over every entry's scene and demo scenes, in order of first
    appearance, and the first entry's own scene's list (the player's side reads that one)."""
    seen, first = [], []
    for i, entry in enumerate(entries):
        for j, sc in enumerate([entry.get("scene", {})] + [d.get("scene", {}) for d in entry.get("demos", [])]):
            for u in sc.get("units", []) or []:
                if (u.get("owner") or 0) == 0 and isinstance(u.get("type"), str):
                    if u["type"] not in seen:
                        seen.append(u["type"])
                    if i == 0 and j == 0:
                        first.append(u["type"])
    return seen, first


def player_side(sides, first, units):
    """The player's side, which names MAIN2, GEN, DL and PANELSIDE (the engine takes it from the player's
    PlayerInfo+0x95, which a stage has no copy of). In order, over the first entry's own scene's owner-0 units:
    1. the first unit whose FBI's `Side` equals a side's `name` without case;
    2. else the first unit whose type name begins with a side's `nameprefix` without case (the longest prefix): a
       side's files are named that way, and some installs' units declare a Side no sidedata names (TA Zero's
       human units say `NotForAI_Arm`, MEASURED, the Q5 scout answer);
    3. else side 0. Returns (index, the step that decided)."""
    names = [s["name"].lower() for s in sides]
    for t in first:
        u = units.get(t.lower())
        if u and u[1].lower() in names:
            return names.index(u[1].lower()), f"{t}'s Side {u[1]}"
    for t in first:
        hits = [(len(s["prefix"]), i) for i, s in enumerate(sides) if s["prefix"] and t.lower().startswith(s["prefix"].lower())]
        if hits:
            i = max(hits, key=lambda h: (h[0], -h[1]))[1]
            return i, f"{t} begins with {sides[i]['prefix']}"
    return 0, "no unit names a side: side 0"


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
        0x4B8D40), or None."""
        if got is None or name.lower() not in got[0].seqs:
            return None
        gaf, path, where = got
        out = []
        for k in range(len(gaf.seqs[name.lower()][1])):
            key = f"{path.lower()}:{where}:{name.upper()}:{k}"
            fr, hx, hy = gaf.frame(name, k)
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
    the last frame, else min(status + 2, n − 1) + base. Clipped to the sequence. A one-frame sequence gives one,
    which the stage draws in every state by the section 2.3 interim (INTERIM until capture C2)."""
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


def gadget_role(g, units):
    """The role 0x41AA00 gives the button's name, in its order: a unit type first (0x488B10), then the substrings
    PREV, NEXT, ORDERS, BUILD (strstr, case-sensitive), then commonattribs bit 2 (a build button whose name is no
    type: greyed), then STOP; any other button is an order. Returns (role, unit or None)."""
    name = g["name"]
    if g["commonattribs"] & 4 and name.lower() in units:
        return "build", units[name.lower()][0].upper()
    for sub, role in (("PREV", "prev"), ("NEXT", "next"), ("ORDERS", "orders"), ("BUILD", "buildtab")):
        if sub in name:
            return role, None
    if g["commonattribs"] & 4:
        return "build", None
    return ("stop" if "STOP" in name else "order"), None


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


def build_screen(art, name, file, path, where, recs, units, page, hide_pagers, report):
    """One screen of the layout and its art: (layout screen, art screen). Gadgets are the buttons (id 1) in file
    order; the panel (id 0), labels (id 5) and fonts (id 7) are the art's. `page` is the build page number (the
    greyed pass runs on page != 0) or None for a side's screen."""
    header = recs[0]
    if header["x"] != 0:
        report.append({"element": f"screen {name}", "note": f"the panel is at x {header['x']}; drawn at 0"})
    if not (1 <= header["w"] <= SIDE_W and header["h"] >= 1 and header["y"] >= 0 and header["y"] + header["h"] <= SIDE_ROWS):
        raise SideError(f"{name}: a {header['w']} x {header['h']} panel at row {header['y']} is outside the column")
    gadgets, pics, labels = [], [], []
    for g in recs[1:]:
        if g is None:
            continue
        if g["id"] == 5:
            labels.append({"rect": [g["x"], g["y"], g["w"], g["h"]], "text": g["text"]})
            continue
        if g["id"] != 1:
            continue
        role, unit = gadget_role(g, units)
        if role == "build" and page:
            # the greyed pass 0x41B04E..0x41B096: on a numbered page a build button naming a type is UN-greyed and
            # any other greyed, whatever the file says (0x4A1200 both ways; the Q4 scout answer)
            grey = unit is None
        else:
            grey = bool(g["grey"]) or (role == "build" and unit is None)
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
        if not g["active"] or (hide_pagers and role in ("prev", "next")):
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
    BUTTON + 4: the unit's name, commonattribs 4, not greyed, the `_gadget` picture. Later entries overwrite. The
    engine never bounds BUTTON + 4 against the gadget count (it writes past the screen's gadgets); here an entry
    whose slot is no gadget of the page is skipped and reported."""
    for unitmenu, menu, button, unitname, path in entries:
        if (menu & 0xFF) - 1 != page:
            continue
        slot = button + 4
        if not 0 <= slot < len(recs) or recs[slot] is None:
            report.append({"element": f"{name} download {unitname}", "refused": f"{path}: BUTTON {button} names "
                           f"gadget {slot}, which the page (of {len(recs)}) does not have"})
            continue
        g = recs[slot]
        g.update(orig=g["name"], name=unitname, commonattribs=4, grey=0, filled=True)
        report.append({"element": f"{name} download {unitname}", "slot": slot, "from": path})


def side_pack(eng, mod, entries, descs, out, pal, report):
    """The sidebar pack of one install (spec section 2.4): side.json, sheet.bin, pal.bin, seen.bin and the minimap
    bases. Returns the side.json object; raises SideError when no layout can be made."""
    units = unit_index(eng, report)
    sides = read_sides(eng, report)
    downloads = read_downloads(eng, report)
    types_seen, first = player_units(entries)
    ps, why = player_side(sides, first, units)
    report.append({"element": "player side", "side": sides[ps]["name"], "rule": why})
    art = SideArt(eng, report)
    screens, art_screens, lay_sides = {}, {}, []

    def add_screen(name, file, page, hide, recs_fill=None):
        path, where, recs = read_screen(eng, file, report)
        if recs_fill:
            recs_fill(recs)
        lay, a = build_screen(art, name, file, path, where, recs, units, page, hide, report)
        screens[name], art_screens[name] = lay, a

    side_map = {}
    for i, s in enumerate(sides):
        try:
            add_screen(f"{s['prefix']}MAIN2", f"{s['prefix']}MAIN2", None, False)
            add_screen(f"{s['prefix']}GEN", f"{s['prefix']}GEN", None, False)
        except SideError as e:
            report.append({"element": f"side {s['name']}", "refused": str(e)})
            continue
        side_map[i] = len(lay_sides)
        lay_sides.append({"name": s["name"], "prefix": s["prefix"], "main2": f"{s['prefix']}MAIN2",
                          "gen": f"{s['prefix']}GEN"})
    if ps not in side_map:
        raise SideError(f"the player's side {sides[ps]['name']} has no MAIN2 and GEN screens")
    prefix = sides[ps]["prefix"]
    types, defs = {}, {}
    for t in types_seen:
        u = units.get(t.lower())
        if u is None:
            report.append({"element": f"type {t}", "refused": f"no {eng.dirs['units']}\\*.FBI has UnitName {t}"})
            continue
        defs[t] = {"canstop": u[2], "fbi": u[3], "where": u[4]}
        got = type_pages(eng, u[0], downloads, prefix, report)
        if got is None:
            report.append({"element": f"type {t}", "note": "no page: the router shows GEN for it"})
            continue
        pages, mine = got
        # SIDE_SCREENS and SIDE_TYPES: the layout's tables; a type that would pass either is left out whole
        if len(screens) + len(pages) > SIDE_SCREENS or len(types) == SIDE_TYPES:
            report.append({"element": f"type {t}", "refused": f"its {len(pages)} pages pass {SIDE_SCREENS} screens"})
            continue
        numbered = [p for p, _f in pages if p]
        made = []
        try:
            for p, file in pages:
                name = f"{t.upper()}{p}"
                add_screen(name, file, p, not numbered,
                           lambda recs, p=p, name=name: fill_downloads(recs, p, mine, name, report))
                made.append(name)
        except SideError as e:
            for name in made:
                screens.pop(name)
                art_screens.pop(name)
            report.append({"element": f"type {t}", "refused": str(e)})
            continue
        types[t.upper()] = {"pages": [f"{t.upper()}{p}" for p in numbered],
                            "orders": f"{t.upper()}0" if pages[0][0] == 0 else None}
        report.append({"element": f"type {t}", "pages": [f for _p, f in pages]})
    height = max([SIDE_MIN_H] + [s["y"] + s["h"] for s in screens.values()])
    layout = {"format": 1, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, never committed",
              "width": SIDE_W, "height": height, "sides": lay_sides, "player_side": side_map[ps],
              "types": types, "screens": screens}

    # PANELSIDE and the dots
    side_art = {}
    got = art.bank(sides[ps]["intgaf"]) if sides[ps]["intgaf"] else None
    fr = art.seq(got, "PANELSIDE")
    side_art["panelside"] = {"f": fr[0][0], "hot": [fr[0][2], fr[0][3]], "key": fr[0][1].transparent} if fr else None
    report.append({"element": "panelside", "from": got and got[1], "where": got and got[2],
                   **({} if fr else {"refused": f"no PANELSIDE in {eng.dirs['anims']}\\{sides[ps]['intgaf']}.GAF"})})
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
    ps_art = side_art["panelside"]
    art_out["panelside"] = {"f": rect(ps_art["f"], *ps_art["hot"]), "key": ps_art["key"]} if ps_art else None
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
    out.mkdir(parents=True, exist_ok=True)
    (out / "sheet.bin").write_bytes(sheet)
    (out / "pal.bin").write_bytes(bytes(b for c in pal for b in (*c, 255)))
    (out / "seen.bin").write_bytes(seen)
    pack = {"format": FORMAT, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, never "
            "committed", "mod": mod,
            "folders": {**{k: eng.dirs[k] for k in FOLDERS + ("anims", "commongui", "units")},
                        "archive": eng.vfs.patterns[0], "from": eng.folders_from},
            "layout": layout, "art": art_out, "defs": defs, "report": report}
    (out / "side.json").write_text(json.dumps(pack, indent=1) + "\n")
    return pack


# --------------------------------------------------------------------------- main

def build_entry(eng, entry, desc, out, palette_block, alp):
    report = []
    ground = {"ok": False, "miss": None}
    mapblock = features = minimap = None
    try:
        if desc is None:
            raise Miss(f"no stage description for {entry['id']}")
        if list(desc["cells"]) != [entry["scene"]["map"]["w"], entry["scene"]["map"]["h"]]:
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
    units = write_units(out, eng, entry, report)
    cells = [entry["scene"]["map"]["w"], entry["scene"]["map"]["h"]]
    scene = {"format": "ta-scene/1", "wiki": FORMAT, "entry": entry["id"], "cells": cells,
             "ground": ground, "map": mapblock, "features": features, "units": units, "minimap": minimap,
             "palette": palette_block, "shaders": "../shaders/",
             "depth": {"terrain": 0.10, "scale": 512.0, "flat0": 0.40, "flatSpan": 0.10, "tall0": 3.0,
                       "rowStep": 4.0, "unit0": 1.0, "shadowTall": 0.30, "shadowFlat": 0.03},
             "report": report}
    (out / "scene.json").write_text(json.dumps(scene, indent=1) + "\n")
    return scene


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
                    help="write only the sidebar pack, gfx/side/<mod>/ (tools/wiki/sidepacks.py runs this per setup)")
    ap.add_argument("--mod", default="stock", help="the sidebar pack's name (default: stock)")
    ap.add_argument("--folders", default="",
                    help="gui=..,unitpics=..,gamedata=..,download=..: folder names that replace the executable's, "
                         "for a mod whose loader renames them in memory")
    ap.add_argument("--archive", help="the first archive pattern, replacing the executable's (same reason)")
    args = ap.parse_args(argv)

    root = tavfs.find_install(args.gamedir)
    try:
        folders = dict(kv.split("=", 1) for kv in args.folders.split(",") if kv)
    except ValueError:
        print("gfx.py: --folders takes name=folder pairs separated by commas", file=sys.stderr)
        return 2
    try:
        eng = Engine(root, args.exe, folders, args.archive)
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
        dpath = stage_dir / f"{entry['id']}.json"
        desc = json.loads(dpath.read_text()) if dpath.exists() else None
        if desc and list(desc["cells"]) != [entry["scene"]["map"]["w"], entry["scene"]["map"]["h"]]:
            desc = None
        descs[entry["id"]] = desc
    if args.side_only:
        out = out_root / "side" / args.mod
        report = []
        try:
            pack = side_pack(eng, args.mod, entries, descs, out, palette, report)
        except SideError as e:
            print(f"gfx.py: no sidebar pack for {args.mod}: {e}", file=sys.stderr)
            return 1
        lay = pack["layout"]
        print(f"gfx.py: wrote {out}/: {len(lay['screens'])} screens, types "
              f"{', '.join(f'{t} ({len(v['pages'])} pages)' for t, v in lay['types'].items()) or 'none'}, "
              f"{lay['height']} rows, {sum(1 for r in report if 'refused' in r)} refused")
        return 0
    alp, _how = install_alp(eng, palette)
    (out_root / "palette").mkdir(exist_ok=True)
    (out_root / "palette/pal.bin").write_bytes(bytes(b for c in palette for b in (*c, 255)))
    shdb = shd[0] if shd else b""
    if shdb:
        (out_root / "palette/shd.bin").write_bytes(shdb)
    neutral = tascene.shd_neutral(shdb, palette) if shdb else [15, 1]
    palette_block = {"file": "../palette/pal.bin", "shd": "../palette/shd.bin" if shdb else None,
                     "shdRows": len(shdb) // 256, "neutral": neutral}
    (out_root / "shaders").mkdir(exist_ok=True)
    for name, src in tascene.extract_shaders(tascene.SHADER_SET).items():
        (out_root / "shaders" / name).write_text(src)

    index = {"format": FORMAT, "note": "derived from the local install by tools/wiki/gfx.py; gitignored, "
             "never committed", "install": {"archives": [a for a, _ in eng.vfs.archives],
                                            "dirs": eng.dirs, "palette": pal[2]},
             "entries": {}}
    index["cursors"] = cursors(eng, palette, out_root)
    full = {}
    for entry in entries:
        dpath = stage_dir / f"{entry['id']}.json"
        desc = json.loads(dpath.read_text()) if dpath.exists() else None
        out = out_root / entry["id"]
        out.mkdir(exist_ok=True)
        scene = build_entry(eng, entry, desc, out, palette_block, alp)
        index["entries"][entry["id"]] = {"scene": f"{entry['id']}/scene.json", "ground": scene["ground"]["ok"],
                                         "units": sorted(scene["units"]["meshes"]),
                                         "refused": scene["units"]["refused"]}
        full[entry["id"]] = scene["report"]
        g = "ground ok" if scene["ground"]["ok"] else f"ground FALLS BACK: {scene['ground']['miss']}"
        print(f"  {entry['id']}: {g}; units {', '.join(sorted(scene['units']['meshes'])) or 'none'}"
              + (f"; refused {', '.join(scene['units']['refused'])}" if scene["units"]["refused"] else ""))
    (out_root / "index.json").write_text(json.dumps(index, indent=1) + "\n")
    if args.resolve_json:
        Path(args.resolve_json).write_text(json.dumps({"install": index["install"], "cursors": index["cursors"], "entries": full}, indent=1) + "\n")
    print(f"gfx.py: wrote {out_root}/ ({len(index['entries'])} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
