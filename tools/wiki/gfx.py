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

and gfx/index.json, which names each entry's pack and carries the whole resolution report.

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
"""

import argparse
import json
import os
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
      "palettes": 0x5031BC, "palette": 0x5033A4}


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

    def __init__(self, root, exe=None):
        self.vfs = tavfs.Vfs(root, exe)
        exe_path = Path(exe) if exe else self.vfs._loose_path(tavfs.EXE_NAME)
        blob = exe_path.read_bytes()
        self.dirs = {k: tavfs.exe_cstr(blob, va) for k, va in VA.items()}
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

    def gaf(self, d, name):
        hit = self.open(d, name, "GAF")
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
        self.seqs = {}
        for off in struct.unpack_from(f"<{count}I", blob, 12):
            n = struct.unpack_from("<H", blob, off)[0]
            name = blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1").lower()
            frames = [struct.unpack_from("<I", blob, off + 40 + 8 * i)[0] for i in range(n)]
            self.seqs.setdefault(name, (n, frames))

    def frame(self, seq, k=0):
        """(GafFrame, hotspot x, hotspot y) of frame k of `seq`, or None."""
        hit = self.seqs.get(seq.lower())
        if not hit or not 0 <= k < len(hit[1]):
            return None
        off = hit[1][k]
        hx, hy = struct.unpack_from("<hh", self.blob, off + 4)
        return ta3do.gaf_frame(self.blob, off), hx, hy


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
    """The engine's vertex normal of every vertex of a node (tagpu_posebake.c `piece_vnormals`, 0x45A0A0..0x45A22C):
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


# --------------------------------------------------------------------------- main

def build_entry(eng, entry, desc, out, palette_block):
    report = []
    ground = {"ok": False, "miss": None}
    mapblock = features = None
    try:
        if desc is None:
            raise Miss(f"no stage description for {entry['id']}")
        if list(desc["cells"]) != [entry["scene"]["map"]["w"], entry["scene"]["map"]["h"]]:
            raise Miss(f"the description's cells {desc['cells']} are not the scene's map")
        tiles, feats = resolve_ground(eng, desc, report)
        mapblock, features = write_ground(out, desc, tiles, feats)
        ground["ok"] = True
    except Miss as e:
        ground["miss"] = str(e)
        report.append({"element": "ground", "refused": str(e)})
    units = write_units(out, eng, entry, report)
    cells = [entry["scene"]["map"]["w"], entry["scene"]["map"]["h"]]
    scene = {"format": "ta-scene/1", "wiki": FORMAT, "entry": entry["id"], "cells": cells,
             "ground": ground, "map": mapblock, "features": features, "units": units,
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
    args = ap.parse_args(argv)

    root = tavfs.find_install(args.gamedir)
    try:
        eng = Engine(root, args.exe)
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
    full = {}
    for f in sorted(ENTRIES.glob("*.json")):
        entry = json.loads(f.read_text())
        dpath = stage_dir / f"{entry['id']}.json"
        desc = json.loads(dpath.read_text()) if dpath.exists() else None
        out = out_root / entry["id"]
        out.mkdir(exist_ok=True)
        scene = build_entry(eng, entry, desc, out, palette_block)
        index["entries"][entry["id"]] = {"scene": f"{entry['id']}/scene.json", "ground": scene["ground"]["ok"],
                                         "units": sorted(scene["units"]["meshes"]),
                                         "refused": scene["units"]["refused"]}
        full[entry["id"]] = scene["report"]
        g = "ground ok" if scene["ground"]["ok"] else f"ground FALLS BACK: {scene['ground']['miss']}"
        print(f"  {entry['id']}: {g}; units {', '.join(sorted(scene['units']['meshes'])) or 'none'}"
              + (f"; refused {', '.join(scene['units']['refused'])}" if scene["units"]["refused"] else ""))
    (out_root / "index.json").write_text(json.dumps(index, indent=1) + "\n")
    if args.resolve_json:
        Path(args.resolve_json).write_text(json.dumps({"install": index["install"], "entries": full}, indent=1) + "\n")
    print(f"gfx.py: wrote {out_root}/ ({len(index['entries'])} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
