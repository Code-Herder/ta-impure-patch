#!/usr/bin/env python3
"""Native-input assertions for multi-building placement (tagpu_place.c): with a building armed,
Shift + drag lays a line (Ctrl: held to its axis), Shift + Alt a filled rectangle (Ctrl: hollow),
Shift + Ctrl over a unit a ring round it.

    place_regression.py <instance> core      every case below, then the level ended for (m)
    place_regression.py <instance> replay    the place-basic test group's gestures
    place_regression.py <instance> preview   a 5-site line held mid-drag: the marker pass's count
    place_regression.py <instance> spacing   Alt+Z / Alt+X, the spacing in a line, impure.cfg and a
                                             relaunch (takes --launch)
    place_regression.py --selftest           the Python geometry against placetest.py dump

Each mode plays on scenarios/place-basic.json, handed the level held (resource_regression.py,
IN LOCKSTEP): tareplay's `run` step hands `replay` over at its GameTime; `core` and `preview`
take `--launch`, which holds the run lock (tools/runlock.py), launches the instance with the
launch's hold (tagpu_hold.on) on the scenario and stops it afterwards.

The cases (core), each from a builder with no build order, ARMCK selected unless named:
  (a) a line of 5 ARMLLT; (b) a diagonal line; (c) Ctrl, the line held to its axis; (d) Alt, a
  3 x 2 rectangle in serpentine order; (e) Alt + Ctrl, a hollow 4 x 3 of 10 sites; (f) Shift +
  Ctrl on the ARMSOLAR, the ring round it; (g) a line across the solar, its 3 sites there
  skipped; (h) a line whose first site lies on a queued build removes it and adds the rest, and
  ARMMLV's line of six 1 x 1 mines adds six (no site toggles one the same gesture adds);
  (i) a Shift-click without a drag is stock's (a second one takes the order back off);
  (j) a plain click is stock's (one order, the building disarmed); (k) Shift released before the
  button orders nothing and keeps the building armed; (l) a right press mid-drag orders nothing
  and disarms; (m) the level's counters: no self-check mismatch, no plan-table overflow.

What each case expects is computed here from the rules the DLL implements: the shapes by a port
of tagpu_placegeo.c (checked against the native one by --selftest), and the queue by the commit's
TA semantics (`after_commit`). The orders are read out of the engine (Check.orders); the DLL's
own account of each commit from its log (`place_line`).
"""
import argparse
import random
import re
import struct
import subprocess
import sys
import time

from resource_regression import ACK_S, CHECK_FAILURES, TACLI, TREE, Check, tacli_api
import runlock

SCENARIO = TREE / "scenarios/place-basic.json"
END_SCENARIO = TREE / "scenarios/replay-end.json"
PLACETEST = TREE / "tagpu/ddraw/tests/placetest.py"

# -- the shapes: tagpu_placegeo.c, line for line --------------------------------------------------

MAX_SITES = 256                                    # PLACE_MAX_SITES
LINE, AXIS, RECT, HOLLOW, RING = 1, 2, 3, 4, 5     # tagpu_placegeo.h's enum
SHAPE_NAME = {LINE: "line", AXIS: "axis", RECT: "rect", HOLLOW: "hollow", RING: "ring"}


def _cell(v):
    """The int16 a PlaceCell stores."""
    return ((v + 0x8000) & 0xFFFF) - 0x8000


def _count_along(d, pitch):
    """Recoil's (|delta| + 1.4 size) / size, in cells."""
    return (10 * abs(d) + 14 * pitch) // (10 * pitch)


def place_shape(kind, fx, fz, spacing, frm, to, cap=MAX_SITES):
    """The sites, first cells, of a dragged shape from cell `frm` to cell `to` (place_shape)."""
    px, pz = fx + spacing, fz + spacing
    dx, dz = to[0] - frm[0], to[1] - frm[1]
    xnum, znum = _count_along(dx, px), _count_along(dz, pz)
    sx, sz = (px if dx > 0 else -px), (pz if dz > 0 else -pz)
    out = []

    def put(cx, cz):
        if len(out) < cap:
            out.append((_cell(cx), _cell(cz)))

    if kind in (LINE, AXIS):
        if abs(dx) > abs(dz):
            for i in range(xnum):
                put(frm[0] + i * sx,
                    frm[1] if kind == AXIS else frm[1] + (abs(dx) + 2 * i * px * dz) // (2 * abs(dx)))
        else:
            for i in range(znum):
                put(frm[0] if kind == AXIS or dz == 0
                    else frm[0] + (abs(dz) + 2 * i * pz * dx) // (2 * abs(dz)),
                    frm[1] + i * sz)
    elif kind == RECT:
        for j in range(znum):
            for i in range(xnum):
                put(frm[0] + ((xnum - 1 - i) if j & 1 else i) * sx, frm[1] + j * sz)
    elif kind == HOLLOW:
        if xnum > 1 and znum > 1:
            for j in range(znum - 1):
                put(frm[0], frm[1] + (1 + j) * sz)
            for j in range(xnum - 1):
                put(frm[0] + (1 + j) * sx, frm[1] + (znum - 1) * sz)
            for j in range(znum - 1):
                put(frm[0] + (xnum - 1) * sx, frm[1] + (znum - 2 - j) * sz)
            for j in range(xnum - 1):
                put(frm[0] + (xnum - 2 - j) * sx, frm[1])
        elif xnum == 1:
            for j in range(znum):
                put(frm[0], frm[1] + j * sz)
        else:
            for j in range(xnum):
                put(frm[0] + j * sx, frm[1])
    return out[:cap]


def place_ring(fx, fz, tx, tz, tw, th, cap=MAX_SITES):
    """The ring of fx x fz sites round the cells [tx, tx+tw) x [tz, tz+th) (place_ring)."""
    down, across = 1 + th // fz, 1 + tw // fx
    out = [(tx + tw, tz + k * fz) for k in range(down)]
    out += [(tx + tw - fx - k * fx, tz + th) for k in range(across)]
    out += [(tx - fx, tz + th - fz - k * fz) for k in range(down)]
    out += [(tx + k * fx, tz - fz) for k in range(across)]
    return [(_cell(x), _cell(z)) for x, z in out[:cap]]


def order_pos(cell, fx, fz):
    """A site's order position, x and z in 16.16: 0x419670's ((2cx + fx) << 19, (2cz + fz) << 19)."""
    return (2 * cell[0] + fx) << 19, (2 * cell[1] + fz) << 19


def overlaps(cell, fx, fz, block):
    """The site's footprint shares a cell with `block`, (x, z, w, h) in cells."""
    bx, bz, bw, bh = block
    return cell[0] < bx + bw and bx < cell[0] + fx and cell[1] < bz + bh and bz < cell[1] + fz


# -- the commit: TA semantics (design D1-A) -------------------------------------------------------

TOGGLE = 0x100000    # 0x43AFC0's reach: a same-action order within 16 wu on both axes, inclusive


def after_commit(before, sites, red, fx, fz, type_id):
    """A builder's build orders after a commit of `sites` (red: the indices skipped), from its
    build orders `before`, each (x, z, type) in chain order. Every site that is not red is the
    builder's stock Shift-click judged against the queue as it stood before the gesture: the order
    at its own position, else the first within TOGGLE of it, that no earlier site took is removed
    (tagpu_place.c, remove_toggled); a site that removes
    none is appended, in shape order, after every order kept. Returns (orders, appended, removed)."""
    kept, added, removed = list(before), [], 0
    for i, cell in enumerate(sites):
        if i in red:
            continue
        x, z = order_pos(cell, fx, fz)
        hit = next((j for j, o in enumerate(kept) if o[0] == x and o[1] == z), None)
        if hit is None:
            hit = next((j for j, o in enumerate(kept)
                        if abs(o[0] - x) <= TOGGLE and abs(o[1] - z) <= TOGGLE), None)
        if hit is None:
            added.append((x, z, type_id))
        else:
            del kept[hit]
            removed += 1
    return kept + added, len(added), removed


# -- the DLL's account: tagpu_place.c's log lines -------------------------------------------------

ARMED_RX = r"place: (armed \(|.*; not armed$)"
NOTES_RX = r"place: (mismatch type=|more than \d+ queued orders to check)"


def place_line(line):
    """One of tagpu_place.c's lines as (kind, fields), or None. A commit,
        place: commit shape=line sites=5 red=0 builders=2 issued=10 removed=0 split=0 trunc=0 listcap=0
    (`sites` after the order budget's cut; `issued`, the orders appended, and `removed`, the
    Shift-toggle removals, summed over the builders), and the counters a level's real end (the
    quit to the menu) writes,
        place: level end evals=.. commits=.. issued=.. removed=.. red=.. mismatch=.. trunc=.. listcap=.. plancap=..
    Fields are `key=value`; a value of digits is an int. With ARMED_RX (the install's verdict) and
    NOTES_RX (the self-check's mismatch and the plan table's overflow, each logged at most 8
    times a level as it happens), this is where the module's line format is written down."""
    head = "place: "
    at = line.find(head)
    if at < 0:
        return None
    words = line[at + len(head):].split()
    if words[:1] == ["commit"]:
        kind, rest = "commit", words[1:]
    elif words[:2] == ["level", "end"]:
        kind, rest = "level end", words[2:]
    else:
        return None
    fields = {}
    for w in rest:
        k, eq, v = w.partition("=")
        if eq:
            fields[k] = int(v) if v.isdigit() else v
    return kind, fields


def mark_place_count(line):
    """The marker pass's count of placement sites drawn, from its status line (tagpu_mark.c,
    `mark: bars=.. ... place=N ...`, every 120 frames under mark.on=log), or None."""
    if not line.lstrip().startswith("mark: bars="):
        return None
    for w in line.split():
        k, eq, v = w.partition("=")
        if eq and k == "place" and v.isdigit():
            return int(v)
    return None


# -- driving the game -----------------------------------------------------------------------------

PLACE_UNITS = {"ARMCK", "ARMMLV", "ARMSOLAR"}
# One builder: the stock interface arms a building only for a single selected unit. With two or
# more selected, of one type or several, the panel is ARMGEN.GUI with ARMBUILD greyed (MEASURED
# 2026-10-25 on place1), and recalling a control group disarms: the recall 0x48D9A0 ends in
# 0x495860 (0x48DBE2), which writes main+0x2CC3 = 1 and clears 0x2CC6 bit 5 (DISASSEMBLED).
BUILDERS = ("ARMCK",)
EYE = (1000, 5630)      # the scenario's flat plateau, cells 66..124 x 350..415 at height 55
MAIN = "*0x511DE8"


def tacli(*args, timeout=600):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    assert r.returncode == 0, f"tacli {' '.join(map(str, args[:2]))}: {(r.stdout + r.stderr).strip()}"
    return r.stdout


class Place(Check):
    def __init__(self, name):
        super().__init__(name, PLACE_UNITS)
        self.down = []                      # the keys and buttons this run holds, in order
        self.types = {t["name"].upper(): t["id"] for t in self.call("units", "--json")["units"]}
        self.actions = {}
        verdict = self.call("log", "--grep", ARMED_RX, "--tail", 1, "--json")["lines"]
        assert verdict and "place: armed (" in verdict[-1], \
            f"the place module's install: {verdict[-1] if verdict else 'no line this run'}"

    def notes(self):
        """No self-check mismatch and no plan-table overflow logged this run (NOTES_RX)."""
        got = self.call("log", "--grep", NOTES_RX, "--tail", 8, "--json")["lines"]
        assert not got, f"the place module noted {got}"

    # -- input
    def press(self, *tokens):
        """`down:` each token, remembered so a failed case lets go of it (let_go)."""
        self.keys(*(f"down:{t}" for t in tokens))
        self.down += tokens

    def lift(self, *tokens):
        self.keys(*(f"up:{t}" for t in tokens))
        for t in tokens:
            self.down.remove(t)

    def let_go(self):
        """Every key and button still held, released in the reverse order of their presses."""
        held, self.down = self.down[::-1], []
        if held:
            self.keys(*(f"up:{t}" for t in held))

    def aim(self, cell):
        """The pointer where the build cursor's site, the engine's own (the build rectangle
        main+0x2C92 / 16, written for every record in mode 14), is `cell`. The first move assumes
        flat ground at the altitude the rectangle last held; each further move corrects by the
        cells it missed, so terrain and projection need not be modelled here."""
        fx, fz = self.footprint(self.armed())
        alt = self.rect()[1]
        ex, ez = self.values(f"{MAIN}+0x1431f", f"{MAIN}+0x14323")
        sx = 16 * cell[0] + 8 * fx - ex + 128
        sy = 16 * cell[1] + 8 * fz - alt // 2 - ez + 32
        for _ in range(6):
            self.keys(f"pmove:{sx},{sy}")
            r = self.rect()
            got = (r[0] >> 4, r[2] >> 4)
            if got == tuple(cell):
                return
            sx += 16 * (cell[0] - got[0])
            sy += 16 * (cell[1] - got[1])
        raise AssertionError(f"the build cursor did not reach cell {cell}: it stands at {got}")

    # -- engine state
    def mode(self):
        return self.values(f"{MAIN}+0x2cc3:1")[0]

    def armed(self):
        return self.values(f"{MAIN}+0x2cc4:2")[0]

    def flags(self):
        """main+0x2CC6: bit 5 (0x20) after a Shift placement, bit 6 (0x40) the cursor refused."""
        return self.values(f"{MAIN}+0x2cc6:1")[0]

    def footprint(self, type_id):
        defs = self.values(f"{MAIN}+0x1439b")[0]
        return struct.unpack("<hh", self.bytes(f"0x{defs + type_id * 0x249 + 0x14a:x}:x4"))

    def arm(self, name):
        """`name` armed (mode 14, BuildUnitID its type) for the units selected, with Shift up.
        After a Shift placement (main+0x2CC6 bit 5) the engine disarms at the next record that
        finds Shift up (0x4992D8), which need not have run when the release's batch returns: that
        disarm is waited for first, and a building armed here carries no bit 5, so it stays
        armed."""
        assert "shift" not in self.down, "arm with Shift held"
        tid = self.types[name]
        if self.flags() & 0x20:
            self.until(lambda: self.mode() != 14, "Shift's release after a Shift placement did "
                       "not disarm the building")
        if self.mode() != 14 or self.armed() != tid:
            self.build(name)
        self.once(self.mode() == 14 and self.armed() == tid and not self.flags() & 0x20,
                  f"{name} is not armed for a placement without Shift's disarm "
                  f"(mode {self.mode()}, type {self.armed()}, main+0x2CC6 0x{self.flags():02x})")

    def action_name(self, action):
        """The order descriptor's name (0x438760 compares the char* at descriptor +0x15)."""
        if action not in self.actions:
            desc = self.values("0x512344")[0] + action * 0x19
            ptr = self.values(f"0x{desc + 0x15:x}")[0]
            self.actions[action] = self.bytes(f"0x{ptr:x}:x20").split(b"\0")[0].decode("latin-1")
        return self.actions[action]

    def builds(self, kind):
        """`kind`'s build orders in chain order, (x, z, type) with x and z in 16.16, each checked
        to be a MOBILEBUILD: the action 0x419670 gives a ground builder's click."""
        placed = self.placed(self.orders(kind))
        for o in placed:
            name = self.action_name(o["action"])
            assert name.upper() == "MOBILEBUILD", f"{kind}'s build order has action {o['action']} ({name})"
        return [(o["pos"][0], o["pos"][2], o["type"]) for o in placed]

    def clear(self, *kinds):
        """No build order left to `kinds`: a stop through the order applier, which replaces the
        queue (0x43ADC0 without Shift) and leaves the selection and the build cursor alone."""
        for kind in kinds:
            self.call("order", "stop", "--unit", self.units[kind], "--expect", kind)
        self.once(not any(self.builds(k) for k in kinds), f"{kinds} kept build orders after a stop")

    def solar(self):
        """The ARMSOLAR's cells (x, z, w, h): its first cell as 0x4197D0 rounds a footprint about
        a centre, from its position unit+0x6A / +0x72 (16.16)."""
        tw, th = self.footprint(self.types["ARMSOLAR"])
        ux, _, uz = struct.unpack("<iii", self.bytes(f"0x{self.address('ARMSOLAR') + 0x6a:x}:xc"))
        return (ux - (tw << 19) + 0x80000) >> 20, (uz - (th << 19) + 0x80000) >> 20, tw, th

    # -- the log
    def mark(self):
        """The log index from which the next input's lines are looked for."""
        self.log += [ln.rstrip() for ln in self.reader()]
        return len(self.log)

    def place_lines(self, start, kind):
        """tagpu_place.c's `kind` lines from log index `start` on. Read after `tacli keys` has
        returned, they are complete: the DLL logs a commit inside the engine's handling of the
        release, and the input batch's `done` line, which tacli waits for, after the engine has
        consumed its last event (tools/tacli, _write_tokens), through the one sink (tagpu_log.h)."""
        self.mark()
        return [f for k, f in filter(None, map(place_line, self.log[start:])) if k == kind]

    def no_commit(self, start, what):
        got = self.place_lines(start, "commit")
        assert not got, f"{what}: the place module committed {got}"

    def commit(self, start, what, **want):
        got = self.place_lines(start, "commit")
        assert len(got) == 1, f"{what}: {len(got)} commit lines, want 1: {got}"
        want = {"split": 0, "trunc": 0, "listcap": 0, **want}
        wrong = {k: (got[0].get(k), v) for k, v in want.items() if got[0].get(k) != v}
        assert not wrong, f"{what}: commit line {got[0]}: (got, want) {wrong}"


# -- the cases ------------------------------------------------------------------------------------

# Cells for ARMLLT (2 x 2) on the plateau: every shape on ground no unit stands on, the solar at
# cells 98..102 x 360..364 the only structure, the builders standing from row 393.
SHAPES = {
    "a": ("line", LINE, (70, 356), (78, 356), (), 5),
    "b": ("diagonal line", LINE, (70, 362), (78, 368), (), 5),
    "c": ("Ctrl axis line", AXIS, (70, 372), (78, 375), ("ctrl",), 5),
    "d": ("Alt rectangle", RECT, (82, 356), (86, 358), ("alt",), 6),
    "e": ("Alt+Ctrl hollow", HOLLOW, (82, 364), (88, 368), ("alt", "ctrl"), 10),
    "g": ("line across the solar", LINE, (88, 361), (106, 361), (), 10),
}
TOGGLE_LINE = ((70, 380), (78, 380))
MINE_LINE = ((70, 386), (75, 386))


def expect(p, what, building, kinds, before, sites, start, shape):
    """Every builder's build orders against after_commit, and the commit line against the sum."""
    tid = p.types[building]
    fx, fz = p.footprint(tid)
    block = p.solar()
    red = {i for i, s in enumerate(sites) if overlaps(s, fx, fz, block)}
    issued = removed = 0
    for kind in kinds:
        want, added, gone = after_commit(before[kind], sites, red, fx, fz, tid)
        got = p.builds(kind)
        assert got == want, f"{what}: {kind}'s build orders {got}, want {want}"
        issued, removed = issued + added, removed + gone
    p.commit(start, what, shape=shape, sites=len(sites), red=len(red), builders=len(kinds),
             issued=issued, removed=removed)
    return red


def drag(p, frm, to, mods=()):
    """Shift (and `mods`) + a left drag from cell `frm` to cell `to`; the log index before it."""
    start = p.mark()
    p.press("shift", *mods)
    p.aim(frm)
    p.press("lbutton")
    p.aim(to)
    p.lift("lbutton")
    p.lift(*reversed(("shift", *mods)))
    return start


def shape_case(p, key, building="ARMLLT", kinds=BUILDERS, clear=True):
    what, kind, frm, to, mods, n = SHAPES[key]
    what = f"({key}) {what}"
    if clear:
        p.clear(*kinds)
    p.arm(building)
    fx, fz = p.footprint(p.types[building])
    sites = place_shape(kind, fx, fz, 0, frm, to)
    assert len(sites) == n, f"{what}: the layout makes {len(sites)} sites, not {n}"
    before = {k: p.builds(k) for k in kinds}
    start = drag(p, frm, to, mods)
    red = expect(p, what, building, kinds, before, sites, start, SHAPE_NAME[kind])
    return len(sites), len(red)


def ring_case(p, kinds=BUILDERS, clear=True):
    what = "(f) Shift+Ctrl ring round the solar"
    if clear:
        p.clear(*kinds)
    p.arm("ARMLLT")
    fx, fz = p.footprint(p.types["ARMLLT"])
    tx, tz, tw, th = p.solar()
    sites = place_ring(fx, fz, tx, tz, tw, th)
    before = {k: p.builds(k) for k in kinds}
    x, y = p.screen("ARMSOLAR")
    start = p.mark()
    p.press("shift", "ctrl")
    p.keys(f"pmove:{x},{y}")
    p.press("lbutton")
    p.lift("lbutton")
    p.lift("ctrl", "shift")
    expect(p, what, "ARMLLT", kinds, before, sites, start, "ring")
    return len(sites)


def click(p, cell, shift):
    """One left click at `cell`, under Shift when `shift`; the log index before it."""
    start = p.mark()
    if shift:
        p.press("shift")
    p.aim(cell)
    p.press("lbutton")
    p.lift("lbutton")
    if shift:
        p.lift("shift")
    return start


def toggle_case(p, kinds=BUILDERS, clear=True):
    """(h), first half: a stock Shift-click queues a site, then a line from it removes that order
    from every builder and appends the rest."""
    what = "(h) a line over a queued build"
    if clear:
        p.clear(*kinds)
    p.arm("ARMLLT")
    fx, fz = p.footprint(p.types["ARMLLT"])
    frm, to = TOGGLE_LINE
    before = {k: p.builds(k) for k in kinds}
    start = click(p, frm, shift=True)
    p.no_commit(start, f"{what}: the single Shift-click")
    for k in kinds:
        want, _, _ = after_commit(before[k], [frm], set(), fx, fz, p.types["ARMLLT"])
        assert p.builds(k) == want, f"{what}: the Shift-click left {k} with {p.builds(k)}, want {want}"
    p.arm("ARMLLT")      # Shift's release disarms after a Shift placement (0x4992D8)
    before = {k: p.builds(k) for k in kinds}
    sites = place_shape(LINE, fx, fz, 0, frm, to)
    start = drag(p, frm, to)
    expect(p, what, "ARMLLT", kinds, before, sites, start, "line")


def mines_case(p):
    """(h), second half: 1 x 1 sites 16 wu apart, each within the toggle reach of the next."""
    what = "(h) ARMMLV's line of 1x1 mines"
    p.select("ARMMLV")
    p.clear("ARMMLV")
    p.arm("ARMMINE1")
    fx, fz = p.footprint(p.types["ARMMINE1"])
    assert (fx, fz) == (1, 1), f"{what}: ARMMINE1's footprint is {fx}x{fz}"
    frm, to = MINE_LINE
    sites = place_shape(LINE, fx, fz, 0, frm, to)
    assert len(sites) == 6, f"{what}: the layout makes {len(sites)} sites, not 6"
    start = drag(p, frm, to)
    expect(p, what, "ARMMINE1", ("ARMMLV",), {"ARMMLV": []}, sites, start, "line")
    p.select(*BUILDERS)


def stock_shift_case(p, kinds=BUILDERS):
    """(i) Shift-click, Shift-click again, then Shift up: stock's toggle, and stock's disarm."""
    what = "(i) a Shift-click without a drag"
    p.clear(*kinds)
    p.arm("ARMLLT")
    fx, fz = p.footprint(p.types["ARMLLT"])
    cell = (84, 380)
    x, z = order_pos(cell, fx, fz)
    start = p.mark()
    p.press("shift")
    p.aim(cell)
    p.press("lbutton")
    p.lift("lbutton")
    for k in kinds:
        assert p.builds(k) == [(x, z, p.types["ARMLLT"])], f"{what}: {k} holds {p.builds(k)}"
    p.press("lbutton")
    p.lift("lbutton")
    for k in kinds:
        assert p.builds(k) == [], f"{what}: the second click left {k} with {p.builds(k)}"
    p.lift("shift")
    p.no_commit(start, what)
    p.until(lambda: p.mode() == 1, f"{what}: Shift's release after a Shift placement did not disarm")


def plain_case(p, kinds=BUILDERS):
    """(j) a click without Shift: stock places on the press and disarms."""
    what = "(j) a plain click"
    p.clear(*kinds)
    p.arm("ARMLLT")
    fx, fz = p.footprint(p.types["ARMLLT"])
    cell = (84, 386)
    start = click(p, cell, shift=False)
    p.no_commit(start, what)
    x, z = order_pos(cell, fx, fz)
    for k in kinds:
        assert p.builds(k) == [(x, z, p.types["ARMLLT"])], f"{what}: {k} holds {p.builds(k)}"
    assert p.mode() == 1, f"{what}: the building stayed armed (mode {p.mode()})"


def shift_first_case(p, kinds=BUILDERS):
    """(k) Shift released before the button: the gesture is dropped and the building stays armed.
    Stock disarms on Shift's release only after a Shift placement (bit 5, 0x4992D8), which (j)'s
    plain placement cleared (0x498F70)."""
    what = "(k) Shift released before the button"
    p.clear(*kinds)
    p.arm("ARMLLT")
    assert not p.flags() & 0x20, f"{what}: armed with main+0x2CC6 bit 5 set, so Shift's release disarms"
    start = p.mark()
    p.press("shift")
    p.aim((90, 380))
    p.press("lbutton")
    p.aim((98, 380))
    p.lift("shift")
    p.lift("lbutton")
    p.no_commit(start, what)
    for k in kinds:
        assert p.builds(k) == [], f"{what}: {k} holds {p.builds(k)}"
    # bit 5 still clear: no later record can disarm on Shift's release (0x4992D8)
    assert p.mode() == 14 and not p.flags() & 0x20, \
        f"{what}: mode {p.mode()}, main+0x2CC6 0x{p.flags():02x}: the building is not kept armed"


def right_case(p, kinds=BUILDERS):
    """(l) a right press mid-drag: the engine's cancel (0x499100) disarms and the gesture ends."""
    what = "(l) a right press mid-drag"
    p.clear(*kinds)
    p.arm("ARMLLT")
    start = p.mark()
    p.press("shift")
    p.aim((90, 386))
    p.press("lbutton")
    p.aim((98, 386))
    p.press("rbutton")
    p.lift("rbutton")
    p.lift("lbutton")
    p.lift("shift")
    p.no_commit(start, what)
    for k in kinds:
        assert p.builds(k) == [], f"{what}: {k} holds {p.builds(k)}"
    assert p.mode() == 1, f"{what}: the building stayed armed (mode {p.mode()})"


def level_end(p):
    """(m) The level ended (replay-end.json, as tareplay ends a fixture; the module writes its
    counters at the teardown, so the results screen is left for the menu) and its counters read:
    the cursor site's verdict never disagreed with the engine's (mismatch) and the plan table
    never overflowed (plancap)."""
    start = p.mark()
    tacli("scenario", "apply", p.name, END_SCENARIO, timeout=300)
    p._resume()
    p.held = False
    end, results = time.monotonic() + 300, False
    while not (got := p.place_lines(start, "level end")):
        assert time.monotonic() < end, "(m) no `place: level end` line in 300 s"
        if not results and p.call("ui", "--json")["gui"].upper() == "ENDMSN.GUI":
            p.call("ui", "click", "MainMenu")
            results = True
        time.sleep(1)
    f = got[-1]
    for key in ("mismatch", "plancap"):
        assert f.get(key) == 0, f"(m) the level ended with {key}={f.get(key)}: {f}"
    return f


def core(p):
    p.hold()
    p.eye(*EYE)
    p.select(*BUILDERS)
    for key in "abcde":
        shape_case(p, key)
    ring_case(p)
    _, red = shape_case(p, "g")
    assert red == 3, f"(g) {red} sites of the line lie on the solar, the layout wants 3"
    toggle_case(p)
    mines_case(p)
    stock_shift_case(p)
    plain_case(p)
    shift_first_case(p)
    right_case(p)
    p.notes()
    f = level_end(p)
    print(f"place-core: (a)-(l) passed; (m) level end {f}")


def replay(p):
    """The test group's gestures, on the queue each leaves: a line, a rectangle, a hollow, a ring
    and a toggle, all on ARMCK, so the recording carries the orders of each and the
    builders work through them for the rest of the level."""
    p.hold()
    p.eye(*EYE)
    p.select(*BUILDERS)
    p.clear(*BUILDERS)
    for key in "ade":
        shape_case(p, key, clear=False)
    ring_case(p, clear=False)
    toggle_case(p, clear=False)
    p.notes()
    total = {k: len(p.builds(k)) for k in BUILDERS}
    print(f"place-replay: line, rect, hollow, ring and toggle committed; build orders {total}")


def preview(p, gamedir):
    """V3's count: a 5-site line held mid-drag is what the marker pass draws (its status line
    under mark.on=log), then dropped by releasing Shift first."""
    lever = gamedir / "tagpu_mark.on"
    old = lever.read_text() if lever.exists() else None
    words = (old or "").split()
    lever.write_text(" ".join(words + ([] if "log" in (w.lower() for w in words) else ["log"])) + "\n")
    try:
        p.hold()
        p.eye(*EYE)
        p.select(*BUILDERS)
        p.clear(*BUILDERS)
        p.arm("ARMLLT")
        what, kind, frm, to, mods, n = SHAPES["a"]
        p.press("shift")
        p.aim(frm)
        p.press("lbutton")
        p.aim(to)
        # The second status line after the drag stopped: it comes 120 frames after the first,
        # which may have drawn a packet filled before the last move was handled.
        start = p.mark()
        end = time.monotonic() + 3 * ACK_S
        while True:
            p.mark()
            counts = [c for c in map(mark_place_count, p.log[start:]) if c is not None]
            if len(counts) >= 2:
                break
            assert time.monotonic() < end, f"no two `mark:` status lines with place= in {3 * ACK_S} s"
            time.sleep(0.2)
        assert counts[1] == n, f"V3: the marker pass drew {counts[1]} sites mid-drag, want {n}"
        p.lift("shift")
        p.lift("lbutton")
        for k in BUILDERS:
            assert p.builds(k) == [], f"V3: the dropped drag left {k} with {p.builds(k)}"
        print(f"place-preview: the marker pass drew {n} sites mid-drag")
    finally:
        if old is None:
            lever.unlink(missing_ok=True)
        else:
            lever.write_text(old)


SPACING_RX = r"place: spacing "


def alt_step(p, letter, times):
    """Alt + `letter`, `times` times: Alt+Z one more empty cell, Alt+X one less."""
    for _ in range(times):
        p.press("alt")
        p.keys(f"down:{letter}", f"up:{letter}")
        p.lift("alt")


def spacing_is(p, start, name, want, what):
    """The last `place: spacing` line from log index `start` names `name` at `want`."""
    p.mark()
    got = [ln.split("place: spacing ", 1)[1].strip() for ln in p.log[start:]
           if "place: spacing " in ln]
    assert got and got[-1].upper() == f"{name}={want}".upper(), \
        f"{what}: the spacing lines {got}, want the last to be {name}={want}"


def spacing_line(p, frm, to, spacing, what):
    """A Shift-drag line of ARMLLT laid at `spacing` empty cells, against the model's sites."""
    fx, fz = p.footprint(p.types["ARMLLT"])
    sites = place_shape(LINE, fx, fz, spacing, frm, to)
    before = {k: p.builds(k) for k in BUILDERS}
    start = drag(p, frm, to)
    expect(p, what, "ARMLLT", BUILDERS, before, sites, start, "line")
    return sites


def spacing(p):
    """Alt+Z / Alt+X step the armed type's spacing, a line uses it, it stops at 0, and it is
    in impure.cfg after the level ends and in force after a relaunch."""
    p.hold()
    p.eye(*EYE)
    p.select(*BUILDERS)
    p.clear(*BUILDERS)
    p.arm("ARMLLT")
    start = p.mark()
    alt_step(p, "z", 2)
    spacing_is(p, start, "ARMLLT", 2, "(s1) Alt+Z twice")
    assert p.mode() == 14, "(s1) Alt+Z disarmed the building"
    sites = spacing_line(p, (70, 356), (78, 356), 2, "(s2) a line at spacing 2")
    assert [c for c, _ in sites] == [70, 74, 78], f"(s2) the layout {sites}"
    p.clear(*BUILDERS)
    p.arm("ARMLLT")
    start = p.mark()
    alt_step(p, "x", 3)
    spacing_is(p, start, "ARMLLT", 0, "(s3) Alt+X three times stops at 0")
    spacing_line(p, (70, 362), (78, 362), 0, "(s4) a line at spacing 0")
    p.clear(*BUILDERS)
    p.arm("ARMLLT")
    start = p.mark()
    alt_step(p, "z", 1)
    spacing_is(p, start, "ARMLLT", 1, "(s5) Alt+Z once")
    p.notes()
    level_end(p)
    text = (p.gamedir / "impure.cfg").read_text()
    assert re.search(r"(?im)^placespacing=(.*,)?ARMLLT:1(,|$)", text), \
        f"(s6) impure.cfg after the level end has no ARMLLT:1: {[ln for ln in text.splitlines() if 'spacing' in ln.lower()]}"
    print("place-spacing: (s1)-(s6) passed")


def spacing_relaunch(p):
    """(s7) After a relaunch the saved spacing is in force: ARMLLT:1 lays a line at pitch 3."""
    p.hold()
    p.eye(*EYE)
    p.select(*BUILDERS)
    p.clear(*BUILDERS)
    p.arm("ARMLLT")
    sites = spacing_line(p, (70, 368), (79, 368), 1, "(s7) a line at the saved spacing 1")
    assert [c for c, _ in sites] == [70, 73, 76, 79], f"(s7) the layout {sites}"
    p.notes()
    print("place-spacing: (s7) the saved spacing holds after a relaunch")


# -- offline --------------------------------------------------------------------------------------

def selftest(cases=3000, seed=20261025):
    """The Python shapes against tagpu_placegeo.c through `placetest.py dump`: the layout's own
    cases and `cases` random ones, every kind, footprints 1..8 and 64, spacing 0..16."""
    rng = random.Random(seed)
    lines = []
    for _, kind, frm, to, _, _ in SHAPES.values():
        lines.append((kind, 2, 2, 0, frm, to))
    lines.append((LINE, 1, 1, 0, *MINE_LINE))
    lines.append((LINE, 2, 2, 0, *TOGGLE_LINE))
    for _ in range(cases):
        fx, fz = (rng.choice([1, 2, 3, 4, 5, 8, 64]) for _ in range(2))
        frm = (rng.randint(-200, 200), rng.randint(-200, 200))
        to = (frm[0] + rng.randint(-120, 120), frm[1] + rng.randint(-120, 120))
        lines.append((rng.choice([LINE, AXIS, RECT, HOLLOW]), fx, fz, rng.randint(0, 16), frm, to))
    rings = [(2, 2, 98, 360, 5, 5)] + [
        (rng.randint(1, 4), rng.randint(1, 4), rng.randint(-50, 50), rng.randint(-50, 50),
         rng.randint(1, 8), rng.randint(1, 8)) for _ in range(cases // 4)]
    text = "".join(f"shape {k} {fx} {fz} {s} {a[0]} {a[1]} {b[0]} {b[1]}\n"
                   for k, fx, fz, s, a, b in lines)
    text += "".join("ring " + " ".join(map(str, r)) + "\n" for r in rings)
    r = subprocess.run([sys.executable, str(PLACETEST), "dump"], input=text,
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, f"placetest.py dump: {(r.stdout + r.stderr).strip()}"
    native = [ln for ln in r.stdout.splitlines() if not ln.startswith("placetest:")]
    ours = [place_shape(k, fx, fz, s, a, b) for k, fx, fz, s, a, b in lines]
    ours += [place_ring(*ring) for ring in rings]
    assert len(native) == len(ours), f"placetest.py dump answered {len(native)} of {len(ours)} cases"
    for case, want, got in zip(text.splitlines(), native, ours):
        mine = " ".join(f"{x},{z}" for x, z in got)
        assert mine == want, f"{case}: python {mine!r}, native {want!r}"
    print(f"place selftest: {len(ours)} shapes and rings equal tagpu_placegeo.c's")


def launched(instance, mode):
    """The instance launched on the scenario with the launch's hold, under the run lock; the
    callable that stops it and takes the hold lever back."""
    runlock.hold(TREE, f"place_regression {mode} on {instance}")
    gamedir = tacli_api().Instance(instance).gamedir
    lever = gamedir / "tagpu_hold.on"
    had = lever.exists()
    lever.write_text("")
    # Every case lays its sites with no spacing but `spacing`'s own, which a store the instance
    # keeps between launches (tacli ensure_store leaves an existing one alone) would carry over.
    store = gamedir / "impure.cfg"
    if store.exists():
        kept = [ln for ln in store.read_text().splitlines(keepends=True)
                if not ln.strip().lower().startswith("placespacing=")]
        store.write_text("".join(kept))
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
    p.add_argument("instance", nargs="?")
    p.add_argument("mode", nargs="?", choices=("core", "replay", "preview", "spacing"))
    p.add_argument("--launch", action="store_true",
                   help="core/preview: launch the instance on place-basic.json first (run lock)")
    p.add_argument("--selftest", action="store_true", help="geometry against placetest.py, no game")
    a = p.parse_args()
    if a.selftest:
        try:
            selftest()
        except CHECK_FAILURES as e:
            print(f"place selftest FAILED: {e}", file=sys.stderr)
            return 1
        return 0
    if not a.instance or not a.mode:
        p.error("an instance and a mode, or --selftest")
    if a.mode == "spacing" and not a.launch:
        p.error("spacing relaunches the instance: it takes --launch")
    if a.launch and a.mode == "replay":
        p.error("replay runs inside its test group (tools/replay/fixtures.json, place-basic)")
    stop = None
    try:
        if a.launch:
            stop = launched(a.instance, a.mode)
        check = Place(a.instance)
        def spacing_both(c):
            spacing(c)
            # the second launch keeps the store: only the hold lever and the scenario again
            tacli("stop", a.instance, timeout=300)
            lever = c.gamedir / "tagpu_hold.on"
            lever.write_text("")
            tacli("scenario", "load", a.instance, SCENARIO, "--defaults", timeout=900)
            spacing_relaunch(Place(a.instance))
        run = {"core": core, "replay": replay, "spacing": spacing_both,
               "preview": lambda c: preview(c, c.gamedir)}[a.mode]
        # As resource_regression.py: the case's own failure is the exit reason and is printed
        # first; a cleanup step that fails after it is reported beside it, never in its place.
        failures, raised = [], True
        try:
            run(check)
            raised = False
        except CHECK_FAILURES as e:
            failures.append(("", e))
            raised = False
        finally:
            cleanup = [("let go", check.let_go), ("release", check.release)]
            for what, step in cleanup:
                try:
                    step()
                except CHECK_FAILURES as e:
                    failures.append((what, e))
            for i, (what, e) in enumerate(failures):
                if i == 0 and not raised:
                    print(f"place regression FAILED: {what + ': ' if what else ''}{e}",
                          file=sys.stderr)
                else:
                    print(f"place regression: {what} failed as well: {e}", file=sys.stderr)
        if failures:
            return 1
    except CHECK_FAILURES as e:
        print(f"place regression FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if stop:
            stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
