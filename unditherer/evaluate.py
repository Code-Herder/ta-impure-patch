#!/usr/bin/env python3
"""Score models on ONE fixed synthetic validation set, so numbers from different
training runs are comparable (train.py's own validation set follows whatever
augmentation was current when that run started).

    python -m unditherer.evaluate runs/full/best.pt runs/tiny/best.pt full tiny \
        --json runs/eval.json

Arguments are checkpoints (.pt), ONNX exports (.onnx) or shipped model names
(full, tiny).  The 512-patch / seed-1234 set is the one every number in
LEARNINGS.md and the wiki page was measured on.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from .data import bilateral_baseline, corpus_files, make_val, psnr, split
from .paths import data_root
from .synth import DEFAULT_AUG


def fixed_val(corpus=None, patch=96, n=512, seed=1234, split_seed=0, aug=DEFAULT_AUG):
    corpus = Path(corpus) if corpus else data_root()
    files = corpus_files(corpus)
    if not files:
        raise SystemExit(f"no corpus images under {corpus}")
    _, val_files = split(files, seed=split_seed)
    return make_val(val_files, patch, n, seed=seed, aug=aug)


def evaluate_models(specs, corpus=None, patch=96, n=512, seed=1234, device="auto",
                    presets=("tuned",), aug=DEFAULT_AUG, verbose=True):
    """Returns {"n","patch","seed","input", "bilateral_<preset>": dB, <spec>: {...}}."""
    from .infer import LearnedRestorer
    vd, vc = fixed_val(corpus, patch, n, seed, aug=aug)
    res = {"n": n, "patch": patch, "seed": seed, "input": psnr(vd, vc)}
    line = f"val ({n} patches, seed {seed}): input {res['input']:.2f} dB"
    for p in presets:
        res[f"bilateral_{p}"] = psnr(bilateral_baseline(vd, p), vc)
        line += f", {p} bilateral {res[f'bilateral_{p}']:.2f} dB"
    if verbose:
        print(line)
    for spec in specs:
        r = LearnedRestorer(spec, device=device)
        out = r.restore_batch(vd)
        v = psnr(out, vc)
        res[str(spec)] = {"psnr": v, **r.info()}
        if verbose:
            i = r.info()
            print(f"{spec}: {v:.2f} dB  (depth {i['depth']} ch {i['ch']} params {i['params']:,} "
                  f"step {i.get('step')} via {i['backend']})")
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="+", help="checkpoint / .onnx paths or shipped names (full, tiny)")
    ap.add_argument("--corpus", default=None)
    ap.add_argument("--patch", type=int, default=96)
    ap.add_argument("--n", type=int, default=512)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--presets", default="tuned", help="classical presets to score too (comma list)")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--json", help="write results here")
    args = ap.parse_args(argv)
    res = evaluate_models(args.models, args.corpus, args.patch, args.n, args.seed, args.device,
                          tuple(p for p in args.presets.split(",") if p))
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
