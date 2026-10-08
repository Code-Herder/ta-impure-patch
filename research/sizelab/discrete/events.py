"""The events of the stream, kind by kind: what their fields hold and how much of each repeats --
the same unit's same event in the step before, the same name, the same arguments."""
import collections
import pickle
import struct

from common import T, OUT


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    c = collections.defaultdict(collections.Counter)
    last = {}                         # (kind, unit) -> (tick, body)
    per_tick_unit = collections.Counter()
    tick = None
    in_tick_root = False
    for bi, tk, rk, b in keep:
        if rk == T.RK_TICK:
            tick = tk
            continue
        if rk == T.RK_EV_START_NAMED:
            uid, argc, now = struct.unpack_from("<IBB", b)
            args = struct.unpack_from("<4i", b, 8)
            name = b[24:].decode("latin-1")
            c["named: name"][name] += 1
            c["named: argc,now"][(argc, now)] += 1
            c["named: args used"][tuple(args[:argc])[:1] if argc else ()] += 0
            c["named: args beyond argc nonzero"][any(args[argc:])] += 1
            key = (rk, uid, name)
            p = last.get(key)
            if p:
                c["named: gap to the same unit+name"][min(tick - p[0], 31)] += 1
                c["named: same body as last"][p[1] == b] += 1
            last[key] = (tick, b)
            per_tick_unit[(tick, uid, name)] += 1
        elif rk == T.RK_EV_START:
            uid, idx, argc = struct.unpack_from("<Iii", b)
            args = struct.unpack_from("<4i", b, 12)
            c["start: index"][idx] += 1
            c["start: argc"][argc] += 1
            c["start: args beyond argc nonzero"][any(args[argc:])] += 1
            key = (rk, uid, idx)
            p = last.get(key)
            if p:
                c["start: gap to the same unit+index"][min(tick - p[0], 31)] += 1
                c["start: same body as last"][p[1] == b] += 1
            last[key] = (tick, b)
            c["start: (index, args)"][(idx, args[:argc])] += 1
        elif rk == T.RK_EV_REPLY:
            uid, kind, root, given = struct.unpack_from("<IIBB", b)
            c["reply: kind,root,given"][(kind, root, given)] += 1
            if given:
                c["reply: text"][b[12:].decode("latin-1")] += 1
            key = (rk, uid, kind)
            p = last.get(key)
            if p:
                c["reply: gap to the same unit+kind"][min(tick - p[0], 31)] += 1
            last[key] = (tick, b)
        elif rk == T.RK_EV_SPRAY:
            uid, rev = struct.unpack_from("<II", b)
            point = struct.unpack_from("<3i", b, 8)
            box = struct.unpack_from("<6i", b, 20)
            layer = struct.unpack_from("<I", b, 44)[0]
            c["spray: reverse,layer"][(rev, layer)] += 1
            c["spray: credited"][uid != 0] += 1
            key = (rk, uid)
            p = last.get(key)
            if p:
                pb = p[1]
                c["spray: gap to the same unit"][min(tick - p[0], 31)] += 1
                c["spray: same box as last"][pb[20:44] == b[20:44]] += 1
                c["spray: same point as last"][pb[8:20] == b[8:20]] += 1
            last[key] = (tick, b)
        elif rk == T.RK_EV_SHOT:
            m = b
            shooter = struct.unpack_from("<H", m, 0x21)[0]
            target = struct.unpack_from("<H", m, 0x1F)[0]
            wid = m[0x19] | (m[0x23] >> 4) << 8
            c["shot: has target unit"][target != 0] += 1
            key = (rk, shooter, m[0x23] & 15)
            p = last.get(key)
            if p:
                pb = p[1]
                c["shot: gap to the same shooter+slot"][min(tick - p[0], 63)] += 1
                c["shot: same start"][pb[1:13] == m[1:13]] += 1
                c["shot: same target point"][pb[13:25] == m[13:25]] += 1
                c["shot: same target unit"][pb[0x1F:0x21] == m[0x1F:0x21]] += 1
                c["shot: same angles"][pb[0x1B:0x1F] == m[0x1B:0x1F]] += 1
            last[key] = (tick, b)
            c["shot: weapon"][wid] += 1
        elif rk == T.RK_EV_FEATURE:
            c["feature: root"][b[0]] += 1
            c["feature: root,phase"][(b[0], b[1])] += 1
        elif rk == T.RK_EV_FEATURE_END:
            c["feature end: root,result"][(b[0], struct.unpack_from("<I", b, 4)[0] != 0)] += 1
        elif rk == T.RK_EV_CREATE:
            c["create"]["n"] += 1
        elif rk == T.RK_EV_DEATH:
            c["death: severity,+0xA,carried"][(struct.unpack_from("<b", b, 9)[0], b[10], b[11])] += 1
    for (t, u, n), k in per_tick_unit.items():
        c["named: per step per unit+name"][min(k, 9)] += 1
    for name, ctr in c.items():
        tot = sum(ctr.values())
        print(f"{name} (total {tot:,}):")
        for k, v in ctr.most_common(14):
            print(f"    {k!r}: {v:,}  {100 * v / tot:.1f}%" if tot else f"    {k!r}: {v}")


if __name__ == "__main__":
    main()
