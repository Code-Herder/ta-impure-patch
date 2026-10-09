#!/usr/bin/env python3
"""run3.py -- frame3 (frame2 + the air step's rules) on a recording's units.npz: encode per stream
block, decode the bytes back and compare every live unit at every iteration, compress each block's
section alone, and attribute the bytes to aircraft and the rest.

    run3.py ruleset=a,b,... [tag=name] [verify=1] [limit=GT]

Rule names: heading velocity position stamp counts (frame2's) and air_v air_rot air_vy vz_sat
speed accs bank y foot sight m26 (this study's).  Writes results.jsonl and <tag>.lanes.json.
"""
import json
import sys
import time
import zlib

import numpy as np

import frame3
import sim
import airdefs
from run import comp_all
from frame import varint, zz

MiB = 1 << 20


def defl(b):
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(b) + c.flush())


def main(argv):
    opts = dict(a.split("=", 1) for a in argv)
    rs = tuple(opts["ruleset"].split(","))
    tag = opts.get("tag", "f3-" + "-".join(rs))
    z = sim.load(opts.get("npz", "units.npz"))
    reset = set(int(t) for t in z["snap_ticks"])
    defs = dict(airdefs.DEF)
    for pair in filter(None, opts.get("typemap", "").split(",")):      # this recording's id: air-war's id
        a, b = (int(v) for v in pair.split(":"))
        defs[a] = airdefs.DEF[b]
    if "only" in opts:                                               # keep only these ids as aircraft
        keep = {int(v) for v in opts["only"].split(":")}
        defs = {t: d for t, d in defs.items() if t in keep}
    tt = frame3.TypeTable(defs, airdefs.GRAVITY)
    cls_air = tt.air.copy()                    # the class split, whatever the rules use
    if opts.get("noair") == "1":
        tt.air[:] = False
    enc = frame3.Codec(tt, rs, reset_ticks=reset)
    dec = frame3.Codec(tt, rs, reset_ticks=reset) if opts.get("verify", "1") == "1" else None
    ln = enc.ln
    LT = ln.index[0xA6]
    sections = []
    st = {"units": 0, "air_units": 0, "residuals": 0, "checked_ticks": 0, "checked_units": 0,
          "mismatch_ticks": 0}
    nzc = np.zeros((2, ln.n), np.int64)        # [other, air] nonzero residuals per lane
    vbc = np.zeros((2, ln.n), np.int64)        # varint bytes per lane and class
    colk = np.zeros(ln.n, np.int64)            # per-lane column deflated alone
    frm = {"gaps": 0, "mask dict": 0, "mask ids": 0}
    t0 = time.time()

    def flush(group):
        if not group:
            return
        fr, cls = [], []
        for tk in group:
            s, R = enc.encode_tick(tk)
            fr.append((s, R))
            c = cls_air[ln.split(tk.S[s])[:, LT]] if len(s) else np.zeros(0, bool)
            cls.append(c)
            st["units"] += len(s)
            st["air_units"] += int(c.sum())
            st["residuals"] += int((R != 0).sum())
        buf = enc.block_bytes(fr)
        sections.append(buf)
        R = np.concatenate([r for _, r in fr])
        C = np.concatenate(cls).astype(np.int64)
        Mk = R != 0
        gaps = varint(np.concatenate([np.diff(np.concatenate(([-1], s))) - 1 for s, _ in fr]))
        packed = np.packbits(Mk, axis=1, bitorder="little")
        uniq = {}
        ids = np.array([uniq.setdefault(r.tobytes(), len(uniq)) for r in packed], np.int64)
        idb = ids.astype(np.uint8).tobytes() if len(uniq) <= 256 else varint(ids)
        for k, b in (("gaps", gaps), ("mask dict", b"".join(uniq)), ("mask ids", idb)):
            frm[k] += defl(b)
        for j in range(ln.n):
            col = R[Mk[:, j], j]
            if len(col):
                colk[j] += defl(varint(zz(col)))
                cc = C[Mk[:, j]]
                for k in (0, 1):
                    sel = cc == k
                    if sel.any():
                        nzc[k, j] += int(sel.sum())
                        vbc[k, j] += len(varint(zz(col[sel])))
        if dec is not None:
            meta = [(tk.gametime, tk.live, tk.created, tk.gone, tk.S) for tk in group]
            got = dec.decode_block(buf, meta)
            for tk, L in zip(group, got):
                want = dec.ln.split(tk.S)
                lv = tk.live
                same = np.array_equal(L[lv], want[lv]) and np.array_equal(dec.ln.join(L[lv]), tk.S[lv])
                st["checked_ticks"] += 1
                st["checked_units"] += int(lv.sum())
                st["mismatch_ticks"] += 0 if same else 1

    group, gkey = [], None
    limit = int(opts.get("limit", 10 ** 9))
    for tk in sim.ticks(z):
        if tk.gametime > limit:
            break
        if tk.block != gkey:
            flush(group)
            group, gkey = [], tk.block
        group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
    flush(group)
    t1 = time.time()
    raw = sum(len(s) for s in sections)
    res = comp_all(sections)
    out = {"tag": tag, "ruleset": list(rs), "raw": raw, "sections": len(sections),
           "encode_decode_s": round(t1 - t0, 1), **st, **res,
           "framing_deflated": frm, "columns_deflated": int(colk.sum())}
    print(json.dumps(out))
    with open("results.jsonl", "a") as f:
        f.write(json.dumps(out) + "\n")
    lanes = []
    for j in np.argsort(-colk):
        if colk[j] == 0:
            continue
        o, s = ln.table[j]
        lanes.append({"lane": f"+0x{o:03X}/{s}", "deflated_KiB": round(colk[j] / 1024, 1),
                      "air_nz": int(nzc[1, j]), "other_nz": int(nzc[0, j]),
                      "air_varint_KiB": round(vbc[1, j] / 1024, 1), "other_varint_KiB": round(vbc[0, j] / 1024, 1)})
    json.dump({"tag": tag, "framing": frm, "lanes": lanes}, open(f"{tag}.lanes.json", "w"), indent=1)


if __name__ == "__main__":
    main(sys.argv[1:])
