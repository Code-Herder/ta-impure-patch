#!/usr/bin/env python3
"""Where is the water? Walk the camera over a map and classify the terrain.

    promo/survey-map.py <instance> <outdir> [--step 1500] [--x0 N --x1 N --y0 N --y1 N]

A naval scenario that puts a fleet one row too far east beaches it, and a beached
rank of ships sits there in formation for the whole clip — which is exactly what
happened to `naval-push` and `shore-raid` on Anteer Strait. Guessing at
coordinates does not work and `scenario validate` cannot help: it checks the
schema, not the shoreline.

So ask the map. This holds the camera at a grid of positions, takes a `glshot` at
each (the fork's own FBO, so it is correct whether or not the window is visible),
classifies every pixel as water or land, and writes:

  <outdir>/survey-<map>.png    the stitched terrain, land white / water black
  <outdir>/survey-<map>.txt    a coarse world-coordinate grid, `.` water `#` land

Run it on an instance that is ALREADY LOADED on the map you care about, with as
few units as possible — units and explosions classify as land, so a survey taken
during a battle maps the battle, not the shore.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

PANEL = 128              # TA's side panel, in game px, at a 2048-wide client


def tacli(*args: str) -> str:
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=True).stdout.strip()
    out = subprocess.run([f"{root}/tools/tacli", *args],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit(f"tacli {' '.join(args)}: {out.stderr.strip()}")
    return out.stdout


def instance(name: str) -> dict:
    for i in json.loads(tacli("ls", "--json")):
        if i["name"] == name:
            return i
    raise SystemExit(f"no instance {name!r}")


def is_water(a: np.ndarray) -> np.ndarray:
    """TA's sea is saturated blue; everything else here is not."""
    r, g, b = a[:, :, 0].astype(int), a[:, :, 1].astype(int), a[:, :, 2].astype(int)
    return (b > 110) & (b > r + 35) & (b > g + 25)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("instance")
    ap.add_argument("outdir")
    ap.add_argument("--step", type=int, default=1400)
    ap.add_argument("--x0", type=int, default=1200)
    ap.add_argument("--x1", type=int, default=5200)
    ap.add_argument("--y0", type=int, default=1200)
    ap.add_argument("--y1", type=int, default=4000)
    ap.add_argument("--cell", type=int, default=200,
                    help="world units per character in the text map")
    args = ap.parse_args()

    inst = instance(args.instance)
    if not inst.get("running"):
        raise SystemExit(f"{args.instance} is not running — load the map first")
    want = tuple(inst["res"])
    gamedir = Path(inst["gamedir"])
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    # world -> cell grid, accumulated across every shot
    nx = (args.x1 - args.x0) // args.cell + 1
    ny = (args.y1 - args.y0) // args.cell + 1
    land_hits = np.zeros((ny, nx), dtype=np.int64)
    seen = np.zeros((ny, nx), dtype=np.int64)

    shots = 0
    for cy in range(args.y0, args.y1 + 1, args.step):
        for cx in range(args.x0, args.x1 + 1, args.step):
            tacli("eye", args.instance, str(cx), str(cy))
            tacli("glshot", args.instance)
            ppm = gamedir / "tagpu_gl.ppm"
            if not ppm.exists():
                print(f"  ({cx},{cy}) no glshot", file=sys.stderr)
                continue
            a = np.asarray(Image.open(ppm).convert("RGB"))
            h, w = a.shape[:2]
            if (w, h) != want:
                raise SystemExit(
                    f"glshot came back {w}x{h}, not {want[0]}x{want[1]} — the game "
                    f"is in the 640x480 SHELL, not in a map. An empty scenario "
                    f"does that: with no units alive TA ends the game instantly, "
                    f"so survey with clear_existing false.")
            water = is_water(a)
            # The eye is the top-left of the view; the game area starts after the
            # side panel and is 1:1 with world units (ta-capture rule 4).
            panel = int(PANEL * w / 2048)
            # `tacli eye X Y` sets the world coordinate at the LEFT EDGE OF THE
            # GAME AREA (just right of the side panel) and the TOP of the view --
            # it is the eye, not the centre. Verified against `tacli roster`,
            # whose own `eye` is the world coordinate at screen x=0, one panel
            # width smaller: eye(4200,4200) reads back as (4072,4210).
            ys, xs = np.mgrid[0:h, panel:w]
            wx = (xs - panel) + cx
            wy = ys + cy
            gx = (wx - args.x0) // args.cell
            gy = (wy - args.y0) // args.cell
            ok = (gx >= 0) & (gx < nx) & (gy >= 0) & (gy < ny)
            wsub = water[:, panel:]
            np.add.at(seen, (gy[ok], gx[ok]), 1)
            np.add.at(land_hits, (gy[ok], gx[ok]), (~wsub)[ok].astype(np.int64))
            shots += 1
            print(f"  ({cx},{cy}) water {wsub.mean()*100:.0f}%")

    if not shots:
        raise SystemExit("no shots taken")

    frac = np.where(seen > 0, land_hits / np.maximum(seen, 1), np.nan)
    mp = inst.get("name", "map")
    txt = [f"# survey of {args.instance}: '.' water  '#' land  '?' unseen",
           f"# cell = {args.cell} world units; columns x={args.x0}..{args.x1}, "
           f"rows y={args.y0}..{args.y1}", ""]
    header = "      " + "".join(
        f"{(args.x0 + i * args.cell) // 100 % 10}" for i in range(nx))
    txt.append(header)
    for j in range(ny):
        row = "".join("?" if np.isnan(frac[j, i]) else
                      ("#" if frac[j, i] > 0.35 else ".") for i in range(nx))
        txt.append(f"{args.y0 + j * args.cell:5d} {row}")
    (out / f"survey-{mp}.txt").write_text("\n".join(txt) + "\n")

    img = np.where(np.isnan(frac), 0.5, frac)
    Image.fromarray((img * 255).astype(np.uint8)).resize(
        (nx * 6, ny * 6), Image.NEAREST).save(out / f"survey-{mp}.png")
    print(f"\n{out}/survey-{mp}.txt")
    print("\n".join(txt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
