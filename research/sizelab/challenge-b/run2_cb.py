#!/usr/bin/env python3
"""run2.py -- frame2's codec on the battle: encode per block, decode the bytes back and compare
every live unit's image at every iteration, then compress each block's section alone.

    run2.py lanes=field rules=1 pred=win [blocks=orig|300] [verify=1]
"""
import json
import sys
import time

import numpy as np
import os
U = os.path.abspath('../units')
sys.path.insert(0, U)

import frame2
import sim
from run import comp_all


def main(argv):
    opts = dict(a.split("=", 1) for a in argv)
    z = sim.load(os.path.join(U, "units.npz"))
    reset = set(int(t) for t in z["snap_ticks"])
    kw = dict(lanes=opts.get("lanes", "field"), rules=bool(int(opts.get("rules", 1))),
              pred=opts.get("pred", "win"), reset_ticks=reset,
              ruleset=tuple(opts["ruleset"].split(",")) if "ruleset" in opts else None)
    enc = frame2.Codec(**kw)
    dec = frame2.Codec(**kw) if opts.get("verify", "1") == "1" else None
    regroup = opts.get("blocks", "orig")
    sections, stats = [], {"units": 0, "residuals": 0, "lanes": enc.ln.n, "checked_ticks": 0,
                           "checked_units": 0, "mismatch_ticks": 0}
    t0 = time.time()

    def flush(group):
        if not group:
            return
        fr = []
        for tk in group:
            s, R = enc.encode_tick(tk)
            fr.append((s, R))
            stats["units"] += len(s)
            stats["residuals"] += int((R != 0).sum())
        buf = enc.block_bytes(fr)
        sections.append(buf)
        if dec is not None:
            meta = [(tk.gametime, tk.live, tk.created, tk.gone, tk.S) for tk in group]
            got = dec.decode_block(buf, meta)
            for tk, L in zip(group, got):
                want = dec.ln.split(tk.S)
                lv = tk.live
                same = np.array_equal(L[lv], want[lv]) and np.array_equal(dec.ln.join(L[lv]), tk.S[lv])
                stats["checked_ticks"] += 1
                stats["checked_units"] += int(lv.sum())
                stats["mismatch_ticks"] += 0 if same else 1

    group, gkey = [], None
    snaps = sorted(reset)
    limit = int(opts.get("limit", 10 ** 9))
    for tk in sim.ticks(z):
        if tk.gametime > limit:
            break
        if regroup == "orig":
            key = tk.block
        else:
            k = int(regroup)
            last = max([t for t in snaps if t < tk.gametime], default=0)
            key = (last, (tk.gametime - last - 1) // k)
        if key != gkey:
            flush(group)
            group, gkey = [], key
        group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
    flush(group)
    t1 = time.time()
    raw = sum(len(s) for s in sections)
    res = comp_all(sections)
    tag = "cb-frame2-" + "-".join(f"{k}{v}" for k, v in sorted(opts.items()))
    out = {"tag": tag, "raw": raw, "sections": len(sections), "encode_decode_s": round(t1 - t0, 1),
           **stats, **res}
    print(json.dumps(out))
    with open("units_rules.jsonl", "a") as f:
        f.write(json.dumps(out) + "\n")
    if opts.get("save") == "1":
        import pickle
        with open(f"out-{tag}.pkl", "wb") as f:
            pickle.dump(sections, f)


if __name__ == "__main__":
    main(sys.argv[1:])
