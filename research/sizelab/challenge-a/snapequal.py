"""Does each snapshot's unit set and images equal the stream's state at the snapshot tick (what a
seeking reader starts from, vs. what the units decoder carried across its reset)?"""
import sys, numpy as np
z = np.load(sys.argv[1])
ops, dslot, dmask, dvoff, vals, images = (z[k] for k in ("ops", "dslot", "dmask", "dvoff", "vals", "images"))
snaps = [int(t) for t in z["snap_ticks"]]
S = np.zeros((2501, 82), np.uint32); live = np.zeros(2501, bool)
rows = np.flatnonzero(ops[:, 0] == 0); ends = list(rows[1:]) + [len(ops)]
for r0, r1 in zip(rows, ends):
    gt = int(ops[r0, 2])
    for op, bi, a, b in ops[r0 + 1:r1]:
        if op == 1: S[a] = images[b]; live[a] = True
        elif op == 3: live[a] = False; S[a] = 0
        elif op == 2:
            m = np.unpackbits(dmask[b], bitorder="little")[:82].astype(bool)
            row = S[a].copy(); row[m] = vals[dvoff[b]:dvoff[b + 1]]; S[a] = row
    if gt in snaps:
        sl = z[f"snap_{gt}_slots"]; im = z[f"snap_{gt}_imgs"]
        same_set = np.array_equal(np.flatnonzero(live), np.sort(sl))
        eq = np.array_equal(S[sl], im)
        print(gt, "live set equal:", same_set, "images equal:", eq, "units", len(sl))
