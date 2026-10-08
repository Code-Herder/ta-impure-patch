"""airseries.py -- one pass over units.npz: every live unit-iteration of the air-war's aircraft
types as (tick index, gametime, slot, created, 82 dwords).  Writes air.npz.

    airseries.py <units.npz> <out.npz>
"""
import sys
import numpy as np
import sim

AIR_TYPES = {26: "ARMBRAWL", 51: "ARMFIG", 123: "ARMTHUND", 183: "CORFINK", 241: "CORSHAD", 271: "CORVAMP"}


def main(src, out):
    z = sim.load(src)
    ti, gts, slots, cre, rows = [], [], [], [], []
    gtl = []
    air = np.array(sorted(AIR_TYPES))
    for k, tk in enumerate(sim.ticks(z)):
        gtl.append(tk.gametime)
        lv = np.flatnonzero(tk.live)
        if not len(lv):
            continue
        typ = sim.field(tk.S[lv], 0xA6, 2, False)
        sel = lv[np.isin(typ, air)]
        if not len(sel):
            continue
        ti.append(np.full(len(sel), k, np.int32))
        slots.append(sel.astype(np.int32))
        c = np.zeros(len(sel), bool)
        if tk.created:
            c = np.isin(sel, tk.created)
        cre.append(c)
        rows.append(tk.S[sel].copy())
    np.savez(out, tick=np.concatenate(ti), slot=np.concatenate(slots), created=np.concatenate(cre),
             S=np.concatenate(rows), gametime=np.array(gtl, np.int64),
             snap_ticks=z["snap_ticks"])
    print(len(np.concatenate(ti)), "aircraft unit-iterations over", len(gtl), "ticks")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
