#!/usr/bin/env python3
"""run.py -- encode the battle's unit deltas with frame.py's codec and compress each block.

    run.py baseline                       today's RK_UNIT_DELTA records, per block
    run.py frame lanes=field pred=auto rules=0 masks=dict values=varint scope=block [blocks=orig|300]

Prints raw and compressed MB per compressor (each block's section compressed alone, the way
recsize.py attributes a category), and with save=1 writes the sections to out/<tag>.bin for the
decoder check.
"""
import json
import lzma
import struct
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor

import numpy as np
import zstandard

import frame
import sim

COMP = ["writer", "zlib9", "zstd3", "zstd19", "lzma"]


def csize(name, data):
    if not data:
        return 0
    if name == "writer":
        c = zlib.compressobj(6, zlib.DEFLATED, 11)
        return len(c.compress(data) + c.flush())
    if name == "zlib9":
        return len(zlib.compress(data, 9))
    if name == "zstd3":
        return len(zstandard.ZstdCompressor(level=3).compress(data))
    if name == "zstd19":
        return len(zstandard.ZstdCompressor(level=19).compress(data))
    if name == "lzma":
        return len(lzma.compress(data, preset=6))


def comp_all(sections, workers=5):
    with ProcessPoolExecutor(workers) as ex:
        futs = {c: ex.submit(_sum, c, sections) for c in COMP}
        return {c: f.result() for c, f in futs.items()}


def _sum(c, sections):
    return sum(csize(c, s) for s in sections)


def baseline(z):
    ops = z["ops"]
    dmask, dvoff, vals, dslot = z["dmask"], z["dvoff"], z["vals"], z["dslot"]
    per = {}
    for op, bi, a, b in ops[ops[:, 0] == sim.OP_DELTA]:
        n = int(dvoff[b + 1] - dvoff[b])
        body = struct.pack("<H", a) + dmask[b].tobytes() + b"\0\0\0" + vals[dvoff[b]:dvoff[b + 1]].tobytes()
        per.setdefault(int(bi), []).append(struct.pack("<HH", 2, len(body)) + body)
    return [b"".join(v) for _, v in sorted(per.items())]


def frames(z, opts):
    reset = set(int(t) for t in z["snap_ticks"])
    enc = frame.Encoder(lanes=opts.get("lanes", "field"), pred=opts.get("pred", "auto"),
                        rules=bool(int(opts.get("rules", 0))), masks=opts.get("masks", "dict"),
                        values=opts.get("values", "varint"), scope=opts.get("scope", "block"),
                        reset_ticks=reset)
    regroup = opts.get("blocks", "orig")
    sections, stats = [], {"units": 0, "residuals": 0, "lanes": enc.ln.n}
    nlin = []

    def flush(group):
        if not group:
            return
        if enc.pred == "auto":
            lin = frame.choose_lin(enc, group)
        else:
            lin = np.full(enc.ln.n, enc.pred == "lin")
        nlin.append(int(lin.sum()))
        fr = []
        for tk in group:
            s, R, L0 = enc.residuals(tk, lin)
            enc.advance(tk, L0)
            fr.append((s, R, L0[s]))
            stats["units"] += len(s)
            stats["residuals"] += int((R != 0).sum())
        if enc.scope == "block":
            sections.append(enc.block_bytes(fr, lin))
        else:
            sections.append(b"".join(enc.block_bytes([f], lin) for f in fr))

    group, gblock, gstart = [], None, None
    for tk in sim.ticks(z):
        if regroup == "orig":
            key = tk.block
        else:   # fixed-size groups, never across a snapshot
            k = int(regroup)
            last_snap = max([t for t in reset if t < tk.gametime], default=0)
            key = (last_snap, (tk.gametime - last_snap - 1) // k)
        if key != gblock:
            flush(group)
            group, gblock = [], key
        group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
    flush(group)
    stats["lin_lanes_mean"] = float(np.mean(nlin)) if nlin else 0
    return sections, stats


def main(argv):
    z = sim.load("units.npz")
    mode = argv[0]
    opts = dict(a.split("=", 1) for a in argv[1:])
    t0 = time.time()
    if mode == "baseline":
        sections, stats = baseline(z), {}
    else:
        sections, stats = frames(z, opts)
    t1 = time.time()
    raw = sum(len(s) for s in sections)
    res = comp_all(sections)
    tag = mode + ("" if mode == "baseline" else "-" + "-".join(f"{k}{v}" for k, v in sorted(opts.items()) if k != "save"))
    out = {"tag": tag, "raw": raw, "sections": len(sections), "encode_s": round(t1 - t0, 1), **stats,
           **{c: v for c, v in res.items()}}
    print(json.dumps(out))
    with open("results.jsonl", "a") as f:
        f.write(json.dumps(out) + "\n")
    if opts.get("save") == "1":
        import pickle
        with open(f"out-{tag}.pkl", "wb") as f:
            pickle.dump(sections, f)


if __name__ == "__main__":
    main(sys.argv[1:])
