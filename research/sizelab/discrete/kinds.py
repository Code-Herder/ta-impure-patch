"""Per record kind of the stream: records, raw bytes, and each kind deflated alone per block with
the writer's settings and with zstd-19 (the attribution recsize.py uses, one level finer).
Also pickles every record outside the unit/projectile kinds, with its block and tick, for the
other experiments."""
import collections
import pickle
import struct

from common import T, load, deflate_writer, zstd19, rec_bytes, OUT, mb, kb

SKIP = {T.RK_UNIT_DELTA, T.RK_PROJ_DELTA}       # the other researchers' areas, not kept


def main():
    bl = load()
    raw = collections.Counter()
    n = collections.Counter()
    alone = collections.Counter()
    alonez = collections.Counter()
    keep = []                       # (block, tick, kind, body)
    tick = None
    nb = 0
    for bi, (kind, first, last, payload) in enumerate(bl):
        if kind != T.RB_STREAM:
            continue
        nb += 1
        parts = collections.defaultdict(bytearray)
        for rk, body in T.records(payload):
            rb = rec_bytes(rk, body)
            raw[rk] += len(rb)
            n[rk] += 1
            parts[rk] += rb
            if rk == T.RK_TICK:
                tick = struct.unpack_from("<I", body)[0]
            if rk not in SKIP:
                keep.append((bi, tick, rk, bytes(body)))
        for rk, data in parts.items():
            alone[rk] += deflate_writer(bytes(data))
            alonez[rk] += zstd19(bytes(data))
    names = {v: k for k, v in vars(T).items() if k.startswith("RK_") and isinstance(v, int)}
    print(f"{nb} stream blocks")
    print(f"{'kind':<22}{'records':>10}{'raw':>12}{'deflate alone':>16}{'zstd19 alone':>14}")
    rows = []
    for rk in sorted(raw, key=lambda k: -alone[k]):
        print(f"{names[rk]:<22}{n[rk]:>10}{mb(raw[rk]):>12}{kb(alone[rk]):>16}{kb(alonez[rk]):>14}")
        rows.append((names[rk], n[rk], raw[rk], alone[rk], alonez[rk]))
    with open(OUT / "kinds.pkl", "wb") as f:
        pickle.dump({"rows": rows, "keep": keep}, f, protocol=4)


if __name__ == "__main__":
    main()
