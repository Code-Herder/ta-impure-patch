import sys, os; sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent.parent / "units")); os.chdir(str(__import__("pathlib").Path(__file__).resolve().parent.parent / "units"))
"""lanestats.py -- where the best frame's bytes go: per lane nonzero residuals, varint bytes, and
each lane's columns deflated alone (writer settings) per block; plus the framing."""
import sys, zlib, collections, numpy as np
import frame2, sim
from frame import varint, zz
opts = dict(a.split("=", 1) for a in sys.argv[1:])
z = sim.load("units.npz")
reset = set(int(t) for t in z["snap_ticks"])
enc = frame2.Codec(lanes=opts.get("lanes", "field"), rules=bool(int(opts.get("rules", 1))), pred="win", reset_ticks=reset)
ln = enc.ln
M_ = 0x118
DROP = [ln.index[o] for o in (0x6A, 0x6E, 0x72, 0x64, 0x66, 0x68, M_ + 8, M_ + 0xC, M_ + 0x10,
        M_ + 0x20, M_ + 0x24, M_ + 0x2A)] if opts.get("drop") == "motion" else []
def defl(b):
    c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
nz = np.zeros(ln.n, np.int64); vb = np.zeros(ln.n, np.int64); pk = np.zeros(ln.n, np.int64)
frame_raw = collections.Counter(); frame_pk = collections.Counter()
group, gkey = [], None
def flush(group):
    if not group: return
    fr = [enc.encode_tick(tk) for tk in group]
    if DROP:
        fr2 = []
        for sl, R in fr:
            R[:, DROP] = 0
            k = (R != 0).any(axis=1)
            fr2.append((sl[k], R[k]))
        fr = fr2
    R = np.concatenate([r for _, r in fr]); M = R != 0
    gaps = varint(np.concatenate([np.diff(np.concatenate(([-1], s))) - 1 for s, _ in fr]))
    packed = np.packbits(M, axis=1, bitorder="little"); uniq = {}
    ids = np.array([uniq.setdefault(r.tobytes(), len(uniq)) for r in packed], np.int64)
    idb = ids.astype(np.uint8).tobytes() if len(uniq) <= 256 else varint(ids)
    for k, b in (("gaps", gaps), ("mask dict", b"".join(uniq)), ("mask ids", idb)):
        frame_raw[k] += len(b); frame_pk[k] += defl(b)
    for j in range(ln.n):
        col = R[M[:, j], j]
        if len(col):
            b = varint(zz(col)); nz[j] += len(col); vb[j] += len(b); pk[j] += defl(b)
for tk in sim.ticks(z):
    if tk.block != gkey:
        flush(group); group, gkey = [], tk.block
    group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
flush(group)
names = {0x66: "heading", 0x64: "pitch", 0x68: "roll", 0x6A: "x", 0x6E: "y (altitude)", 0x72: "z",
         0x118 + 8: "mover vx", 0x118 + 0xC: "mover vy", 0x118 + 0x10: "mover vz", 0x118 + 0x20: "mover speed",
         0x118 + 0x24: "mover turn rate", 0x118 + 0x26: "mover +0x26", 0x118 + 0x28: "mover +0x28",
         0x118 + 0x2A: "mover stamp", 0x118 + 0x2E: "mover layer", 0x18: "w1 reload", 0x1A: "w1 aim heading",
         0x1C: "w1 aim pitch", 0x10: "w1 weapon", 0x0C: "w1 aimed", 0x08: "w1 vtable", 0x04: "w1 target lo", 0x06: "w1 target hi",
         0x34: "w2 reload", 0x36: "w2 aim heading", 0x38: "w2 aim pitch", 0x50: "w3 reload", 0x52: "w3 aim heading",
         0x54: "w3 aim pitch", 0xFA: "+0xFA damage countdown", 0xFB: "+0xFB give lock", 0x76: "footprint x", 0x78: "footprint z",
         0xF8: "sight h", 0x108: "hp", 0xB0: "GameTime deadline", 0xB8: "kills", 0xBA: "+0xBA", 0xF0: "attacker",
         0xF6: "hp% now", 0xF7: "hp% 30 ago", 0xF4: "attacker player", 0xF5: "last hit kind", 0x110: "state bits",
         0x7A: "sight x", 0x7C: "sight z", 0x82: "sort bucket", 0x8E: "next", 0x23: "w1 flags", 0x1F: "w1 flags", 0x1E: "w1 stockpile",
         0x104: "build fraction", 0x10E: "state byte", 0x10A: "water", 0x5C: "orders", 0x58: "+0x58"}
tot = pk.sum() + sum(frame_pk.values())
print(f"total deflated (per-column attribution) {tot/1048576:.2f} MiB; framing:")
for k in frame_raw: print(f"   {k:12s} raw {frame_raw[k]/1048576:7.2f} MiB  deflated {frame_pk[k]/1048576:6.2f} MiB")
order = np.argsort(-pk)
print(f"{'lane':28s}{'nonzero':>10}{'varint MiB':>12}{'deflated MiB':>14}")
for j in order[:30]:
    o, s = ln.table[j]
    print(f"+0x{o:03X}/{s} {names.get(o, ''):20s}{nz[j]:>10}{vb[j]/1048576:12.2f}{pk[j]/1048576:14.2f}")
