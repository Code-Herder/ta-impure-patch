#!/usr/bin/env python3
"""The in-game wiki in the game (tagpu_wiki.c; research/notes/ingame-wiki-plan.md, landings 2 to 4):
its button, its window, the input it takes while open, the pause it holds in single player, the
page's controls and the demos playing on the stage.

    wiki_regression.py <instance> sp --launch         a single-player skirmish, not recorded
    wiki_regression.py <instance> page --launch       the page's controls, single player
    wiki_regression.py <instance> demos --launch      the demos: parity, clock, controls, captures
    wiki_regression.py <instance> recorded --launch   a live recorded skirmish: the rewind button
    wiki_regression.py <name> mp                      a two-player network game on Wine, started by
                                                      tools/compat/tacompat.py's launcher

`sp` and `recorded` play scenarios/place-basic.json at --res (default 1920x1080) with the launch's
hold (tagpu_hold.on), held at GameTime 120 and then let run, under the run lock (tools/runlock.py).
The shield is armed, so every device injection (`dmove:`, `dclick:`, `drclick:`, `ddown:`/`dup:`)
walks the shield's injected path, the one tagpu_wiki_mouse sits on. `mp` builds the compatibility
suite's two retail instances on their own DirectPlay port and holds the lock as well.

What the game says is read from the wiki's own state dump (tagpu_wiki.dump -> tagpu_wiki.json: open,
the pause, the hit map of the last frame drawn, the engine screen and the viewport), its log lines
(`wiki: open at ...`, `wiki: closed (...)`), and engine memory through `tacli peek`: GameTime
main+0x38A47, the pause bit main+0x38A51 bit 0, the eye main+0x1431F/+0x14323, the key ring's
indices 0x51F48E/0x51F492, a unit's selection bit (+0x110 & 0x10) and its orders (Check.orders).

THE MARGIN is the world between the window (90 % of the screen, centred) and the screen's edge. The
ARMCK is brought into it with the camera, and every margin point is used twice: closed, as the
control that shows a click there selects, deselects and orders; open, where the same click must do
nothing.

sp, in order:
  (a) closed: the button sits in the rewind button's slot (no recording), 28 bar pixels square,
      its right edge 47 bar pixels from the screen's; device clicks on the ARMCK and on bare ground
      in the margin select and deselect it, and a right click there orders it;
  (b) a device click on the button opens the window: the dump says open and paused, GameTime
      stands still over 2 s, the pause bit is set;
  (c) left and right device clicks on the margin, over the selected ARMCK and over bare ground,
      leave the selection and its orders unchanged;
  (d) an arrow held 1 s and a device move held 1 s on the screen's last column leave the eye where
      it was; the Pause key leaves the game paused; the key ring's two indices stay equal;
  (e) the Esc press leaves the window open, its release closes it; the pause bit is clear and
      GameTime runs; an arrow held 1 s now moves the eye (the control for (d));
  (f) reopened, a press on the close button released over the ARMCK closes it and leaves the
      selection as it was;
  (g) reopened on a world the test's own hold keeps paused (the wiki holds the pause it finds and
      will give nothing back: `pause_was`), the level is ended with scenarios/replay-end.json: the
      log says the window closed at the level's end, and an injected key reaches the engine's key
      ring at the results screen.

--shot PATH grabs the presented frame with the window open, at (b), for the owner: the whole
window by its id on the instance's display (`import -window`, the ta-capture skill's window grab).
page, on the same launch as sp:
  (p1) the open picks the tree's first entry and its first demo, playing, and draws every control;
  (p2) entry:3 selects the second entry, demo:1 its second demo, afresh;
  (p3) fold:1 folds the Building category (no entry row drawn) and unfolds it;
  (p4) two wheel notches down over the text scroll it two steps, one up one back, each held to
       the limit the drawn frame posted;
  (p5) the text's thumb dragged 12 px scrolls it by the travel the drawn frame gave it;
  (p6) Esc closes and frees the demo; reopened, the selection stands and the demo starts afresh.
demos, on the same launch as sp:
  (d1) tagpu_wiki.check plays every demo to its end in the game, and every logged hash equals
       `node tools/wiki/simparity.mjs --hash`;
  (d2) a demo runs about 30 ticks a second at 1x and twice that at 2x;
  (d3) play pauses (the tick stands), replay restarts, an ended demo holds its last frame and
       play starts it again;
  (d4) each entry with a demo paused mid-play; with --shots DIR the game's frame and the mockup's
       at the same screen, entry, demo and tick, side by side (`<res>-<entry>-<demo>-t<tick>`);
  (d5) the most one fill of the open page cost the game thread, logged.
recorded:
  (r) the button sits 3 bar pixels left of the rewind button (the two frames' hit maps); the rewind
      button opens the rewind and closes it; the wiki button opens the wiki and Esc closes it.
mp:
  (m) the wiki opened on the host takes no pause: both peers' GameTime advances over 3 s, and so
      does the host's demo, whose cost to the game thread is logged; a right and a left click on
      the margin leave the host's selected commander's orders and selection as they were; Esc
      closes it, and both peers still run, with no desync line in either log.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

from resource_regression import CHECK_FAILURES, TACLI, TREE, Check, tacli_api
import runlock

SCENARIO = TREE / "scenarios/place-basic.json"
END_SCENARIO = TREE / "scenarios/replay-end.json"
MAIN = "*0x511DE8"
GAMETIME = f"{MAIN}+0x38a47"
PAUSE = f"{MAIN}+0x38a51:2"
EYE = (f"{MAIN}+0x1431f", f"{MAIN}+0x14323")
KEY_RING = ("0x51F48E", "0x51F492")      # the key ring's enqueue and dequeue indices
HELD_AT = 120                            # the plan's GameTime for the single-player case
DUMP_S = 30
GAP = 3                                  # client pixels kept clear of a margin's edges


def tacli(*args, timeout=600):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    assert r.returncode == 0, f"tacli {' '.join(map(str, args[:2]))}: {(r.stdout + r.stderr).strip()}"
    return r.stdout


class Wiki(Check):
    """resource_regression's Check, with the wiki's state and the margin."""

    def attach(self, name):
        """The log reader and the trigger file without Check's roster wait (the network game)."""
        self.name = name
        self.lockstep()
        self.base = self.values(f"{MAIN}+0x14357")[0]
        self.units = {}

    # -- the wiki's own state
    def dump(self):
        """The wiki's state dump, fresh: asked for and waited for (the fill writes it within 30
        fills of the ask)."""
        out, ask = self.gamedir / "tagpu_wiki.json", self.gamedir / "tagpu_wiki.dump"
        out.unlink(missing_ok=True)
        ask.write_text("")
        end = time.monotonic() + DUMP_S
        while True:
            if out.exists() and not ask.exists():
                try:
                    return json.loads(out.read_text())
                except ValueError:
                    pass
            assert time.monotonic() < end, f"no tagpu_wiki.json in {DUMP_S} s: the wiki filled no record"
            time.sleep(0.1)

    @staticmethod
    def hit(d, key):
        got = [h["rect"] for h in d["hits"] if h["key"] == key]
        assert got, f"the last frame drawn has no '{key}' control: {[h['key'] for h in d['hits']]}"
        return got[0]

    @staticmethod
    def centre(r):
        return (r[0] + r[2]) // 2, (r[1] + r[3]) // 2

    @staticmethod
    def client(d, gx, gy):
        """An engine screen point as the client point a device injection names."""
        vx, vy, vw, vh = d["viewport"]
        gw, gh = d["game"]
        return vx + round(gx * vw / gw), vy + round(gy * vh / gh)

    @staticmethod
    def margins(d):
        """The margin's three strips the world shows through, as client rects [x0, y0, x1, y1):
        right of the window, above it under the top bar, below it over the bottom bar. The left
        strip lies under the engine's side panel. The window's rect is computed as the layout
        places it (90 %, centred), because a closed frame's hit map has no window."""
        vx, vy, vw, vh = d["viewport"]
        K = vh / d["game"][1]
        wy0 = vy + vh * 0.05
        wx1, wy1 = vx + vw * 0.95, vy + vh * 0.95
        bar = 32 * K
        return [(wx1 + GAP, vy + bar + GAP, vx + vw - GAP, vy + vh - bar - GAP),
                (vx + 128 * K + GAP, vy + bar + GAP, vx + vw - GAP, wy0 - GAP),
                (vx + 128 * K + GAP, wy1 + GAP, vx + vw - GAP, vy + vh - bar - GAP)]

    # -- the engine
    def gametime(self):
        return self.values(GAMETIME)[0]

    def paused(self):
        return self.values(PAUSE)[0] & 1

    def eye_at(self):
        """The eye, signed: peek answers the dword unsigned, and the camera goes past the map's
        north and west edges (the view centre stays on the map, gpu-status.md §2.3c)."""
        return tuple(v - (1 << 32) if v >= 1 << 31 else v for v in self.values(*EYE))

    def selected(self, kind):
        return bool(self.values(f"0x{self.address(kind) + 0x110:x}")[0] & 0x10)

    def runs(self, seconds):
        a = self.gametime()
        time.sleep(seconds)
        return self.gametime() != a

    def ring(self):
        return tuple(self.values(*KEY_RING))

    def found(self, start, rx):
        return self._found(rx, start)

    def mark(self):
        self.log += [ln.rstrip() for ln in self.reader()]
        return len(self.log)

    def settle_eye(self, x, z):
        """The camera held at (x, z), or as near as the map's edge lets it come. The hold is taken
        at a later frame, so the eye is read until it reads (x, z); one the edge stops short of it
        is taken once two reads 2 s apart agree after that wait."""
        self.call("eye", x, z)
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if self.eye_at() == (x, z):
                return (x, z)
            time.sleep(0.2)
        last, end = self.eye_at(), time.monotonic() + 30
        while True:
            time.sleep(2.0)
            now = self.eye_at()
            if now == last:
                return now
            assert time.monotonic() < end, "the camera never settled"
            last = now

    def to_margin(self, d, kind):
        """The camera moved so that `kind` sits in one of the margin's strips; the unit's client
        point, a bare-ground point in the same strip, and the strip."""
        b = self.bytes(f"0x{self.address(kind) + 0x6a:x}:xc")
        wx, h, wz = (v >> 16 for v in struct.unpack("<iii", b))
        vx, vy, vw, vh = d["viewport"]
        gw, gh = d["game"]
        for s in self.margins(d):
            if s[2] - s[0] < 12 or s[3] - s[1] < 12:
                continue
            # the strip's middle as an engine point, and the eye that puts the unit there
            tx = ((s[0] + s[2]) / 2 - vx) * gw / vw
            ty = ((s[1] + s[3]) / 2 - vy) * gh / vh
            ex, ez = self.settle_eye(round(wx + 128 - tx), round(wz - h // 2 + 32 - ty))
            sx, sy = wx - ex + 128, wz - h // 2 - ez + 32
            ux, uy = self.client(d, sx, sy)
            if not (s[0] <= ux < s[2] and s[1] <= uy < s[3]):
                continue
            # bare ground: along the strip, away from the unit, inside it
            along = (s[2] - s[0]) >= (s[3] - s[1])
            for off in (260, -260, 160, -160):
                gx, gy = (ux + off, uy) if along else (ux, uy + off)
                if s[0] <= gx < s[2] and s[1] <= gy < s[3]:
                    return (ux, uy), (round(gx), round(gy)), s
        raise AssertionError(f"no camera puts the {kind} in the margin {self.margins(d)}")

    def open_wiki(self, expect_pause):
        d = self.dump()
        assert not d["open"], f"the window is already open: {d}"
        start = self.mark()
        self.keys("dclick:%d,%d" % self.centre(self.hit(d, "wiki")))
        d = self.dump()
        assert d["open"] and d["holds"], f"a click on the wiki button did not open the window: {d}"
        assert self.found(start, r"wiki: open at"), "no `wiki: open at` line"
        assert bool(d["paused"]) == expect_pause, f"the window open, the wiki's pause reads {d['paused']}"
        return d


def held_at(c, T):
    """The launch's hold carried to GameTime T and then let go: the world runs from T."""
    c.hold()
    start = c._pause_at(T)
    c._resume()
    c._taken(T, start)
    c._resume()
    c.held = False


def grab(c, path):
    """The presented frame: the game window grabbed by its id on the instance's display."""
    me = next(i for i in json.loads(tacli("ls", "--json")) if i["name"] == c.name)
    r = subprocess.run(["import", "-display", me["display"], "-window", str(me["window"][0]), str(path)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, f"import: {r.stderr.strip()}"
    print(f"wiki: the open window grabbed to {path}")


def sp(c, shot=None):
    held_at(c, HELD_AT)
    d = c.dump()
    vx, vy, vw, vh = d["viewport"]
    K = vh / d["game"][1]

    # (a) the rewind button's slot; the margin's clicks, closed
    r = c.hit(d, "wiki")
    right = vx + vw - round(47 * K)
    assert abs(r[2] - right) <= 1 and abs((r[2] - r[0]) - round(28 * K)) <= 1 \
        and abs(r[1] - (vy + round(2 * K))) <= 1, \
        f"(a) the wiki button {r} is not in the rewind button's slot (right edge {right}, side {28 * K:g})"
    assert not d["open"] and not d["holds"], f"(a) the window is open before any click: {d}"
    c.keys("esc")
    unit, ground, strip = c.to_margin(d, "ARMCK")
    c.keys("dclick:%d,%d" % unit)
    c.once(c.selected("ARMCK"), f"(a) a device click on the ARMCK at {unit} did not select it")
    c.keys("dclick:%d,%d" % ground)
    c.once(not c.selected("ARMCK"), f"(a) a device click on bare ground at {ground} did not deselect it")
    c.keys("dclick:%d,%d" % unit)
    c.once(c.selected("ARMCK"), "(a) the ARMCK did not select again")
    before = c.orders("ARMCK")
    c.keys("drclick:%d,%d" % ground)
    c.until(lambda: c.orders("ARMCK") != before, "(a) a right click on bare ground ordered nothing")
    c.call("order", "stop", "--unit", c.units["ARMCK"], "--expect", "ARMCK")
    print(f"wiki sp: (a) the button at {r}; closed, the margin {tuple(round(v) for v in strip)} selects, "
          "deselects and orders")

    # (b) open: paused
    d = c.open_wiki(expect_pause=True)
    assert not d["pause_was"], "(b) single player was paused before the wiki"
    c.once(c.paused(), "(b) the pause bit is clear with the window open")
    c.once(not c.runs(2.0), "(b) GameTime ran with the window open")
    if shot:
        grab(c, shot)
    print("wiki sp: (b) open; paused, GameTime stands")

    # (c) the margin's clicks, open
    before = c.orders("ARMCK")
    ring = c.ring()
    assert ring[0] == ring[1], f"(c) the key ring holds keys before the clicks: {ring}"
    for tok in ("dclick:%d,%d" % unit, "drclick:%d,%d" % unit,
                "dclick:%d,%d" % ground, "drclick:%d,%d" % ground):
        c.keys(tok)
        c.once(c.selected("ARMCK"), f"(c) {tok} on the margin changed the selection")
        c.once(c.orders("ARMCK") == before, f"(c) {tok} on the margin changed the ARMCK's orders")
    print("wiki sp: (c) the margin's clicks took nothing")

    # (d) the camera holds; the Pause key; the key ring
    c.call("eye", "--release")
    eye = c.eye_at()
    c.keys("down:right")
    time.sleep(1.0)
    c.keys("up:right")
    c.once(c.eye_at() == eye, f"(d) an arrow held 1 s moved the eye {eye} -> {c.eye_at()}")
    c.keys(f"dmove:{vx + vw - 1},{vy + vh // 2}")
    time.sleep(1.0)
    c.once(c.eye_at() == eye, f"(d) the last column held 1 s scrolled the eye {eye} -> {c.eye_at()}")
    c.keys("dmove:%d,%d" % ground)
    c.keys("pause")
    c.once(c.paused() and not c.runs(1.0), "(d) the Pause key let the game run")
    c.once(c.ring() == ring, f"(d) the key ring moved with the window open: {ring} -> {c.ring()}")
    print("wiki sp: (d) the eye held, the Pause key took nothing, the key ring did not move")

    # (e) Esc: the press keeps it open, the release closes it
    start = c.mark()
    c.keys("down:esc")
    d = c.dump()
    assert d["open"], "(e) the Esc press closed the window"
    c.keys("up:esc")
    d = c.dump()
    assert not d["open"] and not d["holds"] and not d["paused"], f"(e) the Esc release did not close it: {d}"
    assert c.found(start, r"wiki: closed"), "(e) no `wiki: closed` line"
    c.once(c.ring() == ring, "(e) the Esc press or release reached the key ring")
    c.once(not c.paused(), "(e) the pause bit is still set after the close")
    c.once(c.runs(2.0), "(e) GameTime stands after the close")
    eye = c.eye_at()
    c.keys("down:right")
    time.sleep(1.0)
    c.keys("up:right")
    c.once(c.eye_at() != eye, "(e) the control: an arrow held 1 s with the window closed did not move the eye")
    print("wiki sp: (e) Esc closed it on the release; the game runs; the arrow scrolls again")

    # (f) the close button, released over the ARMCK
    d = c.dump()
    unit, ground, strip = c.to_margin(d, "ARMCK")
    c.once(c.selected("ARMCK"), "(f) the ARMCK is not selected before the close")
    d = c.open_wiki(expect_pause=True)
    x = c.centre(c.hit(d, "close"))
    c.keys(f"ddown:{x[0]},{x[1]}", "dmove:%d,%d" % unit, "dup:%d,%d" % unit)
    d = c.dump()
    assert not d["open"] and not d["owed_buttons"], f"(f) the close button did not close it cleanly: {d}"
    c.once(c.selected("ARMCK"), "(f) the close button's release over the ARMCK changed the selection")
    c.once(not c.paused(), "(f) the pause bit is still set after the close")
    print("wiki sp: (f) the close button closed it; its release took nothing")

    # (g) the level ends with the window open, on the test's own hold
    T = c.gametime() + 30
    start = c._pause_at(T)
    c._taken(T, start)
    d = c.open_wiki(expect_pause=True)
    assert d["pause_was"] == 1, f"(g) the wiki did not see the hold's pause: {d}"
    start = c.mark()
    tacli("scenario", "apply", c.name, END_SCENARIO, timeout=300)
    c._resume()
    c.held = False
    end = time.monotonic() + 300
    while not c.found(start, r"wiki: closed \(the level ended\)"):
        assert time.monotonic() < end, "(g) no `wiki: closed (the level ended)` line in 300 s"
        time.sleep(1)
    while "ENDMSN" not in c.call("ui", "--json")["gui"].upper():
        assert time.monotonic() < end, "(g) the results screen never came up"
        time.sleep(1)
    # no dump here: the fill runs on in-play frames only, and the log line above is the close
    ring = c.ring()
    c.keys("a")
    c.once(c.ring()[0] != ring[0], "(g) an injected key did not reach the engine's key ring at the results screen")
    print("wiki sp: (g) the window closed at the level's end; the results screen takes keys")


# ---- the page and the demos (landings 3 and 4) ----------------------------------------------------

TEXT = 2                                 # WIKI_SCROLL_TEXT: the panels are tree, demos, text
WHEEL_UNITS = 48                         # tagpu_wiki.c: a notch scrolls three of the tree's rows


def wait_dump(c, pred, what, seconds=20):
    """The state dump until `pred` holds of it: a press is the game thread's at once, but what a
    frame drew (the hit map, a scroll as drawn) is the next frame's."""
    end = time.monotonic() + seconds
    while True:
        d = c.dump()
        if pred(d):
            return d
        assert time.monotonic() < end, f"{what}: {json.dumps({k: d[k] for k in d if k != 'hits'})[:600]}"
        time.sleep(0.3)


def press(c, d, key):
    c.keys("dclick:%d,%d" % c.centre(c.hit(d, key)))


def keys_of(d, prefix):
    return sorted(h["key"] for h in d["hits"] if h["key"].startswith(prefix))


def page(c):
    """(p1)..(p6): the page's controls, against the dump's state."""
    held_at(c, HELD_AT)
    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and "entry:3" in keys_of(d, "entry:"), "(p1) the page was not drawn")
    assert (d["entry"], d["demo"], d["sim"], d["stage"]) == ("quickbuild", "snap", 1, 1), \
        f"(p1) the open did not pick the tree's first entry and its first demo, playing: {d['entry']}/{d['demo']}"
    for k in ("fold:0", "fold:1", "entry:2", "entry:3", "demo:0", "play", "replay", "x1", "x2", "try",
              "stage", "scroll:tree", "scroll:demos", "scroll:text", "close", "wiki", "window"):
        c.hit(d, k)
    print(f"wiki page: (p1) open on quickbuild/snap, {len(d['hits'])} controls drawn")

    press(c, d, "entry:3")
    d = wait_dump(c, lambda d: d["entry"] == "buildshapes" and d["demo"] == "clicks", "(p2) entry:3 did not select buildshapes")
    d = wait_dump(c, lambda d: "demo:9" in keys_of(d, "demo:"), "(p2) buildshapes' ten demos were not drawn")
    press(c, d, "demo:1")
    d = wait_dump(c, lambda d: d["demo"] == "line" and d["tick"] < 200, "(p2) demo:1 did not start buildshapes/line")
    print("wiki page: (p2) the second entry and its second demo selected")

    press(c, d, "fold:1")
    d = wait_dump(c, lambda d: d["folds"] == [1] and not keys_of(d, "entry:"), "(p3) fold:1 did not fold Building")
    press(c, d, "fold:1")
    d = wait_dump(c, lambda d: d["folds"] == [] and keys_of(d, "entry:") == ["entry:2", "entry:3"],
                  "(p3) fold:1 did not unfold Building")
    print("wiki page: (p3) Building folded and unfolded")

    mx = d["scroll_max"][TEXT]
    assert mx > 0, f"(p4) the text does not scroll at this resolution: {d['scroll_max']}"
    x, y = c.centre(c.hit(d, "scroll:text"))
    c.keys(f"dwheel:{x},{y},-2")
    want = min(2 * WHEEL_UNITS, mx)
    d = wait_dump(c, lambda d: abs(d["scroll_drawn"][TEXT] - want) < 0.02, f"(p4) two notches down did not scroll the text to {want}")
    c.keys(f"dwheel:{x},{y},1")
    want -= WHEEL_UNITS
    d = wait_dump(c, lambda d: abs(d["scroll_drawn"][TEXT] - max(0, want)) < 0.02, "(p4) a notch up did not scroll back")
    print(f"wiki page: (p4) the wheel scrolled the text to {d['scroll_drawn'][TEXT]:g} of {mx:g} units")

    b = d["bars"][TEXT]
    assert b["track"][3] > b["track"][1] and b["thumb"][1] > 0, f"(p5) the text panel shows no scrollbar: {b}"
    tx, ty = (b["track"][0] + b["track"][2]) // 2, b["thumb"][0] + b["thumb"][1] // 2
    before, dy = d["scroll_drawn"][TEXT], 12
    c.keys(f"ddown:{tx},{ty}", f"dmove:{tx},{ty + dy}", f"dup:{tx},{ty + dy}")
    want = min(mx, before + dy * b["per_px"])
    d = wait_dump(c, lambda d: abs(d["scroll_drawn"][TEXT] - want) < 0.05,
                  f"(p5) a {dy} px drag of the thumb did not scroll the text from {before:g} to {want:g}")
    print(f"wiki page: (p5) the thumb dragged {dy} px scrolled the text {before:g} -> {d['scroll_drawn'][TEXT]:g}")

    c.keys("esc")
    d = wait_dump(c, lambda d: not d["open"] and not d["sim"], "(p6) Esc did not close the window and free the demo")
    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and d["sim"], "(p6) the reopened page was not drawn")
    assert (d["entry"], d["demo"], d["folds"]) == ("buildshapes", "line", []), \
        f"(p6) the reopened page lost its selection: {d['entry']}/{d['demo']} folds {d['folds']}"
    assert d["tick"] < 200, f"(p6) the demo did not start afresh at the open: tick {d['tick']}"
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(p6) Esc did not close it again")
    print("wiki page: (p6) closed and reopened on the same entry and demo, the demo afresh")


def simparity_hashes():
    r = subprocess.run(["node", str(TREE / "tools/wiki/simparity.mjs"), "--hash"], capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 0, f"simparity --hash: {r.stderr.strip()}"
    return dict(ln.split() for ln in r.stdout.splitlines() if ln.strip())


def tick_rate(c, seconds=4.0):
    a, t0 = c.dump(), time.monotonic()
    time.sleep(seconds)
    b = c.dump()
    return (b["tick"] - a["tick"]) / (time.monotonic() - t0), b


def mockup_shot(state, path):
    """The mockup at the same state -- the screen, the entry, the demo paused at the tick -- grabbed
    headless by playwright-cli, one CSS pixel a device pixel, as wikilaycheck.py opens it."""
    sys.path.insert(0, str(TREE / "tools/wiki"))
    import wikilaycheck as wl
    from PIL import Image
    gw, gh = state["res"]
    httpd = wl.serve()
    tmp = path.parent / f".mock{os.getpid()}"
    tmp.mkdir(parents=True, exist_ok=True)
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"contextOptions": {"deviceScaleFactor": 1, "viewport": {"width": 1200, "height": 900}},
                                           "launchOptions": {"ignoreDefaultArgs": ["--hide-scrollbars"]}}}))
    pw = ["playwright-cli", f"-s=wikishot{os.getpid()}", f"--config={cfg}"]
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    try:
        run("open", "about:blank")
        run("resize", str(gw + 34), str(gh + 900))
        run("goto", f"http://127.0.0.1:{httpd.server_address[1]}/tools/wiki/mockup/index.html?res={gw}x{gh}&open&skin=kit"
                    f"&mod=stock&rewind=0&entry={state['entry']}&demo={state['demo_n'] + 1}")
        for _ in range(100):
            if wl.result(run("eval", "() => !!(window.WikiPage && WikiPage.sim() && document.fonts.status === 'loaded'"
                                     " && document.querySelector('#win canvas.skin'))").stdout):
                break
            time.sleep(0.1)
        v = wl.result(run("eval", "async () => { const S = WikiPage.state, $ = s => document.querySelector(s);"
                                  " if (S.playing) $('#c-play').click(); const sim = WikiPage.sim();"
                                  f" while (sim.tick < {state['tick']} && !sim.done) sim.step();"
                                  " for (let i = 0; i < 4; i++) await new Promise(r => requestAnimationFrame(r));"
                                  " const f = $('#frame').getBoundingClientRect();"
                                  " return JSON.stringify([f.left, f.top, f.width, f.height, sim.tick]); }").stdout)
        fx, fy, fw, fh, tick = json.loads(v)
        assert (round(fw), round(fh)) == (gw, gh) and tick == state["tick"], \
            f"the mockup's frame is {fw} x {fh} at tick {tick}, not {gw} x {gh} at {state['tick']}"
        run("screenshot", f"--filename={tmp / 'page.png'}")
        Image.open(tmp / "page.png").crop((round(fx), round(fy), round(fx) + gw, round(fy) + gh)).save(path)
        print(f"wiki: the mockup at the same state grabbed to {path}")
    finally:
        run("close")
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)


def demos(c, shots, res):
    """(d1)..(d6): the demos' parity in the game, their clock, the controls, captures beside the
    mockup's, and what the page costs the game thread."""
    held_at(c, HELD_AT)
    want = simparity_hashes()
    start = c.mark()
    (c.gamedir / "tagpu_wiki.check").write_text("")
    end = time.monotonic() + 120
    while not (line := c.found(start, r"wiki check: \d+ demos played")):
        assert time.monotonic() < end, "(d1) no `wiki check: N demos played` line in 120 s"
        time.sleep(0.5)
    got = dict(m.groups() for ln in c.log[start:] if (m := re.search(r"wiki check: (\S+/\S+) ([0-9a-f]{16})$", ln)))
    differ = sorted(k for k in want if got.get(k) != want[k])
    assert not differ and len(got) == len(want), \
        f"(d1) the game's hashes differ from simparity --hash for {differ or sorted(set(got) ^ set(want))}"
    print(f"wiki demos: (d1) {line.split('wiki check: ', 1)[1]}; all {len(want)} hashes equal simparity --hash")

    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and d["sim"], "(d2) the page was not drawn")
    press(c, d, "demo:3")
    wait_dump(c, lambda d: d["demo"] == "bestfit" and d["tick"] < 100, "(d2) demo:3 did not start quickbuild/bestfit")
    r1, d = tick_rate(c)
    press(c, d, "x2")
    wait_dump(c, lambda d: d["speed"] == 2, "(d2) x2 did not set 2x")
    r2, d = tick_rate(c)
    assert 15 <= r1 <= 45 and 1.5 <= r2 / r1 <= 2.5, f"(d2) the demo ran {r1:.1f} ticks/s at 1x and {r2:.1f} at 2x"
    print(f"wiki demos: (d2) quickbuild/bestfit at {r1:.1f} ticks/s at 1x, {r2:.1f} at 2x")

    press(c, d, "play")
    d = wait_dump(c, lambda d: not d["playing"], "(d3) play did not pause the demo")
    t = d["tick"]
    time.sleep(1.5)
    d = c.dump()
    assert d["tick"] == t, f"(d3) the paused demo ran from tick {t} to {d['tick']}"
    press(c, d, "replay")
    d = wait_dump(c, lambda d: d["playing"] and d["tick"] < t, "(d3) replay did not restart the demo, playing")
    press(c, d, "demo:5")
    d = wait_dump(c, lambda d: d["demo"] == "cancel", "(d3) demo:5 did not select quickbuild/cancel")
    d = wait_dump(c, lambda d: d["done"], "(d3) quickbuild/cancel did not end at 2x", seconds=40)
    t = d["tick"]
    time.sleep(1.0)
    d = c.dump()
    assert d["done"] and d["tick"] == t, f"(d3) the ended demo did not hold its last frame: {t} -> {d['tick']}"
    press(c, d, "play")
    d = wait_dump(c, lambda d: not d["done"] and d["tick"] < t, "(d3) play on an ended demo did not start it again")
    press(c, d, "x1")
    wait_dump(c, lambda d: d["speed"] == 1, "(d3) x1 did not set 1x")
    print("wiki demos: (d3) pause, replay, the last frame held and played again")

    # (d4) each entry with a demo paused mid-play, grabbed beside the mockup at the same state
    for node, entry, k, demo, at in ((2, "quickbuild", 0, "snap", 230), (3, "buildshapes", 1, "line", 250)):
        d = c.dump()
        press(c, d, f"entry:{node}")
        d = wait_dump(c, lambda d: d["entry"] == entry and d["drawn_open"], f"(d4) entry:{node} did not select {entry}")
        if k:
            d = wait_dump(c, lambda d: f"demo:{k}" in keys_of(d, "demo:"), f"(d4) {entry}'s demos were not drawn")
            press(c, d, f"demo:{k}")
        d = wait_dump(c, lambda d: d["demo"] == demo and d["tick"] >= at, f"(d4) {entry}/{demo} did not reach tick {at}", seconds=60)
        press(c, d, "play")
        d = wait_dump(c, lambda d: not d["playing"], f"(d4) play did not pause {entry}/{demo}")
        time.sleep(1.0)          # the paused frame presented
        if shots:
            shots.mkdir(parents=True, exist_ok=True)
            base = shots / f"{res}-{entry}-{demo}-t{d['tick']}"
            grab(c, base.with_suffix(".game.png"))
            mockup_shot({"res": [int(v) for v in res.split("x")], "entry": entry, "demo_n": k, "tick": d["tick"]},
                        base.with_suffix(".mockup.png"))
        press(c, d, "play")
        print(f"wiki demos: (d4) {entry}/{demo} paused at tick {d['tick']}")

    # (d5) the game thread's cost: the most one fill of the open page took, the demos above playing
    d = c.dump()
    print(f"wiki demos: (d5) the page cost the game thread at most {d['cost_max_ms']:.3f} ms in one frame "
          f"over {d['cost_frames']} frames (the plan's target is under 0.5 ms)")
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(d5) Esc did not close the window")


def hud(c):
    return json.loads(tacli("hud", c.name, "show", "--json", timeout=180))


def rewind_state(c):
    return hud(c)["state"]["rw"]["state"]


def recorded(c):
    held_at(c, HELD_AT)
    d = c.dump()
    h = hud(c)
    ctl = {k["key"]: k["r"] for k in h["hits"]["at"]}
    assert "rewind" in ctl, f"(r) no rewind button in a recorded skirmish: {sorted(ctl)}"
    K = d["viewport"][3] / d["game"][1]
    w, rw = c.hit(d, "wiki"), ctl["rewind"]
    assert abs((rw[0] - w[2]) - round(3 * K)) <= 1 and abs(w[1] - rw[1]) <= 1 \
        and abs((w[2] - w[0]) - (rw[2] - rw[0])) <= 1, \
        f"(r) the wiki button {w} is not 3 bar pixels left of the rewind button {rw}"
    tacli("hud", c.name, "click", "rewind", timeout=180)
    c.until(lambda: rewind_state(c) == "open", "(r) the rewind button did not open the rewind", seconds=120)
    tacli("hud", c.name, "click", "rewind", timeout=180)
    c.until(lambda: rewind_state(c) == "idle", "(r) the rewind button did not close the rewind", seconds=120)
    c.until(lambda: c.runs(1.0), "(r) the game does not run after the rewind closed", seconds=120)
    d = c.open_wiki(expect_pause=True)
    c.once(not c.runs(2.0), "(r) GameTime ran with the window open")
    c.keys("esc")
    d = c.dump()
    assert not d["open"] and not d["paused"], f"(r) Esc did not close the wiki: {d}"
    c.once(c.runs(2.0), "(r) GameTime stands after the close")
    print(f"wiki recorded: (r) the wiki button {w}, the rewind button {rw}; each opens what it should")


def launched(instance, mode, res):
    """The instance on the scenario with the launch's hold, under the run lock; the callable that
    stops it and puts its levers back."""
    runlock.hold(TREE, f"wiki_regression {mode} on {instance}")
    gamedir = tacli_api().Instance(instance).gamedir
    levers = {"tagpu_hold.on": True, "tagpu_recorder.off": mode != "recorded"}
    had = {n: (gamedir / n).exists() for n in levers}
    for n, want in levers.items():
        if want:
            (gamedir / n).write_text("")
        else:
            (gamedir / n).unlink(missing_ok=True)
    tacli("scenario", "load", instance, SCENARIO, "--defaults", "--res", res, timeout=900)

    def stop():
        try:
            tacli("stop", instance, timeout=300)
        finally:
            for n in levers:
                if had[n]:
                    (gamedir / n).write_text("")
                else:
                    (gamedir / n).unlink(missing_ok=True)
    return stop


def mp(name):
    """(m): the compatibility suite's two-player start on the retail setup, then the wiki on the
    host."""
    sys.path.insert(0, str(TREE / "tools/compat"))
    import tacompat
    import dpport
    runlock.hold(TREE, f"wiki_regression mp ({name})")
    setup = next(s for s in tacompat.load_json("setups.json")["setups"] if s["name"] == "retail")
    dll = tacompat.snapshot_dll(TREE / "tagpu/ddraw/ddraw.dll")
    xv = tacompat.start_xvfb()
    try:
        host = tacompat.prepare_wine(setup, dll, xv.tacompat_display, suffix="-wh")
        join = tacompat.prepare_wine(setup, dll, xv.tacompat_display, suffix="-wj")
    finally:
        tacompat.stop_xvfb(xv)
    port = tacompat.MP_PORT_BASE
    games = {}
    try:
        for inst in (host, join):
            dpport.set_port(inst["prefix"], port)
        held = tacompat.free_dplay_port(port, {str(host["prefix"]), str(join["prefix"])})
        assert not held, f"(m) DirectPlay's port {port} is held by {held}"
        games["host"] = tacompat.start_wine(host, None)
        time.sleep(3)
        games["join"] = tacompat.start_wine(join, None)
        end = time.monotonic() + tacompat.POLL_S
        for g in games.values():
            while "MAINMENU" not in (tacompat.tacli("ui", g["inst"]["name"], timeout=15).stdout.splitlines() or [""])[0]:
                assert time.monotonic() < end, f"(m) {g['inst']['name']}: the main menu never came up"
                time.sleep(2)
        tacompat.wine_lobby(host["name"], join["name"], port)
        peers = {}
        for role, inst in (("host", host), ("join", join)):
            peers[role] = Wiki.__new__(Wiki)
            peers[role].attach(inst["name"])
        c = peers["host"]
        roster = c.call("roster", "--json")["units"]
        com = next(u for u in roster if u["owner"] == 0)
        c.units["COM"] = com["engine_index"]
        marks = {r: p.mark() for r, p in peers.items()}

        d = c.dump()
        unit, ground, strip = c.to_margin(d, "COM")
        c.keys("dclick:%d,%d" % unit)
        c.once(c.selected("COM"), f"(m) a device click on the host's commander at {unit} did not select it")
        d = c.open_wiki(expect_pause=False)
        assert not d["single_player"], f"(m) the host's level reads as single player: {d}"
        c.once(not c.paused(), "(m) the host's pause bit is set with the window open")
        t0 = {r: p.gametime() for r, p in peers.items()}
        time.sleep(3)
        for r, p in peers.items():
            now = p.gametime()
            assert now > t0[r], f"(m) the {r}'s GameTime stood at {now} over 3 s with the host's wiki open"
        d2 = c.dump()
        assert d2["sim"] and d2["tick"] > d["tick"], f"(m) the host's demo did not play: tick {d['tick']} -> {d2['tick']}"
        print(f"wiki mp: (m) the host's demo played {d2['tick'] - d['tick']} ticks; the page cost the game thread "
              f"at most {d2['cost_max_ms']:.3f} ms in one frame over {d2['cost_frames']} frames")
        before = c.orders("COM")
        for tok in ("drclick:%d,%d" % ground, "dclick:%d,%d" % ground):
            c.keys(tok)
            c.once(c.orders("COM") == before, f"(m) {tok} on the margin ordered the commander")
            c.once(c.selected("COM"), f"(m) {tok} on the margin changed the selection")
        c.keys("esc")
        d = c.dump()
        assert not d["open"] and not d["holds"], f"(m) Esc did not close the window: {d}"
        t0 = {r: p.gametime() for r, p in peers.items()}
        time.sleep(3)
        for r, p in peers.items():
            assert p.gametime() > t0[r], f"(m) the {r}'s game stands after the close"
            p.mark()
            bad = [ln for ln in p.log[marks[r]:] if re.search(r"(?i)desync|out of sync", ln)]
            assert not bad, f"(m) the {r}'s log has a desync line: {bad[:2]}"
        for r, g in games.items():
            assert g["proc"].poll() is None, f"(m) the {r}'s game exited"
        print("wiki mp: (m) the host's wiki took no pause; both peers ran; the margin ordered nothing; "
              "both run on after the close, no desync line")
    finally:
        for g in games.values():
            tacompat.stop_wine(g)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("instance")
    p.add_argument("mode", choices=("sp", "page", "demos", "recorded", "mp"))
    p.add_argument("--launch", action="store_true",
                   help="sp/recorded: launch on place-basic.json first, under the run lock")
    p.add_argument("--res", default="1920x1080", help="sp/recorded: the game's resolution")
    p.add_argument("--shot", help="sp: grab the presented frame with the window open to this PNG")
    p.add_argument("--shots", type=Path, help="demos: grab each entry's demo mid-play, and the mockup's, into this folder")
    a = p.parse_args()
    t0 = time.monotonic()
    stop = None
    try:
        if a.mode == "mp":
            mp(a.instance)
        else:
            if a.launch:
                stop = launched(a.instance, a.mode, a.res)
            c = Wiki(a.instance, {"ARMCK"})
            try:
                if a.mode == "sp":
                    sp(c, a.shot)
                elif a.mode == "page":
                    page(c)
                elif a.mode == "demos":
                    demos(c, a.shots, a.res)
                else:
                    recorded(c)
            finally:
                c.call("eye", "--release")
    except CHECK_FAILURES as e:
        print(f"wiki regression FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if stop:
            stop()
    print(f"wiki regression {a.mode}: passed in {time.monotonic() - t0:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
