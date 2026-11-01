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
    wiki_regression.py <instance> watch --launch [--shots DIR]
                                                      Watching, a frame:game entry: its replay, the stage
                                                      HUD and Try it's presses on it
    wiki_regression.py <name> mp                     a two-player network game on Wine, started by
                                                      tools/compat/tacompat.py's launcher
    wiki_regression.py all|<setup>[,<setup>...] mods --shots DIR
                                                      the 3D stage and the sidebar under each
                                                      setup of the compatibility suite, one game
                                                      a setup
    wiki_regression.py - offline                      the offline checks, no game: OFFLINE_CHECKS
    wiki_regression.py - gate --shots DIR [-j N]      all of it at once under one hold of the run lock:
                                                      the offline checks, each single-player mode on
                                                      its own instance and X server, and each setup
                                                      of mods, every lane a process (gate)

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
  (p3) fold:1 folds the Building category (Building's entry rows not drawn) and unfolds it;
  (p4) two wheel notches down over the text scroll it two steps, one up one back, each held to
       the limit the drawn frame posted;
  (p5) the text's thumb dragged 12 px scrolls it by the travel the drawn frame gave it;
  (p7) four real wheel notches sent at once through the instance's X server (xdotool), which the
       window procedure pumps between two frames, scroll the text four steps, not one -- run where
       the text scrolls past two notches; (p4), (p5) and (p7) are not run where it does not scroll;
  (p6) Esc closes and frees the demo; reopened, the selection stands and the demo starts afresh.
demos, on the same launch as sp:
  (d1) tagpu_wiki.check plays every demo to its end in the game, on the sidebar layout a demo would
       start on, on each of its sides at both interface types, and every logged hash equals `node
       tools/wiki/simparity.mjs --hash --layout --side --iface` on the layout the game wrote beside it
       (with --pack, the side pack of that layout, when it is the mod's); a game entry's demo is played
       as a replay, to its end, and the game ticks and milliseconds the check logged for it are printed;
  (d2) a demo's pace on the page's own clock: the ticks played between two dumps equal 30 a second
       times the speed (1x, then 2x) of the seconds the clock played, less the ticks its per-frame
       bound took off, to within a tick -- the rule the clock paces by, which no load can break;
  (d3) play pauses (the tick stands), replay restarts, an ended demo holds its last frame and
       play starts it again;
  (d4) each entry with a demo paused mid-play on the tick tagpu_wiki.pause asks for, played there
       under tagpu_wiki.turbo; with --shots DIR the game's frame and the mockup's at the same screen,
       entry, demo and tick, side by side (`<res>-<entry>-<demo>-t<tick>`), the mockup in the game
       look with the stock pack's sidebar (sidebar=mod&mod=stock&gfx=game);
  (d5) the most one fill of the open page cost the game thread, logged.
try, on the same launch as sp:
  (t1) Quick Build's `snap` and then Build Lines' `line` selected, and Try it yourself pressed: the
       stage is the player's, with the sidebar on (the dump's `side_h` rows) showing its builder's
       first page;
  (t2) the demo's inputs given as device input: each world point turned into the client point the
       dump's stage draws it at (tagpu_wikistage_fit inverted, from the `stage` hit and the frame's
       unit), moves as `dmove:`, the left button as `ddown:`/`dup:` there, Shift as `down:`/`up:`,
       a build as a player arms it on the stage's sidebar -- PREV or NEXT pressed the nearer way
       round to the building's page, then its button, each a press and a release inside. Paused,
       the dump's log of what the game gave its simulation holds the demo's presses and releases
       and the sidebar's in order, every move within a pixel of the point it was given at, and its
       state hash equals sim.js fed the same logged inputs at the same ticks (`simparity.mjs
       --try`); sim.js's view of it (tools/wiki/sideview.mjs --dump) then gives the page each pager
       reached and the building each button armed. With --shot, the presented frame is grabbed
       before `snap`'s click and before `line`'s first release, the line's sites on the stage;
  (t2c) on the sidebar, read back the same way: NEXT and PREV page the ARMCK, the ORDERS tab shows
       the side's GEN and the BUILD tab its page again, and STOP disarms the building armed and
       empties the orders;
  (t2b) exactly one cursor: the pointer over the sidebar's BUILD tab and over the stage's centre,
       one, on the stage; over the play control, off the stage, the player's alone (needs
       tools/wiki/gfx.py's pack, as cursor does);
  (t3) Shift and the left button held on the stage when Back to demo is pressed are let go in the
       simulation, and their late releases are taken, nothing owed;
  (t4) Ctrl and the left button held when Esc closes the window: let go, nothing owed after their
       releases, and the engine's key ring unmoved over the whole mode;
  (t5) reopened, the window is in Try it, afresh; Back to demo, and Esc.
watch, on the same launch as sp (the presentation spec's 6.5):
  (v1) entry:7 picks Watching, drawn as a game frame: the theatre's drawer, demo:0..6, play, replay, x1, x2, try and
       about are the page's controls (the hit map's `src` 0), the stage HUD's (`src` 1) lie inside the `stage` hit;
       About puts its text's `scroll:text` first in the map, the drawer its `entry:*` (and closes About);
  (v2) each demo paused (tagpu_wiki.pause, under the turbo) at each of its viewer's trips' presses: the frame record
       the game thread placed -- its playhead, nmark, toast, question, held and cur -- and the replay's hash equal
       replay.js's at the same demo tick in the game's context (`simparity.mjs --hash --frame`);
  (v3) with --shots DIR, each demo held at a tick (tagpu_wiki.shot) and the presented frame grabbed
       (`<res>-watching-<demo>-t<tick>.game.png`); play, track, bookmarks and leave also beside the mockup at the same
       state (the game look, the stock pack): the frame's column and both bars equal pixel for pixel, located by the
       dump's frame_fit (the render thread's fit of the same canvas), and the world with the HUD over it measured;
  (v4) Try it on the first demo through device clicks at the stage HUD's controls: play toggles the pause; fwd, fwd,
       rew move the playhead by +300, +300, -300, clamped; the track pressed at 25 % of its run seeks there to within
       a pixel's ticks, a 10-move drag to 75 % seeks along it and the release lands there; bookmark gives its toast;
       leave opens the question, a click on the world outside it is No, the replay as it was; leave and Yes end Try
       it with the line that says where the game goes. GameTime stands and the key ring's indices stay equal;
  (v5) the window closed and opened again on Watching, Try it and the track pressed at its end: the seek's time and
       the most one fill cost the game thread from the open;
       Esc, and the render thread's stage HUD and bars cost and the pass's peak instances and batches against its
       caps, from the log.
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
       frame draws it again -- each over the frame count, tagpu_wiki3d.count: the stage's next 60
       drawn frames from the first at rest;
  (s5) the stage at rest does no work over those frames -- no ground or type records built, nothing
       baked or dropped, no record the bake refused -- and its render-thread time is said.
units, on the same launch as sp (the plan's landing 6h):
  (u1) the open's 3D asset load ends and the first stage's types line names every type in 3D;
  (u2) at ss 2, Quick Build 3 and 8 (a construction, a reclaim, the upgrade's nanoframe), Build
       Lines 2 and 4 (the gesture's ghosts), Quick Build 9 (an ARMCK walking toward +x) and Reclaim 3
       (the field's wrecks, all standing and then two reclaimed) held at
       fixed ticks through tagpu_wiki.shot, each capture written to DIR as in stage3d with the
       presented frame grabbed beside it;
  (u3) tools/wiki/stage3dcheck.py over DIR, the unit pixels within question 8's tolerance;
  (u4) the walking ARMCK's facing in the mockup's simulation at that tick, toward +x, which the
       capture's unit pixels (u3) then show the game's model turned to;
  (u5) the lever `nounit=ARMCK`: the types line puts ARMCK alone among the boxes, every frame still
       draws the image, and the capture of Quick Build 3 differs from (u2)'s where the ARMCK stood;
  (u6) the lever gone, the stage at rest does no work over the frame count, as (s5), and its
       render-thread time, the producer and the passes, is said;
  (u7) the window closed, a reclaim in the game itself at its top speed: the ARMCK ordered (tacli
       order) to reclaim the scenario's ARMSOLAR, the solar's build fraction remaining (+0x104) and HP
       (+0x108) read until it is gone or 12000 ticks of GameTime have run, and the presented frame
       grabbed mid-reclaim -- the engine's own staging of a reclaimed unit, which the stage's records
       follow. (u3)'s check runs while (u4)..(u7) do.
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
Twilight), one game a setup, under the run lock (the plan's landing 6i; the sidebar's spec, 2E):
  (w1) the setup's folder built by tacompat's prepare_wine with this tree's DLL, started on a private
       2560x1440 Xvfb (the game takes the display's size) and walked into a skirmish on the suite's
       map (tacompat.wine_skirmish);
  (b1) the setup's own sidebar against the composer: the engine's frame cropped to the column, rows
       128 down to the layout's height, equal pixel for pixel to tools/wiki/sidedump.mjs on the
       setup's pack (tools/wiki/sidepacks.py) with each gadget greyed as the engine's snapshot of
       the screen has it -- nothing selected, and an ARMCK placed and selected where the pack gives
       it pages (its first page);
  (w2) the wiki opened: the 3D asset load's line, and its report tagpu_wiki3d.json copied to
       DIR/<setup>/ -- which archive each ground source, feature, unit file and texture came from;
  (w3) Quick Build demo 3 at tick 180 and Build Lines demo 2 at tick 100 captured through
       tagpu_wiki.shot (DIR/<setup>/<entry>-d<demo>-t<tick>.game.png) with the whole display grabbed
       beside each (.screen.png); each stage's ground line, its types line (in 3D, and the boxes) and
       the frame count at rest, as (s4);
  (b2) the check (tagpu_wiki.check): the layout every demo starts on is the mod's, its file
       tagpu_wiki.layout.json equal to the pack's layout, and every demo's hash on each side at both
       interface types equal to `simparity.mjs --hash --layout --pack --side --iface` of that file;
  (b3) Quick Build demo 1 held at tick 10 (the ARMCK selected, its first page at rest; TA Zero, which
       has no ARMCK page, its side's GEN): the presented frame's sidebar column, each client pixel
       mapped back to the texel its nearest sampling shows, equal to the composer on the pack for
       the same view (tools/wiki/sideview.mjs --play --compose); DIR/<setup>/sidebar.png puts the
       stage's column, the composed one and the engine's own sidebar side by side. With (b1) this
       is the chain stage == composer(pack, the wiki's state) and composer(pack, the engine's
       state) == the engine: one pack, one composer, the states apart only where the wiki sets
       them (decision 7 greys every order but STOP);
  (b4) Build Lines demo 1 played in the player's context: where sim.js, replaying the dump's ctx,
       locks it (a role with no unit, or a build the sidebar cannot reach) it holds at that tick with
       the "Turned off in this mod" badge above the stage; elsewhere it plays past it, no badge;
  (w6) Watching's first demo held at tick 150 (tagpu_wiki.shot): the game frame's two bars, each client pixel against
       the texel of bar.js's picture on the setup's pack that its nearest sampling shows, Impure's emblems left out
       (tools/wiki/barcheck.mjs --compose) -- the per-mod bar art the loader reads (Mayhem's GUI table, TA Zero's three
       sides and logos, Escalation's NEWINT) as the game draws it;
  (w4) Esc closes the window;
  (w5) each stage drew what the report resolved: a ground that resolved in 3D on every frame of the
       line, one that did not as the whitebox on every frame.
  At every step the display is polled: any top-level window that is not the game's own, an
  ErrorLog.txt, or the game gone fails the setup. The table of every setup is printed at the end and
  kept in DIR/rows.json.
mp:
  (m) the wiki opened on the host takes no pause: both peers' GameTime advances over 3 s, and so
      does the host's demo, whose cost to the game thread is logged, and the host's stage at rest
      does no work over the frame count, as (s5); a right and a left click on
      the margin leave the host's selected commander's orders and selection as they were; Esc
      closes it, and both peers still run, with no desync line in either log.
"""
import argparse
import itertools
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from importlib.machinery import SourceFileLoader
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


def trigger(path, text):
    """A trigger file with content written whole: the DLL reads it on its own beat and takes an empty
    read as a malformed ask, so it is written beside and renamed onto its name, which the reader sees
    as absent or as all of it."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text)
    os.replace(tmp, path)


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

    # Building's leaves are nodes 2 and 3; Reclaiming's entry, node 5, and the Replays category's Watching, node
    # WATCH_NODE, are drawn either way
    press(c, d, "fold:1")
    d = wait_dump(c, lambda d: d["folds"] == [1] and keys_of(d, "entry:") == ["entry:5", f"entry:{WATCH_NODE}"],
                  "(p3) fold:1 did not fold Building")
    press(c, d, "fold:1")
    d = wait_dump(c, lambda d: d["folds"] == [] and keys_of(d, "entry:") == ["entry:2", "entry:3", "entry:5",
                                                                             f"entry:{WATCH_NODE}"],
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


# a line of the game's check (tagpu_wiki.c check): `<entry>/<demo>/<side>/<iface> <hash>`, or the same key and
# `did not start - <why>` for a demo the DLL's loader or sim refused
CHECK_LINE = re.compile(r"wiki check: (\S+?/\S+?/\d+/\d+) (?:([0-9a-f]{16})|did not start - .*)$")
REFUSED = "refused"


def check_hashes(lines):
    """The game's check as `{entry/demo/side/iface: hash}`, REFUSED for a demo that did not start."""
    out = {}
    for ln in lines:
        if (m := CHECK_LINE.search(ln)):
            out[m.group(1)] = m.group(2) or REFUSED
    return out


def simparity_hashes(layout, pack=None, scripts=False):
    """sim.js's hashes of every demo in every context the game's check plays (tagpu_wiki.c check): each side of the
    sidebar layout file `layout` (tagpu_wiki.layout.json, the one the check played on and wrote: the running mod's,
    the fixture, or null and one side) at Interface Type 0 and 1, with that side's casting from the side pack `pack`
    when the layout is the mod's -- the check gives a casting on the mod's layout only -- and with `scripts` the
    units' scripts, from the packs gfx.py built from the same install, as the check runs them when its log says so.
    Keyed as check_hashes keys the game's lines; REFUSED where sim.js refuses the demo."""
    lay = json.loads(Path(layout).read_text())
    want = {}
    for k in range(len(lay["sides"]) if lay else 1):
        for i in (0, 1):
            cmd = ["node", str(TREE / "tools/wiki/simparity.mjs"), "--hash", "--layout", str(layout), "--side", str(k),
                   "--iface", str(i), *(["--pack", str(pack)] if pack else []), *(["--scripts"] if scripts else [])]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            assert r.returncode == 0, f"simparity --hash --side {k} --iface {i}: {r.stderr.strip()}"
            for ln in r.stdout.splitlines():
                if ln.strip():
                    key, val = ln.split(None, 1)
                    want[f"{key}/{k}/{i}"] = REFUSED if val.startswith("refused") else val.strip()
    return want


def check_against(got, want, what):
    """The game's check equal to sim.js's, key for key: the same contexts played, each hash equal, and a demo
    refused on one side exactly when it is on the other. A difference fails naming its keys."""
    missing, extra = sorted(set(want) - set(got)), sorted(set(got) - set(want))
    differ = sorted(k for k in set(want) & set(got) if got[k] != want[k])
    assert not (missing or extra or differ), \
        f"{what} the game's check and simparity --hash differ: " + "; ".join(
            f"{name} {', '.join(f'{k} (game {got.get(k)}, sim.js {want.get(k)})' for k in keys[:12])}"
            f"{f' and {len(keys) - 12} more' if len(keys) > 12 else ''}"
            for name, keys in (("not played in the game:", missing), ("played only in the game:", extra),
                               ("differ:", differ)) if keys)
    return len(want)


def pack_for_layout(layout):
    """The side pack whose layout is the game's (tagpu_wiki.layout.json), whose castings the check plays on the mod's
    layout: the one pack under SIDE_PACKS with the same layout, or a failure naming why."""
    game = layout_norm(json.loads(Path(layout).read_text()))
    hits = [d for d in sorted(SIDE_PACKS.glob("*")) if (d / "side.json").is_file()
            and layout_norm(json.loads((d / "side.json").read_text())["layout"]) == game]
    assert len(hits) == 1, (f"the game's layout {layout} is the layout of {len(hits)} side packs "
                            f"({', '.join(d.name for d in hits) or 'none: tools/wiki/sidepacks.py builds them'})")
    return hits[0]


def tick_rate(c, speed, ticks=60):
    """(d2) the demo's pace on its own clock (tagpu_wiki.c, THE CLOCK'S OWN ACCOUNT): from one dump to
    a later one of the same demo, the ticks it played equal 30 x `speed` x the seconds the page's
    clock played, less the ticks STEPS_MAX took off them, to within the one tick a fill may leave
    owed. Exact under any load: a slow frame is seconds the clock clamped, and ticks it took off,
    never a rate the harness measured. (ticks played, clock seconds, the later dump)"""
    a = c.dump()
    assert a["speed"] == speed and not a["turbo"], f"(d2) the demo is not at {speed}x on the clock: {a}"
    b = wait_dump(c, lambda d: d["tick"] - a["tick"] >= ticks or d["done"] or d["demo"] != a["demo"],
                  f"(d2) {a['demo']} did not play {ticks} ticks", seconds=120)
    assert b["demo"] == a["demo"] and not b["done"], f"(d2) the demo ended or changed inside the window, at tick {b['tick']}"
    played, secs = b["tick"] - a["tick"], b["clock_s"] - a["clock_s"]
    owed = 30 * speed * secs - (b["clipped"] - a["clipped"])
    assert abs(played - owed) < 1.01, (f"(d2) {a['demo']} at {speed}x played {played} ticks over {secs:.3f} s of its "
                                       f"clock, where the clock owes {owed:.2f}")
    return played, secs, b


def turbo(c, k):
    """tagpu_wiki.turbo at k ticks a fill (0: gone, the demo on the clock again), once the game says so"""
    f, start = c.gamedir / "tagpu_wiki.turbo", c.mark()
    if k:
        trigger(f, f"{k}\n")
        wait_line(c, start, r"wiki: tagpu_wiki\.turbo: a playing demo steps", "the turbo was not read")
    else:
        f.unlink(missing_ok=True)
        wait_line(c, start, r"wiki: tagpu_wiki\.turbo: gone", "the turbo's end was not read")


def mockup_shot(state, path):
    """The mockup at the same state -- the screen, the entry, the demo paused at the tick -- grabbed
    headless by playwright-cli, one CSS pixel a device pixel, as wikilaycheck.py opens it. The
    sidebar is the mod's (`mod`, a tools/wiki/sidepacks.py pack name, stock by default), its pack
    loaded before the demo is read. `look` "game" draws the stage in the game's graphics and the
    sidebar in the mod's pictures, as the game draws both (gfx=game, on the GPU as
    tools/wiki/stage3dcheck.py renders it); without it the stage is the whitebox. tools/wiki/serve.py
    serves it, so its demo plays with the units' scripts as the game's does. A frame:game entry's demo is its replay,
    stepped to the demo tick as tagpu_wiki.shot steps the game's. Returns the frame's viewer's cursor, the point it
    is drawn at in the picture's pixels, or None."""
    sys.path.insert(0, str(TREE / "tools/wiki"))
    import wikilaycheck as wl
    from PIL import Image
    gw, gh = state["res"]
    httpd = SourceFileLoader("wikiserve", str(TREE / "tools/wiki/serve.py")).load_module().server(quiet=True)
    tmp = path.parent / f".mock{os.getpid()}"
    tmp.mkdir(parents=True, exist_ok=True)
    cfg = tmp / "pw.json"
    game = state.get("look") == "game"
    launch = {"ignoreDefaultArgs": ["--hide-scrollbars"]}
    if game:
        import stage3dcheck
        launch.update(headless=True, args=stage3dcheck.GPU_ARGS["vulkan"])
    cfg.write_text(json.dumps({"browser": {"contextOptions": {"deviceScaleFactor": 1, "viewport": {"width": 1200, "height": 900}},
                                           "launchOptions": launch}}))
    pw = ["playwright-cli", f"-s=wikishot{os.getpid()}", f"--config={cfg}"]
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    try:
        run("open", "about:blank")
        run("resize", str(gw + 34), str(gh + 900))
        run("goto", f"http://127.0.0.1:{httpd.server_address[1]}/tools/wiki/mockup/index.html?res={gw}x{gh}&open&skin=kit"
                    f"&sidebar=mod&mod={state.get('mod', 'stock')}&rewind=0{'&gfx=game' if game else ''}"
                    f"&entry={state['entry']}&demo={state['demo_n'] + 1}")
        # THE PAGE READY, waited for inside the page in one call (a playwright-cli call is a process, a
        # second each): the fonts, the window, the mod's sidebar pack, and with the game look the
        # demo's stage -- WikiGfx keys a stage `<entry>.<scene>`, WikiPage.stageId() -- or the wait's
        # bound fails the grab, which a picture of a page still loading would otherwise pass
        bound = 30000 if game else 10000
        ready = wl.result(run("eval", "async () => { const ok = () => !!(window.WikiPage && (WikiPage.sim() || WikiPage.replay())"
                                      " && document.fonts.status === 'loaded' && document.querySelector('#win canvas.skin')"
                                      " && !document.querySelector('#sidebar-note').textContent.startsWith('loading')"
                                      f"{' && WikiGfx.ready(WikiPage.stageId())' if game else ''});"
                                      f" const t0 = performance.now(); while (!ok() && performance.now() - t0 < {bound})"
                                      " await new Promise(r => setTimeout(r, 50)); return ok(); }").stdout)
        assert ready is True, f"the mockup page was not ready within {bound / 1000:.0f} s ({state['entry']}, " \
                                f"demo {state['demo_n'] + 1}{', game look' if game else ''}): {ready}"
        v = wl.result(run("eval", "async () => { const S = WikiPage.state, $ = s => document.querySelector(s);"
                                  # the demo afresh and paused in one task, whatever the load left it at: past
                                  # the tick, or done (play on a done demo restarts it playing), and no frame
                                  # runs between the two
                                  " $('#c-replay').click(); if (S.playing) $('#c-play').click();"
                                  " const sim = WikiPage.sim(), rep = WikiPage.replay();"
                                  f" if (sim) while (sim.tick < {state['tick']} && !sim.done) sim.step();"
                                  f" else while (rep.view().tick < {state['tick']} && !rep.view().done) rep.step();"
                                  " for (let i = 0; i < 4; i++) await new Promise(r => requestAnimationFrame(r));"
                                  " const f = $('#frame').getBoundingClientRect();"
                                  " const F = WikiPage.frameDrawn && WikiPage.frameDrawn(), q = F && F.cursor,"
                                  " c = $('#stage'), r = c.getBoundingClientRect();"
                                  " const at = q ? [r.left + q.x * r.width / c.width - f.left, r.top + q.y * r.height / c.height - f.top] : null;"
                                  " return JSON.stringify([f.left, f.top, f.width, f.height, sim ? sim.tick : rep.view().tick, at]); }").stdout)
        fx, fy, fw, fh, tick, cursor_at = json.loads(v)
        assert (round(fw), round(fh)) == (gw, gh) and tick == state["tick"], \
            f"the mockup's frame is {fw} x {fh} at tick {tick}, not {gw} x {gh} at {state['tick']}"
        run("screenshot", f"--filename={tmp / 'page.png'}")
        Image.open(tmp / "page.png").crop((round(fx), round(fy), round(fx) + gw, round(fy) + gh)).save(path)
        print(f"wiki: the mockup at the same state grabbed to {path}")
        return cursor_at
    finally:
        run("close")
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)


def demos(c, shots, res):
    """(d1)..(d6): the demos' parity in the game, their clock, the controls, captures beside the
    mockup's, and what the page costs the game thread."""
    held_at(c, HELD_AT)
    start = c.mark()
    (c.gamedir / "tagpu_wiki.check").write_text("")
    end = time.monotonic() + 120
    while not (line := c.found(start, r"wiki check: \d+ demos played")):
        assert time.monotonic() < end, "(d1) no `wiki check: N demos played` line in 120 s"
        time.sleep(0.5)
    lay = c.found(start, r"wiki check: the sidebar layout is ")
    assert lay, "(d1) no `wiki check: the sidebar layout is` line"
    print(f"wiki demos: (d1) {lay.split('wiki check: ', 1)[1]}")
    layf = c.gamedir / "tagpu_wiki.layout.json"
    pack = pack_for_layout(layf) if "the mod's" in lay else None
    # the check names both of its sources; simparity --hash --scripts gives sim.js both or neither, as the game's one 3D
    # load does, so a line that says otherwise, or does not say, is a failure rather than a guess
    src = re.search(r"the units' scripts are (on|off), their sounds (on|off)", lay)
    assert src, f"(d1) the check's line does not say whether the units' scripts and sounds are on: {lay}"
    assert src.group(1) == src.group(2), f"(d1) the game's check has scripts {src.group(1)} but sounds {src.group(2)}"
    n = check_against(check_hashes(c.log[start:]), simparity_hashes(layf, pack, src.group(1) == "on"), "(d1)")
    print(f"wiki demos: (d1) {line.split('wiki check: ', 1)[1]}; all {n} hashes (every side at both interface types"
          f"{f', with {pack.name} castings' if pack else ''}) equal simparity --hash")
    # a game entry's demos, played as replays: their cost in the game, what the replay layer's step budgets are set from
    for ln in c.log[start:]:
        if "wiki check: the replay " in ln:
            print(f"wiki demos: (d1) {ln.split('wiki check: ', 1)[1]}")

    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"] and d["sim"], "(d2) the page was not drawn")
    t0 = press(c, d, "demo:3")
    wait_dump(c, lambda d: d["demo"] == "bestfit" and fresh(d, t0), "(d2) demo:3 did not start quickbuild/bestfit")
    n1, s1, d = tick_rate(c, 1)
    press(c, d, "x2")
    d = wait_dump(c, lambda d: d["speed"] == 2, "(d2) x2 did not set 2x")
    # from the start again, so the window at 2x ends inside the demo's 722 ticks however long the
    # dumps take to come back
    t0 = press(c, d, "replay")
    wait_dump(c, lambda d: d["playing"] and fresh(d, t0), "(d2) replay did not restart the demo at 2x")
    n2, s2, d = tick_rate(c, 2)
    print(f"wiki demos: (d2) quickbuild/bestfit on its own clock: {n1} ticks in {s1:.3f} s at 1x, {n2} in {s2:.3f} s at "
          f"2x, each 30 a second times the speed less what the clock clamped, to within a tick")

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
    turbo(c, 8)                  # to its end the sooner; an end is an end at any pace
    try:
        d = wait_dump(c, lambda d: d["done"], "(d3) quickbuild/cancel did not end", seconds=120)
    finally:
        turbo(c, 0)
    t = d["tick"]
    time.sleep(1.0)
    d = c.dump()
    assert d["done"] and d["tick"] == t, f"(d3) the ended demo did not hold its last frame: {t} -> {d['tick']}"
    t0 = press(c, d, "play")
    d = wait_dump(c, lambda d: not d["done"] and fresh(d, t0), "(d3) play on an ended demo did not start it again")
    press(c, d, "x1")
    wait_dump(c, lambda d: d["speed"] == 1, "(d3) x1 did not set 1x")
    print("wiki demos: (d3) pause, replay, the last frame held and played again")

    # (d4) each entry with a demo paused mid-play, on the tick tagpu_wiki.pause asks for, grabbed beside
    # the mockup at the same state -- the retail game's, so the stock pack's sidebar: past tick 150 of
    # either building demo, the METAL ring at 190, its drag nearly out before its release at 248, and
    # the ENERGY ring at 812, its two trees reclaimed (780, 807) and so gone from the game look's image
    # before the demo's end at 878. The turbo plays each there the sooner; the pause is on its tick at any pace.
    turbo(c, 8)
    try:
        for entry, k, demo, at in (("quickbuild", 0, "snap", 150), ("buildshapes", 1, "line", 150),
                                   ("reclaim", 2, "metal", 190), ("reclaim", 3, "energy", 812)):
            start = c.mark()
            trigger(c.gamedir / "tagpu_wiki.pause", f"{entry} {k + 1} {at}\n")
            asked = wait_line(c, start, r"wiki: (pause \d+ asked|tagpu_wiki\.pause: )", f"(d4) {entry}/{demo}")
            assert re.search(r" asked: .*\d$", asked), f"(d4) {asked}"
            n = re.search(r"pause (\d+) asked", asked).group(1)
            wait_line(c, start, rf"wiki: pause {n}: ", f"(d4) {entry}/{demo} did not pause at tick {at}", 120)
            d = wait_dump(c, lambda d: d["entry"] == entry and d["demo"] == demo and not d["playing"],
                          f"(d4) {entry}/{demo} is not the paused demo")
            assert d["tick"] == at and not d["done"], f"(d4) {entry}/{demo} paused at tick {d['tick']}, not {at}"
            time.sleep(1.0)          # the paused frame presented
            if shots:
                shots.mkdir(parents=True, exist_ok=True)
                base = shots / f"{res}-{entry}-{demo}-t{d['tick']}"
                grab(c, base.with_suffix(".game.png"))
                mockup_shot({"res": [int(v) for v in res.split("x")], "entry": entry, "demo_n": k, "tick": d["tick"],
                             "mod": "stock", "look": "game"}, base.with_suffix(".mockup.png"))
            print(f"wiki demos: (d4) {entry}/{demo} paused at tick {d['tick']}")
    finally:
        turbo(c, 0)

    # (d5) the game thread's cost: the most one fill of the open page took, the demos above playing
    d = c.dump()
    print(f"wiki demos: (d5) the page cost the game thread at most {d['cost_max_ms']:.3f} ms in one frame "
          f"over {d['cost_frames']} frames (the plan's target is under 0.5 ms)")
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(d5) Esc did not close the window")


TRY_DEMOS = (("quickbuild", 2, "snap", 83), ("buildshapes", 3, "line", 60))   # (entry, its tree node,
                                         # the demo, the input before which the stage is grabbed)
TRY_KEYS = {"shift": "shift", "alt": "alt", "ctrl": "ctrl", "z": "z", "x": "x"}
SIDE_W = 128                             # the sidebar's column, in the stage's world units left of x = 0


def demo_inputs(entry, demo_id):
    return next(x for x in json.loads((TREE / f"art/wiki/entries/{entry}.json").read_text())["demos"]
                if x["id"] == demo_id)["inputs"]


def stage_fit(d):
    """tagpu_wikistage_fit on the stage the dump's hit map drew: the map's scale and the client
    pixel of its world origin, from the stage's canvas -- its `stage` hit -- and the frame's unit,
    as both of the game's threads fit it. The content is the map and, with the sidebar on (the
    dump's `side_h` rows), the 128-unit column left of it, as tall as the taller of the two."""
    x0, y0, x1, y1 = Wiki.hit(d, "stage")
    u, (mw, mh), cell, side = d["u"], d["map"], 16, d.get("side_h", 0)
    pad = 6 * u
    cx0, cw, ch = (-SIDE_W, SIDE_W + mw * cell, max(mh * cell, side)) if side > 0 else (0, mw * cell, mh * cell)
    s = min((x1 - x0 - 2 * pad) / cw, (y1 - y0 - 2 * pad) / ch)
    return s, x0 + (x1 - x0 - cw * s) / 2 - cx0 * s, y0 + (y1 - y0 - ch * s) / 2


def try_view(c, d, what):
    """Try it yourself as the game played it, read back through sim.js: the dump's state hash must
    equal simparity.mjs --try's -- sim.js fed the inputs the dump logged at the ticks it logged them
    -- and then tools/wiki/sideview.mjs --dump's view describes the game's simulation: the routed
    screen, its page and gadget states, the armed mode, the selection's orders."""
    assert not d["try_cut"], f"{what}: the dump's log of Try it was cut ({d['try_cut']} inputs)"
    dumpf = c.gamedir / "tagpu_wiki.try.json"
    dumpf.write_text(json.dumps(d))
    try:
        r = subprocess.run(["node", str(TREE / "tools/wiki/simparity.mjs"), "--try", str(dumpf)], capture_output=True,
                           text=True, timeout=300)
        assert r.returncode == 0, f"simparity --try: {r.stderr.strip()}"
        if r.stdout.strip() != d["hash"]:
            # kept for an offline replay: simparity.mjs --try <it>
            kept = c.gamedir / "tagpu_wiki.try.differ.json"
            kept.write_text(dumpf.read_text())
            raise AssertionError(f"{what}: the game's Try it hashes {d['hash']} at tick {d['tick']}, sim.js fed its inputs "
                                 f"{r.stdout.strip()}; the dump kept in {kept}")
        r = subprocess.run(["node", str(TREE / "tools/wiki/sideview.mjs"), "--dump", str(dumpf)], capture_output=True,
                           text=True, timeout=300)
        assert r.returncode == 0, f"sideview --dump: {r.stderr.strip()}"
    finally:
        dumpf.unlink(missing_ok=True)
    return json.loads(r.stdout)


def view_side(d, sv, what):
    """The layout side the game's wiki plays, from its state dump `d`: `ctx.side`, the side its simulation was made
    for, which on the mod's layout is the engine's own (`engine_side`, the controlled player's PlayerInfo+0x95). Its
    types hold the shown builder's pages and its GEN is the ORDERS tab's screen. The view `sv` is sideview.mjs's replay
    of the dump, made with the same ctx, so it must show that side too."""
    lay, ctx = d["layout"], d.get("ctx") or {}
    k = ctx.get("side")
    assert isinstance(k, int) and 0 <= k < len(lay["sides"]), f"{what}: the dump's ctx names no side of the layout: {ctx}"
    if d["layout_src"] == "mod":
        assert k == d["engine_side"], f"{what}: the wiki plays side {k} on the mod's layout, the engine's is {d['engine_side']}"
    assert sv and sv.get("index") == k, f"{what}: sim.js's replay shows side {sv and sv.get('index')}, the game side {k}: {sv}"
    return lay["sides"][k]


def side_by_prefix(lay, prefix, what):
    """The layout side whose screens carry `prefix`, the engine's own side prefix (main + 0x37F5B + side * 0x232, the
    one its MAIN2's file name starts with): the side the player plays in the running game."""
    hit = [sd for sd in lay["sides"] if sd["prefix"].upper() == prefix.upper()]
    assert len(hit) == 1, f"{what}: {len(hit)} sides of the layout have the prefix {prefix}: {[sd['name'] for sd in lay['sides']]}"
    return hit[0]


def side_centre(lay, screen, i):
    """Gadget i of a layout screen: its centre in the stage's world units, where the build trip
    presses it (sim.js centreOf)."""
    s = lay["screens"][screen]
    gx, gy, gw, gh = s["gadgets"][i]["rect"]
    return -SIDE_W + gx + gw / 2, s["y"] + gy + gh / 2


def side_gadget(lay, screen, pred, what):
    i = next((k for k, g in enumerate(lay["screens"][screen]["gadgets"]) if pred(g) and not g.get("hidden")), None)
    assert i is not None, f"{what}: {screen} has no such gadget"
    return i


class Stage:
    """The Try it stage's client geometry (stage_fit) and the presses given on its sidebar: each
    a move onto the gadget's centre, the left button down and up there -- a release inside, where
    the engine's buttons act (0x4A6AE0), a tab having acted on the press already. Every world
    point given is kept, so the log's moves can be checked against them."""

    def __init__(self, c, d):
        self.c = c
        self.s, self.ox, self.oz = stage_fit(d)
        self.points, self.want = [], []

    def client(self, x, z):
        return round(self.ox + x * self.s), round(self.oz + z * self.s)

    def press(self, lay, screen, i):
        x, z = side_centre(lay, screen, i)
        at = self.client(x, z)
        self.c.keys("dmove:%d,%d" % at, "ddown:%d,%d" % at, "dup:%d,%d" % at)
        self.points.append((x, z))
        self.want += [["down", "lmb"], ["up", "lmb"]]
        return at

    def view(self, what):
        return try_view(self.c, self.c.dump(), what)

    def arm(self, T, what):
        """T armed through the sidebar, as a player arms it: the pager pressed the nearer way round
        to T's page (a tie goes to NEXT, as the build trip goes), then T's button. The page reached
        and the mode armed are read back from the simulation."""
        v = self.view(what)
        d = self.c.dump()
        sv, lay = v["side"], d["layout"]
        types = view_side(d, sv, what)["types"]
        assert sv["type"] in types, f"{what}: the sidebar shows no builder's page: {sv}"
        pages = types[sv["type"]]["pages"]
        holds = [k for k, n in enumerate(pages, 1) if any(g["role"] == "build" and g.get("unit") == T
                                                          for g in lay["screens"][n]["gadgets"])]
        assert holds, f"{what}: no page of {sv['type']} holds {T}"
        n, page, p = len(pages), sv["page"], holds[0]
        k_next, k_prev = (p - page) % n, (page - p) % n
        role = "next" if k_next <= k_prev else "prev"
        for _ in range(min(k_next, k_prev)):
            self.press(lay, pages[page - 1], side_gadget(lay, pages[page - 1], lambda g: g["role"] == role, what))
            page = page % n + 1 if role == "next" else (page - 2) % n + 1
        v = self.view(what)
        assert v["side"]["screen"] == pages[p - 1] and v["side"]["page"] == p, \
            f"{what}: {min(k_next, k_prev)} x {role.upper()} did not reach page {p} ({pages[p - 1]}): {v['side']}"
        i = side_gadget(lay, pages[p - 1], lambda g: g["role"] == "build" and g.get("unit") == T, what)
        assert v["side"]["state"][i] == 0, f"{what}: {T}'s button on {pages[p - 1]} is not live: state {v['side']['state'][i]}"
        self.press(lay, pages[p - 1], i)
        v = self.view(what)
        assert v["mode"] == f"build:{T}", f"{what}: the release on {T}'s button armed {v['mode']}"
        return min(k_next, k_prev), role, p


def replay_try(c, d, entry, demo_id, grab_at=None, shot=None):
    """One demo's inputs (art/wiki/entries/<entry>.json) given to Try it yourself as device input:
    each world point turned into the client point the dump's stage shows it at (stage_fit
    inverted), a move as `dmove:`, the left button as `ddown:`/`dup:` there, the right as
    `down:rbutton`/`up:rbutton` where the pointer is, a key as `down:`/`up:`, a build as a player
    arms it on the stage's sidebar (Stage.arm), the pointer then brought back to the demo's point;
    captions are the demo's own and are skipped. With `grab_at`, the presented frame is grabbed to
    `shot` before that input is given. The Stage, holding what the simulation must have been given
    without its moves, and every world point given."""
    st = Stage(c, d)
    at = None
    pending, arms = [], []

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
            at = st.client(*ev["move"])
            st.points.append(tuple(ev["move"]))
            pending.append("dmove:%d,%d" % at)
        for kind in ("down", "up"):
            k = ev.get(kind)
            if k is None:
                continue
            st.want.append([kind, k])
            if k in ("lmb", "rmb"):
                assert at, f"{entry}/{demo_id}: a button before any move"
                pending.append(("ddown:%d,%d" if kind == "down" else "dup:%d,%d") % at if k == "lmb"
                               else f"{kind}:rbutton")
            else:
                pending.append(f"{kind}:{TRY_KEYS[k]}")
        if "build" in ev:
            flush()
            arms.append((ev["build"], *st.arm(ev["build"], f"(t2) {entry}/{demo_id}: {ev['build']}")))
            if at:
                pending.append("dmove:%d,%d" % at)
    flush()
    return st, arms


def side_controls(c, d):
    """(t2c) the sidebar's other controls, on the ARMCK the last demo left armed and ordered: NEXT
    and PREV page it, ORDERS shows the side's GEN and BUILD its page again (tabs act on the press),
    and STOP disarms and empties the orders; each read back from the simulation, whose hash equals
    the game's at every read."""
    st = Stage(c, d)
    lay = d["layout"]
    v = st.view("(t2c)")
    sv = v["side"]
    shown = view_side(d, sv, "(t2c)")
    pages = shown["types"][sv["type"]]["pages"]
    n, page, screen = len(pages), sv["page"], sv["screen"]
    gen = shown["gen"]
    steps = []
    for role, want in (("next", page % n + 1), ("prev", page)):
        st.press(lay, screen, side_gadget(lay, screen, lambda g: g["role"] == role, "(t2c)"))
        v = st.view(f"(t2c) {role.upper()}")
        assert v["side"]["page"] == want and v["side"]["screen"] == pages[want - 1], \
            f"(t2c) {role.upper()} on {screen} did not show page {want}: {v['side']}"
        screen = v["side"]["screen"]
        steps.append(f"{role.upper()} -> {screen}")
    for role, want in (("orders", gen), ("buildtab", screen)):
        st.press(lay, v["side"]["screen"], side_gadget(lay, v["side"]["screen"], lambda g: g["role"] == role, "(t2c)"))
        v = st.view(f"(t2c) {role}")
        assert v["side"]["screen"] == want, f"(t2c) the {role} tab did not show {want}: {v['side']}"
        steps.append(f"{role} -> {want}")
    before = {u: len(o) for u, o in v["orders"].items()}
    assert v["mode"] != "contextual" and any(before.values()), \
        f"(t2c) STOP needs a building armed and an order to clear: mode {v['mode']}, orders {before}"
    st.press(lay, screen, side_gadget(lay, screen, lambda g: g["role"] == "stop", "(t2c)"))
    v = st.view("(t2c) STOP")
    assert v["mode"] == "contextual" and not any(v["orders"].values()), \
        f"(t2c) STOP left mode {v['mode']} and orders {v['orders']}"
    steps.append(f"STOP: {before} orders and the armed building -> none")
    d = c.dump()
    got = [[k, a] for _, k, a, *_ in d["try_log"] if k != "move"]
    assert got[-len(st.want):] == st.want, f"(t2c) the game gave its simulation {got[-len(st.want):]}, the presses {st.want}"
    print(f"wiki try: (t2c) on the sidebar: {'; '.join(steps)}; tick {d['tick']}, hash {d['hash']} equal to sim.js's")
    return d


def one_cursor(c, d):
    """(t2b) Try it shows exactly one cursor: with the pointer over a gadget of the stage's sidebar,
    one, on the stage, and over the stage's centre one, on the stage -- the stage's, the player's
    hidden; over the page's play control, off the stage, the player's alone. Found against the
    game's own frames (tools/wiki/gfx.py's pack), as cursor's (c2)."""
    import numpy as np
    from PIL import Image
    frames = sprite_frames()
    x0, y0, x1, y1 = c.hit(d, "stage")
    st = Stage(c, d)
    v = st.view("(t2b)")
    lay = d["layout"]
    gx, gz = side_centre(lay, v["side"]["screen"],
                         side_gadget(lay, v["side"]["screen"], lambda g: g["role"] == "buildtab", "(t2b)"))
    path = c.gamedir / "tagpu_wiki.cursor.png"
    for where, at, on_stage in (("the sidebar's BUILD tab", st.client(gx, gz), 1),
                                ("the stage", ((x0 + x1) // 2, (y0 + y1) // 2), 1),
                                ("the play control", c.centre(c.hit(d, "play")), 0)):
        c.keys("dmove:%d,%d" % at)
        time.sleep(1.0)
        grab(c, path)
        img = np.asarray(Image.open(path).convert("RGB"))
        hits = [(name, p) for name, _f, rgba in frames for p in find_sprite(img, rgba)]
        on = [h for h in hits if x0 <= h[1][0] < x1 and y0 <= h[1][1] < y1]
        assert len(hits) == 1 and len(on) == on_stage, \
            f"(t2b) the pointer over {where} at {at}: the frame holds {hits}, {len(on)} on the stage"
        print(f"wiki try: (t2b) the pointer over {where}: one cursor, {hits[0][0]} at {hits[0][1]}, "
              f"{'on' if on_stage else 'off'} the stage")
    path.unlink(missing_ok=True)


def try_it(c, shot=None):
    """(t1)..(t5): Try it yourself driven by device input on the stage and its sidebar, its
    simulation against sim.js fed what the game logged."""
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
        d = wait_dump(c, lambda d: d["try"] and d["sim"] and d["side_h"] > 0, "(t1) Try it yourself did not take the stage")
        v = try_view(c, d, "(t1)")
        sv = v["side"]
        types = view_side(d, sv, "(t1)")["types"]
        assert sv["page"] == 1 and sv["type"] in types and sv["screen"] == types[sv["type"]]["pages"][0], \
            f"(t1) {entry}/{demo}: Try it's sidebar is not its builder's first page: {sv}"
        print(f"wiki try: (t1) {entry}/{demo}: Try it took the stage, its sidebar the {d['layout_src']} layout's "
              f"{sv['screen']} ({sv['type']} page 1 of {len(types[sv['type']]['pages'])}), "
              f"{d['side_h']} rows")

        shot_n = shot and Path(shot).with_name(f"{Path(shot).stem}-{entry}-{demo}{Path(shot).suffix}")
        if shot:
            # the control for the hidden cursor: the pointer over a page control, off the stage
            c.keys("dmove:%d,%d" % c.centre(c.hit(d, "play")))
            time.sleep(1.0)
            grab(c, Path(shot).with_name(f"{Path(shot).stem}-{entry}-{demo}-page{Path(shot).suffix}"))
        st, arms = replay_try(c, d, entry, demo, grab_at, shot_n)
        press(c, wait_dump(c, lambda d: d["playing"], "(t2) Try it is not playing"), "play")
        d = wait_dump(c, lambda d: not d["playing"], "(t2) play did not pause Try it")
        got = [[k, a] for _, k, a, *_ in d["try_log"] if k != "move"]
        assert got == st.want, f"(t2) {entry}/{demo}: the game gave its simulation {got}, the demo and the sidebar {st.want}"
        moves = [(x, z) for _, k, x, z in (e for e in d["try_log"] if e[1] == "move")]
        far = [m for m in moves if min(math.hypot(m[0] - p[0], m[1] - p[1]) for p in st.points) > 0.75 / st.s]
        assert not far, f"(t2) {len(far)} of {len(moves)} moves landed more than a pixel off the points given: {far[:4]}"
        try_view(c, d, f"(t2) {entry}/{demo}")
        print(f"wiki try: (t2) {entry}/{demo} replayed through the stage: {len(d['try_log'])} inputs "
              f"({len(moves)} moves, every one within a pixel of its point), "
              + ", ".join(f"{T} armed on page {p} after {k} x {role.upper()}" for T, k, role, p in arms)
              + f"; tick {d['tick']}, hash {d['hash']} equal to sim.js's")
        press(c, d, "play")
        d = wait_dump(c, lambda d: d["playing"], "(t2) play did not start Try it again")

    d = side_controls(c, d)
    one_cursor(c, d)

    # (t3) a button and a key held when Try it is left: both let go in the simulation, and their late
    # releases are taken -- nothing reaches the engine
    x0, y0, x1, y1 = c.hit(d, "stage")
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    c.keys("down:shift", f"ddown:{cx},{cy}")
    d = wait_dump(c, lambda d: d["try_down"] == ["shift", "lmb"], "(t3) the held Shift and left button are not in Try it's set")
    press(c, d, "try")
    d = wait_dump(c, lambda d: not d["try"], "(t3) Back to demo did not return the demo")
    assert d["try_down"] == [], f"(t3) leaving Try it left {d['try_down']} held"
    c.keys("up:shift", f"dup:{cx},{cy}")
    d = c.dump()
    assert not d["owed_keys"] and not d["owed_buttons"], f"(t3) owed after the late releases: {d['owed_keys']} {d['owed_buttons']}"
    print("wiki try: (t3) Back to demo let Shift and the left button go; their late releases were taken")

    # (t4) held when the window closes (Esc): released, and nothing owed once let go
    press(c, d, "try")
    d = wait_dump(c, lambda d: d["try"] and d["sim"], "(t4) Try it did not take the stage again")
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
# (entry, demo, ticks): the mockup's sim (tools/wiki/mockup/sim.js) puts a nanoframe in Quick Build 3 at
# 267..287, the reclaim in Quick Build 8 at 171..180 and the upgrade's nanoframe after it, the gesture's
# ghosts in Build Lines 2 and 4 from tick 0, Quick Build 9's ARMCK walking toward +x at 164..233, and Reclaim 3's
# field of wrecks (3D features through the unit pass) all nine standing at 300 and two of them reclaimed by 500
UNITS_SHOTS = (("quickbuild", 3, (180, 277)), ("quickbuild", 8, (120, 175, 300)),
               ("buildshapes", 2, (100, 350)), ("buildshapes", 4, (100, 350)), ("quickbuild", 9, (170,)),
               ("reclaim", 3, (300, 500)))
FACING = ("quickbuild", 9, 170)
NOUNIT = ("quickbuild", 3, 180)
# the stage's frame count (tagpu_wikiworld.c, THE COUNT): COUNT_FRAMES drawn frames from the first at rest
COUNT_FRAMES = 60
COUNTED = (r"wiki3d: count (\d+): (\d+) of (\d+) frame\(s\) drew the image, frames \d+\.\.\d+; the work: (\d+) ground\(s\), "
           r"(\d+) type record set\(s\), (\d+) bake\(s\), (\d+) drop\(s\), (\d+) frame\(s\) with a record the bake refused; "
           r"[\d.]+ model record\(s\) a frame; render thread: the producer ([\d.]+) ms and the passes ([\d.]+) ms a frame "
           r"on average, the passes at most ([\d.]+) ms")


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
    """The stage's next COUNT_FRAMES drawn frames from the first at rest, counted by the DLL on the
    trigger tagpu_wiki3d.count: (drew, of, producer ms, passes ms, max ms). Each frame drew the image
    (want_all), or none did (False), or either (None: the caller judges); and the stage did no work over them -- no ground or type records built,
    nothing baked or dropped, no record the bake refused: the stage's cost judged by its work, its
    milliseconds only said. A stage that works on every frame never comes to rest, and the count is
    never answered."""
    start = c.mark()
    trigger(c.gamedir / "tagpu_wiki3d.count", f"{COUNT_FRAMES}\n")
    asked = wait_line(c, start, r"wiki3d: count (\d+ asked|: )|wiki3d: count: ", f"{what}: the count", 60)
    assert " asked: " in asked, f"{what}: {asked}"
    n = re.search(r"count (\d+) asked", asked).group(1)
    m = re.search(COUNTED, wait_line(c, start, rf"wiki3d: count {n}: ", f"{what}: the stage never came to rest", 180))
    assert m and m.group(1) == n, f"{what}: the count's line does not read as one"
    drew, of = int(m.group(2)), int(m.group(3))
    work = dict(zip(("ground(s) built", "type record set(s) built", "bake(s)", "drop(s)", "frame(s) refused"),
                    (int(m.group(k)) for k in range(4, 9))))
    assert want_all is None or ((drew == of) if want_all else (drew == 0)), f"{what}: {drew} of {of} frame(s) drew the image"
    assert not any(work.values()), f"{what}: the stage worked over {of} frames at rest: " + \
        ", ".join(f"{v} {k}" for k, v in work.items() if v)
    return drew, of, float(m.group(9)), float(m.group(10)), float(m.group(11))


def shoot(c, shots, res, entry, demo, tick, ss, name=None, window=True):
    """One capture of the wiki's target through tagpu_wiki.shot, copied into `shots` (as
    `<entry>-d<demo>-t<tick>-ss<ss>.game.ppm` unless `name`), with the presented frame grabbed beside
    it; the capture's `armed` line. `ss` None takes the capture at the Supersampling setting's ss."""
    ppm = c.gamedir / "tagpu_wiki3d_shot.ppm"
    start = c.mark()
    ppm.unlink(missing_ok=True)
    trigger(c.gamedir / "tagpu_wiki.shot", f"{entry} {demo} {tick}\n")
    asked = wait_line(c, start, r"wiki: shot \d+ asked|wiki: tagpu_wiki\.shot: ", f"{entry} {demo} {tick}")
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


class Stage3dCheck:
    """tools/wiki/stage3dcheck.py over DIR, started once the captures are written and judged at `end`: its
    mockup renders on the GPU while the game goes on to its next steps, which write no `*.game.ppm`
    there. A mode that fails before `end` ends it too."""
    def __init__(self, shots):
        self.p = subprocess.Popen([sys.executable, str(TREE / "tools/wiki/stage3dcheck.py"), str(shots), "--out",
                                   str(shots / "check")], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def end(self):
        """(exit code, its stdout, the tail of its stderr)"""
        out, err = self.p.communicate(timeout=3600)
        return self.p.returncode, out.rstrip(), err.strip()[-600:]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


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
    with Stage3dCheck(shots) as check:
        stage3d_levers(c, shots, res, gamedir, lever)
        code, out, err = check.end()
    print(out)
    assert code == 0, f"(s3) stage3dcheck exited {code}: {err}"


def stage3d_levers(c, shots, res, gamedir, lever):
    """(s4), (s5): the levers, then the stage at rest; Esc"""
    # the levers on quickbuild, whose stage has features (buildshapes' has none, so `nofeat` would
    # withhold nothing the image expects there)
    start = c.mark()
    trigger(gamedir / "tagpu_wiki.shot", "quickbuild 1 30\n")
    wait_line(c, start, r"wiki3d: shot \d+: quickbuild", "(s4) quickbuild was not drawn before the levers")
    try:
        for tok in ("nomap", "nofeat"):
            start = c.mark()
            trigger(lever, tok)
            wait_line(c, start, rf"wiki3d: tagpu_wiki3d\.on: {tok}", f"(s4) the lever {tok} was not read")
            got = drawn_window(c, f"(s4) under {tok}", want_all=False)
            grab(c, shots / f"{res}-lever-{tok}.window.png")
            print(f"wiki stage3d: (s4) {tok}: {got[0]} of {got[1]} frames drew the image - the whitebox whole")
    finally:
        gone = c.mark()
        lever.unlink(missing_ok=True)
    wait_line(c, gone, r"wiki3d: tagpu_wiki3d\.on: the 3D stage on", "(s4) the lever's end was not read")
    got = drawn_window(c, "(s4) the lever gone", want_all=True)
    print(f"wiki stage3d: (s4) the lever gone: {got[0]} of {got[1]} frames drew the image")
    print(f"wiki stage3d: (s5) at rest: nothing built, baked or dropped; render thread: the producer {got[2]:.3f} ms "
          f"and the passes {got[3]:.3f} ms a frame on average, the passes at most {got[4]:.3f} ms ({res})")
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
        trigger(c.gamedir / "tagpu_wiki.shot", f"{entry} {demo} {tick}\n")
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
    with Stage3dCheck(shots) as check:
        units_rest(c, shots, res, gamedir, lever)
        code, out, err = check.end()
    print(out)
    # (u3)'s verdict is given after (u4)..(u7), so one run reports every step
    assert code == 0, f"(u3) stage3dcheck exited {code}: {err}"


def units_rest(c, shots, res, gamedir, lever):
    """(u4)..(u7), while (u3)'s check runs"""
    f = sim_facing(*FACING, "ARMCK")
    assert f["hx"] > 0.9 and f["dx"] > 0, f"(u4) the ARMCK at {FACING} is not walking toward +x: {f}"
    print(f"wiki units: (u4) {FACING[0]} demo {FACING[1]} tick {FACING[2]}: the ARMCK walks ({f['dx']:.3f}, {f['dz']:.3f}) "
          f"a tick facing ({f['hx']:.3f}, {f['hz']:.3f}); its capture is {FACING[0]}-d{FACING[1]}-t{FACING[2]}, "
          f"against the mockup's in (u3)")

    try:
        start = c.mark()
        trigger(lever, "nounit=ARMCK")
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
        gone = c.mark()
        lever.unlink(missing_ok=True)
    wait_line(c, gone, r"wiki3d: tagpu_wiki3d\.on: the 3D stage on", "(u6) the lever's end was not read")
    got = drawn_window(c, "(u6) the lever gone", want_all=True)
    print(f"wiki units: (u6) at rest over {got[1]} frames: nothing built, baked or dropped; render thread: the producer "
          f"{got[2]:.3f} ms and the passes {got[3]:.3f} ms a frame on average, the passes at most {got[4]:.3f} ms ({res})")
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(u6) Esc did not close the window")
    reclaim(c, shots, res)


def unit_state(c, kind):
    """A unit's build fraction remaining (+0x104, float) and HP (+0x108, word), read at one instant."""
    b = c.bytes(f"0x{c.address(kind) + 0x104:x}:x6")
    frac, hp = struct.unpack("<fH", b)
    return frac, hp


# (u7)'s bound on the game's own clock, past which the reads stop: past the 7134 ticks 240 s of 1x ran
# (MEASURED 2026-10-30, place-basic.json); a game whose clock stands RECLAIM_STALL_S fails as hung
RECLAIM_TICKS = 12000
RECLAIM_STALL_S = 60


def reclaim(c, shots, res):
    """(u7): the engine's own reclaim, read as it runs, at the game's top speed (TA's `+`, ten
    steps: twice the normal tick) and bounded on GameTime."""
    ck, solar = c.units["ARMCK"], c.units["ARMSOLAR"]
    frac, hp = unit_state(c, "ARMSOLAR")
    assert frac == 0.0 and hp > 0, f"(u7) the solar is not a finished unit before the reclaim: {frac}, {hp}"
    sx, sz = 1600, 5800
    c.settle_eye(sx - 500, sz - 400)
    c.keys(*["plus"] * 10)
    try:
        tacli("order", c.name, "reclaim", "unit", solar, "--unit", ck, "--expect", "ARMCK", "--expect-target",
              "ARMSOLAR", timeout=60)
        rows, grabbed, hp0 = [], False, hp
        t0 = c.gametime()
        last, moved = t0, time.monotonic()
        while True:
            try:
                frac, hp = unit_state(c, "ARMSOLAR")
                alive = c.values(f"0x{c.address('ARMSOLAR') + 0x110:x}")[0] != 0
            except AssertionError:
                break
            now = c.gametime()
            rows.append((now, frac, hp))
            if not grabbed and 0 < hp < hp0 * 0.6:
                grab(c, shots / f"{res}-reclaim-live.window.png")
                grabbed = True
            # gone: HP at 0, or the word past where a finished unit's full HP started -- under a reclaim it only
            # falls, so a larger one has passed below zero (MEASURED 2026-10-30: 326 read 65534 at the end)
            if hp == 0 or hp > hp0 or not alive or (len(rows) > 3 and rows[-1][2] == rows[-4][2] == 0) \
                    or now - t0 > RECLAIM_TICKS:
                break
            if now != last:
                last, moved = now, time.monotonic()
            assert time.monotonic() - moved < RECLAIM_STALL_S, f"(u7) GameTime stood at {now} for {RECLAIM_STALL_S} s"
            time.sleep(0.5)
    finally:
        c.keys(*["minus"] * 10)
    fell = [r for r in rows if r[2] < hp0]
    assert fell, f"(u7) the solar's HP never fell under the reclaim over GameTime {t0}..{rows[-1][0] if rows else t0}: {rows[:5]}"
    nano = [r for r in rows if r[1] != 0.0]
    print(f"wiki units: (u7) the ARMCK reclaimed the ARMSOLAR: HP {hp0} -> {rows[-1][2]} over GameTime "
          f"{rows[0][0]}..{rows[-1][0]} ({len(rows)} reads); the build fraction remaining (+0x104) read "
          f"{'0.0 at every read' if not nano else f'non-zero at {len(nano)} reads, e.g. {nano[0]}'}")
    assert not nano, "(u7) the engine staged the reclaimed unit as a nanoframe"



# ---- watch: a frame:game entry on the page, Watching (research/notes/ingame-wiki-replay-plan.md; the presentation
# spec's 6.5) ----

WATCH = "watching"
WATCH_NODE = 7                                 # entry:7: the tree's Replays > Watching (art/wiki/tree.json)
WATCH_DEMOS = ("play", "speed", "rewfwd", "track", "markers", "bookmarks", "leave")
WATCH_PAGE = ("drawer", "play", "replay", "x1", "x2", "try", "about")
# (v3) each demo held at a tick it shows its point at, no seek pending there (a capture then shows no world): play
# paused, speed at 2x, fwd's seek done, the track let go, Bram's commander gone, the bookmark's toast, the question
WATCH_SHOTS = (("play", 150), ("speed", 120), ("rewfwd", 100), ("track", 180), ("markers", 130), ("bookmarks", 90),
               ("leave", 201))     # leave at 201: Leave's question up, No held (replay.js: held 199..204)
WATCH_PAIRED = ("play", "track", "bookmarks", "leave")    # (v3) beside the mockup at the same state
HIT_PAGE, HIT_HUD, HIT_DIALOG = 0, 1, 2        # TAGPU_CMD_WIKIHIT.src (tagpu_wikiframe.h WIKIHIT_*)
S_LEFT = "In the game, this goes back to the Replays browser."    # tagpu_wikiframe.h WIKIFRAME_LEFT
FRAME_KEYS = ("playhead", "nmark", "toast", "question", "held", "cur")   # (v2) the record's fields compared


def hit_of(d, key, src):
    """The drawn frame's control `key` of source `src` (HIT_*): the same key can be the page's and the stage HUD's"""
    got = [h["rect"] for h in d["hits"] if h["key"] == key and h.get("src", HIT_PAGE) == src]
    assert got, f"the last frame drawn has no '{key}' control of source {src}: " \
                f"{[(h['key'], h.get('src')) for h in d['hits']]}"
    return got[0]


def frame_rects(d):
    """A drawn game frame's parts in client pixels [x0, y0, x1, y1), from the dump's frame_fit (the render thread's
    fit of the same canvas): the whole frame, its column, its top bar and its bottom bar (stage.js frame mode: the
    column left of the map's origin, the bars from it to the frame's right edge), each edge snapped as the stage
    snaps it."""
    f = d["frame_fit"]
    cx, cy, s, ox, oz, Wg, Hg = *f["canvas"], f["s"], f["ox"], f["oz"], f["Wg"], f["Hg"]
    snap = lambda v: math.floor(v + 0.5 + 1 / 64)
    X = lambda wx: cx + snap(ox + wx * s)
    Z = lambda wz: cy + snap(oz + wz * s)
    return {"frame": [X(-128), Z(-32), X(Wg - 128), Z(Hg - 32)], "column": [X(-128), Z(-32), X(0), Z(Hg - 32)],
            "top": [X(0), Z(-32), X(Wg - 128), Z(0)], "bottom": [X(0), Z(Hg - 64), X(Wg - 128), Z(Hg - 32)],
            "world": [X(0), Z(0), X(Wg - 128), Z(Hg - 64)]}


def watch_open(c):
    """The wiki opened on Watching, drawn as a game frame: picked from the tree, or already the selection, which a
    reopened window keeps (its tree is then in the closed drawer)"""
    d = c.open_wiki(expect_pause=True)
    d = wait_dump(c, lambda d: d["drawn_open"], "(v1) the page was not drawn")
    if d["entry"] != WATCH:
        c.keys("dclick:%d,%d" % c.centre(c.hit(d, f"entry:{WATCH_NODE}")))
    return wait_dump(c, lambda d: d["entry"] == WATCH and d["drawn_entry"] >= 0 and d.get("drawn_frame")
                     and any(h.get("src") == HIT_HUD for h in d["hits"]),
                     "(v1) entry:7 did not draw Watching's game frame with its stage HUD", seconds=60)


def watch_v1(c, d):
    """(v1) the theatre's controls, the stage HUD's inside the stage, and About's and the drawer's over everything"""
    keys = {(h["key"], h.get("src", HIT_PAGE)) for h in d["hits"]}
    want = [k for k in WATCH_PAGE + tuple(f"demo:{i}" for i in range(len(WATCH_DEMOS))) if (k, HIT_PAGE) not in keys]
    assert not want, f"(v1) the theatre has no {want}: {sorted(keys)}"
    st = hit_of(d, "stage", HIT_PAGE)
    hud = [h for h in d["hits"] if h.get("src") == HIT_HUD]
    out = [h["key"] for h in hud if not (st[0] <= h["rect"][0] and h["rect"][2] <= st[2] and st[1] <= h["rect"][1]
                                         and h["rect"][3] <= st[3])]
    assert not out, f"(v1) stage HUD controls outside the stage {st}: {out}"
    assert {"play", "track", "leave"} <= {h["key"] for h in hud}, f"(v1) the stage HUD lacks play, track or leave: " \
                                                                 f"{sorted(h['key'] for h in hud)}"
    press(c, d, "about")
    d = wait_dump(c, lambda d: d["about"] and d["hits"] and d["hits"][0]["key"] == "scroll:text",
                  "(v1) About did not put its text first in the hit map")
    press(c, d, "drawer")
    d = wait_dump(c, lambda d: d["drawer"] and not d["about"] and d["hits"] and d["hits"][0]["key"].startswith("entry:"),
                  "(v1) the drawer did not put its entries first in the hit map, or left About open")
    press(c, d, "drawer")
    d = wait_dump(c, lambda d: not d["drawer"] and not d["about"], "(v1) the drawer did not close")
    print(f"wiki watch: (v1) the theatre: {len(WATCH_PAGE) + len(WATCH_DEMOS)} page controls, {len(hud)} stage HUD "
          f"controls inside the stage; About's text and then the drawer's entries topmost")
    return d


def simparity_frames(d, asks):
    """(v2) replay.js's hash and frame record at each (demo, tick) of `asks` in the dump's context: its layout, side,
    interface type, the pack of a mod's layout and the units' scripts (simparity.mjs --hash --frame)"""
    lay = TREE / ".wiki-watch-layout.json"
    lay.write_text(json.dumps(d["layout"]))
    try:
        args = ["--layout", str(lay) if d["layout"] is not None else "null", "--side", str(d["ctx"]["side"]),
                "--iface", str(d["ctx"]["iface"])]
        if d["layout_src"] == "mod":
            args += ["--pack", str(pack_for_layout(lay))]
        if d["ctx"]["cob"]:
            args.append("--scripts")
        r = subprocess.run(["node", str(TREE / "tools/wiki/simparity.mjs"), "--hash", *args, "--frame",
                            ",".join(f"{WATCH}/{k}@{t}" for k, t in asks)], capture_output=True, text=True, timeout=900)
    finally:
        lay.unlink(missing_ok=True)
    assert r.returncode == 0, f"(v2) simparity --hash --frame: {r.stderr.strip()[-600:]}"
    out = {}
    for ln in r.stdout.splitlines():
        m = re.match(r"frame (\S+?)/(\S+?)@(\d+) ([0-9a-f]{16}) (.*)$", ln)
        if m:
            out[(m.group(2), int(m.group(3)))] = (m.group(4), json.loads(m.group(5)))
    return out


def watch_v2(c):
    """(v2) each demo paused at its trips' presses: the frame record the game thread placed equals replay.js's at
    that demo tick, and so does the whole replay's hash"""
    entry = json.loads((TREE / f"art/wiki/entries/{WATCH}.json").read_text())
    # a trip's press is HUD_DOWN (WIKI_PICK_DOWN, 20) demo ticks after it starts
    asks = [(dm["id"], ev["at"] + 20) for dm in entry["demos"] for ev in dm["inputs"] if "hud" in ev]
    got = {}
    turbo(c, 8)
    try:
        for k, t in asks:
            n = WATCH_DEMOS.index(k) + 1
            start = c.mark()
            trigger(c.gamedir / "tagpu_wiki.pause", f"{WATCH} {n} {t}\n")
            asked = wait_line(c, start, r"wiki: (pause \d+ asked|tagpu_wiki\.pause: )", f"(v2) {k}@{t}")
            assert re.search(r" asked: .*\d$", asked), f"(v2) {asked}"
            pn = re.search(r"pause (\d+) asked", asked).group(1)
            wait_line(c, start, rf"wiki: pause {pn}: ", f"(v2) {k} did not pause at tick {t}", 120)
            d = wait_dump(c, lambda d: d["demo"] == k and not d["playing"] and d.get("replay"),
                          f"(v2) {k} is not the paused replay")
            assert d["tick"] == t, f"(v2) {k} paused at tick {d['tick']}, not {t}"
            got[(k, t)] = (d["hash"], d["frame"])
    finally:
        turbo(c, 0)
    want = simparity_frames(d, asks)
    for key, (h, rec) in got.items():
        assert key in want, f"(v2) simparity gave no frame for {key}"
        wh, wrec = want[key]
        bad = [f for f in FRAME_KEYS if f != "nmark" and rec[f] != wrec[f]] + \
              (["nmark"] if rec["nmark"] != wrec["nmark"] else [])
        assert not bad, f"(v2) {key}: the record's {bad} differ: game {[rec[f] for f in bad]}, replay.js {[wrec[f] for f in bad]}"
        assert h == wh, f"(v2) {key}: the replay's hash {h}, replay.js's {wh}"
    print(f"wiki watch: (v2) {len(got)} trips' presses over {len(WATCH_DEMOS)} demos: each record's "
          f"{', '.join(FRAME_KEYS)} and the replay's hash equal replay.js's")


def watch_shot(c, demo, tick):
    """A demo held at a tick through tagpu_wiki.shot (stepped there in one fill, as the mockup's capture is), once
    the game frame drawn is that one's: the dump"""
    n = WATCH_DEMOS.index(demo) + 1
    start = c.mark()
    trigger(c.gamedir / "tagpu_wiki.shot", f"{WATCH} {n} {tick}\n")
    asked = wait_line(c, start, r"wiki: shot \d+ asked|wiki: tagpu_wiki\.shot: ", f"(v3) {demo} {tick}")
    assert " asked: " in asked and asked.endswith(f"held at tick {tick}"), f"(v3) {asked}"
    d = wait_dump(c, lambda d: d["demo"] == demo and not d["playing"] and d.get("frame_fit"),
                  f"(v3) {demo} at {tick} was not drawn as a frame", seconds=60)
    time.sleep(1.0)          # the held frame presented
    return d


def region_diff(a, b, r, texels=None, skip=None, over=()):
    """(pixels that differ, pixels compared, the largest channel difference) over client rect r of two captures. With
    `texels` (tw, th), r shows a picture of tw x th texels sampled nearest: a pixel whose centre lies exactly on the
    edge between two texels may show either, so there it may equal b's pixel on either side of that edge. The GPU's
    sampler and the browser's round apart there: MEASURED 2026-10-31 on Watching at 1920 x 1080, one client column of
    the column and one row of the top bar, each centred on an edge, and no other pixel of either. `skip` (x0, x1):
    client columns left out; `over`, client rects [x0, y0, x1, y1) of what is drawn over the picture, each left out
    with the pixel round it, where its edge's filtered art blends into the picture."""
    import numpy as np
    x0, y0, x1, y1 = r
    A, B = a[y0:y1, x0:x1, :3].astype(int), b[y0:y1, x0:x1, :3].astype(int)
    if A.size == 0 or A.shape != B.shape:
        return 0, 0, 0
    ok = np.all(A == B, axis=2)
    if texels:
        eps = 1e-6
        for axis, n, t in ((1, x1 - x0, texels[0]), (0, y1 - y0, texels[1])):
            u = (np.arange(n) + 0.5) * t / n
            edge = np.abs(u - np.round(u)) < eps
            for k in (-1, 1):
                Bk = np.roll(B, -k, axis=axis)
                eq = np.all(A == Bk, axis=2)
                ok |= eq & (edge[None, :] if axis == 1 else edge[:, None])
    if skip:
        ok[:, max(0, skip[0] - x0):max(0, skip[1] - x0)] = True
    for q in over:
        ok[max(0, q[1] - 1 - y0):max(0, q[3] + 1 - y0), max(0, q[0] - 1 - x0):max(0, q[2] + 1 - x0)] = True
    dif = np.abs(A - B).max(axis=2)
    return int((~ok).sum()), int(ok.size), int(dif[~ok].max()) if (~ok).any() else 0


def watch_v3(c, shots, res):
    """(v3) each demo held at its tick and the presented frame grabbed; four of them beside the mockup at the same
    state (the game look, the stock pack): the column and both bars pixel-equal, the world and the HUD over it
    measured"""
    import numpy as np
    shots.mkdir(parents=True, exist_ok=True)
    for demo, tick in WATCH_SHOTS:
        d = watch_shot(c, demo, tick)
        base = shots / f"{res}-{WATCH}-{demo}-t{tick}"
        grab(c, base.with_suffix(".game.png"))
        base.with_suffix(".frame.json").write_text(json.dumps({"frame_fit": d["frame_fit"], "rects": frame_rects(d),
                                                               "hud": [h for h in d["hits"] if h.get("src") == HIT_HUD]}))
        if demo not in WATCH_PAIRED:
            continue
        at = mockup_shot({"res": [int(v) for v in res.split("x")], "entry": WATCH, "demo_n": WATCH_DEMOS.index(demo),
                          "tick": tick, "mod": "stock", "look": "game"}, base.with_suffix(".mockup.png"))
        g, m = png_rgba(base.with_suffix(".game.png")), png_rgba(base.with_suffix(".mockup.png"))
        R, f = frame_rects(d), d["frame_fit"]
        hud = [h["rect"] for h in d["hits"] if h.get("src") == HIT_HUD]   # the stage HUD's controls, over the bars
        # and the viewer's cursor, the game's sprite of at most HUD_SPRITE_CELL (64) texels about its point
        if at:
            hud.append([round(at[0]) - 64, round(at[1]) - 64, round(at[0]) + 64, round(at[1]) + 64])
        said = []
        # Impure's emblems on the top bar, frame x Wg - 75 .. Wg - 17 (the presentation spec's 3.4): kit parts each
        # renderer scales with its own filter, measured with the HUD, not held to the bars' equality
        cx, s_, ox = f["canvas"][0], f["s"], f["ox"]
        emb = [cx + math.floor(ox + (f["Wg"] - 128 - k) * s_ + 0.5 + 1 / 64) for k in (76, 16)]
        # the column one texel a game pixel from the frame's top (HUD_TEX_SIDE), each bar Wg - 128 x 32 (HUD_TEX_BAR)
        for part, tex in (("column", (128, f["Hg"])), ("top", (f["Wg"] - 128, 32)), ("bottom", (f["Wg"] - 128, 32))):
            n, of, mx = region_diff(g, m, R[part], tex, emb if part == "top" else None, hud)
            assert of > 0, f"(v3) {demo}@{tick}: the {part} rect {R[part]} is empty"
            assert n == 0, f"(v3) {demo}@{tick}: {n} of {of} pixels of the {part} differ from the mockup's (up to {mx})"
            said.append(f"{part} {of} px equal")
        n, of, mx = region_diff(g, m, [emb[0], R["top"][1], emb[1], R["top"][3]])
        said.append(f"the emblems {n} of {of} px differ (up to {mx})")
        # Leave's question: drawn in the game whenever the record has it open, measured beside the mockup's
        dlg = [h for h in d["hits"] if h.get("src") == HIT_DIALOG]
        if d["frame"].get("question"):
            box = next((h["rect"] for h in dlg if h["key"] == "dialog"), None)
            assert box and {"yes", "no"} <= {h["key"] for h in dlg}, \
                f"(v3) {demo}@{tick}: the record has Leave's question open but the game drew no dialog: {dlg}"
            n, of, mx = region_diff(g, m, box)
            said.append(f"the question (held {d['frame'].get('held')!r}) {n} of {of} px differ (up to {mx})")
        n, of, mx = region_diff(g, m, R["world"])
        said.append(f"world and HUD {n} of {of} px differ (up to {mx})")
        print(f"wiki watch: (v3) {demo} at tick {tick}: {'; '.join(said)}")
    print(f"wiki watch: (v3) {len(WATCH_SHOTS)} demos grabbed to {shots}")


def watch_v4(c):
    """(v4) Try it on demo 1 through the stage HUD's controls, device clicks at their centres"""
    def hud_click(d, key):
        c.keys("dclick:%d,%d" % c.centre(hit_of(d, key, HIT_HUD)))

    def settled(what, pred=lambda d: True):
        return wait_dump(c, lambda d: d.get("replay") and d["replay"]["target"] < 0 and d.get("drawn_frame") and pred(d),
                         what, seconds=60)
    gt0, ring0 = c.gametime(), c.ring()
    d = c.dump()
    press(c, d, "demo:0")
    d = wait_dump(c, lambda d: d["demo"] == "play", "(v4) demo:0 did not pick Watching's first demo")
    press(c, d, "try")
    d = settled("(v4) Try it did not start the replay for the player",
                lambda d: d["try"] and d["replay"]["try"] and any(h.get("src") == HIT_HUD for h in d["hits"]))
    p0 = d["replay"]["paused"]
    hud_click(d, "play")
    d = settled("(v4) play did not toggle the pause", lambda d: d["replay"]["paused"] != p0)
    if not d["replay"]["paused"]:
        hud_click(d, "play")
        d = settled("(v4) play did not pause", lambda d: d["replay"]["paused"])
    end = d["replay"]["end"]
    for key, delta in (("fwd", 300), ("fwd", 300), ("rew", -300)):
        was = d["replay"]["playhead"]
        hud_click(d, key)
        want = min(max(was + delta, 0), end)
        d = settled(f"(v4) {key} did not move the playhead from {was} to {want}", lambda d: d["replay"]["playhead"] == want)
    # the track: pressed at 25 % of its run, dragged in 10 moves to 75 %, let go there
    x0, x1, hend = d["hud_run"]
    tr = hit_of(d, "track", HIT_HUD)
    y = (tr[1] + tr[3]) // 2
    per = hend / max(1, x1 - x0)
    at = lambda f: x0 + round(f * (x1 - x0))
    tick = lambda x: math.trunc(min(1, max(0, (x - x0) / (x1 - x0))) * hend + 0.5)
    c.keys("dmove:%d,%d" % (at(0.25), y), "ddown:%d,%d" % (at(0.25), y))
    d = wait_dump(c, lambda d: d["replay"]["press_held"] == "track", "(v4) the track was not held")
    assert abs(d["replay"]["playhead"] - round(0.25 * hend)) <= per + 1, \
        f"(v4) the press at 25 % sought {d['replay']['playhead']}, not {round(0.25 * hend)}"
    seen = []
    for k in range(1, 11):
        x = at(0.25 + 0.05 * k)
        c.keys("dmove:%d,%d" % (x, y))
        d = wait_dump(c, lambda d: abs(d["replay"]["playhead"] - tick(x)) <= per + 1,
                      f"(v4) the drag to {x} did not seek to {tick(x)}")
        seen.append(d["replay"]["playhead"])
    c.keys("dup:%d,%d" % (at(0.75), y))
    d = settled("(v4) the track's release did not land at 75 %",
                lambda d: not d["replay"]["press_held"] and abs(d["replay"]["playhead"] - tick(at(0.75))) <= per + 1)
    hud_click(d, "bookmark")
    d = settled("(v4) bookmark gave no toast", lambda d: d["frame"]["toast"])
    toast = d["frame"]["toast"]
    paused = d["replay"]["paused"]
    hud_click(d, "leave")
    d = settled("(v4) leave did not open the question", lambda d: d["frame"]["question"])
    # outside the dialog, on the frame's world: No at once
    out = hit_of(d, "outside", HIT_DIALOG)
    dl = hit_of(d, "dialog", HIT_DIALOG)
    c.keys("dclick:%d,%d" % ((out[0] + dl[0]) // 2, (dl[1] + dl[3]) // 2))
    d = settled("(v4) a click outside the question did not close it", lambda d: not d["frame"]["question"])
    assert d["replay"]["paused"] == paused, f"(v4) No left the replay paused={d['replay']['paused']}, not {paused}"
    hud_click(d, "leave")
    d = settled("(v4) leave did not open the question again", lambda d: d["frame"]["question"])
    c.keys("dclick:%d,%d" % c.centre(hit_of(d, "yes", HIT_DIALOG)))
    d = wait_dump(c, lambda d: d.get("replay") and d["replay"]["left"] and not d["try"] and d["caption"] == S_LEFT,
                  "(v4) Yes did not end Try it with the line that says where the game goes")
    assert c.gametime() == gt0, f"(v4) GameTime ran under the page: {gt0} -> {c.gametime()}"
    assert c.ring() == ring0, f"(v4) the engine's key ring moved: {ring0} -> {c.ring()}"
    print(f"wiki watch: (v4) Try it on {WATCH}/play: play toggled the pause, fwd fwd rew moved the playhead by 300 "
          f"each, the track pressed at 25 % sought {round(0.25 * hend)} and followed a 10-move drag ({seen[0]}..{seen[-1]}) "
          f"to 75 %, bookmark said '{toast}', a click outside the question was No, and Yes left; GameTime stood and the "
          f"key ring stayed equal; no replay HUD is underway in this skirmish, and the stage's hits are the wiki's map's")
    return d


def watch_v5(c):
    """(v5) the costs: one fill's most during Try it's seek to the end (paused, so the seek alone moves the game), the
    render thread's stage HUD and bars, the pass's peak against its caps. The window is closed and opened again first: the page's cost is counted from its
    open, and the shots above step a whole replay in one fill, which no player's fill does."""
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(v5) Esc did not close the window")
    d = watch_open(c)
    press(c, d, "try")
    # the walk over first (a press does nothing while it runs), then paused, so that only the seek moves the game:
    # a lost press then fails here rather than the replay playing to the end at 1x
    d = wait_dump(c, lambda d: d.get("replay") and d["replay"]["try"] and d["try"]
                  and d["replay"]["walked"] >= d["replay"]["end"] and any(h.get("src") == HIT_HUD for h in d["hits"]),
                  "(v5) Try it did not start again", seconds=60)
    if not d["replay"]["paused"]:
        c.keys("dclick:%d,%d" % c.centre(hit_of(d, "play", HIT_HUD)))
        d = wait_dump(c, lambda d: d["replay"]["paused"], "(v5) play did not pause the replay")
    x0, x1, _ = d["hud_run"]
    tr = hit_of(d, "track", HIT_HUD)
    k0 = d["replay"]["tick"]
    t0 = time.monotonic()
    c.keys("dclick:%d,%d" % (x1 - 1, (tr[1] + tr[3]) // 2))
    d = wait_dump(c, lambda d: d["replay"]["target"] < 0 and d["replay"]["game_tick"] >= d["replay"]["end"] - 30,
                  "(v5) the seek to the end did not finish", seconds=60)
    secs = time.monotonic() - t0
    print(f"wiki watch: (v5) a seek to the end ({d['replay']['end']} game ticks) in {secs:.1f} s and "
          f"{d['replay']['tick'] - k0} demo ticks (the walk {d['replay']['walked']}); the page cost the game thread at "
          f"most {d['cost_max_ms']:.3f} ms in one frame over {d['cost_frames']} frames")
    start = c.mark()
    c.keys("esc")
    wait_dump(c, lambda d: not d["open"], "(v5) Esc did not close the window")
    for rx in (r"wiki: the stage HUD cost the render thread", r"wiki: its frames peaked at"):
        print(f"wiki watch: (v5) {wait_line(c, start, rx, f'(v5) no `{rx}` line', 30).split('] ', 1)[-1]}")


def watch(c, shots, res):
    """(v1)..(v5): Watching, a frame:game entry, on the page"""
    held_at(c, HELD_AT)
    d = watch_open(c)
    d = watch_v1(c, d)
    watch_v2(c)
    if shots:
        watch_v3(c, shots, res)
    watch_v4(c)
    watch_v5(c)


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
        print(f"wiki mp: (m) the host's stage in 3D: {got[0]} of {got[1]} frames drew the image, at rest: nothing "
              f"built, baked or dropped; the render thread {got[2]:.3f} ms producer and {got[3]:.3f} ms passes a "
              f"frame on average, the passes at most {got[4]:.3f} ms")
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


MODS_SCREEN = "2560x1440"                     # the private display, and so the game's screen (private_xvfb)
# the setups the wiki's 3D stage is measured under, as tools/compat/setups.json names them
MODS_SETUPS = ("retail", "escalation", "mayhem-11.3.0", "prota-4.8", "tazero-alpha5", "twilight-2.0b98")
# (entry, demo, tick) captured under each: a construction among Quick Build's units, the Metal maps demo on
# Quick Build's metal stage, and the gesture's ghosts on Build Lines' stage (the first frames of UNITS_SHOTS for
# each entry)
MODS_SHOTS = (("quickbuild", 3, 180), ("quickbuild", 10, 100), ("buildshapes", 2, 100))


def stage_of(entry, demo):
    """The stage demo `demo` (from 1) of `entry` plays on, `<entry>.<scene>`: its `on`, else the first scene."""
    e = json.loads((TREE / f"art/wiki/entries/{entry}.json").read_text())
    return f"{entry}.{e['demos'][demo - 1].get('on', next(iter(e['scenes'])))}"


# the sidebar's checks under each setup (mods (b1)..(b4)): the stage's state, the demo at a tick where its ARMCK
# stands selected on its first page at rest (TA Zero: its side's GEN, the ARMCK having no page there), and the
# demo whose build trip TA Zero cannot reach, which locks at its first build (tick 20) and plays on elsewhere
SIDE_STATE = ("quickbuild", 1, 10)
SIDE_LOCK = ("buildshapes", 3, 0, 20)         # (entry, its tree node, the demo's index, its first build's tick)
LOCK_RGB = (0xD0, 0x18, 0x18)                 # the badge's disc (tagpu_wikilay.c LOCK_RGB, wiki.css .lock)
SIDE_PACKS = TREE / "tools/wiki/mockup/gfx/side"


def pack_of(setup):
    """The setup's sidebar pack (tools/wiki/sidepacks.py, gitignored: drawn from the game's files)."""
    sys.path.insert(0, str(TREE / "tools/wiki"))
    import sidepacks
    mod = next((m for m, s in sidepacks.SETUPS.items() if s == setup), None)
    assert mod and (SIDE_PACKS / mod / "side.json").exists(), \
        f"no sidebar pack for {setup}: run tools/wiki/sidepacks.py {mod or ''}"
    return mod, SIDE_PACKS / mod


def private_xvfb(size):
    """A private X server of `size`, its display number read off its readiness pipe as
    tacompat.start_xvfb reads it: the game follows the display's size (impure.cfg
    resolution=native), and the stage's sidebar is drawn at 768 rows a mod, so a 1440-row screen
    shows nine in ten of its rows where a 1024-row one shows six."""
    sys.path.insert(0, str(TREE / "tools/compat"))
    import tacompat
    r, w = os.pipe()
    p = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", f"{size}x24", "-nolisten", "tcp"],
                         pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    os.close(w)
    try:
        answer = tacompat.xvfb_reply(r)
        if not re.fullmatch(rb"\d+\n", answer) or p.poll() is not None:
            raise AssertionError(f"Xvfb did not start: {answer!r}")
    except BaseException:
        tacompat.stop_xvfb(p)
        raise
    finally:
        os.close(r)
    p.tacompat_display = int(answer)
    return p


def window_grab(display, d, path):
    """The game's client window on a private display, grabbed: of the windows titled as the game's
    (one is its frame), the one the engine's screen fills, so the picture's pixels are the client
    points the dump's hit map names."""
    r = subprocess.run(["xwininfo", "-root", "-tree"], env={**os.environ, "DISPLAY": f":{display}"},
                       capture_output=True, text=True, timeout=10)
    wins = [(m.group(1), int(m.group(2)), int(m.group(3))) for m in
            re.finditer(r'^\s+(0x[0-9a-f]+) "Total Annihilation[^"]*": \([^)]*\)\s+(\d+)x(\d+)', r.stdout, re.M)]
    vx, vy, vw, vh = d["viewport"]
    fits = sorted((w * h, wid) for wid, w, h in wins if w >= vx + vw and h >= vy + vh)
    assert fits, f"no game window holds the viewport {d['viewport']}: {wins}"
    r = subprocess.run(["import", "-display", f":{display}", "-window", fits[0][1], str(path)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, f"import: {r.stderr.strip()}"


def png_rgba(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGBA")).copy()


def column_diff(cap, comp, layout, screen, rows):
    """The engine's capture (RGBA, the column at its left edge) against a composed column, over
    `rows`, as tools/wiki/sidedump.mjs's comparisons with the phase-1 captures read them: a pixel
    the composer left unpainted differs. The count, and the gadgets (or `panel`) holding them."""
    import numpy as np
    y0, y1 = rows
    a, b = cap[y0:y1, :128, :3].astype(int), comp[y0:y1, :, :3].astype(int)
    bad = np.any(a != b, axis=2) | (comp[y0:y1, :, 3] == 0)
    where = set()
    s = layout["screens"][screen]
    for y, x in zip(*np.nonzero(bad)):
        y += y0
        g = next((g["name"] for g in reversed(s["gadgets"]) if g["rect"][0] <= x < g["rect"][0] + g["rect"][2]
                  and s["y"] + g["rect"][1] <= y < s["y"] + g["rect"][1] + g["rect"][3]), None)
        where.add(g or ("the minimap" if y < 128 else "panel"))
    return int(bad.sum()), sorted(where)


def sidedump(pack, args, png):
    r = subprocess.run(["node", str(TREE / "tools/wiki/sidedump.mjs"), "--pack", str(pack), *map(str, args),
                        "--out", str(png)], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"sidedump {' '.join(map(str, args))}: {r.stderr.strip()}"
    # its line names `<side> <screen>, ...`; the screen is the layout's key
    return r.stdout.split(": ")[2].split(",")[0].split()[-1]


def live_sidebar(c, inst, pack, out, row):
    """(b1) the setup's own sidebar against the composer on its pack, by the phase-1 method (the
    captures in .data/wiki-sidebar/captures/): the engine's frame (`tacli shot`, one pixel a game
    pixel) cropped to the column, its rows from 128 down to the layout's height, against
    tools/wiki/sidedump.mjs on the pack with each gadget greyed as the engine's own snapshot of the
    screen has it (`tacli ui`, `--live`). Nothing selected (the player's side's MAIN2); then,
    where the player's side gives ARMCK pages, an ARMCK placed beside the commander and selected
    (its first page, the BUILD tab and the move-order button current, as the engine draws them for
    it)."""
    name, lay = inst["name"], json.loads((pack / "side.json").read_text())["layout"]
    H = lay["height"]
    got = {}

    def capture(state):
        full, ui = out / f"live-{state}.full.png", out / f"live-{state}.ui.json"
        tacli("shot", name, "-o", full, timeout=120)
        snap = json.loads(tacli("ui", name, "--json", timeout=60))
        ui.write_text(json.dumps(snap))
        return png_rgba(full), snap, ui

    cap, snap, ui = capture("main2")
    gui = snap["gui"].upper()
    assert gui.endswith("MAIN2.GUI"), f"(b1) nothing selected, the engine shows {snap['gui']}, not a MAIN2"
    prefix = gui[:-len("MAIN2.GUI")]
    player = side_by_prefix(lay, prefix, "(b1)")
    screen = sidedump(pack, ["--live", ui, "--screen", "main2", "--side", prefix], out / "live-main2.composed.png")
    n, where = column_diff(cap, png_rgba(out / "live-main2.composed.png"), lay, screen, (128, H))
    got["main2"] = (screen, n, where)
    assert n == 0, f"(b1) {snap['gui']}: {n} pixels of rows 128..{H - 1} differ from the composer, in {where}"

    if "ARMCK" in player["types"]:
        roster = c.call("roster", "--json")["units"]
        com = next(u for u in roster if u["owner"] == 0)
        cx, cz = com["world"]
        sc = {"format": "ta-scenario/1", "description": "An ARMCK of the player's beside its commander, for the sidebar.",
              "seed": 20261027, "on_error": "abort", "setup": {"clear_existing": False},
              "units": [{"id": "ck", "type": "ARMCK", "owner": 0, "pos": [cx, cz + 200], "facing": 0}]}
        scen = out / "live-ck.json"
        scen.write_text(json.dumps(sc, indent=1))
        tacli("scenario", "apply", name, scen, timeout=180)
        end = time.monotonic() + 60
        while "ARMCK" not in c.units:
            for u in c.call("roster", "--json")["units"]:
                if u["owner"] == 0 and u["type"].upper() == "ARMCK":
                    c.units["ARMCK"] = u["engine_index"]
            assert "ARMCK" in c.units or time.monotonic() < end, "(b1) the ARMCK never reached the roster"
            time.sleep(1)
        try:
            c.select("ARMCK")
            time.sleep(1.0)
            cap, snap, ui = capture("armck")
        finally:
            c.call("eye", "--release")
        page = player["types"]["ARMCK"]["pages"][0]
        current = [g["name"] for g in lay["screens"][page]["gadgets"] if g["role"] == "buildtab"] + ["ARMMOVEORD"]
        screen = sidedump(pack, ["--live", ui, "--side", prefix, "--type", "ARMCK", "--page", 1, "--pressed", ",".join(current)],
                          out / "live-armck.composed.png")
        n, where = column_diff(cap, png_rgba(out / "live-armck.composed.png"), lay, screen, (128, H))
        got["armck"] = (screen, n, where)
        assert n == 0, f"(b1) {snap['gui']}: {n} pixels of rows 128..{H - 1} differ from the composer, in {where}"
    row["live"] = got
    print(f"wiki mods: {row['setup']}: (b1) the engine's own sidebar equals the composer on the pack, rows 128..{H - 1}: "
          + "; ".join(f"{k} {s} ({n} differ)" for k, (s, n, _w) in got.items()))


def layout_norm(L):
    """A layout as the sim reads it, for comparing the game's tagpu_wiki.layout.json with the
    pack's: a gadget's absent `grey`/`hidden` is false, its absent `unit` null and its absent `key` 0."""
    out = json.loads(json.dumps(L))
    for s in out.get("screens", {}).values():
        for g in s["gadgets"]:
            g["grey"], g["hidden"], g["unit"] = bool(g.get("grey")), bool(g.get("hidden")), g.get("unit")
            g["key"] = g.get("key", 0)
    out.pop("note", None)
    return out


def column_rect(d, H):
    """The client rect [x0, y0, x1, y1) the stage draws its sidebar's image into, HOP_SIDE's: the
    128-unit column left of the map's origin (stage_fit), from the stage's top, H rows, each edge
    snapped as tagpu_wikistage.c's snapd snaps it."""
    s, ox, oz = stage_fit(d)
    snap = lambda v: math.floor(v + 0.5 + 1 / 64)
    return [snap(ox - SIDE_W * s), snap(oz), snap(ox), snap(oz + H * s)]


def sample(img, rect, H):
    """The client pixels of the column's rect, and the texel column and row each samples: both
    texels where a pixel's centre falls on the edge between two, which the sampler may round
    either way (the centre of client pixel p lands at texel (p + 0.5 - x0) * 128 / w)."""
    import numpy as np
    x0, y0, x1, y1 = rect
    eps = 1e-6

    def texels(n0, n1, n):
        u = (np.arange(n0, n1) + 0.5 - n0) * n / (n1 - n0)
        return np.clip(np.floor(u - eps), 0, n - 1).astype(int), np.clip(np.floor(u + eps), 0, n - 1).astype(int)
    return img[y0:y1, x0:x1], texels(x0, x1, SIDE_W), texels(y0, y1, H)


def stage_vs(img, rect, comp, H):
    """Each client pixel of the column's rect against the composed texel it samples (either, on an
    edge): (differing pixels, pixels compared, the texels sampled, the column resampled to texels
    with an unsampled texel magenta)."""
    import numpy as np
    shown, (tx0, tx1), (ty0, ty1) = sample(img, rect, H)
    ok = np.zeros(shown.shape[:2], bool)
    for ty in (ty0, ty1):
        for tx in (tx0, tx1):
            ok |= np.all(shown == comp[ty][:, tx][:, :, :3].astype(int), axis=2)
    grid = np.zeros((H, SIDE_W, 3), np.uint8)
    grid[:] = (255, 0, 255)
    grid[np.ix_(ty1, tx1)] = shown.astype(np.uint8)
    seen = np.zeros((H, SIDE_W), bool)
    seen[np.ix_(ty1, tx1)] = True
    return int((~ok).sum()), int(ok.size), int(seen.sum()), grid


def stage_sidebar(c, display, gamedir, pack, out, row):
    """(b2)..(b4) the wiki's sidebar under the setup."""
    import numpy as np
    from PIL import Image
    # (b2) the layout every demo starts on is the mod's, and the game plays every demo on it to
    # the hash sim.js gives on the same file
    start = c.mark()
    (gamedir / "tagpu_wiki.check").write_text("")
    line = wait_line(c, start, r"wiki check: \d+ demos played", "(b2) the check", 120)
    src = wait_line(c, start, r"wiki check: the sidebar layout is ", "(b2) the check's layout line", 5)
    assert "the mod's" in src, f"(b2) {src.split('wiki check: ', 1)[1]}"
    layf = out / "tagpu_wiki.layout.json"
    shutil.copyfile(gamedir / "tagpu_wiki.layout.json", layf)
    game_lay = json.loads(layf.read_text())
    pack_lay = json.loads((pack / "side.json").read_text())["layout"]
    assert layout_norm(game_lay) == layout_norm(pack_lay), \
        f"(b2) the game's layout {layf} is not the pack's {pack / 'side.json'}"
    # the check runs the units' scripts and their sounds when the packs hold them, and its line says which, as (d1) reads it
    scr = re.search(r"the units' scripts are (on|off), their sounds (on|off)", src)
    assert scr, f"(b2) the check's line does not say whether the units' scripts and sounds are on: {src}"
    assert scr.group(1) == scr.group(2), f"(b2) the game's check has scripts {scr.group(1)} but sounds {scr.group(2)}"
    c.mark()
    nhash = check_against(check_hashes(c.log[start:]), simparity_hashes(layf, pack, scr.group(1) == "on"), "(b2)")
    row["layout"] = {"screens": len(game_lay["screens"]), "height": game_lay["height"],
                     "types": {s["name"]: sorted(s["types"]) for s in game_lay["sides"]}}
    print(f"wiki mods: {row['setup']}: (b2) {src.split('wiki check: ', 1)[1]}, equal to the pack's "
          f"({row['layout']}); {line.split('wiki check: ', 1)[1]}; all {nhash} hashes (every side at both interface "
          f"types, with the pack's castings) equal simparity --hash")

    # (b3) the stage's column at a state, against the composer on the pack for the same view
    entry, demo, tick = SIDE_STATE
    start = c.mark()
    trigger(gamedir / "tagpu_wiki.shot", f"{entry} {demo} {tick}\n")
    wait_line(c, start, r"wiki: shot \d+ asked", f"(b3) {entry} {demo} {tick}")
    d = wait_dump(c, lambda d: d["tick"] == tick and not d["playing"] and d["side_h"] > 0, f"(b3) {entry} {demo} not held at {tick}")
    assert d["layout_src"] == "mod", f"(b3) the demo started on the {d['layout_src']} layout"
    c.keys("dmove:%d,%d" % c.centre(c.hit(d, "close")))
    time.sleep(1.5)
    d = c.dump()
    shot = out / f"stage-{entry}-d{demo}-t{tick}.window.png"
    window_grab(display, d, shot)
    comp = out / f"stage-{entry}-d{demo}-t{tick}.composed.png"
    # the context the game's demo plays in, side, interface type and casting, for sim.js's play of it
    ctxf = out / "stage-ctx.json"
    ctxf.write_text(json.dumps(d["ctx"]))
    r = subprocess.run(["node", str(TREE / "tools/wiki/sideview.mjs"),
                        *map(str, ("--play", entry, demo, tick, "--layout", layf, "--ctx", ctxf, "--compose", pack, "--out", comp))],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"sideview: {r.stderr.strip()}"
    v = json.loads(r.stdout)
    H = v["side"]["h"]
    img, rect, cm = png_rgba(shot)[:, :, :3].astype(int), column_rect(d, H), png_rgba(comp)
    # the rect as snapped, or the placement within a pixel of each edge that matches the most texels,
    # which the line names when it is not the snapped one
    best = (rect, stage_vs(img, rect, cm, H))
    for dx0, dy0, dx1, dy1 in itertools.product((-1, 0, 1), repeat=4) if best[1][0] else ():
        rr = [rect[0] + dx0, rect[1] + dy0, rect[2] + dx1, rect[3] + dy1]
        res = stage_vs(img, rr, cm, H)
        if res[0] < best[1][0]:
            best = (rr, res)
    rr, (bad, painted, seen, grid) = best
    s = stage_fit(d)[0]
    row["stage"] = {"screen": v["side"]["screen"], "rect": rr, "snapped": rect, "scale": round(s, 4), "differ": bad, "pixels": painted,
                    "texels": seen, "of": SIDE_W * H, "minimap": v["composed"]["minimap"]}
    # the picture for the owner: the stage's column as drawn, the composer's for the same view, and
    # the engine's own sidebar (its first page with the ARMCK selected, else nothing selected)
    live = out / ("live-armck.full.png" if (out / "live-armck.full.png").exists() else "live-main2.full.png")
    lv = png_rgba(live)[:H, :SIDE_W, :3]
    pair = np.full((H, 3 * SIDE_W + 16, 3), (255, 0, 255), np.uint8)
    pair[:, :SIDE_W] = grid
    pair[:, SIDE_W + 8:2 * SIDE_W + 8] = cm[:, :, :3]
    pair[:lv.shape[0], 2 * SIDE_W + 16:] = lv
    Image.fromarray(pair).resize((pair.shape[1] * 2, H * 2), Image.NEAREST).save(out / "sidebar.png")
    print(f"wiki mods: {row['setup']}: (b3) {entry} demo {demo} held at tick {tick}: the stage's column "
          f"({v['side']['screen']}, the shown unit's page {v['side']['page']}) drawn at {s:.3f} client pixels a game "
          f"pixel into {rr}{'' if rr == rect else f' (the stage snaps it to {rect})'}: {bad} of {painted} pixels differ "
          f"from the composer on the pack, {seen} of {SIDE_W * H} "
          f"texels sampled; the minimap {'composed' if v['composed']['minimap'] else 'left to the whitebox'}")
    assert bad == 0, f"(b3) {bad} pixels of the stage's column differ from the composer (out: {out / 'sidebar.png'})"

    # (b4) the demo played in the player's context: held at sim.js's lock with the badge where sim.js
    # locks it, playing on past its first build elsewhere
    entry, node, k, at = SIDE_LOCK
    r = subprocess.run(["node", str(TREE / "tools/wiki/sideview.mjs"),
                        *map(str, ("--play", entry, k + 1, at + 60, "--layout", layf, "--ctx", ctxf, "--scripts", pack))],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"sideview: {r.stderr.strip()}"
    v = json.loads(r.stdout)
    d = c.dump()
    if d["entry"] != entry:
        press(c, d, f"entry:{node}")
        d = wait_dump(c, lambda d: d["entry"] == entry and f"demo:{k}" in keys_of(d, "demo:"), f"(b4) entry:{node}")
    t0 = press(c, d, f"demo:{k}")
    d = wait_dump(c, lambda d: d["playing"] and fresh(d, t0) and d["tick"] >= 0, f"(b4) demo:{k} did not start")
    time.sleep(3.0)
    d = c.dump()
    time.sleep(1.5)
    d2 = c.dump()
    c.keys("dmove:%d,%d" % c.centre(c.hit(d2, "close")))
    time.sleep(1.0)
    shot = out / f"lock-{entry}-d{k + 1}.window.png"
    window_grab(display, d2, shot)
    img = png_rgba(shot)[:, :, :3].astype(int)
    sx0, sy0, sx1, _ = c.hit(d2, "stage")
    above = img[d2["viewport"][1]:sy0, sx0:sx1]
    red = int(np.all(np.abs(above - LOCK_RGB) <= 8, axis=2).sum())
    if v["locked"]:
        assert d["done"] and d2["done"] and d["tick"] == d2["tick"] == v["tick"] == at, \
            f"(b4) {entry} demo {k + 1}: ticks {d['tick']} -> {d2['tick']}, done {d['done']}, not held at {at} (sim.js: {v})"
        assert red > 0, f"(b4) {entry} demo {k + 1} held at its lock, but no badge above the stage ({shot})"
    else:
        assert d2["tick"] > d["tick"] > at and not red, \
            f"(b4) {entry} demo {k + 1}: ticks {d['tick']} -> {d2['tick']}, {red} badge pixels (sim.js: not locked)"
    row["lock"] = {"locked": v["locked"], "ticks": [d["tick"], d2["tick"]], "badge_px": red}
    print(f"wiki mods: {row['setup']}: (b4) {entry} demo {k + 1} played: "
          + (f"held at its lock, tick {d['tick']} -> {d2['tick']} over 1.5 s, {v['locked']} out of reach, the badge "
             f"shown ({red} pixels of its disc)" if v["locked"] else
             f"past its first build at tick {at}, tick {d['tick']} -> {d2['tick']}, no badge"))


MODS_WATCH = ("play", 150)                    # (w6) Watching's first demo, paused, no seek pending


def mods_bars(c, display, pack, out, row):
    """(w6) Watching's first demo held at a tick through tagpu_wiki.shot: the frame's two bars, each client pixel
    against the texel of bar.js's picture on the setup's pack (side and badge colour the record's, every number 0)
    its nearest sampling shows (either, on an edge, as the column's check takes it), Impure's emblems' columns left
    out (the stage draws them over the bar, from the kit). A side the pack has no bars for draws the whitebox strips
    and is said, not compared."""
    import numpy as np
    demo, tick = MODS_WATCH
    d = watch_shot(c, demo, tick)
    png = out / f"{WATCH}-{demo}-t{tick}.window.png"
    window_grab(display, d, png)
    img = png_rgba(png)
    f, raw = d["frame_fit"], out / "bars.rgba"
    r = subprocess.run(["node", str(TREE / "tools/wiki/barcheck.mjs"), "--compose", str(pack), str(d["ctx"]["side"]),
                        str(d["frame"]["colour"]), str(f["Wg"]), str(f["Hg"]), str(raw)],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"(w6) barcheck --compose: {r.stderr.strip()[-400:]}"
    if r.stdout.strip() == "none":
        row["bars"] = "none: the pack has no bars for the side"
        print(f"wiki mods: (w6) {pack.name} side {d['ctx']['side']}: bar.js composes no bars, the stage the whitebox")
        return
    w = int(r.stdout.strip())
    assert w == f["Wg"] - 128, f"(w6) bar.js's bars are {w} wide, the frame's {f['Wg'] - 128}"
    comp = np.frombuffer(raw.read_bytes(), np.uint8).reshape(64, 2048, 4)[:, :, :3].astype(int)
    R, eps, said = frame_rects(d), 1e-6, []

    def texels(n0, n1, n):
        u = (np.arange(n0, n1) + 0.5 - n0) * n / (n1 - n0)
        return np.clip(np.floor(u - eps), 0, n - 1).astype(int), np.clip(np.floor(u + eps), 0, n - 1).astype(int)
    for part, row0 in (("top", 0), ("bottom", 32)):
        x0, y0, x1, y1 = R[part]
        shown = img[y0:y1, x0:x1, :3].astype(int)
        (tx0, tx1), (ty0, ty1) = texels(x0, x1, w), texels(y0, y1, 32)
        ok = np.zeros(shown.shape[:2], bool)
        for ty in (ty0, ty1):
            for tx in (tx0, tx1):
                ok |= np.all(shown == comp[row0 + ty][:, tx], axis=2)
        if part == "top":
            # the emblems: frame x Wg - 75 .. Wg - 17 (the presentation spec's 3.4), bar texels from 128 less
            ok[:, (tx1 >= f["Wg"] - 76 - 128) & (tx0 < f["Wg"] - 16 - 128)] = True
        bad = int((~ok).sum())
        assert bad == 0, f"(w6) {bad} of {ok.size} pixels of the {part} bar differ from bar.js on {pack.name}"
        said.append(f"{part} {ok.size} px")
    row["bars"] = ", ".join(said) + " equal to bar.js"
    print(f"wiki mods: (w6) {WATCH}/{demo} at tick {tick} on {pack.name}, side {d['ctx']['side']}: the {row['bars']}")


def mods_setup(tacompat, setup, dll, out):
    """(w1)..(w5) on one setup: its folder as a player has it, started on a private Xvfb and walked
    into a skirmish, the wiki opened on both entries. The row, passed or with why it failed."""
    from PIL import Image
    row = {"setup": setup["name"], "boxes": [], "pass": False}
    out.mkdir(parents=True, exist_ok=True)
    mod, pack = pack_of(setup["name"])
    row["pack"] = mod
    xv = private_xvfb(MODS_SCREEN)
    try:
        inst = tacompat.prepare_wine(setup, dll, xv.tacompat_display, suffix="-wk")
    except BaseException:
        tacompat.stop_xvfb(xv)
        raise
    g = tacompat.start_wine(inst, xv.tacompat_display, display_server=xv)
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
        live_sidebar(c, inst, pack, out, row)
        poll("b1")
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
            sid = stage_of(entry, demo)
            ln = wait_line(c, start, rf"wiki3d: stage {re.escape(sid)}: the ground", f"(w3) no ground line for {sid}")
            row["stages"][sid] = ln.split(f"stage {sid}: ", 1)[1]
            ln = wait_line(c, start, rf"wiki3d: {entry}: the types in 3D:", f"(w3) no types line for {entry}")
            row["types"][entry] = ln.split(f"{entry}: ", 1)[1]
            # the frames of this stage alone, at rest; (w5) judges how many drew the image
            got = drawn_window(c, f"(w3) {entry}", want_all=None)
            row["drawn"][sid] = got[:2]
            screen_grab(f"{entry}-d{demo}-t{tick}.screen.png")
            poll("w3")
            print(f"wiki mods: {setup['name']}: (w3) {entry} demo {demo} tick {tick}: "
                  f"{armed.split('wiki3d: ', 1)[1]}; {row['stages'][sid]}; {row['types'][entry]}; "
                  f"{got[0]} of {got[1]} frames drew the image")
        stage_sidebar(c, display, gamedir, pack, out, row)
        poll("b4")
        mods_bars(c, display, pack, out, row)
        poll("w6")
        c.keys("esc")
        wait_dump(c, lambda d: not d["open"], "(w4) Esc did not close the window")
        time.sleep(2)
        poll("w4")
        # (w5) the claim: a ground the report resolved is drawn in 3D on every frame, one it did not
        # is the whitebox on every frame
        for st in report["stages"]:
            e = f"{st['entry']}.{st['scene']}"
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
        print(f"   sidebar ({r.get('pack')}'s pack): the engine's {r.get('live')}; the layout {r.get('layout')}; "
              f"the stage {r.get('stage')}; the lock {r.get('lock')}; the frame's bars {r.get('bars')}")
        for st in rep.get("stages", []):
            srcs = "; ".join(f"{s['map']} <- {arc(s['from'])}" + ("" if s["ok"] else f" ({s['why']})")
                             for s in st.get("sources", []))
            feats = sorted({f"{f['name']} <- {arc(f.get('from'))}/{arc(f.get('gaf_from'))}" + ("" if f["ok"] else " MISS")
                            for f in st.get("features", [])})
            sid = f"{st['entry']}.{st['scene']}"
            print(f"   stage {sid}: ok={st['ok']} {st.get('why') or ''} | {srcs} | {', '.join(feats)}")
            print(f"     drawn: {r.get('stages', {}).get(sid)}; {r.get('drawn', {}).get(sid)} frames")
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


# the gate's shards of each simparity run (simparity.mjs --shard): its 48 contexts of demo plays in four processes
SIMPARITY_SHARDS = 4


def offline_runs(shards=1):
    """OFFLINE_CHECKS: every check of the wiki that plays no game, as (name, argv) -- the mockup's demos on the
    fixture, without a sidebar and on each side pack present (tools/wiki/sidepacks.py builds them, gitignored), then
    the DLL's code against the mockup's: the simulation (simparity), the text's fill (textparity), each natively and
    under Wine as the DLL is built, the sidebar's column (sidecheck), the stage's ops (stagecheck), the 3D scene
    (scenecheck) and the window's load-time check over every pack's castings (wikilaycheck --check-only). None of
    them plays a game, and none writes what another reads: they run in any order, at once. `shards` splits each
    simparity run into that many processes, which together check what one does."""
    packs = sorted(d for d in (TREE / "tools/wiki/mockup/gfx/side").glob("*") if (d / "side.json").is_file())
    wiki = TREE / "tools/wiki"
    runs = [("simcheck", [wiki / "simcheck.mjs"]), ("simcheck --null-layout", [wiki / "simcheck.mjs", "--null-layout"])]
    runs += [(f"simcheck --pack {d.name}", [wiki / "simcheck.mjs", "--pack", d]) for d in packs]
    for wine in ([], ["--wine"]):
        runs += [(" ".join(["simparity", *wine]), [wiki / "simparity.mjs", *wine])] if shards == 1 else \
            [(" ".join(["simparity", *wine, f"--shard {k}/{shards}"]), [wiki / "simparity.mjs", *wine, "--shard", f"{k}/{shards}"])
             for k in range(1, shards + 1)]
    runs += [("textparity", [wiki / "textparity.mjs"]), ("textparity --wine", [wiki / "textparity.mjs", "--wine"]),
             ("sidecheck", [wiki / "sidecheck.mjs"]), ("stagecheck", [wiki / "stagecheck.mjs"]),
             ("scenecheck", [wiki / "scenecheck.mjs"])]
    runs = [(name, ["node", *cmd]) for name, cmd in runs]
    runs += [("wikilaycheck --check-only", [sys.executable, wiki / "wikilaycheck.py", "--check-only"])]
    if not packs:
        print("wiki offline: no side pack present (tools/wiki/sidepacks.py builds them): the mods' runs are left out")
    return runs


def offline_one(name, cmd, popen=subprocess.Popen, env=None, done=lambda p: None, bound=None):
    """One offline check to its end: its verdict line printed, and its exit code -- 2 is a check that could not run,
    which fails like one that differs. `popen` starts it and `done` lets it go: the gate's own (Lanes), which end
    it with the gate; past `bound` seconds it is hung, ended and fails."""
    t0 = time.monotonic()
    p = popen(list(map(str, cmd)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=TREE, env=env)
    try:
        stdout, stderr = p.communicate(timeout=bound)
    except subprocess.TimeoutExpired:
        Lanes.end(p)
        stdout, stderr = p.communicate()
        print(f"wiki offline: {name}: FAILED (still running after {bound} s, ended)", flush=True)
        return 124, f"still running after {bound} s, ended"
    finally:
        done(p)
    # the check's own verdict, the line it starts with its name (sidecheck prints its packs after it), else the last
    out = stdout.strip().splitlines() or stderr.strip().splitlines() or [""]
    own = Path(str(cmd[1])).stem + ":"
    last = next((ln for ln in out if ln.startswith(own)), out[-1])
    verdict = "passed" if p.returncode == 0 else "CANNOT RUN" if p.returncode == 2 else "FAILED"
    print(f"wiki offline: {name}: {verdict} ({p.returncode}, {time.monotonic() - t0:.0f} s): {last[:300]}", flush=True)
    return p.returncode, last[:300]


def offline_checks():
    """offline_runs one after another, each to its end whatever the one before said. Takes no lock."""
    failed = [name for name, cmd in offline_runs() if offline_one(name, cmd)[0]]
    assert not failed, f"the offline checks {', '.join(failed)} did not pass"


GATE_MODES = ("sp", "page", "demos", "try", "asset", "stage3d", "units", "cursor", "watch")
GATE_SHOTS = {"demos", "stage3d", "units", "cursor", "watch"}    # the modes that take --shots DIR
GATE_RES = "1920x1080"                                  # each game lane's private display, and so its game's screen
GATE_LANE_S = 3600                                      # a lane still running after this is hung, not slow: the
                                                        # slowest, units, takes some 750 s alone and 863 s at -j 6
GATE_STOP_S = 120                                       # a lane asked to stop (SIGINT, which runs its finally blocks:
                                                        # the game stopped, its X server and levers put back) is
                                                        # killed if it has not exited by then
GATE_VERDICT = re.compile(r"wiki regression (\w+: passed|FAILED)|wiki mods: \S+: (FAILED|\(w4, w5\))")


class Lanes:
    """Every process the gate starts, each in a session of its own, so the gate can end each with all it started.
    THE LOCK'S LIFETIME: the lanes run under the gate's hold of the run lock (RUNLOCK_HELD, inherited), which the
    kernel drops when the gate exits -- so the gate never exits while a lane runs: on any way out, a signal
    included, it stops starting lanes and ends every live one before it returns."""

    def __init__(self):
        import threading
        self.lock, self.live, self.stopping = threading.Lock(), set(), False

    def popen(self, cmd, **kw):
        with self.lock:
            if self.stopping:
                raise OSError("the gate is stopping: no new lane")
            p = subprocess.Popen(cmd, start_new_session=True, **kw)
            self.live.add(p)
        return p

    def done(self, p):
        with self.lock:
            self.live.discard(p)

    @staticmethod
    def end(p):
        """SIGINT to the process's session, then SIGKILL past GATE_STOP_S."""
        for sig, wait in ((2, GATE_STOP_S), (9, None)):
            if p.poll() is not None:
                return
            try:
                os.killpg(p.pid, sig)
            except ProcessLookupError:
                return
            try:
                p.wait(timeout=wait)
            except subprocess.TimeoutExpired:
                pass

    def end_all(self):
        """Every live lane asked at once, then killed together past one GATE_STOP_S: never one wait a lane."""
        with self.lock:
            self.stopping = True
            live = list(self.live)
        for p in live:
            try:
                os.killpg(p.pid, 2)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + GATE_STOP_S
        for p in live:
            try:
                p.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
        for p in live:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 9)
                except ProcessLookupError:
                    pass
                p.wait()


def gate_lane(lanes, cmd, log, env):
    """One lane's process to its end, everything it says into `log`: its exit code and its verdict line, the last
    line naming the regression's verdict, else the last it printed. A lane past GATE_LANE_S is ended and fails."""
    with open(log, "w") as f:
        p = lanes.popen(list(map(str, cmd)), stdout=f, stderr=subprocess.STDOUT, cwd=TREE, env=env)
        try:
            code = p.wait(timeout=GATE_LANE_S)
        except subprocess.TimeoutExpired:
            lanes.end(p)
            return 124, f"still running after {GATE_LANE_S} s, ended"
        finally:
            lanes.done(p)
    lines = [ln for ln in Path(log).read_text(errors="replace").splitlines() if ln.strip()]
    said = [ln for ln in lines if GATE_VERDICT.match(ln)]
    return code, (said or lines or [""])[-1][:300]


def gate_instance(name, *create):
    """The instance `name` made afresh, with registry hives of its own: lanes running at once must not share the
    template's hive inode, which Wine rewrites in place (tacli privatise_hives). Made before any lane starts, so no
    wineserver of the gate is writing the template's hives while they are copied."""
    if tacli_api().Instance(name).exists:
        tacli("rm", name, "--force", timeout=120)
    tacli("create", name, "--private-registry", *create, timeout=300)
    return tacli_api().Instance(name)


def gate(shots, jobs, offline_jobs, modes, setups):
    """Every part of the wiki's gate at once under one hold of the run lock: the offline checks on `offline_jobs`
    workers, the Wine ones in a prefix of their own, and on `jobs` workers each game lane -- a mode on its own
    instance `wikigate-<mode>` with its own registry and its own private X server, and a setup of the mods run
    (prepare_wine makes each its own) -- each a process of this script, so every lane is the command a serial run
    would give and says the same verdict. The runs viewer shows one step a lane."""
    sys.path.insert(0, str(TREE / "tools/compat"))
    import concurrent.futures
    import signal
    import tacompat
    import tarun
    runlock.hold(TREE, f"wiki_regression gate (-j {jobs})")
    shots.mkdir(parents=True, exist_ok=True)
    offline = offline_runs(SIMPARITY_SHARDS) if offline_jobs else []
    items = [("offline", [{"id": n, "pool": "offline", "order": k} for k, (n, _) in enumerate(offline)]),
             ("game", [{"id": m, "pool": "game", "order": k} for k, m in enumerate(modes)]),
             ("mods", [{"id": n, "pool": "game", "order": len(modes) + k} for k, n in enumerate(setups)])]
    items = [it for it in items if it[1]]
    J = tarun.begin("wiki_regression", items, kind="gate", title=f"wiki gate ({len(modes)} modes, {len(setups)} "
                    f"setups, {len(offline)} offline checks) -j {jobs}", pools={"game": jobs, "offline": max(1, offline_jobs)},
                    gate=True)
    me = [sys.executable, "-u", str(Path(__file__).resolve())]
    env = dict(os.environ)
    lanes = Lanes()
    servers = set()             # the game lanes' X servers not yet stopped by their lane
    state = "stopped"

    def on_term(*_):
        raise KeyboardInterrupt("SIGTERM")

    def verdict(code):
        return "pass" if code == 0 else "unrun" if code == 2 else "fail"

    def lane_offline(name, cmd, wenv):
        J.start("offline", name)
        try:
            code, line = offline_one(name, cmd, lanes.popen, wenv, lanes.done, GATE_LANE_S)
        except (Exception, SystemExit) as e:        # noqa: BLE001 -- the lane's verdict, never the gate's crash
            code, line = 2, f"could not run: {type(e).__name__}: {e}"
        J.finish("offline", name, verdict(code), line)
        return ("offline", name, code, line)

    def lane_mode(mode, inst, xv):
        J.start("game", mode)
        try:
            cmd = me + [inst.name, mode, "--launch", "--res", GATE_RES] + (["--shots", shots / mode] if mode in GATE_SHOTS else [])
            code, line = gate_lane(lanes, cmd, shots / f"{mode}.log", env)
        except (Exception, SystemExit) as e:        # noqa: BLE001
            code, line = 2, f"could not run: {type(e).__name__}: {e}"
        finally:
            servers.discard(xv)
            tacompat.stop_xvfb(xv)
        J.finish("game", mode, verdict(code), line)
        print(f"wiki gate: {mode}: {'passed' if code == 0 else 'FAILED'} - {line}", flush=True)
        return ("game", mode, code, line)

    def lane_setup(name):
        J.start("mods", name)
        try:
            code, line = gate_lane(lanes, me + [name, "mods", "--shots", shots / "mods" / name], shots / f"mods-{name}.log", env)
        except (Exception, SystemExit) as e:        # noqa: BLE001
            code, line = 2, f"could not run: {type(e).__name__}: {e}"
        J.finish("mods", name, verdict(code), line)
        print(f"wiki gate: mods {name}: {'passed' if code == 0 else 'FAILED'} - {line}", flush=True)
        return ("mods", name, code, line)

    t0 = time.monotonic()
    old_term = signal.signal(signal.SIGTERM, on_term)
    games = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs))
    checks = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, offline_jobs))
    try:
        # every instance made before the first lane starts (gate_instance), each game lane's on its own X server,
        # which an instance's display is pinned to when it is made
        wine = gate_instance("wikigate-wine") if any("--wine" in map(str, c) for _, c in offline) else None
        wenv = {**env, "WINEPREFIX": str(wine.prefix)} if wine else env
        made = []
        for m in modes:
            # held until the server is in `servers`, where the clean-up finds it: a signal between the two would
            # leave it running in a session of its own
            signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
            try:
                xv = private_xvfb(GATE_RES)
                servers.add(xv)
            finally:
                signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGINT, signal.SIGTERM})
            made.append((m, gate_instance(f"wikigate-{m}", "--display", f":{xv.tacompat_display}"), xv))
        futs = [checks.submit(lane_offline, n, c, wenv if "--wine" in map(str, c) else env) for n, c in offline]
        futs += [games.submit(lane_mode, m, inst, xv) for m, inst, xv in made]
        futs += [games.submit(lane_setup, n) for n in setups]
        done = [f.result() for f in futs]
        bad = [f"{kind} {name}" for kind, name, code, _ in done if code]
        state = "fail" if bad else "pass"
    except BaseException as e:
        state = "stopped" if isinstance(e, KeyboardInterrupt) else "unrun"
        raise
    finally:
        # the clean-up runs to its end: a second Ctrl-C or SIGTERM inside it would leave lanes playing under a
        # released lock, so both are ignored until it is done
        old_int = signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if state in ("stopped", "unrun"):
            lanes.end_all()
        games.shutdown(wait=True, cancel_futures=True)
        checks.shutdown(wait=True, cancel_futures=True)
        for xv in list(servers):
            tacompat.stop_xvfb(xv)
        J.end(state)
        signal.signal(signal.SIGTERM, old_term)
        signal.signal(signal.SIGINT, old_int)
    print(f"wiki gate: {len(done) - len(bad)} of {len(done)} lanes passed in {time.monotonic() - t0:.0f} s"
          f"{'; failed: ' + ', '.join(bad) if bad else ''}; the logs in {shots}")
    assert not bad, f"the lanes {', '.join(bad)} did not pass"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("instance")
    p.add_argument("mode", choices=("sp", "page", "demos", "try", "recorded", "asset", "stage3d", "units", "cursor", "watch",
                                    "mp", "mods", "offline", "gate"))
    p.add_argument("--launch", action="store_true",
                   help="sp/recorded: launch on place-basic.json first, under the run lock")
    p.add_argument("--res", default="1920x1080", help="sp/recorded: the game's resolution")
    p.add_argument("--shot", help="sp, page, try: grab the presented frame with the window open to this PNG "
                                  "(page: on the second entry and its second demo, at (p2); try: one a demo, "
                                  "named <stem>-<entry>-<demo>, mid-replay)")
    p.add_argument("--shots", type=Path, help="demos: grab each entry's demo mid-play, and the mockup's, into this folder; "
                                              "stage3d, units: the captures of the wiki's target and the presented frames; "
                                              "watch: each Watching demo's frame, four beside the mockup's")
    p.add_argument("--jobs", "-j", type=int, default=6, help="gate: the game lanes played at once")
    p.add_argument("--offline-jobs", type=int, default=8, help="gate: the offline checks run at once, 0 for none")
    p.add_argument("--modes", default=",".join(GATE_MODES), help="gate: the single-player modes, each a lane")
    p.add_argument("--setups", default=",".join(MODS_SETUPS), help="gate: the mods run's setups, each a lane, '' for none")
    a = p.parse_args()
    t0 = time.monotonic()
    stop = None
    try:
        if a.mode == "offline":
            offline_checks()
        elif a.mode == "gate":
            assert a.shots, "gate needs --shots DIR"
            gate(a.shots, a.jobs, a.offline_jobs, [m for m in a.modes.split(",") if m], [n for n in a.setups.split(",") if n])
        elif a.mode == "mp":
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
                elif a.mode == "watch":
                    watch(c, a.shots, a.res)
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
