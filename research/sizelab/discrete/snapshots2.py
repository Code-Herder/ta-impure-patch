"""The snapshots rebuilt three ways and compressed whole (deflate as the writer, zstd-19):
  base:   wreck records only where they differ from the recording's first iteration (lossless:
          the reader takes the first iteration as the cells' base already);
  base+P: base, and the projectile records [count, hw) left out -- the records the compacting
          pool (0x49AEE0) left behind its live count. NOT lossless: a loosening, priced only;
  gop:    base, each snapshot compressed with the previous snapshot's payload as a zstd
          dictionary (a chain: priced only).
The wreck variant is decoded back: base records + the snapshot's records = today's pool image."""
import collections
import struct

from common import T, load, kb, rec_bytes, zstandard
from codec import packed


def main():
    bl = load()
    base_pool = {}
    tick0 = None
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                t = struct.unpack_from("<I", b)[0]
                if tick0 is None:
                    tick0 = t
                elif t != tick0:
                    break
            elif rk == T.RK_WRECK:
                base_pool[struct.unpack_from("<H", b)[0]] = bytes(b[4:])
        break
    tot = collections.Counter()
    prev = None
    for kind, first, last, payload in bl:
        if kind != T.RB_SNAPSHOT:
            continue
        out_base = bytearray()
        out_p = bytearray()
        pool_today = {}
        pool_new = dict(base_pool)
        count = hw = None
        for rk, b in T.records(payload):
            rb = rec_bytes(rk, b)
            if rk == T.RK_WRECK:
                i = struct.unpack_from("<H", b)[0]
                pool_today[i] = bytes(b[4:])
                if base_pool.get(i) != bytes(b[4:]):
                    out_base += rb
                    out_p += rb
                    pool_new[i] = bytes(b[4:])
                continue
            if rk == T.RK_PROJ_HEAD:
                count, hw = struct.unpack("<II", b)
            if rk == T.RK_PROJ and struct.unpack_from("<H", b)[0] >= count:
                out_base += rb
                continue
            out_base += rb
            out_p += rb
        # today's pool image is every non-zero record; the base form gives the same image
        full_today = {i: v for i, v in pool_today.items() if any(v)}
        full_new = {i: v for i, v in pool_new.items() if any(v)}
        assert full_today == full_new, "wreck base form not lossless"
        dfl = packed(payload, "deflate")
        z = packed(payload, "zstd19")
        bd = packed(bytes(out_base), "deflate")
        bz = packed(bytes(out_base), "zstd19")
        pz = packed(bytes(out_p), "zstd19")
        gz = None
        if prev is not None:
            dct = zstandard.ZstdCompressionDict(prev)
            gz = len(zstandard.ZstdCompressor(level=19, dict_data=dct).compress(bytes(out_base)))
        prev = bytes(out_base)
        print(f"{last}: today deflate {kb(dfl)} zstd19 {kb(z)} | base deflate {kb(bd)} zstd19 "
              f"{kb(bz)} | base+P zstd19 {kb(pz)} | gop {kb(gz) if gz else '-'}  [wrecks equal]")
        for k, v in (("today deflate", dfl), ("today zstd19", z), ("base deflate", bd),
                     ("base zstd19", bz), ("base+P zstd19", pz), ("gop", gz or bz)):
            tot[k] += v
    print("totals:", {k: kb(v) for k, v in tot.items()})


if __name__ == "__main__":
    main()
