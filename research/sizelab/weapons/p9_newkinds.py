"""p9: the new-record section by kind: a burst copy (a record of the old pool of the same weapon and
attacker with burst shots left), a burst template (+0x60 > 0), a single shot. Per kind: records,
raw LEB bytes of the residuals against the reference codec3 chose, and the same compressed alone
(zstd 19 of each kind's residual columns, all blocks together, an attribution)."""
import sys, struct, collections
import numpy as np, zstandard
from pool import iterations
import codec as C
import codec3 as C3

ZC = zstandard.ZstdCompressor(level=19)
enc = C3.Codec3(True)
cols = collections.defaultdict(lambda: collections.defaultdict(list))
nrec = collections.Counter(); zero_dw = collections.Counter()
for it in iterations(sys.argv[1], want_recs=False):
    Q, qc = enc.Q, enc.qc
    ctl, rs, rn, rt, refs = enc.encode(it["pool"], it["count"], it["hw"], it["snap_before"])
    P, c = it["pool"], it["count"]
    nnew = len(rn[0]); ns = c - nnew
    for i in range(nnew):
        row = P[ns + i]
        burst = struct.unpack_from("<H", row.tobytes(), 0x60)[0]
        att = struct.unpack_from("<I", row.tobytes(), 0x52)[0]
        if burst:
            kind = "template"
        else:
            tm = [k for k in range(qc) if Q[k, 0] == row[0] and
                  struct.unpack_from("<I", Q[k].tobytes(), 0x52)[0] == att and
                  struct.unpack_from("<H", Q[k].tobytes(), 0x60)[0] > 0]
            kind = "burst copy" if tm else "single shot"
        nrec[kind] += 1
        for col in range(27):
            v = int(rn[col][i])
            cols[kind][col].append(v)
            zero_dw[kind] += v == 0
print(f"{'kind':12s} {'records':>8s} {'zero dwords/rec':>15s} {'LEB raw KB':>10s} {'zstd19 KB':>9s}")
for kind, n in nrec.items():
    raw = b"".join(C.leb(C.zz(np.array(cols[kind][col], np.uint32))) for col in range(27))
    z = sum(len(ZC.compress(C.leb(C.zz(np.array(cols[kind][col], np.uint32))))) for col in range(27))
    print(f"{kind:12s} {n:8d} {zero_dw[kind]/n:15.1f} {len(raw)/1024:10.1f} {z/1024:9.1f}")
