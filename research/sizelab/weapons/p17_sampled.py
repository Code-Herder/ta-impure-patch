"""p5: codec3 (identity, best references, per-block predictor choice) on a recording: today's
projectile records against the re-encoding, per stream block and per 900-tick interval, under four
compressors; every iteration decoded back from the bytes and checked; the bytes by section.

    p5_measure.py <file.imprec> [stamp|nostamp] [leb|planes]"""
import sys, struct, zlib, lzma, collections, time
import numpy as np
import zstandard
from pool import iterations as _iterations
EVERY = int([a for a in sys.argv if a.startswith("every=")][0][6:]) if any(a.startswith("every=") for a in sys.argv) else 1
def iterations(path):
    """every EVERY-th iteration: the pool as it stands there, the shots of the skipped ones
    carried along, a snapshot between them kept"""
    shots = []; snap = False
    for it in _iterations(path):
        shots += it["shots"]; snap = snap or it["snap_before"]
        if it["tick"] % EVERY == 0:
            it["shots"] = shots; it["snap_before"] = snap
            shots = []; snap = False
            yield it
import codec as C
import codec3 as C3
import codec4 as C4

path = sys.argv[1]
stamp = True
layout = "leb"
EVENT = "event" in sys.argv[2:]
XP = "xpos" in sys.argv[2:]

def writer(b): c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
def z9(b): return len(zlib.compress(b, 9))
ZC = zstandard.ZstdCompressor(level=19)
def zs(b): return len(ZC.compress(b))
def xz(b): return len(lzma.compress(b, preset=6))
COMP = {"writer": writer, "zlib9": z9, "zstd19": zs, "lzma6": xz}

def pack(vals, signed=True):
    vals = np.asarray(vals, np.uint32)
    z = C.zz(vals) if signed else vals
    if layout == "leb":
        return C.leb(z)
    # byte planes of the zigzag values, each value 4 bytes
    b = z.astype("<u4").tobytes()
    a = np.frombuffer(b, np.uint8).reshape(-1, 4) if len(b) else np.zeros((0, 4), np.uint8)
    return b"".join(a[:, k].tobytes() for k in range(4))

def unpack(data, n, signed=True):
    if layout == "leb":
        v, _ = C.unleb(data, n)
        v = np.array(v, np.uint64).astype(np.uint32)
    else:
        a = np.frombuffer(data, np.uint8).reshape(4, n).T.copy() if n else np.zeros((0, 4), np.uint8)
        v = a.view("<u4").ravel().astype(np.uint32) if n else np.zeros(0, np.uint32)
    return C.unzz(v) if signed else v

SECTIONS = ["ctl", "refs", "surv", "new", "tail"]

def serialise(ticks):
    ctl = []
    for t in ticks:
        ctl += [len(t[0])] + t[0]
    parts = [("ctl", C.leb(ctl)), ("refs", C.leb(np.concatenate([t[4] for t in ticks])))]
    choice = []
    for col in C.ORDER:
        best = None
        for mi, md in enumerate(C3.modes_for(col, stamp)):
            vals = np.concatenate([t[1][col][md] for t in ticks])
            b = pack(vals)
            # choose by the compressed size of the column alone (the writer's deflate)
            sz = writer(b)
            if best is None or sz < best[0]:
                best = (sz, mi, b)
        choice.append(best[1]); parts.append(("surv", best[2]))
    for col in range(27):
        vals = np.concatenate([t[2][col] for t in ticks])
        parts.append(("new", pack(vals)))
    for col in range(27):
        vals = np.concatenate([t[3][col] for t in ticks])
        parts.append(("tail", pack(vals)))
    sizes = [len(p) for _, p in parts]
    head = C.leb([len(sizes)] + sizes + choice)
    return head + b"".join(p for _, p in parts), parts, len(head)

def deserialise(data, dec):
    (n,), at = C.unleb(data, 1)
    sizes, at = C.unleb(data, n, at)
    choice, at = C.unleb(data, len(C.ORDER), at)
    parts = []
    for s in sizes:
        parts.append(data[at:at + s]); at += s
    p0 = parts[0]; a = 0; ctls = []
    while a < len(p0):
        (k,), a = C.unleb(p0, 1, a)
        v, a = C.unleb(p0, k, a)
        ctls.append(v)
    qc = dec.qc; counts = []
    for v in ctls:
        c, hw, nd = v[0], v[1], v[2]
        ns = qc - nd
        counts.append((ns, c - ns, v[3 + nd])); qc = c
    nref = sum(x[1] + x[2] for x in counts)
    refs, _ = C.unleb(parts[1], nref)
    refs = np.array(refs, np.uint64).astype(np.uint32)
    ticks = []
    a = 0
    for i, x in enumerate(counts):
        ticks.append([ctls[i], {}, {}, {}, refs[a:a + x[1] + x[2]]]); a += x[1] + x[2]
    pi = 2
    for ci, col in enumerate(C.ORDER):
        md = C3.modes_for(col, stamp)[choice[ci]]
        vals = unpack(parts[pi], sum(x[0] for x in counts)); pi += 1
        a = 0
        for i, x in enumerate(counts):
            ticks[i][1][col] = (md, vals[a:a + x[0]]); a += x[0]
    for k in (2, 3):
        for col in range(27):
            vals = unpack(parts[pi], sum(x[k - 1] for x in counts)); pi += 1
            a = 0
            for i, x in enumerate(counts):
                ticks[i][k][col] = vals[a:a + x[k - 1]]; a += x[k - 1]
    return ticks

def old_bytes(recs):
    return b"".join(struct.pack("<HH", rk, len(b)) + b + bytes(-len(b) % 4) for rk, b in recs)

enc = C4.Codec4(True, EVENT, XP); dec = C4.Codec4(True, EVENT, XP)
shots_of = []
t0 = time.time()
tot_old = collections.Counter(); tot_new = collections.Counter()
seg_old = collections.defaultdict(bytearray); seg_new = collections.defaultdict(list)
sec_raw = collections.Counter(); sec_w = collections.Counter(); sec_z = collections.Counter()
modecount = collections.Counter(); partw = collections.Counter()
nchk = 0
buf = []; obuf = bytearray(); pools = []; snaps = []; cur_block = None
def flush():
    global buf, obuf, pools, snaps, nchk
    if not buf:
        return
    data, parts, headlen = serialise(buf)
    for k, f in COMP.items():
        tot_old[k] += f(bytes(obuf)) if obuf else 0
        tot_new[k] += f(data)
    tot_old["raw"] += len(obuf); tot_new["raw"] += len(data)
    by = collections.defaultdict(bytearray)
    for pi_, (s, p) in enumerate(parts):
        by[s] += p
        partw[pi_] += writer(p) if p else 0
    by["ctl"] += data[:headlen]
    for s, b in by.items():
        sec_raw[s] += len(b); sec_w[s] += writer(bytes(b)); sec_z[s] += zs(bytes(b))
    seg = pools[0][0] // 900
    seg_old[seg] += obuf; seg_new[seg].append(data)
    ticks = deserialise(data, dec)
    for (ctl, rs, rn, rt, refs), (tick, P, c, hw), sn, sh in zip(ticks, pools, snaps, shots_of):
        for col, (md, _) in rs.items(): modecount[(col, md)] += 1
        dec.set_shots(sh)
        Pd, cd, hwd = dec.decode(ctl, rs, rn, rt, refs, sn)
        assert cd == c and hwd == hw and np.array_equal(Pd, P), f"mismatch at tick {tick}"
        nchk += 1
    buf = []; obuf = bytearray(); pools = []; snaps = []; shots_of.clear()

lastshots = []
for it in iterations(path):
    if it["block"] != cur_block:
        flush(); cur_block = it["block"]
    enc.set_shots(lastshots); shots_of.append(lastshots)
    buf.append(list(enc.encode(it["pool"], it["count"], it["hw"], it["snap_before"])))
    lastshots = it["shots"]
    pools.append((it["tick"], it["pool"], it["count"], it["hw"]))
    snaps.append(it["snap_before"])
    obuf += old_bytes(it["recs"])
flush()
MiB = 1048576
print(f"event {EVENT}, xpos {XP}: {nchk} iterations decoded back exactly ({time.time() - t0:.0f} s)")
print(f"{'MiB, per stream block':24s} " + " ".join(f"{k:>9s}" for k in ["raw"] + list(COMP)))
print(f"{'today':24s} " + " ".join(f"{tot_old[k]/MiB:9.3f}" for k in ["raw"] + list(COMP)))
print(f"{'re-encoded':24s} " + " ".join(f"{tot_new[k]/MiB:9.3f}" for k in ["raw"] + list(COMP)))
for k in ("zstd19", "lzma6"):
    o = sum(COMP[k](bytes(v)) for v in seg_old.values())
    n = sum(COMP[k](b"".join(v)) for v in seg_new.values())
    print(f"per 900-tick interval, {k}: today {o/MiB:.3f} MiB, re-encoded {n/MiB:.3f} MiB")
print("sections (each deflated alone per block, attribution): raw / writer / zstd19, MiB")
for s in SECTIONS:
    print(f"   {s:6s} {sec_raw[s]/MiB:8.3f} {sec_w[s]/MiB:8.3f} {sec_z[s]/MiB:8.3f}")
print("predictor chosen (column: mode ticks)")
cols = collections.defaultdict(dict)
for (col, md), n in modecount.items(): cols[col][md] = n
for col in sorted(cols):
    print(f"   d{col:2d}: " + ", ".join(f"{md} {n}" for md, n in sorted(cols[col].items(), key=lambda x: -x[1])))

names = ["ctl", "refs"] + [f"surv d{c}" for c in C.ORDER] + [f"new d{c}" for c in range(27)] + [f"tail d{c}" for c in range(27)]
print("parts, the writer's deflate alone per block (attribution), KiB")
for i, n in sorted(partw.items(), key=lambda x: -x[1])[:45]:
    print(f"   {names[i]:10s} {n/1024:8.1f}")
