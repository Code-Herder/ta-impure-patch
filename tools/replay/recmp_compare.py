#!/usr/bin/env python3
"""Compare a mode-3 playing's dumps with the world its recording implies.

    tools/replay/recmp_compare.py RECORDING DUMP...

The expected world of a dump at GameTime T is the primary stream's iteration T -- its features,
projectiles, globals and tables, as the recorder observed them -- with every owner block's units,
the actors' player records and the units' order lists taken from the owners' contributions
applied by the recorder's capture of T (tagpu_replay.c, THE CLOCK). A unit, player or list no
owner has described yet holds its level-start image. The features are the primary stream's at
every dump, corpses included (tagpu_replay.c, THE DEATHS ON A MULTIPLAYER LEVEL). The grid's unit
slots and yard bits are the engine's own on such a level, its units following their owners: they
are checked instead of compared -- every slot a dump cell names is a live unit there whose
footprint holds the cell, every cell of a live mobile unit's footprint names it (a structure's
yardmap leaves some of its cells open), and every set yard bit lies inside a live structure's
footprint. An order whose target its owner has
already destroyed names no target: a unit's death keeps its owner's clock, so another owner's
image can still name it, and the host cannot link a unit it no longer holds (ord_target); the
owner approved that expectation on 2026-10-06. The puppet holds of `tarec compare
--puppet` apply. Exit 0 when every dump is EQUAL, 1 otherwise.
"""
import struct
import sys

import tarec

LIMIT = 20
from recmp_file import History


def history(path):
    blocks, stopped = tarec.blocks(path)
    if stopped:
        raise tarec.Unreadable(stopped)
    hist = History(tarec.segments(blocks)[0]["header"])
    for kind, _, _, raw in blocks:
        if kind != tarec.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(raw):
            span = struct.unpack_from("<I", raw, at + 28)[0]
            hist.block(bytes(raw[at:at + 16 + span]))
            at += 16 + span
    return hist


ORDER_TARGET = 0x16      # an order node's target, its unit's slot + 1 (tagpu_replay.c, ORD_TARGET)


def compose(w, start, applied, seats):
    """The owner-authority world over primary world `w`: its units, players and lists replaced."""
    units, lists = {}, {}
    for actor in seats:
        origin, row, first, last = actor
        for slot in range(first, last + 1):
            u = applied.units.get(slot)
            if u is None:
                if slot in start.units:
                    units[slot] = bytearray(start.units[slot])
                    for which in (0, 1):
                        if (slot, which) in start.lists:
                            lists[slot, which] = bytearray(start.lists[slot, which])
                continue
            if not u["live"]:
                continue
            units[slot] = bytearray(u["image"])
            for which in (0, 1):
                o = applied.orders.get((slot, which))
                if o is not None and o["birth"] == u["birth"]:
                    if o["count"]:
                        lists[slot, which] = bytearray(o["image"])
                elif (slot, which) in start.lists and slot in start.units:
                    lists[slot, which] = bytearray(start.lists[slot, which])
        # +0xF0, the players phase's 30-tick deadline, is the host's own on its own clock
        # (players_put leaves it): the recorder's, whatever the owner's clock read
        deadline = bytes(w.players[row][0xF0:0xF4])
        p = applied.players.get(origin)
        w.players[row] = bytearray(p["image"] if p is not None else start.players[row])
        w.players[row][0xF0:0xF4] = deadline
    for img in lists.values():
        for at in range(0, len(img), tarec.REC_ORDER_IMAGE):
            target = struct.unpack_from("<I", img, at + ORDER_TARGET)[0]
            if target and target - 1 not in units:
                struct.pack_into("<I", img, at + ORDER_TARGET, 0)
    w.units, w.lists = units, lists


STRUCTURE = 1 << 29     # unit +0x110: a structure, whose yard bits a footprint writer sets


def rect(u):
    """The cells a unit's footprint covers: +0x76/+0x78 by +0x7E/+0x80, i16 each."""
    x, z = struct.unpack_from("<hh", u, 0x76)
    w, h = struct.unpack_from("<hh", u, 0x7E)
    return {(cx, cz) for cx in range(x, x + w) for cz in range(z, z + h)}


def structure(u):
    return struct.unpack_from("<I", u, 0x110)[0] & STRUCTURE


def footprints(dw):
    """Every cell a live structure of the dump covers."""
    return set().union(*(rect(u) for u in dw.units.values() if structure(u)))


def occupancy(w, dw):
    """The grid's unit slots and yard bit are the engine's on a mode-3 level (tagpu_puppet_begin,
    engine_cells): the expected cells take the dump's, and the module's checks stand in for the
    comparison. The differences those checks find."""
    out = []
    cb = tarec.REC_CELL_BYTES
    yards = footprints(dw)
    rects = {slot: rect(u) for slot, u in dw.units.items()}
    for slot, u in sorted(dw.units.items()):
        if structure(u):
            continue
        for cx, cz in sorted(rects[slot]):
            i = (cz * w.gw + cx) * cb
            if not (0 <= cx < w.gw and 0 <= i < len(dw.grid)) or \
                    slot not in struct.unpack_from("<HH", dw.grid, i):
                out.append(f"occupancy: cell {(cx, cz)} of mobile unit {slot}'s footprint "
                           "does not name it")
    for i in range(0, len(dw.grid), cb):
        c = i // cb
        xz = (c % w.gw, c // w.gw)
        w.grid[i:i + 4] = dw.grid[i:i + 4]
        w.grid[i + 0x0C] = (w.grid[i + 0x0C] & ~2) | (dw.grid[i + 0x0C] & 2)
        for slot in struct.unpack_from("<HH", dw.grid, i):
            if slot and slot not in dw.units:
                out.append(f"occupancy: cell {xz} names slot {slot}, not live")
            elif slot and xz not in rects[slot]:
                out.append(f"occupancy: cell {xz} names slot {slot}, outside its footprint")
        if dw.grid[i + 0x0C] & 2 and xz not in yards:
            out.append(f"occupancy: cell {xz} holds a yard bit outside every live structure")
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path, dumps = argv[1], argv[2:]
    h, bl, _ = tarec.load(path)
    hist = history(path)
    seats = hist.roster.actors       # (origin, row, first slot, last slot) of every actor
    start = tarec.stream_to(h, bl, want=h["start"])
    held = tarec.puppet_rows()
    gslots = tarec.globals_list(h.get("globals"))[2]
    gnames = tarec.globals_names(gslots)
    status = 0
    for d in dumps:
        dh, dw = tarec.dump_world(d, h)
        w = tarec.stream_to(h, bl, want=dw.tick)
        compose(w, start, hist.at(dw.tick), seats)
        radar = dw.view[1] == w.view[1]
        bit = dw.players[tarec.viewer_of(h)][tarec.PL_MAPBIT]
        tarec.mapped_hold(w, bit)
        tarec.mapped_hold(dw, bit)
        tarec.puppet_view(w, tarec.viewer_of(h), radar)
        tarec.puppet_view(dw, tarec.viewer_of(h), radar)
        tarec.puppet_hold(w, held)
        tarec.globals_hold(w, gslots)
        tarec.globals_hold(dw, gslots)
        stale = occupancy(w, dw)
        lines, _ = w.differences(dw, h["tables"], dh["tables"], gnames=gnames, limit=1 << 30)
        lines += stale
        n = len(lines)
        print(f"{d}: GameTime {dw.tick}, {len(dw.units)} live units, {len(w.units)} expected: "
              + ("EQUAL" if not n else f"{n} DIFFERENCES"))
        for line in lines[:LIMIT]:
            print("  " + line)
        status |= 1 if n else 0
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
