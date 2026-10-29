#!/usr/bin/env python3
"""assetcheck.py -- the in-game wiki's 3D asset load (tagpu/ddraw/src/tagpu_wikiasset.c, landing 6e), checked natively
against an independent Python resolution of the same install.

    PY=.venv-undither/bin/python                     # the main checkout's venv
    $PY tools/wiki/assetcheck.py                     # build, load, compare, fuzz; exit 0 when all agree
    $PY tools/wiki/assetcheck.py --gamedir DIR --mounts FILE --report OUT.json
                                                     # the load over DIR with that mount list (one archive a
                                                     # line, as the game's report lists them), its JSON into OUT

1. **The build**: tagpu/ddraw/tests/wikiassettest.c with the host's gcc, -fsanitize=address,undefined and
   -fno-sanitize-recover=all (lodepng, vendored, with AddressSanitizer alone: its own shifts are UBSan's findings,
   not ours), into tagpu/ddraw/tests/wikiassettest-native.exe.
2. **The load**: `wikiassettest load` over the install (--gamedir, else TA3DO_GAMEDIR, else Steam's library) and
   art/wiki/ packed as the DLL packs it (tools/hudpak.py --set wiki); its report is the DLL's tagpu_wiki3d.json. As
   in the DLL, the sidebar is loaded first, so the unit types are every entry's own and, after each entry's, every
   layout side's type for each cast the entry's units name.
3. **The reference**: the same questions answered here, from tools/wiki/tavfs.py's mount and tools/ta3do's HPI
   reader and tools/tascene's TDF reader, with the engine's rules written out a second time: the language's folder
   first, the shadow mark in every enumeration, the first TDF holding a section, units\\<name>.FBI by path, the 3DO
   built as the engine holds it (the selection face swapped to 0, faces 1.. bubble-sorted by mean Y, X and Z
   negated) and bound (the texture bank in its enumeration order, then logos.GAF, FF_TEAM for exactly 10 frames),
   each model's shape hash over that, Create()'s hides and dont-caches (ta3do's COB reader, the engine's swap 0x45A950 by name
   over its piece order, bits over the readers' walk) and the FBI's facts (BMcode, Digger, NoShadow, CanHover, Floater).
   The unit types are gfx.py's `stage_types` per entry over the casting of `gfx.py --side-only` on the same install,
   which takes no mount list: under --mounts the casting is the install's own mount order's.
   Every field must agree but `arena_bytes` (the C side's
   alone) and the wording of a refusal (only whether there is one).
4. **The mockup's resolution**: tools/wiki/gfx.py --resolve-json (landing 6d), element by element: each map's path,
   archive and tile count, each tile's hash, each feature's TDF, archive and GAF, each unit's FBI, model and script
   with their archives, its hidden and dont-cache pieces, its script's posed order and visible pieces and its facts.
   Skipped under --mounts, which gfx.py does not take.
5. **The fuzz**: `wikiassettest fuzz-hpi` on two archives (LZ77 and keyed, zlib and plain) and `fuzz-3do` on one
   model, 1000 damaged copies each; and `fuzz-side`, the sidebar's readers over synthetic files (no game data): the
   positional .GUI reader and the GAF, PCX and number readers on damaged and garbage copies with huge counts, and
   the whole sidebar load over a synthetic install with one of its files damaged each time. Every refusal must be
   named and the sanitizers must stay silent. The undamaged install's casting must be the rule's, its solar a unit
   of its own past a tower that makes energy; gfx.py's rule runs over the same units (`cast_rule_case`).
6. **The sidebar, per compat setup** (`--side`, or with the rest; `--mods` to choose): each setup's game folder laid
   out as tools/wiki/sidepacks.py lays it out (the retail install, then the setup's files over it), its folder names
   and first archive from sidepacks.py's record where the mod's loader renames them in memory, and the DLL's
   `wikiassettest side` against `gfx.py --side-only` with the same names: the layout (every side with its own types),
   every screen's file and panel, every button's picture source (step, file, archive, sequence) and its frames'
   pixels and hotspots, each side's PANELSIDE, the dots, the minimap's colours, tables and bases, the greyed tint
   and each side's casting -- every cast's type, title, footprint, figures, canstop, G cells and build list; the
   rule's `report` prose is gfx.py's alone -- must be equal, and the sheet byte for byte; and the 3D load's unit types,
   in its order, must be gfx.py's `stage_types` of every entry over the pack's casting. Both halves read the
   display names under --language (english by default). The setups' fixtures must be present
   (tools/compat/tacompat.py fetch).

No game is run and nothing is written into the repository but the test's executable, which `*.exe` ignores.
"""
import argparse
import atexit
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DDRAW = REPO / "tagpu" / "ddraw"
EXE = DDRAW / "tests" / "wikiassettest-native.exe"
sys.path.insert(0, str(HERE))
import tavfs  # noqa: E402
import gfx as gfxpy  # noqa: E402

ta3do = tavfs.ta3do
tascene = tavfs.tascene

SRCS = ["tests/wikiassettest.c", "src/tagpu_hpi.c", "src/tagpu_vfs.c", "src/tagpu_tdf.c", "src/tagpu_json.c",
        "src/tagpu_pak.c", "src/tagpu_wikidata.c", "src/tagpu_cobscan.c", "src/tagpu_guifile.c", "src/tagpu_wav.c"]
NAME_VAS = {"units": 0x503920, "textures": 0x503720, "logos": 0x504734, "logos_gaf": 0x503704,
            "objects3d": 0x5036E4, "scripts": 0x503740, "anims": 0x502E30, "features": 0x502C7C}
FEATURE_BITS = ["animating", "animtrans", "shadtrans", "flamable", "geothermal", "blocking", "reclaimable",
                "autoreclaimable", "indestructible", "nodisplayinfo", "nodrawundergray"]
FNV0 = 0x811C9DC5


def build():
    lode = DDRAW / "tests" / "lodepng-native.o"
    subprocess.run(["gcc", "-std=c99", "-O1", "-g", "-Iinc", "-Isrc", "-fsanitize=address", "-c", "-o", str(lode),
                    "src/lodepng.c"], cwd=DDRAW, check=True)
    subprocess.run(["gcc", "-std=c99", "-D_DEFAULT_SOURCE", "-O1", "-g", "-Wall", "-Iinc", "-Isrc",
                    "-Wno-format-truncation", "-Wno-restrict", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-o", str(EXE), *SRCS, str(lode), "-lm"],
                   cwd=DDRAW, check=True)
    lode.unlink()


def fnv(h, data):
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def low(s):
    return None if s is None else s.lower()


def ascii_lower(s):
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def atoi(v, dflt):
    if v is None:
        return dflt
    m = re.match(r"[ \t\n\v\f\r]*([+-]?)(\d*)", v)
    digits = m.group(2)[:9] if m else ""
    n = int(digits) if digits else 0
    return -n if m and m.group(1) == "-" else n


# --------------------------------------------------------------------------- the engine's file rules

class Engine:
    def __init__(self, vfs, names, language):
        self.vfs, self.names, self.lang = vfs, names, language

    def find(self, path):
        """(key, where) by the engine's precedence, or None."""
        if self.vfs._loose_path(path):
            return tavfs._key(path), "loose"
        key = tavfs._key(path)
        for name, arc in self.vfs.archives:
            if key in arc.files:
                return key, name
        return None

    def read(self, hit):
        key, where = hit
        return self.vfs.read_from(key.replace("/", "\\"), where)

    @staticmethod
    def built(d, lang, name, ext):
        p = f"{d}-{lang}\\{name}" if lang is not None else f"{d}\\{name}"
        if ext:
            if "." in p:
                p = p[:p.rindex(".")]
            p = f"{p}.{ext}"
        return p

    def find_built(self, d, name, ext):
        for lang in (self.lang, None):
            p = self.built(d, lang, name, ext)
            hit = self.find(p)
            if hit:
                return p, hit
        return self.built(d, None, name, ext), None

    def shadowed(self, k, key):
        if self.vfs._loose_path(key.replace("/", "\\")):
            return True
        return any(key in arc.files for _n, arc in self.vfs.archives[:k])

    def enumerate(self, d, pattern, recursive):
        """[(path as the engine spells it, where)] in the enumeration's order, shadowed entries skipped."""
        out = [(rel.replace("/", "\\"), "loose") for rel, _p in self.vfs._loose_walk(d, recursive)
               if tavfs.fnmatch.fnmatch(rel.rsplit("/", 1)[-1], pattern.lower())]
        prefix = tavfs._key(d) + "/"
        for k, (name, arc) in enumerate(self.vfs.archives):
            for key in arc.files:
                if not key.startswith(prefix):
                    continue
                rest = key[len(prefix):]
                if "/" in rest and not recursive:
                    continue
                if not tavfs.fnmatch.fnmatch(rest.rsplit("/", 1)[-1], pattern.lower()) or self.shadowed(k, key):
                    continue
                out.append((key.replace("/", "\\"), name))
        return out


# --------------------------------------------------------------------------- GAF

class Gaf:
    def __init__(self, blob):
        self.b = blob
        count = struct.unpack_from("<h", blob, 4)[0]
        self.seqs = list(struct.unpack_from(f"<{max(count, 0)}I", blob, 12))

    def find(self, name):
        want = ascii_lower(name)
        for so in self.seqs:
            nm = self.b[so + 8:so + 40].split(b"\0")[0].decode("latin-1")
            if ascii_lower(nm) == want:
                return so
        return None

    def frames(self, so):
        return struct.unpack_from("<H", self.b, so)[0]


# --------------------------------------------------------------------------- the 3DO, as the engine holds it

class Tex:
    def __init__(self, name):
        self.name, self.kind, self.frames, self.gaf = name, "pending", 0, None


class Model:
    """A 3DO built as tagpu_wikiasset.c builds it; faces keep their texture by the loader's shared table."""

    def __init__(self, blob, textable):
        self.blob, self.nodes, self.texfaces = blob, [], []
        self.root = self.node(0, textable)

    def node(self, off, textable):
        b = self.blob
        (version, nv, nf, sel, x, y, z, name_off, _zero, vo, fo, sib, child) = struct.unpack_from("<IIIiiiiIIIIII", b, off)
        assert version == 1
        name = b[name_off:b.index(b"\0", name_off)] if name_off else b""
        verts = [struct.unpack_from("<iii", b, vo + 12 * i) for i in range(nv)]
        faces = []
        for i in range(nf):
            color, nidx, _z, io, to, f1, f2, flags = struct.unpack_from("<IIIIIIII", b, fo + 32 * i)
            idx = list(struct.unpack_from(f"<{nidx}H", b, io))
            tex = b[to:b.index(b"\0", to)].decode("latin-1") if to else None
            faces.append({"color": color, "idx": idx, "flags": flags, "tex": tex})
        if sel != -1 and nf:
            faces[0], faces[sel] = faces[sel], faces[0]
            sel = 0
        if nf > 2:
            def key(f):
                s = sum(verts[v][1] for v in f["idx"]) & 0xFFFFFFFF
                s = s - (1 << 32) if s >= 1 << 31 else s
                q = abs(s) // len(f["idx"])
                return -q if s < 0 else q
            keys = [None] + [key(f) for f in faces[1:]]
            swapped = True
            while swapped:
                swapped = False
                for i in range(1, nf - 1):
                    if keys[i] > keys[i + 1]:
                        keys[i], keys[i + 1] = keys[i + 1], keys[i]
                        faces[i], faces[i + 1] = faces[i + 1], faces[i]
                        swapped = True
        neg = lambda v: (-v) & 0xFFFFFFFF  # noqa: E731
        n = {"name": name, "nv": nv, "nf": nf, "sel": sel, "off": (neg(x), y & 0xFFFFFFFF, neg(z)),
             "verts": [(neg(a), c & 0xFFFFFFFF, neg(e)) for a, c, e in verts], "faces": faces, "child": None, "sib": None}
        self.nodes.append(n)
        for f in faces:
            if f["tex"] is not None:
                k = ascii_lower(f["tex"])
                if k not in textable:
                    textable[k] = Tex(f["tex"])
                f["T"] = textable[k]
                self.texfaces.append(f)
        if child:
            n["child"] = self.node(child, textable)
        if sib:
            n["sib"] = self.node(sib, textable)
        return n

    def walk(self):
        stack = [self.root]
        while stack:
            n = stack.pop()
            yield n
            if n["sib"]:
                stack.append(n["sib"])
            if n["child"]:
                stack.append(n["child"])


def bind(model):
    for f in model.texfaces:
        t = f["T"]
        flags = f["flags"] & ~4
        if t.kind == "miss":
            f["color"], f["flags"] = 0xD1, flags | 1
            continue
        if t.kind == "team":
            flags |= 4
        flags = flags | 2 if t.frames > 1 else flags & ~2
        f["flags"] = flags


def model_report(path, where, model, why, bank, logos):
    r = {"path": low(path), "from": low(where), "ok": model is not None, "why": why}
    if model is None:
        return r
    h, counts, listed = FNV0, dict(flat=0, static=0, anim=0, team=0, miss=0), []
    for n in model.walk():
        h = fnv(h, n["name"] + b"\0")
        h = fnv(h, struct.pack("<IIi", n["nv"], n["nf"], n["sel"]))
        h = fnv(h, struct.pack("<III", *n["off"]))
        for v in n["verts"]:
            h = fnv(h, struct.pack("<III", *v))
        for f in n["faces"]:
            t = f.get("T")
            h = fnv(h, struct.pack("<II", f["color"], len(f["idx"])))
            h = fnv(h, struct.pack(f"<{len(f['idx'])}H", *f["idx"]))
            h = fnv(h, struct.pack("<I", f["flags"]))
            h = fnv(h, (t.name.encode("latin-1") if t else b"") + b"\0")
            if f["flags"] & 1:
                counts["flat"] += 1
            if t:
                if t.kind == "miss":
                    counts["miss"] += 1
                elif f["flags"] & 4:
                    counts["team"] += 1
                elif f["flags"] & 2:
                    counts["anim"] += 1
                else:
                    counts["static"] += 1
                if t not in listed:
                    listed.append(t)
    r.update(pieces=len(model.nodes), faces=sum(n["nf"] for n in model.nodes), **counts, shape=f"{h:08x}")
    texes = []
    for t in listed:
        g = None if t.kind == "miss" else (logos if t.gaf == -1 else bank[t.gaf])
        texes.append({"name": t.name, "gaf": low(g["gaf"]) if g else None, "from": low(g["from"]) if g else None,
                      "kind": t.kind, "frames": 0 if t.kind == "miss" else t.frames})
    r["textures"] = texes
    return r


# --------------------------------------------------------------------------- Create()'s hides

MARKS = {"hides": (ta3do.OP_HIDE, ta3do.OP_SHOW), "uncached": (ta3do.OP_DONT_CACHE, ta3do.OP_CACHE)}


def atoi(v, dflt):
    """An FBI value as the engine's integer reader takes it (tagpu_tdf_int): leading blanks, a
    sign, the digits; `dflt` when the key is absent."""
    if v is None:
        return dflt
    m = re.match(r"\s*([+-]?)(\d*)", v)
    n = int(m.group(2) or "0")
    return -n if m.group(1) == "-" else n


def cob_create(blob, ops=MARKS["hides"]):
    """(piece names, marked piece indices) from Create()'s opening run, by ta3do's opcode table:
    `ops` is the opcode that marks a piece and the one that clears it (MARKS); None for a script
    ta3do cannot read, (pieces, set()) for one without Create()."""
    parsed = ta3do._cob_read(blob)
    if parsed is None:
        return None
    entries, names, _p, code = parsed
    n_pieces, off_pnames = struct.unpack_from("<i", blob, 8)[0], struct.unpack_from("<i", blob, 32)[0]
    pieces = [blob[o:blob.index(b"\0", o)] for o in struct.unpack_from(f"<{n_pieces}i", blob, off_pnames)]
    if "create" not in names:
        return pieces, set()
    pc, hidden = entries[names.index("create")], set()
    while 0 <= pc < len(code):
        op = code[pc] & 0xFFFFFFFF
        inline = ta3do.COB_PROLOGUE.get(op)
        if inline is None:
            break
        if op in ops and pc + 1 < len(code) and 0 <= code[pc + 1] < n_pieces:
            (hidden.add if op == ops[0] else hidden.discard)(code[pc + 1])
        pc += 1 + inline
    return pieces, hidden


def create_hides(blob, model, ops=MARKS["hides"]):
    """The report's `hides` (or `uncached`, with that pair's `ops`): the engine's swap (0x45A950: COB piece i takes the first node at an
    index >= i of its name, in the engine's order a node, its child's subtree, its sibling's),
    the hidden pieces' nodes as bits over the readers' walk (root, then children pushed in
    sibling order, the root's siblings not)."""
    walk, stack = [], [model.root]
    while stack:
        n = stack.pop()
        walk.append(n)
        ch = n["child"]
        while ch:
            stack.append(ch)
            ch = ch["sib"]
    prim, stack = [], [model.root]
    while stack:
        n = stack.pop()
        prim.append(n)
        if n["sib"]:
            stack.append(n["sib"])
        if n["child"]:
            stack.append(n["child"])
    words = [0] * 8
    got = cob_create(blob, ops)
    if got is None:
        return {"pieces": [], "bits": "0" * 64, "why": "refused"}
    pieces, hidden = got
    fold = lambda b: ascii_lower(b.decode("latin-1"))  # noqa: E731
    upto = max(hidden) if hidden else -1
    for i in range(min(len(pieces), len(prim), upto + 1)):
        j = next((j for j in range(i, len(prim)) if fold(prim[j]["name"]) == fold(pieces[i])), None)
        if j is not None:
            prim[i], prim[j] = prim[j], prim[i]
    names = []
    for g, n in enumerate(walk):
        if any(k < len(prim) and prim[k] is n for k in hidden):
            words[g >> 5] |= 1 << (g & 31)
            names.append(n["name"].decode("latin-1"))
    return {"pieces": names, "bits": "".join(f"{w:08x}" for w in words), "why": None}


def posed(blob, model):
    """The report's `posed`: the engine's order for a new unit (a node, its child's subtree, its sibling's) put in place
    by the swap 0x45A950 for every COB piece below the primitive count, its nodes' names and each one's visibility
    (0x45AF1B: three or more vertices) as a string of 0 and 1; both empty for a script ta3do cannot read."""
    prim, stack = [], [model.root]
    while stack:
        n = stack.pop()
        prim.append(n)
        if n["sib"]:
            stack.append(n["sib"])
        if n["child"]:
            stack.append(n["child"])
    if ta3do._cob_read(blob) is None:
        return {"pieces": [], "visible": ""}
    n_pieces, off_pnames = struct.unpack_from("<i", blob, 8)[0], struct.unpack_from("<i", blob, 32)[0]
    pieces = [blob[o:blob.index(b"\0", o)] for o in struct.unpack_from(f"<{n_pieces}i", blob, off_pnames)]
    fold = lambda b: ascii_lower(b.decode("latin-1"))  # noqa: E731
    for i in range(min(len(pieces), len(prim))):
        j = next((j for j in range(i, len(prim)) if fold(prim[j]["name"]) == fold(pieces[i])), None)
        if j is not None:
            prim[i], prim[j] = prim[j], prim[i]
    return {"pieces": [n["name"].decode("latin-1") for n in prim],
            "visible": "".join("1" if len(n["verts"]) >= 3 else "0" for n in prim)}


# --------------------------------------------------------------------------- the reference

def entries_in_order():
    tree = json.loads((REPO / "art/wiki/tree.json").read_text())
    out = []

    def walk(nodes):
        for n in nodes:
            if "entry" in n and n["entry"] not in out:
                out.append(n["entry"])
            walk(n.get("children", []))
    walk(tree["tree"])
    return out


def stage_names(casting):
    """The 3D load's unit types: gfx.py's `stage_types` of every entry, in the tree's order, each name once over all."""
    import gfx  # noqa: E402  (tools/wiki, beside this file)
    names = []
    for eid in entries_in_order():
        for t in gfx.stage_types(json.loads((REPO / f"art/wiki/entries/{eid}.json").read_text()), casting):
            if t not in names:
                names.append(t)
    return names


def install_casting(gamedir, exe, language):
    """The casting `gfx.py --side-only` gives the install, [] when it makes no sidebar pack: the casts the native load
    adds to its unit types, the sidebar loaded first as the DLL loads it."""
    with tempfile.TemporaryDirectory(prefix="assetcheck-cast-") as tmp:
        cmd = [sys.executable, str(HERE / "gfx.py"), "--side-only", "--mod", "install", "--gamedir", str(gamedir), "--out",
               tmp, "--language", language] + (["--exe", str(exe)] if exe else [])
        if subprocess.run(cmd, capture_output=True, text=True).returncode:
            return []
        return json.loads((Path(tmp) / "side/install/side.json").read_text())["casting"]


def reference(gamedir, exe, language, mounts=None, casting=None):
    exe_blob = Path(exe).read_bytes()
    names = {k: tavfs.exe_cstr(exe_blob, va) for k, va in NAME_VAS.items()}
    vfs = tavfs.Vfs(gamedir, exe)
    if mounts is not None:
        byname = {n.lower(): (n, a) for n, a in vfs.archives}
        vfs.archives = [byname[m.lower()] for m in mounts if m.lower() in byname]
    E = Engine(vfs, names, language)
    textable = {}
    models = []

    def load_model(d, name):
        p, hit = E.find_built(d, name, "3DO")
        if not hit:
            return p, None, None, f"no source holds {p}"
        m = Model(E.read(hit), textable)
        models.append(m)
        return p, hit[1], m, None

    rep = {"format": 1, "language": language, "names": names,
           "mounts": [{"archive": n.lower(), "refused": None} for n, _a in vfs.archives]}
    entries = entries_in_order()
    stages, units = [], []
    pending_models = []                     # (dict to fill, path, where, model, why) once bound

    # a stage per scene, entry by entry in the scenes' order, as the native load makes them
    scenes = [(eid, name) for eid in entries for name in json.loads((REPO / f"art/wiki/entries/{eid}.json").read_text())["scenes"]]
    for eid, name in scenes:
        desc = json.loads((REPO / f"art/wiki/stage/{eid}.{name}.json").read_text())
        st = {"entry": eid, "scene": name, "cells": desc["cells"], "sources": [], "tiles": [], "features": []}
        ok = True
        srcs = []
        for s in desc["sources"]:
            d, base = s["map"].split("\\", 1)
            ext = base.rsplit(".", 1)[1] if "." in base else ""
            p, hit = E.find_built(d, base, ext)
            rec = {"map": s["map"].lower(), "path": low(p) if hit else None, "from": low(hit[1]) if hit else None,
                   "tiles": s["tiles"], "ok": False, "why": None}
            tnt = None
            if hit:
                blob = E.read(hit)
                version, = struct.unpack_from("<I", blob, 0)
                gfx, count = struct.unpack_from("<II", blob, 0x14)
                if version in (0x2000, 0x1020) and count == s["tiles"] and gfx + count * 1024 <= len(blob):
                    rec["ok"], tnt = True, (blob, gfx, count)
            if not rec["ok"]:
                rec["why"], ok = "refused", False
            st["sources"].append(rec)
            srcs.append(tnt)
        for src, index, want in desc["tileset"]:
            tnt = srcs[src]
            rec = {"source": src, "index": index, "hash": want, "ok": False, "why": "refused"}
            if tnt and index < tnt[2]:
                o = tnt[1] + index * 1024
                if f"{fnv(FNV0, tnt[0][o:o + 1024]):08x}" == want:
                    rec["ok"], rec["why"] = True, None
            ok = ok and rec["ok"]
            st["tiles"].append(rec)
        # the feature sections: the first TDF of the enumeration holding each
        want = []
        for f in desc["features"]:
            if ascii_lower(f["name"]) not in [ascii_lower(w) for w in want]:
                want.append(f["name"])
        defs = {}
        if want:
            for path, where in E.enumerate(names["features"], "*.tdf", True):
                body = tascene.parse_tdf(vfs.read_from(path, where).decode("latin-1"))
                for w in want:
                    k = ascii_lower(w)
                    sec = body.get(w.lower())
                    if k not in defs and isinstance(sec, dict) and sec:
                        defs[k] = (path, where, sec)
                if len(defs) == len(want):
                    break
        resolved = {}
        n3d = 0
        for w in want:
            k = ascii_lower(w)
            if k not in defs:
                resolved[k] = {"tdf": None, "from": None, "kind": None, "ok": False, "why": "refused"}
                continue
            path, where, sec = defs[k]
            obj = sec.get("object")
            flags = 0 if obj is not None else 1
            for i, key in enumerate(FEATURE_BITS):
                if atoi(sec.get(key), 1 if key == "autoreclaimable" else 0):
                    flags |= 2 << i
            if k in ("dragonsteeth", "dragonsteeth_core", "fortification", "fortification_core"):
                flags |= 1 << 11
            r = {"tdf": low(path), "from": low(where), "kind": "3d" if obj is not None else "2d",
                 "footprint": [atoi(sec.get("footprintx"), 0) & 0xFFFF, atoi(sec.get("footprintz"), 0) & 0xFFFF],
                 "height": atoi(sec.get("height"), 0) & 0xFF, "flags": flags}
            if obj is not None:
                n3d += 1
                p, mwhere, m, why = load_model(names["objects3d"], obj[:31])
                r["model"] = {}
                pending_models.append((r["model"], p, mwhere, m, why))
                r["ok"], r["why"] = m is not None, None if m else "refused"
            else:
                fn = sec.get("filename") or ""
                p, hit = E.find_built(names["anims"], fn, "GAF")
                r.update(gaf=low(p) if hit else None, gaf_from=low(hit[1]) if hit else None, frames=None, shadow_frames=None)
                if hit:
                    g = Gaf(E.read(hit))
                    for key, field in (("seqname", "frames"), ("seqnameshad", "shadow_frames")):
                        v = sec.get(key)
                        so = g.find(v) if v else None
                        if so is not None:
                            r[field] = g.frames(so)
                r["ok"] = bool(hit) and (r["frames"] or 0) >= 1
                r["why"] = None if r["ok"] else "refused"
            resolved[k] = r
        for f in desc["features"]:
            r = dict(name=f["name"], cell=f["cell"], **resolved[ascii_lower(f["name"])])
            ok = ok and r["ok"]
            st["features"].append(r)
        st["features_3d"] = n3d
        st["ok"], st["why"] = ok and n3d <= 64, None if ok and n3d <= 64 else "refused"
        stages.append(st)

    for t in stage_names(casting):
        u = {"name": t, "fbi": None, "from": None, "unitname": None, "object": None, "model": None,
             "script": None, "script_from": None, "hides": None, "uncached": None, "posed": None, "facts": None,
             "ok": False, "why": "refused"}
        p, hit = E.find_built(names["units"], t, "FBI")
        if hit:
            u["fbi"], u["from"] = low(p), low(hit[1])
            sec = tascene.parse_tdf(E.read(hit).decode("latin-1")).get("unitinfo")
            if isinstance(sec, dict) and sec:
                un = (sec.get("unitname") or "")[:31]
                obj = (sec.get("objectname") if sec.get("objectname") is not None else un)[:31]
                u["unitname"], u["object"] = un or None, low(obj) or None
                mobile = atoi(sec.get("bmcode"), 1) != 0
                slant = not mobile and atoi(sec.get("digger"), 0) == 0
                u["soundcategory"] = sec.get("soundcategory")
                u["facts"] = {"mobile": mobile, "slant": slant,
                              "shadow": atoi(sec.get("noshadow"), 0) == 0 and
                              (slant or (atoi(sec.get("canhover"), 0) == 0 and atoi(sec.get("floater"), 0) == 0))}
                if ascii_lower(un) == ascii_lower(t):
                    mp, mwhere, m, why = load_model(names["objects3d"], obj)
                    u["model"] = {}
                    pending_models.append((u["model"], mp, mwhere, m, why))
                    u["ok"], u["why"] = m is not None, None if m else "refused"
                if un:
                    sp, shit = E.find_built(names["scripts"], un, "COB")
                    if shit:
                        u["script"], u["script_from"] = low(sp), low(shit[1])
                        if u["ok"]:
                            u["hides"] = create_hides(E.read(shit), m)
                            u["uncached"] = create_hides(E.read(shit), m, MARKS["uncached"])
                            u["posed"] = posed(E.read(shit), m)
        units.append(u)

    # the bank: textures\*.GAF in the enumeration's order, logos.GAF aside; the count is the listing's less one
    listed = E.enumerate(names["textures"], "*.GAF", False)
    bank_names = [p.rsplit("\\", 1)[1] for p, _w in listed if ascii_lower(p.rsplit("\\", 1)[1]) != ascii_lower(names["logos_gaf"])]
    bank_names = bank_names[:max(len(listed) - 1, 0)]
    bank = []
    for i, n in enumerate(bank_names):
        p, hit = E.find_built(names["textures"], n, "GAF")
        g = {"gaf": p.lower(), "from": low(hit[1]) if hit else None, "refused": None if hit else "refused"}
        bank.append(g)
        if hit:
            gaf = Gaf(E.read(hit))
            for t in textable.values():
                if t.kind == "pending" and (so := gaf.find(t.name)) is not None:
                    t.frames, t.gaf = gaf.frames(so), i
                    t.kind = "miss" if not t.frames else "anim" if t.frames > 1 else "static"
    logos = None
    p, hit = E.find_built(names["textures"], names["logos"], "GAF")
    if hit:
        logos = {"gaf": p.lower(), "from": low(hit[1]), "refused": None}
        gaf = Gaf(E.read(hit))
        for t in textable.values():
            if t.kind == "pending" and (so := gaf.find(t.name)) is not None:
                t.frames, t.gaf = gaf.frames(so), -1
                t.kind = "miss" if not t.frames else "team" if t.frames == 10 else "anim" if t.frames > 1 else "static"
    for t in textable.values():
        if t.kind == "pending":
            t.kind = "miss"
    for m in models:
        bind(m)
    for slot, p, where, m, why in pending_models:
        slot.update(model_report(p, where, m, why, bank, logos))
    # the game's cursors: <anims>\cursors.GAF by the path rule, and the sequences each shape shows (WIKI_CURSOR_*)
    seqs = ["cursornormal", "cursormove", "cursornormal"]
    p, hit = E.find_built(names["anims"], "cursors", "GAF")
    cur = {"gaf": p.lower() if hit else None, "from": low(hit[1]) if hit else None,
           "sequences": [{"name": n, "frames": None} for n in seqs], "ok": False, "why": "refused"}
    if hit:
        gaf = Gaf(E.read(hit))
        for s in cur["sequences"]:
            so = gaf.find(s["name"])
            s["frames"] = gaf.frames(so) if so is not None else None
        cur["ok"] = all(s["frames"] is not None for s in cur["sequences"])
        cur["why"] = None if cur["ok"] else "refused"
    # THE SOUND (tagpu_wikiasset.c sound_load, gfx.py `unit_sound` and `write_sounds`): each resolved unit's category
    # over <gamedata>\sound.TDF read by position, the categories in use in the units' order with their variants' WAVs for
    # the kinds the wiki says, and the button sounds' through allsound.TDF; a WAV gfx.py's PCM rule refuses is null
    gd = tavfs.exe_cstr(exe_blob, 0x503360)

    def sections(name):
        _p, hit = E.find_built(gd, name, "TDF")
        try:
            return tavfs.tdf_sections(E.read(hit)) if hit else []
        except tavfs.TdfError:
            return []

    def variants(keys, key):
        out = [keys[key][:63]] if key in keys else []
        i = 1
        while f"{key}{i}" in keys:
            out.append(keys[f"{key}{i}"][:63])
            i += 1
        return out

    def clip(name):
        if not name:
            return None
        _p, hit = E.find_built("sounds", name, "WAV")
        return name.lower() if hit and gfxpy.wav_pcm(E.read(hit)) is None else None

    table = sections("sound")
    snd = {"categories": len(table), "cats": [], "named": {}}
    for u in units:
        sc = u.pop("soundcategory", None)
        if not u["ok"]:
            u["sound"] = None
            continue
        if sc is None:
            cat = 0
        else:
            sc = sc[:99]
            cat = next((i for i, t in enumerate(table) if t["_name"][:63].lower() == sc.lower()), None)
            cat = atoi(sc, 0) if cat is None else cat
        cat = cat if 0 <= cat < len(table) else -1
        u["sound"] = {"cat": cat, "count": [0] + [len(variants(table[cat]["keys"], k)) if cat >= 0 else 0
                                                  for k in gfxpy.SND_KEYS[1:]]}
        if cat >= 0 and all(c["index"] != cat for c in snd["cats"]):
            snd["cats"].append({"index": cat, "kinds": {str(k): [clip(w) for w in variants(table[cat]["keys"], gfxpy.SND_KEYS[k])]
                                                        for k in gfxpy.SND_WIKI_KINDS}})
    allsound = {}
    for sec in sections("allsound"):
        nm = sec["_name"][:31].lower()
        if nm not in allsound and len(allsound) < 255:
            allsound[nm] = sec["keys"].get("sound", "")[:255]
    snd["named"] = {n: clip(allsound.get(n.lower())) for n in gfxpy.SND_NAMES}
    for u in units:
        u.pop("soundcategory", None)
        u.setdefault("sound", None)
    rep.update(bank=bank, logos=logos, cursors=cur, stages=stages, units=units, sound=snd)
    return rep


# --------------------------------------------------------------------------- the comparison

def pak_of_tree():
    """art/wiki/ packed as the DLL packs it, into a file that lasts the process."""
    tmp = Path(tempfile.mkdtemp(prefix="assetcheck-"))
    atexit.register(shutil.rmtree, tmp, True)
    pak = tmp / "wiki.bin"
    subprocess.run([sys.executable, str(REPO / "tools/hudpak.py"), "--set", "wiki", str(REPO / "art/wiki"), str(pak)], check=True)
    return pak


def diff(a, b, path="", out=None, exact=False):
    """Where two reports differ, `arena_bytes` aside (the arena's own structs hold pointers, 4 bytes
    in the DLL and 8 natively). Between two runs of the C load (`exact`) a refusal's words must
    agree as well; against the Python reference only whether there is one."""
    out = [] if out is None else out
    if not exact and (path.endswith(".why") or path.endswith(".refused")):
        if (a is None) != (b is None):
            out.append(f"{path}: {a!r} != {b!r}")
        return out
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k == "arena_bytes" and not path:
                continue
            if k not in a or k not in b:
                out.append(f"{path}.{k}: only in {'C' if k in a else 'Python'}")
            else:
                diff(a[k], b[k], f"{path}.{k}", out, exact)
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}: {len(a)} items != {len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            diff(x, y, f"{path}[{i}]", out, exact)
    elif a != b:
        out.append(f"{path}: {a!r} != {b!r}")
    return out


def against_gfx(native, gamedir, exe):
    """The mockup's own resolution (tools/wiki/gfx.py --resolve-json, landing 6d) element by
    element against the native report: each map's path, archive and tile count, each tile's hash,
    each feature's TDF, archive and GAF, each unit's FBI, model and script with their archives and
    its hidden pieces. gfx.py writes its packs into a temporary folder, never the mockup's."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "resolve.json"
        cmd = [sys.executable, str(HERE / "gfx.py"), "--gamedir", str(gamedir), "--out", str(Path(tmp) / "packs"),
               "--resolve-json", str(out)] + (["--exe", str(exe)] if exe else [])
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        resolved = json.loads(out.read_text())
        gfx = resolved["entries"]
    bad, n = [], 0
    stages = {f"{s['entry']}.{s['scene']}": s for s in native["stages"]}
    units = {u["name"]: u for u in native["units"]}

    def same(where, a, b):
        if (a.lower() if isinstance(a, str) else a) != (b.lower() if isinstance(b, str) else b):
            bad.append(f"{where}: native {a!r} != gfx.py {b!r}")

    # the game's cursors: the GAF, its source, each sequence's frames
    nc, gc = native.get("cursors") or {}, resolved.get("cursors") or {}
    n += 1
    same("cursors gaf", nc.get("gaf"), gc.get("gaf")); same("cursors from", nc.get("from"), gc.get("from"))
    same("cursors sequences", nc.get("sequences"), gc.get("sequences"))

    for eid, els in gfx.items():
        st = stages.get(eid)
        if st is None:
            bad.append(f"{eid}: no stage in the native report")
            continue
        maps = iter(st["sources"])
        feats = iter(st["features"])
        for e in els:
            n += 1
            w = f"{eid} {e['element']}"
            if e["element"] == "map":
                m = next(maps, {})
                same(w + " path", m.get("path"), e["path"]); same(w + " from", m.get("from"), e["from"])
                same(w + " tiles", m.get("tiles"), e["tiles"])
            elif e["element"] == "tile":
                t = st["tiles"][e["row"]]
                same(w + " source", t["source"], e["source"]); same(w + " index", t["index"], e["tile"])
                same(w + " hash", t["hash"], e["hash"])
            elif e["element"] == "feature":
                f = next(feats, {})
                same(w + " name", f.get("name"), e["name"]); same(w + " cell", f.get("cell"), e["cell"])
                same(w + " tdf", (f.get("tdf") or "").replace("\\", "/"), e["tdf"].replace("\\", "/"))
                same(w + " from", f.get("from"), e["from"])
                same(w + " gaf", f.get("gaf"), e.get("gaf")); same(w + " gaf_from", f.get("gaf_from"), e.get("gafFrom"))
            elif e["element"] == "unit":
                u = units.get(e["name"], {})
                same(w + " fbi", u.get("fbi"), e["fbi"]); same(w + " from", u.get("from"), e["fbiFrom"])
                same(w + " model", (u.get("model") or {}).get("path"), e["model"])
                same(w + " model_from", (u.get("model") or {}).get("from"), e["modelFrom"])
                same(w + " script", u.get("script"), e["script"]); same(w + " script_from", u.get("script_from"), e["scriptFrom"])
                same(w + " hidden", sorted(p.lower() for p in (u.get("hides") or {}).get("pieces", [])),
                     sorted(p.lower() for p in e["hidden"]))
                same(w + " uncached", sorted(p.lower() for p in (u.get("uncached") or {}).get("pieces", [])),
                     sorted(p.lower() for p in e["uncached"]))
                same(w + " facts", u.get("facts"), e["facts"])
                same(w + " posed", [p.lower() for p in (u.get("posed") or {}).get("pieces", [])] or None,
                     [p.lower() for p in (e.get("posed") or {}).get("pieces", [])] or None)
                same(w + " visible", (u.get("posed") or {}).get("visible") or None, (e.get("posed") or {}).get("visible") or None)
    return bad, n


# --------------------------------------------------------------------------- the sidebar, per compat setup

SIDE_FOLDERS = ("gui", "unitpics", "gamedata", "download", "anims", "commongui", "units")


def fnv_hex(data):
    return f"{fnv(FNV0, data):08x}"


def pack_side(out, mod):
    """gfx.py's side pack of `mod` under `out`, in the shape the native report is read into (native_side)."""
    d = out / "side" / mod
    pack = json.loads((d / "side.json").read_text())
    art = pack["art"]
    sheet, sw = (d / art["sheet"]["file"]).read_bytes(), art["sheet"]["w"]

    def frame(f):
        if f is None:
            return None
        sx, sy, w, h, hx, hy = f
        return {"w": w, "h": h, "hx": hx, "hy": hy,
                "fnv": fnv_hex(b"".join(sheet[(sy + r) * sw + sx:(sy + r) * sw + sx + w] for r in range(h)))}

    def low_(v):
        return v.lower() if isinstance(v, str) else v

    screens = {}
    for name, a in art["screens"].items():
        pan = a["panel"]
        pics = []
        for q in a["pics"]:
            label = q["from"] == "label"
            pics.append({"from": q["from"], "path": low_(q.get("path")), "where": low_(q.get("where")),
                         "seq": q.get("seq"), "frames": None if label else [frame(f) for f in q["frames"]],
                         "key": None if label else q.get("key"),
                         "text": q.get("text") if q["from"] in ("label", "fallback") else None, "attribs": q["attribs"]})
        screens[name] = {"file": a["file"], "path": low_(a["path"]), "where": low_(a["where"]),
                         "panel": None if pan is None else {"frame": frame(pan["f"]), "key": pan["key"], "from": pan["from"],
                                                           "path": low_(pan["path"]), "where": low_(pan["where"])},
                         "pics": pics, "labels": a["labels"]}
    rep = {r["element"]: r for r in pack["report"] if isinstance(r, dict) and "element" in r}
    mm = rep.get("minimap", {})
    dots = art["dots"]
    bases = {}
    for eid, b in art["minimap"]["bases"].items():
        bases[eid] = {"w": b["w"], "h": b["h"], "offx": b["offx"], "offy": b["offy"], "fnv": fnv_hex((d / b["file"]).read_bytes())}
    layout = {k: v for k, v in pack["layout"].items() if k != "note"}
    return {"refused": None, "layout": layout, "folders": {k: pack["folders"][k] for k in SIDE_FOLDERS},
            "sheet": {"w": sw, "h": art["sheet"]["h"], "fnv": fnv_hex(sheet)}, "pal": fnv_hex((d / art["palette"]).read_bytes()),
            "panelside": [None if ps is None else {"frame": frame(ps["f"]), "key": ps["key"]} for ps in art["panelside"]],
            "screens": screens,
            "dots": {"radlogo": [frame(f) for f in dots["radlogo"]] if dots["radlogo"] else None,
                     "high": frame(dots["high"]), "key": dots["key"]},
            "minimap": {"unexplored": art["minimap"]["unexplored"], "box": art["minimap"]["box"],
                        "gui": low_((mm.get("gui") or {}).get("path")), "seen": fnv_hex((d / art["minimap"]["seen"]).read_bytes()),
                        "alp": low_((mm.get("alp") or {}).get("path"))},
            "greyed": fnv_hex((d / art["greyed"]).read_bytes()) if art.get("greyed") else None,
            "casting": [casting_of(c) for c in pack["casting"]],
            "bases": bases}


def casting_of(c):
    """One side's casting as both halves compare it: the side, its commander and every cast (None where unbound),
    without the rule's `report`, whose prose only gfx.py writes."""
    return {"side": c["side"], "commander": c["commander"],
            "casts": {k: None if v is None else dict(v) for k, v in c["casts"].items()}}


def native_side(doc):
    """`wikiassettest side`'s output in pack_side's shape: a PANELSIDE per layout side, a casting per side."""
    s = doc["side"]
    if s.get("refused"):
        return {"refused": s["refused"]}
    art = s["art"]
    screens = {}
    for name, a in art["screens"].items():
        pics = []
        for q in a["pics"]:
            label = q["from"] == "label"
            pics.append({"from": q["from"], "path": None if label else q["path"], "where": None if label else q["where"],
                         "seq": None if label else q["seq"], "frames": None if label else q["frames"],
                         "key": None if label else q["key"], "text": q.get("text"), "attribs": q["attribs"]})
        screens[name] = {"file": a["file"], "path": a["path"], "where": a["where"], "panel": a["panel"], "pics": pics,
                         "labels": a["labels"]}
    mm = art["minimap"]
    return {"refused": None, "layout": s["layout"], "folders": s["folders"], "sheet": art["sheet"], "pal": art["pal"],
            "panelside": art["panelside"], "screens": screens, "dots": art["dots"],
            "minimap": {"unexplored": mm["unexplored"], "box": mm["box"], "gui": mm["gui"], "seen": mm["seen"],
                        "alp": mm["alp"]["path"]},
            "greyed": art["greyed"], "casting": [casting_of(c) for c in s["casting"]],
            "bases": {k: v for k, v in doc["minimap"].items() if v is not None}}


def against_sidepacks(mods, retail, pak, language="english"):
    """Section 6 of the header, per setup: (all equal, lines to print)."""
    sys.path.insert(0, str(REPO / "tools" / "compat"))
    import sidepacks  # noqa: E402  (tools/wiki, beside this file)
    import tacompat  # noqa: E402
    by_name = {s["name"]: s for s in tacompat.SETUPS}
    ok, lines = True, []
    for mod in mods:
        setup = by_name[sidepacks.SETUPS[mod]]
        missing = sorted({a["fixture"] for a in setup["add"] if tacompat.fixture_problems(a["fixture"])})
        if missing:
            lines.append(f"assetcheck: sidebar {mod}: CANNOT RUN, setup {setup['name']} needs fixtures {', '.join(missing)}")
            ok = False
            continue
        with tempfile.TemporaryDirectory(prefix="assetcheck-side-") as tmp:
            tree, out = Path(tmp) / "game", Path(tmp) / "packs"
            sidepacks.setup_tree(setup, retail, tree)
            exe = sidepacks.child(tree, tavfs.EXE_NAME)
            rec = sidepacks.RECORDS.get(mod)
            names = []
            if rec:
                names = ["--folders", ",".join(f"{k}={v}" for k, v in rec["folders"].items()), "--archive", rec["archive"]]
            g = subprocess.run([sys.executable, str(HERE / "gfx.py"), "--side-only", "--mod", mod, "--gamedir", str(tree),
                                "--exe", str(exe), "--out", str(out), "--language", language, *names],
                               capture_output=True, text=True)
            if g.returncode:
                lines.append(f"assetcheck: sidebar {mod}: gfx.py made no pack ({g.returncode}): {(g.stdout + g.stderr).strip()[-500:]}")
                ok = False
                continue
            r = subprocess.run([str(EXE), "side", str(tree), str(pak), "--exe", str(exe), "--language", language, *names],
                               capture_output=True, text=True)
            if r.returncode:
                lines.append(f"assetcheck: sidebar {mod}: wikiassettest side FAILED ({r.returncode}): {r.stderr.strip()[-2000:]}")
                ok = False
                continue
            doc = json.loads(r.stdout)
            ref = pack_side(out, mod)
            native = native_side(doc)
            # the 3D load's unit types, which a demo bound to any side's casting draws its models from
            native["units3d"] = doc["units"]
            ref["units3d"] = stage_names(json.loads((out / "side" / mod / "side.json").read_text())["casting"])
        bad = diff(native, ref, exact=True)
        if bad:
            ok = False
            lines.append(f"assetcheck: sidebar {mod}: the native load and gfx.py's pack DIFFER in {len(bad)} place(s):")
            lines += [f"  {b}" for b in bad[:40]]
            lines += [f"  the native load refused: {e.get('element')}: {e['refused']}"
                      for e in doc["side"].get("report", []) if isinstance(e, dict) and "refused" in e]
            continue
        lay = native["layout"]
        npic = sum(len(s["pics"]) for s in native["screens"].values())
        srcs = {}
        for scr in native["screens"].values():
            for q in scr["pics"]:
                srcs[q["from"]] = srcs.get(q["from"], 0) + 1
        types = "; ".join(f"{sd['name']} {', '.join(f'{t} ({len(v['pages'])})' for t, v in sd['types'].items()) or 'none'}"
                          for sd in lay["sides"])
        lines.append(f"assetcheck: sidebar {mod}: native == gfx.py: {len(lay['screens'])} screens, types {types}, "
                     f"{len(native['casting'])} castings ({sum(1 for c in native['casting'] for k in c['casts'].values() if k)} "
                     f"casts bound), {len(native['units3d'])} 3D unit types, {npic} pictures "
                     f"({', '.join(f'{k} {v}' for k, v in sorted(srcs.items()))}), a {native['sheet']['h']}-row sheet byte "
                     f"for byte, {len(native['bases'])} minimap bases; {r.stderr.strip().splitlines()[-1].split(': ', 1)[-1]}")
    return ok, lines


def run_load(gamedir, pak, mounts=None, exe=None, language="english"):
    cmd = [str(EXE), "load", str(gamedir), str(pak)]
    if mounts:
        cmd += ["--mounts", str(mounts)]
    if exe:
        cmd += ["--exe", str(exe)]
    cmd += ["--language", language]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"assetcheck: wikiassettest load failed ({r.returncode}): {r.stderr.strip()[-2000:]}")
    return json.loads(r.stdout), r.stderr.strip()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--gamedir")
    p.add_argument("--exe", help="the executable whose names and pattern apply (default GAMEDIR/TotalA.exe)")
    p.add_argument("--mounts", help="the mount list, one archive a line (default: InitTAHPIAry's over GAMEDIR)")
    p.add_argument("--report", help="write the native report here and stop (no reference, no fuzz)")
    p.add_argument("--language", default="english")
    p.add_argument("--fuzz", type=int, default=1000, help="damaged copies per fuzz (default 1000)")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--side", action="store_true", help="only the sidebar: section 6 and its fuzz")
    p.add_argument("--no-side", action="store_true", help="leave section 6 out")
    p.add_argument("--mods", default="", help="m1,m2,...: section 6's setups (default: every one sidepacks.py knows)")
    a = p.parse_args()
    gamedir = tavfs.find_install(a.gamedir)
    exe = Path(a.exe) if a.exe else None
    if not a.no_build:
        build()
    if a.side:
        return side_main(a, gamedir)
    native, log = run_load(gamedir, pak_of_tree(), a.mounts, exe, a.language)
    print(f"assetcheck: native {log.splitlines()[-1] if log else ''}")
    if a.report:
        Path(a.report).write_text(json.dumps(native, indent=1) + "\n")
        return 0
    mounts = None
    if a.mounts:
        mounts = [ln.strip() for ln in Path(a.mounts).read_text().splitlines() if ln.strip()]
    ref = reference(gamedir, exe or Path(gamedir) / "TotalA.exe", a.language, mounts, install_casting(gamedir, exe, a.language))
    bad = diff(native, ref)
    if bad:
        print(f"assetcheck: the native load and the Python reference DIFFER in {len(bad)} place(s):")
        for b in bad[:40]:
            print(f"  {b}")
        return 1
    nmodels = sum(1 for u in native["units"] if u["model"]) + sum(1 for s in native["stages"] for f in s.get("features", []) if f.get("model"))
    print(f"assetcheck: native == Python: {len(native['mounts'])} mounts, {len(native['bank'])} bank GAFs, "
          f"{len(native['stages'])} stages ({sum(s['ok'] for s in native['stages'])} grounds resolved), "
          f"{len(native['units'])} unit types ({sum(u['ok'] for u in native['units'])} resolved), {nmodels} models' shapes equal")
    if not a.mounts:
        bad, n = against_gfx(native, gamedir, exe)
        if bad:
            print(f"assetcheck: the native load and gfx.py --resolve-json DIFFER in {len(bad)} place(s):")
            for b in bad[:40]:
                print(f"  {b}")
            return 1
        print(f"assetcheck: native == gfx.py --resolve-json on {n} elements (paths, sources, tile hashes, hidden pieces, dont-caches, posed orders, FBI facts, the cursors)")
    if a.fuzz:
        # two small archives, one keyed with LZ77 chunks and one plain with zlib's, and a model
        g = Path(gamedir)
        for cmd in (["fuzz-hpi", str(g / "AFark.ufo"), str(a.fuzz), "1"],
                    ["fuzz-hpi", str(g / "CorNecro.ufo"), str(a.fuzz), "3"],
                    ["fuzz-3do", str(g / "totala1.hpi"), "objects3d\\armck.3do", str(a.fuzz), "2"]):
            r = subprocess.run([str(EXE), *cmd], capture_output=True, text=True)
            print(r.stdout.rstrip())
            if r.returncode:
                print(f"assetcheck: {cmd[0]} FAILED ({r.returncode}): {r.stderr.strip()[-3000:]}")
                return 1
    if not a.no_side:
        return side_main(a, gamedir)
    print("assetcheck: passed")
    return 0


def cast_rule_case():
    """gfx.py's cast rule (`side_casting`) over fuzz-side's synthetic units, menus as its install gives them: the
    tower ARMWIN makes energy and stands before ARMSOLAR on the builder's menu, so the solar must skip it for a unit
    of its own. fuzz-side checks the C rule on the same install. True when every cast is the expected unit."""
    import gfx  # noqa: E402
    base = dict(bmcode=0, maxvelocity=0, fx=2, fz=2, builder=0, workertime=0, canfly=0, minwaterdepth=None,
                extractsmetal=0, g=[], energymake=0, energyuse=0, weapon1=False, fort=False)
    mobile = dict(bmcode=1, maxvelocity=1)
    spec = {"ARMCOM": dict(mobile, builder=1, workertime=300), "ARMCK": dict(mobile, builder=1, workertime=80),
            "ARMACK": dict(mobile, builder=1, workertime=180), "ARMPW": dict(mobile, fx=1, fz=1, weapon1=True),
            "ARMMEX": dict(extractsmetal=.001), "ARMMOHO": dict(fx=3, fz=3, extractsmetal=.003),
            "ARMGEO": dict(fx=4, fz=4, g=[[1, 1]], energymake=250, energyuse=1),
            "ARMWIN": dict(weapon1=True, energymake=5), "ARMDRAG": dict(fort=True),
            "ARMSOLAR": dict(fx=5, fz=5, energyuse=-20)}
    units = {k.lower(): {**base, "name": k, **v} for k, v in spec.items()}
    menus = {"armcom": ["armck", "armack", "armpw"], "armck": ["armmex", "armwin", "armgeo", "armdrag", "armsolar"],
             "armack": ["armmoho"]}
    picks, _why = gfx.side_casting(units, "ARMCOM", lambda t: menus.get(t, []))
    want = {"builder": "armck", "advbuilder": "armack", "mex": "armmex", "moho": "armmoho", "geo": "armgeo",
            "walker": "armpw", "tower": "armwin", "wall": "armdrag", "solar": "armsolar"}
    if picks != want:
        print(f"assetcheck: gfx.py's cast rule over the synthetic units FAILED: {picks}")
        return False
    print("assetcheck: gfx.py's cast rule over the synthetic units: every cast its own unit, the solar past the tower")
    return True


def side_fuzz(n):
    """`wikiassettest fuzz-side`: synthetic files only, so it runs without a game"""
    with tempfile.TemporaryDirectory(prefix="assetcheck-fuzz-") as tmp:
        r = subprocess.run([str(EXE), "fuzz-side", tmp, str(n), "5"], capture_output=True, text=True)
    print(r.stdout.rstrip())
    if r.returncode:
        print(f"assetcheck: fuzz-side FAILED ({r.returncode}): {r.stderr.strip()[-3000:]}")
    return r.returncode == 0


def side_main(a, gamedir):
    sys.path.insert(0, str(HERE))
    import sidepacks  # noqa: E402
    mods = [m for m in a.mods.split(",") if m] or list(sidepacks.SETUPS)
    unknown = [m for m in mods if m not in sidepacks.SETUPS]
    if unknown:
        print(f"assetcheck: no setup for {', '.join(unknown)} (the mods: {', '.join(sidepacks.SETUPS)})")
        return 2
    ok, lines = against_sidepacks(mods, gamedir, pak_of_tree(), a.language)
    print("\n".join(lines))
    if a.fuzz and not side_fuzz(a.fuzz):
        return 1
    if not cast_rule_case():
        return 1
    if not ok:
        print("assetcheck: the sidebar FAILED")
        return 1
    print("assetcheck: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
