#!/usr/bin/env python3
"""arenascan.py -- the calibration arena, derived from the install rather than written down.

    tools/tasim/arenascan.py scan [--gamedir DIR] [--top N] [--json]
    tools/tasim/arenascan.py pick [--gamedir DIR] [MAP]     # write the arena config and scenario
    tools/tasim/arenascan.py check [--gamedir DIR]          # the configured arena against the install

Every map the install resolves (tavfs: loose files first, then the archives in the engine's mount
order, each path from the first source that holds it) is graded on what the arena needs:

- METEORS OFF in every schema of its .ota, by the engine's own rule: the .ota reader switches the
  shower on (0x437D40 at 0x4367F9, `meteoron` 0x51232C = 1) only when MeteorWeapon is not empty
  (0x43675A) and MeteorRadius (an int, 0x43676A) and MeteorDensity, MeteorDuration and
  MeteorInterval (floats, 0x436780, 0x436796, 0x4367AC) are all non-zero (0x4367B9..0x4367EE);
  otherwise it switches it off (0x437D50 at 0x436800). With the switch off the shower tick 0x437DE0
  still schedules and draws its points, but clears `meteoractive` 0x512318 (0x437EF5..0x437EFE), so
  no stone falls (DISASSEMBLED, engine/weapons.md). Every schema is checked: which one a skirmish
  reads is not what this rests on;
- two start positions in schema 0, the two seats the arena's skirmish has;
- a TNT the readers take;
- the flat area: the rectangle of height samples (one per 16 world units, the TNT's attribute byte
  0) that all hold ONE height above the TNT's sea level, at least BORDER samples in from every
  edge. Among those whose short side spans at least MIN_SHORT world units, the one with the
  longest long side, then the largest area. A duel is laid out along the long side, so the long
  side bounds the farthest duel duelgen can place.

`pick` writes the arena config (tools/tasim/calib-arena.json) from the best map, or the one named,
and the map of its scenario; `check` re-derives the configured map's grade and fails when the
install no longer gives what the config says -- calib.py runs it before every `calib engine`, and
every lane reads the switch itself at its launch (engrun.py). A map is named by its file stem, the
spelling the skirmish list and `tacli scenario` match case-insensitively.
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools" / "wiki"))
sys.path.insert(0, str(HERE))
import tavfs  # noqa: E402
import calibref  # noqa: E402

CELL = 16               # world units per height sample
BORDER = 8              # samples kept clear of the map's edge
MIN_SHORT = 1600        # world units across: the courses' lanes (MARGIN each side, four lanes)
MARGIN = 256            # world units from the rectangle's ends to a and b, and from its sides
SCAN_FIELDS = ("map", "flat", "axis", "a", "b", "courses")
COURSE_ORDER = ("kbot", "tank", "hover", "amphibious")

_INT = re.compile(r"\s*[-+]?\d+")
_FLOAT = re.compile(r"\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?")


def _int(s):
    m = _INT.match(s or "")
    return int(m.group()) if m else 0


def _float(s):
    m = _FLOAT.match(s or "")
    return float(m.group()) if m else 0.0


def meteors_on(keys: dict) -> bool:
    """The .ota reader's test for one schema (the module header)."""
    return (bool((keys.get("meteorweapon") or "").strip()) and _int(keys.get("meteorradius")) != 0
            and _float(keys.get("meteordensity")) != 0.0
            and _float(keys.get("meteorduration")) != 0.0
            and _float(keys.get("meteorinterval")) != 0.0)


def schemas(ota: bytes):
    """[(schema section name, its keys, its subsections)] of the .ota's GlobalHeader."""
    out = []
    for top in tavfs.tdf_sections(ota):
        for sub in top["sub"]:
            if sub["_name"].lower().startswith("schema"):
                out.append((sub["_name"], sub["keys"], sub["sub"]))
    return out


def start_positions(subs) -> int:
    n = 0
    for s in subs:
        if s["_name"].lower() == "specials":
            n += sum(1 for sp in s["sub"]
                     if (sp["keys"].get("specialwhat") or "").lower().startswith("startpos"))
    return n


def flat_rect(tnt, border=BORDER, min_short=MIN_SHORT):
    """(i0, j0, ni, nj, height): the best one-height dry rectangle of samples (the module header),
    or None. Every maximal rectangle is some row's bar extended left and right across columns of
    the same height, so the stack walk over each row's runs of one height sees them all."""
    w, h = tnt.w16, tnt.h16
    hs = tnt.heights()
    sea = tnt.sealevel
    x0, x1, y0, y1 = border, w - border, border, h - border
    if x1 - x0 < 2 or y1 - y0 < 2:
        return None
    min_n = min_short // CELL + 1
    best, best_key = None, None
    up = [0] * w
    prev = None
    for j in range(y0, y1):
        row = hs[j * w:(j + 1) * w]
        for i in range(x0, x1):
            v = row[i]
            if v <= sea:
                up[i] = 0
            elif prev is not None and up[i] and prev[i] == v:
                up[i] += 1
            else:
                up[i] = 1
        prev = row
        i = x0
        while i < x1:
            v = row[i]
            if up[i] == 0:
                i += 1
                continue
            k = i
            while k < x1 and row[k] == v and up[k]:
                k += 1
            stack = []                      # (start column, bar height)
            for c in range(i, k + 1):
                hc = up[c] if c < k else 0
                start = c
                while stack and stack[-1][1] >= hc:
                    s, bh = stack.pop()
                    ni, nj = c - s, bh
                    short, long_ = sorted((ni, nj))
                    if short >= min_n:
                        key = (long_, ni * nj)
                        if best_key is None or key > best_key:
                            best_key, best = key, (s, j - nj + 1, ni, nj, v)
                    start = s
                if hc:
                    stack.append((start, hc))
            i = k
    return best


def grade(vfs, path, where):
    """One map's grade: a dict with `name`, `why` (None when it qualifies) and its rectangle."""
    name = Path(path).stem
    out = {"name": name, "ota": f"{path} ({where})", "why": None, "rect": None}
    ota = vfs.read_from(path, where)
    sch = schemas(ota)
    if not sch:
        out["why"] = "no schema"
        return out
    on = [s for s, keys, _sub in sch if meteors_on(keys)]
    if on:
        out["why"] = f"meteors on in {', '.join(on)}"
        return out
    if start_positions(sch[0][2]) < 2:
        out["why"] = "fewer than two start positions in schema 0"
        return out
    tnt_path = path[:-4] + ".tnt"
    hit = vfs.open(tnt_path)
    if not hit or not hit[0]:
        out["why"] = "no .tnt"
        return out
    try:
        tnt = tavfs.tascene.Tnt(hit[0], tnt_path)
    except ValueError as e:
        out["why"] = f"the .tnt: {e}"
        return out
    r = flat_rect(tnt)
    if r is None:
        out["why"] = f"no dry one-height rectangle {MIN_SHORT} world units across"
        return out
    i0, j0, ni, nj, v = r
    out["rect"] = [i0 * CELL, j0 * CELL, (ni - 1) * CELL, (nj - 1) * CELL]
    out["height"], out["sealevel"] = v, tnt.sealevel
    out["long"] = max(out["rect"][2], out["rect"][3])
    return out


def install(gamedir=None):
    return tavfs.Vfs(tavfs.find_install(gamedir))


def scan(vfs):
    rows = [grade(vfs, p, w) for p, w in vfs.enumerate("maps", "*.ota", False)]
    good = sorted((r for r in rows if r["why"] is None),
                  key=lambda r: (-r["long"], -r["rect"][2] * r["rect"][3], r["name"].lower()))
    return good, [r for r in rows if r["why"] is not None]


def arena_fields(row):
    """The arena config's map, flat, axis, a, b and courses from a graded map: the duel line along
    the long side through the middle, a and b MARGIN in from its ends, and one course lane per
    land move class at a fifth, two fifths, three fifths and four fifths across."""
    x, y, wd, ht = row["rect"]
    axis = "x" if wd >= ht else "y"

    def snap(v):
        return int(round(v / CELL)) * CELL

    if axis == "x":
        cy = snap(y + ht / 2)
        a, b = [x + MARGIN, cy], [x + wd - MARGIN, cy]
        lanes = [snap(y + ht * k / 5) for k in (1, 2, 3, 4)]
        courses = {c: [[a[0], ly], [b[0], ly]] for c, ly in zip(COURSE_ORDER, lanes)}
    else:
        cx = snap(x + wd / 2)
        a, b = [cx, y + MARGIN], [cx, y + ht - MARGIN]
        lanes = [snap(x + wd * k / 5) for k in (1, 2, 3, 4)]
        courses = {c: [[lx, a[1]], [lx, b[1]]] for c, lx in zip(COURSE_ORDER, lanes)}
    return {"map": row["name"], "flat": {"rect": row["rect"], "height": row["height"],
                                         "sealevel": row["sealevel"]},
            "axis": axis, "a": a, "b": b, "courses": courses}


def check(cfg, vfs):
    """Raises calibref.CalibError unless the install still grades the configured map as the
    config says. Returns a one-line verdict."""
    hits = [(p, w) for p, w in vfs.enumerate("maps", "*.ota", False)
            if Path(p).stem.lower() == cfg["map"].lower()]
    if not hits:
        raise calibref.CalibError(f"the arena map {cfg['map']!r} has no .ota in the install")
    row = grade(vfs, *hits[0])
    if row["why"]:
        raise calibref.CalibError(f"{cfg['map']}: {row['why']}")
    want = arena_fields(row)
    for k in SCAN_FIELDS:
        if cfg.get(k) != want[k]:
            raise calibref.CalibError(f"{cfg['map']}: the config's {k} {cfg.get(k)} is not the "
                                      f"install's {want[k]} (tools/tasim/arenascan.py pick)")
    scn_map = json.loads((TREE / cfg["scenario"]).read_text())["setup"]["map"]
    if scn_map.lower() != cfg["map"].lower():
        raise calibref.CalibError(f"{cfg['scenario']} loads {scn_map!r}, the arena is {cfg['map']!r}")
    return (f"meteors off in every schema, flat {row['rect']} at height {row['height']} "
            f"(sea {row['sealevel']})")


def dumps(doc) -> str:
    """JSON with one key a line and every list of numbers on its own line, as the config is
    read by people."""
    text = json.dumps(doc, indent=1)
    return re.sub(r"\[\s*([-\d.,\s]+?)\s*\]",
                  lambda m: "[" + ", ".join(v.strip() for v in m.group(1).split(",")) + "]",
                  text) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("--gamedir")
    s.add_argument("--top", type=int, default=15)
    s.add_argument("--json", action="store_true")
    k = sub.add_parser("pick")
    k.add_argument("--gamedir")
    k.add_argument("map", nargs="?")
    c = sub.add_parser("check")
    c.add_argument("--gamedir")
    a = p.parse_args(argv)
    try:
        vfs = install(a.gamedir)
    except Exception as e:                                  # noqa: BLE001 -- any install trouble
        print(f"arenascan: no install: {e}")
        return 2
    if a.cmd == "check":
        try:
            print(f"arena {calibref.arena()['map']}: {check(calibref.arena(), vfs)}")
        except calibref.CalibError as e:
            print(f"arenascan: {e}")
            return 1
        return 0
    good, bad = scan(vfs)
    if a.cmd == "scan":
        if a.json:
            print(json.dumps({"qualify": good, "refused": bad}, indent=1))
            return 0
        for r in good[:a.top]:
            print(f"{r['name']:32s} long {r['long']:5d}  rect {r['rect']}  height {r['height']}")
        print(f"arenascan: {len(good)} maps qualify, {len(bad)} refused "
              f"({sum(1 for r in bad if r['why'].startswith('meteors'))} for meteors)")
        return 0
    row = good[0] if not a.map else next(
        (r for r in good + bad if r["name"].lower() == a.map.lower()), None)
    if row is None or row["why"]:
        print(f"arenascan: {a.map}: {row['why'] if row else 'not in the install'}")
        return 1
    path = calibref.ARENA
    cfg = json.loads(path.read_text())
    cfg.update(arena_fields(row))
    path.write_text(dumps(cfg))
    scn_path = TREE / cfg["scenario"]
    scn = json.loads(scn_path.read_text())
    scn["setup"]["map"] = row["name"]
    scn_path.write_text(json.dumps(scn, indent=1, ensure_ascii=False) + "\n")
    print(f"arenascan: {path.name} and {scn_path.name} now name {row['name']}, flat {row['rect']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
