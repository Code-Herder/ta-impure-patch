#!/usr/bin/env python3
"""The in-game compute restorer's dumps against the unditherer's own torch model.

    .venv-undither/bin/python tools/restore-dumpcheck.py <gamedir> [--tag feat ...]

`tagpu_restoredump.on` in a game directory makes each restore job write, once
its queue drains (tagpu_vk_restore.c `dump_step`):

    tagpu_restore_<tag>_vk.rgba   the restored destination, RGBA8 (.mips: level 0
                                  then every reduced level, for a chained job)
    tagpu_restore_<tag>_vk.base   the source, RGBA8 with alpha 0 where keyed
    tagpu_restore_<tag>_vk.r8     ... or the source as palette indices
    tagpu_restore_<tag>_vk.pal    the job's palette snapshot, 256 x RGBA
    tagpu_restore_<tag>_vk.idx    `# atlas W H`, then `x y w h key wrap border
                                  padR padB` per painted frame

and this restores every listed frame again from that source with
unditherer/models/<model>.pt in strict fp32 (no TF32), and holds the dump to it.

THE INPUT IS THE DLL'S, ON PURPOSE. The unditherer inpaints a keyed texel with
OpenCV's TELEA; the game stands in with the mean colour of the nearest ring of
opaque texels (tagpu_restore_comp.h FILL). Both are fed to the same network, so
the reference here computes the game's stand-in and the comparison measures the
port -- the conv arithmetic, the padding rule, the wrap, the crop, the rounding
-- over every byte, the near-key halo included. How far the stand-in sits from
TELEA is a separate question (tools/tascene featdiff's near band).

WHAT PASSES, per job (research/notes/compute-restorer.md D11):
  every opaque texel's RGB within 1 level, with under 0.01 % of those bytes
  differing at all, and its alpha 255;
  every keyed texel (0, 0, 0, 0);
  the ring OUT replicates -- `border` on every side plus `padR`/`padB` of slack
  on the right and bottom -- a copy of the nearest edge texel, all four bytes;
  and for a `.mips` dump, every reduced level EXACTLY the integer 2x2 box
  average (sum + 1) / 4 of the level above (tagpu_restore_comp.h MIP).

A cell painted twice keeps its last entry; an earlier entry that a later one
overlaps is not checked, and the count is reported.

Exit 0 when every job checked passes, 1 when any fails, 2 when nothing could be
checked.
"""

import argparse
import json
import pathlib
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
TAGS = ("terr", "feat", "fx", "unit", "gui", "pic")


def read_idx(path):
    """(W, H, entries) of one `.idx`"""
    W = H = None
    ent = []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        if line.startswith("#"):
            p = line.split()
            if len(p) == 4 and p[1] == "atlas":
                W, H = int(p[2]), int(p[3])
            continue
        v = [int(t) for t in line.split()]
        if len(v) != 9:
            raise SystemExit(f"{path}: an entry line has {len(v)} columns, not 9 -- a dump from an older build")
        ent.append(dict(zip(("x", "y", "w", "h", "key", "wrap", "border", "padR", "padB"), v)))
    if W is None:
        raise SystemExit(f"{path}: no `# atlas W H` line -- a dump from an older build")
    return W, H, ent


def last_wins(ent):
    """the entries whose cell no later entry overlaps, and how many were dropped"""
    keep, dropped = [], 0
    for i, e in enumerate(ent):
        x0, y0 = e["x"] - e["border"], e["y"] - e["border"]
        x1 = e["x"] + e["w"] + e["border"] + e["padR"]
        y1 = e["y"] + e["h"] + e["border"] + e["padB"]
        over = False
        for f in ent[i + 1:]:
            fx0, fy0 = f["x"] - f["border"], f["y"] - f["border"]
            fx1 = f["x"] + f["w"] + f["border"] + f["padR"]
            fy1 = f["y"] + f["h"] + f["border"] + f["padB"]
            if fx0 < x1 and x0 < fx1 and fy0 < y1 and y0 < fy1:
                over = True
                break
        if over:
            dropped += 1
        else:
            keep.append(e)
    return keep, dropped


def box_sum(a, r, R, h, w):
    """the sum over the (2r+1)^2 box around every texel of an h x w frame,
    out of `a`, the frame padded by R >= r on every side"""
    c = np.zeros((a.shape[0] + 1, a.shape[1] + 1) + a.shape[2:], dtype=np.float64)
    c[1:, 1:] = a.cumsum(0).cumsum(1)
    y0, x0, k = R - r, R - r, 2 * r + 1
    return (c[y0 + k:y0 + k + h, x0 + k:x0 + k + w] - c[y0:y0 + h, x0 + k:x0 + k + w]
            - c[y0 + k:y0 + k + h, x0:x0 + w] + c[y0:y0 + h, x0:x0 + w])


def stand_in(rgb, keyed, wrap, R):
    """FILL's colour for every keyed texel: the mean of the opaque texels on the
    nearest Chebyshev ring (radius 1..R) that has any, searched wrapped when the
    frame wraps and clipped when it does not; 0 when no ring within R has one.
    `rgb` is integer 0..255. A wrapped tap past a small frame revisits a texel,
    and counts again -- as the shader's loop does -- because the padding below
    is by index, not by reflection."""
    h, w = keyed.shape
    op = (~keyed).astype(np.float64)
    col = rgb.astype(np.float64) * op[..., None]
    if wrap:
        iy = np.arange(-R, h + R) % h
        ix = np.arange(-R, w + R) % w
        opP, colP = op[iy][:, ix], col[iy][:, ix]
    else:
        opP = np.pad(op, R)
        colP = np.pad(col, ((R, R), (R, R), (0, 0)))
    out = np.zeros((h, w, 3))
    todo = keyed.copy()
    prevN = np.zeros((h, w)); prevC = np.zeros((h, w, 3))
    for r in range(1, R + 1):
        N = box_sum(opP, r, R, h, w); C = box_sum(colP, r, R, h, w)
        n, c = N - prevN, C - prevC
        hit = todo & (n > 0.5)
        out[hit] = c[hit] / n[hit][:, None]
        todo &= ~hit
        prevN, prevC = N, C
        if not todo.any():
            break
    return out


class Model:
    def __init__(self, name, device):
        import torch
        sys.path.insert(0, str(ROOT))
        from unditherer.model import Restorer
        torch.backends.cudnn.allow_tf32 = False
        torch.backends.cuda.matmul.allow_tf32 = False
        dev = torch.device("cuda" if torch.cuda.is_available() else "cpu") if device == "auto" else torch.device(device)
        ck = torch.load(ROOT / "unditherer" / "models" / f"{name}.pt", map_location=dev, weights_only=False)
        m = Restorer(ck["depth"], ck["ch"]).to(dev)
        m.load_state_dict(ck["model"])
        m.eval()
        self.torch, self.m, self.dev, self.depth = torch, m, dev, ck["depth"]

    def run(self, xs):
        """a list of HxWx3 float32 inputs (one shape) -> the model's outputs"""
        t = self.torch
        with t.no_grad():
            x = t.from_numpy(np.ascontiguousarray(np.stack(xs).transpose(0, 3, 1, 2))).to(self.dev)
            return self.m(x).float().cpu().numpy().transpose(0, 2, 3, 1)


def check_job(pre, model, batch):
    idx = pre.with_suffix(".idx")
    W, H, ent = read_idx(idx)
    mips = pre.with_suffix(".mips")
    got_all = np.fromfile(mips if mips.exists() else pre.with_suffix(".rgba"), dtype=np.uint8)
    got = got_all[:W * H * 4].reshape(H, W, 4)
    if pre.with_suffix(".base").exists():
        base = np.fromfile(pre.with_suffix(".base"), dtype=np.uint8).reshape(H, W, 4)
        src_rgb, src_key, kind = base[..., :3], None, "base"
    else:
        r8 = np.fromfile(pre.with_suffix(".r8"), dtype=np.uint8).reshape(H, W)
        pal = np.fromfile(pre.with_suffix(".pal"), dtype=np.uint8).reshape(256, 4)
        src_rgb, src_key, kind = pal[r8][..., :3], r8, "r8"
    keep, dropped = last_wins(ent)
    R = model.depth

    def frame_in(e):
        x, y, w, h = e["x"], e["y"], e["w"], e["h"]
        rgb = src_rgb[y:y + h, x:x + w].astype(np.int64)
        if e["key"] < 0:
            keyed = np.zeros((h, w), bool)
        elif kind == "base":
            keyed = base[y:y + h, x:x + w, 3] == 0
        else:
            keyed = src_key[y:y + h, x:x + w] == e["key"]
        f = rgb.astype(np.float64)
        if keyed.any():
            f[keyed] = stand_in(rgb, keyed, e["wrap"], R)[keyed]
        f = (f / 255.0).astype(np.float32)
        p = R if e["wrap"] else 0
        if p:
            f = f[np.arange(-p, h + p) % h][:, np.arange(-p, w + p) % w]
        return f, keyed, p

    acc = dict(frames=len(keep), overlapped=dropped, bytes=0, differing=0, max=0, hist=[0] * 256,
               keyed=0, keyed_bad=0, alpha_bad=0, ring=0, ring_bytes=0, worst=None, worst_n=0)
    groups = {}
    for e in keep:
        pw = e["w"] + (2 * R if e["wrap"] else 0)
        ph = e["h"] + (2 * R if e["wrap"] else 0)
        groups.setdefault((ph, pw), []).append(e)
    for shape, es in groups.items():
        for b0 in range(0, len(es), batch):
            chunk = es[b0:b0 + batch]
            ins = [frame_in(e) for e in chunk]
            outs = model.run([i[0] for i in ins])
            for e, (f, keyed, p), o in zip(chunk, ins, outs):
                x, y, w, h = e["x"], e["y"], e["w"], e["h"]
                o = o[p:p + h, p:p + w]
                ref = np.clip(np.floor(o * 255.0 + 0.5), 0, 255).astype(np.int64)
                g = got[y:y + h, x:x + w].astype(np.int64)
                op = ~keyed
                d = np.abs(g[..., :3] - ref)[op]
                n = int((d > 0).sum())
                acc["bytes"] += d.size
                acc["differing"] += n
                if d.size:
                    acc["max"] = max(acc["max"], int(d.max()))
                    for v, c in zip(*np.unique(d[d > 0], return_counts=True)):
                        acc["hist"][int(v)] += int(c)
                acc["alpha_bad"] += int((g[..., 3][op] != 255).sum())
                acc["keyed"] += int(keyed.sum())
                acc["keyed_bad"] += int((g[keyed] != 0).any(axis=-1).sum())
                if n > acc["worst_n"]:
                    acc["worst"], acc["worst_n"] = (x, y, w, h), n
                # the replicated ring: every texel of the cell's quad outside
                # the frame is its nearest edge texel, clipped to the atlas
                bd = e["border"]
                qy = np.arange(y - bd, y + h + bd + e["padB"])
                qx = np.arange(x - bd, x + w + bd + e["padR"])
                qy = qy[(qy >= 0) & (qy < H)]; qx = qx[(qx >= 0) & (qx < W)]
                ring = ~(((qy >= y) & (qy < y + h))[:, None] & ((qx >= x) & (qx < x + w))[None, :])
                if ring.any():
                    cy = np.clip(qy, y, y + h - 1); cx = np.clip(qx, x, x + w - 1)
                    q = got[qy][:, qx]
                    s = got[cy][:, cx]
                    acc["ring_bytes"] += int(ring.sum()) * 4
                    acc["ring"] += int((q != s)[ring].sum())
    acc["pct"] = 100.0 * acc["differing"] / max(1, acc["bytes"])
    acc["hist"] = acc["hist"][1:acc["max"] + 1]

    # the reduced levels
    lv = []
    if mips.exists():
        off, w, h, prev = W * H * 4, W, H, got
        while off < got_all.size:
            w, h = w // 2, h // 2
            cur = got_all[off:off + w * h * 4].reshape(h, w, 4).astype(np.int64)
            p = prev.astype(np.int64)
            want = (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2] + 1) // 4
            lv.append({"level": len(lv) + 1, "w": w, "h": h, "bytes_off": int((cur != want).sum())})
            off += w * h * 4
            prev = cur
    acc["mips"] = lv
    acc["pass"] = (bool(keep) and acc["max"] <= 1 and acc["pct"] < 0.01 and acc["keyed_bad"] == 0
                   and acc["alpha_bad"] == 0 and acc["ring"] == 0 and all(m["bytes_off"] == 0 for m in lv))
    acc.update(atlas=[W, H], source=kind)
    return acc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("gamedir", help="the instance's game directory holding tagpu_restore_*_vk.*")
    ap.add_argument("--tag", action="append", choices=TAGS, help="check only these jobs (repeatable)")
    ap.add_argument("--model", default="full", help="unditherer/models/<model>.pt (default full)")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    gd = pathlib.Path(a.gamedir).expanduser()
    model = Model(a.model, a.device)
    rep, checked = {}, 0
    for tag in a.tag or TAGS:
        pre = gd / f"tagpu_restore_{tag}_vk.rgba"
        if not pre.with_suffix(".idx").exists():
            rep[tag] = None
            continue
        rep[tag] = check_job(pre, model, a.batch)
        checked += 1
    if a.json:
        print(json.dumps(rep, indent=1))
    else:
        for tag, r in rep.items():
            if r is None:
                print(f"{tag}: no dump")
                continue
            mp = "".join(f", level {m['level']} {m['bytes_off']} bytes off the box average" for m in r["mips"])
            ov = f" ({r['overlapped']} overlapped, not checked)" if r["overlapped"] else ""
            print(f"{tag}: {'PASS' if r['pass'] else 'FAIL'}: {r['frames']} frames"
                  f"{ov}, "
                  f"max {r['max']} level(s), {r['differing']}/{r['bytes']} bytes differ ({r['pct']:.4f}%), "
                  f"hist {r['hist']}; {r['keyed']} keyed texels, {r['keyed_bad']} not (0,0,0,0); "
                  f"alpha wrong on {r['alpha_bad']}; ring {r['ring']}/{r['ring_bytes']} bytes not a copy of the edge"
                  f"{mp}; worst frame {r['worst']} ({r['worst_n']} bytes)")
    if not checked:
        sys.exit(2)
    sys.exit(0 if all(r["pass"] for r in rep.values() if r) else 1)


if __name__ == "__main__":
    main()
