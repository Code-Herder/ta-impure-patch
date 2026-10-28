#!/usr/bin/env python3
"""Native-input assertions for the reclaim sweep (tagpu_sweep.c): with RECLAIM armed, a left drag
on the ground draws a ring whose release orders the selected reclaimers to take the features of
the chosen kind inside it, shared nearest-first; the RECLAIM gadget's quickkey cycles the kind.

    sweep_regression.py <instance> core      cases (a)-(m) below
    sweep_regression.py <instance> replay    the sweep-basic test group's gestures (two arms)
    sweep_regression.py <instance> preview   a ring held mid-drag: the marker pass's count; then
                                             the zoom lever's 0.5: (c) again, the drag's radius and the
                                             drag threshold
    sweep_regression.py <instance> markers   the engine's order markers draw what a ring queued:
                                             with and without Shift, deselected, one selected
    sweep_regression.py <instance> popup     the popup's anchor and drawn rect, a row click, the
                                             cursor over it, the same at the zoom lever's 0.5; at
                                             whatever HUD scale the instance runs
    sweep_regression.py <instance> kindsave  ENERGY chosen, the level ended and the instance
                                             relaunched: impure.cfg and the first arming carry it
                                             (takes --launch)

`--hold-for S` keeps each state the owner wants a picture of for S seconds, printing a
`sweep-capture:` line as it starts, for the runner to capture from the live display.

Each mode plays on scenarios/sweep-basic.json, handed the level held (resource_regression.py,
IN LOCKSTEP): tareplay's `run` step hands `replay` over at its GameTime; `core` and `preview`
take `--launch`, which holds the run lock (tools/runlock.py), launches the instance with the
launch's hold (tagpu_hold.on) on the scenario and stops it afterwards.

The cases (core), ARMCOM and both ARMCK selected:
  (a) the RECLAIM gadget arms mode 12; (b) its quickkey cycles METAL, ENERGY, ALL and keeps the
  mode; (c) a METAL ring without Shift: the union of the lists' reclaim-family nodes is the
  expected METAL set, each feature once, and each builder's own targets come in the order
  sweep_split gives for the inputs read back here, then mode 1; (d) ENERGY and (e) ALL, the same;
  (f) a Shift ring after a METAL prefix: every list keeps its prefix, no feature twice, mode 12
  with main+0x2CC6 bit 5, and Shift's release disarms; (g) a click on a wreck under METAL is
  stock's: every list is that wreck alone, mode 1; (h) on a tree under METAL the cursor is the
  engine's "nothing", 0x13, and a click orders nothing and keeps the mode; (i) a right press
  mid-drag orders nothing and keeps the mode, a second one disarms; (j) Esc the same; (k) a ring
  on bare ground orders nothing and keeps the mode; (l) a drag across the view gives the drag's own
  radius, no screen cap; (m) the level's counters: no self-check mismatch, no list cap.

What each ring holds is computed here from the engine's own grid, by peek, with the rules the DLL
applies (tagpu_sweep.c, enumerate): anchors whose centre is in the ring, the def's reclaimable
bit, the kind's float, and the mapped bit at the feature's order position. The order position's
altitude is 0x485070's, which is not modelled here: the mapped bit is read for every row the
heights of the 3 x 3 cells round the centre allow, and a feature whose rows disagree is ambiguous
-- it may be taken or not, and when taken it joins the split's inputs. Never from the scenario's
list alone, so a native feature in the ring joins the expectation instead of failing it.
"""
import argparse
import importlib.util
import math
import re
import struct
import subprocess
import sys
import time

from resource_regression import ACK_S, CHECK_FAILURES, TACLI, TREE, Check, tacli_api
import runlock

SCENARIO = TREE / "scenarios/sweep-basic.json"
END_SCENARIO = TREE / "scenarios/replay-end.json"
MAIN = "*0x511DE8"


def _sweeptest():
    """tagpu/ddraw/tests/sweeptest.py as a module: its Python split is checked there against
    tagpu_sweepgeo.c, so the split expected here is the DLL's."""
    spec = importlib.util.spec_from_file_location("_sweeptest", TREE / "tagpu/ddraw/tests/sweeptest.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


GEO = _sweeptest()

KINDS = ("METAL", "ENERGY", "ALL")      # tagpu_sweep.h's enum, in the quickkey's cycle
FAMILY = {"RECLAIM", "VTOL_RECLAIM", "RESURRECT"}
NOTHING = 0x13                          # the engine's "nothing" cursor (0x49925D, 0x43F098)
RING_MAX_WU = 4096                      # tagpu_sweep.c: the scan's bound, world px
LIST_CAP = 1024                         # SWEEP_LIST_CAP
FOOT_MAX = 16
EYE = (800, 1000)                       # the whole scene in view at 1024x768

# the scene (sweep-basic.json): its wrecks and trees, world px
WRECKS = [(1300, 1250), (1380, 1300), (1340, 1180), (1300, 1400), (1000, 1450), (1080, 1500)]
TREES = [(1220, 1170), (1260, 1330), (1240, 1230), (1370, 1390), (1200, 1450), (1040, 1390),
         (1150, 1480), (950, 1320)]
BIG = ((1200, 1360), (1470, 1360))      # a ring of r 270 over the whole scene (farthest centre 251)
PREFIX = ((1300, 1250), (1400, 1250))   # r 100: part of the wrecks, for (f)'s prefix


# -- the DLL's account: tagpu_sweep.c's log lines ---------------------------------------------

ARMED_RX = r"sweep: (armed$|.*not armed$)"


def sweep_line(line):
    """One of tagpu_sweep.c's lines as (kind, fields), or None. A commit,
        sweep: commit kind=METAL r=230 found=6 taken=6 builders=3 issued=6 unassigned=0 mismatch=0 listcap=0 shift=0
    the kind the quickkey set, `sweep: kind METAL` (fields {"kind": "METAL"}), and the counters a
    level's real end writes,
        sweep: level end commits=.. issued=.. clicks=.. mismatch=.. listcap=.. unassigned=.. trunc=..
    Fields are `key=value`; a value of digits is an int."""
    head = "sweep: "
    at = line.find(head)
    if at < 0:
        return None
    words = line[at + len(head):].split()
    if words[:1] == ["commit"]:
        kind, rest = "commit", words[1:]
    elif words[:2] == ["level", "end"]:
        kind, rest = "level end", words[2:]
    elif words[:1] == ["kind"] and len(words) == 2:
        return "kind", {"kind": words[1]}
    else:
        return None
    fields = {}
    for w in rest:
        k, eq, v = w.partition("=")
        if eq:
            fields[k] = int(v) if v.isdigit() else v
    return kind, fields


def mark_sweep_count(line, key="sweep"):
    """A count from the marker pass's status line (tagpu_mark.c, `mark: bars=.. ... sweep=N ...
    ordart=A ...`, every 120 frames under mark.on=log): `sweep` the ring's targets bracketed,
    `ordart` the order markers' art quads; None for any other line."""
    if not line.lstrip().startswith("mark: bars="):
        return None
    for w in line.split():
        k, eq, v = w.partition("=")
        if eq and k == key and v.isdigit():
            return int(v)
    return None


def anchor_line(line):
    """tagpu_sweep.c's anchor line as a dict, or None:
        sweep: anchor gadget=ARMRECLAIM rect=5,300,55,31 popup=129,279,253,351 q8=256
    `rect` the gadget's x, y, w, h in the engine's 1x units, `popup` the screen rect x0, y0, x1, y1."""
    at = line.find("sweep: anchor ")
    if at < 0:
        return None
    out = {}
    for w in line[at + len("sweep: anchor "):].split():
        k, _, v = w.partition("=")
        out[k] = v if k == "gadget" else [int(x) for x in v.split(",")]
    return out


def drawn_line(line):
    """tagpu_sweep.c's drawn line: the popup rect the render thread drew, [] for none, or None."""
    at = line.find("sweep: drawn ")
    if at < 0:
        return None
    rest = line[at + len("sweep: drawn "):].strip()
    return [] if rest == "none" else [int(x) for x in rest.partition("=")[2].split(",")]


def popup_geometry(b, q8, H):
    """tagpu_sweep.c's popup_rect for the gadget rect `b` (x, y, w, h), the HUD scale `q8` and the
    engine's screen height `H`: (pop, rows), screen rects x0, y0, x1, y1; None when not shown."""
    if (b[0] < 0 or b[0] + b[2] > 128 or b[1] < 32 or (b[1] + b[3] + 32) * q8 > H * 256):
        return None
    lo, hi = 34, H * 256 // q8 - 32 - 72 - 2
    if hi < lo:
        return None
    y0 = min(max(b[1] + b[3] // 2 - 36, lo), hi)

    def scr(x, y, w, h):
        f = lambda v: (v * q8 + 128) >> 8
        return [f(x), f(y), f(x + w), f(y + h)]
    return scr(129, y0, 124, 72), [scr(135, y0 + 6 + 21 * i, 112, 18) for i in range(3)]


def tacli(*args, timeout=600):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    assert r.returncode == 0, f"tacli {' '.join(map(str, args[:2]))}: {(r.stdout + r.stderr).strip()}"
    return r.stdout


def floor16(v):
    return v // 16


class Feature:
    """An anchor of the feature grid: its cell, footprint and centre, and its def's fields."""
    def __init__(self, cell, w, foot, energy, metal, recl):
        self.cell, self.ax, self.az = cell, cell % w, cell // w
        self.fx = min(max(foot[0], 1), FOOT_MAX)
        self.fz = min(max(foot[1], 1), FOOT_MAX)
        self.cx, self.cz = 16 * self.ax + 8 * self.fx, 16 * self.az + 8 * self.fz
        self.energy, self.metal, self.recl = energy, metal, recl

    def kind_ok(self, kind):
        return {"METAL": self.metal > 0, "ENERGY": self.energy > 0}.get(kind, True)

    def __repr__(self):
        return f"<{self.cx},{self.cz} e={self.energy:g} m={self.metal:g}>"


class Sweep(Check):
    def __init__(self, name):
        super().__init__(name, {"ARMCOM", "ARMCK"})
        self.down = []
        self.arms = 0
        self.actions = {}
        self.defcache = {}
        roster = [u for u in self.call("roster", "--json")["units"] if u["owner"] == 0]
        cks = sorted((u for u in roster if u["type"].upper() == "ARMCK"), key=lambda u: u["world"][1])
        com = [u for u in roster if u["type"].upper() == "ARMCOM"]
        assert len(cks) == 2 and len(com) == 1, f"sweep-basic's builders: {roster}"
        # The two ARMCK are told apart by where the scenario stood them (z 1250 and 1330): read
        # once, before any order moves them.
        self.units.update({"COM": com[0]["engine_index"], "CK1": cks[0]["engine_index"],
                           "CK2": cks[1]["engine_index"]})
        self.type_of = {"COM": "ARMCOM", "CK1": "ARMCK", "CK2": "ARMCK"}
        # the builders in slot order, the order the DLL hands them to the split
        self.builders = sorted(self.type_of, key=lambda k: self.units[k])
        verdict = self.call("log", "--grep", ARMED_RX, "--tail", 1, "--json")["lines"]
        assert verdict and verdict[-1].rstrip().endswith("sweep: armed"), \
            f"the sweep's install: {verdict[-1] if verdict else 'no line this run'}"

    # -- input
    def press(self, *tokens):
        self.keys(*(f"down:{t}" for t in tokens))
        self.down += tokens

    def lift(self, *tokens):
        self.keys(*(f"up:{t}" for t in tokens))
        for t in tokens:
            self.down.remove(t)

    def let_go(self):
        held, self.down = self.down[::-1], []
        if held:
            self.keys(*(f"up:{t}" for t in held))

    # -- engine state
    def mode(self):
        return self.values(f"{MAIN}+0x2cc3:1")[0]

    def flags(self):
        return self.values(f"{MAIN}+0x2cc6:1")[0]

    def cursor(self):
        return self.values(f"{MAIN}+0x2cbe:1")[0]

    def point(self):
        """main+0x2CAA, the world point 0x498DA0 wrote for the last record: x, altitude, z, 16.16."""
        return struct.unpack("<iii", self.bytes(f"{MAIN}+0x2caa:xc"))

    def view(self):
        return self.values(f"{MAIN}+0x1431f", f"{MAIN}+0x14323")

    def grid(self):
        """The feature grid's cells, W, H, the feature defs and their count."""
        cells, w, h, defs, n = self.values(f"{MAIN}+0x14287", f"{MAIN}+0x14233", f"{MAIN}+0x14237",
                                           f"{MAIN}+0x1426f", f"{MAIN}+0x14253")
        assert cells and defs and 1 <= w <= 4096 and 1 <= h <= 4096 and 0 < n < 0xFFFB, \
            f"the feature grid: cells 0x{cells:x} {w}x{h}, defs 0x{defs:x} count {n}"
        return cells, w, h, defs, n

    def rows(self, specs):
        out = []
        for i in range(0, len(specs), 48):
            out += [bytes.fromhex(v) for v in self.snapshot(*specs[i:i + 48])]
        return out

    def cells(self, g, x0, x1, z0, z1):
        """The 13-byte cells of [x0, x1] x [z0, z1], cut to the grid, keyed by cell index."""
        base, w, h = g[0], g[1], g[2]
        x0, z0, x1, z1 = max(x0, 0), max(z0, 0), min(x1, w - 1), min(z1, h - 1)
        specs, keys = [], []
        for z in range(z0, z1 + 1):
            for x in range(x0, x1 + 1, 19):
                n = min(19, x1 + 1 - x)
                specs.append(f"0x{base + 13 * (z * w + x):x}:x{13 * n:x}")
                keys.append((z * w + x, n))
        out = {}
        for (first, n), data in zip(keys, self.rows(specs)):
            for k in range(n):
                out[first + k] = data[13 * k:13 * k + 13]
        return out

    def feature_def(self, g, idx):
        """(footprint, energy, metal, reclaimable) of def `idx`: +0x94/+0x96 i16, +0xEC/+0xF0
        float, +0xFE bit 7."""
        if idx not in self.defcache:
            d = self.bytes(f"0x{g[3] + idx * 0x100 + 0x94:x}:x6c")
            fx, fz = struct.unpack_from("<hh", d, 0)
            energy, metal = struct.unpack_from("<ff", d, 0xEC - 0x94)
            self.defcache[idx] = ((fx, fz), energy, metal, bool(d[0xFE - 0x94] & 0x80))
        return self.defcache[idx]

    def anchors(self, g, x0, x1, z0, z1):
        """Every anchor of the cells [x0, x1] x [z0, z1] (def word < 0xFFFB and < the count) as a
        Feature, with the cells read (for the heights)."""
        cells = self.cells(g, x0, x1, z0, z1)
        out = []
        for cell, c in cells.items():
            word = struct.unpack_from("<H", c, 8)[0]
            if word < 0xFFFB and word < g[4]:
                foot, energy, metal, recl = self.feature_def(g, word)
                out.append(Feature(cell, g[1], foot, energy, metal, recl))
        return out, cells

    def anchor_at(self, g, pos):
        """The anchor cell of the feature at a 16.16 point (0x4815A0 and the 0xFFFE hop), or None."""
        w = g[1]
        x, z = pos[0] >> 20, pos[2] >> 20
        if not (0 <= x < w and 0 <= z < g[2]):
            return None
        cell = z * w + x
        c = self.bytes(f"0x{g[0] + 13 * cell:x}:xd")
        word = struct.unpack_from("<H", c, 8)[0]
        if word == 0xFFFE:
            cell -= c[0xA] * w + c[0xB]
            if cell < 0:
                return None
            word = self.values(f"0x{g[0] + 13 * cell + 8:x}:2")[0]
        return cell if word < 0xFFFB and word < g[4] else None

    def mapped(self, feats, cells, g):
        """For each feature, True, False or None (ambiguous): the mapped bit 0x408090 reads for
        the local player at the feature's centre, col x >> 5, row (z - alt / 2) >> 5, for every
        altitude between the lowest and highest height byte (+4) of the 3 x 3 cells round the
        centre, the player's grid width and height at +0x80 / +0x84, the grid main+0x14273 and
        the bit main+0x2A43."""
        player = self.values(f"0x{self.address('COM') + 0x96:x}")[0]
        pw, ph, grid, bit = self.values(f"0x{player + 0x80:x}", f"0x{player + 0x84:x}",
                                        f"{MAIN}+0x14273", f"{MAIN}+0x2a43:1")
        want, specs = [], []
        for f in feats:
            hs = [cells[k][4] for k in ((f.cz // 16 + dz) * g[1] + f.cx // 16 + dx
                                        for dz in (-1, 0, 1) for dx in (-1, 0, 1)) if k in cells]
            col = f.cx >> 5
            rows = sorted({(f.cz - (h >> 1)) >> 5 for h in range(min(hs), max(hs) + 1)}) if hs else []
            # off the player's grid the engine answers 0 (0x4080B5..0x4080C4)
            idx = [row * pw + col if 0 <= col < pw and 0 <= row < ph else None for row in rows]
            want.append(idx if hs else None)
            for i in idx or ():
                if i is not None:
                    specs.append(f"0x{grid + 2 * i:x}:2")
        got = iter(int(v.split()[0]) for v in self.snapshot(*specs)) if specs else iter(())
        out = []
        for idx in want:
            if idx is None:
                out.append(None)
                continue
            bits = [i is not None and bool(next(got) & (1 << bit)) for i in idx]
            out.append(True if bits and all(bits) else False if not any(bits) else None)
        return out

    def ring(self, c, r, kind):
        """What a ring of radius `r` round world point `c` (x, z) holds for `kind`: (sure,
        ambiguous), each a {cell: Feature}, by enumerate's rules."""
        g = self.grid()
        feats, cells = self.anchors(g, floor16(c[0] - r) - FOOT_MAX // 2 - 1, floor16(c[0] + r) + 1,
                                    floor16(c[1] - r) - FOOT_MAX // 2 - 1, floor16(c[1] + r) + 1)
        feats = [f for f in feats if GEO.inside(c[0], c[1], r, f.cx, f.cz) and f.recl and f.kind_ok(kind)]
        sure, amb = {}, {}
        for f, m in zip(feats, self.mapped(feats, cells, g)):
            if m:
                sure[f.cell] = f
            elif m is None:
                amb[f.cell] = f
        return sure, amb

    def scene(self):
        """The scenario's wrecks and trees as anchors, each the one anchored at its position's
        cell, and whether each is mapped for certain."""
        g = self.grid()
        pts = WRECKS + TREES
        feats, cells = self.anchors(g, min(x for x, _ in pts) // 16 - FOOT_MAX,
                                    max(x for x, _ in pts) // 16 + 4,
                                    min(z for _, z in pts) // 16 - FOOT_MAX,
                                    max(z for _, z in pts) // 16 + 4)
        found = {}
        for name, pts in (("wreck", WRECKS), ("tree", TREES)):
            for p in pts:
                # a scenario feature's position is its anchor cell's corner, not its centre
                # (tagpu_scenario.c hands GetGridPosPLOT(x / 16, z / 16) to 0x423C50)
                near = min(feats, key=lambda f: (16 * f.ax - p[0]) ** 2 + (16 * f.az - p[1]) ** 2,
                           default=None)
                assert near and (near.ax, near.az) == (p[0] // 16, p[1] // 16), \
                    f"no feature anchored at the scenario's {name} at {p}"
                found[near.cell] = (name, near)
        return found, self.mapped([f for _, f in found.values()], cells, g)

    def action_name(self, action):
        """The order descriptor's name (0x438760 compares the char* at descriptor +0x15)."""
        if action not in self.actions:
            desc = self.values("0x512344")[0] + action * 0x19
            ptr = self.values(f"0x{desc + 0x15:x}")[0]
            self.actions[action] = self.bytes(f"0x{ptr:x}:x20").split(b"\0")[0].decode("latin-1")
        return self.actions[action]

    def targets(self, name):
        """`name`'s reclaim-family orders in list order, as the anchor cells of the features at
        their positions (a ring's order stands at the centre, a click's at the clicked point)."""
        g = self.grid()
        out = []
        for o in self.orders(name):
            if self.action_name(o["action"]).upper() not in FAMILY:
                continue
            cell = self.anchor_at(g, o["pos"])
            assert cell is not None, f"{name}'s {self.action_name(o['action'])} at {o['pos']} has no feature"
            out.append(cell)
        return out

    def lists(self):
        return {b: self.targets(b) for b in self.builders}

    def clear(self):
        """No order left to any builder: a stop through the order applier, which replaces the
        queue and leaves the selection and the cursor mode alone."""
        for b in self.builders:
            self.call("order", "stop", "--unit", self.units[b], "--expect", self.type_of[b])
        self.once(not any(self.lists().values()), "the builders kept reclaim orders after a stop")

    def inputs(self):
        """The split's inputs for a commit without Shift, read back from the engine: each
        builder's position (unit +0x6A / +0x72 >> 16), its def's maxvelocity +0x192 (16.16) and
        builddistance +0x212, no load, and the room its list leaves under LIST_CAP."""
        defs = self.values(f"{MAIN}+0x1439b")[0]
        out = []
        for b in self.builders:
            u = self.address(b)
            x, _, z = (v >> 16 for v in struct.unpack("<iii", self.bytes(f"0x{u + 0x6a:x}:xc")))
            d = self.values(f"0x{u + 0x92:x}")[0]
            assert d >= defs and (d - defs) % 0x249 == 0, f"{b}'s def pointer 0x{d:x}"
            speed, reach = self.values(f"0x{d + 0x192:x}", f"0x{d + 0x212:x}:2")
            speed = struct.unpack("<i", struct.pack("<I", speed))[0] / 65536.0
            steps = len(self.orders(b))
            out.append([x, z, speed, float(reach), 0.0, max(LIST_CAP - steps, 0)])
        return out

    def arm(self):
        """RECLAIM armed, mode 12, by the gadget whose name holds RECLAIM -- the engine's own
        substring rule (0x419F56), which the DLL's quickkey match uses as well."""
        assert "shift" not in self.down, "arm with Shift held"
        if self.flags() & 0x20:
            self.until(lambda: self.mode() != 12, "Shift's release after a Shift commit did not disarm")
        if self.mode() == 12:
            return
        names = [g["name"] for g in self.call("ui", "--json")["gadgets"] if "RECLAIM" in g["name"].upper()]
        assert names, "no RECLAIM gadget on the selection's panel"
        self.call("ui", "click", names[0])
        self.until(lambda: self.mode() == 12, f"{names[0]} did not arm reclaim")
        self.arms += 1

    def quickkey(self):
        """The RECLAIM gadget's quickkey, as the DLL finds it (tagpu_sweep.c, reclaim_gadget): the
        first button (id 1) of the top screen's records 1..count (ControlsAry [main+0x531]+4,
        stride 0x15B, the count the i16 at +0xB6) whose name (+2, 16 bytes) holds RECLAIM, its
        byte +0x13A."""
        top = self.values(f"{MAIN}+0x531")[0]
        ctrls = self.values(f"0x{top + 4:x}")[0] if top else 0
        assert ctrls, "no top screen's gadgets"
        count = struct.unpack("<h", struct.pack("<H", self.values(f"0x{ctrls + 0xb6:x}:2")[0]))[0]
        assert 1 <= count <= 512, f"the top screen's gadget count {count}"
        specs = []
        for i in range(1, count + 1):
            rec = ctrls + i * 0x15B
            specs += [f"0x{rec:x}:1", f"0x{rec + 2:x}:x10", f"0x{rec + 0x13a:x}:1"]
        vals = []
        for i in range(0, len(specs), 48):
            vals += self.snapshot(*specs[i:i + 48])
        for i in range(count):
            kind, name, key = vals[3 * i:3 * i + 3]
            name = bytes.fromhex(name).split(b"\0")[0].decode("latin-1")
            if int(kind.split()[0]) == 1 and "RECLAIM" in name:
                key = int(key.split()[0])
                assert key, f"{name} has no quickkey"
                return chr(key).lower()
        raise AssertionError("no RECLAIM button on the top screen")

    def tap_quickkey(self):
        k = self.quickkey()
        self.keys(f"down:{k}", f"up:{k}")

    def kind(self):
        got = self.call("log", "--grep", r"sweep: kind ", "--tail", 1, "--json")["lines"]
        return got[-1].split("sweep: kind ", 1)[1].strip() if got else "ALL"

    def set_kind(self, want):
        for _ in range(3):
            if self.kind() == want:
                return
            self.tap_quickkey()
            self.once(self.mode() == 12, "the quickkey disarmed reclaim")
        assert self.kind() == want, f"three quickkeys did not reach {want}: the kind is {self.kind()}"

    # -- aiming
    def aim(self, x, z):
        """The pointer where the engine's world point is (x, z), within a few px, and the screen
        point it took. The first move assumes 1:1 and the ground's height there; each further
        move corrects by what it missed, scaled by the zoom measured from the last two moves."""
        ex, ez = self.view()
        g = self.grid()
        h = self.bytes(f"0x{g[0] + 13 * ((z >> 4) * g[1] + (x >> 4)) + 4:x}:x1")[0]
        sx, sy = x - ex + 128, z - h // 2 - ez + 32
        scale = 1.0
        last = None
        for _ in range(8):
            self.keys(f"pmove:{sx},{sy}")
            p = self.point()
            gx, gz = p[0] >> 16, p[2] >> 16
            if abs(gx - x) <= 2 / scale + 1 and abs(gz - z) <= 2 / scale + 1:
                return p, (sx, sy)
            if last and gx != last[2]:
                scale = max(0.1, min(4.0, abs((sx - last[0]) / (gx - last[2]))))
            last = (sx, sy, gx)
            sx += round((x - gx) * scale)
            sy += round((z - gz) * scale)
        raise AssertionError(f"the world point did not reach ({x}, {z}): it stands at ({gx}, {gz})")

    # -- the log
    def mark(self):
        self.log += [ln.rstrip() for ln in self.reader()]
        return len(self.log)

    def lines(self, start, kind):
        """tagpu_sweep.c's `kind` lines from log index `start` on, complete once `tacli keys`
        has returned (the DLL logs inside the engine's handling of the event, and the batch's
        `done` line comes after the engine consumed its last event, through the one sink)."""
        self.mark()
        return [f for k, f in filter(None, map(sweep_line, self.log[start:])) if k == kind]

    def commit_line(self, start, what):
        got = self.lines(start, "commit")
        assert len(got) == 1, f"{what}: {len(got)} commit lines, want 1: {got}"
        assert got[0].get("mismatch") == 0 and got[0].get("listcap") == 0, f"{what}: {got[0]}"
        return got[0]

    def no_commit(self, start, what):
        got = self.lines(start, "commit")
        assert not got, f"{what}: the sweep committed {got}"

    def drag(self, frm, to, shift=False):
        """A left drag from world point `frm` to `to`, under Shift when `shift` (kept held after
        the release); the press's and the last move's world points and the log index before."""
        start = self.mark()
        if shift:
            self.press("shift")
        pc, _ = self.aim(*frm)
        self.press("lbutton")
        pe, _ = self.aim(*to)
        self.lift("lbutton")
        return pc, pe, start


def radius(pc, pe, zoom):
    """tagpu_sweep.c's radius: the drag's world length, bounded by RING_MAX_WU, truncated. `zoom`
    does not enter it: the ring follows the drag at any zoom."""
    d = math.hypot((pe[0] - pc[0]) / 65536.0, (pe[2] - pc[2]) / 65536.0)
    return int(min(d, RING_MAX_WU))


def ring_case(s, what, kind, ring=BIG, zoom=1.0, must=()):
    """A ring without Shift from builders with no orders: the union of the lists is what the ring
    holds, each feature once, each builder's targets in sweep_split's order, then mode 1."""
    s.clear()
    s.arm()
    s.set_kind(kind)
    builders = s.inputs()
    pc, pe, start = s.drag(*ring)
    line = s.commit_line(start, what)
    c = (pc[0] >> 16, pc[2] >> 16)
    r = radius(pc, pe, zoom)
    assert line["r"] == r and line["kind"] == kind and line["shift"] == 0, \
        f"{what}: commit line {line}, want kind={kind} r={r}"
    sure, amb = s.ring(c, r, kind)
    got = s.lists()
    taken = [cell for b in s.builders for cell in got[b]]
    assert len(taken) == len(set(taken)), f"{what}: a feature taken twice: {got}"
    assert set(sure) <= set(taken) <= set(sure) | set(amb), \
        f"{what}: taken {sorted(taken)}, want {sorted(sure)} (+ any of {sorted(amb)})"
    for cell in must:
        assert cell in taken, f"{what}: the scene's feature at cell {cell} was not taken"
    feats = {**sure, **amb}
    order = sorted(taken, key=lambda k: ((feats[k].cx - c[0]) ** 2 + (feats[k].cz - c[1]) ** 2,
                                         feats[k].cz, feats[k].cx, k))
    targets = [(feats[k].cx, feats[k].cz, GEO.reclaim_ticks(feats[k].energy, feats[k].metal))
               for k in order]
    pairs, _ = GEO.order_sweep(builders, targets)
    for i, b in enumerate(s.builders):
        want = [order[k] for who, k in pairs if who == i]
        assert got[b] == want, f"{what}: {b}'s targets {got[b]}, the split gives {want}"
    assert line["found"] == len(taken) and line["issued"] == len(taken) and \
        line["unassigned"] == 0 and line["builders"] == len(s.builders), \
        f"{what}: commit line {line}, {len(taken)} taken"
    want_mode = 1 if taken else 12
    assert s.mode() == want_mode, f"{what}: mode {s.mode()}, want {want_mode}"
    return got


def cases_ab(s):
    s.arm()
    s.once(s.mode() == 12, "(a) the RECLAIM gadget did not arm mode 12")
    first = s.kind()
    seen = []
    for _ in range(3):
        start = s.mark()
        s.tap_quickkey()
        got = s.lines(start, "kind")
        assert len(got) == 1, f"(b) one quickkey wrote {got}"
        seen.append(got[0]["kind"])
        assert s.mode() == 12, f"(b) the quickkey disarmed reclaim (mode {s.mode()})"
    i = KINDS.index(first)
    want = [KINDS[(i + k) % 3] for k in (1, 2, 3)]
    assert seen == want, f"(b) the quickkey cycled {seen}, want {want}"


def case_f(s, scene):
    what = "(f) a Shift ring after a METAL prefix"
    s.clear()
    s.arm()
    s.set_kind("METAL")
    pc, pe, start = s.drag(*PREFIX)
    s.commit_line(start, f"{what}: the prefix")
    before = s.lists()
    assert any(before.values()), f"{what}: the prefix ring took nothing"
    s.arm()
    s.set_kind("ALL")
    pc, pe, start = s.drag(*BIG, shift=True)
    line = s.commit_line(start, what)
    assert line["shift"] == 1, f"{what}: {line}"
    after = s.lists()
    for b in s.builders:
        assert after[b][:len(before[b])] == before[b], f"{what}: {b}'s prefix {before[b]} became {after[b]}"
    taken = [cell for b in s.builders for cell in after[b]]
    assert len(taken) == len(set(taken)), f"{what}: a feature queued twice: {after}"
    sure, amb = s.ring((pc[0] >> 16, pc[2] >> 16), radius(pc, pe, 1.0), "ALL")
    assert set(sure) <= set(taken) <= set(sure) | set(amb) | {c for v in before.values() for c in v}, \
        f"{what}: queued {sorted(taken)}, want {sorted(sure)}"
    assert s.mode() == 12 and s.flags() & 0x20, \
        f"{what}: mode {s.mode()}, main+0x2CC6 0x{s.flags():02x}: the order is not kept armed"
    s.lift("shift")
    s.until(lambda: s.mode() == 1, f"{what}: Shift's release did not disarm")


def click_at(s, x, z):
    start = s.mark()
    s.aim(x, z)
    cursor = s.cursor()
    s.press("lbutton")
    s.lift("lbutton")
    return cursor, start


def case_gh(s, scene):
    wreck = next(f for name, f in scene.values() if name == "wreck")
    tree = next(f for name, f in scene.values() if name == "tree")
    what = "(g) a click on a wreck under METAL"
    s.clear()
    s.arm()
    s.set_kind("METAL")
    cursor, start = click_at(s, wreck.cx, wreck.cz)
    assert cursor != NOTHING, f"{what}: the cursor over the wreck is the engine's nothing"
    s.no_commit(start, what)
    got = s.lists()
    assert all(v == [wreck.cell] for v in got.values()), f"{what}: the lists {got}, want {wreck.cell} alone"
    assert s.mode() == 1, f"{what}: mode {s.mode()}"
    what = "(h) a click on a tree under METAL"
    s.arm()
    s.set_kind("METAL")
    cursor, start = click_at(s, tree.cx, tree.cz)
    assert cursor == NOTHING, f"{what}: the cursor over the tree is 0x{cursor:x}, want 0x13"
    s.no_commit(start, what)
    assert s.lists() == got, f"{what}: the lists changed to {s.lists()}"
    assert s.mode() == 12, f"{what}: mode {s.mode()}"


def case_ij(s):
    for key, what in (("rbutton", "(i) a right press mid-drag"), ("esc", "(j) Esc mid-drag")):
        s.clear()
        s.arm()
        before = s.lists()
        start = s.mark()
        s.aim(*BIG[0])
        s.press("lbutton")
        s.aim(*BIG[1])
        if key == "esc":
            s.keys("esc")
        else:
            s.press("rbutton")
            s.lift("rbutton")
        s.lift("lbutton")
        s.no_commit(start, what)
        assert s.lists() == before and s.mode() == 12, f"{what}: mode {s.mode()}, lists {s.lists()}"
        if key == "esc":
            s.keys("esc")
        else:
            s.press("rbutton")
            s.lift("rbutton")
        s.until(lambda: s.mode() == 1, f"{what}: the second {key} did not disarm")


def bare(s, r=60):
    """A centre in view whose ring of radius `r` holds no anchor at all."""
    g = s.grid()
    for cx in range(1520, 1620, 40):
        for cz in range(1060, 1640, 40):
            feats, _ = s.anchors(g, floor16(cx - r) - FOOT_MAX, floor16(cx + r) + 1,
                                 floor16(cz - r) - FOOT_MAX, floor16(cz + r) + 1)
            if not any(GEO.inside(cx, cz, r + 8, f.cx, f.cz) for f in feats):
                return cx, cz
    raise AssertionError("no bare ground in view east of the scene")


def case_k(s):
    what = "(k) a ring on bare ground"
    s.clear()
    s.arm()
    s.set_kind("ALL")
    cx, cz = bare(s)
    pc, pe, start = s.drag((cx, cz), (cx + 50, cz))
    line = s.commit_line(start, what)
    assert line["found"] == 0 and line["issued"] == 0, f"{what}: {line}"
    assert not any(s.lists().values()) and s.mode() == 12, f"{what}: mode {s.mode()}, lists {s.lists()}"


def long_drag(s, what, zoom):
    """A drag across the viewport's diagonal: the commit line's r is the drag's own world length
    at this zoom, with no screen cap (it is far below RING_MAX_WU)."""
    s.clear()
    s.arm()
    start = s.mark()
    s.keys("pmove:140,45")
    pc = s.point()
    s.press("lbutton")
    s.keys("pmove:1010,730")
    pe = s.point()
    s.lift("lbutton")
    line = s.commit_line(start, what)
    want = radius(pc, pe, zoom)
    assert want < RING_MAX_WU and abs(line["r"] - want) <= 1, \
        f"{what}: r={line['r']}, want the drag's {want} world px"
    return line


def level_end(s):
    """(m) The level ended (replay-end.json, as tareplay ends a fixture; the module writes its
    counters at the teardown) and its counters read: no self-check mismatch, no list cap."""
    start = s.mark()
    tacli("scenario", "apply", s.name, END_SCENARIO, timeout=300)
    s._resume()
    s.held = False
    end, results = time.monotonic() + 300, False
    while not (got := s.lines(start, "level end")):
        assert time.monotonic() < end, "(m) no `sweep: level end` line in 300 s"
        if not results and s.call("ui", "--json")["gui"].upper() == "ENDMSN.GUI":
            s.call("ui", "click", "MainMenu")
            results = True
        time.sleep(1)
    f = got[-1]
    for key in ("mismatch", "listcap"):
        assert f.get(key) == 0, f"(m) the level ended with {key}={f.get(key)}: {f}"
    return f


def setup(s):
    """The world held, the scene in view, the builders selected, and every one of the scene's
    features mapped for certain (the builders' sight maps them within seconds: the world is
    advanced in lockstep until it has)."""
    s.hold()
    s.eye(*EYE)
    s.select(*s.builders)
    def mapped():
        scene, bits = s.scene()
        return scene if all(bits) else None
    scene = s.until(mapped, "the scene's features are not all mapped", ticks=900, step=30)
    for name, f in scene.values():
        assert f.recl, f"the scene's {name} at {f.cx},{f.cz} is not reclaimable"
        assert (f.metal > 0) == (name == "wreck"), f"the scene's {name} at {f.cx},{f.cz} has metal {f.metal}"
        assert name == "wreck" or f.energy > 0, f"the scene's tree at {f.cx},{f.cz} has no energy"
    return scene


def core(s):
    scene = setup(s)
    wrecks = [c for c, (n, _) in scene.items() if n == "wreck"]
    trees = [c for c, (n, _) in scene.items() if n == "tree"]
    cases_ab(s)
    got = ring_case(s, "(c) a METAL ring", "METAL", must=wrecks)
    assert not set(trees) & {c for v in got.values() for c in v}, f"(c) a tree taken under METAL: {got}"
    ring_case(s, "(d) an ENERGY ring", "ENERGY", must=trees)
    ring_case(s, "(e) an ALL ring", "ALL", must=wrecks + trees)
    case_f(s, scene)
    case_gh(s, scene)
    case_ij(s)
    case_k(s)
    long_drag(s, "(l) a drag across the view", 1.0)
    s.clear()
    f = level_end(s)
    print(f"sweep-core: (a)-(l) passed; (m) level end {f}")


def replay(s):
    """The test group's gestures, arming reclaim twice: a METAL ring without Shift, then an
    ENERGY ring under Shift that appends to it, so the recording carries the orders of both and
    the builders reclaim through them for the rest of the level."""
    setup(s)
    s.arm()
    s.set_kind("METAL")
    pc, pe, start = s.drag(*BIG)
    first = s.commit_line(start, "the METAL ring")
    assert first["issued"] > 0 and s.mode() == 1, f"the METAL ring: {first}, mode {s.mode()}"
    s.arm()
    s.set_kind("ENERGY")
    pc, pe, start = s.drag(*BIG, shift=True)
    second = s.commit_line(start, "the Shift ENERGY ring")
    assert second["issued"] > 0, f"the Shift ENERGY ring: {second}"
    s.lift("shift")
    s.until(lambda: s.mode() == 1, "Shift's release did not disarm")
    assert s.arms <= 2, f"the script armed reclaim {s.arms} times"
    total = {b: len(v) for b, v in s.lists().items()}
    print(f"sweep-replay: METAL and Shift ENERGY rings committed; reclaim orders {total}")


def held_count(s, what, key="sweep"):
    """The second marker-pass status line's `key`= after the pointer stopped (the first may have
    drawn a packet filled before the last move was handled)."""
    start = s.mark()
    end = time.monotonic() + 3 * ACK_S
    while True:
        s.mark()
        counts = [c for c in (mark_sweep_count(ln, key) for ln in s.log[start:]) if c is not None]
        if len(counts) >= 2:
            return counts[1]
        assert time.monotonic() < end, f"{what}: no two `mark:` status lines with {key}= in {3 * ACK_S} s"
        time.sleep(0.2)


HOLD_FOR = 0.0          # --hold-for: seconds each picture's state is held for the runner


def capture(what):
    """A state the owner wants a picture of: announced, then held for --hold-for seconds."""
    if HOLD_FOR > 0:
        print(f"sweep-capture: {what} (held {HOLD_FOR:g} s)", flush=True)
        time.sleep(HOLD_FOR)


class Lever:
    """A lever file with `word` added to whatever it held, put back afterwards (a file overrides
    the play defaults, tagpu_opt.c)."""
    def __init__(self, gamedir, name, word):
        self.lever = gamedir / name
        self.word = word
        self.old = self.lever.read_text() if self.lever.exists() else None

    def __enter__(self):
        words = (self.old or "").split()
        if self.word not in (w.lower() for w in words):
            words.append(self.word)
        self.lever.write_text(" ".join(words) + "\n")
        return self

    def __exit__(self, *exc):
        if self.old is None:
            self.lever.unlink(missing_ok=True)
        else:
            self.lever.write_text(self.old)


def MarkLog(gamedir):
    """tagpu_mark.on with `log`, for the marker pass's status line."""
    return Lever(gamedir, "tagpu_mark.on", "log")


def queued(s, names):
    """The reclaim-family orders of `names`, whose features stand: what the order markers draw."""
    return sum(len(s.targets(b)) for b in names)


# bare ground in sweep-basic's view, away from every feature and from where any builder walks
PARK = (1600, 1060)


def markers(s, gamedir):
    """L2a: what a ring queues is drawn by the engine's own queued-order markers, as any other
    order is, and the sweep draws none of its own. After an ALL ring, the pointer parked on bare
    ground: no order art without Shift. With Shift, and the order markers' route dots off
    (`nodots`) so the count is the cursors alone: one reclaim cursor per queued target of the
    selected builders; none with nothing selected; one builder's alone when it alone is selected.
    Then, dots back on, that lone builder -- the unit the GUI shows, which stock flags as it does a
    hovered one -- has its route dots as well. The marker pass's status line carries no queue
    count."""
    def ordart(what, shift=True):
        if shift:
            s.press("shift")
        try:
            got = held_count(s, what, "ordart")
            if shift:
                capture(what)
            return got
        finally:
            if shift:
                s.lift("shift")

    with MarkLog(gamedir):
        setup(s)
        s.clear()
        s.arm()
        s.set_kind("ALL")
        pc, pe, start = s.drag(*BIG)
        s.commit_line(start, "the ALL ring")
        want = queued(s, s.builders)
        assert want > 0, "the ALL ring queued nothing"
        s.aim(*PARK)
        got = ordart("(L2a) all selected, no Shift", shift=False)
        assert got == 0, f"(L2a) ordart={got} without Shift, want 0"
        status = [ln for ln in s.log[start:] if ln.lstrip().startswith("mark: bars=")]
        assert status and not any("sweepq=" in ln for ln in status), \
            f"(L2a) the status line still counts queued features: {status[-1:]}"
        with Lever(gamedir, "tagpu_order.on", "nodots"):
            got = ordart("(L2a) all selected, Shift held")
            assert got == want, f"(L2a) ordart={got} with every builder selected, want {want}"
            s.keys("esc")
            s.once(not any(s.values(f"0x{s.address(b) + 0x110:x}")[0] & 0x10 for b in s.builders),
                   "(L2a) Esc left a builder selected")
            s.aim(*PARK)
            got = ordart("(L2a) none selected, Shift held")
            assert got == 0, f"(L2a) ordart={got} with nothing selected"
            s.select("CK1")
            one = queued(s, ["CK1"])
            s.aim(*PARK)
            got = ordart("(L2a) one selected, Shift held, no dots")
            assert got == one, f"(L2a) ordart={got} with CK1 alone selected, want {one}"
        dotted = ordart("(L2a) one selected, Shift held")
        assert dotted > one, f"(L2a) ordart={dotted} with CK1 alone selected and dots on: no route dots"
        print(f"sweep-markers: the engine's markers draw the queue, one reclaim cursor a target, only "
              f"under Shift ({one} for CK1 alone, {dotted - one} route dots with it)")


def engine_point(sx, sy, q8):
    """The engine's point for a screen point in the world region at HUD scale q8: the screen point
    less (128s - 128, 32s - 32) (tagpu_hud.h, the pointer map's world branch) [ASSUMED: the
    rounding of 128s and 32s is tagpu_hud_bands'; exact at q8 256]."""
    pw, bh = (128 * q8 + 128) >> 8, (32 * q8 + 128) >> 8
    return sx - (pw - 128), sy - (bh - 32)


def popup(s, gamedir):
    """L3a/L3b: armed, the anchor line names the RECLAIM gadget and the popup rect popup_geometry
    gives for it; the render thread drew it there; a click on the ENERGY row's centre picks
    ENERGY with no order, no gesture and the mode kept; the pointer over the popup is the engine's
    nothing; then at the zoom lever's 0.5 a click on the METAL row picks METAL."""
    zoomfile, ztmp = gamedir / "tagpu_zoom.txt", gamedir / "tagpu_zoom.txt.tmp"
    with MarkLog(gamedir):
        try:
            popup_cases(s, zoomfile, ztmp)
        finally:
            zoomfile.unlink(missing_ok=True)


def popup_cases(s, zoomfile, ztmp):
    setup(s)
    s.clear()
    start = s.mark()
    s.arm()
    s.set_kind("ALL")
    end = time.monotonic() + ACK_S
    while True:
        s.mark()
        anchors = [a for a in map(anchor_line, s.log[start:]) if a]
        drawn = [d for d in map(drawn_line, s.log[start:]) if d is not None]
        if anchors and drawn and drawn[-1]:
            break
        assert time.monotonic() < end, f"(L3a) no anchor and drawn popup lines in {ACK_S} s: {anchors} {drawn}"
        time.sleep(0.2)
    a = anchors[-1]
    H = s.values(f"{MAIN}+0x37e23")[0]
    geo = popup_geometry(a["rect"], a["q8"][0], H)
    assert "RECLAIM" in a["gadget"] and geo, f"(L3a) the anchor {a} (screen height {H})"
    pop, rows = geo
    assert a["popup"] == pop and drawn[-1] == pop, \
        f"(L3a) the popup at {a['popup']}, drawn at {drawn[-1]}, the geometry gives {pop}"
    q8 = a["q8"][0]
    capture(f"the armed popup at q8 {q8}")
    for zoom, row, kind in ((None, 1, "ENERGY"), ("0.5", 0, "METAL")):
        what = f"(L3{'a' if zoom is None else 'b'}) a click on the {kind} row" + \
               (f" at zoom {zoom}" if zoom else "")
        if zoom:
            ztmp.write_text(zoom)
            ztmp.replace(zoomfile)
            # two marker-pass status lines: frames drawn at the lever's level since
            held_count(s, "the zoom lever's 0.5")
        before = s.lists()
        r = rows[row]
        x, y = engine_point((r[0] + r[2]) // 2, (r[1] + r[3]) // 2, q8)
        start = s.mark()
        s.keys(f"pmove:{x},{y}")
        assert s.cursor() == NOTHING, f"{what}: the cursor over the popup is 0x{s.cursor():x}, want 0x13"
        s.press("lbutton")
        s.lift("lbutton")
        got = s.lines(start, "kind")
        assert got and got[-1]["kind"] == kind, f"{what}: the kind lines {got}"
        s.no_commit(start, what)
        assert s.lists() == before and s.mode() == 12, f"{what}: mode {s.mode()}, lists {s.lists()}"
    print(f"sweep-popup: anchored at {a['gadget']} {a['rect']}, drawn at {pop} (q8 {q8}); "
          f"the ENERGY and, at zoom 0.5, the METAL row picked")


def kindsave(s, instance):
    """L3c: ENERGY chosen and the level ended -- the store takes a pending kind there -- then the
    instance stopped: impure.cfg holds reclaimkind=energy; relaunched, the first arming is ENERGY."""
    setup(s)
    s.arm()
    s.set_kind("ENERGY")
    s.clear()
    level_end(s)
    tacli("stop", instance, timeout=300)
    text = (s.gamedir / "impure.cfg").read_text()
    assert re.search(r"(?im)^reclaimkind=energy$", text), \
        f"(L3c) impure.cfg after the stop: {[ln for ln in text.splitlines() if 'reclaim' in ln.lower()]}"
    (s.gamedir / "tagpu_hold.on").write_text("")
    tacli("scenario", "load", instance, SCENARIO, "--defaults", timeout=900)
    t = Sweep(instance)
    t.hold()
    t.eye(*EYE)
    t.select(*t.builders)
    t.arm()
    assert t.kind() == "ENERGY", f"(L3c) the first arming after the relaunch is {t.kind()}"
    t.release()
    print("sweep-kindsave: reclaimkind=energy in impure.cfg, and the relaunch arms ENERGY")


def threshold(s, zoom):
    """A press and a move of 5 screen px is a click (no commit), of 8 px a ring whose radius is
    8 px in world px at the zoom: the threshold is 7 screen px whatever the zoom."""
    s.arm()
    cx, cz = bare(s)
    for px, ring in ((5, False), (8, True)):
        start = s.mark()
        pc, (sx, sy) = s.aim(cx, cz)
        s.press("lbutton")
        s.keys(f"pmove:{sx + px},{sy}")
        pe = s.point()
        s.lift("lbutton")
        what = f"a {px}-px drag at zoom {zoom}"
        if ring:
            line = s.commit_line(start, what)
            assert line["r"] == radius(pc, pe, zoom) and abs(line["r"] - px / zoom) <= 2 / zoom + 1, \
                f"{what}: r={line['r']}"
        else:
            s.no_commit(start, what)
        s.once(s.mode() == 12, f"{what}: mode {s.mode()}")


def preview(s, gamedir):
    """V3: a METAL ring held mid-drag is what the marker pass draws (its status line under
    mark.on=log), dropped by a right press; then at the zoom lever's 0.5, (c) again, the drag radius and
    the threshold."""
    lever = gamedir / "tagpu_mark.on"
    zoomfile, ztmp = gamedir / "tagpu_zoom.txt", gamedir / "tagpu_zoom.txt.tmp"
    old = lever.read_text() if lever.exists() else None
    words = (old or "").split()
    lever.write_text(" ".join(words + ([] if "log" in (w.lower() for w in words) else ["log"])) + "\n")
    try:
        scene = setup(s)
        wrecks = [c for c, (n, _) in scene.items() if n == "wreck"]
        s.clear()
        s.arm()
        s.set_kind("METAL")
        pc, _ = s.aim(*BIG[0])
        s.press("lbutton")
        pe, _ = s.aim(*BIG[1])
        sure, amb = s.ring((pc[0] >> 16, pc[2] >> 16), radius(pc, pe, 1.0), "METAL")
        n = held_count(s, "V3")
        assert len(sure) <= n <= len(sure) + len(amb), \
            f"V3: the marker pass drew {n} brackets mid-drag, want {len(sure)} (+ up to {len(amb)})"
        s.press("rbutton")
        s.lift("rbutton")
        s.lift("lbutton")
        assert not any(s.lists().values()), f"V3: the dropped drag left orders {s.lists()}"
        threshold(s, 1.0)
        ztmp.write_text("0.5")
        ztmp.replace(zoomfile)
        held_count(s, "the zoom lever's 0.5")
        ring_case(s, "(c) at zoom 0.5", "METAL", zoom=0.5, must=wrecks)
        long_drag(s, "a drag across the view at zoom 0.5", 0.5)
        s.clear()
        threshold(s, 0.5)
        print(f"sweep-preview: the marker pass drew {n} brackets mid-drag; zoom 0.5: the ring, the "
              f"drag radius and the threshold follow the zoom")
    finally:
        zoomfile.unlink(missing_ok=True)
        if old is None:
            lever.unlink(missing_ok=True)
        else:
            lever.write_text(old)


def launched(instance, mode):
    """The instance launched on the scenario with the launch's hold, under the run lock; the
    callable that stops it and takes the hold lever back."""
    runlock.hold(TREE, f"sweep_regression {mode} on {instance}")
    gamedir = tacli_api().Instance(instance).gamedir
    lever = gamedir / "tagpu_hold.on"
    had = lever.exists()
    lever.write_text("")
    tacli("scenario", "load", instance, SCENARIO, "--defaults", timeout=900)

    def stop():
        try:
            tacli("stop", instance, timeout=300)
        finally:
            if not had:
                lever.unlink(missing_ok=True)
    return stop


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("instance")
    p.add_argument("mode", choices=("core", "replay", "preview", "markers", "popup", "kindsave"))
    p.add_argument("--launch", action="store_true",
                   help="launch the instance on sweep-basic.json first (run lock); not for replay")
    p.add_argument("--hold-for", type=float, default=0.0,
                   help="seconds to hold each state the owner wants a picture of")
    a = p.parse_args()
    global HOLD_FOR
    HOLD_FOR = a.hold_for
    if a.launch and a.mode == "replay":
        p.error("replay runs inside its test group (tools/replay/fixtures.json, sweep-basic)")
    if a.mode == "kindsave" and not a.launch:
        p.error("kindsave relaunches the instance: it takes --launch")
    stop = None
    try:
        if a.launch:
            stop = launched(a.instance, a.mode)
        check = Sweep(a.instance)
        run = {"core": core, "replay": replay, "preview": lambda c: preview(c, c.gamedir),
               "markers": lambda c: markers(c, c.gamedir), "popup": lambda c: popup(c, c.gamedir),
               "kindsave": lambda c: kindsave(c, a.instance)}[a.mode]
        # As place_regression.py: the case's own failure is the exit reason and is printed first;
        # a cleanup step that fails after it is reported beside it, never in its place.
        failures, raised = [], True
        try:
            run(check)
            raised = False
        except CHECK_FAILURES as e:
            failures.append(("", e))
            raised = False
        finally:
            for what, step in (("let go", check.let_go), ("release", check.release)):
                try:
                    step()
                except CHECK_FAILURES as e:
                    failures.append((what, e))
            for i, (what, e) in enumerate(failures):
                if i == 0 and not raised:
                    print(f"sweep regression FAILED: {what + ': ' if what else ''}{e}", file=sys.stderr)
                else:
                    print(f"sweep regression: {what} failed as well: {e}", file=sys.stderr)
        if failures:
            return 1
    except CHECK_FAILURES as e:
        print(f"sweep regression FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if stop:
            stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
