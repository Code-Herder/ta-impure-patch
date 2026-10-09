"""L0 (new, an extension of cells_predict.py): the tick input predicted from the footprint moves
for BOTH slots -- slot A from layer-1 units (+0x110 & 3 == 1, no bit 29), slot B from layer-2
(airborne, +0x110 & 3 == 2), the stamp 0x47CC30's own filing rule -- between the iteration's
images and the next's. Each tick-input cell compared with the grid the reader holds (images):
per slot, predicted exactly / another value / not predicted / predicted but unchanged; the other
fields as cells_codec B codes them. ESTIMATE of the tick input under the prediction: the
exceptions as gap + flag byte + slot values + changed fields, by column (not decoded)."""
import collections
import pickle
import struct

from common import OUT, kb, T
from codec import Cols, varint, packed
from predict import rect
import cells_codec
from common import GRID as _GRID
W, H = _GRID
CB = T.REC_CELL_BYTES
FIELDS = cells_codec.FIELDS


def main():
    d = pickle.load(open(OUT / "units_pass.pkl", "rb"))
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    feet = d["feet"]
    lists = cells_codec.gather(keep)
    grid = bytearray(W * H * CB)
    st = collections.Counter()
    blocks = collections.defaultdict(Cols)
    base = collections.defaultdict(Cols)          # cells_codec B's tick input alone, for scale
    for what, bi, tick, cells in lists:
        if what in ("first", "img"):
            for cell, v in cells:
                grid[cell * CB:(cell + 1) * CB] = v
            continue
        a, b = feet.get(tick, {}), feet.get(tick + 1, {})
        pred = ({}, {})
        for s in set(a) | set(b):
            fa, fb = a.get(s), b.get(s)
            if fa == fb:
                continue
            for layer, p in ((1, pred[0]), (2, pred[1])):
                ok = lambda f: f is not None and f[4] & 3 == layer and not f[4] >> 29 & 1
                ra = rect(fa) if ok(fa) else set()
                rb = rect(fb) if ok(fb) else set()
                for c in ra - rb:
                    p.setdefault(c, 0)
                for c in rb - ra:
                    p[c] = s
        c = blocks[bi]
        cb = base[bi]
        exc = []
        seen = set()
        for cell, v in cells:
            seen.add(cell)
            old = grid[cell * CB:(cell + 1) * CB]
            flag = 0
            vals = bytearray()
            for k, (lo, p) in enumerate(((0, pred[0]), (2, pred[1]))):
                o, n = struct.unpack_from("<H", old, lo)[0], struct.unpack_from("<H", v, lo)[0]
                pv = p.get(cell, o)
                if pv == n:
                    if o != n:
                        st[f"slot {'AB'[k]}: predicted exactly"] += 1
                    elif cell in p:
                        st[f"slot {'AB'[k]}: predicted, no change"] += 1   # cannot happen: pv==n==o
                else:
                    st[f"slot {'AB'[k]}: " + ("another value" if cell in p else "not predicted")
                       if o != n else f"slot {'AB'[k]}: predicted, no change"] += 1
                    flag |= 1 << k
                    vals += struct.pack("<H", n)
            m = 0
            for f, (lo, hi) in enumerate(FIELDS[2:], 2):
                if old[lo:hi] != v[lo:hi]:
                    m |= 1 << f
                    vals += v[lo:hi]
            if m:
                st["cells with another field changed"] += 1
            if flag or m:
                exc.append((cell, flag | m, vals))
            # the B coding of this cell, for scale
            bm = sum(1 << f for f, (lo, hi) in enumerate(FIELDS) if old[lo:hi] != v[lo:hi])
            cb["fmask"].append(bm)
            for f, (lo, hi) in enumerate(FIELDS):
                if bm >> f & 1:
                    cb[f"f{f}"].extend(v[lo:hi])
        for p in pred:
            for cell, s in p.items():
                if cell not in seen and struct.unpack_from("<H", grid, cell * CB + (0 if p is pred[0] else 2))[0] != s:
                    st[f"slot {'AB'[p is pred[1]]}: predicted, no change"] += 1
                    exc.append((cell, 0x80 | (1 if p is pred[0] else 2), b""))
        varint(c["n"], len(exc))
        prev = -1
        for cell, f, vals in sorted(exc):
            varint(c["gap"], cell - prev - 1)
            prev = cell
            c["f"].append(f)
            c["v"].extend(vals)
        varint(cb["n"], len(cells))
        prev = -1
        for cell, v in cells:
            varint(cb["gap"], cell - prev - 1)
            prev = cell
    for k in sorted(st):
        print(f"  {k}: {st[k]:,}")
    for name, bb in (("tick input, B coding (reference)", base), ("tick input under both-slot prediction", blocks)):
        res = {h: sum(packed(x.blob(), h) for x in bb.values()) for h in ("deflate", "zstd3", "zstd19")}
        print(f"{name}: raw {kb(sum(x.raw() for x in bb.values()))}, " +
              ", ".join(f"{h} {kb(v)}" for h, v in res.items()))


if __name__ == "__main__":
    main()
