"""Independent check of the engine rules the units and fidelity reports disagree on, on the
battle's unit images reconstructed from the stream (units.npz holds the stream's records as
tarec read them; the replay of deltas below is my own).
  velocity: vx = -S(h,s), vz = -C(h,s) with S/C = 0x4B70EF/0x4B7123 (DISASSEMBLED by me):
            idx = ((h + 0x20 [0x4020 for cos]) >> 7) & 511, (T[idx]*s + 0x1000) >> 13, low 32 bits
  heading:  h(t) = h(t-1) + tr(t)  (mover+0x24, i16)          0x43CFD1..0x43CFDB
  position: x(t) = x(t-1) + vx(t)  when mover+0x2E bit 2 clear
"""
import struct, sys, numpy as np
Z = sys.argv[1]; EXE = sys.argv[2]
d = open(EXE, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]; nsec = struct.unpack_from("<H", d, pe + 6)[0]
opt = struct.unpack_from("<H", d, pe + 20)[0]; base = struct.unpack_from("<I", d, pe + 52)[0]
for i in range(nsec):
    name, vsize, vaddr, rsize, rptr = struct.unpack_from("<8sIIII", d, pe + 24 + opt + 40 * i)
    if vaddr <= 0x509F00 - base < vaddr + max(vsize, rsize):
        TAB = np.frombuffer(d[rptr + 0x509F00 - base - vaddr:][:1024], "<i2").astype(np.int64)
z = np.load(Z)
ops, dslot, dmask, dvoff, vals, images = (z[k] for k in ("ops", "dslot", "dmask", "dvoff", "vals", "images"))
SL, NDW = 2501, 82
S = np.zeros((SL, NDW), np.uint32); live = np.zeros(SL, bool)
def f(S, off, n, signed):
    b = np.ascontiguousarray(S.view(np.uint8)[:, off:off + n])
    return b.view({(2, 1): "<i2", (2, 0): "<u2", (4, 1): "<i4", (4, 0): "<u4", (1, 0): "u1"}[(n, int(signed))])[:, 0].astype(np.int64)
M = 0x118
def snap(S):
    return dict(h=f(S, 0x66, 2, 0), tr=f(S, M + 0x24, 2, 1), sp=f(S, M + 0x20, 4, 1),
                vx=f(S, M + 0x08, 4, 1), vy=f(S, M + 0x0C, 4, 1), vz=f(S, M + 0x10, 4, 1),
                x=f(S, 0x6A, 4, 1), z=f(S, 0x72, 4, 1), bits=f(S, M + 0x2E, 1, 0),
                mover=f(S, 0x00, 4, 0))
def S_(h, s):
    return (TAB[((h + 0x20) >> 7) & 511] * s + 0x1000) >> 13
def C_(h, s):
    return (TAB[((h + 0x4020) >> 7) & 511] * s + 0x1000) >> 13
def s32(a): a = a & 0xFFFFFFFF; return np.where(a >= 1 << 31, a - (1 << 32), a)
tick_rows = np.flatnonzero(ops[:, 0] == 0); ends = list(tick_rows[1:]) + [len(ops)]
prev = None; prev_live = None
cnt = dict(n=0, vel=0, head=0, posn=0, pos_ok=0, vy_nz=0, mov=0)
for r0, r1 in zip(tick_rows, ends):
    body = ops[r0 + 1:r1]
    created = []
    for op, bi, a, b in body:
        if op == 1: S[a] = images[b]; live[a] = True; created.append(a)
        elif op == 3: live[a] = False; S[a] = 0
        elif op == 2:
            m = np.unpackbits(dmask[b], bitorder="little")[:NDW].astype(bool)
            row = S[a].copy(); row[m] = vals[dvoff[b]:dvoff[b + 1]]; S[a] = row
    cur = snap(S)
    ok = live & (cur["mover"] != 0)
    if prev is not None:
        ok2 = ok & prev_live
        ok2[created] = False
        cnt["n"] += ok2.sum()
        vxp = s32(-S_(cur["h"], cur["sp"])); vzp = s32(-C_(cur["h"], cur["sp"]))
        cnt["vel"] += (ok2 & (vxp == cur["vx"]) & (vzp == cur["vz"])).sum()
        cnt["vy_nz"] += (ok2 & (cur["vy"] != 0)).sum()
        cnt["mov"] += (ok2 & ((cur["vx"] != 0) | (cur["vz"] != 0))).sum()
        cnt["head"] += (ok2 & (((prev["h"] + cur["tr"]) & 0xFFFF) == cur["h"])).sum()
        nb = ok2 & ((cur["bits"] & 4) == 0)
        cnt["posn"] += nb.sum()
        cnt["pos_ok"] += (nb & (s32(prev["x"] + cur["vx"]) == cur["x"]) & (s32(prev["z"] + cur["vz"]) == cur["z"])).sum()
    prev = cur; prev_live = ok.copy()
print({k: int(v) for k, v in cnt.items()})
