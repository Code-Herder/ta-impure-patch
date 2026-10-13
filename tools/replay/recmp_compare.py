#!/usr/bin/env python3
"""Compare a mode-3 playing's dumps with the world its recording implies.

    tools/replay/recmp_compare.py RECORDING DUMP...
    tools/replay/recmp_compare.py --fork N RECORDING
    tools/replay/recmp_compare.py --chat VIEW[:SINCE] RECORDING DUMP...

The expected world of a dump at GameTime T is the primary stream's iteration T -- its features,
projectiles, globals and tables, as the recorder observed them -- with every owner block's units,
the actors' player records and the units' order lists taken from the owners' contributions
applied by the recorder's capture of T (tagpu_replay.c, THE CLOCK), and each actor's bit of the
explored map from its owner's bitmap. A unit, player or list no
owner has described yet holds its level-start image. The features are the primary stream's at
every dump, corpses included (tagpu_replay.c, THE DEATHS ON A MULTIPLAYER LEVEL). The grid's unit
slots and yard bits are the engine's own on such a level, its units following their owners: they
are checked instead of compared -- every slot a dump cell names is a live unit there whose
footprint holds the cell, every cell of a live mobile unit's footprint names it (a structure's
yardmap leaves some of its cells open), and every set yard bit lies inside a live structure's
footprint. The sort buckets' order is the engine's own too (tagpu_replay.c, THE SORT BUCKETS ON A
MULTIPLAYER LEVEL): each unit's bucket +0x82 is compared, and its next +0x8E, when it is not
carried, is checked instead (tarec.sort_chains). An order whose target its owner has
already destroyed names no target: a unit's death keeps its owner's clock, so another owner's
image can still name it, and the host cannot link a unit it no longer holds (ord_target); the
owner approved that expectation on 2026-10-06. With the recorder's viewed seat
viewed in the dump too, a unit's contact bits (+0x110 bits 8-10 and 12) are the recorder's own scan,
as --fork takes them (contacts). The puppet holds of `tarec compare --puppet` apply. Exit 0 when every dump is EQUAL, 1 otherwise.

--fork N compares timeline N's first iteration, the world a takeover made, with the expected world
at its fork_at as above, less what `tarec compare-fork` takes off (decision D2, tarec.fork_view);
the grid's unit slots and yard bits and the sort buckets' chains are checked on the fork's
iteration, which the host's engine made. Exit 0 EQUAL, 1 otherwise.

--chat checks each dump's message log (main+0x12EF, a leave row the comparison does not read)
against the chat its viewer should see (tagpu_replay.c, THE CHAT OF A MULTIPLAYER RECORDING):
VIEW `full` is the full map, every message once at its author's original, else a record whose
view shows its own lines as the local echo (source row 10) and the lines its machine received.
Every text entry the recorder applied after SINCE (a seek's target; 0 for a playing from the
start) and by the dump's GameTime counts; a chat line is one in the formatter's `<name> text` form.
The log's chat lines must be the newest expected ones in order, each stamped with the GameTime of
the step that applied it (its application tick, or the one before, THE CLOCK), and every older
expected line must have left the ring, which drops only its oldest line: the step's expiry
0x463EF0 once its stamp plus (main+0x37F23 + 1) * 30 ticks is below GameTime, and the draw
(0x448CFA..0x448D2C) while more lines are queued than the panel holds, which the engine's own
notices fill too. So a missing line has expired, or is older than the oldest line still in the
ring. Exit 0 when every dump passes.
"""
import struct
import sys

import devpy
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


def explored(w, row, state):
    """An actor's bit in every explored-map entry is its owner's bitmap's (tagpu_replay.c,
    mp_world): one bit per entry in the map's order, at the record's +0x146 as the recorder stamped
    it. A player no owner has described keeps the recorder's bits."""
    if state is None:
        return
    bit, img = w.players[row][tarec.PL_MAPBIT], state["image"]
    if bit >= 16:
        raise tarec.Unreadable(f"record {row}'s explored-map bit is {bit}")
    m = w.mapped
    for i in range(len(m) // 2):
        v = struct.unpack_from("<H", m, 2 * i)[0] & ~(1 << bit)
        struct.pack_into("<H", m, 2 * i, v | (img[i >> 3] >> (i & 7) & 1) << bit)


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
        explored(w, row, applied.explored.get(origin))
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


CONTACT = 0x1700        # unit +0x110 bits 8-10 and 12, the viewed seat's scan history (tarec.puppet_view)


def recorder_scan(w):
    """The primary stream's contact bits per slot: the recorder's own scan of its viewed seat."""
    return {slot: struct.unpack_from("<I", u, tarec.U_STATE)[0] & CONTACT
            for slot, u in w.units.items()}


def contacts(a, b, scan):
    """The contact bits are the viewed seat's scan (tagpu_replay.c, R14b), and an owner's image
    carries its own machine's: the recorder's are its primary stream's. So with one player viewed
    in both worlds (puppet_view's and fork_view's rule), a composed unit the primary stream holds takes the primary's
    bits, and one it does not hold yet -- an owner-first unit, which no recorded scan has seen --
    is compared without them. The count of those."""
    if a.view[1] != b.view[1]:
        return 0
    n = 0
    for slot, img in a.units.items():
        v = struct.unpack_from("<I", img, tarec.U_STATE)[0] & ~CONTACT
        if slot in scan:
            struct.pack_into("<I", img, tarec.U_STATE, v | scan[slot])
            continue
        struct.pack_into("<I", img, tarec.U_STATE, v)
        if slot in b.units:
            u = b.units[slot]
            struct.pack_into("<I", u, tarec.U_STATE,
                             struct.unpack_from("<I", u, tarec.U_STATE)[0] & ~CONTACT)
        n += 1
    return n


def fork(path, n):
    segs, _ = tarec.file_segments(path, decode=True)
    c = tarec.chain(segs, n)[-1]
    if not c["id"]:
        raise tarec.Unreadable("timeline 0 is the original: it forks from nothing")
    T = c["fork_at"]
    h, bl, _ = tarec.load(path, timeline=c["parent"])
    hist = history(path)
    start = tarec.stream_to(h, bl, want=h["start"])
    a = tarec.stream_to(h, bl, want=T)
    scan = recorder_scan(a)
    b = tarec.stream_to(c["header"], c["blocks"], want=T)
    if a.tick != T or b.tick != T:
        print(f"{path}: the parent reads to GameTime {a.tick} and timeline {n} begins at "
              f"{b.tick}, not at its fork_at {T}")
        return 2
    compose(a, start, hist.at(T), hist.roster.actors)
    unseen = contacts(a, b, scan)
    tarec.fork_view(a, b, h, c["header"])
    gnames = tarec.globals_names(tarec.globals_list(h.get("globals"))[2])
    stale = tarec.occupancy(a, b) + tarec.sort_chains(a, b)
    lines, _ = a.differences(b, h["tables"], c["header"]["tables"], gnames=gnames,
                             limit=1 << 30)
    lines += stale
    print(f"{path}: timeline {n} at GameTime {T} against the composed parent, {len(b.units)} "
          f"live units, {len(a.units)} expected, {unseen} no recorded scan has seen: " +
          ("EQUAL" if not lines else f"{len(lines)} DIFFERENCES"))
    for line in lines[:LIMIT]:
        print("  " + line.replace("dump", f"timeline {n}"))
    return 1 if lines else 0


MSG_RING, MSG_LINES, MSG_STRIDE, MSG_HEAD = 0x12EF, 30, 0x48, 0x2A3E
TEXT_ENTRY = 136


def texts(path):
    """Every text entry the recorder applied, in ledger order: (application tick, entry)."""
    blocks, stopped = tarec.blocks(path)
    if stopped:
        raise tarec.Unreadable(stopped)
    out = []
    for kind, _, _, raw in blocks:
        if kind != tarec.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(raw):
            _, applied, phase = struct.unpack_from("<QII", raw, at)
            span, _, _, _, kind12 = struct.unpack_from("<IIIII", raw, at + 28)
            body = raw[at + 16 + 64:at + 16 + span]
            if phase != 3 and kind12 == 12:
                for i in range(struct.unpack_from("<I", body)[0]):
                    e = body[4 + i * TEXT_ENTRY:4 + (i + 1) * TEXT_ENTRY]
                    p, _, _, origin, recipient, _, display = struct.unpack_from("<BBHIIII", e)
                    text = bytes(e[69:69 + 64]).split(b"\0")[0]
                    out.append((applied, dict(phase=p, origin=origin, recipient=recipient,
                                              display=display, text=text)))
            at += 16 + span
    return out


def chat_expected(entries, rows, view, since, tick):
    """(text, source row, application tick) of the lines VIEW's message log is given by `tick`,
    oldest first."""
    mine = next((o for o, r in rows.items() if r == view), None)
    lines = []
    for applied, e in entries:
        if not since < applied <= tick or e["origin"] not in rows:
            continue
        if view == "full":
            if e["phase"] == 1:
                lines.append((e["text"], rows[e["origin"]], applied))
        elif e["phase"] == 1 and e["origin"] == mine:
            lines.append((e["text"], 10, applied))
        elif e["phase"] == 2 and e["recipient"] == mine:
            lines.append((e["text"], rows[e["origin"]], applied))
    return lines


MSG_DURATION = 0x37F23   # u32: a line shows (it + 1) * 30 ticks (0x463F15..0x463F32)


def chat_log(path):
    """(GameTime, a line's lifetime in ticks, the log's chat lines oldest first as (text, source
    row, stamp))."""
    tick, _, images = tarec.dump_images(path)
    main = images[0]
    head, tail = struct.unpack_from("<HH", main, MSG_HEAD)
    life = (struct.unpack_from("<I", main, MSG_DURATION)[0] + 1) * 30
    lines, i, oldest = [], tail, None
    while i != head and len(lines) < MSG_LINES:
        e = main[MSG_RING + i * MSG_STRIDE:MSG_RING + (i + 1) * MSG_STRIDE]
        text = bytes(e[:0x40]).split(b"\0")[0]
        stamp = struct.unpack_from("<I", e, 0x40)[0]
        oldest = stamp if oldest is None else oldest
        if text.startswith(b"<"):
            lines.append((text, e[0x46], stamp))
        i = (i + 1) % MSG_LINES
    return tick, life, oldest, lines


def chat_check(want, got, tick, life, oldest):
    """What is wrong with a dump's chat lines, or None."""
    gone = want[:len(want) - len(got)]
    kept = want[len(want) - len(got):]
    if [(t, r) for t, r, _ in kept] != [(t, r) for t, r, _ in got]:
        return "the lines are not the newest expected ones in order"
    for (text, _, applied), (_, _, stamp) in zip(kept, got):
        if stamp not in (applied - 1, applied):
            return f"{text!r} is stamped {stamp}, applied at {applied}"
    for text, _, applied in gone:
        if applied + life > tick and (oldest is None or applied - 1 > oldest):
            return f"{text!r}, applied at {applied}, is missing, has not expired and is not " \
                   "older than the ring's oldest line"
    return None


def chat(path, spec, dumps):
    view, _, since = spec.partition(":")
    view = view if view == "full" else int(view)
    hist = history(path)
    rows = {origin: row for origin, row, _, _ in hist.roster.actors}
    entries = texts(path)
    status = 0
    for d in dumps:
        tick, life, oldest, got = chat_log(d)
        want = chat_expected(entries, rows, view, int(since or 0), tick)
        why = chat_check(want, got, tick, life, oldest)
        ok = why is None
        print(f"{d}: GameTime {tick}, view {view}: {len(got)} chat lines of {len(want)} given, "
              f"a line lives {life} ticks: " + ("EQUAL" if ok else "DIFFERENT: " + why))
        if not ok:
            for label, lines in (("expected", want), ("dump", got)):
                for text, row, at in lines:
                    print(f"  {label}: row {row} at {at}: {text.decode('latin-1')}")
        status |= 0 if ok else 1
    return status


def main(argv):
    devpy.ensure()
    if len(argv) >= 4 and argv[1] == "--chat":
        return chat(argv[3], argv[2], argv[4:])
    if len(argv) == 4 and argv[1] == "--fork":
        return fork(argv[3], int(argv[2]))
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
        scan = recorder_scan(w)
        compose(w, start, hist.at(dw.tick), seats)
        # before puppet_view, which clears both worlds' views: contacts() compares them
        unseen = contacts(w, dw, scan)
        radar = dw.view[1] == w.view[1]
        tarec.mapped_hold(w)
        tarec.mapped_hold(dw)
        tarec.puppet_view(w, radar)
        tarec.puppet_view(dw, radar)
        tarec.puppet_hold(w, held)
        tarec.globals_hold(w, gslots)
        tarec.globals_hold(dw, gslots)
        stale = tarec.occupancy(w, dw) + tarec.sort_chains(w, dw)
        lines, _ = w.differences(dw, h["tables"], dh["tables"], gnames=gnames, limit=1 << 30)
        lines += stale
        n = len(lines)
        print(f"{d}: GameTime {dw.tick}, {len(dw.units)} live units, {len(w.units)} expected, "
              f"{unseen} no recorded scan has seen: " + ("EQUAL" if not n else f"{n} DIFFERENCES"))
        for line in lines[:LIMIT]:
            print("  " + line)
        status |= 1 if n else 0
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
