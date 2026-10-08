"""lanecost.py -- a copy of research/sizelab/units/lanestats.py's attribution (best frame, rules=1),
adding a split by unit class (from classstats.json): per lane and class, nonzero residuals and
their varint bytes; per lane, the columns deflated alone per block (writer settings)."""
import json, sys, zlib, collections, numpy as np
import frame2, sim
from frame import varint, zz
z = sim.load("units.npz")
reset = set(int(t) for t in z["snap_ticks"])
enc = frame2.Codec(lanes="field", rules=True, pred="win", reset_ticks=reset)
ln = enc.ln
CL = ["ground", "floater", "air", "structure"]
cmap = np.full(65536, 3, np.int64)
for t, d in json.load(open("classstats.json"))["types"].items():
    cmap[int(t)] = CL.index(d["class"])
LT = ln.index[0xA6]
defl = lambda b: len((lambda c: c.compress(b) + c.flush())(zlib.compressobj(6, zlib.DEFLATED, 11)))
nz = np.zeros((4, ln.n), np.int64); vb = np.zeros((4, ln.n), np.int64); pk = np.zeros(ln.n, np.int64)
listed = np.zeros(4, np.int64)
fpk = collections.Counter()
def flush(group):
    if not group: return
    fr, cls = [], []
    for tk in group:
        s, R = enc.encode_tick(tk)
        fr.append((s, R)); cls.append(cmap[ln.split(tk.S[s])[:, LT]] if len(s) else np.zeros(0, np.int64))
    R = np.concatenate([r for _, r in fr]); C = np.concatenate(cls); M = R != 0
    for k in range(4): listed[k] += int((C == k).sum())
    gaps = varint(np.concatenate([np.diff(np.concatenate(([-1], s))) - 1 for s, _ in fr]))
    packed = np.packbits(M, axis=1, bitorder="little"); uniq = {}
    ids = np.array([uniq.setdefault(r.tobytes(), len(uniq)) for r in packed], np.int64)
    idb = ids.astype(np.uint8).tobytes() if len(uniq) <= 256 else varint(ids)
    for k, b in (("gaps", gaps), ("mask dict", b"".join(uniq)), ("mask ids", idb)): fpk[k] += defl(b)
    for j in range(ln.n):
        col = R[M[:, j], j]
        if len(col):
            pk[j] += defl(varint(zz(col)))
            cc = C[M[:, j]]
            for k in range(4):
                sel = cc == k
                if sel.any():
                    nz[k, j] += int(sel.sum()); vb[k, j] += len(varint(zz(col[sel])))
group, gkey = [], None
for tk in sim.ticks(z):
    if tk.block != gkey:
        flush(group); group, gkey = [], tk.block
    group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
flush(group)
MiB = 1048576
print(f"per-column deflated total {(pk.sum() + sum(fpk.values()))/MiB:.3f} MiB; framing " +
      ", ".join(f"{k} {v/MiB:.3f}" for k, v in fpk.items()))
print("listed unit-iterations by class:", dict(zip(CL, listed.tolist())))
out = []
for j in np.argsort(-pk)[:25]:
    o, s = ln.table[j]
    row = {"lane": f"+0x{o:03X}/{s}", "deflated_MiB": round(pk[j] / MiB, 3),
           **{f"{CL[k]}_nz": int(nz[k, j]) for k in range(4)}, **{f"{CL[k]}_varintKiB": round(vb[k, j] / 1024, 1) for k in range(4)}}
    out.append(row); print(row)
json.dump({"framing": dict(fpk), "lanes": out, "listed": dict(zip(CL, listed.tolist()))}, open("lanecost.json", "w"), indent=1)
