"""Everything in this area together, per block: today's records of these kinds in stream order
compressed as one piece, against the re-encodings' columns (orders_codec columns+values,
cells_codec B, events_codec, small_codec's wrecks/players/globals/feature heads) concatenated
per block and compressed as one piece. Each re-encoding was decoded back by its own script.
The first grid is left out of both sides (priced in cells_codec.txt)."""
import collections
import pickle
import struct

from common import T, OUT, mb, kb, rec_bytes, load
from codec import packed, HOWS
import orders_codec
import cells_codec
import events_codec
import small_codec

MINE = {T.RK_ORDERS, T.RK_CELLS, T.RK_WRECK, T.RK_FEATURES, T.RK_GLOBALS, T.RK_GLOBALS_DELTA,
        T.RK_PLAYER, T.RK_PLAYER_DELTA, T.RK_TABLE_SIZE, T.RK_TABLE_SPAN, T.RK_TICK,
        T.RK_UNIT_GONE, T.RK_CONTROLLER, T.RK_VIEWER} | set(T.EVENTS)


def snapshot_resets():
    """the stream blocks a seek may start reading at: the first, and each one a snapshot follows"""
    resets, prev, first = set(), None, None
    for i, (kind, a, b, p) in enumerate(load()):
        if kind == T.RB_STREAM:
            first = i if first is None else first
            prev = i
        elif kind == T.RB_SNAPSHOT:
            resets.add(prev)
    return resets | {first}


def main():
    events_codec.RESETS = snapshot_resets()
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    up = pickle.load(open(OUT / "units_pass.pkl", "rb"))
    first_tick = next(t for _, t, k, _ in keep if k == T.RK_TICK)
    today = collections.defaultdict(bytearray)
    for bi, t, k, b in keep:
        if k in MINE and not (k == T.RK_CELLS and t == first_tick):
            today[bi] += rec_bytes(k, b)
    base = {h: sum(packed(bytes(p), h) for p in today.values()) for h in HOWS}
    print(f"today, these kinds together per block (first grid apart): raw "
          f"{mb(sum(len(p) for p in today.values()))}, "
          + ", ".join(f"{h} {kb(v)}" for h, v in base.items()))
    per = collections.defaultdict(bytearray)
    ob = orders_codec.encode(orders_codec.lists_of(keep), True, False)
    cb = cells_codec.encode(cells_codec.gather(keep), True)
    evs = [(bi, t, k, b) for bi, t, k, b in keep if k in T.EVENTS]
    dq, sq = iter(up["deaths"]), iter(up["shots"])
    side = []
    for bi, t, k, b in evs:
        side.append(next(dq)[3] if k == T.RK_EV_DEATH else
                    ((lambda s: (s[2], s[3]))(next(sq)) if k == T.RK_EV_SHOT else None))
    eb = events_codec.encode(evs, side)
    wb, _ = small_codec.generic(keep, {T.RK_WRECK}, lambda k, b: struct.unpack_from("<H", b)[0],
                                lambda k, b: bytes(b[4:]), True)
    gb, _ = small_codec.globals_predicted(keep)
    fb, _ = small_codec.features_predicted(keep)
    for fam in (ob, cb, eb, wb, gb, fb):
        for bi, c in fam.items():
            c.c.pop("first", None)
            per[bi] += c.blob()
    # what the re-encodings above leave as written: players, tables, ticks, gone, controller
    for bi, t, k, b in keep:
        if k in (T.RK_PLAYER, T.RK_PLAYER_DELTA, T.RK_TABLE_SIZE, T.RK_TABLE_SPAN, T.RK_TICK,
                 T.RK_UNIT_GONE, T.RK_CONTROLLER, T.RK_VIEWER):
            per[bi] += rec_bytes(k, b)
    new = {h: sum(packed(bytes(p), h) for p in per.values()) for h in HOWS}
    print(f"re-encoded together per block: raw {mb(sum(len(p) for p in per.values()))}, "
          + ", ".join(f"{h} {kb(v)} ({v / base[h]:.2f}x)" for h, v in new.items()))


if __name__ == "__main__":
    main()
