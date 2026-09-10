#!/usr/bin/env python3
"""guiart.py — the gadget art and screen definitions the GUI lab needs.

`tools/ta-guiscreen.html` draws a proposed `.GUI` screen with the engine's OWN
gadget art. That art is the original game's and can never be tracked, so this
script extracts it into a gitignored directory instead, and the lab loads it
from there. Run it once; the lab works from then on.

    .venv-undither/bin/python tools/guiart.py            # -> tmp/gafui/
    .venv-undither/bin/python tools/guiart.py --out DIR

It writes, from `anims/commongui.gaf`:

  stagebuttn1..4   the multi-stage button plates, EVERY sub-frame
  checkbox sliders listbox armprev armnext armorders armonoff armactivate
  visualsrt soundsrt musicrt speedsrt igopt armpan   the panel backgrounds

...plus `manifest.json`, the `.GUI` text of the three screens the design rests
on, and `renderrt-0.png` — a seven-recess panel spliced from `visualsrt`'s own
pixels, since no stock runtime panel has seven `h20` recesses (see the report
this prints, and gui-gadgets.md "Panel art: the recesses are the layout").

Needs the unditherer venv (PIL + numpy) and the retail install `ta3do` finds.
"""

import argparse
import importlib.util
import json
import struct
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

import numpy as np
from PIL import Image

TOOLS = Path(__file__).resolve().parent


def load_ta3do():
    loader = SourceFileLoader("ta3do", str(TOOLS / "ta3do"))
    spec = importlib.util.spec_from_loader("ta3do", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


# The frames the lab draws with. commongui.gaf is the shared art every screen
# uses -- the per-screen <name>.gaf files hold that screen's own buttons.
WANT = [
    "stagebuttn1", "stagebuttn2", "stagebuttn3", "stagebuttn4",
    "checkbox", "sliders", "listbox", "textinput",
    "armprev", "armnext", "armorders", "armbuild",
    "armonoff", "armactivate", "armcloak", "armmoveord", "armfireord",
    "visualsrt", "soundsrt", "musicrt", "speedsrt", "igopt", "armpan", "lightbar",
]
GUIS = ["guis/visualrt.gui", "guis/prefs.gui", "guis/armopt.gui"]


def entry_frames(blob):
    """entry name (lower) -> [offset of every sub-frame's data].

    ta3do.gaf_entries stops at the first frame; a stage button's states are the
    rest of them. The GAFENTRY header is 40 bytes and is followed by one
    8-byte GAFFRAMEENTRY per frame, each opening with the data pointer.
    """
    sig, count, _ = struct.unpack_from("<III", blob, 0)
    if sig != 0x00010100:
        raise SystemExit("bad GAF signature")
    out = {}
    for off in struct.unpack_from(f"<{count}I", blob, 12):
        n = struct.unpack_from("<H", blob, off)[0]
        if n <= 0:
            continue
        name = blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1").lower()
        out[name] = [struct.unpack_from("<I", blob, off + 40 + i * 8)[0] for i in range(n)]
    return out


def recesses(im, x0=13, x1=133, thr=0.78):
    """The dark inset bands a panel's art paints: y and height of each.

    They ARE the layout -- every VISUALRT gadget sits in one (gui-gadgets.md).
    """
    g = np.asarray(im.convert("L")).astype(float)[:, x0:x1].mean(axis=1)
    cut = g.mean() * thr
    runs, start = [], None
    for i, dark in enumerate(g < cut):
        if dark and start is None:
            start = i
        elif not dark and start is not None:
            if i - start > 6:
                runs.append((start, i - start))
            start = None
    if start is not None and len(g) - start > 6:
        runs.append((start, len(g) - start))
    return runs


def splice_seven(src, dst):
    """A seven-recess panel from visualsrt's own pixels.

    Its top edge, its SHADING recess band (rows 63..83) seven times at a 44 px
    pitch with its own flat ground (rows 200..222, nothing between y=171 and
    y=268) between, then its bottom edge. Indexed throughout, so the palette
    and the transparency key survive.
    """
    a = np.asarray(src)
    parts = [a[0:23]] + [a[63:84], a[200:223]] * 7 + [a[331:354]]
    out = np.concatenate(parts, axis=0)
    if out.shape != a.shape:
        raise SystemExit(f"splice came out {out.shape}, expected {a.shape}")
    im = Image.fromarray(out, mode="P")
    im.putpalette(src.getpalette())
    key = src.info.get("transparency")
    if key is not None:
        im.info["transparency"] = key
    im.save(dst)
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="tmp/gafui", help="output directory (default tmp/gafui)")
    a = ap.parse_args()

    ta3do = load_ta3do()
    assets = ta3do.Assets()
    palette = ta3do.load_palette(assets)
    flat = []
    for c in palette[:256]:
        flat += list(c)[:3]
    flat += [0] * (768 - len(flat))

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    blob = assets.read("anims/commongui.gaf")
    table = entry_frames(blob)
    manifest = {}
    for name in WANT:
        frames = table.get(name)
        if not frames:
            print(f"  MISSING {name}", file=sys.stderr)
            continue
        manifest[name] = []
        for i, off in enumerate(frames):
            fr = ta3do.gaf_frame(blob, off)
            im = Image.new("P", (fr.width, fr.height))
            im.putdata(bytes(fr.pixels))
            im.putpalette(flat)
            im.info["transparency"] = fr.transparent
            fn = f"{name}-{i}.png"
            im.save(out / fn)
            manifest[name].append({"file": fn, "w": fr.width, "h": fr.height})
        w, h = manifest[name][0]["w"], manifest[name][0]["h"]
        print(f"  {name:14s} {len(frames)} frame(s)  {w}x{h}")

    for path in GUIS:
        (out / Path(path).name).write_bytes(assets.read(path))

    print("\npanel art — the recesses ARE the layout (y, h), measured over x=13..133:")
    for name in ("visualsrt", "soundsrt", "musicrt", "speedsrt"):
        if name not in manifest:
            continue
        im = Image.open(out / f"{name}-0.png")
        r = recesses(im)
        h20 = sum(1 for _, h in r if h >= 20)
        print(f"  {name:10s} {len(r)} recesses, {h20} of them h>=20   "
              + "  ".join(f"y={y} h={h}" for y, h in r))

    if "visualsrt" in manifest:
        im = splice_seven(Image.open(out / "visualsrt-0.png"), out / "renderrt-0.png")
        print("\nrenderrt-0.png  spliced, 7 recesses:  "
              + "  ".join(f"y={y} h={h}" for y, h in recesses(im)))
        manifest["renderrt"] = [{"file": "renderrt-0.png", "w": im.width, "h": im.height}]

    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"\n{sum(len(v) for v in manifest.values())} frames + {len(GUIS)} .GUI files -> {out}/")
    print("NOTE: every file above is original game art or an original game file.")
    print("      The directory is gitignored and must stay that way.")


if __name__ == "__main__":
    main()
