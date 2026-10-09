#!/usr/bin/env python3
"""mp_account.py -- where a mode-3 recording's bytes go: the contributions by kind and phase, and
the primary stream's unit/player/order records split by the slot's owner (local actor, remote
actor, no actor), each category deflated alone per block with the writer's settings (recsize's
attribution method: zlib level 6, 2 KB window).

    mp_account.py FILE [--json OUT]
"""
import collections
import json
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402
from recmp_file import Contributions, Seats  # noqa: E402

KINDS = {1: "legacy resources", 2: "orders", 4: "seal", 6: "player", 7: "roster", 8: "units",
         9: "named (legacy)", 10: "roots", 11: "explored", 12: "text", 13: "pov", 14: "click",
         15: "select"}
PHASES = {1: "produced (local)", 2: "consumed (remote)", 3: "proof tail"}
ROOTS = {T.RK_EV_CREATE: "create end", 0x100: "create begin", T.RK_EV_DEATH: "death",
         T.RK_EV_DEATH_END: "death end", T.RK_EV_START_NAMED: "named start",
         T.RK_EV_REPLY: "reply", T.RK_EV_SPRAY: "spray"}


def deflate(data):
    if not data:
        return 0
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def contribution_records(raw):
    """(meta, envelope, body) of every record of an RB_CONTRIBUTIONS payload."""
    at = 0
    while at < len(raw):
        ordinal, app, phase = struct.unpack_from("<QII", raw, at)
        (magic, version, head, span, origin, tick, seq, kind) = struct.unpack_from("<8sHHIIIII", raw, at + 16)
        yield dict(ordinal=ordinal, app=app, phase=phase, origin=origin, tick=tick, seq=seq,
                   kind=kind, span=span, rec=raw[at:at + 16 + span], body=raw[at + 80:at + 16 + span])
        at += 16 + span


def main(path, out=None):
    sizes = []
    bl, stop = T.blocks(path, sizes)
    assert stop is None, stop
    h = T.header(bl[0][3])
    ledger = Contributions(h["multiplayer"])
    seats = Seats(h["mp_seats"], ledger.actors, h["slots"], h["units_each"], h["players"])
    local_origins = {o for o, l in ledger.actors if l}
    owner = {}                      # slot -> 'local' / 'remote'
    seat_of = {}
    for origin, seat, first, last in seats.actors:
        for s in range(first, last + 1):
            owner[s] = "local" if origin in local_origins else "remote"
        seat_of[seat] = "local" if origin in local_origins else "remote"
    res = {"file": str(path), "bytes": Path(path).stat().st_size,
           "actors": [(o, l) for o, l in ledger.actors], "seats": seats.actors}

    # blocks
    byb = collections.defaultdict(lambda: [0, 0, 0])
    for kind, first, last, raw, packed in sizes:
        b = byb[T.RB_NAMES.get(kind, kind)]
        b[0] += 1; b[1] += raw; b[2] += packed + 32
    res["blocks"] = {k: dict(n=v[0], raw=v[1], disk=v[2]) for k, v in byb.items()}

    # contributions
    cnt = collections.Counter(); raw_b = collections.Counter(); body_b = collections.Counter()
    alone = collections.Counter(); entries = collections.Counter(); roots = collections.Counter()
    rootb = collections.Counter(); units = collections.Counter()
    real = sum(p + 32 for k, _, _, _, p in sizes if k == T.RB_CONTRIBUTIONS)
    for (kind, first, last, payload), (_, _, _, r, pk) in zip(bl, sizes):
        if kind != T.RB_CONTRIBUTIONS:
            continue
        parts = collections.defaultdict(bytearray)
        for c in contribution_records(payload):
            key = (KINDS.get(c["kind"], str(c["kind"])), PHASES[c["phase"]])
            cnt[key] += 1
            raw_b[key] += len(c["rec"])
            body_b[key] += len(c["body"])
            parts[key] += c["rec"]
            b = c["body"]
            if c["kind"] == 8:
                n = struct.unpack_from("<I", b)[0]
                entries[key] += n
                at = 4
                for _ in range(n):
                    slot, live, full, birth, mask, _r = struct.unpack_from("<HBBI11sB", b, at)
                    w = bin(int.from_bytes(mask, "little")).count("1")
                    at += 20 + 4 * w
                    units[(key[1], "full" if full and live else "absent" if not live else "delta")] += 1
                    units[(key[1], "words")] += w
            elif c["kind"] == 10:
                n = struct.unpack_from("<I", b)[0]
                entries[key] += n
                at = 4
                for _ in range(n):
                    rk, span = struct.unpack_from("<HH", b, at)
                    roots[(key[1], ROOTS.get(rk, rk))] += 1
                    rootb[(key[1], ROOTS.get(rk, rk))] += 12 + span
                    at += 12 + span
            elif c["kind"] == 12:
                entries[key] += struct.unpack_from("<I", b)[0]
        for key, data in parts.items():
            alone[key] += deflate(bytes(data))
    res["contributions"] = {"disk": real, "sum_alone": sum(alone.values()),
                            "by": [dict(kind=k[0], phase=k[1], records=cnt[k], raw=raw_b[k],
                                        body=body_b[k], envelopes=cnt[k] * 80, entries=entries[k],
                                        packed_alone=alone[k])
                                   for k in sorted(cnt, key=lambda k: -alone[k])],
                            "roots": [dict(phase=k[0], root=str(k[1]), n=roots[k], bytes=rootb[k])
                                      for k in sorted(roots)],
                            "units": {f"{a}/{b}": v for (a, b), v in units.items()}}

    # primary stream, unit/player/order records by owner
    def cat(rk, b):
        if rk in (T.RK_UNIT, T.RK_UNIT_DELTA, T.RK_UNIT_GONE):
            return "units " + owner.get(struct.unpack_from("<H", b)[0], "no actor")
        if rk in (T.RK_PLAYER, T.RK_PLAYER_DELTA):
            return "players " + seat_of.get(struct.unpack_from("<H", b)[0], "no actor")
        if rk == T.RK_ORDERS:
            return "orders " + owner.get(struct.unpack_from("<H", b)[0], "no actor")
        if rk in (T.RK_EV_STATE, T.RK_EV_BUILT, T.RK_EV_CREATE, T.RK_EV_NET_CREATE):
            return "events playback ignores (state, built, create, net create)"
        if rk == T.RK_EV_DEATH:
            return "death events (their 340 bytes)"
        if rk in T.EVENTS:
            return "other events"
        if rk in (T.RK_POV, T.RK_CLICK, T.RK_SELECT, T.RK_POV_AT):
            return "perspective"
        return {T.RK_CELLS: "cells", T.RK_MAPPED: "explored map", T.RK_MAPPED_RUNS: "explored map"}.get(
            rk, "projectiles" if rk in (T.RK_PROJ_HEAD, T.RK_PROJ, T.RK_PROJ_DELTA) else "other shared")
    sraw = collections.Counter(); salone = collections.Counter(); snum = collections.Counter()
    sreal = 0
    for (kind, first, last, payload), (_, _, _, r, pk) in zip(bl, sizes):
        if kind != T.RB_STREAM:
            continue
        sreal += pk + 32
        parts = collections.defaultdict(bytearray)
        for rk, b in T.records(payload):
            g = cat(rk, b)
            rec = struct.pack("<HH", rk, len(b)) + b + bytes(-len(b) % 4)
            parts[g] += rec
            sraw[g] += len(rec); snum[g] += 1
        for g, data in parts.items():
            salone[g] += deflate(bytes(data))
    res["stream"] = {"disk": sreal, "sum_alone": sum(salone.values()),
                     "by": [dict(cat=g, records=snum[g], raw=sraw[g], packed_alone=salone[g])
                            for g in sorted(sraw, key=lambda g: -salone[g])]}
    if out:
        Path(out).write_text(json.dumps(res, indent=1))
    show(res)


def kb(n):
    return f"{n / 1024:,.1f}"


def show(r):
    print(f"{r['file']}: {kb(r['bytes'])} KiB; actors {r['actors']}; seats {r['seats']}")
    print("blocks (KiB on disk incl. 32-byte headers):")
    for k, v in sorted(r["blocks"].items(), key=lambda kv: -kv[1]["disk"]):
        print(f"  {k:<14}{v['n']:>4}  raw {kb(v['raw']):>9}  disk {kb(v['disk']):>9}  "
              f"{100 * v['disk'] / r['bytes']:5.1f} %")
    c = r["contributions"]
    print(f"contributions: disk {kb(c['disk'])}, kinds deflated alone sum {kb(c['sum_alone'])} KiB")
    print(f"  {'kind':<16}{'phase':<20}{'records':>8}{'entries':>9}{'raw':>10}{'body':>10}"
          f"{'env+meta':>10}{'alone':>9}{'share':>7}")
    for x in c["by"]:
        print(f"  {x['kind']:<16}{x['phase']:<20}{x['records']:>8}{x['entries']:>9}{kb(x['raw']):>10}"
              f"{kb(x['body']):>10}{kb(x['envelopes']):>10}{kb(x['packed_alone']):>9}"
              f"{100 * x['packed_alone'] / c['sum_alone']:6.1f}%")
    print("  roots:", ", ".join(f"{x['phase'][:8]} {x['root']} {x['n']} ({kb(x['bytes'])} KiB)" for x in c["roots"]))
    print("  unit entries:", c["units"])
    s = r["stream"]
    print(f"stream: disk {kb(s['disk'])}, categories deflated alone sum {kb(s['sum_alone'])} KiB")
    for x in s["by"]:
        print(f"  {x['cat']:<62}{x['records']:>8}{kb(x['raw']):>10}{kb(x['packed_alone']):>9}"
              f"{100 * x['packed_alone'] / s['sum_alone']:6.1f}%")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--json" else None)
