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
   art/wiki/ packed as the DLL packs it (tools/hudpak.py --set wiki); its report is the DLL's tagpu_wiki3d.json.
3. **The reference**: the same questions answered here, from tools/wiki/tavfs.py's mount and tools/ta3do's HPI
   reader and tools/tascene's TDF reader, with the engine's rules written out a second time: the language's folder
   first, the shadow mark in every enumeration, the first TDF holding a section, units\\<name>.FBI by path, the 3DO
   built as the engine holds it (the selection face swapped to 0, faces 1.. bubble-sorted by mean Y, X and Z
   negated) and bound (the texture bank in its enumeration order, then logos.GAF, FF_TEAM for exactly 10 frames),
   and each model's shape hash over that. Every field must agree but `arena_bytes` (the C side's alone) and the
   wording of a refusal (only whether there is one).
4. **The fuzz**: `wikiassettest fuzz-hpi` on two archives (LZ77 and keyed, zlib and plain) and `fuzz-3do` on one
   model, 1000 damaged copies each;
   every refusal must be named and the sanitizers must stay silent.

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

ta3do = tavfs.ta3do
tascene = tavfs.tascene

SRCS = ["tests/wikiassettest.c", "src/tagpu_hpi.c", "src/tagpu_vfs.c", "src/tagpu_tdf.c", "src/tagpu_json.c",
        "src/tagpu_pak.c", "src/tagpu_wikidata.c"]
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


def reference(gamedir, exe, language, mounts=None):
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

    for eid in entries:
        desc = json.loads((REPO / f"art/wiki/stage/{eid}.json").read_text())
        st = {"entry": eid, "cells": desc["cells"], "sources": [], "tiles": [], "features": []}
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

    types = []
    for eid in entries:
        for t in json.loads((REPO / f"art/wiki/entries/{eid}.json").read_text())["units"]:
            if t not in types:
                types.append(t)
    for t in types:
        u = {"name": t, "fbi": None, "from": None, "unitname": None, "object": None, "model": None,
             "script": None, "script_from": None, "hides": None, "ok": False, "why": "refused"}
        p, hit = E.find_built(names["units"], t, "FBI")
        if hit:
            u["fbi"], u["from"] = low(p), low(hit[1])
            sec = tascene.parse_tdf(E.read(hit).decode("latin-1")).get("unitinfo")
            if isinstance(sec, dict) and sec:
                un = (sec.get("unitname") or "")[:31]
                obj = (sec.get("objectname") if sec.get("objectname") is not None else un)[:31]
                u["unitname"], u["object"] = un or None, low(obj) or None
                if ascii_lower(un) == ascii_lower(t):
                    mp, mwhere, m, why = load_model(names["objects3d"], obj)
                    u["model"] = {}
                    pending_models.append((u["model"], mp, mwhere, m, why))
                    u["ok"], u["why"] = m is not None, None if m else "refused"
                if un:
                    sp, shit = E.find_built(names["scripts"], un, "COB")
                    if shit:
                        u["script"], u["script_from"] = low(sp), low(shit[1])
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
    rep.update(bank=bank, logos=logos, stages=stages, units=units)
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
    a = p.parse_args()
    gamedir = tavfs.find_install(a.gamedir)
    exe = Path(a.exe) if a.exe else None
    if not a.no_build:
        build()
    native, log = run_load(gamedir, pak_of_tree(), a.mounts, exe, a.language)
    print(f"assetcheck: native {log.splitlines()[-1] if log else ''}")
    if a.report:
        Path(a.report).write_text(json.dumps(native, indent=1) + "\n")
        return 0
    mounts = None
    if a.mounts:
        mounts = [ln.strip() for ln in Path(a.mounts).read_text().splitlines() if ln.strip()]
    ref = reference(gamedir, exe or Path(gamedir) / "TotalA.exe", a.language, mounts)
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
    print("assetcheck: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
