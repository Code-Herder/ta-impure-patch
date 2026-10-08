"""Re-encode the stream's framing (container-level, every category), decode it back, and measure.

Framing variants (cumulative):
  H0  today: u16 kind, u16 len, body, pad to a dword
  H1  u8 kind, varint len, body, no pad
  H2  u8 kind, the length implicit where the format already fixes it or derives it (fixed kinds,
      deltas from the mask's popcount, RK_ORDERS from total/first, RK_CELLS from cells), else varint
  H3  H2, consecutive records of one kind as one run: u8 kind, varint count
Delta heads (unit, projectile deltas):
  D0  u16 slot, the mask, the reserved zeros (today)
  D1  varint slot gap from the run's previous slot, the mask, no reserved bytes
  D2  varint (gap << 1 | same), same = this slot's mask equals its last one; the mask only if not
  D3  D2, a mask that differs coded as a varint list of set-bit gaps (count, gaps)
  D4  D2, a mask that differs coded as the XOR against the slot's last mask, as a varint list
Values (the changed dwords of unit/projectile/player/globals deltas):
  T0 raw; T1 minus the key's previous value (the writer's copy); T2 XOR it
Layout:
  L0 one stream, values inline;  L1 frame stream then values in record order;
  L2 values by column ((kind, dword) groups);  L3 L2 with each column split into byte planes

Coding state that is not world state (a slot's last mask) resets at every block, so any block
decodes given the world at its start (which the snapshot or the reader's copy gives).
"""
import sys, struct, collections, zlib, time, json
import numpy as np
import zstandard
from common import *

K = dict(TICK=1, UNIT=2, UNIT_DELTA=3, UNIT_GONE=4, PLAYER=5, PLAYER_DELTA=6, TABLE_SIZE=7,
         TABLE_SPAN=8, ORDERS=21, CELLS=34, WRECK=35, FEATURES=36, GLOBALS=38, GLOBALS_DELTA=39,
         PROJ_HEAD=40, PROJ=41, PROJ_DELTA=42)
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
print("kinds", T.RK_ORDERS, T.RK_CELLS, T.RK_PROJ_DELTA, T.RK_GLOBALS_DELTA, file=sys.stderr)
FIXED = {T.RK_TICK: 4, T.RK_UNIT: 4 + T.REC_UNIT_IMAGE, T.RK_UNIT_GONE: 4, T.RK_PLAYER: 4 + T.REC_PLAYER_IMAGE,
         T.RK_TABLE_SIZE: 8, T.RK_PROJ_HEAD: 8, T.RK_PROJ: 4 + T.REC_PROJ_IMAGE, T.RK_WRECK: 4 + T.REC_WRECK_BYTES,
         T.RK_FEATURES: 16, T.RK_EV_CREATE: 28, T.RK_EV_DEATH: 12 + T.REC_UNIT_IMAGE, T.RK_EV_SHOT: 36,
         T.RK_EV_INTERCEPT: 16, T.RK_EV_START: 28, T.RK_EV_BUILT: 8, T.RK_EV_STATE: 8, T.RK_EV_SPRAY: 48,
         T.RK_EV_DRAW: 12, T.RK_EV_FEATURE_END: 8, T.RK_EV_DEATH_END: 4, T.RK_POV: 32}
# delta kinds: (key bytes, mask bytes, head bytes (to the first value), dwords)
GLOB_DW = None
def delta_spec(kind):
    if kind == T.RK_UNIT_DELTA: return (2, T.REC_UNIT_BITS, T.REC_UNIT_DELTA_HEAD, T.REC_UNIT_DWORDS)
    if kind == T.RK_PROJ_DELTA: return (2, T.REC_PROJ_BITS, T.REC_PROJ_DELTA_HEAD, T.REC_PROJ_DWORDS)
    if kind == T.RK_PLAYER_DELTA: return (2, T.REC_PLAYER_BITS, T.REC_PLAYER_DELTA_HEAD, T.REC_PLAYER_DWORDS)
    if kind == T.RK_GLOBALS_DELTA:
        nb = (GLOB_DW + 7) // 8
        return (0, nb, (nb + 3) & ~3, GLOB_DW)
    return None
IMAGE_OF = {T.RK_UNIT: T.RK_UNIT_DELTA, T.RK_PROJ: T.RK_PROJ_DELTA, T.RK_PLAYER: T.RK_PLAYER_DELTA,
            T.RK_GLOBALS: T.RK_GLOBALS_DELTA}
SLOTTED = (T.RK_UNIT_DELTA, T.RK_PROJ_DELTA)

def varint(n, out):
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80); n >>= 7
    out.append(n)

def rd_varint(b, at):
    n = s = 0
    while True:
        c = b[at]; at += 1
        n |= (c & 0x7F) << s; s += 7
        if c < 0x80: return n, at

def popcount(m):
    return sum(bin(x).count("1") for x in m)

def bits_list(m):
    out = []
    for i, x in enumerate(m):
        while x:
            l = x & -x; out.append(i * 8 + l.bit_length() - 1); x ^= l
    return out

def list_code(idx, out):
    varint(len(idx), out); p = -1
    for i in idx:
        varint(i - p - 1, out); p = i

def list_read(b, at, nbytes):
    n, at = rd_varint(b, at); m = bytearray(nbytes); p = -1
    for _ in range(n):
        g, at = rd_varint(b, at); p += g + 1; m[p >> 3] |= 1 << (p & 7)
    return bytes(m), at


class World:
    """Previous value of each delta kind's dwords, by key: what the writer's copy holds."""
    def __init__(self):
        self.prev = {}
    def image(self, dk, key, img):
        self.prev[(dk, key)] = list(struct.unpack_from(f"<{len(img) // 4}I", img))
    def get(self, dk, key, ndw):
        p = self.prev.get((dk, key))
        if p is None:
            p = self.prev[(dk, key)] = [0] * ndw
        return p


def body_len_implicit(kind, b_or_head):
    return None


def encode_block(p, H, D, Tv, world, last_mask=None):
    """One stream payload -> (frame bytes, values list of (col, value-after-transform)) or, for L0,
    frame bytes with the values inline (returned as a third item: positions)."""
    fr = bytearray()
    vals = []          # (col, transformed u32, inline position in fr) -- position for L0
    if last_mask is None: last_mask = {}
    recs = []
    at = 0; n = len(p)
    while at + 4 <= n:
        kind, ln = struct.unpack_from("<HH", p, at)
        recs.append((kind, p[at + 4:at + 4 + ln]))
        at += (4 + ln + 3) & ~3
    i = 0
    while i < len(recs):
        kind = recs[i][0]
        j = i
        if H == 3:
            while j < len(recs) and recs[j][0] == kind: j += 1
            fr.append(kind); varint(j - i, fr)
        else:
            j = i + 1
        prev_slot = -1
        for kk in range(i, j):
            b = recs[kk][1]
            ln = len(b)
            ds = delta_spec(kind)
            # the record's header
            if H == 0:
                fr += struct.pack("<HH", kind, ln)
            elif H == 1:
                fr.append(kind); varint(ln, fr)
            else:
                if H == 2: fr.append(kind)
                implicit = (kind in FIXED or ds is not None or kind in (T.RK_ORDERS, T.RK_CELLS))
                if kind in FIXED: assert ln == FIXED[kind], (kind, ln)
                if not implicit: varint(ln, fr)
            # the body
            if ds is not None and (D > 0 or Tv > 0):
                kb, nb, hd, ndw = ds
                key = struct.unpack_from("<H", b)[0] if kb else 0
                mask = bytes(b[kb:kb + nb])
                assert not any(b[kb + nb:hd])
                cnt = popcount(mask)
                assert ln == hd + 4 * cnt
                if kind in SLOTTED and D >= 1:
                    gap = key - prev_slot - 1 if H == 3 else key
                    assert gap >= 0
                    prev_slot = key
                    if D >= 2:
                        lm = last_mask.get((kind, key))
                        same = lm == mask
                        varint(gap << 1 | same, fr)
                        if not same:
                            if D == 2: fr += mask
                            elif D == 3: list_code(bits_list(mask), fr)
                            else:
                                x = bytes(a ^ c for a, c in zip(mask, lm or bytes(nb)))
                                list_code(bits_list(x), fr)
                        last_mask[(kind, key)] = mask
                    else:
                        varint(gap, fr); fr += mask
                else:
                    fr += b[:kb + nb] if D >= 1 else b[:hd]
                prevw = world.get(kind, key, ndw)
                for k2, d in enumerate(bits_list(mask)):
                    v = struct.unpack_from("<I", b, hd + 4 * k2)[0]
                    t = v if Tv == 0 else ((v - prevw[d]) & 0xFFFFFFFF if Tv == 1 else v ^ prevw[d])
                    prevw[d] = v
                    vals.append((kind * 256 + d, t, len(fr)))
            else:
                if kind in IMAGE_OF:
                    dk = IMAGE_OF[kind]
                    if kind == T.RK_GLOBALS:
                        world.image(dk, 0, b)
                    else:
                        world.image(dk, struct.unpack_from("<H", b)[0], b[4:])
                elif ds is not None:
                    # T0/D0 path: still keep the world in step for later variants? (not needed)
                    pass
                fr += b
                if H == 0:
                    fr += bytes(-ln % 4)
        i = j
    return fr, vals


def decode_block(fr, values, H, D, Tv, world, last_mask=None):
    """Inverse of encode_block, given the frame stream and the values in record order."""
    out = bytearray(); at = 0; vi = 0
    if last_mask is None: last_mask = {}
    while at < len(fr):
        if H == 0:
            kind, ln = struct.unpack_from("<HH", fr, at); at += 4; cnt_run = 1
        else:
            kind = fr[at]; at += 1
            if H == 3:
                cnt_run, at = rd_varint(fr, at)
            else:
                cnt_run = 1
        prev_slot = -1
        for r in range(cnt_run):
            ds = delta_spec(kind)
            if H == 1:
                ln, at = rd_varint(fr, at)
            elif H >= 2 and not (kind in FIXED or ds is not None or kind in (T.RK_ORDERS, T.RK_CELLS)):
                ln, at = rd_varint(fr, at)
            if H >= 2 and kind in FIXED: ln = FIXED[kind]
            if ds is not None and (D > 0 or Tv > 0):
                kb, nb, hd, ndw = ds
                if kind in SLOTTED and D >= 1:
                    if D >= 2:
                        g, at = rd_varint(fr, at); same = g & 1; g >>= 1
                    else:
                        g, at = rd_varint(fr, at)
                    key = prev_slot + 1 + g if H == 3 else g
                    prev_slot = key
                    if D >= 2:
                        if same: mask = last_mask[(kind, key)]
                        elif D == 2: mask = bytes(fr[at:at + nb]); at += nb
                        elif D == 3: mask, at = list_read(fr, at, nb)
                        else:
                            x, at = list_read(fr, at, nb)
                            lm = last_mask.get((kind, key)) or bytes(nb)
                            mask = bytes(a ^ c for a, c in zip(x, lm))
                        last_mask[(kind, key)] = mask
                    else:
                        mask = bytes(fr[at:at + nb]); at += nb
                    head = struct.pack("<H", key) + mask
                else:
                    head = bytes(fr[at:at + kb + nb]) if D >= 1 else bytes(fr[at:at + hd])
                    at += len(head)
                    key = struct.unpack_from("<H", head)[0] if kb else 0
                    mask = head[kb:kb + nb]
                head = head[:kb + nb] + bytes(hd - kb - nb)
                prevw = world.get(kind, key, ndw)
                ws = []
                for d in bits_list(mask):
                    t = values[vi]; vi += 1
                    v = t if Tv == 0 else ((t + prevw[d]) & 0xFFFFFFFF if Tv == 1 else t ^ prevw[d])
                    prevw[d] = v; ws.append(v)
                b = head + struct.pack(f"<{len(ws)}I", *ws)
            else:
                if H >= 2 and kind == T.RK_ORDERS:
                    total, first = struct.unpack_from("<HH", fr, at + 4)
                    ln = 8 + T.REC_ORDER_IMAGE * min(total - first, T.REC_ORDERS_CHUNK if hasattr(T, "REC_ORDERS_CHUNK") else 512)
                elif H >= 2 and kind == T.RK_CELLS:
                    cells = struct.unpack_from("<H", fr, at + 4)[0]
                    ln = 8 + T.REC_CELL_BYTES * cells
                elif H >= 2 and ds is not None:   # D0/T0 delta with implicit length
                    kb, nb, hd, ndw = ds
                    ln = hd + 4 * popcount(fr[at + kb:at + kb + nb])
                b = bytes(fr[at:at + ln]); at += ln
                if H == 0: at += -ln % 4
                if kind in IMAGE_OF:
                    dk = IMAGE_OF[kind]
                    if kind == T.RK_GLOBALS: world.image(dk, 0, b)
                    else: world.image(dk, struct.unpack_from("<H", b)[0], b[4:])
            out += struct.pack("<HH", kind, len(b)) + b + bytes(-len(b) % 4)
    assert vi == len(values)
    return bytes(out)


def layout(fr, vals, L):
    """-> list of byte sections the block is compressed as (one section = one compressor call over
    their concatenation; sections are concatenated)."""
    if L == 0:
        out = bytearray(); last = 0
        for col, t, pos in vals:
            out += fr[last:pos]; out += struct.pack("<I", t); last = pos
        out += fr[last:]
        return bytes(out)
    v = np.array([t for _, t, _ in vals], dtype=np.uint32)
    if L == 1:
        return bytes(fr) + v.tobytes()
    cols = np.array([c for c, _, _ in vals], dtype=np.int32)
    order = np.argsort(cols, kind="stable")
    vs = v[order]
    if L == 2:
        return bytes(fr) + vs.tobytes()
    # L3: byte planes within each column
    cs = cols[order]
    out = bytearray(fr)
    bounds = np.flatnonzero(np.diff(cs)) + 1
    for seg in np.split(vs, bounds):
        out += seg.view(np.uint8).reshape(-1, 4).T.tobytes()
    return bytes(out)


def unlayout(blob, frlen, vals_cols, L):
    """Values in record order from a laid-out blob (the frame's length and the columns known)."""
    if L == 1:
        return [int(x) for x in np.frombuffer(blob[frlen:], dtype=np.uint32)]
    cols = np.array(vals_cols, dtype=np.int32)
    order = np.argsort(cols, kind="stable")
    if L == 2:
        vs = np.frombuffer(blob[frlen:], dtype=np.uint32)
    else:
        cs = cols[order]
        bounds = list(np.flatnonzero(np.diff(cs)) + 1) + [len(cs)]
        parts = []; at = frlen; s = 0
        for e in bounds:
            n = e - s
            planes = np.frombuffer(blob[at:at + 4 * n], dtype=np.uint8).reshape(4, n)
            parts.append(planes.T.copy().view(np.uint32).reshape(-1)); at += 4 * n; s = e
        vs = np.concatenate(parts) if parts else np.zeros(0, np.uint32)
    v = np.empty_like(vs); v[order] = vs
    return [int(x) for x in v]


def comps():
    z3 = zstandard.ZstdCompressor(level=3); z19 = zstandard.ZstdCompressor(level=19)
    z9 = zstandard.ZstdCompressor(level=9)
    def defl(b):
        c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
    return {"deflate2K": defl, "zstd3": lambda b: len(z3.compress(b)), "zstd9": lambda b: len(z9.compress(b)),
            "zstd19": lambda b: len(z19.compress(b))}


def main():
    global GLOB_DW
    name = sys.argv[1]
    variants = [tuple(int(c) for c in v.split(",")) for v in sys.argv[2:]]   # H,D,T,L
    bl = load(name)
    streams = [p for k, f, l, p, pk in bl if k == RB_STREAM]
    # the globals' dword count, from the first RK_GLOBALS
    for p in streams:
        for kind, b in records(p):
            if kind == T.RK_GLOBALS: GLOB_DW = len(b) // 4; break
        if GLOB_DW: break
    C = comps()
    results = {}
    for (H, D, Tv, L) in variants:
        t0 = time.time()
        we, wd = World(), World()
        tot = collections.Counter(); raw = 0; frb = 0; vb = 0
        check = True
        for bi, p in enumerate(streams):
            fr, vals = encode_block(p, H, D, Tv, we)
            blob = layout(fr, vals, L)
            raw += len(blob); frb += len(fr); vb += 4 * len(vals)
            for cn, f in C.items():
                if cn == "zstd19" and "--no19" in sys.argv: continue
                tot[cn] += f(blob)
            if check:
                if L == 0:
                    # inline: rebuild frame/values by re-encoding is circular; decode needs frame and values split
                    values = [t for _, t, _ in vals]
                else:
                    values = unlayout(blob, len(fr), [c for c, _, _ in vals], L)
                back = decode_block(fr if L == 0 else blob[:len(fr)], values, H, D, Tv, wd)
                if back != p:
                    print("ROUND TRIP FAILED at block", bi); check = False
        key = f"H{H}D{D}T{Tv}L{L}"
        results[key] = {"raw": raw, "frame": frb, "values": vb, **tot, "roundtrip": check}
        print(key, f"raw {raw/2**20:.1f} MiB (frame {frb/2**20:.1f} values {vb/2**20:.1f})  " +
              "  ".join(f"{cn} {v/2**20:.2f}" for cn, v in tot.items()) + f"  roundtrip {'OK' if check else 'FAIL'}  {time.time()-t0:.0f}s", flush=True)
    json.dump(results, open(f"reencode-{name}-{'_'.join(sys.argv[2:])}.json", "w"), indent=1)

if __name__ == "__main__":
    main()
