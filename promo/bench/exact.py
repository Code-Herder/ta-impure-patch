"""A GPU compositor that resamples ONCE, with PIL's own filter.

The first prototype prefiltered the source to the tile's scale and then
bilinear-shifted it into place. Two resamples: the picture came out 23-40%
softer, which showed up as LOWER jitter and looked like a win until the
gradient energy was measured. Blur is not steadiness.

So do what PIL does, in one pass, on the GPU. PIL's reduction is a separable
triangle filter whose support scales with the reduction ratio:

    scale  = span / dest                 (source pixels per destination pixel)
    fscale = max(1, scale)
    center = box0 + (j + 0.5) * scale
    w(i)   = tri((i + 0.5 - center) / fscale),  normalised over the taps

Weights are computed PER TILE, so there is no phase quantisation at all -- the
CPU path rounds the sub-pixel offset to a quarter pixel to keep its cache
sharable, and this does not have to.
"""
import numpy as np
import torch


def _weights(s0, span, dest, src_len, dev):
    """PIL's resample weights for one axis. -> idx [n,dest,taps], w [n,dest,taps]"""
    scale = span / dest
    fscale = scale.clamp(min=1.0)
    support = fscale
    j = torch.arange(dest, device=dev, dtype=torch.float32)
    center = s0[:, None] + (j[None, :] + 0.5) * scale[:, None]
    xmin = (center - support[:, None] + 0.5).floor()
    taps = int(np.ceil(float(2 * support.max()) + 2))
    k = torch.arange(taps, device=dev, dtype=torch.float32)
    idx = xmin[:, :, None] + k                                  # [n,dest,taps]
    d = (idx + 0.5 - center[:, :, None]) / fscale[:, None, None]
    w = (1.0 - d.abs()).clamp(min=0.0)
    w = w * ((idx >= 0) & (idx < src_len))
    w = w / w.sum(-1, keepdim=True).clamp(min=1e-8)
    return idx.clamp(0, src_len - 1).long(), w


class ExactGpuCompositor:
    def __init__(self, device="cuda", dtype=torch.float16, chunk_elems=48 << 20):
        self.dev = torch.device(device)
        self.dtype = dtype
        self.chunk = chunk_elems
        self._src = {}

    def _upload(self, key, pil):
        t = self._src.get(key)
        if t is None:
            a = np.ascontiguousarray(np.asarray(pil.convert("RGB")))
            t = torch.from_numpy(a).to(self.dev)
            self._src[key] = t
        return t

    def evict(self, keep):
        for k in list(self._src):
            if k not in keep:
                del self._src[k]

    def frame(self, sources, tiles, flats, bg, w, h):
        dev = self.dev
        canvas = torch.empty((h, w, 3), dtype=self.dtype, device=dev)
        canvas[:] = torch.tensor(bg, dtype=self.dtype, device=dev)
        for col, X0, Y0, X1, Y1 in flats:
            x0, y0, x1, y1 = max(0, X0), max(0, Y0), min(w, X1), min(h, Y1)
            if x1 > x0 and y1 > y0:
                canvas[y0:y1, x0:x1] = torch.tensor(col, dtype=self.dtype, device=dev)

        groups = {}
        for tl in tiles:
            groups.setdefault((tl[3] - tl[1], tl[4] - tl[2], tl[0][2], tl[0][3]),
                              []).append(tl)
        for (dw, dh, _cw, _ch), grp in groups.items():
            # chunk so the [n, dh, Wc, 3] intermediate stays bounded
            per = max(1, int(self.chunk // max(1, dh * _cw * 3)))
            for i in range(0, len(grp), per):
                self._draw(canvas, sources, grp[i:i + per], dw, dh, w, h)
        return canvas

    def _draw(self, canvas, sources, grp, dw, dh, W, H):
        dev = self.dev
        keys = sorted({t[0] for t in grp}, key=str)
        kidx = {k: i for i, k in enumerate(keys)}
        stack = torch.stack([self._upload(k, sources[k]) for k in keys]).to(self.dtype)
        S, sh, sw, _ = stack.shape
        n = len(grp)
        col = lambda i, dt=torch.float32: torch.tensor(
            [t[i] for t in grp], device=dev, dtype=dt)
        sx0, sy0, sx1, sy1 = col(5), col(6), col(7), col(8)
        alpha = col(9)
        sid = torch.tensor([kidx[t[0]] for t in grp], device=dev, dtype=torch.long)

        iy, wy = _weights(sy0, sy1 - sy0, dh, sh, dev)     # [n,dh,ty]
        ix, wx = _weights(sx0, sx1 - sx0, dw, sw, dev)     # [n,dw,tx]

        # vertical first: [S,sh,sw,3] -> [n,dh,sw,3], accumulating over taps so
        # the gather never materialises the tap axis.
        src = stack[sid]                                    # [n,sh,sw,3] (view-copy)
        acc = torch.zeros((n, dh, sw, 3), dtype=torch.float32, device=dev)
        for k in range(iy.shape[2]):
            rows = iy[:, :, k]                              # [n,dh]
            g = torch.gather(src, 1, rows[:, :, None, None].expand(n, dh, sw, 3))
            acc += g.to(torch.float32) * wy[:, :, k][:, :, None, None]

        out = torch.zeros((n, dh, dw, 3), dtype=torch.float32, device=dev)
        for k in range(ix.shape[2]):
            cols = ix[:, :, k]                              # [n,dw]
            g = torch.gather(acc, 2, cols[:, None, :, None].expand(n, dh, dw, 3))
            out += g * wx[:, :, k][:, None, :, None]

        X0 = col(1, torch.long); Y0 = col(2, torch.long)
        dx = X0[:, None, None] + torch.arange(dw, device=dev)[None, None, :]
        dy = Y0[:, None, None] + torch.arange(dh, device=dev)[None, :, None]
        inside = ((dx >= 0) & (dx < W) & (dy >= 0) & (dy < H)).expand(n, dh, dw)
        lin = (dy.clamp(0, H - 1) * W + dx.clamp(0, W - 1)).expand(n, dh, dw).reshape(-1)
        vals = out.reshape(-1, 3)
        flat = canvas.view(-1, 3)
        if float(alpha.min()) < 1.0:
            av = alpha[:, None, None, None].expand(n, dh, dw, 3).reshape(-1, 3)
            under = flat[lin].to(torch.float32)
            vals = under + (vals - under) * av
        keep = inside.reshape(-1)
        flat[lin[keep]] = vals[keep].to(self.dtype)

    @staticmethod
    def to_numpy(c):
        return c.clamp(0, 255).round().to(torch.uint8).cpu().numpy()
