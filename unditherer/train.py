#!/usr/bin/env python3
"""Train the learned undither restorer on synthesised pairs.

    python -m unditherer.train --out .data/undither-train/runs/full --depth 12 --ch 64 --steps 40000
    python -m unditherer.train --out .../runs/full-mc --init .../runs/full/best.pt \
        --mean-weight 1.0 --lr 2e-4 --steps 8000          # the shipped fine-tune

Pairs are generated on the fly in DataLoader workers from the CC0 corpus
(fetch_data.py), so every step sees fresh crops, scales and dither modes.  A
fixed validation set (held-out source images, seeded) reports PSNR against the
clean target, alongside the PSNR of the dithered input itself and of the tuned
bilateral from classical.py, so "better than the filter" is a number.

Every augmentation knob is a flag (--hue-p, --sat-min/--sat-max, --vivid-p,
--dither-modes, ...) and the run's log.json records the recipe it used.
--resume continues a run (weights, optimiser, schedule position); --init is
the fine-tune path (weights only, fresh optimiser and schedule).
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader

from .data import PairStream, bilateral_baseline, corpus_files, make_val, psnr, split
from .model import Restorer, count_params
from .paths import REPO, data_root
from .synth import DEFAULT_AUG, Aug


def add_aug_args(ap):
    g = ap.add_argument_group("augmentation (defaults = the shipped recipe)")
    d = DEFAULT_AUG
    g.add_argument("--scale-min", type=float, default=d.scale_range[0], help="min random downscale")
    g.add_argument("--scale-max", type=float, default=d.scale_range[1], help="max random downscale")
    g.add_argument("--jitter-p", type=float, default=d.jitter_p, help="p(brightness/contrast/saturation jitter)")
    g.add_argument("--sat-min", type=float, default=d.saturation[0], help="saturation jitter, low end")
    g.add_argument("--sat-max", type=float, default=d.saturation[1], help="saturation jitter, high end")
    g.add_argument("--hue-p", type=float, default=d.hue_p, help="p(rotate hue through the full circle)")
    g.add_argument("--vivid-p", type=float, default=d.vivid_p,
                   help="p(push saturation toward 255 | hue rotated) -- the recorded dead end, off by default")
    g.add_argument("--dither-modes", default=",".join(d.dither_modes),
                   help="comma list drawn uniformly per sample; repeat a mode to weight it")
    g.add_argument("--ordered-amp-min", type=float, default=d.ordered_amp[0])
    g.add_argument("--ordered-amp-max", type=float, default=d.ordered_amp[1])
    g.add_argument("--margin", type=int, default=d.margin, help="dither warm-up border (px)")


def aug_from_args(a):
    return Aug(scale_range=(a.scale_min, a.scale_max), jitter_p=a.jitter_p,
               saturation=(a.sat_min, a.sat_max), hue_p=a.hue_p, vivid_p=a.vivid_p,
               dither_modes=tuple(m.strip() for m in a.dither_modes.split(",") if m.strip()),
               ordered_amp=(a.ordered_amp_min, a.ordered_amp_max), margin=a.margin)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default=None, help=f"corpus root (default {data_root()})")
    ap.add_argument("--out", required=True, help="run directory: best.pt, last.pt, log.json")
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--ch", type=int, default=64)
    ap.add_argument("--patch", type=int, default=96)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--steps", type=int, default=40000)
    ap.add_argument("--lr", type=float, default=1e-3, help="OneCycle peak learning rate")
    ap.add_argument("--weight-decay", type=float, default=1e-5)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--val-every", type=int, default=1000)
    ap.add_argument("--val-n", type=int, default=256)
    ap.add_argument("--val-seed", type=int, default=1, help="seed of the run's own validation set")
    ap.add_argument("--split-seed", type=int, default=0, help="seed of the held-out image split")
    ap.add_argument("--seed", type=int, default=0, help="pair-stream seed (salted with the clock unless --deterministic)")
    ap.add_argument("--deterministic", action="store_true", help="no clock salt in the pair stream")
    ap.add_argument("--mean-weight", type=float, default=0.0,
                    help="weight of the block-mean consistency term on dithered samples: error "
                         "diffusion preserves the local mean, so the output must too (kills the "
                         "colour drift the plain L1 model shows on saturated dither)")
    ap.add_argument("--mean-block", type=int, default=8, help="block size of the mean-consistency term")
    ap.add_argument("--baseline-preset", default="tuned", help="classical preset reported next to val PSNR")
    ap.add_argument("--resume", help="continue a run: weights, optimiser and schedule position")
    ap.add_argument("--init", help="fine-tune: load weights only, fresh optimiser and schedule")
    ap.add_argument("--device", default="auto", help="auto | cuda | cpu")
    add_aug_args(ap)
    return ap


def pick_device(name="auto"):
    if name == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    return torch.device(name)


@torch.no_grad()
def evaluate(model, d_u8, c_u8, device, bs=32):
    model.eval()
    outs = []
    for i in range(0, len(d_u8), bs):
        x = torch.from_numpy(d_u8[i:i + bs]).permute(0, 3, 1, 2).float().div(255).to(device)
        y = model(x).clamp(0, 1).mul(255).round().byte().permute(0, 2, 3, 1).cpu().numpy()
        outs.append(y)
    model.train()
    return psnr(np.concatenate(outs), c_u8)


def git_head():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                              check=True, cwd=REPO).stdout.strip()
    except Exception:
        return None


def train(args):
    """Run one training job; returns the log dict that was written to <out>/log.json."""
    aug = aug_from_args(args)
    corpus = Path(args.corpus) if args.corpus else data_root()
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    device = pick_device(args.device)
    torch.manual_seed(args.seed)
    files = corpus_files(corpus)
    if not files:
        raise SystemExit(f"no corpus images under {corpus} (run fetch_data.py first)")
    train_files, val_files = split(files, seed=args.split_seed)
    print(f"corpus: {len(files)} images -> {len(train_files)} train / {len(val_files)} val; device {device}")

    vd, vc = make_val(val_files, args.patch, args.val_n, seed=args.val_seed, aug=aug)
    base_in = psnr(vd, vc)
    try:
        base_bl = psnr(bilateral_baseline(vd, args.baseline_preset), vc)
    except Exception as e:                       # cv2 missing, etc.
        print(f"(no classical baseline: {e})")
        base_bl = float("nan")
    print(f"val PSNR: dithered input {base_in:.2f} dB, {args.baseline_preset} bilateral {base_bl:.2f} dB")

    model = Restorer(args.depth, args.ch).to(device)
    if args.init:
        ck = torch.load(args.init, map_location=device)
        model.load_state_dict(ck["model"])
        print(f"init from {args.init} (step {ck.get('step')}, val {ck.get('val_psnr')})")
    print(f"model: depth {args.depth} ch {args.ch} params {count_params(model):,} rf {model.receptive_field()} px")
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=args.steps,
                                                pct_start=0.05, anneal_strategy="cos")
    use_amp = device.type == "cuda"
    scaler = torch.amp.GradScaler(device.type, enabled=use_amp)
    step = 0
    if args.resume:
        ck = torch.load(args.resume, map_location=device)
        model.load_state_dict(ck["model"]); opt.load_state_dict(ck["opt"]); step = ck["step"]
        for _ in range(step):
            sched.step()

    loader_kw = dict(batch_size=args.batch, num_workers=args.workers, pin_memory=device.type == "cuda")
    if args.workers > 0:
        loader_kw.update(persistent_workers=True, prefetch_factor=4)
    loader = DataLoader(PairStream(train_files, args.patch, seed=args.seed, aug=aug,
                                   deterministic=args.deterministic), **loader_kw)
    log = []
    best = -1.0
    t0 = time.time()
    started = time.strftime("%Y-%m-%d %H:%M:%S")
    run_loss = 0.0
    n_loss = 0
    summary = {"args": vars(args), "aug": aug.to_dict(), "params": count_params(model),
               "receptive_field": model.receptive_field(), "device": str(device), "git": git_head(),
               "corpus": str(corpus), "n_train_images": len(train_files), "n_val_images": len(val_files),
               "baseline_input_psnr": base_in, "baseline_bilateral_psnr": base_bl,
               "baseline_preset": args.baseline_preset, "started": started, "best_val_psnr": best, "log": log}

    def write_log():
        summary["best_val_psnr"] = best
        summary["elapsed_s"] = round(time.time() - t0)
        (out / "log.json").write_text(json.dumps(summary, indent=1))

    for x, y, isd in loader:
        if step >= args.steps:
            break
        x, y, isd = x.to(device, non_blocking=True), y.to(device, non_blocking=True), isd.to(device)
        with torch.autocast(device.type, dtype=torch.float16, enabled=use_amp):
            pred = model(x)
            loss = F.l1_loss(pred, y)
            if args.mean_weight > 0:
                # block means of output vs *input*, on dithered samples only
                b = args.mean_block
                bm = (F.avg_pool2d(pred, b) - F.avg_pool2d(x, b)).abs().mean(dim=(1, 2, 3))
                loss = loss + args.mean_weight * (bm * isd).sum() / isd.sum().clamp(min=1.0)
        opt.zero_grad(set_to_none=True)
        scaler.scale(loss).backward()
        scaler.unscale_(opt)
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        scaler.step(opt); scaler.update(); sched.step()
        step += 1
        run_loss += loss.item(); n_loss += 1
        if step % args.val_every == 0 or step == args.steps:
            v = evaluate(model, vd, vc, device)
            el = time.time() - t0
            rec = {"step": step, "loss": run_loss / max(n_loss, 1), "val_psnr": v,
                   "lr": sched.get_last_lr()[0], "elapsed_s": round(el)}
            log.append(rec)
            print(f"step {step:6d}  loss {rec['loss']:.4f}  val {v:.2f} dB  "
                  f"(input {base_in:.2f}, bilateral {base_bl:.2f})  {el / 60:.1f} min", flush=True)
            run_loss = 0.0; n_loss = 0
            state = {"model": model.state_dict(), "step": step, "depth": args.depth, "ch": args.ch,
                     "aug": aug.to_dict(), "val_psnr": v}
            torch.save({**state, "opt": opt.state_dict()}, out / "last.pt")
            if v > best:
                best = v
                torch.save(state, out / "best.pt")
            write_log()
    summary["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    write_log()
    print(f"done: best val {best:.2f} dB (input {base_in:.2f}, bilateral {base_bl:.2f}) -> {out}")
    return summary


def main(argv=None):
    args = build_parser().parse_args(argv)
    train(args)


if __name__ == "__main__":
    main()
