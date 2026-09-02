#!/usr/bin/env python3
"""Measure each captured screenshot with restore.py's own analysis, and emit the
per-image parameters the browser prototype needs plus the QA numbers the wiki page
quotes.

restore.py derives every filter parameter from the image (see its §3). The GPU
prototype cannot run cv2, so we run the same functions here and hand the results
over in shots.json — no parameter is re-invented on the JavaScript side.

    python3 tools/undither/prep.py --shots research/notes/assets/undither/shots
"""
import argparse, json, sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from restore import (PRESETS, analyze, band_step, deband, edge_retention, enhance_procedural,
                     is_tileable, load_rgba, qa, undither)

THUMB_W = 480


def measure(path):
    rgba, info = load_rgba(path)
    rgb = rgba[..., :3].copy()

    st = analyze(rgb)
    step_raw = band_step(rgb)

    # every kernel preset, forced on (which is what the viewer does): the band step
    # left after it -- the deband gate -- and the QA numbers for undither(+deband)
    variants = {}
    for name in PRESETS:
        und = undither(rgb, st["q"], name) if st["q"] > 0 else rgb
        step_after = band_step(und)
        out_v = deband(und, step_after) if step_after > 8.0 else und
        variants[name] = {"step_after": round(step_after, 2), "deband": bool(step_after > 8.0),
                          "qa": {k: round(v, 3) for k, v in qa(rgb, out_v, st).items()}}
        if name == "spec":
            undithered, step_undithered = und, step_after

    # the learned restorer's output, if unditherer.infer has baked it (it replaces
    # undither + deband, so its QA is measured on the bake as-is)
    learned = None
    baked = path.parent.parent / "learned" / (path.stem + ".webp")
    if baked.exists():
        lrgb = np.asarray(Image.open(baked).convert("RGB"), dtype=np.uint8)
        if lrgb.shape == rgb.shape:
            learned = f"learned/{path.stem}.webp"
            variants["learned"] = {"step_after": round(band_step(lrgb), 2), "deband": False,
                                   "qa": {k: round(v, 3) for k, v in qa(rgb, lrgb, st).items()}}

    # what restore.py's own decision tree would do with this image
    decisions = {"undither": st["dither_present"], "deband": False}
    out = rgb
    if st["dither_present"]:
        out = undithered
        post = analyze(out)
        if post["flat_hf_energy"] > 2.0 and step_undithered > 8.0:
            decisions["deband"] = True
            out = deband(out, step_undithered)
    elif step_raw > 8.0:
        decisions["deband"] = True
        out = deband(out, step_raw)

    im = Image.open(path)
    colours = len(np.unique(rgb.reshape(-1, 3), axis=0))
    return {
        "file": path.name,
        "width": im.width, "height": im.height,
        "palette_mode": im.mode,
        "distinct_colours": int(colours),
        "q": round(st["q"], 2),
        "step": round(step_raw, 2),
        "step_undithered": round(step_undithered, 2),
        "lag1_autocorr": round(st["lag1_autocorr"], 3),
        "flat_fraction": round(st["flat_fraction"], 3),
        "flat_hf_energy": round(st["flat_hf_energy"], 1),
        "dither_present": st["dither_present"],
        "tileable": is_tileable(rgb),
        "decisions": decisions,
        "qa": {k: round(v, 3) for k, v in qa(rgb, out, st).items()},
        "variants": variants,
        "learned": learned,
    }


def write_thumb(path, dest):
    im = Image.open(path).convert("RGB")
    h = round(im.height * THUMB_W / im.width)
    im.resize((THUMB_W, h), Image.LANCZOS).save(dest, quality=82, method=6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", default="research/notes/assets/undither/shots")
    ap.add_argument("--refs", help="also write restore.py reference outputs here")
    ap.add_argument("--no-thumbs", action="store_true")
    args = ap.parse_args()

    shots = Path(args.shots)
    manifest = json.loads((shots / "manifest.json").read_text())
    by_file = {m["file"]: m for m in manifest if m.get("file")}

    out = []
    for name, cap in by_file.items():
        path = shots / name
        if not path.exists():
            print(f"!! missing {path}")
            continue
        rec = measure(path)
        rec.update({"env": cap["env"], "map": cap["map"], "expansion": cap["expansion"],
                    "bytes": path.stat().st_size})
        if not args.no_thumbs:
            thumbs = shots.parent / "thumbs"
            thumbs.mkdir(parents=True, exist_ok=True)
            write_thumb(path, thumbs / (path.stem + ".webp"))
            rec["thumb"] = f"thumbs/{path.stem}.webp"
        out.append(rec)
        d, q = rec["decisions"], rec["qa"]
        v = "  ".join(f"{k}:{x['step_after']:5.1f}/{x['qa']['edge_retention']:.2f}"
                      for k, x in rec["variants"].items())
        print(f"{name:22} q={rec['q']:6.1f} step={rec['step']:6.1f} ac1={rec['lag1_autocorr']:+.2f} "
              f"colours={rec['distinct_colours']:5d} undither={d['undither']} deband={d['deband']} "
              f"hf_red={q['hf_energy_reduction']:.2f} edges={q['edge_retention']:.2f} "
              f"shift={q['mean_color_shift']:.2f}  step-after/edges: {v}")

    order = [m["env"] for m in manifest]
    out.sort(key=lambda r: order.index(r["env"]))
    dest = shots.parent / "shots.json"
    dest.write_text(json.dumps(out, indent=2))
    print(f"\nwrote {dest} ({len(out)} images)")

    if args.refs:
        from restore import process
        refs = Path(args.refs)
        refs.mkdir(parents=True, exist_ok=True)
        for name in by_file:
            process(shots / name, refs)
        print(f"reference outputs -> {refs}")


if __name__ == "__main__":
    main()
