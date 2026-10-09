"""Per 900-tick window: each category's packed bytes (writer deflate, each category alone per
stream block, recsize's attribution), and each snapshot's on-disk size with its tick."""
import sys, struct, collections, json
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
import recsize as R
path = sys.argv[1]
sizes = []
bl, stop = T.blocks(path, sizes)
win = collections.defaultdict(collections.Counter)
snaps = []
last_tick = 0
for (kind, first, last, payload), (_, _, _, raw, pk) in zip(bl, sizes):
    if kind == T.RB_SNAPSHOT:
        snaps.append((last_tick, raw, pk + 32))
        continue
    if kind != T.RB_STREAM:
        continue
    parts = collections.defaultdict(bytearray)
    t0 = None
    for rk, body in T.records(payload):
        if rk == T.RK_TICK:
            t = struct.unpack_from("<I", body)[0]
            if t0 is None: t0 = t
            last_tick = t
        g = R.GROUP_OF.get(rk, "other")
        parts[g] += struct.pack("<HH", rk, len(body)) + body + bytes(-len(body) % 4)
    w = (t0 or last_tick) // 900
    for g, d in parts.items():
        win[w][g] += R.deflate_like_writer(bytes(d))
    win[w]["_real"] += pk + 32
out = {"windows": {str(k): dict(v) for k, v in sorted(win.items())}, "snapshots": snaps}
json.dump(out, open(sys.argv[2], "w"), indent=1)
M = 2**20
for k, v in sorted(win.items()):
    print(k*900, " ".join(f"{g[:10]}={b/M:.3f}" for g, b in sorted(v.items(), key=lambda x: -x[1])[:8]))
for s in snaps: print("snapshot after tick", s[0], "raw", s[1], "disk", s[2])
