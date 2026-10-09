"""p13: lossy variants of the identity codec, closed loop (the encoder codes against the decoder's
own state, so errors never accumulate past a sample):
  every K-th tick the survivors' corrections; between, the survivors are what the predictors make
  of the last state (dead reckoning: the position moves by the velocity and its drift, the vertical
  velocity by its last change, everything else held); births and deaths at their own tick, exact;
  the compaction's stale tail not sent (it is not drawn and not simulated until an append
  overwrites it); QUANT>0 clears the low QUANT bits of the position and tail corrections.
Reports the size per stream block (the writer's deflate, zstd 19) and the position error of the
decoded pool against the recording, in world units (65,536 a world unit, 16.16), per weapon.

    p13_lossy.py <file> K [QUANT]"""
import sys, struct, zlib, collections, copy
import numpy as np, zstandard
from pool import iterations, match
import codec as C
import codec3 as C3

K = int(sys.argv[2]); QUANT = int(sys.argv[3]) if len(sys.argv) > 3 else 0
MODES = {d: "prev" for d in range(27)}
MODES.update({1: "phys", 2: "phys", 3: "phys", 4: "lin", 5: "lin", 6: "lin", 8: "lin", 25: "idx25"})
ZC = zstandard.ZstdCompressor(level=19)
def writer(b): c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())

enc = C3.Codec3(True); dec = C3.Codec3(True); enc.notail = True
def sync():
    enc.Q, enc.qc, enc.qhw, enc.H, enc.Hv = dec.Q.copy(), dec.qc, dec.qhw, dec.H.copy(), dec.Hv.copy()
    enc.lastnew = dict(dec.lastnew)

tot_w = tot_z = 0
err = collections.defaultdict(list)
buf = bytearray(); cur_block = None
nticks = 0
for it in iterations(sys.argv[1], want_recs=False):
    if it["block"] != cur_block:
        if buf: tot_w += writer(bytes(buf)); tot_z += len(ZC.compress(bytes(buf)))
        buf = bytearray(); cur_block = it["block"]
    P, c, hw = it["pool"], it["count"], it["hw"]
    sample = it["tick"] % K == 0 or it["snap_before"]
    Q, qc = enc.Q, enc.qc
    m = C.prefix_map(match(Q, qc, P, c), qc)
    ns = len(m)
    # what the decoder will hold: its own prediction of the survivors between samples (closed
    # loop), the truth at a sample (QUANT: the prediction plus the correction's coarse part)
    Pq = P.copy()
    if ns and (not sample or QUANT):
        probe = copy.deepcopy(dec)
        nd_ = qc - ns
        dead = np.setdiff1d(np.arange(qc), m)
        ctl0 = [c, hw, len(dead)]; last = -1
        for k in dead: ctl0.append(int(k) - last - 1); last = int(k)
        ctl0.append(0)
        z = {col: (MODES[col], np.zeros(ns, np.uint32)) for col in range(27)}
        zn = {col: np.zeros(c - ns, np.uint32) for col in range(27)}
        Pp, _, _ = probe.decode(ctl0, z, zn, {col: np.zeros(0, np.uint32) for col in range(27)},
                                np.zeros(c - ns, np.uint32), it["snap_before"])
        if not sample:
            Pq[:ns] = Pp[:ns]
        else:
            r = (P[:ns, 1:7].astype(np.int64) - Pp[:ns, 1:7].astype(np.int64)) % 2**32
            r = np.where(r >= 2**31, r - 2**32, r)
            r = (r >> QUANT) << QUANT
            Pq[:ns, 1:7] = ((Pp[:ns, 1:7].astype(np.int64) + r) % 2**32).astype(np.uint32)
    ctl, rs, rn, rt, refs = enc.encode(Pq, c, hw, it["snap_before"], m)
    nd = ctl[2]
    ctl = ctl[:3 + nd] + [0]
    nnew = c - ns
    refs = refs[:nnew]
    rt = {col: np.zeros(0, np.uint32) for col in range(27)}
    rsel = {col: (MODES[col], rs[col][MODES[col]]) for col in range(27)}
    Pd, cd, hwd = dec.decode(ctl, rsel, rn, rt, refs, it["snap_before"])
    assert np.array_equal(Pd[:c], Pq[:c]), it["tick"]
    sync()
    # error of the survivors' position against the recording
    if ns:
        d = (Pd[:ns, 1:4].astype(np.int64) - P[:ns, 1:4].astype(np.int64))
        d = (d + 2**31) % 2**32 - 2**31
        e = np.abs(d).max(axis=1) / 65536.0
        for j in range(ns):
            err[int(P[j, 0])].append(e[j])
    # serialise like p5 (fixed modes, no choice header)
    buf += C.leb([len(ctl)] + ctl) + C.leb(refs)
    for col in C.ORDER:
        if sample: buf += C.leb(C.zz(rsel[col][1]))
        else: assert not rsel[col][1].any()
    for col in range(27):
        buf += C.leb(C.zz(rn[col]) if col else rn[col])
    nticks += 1
if buf: tot_w += writer(bytes(buf)); tot_z += len(ZC.compress(bytes(buf)))
MiB = 1048576
print(f"K {K} QUANT {QUANT}: writer {tot_w/MiB:.3f} MiB, zstd19 {tot_z/MiB:.3f} MiB per stream block")
allerr = np.concatenate([np.array(v) for v in err.values()])
print(f"position error over all survivor-ticks (wu): p50 {np.percentile(allerr,50):.3f} p90 {np.percentile(allerr,90):.3f} p99 {np.percentile(allerr,99):.3f} max {allerr.max():.2f}; exact {np.mean(allerr==0)*100:.1f} %")
for w, v in sorted(err.items(), key=lambda x: -len(x[1]))[:7]:
    v = np.array(v)
    print(f"   weapon {w:4d}: n {len(v):7d} exact {np.mean(v==0)*100:5.1f} % p99 {np.percentile(v,99):8.3f} max {v.max():8.2f} wu")
