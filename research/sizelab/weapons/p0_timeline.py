"""p0: the projectile pool's timeline in size-battle.imprec: count, extent, records per tick."""
import sys, struct, collections
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
path = sys.argv[1]
bl, stop = T.blocks(path)
print("stop:", stop, "blocks", len(bl))
kinds = collections.Counter()
tick = None
rows = []
cur = None
order_in_iter = collections.Counter()
seq = []
nb = 0
for kind, first, last, payload in bl:
    if kind != T.RB_STREAM:
        continue
    nb += 1
    for rk, b in T.records(payload):
        if rk == T.RK_TICK:
            if cur: rows.append(cur)
            tick = struct.unpack_from("<I", b)[0]
            cur = dict(tick=tick, head=None, whole=0, delta=0, shots=0, icpt=0, seq=[])
            continue
        if cur is None: continue
        if rk == T.RK_PROJ_HEAD:
            cur["head"] = struct.unpack_from("<II", b)
            cur["seq"].append("H")
        elif rk == T.RK_PROJ:
            cur["whole"] += 1; cur["seq"].append("W")
        elif rk == T.RK_PROJ_DELTA:
            cur["delta"] += 1; cur["seq"].append("D")
        elif rk == T.RK_EV_SHOT:
            cur["shots"] += 1; cur["seq"].append("S")
        elif rk == T.RK_EV_INTERCEPT:
            cur["icpt"] += 1
if cur: rows.append(cur)
print("iterations", len(rows), "stream blocks", nb)
# sequence pattern: do proj records come before or after shot events within an iteration?
pat = collections.Counter()
for r in rows:
    s = "".join(r["seq"])
    # compress runs
    out = []
    for ch in s:
        if not out or out[-1] != ch: out.append(ch)
    pat["".join(out)] += 1
print(pat.most_common(10))
head = None
for r in rows:
    if r["head"]: head = r["head"]
    r["count"], r["hw"] = head if head else (0, 0)
import statistics
print("total whole", sum(r["whole"] for r in rows), "delta", sum(r["delta"] for r in rows),
      "shots", sum(r["shots"] for r in rows), "intercepts", sum(r["icpt"] for r in rows))
for t in range(0, len(rows), 300):
    r = rows[t]
    print(r["tick"], "count", r["count"], "hw", r["hw"], "whole", r["whole"], "delta", r["delta"], "shots", r["shots"])
print("max count", max(r["count"] for r in rows), "max hw", max(r["hw"] for r in rows))
print("mean count", statistics.mean(r["count"] for r in rows))
