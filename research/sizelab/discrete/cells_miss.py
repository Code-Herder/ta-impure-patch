"""L0 diagnostic (new): the cells predictor's exceptions, by the unit types involved. Reruns
units_pass's walk keeping each slot's type (+0x92) and state (+0x110) at each iteration's images."""
import collections, pickle, struct
from common import T, OUT, load, GRID
from predict import rect
W, H = GRID
d = pickle.load(open(OUT / "units_pass.pkl", "rb"))
feet = d["feet"]
# type per slot per tick
bl = load()
units = {}
types = {}
tick = None
for kind, first, last, payload in bl:
    if kind != T.RB_STREAM:
        continue
    for rk, b in T.records(payload):
        if rk == T.RK_TICK:
            if tick is not None:
                types[tick] = {s: (struct.unpack_from("<H", u, 0x92)[0]) for s, u in units.items()}
            tick = struct.unpack_from("<I", b)[0]
        elif rk == T.RK_UNIT:
            units[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + T.REC_UNIT_IMAGE])
        elif rk == T.RK_UNIT_DELTA:
            s = struct.unpack_from("<H", b)[0]
            T.World._delta(units[s], b[2:2 + T.REC_UNIT_BITS], T.REC_UNIT_DWORDS, b[T.REC_UNIT_DELTA_HEAD:])
        elif rk == T.RK_UNIT_GONE:
            units.pop(struct.unpack_from("<H", b)[0], None)
types[tick] = {s: (struct.unpack_from("<H", u, 0x92)[0]) for s, u in units.items()}
bytick = collections.defaultdict(list)
for t, cell, old, new in d["tin"]:
    bytick[t].append((cell, old, new))
why = collections.Counter()
for t, changes in bytick.items():
    a, b = feet.get(t, {}), feet.get(t + 1, {})
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
    ty = types.get(t, {})
    for c, o, n in changes:
        if o == n:
            continue
        p = pred.get(c)
        if p == n:
            continue
        who = n if n else o
        f = a.get(who) or b.get(who)
        st = f[4] if f else None
        why[("other value" if c in pred else "not predicted", "set" if n else "cleared",
             ty.get(who), None if st is None else (st & 3, st >> 29 & 1))] += 1
    for c in pred:
        if not any(cc == c and o != n for cc, o, n in changes):
            s = pred[c] or next((x for x in set(a) | set(b) if c in (rect(a[x]) if x in a else set())), None)
            f = a.get(s) or b.get(s) if s else None
            why[("predicted, no change", "-", ty.get(s), None if not f else (f[4] & 3, f[4] >> 29 & 1))] += 1
for k, v in why.most_common(25):
    print(f"  {v:>7,}  {k}")
