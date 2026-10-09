#!/usr/bin/env python3
"""local_equal.py -- does the recorder's OWN contribution state equal its primary stream at EVERY
iteration, for every local slot (not only where a packet changed it), and for order lists, player
records and the explored map too?

For each iteration T of the primary stream (world = image T, before T's events change nothing in
the reader's unit/list/player/map images), the local-origin records of kinds 2/6/7/8/11 with
application tick <= T are fed to a recmp_file.History (none of these kinds is a root, so the
post-capture split cannot move them), and compared:

  units   every slot of a local actor's block: primary live <-> history live, images equal
  orders  every (slot, list) of a local block: primary list bytes <-> history list (birth ignored)
  players every local seat: primary record <-> history mapped image (+0xF0 included)
  map     every local seat: the primary map's bit PL_MAPBIT of that record <-> history bitmap

    local_equal.py FILE
"""
import collections
import struct
import sys
from pathlib import Path

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402
from recmp_file import History  # noqa: E402


def main(path):
    bl, stop = T.blocks(path)
    assert stop is None, stop
    h = T.segments(bl)[0]["header"]
    hist = History(h)
    local = {o for o, l in hist.ledger.actors if l}
    seats = [a for a in hist.roster.actors if a[0] in local]
    recs = []
    for kind, _, _, raw in bl:
        if kind != T.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(raw):
            ordinal, app, phase = struct.unpack_from("<QII", raw, at)
            span, origin, tick, seq, rk = struct.unpack_from("<IIIII", raw, at + 28)
            if phase == 1 and origin in local and rk in (2, 6, 7, 8, 11):
                recs.append((app, (origin, tick, seq, rk, bytes(raw[at + 64:at + 80]), bytes(raw[at + 80:at + 16 + span]), app)))
            at += 16 + span
    hl, bl2, _ = T.load(path)
    c = collections.Counter()
    first = {}
    state = dict(i=0)

    def compare(w, t):
        while state["i"] < len(recs) and recs[state["i"]][0] <= t:
            o, tk, sq, k, st, body, ap = recs[state["i"]][1]
            hist._semantic(o, tk, sq, k, st, body, ap, False)
            state["i"] += 1
        for origin, row, lo, hi in seats:
            for s in range(lo, hi + 1):
                u = hist.units.get(s)
                hl_ = u is not None and u["live"]
                pl = s in w.units
                if not hl_ and not pl:
                    continue
                if u is None:
                    c["unit: no owner image yet"] += 1
                    continue
                if hl_ != pl:
                    c["unit: LIVE DIFFERS"] += 1
                    first.setdefault("unit live", (t, s, hl_, pl))
                elif bytes(w.units[s]) != bytes(u["image"]):
                    c["unit: IMAGE DIFFERS"] += 1
                    first.setdefault("unit image", (t, s))
                else:
                    c["unit: equal"] += 1
                for which in (0, 1):
                    o = hist.orders.get((s, which))
                    pw = bytes(w.lists.get((s, which), b""))
                    hw = o["image"] if o is not None else None
                    if hw is None:
                        c["list: no owner list yet" + (" (primary empty)" if not pw else " (PRIMARY HAS ONE)")] += 1
                    elif pw == hw:
                        c["list: equal" + (" (empty)" if not pw else "")] += 1
                    else:
                        c["list: DIFFERS"] += 1
                        first.setdefault("list", (t, s, which, len(pw) // 88, len(hw) // 88))
            p = hist.players.get(origin)
            if p is None:
                c["player: none yet"] += 1
            elif bytes(w.players[row]) == bytes(p["image"]):
                c["player: equal"] += 1
            else:
                c["player: DIFFERS"] += 1
                d = [4 * k for k in range(130) if w.players[row][4 * k:4 * k + 4] != p["image"][4 * k:4 * k + 4]]
                first.setdefault("player", (t, row, [hex(x) for x in d[:8]]))
            e = hist.explored.get(origin)
            if e is None:
                c["map: none yet"] += 1
            else:
                bit = w.players[row][T.PL_MAPBIT]
                m = w.mapped
                n = len(m) // 2
                bad = sum(1 for i in range(n)
                          if (struct.unpack_from("<H", m, 2 * i)[0] >> bit & 1) != (e["image"][i >> 3] >> (i & 7) & 1))
                if bad:
                    c["map: DIFFERS"] += 1
                    first.setdefault("map", (t, row, bad))
                else:
                    c["map: equal"] += 1

    last = dict(t=None)

    def on_record(w, rk, b):
        if rk is None and w.tick is not None:
            compare(w, w.tick)
            c["iterations"] += 1
    w = T.stream_to(hl, bl2, on_record=on_record)
    w.whole(True)
    compare(w, w.tick)
    c["iterations"] += 1
    print(path)
    for k, v in sorted(c.items()):
        print(f"  {k:<45}{v:>10}")
    for k, v in first.items():
        print("  first", k, v)


if __name__ == "__main__":
    main(sys.argv[1])
