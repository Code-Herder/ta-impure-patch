#!/usr/bin/env python3
"""uiwalk — drive one instance through the UI screen inventory and collect the
Phase E measurements at every stop (research/notes/gui-renderer.md 3.11).

Two modes:

G15a, the CENSUS (default). The DLL is armed with `tagpu_gui.on=census log pgm trace`,
so at every engine flip it diffs the flipped surface against its previous copy,
subtracts every recorded op and the world viewport, and logs what nobody explained.
At each screen the walk lets the census run, pulls its lines, asks for the PGM of
the last flip and the engine's surface shot, and writes a report.

G15b/G15c, the LAYER (`--layer`). The DLL is armed with `tagpu_gui.on=strict log`: the
GL UI layer draws with the fallback off, and at each stop the walk takes the engine's
surface and our GL frame and counts differing pixels — outside the world viewport, and
inside it wherever the engine's surface is not the terrain key (a dialog, the chat, the
clock, the space popup) — plus magenta holes and the layer's heartbeat (fps, resets,
overflows, atlas). Needs numpy and PIL: run it with the venv's python.

The in-game inventory (G15c) is side-aware: `--side core` walks CORMAIN2/CORCOM1/2 on
the CORE parity fixture. After the screens it exercises the HUD extras — the `+clock`
string, the `+bps` lines, the hold-space unit popup — then the dialogs over the
viewport at zoom 0.5x and 2x (opened at 1x, zoomed by the file lever, so no click is
bent), and finally a moving commander for the minimap's dots and box.

    tools/uiwalk.py --inst uiw --res 1024x768 --out /tmp/uiwalk
    tools/uiwalk.py --inst uiw --shell-only
    ../.venv-undither/bin/python tools/uiwalk.py --inst uiw --res 1920x1080 --layer --out /tmp/layer
    ../.venv-undither/bin/python tools/uiwalk.py --inst uiwc --side core --layer --game-only --out /tmp/layer-core

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
           "zoom.on", "vpwide.on"]          # mark.on=log: its periodic line carries the live zoom
KEY = 254          # terrown's viewport fill index (tagpu_terr.c, `key=N` moves it)

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
    """Type a chat line (or a `+cheat`) the way a player does: Return, the characters, Return."""
    return [["keys", "return"], ["keys", *[f"char:{c}" for c in text]], ["keys", "return"]]


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
        # between the surface shot and the GL shot, so the stop is taken with the in-game
        # menu open: ARMOPT pauses the game and the clock with it.
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
    def __init__(self, inst, out, res, parity=False):
        self.inst, self.out, self.res = inst, Path(out), res
        self.out.mkdir(parents=True, exist_ok=True)
        self.rows = []
        self.gamedir = None
        self.log_seen = 0
        self.parity = parity
        self.W, self.H = [int(v) for v in res.lower().split("x")]

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
            else:
                print(f"  [{label}] unknown verb {a}", file=sys.stderr)
            return
        if a[0] == "ui" and a[1] == "click" and len(a) > 3:
            # alternatives: click the first gadget that exists on this screen
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
        rc, out = tacli(a[0], self.inst, *a[1:])
        if rc != 0:
            print(f"  [{label}] {' '.join(a)} -> rc={rc}: {out.strip().splitlines()[-1] if out.strip() else ''}", file=sys.stderr)

    def heartbeat(self):
        """The layer's newest `gui: twins=` line, parsed."""
        rc, out = tacli("log", self.inst, "-g", r"^gui: twins=")
        line = out.strip().splitlines()[-1] if out.strip() else ""
        return {k: (float(v) if "." in v else int(v)) for k, v in re.findall(r"(\w+)=([0-9.]+)", line)}

    def zoom_level(self):
        """The DLL's live zoom: the file lever logs nothing, but the mark pass's periodic
        line (`mark.on=log`) carries `zoom=`; the newest one is at most a few frames old."""
        rc, out = tacli("log", self.inst, "-g", r"^mark: .*zoom=")
        m = re.findall(r"zoom=([0-9.]+)", out) if out.strip() else []
        return float(m[-1]) if m else None

    def stop_at(self, label, actions, in_game=False):
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
        parity = {}
        if self.parity:
            # the GL frame right after the surface: the two are a few frames apart, so an
            # animated screen (MAINMENU's sparkles, a walking unit's minimap dot) differs
            # by the animation's motion
            rc, out = tacli("glshot", self.inst, "-o", str(self.out / f"{label}-gl.ppm"))
            cur = cursor_rect(self.inst)
            parity = frame_parity(self.out / f"{label}-gl.ppm", surf, in_game, self.res, cur)
            if in_game:
                # bracket the GL shot: a second surface shot right after it. The engine's own
                # frame moves between shots (a chat line expiring, a minimap dot, the clock),
                # and the GL frame is whichever state the layer had at its present, so the
                # diff is taken against the closer surface and the engine's own change is its
                # own column — a layer error differs from BOTH surfaces.
                surf2 = self.out / f"{label}-surface2.png"
                tacli("shot", self.inst, "-o", str(surf2))
                if surf2.exists():
                    p2 = frame_parity(self.out / f"{label}-gl.ppm", surf2, in_game, self.res, cur)
                    parity["selfdiff"] = self_diff(surf, surf2, self.res, cur)
                    if p2.get("differing", -1) >= 0 and (p2["differing"] + p2.get("vpdiff", 0)) < \
                            (parity["differing"] + parity.get("vpdiff", 0)):
                        p2["against"] = "after"
                        parity = {**parity, **p2}
        summary = parse_census(lines)
        hb = self.heartbeat() if self.parity else {}
        self.rows.append({"label": label, "screen": screen, "in_game": in_game, "lines": lines, **summary, **parity,
                          "heartbeat": hb, "zoom": self.zoom_level() if (self.parity and in_game) else None})
        extra = (f" | parity: differing={parity.get('differing', '-')} vpdiff={parity.get('vpdiff', '-')}"
                 f"/{parity.get('vpui', '-')} holes={parity.get('holes', '-')}"
                 f" {parity.get('bbox', '')}{parity.get('vpbbox', '')}"
                 f"{' self=' + str(parity['selfdiff']) if 'selfdiff' in parity else ''}"
                 f" | fps={hb.get('fps', '-')} resets={hb.get('resets', '-')}"
                 f" overflows={hb.get('overflows', '-')} atlas={hb.get('atlas', '-')}" if parity else "")
        print(f"  {label:14s} {screen:40s} flips={summary['flips']:4d} changed={summary['changed']:7d} "
              f"unexplained={summary['unexplained']:7d} worst={summary['worst']}{extra}", file=sys.stderr)

    def report(self):
        p = self.out / "report.md"
        with p.open("w") as f:
            f.write(f"# uiwalk — {self.inst} at {self.res}\n\n")
            if self.parity:
                f.write("| stop | screen | game | zoom | differing px outside the viewport | inside it, engine non-key px: differing / total | strict holes | box | engine self-diff | fps | resets | overflows | twins | atlas |\n"
                        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
                for r in self.rows:
                    hb = r.get("heartbeat", {})
                    f.write(f"| {r['label']} | {r['screen']} | {'y' if r['in_game'] else ''} | {r.get('zoom') or ''} | "
                            f"{r.get('differing', '-')} | {r.get('vpdiff', '-')} / {r.get('vpui', '-')} | "
                            f"{r.get('holes', '-')} | {r.get('bbox', '')} {r.get('vpbbox', '')} | {r.get('selfdiff', '')} | "
                            f"{hb.get('fps', '-')} | {hb.get('resets', '-')} | "
                            f"{hb.get('overflows', '-')} | {hb.get('twins', '-')} | {hb.get('atlas', '-')} |\n")
                f.write("\n")
            f.write("| stop | screen | game | censuses | changed px | unexplained px | worst census box | ops in the window |\n|---|---|---|---|---|---|---|---|\n")
            for r in self.rows:
                f.write(f"| {r['label']} | {r['screen']} | {'y' if r['in_game'] else ''} | {r['flips']} | {r['changed']} | "
                        f"{r['unexplained']} | {r['worst']} | {r['ops']} |\n")
            f.write("\n## Raw census lines\n\n")
            for r in self.rows:
                f.write(f"### {r['label']} — {r['screen']}\n\n```\n" + "\n".join(r["lines"][-12:]) + "\n```\n\n")
        (self.out / "report.json").write_text(json.dumps(self.rows, indent=1))
        print(f"report: {p}", file=sys.stderr)


def peek_values(out):
    """`tacli peek` prints one line per expression: `<expr>  <address>  <decimal> (<hex>)`."""
    return [int(m.group(1)) for m in re.finditer(r"\s(\d+) \(0x[0-9A-Fa-f]+\)\s*$", out, re.M)]


def cursor_rect(inst):
    """The engine's cursor rect from the mouse object (its last drawn position and the
    sprite record's width and height): our layer leaves it to the engine, and the flip
    draws it into the primary the surface shot reads. Padded by 8 px: the sprite is
    animated, so the record (and its size) can change frame between the two shots."""
    try:
        x, y, rec = peek_values(tacli("peek", inst, "*0x51FBD0+0x1B6:4", "*0x51FBD0+0x1BA:4",
                                      "*0x51FBD0+0x1B2:4")[1])
        w, h = peek_values(tacli("peek", inst, f"0x{rec:08X}:2", f"0x{rec + 2:08X}:2")[1])
        if not (0 < w < 128 and 0 < h < 128):
            return None
        return (x - 8, y - 8, w + 16, h + 16)
    except Exception:
        return None


def load_surface(surf_path):
    """The engine's surface shot as (RGB, palette index or None). `tacli shot` writes
    an 8-bit palette PNG, so the raw indices are there to read."""
    import numpy as np
    from PIL import Image
    im = Image.open(surf_path)
    idx = np.asarray(im).copy() if im.mode == "P" else None
    return np.asarray(im.convert("RGB")).astype(int), idx


def viewport(res):
    W, H = [int(v) for v in res.lower().split("x")]
    return 128, 32, W, H - 32           # x0, y0, x1, y1 (exclusive): the true viewport


def frame_parity(gl_path, surf_path, in_game, res, cursor=None):
    """Our presented frame against the engine's surface: differing pixels outside the
    world viewport, differing pixels inside it wherever the engine's surface is not the
    terrain key (the pixels the composite shows from the engine: a dialog, chat, the
    clock), and strict holes (magenta). The cursor rect is left to the engine."""
    try:
        import numpy as np
        from PIL import Image
        gl = np.asarray(Image.open(gl_path).convert("RGB")).astype(int)
        su, idx = load_surface(surf_path)
        if gl.shape != su.shape:
            return {"differing": -1, "holes": -1, "bbox": f"size mismatch {gl.shape} vs {su.shape}"}
        d = np.abs(gl - su).max(axis=2)
        mag = (gl[..., 0] > 200) & (gl[..., 1] < 60) & (gl[..., 2] > 200)
        outside = np.ones(d.shape, bool)
        inside = np.zeros(d.shape, bool)
        if in_game:
            x0, y0, x1, y1 = viewport(res)
            outside[y0:y1, x0:x1] = False
            if idx is not None:
                nonkey = idx != KEY
            else:                          # an RGB shot: the key is the viewport's dominant colour
                vp = su[y0:y1, x0:x1].reshape(-1, 3)
                cols, counts = np.unique(vp, axis=0, return_counts=True)
                key = cols[counts.argmax()]
                nonkey = (su != key).any(axis=2)
            inside[y0:y1, x0:x1] = nonkey[y0:y1, x0:x1]
        if cursor:
            cx, cy, cw, ch = cursor
            outside[max(cy, 0):cy + ch, max(cx, 0):cx + cw] = False
            inside[max(cy, 0):cy + ch, max(cx, 0):cx + cw] = False
        holes = int(mag.sum())
        n = int(((d > 0) & outside).sum())
        out = {"differing": n, "holes": holes, "bbox": ""}
        if in_game:
            out["vpui"] = int(inside.sum())
            out["vpdiff"] = int(((d > 0) & inside).sum())
            out["vpbbox"] = ""
        diff = (d > 0) & (outside | inside)
        if diff.any():
            ys, xs = np.nonzero((d > 0) & outside)
            if len(xs):
                out["bbox"] = f"({xs.min()},{ys.min()})-({xs.max()},{ys.max()})"
            if in_game:
                ys, xs = np.nonzero((d > 0) & inside)
                if len(xs):
                    out["vpbbox"] = f" vp({xs.min()},{ys.min()})-({xs.max()},{ys.max()})"
            Image.fromarray(diff.astype(np.uint8) * 255).save(str(gl_path).replace("-gl.ppm", "-diff.png"))
        return out
    except Exception as e:
        return {"differing": -1, "holes": -1, "bbox": f"diff failed: {e}"}


def self_diff(p1, p2, res, cursor=None):
    """Differing pixels between two engine surface shots: outside the viewport, and inside
    it where either is not the key (the UI pixels the engine draws over the world); the
    cursor rect excluded like everywhere else."""
    try:
        import numpy as np
        a, ia = load_surface(p1)
        b, ib = load_surface(p2)
        d = (np.abs(a - b).max(axis=2) > 0)
        x0, y0, x1, y1 = viewport(res)
        if ia is not None and ib is not None:
            keep = (ia != KEY) | (ib != KEY)
            d[y0:y1, x0:x1] &= keep[y0:y1, x0:x1]
        if cursor:
            cx, cy, cw, ch = cursor
            d[max(cy, 0):cy + ch, max(cx, 0):cx + cw] = False
        return int(d.sum())
    except Exception as e:
        return f"failed: {e}"


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
    ap.add_argument("--layer", action="store_true",
                    help="G15b/G15c: draw the GL UI layer (strict) instead of running the census, and diff our frame "
                         "against the engine's surface at every stop")
    a = ap.parse_args()
    scenario = a.scenario or ("tascene-parity-core" if a.side == "core" else "tascene-parity")
    w = Walk(a.inst, a.out, a.res, parity=a.layer)

    tacli("stop", a.inst)
    tacli("arm", a.inst, "gui.on=strict log" if a.layer else "gui.on=census log pgm trace", check=True)
    if not a.no_passes:
        tacli("arm", a.inst, *ARM_SET, check=True)
    if not a.game_only:
        rc, out = tacli("launch", a.inst, "--res", a.res, timeout=300)
        print(out.strip().splitlines()[-1] if out.strip() else "", file=sys.stderr)
        w.gamedir = instance_dir(a.inst) or (TREE / "tagpu" / "instances" / a.inst / "gamedir")
        rc, out = tacli("log", a.inst, "-g", "gui: ")
        print("  " + " | ".join(l.strip() for l in out.splitlines()[-2:]), file=sys.stderr)
        tacli("ui", a.inst, "wait", "--gui", "MAINMENU", "--timeout", "30")
        for label, actions in SHELL_WALK:
            w.stop_at(label, actions)
    if not a.shell_only:
        rc, out = tacli("scenario", "load", a.inst, scenario, "--restart", "--res", a.res, timeout=600)
        print(out.strip().splitlines()[-1] if out.strip() else "", file=sys.stderr)
        w.gamedir = w.gamedir or instance_dir(a.inst) or (TREE / "tagpu" / "instances" / a.inst / "gamedir")
        w.log_seen = 0
        time.sleep(3.0)
        walk = game_walk(a.side)
        if a.screens_only:
            walk = walk[:walk.index(next(s for s in walk if s[0] == "clock"))]
        for label, actions in walk:
            w.stop_at(label, actions, in_game=True)
    w.report()
    if not a.keep:
        tacli("stop", a.inst)


if __name__ == "__main__":
    main()
