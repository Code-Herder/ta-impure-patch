#!/usr/bin/env python3
"""Run a trained restorer on images: PyTorch checkpoint (.pt) or ONNX (.onnx).

    python -m unditherer.infer --model full --in research/notes/assets/undither/shots \
        --out research/notes/assets/undither/learned          # bake the wiki frames

`LearnedRestorer` is the one object the CLI, the evaluator and the pipeline use:
it resolves a shipped model name or a path, picks a backend (torch on CUDA when
available, else onnxruntime, else torch on CPU), tiles large images with an
overlap wider than the receptive field, and handles tileable textures by
wrap-padding the input by the receptive radius before the network sees it.
"""
import argparse
import json
import time
from pathlib import Path

import numpy as np
from PIL import Image

from .paths import MODELS
from .synth import expand

SHIPPED = ("full", "tiny")
META_KEYS = ("name", "source_run", "step", "depth", "ch", "params", "val_psnr", "eval_psnr", "trained")


def _has(mod):
    try:
        __import__(mod)
        return True
    except Exception:
        return False


def _torch_cuda():
    try:
        import torch
        return torch.cuda.is_available()
    except Exception:
        return False


def resolve_model(spec="full", backend="auto"):
    """spec: a shipped name (full, tiny) or a path to .pt/.onnx.  Returns (Path, backend)."""
    p = Path(str(spec)).expanduser()
    if p.suffix.lower() in (".pt", ".pth", ".onnx") or p.exists():
        if not p.exists():
            raise FileNotFoundError(f"model not found: {p}")
        b = "onnx" if p.suffix.lower() == ".onnx" else "torch"
        if backend not in ("auto", b):
            raise ValueError(f"{p.name} is a {b} model; --backend {backend} cannot load it")
        return p, b
    name = str(spec)
    pt, onnx = MODELS / f"{name}.pt", MODELS / f"{name}.onnx"
    if backend == "torch":
        cands = [(pt, "torch")]
    elif backend == "onnx":
        cands = [(onnx, "onnx")]
    elif _torch_cuda():
        cands = [(pt, "torch"), (onnx, "onnx")]
    elif _has("onnxruntime"):
        cands = [(onnx, "onnx"), (pt, "torch")]
    else:
        cands = [(pt, "torch"), (onnx, "onnx")]
    for path, b in cands:
        if path.exists() and (b == "onnx" and _has("onnxruntime") or b == "torch" and _has("torch")):
            return path, b
    raise FileNotFoundError(f"no loadable model for {spec!r} (looked for {pt.name} / {onnx.name} in {MODELS}; "
                            f"torch={_has('torch')}, onnxruntime={_has('onnxruntime')})")


class LearnedRestorer:
    """rgb uint8 HxWx3 in -> rgb uint8 HxWx3 out, through the residual CNN."""

    def __init__(self, spec="full", backend="auto", device="auto", tile=1088, overlap=32, wrap=False):
        self.path, self.backend = resolve_model(spec, backend)
        self.tile, self.overlap, self.wrap = int(tile), int(overlap), bool(wrap)
        if self.tile - 2 * self.overlap <= 0:
            raise ValueError("tile must exceed twice the overlap")
        self.meta = {}
        if self.backend == "torch":
            self._init_torch(device)
        else:
            self._init_onnx(device)
        if self.overlap < self.depth:
            raise ValueError(f"overlap {self.overlap} is narrower than the receptive radius {self.depth}")

    def _init_torch(self, device):
        import torch
        from .model import Restorer, count_params
        dev = torch.device("cuda" if torch.cuda.is_available() else "cpu") if device == "auto" else torch.device(device)
        ck = torch.load(self.path, map_location=dev, weights_only=False)
        m = Restorer(ck["depth"], ck["ch"]).to(dev)
        m.load_state_dict(ck["model"])
        m.eval()
        self.model, self.device = m, str(dev)
        self.depth, self.ch, self.params, self.step = ck["depth"], ck["ch"], count_params(m), ck.get("step")
        self.meta = {k: ck.get(k) for k in META_KEYS if ck.get(k) is not None}

    def _init_onnx(self, device):
        import onnxruntime as ort
        avail = ort.get_available_providers()
        providers = ["CPUExecutionProvider"]
        if device in ("auto", "cuda") and "CUDAExecutionProvider" in avail:
            providers = ["CUDAExecutionProvider"] + providers
        so = ort.SessionOptions()
        so.log_severity_level = 3
        self.sess = ort.InferenceSession(str(self.path), so, providers=providers)
        md = dict(self.sess.get_modelmeta().custom_metadata_map)
        self.meta = {k: md[k] for k in META_KEYS if k in md}
        self.depth, self.ch = int(md.get("depth", 0)), int(md.get("ch", 0))
        self.params = int(md.get("params", 0))
        self.step = int(md["step"]) if md.get("step") else None
        self.device = self.sess.get_providers()[0].replace("ExecutionProvider", "").lower()
        if not self.depth:
            raise ValueError(f"{self.path.name} carries no depth metadata (export it with unditherer.infer.export_onnx)")

    def receptive_field(self):
        return 2 * self.depth + 1

    def info(self):
        d = {"model": str(self.path), "backend": self.backend, "device": self.device,
             "depth": self.depth, "ch": self.ch, "params": self.params, "step": self.step,
             "receptive_field": self.receptive_field(), "tile": self.tile, "overlap": self.overlap,
             "wrap": self.wrap}
        d.update({k: v for k, v in self.meta.items() if k not in d})
        return d

    # ---- raw NCHW float32 in [0,1] -----------------------------------------
    def _run(self, x):
        if self.backend == "torch":
            import torch
            with torch.no_grad():
                return self.model(torch.from_numpy(x).to(self.device)).float().cpu().numpy()
        return self.sess.run(None, {"rgb": x})[0]

    def _run_tiled(self, x):
        _, _, h, w = x.shape
        if max(h, w) <= self.tile:
            return self._run(x)
        ov, step = self.overlap, self.tile - 2 * self.overlap
        y = np.zeros_like(x)
        for ty in range(0, h, step):
            for tx in range(0, w, step):
                y0, x0 = max(0, ty - ov), max(0, tx - ov)
                y1, x1 = min(h, ty + step + ov), min(w, tx + step + ov)
                out = self._run(np.ascontiguousarray(x[:, :, y0:y1, x0:x1]))
                cy1, cx1 = min(h, ty + step), min(w, tx + step)
                y[:, :, ty:cy1, tx:cx1] = out[:, :, ty - y0:cy1 - y0, tx - x0:cx1 - x0]
        return y

    @staticmethod
    def _to_u8(y):
        return np.clip(np.rint(y * 255.0), 0, 255).astype(np.uint8)

    def restore(self, rgb_u8):
        h, w = rgb_u8.shape[:2]
        pad = self.depth if self.wrap else 0
        rgb = np.pad(rgb_u8, ((pad, pad), (pad, pad), (0, 0)), mode="wrap") if pad else rgb_u8
        x = np.ascontiguousarray(rgb.astype(np.float32).transpose(2, 0, 1)[None] / 255.0)
        y = self._to_u8(self._run_tiled(x)[0].transpose(1, 2, 0))
        return y[pad:pad + h, pad:pad + w] if pad else y

    def restore_batch(self, nhwc_u8, bs=32):
        outs = []
        for i in range(0, len(nhwc_u8), bs):
            x = np.ascontiguousarray(nhwc_u8[i:i + bs].astype(np.float32).transpose(0, 3, 1, 2) / 255.0)
            outs.append(self._to_u8(self._run(x).transpose(0, 2, 3, 1)))
        return np.concatenate(outs)


# --------------------------------------------------------------------------- export

def export_onnx(ckpt, out, meta=None, opset=17):
    """Checkpoint -> single-file ONNX with the model's identity in metadata_props.
    Returns a dict with the torch-vs-onnxruntime parity on a random input."""
    import onnx
    import torch
    from .model import Restorer, count_params
    ck = torch.load(ckpt, map_location="cpu", weights_only=False)
    m = Restorer(ck["depth"], ck["ch"])
    m.load_state_dict(ck["model"])
    m.eval()
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(1, 3, 64, 64)
    torch.onnx.export(m, dummy, str(out), input_names=["rgb"], output_names=["out"],
                      dynamic_axes={"rgb": {0: "n", 2: "h", 3: "w"}, "out": {0: "n", 2: "h", 3: "w"}},
                      opset_version=opset, dynamo=False)
    model = onnx.load(str(out))
    props = {"depth": ck["depth"], "ch": ck["ch"], "params": count_params(m), "step": ck.get("step"),
             "val_psnr": ck.get("val_psnr")}
    props.update(meta or {})
    del model.metadata_props[:]
    for k, v in props.items():
        if v is None:
            continue
        e = model.metadata_props.add()
        e.key, e.value = str(k), str(v)
    onnx.checker.check_model(model)
    onnx.save(model, str(out))
    sidecar = out.with_suffix(out.suffix + ".data")
    if sidecar.exists():
        raise RuntimeError(f"export produced external data ({sidecar}); expected a single file")
    # parity
    rng = np.random.default_rng(0)
    x = rng.random((1, 3, 80, 96), dtype=np.float32)
    with torch.no_grad():
        ref = m(torch.from_numpy(x)).numpy()
    diff = None
    try:
        import onnxruntime as ort
        sess = ort.InferenceSession(str(out), providers=["CPUExecutionProvider"])
        diff = float(np.abs(sess.run(None, {"rgb": x})[0] - ref).max())
    except ImportError:
        pass
    return {"onnx": str(out), "bytes": out.stat().st_size, "max_abs_diff_vs_torch": diff, **props}


# --------------------------------------------------------------------------- the frames bake

def bake_frames(restorer, inp, out, png=False, verbose=True):
    """Restore every indexed PNG in `inp` (a dir or one file) into `out` as
    lossless WebP (or PNG) and write model.json, the file the wiki viewer reads."""
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    inp = Path(inp)
    files = sorted(p for p in (inp.iterdir() if inp.is_dir() else [inp])
                   if p.suffix.lower() == ".png" and "mapfield" not in p.name)
    info = restorer.info()
    meta = {"checkpoint": info["model"], "backend": info["backend"], "step": info.get("step"),
            "depth": info["depth"], "ch": info["ch"], "params": info["params"],
            "val_psnr": _float(info.get("val_psnr")), "eval_psnr": _float(info.get("eval_psnr")),
            "name": info.get("name"), "files": {}}
    for p in files:
        rgb = expand(p)
        t = time.time()
        y = restorer.restore(rgb)
        dt = time.time() - t
        dest = out / (p.stem + (".png" if png else ".webp"))
        if png:
            Image.fromarray(y).save(dest, optimize=True)
        else:
            Image.fromarray(y).save(dest, lossless=True, quality=100, method=6)
        shift = float(np.abs(y.reshape(-1, 3).mean(0, dtype=np.float64) - rgb.reshape(-1, 3).mean(0, dtype=np.float64)).max())
        meta["files"][p.name] = {"out": dest.name, "bytes": dest.stat().st_size, "seconds": round(dt, 3),
                                 "mean_colour_shift": round(shift, 2)}
        if verbose:
            print(f"{p.name:22} {dt * 1000:6.0f} ms  shift {shift:.2f}  -> {dest.name} {dest.stat().st_size / 1e6:.2f} MB")
    (out / "model.json").write_text(json.dumps(meta, indent=1))
    return meta


def _float(v):
    try:
        return None if v is None else float(v)
    except (TypeError, ValueError):
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="full", help="shipped name (full, tiny) or a .pt/.onnx path")
    ap.add_argument("--backend", default="auto", choices=["auto", "torch", "onnx"])
    ap.add_argument("--device", default="auto")
    ap.add_argument("--in", dest="inp", required=True, help="an indexed PNG or a directory of them")
    ap.add_argument("--out", required=True)
    ap.add_argument("--png", action="store_true", help="write PNG instead of lossless WebP")
    ap.add_argument("--wrap", action="store_true", help="wrap padding (tileable textures)")
    ap.add_argument("--tile", type=int, default=1088)
    ap.add_argument("--overlap", type=int, default=32)
    args = ap.parse_args(argv)
    r = LearnedRestorer(args.model, args.backend, args.device, args.tile, args.overlap, args.wrap)
    print(f"model {r.path.name} via {r.backend} on {r.device}: depth {r.depth} ch {r.ch} params {r.params:,}")
    bake_frames(r, args.inp, args.out, png=args.png)


if __name__ == "__main__":
    main()
