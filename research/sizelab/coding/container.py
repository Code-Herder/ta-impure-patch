"""A sectioned container, prototyped: each block is a kind sequence and one section per category,
each record's body going to its category's section, encoded by the category's policy. Decodes
back to the exact payload. Compressed with one zstd context per block (sections concatenated) or
one context per section, alone or chained through a snapshot interval.

    container.py battle N [--udelta D] [--pvals T0|T1]

Sections:
  seq  the kind sequence: u8 kind, varint run length (H3)
  uf   unit deltas' slots and masks (varint gap<<1|same, the XOR list when not same: D4)
  uv   unit deltas' values, minus the slot's previous value (T1), by column ((dword) groups)
  pf   projectile deltas' indices and masks (varint gap, the raw mask: D1)
  pv   projectile deltas' values raw (T0), in record order; projectile images (RK_PROJ) too
  ce   feature-grid cells' bodies
  or   order lists' bodies
  ev   events' bodies (a varint length where the format lets it vary)
  ot   everything else (tick as varint step, player/globals deltas as mask + T1 values, wrecks,
       features, tables, unit images, gone)
"""
import sys, struct, collections, zlib, time, json
import numpy as np
import zstandard
import reencode as R
from common import *
T = R.T
SECTIONS = ["seq", "uf", "uv", "pf", "pv", "pi", "ce", "or", "ev", "ot"]
UT0 = "--uT0" in sys.argv
CAP = int(sys.argv[sys.argv.index("--cap") + 1]) if "--cap" in sys.argv else 0
PSHUF = "shuf" if "--pshuf" in sys.argv else ("col" if "--pcol" in sys.argv else None)
EVK = set(T.EVENTS)

def sec_of(kind):
    if kind == T.RK_UNIT_DELTA: return "u"
    if kind in (T.RK_PROJ_DELTA, T.RK_PROJ): return "p"
    if kind == T.RK_CELLS: return "ce"
    if kind == T.RK_ORDERS: return "or"
    if kind in EVK: return "ev"
    return "ot"

class State:
    def __init__(self):
        self.world = R.World()
        self.last_mask = {}
        self.tick = 0

def encode(p, st, pvals="T0"):
    S = {s: bytearray() for s in SECTIONS}
    ucols = collections.defaultdict(list); pcols = collections.defaultdict(list)
    recs = list(records(p))
    i = 0
    while i < len(recs):
        kind = recs[i][0]; j = i
        while j < len(recs) and recs[j][0] == kind: j += 1
        S["seq"].append(kind); R.varint(j - i, S["seq"])
        prev_slot = -1
        for kk in range(i, j):
            b = recs[kk][1]; sec = sec_of(kind)
            ds = R.delta_spec(kind)
            if kind == T.RK_UNIT_DELTA or kind == T.RK_PROJ_DELTA:
                kb, nb, hd, ndw = ds
                key = struct.unpack_from("<H", b)[0]; mask = bytes(b[2:2 + nb])
                gap = key - prev_slot - 1; prev_slot = key
                prevw = st.world.get(kind, key, ndw)
                if kind == T.RK_UNIT_DELTA:
                    lm = st.last_mask.get((kind, key)); same = lm == mask
                    R.varint(gap << 1 | same, S["uf"])
                    if not same:
                        R.list_code(R.bits_list(bytes(a ^ c for a, c in zip(mask, lm or bytes(nb)))), S["uf"])
                    st.last_mask[(kind, key)] = mask
                    for k2, d in enumerate(R.bits_list(mask)):
                        v = struct.unpack_from("<I", b, hd + 4 * k2)[0]
                        if UT0: S["uv"] += struct.pack("<I", v)
                        else: ucols[d].append((v - prevw[d]) & 0xFFFFFFFF)
                        prevw[d] = v
                else:
                    R.varint(gap, S["pf"]); S["pf"] += mask
                    for k2, d in enumerate(R.bits_list(mask)):
                        v = struct.unpack_from("<I", b, hd + 4 * k2)[0]
                        t = v if pvals == "T0" else (v - prevw[d]) & 0xFFFFFFFF
                        if PSHUF: pcols[d].append(t)
                        else: S["pv"] += struct.pack("<I", t)
                        prevw[d] = v
            elif kind in (T.RK_PLAYER_DELTA, T.RK_GLOBALS_DELTA):
                kb, nb, hd, ndw = ds
                key = struct.unpack_from("<H", b)[0] if kb else 0
                S["ot"] += b[:kb + nb]
                prevw = st.world.get(kind, key, ndw)
                for k2, d in enumerate(R.bits_list(b[kb:kb + nb])):
                    v = struct.unpack_from("<I", b, hd + 4 * k2)[0]
                    S["ot"] += struct.pack("<I", (v - prevw[d]) & 0xFFFFFFFF); prevw[d] = v
            elif kind == T.RK_TICK:
                t = struct.unpack_from("<I", b)[0]
                R.varint(t - st.tick, S["ot"]); st.tick = t
            else:
                if kind in R.IMAGE_OF:
                    dk = R.IMAGE_OF[kind]
                    if kind == T.RK_GLOBALS: st.world.image(dk, 0, b)
                    else: st.world.image(dk, struct.unpack_from("<H", b)[0], b[4:])
                tgt = ("pi" if PSHUF else "pv") if kind == T.RK_PROJ else sec
                if kind in R.FIXED:
                    assert len(b) == R.FIXED[kind]
                elif kind not in (T.RK_ORDERS, T.RK_CELLS):
                    R.varint(len(b), S[tgt])
                S[tgt] += b
        i = j
    for d in sorted(ucols):
        S["uv"] += np.array(ucols[d], dtype=np.uint32).tobytes()
    for d in sorted(pcols):
        a = np.array(pcols[d], dtype=np.uint32)
        S["pv"] += a.view(np.uint8).reshape(-1, 4).T.tobytes() if PSHUF == "shuf" else a.tobytes()
    return S

class Rd:
    def __init__(self, b): self.b = b; self.at = 0
    def take(self, n): v = self.b[self.at:self.at + n]; self.at += n; return bytes(v)
    def var(self): v, self.at = R.rd_varint(self.b, self.at); return v

def decode(S, st, pvals="T0"):
    seq = Rd(S["seq"]); uf = Rd(S["uf"]); pf = Rd(S["pf"]); pv = Rd(S["pv"])
    rd = {k: Rd(S[k]) for k in ("ce", "or", "ev", "ot", "pi")}
    rd["pv"] = pv; pdel = []
    # pass 1: the unit deltas' masks, to size the value columns
    out = []                # list of (kind, body or None for unit deltas filled later)
    udel = []               # (out index, key, mask)
    while seq.at < len(seq.b):
        kind = seq.b[seq.at]; seq.at += 1; n = seq.var()
        prev_slot = -1
        for _ in range(n):
            ds = R.delta_spec(kind)
            if kind == T.RK_UNIT_DELTA:
                kb, nb, hd, ndw = ds
                g = uf.var(); same = g & 1; key = prev_slot + 1 + (g >> 1); prev_slot = key
                if same: mask = st.last_mask[(kind, key)]
                else:
                    x, uf.at = R.list_read(uf.b, uf.at, nb)
                    lm = st.last_mask.get((kind, key)) or bytes(nb)
                    mask = bytes(a ^ c for a, c in zip(x, lm))
                st.last_mask[(kind, key)] = mask
                udel.append((len(out), key, mask, None)); out.append([kind, None])
            elif kind == T.RK_PROJ_DELTA:
                kb, nb, hd, ndw = ds
                key = prev_slot + 1 + pf.var(); prev_slot = key
                mask = pf.take(nb)
                if PSHUF:
                    pdel.append((len(out), key, mask, None)); out.append([kind, None]); continue
                prevw = st.world.get(kind, key, ndw); ws = []
                for d in R.bits_list(mask):
                    t = struct.unpack("<I", pv.take(4))[0]
                    v = t if pvals == "T0" else (t + prevw[d]) & 0xFFFFFFFF
                    prevw[d] = v; ws.append(v)
                out.append([kind, struct.pack("<H", key) + mask + bytes(hd - 2 - nb) + struct.pack(f"<{len(ws)}I", *ws)])
            elif kind in (T.RK_PLAYER_DELTA, T.RK_GLOBALS_DELTA):
                kb, nb, hd, ndw = ds
                head = rd["ot"].take(kb + nb)
                key = struct.unpack_from("<H", head)[0] if kb else 0
                prevw = st.world.get(kind, key, ndw); ws = []
                for d in R.bits_list(head[kb:]):
                    v = (struct.unpack("<I", rd["ot"].take(4))[0] + prevw[d]) & 0xFFFFFFFF
                    prevw[d] = v; ws.append(v)
                out.append([kind, head + bytes(hd - kb - nb) + struct.pack(f"<{len(ws)}I", *ws)])
            elif kind == T.RK_TICK:
                st.tick += rd["ot"].var(); out.append([kind, struct.pack("<I", st.tick)])
            else:
                sec = ("pi" if PSHUF else "pv") if kind == T.RK_PROJ else sec_of(kind)
                r = rd[sec]
                if kind in R.FIXED: ln = R.FIXED[kind]
                elif kind == T.RK_ORDERS:
                    total, first = struct.unpack_from("<HH", r.b, r.at + 4)
                    ln = 8 + T.REC_ORDER_IMAGE * min(total - first, 512)
                elif kind == T.RK_CELLS:
                    ln = 8 + T.REC_CELL_BYTES * struct.unpack_from("<H", r.b, r.at + 4)[0]
                else: ln = r.var()
                b = r.take(ln)
                if kind == T.RK_UNIT:
                    udel.append((len(out), struct.unpack_from("<H", b)[0], None, b))
                elif kind == T.RK_PROJ and PSHUF:
                    pdel.append((len(out), struct.unpack_from("<H", b)[0], None, b))
                elif kind in R.IMAGE_OF:
                    dk = R.IMAGE_OF[kind]
                    if kind == T.RK_GLOBALS: st.world.image(dk, 0, b)
                    else: st.world.image(dk, struct.unpack_from("<H", b)[0], b[4:])
                out.append([kind, b])
    # pass 2: the unit values, by column
    cnt = collections.Counter()
    for _, key, mask, img in udel:
        if mask is not None:
            for d in R.bits_list(mask): cnt[d] += 1
    cols = {}; at = 0
    if not UT0:
        for d in sorted(cnt):
            cols[d] = np.frombuffer(S["uv"][at:at + 4 * cnt[d]], dtype=np.uint32); at += 4 * cnt[d]
    pos = collections.Counter(); uvr = Rd(S["uv"])
    for oi, key, mask, img in udel:
        if mask is None:
            st.world.image(T.RK_UNIT_DELTA, key, img[4:]); continue
        prevw = st.world.get(T.RK_UNIT_DELTA, key, T.REC_UNIT_DWORDS); ws = []
        for d in R.bits_list(mask):
            if UT0: v = struct.unpack("<I", uvr.take(4))[0]
            else: v = (int(cols[d][pos[d]]) + prevw[d]) & 0xFFFFFFFF; pos[d] += 1
            prevw[d] = v; ws.append(v)
        out[oi][1] = struct.pack("<H", key) + mask + bytes(T.REC_UNIT_DELTA_HEAD - 2 - len(mask)) + struct.pack(f"<{len(ws)}I", *ws)
    if PSHUF:
        cnt = collections.Counter()
        for _, key, mask, img in pdel:
            if mask is not None:
                for d in R.bits_list(mask): cnt[d] += 1
        cols = {}; at = 0
        for d in sorted(cnt):
            n = cnt[d]
            raw = np.frombuffer(S["pv"][at:at + 4 * n], dtype=np.uint8); at += 4 * n
            cols[d] = raw.reshape(4, n).T.copy().view(np.uint32).reshape(-1) if PSHUF == "shuf" else raw.view(np.uint32)
        pos = collections.Counter()
        for oi, key, mask, img in pdel:
            if mask is None:
                st.world.image(T.RK_PROJ_DELTA, key, img[4:]); continue
            prevw = st.world.get(T.RK_PROJ_DELTA, key, T.REC_PROJ_DWORDS); ws = []
            for d in R.bits_list(mask):
                t = int(cols[d][pos[d]]); pos[d] += 1
                v = t if pvals == "T0" else (t + prevw[d]) & 0xFFFFFFFF
                prevw[d] = v; ws.append(v)
            out[oi][1] = struct.pack("<H", key) + mask + bytes(T.REC_PROJ_DELTA_HEAD - 2 - len(mask)) + struct.pack(f"<{len(ws)}I", *ws)
    return b"".join(struct.pack("<HH", k, len(b)) + b + bytes(-len(b) % 4) for k, b in out)

def main():
    name = sys.argv[1]; N = int(sys.argv[2])
    pvals = "T1" if "--pT1" in sys.argv else "T0"
    bl = load(name)
    for k, f, l, p, pk in bl:
        if k == RB_STREAM:
            for kind, b in records(p):
                if kind == T.RK_GLOBALS: R.GLOB_DW = len(b) // 4
            break
    snaps = {l for k, f, l, p, pk in bl if k == RB_SNAPSHOT}
    blocks = []; cur = []; chain = 0
    for k, f, l, p, pk in bl:
        if k == RB_SNAPSHOT and N == 0: chain += 1
        if k != RB_STREAM: continue
        if N == 0: blocks.append((chain, p)); continue
        for t, b in iterations(p):
            cur.append(b)
            if len(cur) >= N or t in snaps or (CAP and sum(len(x) for x in cur) >= CAP):
                blocks.append((chain, b"".join(cur))); cur = []
                if t in snaps: chain += 1
    if cur: blocks.append((chain, b"".join(cur)))
    se, sd = State(), State()
    enc = []; ok = True; t0 = time.time(); last_ch = None
    for ch, p in blocks:
        if ch != last_ch:     # coding state that is not world state resets at a snapshot
            se.last_mask = {}; sd.last_mask = {}; last_ch = ch
        if "--blockbase" in sys.argv:   # self-contained blocks: differences only inside the block
            se.world = R.World(); sd.world = R.World(); se.last_mask = {}; sd.last_mask = {}
        S = encode(p, se, pvals)
        enc.append((ch, S))
        if ok and "--nocheck" not in sys.argv:
            if decode(S, sd, pvals) != p: ok = False; print("ROUND TRIP FAILED")
    raw = {s: sum(len(S[s]) for _, S in enc) for s in SECTIONS}
    if "--dump" in sys.argv:
        idx = []; off = 0; cur = None
        tagd = f"ct-N{N}" + ("-bb" if "--blockbase" in sys.argv else "")
        with open(tagd + ".bin", "wb") as f:
            for (ch, S), (_, p) in zip(enc, blocks):
                if cur is not None and ch != cur: idx.append((3, 0, 0, 0, 0, off))
                cur = ch
                b = b"".join(bytes(S[s]) for s in SECTIONS)
                t1 = struct.unpack_from("<I", p, 4)[0]
                idx.append((2, t1, t1, len(b), 0, off)); f.write(b); off += len(b)
        with open(tagd + ".idxbin", "wb") as f:
            f.write(struct.pack("<I", len(idx)))
            for t in idx: f.write(struct.pack("<6I", *t))
        if "--dumponly" in sys.argv: return
    print(f"{name} N{N} pvals {pvals}{' uT0' if UT0 else ''}{' p' + PSHUF if PSHUF else ''}{' blockbase' if '--blockbase' in sys.argv else ''}: {len(enc)} blocks, encoded {time.time()-t0:.0f}s, roundtrip {'OK' if ok else 'FAIL'}; raw MiB " +
          " ".join(f"{s} {v/2**20:.2f}" for s, v in raw.items()) + f" total {sum(raw.values())/2**20:.2f}", flush=True)
    res = {"raw": raw}
    for lv in (3, 9, 19):
        for mode in ("alone", "chain"):
            if lv == 19 and mode == "chain": continue
            for ctxs in ("one", "per-section"):
                t1 = time.perf_counter()
                tot = collections.Counter()
                objs = {}; cur = None
                def comp_for(): return zstandard.ZstdCompressor(level=lv, write_content_size=False)
                for ch, S in enc:
                    if mode == "chain" and ch != cur:
                        for key, o in objs.items(): tot[key] += len(o.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
                        objs = {}; cur = ch
                    parts = [("all", b"".join(bytes(S[s]) for s in SECTIONS))] if ctxs == "one" else [(s, bytes(S[s])) for s in SECTIONS if S[s]]
                    for key, data in parts:
                        if mode == "alone":
                            tot[key] += len(comp_for().compress(data))
                        else:
                            if key not in objs: objs[key] = comp_for().compressobj()
                            tot[key] += len(objs[key].compress(data)) + len(objs[key].flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK))
                for key, o in objs.items(): tot[key] += len(o.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
                dt = time.perf_counter() - t1
                label = f"zstd{lv} {mode} {ctxs}"
                res[label] = dict(tot)
                print(f"  {label:<26}{sum(tot.values())/2**20:8.2f} MiB  {sum(raw.values())/2**20/dt:7.1f} MB/s  " +
                      (" ".join(f"{k} {v/2**20:.2f}" for k, v in sorted(tot.items(), key=lambda kv: SECTIONS.index(kv[0]))) if ctxs != "one" else ""), flush=True)
    json.dump(res, open(f"container-{name}-N{N}-{pvals}{'-uT0' if UT0 else ''}{'-p' + PSHUF if PSHUF else ''}{'-bb' if '--blockbase' in sys.argv else ''}.json", "w"), indent=1)

if __name__ == "__main__":
    main()
