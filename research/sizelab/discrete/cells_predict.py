"""The tick input's cells predicted from the units' footprints, strictly: a ground unit's
(+0x110 & 3 == 1, no bit 29) footprint cells it left get 0 and the ones it entered get its slot,
between the iteration's images and the next's. The exceptions -- a change not predicted, a
predicted change with another value, a predicted change that did not happen, and every change of
a field but slot A -- are coded as cells_codec's B codes cells, per block, and priced: an
estimate of the tick input under the prediction (not a decoder: the exceptions are what a
decoder would read)."""
import collections
import pickle

from common import OUT, kb
from codec import Cols, varint, packed
from predict import rect

W, H = 642, 648


def main():
    d = pickle.load(open(OUT / "units_pass.pkl", "rb"))
    kp = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    block_of = {}
    for bi, t, k, b in kp:
        block_of.setdefault(t, bi)
    feet = d["feet"]
    bytick = collections.defaultdict(list)
    for tick, cell, old, new in d["tin"]:
        bytick[tick].append((cell, old, new))
    st = collections.Counter()
    blocks = collections.defaultdict(Cols)
    for tick in sorted(bytick):
        changes = bytick[tick]
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
                pred[c] = s
        actual = {c: n for c, o, n in changes if o != n}
        nonslot = len(changes) - len(actual)
        exc = []
        for c, n in actual.items():
            if pred.get(c) == n:
                st["predicted exactly"] += 1
            else:
                st["not predicted or another value"] += 1
                exc.append((c, n))
        for c in pred:
            if c not in actual:
                st["predicted, no change"] += 1
                exc.append((c, None))
        st["cells whose change is not slot A alone (coded as B)"] += nonslot
        cc = blocks[block_of.get(tick, 0)]
        varint(cc["n"], len(exc) + nonslot)
        prev = -1
        for c, n in sorted(exc, key=lambda x: x[0]):
            varint(cc["gap"], c - prev - 1)
            prev = c
            if n is None:
                cc["f"].append(0x40)
            elif n == 0:
                cc["f"].append(0x80)
            else:
                cc["f"].append(1)
                cc["v"].extend(n.to_bytes(2, "little"))
        for _ in range(nonslot):                 # priced as B prices a cell: gap, mask, ~4 bytes
            varint(cc["gap"], 300)
            cc["f"].append(0x15)
            cc["v"].extend(b"\x01\x02\x03\x04")
    for k, v in st.items():
        print(f"  {k}: {v:,}")
    res = {h: sum(packed(c.blob(), h) for c in blocks.values()) for h in ("deflate", "zstd19")}
    print("the tick input under the prediction, estimated: raw "
          f"{kb(sum(c.raw() for c in blocks.values()))}, " +
          ", ".join(f"{h} {kb(v)}" for h, v in res.items()))


if __name__ == "__main__":
    main()
