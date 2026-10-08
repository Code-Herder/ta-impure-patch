"""p8: the unit record's three weapon slots (+0x04, stride 0x1C) in the unit deltas: per slot field,
how often it changes, how often a linear prediction (2 prev - prev2, per unit) misses, and how often
the reload countdown (+0x14, u16) is the AutoAim decrement (prev - 1 while > 0, 0x49E1C8..0x49E1D2)."""
import sys, struct, collections
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T

bl, stop = T.blocks(sys.argv[1])
ND = T.REC_UNIT_DWORDS
img = {}       # slot -> list of dwords
hist = {}      # slot -> previous image (for lin), by tick
lastchg = {}   # (slot, dword) -> tick of the last change
FIELDS = []    # (name, slot index, offset in the unit, size)
for s in range(3):
    b = 0x04 + 0x1C * s
    FIELDS += [(f"w{s+1} target", b + 0x0, 4), (f"w{s+1} aimdone", b + 0x8, 4),
               (f"w{s+1} dist", b + 0x10, 4), (f"w{s+1} reload", b + 0x14, 2),
               (f"w{s+1} heading", b + 0x16, 2), (f"w{s+1} pitch", b + 0x18, 2),
               (f"w{s+1} stockpile", b + 0x1A, 1), (f"w{s+1} flags", b + 0x1B, 1)]
chg = collections.Counter(); linmiss = collections.Counter(); dec1 = collections.Counter()
tick = 0
prevval = {}   # (slot, field) -> (value at last tick seen, value before)
def field(im, off, size):
    b = struct.pack(f"<{ND}I", *im)
    return int.from_bytes(b[off:off + size], "little")
nunit_ticks = 0
for kind, first, last, payload in bl:
    if kind != T.RB_STREAM:
        continue
    for rk, b in T.records(payload):
        if rk == T.RK_TICK:
            tick = struct.unpack_from("<I", b)[0]
        elif rk == T.RK_UNIT:
            s = struct.unpack_from("<H", b)[0]
            img[s] = list(struct.unpack_from(f"<{ND}I", b, 4))
            for name, off, size in FIELDS:
                prevval[(s, name)] = (field(img[s], off, size), None, tick)
        elif rk == T.RK_UNIT_DELTA:
            s = struct.unpack_from("<H", b)[0]
            bits = b[2:2 + T.REC_UNIT_BITS]
            words = struct.unpack_from(f"<{(len(b) - T.REC_UNIT_DELTA_HEAD) // 4}I", b, T.REC_UNIT_DELTA_HEAD)
            im = img.get(s)
            if im is None: continue
            k = 0; touched = False
            for d in range(ND):
                if bits[d >> 3] >> (d & 7) & 1:
                    im[d] = words[k]; k += 1
                    if 1 <= d <= 22: touched = True
            if not touched: continue
            for name, off, size in FIELDS:
                v = field(im, off, size)
                pv, ppv, pt = prevval.get((s, name), (v, None, tick))
                if v != pv:
                    chg[name] += 1
                    if "reload" in name and pv > 0 and v == pv - 1 and pt == tick - 1:
                        dec1[name] += 1
                    lin = None if ppv is None else (2 * pv - ppv) % (1 << (8 * size))
                    if lin != v:
                        linmiss[name] += 1
                prevval[(s, name)] = (v, pv if pt == tick - 1 else None, tick)
print(f"{'field':16s} {'changes':>9s} {'lin misses':>10s} {'decrement by 1':>14s}")
for name, off, size in FIELDS:
    print(f"{name:16s} {chg[name]:9d} {linmiss[name]:10d} {dec1[name]:14d}")
