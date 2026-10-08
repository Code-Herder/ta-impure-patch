"""What the unit images could predict, measured on units_pass.pkl:
  deaths: the death image's dwords that differ from the unit's image at the iteration's images;
  shots: start - shooter position, target point - target position (world units, 16.16 >> 16);
  cells: the tick input's slot-A changes against a prediction from the footprint moves between
         the iteration's images and the next's (a ground unit, +0x110 & 3 == 1, no bit 29)."""
import collections
import pickle
import struct

from common import OUT

W, H = 642, 648


def rect(f):
    x, z, w, h, st = f
    if x < 0 or z < 0 or x + w >= W or z + h >= H:
        return set()
    return {(z + j) * W + x + i for j in range(h) for i in range(w)}


def main():
    d = pickle.load(open(OUT / "units_pass.pkl", "rb"))
    # deaths
    nd = collections.Counter()
    for tick, slot, dimg, uimg in d["deaths"]:
        if uimg is None:
            nd["no image"] += 1
            continue
        k = sum(dimg[i:i + 4] != uimg[i:i + 4] for i in range(0, len(dimg), 4))
        nd[min(k, 20)] += 1
    print("death image vs the images' unit image, dwords that differ:", sorted(nd.items(), key=str))
    # shots
    ds = collections.Counter()
    dt = collections.Counter()
    for tick, b, sp, tp in d["shots"]:
        start = struct.unpack_from("<iii", b, 1)
        tgt = struct.unpack_from("<iii", b, 13)
        if sp:
            m = max(abs((a - c) >> 16) for a, c in zip(start, sp))
            ds[m.bit_length()] += 1
        if tp:
            m = max(abs((a - c) >> 16) for a, c in zip(tgt, tp))
            dt[m.bit_length()] += 1
    print("shot start - shooter position, bits of the largest axis (world units):",
          sorted(ds.items()))
    print("shot target - target position, bits:", sorted(dt.items()))
    s0 = d["shots"][0]
    print("a shot:", struct.unpack_from("<iii", s0[1], 1), struct.unpack_from("<iii", s0[1], 13),
          "shooter", s0[2], "target", s0[3])
    # cells
    feet = d["feet"]
    bytick = collections.defaultdict(list)
    for tick, cell, old, new in d["tin"]:
        bytick[tick].append((cell, old, new))
    st = collections.Counter()
    for tick, changes in bytick.items():
        a, b = feet.get(tick, {}), feet.get(tick + 1, {})
        pred = {}
        for s in set(a) | set(b):
            fa, fb = a.get(s), b.get(s)
            if fa == fb:
                continue
            ground = lambda f: f is not None and f[4] & 3 == 1 and not f[4] >> 29 & 1
            ra = rect(fa) if ground(fa) else set()
            rb = rect(fb) if ground(fb) else set()
            for c in ra - rb:
                pred.setdefault(c, 0)
            for c in rb - ra:
                pred[c] = s + 0          # the slot's index as the stamp writes it [INFERRED]
        actual = {c: n for c, o, n in changes if o != n}
        for c, n in actual.items():
            if c in pred:
                st["actual, predicted same value" if pred[c] in (n, n - 1, n + 1) and
                   (pred[c] == 0) == (n == 0) else "actual, predicted other value"] += 1
            else:
                st["actual, not predicted"] += 1
        for c in pred:
            if c not in actual:
                st["predicted, no change"] += 1
        st["slot A changes"] += len(actual)
        st["cells in the tick input"] += len(changes)
    for k, v in st.items():
        print(f"  {k}: {v:,}")
    # which index the stamp writes: compare an actual new value with the unit's slot
    eq = collections.Counter()
    for tick, changes in list(bytick.items())[:400]:
        b = feet.get(tick + 1, {})
        for c, o, n in changes:
            if n and (n in b or n - 1 in b):
                f = b.get(n) or b.get(n - 1)
                eq["slot == value" if n in b and c in rect(b[n]) else
                   "slot == value - 1" if n - 1 in b and c in rect(b[n - 1]) else "neither"] += 1
    print("the new slot A value against the slots:", dict(eq))


if __name__ == "__main__":
    main()
