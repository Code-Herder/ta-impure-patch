#!/usr/bin/env python3
"""Restore 256-color (indexed) textures to smooth 32-bit RGBA.

The stages live in unditherer/classical.py; this file re-exports them so
prep.py, the wiki page and the command lines it quotes keep working.  For the
full tool -- kernel knobs, the learned models, per-file reports -- use

    python -m unditherer restore <image> [-o out] [--preset tuned|spec|tight|split|learned]

Pipeline: expand -> analyze -> undither (bilateral) -> deband -> [enhance] -> QA -> save
Every decision and metric is written to a JSON sidecar next to the output.

Usage:
    python restore.py --in textures_8bit --out textures_32bit [--enhance procedural] [--wrap auto]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from unditherer.classical import *                                        # noqa: E402,F401,F403
from unditherer.classical import (PRESETS, SPLIT_C_FRAC, SPLIT_Y_FRAC, LUMA_W, analyze, band_step,   # noqa: E402,F401
                                  deband, edge_retention, enhance_procedural, flat_mask, is_tileable,
                                  load_rgba, process, qa, undither, undither_bilateral, undither_guided,
                                  undither_split)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--enhance", choices=["none", "procedural"], default="none")
    ap.add_argument("--wrap", choices=["auto", "yes", "no"], default="auto")
    ap.add_argument("--deband-sigma", type=float, default=6.0)
    ap.add_argument("--preset", choices=sorted(PRESETS), default="spec",
                    help="undither kernel preset (default: the handover document's)")
    ap.add_argument("--sigma-s", type=float, help="override the preset's spatial sigma")
    ap.add_argument("--passes", type=int, help="override the preset's pass count")
    ap.add_argument("--k", type=float, help="override sigma_r = k*q")
    ap.add_argument("--metric", choices=["l1", "split"], help="override the range metric")
    ap.add_argument("--force-undither", action="store_true",
                    help="undither even when the lag-1 detector does not fire (what the viewer does)")
    args = ap.parse_args()
    override = {"sigma_s": args.sigma_s, "passes": args.passes, "k": args.k, "metric": args.metric}
    Path(args.out).mkdir(parents=True, exist_ok=True)
    exts = {".png", ".bmp", ".gif", ".pcx", ".tga", ".tif", ".tiff"}
    files = sorted(p for p in Path(args.inp).iterdir() if p.suffix.lower() in exts)
    if not files:
        sys.exit(f"no images found in {args.inp}")
    for p in files:
        r = process(p, args.out, args.enhance, args.wrap, args.deband_sigma,
                    args.preset, override, args.force_undither)
        d, m = r["decisions"], r["qa"]
        print(f"{p.name}: undither={d['undither']} deband={d['deband']} wrap={d['wrap']} "
              f"preset={d['preset']} q={r['q_used']:.1f} hf_reduction={m['hf_energy_reduction']:.2f} "
              f"edges={m['edge_retention']:.2f} shift={m['mean_color_shift']:.2f}")


if __name__ == "__main__":
    main()
