#!/usr/bin/env python3
"""uiwalk — drive one instance through the UI screen inventory and collect the
Phase E measurements at every stop (research/notes/gui-renderer.md 3.11).

G15a (this version): the CENSUS. The DLL is armed with `tagpu_gui.on=census log pgm trace`,
so at every engine flip it diffs the flipped surface against its previous copy,
subtracts every recorded op and the world viewport, and logs what nobody explained.
At each screen the walk lets the census run, pulls its lines, asks for the PGM of
the last flip and the engine's surface shot, and writes a report.

    tools/uiwalk.py --inst uiw --res 1024x768 --out /tmp/uiwalk
    tools/uiwalk.py --inst uiw --shell-only

The instance is created if needed and STOPPED at the end (kept for inspection with
--keep). Everything goes through tacli; nothing here touches X.
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TACLI = HERE / "tacli"
TREE = HERE.parent

ARM_SET = ["native.on=all wrecks", "terr.on", "feat.on", "fx.on", "sfx.on", "mark.on", "order.on", "zoom.on"]

# (label, actions) — actions are tacli argument lists run in order; a `ui click`
# that fails is logged and the walk goes on (the screen is then whatever it is).
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

GAME_WALK = [
    ("ARMMAIN2", []),
    ("ARMCOM1", ["select-commander"]),
    ("ARMCOM2", [["ui", "click", "ARMNEXT"]]),
    ("ARMCOM1-back", [["ui", "click", "ARMPREV"]]),
    ("ARMOPT", [["keys", "tab"]]),
    ("PREFS", [["ui", "click", "PREFS"]]),
    ("VISUALRT", [["ui", "click", "VISUALS"]]),
    ("PREFS-back", [["ui", "click", "PREV", "CANCEL", "PREVMENU"]]),
    ("ARMOPT-back", [["ui", "click", "PREV", "CANCEL", "PREVMENU", "OK"]]),
    ("game-back", [["ui", "click", "OK", "CANCEL", "PREV"]]),
    ("chat", [["keys", "return"], ["keys", "char:h", "char:e", "char:l", "char:l", "char:o"], ["keys", "return"]]),
    ("F4", [["keys", "f4"]]),
    ("F4-close", [["keys", "f4"]]),
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

    def stop_at(self, label, actions, in_game=False):
        if not self.rows:
            self.census_lines()             # start the first window here
        for a in actions:
            if a == "select-commander":
                rc, out = tacli("roster", self.inst, "--json")
                try:
                    units = json.loads(out)
                    me = [u for u in (units.get("units") if isinstance(units, dict) else units) if u.get("mine") or u.get("owner") == units.get("me")]
                    u = me[0] if me else None
                    sx, sy = u["screen"] if u else (512, 384)
                except Exception:
                    sx, sy = 512, 384
                tacli("click", self.inst, str(int(sx)), str(int(sy)))
                time.sleep(0.5)
                continue
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
                    continue
                a = ["ui", "click", pick]
            rc, out = tacli(a[0], self.inst, *a[1:])
            if rc != 0:
                print(f"  [{label}] {' '.join(a)} -> rc={rc}: {out.strip().splitlines()[-1] if out.strip() else ''}", file=sys.stderr)
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
        rc, out = tacli("shot", self.inst, "-o", str(self.out / f"{label}-surface.png"))
        if rc != 0 or not (self.out / f"{label}-surface.png").exists():
            print(f"  [{label}] shot failed rc={rc}: {out.strip().splitlines()[-1] if out.strip() else ''}", file=sys.stderr)
        parity = {}
        if self.parity:
            # the GL frame right after the surface: the two are a few frames apart, so an
            # animated screen (MAINMENU's sparkles) differs by the animation's motion
            rc, out = tacli("glshot", self.inst, "-o", str(self.out / f"{label}-gl.ppm"))
            parity = frame_parity(self.out / f"{label}-gl.ppm", self.out / f"{label}-surface.png",
                                  self.gamedir, in_game, self.res)
        summary = parse_census(lines)
        self.rows.append({"label": label, "screen": screen, "in_game": in_game, "lines": lines, **summary, **parity})
        extra = (f" | parity: differing={parity.get('differing', '-')} holes={parity.get('holes', '-')}"
                 f" {parity.get('bbox', '')}" if parity else "")
        print(f"  {label:14s} {screen:40s} flips={summary['flips']:4d} changed={summary['changed']:7d} "
              f"unexplained={summary['unexplained']:7d} worst={summary['worst']}{extra}", file=sys.stderr)

    def report(self):
        p = self.out / "report.md"
        with p.open("w") as f:
            f.write(f"# uiwalk census — {self.inst} at {self.res}\n\n")
            if self.parity:
                f.write("| stop | screen | game | differing px (outside the viewport in game) | strict holes | box |\n|---|---|---|---|---|---|\n")
                for r in self.rows:
                    f.write(f"| {r['label']} | {r['screen']} | {'y' if r['in_game'] else ''} | {r.get('differing', '-')} | "
                            f"{r.get('holes', '-')} | {r.get('bbox', '')} |\n")
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


def cursor_rect(inst):
    """The engine's cursor rect from the mouse object: our layer leaves it to the engine."""
    try:
        g = tacli("peek", inst, "*0x51FBD0+0x1B6:4", "*0x51FBD0+0x1BA:4", "*0x51FBD0+0x1B2:4")[1]
        vals = [int(l.split()[-1].strip("()"), 16) for l in g.splitlines() if l.startswith("peek:")]
        x, y, rec = vals[0], vals[1], vals[2]
        wh = tacli("peek", inst, f"0x{rec:08X}:2", f"0x{rec + 2:08X}:2")[1]
        sz = [int(l.split()[-1].strip("()"), 16) for l in wh.splitlines() if l.startswith("peek:")]
        return (x, y, sz[0], sz[1])
    except Exception:
        return None


def frame_parity(gl_path, surf_path, gamedir, in_game, res):
    """Our presented frame against the engine's surface: differing pixels (outside the
    world viewport in game, the cursor rect excluded) and strict holes (magenta)."""
    try:
        import numpy as np
        from PIL import Image
        gl = np.asarray(Image.open(gl_path).convert("RGB")).astype(int)
        su = np.asarray(Image.open(surf_path).convert("RGB")).astype(int)
        if gl.shape != su.shape:
            return {"differing": -1, "holes": -1, "bbox": f"size mismatch {gl.shape} vs {su.shape}"}
        d = np.abs(gl - su).max(axis=2)
        mag = (gl[..., 0] > 200) & (gl[..., 1] < 60) & (gl[..., 2] > 200)
        mask = np.ones(d.shape, bool)
        if in_game:
            W, H = [int(v) for v in res.lower().split("x")]
            mask[32:H - 32, 128:W] = False           # the true viewport: the world, ours
        holes = int(mag.sum())
        n = int(((d > 0) & mask).sum())
        out = {"differing": n, "holes": holes, "bbox": ""}
        if n:
            ys, xs = np.nonzero((d > 0) & mask)
            out["bbox"] = f"({xs.min()},{ys.min()})-({xs.max()},{ys.max()})"
            Image.fromarray(((d > 0) & mask).astype(np.uint8) * 255).save(str(gl_path).replace("-gl.ppm", "-diff.png"))
        return out
    except Exception as e:
        return {"differing": -1, "holes": -1, "bbox": f"diff failed: {e}"}


def parse_census(lines):
    import re
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
    ap.add_argument("--scenario", default="tascene-parity")
    ap.add_argument("--shell-only", action="store_true")
    ap.add_argument("--game-only", action="store_true")
    ap.add_argument("--keep", action="store_true", help="leave the instance running")
    ap.add_argument("--no-passes", action="store_true", help="do not arm the world passes (engine draws the world)")
    ap.add_argument("--layer", action="store_true",
                    help="G15b: draw the GL UI layer (strict) instead of running the census, and diff our frame "
                         "against the engine's surface at every stop")
    a = ap.parse_args()
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
        rc, out = tacli("scenario", "load", a.inst, a.scenario, "--restart", "--res", a.res, timeout=600)
        print(out.strip().splitlines()[-1] if out.strip() else "", file=sys.stderr)
        w.gamedir = w.gamedir or instance_dir(a.inst) or (TREE / "tagpu" / "instances" / a.inst / "gamedir")
        w.log_seen = 0
        time.sleep(3.0)
        for label, actions in GAME_WALK:
            w.stop_at(label, actions, in_game=True)
    w.report()
    if not a.keep:
        tacli("stop", a.inst)


if __name__ == "__main__":
    main()
