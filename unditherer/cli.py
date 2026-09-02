#!/usr/bin/env python3
"""unditherer -- command-line restoration of 256-colour dithered images.

    python -m unditherer restore frame.png -o frame.restored.png
    python -m unditherer restore frame.png --preset learned
    python -m unditherer restore frame.png --kernel 5x5 --metric rgb-l1 --passes 3 --k 1.1
    python -m unditherer analyze frame.png
    python -m unditherer presets

See README.md for the full parameter reference.
"""
import argparse
import json
import sys
from pathlib import Path

from . import __version__
from . import classical as C
from .restore import LEARNED_PRESETS, PRESET_HINTS, Options, analyze_file, normalise_metric, restore_file

OUT_SUFFIX = ".undithered"


def _fmt(v, d=2):
    return "-" if v is None else f"{v:.{d}f}"


def add_restore_args(ap):
    ap.add_argument("inputs", nargs="+", help="image file(s): indexed PNG, or any RGB image")
    ap.add_argument("-o", "--out", help="output file (one input) or directory (several); "
                                       f"default: <input>{OUT_SUFFIX}.png next to the input")
    ap.add_argument("--format", choices=["png", "webp"], default=None,
                    help="output format when -o is a directory or omitted (default png; webp is lossless)")
    g = ap.add_argument_group("undither stage")
    g.add_argument("--preset", "-p", default="tuned", choices=list(C.PRESETS) + list(LEARNED_PRESETS),
                   help="kernel preset or learned model (default tuned)")
    kz = g.add_mutually_exclusive_group()
    kz.add_argument("--kernel", choices=sorted(C.KERNELS), help="disc size: 3x3 / 5x5 / 7x7 (sets sigma_s)")
    kz.add_argument("--sigma-s", type=float, help="spatial sigma; OpenCV disc radius = round(1.5 sigma_s)")
    g.add_argument("--metric", help="range metric: rgb-l1 (OpenCV's |dr|+|dg|+|db|) or split (luma/chroma)")
    g.add_argument("--passes", type=int, help="bilateral passes (presets use 2)")
    g.add_argument("--k", type=float, help="sigma_r = k * q (presets use 1.3)")
    g.add_argument("--strength", type=float, default=1.0, help="blend original -> filtered, 0..1 (default 1)")
    g.add_argument("--q", type=float, help="override the measured dither amplitude q")
    g.add_argument("--undither", choices=["on", "auto", "off"], default="on",
                   help="on: always (the viewer); auto: only if the lag-1 detector fires (restore.py); off")
    g.add_argument("--wrap", choices=["auto", "yes", "no"], default="auto",
                   help="tileable-texture borders: auto measures it (default)")
    d = ap.add_argument_group("deband / enhance stages")
    d.add_argument("--deband", choices=["auto", "on", "off"], default="auto",
                   help="auto: restore.py's gate on the band step left after undithering (default)")
    d.add_argument("--deband-sigma", type=float, default=6.0, help="deband blur sigma in px (default 6)")
    d.add_argument("--enhance", action="store_true", help="procedural local contrast + vibrance (invents)")
    d.add_argument("--enhance-amount", type=float, default=0.5)
    d.add_argument("--enhance-clarity", type=float, default=0.5)
    d.add_argument("--enhance-vibrance", type=float, default=0.6)
    m = ap.add_argument_group("learned presets")
    m.add_argument("--model", help="shipped name (full, tiny) or a .pt/.onnx path; default per preset")
    m.add_argument("--backend", choices=["auto", "torch", "onnx"], default="auto",
                   help="auto: torch on CUDA if available, else onnxruntime, else torch on CPU")
    m.add_argument("--device", default="auto", help="auto | cuda | cpu (torch backend)")
    m.add_argument("--tile", type=int, default=1088, help="tile size for large images (default 1088)")
    m.add_argument("--overlap", type=int, default=32, help="tile overlap; must exceed the receptive radius")
    r = ap.add_argument_group("reporting")
    r.add_argument("--report", action="store_true", help="write <output>.json with analysis, decisions and QA")
    r.add_argument("--json", action="store_true", help="print the report(s) as JSON on stdout")
    r.add_argument("--consistency", action="store_true",
                   help="also measure how well the result re-dithers into the source (slower)")
    r.add_argument("--quiet", "-q", action="store_true")


def options_from_args(a):
    return Options(preset=a.preset,
                   sigma_s=C.KERNELS[a.kernel] if a.kernel else a.sigma_s,
                   passes=a.passes, k=a.k, metric=normalise_metric(a.metric), strength=a.strength, q=a.q,
                   undither=a.undither, deband=a.deband, deband_sigma=a.deband_sigma, enhance=a.enhance,
                   enhance_amount=a.enhance_amount, enhance_clarity=a.enhance_clarity,
                   enhance_vibrance=a.enhance_vibrance, wrap=a.wrap, model=a.model, backend=a.backend,
                   device=a.device, tile=a.tile, overlap=a.overlap, consistency=a.consistency)


def output_paths(inputs, out, fmt):
    """Resolve where each input's result goes."""
    inputs = [Path(p) for p in inputs]
    ext = "." + (fmt or "png")
    if out is None:
        return [p.with_name(p.stem + OUT_SUFFIX + ext) for p in inputs]
    out = Path(out)
    is_dir = out.is_dir() or str(out).endswith(("/", "\\")) or (len(inputs) > 1) or out.suffix.lower() not in (".png", ".webp")
    if is_dir:
        if fmt is None and out.suffix.lower() in (".png", ".webp") and len(inputs) > 1:
            pass
        return [out / (p.stem + OUT_SUFFIX + ext) for p in inputs]
    return [out]


def cmd_restore(a):
    try:
        opts = options_from_args(a)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    missing = [p for p in a.inputs if not Path(p).is_file()]
    if missing:
        print("error: no such file: " + ", ".join(missing), file=sys.stderr)
        return 1
    dests = output_paths(a.inputs, a.out, a.format)
    restorer = None
    if opts.learned:
        try:
            from .infer import LearnedRestorer
            restorer = LearnedRestorer(opts.model_spec(), opts.backend, opts.device, opts.tile, opts.overlap,
                                       wrap=(opts.wrap == "yes"))
        except Exception as e:
            print(f"error: cannot load the learned model: {e}", file=sys.stderr)
            return 1
        if not a.quiet:
            i = restorer.info()
            print(f"model {Path(i['model']).name} via {i['backend']} on {i['device']}: "
                  f"depth {i['depth']} x {i['ch']} ch, {i['params']:,} params", file=sys.stderr)
    reports = []
    for src, dest in zip(a.inputs, dests):
        rep_path = dest.with_suffix(dest.suffix + ".json") if a.report else None
        if restorer is not None and opts.wrap == "auto":
            # per-image tileability decides the wrap padding of the CNN
            from .restore import load_image
            rgb, _, _ = load_image(src)
            restorer.wrap = C.is_tileable(rgb)
        rep = restore_file(src, dest, opts, restorer, rep_path)
        reports.append(rep)
        if not a.quiet:
            print(summary_line(Path(src).name, rep, dest), file=sys.stderr)
    if a.json:
        print(json.dumps(reports if len(reports) > 1 else reports[0], indent=2, default=str))
    return 0


def summary_line(name, rep, dest):
    d, q = rep["decisions"], rep["qa"]
    if rep["kernel"]:
        k = rep["kernel"]
        how = (f"{rep['preset']} {k['size']} x{k['passes']} {k['metric']} sigma_r={k['sigma_r']:.1f}")
    else:
        how = rep["preset"]
    return (f"{name}: q={rep['q_used']:.1f} step={rep['band_step']:.1f}->{rep['band_step_after_undither']:.1f} "
            f"{how} undither={d['undither']} deband={d['deband']} "
            f"edges={q['edge_retention']:.2f} hf_red={q['hf_energy_reduction']:.2f} shift={q['mean_color_shift']:.2f} "
            f"{rep['seconds']:.2f}s -> {dest}")


def cmd_analyze(a):
    missing = [p for p in a.inputs if not Path(p).is_file()]
    if missing:
        print("error: no such file: " + ", ".join(missing), file=sys.stderr)
        return 1
    reps = [analyze_file(p) for p in a.inputs]
    if a.json:
        print(json.dumps(reps if len(reps) > 1 else reps[0], indent=2, default=str))
        return 0
    for r in reps:
        st, i = r["analysis"], r["info"]
        print(f"{Path(r['source']).name}: {i['size'][0]}x{i['size'][1]} {i['mode']}"
              f"{' (colourkey ' + str(i['colorkey_index']) + ')' if 'colorkey_index' in i else ''}"
              f" colours={r['distinct_colours']} q={st['q']:.2f} lag1={st['lag1_autocorr']:+.3f} "
              f"flat={st['flat_fraction']:.2f} hf={st['flat_hf_energy']:.1f} step={r['band_step']:.2f} "
              f"tileable={r['tileable']} detector={'dither' if st['dither_present'] else 'silent'} "
              f"-> auto: undither={r['auto_decisions']['undither']} "
              f"deband={r['auto_decisions']['deband_if_not_undithered']}")
    return 0


def cmd_presets(a):
    rows = []
    for name, p in C.PRESETS.items():
        rows.append((name, C.kernel_name(p["sigma_s"]), f"{p['sigma_s']:g}", str(p["passes"]), f"{p['k']:g}",
                     "RGB L1" if p["metric"] == "l1" else "luma/chroma", PRESET_HINTS[name]))
    for name in LEARNED_PRESETS:
        rows.append((name, "CNN", "-", "-", "-", "-", PRESET_HINTS[name]))
    if a.json:
        print(json.dumps({"classical": C.PRESETS, "learned": LEARNED_PRESETS, "hints": PRESET_HINTS,
                          "kernels": C.KERNELS, "split_fractions": {"luma": C.SPLIT_Y_FRAC, "chroma": C.SPLIT_C_FRAC}},
                         indent=2))
        return 0
    print(f"{'preset':13} {'kernel':10} {'sigma_s':7} {'passes':6} {'k':4} {'metric':11} note")
    for r in rows:
        print(f"{r[0]:13} {r[1]:10} {r[2]:7} {r[3]:6} {r[4]:4} {r[5]:11} {r[6]}")
    print("\nkernels: " + ", ".join(f"{k} = sigma_s {v:g}" for k, v in C.KERNELS.items())
          + f"; split metric: luma {C.SPLIT_Y_FRAC} sigma_r, chroma {C.SPLIT_C_FRAC} sigma_r")
    return 0


def cmd_models(a):
    from .paths import MODELS
    j = MODELS / "models.json"
    if not j.exists():
        print(f"no models.json in {MODELS} (run the pipeline's export stage)", file=sys.stderr)
        return 1
    models = json.loads(j.read_text())
    if a.json:
        print(json.dumps(models, indent=2))
        return 0
    for name, m in models.items():
        files = ", ".join(f"{k} {v / 1e6:.2f} MB" for k, v in m.get("files", {}).items())
        print(f"{name:6} depth {m['depth']} x {m['ch']} ch, {m['params']:,} params, step {m.get('step')}, "
              f"fixed-set {_fmt(m.get('eval_psnr'))} dB, from run {m.get('source_run')} ({files})")
    return 0


def build_parser():
    ap = argparse.ArgumentParser(prog="unditherer", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"unditherer {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("restore", help="undither / deband / enhance image files",
                       formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    add_restore_args(r)
    r.set_defaults(fn=cmd_restore)
    an = sub.add_parser("analyze", help="measure q, band step, the detector verdict")
    an.add_argument("inputs", nargs="+")
    an.add_argument("--json", action="store_true")
    an.set_defaults(fn=cmd_analyze)
    pr = sub.add_parser("presets", help="list the kernel presets and learned models")
    pr.add_argument("--json", action="store_true")
    pr.set_defaults(fn=cmd_presets)
    mo = sub.add_parser("models", help="list the shipped models and their provenance")
    mo.add_argument("--json", action="store_true")
    mo.set_defaults(fn=cmd_models)
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
