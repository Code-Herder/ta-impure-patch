"""A GPU compositor for the montage, as a prototype.

The idea it tests: every tile at a given camera position is the SAME size and
samples one of a handful of source images, so the whole frame is one batched
bilinear gather out of a stacked source tensor -- about fifteen kernel launches
per frame instead of one PIL resize per tile.

Minification is handled the way a GPU handles it: reduce the stacked sources to
the mip level where the remaining ratio is in [1, 2) and sample bilinear there.
That is what replaces PIL's support-scaled filter, whose cost grows as the
square of the reduction ratio -- which is where the CPU path's time goes.
"""
import numpy as np
import torch
import torch.nn.functional as F


class GpuCompositor:
    def __init__(self, device="cuda", dtype=torch.float16, mode="resize"):
        self.dev = torch.device(device)
        self.dtype = dtype
        self.mode = mode
        self._src_cache = {}          # key -> uint8 tensor [3, H, W] on device
        self._pyr_cache = {}          # (size-class, level) -> stacked tensor

    # ---- source upload -------------------------------------------------
    def _upload(self, key, pil):
        t = self._src_cache.get(key)
        if t is None:
            a = np.asarray(pil.convert("RGB"))
            t = torch.from_numpy(np.ascontiguousarray(a)).to(
                self.dev, non_blocking=True)
            self._src_cache[key] = t
        return t                                   # [H, W, 3] uint8

    def evict(self, keep_keys):
        for k in list(self._src_cache):
            if k not in keep_keys:
                del self._src_cache[k]

    # ---- the frame -----------------------------------------------------
    def frame(self, sources, tiles, flats, bg, w, h):
        dev = self.dev
        canvas = torch.empty((h, w, 3), dtype=self.dtype, device=dev)
        canvas[:] = torch.tensor(bg, dtype=self.dtype, device=dev)

        for col, X0, Y0, X1, Y1 in flats:
            x0, y0 = max(0, X0), max(0, Y0)
            x1, y1 = min(w, X1), min(h, Y1)
            if x1 > x0 and y1 > y0:
                canvas[y0:y1, x0:x1] = torch.tensor(
                    col, dtype=self.dtype, device=dev)

        # Group by destination size: within one zoom there is normally exactly
        # one group, which is the whole point.
        groups = {}
        for tl in tiles:
            key, X0, Y0, X1, Y1 = tl[0], tl[1], tl[2], tl[3], tl[4]
            groups.setdefault((X1 - X0, Y1 - Y0, key[2], key[3]), []).append(tl)

        for (dw, dh, cw, ch), grp in groups.items():
            self._draw_group(canvas, sources, grp, dw, dh, w, h)
        return canvas

    def _draw_group(self, canvas, sources, grp, dw, dh, w, h):
        dev = self.dev
        keys = sorted({t[0] for t in grp}, key=lambda k: str(k))
        kidx = {k: i for i, k in enumerate(keys)}
        stack = torch.stack([self._upload(k, sources[k]) for k in keys])  # [S,H,W,3]
        S, sh, sw, _ = stack.shape

        # Prefilter the whole stack ONCE to the tile's own scale, with a
        # support-scaled filter -- the same thing PIL's resize does, and the
        # reason the naive mip+bilinear path sat at 36 dB: a power-of-two mip
        # leaves a residual ratio of up to 2x that plain bilinear cannot filter.
        # Every tile in a group shares the scale (they are all the same window
        # at the same zoom), so this costs ONE resize for the whole group and
        # leaves the per-tile gather at ~1:1, where bilinear is exact enough.
        span_x = float(grp[0][7] - grp[0][5])
        span_y = float(grp[0][8] - grp[0][6])
        if self.mode == "resize" and span_x > 0 and span_y > 0:
            fx, fy = dw / span_x, dh / span_y
            tw, th = max(1, round(sw * fx)), max(1, round(sh * fy))
            if (tw, th) != (sw, sh):
                x = stack.permute(0, 3, 1, 2).to(self.dtype)
                x = F.interpolate(x, size=(th, tw), mode="bilinear",
                                  align_corners=False, antialias=(tw < sw))
                mip = x.permute(0, 2, 3, 1).contiguous()
            else:
                mip = stack.to(self.dtype)
            mh, mw = mip.shape[1], mip.shape[2]
            fu, fv = sw / mw, sh / mh
        else:
            ratio = sw / dw
            level = max(0, int(np.floor(np.log2(max(1.0, ratio)))))
            if level:
                x = stack.permute(0, 3, 1, 2).to(self.dtype)
                x = F.avg_pool2d(x, 2 ** level)
                mip = x.permute(0, 2, 3, 1).contiguous()
            else:
                mip = stack.to(self.dtype)
            mh, mw = mip.shape[1], mip.shape[2]
            fu = fv = 2.0 ** level

        n = len(grp)
        X0 = torch.tensor([t[1] for t in grp], device=dev, dtype=torch.int32)
        Y0 = torch.tensor([t[2] for t in grp], device=dev, dtype=torch.int32)
        sx0 = torch.tensor([t[5] for t in grp], device=dev, dtype=torch.float32)
        sy0 = torch.tensor([t[6] for t in grp], device=dev, dtype=torch.float32)
        sx1 = torch.tensor([t[7] for t in grp], device=dev, dtype=torch.float32)
        sy1 = torch.tensor([t[8] for t in grp], device=dev, dtype=torch.float32)
        alpha = torch.tensor([t[9] for t in grp], device=dev, dtype=torch.float32)
        sid = torch.tensor([kidx[t[0]] for t in grp], device=dev, dtype=torch.int64)

        jx = torch.arange(dw, device=dev, dtype=torch.float32)
        jy = torch.arange(dh, device=dev, dtype=torch.float32)
        # PIL's box semantics: output pixel j covers source span
        # [sx0 + j*du, sx0 + (j+1)*du); its centre samples at (j+0.5)*du.
        du = ((sx1 - sx0) / dw)[:, None]
        dv = ((sy1 - sy0) / dh)[:, None]
        u = (sx0[:, None] + (jx[None, :] + 0.5) * du) / fu     # [n, dw]
        v = (sy0[:, None] + (jy[None, :] + 0.5) * dv) / fv     # [n, dh]

        # bilinear taps, in mip pixel coordinates (pixel i is centred at i+0.5)
        pu = (u - 0.5).clamp(0, mw - 1)
        pv = (v - 0.5).clamp(0, mh - 1)
        u0 = pu.floor(); v0 = pv.floor()
        wu = (pu - u0)[:, None, :]                              # [n,1,dw]
        wv = (pv - v0)[:, :, None]                              # [n,dh,1]
        u0 = u0.long().clamp(0, mw - 1); u1 = (u0 + 1).clamp(0, mw - 1)
        v0 = v0.long().clamp(0, mh - 1); v1 = (v0 + 1).clamp(0, mh - 1)

        flat = mip.view(S * mh * mw, 3)
        base = (sid * (mh * mw))[:, None, None]
        def tap(vi, ui):
            idx = base + vi[:, :, None] * mw + ui[:, None, :]   # [n,dh,dw]
            return flat[idx.reshape(-1)].view(n, dh, dw, 3).to(torch.float32)
        a = tap(v0, u0); b = tap(v0, u1); c = tap(v1, u0); d = tap(v1, u1)
        top = a + (b - a) * wu[..., None]
        bot = c + (d - c) * wu[..., None]
        out = top + (bot - top) * wv[..., None]                 # [n,dh,dw,3]

        # Scatter. Tiles never overlap (the stage is a grid with gaps), so a
        # plain index_put_ is safe -- no accumulation, no race.
        dx = X0[:, None, None].long() + torch.arange(dw, device=dev)[None, None, :]
        dy = Y0[:, None, None].long() + torch.arange(dh, device=dev)[None, :, None]
        inside = (dx >= 0) & (dx < w) & (dy >= 0) & (dy < h)
        lin = (dy.clamp(0, h - 1) * w + dx.clamp(0, w - 1))
        lin = lin.expand(n, dh, dw).reshape(-1)
        keep = inside.expand(n, dh, dw).reshape(-1)
        vals = out.reshape(-1, 3)
        av = alpha[:, None, None, None].expand(n, dh, dw, 3).reshape(-1, 3)
        flatc = canvas.view(-1, 3)
        if float(alpha.min()) < 1.0:
            under = flatc[lin].to(torch.float32)
            vals = under + (vals - under) * av
        flatc[lin[keep]] = vals[keep].to(self.dtype)

    @staticmethod
    def to_numpy(canvas):
        return canvas.clamp(0, 255).round().to(torch.uint8).cpu().numpy()
