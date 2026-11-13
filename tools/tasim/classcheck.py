#!/usr/bin/env python3
"""classcheck.py -- the shared core's feature grid and class maps against the engine's own, from one running game.

    tools/tasim/classcheck.py <instance> --map NAME [--schema N] [--gamedir DIR] [--no-launch] [--show N]

THE CORE'S SIDE. The map's files are read with the repository's readers (tools/wiki/tavfs.py for the archives and the
engine's TDF grammar, tascene's Tnt for the TNT) and handed as values to tools/tasim/classmap.c, which runs the core's
rules on them (tasim_map.c's LoadMap, tasim_class.c's fill) and prints the grid and every present class's map:

  - the TNT `maps/<NAME>.tnt`: W and H in 16-px cells, the sea level (the header's +0x24, low byte), the attribute
    records, the feature name table;
  - the OTA `maps/<NAME>.ota`: GlobalHeader `lavaworld`, and from [Schema N] (0 by default) `SurfaceMetal` and the
    [features] subsections (`Featurename`, `XPos`, `ZPos`, each integer as atol reads it, the positions -1 when
    absent and a record with either negative placing nothing);
  - the FeatureDefs in the engine's order: the TNT's names as 0x421F20 loads them, one LoadFeature 0x4224B0 each, then
    each schema feature whose name no def has yet (0x423160); a def's section is the first `features/**.tdf` the
    archives enumerate that holds it, its keys as tavfs.tdf_sections reads them;
  - `gamedata/moveinfo.tdf`'s [CLASS0]..[CLASS31], each the first section of its name.

THE ENGINE'S SIDE. Under the run lock (tools/runlock.py), one launch: `tacli scenario load <instance> FILE --defaults
--restart` on a scenario written here -- the map, every unit cleared (UNITS_KillUnit mode 0: no explosion, no wreck),
its features kept, the objective off, seat 1 an AI seat with its AI off -- and `tacli peek` reads:

  - main = *0x511DE8: W +0x14233, H +0x14237, the sea level +0x1427F, the grid *(+0x14287), 13 bytes a cell;
  - the schema record *(main+0x391E9): SurfaceMetal +0xD30, lavaworld +0xD44, its feature count +0xDC0 -- the files'
    values for the schema chosen, or the comparison is of another schema and cannot be made;
  - the FeatureDefs *(main+0x1426F), 0x100 each, *(main+0x14253) of them: the first names must be the core's, in order;
  - the 32 class records at 0x512358, 0x20 each: W +0x10, H +0x14, the map *(+0x18), the watermark +0x1C, and each
    present class's map, ((H + 15) >> 4) * W dwords, entry (x, z) at bits 2 (z & 15) of dword (z >> 4) * W + x.

THE COMPARISON. Every entry of every present class, the core's fill against the engine's, and the grid it was filled
from. It holds only for maps no search has touched since the fill 0x440940 (every watermark 0) and with no unit on
the grid (every slot 0), which the scenario arranges and the check confirms. The grid is compared field by field:
height +4, metal +7, the feature word +8, the back-reference +0xA only where the core wrote it (an anchor or a
footprint cell), the flags +0xC under 0x7B (bit 2 is the draw's, bit 7 nobody's), the 2x2 extremes +5/+6 except on the
last column and row. Those LoadMap never writes (0x483210 clamps its far ends to W-1/H-1): the core holds 0 there,
the engine whatever its allocator left, and the fill reads them. The check reports the engine's strip; when a class
map differs, it runs the core again with the engine's strip values and says whether every difference was theirs.

--no-launch peeks the level already running. --show N prints at most N differences a kind (default 20).
Exit 0 when every class map entry and grid field equals; 1 when one differs (or a FeatureDef is out of order or a
class's presence or footprint differs); 2 when the files, the launch or a peek cannot be had, or the engine's level
is not the fill's (a watermark, a unit, another schema: the schema record's feature count is taken to be the number of
its [features] subsections [INFERRED: 0x436C30 stores one record each]).
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
DDRAW = TREE / "tagpu" / "ddraw"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "wiki"))

TACLI = TREE / "tools" / "tacli"
EXE = HERE / "classmap-native.exe"            # `.exe` so the tree's *.exe ignore covers it
SRCS = ["src/tasim_class.c", "src/tasim_map.c", "src/tasim_keys.c", "src/tasim_fixed.c"]
DEPS = [DDRAW / s for s in SRCS] + [DDRAW / f"src/{h}" for h in ("tasim_class.h", "tasim_map.h", "tasim_keys.h",
                                                                   "tasim_fixed.h")] + \
       [DDRAW / "inc" / "tagpu_limits.h", HERE / "classmap.c"]
LOAD_S = 900                    # a launch to a live map, a cold prefix included
PEEK_S = 120
BATCH = 200                     # specs a peek trigger carries (its reader takes 4 KB)
PEEK_MAX = 256                  # bytes a spec reads (tagpu_peek.c PEEK_MAX)
MAIN = 0x511DE8
M_W, M_H, M_SEA, M_GRID = 0x14233, 0x14237, 0x1427F, 0x14287
M_FDEF, M_FCOUNT, M_SCHEMA = 0x1426F, 0x14253, 0x391E9
S_METAL, S_LAVA, S_NFEAT = 0xD30, 0xD44, 0xDC0
FDEF_SIZE = 0x100
CLASSES, CLASS_SIZE, NCLASS = 0x512358, 0x20, 32
CELL = 13
FLAGS_MASK = 0x7B


class CouldNotRun(Exception):
    pass


# ---- the core's side ---------------------------------------------------------------------------------------------

def atol(text):
    """The engine's integer getter 0x4C46C0 on a TDF value: C atol, leading blanks, a sign, the digits, the low 32
    bits as a signed int (scout-Jb J7, tasim_keys.c)."""
    s = text.lstrip(" \t\n\v\f\r")
    neg = s[:1] == "-"
    if s[:1] in "+-":
        s = s[1:]
    v = 0
    for ch in s:
        if not "0" <= ch <= "9":
            break
        v = (v * 10 + ord(ch) - 48) & 0xFFFFFFFF
    if neg:
        v = (-v) & 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _first(sections, name):
    return next((s for s in sections if s["_name"].lower() == name.lower()), None)


def feature_sections(vfs):
    """{lowercased name: keys} of every feature section, from the first `features/**.tdf` that holds it in the
    archives' enumeration order, as LoadFeature 0x4224B0 searches its list of files (0x4224C9..0x4224F3), and the first
    section of the name in that file."""
    import tavfs
    out = {}
    for path, where in vfs.enumerate("features", "*.tdf", True):
        try:
            secs = tavfs.tdf_sections(vfs.read_from(path, where))
        except tavfs.TdfError:
            continue
        for sec in secs:
            out.setdefault(sec["_name"].lower(), sec["keys"])
    return out


def class_sections(vfs):
    """{i: keys} of moveinfo.tdf's [CLASS<i>] for i in 0..31, each the first section of its name (0x42D328..0x42D3AA)."""
    import tavfs
    hit = vfs.open("gamedata/moveinfo.tdf")
    if hit is None:
        raise CouldNotRun("no gamedata/moveinfo.tdf in the install")
    secs = tavfs.tdf_sections(hit[0])
    out = {}
    for i in range(NCLASS):
        sec = _first(secs, f"CLASS{i}")
        if sec is not None:
            out[i] = sec["keys"]
    return out


def map_inputs(vfs, name, schema):
    """What LoadMap reads of the map `name`, from its TNT and OTA."""
    import tavfs
    tnt = vfs.open(f"maps/{name}.tnt")
    ota = vfs.open(f"maps/{name}.ota")
    if tnt is None or ota is None:
        raise CouldNotRun(f"maps/{name}.tnt or .ota is not in the install")
    try:
        t = tavfs.tascene.Tnt(tnt[0], name)
    except ValueError as e:
        raise CouldNotRun(f"the TNT: {e}")
    gh = _first(tavfs.tdf_sections(ota[0]), "GlobalHeader")
    if gh is None:
        raise CouldNotRun("the OTA has no [GlobalHeader]")
    sch = _first(gh["sub"], f"Schema {schema}")
    if sch is None:
        raise CouldNotRun(f"the OTA has no [Schema {schema}]")
    feats = []
    fsec = _first(sch["sub"], "features")
    for rec in (fsec["sub"] if fsec else []):
        k = rec["keys"]
        x, z = atol(k.get("xpos", "-1")), atol(k.get("zpos", "-1"))
        feats.append(("" if x < 0 or z < 0 else k.get("featurename", ""), x, z))
    return {"w": t.w16, "h": t.h16, "sea": t.sealevel & 0xFF, "version": t.version,
            "attr": t.blob[t.p_mapattr:t.p_mapattr + t.w16 * t.h16 * 4], "names": t.feature_names(),
            "surface": atol(sch["keys"].get("surfacemetal", "0")), "lava": atol(gh["keys"].get("lavaworld", "0")),
            "schema": feats, "where": tnt[1]}


def resolve_defs(names, schema, fsecs):
    """The FeatureDef table the level builds -- [(name, keys)] -- and the schema features as (def, x, z)."""
    defs = []
    for n in names:
        if n.lower() not in fsecs:
            raise CouldNotRun(f"the TNT names feature {n!r}, which no features/*.tdf holds (the engine stops there)")
        defs.append((n, fsecs[n.lower()]))
    placed = []
    for n, x, z in schema:
        if not n:
            placed.append((-1, x, z))
            continue
        i = next((k for k, (dn, _keys) in enumerate(defs) if dn.lower() == n.lower()), None)
        if i is None:
            if n.lower() not in fsecs:
                raise CouldNotRun(f"the schema names feature {n!r}, which no features/*.tdf holds")
            defs.append((n, fsecs[n.lower()]))
            i = len(defs) - 1
        placed.append((i, x, z))
    return defs, placed


def _hex(text):
    return text.encode("latin-1").hex()


def driver_input(inp, defs, placed, classes, strip=()):
    out = [f"map {inp['w']} {inp['h']} {inp['sea']} {inp['version']} {inp['surface']} {inp['lava']}",
           f"attr {inp['attr'].hex()}"]
    for n, keys in defs:
        out.append(f"fdef {_hex(n)}")
        out += [f"k {_hex(k)} {_hex(v)}" for k, v in keys.items()]
        out.append("end")
    out += [f"schema {d} {x} {z}" for d, x, z in placed]
    for i, keys in sorted(classes.items()):
        out.append(f"class {i}")
        out += [f"k {_hex(k)} {_hex(v)}" for k, v in keys.items()]
        out.append("end")
    out += [f"strip {c} {mx} {mn}" for c, mx, mn in strip]
    out.append("go")
    return "\n".join(out) + "\n"


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= d.stat().st_mtime for d in DEPS):
        return EXE
    print(f"classcheck: building {EXE.name}", file=sys.stderr, flush=True)
    subprocess.run(["gcc", "-std=c99", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-O2", "-Iinc", "-Isrc",
                    "-o", str(EXE), str(HERE / "classmap.c"), *SRCS, "-lm"], cwd=DDRAW, check=True)
    return EXE


def run_core(exe, text):
    r = subprocess.run([str(exe)], input=text, capture_output=True, text=True)
    if r.returncode != 0:
        raise CouldNotRun(f"classmap: {r.stderr.strip()}")
    core = {"classes": {}}
    for line in r.stdout.splitlines():
        f = line.split()
        if f[0] == "past_defs":
            core["past_defs"] = int(f[1])
        elif f[0] == "grid":
            core["grid"] = bytes.fromhex(f[1])
        elif f[0] == "class":
            core["classes"][int(f[1])] = {"fx": int(f[2]), "fz": int(f[3]), "wm": int(f[4]),
                                         "map": bytes.fromhex(f[5])}
    return core


# ---- the engine's side -------------------------------------------------------------------------------------------

def tacli(*args, timeout=PEEK_S):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True, text=True,
                       timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def tacli_peek(instance):
    """A peek function on the running instance: specs in, each spec's value text out, in order."""
    def peek(specs):
        out = []
        for i in range(0, len(specs), BATCH):
            part = specs[i:i + BATCH]
            rc, o, e = tacli("peek", instance, *part, "--json")
            if rc != 0:
                raise CouldNotRun(f"peek: {(o + e).strip()[-300:]}")
            rows = json.loads(o)["peeks"]
            if [r["spec"] for r in rows] != part:
                raise CouldNotRun(f"peek answered {len(rows)} of {len(part)} specs")
            out += [r["value"] for r in rows]
        return out
    return peek


def as_int(v):
    try:
        return int(v.split()[0])
    except (ValueError, IndexError):
        raise CouldNotRun(f"an unreadable value: {v!r}")


def as_str(v):
    return v[1:-1] if v.startswith('"') and v.endswith('"') else None


def read_bytes(peek, ranges):
    """[(address, size)] -> [bytes], each read in PEEK_MAX pieces; an unreadable piece cannot be compared."""
    specs, per = [], []
    for a, n in ranges:
        k = 0
        for off in range(0, n, PEEK_MAX):
            specs.append(f"0x{a + off:X}:x{min(PEEK_MAX, n - off):X}")
            k += 1
        per.append(k)
    vals, out, j = peek(specs), [], 0
    for (a, _n), k in zip(ranges, per):
        try:
            out.append(b"".join(bytes.fromhex(v.replace(" ", "")) for v in vals[j:j + k]))
        except ValueError:
            raise CouldNotRun(f"0x{a:X} is unreadable")
        j += k
    return out


def engine_read(peek, nfd):
    """The engine's level: its sizes, schema record, FeatureDef names, class records and maps, grid."""
    main = as_int(peek([f"0x{MAIN:X}:4"])[0])
    w, h, sea, grid, fbase, fcount, sch = (as_int(v) for v in peek(
        [f"0x{main + o:X}:4" for o in (M_W, M_H)] + [f"0x{main + M_SEA:X}:1"] +
        [f"0x{main + o:X}:4" for o in (M_GRID, M_FDEF, M_FCOUNT, M_SCHEMA)]))
    surface, lava, nsch = (as_int(v) for v in peek([f"0x{sch + o:X}:4" for o in (S_METAL, S_LAVA, S_NFEAT)]))
    surface -= (1 << 32) if surface & 0x80000000 else 0
    names = [as_str(v) for v in peek([f"0x{fbase + FDEF_SIZE * i:X}:s80" for i in range(min(nfd, fcount))])]
    recs = read_bytes(peek, [(CLASSES + CLASS_SIZE * i, CLASS_SIZE) for i in range(NCLASS)])
    classes = {}
    for i, r in enumerate(recs):
        if int.from_bytes(r[0:4], "little") == 0:
            continue
        cw, ch, cmap, wm = (int.from_bytes(r[o:o + 4], "little") for o in (0x10, 0x14, 0x18, 0x1C))
        classes[i] = {"fx": int.from_bytes(r[4:6], "little", signed=True),
                      "fz": int.from_bytes(r[6:8], "little", signed=True), "w": cw, "h": ch, "ptr": cmap, "wm": wm}
    maps = read_bytes(peek, [(c["ptr"], ((c["h"] + 15) >> 4) * c["w"] * 4) for c in classes.values()])
    for c, m in zip(classes.values(), maps):
        c["map"] = m
    cells = read_bytes(peek, [(grid, w * h * CELL)])[0]
    return {"w": w, "h": h, "sea": sea, "grid": cells, "fcount": fcount, "names": names, "surface": surface,
            "lava": lava, "nschema": nsch, "classes": classes}


def scenario(name):
    return {"format": "ta-scenario/1",
            "description": "tools/tasim/classcheck.py's level: the map with its own features, no unit, the objective "
                           "off, seat 1 an AI seat with its AI off, so nothing re-marks a class map after the fill.",
            "setup": {"map": name, "clear_existing": True, "clear_features": False, "objective": "off",
                      "players": [{"slot": 0, "controller": "human", "side": "arm", "color": 0},
                                  {"slot": 1, "controller": "ai", "side": "core", "color": 1, "ai": "off"}]},
            "units": []}


# ---- the comparison ----------------------------------------------------------------------------------------------

class Tally:
    def __init__(self, show):
        self.show, self.n, self.bad, self.lines = show, {}, {}, []

    def field(self, kind, ok, text):
        self.n[kind] = self.n.get(kind, 0) + 1
        if not ok:
            self.bad[kind] = self.bad.get(kind, 0) + 1
            if self.bad[kind] <= self.show:
                self.lines.append(f"  {kind}: {text()}")


def strip_cells(w, h):
    return [z * w + w - 1 for z in range(h)] + [(h - 1) * w + x for x in range(w - 1)]


def compare_grid(t, w, h, ours, theirs):
    strip = set(strip_cells(w, h))
    for c in range(w * h):
        a, b = ours[c * CELL:(c + 1) * CELL], theirs[c * CELL:(c + 1) * CELL]
        x, z = c % w, c // w
        at = f"({x}, {z})"
        t.field("grid height", a[4] == b[4], lambda: f"{at} core {a[4]} engine {b[4]}")
        if c not in strip:
            t.field("grid extremes", a[5:7] == b[5:7], lambda: f"{at} max/min core {a[5]}/{a[6]} engine {b[5]}/{b[6]}")
        t.field("grid metal", a[7] == b[7], lambda: f"{at} core {a[7]} engine {b[7]}")
        wa, wb = int.from_bytes(a[8:10], "little"), int.from_bytes(b[8:10], "little")
        t.field("grid feature", wa == wb, lambda: f"{at} word core {wa:04X} engine {wb:04X}")
        if wa == wb and (wa == 0xFFFE or wa < 0xFFFB):
            t.field("grid back", a[10:12] == b[10:12], lambda: f"{at} word {wa:04X} core {a[10:12].hex()} engine "
                                                               f"{b[10:12].hex()}")
        t.field("grid flags", a[12] & FLAGS_MASK == b[12] & FLAGS_MASK,
                lambda: f"{at} core {a[12] & FLAGS_MASK:02X} engine {b[12] & FLAGS_MASK:02X} (mask 7B)")


def class_diffs(w, h, ours, theirs):
    """[(x, z, core, engine)] for each entry z < h whose codes differ."""
    out = []
    for d in range(len(ours) // 4):
        a = int.from_bytes(ours[4 * d:4 * d + 4], "little")
        b = int.from_bytes(theirs[4 * d:4 * d + 4], "little")
        if a == b:
            continue
        x, z0 = d % w, (d // w) * 16
        for k in range(16):
            if z0 + k < h and (a >> 2 * k) & 3 != (b >> 2 * k) & 3:
                out.append((x, z0 + k, (a >> 2 * k) & 3, (b >> 2 * k) & 3))
    return out


def check(inp, defs, placed, classes, eng, exe, show):
    """The comparison on everything read: (exit code, report lines)."""
    lines = []
    w, h = inp["w"], inp["h"]
    pre = []
    if (eng["w"], eng["h"], eng["sea"]) != (w, h, inp["sea"]):
        pre.append(f"the engine's map is {eng['w']}x{eng['h']} sea {eng['sea']}, the TNT's {w}x{h} sea {inp['sea']}")
    if (eng["surface"], 1 if eng["lava"] else 0, eng["nschema"]) != \
            (inp["surface"], 1 if inp["lava"] else 0, len(inp["schema"])):
        pre.append(f"the engine's schema has SurfaceMetal {eng['surface']}, lavaworld {eng['lava']}, "
                   f"{eng['nschema']} features; the file's {inp['surface']}, {inp['lava']}, {len(inp['schema'])} "
                   f"(--schema names another)")
    wms = {i: c["wm"] for i, c in eng["classes"].items() if c["wm"]}
    if wms:
        pre.append(f"a search has run: watermarks {wms}")
    for i, c in eng["classes"].items():
        if (c["w"], c["h"]) != (w, h):
            pre.append(f"CLASS{i}'s map is {c['w']}x{c['h']}, not the grid's")
    g = eng["grid"]
    units = [c for c in range(len(g) // CELL) if g[c * CELL:c * CELL + 4] != bytes(4)]
    if units:
        pre.append(f"{len(units)} cell(s) hold a unit, the first ({units[0] % w}, {units[0] // w})")
    if pre:
        return 2, [f"classcheck: the level is not the fill's, no comparison: {p}" for p in pre]

    t = Tally(show)
    for k, (n, _keys) in enumerate(defs):
        theirs = eng["names"][k] if k < len(eng["names"]) else None
        t.field("feature defs", theirs is not None and theirs.lower() == n.lower(),
                lambda: f"def {k} core {n!r} engine {theirs!r}")
    core = run_core(exe, driver_input(inp, defs, placed, classes))
    if core.get("past_defs"):
        lines.append(f"classcheck: {core['past_defs']} TNT feature word(s) at or past the defs, placed nowhere")
    compare_grid(t, w, h, core["grid"], eng["grid"])
    strip = [(c, eng["grid"][c * CELL + 5], eng["grid"][c * CELL + 6]) for c in strip_cells(w, h)]
    set_strip = [s for s in strip if s[1] or s[2]]
    lines.append(f"classcheck: the last column and row: {len(set_strip)} of {len(strip)} cells hold extremes in the "
                 f"engine (LoadMap writes none; the core holds 0)")

    diffs = {}
    present = sorted(set(core["classes"]) | set(eng["classes"]))
    for i in present:
        a, b = core["classes"].get(i), eng["classes"].get(i)
        t.field("class present", a is not None and b is not None,
                lambda: f"CLASS{i} core {'present' if a else 'absent'} engine {'present' if b else 'absent'}")
        if a is None or b is None:
            continue
        t.field("class footprint", (a["fx"], a["fz"]) == (b["fx"], b["fz"]),
                lambda: f"CLASS{i} core {a['fx']}x{a['fz']} engine {b['fx']}x{b['fz']}")
        d = class_diffs(w, h, a["map"], b["map"])
        diffs[i] = d
        for x, z, ca, cb in d[:show]:
            lines.append(f"  CLASS{i} ({x}, {z}) core {ca} engine {cb}")
    entries = w * h
    for i, d in diffs.items():
        lines.append(f"classcheck: CLASS{i} ({core['classes'][i]['fx']}x{core['classes'][i]['fz']}): "
                     f"{entries - len(d)} of {entries} entries equal")
    nd = sum(len(d) for d in diffs.values())
    if nd and set_strip:
        again = run_core(exe, driver_input(inp, defs, placed, classes, set_strip))
        left = sum(len(class_diffs(w, h, again["classes"][i]["map"], eng["classes"][i]["map"])) for i in diffs)
        lines.append(f"classcheck: with the engine's strip given to the core, {left} of the {nd} differences remain"
                     + (" -- every difference is the strip's" if not left else ""))
    for kind in sorted(t.n):
        lines.append(f"classcheck: {kind}: {t.n[kind] - t.bad.get(kind, 0)} of {t.n[kind]} equal")
    lines += t.lines
    grid_bad = sum(v for k, v in t.bad.items() if k.startswith("grid"))
    bad = nd + sum(t.bad.values())
    lines.append(f"classcheck: {'FAIL' if bad else 'PASS'}, {nd} class map entr{'y' if nd == 1 else 'ies'} and "
                 f"{grid_bad} grid field(s) differ")
    return (1 if bad else 0), lines


def main():
    import tavfs
    from resource_regression import hold_instance, tacli_api
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("instance")
    p.add_argument("--map", required=True, help="the map's name, its TNT's stem (e.g. \"Etorrep Glacier\")")
    p.add_argument("--schema", type=int, default=0, help="the OTA's [Schema N] a skirmish plays (default 0)")
    p.add_argument("--gamedir", help="the install the core reads (default: the instance's game folder)")
    p.add_argument("--no-launch", action="store_true", help="peek the level already running")
    p.add_argument("--show", type=int, default=20)
    a = p.parse_args()
    gamedir = Path(a.gamedir) if a.gamedir else tacli_api().Instance(a.instance).gamedir
    try:
        vfs = tavfs.Vfs(gamedir)
        inp = map_inputs(vfs, a.map, a.schema)
        defs, placed = resolve_defs(inp["names"], inp["schema"], feature_sections(vfs))
        classes = class_sections(vfs)
        exe = build()
        print(f"classcheck: the core read {a.map!r} ({inp['where']}): {inp['w']}x{inp['h']}, sea {inp['sea']}, "
              f"{len(defs)} feature defs, {len(placed)} schema features, {len(classes)} classes")
        if not a.no_launch:
            hold_instance(f"classcheck on {a.instance}", a.instance)
            with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
                json.dump(scenario(a.map), f, indent=1)
            try:
                rc, o, e = tacli("scenario", "load", a.instance, f.name, "--defaults", "--restart", timeout=LOAD_S)
            finally:
                Path(f.name).unlink()
            if rc != 0:
                raise CouldNotRun(f"scenario load: {(o + e).strip()[-400:]}")
        eng = engine_read(tacli_peek(a.instance), len(defs))
        code, lines = check(inp, defs, placed, classes, eng, exe, a.show)
    except (CouldNotRun, tavfs.VfsError, subprocess.TimeoutExpired, subprocess.CalledProcessError, OSError) as e:
        print(f"classcheck: COULD NOT RUN: {e}")
        return 2
    print("\n".join(lines))
    return code


if __name__ == "__main__":
    sys.exit(main())
