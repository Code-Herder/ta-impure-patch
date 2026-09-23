#!/usr/bin/env python3
"""uiwalk — drive one instance through the UI screen inventory and collect the
Phase E measurements at every stop (research/notes/gui-renderer.md 3.11).

Two modes:

G15a, the CENSUS (default). The DLL is armed with `tagpu_gui.on=census log pgm trace`,
so at every engine flip it diffs the flipped surface against its previous copy,
subtracts every recorded op and the world viewport, and logs what nobody explained.
At each screen the walk lets the census run, pulls its lines, asks for the PGM of
the last flip and the engine's surface shot, and writes a report.

The in-game inventory (G15c) is side-aware: `--side core` walks CORMAIN2/CORCOM1/2 on
the CORE parity fixture. After the screens it exercises the HUD extras — the `+clock`
string, the `+bps` lines, the hold-space unit popup — then the dialogs over the
viewport at zoom 0.5x and 2x (opened at 1x, zoomed by the file lever, so no click is
bent), and finally a moving commander for the minimap's dots and box.

G15e, the RESTORED TWIN (`--restore`). The DLL is armed with `tagpu_gui.on=log`,
`tagpu_classicpp.on` and `tagpu_restoredump.on`: the layer draws in Classic++ colour and
the walk's only job is to make every screen paint, so the UI atlas fills with the art the
inventory draws. Nothing is shot and nothing is diffed here. At the end the dumped twin (`tagpu_restore_gui.{r8,rgba,idx}`) is copied
into --out, where `tools/tascene uidiff` holds it to the same restorer run offline on the
same cells (gui-renderer.md 3.11's Q2 diff).

The cycles (G15d, `--cycles N`): after the in-game stops, N times game -> shell -> game
in ONE process (no relaunch): the exit dialogs over the world, the return to MAINMENU
through the switch to 640x480, the whole shell inventory again, the loading screen
held while the map loads (until the world is alive), the fixture re-applied, the side's
screens, and `+gamma` at 1.5 and back — the one lever that makes the engine present a
palette other than main+0x143A7.

    tools/uiwalk.py --inst uiw --res 1024x768 --out /tmp/uiwalk
    tools/uiwalk.py --inst uiw --shell-only
    tools/uiwalk.py --inst uiwc --side core --game-only --out /tmp/uiwalk-core
    tools/uiwalk.py --inst uiwd --game-only --screens-only --cycles 3 --out /tmp/cycles
    tools/uiwalk.py --inst uiwr --restore --out /tmp/uirestore     # then: tascene uidiff /tmp/uirestore/tagpu_restore_gui

The instance is created if needed and STOPPED at the end (kept for inspection with
--keep). Everything goes through tacli; nothing here touches X.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TACLI = HERE / "tacli"
TREE = HERE.parent

# the default arm set of the ta-drive skill, less gui.on (the walk arms its own mode)
ARM_SET = ["native.on=all wrecks", "terr.on", "feat.on", "fx.on", "sfx.on", "mark.on=log", "order.on",
           "zoom.on", "vpwide.on"]
# G17b: aim every gadget click in CLIENT-AREA pixels and let the engine work back
# to a logical pixel by its own arithmetic. A walk that mixes the two proves
# nothing, so it is a whole-run switch (`--device`).
CLICK_DEVICE = False

# (label, actions) — actions are tacli argument lists run in order; a `ui click`
# that fails is logged and the walk goes on (the screen is then whatever it is).
# String actions are the walk's own verbs (see Walk.act).
SHELL_WALK = [
    ("MAINMENU", []),
    ("SINGLE", [["ui", "click", "SINGLE"]]),
    ("SKIRMISH", [["ui", "click", "Skirmish"]]),
    ("SELMAP", [["ui", "click", "SelectMap"]]),
    ("SKIRMISH-back", [["ui", "click", "PREVMENU", "PrevMenu", "PREV", "CANCEL"]]),
    ("SINGLE-back", [["ui", "click", "PrevMenu", "PREVMENU", "PREV", "CANCEL"]]),
    ("OPTIONS", [["ui", "click", "Options", "OPTIONS", "PREFS"]]),
    ("VISUALS", [["ui", "click", "VISUALS", "Visuals"]]),
    ("OPTIONS-back", [["ui", "click", "PREV", "PREVMENU", "PrevMenu", "CANCEL"]]),
    ("SOUNDS", [["ui", "click", "SOUND", "SOUNDS", "Sound"]]),
    ("OPTIONS-back2", [["ui", "click", "PREV", "PREVMENU", "PrevMenu", "CANCEL"]]),
    ("SINGLE-back2", [["ui", "click", "PREV", "PREVMENU", "PrevMenu", "CANCEL"]]),
    ("MAINMENU-back", [["ui", "click", "PrevMenu", "PREVMENU", "PREV", "CANCEL"]]),
]


def chat(text):
    """Type a chat line (or a `+cheat`) the way a player does: Return, the characters, Return.
    A space goes as the `space` key token — `char: ` is dropped, and `+gamma 15` typed that
    way is `+gamma15`, a command that does not exist (the G15d walk's first run)."""
    return [["keys", "return"], ["keys", *["space" if c == " " else f"char:{c}" for c in text]], ["keys", "return"]]


def game_walk(side):
    P = side[:3].upper()      # ARM / COR: the side's own screens and pager buttons
    return [
        (f"{P}MAIN2", []),
        (f"{P}COM1", ["select-commander"]),
        (f"{P}COM2", [["ui", "click", f"{P}NEXT"]]),
        (f"{P}COM1-back", [["ui", "click", f"{P}PREV"]]),
        ("ARMOPT", [["keys", "tab"]]),
        ("PREFS", [["ui", "click", "PREFS"]]),
        ("VISUALRT", [["ui", "click", "VISUALS"]]),
        ("PREFS-back", [["ui", "click", "PREV", "CANCEL", "PREVMENU"]]),
        ("ARMOPT-back", [["ui", "click", "PREV", "CANCEL", "PREVMENU", "OK"]]),
        ("game-back", [["ui", "click", "OK", "CANCEL", "PREV"]]),
        ("chat", chat("hello")),
        ("F4", [["keys", "f4"]]),
        ("F4-close", [["keys", "f4"]]),
        # --- G15c: the HUD extras drawn over the viewport ---
        # 0x46A1D0: h:m:s from the sim tick at the viewport's bottom-left. The tick runs on
        # between shots, so the stop is taken with the in-game menu open: ARMOPT pauses the
        # game and the clock with it.
        ("clock", ["park", *chat("+clock"), ["keys", "tab"]]),
        ("clock-off", [["ui", "click", "PREV", "CANCEL", "PREVMENU", "OK"], *chat("+clock")]),
        ("bps", chat("+bps")),                            # 0x468380: the Receive/Send K/s lines
        ("bps-off", chat("+bps")),
        ("space-popup", ["hover-commander", ["keys", "down:space"]]),   # DrawPopupButtomDialog 0x4689C0
        ("space-popup-up", [["keys", "up:space"], "park"]),
        # --- G15c: the dialogs over the viewport at 0.5x and 2x (opened at 1x, then zoomed) ---
        ("ARMOPT@0.5", [["keys", "tab"], "zoom:0.5"]),
        ("ARMOPT@2", ["zoom:2"]),
        ("PREFS@1", ["zoom:1", ["ui", "click", "PREFS"]]),
        ("PREFS@0.5", ["zoom:0.5"]),
        ("PREFS@2", ["zoom:2"]),
        ("PREFS@2-back", ["zoom:1", ["ui", "click", "PREV", "CANCEL", "PREVMENU"],
                          ["ui", "click", "OK", "CANCEL", "PREV"]]),
        ("F4@0.5", [["keys", "f4"], "zoom:0.5"]),
        ("F4@2", ["zoom:2"]),
        ("chat@2", chat("hello at two")),
        ("F4-close@1", [["keys", "f4"], "zoom:reset"]),
        # --- G15c: the minimap's dots and view box while a unit moves ---
        ("move", ["select-commander", "order-far"]),
        ("move-stop", [["ui", "click", f"{P}STOP"], "park"]),
        # --- G15c: the minimap's view box after the camera moves (the fixture pins the eye,
        # so release it and edge-scroll right for a second; parking the pointer stops it) ---
        ("scroll-box", ["release-eye", "edge-scroll", "wait:1.0", "park"]),
    ]


def cycle_walk(side, k):
    """One game -> shell -> game cycle (G15d), every label suffixed `#k`. Three lists:
    the exit dialogs (in game), the shell after the return (the switch to 640x480 and
    the render thread's restart happen at CHOICE1), and the game after the load — the loading
    screen between the two is its own stop (Walk.stop_loading). `EXITMENU` and
    `YESORNO` sit over the middle of the world, so the cycle runs at zoom 1."""
    P = side[:3].upper()
    sfx = f"#{k}"
    exit_stops = [
        (f"ARMOPT-exit{sfx}", ["park", ["keys", "tab"]]),
        (f"EXITMENU{sfx}", [["ui", "click", "EXIT"]]),
        (f"YESORNO{sfx}", [["ui", "click", "MAINMENU"]]),
    ]
    # CHOICE1 = "yes, to the main menu": the engine frees the game, restores 640x480
    # (0x491ADC) and pushes MAINMENU; cnc-ddraw restarts its render thread (a new
    # swapchain) on the way, which is the switch the counters are read across
    shell_stops = [(f"MAINMENU{sfx}", [["ui", "click", "CHOICE1"],
                                       ["ui", "wait", "--gui", "MAINMENU", "--timeout", "30"], "wait:3.0"])]
    shell_stops += [(f"{lbl}{sfx}", acts) for lbl, acts in SHELL_WALK[1:]]
    shell_stops += [(f"SINGLE{sfx}b", [["ui", "click", "SINGLE"]]),
                    # Mapped, like `scenario load` sets it: the gadget decides, not the registry
                    (f"SKIRMISH{sfx}b", [["ui", "click", "Skirmish"], ["ui", "set", "Mapping", "1"]])]
    game_stops = [
        (f"{P}MAIN2{sfx}", ["apply-fixture", "wait:3.0", "park"]),
        (f"{P}COM1{sfx}", ["select-commander"]),
        (f"ARMOPT{sfx}", [["keys", "tab"]]),
        (f"game-back{sfx}", [["ui", "click", "PREV", "CANCEL", "PREVMENU", "OK"], "park"]),
        # `+gamma N` (the NORMAL cheat table, handler 0x417290) is SetGamma(N/10): the
        # engine keeps presenting through 0x4BA200, which scales every entry by the
        # gamma on its way to SetEntries and never touches main+0x143A7 — so this is
        # the one play-time lever that makes the presented palette differ from the
        # engine's table (`paldiff` > 0), and the stop where the layer's palette
        # source is proven. 10 puts it back.
        (f"gamma15{sfx}", [*chat("+gamma 15"), "wait:4.0"]),     # the heartbeat is 5 s apart: let it catch the change
        (f"gamma10{sfx}", [*chat("+gamma 10"), "wait:4.0"]),
    ]
    return exit_stops, shell_stops, game_stops


def tacli(*args, check=False, timeout=180):
    cmd = [sys.executable, str(TACLI), *args]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if check and p.returncode != 0:
        raise RuntimeError(f"tacli {' '.join(args)} failed:\n{p.stdout}")
    return p.returncode, p.stdout


def instance_dir(inst):
    rc, out = tacli("ls", "--json")
    try:
        for row in json.loads(out):
            if row.get("name") == inst:
                return Path(row["gamedir"]) if "gamedir" in row else None
    except Exception:
        pass
    return None


class Walk:
    def __init__(self, inst, out, res, scenario=None, restore=False):
        self.inst, self.out, self.res = inst, Path(out), res
        self.out.mkdir(parents=True, exist_ok=True)
        self.rows = []
        self.gamedir = None
        self.log_seen = 0
        self.restore = restore
        self.scenario = scenario
        self.W, self.H = [int(v) for v in res.lower().split("x")]

    def log_size(self):
        p = self.gamedir / "tagpu.log" if self.gamedir else None
        return p.stat().st_size if p and p.exists() else 0

    def log_since(self, offset, rx):
        """Lines matching `rx` written to the instance's tagpu.log after byte `offset`
        (a `tacli log` grep sees the previous game's lines too)."""
        p = self.gamedir / "tagpu.log" if self.gamedir else None
        if not p or not p.exists():
            return []
        with p.open("rb") as f:
            f.seek(offset)
            data = f.read()
        return [l for l in data.decode("latin-1", "replace").splitlines() if re.search(rx, l)]

    def t(self, *args, **kw):
        return tacli(self.inst, *args, **kw) if args and args[0] not in ("launch", "stop", "arm", "scenario") else tacli(*args, **kw)

    def census_lines(self):
        """New `gui census:` lines since the last call, read straight from the
        instance's tagpu.log by byte offset (a tacli `log` tail slides)."""
        if not self.gamedir:
            return []
        path = self.gamedir / "tagpu.log"
        if not path.exists():
            return []
        size = path.stat().st_size
        if size < self.log_seen:          # a relaunch truncated it
            self.log_seen = 0
        with path.open("rb") as f:
            f.seek(self.log_seen)
            data = f.read()
        self.log_seen = size
        return [l for l in data.decode("latin-1", "replace").splitlines() if "gui census:" in l]

    def top_gui(self):
        rc, uiout = tacli("ui", self.inst)
        first = uiout.splitlines()[0].strip() if uiout.strip() else ""
        return first.split()[1] if first.startswith("gui ") and len(first.split()) > 1 else "?"

    def my_units(self):
        """The human player's units from the roster, the commander first. `me=` is on
        the `units:` line, not in the roster JSON."""
        me = 0
        rc, out = tacli("log", self.inst, "-g", r"^units: ")
        m = re.search(r"me=(\d+)", out.strip().splitlines()[-1] if out.strip() else "")
        if m:
            me = int(m.group(1))
        try:
            rc, out = tacli("roster", self.inst, "--json")
            units = json.loads(out).get("units", [])
        except Exception as e:
            print(f"  roster unreadable: {e}", file=sys.stderr)
            return []
        mine = [u for u in units if u.get("owner") == me]
        return sorted(mine, key=lambda u: 0 if u["type"].upper().endswith("COM") else 1)

    def select_commander(self, label):
        """Click the human player's commander so its build page comes up. The roster's
        `screen=` is the engine's 1x projection, which is where to click at zoom 1.
        Every candidate is tried until the top screen changes off the bare in-game panel."""
        cands = self.my_units()
        before = self.top_gui()
        for u in cands:
            sx, sy = u["screen"]
            tacli("click", self.inst, str(int(sx)), str(int(sy)))
            time.sleep(0.6)
            after = self.top_gui()
            if after != before or after.upper().endswith("COM1.GUI"):
                return True
        print(f"  [{label}] no build page opened (top gui {before}, {len(cands)} candidates)", file=sys.stderr)
        return False

    def park(self):
        """Put the pointer inside the viewport's bottom-right corner, off every dialog."""
        tacli("keys", self.inst, f"mouse:{self.W - 24},{self.H - 68}")

    def zoom(self, level):
        """The file lever: `tagpu_zoom.txt` in the gamedir, written atomically (the DLL
        reads it every frame). `reset` re-anchors at exactly 1x and hands the wheel back."""
        path = self.gamedir / "tagpu_zoom.txt"
        tmp = self.gamedir / "tagpu_zoom.txt.tmp"
        if level == "reset":
            tmp.write_text("1.0\n")
            os.replace(tmp, path)
            time.sleep(1.0)
            path.unlink(missing_ok=True)
            return
        tmp.write_text(f"{float(level):.3f}\n")
        os.replace(tmp, path)
        time.sleep(1.5)                  # the zoom eases over about six frames

    def order_far(self, label):
        """Right-click ground far from the commander until it walks (a click on water is
        rejected silently), so the minimap's dot and the unit move during the stop."""
        cands = self.my_units()
        if not cands:
            return False
        uid = cands[0].get("id")
        before = tuple(cands[0].get("world", (0, 0))[:2])
        W, H = self.W, self.H
        for (x, y) in ((W - 260, H - 140), (300, H - 140), (W - 260, 140), (300, 140), (W // 2, H - 120)):
            tacli("click", self.inst, str(x), str(y), "--right")
            time.sleep(6.0)              # the roster block is logged every 300 frames
            after = [u for u in self.my_units() if u.get("id") == uid]
            pos = tuple(after[0].get("world", (0, 0))[:2]) if after else before
            if pos != before:
                print(f"  [{label}] commander walking: {before} -> {pos} after a right-click at ({x},{y})", file=sys.stderr)
                return True
        print(f"  [{label}] the commander did not move (every right-click rejected?)", file=sys.stderr)
        return False

    def act(self, label, a):
        """One action: a walk verb (string) or a tacli argument list."""
        if isinstance(a, str):
            if a == "select-commander":
                self.select_commander(label)
            elif a == "hover-commander":
                cands = self.my_units()
                if cands:
                    sx, sy = cands[0]["screen"]
                    tacli("keys", self.inst, f"mouse:{int(sx)},{int(sy)}")
                    time.sleep(0.3)
            elif a == "park":
                self.park()
            elif a.startswith("zoom:"):
                self.zoom(a.split(":", 1)[1])
            elif a == "order-far":
                self.order_far(label)
            elif a == "release-eye":
                tacli("eye", self.inst, "--release")
            elif a == "edge-scroll":
                # TA's edge trigger is an equality on the outermost pixel (ta-drive skill)
                tacli("keys", self.inst, f"mouse:{self.W - 1},{self.H // 2}")
            elif a.startswith("wait:"):
                time.sleep(float(a.split(":", 1)[1]))
            elif a == "apply-fixture":
                # the fixture back onto the game the cycle just started: apply works on
                # a running game and clears the skirmish's own commanders first
                rc, out = tacli("scenario", "apply", self.inst, self.scenario, timeout=300)
                print(f"  [{label}] {out.strip().splitlines()[-1] if out.strip() else 'apply: no output'}", file=sys.stderr)
            else:
                print(f"  [{label}] unknown verb {a}", file=sys.stderr)
            return
        if a[0] == "ui" and a[1] == "click" and len(a) > 3:
            # alternatives: click the first gadget that exists on this screen.
            # Resolved BEFORE --device is appended, or the flag reads as a name.
            rc, uiout = tacli("ui", self.inst)
            present = set()
            for line in uiout.splitlines():
                parts = line.split()
                if len(parts) > 2 and parts[0].isdigit():
                    present.add(parts[2])
            pick = next((g for g in a[2:] if g in present), None)
            if pick is None:
                print(f"  [{label}] none of {a[2:]} on screen", file=sys.stderr)
                return
            a = ["ui", "click", pick]
        if CLICK_DEVICE and a[0] == "ui" and a[1] == "click":
            a = list(a) + ["--device"]
        rc, out = tacli(a[0], self.inst, *a[1:])
        if rc != 0:
            print(f"  [{label}] {' '.join(a)} -> rc={rc}: {out.strip().splitlines()[-1] if out.strip() else ''}", file=sys.stderr)

    def heartbeat(self):
        """The layer's newest `gui: twins=` line, parsed."""
        rc, out = tacli("log", self.inst, "-g", r"^gui: twins=")
        line = out.strip().splitlines()[-1] if out.strip() else ""
        hb = {k: (float(v) if "." in v else int(v)) for k, v in re.findall(r"(\w+)=([0-9.]+)", line)}
        m = re.search(r"paldiff=(\d+)@(-?\d+)", line)      # `n@first`: entries that differ, the first of them
        if m:
            hb["paldiff"] = int(m.group(1)); hb["paldiffat"] = int(m.group(2))
        return hb

    def stop_restore(self, label, actions, in_game=False):
        """G15e (`--restore`): drive the screen and let the UI atlas fill. No census,
        no shots — the measurement is the twin the DLL dumps at the end, held to the
        offline restorer by `tascene uidiff`, and the only per-stop reading is how far
        the atlas has grown and whether colour is live (`col=`, `colvalid=`)."""
        for act in actions:
            self.act(label, act)
            time.sleep(0.4)
        time.sleep(2.0)
        rc, uiout = tacli("ui", self.inst)
        screen = uiout.splitlines()[0].strip() if uiout.strip() else "?"
        hb = self.heartbeat()
        self.rows.append({"label": label, "screen": screen, "in_game": in_game, "lines": [],
                          **parse_census([]), "heartbeat": hb, "zoom": None})
        print(f"  {label:14s} {screen:40s} atlas={hb.get('atlas', '-')} col={hb.get('col', '-')} "
              f"colvalid={hb.get('colvalid', '-')} paldiff={hb.get('paldiff', '-')} "
              f"rgb={hb.get('rgb', '-')} fps={hb.get('fps', '-')}", file=sys.stderr)

    def collect_dump(self, phase, tag="gui"):
        """The DLL rewrites tagpu_restore_<tag>.{r8,rgba,idx} every time the atlas
        grows and its queue drains, so the copy taken at the end of a phase is that
        phase's fullest. Nothing here waits: `dump_if_armed` only writes when the job
        is idle, so a file that exists is a complete twin.

        TAKEN ONCE PER PHASE because the atlas does not survive the shell -> game
        context switch (`gui: atlas reset`): the shell's panels and buttons are gone
        from it by the first in-game frame, so a single copy at the end of the walk
        would diff the HUD and nothing else.

        The .pal beside them is the walk's own addition and the diff needs it: the
        twin was restored through the palette the frame is PRESENTED with, which is
        the engine's table scaled by the Gamma option, and the game's own default
        writes Gamma 15 -- a factor of 1.125, so the presented palette is NOT the
        archives' palette.pal on any stock instance. `tacli shot` writes an 8-bit PNG
        whose palette IS the presented one, so the shot taken here is the palette
        record, and `colvalid=1` in the heartbeat is the proof the twin agrees with
        it."""
        got = []
        for ext in ("r8", "rgba", "idx"):
            src = self.gamedir / f"tagpu_restore_{tag}.{ext}" if self.gamedir else None
            if src and src.exists():
                shutil.copy2(src, self.out / f"{phase}-{src.name}")
                got.append(f"{src.name} ({src.stat().st_size} bytes)")
        pal = self.out / f"{phase}-tagpu_restore_{tag}.pal"
        shot = self.out / f"{phase}-palette.png"
        tacli("shot", self.inst, "-o", str(shot))
        try:
            from PIL import Image
            im = Image.open(shot)
            if im.mode != "P":
                raise ValueError(f"the surface shot is {im.mode}, not an 8-bit palette PNG")
            rgb = im.getpalette()
            pal.write_bytes(bytes(b for i in range(256) for b in (rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2], 255)))
            got.append(pal.name)
        except Exception as e:
            print(f"  [{phase}] no presented palette: {e} — uidiff will fall back to palette.pal", file=sys.stderr)
        idx = self.out / f"{phase}-tagpu_restore_{tag}.idx"
        n = len([l for l in idx.read_text().splitlines() if l.strip()]) if idx.exists() else 0
        hb = self.heartbeat()
        print(f"dump [{phase}]: {', '.join(got) if got else 'NOTHING WRITTEN (is tagpu_restoredump.on armed?)'}"
              f" — {n} entries, colvalid={hb.get('colvalid', '-')} paldiff={hb.get('paldiff', '-')}"
              f"\n  tools/tascene uidiff {self.out}/{phase}-tagpu_restore_{tag}", file=sys.stderr)
        return n

    def stop_at(self, label, actions, in_game=False):
        if self.restore:
            return self.stop_restore(label, actions, in_game)
        if not self.rows:
            self.census_lines()             # start the first window here
        for a in actions:
            self.act(label, a)
            time.sleep(0.4)
        # the transition is where a retained UI draws, so the window starts
        # before the actions (census_lines() was drained at the end of the last
        # stop) and runs two seconds into the new screen
        time.sleep(2.0)
        lines = self.census_lines()
        rc, uiout = tacli("ui", self.inst)
        screen = uiout.splitlines()[0].strip() if uiout.strip() else "?"
        # the PGM of the last flip, and the engine's surface
        tacli("arm", self.inst, "gui_census.trigger")
        time.sleep(0.6)
        pgm_png = self.out / f"{label}-census.png"
        if self.gamedir and (self.gamedir / "tagpu_gui_census.pgm").exists():
            try:
                from PIL import Image
                Image.open(self.gamedir / "tagpu_gui_census.pgm").save(pgm_png)
            except Exception as e:
                print(f"  pgm convert failed: {e}", file=sys.stderr)
        surf = self.out / f"{label}-surface.png"
        rc, out = tacli("shot", self.inst, "-o", str(surf))
        if rc != 0 or not surf.exists():
            print(f"  [{label}] shot failed rc={rc}: {out.strip().splitlines()[-1] if out.strip() else ''}", file=sys.stderr)
        # G17b's hit check goes AFTER the surface shot, never before it: it costs
        # a snapshot round-trip, and anything between the census read and the
        # shot moves where the shot lands on the game's own timeline. It reads
        # no state the shot consumes.
        hit = hit_check(self.inst)
        summary = parse_census(lines)
        self.rows.append({"label": label, "screen": screen, "in_game": in_game, "lines": lines, **summary,
                          "hit": hit})
        kstr = f"{hit['k']:.4f}" if hit.get("k") else "-"
        # the hit half prints on every stop: it costs a snapshot, so it shows what it paid for
        hitstr = (f" | hit: k={kstr} gadgets={hit['n']} MISS={hit['miss']}"
                  f"{' [' + hit['names'] + ']' if hit['miss'] else ''}"
                  f"{' degenerate=' + str(hit['degen']) if hit['degen'] else ''}"
                  f" drift={hit['drift']}px")
        print(f"  {label:14s} {screen:40s} flips={summary['flips']:4d} changed={summary['changed']:7d} "
              f"unexplained={summary['unexplained']:7d} worst={summary['worst']}{hitstr}", file=sys.stderr)

    def stop_loading(self, label, timeout=150.0):
        """The loading screen, held until the world is alive. It is presented exactly
        ONCE: the game entry handler paints it (`0x4288D0("loadgame2bg")`) and flips, and
        nothing presents again until the map is loaded and the mode switches."""
        start = self.log_size()
        rc, out = tacli("ui", self.inst, "click", "Start")
        if rc != 0:
            print(f"  [{label}] Start -> rc={rc}: {out.strip().splitlines()[-1] if out.strip() else ''}", file=sys.stderr)
        t0 = time.time()
        alive = False
        switched = None                  # log offset of the game's mode switch
        while time.time() - t0 < timeout:
            # "alive" is read only after the game's mode switch: the overlay's `units:` line
            # keeps reporting the dead game's array from the shell (MEASURED 2026-09-07:
            # `alive=4` two seconds after Start), so the switch -- the Vulkan lane coming
            # up again at the game's resolution -- is the first sign the map has loaded,
            # and the roster after it the second
            if switched is None and self.log_since(start, r"^vk: up in \d+ ms"):
                switched = self.log_size() - 4096
            if switched is not None and self.log_since(max(switched, 0), r"units: alive=[1-9]"):
                alive = True
                break
            time.sleep(0.5)
        lines = self.census_lines()
        summary = parse_census(lines)
        row = {"label": label, "screen": "loading", "in_game": False, "lines": lines, **summary,
               "alive": alive, "load_s": round(time.time() - t0, 1)}
        self.rows.append(row)
        print(f"  {label:14s} {'loading (one presented frame)':40s} alive={alive} in {row['load_s']}s",
              file=sys.stderr)

    def report(self):
        p = self.out / "report.md"
        with p.open("w") as f:
            f.write(f"# uiwalk — {self.inst} at {self.res}\n\n")
            f.write("| stop | screen | game | censuses | changed px | unexplained px | worst census box | ops in the window |\n|---|---|---|---|---|---|---|---|\n")
            for r in self.rows:
                f.write(f"| {r['label']} | {r['screen']} | {'y' if r['in_game'] else ''} | {r['flips']} | {r['changed']} | "
                        f"{r['unexplained']} | {r['worst']} | {r['ops']} |\n")
            f.write("\n## Raw census lines\n\n")
            for r in self.rows:
                f.write(f"### {r['label']} — {r['screen']}\n\n```\n" + "\n".join(r["lines"][-12:]) + "\n```\n\n")
        (self.out / "report.json").write_text(json.dumps(self.rows, indent=1))
        print(f"report: {p}", file=sys.stderr)


def hit_check(inst):
    """Phase 2's kill rule, as arithmetic (G17b).

    For every gadget on the screen: aim a device-space click where the RENDERER
    draws it (viewport offset plus the logical point scaled by viewport/surface),
    then apply the FORK's own inverse — `(surface-1)/(vp-1)`, `dd.c`'s
    `mouse.unscale_*` — and check the logical point lands back inside the
    gadget's own rect. The two halves are computed independently on purpose: if
    we inverted the input transform instead, the test would agree with itself at
    any k and prove nothing.

    Costs one snapshot and no clicking, so it runs at every stop of every walk
    rather than only under `--device`. Rects with zero width or height cannot
    contain any point and are counted apart from the misses; the shell carries
    one (`DebugString`, height 0).
    """
    rc, out = tacli("ui", inst, "--json", "--all")
    try:
        snap = json.loads(out)
        sw, sh = snap["surface"]
        vx, vy, vw, vh = snap["viewport"]
    except Exception:
        return {"k": None, "n": 0, "miss": -1, "degen": 0, "drift": -1, "names": ""}
    if not (sw and sh and vw > 1 and vh > 1):
        return {"k": None, "n": 0, "miss": -1, "degen": 0, "drift": -1, "names": ""}
    ux, uy = (sw - 1) / (vw - 1), (sh - 1) / (vh - 1)
    miss, degen, n, drift = [], 0, 0, 0
    for g in snap.get("gadgets", []):
        r = g.get("rect")
        if not r:
            continue
        l, t, w, h = r
        if w <= 0 or h <= 0:
            degen += 1
            continue
        cx, cy = g["click"]
        dx, dy = vx + int((cx + 0.5) * vw / sw), vy + int((cy + 0.5) * vh / sh)
        gx, gy = min(int((dx - vx) * ux), sw - 1), min(int((dy - vy) * uy), sh - 1)
        drift = max(drift, abs(gx - cx), abs(gy - cy))
        n += 1
        if not (l <= gx < l + w and t <= gy < t + h):
            miss.append(g["name"])
    return {"k": vw / sw, "n": n, "miss": len(miss), "degen": degen,
            "drift": drift, "names": " ".join(miss[:6])}


def parse_census(lines):
    flips = changed = unexpl = 0
    worst = "-"
    worst_n = -1
    ops = {}
    rx = re.compile(r"flip=(\d+) n=\d+ win=\d+ changed=(\d+) unexplained=(\d+) box=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) ops=(\d+)\[(.*?)\] builds=(\d+)")
    for l in lines:
        m = rx.search(l)
        if not m:
            continue
        flips += 1
        c, u = int(m.group(2)), int(m.group(3))
        changed += c
        unexpl += u
        if u > worst_n:
            worst_n = u
            worst = f"{u} px ({m.group(4)},{m.group(5)})-({m.group(6)},{m.group(7)})" if u else "0"
        for k, v in re.findall(r"(\w+) (\d+)", m.group(9)):
            ops[k] = ops.get(k, 0) + int(v)
    opstr = " ".join(f"{k} {v}" for k, v in sorted(ops.items(), key=lambda kv: -kv[1]))
    return {"flips": flips, "changed": changed, "unexplained": unexpl, "worst": worst, "ops": opstr}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--inst", default="uiw")
    ap.add_argument("--res", default="1024x768")
    ap.add_argument("--window", default=None,
                    help="window CLIENT size when it must differ from --res, e.g. 1920x1080 with "
                         "--res 1280x720 for k = 1.5 (G17b). Sticky in the instance's meta, so it "
                         "is passed at the launch only")
    ap.add_argument("--out", default="/tmp/uiwalk")
    ap.add_argument("--side", choices=["arm", "core"], default="arm",
                    help="the human player's side: picks the in-game screens (ARM*/COR*) and the default fixture")
    ap.add_argument("--scenario", default=None, help="default: tascene-parity, or tascene-parity-core for --side core")
    ap.add_argument("--shell-only", action="store_true")
    ap.add_argument("--game-only", action="store_true")
    ap.add_argument("--screens-only", action="store_true",
                    help="stop after the in-game screens (the G15b inventory), skipping the G15c HUD, zoom and move stops")
    ap.add_argument("--keep", action="store_true", help="leave the instance running")
    ap.add_argument("--no-passes", action="store_true", help="do not arm the world passes (engine draws the world)")
    ap.add_argument("--restore", action="store_true",
                    help="G15e: arm the layer with Classic++ and tagpu_restoredump.on, walk the inventory "
                         "so the UI atlas fills with what every screen draws, and copy the dumped twin out "
                         "for `tascene uidiff`. No census and no shots")
    ap.add_argument("--device", action="store_true",
                    help="G17b: aim every gadget click in CLIENT-AREA pixels, so the walk exercises "
                         "the pointer unscale instead of handing the engine logical coordinates. "
                         "The hit check below runs either way")
    ap.add_argument("--cycles", type=int, default=0,
                    help="G15d: after the in-game stops, this many game -> shell -> game cycles in one process "
                         "(the exit dialogs, the shell inventory again, the loading screen held, the fixture re-applied)")
    a = ap.parse_args()
    if a.device:
        globals()["CLICK_DEVICE"] = True
    scenario = a.scenario or ("tascene-parity-core" if a.side == "core" else "tascene-parity")
    w = Walk(a.inst, a.out, a.res, scenario=scenario, restore=a.restore)

    tacli("stop", a.inst)
    if a.restore:
        tacli("arm", a.inst, "gui.on=log", "classicpp.on", "restoredump.on", check=True)
    else:
        tacli("arm", a.inst, "gui.on=census log pgm trace", check=True)
    if not a.no_passes:
        tacli("arm", a.inst, *ARM_SET, check=True)
    if not a.game_only:
        launch_args = ["launch", a.inst, "--res", a.res]
        if a.window:
            launch_args += ["--window", a.window]
        # A FAILED LAUNCH MUST STOP THE WALK, NOT BE WALKED THROUGH. `rc` was
        # captured here and never looked at, so a launch that did not come up
        # left every shell stop to be taken against nothing -- and a stop taken
        # against nothing is the thing this walker refuses everywhere else.
        # `check=True` raises with tacli's own output. [2026-09-16.]
        rc, out = tacli(*launch_args, timeout=300, check=True)
        print(out.strip().splitlines()[-1] if out.strip() else "", file=sys.stderr)
        w.gamedir = instance_dir(a.inst) or (TREE / "tagpu" / "instances" / a.inst / "gamedir")
        rc, out = tacli("log", a.inst, "-g", "gui: ")
        print("  " + " | ".join(l.strip() for l in out.splitlines()[-2:]), file=sys.stderr)
        tacli("ui", a.inst, "wait", "--gui", "MAINMENU", "--timeout", "30")
        for label, actions in SHELL_WALK:
            w.stop_at(label, actions)
        if a.restore:
            w.collect_dump("shell")      # the atlas does not survive the switch
    if not a.shell_only:
        # AND THE SAME FOR THE SCENARIO LOAD, which is the one that decides
        # whether there is a GAME to walk at all. Unchecked, a failed load sent
        # the walk into `game_walk` against whatever was on screen -- the shell,
        # most likely -- where the stops would find real content, diff it, and
        # report 0 px. The A/B's own guards were about telling a hole from a
        # pass, and this was a hole upstream of all of them. [2026-09-16.]
        rc, out = tacli("scenario", "load", a.inst, scenario, "--restart", "--res", a.res,
                        timeout=600, check=True)
        print(out.strip().splitlines()[-1] if out.strip() else "", file=sys.stderr)
        w.gamedir = w.gamedir or instance_dir(a.inst) or (TREE / "tagpu" / "instances" / a.inst / "gamedir")
        w.log_seen = 0
        time.sleep(3.0)
        walk = game_walk(a.side)
        if a.screens_only:
            walk = walk[:walk.index(next(s for s in walk if s[0] == "clock"))]
        for label, actions in walk:
            w.stop_at(label, actions, in_game=True)
        for k in range(1, a.cycles + 1):
            exit_stops, shell_stops, game_stops = cycle_walk(a.side, k)
            for label, actions in exit_stops:
                w.stop_at(label, actions, in_game=True)
            for label, actions in shell_stops:
                w.stop_at(label, actions)
            w.stop_loading(f"loading#{k}")
            w.log_seen = w.log_size()          # the census window restarts with the game
            time.sleep(3.0)
            for label, actions in game_stops:
                w.stop_at(label, actions, in_game=True)
            w.report()                          # a partial report survives a killed run
    if a.restore and not a.shell_only:
        w.collect_dump("game")
    w.report()
    if not a.keep:
        tacli("stop", a.inst)


if __name__ == "__main__":
    main()
