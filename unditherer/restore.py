"""One image through the stages, with the classical kernels or the learned model.

    from unditherer.restore import Options, restore_rgb, restore_file
    out, report = restore_rgb(rgb_u8, Options(preset="tuned", passes=3))

Stage order is the viewer's and restore.py's: analyze -> undither (bilateral
kernel, or the CNN which also debands) -> strength blend -> deband -> enhance
-> QA.  Every parameter the stages use is either measured from the image
(q, band step, tileability) or an explicit option; the report says which.
"""
import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from . import classical as C
from .synth import dither_consistency

LEARNED_PRESETS = {"learned": "full", "learned-tiny": "tiny"}
PRESET_HINTS = {
    "spec":  "the handover document's kernel: sigma_s 1.8 (7x7 disc), two passes -- flattens painted speckle into mud",
    "tuned": "sigma_s 1.2 (5x5 disc): still resolves a period-2 dither, keeps 3 px+ texture (the default)",
    "tight": "sigma_s 0.9 (3x3 cross): most texture kept, some grain left",
    "split": "5x5 with the range weight split -- tight on luma (0.31 sigma_r), loose on chroma (1.15 sigma_r)",
    "learned": "residual CNN 12x64 (373k params) trained to invert TA-palette dither; replaces undither + deband",
    "learned-tiny": "residual CNN 6x24 (22k params), the fragment-shader-sized version of the above",
}
METRIC_ALIASES = {"l1": "l1", "rgb-l1": "l1", "rgbl1": "l1", "rgb_l1": "l1", "rgb": "l1",
                  "split": "split", "luma-chroma": "split", "lumachroma": "split", "luma_chroma": "split", "yc": "split"}


def normalise_metric(m):
    if m is None:
        return None
    key = str(m).strip().lower()
    if key not in METRIC_ALIASES:
        raise ValueError(f"unknown metric {m!r}; use l1 (RGB L1, OpenCV's) or split (luma/chroma)")
    return METRIC_ALIASES[key]


@dataclass
class Options:
    preset: str = "tuned"            # spec | tuned | tight | split | learned | learned-tiny
    sigma_s: float = None            # kernel overrides (classical presets only)
    passes: int = None
    k: float = None                  # sigma_r = k * q
    metric: str = None               # l1 | split
    strength: float = 1.0            # blend original -> filtered
    q: float = None                  # override the measured dither amplitude
    undither: str = "on"             # on | auto (restore.py's lag-1 detector) | off
    deband: str = "auto"             # auto (restore.py's gate) | on | off
    deband_sigma: float = 6.0
    enhance: bool = False
    enhance_amount: float = 0.5
    enhance_clarity: float = 0.5
    enhance_vibrance: float = 0.6
    wrap: str = "auto"               # auto (measure tileability) | yes | no
    model: str = None                # learned presets: shipped name or .pt/.onnx path
    backend: str = "auto"            # auto | torch | onnx
    device: str = "auto"             # auto | cuda | cpu
    tile: int = 1088
    overlap: int = 32
    consistency: bool = False        # also compute the re-quantisation metric

    def __post_init__(self):
        if self.preset not in C.PRESETS and self.preset not in LEARNED_PRESETS:
            raise ValueError(f"unknown preset {self.preset!r}; one of {', '.join(list(C.PRESETS) + list(LEARNED_PRESETS))}")
        self.metric = normalise_metric(self.metric)
        for name, val, allowed in (("undither", self.undither, ("on", "auto", "off")),
                                   ("deband", self.deband, ("auto", "on", "off")),
                                   ("wrap", self.wrap, ("auto", "yes", "no"))):
            if val not in allowed:
                raise ValueError(f"--{name} must be one of {allowed}, not {val!r}")
        if not 0.0 <= self.strength <= 1.0:
            raise ValueError("strength must be within 0..1")
        if self.passes is not None and self.passes < 1:
            raise ValueError("passes must be >= 1")

    @property
    def learned(self):
        return self.preset in LEARNED_PRESETS

    def kernel(self):
        """The resolved classical kernel (None for the learned presets)."""
        if self.learned:
            return None
        kern = dict(C.PRESETS[self.preset])
        for k in ("sigma_s", "passes", "k", "metric"):
            v = getattr(self, k)
            if v is not None:
                kern[k] = v
        return kern

    def model_spec(self):
        return self.model or LEARNED_PRESETS.get(self.preset)


def _learned_restorer(opts, wrap):
    from .infer import LearnedRestorer
    return LearnedRestorer(opts.model_spec(), opts.backend, opts.device, opts.tile, opts.overlap, wrap)


def restore_rgb(rgb, opts=None, restorer=None):
    """rgb uint8 HxWx3 -> (out uint8 HxWx3, report dict)."""
    opts = opts or Options()
    t0 = time.time()
    st = C.analyze(rgb)
    q = float(opts.q) if opts.q is not None else st["q"]
    tileable = C.is_tileable(rgb) if opts.wrap == "auto" else opts.wrap == "yes"
    step_before = C.band_step(rgb)

    if opts.undither == "off":
        do_undither = False
    elif opts.undither == "auto":
        # the lag-1 detector gates the kernels; the CNN decides per pixel and always runs
        do_undither = opts.learned or bool(st["dither_present"])
    else:
        do_undither = opts.learned or q > 0

    out = rgb
    stages = []
    kern = opts.kernel()
    model_info = None
    if do_undither:
        if opts.learned:
            restorer = restorer or _learned_restorer(opts, tileable)
            filtered = restorer.restore(rgb)
            model_info = restorer.info()
            stages.append("learned")
        else:
            filtered = C.undither(rgb, q, opts.preset, wrap=tileable, **kern)
            stages.append("undither")
        out = C.blend(rgb, filtered, opts.strength)
        if opts.strength < 1.0:
            stages.append(f"strength {opts.strength:g}")

    step_after = C.band_step(out) if do_undither else step_before
    if opts.deband == "on":
        deband_on = True
    elif opts.deband == "off" or (opts.learned and do_undither):
        deband_on = False                      # the CNN already debands
    elif do_undither:
        post = C.analyze(out)                  # restore.py's gate after undithering
        deband_on = post["flat_hf_energy"] > 2.0 and step_after > 8.0
    else:
        deband_on = step_after > 8.0
    if deband_on:
        out = C.deband(out, step_after, sigma=opts.deband_sigma)
        stages.append("deband")
    if opts.enhance:
        out = C.enhance_procedural(out, opts.enhance_amount, opts.enhance_clarity, opts.enhance_vibrance)
        stages.append("enhance")

    report = {
        "preset": opts.preset,
        "options": asdict(opts),
        "analysis": st,
        "q_used": q,
        "band_step": step_before,
        "band_step_after_undither": step_after,
        "tileable": tileable,
        "distinct_colours": C.distinct_colours(rgb),
        "decisions": {"undither": do_undither, "deband": deband_on, "enhance": bool(opts.enhance), "wrap": tileable},
        "kernel": None if kern is None else {**kern, "sigma_r": kern["k"] * q, "radius": C.kernel_radius(kern["sigma_s"]),
                                             "size": C.kernel_name(kern["sigma_s"])},
        "model": model_info,
        "stages": stages,
        "qa": C.qa(rgb, out, st),
        "seconds": round(time.time() - t0, 3),
    }
    if opts.consistency:
        report["dither_consistency"] = {"fs": dither_consistency(rgb, out, True),
                                        "nearest": dither_consistency(rgb, out, False)}
    return out, report


def load_image(path):
    """-> (rgb uint8, alpha uint8 or None, info).  Colourkey pixels are inpainted
    so they do not bleed into the filters; the alpha is put back on save."""
    rgba, info = C.load_rgba(path)
    rgb, alpha = rgba[..., :3].copy(), rgba[..., 3]
    opaque = alpha > 0
    if opaque.all():
        return rgb, None, info
    rgb = cv2.inpaint(rgb, (~opaque).astype(np.uint8), 3, cv2.INPAINT_TELEA)
    return rgb, alpha, info


def save_image(path, rgb, alpha=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if alpha is not None:
        arr = np.dstack([rgb, alpha])
        arr[alpha == 0, :3] = 0
        im = Image.fromarray(arr, "RGBA")
    else:
        im = Image.fromarray(rgb, "RGB")
    if path.suffix.lower() == ".webp":
        im.save(path, lossless=True, quality=100, method=6)
    else:
        im.save(path)
    return path


def restore_file(src, dest, opts=None, restorer=None, report_path=None):
    src, dest = Path(src), Path(dest)
    rgb, alpha, info = load_image(src)
    out, report = restore_rgb(rgb, opts, restorer)
    save_image(dest, out, alpha)
    report.update({"source": str(src), "output": str(dest), "info": {**info, "size": list(info["size"])},
                   "bytes": dest.stat().st_size})
    if report_path:
        Path(report_path).write_text(json.dumps(report, indent=2, default=_json_default))
        report["report"] = str(report_path)
    return report


def analyze_file(src):
    rgb, alpha, info = load_image(src)
    st = C.analyze(rgb)
    step = C.band_step(rgb)
    return {"source": str(src), "info": {**info, "size": list(info["size"])}, "analysis": st,
            "band_step": step, "tileable": C.is_tileable(rgb), "distinct_colours": C.distinct_colours(rgb),
            "has_alpha": alpha is not None,
            "auto_decisions": {"undither": bool(st["dither_present"]),
                               "deband_if_not_undithered": bool(step > 8.0)}}


def _json_default(o):
    if isinstance(o, (np.floating, np.integer)):
        return o.item()
    if isinstance(o, np.bool_):
        return bool(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    return str(o)
