"""windows.py -- the unit category's writer-deflated bytes per 900-tick window of GameTime, for
today's records and the re-encodings saved by run2.py / lossy.py; and the tail rate per hour."""
import pickle, zlib, numpy as np, sim
from run import baseline
z = sim.load("units.npz")
blocks = z["blocks"]
stream = [(int(b[1]), int(b[2])) for b in blocks if b[0] == 2]
def defl(b):
    if not b: return 0
    c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
base = baseline(z)
# baseline() returns one section per block that HAS unit deltas; map by block index
ops = z["ops"]; dl = ops[ops[:, 0] == sim.OP_DELTA]
have = sorted(set(int(b) for b in dl[:, 1]))
blk_first = {i: int(b[1]) for i, b in enumerate(blocks)}
series = {"today": {blk_first[i]: defl(s) for i, s in zip(have, base)}}
stream_blocks = [i for i, b in enumerate(blocks) if b[0] == 2]
for name, f in (("lossless", "out-frame2-lanesfield-predwin-rules1-save1.pkl"),
                ("lossy hz10", "out-lossy-hz10-q1.pkl"), ("lossy eps1/2", "out-lossy-eps-pos1.0-deg2.0-q1.pkl")):
    secs = pickle.load(open(f, "rb"))
    assert len(secs) == len(stream_blocks), (len(secs), len(stream_blocks))
    series[name] = {blk_first[i]: defl(s) for i, s in zip(stream_blocks, secs)}
W = 900
print(f"{'GameTime':<12}" + "".join(f"{k:>16}" for k in series))
tot = {k: 0 for k in series}
for w in range(0, 7830, W):
    row = []
    for k, d in series.items():
        v = sum(b for t, b in d.items() if w <= t < w + W)
        tot[k] += v
        row.append(v)
    print(f"{w}-{w + W - 1:<7}" + "".join(f"{v / 1048576:>13.2f} MB" for v in row))
print(f"{'total':<12}" + "".join(f"{v / 1048576:>13.2f} MB" for v in tot.values()))
# the tail: GameTime 4500..7199 (about 350-430 units), per hour of game time
for k, d in series.items():
    v = sum(b for t, b in d.items() if 4500 <= t < 7200)
    print(f"tail rate {k:>14}: {v / 1048576 / (2700 / 30) * 3600:8.1f} MB/hour")
