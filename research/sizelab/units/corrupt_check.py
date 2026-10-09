"""corrupt_check.py -- the round trip's check can fail: flip one residual byte of a block and
decode; the decoded images must differ (or the parse must refuse)."""
import numpy as np, frame2, sim
z = sim.load("units.npz")
reset = set(int(t) for t in z["snap_ticks"])
enc = frame2.Codec(reset_ticks=reset); dec = frame2.Codec(reset_ticks=reset)
group, gkey, done = [], None, 0
for tk in sim.ticks(z):
    if tk.block != gkey and group:
        fr = [enc.encode_tick(t) for t in group]
        buf = bytearray(enc.block_bytes(fr))
        if len(buf) > 2000 and not done:
            buf[-50] ^= 0x01                         # inside the last residual columns
            done = 1
            try:
                got = dec.decode_block(bytes(buf), [(t.gametime, t.live, t.created, t.gone, t.S) for t in group])
                bad = sum(not np.array_equal(L[t.live], dec.ln.split(t.S)[t.live]) for t, L in zip(group, got))
                print("corrupted block at GameTime", group[0].gametime, ": iterations differing", bad, "of", len(group))
            except Exception as e:
                print("corrupted block refused:", type(e).__name__, e)
            break
        dec.decode_block(bytes(buf), [(t.gametime, t.live, t.created, t.gone, t.S) for t in group])
        group = []
    gkey = tk.block
    group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
