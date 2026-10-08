"""p3: today's projectile records against the identity-tracked re-encoding (codec.py), per stream
block of the file and per 900-tick snapshot interval, under the writer's deflate, zlib 9, zstd 19
and lzma 6. Decodes every block back from its bytes and checks every iteration's pool exactly.

    p3_measure.py <file.imprec> [modes]     modes: 'prev', 'lin', 'best' (default best)
"""
import sys, struct, zlib, lzma, collections, time
import numpy as np
import zstandard
from pool import iterations
import codec as C
import codec2 as C2

path = sys.argv[1]
which = sys.argv[2] if len(sys.argv) > 2 else "best"
MODES = {
    "prev": {},
    "lin": {d: "lin" for d in range(27)},
    "best": {**{d: "lin" for d in range(27)}, 1: "phys", 2: "phys", 3: "phys",
             25: "idx25", 26: "idx26"},
    "best_noidx": {**{d: "lin" for d in range(27)}, 1: "phys", 2: "phys", 3: "phys"},
}[which]

def writer(b): c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
def z9(b): return len(zlib.compress(b, 9))
ZC = zstandard.ZstdCompressor(level=19)
def zs(b): return len(ZC.compress(b))
def xz(b): return len(lzma.compress(b, preset=6))
COMP = {"writer": writer, "zlib9": z9, "zstd19": zs, "lzma6": xz}

def serialise(ticks):
    """a block's ticks [(ctl, rs, rn, rt, refs)] -> bytes: ctl, refs, then each column across the
    ticks"""
    out = bytearray()
    ctl = []
    for t in ticks:
        ctl += [len(t[0])] + t[0]
    parts = [C.leb(ctl), C.leb(np.concatenate([t[4] for t in ticks]) if ticks else [])]
    for k, grp in ((1, C.ORDER), (2, range(27)), (3, range(27))):
        for col in grp:
            vals = np.concatenate([t[k][col] for t in ticks]) if ticks else np.zeros(0, np.uint32)
            if k == 2 and col == 0:
                parts.append(C.leb(vals))
            else:
                parts.append(C.leb(C.zz(vals)))
    sizes = [len(p) for p in parts]
    return C.leb([len(sizes)] + sizes) + b"".join(parts), sizes

def deserialise(data, dec, snaps):
    """bytes -> the ticks' (ctl, rs, rn, rt), using the decoder's state for the counts"""
    nparts, at = C.unleb(data, 1)
    sizes, at = C.unleb(data, nparts[0], at)
    parts = []
    for s in sizes:
        parts.append(data[at:at + s]); at += s
    # ctl: per tick [len] + ints
    p0 = parts[0]; a = 0; ctls = []
    while a < len(p0):
        (n,), a = C.unleb(p0, 1, a)
        v, a = C.unleb(p0, n, a)
        ctls.append(v)
    # counts per tick from ctl and a running qc
    qc = dec.qc
    counts = []
    for v in ctls:
        c, hw, nd = v[0], v[1], v[2]
        ns = qc - nd
        nt = v[3 + nd]
        counts.append((ns, c - ns, nt))
        qc = c
    nref = sum(cn[1] + cn[2] for cn in counts)
    refs, _ = C.unleb(parts[1], nref)
    ticks = [(ctls[i], {}, {}, {}, None) for i in range(len(ctls))]
    a = 0
    for i, cn in enumerate(counts):
        ticks[i] = ticks[i][:4] + (np.array(refs[a:a + cn[1] + cn[2]], np.uint64).astype(np.uint32),)
        a += cn[1] + cn[2]
    pi = 2
    for k, grp in ((1, C.ORDER), (2, range(27)), (3, range(27))):
        for col in grp:
            n = sum(cn[k - 1] for cn in counts)
            vals, _ = C.unleb(parts[pi], n); pi += 1
            vals = np.array(vals, np.uint64).astype(np.uint32)
            if not (k == 2 and col == 0):
                vals = C.unzz(vals)
            a = 0
            for i, cn in enumerate(counts):
                ticks[i][k][col] = vals[a:a + cn[k - 1]]; a += cn[k - 1]
    return ticks

def old_bytes(recs):
    return b"".join(struct.pack("<HH", rk, len(b)) + b + bytes(-len(b) % 4) for rk, b in recs)

enc = C2.Codec2(MODES); dec = C2.Codec2(MODES)
blocks = collections.OrderedDict()     # block -> list of (tick, it-data)
t0 = time.time()
tot_old = collections.Counter(); tot_new = collections.Counter()
seg_old = collections.defaultdict(bytearray); seg_new = collections.defaultdict(list)
colsize = collections.Counter(); colz = collections.Counter(); nzero = collections.Counter()
nchk = 0
cur_block = None; buf = []; obuf = bytearray(); pools = []; snaps = []
def flush():
    global buf, obuf, pools, snaps, nchk
    if not buf and not obuf:
        return
    data, sizes = serialise(buf)
    for k, f in COMP.items():
        tot_old[k] += f(bytes(obuf)) if obuf else 0
        tot_new[k] += f(data)
    tot_old["raw"] += len(obuf); tot_new["raw"] += len(data)
    nparts, at = C.unleb(data, 1)
    psz, at = C.unleb(data, nparts[0], at)
    for i, n in enumerate(psz):
        part = data[at:at + n]; at += n
        colsize[i] += n
        colz[i] += zs(part) if n else 0
        nzero[i] += part.count(0)
    seg = pools[0][0] // 900 if pools else 0
    seg_old[seg] += obuf; seg_new[seg].append(data)
    # decode back from the bytes and check
    ticks = deserialise(data, dec, snaps)
    for (ctl, rs, rn, rt, refs), (tick, P, c, hw), sn in zip(ticks, pools, snaps):
        Pd, cd, hwd = dec.decode(ctl, rs, rn, rt, refs, sn)
        assert cd == c and hwd == hw and np.array_equal(Pd, P), f"mismatch at tick {tick}"
        nchk += 1
    buf = []; obuf = bytearray(); pools = []; snaps = []

for it in iterations(path):
    if it["block"] != cur_block:
        flush(); cur_block = it["block"]
    t = enc.encode(it["pool"], it["count"], it["hw"], it["snap_before"])
    buf.append(t); pools.append((it["tick"], it["pool"], it["count"], it["hw"]))
    snaps.append(it["snap_before"])
    obuf += old_bytes(it["recs"])
flush()
print(f"mode {which}: {nchk} iterations decoded back exactly ({time.time() - t0:.0f} s)")
MB = 1e6
print(f"{'per stream block':20s} " + " ".join(f"{k:>10s}" for k in ["raw"] + list(COMP)))
print(f"{'today':20s} " + " ".join(f"{tot_old[k]/MB:10.3f}" for k in ["raw"] + list(COMP)))
print(f"{'identity':20s} " + " ".join(f"{tot_new[k]/MB:10.3f}" for k in ["raw"] + list(COMP)))
# per 900-tick interval (a context per snapshot interval: seek still starts at a snapshot)
for k in ("zstd19", "lzma6"):
    o = sum(COMP[k](bytes(v)) for v in seg_old.values())
    n = sum(COMP[k](b"".join(v)) for v in seg_new.values())
    print(f"per 900-tick interval, {k}: today {o/MB:.3f} MB, identity {n/MB:.3f} MB")
names = ["ctl", "refs"] + [f"surv d{c}" for c in C.ORDER] + [f"new d{c}" for c in range(27)] + [f"tail d{c}" for c in range(27)]
print("by part: raw LEB bytes, zero bytes, zstd19 per block alone (attribution)")
for i, n in sorted(colz.items(), key=lambda x: -x[1])[:40]:
    print(f"   {names[i]:12s} raw {colsize[i]/1e3:9.1f} KB  zeros {nzero[i]/1e3:9.1f} KB  zstd19 {n/1e3:8.1f} KB")
print("sum of parts zstd19", sum(colz.values()) / 1e6)
