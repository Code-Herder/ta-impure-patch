#!/usr/bin/env python3
"""The in-game wiki in the game (tagpu_wiki.c; research/notes/ingame-wiki-plan.md, landings 2 to 5):
its button, its window, the input it takes while open, the pause it holds in single player, the
page's controls and the demos playing on the stage.

    wiki_regression.py <instance> sp --launch         a single-player skirmish, not recorded
    wiki_regression.py <instance> page --launch       the page's controls, single player
    wiki_regression.py <instance> demos --launch      the demos: parity, clock, controls, captures
    wiki_regression.py <instance> try --launch        Try it yourself, driven through the stage
    wiki_regression.py <instance> recorded --launch   a live recorded skirmish: the rewind button
    wiki_regression.py <instance> asset --launch      the 3D asset load, against the native one
    wiki_regression.py <instance> stage3d --launch --shots DIR
                                                      the stage's ground in 3D, against the mockup
    wiki_regression.py <instance> units --launch --shots DIR
                                                      the stage's units in 3D, against the mockup,
                                                      and a reclaim in the game
    wiki_regression.py <instance> cursor --launch --shots DIR
                                                      the stage's cursor, the game's own sprite,
                                                      against the mockup and the player's cursor
    wiki_regression.py <name> mp                     a two-player network game on Wine, started by
                                                      tools/compat/tacompat.py's launcher
    wiki_regression.py all|<setup>[,<setup>...] mods --shots DIR
                                                      the 3D stage under each setup of the
                                                      compatibility suite, one at a time

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
  (h) a right button pressed on the margin before the open and released while it is open: the
      press reaches the engine, and so does the release (the batch's `engine` count, the shield
      delivering it at the frozen point), with the window still open and the frozen point unmoved;
  (i) a real double-click on the close button (xdotool on the instance's own X server, the four
      messages sent at once) closes the window with nothing owed, and the log says the second
      press, which meets the hit map that drew the window open, was taken;
  (g) reopened on a world the test's own hold keeps paused (the wiki holds the pause it finds and
      will give nothing back: `pause_was`), the level is ended with scenarios/replay-end.json: the
      log says the window closed at the level's end, and an injected key reaches the engine's key
      ring at the results screen.

--shot PATH grabs the presented frame with the window open, at (b) and in page at (p2), for the
owner: the whole window by its id on the instance's display (`import -window`, the ta-capture
skill's window grab).
page, on the same launch as sp:
  (p1) the open picks the tree's first entry and its first demo, playing, and draws every control;
  (p2) entry:3 selects the second entry, demo:1 its second demo, afresh;
  (p3) fold:1 folds the Building category (no entry row drawn) and unfolds it;
  (p4) two wheel notches down over the text scroll it two steps, one up one back, each held to
       the limit the drawn frame posted;
  (p5) the text's thumb dragged 12 px scrolls it by the travel the drawn frame gave it;
  (p7) four real wheel notches sent at once through the instance's X server (xdotool), which the
       window procedure pumps between two frames, scroll the text four steps, not one -- run where
       the text scrolls past two notches; (p4), (p5) and (p7) are not run where it does not scroll;
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
try, on the same launch as sp:
  (t1) Quick Build's `snap` and then Build Lines' `line` selected, and Try it yourself pressed: the
       stage is the player's, and the drawn frame shows its build menu (`build:<type>` controls);
  (t2) the demo's inputs given as device input: each world point turned into the client point the
       dump's stage draws it at (tagpu_wikistage_fit inverted, from the `stage` hit and the frame's
       unit), moves as `dmove:`, the left button as `ddown:`/`dup:` there, Shift as `down:`/`up:`,
       a build as a click on the menu's button. Paused, the dump's log of what the game gave its
       simulation holds the demo's presses, releases and builds in order, every move within a
       pixel of the demo's point, and its state hash equals sim.js fed the same logged inputs at
       the same ticks (`simparity.mjs --try`). With --shot, the presented frame is grabbed before
       `snap`'s click and before `line`'s first release, the line's sites on the stage;
  (t3) Shift and the left button held on the stage when Back to demo is pressed are let go in the
       simulation, and their late releases are taken, nothing owed;
  (t4) Ctrl and the left button held when Esc closes the window: let go, nothing owed after their
       releases, and the engine's key ring unmoved over the whole mode;
  (t5) reopened, the window is in Try it, afresh; Back to demo, and Esc.
recorded:
  (r) the button sits 3 bar pixels left of the rewind button (the two frames' hit maps); the rewind
      button opens the rewind; while it is open a click on the wiki button opens nothing and the log
      says `wiki: not opened - a rewind holds the pause` (the refusal tests the rewind's state, so the
      FLUSH it passes through is refused by the same test, but no step can hold the game in FLUSH);
      the rewind button closes it; the wiki button opens the wiki and Esc closes it.
asset, on the same launch as sp:
  (a1) the open starts the 3D asset load (tagpu_wikiasset.c); the trigger tagpu_wiki3d.dump writes
       its report, and the log's `wiki3d: loaded in` line gives its time and its arena's size;
  (a2) the report equals, field for field, the load tests/wikiassettest.c runs natively over the
       instance's folder with the game's own mount list and language (tools/wiki/assetcheck.py);
  (a3) every stage's ground and every unit type resolved.
stage3d, on the same launch as sp (research/notes/ingame-wiki-3d-stage-plan.md, landing 6g):
  (s1) the open's 3D asset load ends (`wiki3d: loaded in`) and the first stage drawn says its ground
       is in 3D (`wiki3d: stage ... the ground in 3D`);
  (s2) at ss 2 and then at ss 1 (tagpu_ss.off), quickbuild's demos 1 and 11 and buildshapes' demo 1
       held at ticks 30, 180 and 330 through the trigger tagpu_wiki.shot, each frame's capture of the
       wiki's target written to DIR as `<entry>-d<demo>-t<tick>-ss<ss>.game.ppm`, and the presented
       frame at ss 2 grabbed beside it (`<res>-<entry>-d<demo>-t<tick>.window.png`);
  (s3) tools/wiki/stage3dcheck.py over DIR: every terrain pixel equal to the mockup's, at most
       0.05 % of the feature pixels differing, the unit pixels within question 8's tolerance;
  (s4) the lever tagpu_wiki3d.on `nomap` and then `nofeat`: no frame draws the image (the stage is
       the whitebox, R1 whole: `0 of N frame(s) drew the image`), each grabbed; the lever gone, every
       frame draws it again;
  (s5) the stage's render-thread time, as its periodic line gives it.
units, on the same launch as sp (the plan's landing 6h):
  (u1) the open's 3D asset load ends and the first stage's types line names every type in 3D;
  (u2) at ss 2, Quick Build 3 and 8 (a construction, a reclaim, the upgrade's nanoframe), Build
       Lines 2 and 4 (the gesture's ghosts) and Quick Build 9 (an ARMCK walking toward +x) held at
       fixed ticks through tagpu_wiki.shot, each capture written to DIR as in stage3d with the
       presented frame grabbed beside it;
  (u3) tools/wiki/stage3dcheck.py over DIR, the unit pixels within question 8's tolerance;
  (u4) the walking ARMCK's facing in the mockup's simulation at that tick, toward +x, which the
       capture's unit pixels (u3) then show the game's model turned to;
  (u5) the lever `nounit=ARMCK`: the types line puts ARMCK alone among the boxes, every frame still
       draws the image, and the capture of Quick Build 3 differs from (u2)'s where the ARMCK stood;
  (u6) the stage's render-thread time, the producer and the passes, against 0.5 ms;
  (u7) the window closed, a reclaim in the game itself: the ARMCK ordered (tacli order) to reclaim
       the scenario's ARMSOLAR, the solar's build fraction remaining (+0x104) and HP (+0x108) read
       until it is gone, and the presented frame grabbed mid-reclaim -- the engine's own staging
       of a reclaimed unit, which the stage's records follow.
cursor, on the same launch as sp (needs tools/wiki/gfx.py's pack, whose cursors.json and sheet are
the game's frames as the mockup draws them):
  (c1) Quick Build 1 at tick 100 (build placement: cursornormal) and Quick Build 3 at ticks 160 and
       180 (the ARMCK selected: cursormove, its frames 4 and 6) held through tagpu_wiki.shot; the
       presented frame and the mockup's at the same state each searched for every cursor frame,
       texel for texel within 3 a channel: the frame the engine's animation shows at that tick is
       found once, at the same pixel in both; DIR gets each pair cropped side by side, 8x;
  (c2) the pointer moved into the window and the frame grabbed: the player's own cursor and the
       stage's found in the one picture against the same frames, both at the game's scale, and
       cropped side by side (`<res>-cursor-real.pair.png`);
  (c3) Esc closes the window.
mods, per setup of tools/compat/setups.json (`all`: retail, Escalation, Total Mayhem, ProTA, TA Zero,
Twilight), under the run lock (the plan's landing 6i):
  (w1) the setup's folder built by tacompat's prepare_wine with this tree's DLL, started on a private
       Xvfb and walked into a skirmish on the suite's map (tacompat.wine_skirmish);
  (w2) the wiki opened: the 3D asset load's line, and its report tagpu_wiki3d.json copied to
       DIR/<setup>/ -- which archive each ground source, feature, unit file and texture came from;
  (w3) Quick Build demo 3 at tick 180 and Build Lines demo 2 at tick 100 captured through
       tagpu_wiki.shot (DIR/<setup>/<entry>-d<demo>-t<tick>.game.png) with the whole display grabbed
       beside each (.screen.png); each stage's ground line, its types line (in 3D, and the boxes) and
       a whole periodic line of its frames;
  (w4) Esc closes the window;
  (w5) each stage drew what the report resolved: a ground that resolved in 3D on every frame of the
       line, one that did not as the whitebox on every frame.
  At every step the display is polled: any top-level window that is not the game's own, an
  ErrorLog.txt, or the game gone fails the setup. The table of every setup is printed at the end and
  kept in DIR/rows.json.
mp:
  (m) the wiki opened on the host takes no pause: both peers' GameTime advances over 3 s, and so
      does the host's demo, whose cost to the game thread is logged; a right and a left click on
      the margin leave the host's selected commander's orders and selection as they were; Esc
      closes it, and both peers still run, with no desync line in either log.
"""
import argparse
import json
import math
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

    # (h) a button held across the open: the engine gets the release of the press it saw
    c.keys("dmove:%d,%d" % ground)
    got = c.call("keys", "down:rbutton", "--json")
    assert got["engine"] == 1, f"(h) the right press on the margin did not reach the engine: {got}"
    d = c.open_wiki(expect_pause=True)
    frozen = d["frozen"]
    got = c.call("keys", "up:rbutton", "--json")
    assert got["engine"] == 1 and got["menu"] == 0, \
        f"(h) the release of a press the engine saw did not reach it with the window open: {got}"
    d = c.dump()
    assert d["open"] and d["frozen"] == frozen and not d["owed_buttons"], \
        f"(h) the release moved the window's state: {d} (frozen at the open {frozen})"
    c.keys("esc")
    d = c.dump()
    assert not d["open"], f"(h) Esc did not close the window: {d}"
    c.call("order", "stop", "--unit", c.units["ARMCK"], "--expect", "ARMCK")
    print("wiki sp: (h) a right button held across the open was released to the engine, at the frozen point")

    # (i) a double-click on the close button: the second press is the wiki's while the window is
    # still drawn. REAL CLICKS, sent at once through the instance's X server: the window procedure
    # pumps all four messages before the game thread draws again, and the hit map changes only in
    # that draw (tagpu_wiki_take), so the second press meets the hit map that drew the window open.
    # An injected batch posts one event a flip and never reaches that gap. The shield swallows a
    # real press the wiki does not take, so what the wiki did is read from its log.
    d = c.open_wiki(expect_pause=True)
    x = c.centre(c.hit(d, "close"))
    real_pointer(c, *x)
    start = c.mark()
    xdo(c, "click", "--repeat", 2, "--delay", 0, 1)
    d = wait_dump(c, lambda d: not d["open"], "(i) a real double-click on X did not close the window")
    assert not d["owed_buttons"], f"(i) the double-click left a button owed: {d['owed_buttons']}"
    assert c.found(start, r"wiki: closed \(the close button\)"), "(i) no `wiki: closed (the close button)` line"
    assert c.found(start, r"wiki: a press on 'close' of the window still drawn after its close was taken"), \
        "(i) the second press of the double-click was not taken on the window still drawn"
    print("wiki sp: (i) a real double-click on X closed the window, and the second press, on the window "
          "still drawn, was taken")

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
BURST = 4                                # (p7): real wheel notches sent at once


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
    """A click on the drawn frame's control `key`; returns the time just before it."""
    t0 = time.monotonic()
    c.keys("dclick:%d,%d" % c.centre(c.hit(d, key)))
    return t0


def fresh(d, t0):
    """Whether the dump's demo can have started after `t0`: the clock steps at most 30 ticks a
    second times the speed, plus the 8 one frame may carry, so a demo that ran on from before is
    told apart by its tick however slowly the game answers the dump."""
    return d["tick"] <= 30 * d["speed"] * (time.monotonic() - t0) + 9


def keys_of(d, prefix):
    return sorted(h["key"] for h in d["hits"] if h["key"].startswith(prefix))


def text_scroll(c, d):
    """(p4), (p5) and (p7) on the text panel, as far as its scroll range lets each be told apart;
    the dump after them."""
    mx = d["scroll_max"][TEXT]
    if mx <= 0:
        print(f"wiki page: (p4), (p5), (p7) not run: the text does not scroll at {d['viewport'][2]}x{d['viewport'][3]}")
        return d
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

    # (p7) several notches in one frame: real wheel clicks, which the window procedure pumps in one
    # go, where an injected batch posts one event a flip. Told apart from one notch only where the
    # text scrolls past two
    n = min(BURST, int(mx // WHEEL_UNITS))
    if n < 2:
        print(f"wiki page: (p7) not run: the text scrolls {mx:g} units, under two {WHEEL_UNITS}-unit notches, "
              "so one notch and a burst reach the same limit")
        return d
    c.keys(f"dwheel:{x},{y},64")
    d = wait_dump(c, lambda d: d["scroll_drawn"][TEXT] == 0, "(p7) 64 notches up did not bring the text to the top")
    real_pointer(c, x, y)
    xdo(c, "click", "--repeat", n, "--delay", 1, 5)
    want = n * WHEEL_UNITS
    d = wait_dump(c, lambda d: abs(d["scroll_drawn"][TEXT] - want) < 0.02,
                  f"(p7) {n} real notches down did not scroll the text to {want}")
    print(f"wiki page: (p7) {n} real wheel notches in a burst scrolled the text {n} steps, to {want:g}")
    return d


def page(c, shot=None):
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
    t0 = press(c, d, "demo:1")
    d = wait_dump(c, lambda d: d["demo"] == "line" and fresh(d, t0), "(p2) demo:1 did not start buildshapes/line")
    print("wiki page: (p2) the second entry and its second demo selected")
    if shot:
        grab(c, shot)

    press(c, d, "fold:1")
    d = wait_dump(c, lambda d: d["folds"] == [1] and not keys_of(d, "entry:"), "(p3) fold:1 did not fold Building")
    press(c, d, "fold:1")
    d = wait_dump(c, lambda d: d["folds"] == [] and keys_of(d, "entry:") == ["entry:2", "entry:3"],
                  "(p3) fold:1 did not unfold Building")
    print("wiki page: (p3) Building folded and unfolded")

    d = text_scroll(c, d)

    c.keys("esc")
    d = wait_dump(c, lambda d: not d["open"] and not d["sim"], "(p6) Esc did not close the window and free the demo")
    t0 = time.monotonic()
    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and d["sim"], "(p6) the reopened page was not drawn")
    assert (d["entry"], d["demo"], d["folds"]) == ("buildshapes", "line", []), \
        f"(p6) the reopened page lost its selection: {d['entry']}/{d['demo']} folds {d['folds']}"
    assert fresh(d, t0), f"(p6) the demo did not start afresh at the open: tick {d['tick']}"
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(p6) Esc did not close it again")
    print("wiki page: (p6) closed and reopened on the same entry and demo, the demo afresh")


def xdo(c, *args):
    """xdotool on the instance's own display -- a private X server, never the owner's (the instance
    is created with `--display`): the device as a player's hand drives it, through the X server."""
    me = next(i for i in json.loads(tacli("ls", "--json")) if i["name"] == c.name)
    r = subprocess.run(["xdotool", *map(str, args)], capture_output=True, text=True, timeout=30,
                       env={**os.environ, "DISPLAY": me["display"]})
    assert r.returncode == 0, f"xdotool {' '.join(map(str, args))}: {r.stderr.strip()}"
    return me


def real_pointer(c, x, y):
    """The X pointer brought over client point (x, y) of the game's window, as the window open
    notes it (the dump's `ptr`, the client point of the last move the wiki took): moved to the
    point in the X window's own pixels, then by what that point read as, which is the client
    area's offset in the X window."""
    me = next(i for i in json.loads(tacli("ls", "--json")) if i["name"] == c.name)
    wid = me["window"][0]
    want = [x, y]
    at = [x, y]
    d = None
    for _ in range(3):
        xdo(c, "mousemove", "--window", wid, at[0], at[1])
        time.sleep(0.3)
        d = c.dump()
        if d["ptr"] == want:
            return
        if d["ptr"] != [-1, -1]:
            at = [at[0] + want[0] - d["ptr"][0], at[1] + want[1] - d["ptr"][1]]
    raise AssertionError(f"the real pointer reads {d['ptr'] if d else None}, not {want}")


def simparity_hashes():
    r = subprocess.run(["node", str(TREE / "tools/wiki/simparity.mjs"), "--hash"], capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 0, f"simparity --hash: {r.stderr.strip()}"
    return dict(ln.split() for ln in r.stdout.splitlines() if ln.strip())


def tick_rate(c, seconds=4.0):
    a, t0 = c.dump(), time.monotonic()
    time.sleep(seconds)
    b = c.dump()
    assert not b["done"], f"(d2) the demo ended inside the rate's window, at tick {b['tick']}"
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
    t0 = press(c, d, "demo:3")
    wait_dump(c, lambda d: d["demo"] == "bestfit" and fresh(d, t0), "(d2) demo:3 did not start quickbuild/bestfit")
    r1, d = tick_rate(c)
    press(c, d, "x2")
    d = wait_dump(c, lambda d: d["speed"] == 2, "(d2) x2 did not set 2x")
    # from the start again, so the window at 2x ends inside the demo's 722 ticks however long the
    # dumps take to come back
    t0 = press(c, d, "replay")
    wait_dump(c, lambda d: d["playing"] and fresh(d, t0), "(d2) replay did not restart the demo at 2x")
    r2, d = tick_rate(c)
    assert 15 <= r1 <= 45 and 1.5 <= r2 / r1 <= 2.5, f"(d2) the demo ran {r1:.1f} ticks/s at 1x and {r2:.1f} at 2x"
    print(f"wiki demos: (d2) quickbuild/bestfit at {r1:.1f} ticks/s at 1x, {r2:.1f} at 2x")

    # from the start again, so the pause lands inside the demo however long the dumps take
    t0 = press(c, d, "replay")
    d = wait_dump(c, lambda d: d["playing"] and fresh(d, t0), "(d3) replay did not restart the demo")
    press(c, d, "play")
    d = wait_dump(c, lambda d: not d["playing"], "(d3) play did not pause the demo")
    assert not d["done"], f"(d3) the demo ended before play paused it, at tick {d['tick']}"
    t = d["tick"]
    time.sleep(1.5)
    d = c.dump()
    assert d["tick"] == t, f"(d3) the paused demo ran from tick {t} to {d['tick']}"
    t0 = press(c, d, "replay")
    d = wait_dump(c, lambda d: d["playing"] and fresh(d, t0), "(d3) replay did not restart the demo, playing")
    press(c, d, "demo:5")
    d = wait_dump(c, lambda d: d["demo"] == "cancel", "(d3) demo:5 did not select quickbuild/cancel")
    d = wait_dump(c, lambda d: d["done"], "(d3) quickbuild/cancel did not end at 2x", seconds=40)
    t = d["tick"]
    time.sleep(1.0)
    d = c.dump()
    assert d["done"] and d["tick"] == t, f"(d3) the ended demo did not hold its last frame: {t} -> {d['tick']}"
    t0 = press(c, d, "play")
    d = wait_dump(c, lambda d: not d["done"] and fresh(d, t0), "(d3) play on an ended demo did not start it again")
    press(c, d, "x1")
    wait_dump(c, lambda d: d["speed"] == 1, "(d3) x1 did not set 1x")
    print("wiki demos: (d3) pause, replay, the last frame held and played again")

    # (d4) each entry with a demo paused mid-play, grabbed beside the mockup at the same state; past
    # tick 150, which leaves some nine seconds of either demo for the pause to land in
    for node, entry, k, demo, at in ((2, "quickbuild", 0, "snap", 150), (3, "buildshapes", 1, "line", 150)):
        d = c.dump()
        if d["entry"] != entry:
            press(c, d, f"entry:{node}")
        d = wait_dump(c, lambda d: d["entry"] == entry and f"demo:{k}" in keys_of(d, "demo:"),
                      f"(d4) entry:{node} did not select {entry} and draw its demos")
        press(c, d, f"demo:{k}")
        d = wait_dump(c, lambda d: d["demo"] == demo and d["tick"] >= at, f"(d4) {entry}/{demo} did not reach tick {at}", seconds=60)
        press(c, d, "play")
        d = wait_dump(c, lambda d: not d["playing"], f"(d4) play did not pause {entry}/{demo}")
        assert not d["done"], f"(d4) {entry}/{demo} ended before play paused it"
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


TRY_DEMOS = (("quickbuild", 2, "snap", 83), ("buildshapes", 3, "line", 60))   # (entry, its tree node,
                                         # the demo, the input before which the stage is grabbed)
TRY_KEYS = {"shift": "shift", "alt": "alt", "ctrl": "ctrl", "z": "z", "x": "x"}


def demo_inputs(entry, demo_id):
    return next(x for x in json.loads((TREE / f"art/wiki/entries/{entry}.json").read_text())["demos"]
                if x["id"] == demo_id)["inputs"]


def stage_fit(d):
    """tagpu_wikistage_fit on the stage the dump's hit map drew: the map's scale and corner in client
    pixels, from the stage's canvas -- its `stage` hit -- and the frame's unit, as both of the
    game's threads fit it."""
    x0, y0, x1, y1 = Wiki.hit(d, "stage")
    u, (mw, mh), cell = d["u"], d["map"], 16
    pad = 6 * u
    s = min((x1 - x0 - 2 * pad) / (mw * cell), (y1 - y0 - 2 * pad) / (mh * cell))
    return s, x0 + (x1 - x0 - mw * cell * s) / 2, y0 + (y1 - y0 - mh * cell * s) / 2


def replay_try(c, d, entry, demo_id, grab_at=None, shot=None):
    """One demo's inputs (art/wiki/entries/<entry>.json) given to Try it yourself as device input:
    each world point turned into the client point the dump's stage shows it at (stage_fit
    inverted), a move as `dmove:`, the left button as `ddown:`/`dup:` there, the right as
    `down:rbutton`/`up:rbutton` where the pointer is, a key as `down:`/`up:`, a build as a click on
    the build menu's button the drawn frame shows; captions are the demo's own and are skipped. With
    `grab_at`, the presented frame is grabbed to `shot` before that input is given. The world inputs
    the demo would give, in order, without its moves."""
    s, ox, oz = stage_fit(d)
    at = None
    pending, want = [], []

    def flush():
        if pending:
            c.keys(*pending)
        pending.clear()

    for i, ev in sorted(enumerate(demo_inputs(entry, demo_id)), key=lambda p: (p[1]["at"], p[0])):
        if shot and grab_at is not None and i == grab_at:
            flush()
            time.sleep(1.0)
            grab(c, shot)
        if "move" in ev:
            at = (round(ox + ev["move"][0] * s), round(oz + ev["move"][1] * s))
            pending.append("dmove:%d,%d" % at)
        for kind in ("down", "up"):
            k = ev.get(kind)
            if k is None:
                continue
            want.append([kind, k])
            if k in ("lmb", "rmb"):
                assert at, f"{entry}/{demo_id}: a button before any move"
                pending.append(("ddown:%d,%d" if kind == "down" else "dup:%d,%d") % at if k == "lmb"
                               else f"{kind}:rbutton")
            else:
                pending.append(f"{kind}:{TRY_KEYS[k]}")
        if "build" in ev:
            want.append(["build", ev["build"]])
            key = "build:" + (ev["build"] or "none")
            flush()
            d = wait_dump(c, lambda d: key in keys_of(d, "build:"), f"the build menu shows no {key}")
            x, y = c.centre(c.hit(d, key))
            c.keys(f"ddown:{x},{y}", f"dup:{x},{y}")
    flush()
    return want, s


def try_it(c, shot=None):
    """(t1)..(t5): Try it yourself driven by device input on the stage, its simulation against
    sim.js fed what the game logged."""
    held_at(c, HELD_AT)
    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and d["sim"], "(t1) the page was not drawn")
    ring = c.ring()
    for entry, node, demo, grab_at in TRY_DEMOS:
        if d["entry"] != entry:
            press(c, d, f"entry:{node}")
            d = wait_dump(c, lambda d: d["entry"] == entry and "demo:1" in keys_of(d, "demo:"), f"(t1) entry:{node} did not select {entry}")
        if d["demo"] != demo:
            k = [x["id"] for x in json.loads((TREE / f"art/wiki/entries/{entry}.json").read_text())["demos"]].index(demo)
            press(c, d, f"demo:{k}")
            d = wait_dump(c, lambda d: d["demo"] == demo, f"(t1) demo:{k} did not select {entry}/{demo}")
        press(c, d, "try")
        d = wait_dump(c, lambda d: d["try"] and keys_of(d, "build:"), "(t1) Try it yourself did not take the stage")
        print(f"wiki try: (t1) {entry}/{demo}: Try it took the stage, its build menu {keys_of(d, 'build:')}")

        shot_n = shot and Path(shot).with_name(f"{Path(shot).stem}-{entry}-{demo}{Path(shot).suffix}")
        if shot:
            # the control for the hidden cursor: the pointer over the build menu, off the stage
            c.keys("dmove:%d,%d" % c.centre(c.hit(d, "build:none")))
            time.sleep(1.0)
            grab(c, Path(shot).with_name(f"{Path(shot).stem}-{entry}-{demo}-menu{Path(shot).suffix}"))
        want, s = replay_try(c, d, entry, demo, grab_at, shot_n)
        press(c, wait_dump(c, lambda d: d["playing"], "(t2) Try it is not playing"), "play")
        d = wait_dump(c, lambda d: not d["playing"], "(t2) play did not pause Try it")
        got = [[k, a] for _, k, a, *_ in d["try_log"] if k != "move"]
        assert got == want, f"(t2) {entry}/{demo}: the game gave its simulation {got}, the demo {want}"
        assert not d["try_cut"], f"(t2) the dump's log of Try it was cut ({d['try_cut']} inputs)"
        moves = [(x, z) for _, k, x, z in (e for e in d["try_log"] if e[1] == "move")]
        demo_pts = [tuple(ev["move"]) for ev in demo_inputs(entry, demo) if "move" in ev]
        far = [m for m in moves if min(math.hypot(m[0] - p[0], m[1] - p[1]) for p in demo_pts) > 0.75 / s]
        assert not far, f"(t2) {len(far)} of {len(moves)} moves landed more than a pixel off the demo's points: {far[:4]}"
        dumpf = c.gamedir / "tagpu_wiki.try.json"
        dumpf.write_text(json.dumps(d))
        r = subprocess.run(["node", str(TREE / "tools/wiki/simparity.mjs"), "--try", str(dumpf)], capture_output=True,
                           text=True, timeout=300)
        dumpf.unlink(missing_ok=True)
        assert r.returncode == 0, f"simparity --try: {r.stderr.strip()}"
        assert r.stdout.strip() == d["hash"], \
            f"(t2) {entry}/{demo}: the game's Try it hashes {d['hash']} at tick {d['tick']}, sim.js fed its inputs {r.stdout.strip()}"
        print(f"wiki try: (t2) {entry}/{demo} replayed through the stage: {len(d['try_log'])} inputs "
              f"({len(moves)} moves, every one within a pixel of the demo's point), tick {d['tick']}, "
              f"hash {d['hash']} equal to sim.js's")
        press(c, d, "play")
        d = wait_dump(c, lambda d: d["playing"], "(t2) play did not start Try it again")

    # (t3) a button and a key held when Try it is left: both let go in the simulation, and their late
    # releases are taken -- nothing reaches the engine
    s, ox, oz = stage_fit(d)
    x0, y0, x1, y1 = c.hit(d, "stage")
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    c.keys("down:shift", f"ddown:{cx},{cy}")
    d = wait_dump(c, lambda d: d["try_down"] == ["shift", "lmb"], "(t3) the held Shift and left button are not in Try it's set")
    press(c, d, "try")
    d = wait_dump(c, lambda d: not d["try"] and not keys_of(d, "build:"), "(t3) Back to demo did not return the demo")
    assert d["try_down"] == [], f"(t3) leaving Try it left {d['try_down']} held"
    c.keys("up:shift", f"dup:{cx},{cy}")
    d = c.dump()
    assert not d["owed_keys"] and not d["owed_buttons"], f"(t3) owed after the late releases: {d['owed_keys']} {d['owed_buttons']}"
    print("wiki try: (t3) Back to demo let Shift and the left button go; their late releases were taken")

    # (t4) held when the window closes (Esc): released, and nothing owed once let go
    press(c, d, "try")
    d = wait_dump(c, lambda d: d["try"] and keys_of(d, "build:"), "(t4) Try it did not take the stage again")
    c.keys("down:ctrl", f"ddown:{cx},{cy}")
    d = wait_dump(c, lambda d: d["try_down"] == ["ctrl", "lmb"], "(t4) the held Ctrl and left button are not in Try it's set")
    c.keys("esc")
    d = wait_dump(c, lambda d: not d["open"], "(t4) Esc did not close the window")
    assert d["try_down"] == [] and not d["sim"], f"(t4) the close left {d['try_down']} held, sim {d['sim']}"
    c.keys("up:ctrl", f"dup:{cx},{cy}")
    d = c.dump()
    assert not d["owed_keys"] and not d["owed_buttons"], f"(t4) owed after the late releases: {d['owed_keys']} {d['owed_buttons']}"
    assert c.ring() == ring, f"(t4) a key reached the engine's ring: {ring} -> {c.ring()}"
    print("wiki try: (t4) the close let Ctrl and the left button go; nothing reached the engine's key ring")

    # (t5) reopened in Try it, afresh
    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and d["sim"], "(t5) the reopened page was not drawn")
    assert d["try"] and d["try_log"] == [] and d["try_down"] == [], f"(t5) reopened: try {d['try']}, log {len(d['try_log'])}"
    press(c, d, "try")
    d = wait_dump(c, lambda d: not d["try"], "(t5) Back to demo")
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(t5) Esc did not close the window")
    print("wiki try: (t5) the window reopens in Try it, afresh; Back to demo and Esc close it")


STAGE3D_SHOTS = (("quickbuild", 1), ("quickbuild", 11), ("buildshapes", 1))
STAGE3D_TICKS = (30, 180, 330)
DRAWN = r"wiki3d: frame \d+: .*; (\d+) of (\d+) frame\(s\) drew the image; .*render thread: the producer ([\d.]+) ms " \
        r"and the passes ([\d.]+) ms a frame on average, the passes at most ([\d.]+) ms"
# (entry, demo, ticks): the mockup's sim (tools/wiki/mockup/sim.js) puts a nanoframe in Quick Build 3 at
# 267..287, the reclaim in Quick Build 8 at 171..180 and the upgrade's nanoframe after it, the gesture's
# ghosts in Build Lines 2 and 4 from tick 0, and Quick Build 9's ARMCK walking toward +x at 115..207
UNITS_SHOTS = (("quickbuild", 3, (180, 277)), ("quickbuild", 8, (120, 175, 300)),
               ("buildshapes", 2, (100, 350)), ("buildshapes", 4, (100, 350)), ("quickbuild", 9, (150,)))
FACING = ("quickbuild", 9, 150)
NOUNIT = ("quickbuild", 3, 180)
UNIT_MS = 0.5


def wait_line(c, start, rx, what, seconds=30):
    end = time.monotonic() + seconds
    while True:
        c.mark()
        hit = [ln for ln in c.log[start:] if re.search(rx, ln)]
        if hit:
            return hit[-1]
        assert time.monotonic() < end, f"{what}: no line matching `{rx}` in {seconds} s"
        time.sleep(0.3)


def drawn_window(c, what, want_all):
    """The next whole periodic line of the stage's frames: (drew, of, producer ms, passes ms, max ms)."""
    # 300 frames a line: some 12 s at 1920 x 1080 on a private Xvfb, and over 30 s at 3840 x 2160
    start = c.mark()
    wait_line(c, start, DRAWN, what, 180)     # a window that may have begun before the change
    start = c.mark()
    ln = wait_line(c, start, DRAWN, what, 180)
    m = re.search(DRAWN, ln)
    drew, of = int(m.group(1)), int(m.group(2))
    assert (drew == of) if want_all else (drew == 0), f"{what}: {drew} of {of} frame(s) drew the image"
    return drew, of, float(m.group(3)), float(m.group(4)), float(m.group(5))


def shoot(c, shots, res, entry, demo, tick, ss, name=None, window=True):
    """One capture of the wiki's target through tagpu_wiki.shot, copied into `shots` (as
    `<entry>-d<demo>-t<tick>-ss<ss>.game.ppm` unless `name`), with the presented frame grabbed beside
    it; the capture's `armed` line. `ss` None takes the capture at the Supersampling setting's ss."""
    ppm = c.gamedir / "tagpu_wiki3d_shot.ppm"
    start = c.mark()
    ppm.unlink(missing_ok=True)
    (c.gamedir / "tagpu_wiki.shot").write_text(f"{entry} {demo} {tick}\n")
    asked = wait_line(c, start, r"wiki: shot \d+ (asked|: )|wiki: shot: ", f"{entry} {demo} {tick}")
    assert " asked: " in asked, asked
    n = re.search(r"wiki: shot (\d+) asked", asked).group(1)
    armed = wait_line(c, start, rf"wiki3d: shot {n}: ", f"shot {n} was never armed")
    m = re.search(r"the (\d+)x(\d+) target at ss (\d+)", armed)
    assert m and (ss is None or int(m.group(3)) == ss), armed
    ss = int(m.group(3))
    wait_line(c, start, r"shot: wrote tagpu_wiki3d_shot\.ppm", f"shot {n} was never written")
    out = shots / (name or f"{entry}-d{demo}-t{tick}-ss{ss}.game.ppm")
    shutil.copyfile(ppm, out)
    if window:
        time.sleep(0.5)
        grab(c, shots / f"{res}-{out.name.split('.')[0]}.window.png")
    return out, armed


def stage3d(c, shots, res):
    """(s1)..(s5): the stage's ground in 3D, against the mockup."""
    assert shots, "stage3d needs --shots DIR"
    shots.mkdir(parents=True, exist_ok=True)
    held_at(c, HELD_AT)
    gamedir = c.gamedir
    ss_off, lever = gamedir / "tagpu_ss.off", gamedir / "tagpu_wiki3d.on"
    had_ss = ss_off.exists()
    lever.unlink(missing_ok=True)
    start = c.mark()
    c.open_wiki(expect_pause=True)
    line = wait_line(c, start, r"wiki3d: loaded in", "(s1) the 3D asset load", 120)
    print(f"wiki stage3d: (s1) {line.split('wiki3d: ', 1)[1]}")
    line = wait_line(c, start, r"wiki3d: stage \S+: the ground", "(s1) the first stage's ground", 30)
    assert "the ground in 3D" in line, f"(s1) {line}"
    print(f"wiki stage3d: (s1) {line.split('wiki3d: ', 1)[1]}")
    try:
        for ss in (2, 1):
            if ss == 1:
                ss_off.write_text("")
            else:
                ss_off.unlink(missing_ok=True)
            time.sleep(1.5)
            for entry, demo in STAGE3D_SHOTS:
                for tick in STAGE3D_TICKS:
                    out, armed = shoot(c, shots, res, entry, demo, tick, ss, window=ss == 2)
                    print(f"wiki stage3d: (s2) {out.name}: {armed.split('wiki3d: ', 1)[1]}")
    finally:
        if had_ss:
            ss_off.write_text("")
        else:
            ss_off.unlink(missing_ok=True)

    r = subprocess.run([sys.executable, str(TREE / "tools/wiki/stage3dcheck.py"), str(shots), "--out", str(shots / "check")],
                       capture_output=True, text=True, timeout=1800)
    print(r.stdout.rstrip())
    assert r.returncode == 0, f"(s3) stage3dcheck exited {r.returncode}: {r.stderr.strip()[-600:]}"

    # the levers on quickbuild, whose stage has features (buildshapes' has none, so `nofeat` would
    # withhold nothing the image expects there)
    start = c.mark()
    (gamedir / "tagpu_wiki.shot").write_text("quickbuild 1 30\n")
    wait_line(c, start, r"wiki3d: shot \d+: quickbuild", "(s4) quickbuild was not drawn before the levers")
    try:
        for tok in ("nomap", "nofeat"):
            start = c.mark()
            lever.write_text(tok)
            wait_line(c, start, rf"wiki3d: tagpu_wiki3d\.on: {tok}", f"(s4) the lever {tok} was not read")
            got = drawn_window(c, f"(s4) under {tok}", want_all=False)
            grab(c, shots / f"{res}-lever-{tok}.window.png")
            print(f"wiki stage3d: (s4) {tok}: {got[0]} of {got[1]} frames drew the image - the whitebox whole")
    finally:
        lever.unlink(missing_ok=True)
    got = drawn_window(c, "(s4) the lever gone", want_all=True)
    print(f"wiki stage3d: (s4) the lever gone: {got[0]} of {got[1]} frames drew the image")
    print(f"wiki stage3d: (s5) render thread: the producer {got[2]:.3f} ms and the passes {got[3]:.3f} ms a frame "
          f"on average, the passes at most {got[4]:.3f} ms ({res})")
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(s5) Esc did not close the window")


# (entry, demo, tick, frame): the mockup's sim puts quickbuild's demo 1 in build placement from tick 20 (the
# game's cursornormal, one frame), and demo 3's ARMCK selected from tick 0 (cursormove, 8 frames of 8 ticks
# looping: frame 4, the largest, at tick 160, frame 6 -- frame 2 again -- at 180). No demo has nothing selected, the arrow, which
# shows cursornormal too.
CURSOR_SHOTS = (("quickbuild", 1, 100, "build frame 0"), ("quickbuild", 3, 160, "move frame 4"),
                ("quickbuild", 3, 180, "move frame 2/6"))


def sprite_frames():
    """The game's cursor frames as tools/wiki/gfx.py decoded them: [(name, frame, rgba array h x w x 4)]."""
    import numpy as np
    gfx = TREE / "tools/wiki/mockup/gfx"
    j = json.loads((gfx / "cursors.json").read_text())
    sheet = np.frombuffer((gfx / "cursors.rgba").read_bytes(), np.uint8).reshape(j["sheet"][1], j["sheet"][0], 4)
    import numpy as np
    out = []
    for shape in ("build", "move"):
        s = j["seq"][shape]
        mine = []
        for k in range(s["n"]):
            f = j["frames"][s["first"] + k]
            px = sheet[:f["h"], f["sx"]:f["sx"] + f["w"]]
            # cursormove's frames 5..7 repeat 3..1 texel for texel: one name for each picture
            same = next((m for m in mine if m[2].shape == px.shape and np.array_equal(m[2], px)), None)
            if same:
                same[0].append(k)
            else:
                mine.append(([k], f, px))
        out += [(f"{shape} frame {'/'.join(map(str, ks))}", f, px) for ks, f, px in mine]
    return out


def find_sprite(img, rgba, tol=3):
    """Every top-left where the frame's opaque texels all appear in `img` (RGB) within `tol` a channel."""
    import numpy as np
    ys, xs = np.nonzero(rgba[..., 3])
    col = rgba[ys, xs, :3].astype(int)
    a = img.astype(int)
    H, W = a.shape[:2]
    h, w = rgba.shape[:2]
    first = np.all(np.abs(a - col[0]) <= tol, axis=2)
    hits = []
    for y, x in zip(*np.nonzero(first)):
        y0, x0 = y - ys[0], x - xs[0]
        if y0 < 0 or x0 < 0 or y0 + h > H or x0 + w > W:
            continue
        if np.all(np.abs(a[y0 + ys, x0 + xs] - col) <= tol):
            hits.append((int(x0), int(y0)))
    return hits


def cursor(c, shots, res):
    """(c1)..(c3): the stage's cursor is the game's own sprite, against the mockup and the real cursor."""
    import numpy as np
    from PIL import Image
    assert shots, "cursor needs --shots DIR"
    shots.mkdir(parents=True, exist_ok=True)
    frames = sprite_frames()
    held_at(c, HELD_AT)
    start = c.mark()
    d = c.open_wiki(expect_pause=True)
    wait_line(c, start, r"wiki3d: loaded in", "(c1) the 3D asset load", 120)
    for entry, demo, tick, shape in CURSOR_SHOTS:
        start = c.mark()
        (c.gamedir / "tagpu_wiki.shot").write_text(f"{entry} {demo} {tick}\n")
        wait_line(c, start, r"wiki: shot \d+ asked", f"(c1) {entry} {demo} {tick}")
        time.sleep(1.0)
        base = shots / f"{res}-cursor-{entry}-d{demo}-t{tick}"
        grab(c, base.with_suffix(".game.png"))
        mockup_shot({"res": [int(v) for v in res.split("x")], "entry": entry, "demo_n": demo - 1, "tick": tick},
                    base.with_suffix(".mockup.png"))
        # (c1) the frame the engine's animation shows at that tick, found pixel for pixel in both, at one place
        got = {}
        for side in ("game", "mockup"):
            img = np.asarray(Image.open(base.with_suffix(f".{side}.png")).convert("RGB"))
            got[side] = [(name, at) for name, f, rgba in frames for at in find_sprite(img, rgba)]
        both = [g for g in got["game"] if g in got["mockup"]]
        assert len(both) == 1 and both[0][0] == shape, \
            f"(c1) {entry} {demo} {tick}: not the {shape} once - game {got['game']}, mockup {got['mockup']}"
        print(f"wiki cursor: (c1) {entry} {demo} t{tick}: {shape} at {both[0][1]} in the game and the mockup, "
              f"pixel for pixel")
        name, at = both[0]
        f = next(f for n, f, _r in frames if n == name)
        crop = (at[0] - 8, at[1] - 8, at[0] + f["w"] + 8, at[1] + f["h"] + 8)
        pair = Image.new("RGB", (2 * (crop[2] - crop[0]) + 4, crop[3] - crop[1]), (255, 0, 255))
        pair.paste(Image.open(base.with_suffix(".game.png")).convert("RGB").crop(crop), (0, 0))
        pair.paste(Image.open(base.with_suffix(".mockup.png")).convert("RGB").crop(crop), (crop[2] - crop[0] + 4, 0))
        pair.resize((pair.width * 8, pair.height * 8), Image.NEAREST).save(base.with_suffix(".pair.png"))

    # (c2) the player's own cursor in the picture beside the stage's, both at the game's scale: the
    # pointer moved into the window, the frame grabbed once, each found against the same sprites
    vx, vy, vw, vh = d["viewport"]
    c.keys(f"dmove:{vx + vw // 2},{vy + vh // 2}")
    time.sleep(1.0)
    base = shots / f"{res}-cursor-real"
    grab(c, base.with_suffix(".game.png"))
    img = np.asarray(Image.open(base.with_suffix(".game.png")).convert("RGB"))
    hits = [(name, at, f) for name, f, rgba in frames for at in find_sprite(img, rgba)]
    print(f"wiki cursor: (c2) the frame holds {len(hits)} of the game's cursor frames: "
          + ", ".join(f"{n} at {a}" for n, a, _f in hits))
    assert len(hits) >= 2, f"(c2) the player's cursor and the stage's were not both found: {hits}"
    tiles = []
    for name, at, f in hits[:4]:
        tiles.append(Image.fromarray(img).crop((at[0] - 4, at[1] - 4, at[0] + f["w"] + 4, at[1] + f["h"] + 4)))
    side = Image.new("RGB", (sum(t.width + 4 for t in tiles), max(t.height for t in tiles)), (255, 0, 255))
    x = 0
    for t in tiles:
        side.paste(t, (x, 0))
        x += t.width + 4
    side.resize((side.width * 8, side.height * 8), Image.NEAREST).save(base.with_suffix(".pair.png"))
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(c3) Esc did not close the window")


def sim_facing(entry, demo, tick, kind):
    """The facing and the step of the first `kind` moving at `tick` in the mockup's simulation."""
    js = ("const S = require(process.argv[1]); const fs = require('fs');"
          "const e = JSON.parse(fs.readFileSync(process.argv[2], 'utf8')); const sim = S.play(e, e.demos[+process.argv[3] - 1]);"
          "let prev = null; while (sim.tick < +process.argv[4] && !sim.done) { prev = sim.view(); sim.step(); }"
          "const v = sim.view(); const out = v.units.filter(u => u.type === process.argv[5]).map(u => {"
          " const p = prev && prev.units.find(q => q.id === u.id); return { hx: u.hx, hz: u.hz, dx: p ? u.x - p.x : 0, dz: p ? u.z - p.z : 0 }; });"
          "console.log(JSON.stringify(out));")
    r = subprocess.run(["node", "-e", js, str(TREE / "tools/wiki/mockup/sim.js"), str(TREE / f"art/wiki/entries/{entry}.json"),
                        str(demo), str(tick), kind], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    got = [u for u in json.loads(r.stdout) if u["dx"] or u["dz"]]
    assert got, f"no {kind} moves at {entry} demo {demo} tick {tick}"
    return got[0]


def units(c, shots, res):
    """(u1)..(u7): the stage's units in 3D, against the mockup, and a reclaim in the game."""
    assert shots, "units needs --shots DIR"
    shots.mkdir(parents=True, exist_ok=True)
    held_at(c, HELD_AT)
    gamedir = c.gamedir
    ss_off, lever = gamedir / "tagpu_ss.off", gamedir / "tagpu_wiki3d.on"
    assert not ss_off.exists(), "units runs at the Supersampling setting's ss 2: remove tagpu_ss.off"
    lever.unlink(missing_ok=True)
    start = c.mark()
    c.open_wiki(expect_pause=True)
    line = wait_line(c, start, r"wiki3d: loaded in", "(u1) the 3D asset load", 120)
    print(f"wiki units: (u1) {line.split('wiki3d: ', 1)[1]}")
    line = wait_line(c, start, r"wiki3d: \S+: the types in 3D:", "(u1) the first stage's types", 30)
    assert line.endswith("the boxes: none"), f"(u1) {line}"
    print(f"wiki units: (u1) {line.split('wiki3d: ', 1)[1]}")

    for entry, demo, ticks in UNITS_SHOTS:
        for tick in ticks:
            out, armed = shoot(c, shots, res, entry, demo, tick, 2)
            print(f"wiki units: (u2) {out.name}: {armed.split('wiki3d: ', 1)[1]}")
    r = subprocess.run([sys.executable, str(TREE / "tools/wiki/stage3dcheck.py"), str(shots), "--out", str(shots / "check")],
                       capture_output=True, text=True, timeout=3600)
    print(r.stdout.rstrip())
    # (u3)'s verdict is given after (u4)..(u7), so one run reports every step
    u3 = None if r.returncode == 0 else f"(u3) stage3dcheck exited {r.returncode}: {r.stderr.strip()[-600:]}"

    f = sim_facing(*FACING, "ARMCK")
    assert f["hx"] > 0.9 and f["dx"] > 0, f"(u4) the ARMCK at {FACING} is not walking toward +x: {f}"
    print(f"wiki units: (u4) {FACING[0]} demo {FACING[1]} tick {FACING[2]}: the ARMCK walks ({f['dx']:.3f}, {f['dz']:.3f}) "
          f"a tick facing ({f['hx']:.3f}, {f['hz']:.3f}); its capture is {FACING[0]}-d{FACING[1]}-t{FACING[2]}, "
          f"against the mockup's in (u3)")

    try:
        start = c.mark()
        lever.write_text("nounit=ARMCK")
        wait_line(c, start, r"wiki3d: tagpu_wiki3d\.on: nounit=ARMCK", "(u5) the lever was not read")
        entry, demo, tick = NOUNIT
        out, _ = shoot(c, shots, res, entry, demo, tick, 2, name=f"lever-nounit-{entry}-d{demo}-t{tick}.ppm")
        line = wait_line(c, start, rf"wiki3d: {entry}: the types in 3D:", "(u5) no types line under the lever")
        assert line.endswith("the boxes: ARMCK"), f"(u5) {line}"
        got = drawn_window(c, "(u5) under nounit=ARMCK", want_all=True)
        from PIL import Image
        import numpy as np
        a = np.asarray(Image.open(out)); b = np.asarray(Image.open(shots / f"{entry}-d{demo}-t{tick}-ss2.game.ppm"))
        ndiff = int((a != b).any(axis=2).sum())
        assert ndiff > 0, "(u5) the capture under nounit=ARMCK equals the one without it"
        print(f"wiki units: (u5) nounit=ARMCK: {line.split('wiki3d: ', 1)[1]}; {got[0]} of {got[1]} frames drew the "
              f"image; {ndiff} px of the capture differ from (u2)'s")
    finally:
        lever.unlink(missing_ok=True)
    got = drawn_window(c, "(u6) the lever gone", want_all=True)
    total = got[2] + got[3]
    print(f"wiki units: (u6) render thread: the producer {got[2]:.3f} ms and the passes {got[3]:.3f} ms a frame on "
          f"average ({total:.3f} ms, against {UNIT_MS} ms), the passes at most {got[4]:.3f} ms ({res})")
    assert total <= UNIT_MS, f"(u6) the stage costs the render thread {total:.3f} ms a frame"
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(u6) Esc did not close the window")
    reclaim(c, shots, res)
    assert u3 is None, u3


def unit_state(c, kind):
    """A unit's build fraction remaining (+0x104, float) and HP (+0x108, word), read at one instant."""
    b = c.bytes(f"0x{c.address(kind) + 0x104:x}:x6")
    frac, hp = struct.unpack("<fH", b)
    return frac, hp


def reclaim(c, shots, res):
    """(u7): the engine's own reclaim, read as it runs."""
    ck, solar = c.units["ARMCK"], c.units["ARMSOLAR"]
    frac, hp = unit_state(c, "ARMSOLAR")
    assert frac == 0.0 and hp > 0, f"(u7) the solar is not a finished unit before the reclaim: {frac}, {hp}"
    sx, sz = 1600, 5800
    c.settle_eye(sx - 500, sz - 400)
    tacli("order", c.name, "reclaim", "unit", solar, "--unit", ck, "--expect", "ARMCK", "--expect-target", "ARMSOLAR",
          timeout=60)
    rows, grabbed, end = [], False, time.monotonic() + 240
    hp0 = hp
    while time.monotonic() < end:
        try:
            frac, hp = unit_state(c, "ARMSOLAR")
            alive = c.values(f"0x{c.address('ARMSOLAR') + 0x110:x}")[0] != 0
        except AssertionError:
            break
        rows.append((c.gametime(), frac, hp))
        if not grabbed and 0 < hp < hp0 * 0.6:
            grab(c, shots / f"{res}-reclaim-live.window.png")
            grabbed = True
        if hp == 0 or not alive or (len(rows) > 3 and rows[-1][2] == rows[-4][2] == 0):
            break
        time.sleep(0.5)
    fell = [r for r in rows if r[2] < hp0]
    assert fell, f"(u7) the solar's HP never fell under the reclaim: {rows[:5]}"
    nano = [r for r in rows if r[1] != 0.0]
    print(f"wiki units: (u7) the ARMCK reclaimed the ARMSOLAR: HP {hp0} -> {rows[-1][2]} over GameTime "
          f"{rows[0][0]}..{rows[-1][0]} ({len(rows)} reads); the build fraction remaining (+0x104) read "
          f"{'0.0 at every read' if not nano else f'non-zero at {len(nano)} reads, e.g. {nano[0]}'}")
    assert not nano, "(u7) the engine staged the reclaimed unit as a nanoframe"


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
    d = c.dump()
    start = c.mark()
    c.keys("dclick:%d,%d" % c.centre(c.hit(d, "wiki")))
    c.until(lambda: c.found(start, r"wiki: not opened - a rewind holds the pause"),
            "(r) no `wiki: not opened - a rewind holds the pause` line", seconds=30)
    d = c.dump()
    assert not d["open"] and not d["holds"], f"(r) the wiki opened over an open rewind: {d}"
    tacli("hud", c.name, "click", "rewind", timeout=180)
    c.until(lambda: rewind_state(c) == "idle", "(r) the rewind button did not close the rewind", seconds=120)
    c.until(lambda: c.runs(1.0), "(r) the game does not run after the rewind closed", seconds=120)
    d = c.open_wiki(expect_pause=True)
    c.once(not c.runs(2.0), "(r) GameTime ran with the window open")
    c.keys("esc")
    d = c.dump()
    assert not d["open"] and not d["paused"], f"(r) Esc did not close the wiki: {d}"
    c.once(c.runs(2.0), "(r) GameTime stands after the close")
    print(f"wiki recorded: (r) the wiki button {w}, the rewind button {rw}; each opens what it should, "
          "and the wiki does not open over an open rewind")


def asset(c):
    """(a1)..(a3): the 3D asset load in the game, against the same load run natively."""
    sys.path.insert(0, str(TREE / "tools/wiki"))
    import assetcheck
    held_at(c, HELD_AT)
    gamedir = tacli_api().Instance(c.name).gamedir
    out, ask = gamedir / "tagpu_wiki3d.json", gamedir / "tagpu_wiki3d.dump"
    out.unlink(missing_ok=True)
    start = c.mark()
    c.open_wiki(expect_pause=True)
    ask.write_text("")
    end = time.monotonic() + 60
    while not out.exists():
        assert time.monotonic() < end, "(a1) no tagpu_wiki3d.json within 60 s of the open"
        time.sleep(0.5)
    game = json.loads(out.read_text())
    line = c.found(start, r"wiki3d: loaded in")
    assert line, "(a1) no `wiki3d: loaded in` line"
    m = re.search(r"loaded in ([\d.]+) ms - arena (\d+) bytes", line)
    print(f"wiki asset: (a1) the game's load: {line.split('wiki3d: ', 1)[1]}")
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(a1) Esc did not close the window")

    assetcheck.build()
    mounts = gamedir / "tagpu_wiki3d.mounts"
    mounts.write_text("".join(f"{x['archive']}\n" for x in game["mounts"]))
    try:
        native, log = assetcheck.run_load(gamedir, assetcheck.pak_of_tree(), mounts, None, game["language"])
    finally:
        mounts.unlink(missing_ok=True)
    bad = assetcheck.diff(game, native, exact=True)
    assert not bad, f"(a2) the game's report and the native load differ in {len(bad)} place(s): {bad[:8]}"
    print(f"wiki asset: (a2) the game's report equals the native load over the instance's {len(game['mounts'])} "
          f"mounts: {sum(s['ok'] for s in game['stages'])} of {len(game['stages'])} grounds, "
          f"{sum(u['ok'] for u in game['units'])} of {len(game['units'])} unit types; native {log.splitlines()[-1]}")
    assert all(s["ok"] for s in game["stages"]) and all(u["ok"] for u in game["units"]), \
        "(a3) a ground or a unit type did not resolve on the retail setup"
    print(f"wiki asset: (a3) every ground and unit type resolved; the game's load took {m.group(1)} ms, "
          f"its arena {int(m.group(2))} bytes")


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
        got = drawn_window(c, "(m) the host's stage", want_all=True)
        print(f"wiki mp: (m) the host's stage in 3D: {got[0]} of {got[1]} frames drew the image; the render thread "
              f"{got[2]:.3f} ms producer and {got[3]:.3f} ms passes a frame on average, the passes at most "
              f"{got[4]:.3f} ms")
        assert got[2] + got[3] <= UNIT_MS, f"(m) the host's stage costs the render thread {got[2] + got[3]:.3f} ms a frame"
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


# the setups the wiki's 3D stage is measured under, as tools/compat/setups.json names them
MODS_SETUPS = ("retail", "escalation", "mayhem-11.3.0", "prota-4.8", "tazero-alpha5", "twilight-2.0b98")
# (entry, demo, tick) captured under each: a construction among Quick Build's units, and the
# gesture's ghosts on Build Lines' stage (the first frames of UNITS_SHOTS for each entry)
MODS_SHOTS = (("quickbuild", 3, 180), ("buildshapes", 2, 100))


def mods_setup(tacompat, setup, dll, out):
    """(w1)..(w5) on one setup: its folder as a player has it, started on a private Xvfb and walked
    into a skirmish, the wiki opened on both entries. The row, passed or with why it failed."""
    from PIL import Image
    row = {"setup": setup["name"], "boxes": [], "pass": False}
    out.mkdir(parents=True, exist_ok=True)
    xv = tacompat.start_xvfb()
    try:
        inst = tacompat.prepare_wine(setup, dll, xv.tacompat_display, suffix="-wk")
    finally:
        tacompat.stop_xvfb(xv)
    g = tacompat.start_wine(inst, None)
    display, gamedir, t0, seen = g["display"], inst["gamedir"], time.time(), set()

    def poll(step):
        """The display polled: any top-level window that is not the game's own, the crash report,
        the process."""
        tacompat.new_boxes(display, seen, row["boxes"], t0)
        assert not row["boxes"], f"({step}) a box on the display: {row['boxes']}"
        assert not (gamedir / "ErrorLog.txt").exists(), f"({step}) ErrorLog.txt was written"
        assert g["proc"].poll() is None, f"({step}) the game exited"

    def screen_grab(name):
        """The whole private display, so a box beside the game would be in the picture too."""
        r = subprocess.run(["import", "-display", f":{display}", "-window", "root", str(out / name)],
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, f"import: {r.stderr.strip()}"

    try:
        end = time.monotonic() + tacompat.POLL_S
        while "MAINMENU" not in (tacompat.tacli("ui", inst["name"], timeout=15).stdout.splitlines() or [""])[0]:
            poll("w1")
            assert time.monotonic() < end, "(w1) the main menu never came up"
            time.sleep(2)
        why = tacompat.wine_skirmish(inst["name"], g["proc"])
        assert why is None, f"(w1) {why}"
        poll("w1")
        print(f"wiki mods: {setup['name']}: (w1) a skirmish on {tacompat.BATTLE_MAP}")

        c = Wiki.__new__(Wiki)
        c.attach(inst["name"])
        start = c.mark()
        d = c.open_wiki(expect_pause=True)
        row["screen"] = "%dx%d" % tuple(d["game"])
        row["load"] = wait_line(c, start, r"wiki3d: loaded in", "(w2) the 3D asset load", 180).split("wiki3d: ", 1)[1]
        rep, ask = gamedir / "tagpu_wiki3d.json", gamedir / "tagpu_wiki3d.dump"
        rep.unlink(missing_ok=True)
        ask.write_text("")
        end = time.monotonic() + 60
        while True:
            try:
                report = json.loads(rep.read_text())
                break
            except (OSError, ValueError):
                assert time.monotonic() < end, "(w2) no whole tagpu_wiki3d.json within 60 s"
                time.sleep(0.5)
        (out / "tagpu_wiki3d.json").write_text(json.dumps(report, indent=1))
        row["report"] = report
        poll("w2")
        print(f"wiki mods: {setup['name']}: (w2) {row['load']}; {len(report['mounts'])} mounts")

        row["stages"], row["types"], row["drawn"] = {}, {}, {}
        for entry, demo, tick in MODS_SHOTS:
            ppm, armed = shoot(c, out, None, entry, demo, tick, None, name=f"{entry}-d{demo}-t{tick}.game.ppm",
                               window=False)
            Image.open(ppm).save(ppm.with_suffix(".png"))
            ppm.unlink()
            ln = wait_line(c, start, rf"wiki3d: stage {entry}: the ground", f"(w3) no ground line for {entry}")
            row["stages"][entry] = ln.split(f"stage {entry}: ", 1)[1]
            ln = wait_line(c, start, rf"wiki3d: {entry}: the types in 3D:", f"(w3) no types line for {entry}")
            row["types"][entry] = ln.split(f"{entry}: ", 1)[1]
            # a whole periodic line over frames of this stage alone: the second one from here
            mark = c.mark()
            wait_line(c, mark, DRAWN, f"(w3) {entry}: no frame line", 180)
            mark = c.mark()
            m = re.search(DRAWN, wait_line(c, mark, DRAWN, f"(w3) {entry}: no frame line", 180))
            row["drawn"][entry] = (int(m.group(1)), int(m.group(2)))
            screen_grab(f"{entry}-d{demo}-t{tick}.screen.png")
            poll("w3")
            print(f"wiki mods: {setup['name']}: (w3) {entry} demo {demo} tick {tick}: "
                  f"{armed.split('wiki3d: ', 1)[1]}; {row['stages'][entry]}; {row['types'][entry]}; "
                  f"{m.group(1)} of {m.group(2)} frames drew the image")
        c.keys("esc")
        wait_dump(c, lambda d: not d["open"], "(w4) Esc did not close the window")
        time.sleep(2)
        poll("w4")
        # (w5) the claim: a ground the report resolved is drawn in 3D on every frame, one it did not
        # is the whitebox on every frame
        for st in report["stages"]:
            e = st["entry"]
            if e not in row["stages"]:
                continue
            in3d = row["stages"][e].startswith("the ground in 3D")
            assert in3d == bool(st["ok"]), f"(w5) {e}: the report says ok={st['ok']}, the stage `{row['stages'][e]}`"
            drew, of = row["drawn"][e]
            assert (drew == of) if st["ok"] else (drew == 0), f"(w5) {e}: {drew} of {of} frames drew the image"
        row["pass"] = True
        print(f"wiki mods: {setup['name']}: (w4, w5) each stage drew what the report resolved; no box, "
              "no crash report, the game alive")
    except CHECK_FAILURES as e:
        row["why"] = str(e)
        print(f"wiki mods: {setup['name']}: FAILED: {e}", file=sys.stderr)
        try:
            screen_grab("failure.screen.png")
        except Exception:
            pass
    finally:
        logs = sorted((gamedir / "log").glob("tagpu*.log"), key=lambda f: f.stat().st_mtime) \
            if (gamedir / "log").is_dir() else []
        if logs:
            shutil.copyfile(logs[-1], out / "tagpu.log")
        tacompat.stop_wine(g)
    return row


def mods_table(rows):
    """Per setup: the archive each ground source, feature and unit came from, and what fell back."""
    def arc(p):
        return p.replace("\\", "/").rsplit("/", 1)[-1] if p else "-"
    for r in rows:
        rep = r.get("report") or {}
        print(f"== {r['setup']}: {'PASS' if r['pass'] else 'FAIL - ' + r.get('why', '')}")
        print(f"   {r.get('load')}; the screen {r.get('screen')}; boxes: {r['boxes'] or 'none'}; "
              f"the first mounts {[arc(m['archive']) for m in rep.get('mounts', [])][:4]}")
        for st in rep.get("stages", []):
            srcs = "; ".join(f"{s['map']} <- {arc(s['from'])}" + ("" if s["ok"] else f" ({s['why']})")
                             for s in st.get("sources", []))
            feats = sorted({f"{f['name']} <- {arc(f.get('from'))}/{arc(f.get('gaf_from'))}" + ("" if f["ok"] else " MISS")
                            for f in st.get("features", [])})
            print(f"   stage {st['entry']}: ok={st['ok']} {st.get('why') or ''} | {srcs} | {', '.join(feats)}")
            print(f"     drawn: {r.get('stages', {}).get(st['entry'])}; {r.get('drawn', {}).get(st['entry'])} frames")
            print(f"     {r.get('types', {}).get(st['entry'])}")
        for u in rep.get("units", []):
            md = u.get("model") or {}
            tex = sorted({arc(t.get("from")) for t in md.get("textures", []) if t.get("from")})
            print(f"   unit {u['name']}: ok={u['ok']} fbi <- {arc(u.get('from'))}, 3do <- {arc(md.get('from'))}, "
                  f"cob <- {arc(u.get('script_from'))}, textures <- {', '.join(tex) or '-'}"
                  + ("" if u["ok"] else f" - {u.get('why')}"))


def mods(names, shots):
    """(w1)..(w5) per setup, one at a time under the run lock."""
    assert shots, "mods needs --shots DIR"
    sys.path.insert(0, str(TREE / "tools/compat"))
    import tacompat
    runlock.hold(TREE, f"wiki_regression mods ({','.join(names)})")
    byname = {s["name"]: s for s in tacompat.load_json("setups.json")["setups"]}
    unknown = [n for n in names if n not in byname]
    assert not unknown, f"no such setup in tools/compat/setups.json: {unknown}"
    dll = tacompat.snapshot_dll(TREE / "tagpu/ddraw/ddraw.dll")
    rows = []
    for n in names:
        rows.append(mods_setup(tacompat, byname[n], dll, shots / n))
        (shots / "rows.json").write_text(json.dumps(rows, indent=1, default=str))
    mods_table(rows)
    bad = [r["setup"] for r in rows if not r["pass"]]
    assert not bad, f"failed on {bad}"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("instance")
    p.add_argument("mode", choices=("sp", "page", "demos", "try", "recorded", "asset", "stage3d", "units", "cursor", "mp", "mods"))
    p.add_argument("--launch", action="store_true",
                   help="sp/recorded: launch on place-basic.json first, under the run lock")
    p.add_argument("--res", default="1920x1080", help="sp/recorded: the game's resolution")
    p.add_argument("--shot", help="sp, page, try: grab the presented frame with the window open to this PNG "
                                  "(page: on the second entry and its second demo, at (p2); try: one a demo, "
                                  "named <stem>-<entry>-<demo>, mid-replay)")
    p.add_argument("--shots", type=Path, help="demos: grab each entry's demo mid-play, and the mockup's, into this folder; "
                                              "stage3d, units: the captures of the wiki's target and the presented frames")
    a = p.parse_args()
    t0 = time.monotonic()
    stop = None
    try:
        if a.mode == "mp":
            mp(a.instance)
        elif a.mode == "mods":
            mods(MODS_SETUPS if a.instance == "all" else a.instance.split(","), a.shots)
        else:
            if a.launch:
                stop = launched(a.instance, a.mode, a.res)
            c = Wiki(a.instance, {"ARMCK"})
            try:
                if a.mode == "sp":
                    sp(c, a.shot)
                elif a.mode == "page":
                    page(c, a.shot)
                elif a.mode == "demos":
                    demos(c, a.shots, a.res)
                elif a.mode == "try":
                    try_it(c, a.shot)
                elif a.mode == "asset":
                    asset(c)
                elif a.mode == "stage3d":
                    stage3d(c, a.shots, a.res)
                elif a.mode == "units":
                    units(c, a.shots, a.res)
                elif a.mode == "cursor":
                    cursor(c, a.shots, a.res)
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
