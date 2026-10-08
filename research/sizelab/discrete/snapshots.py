"""The snapshots: each category's size per snapshot (raw, the writer's deflate, zstd-19), and three
re-encodings, decoded back where they change bytes:
  wrecks: only the records that differ from the recording's first iteration (the base the cells
          already use), instead of every record that is not all zero;
  projectiles: the records below hw that are not dead, by the pool's own test (weapon NULL);
  whole snapshot under zstd-19 and as a delta from the previous snapshot (priced only).
Projectile record: the weapon pointer imaged as its ID at +0 [INFERRED from proj_extent, which
reads the weapon at the record's start: tagpu_recorder.c 0x3450]."""
import collections
import struct

from common import T, load, mb, kb, rec_bytes
from codec import packed

GROUP = {T.RK_WRECK: "wrecks", T.RK_PROJ: "projectiles", T.RK_PROJ_HEAD: "projectiles",
         T.RK_UNIT: "units", T.RK_ORDERS: "orders", T.RK_CELLS: "cells",
         T.RK_TABLE_SPAN: "tables", T.RK_TABLE_SIZE: "tables", T.RK_PLAYER: "players",
         T.RK_MAPPED: "explored map", T.RK_GLOBALS: "globals"}


def main():
    bl = load()
    base_pool = {}
    first_tick = None
    tick = None
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                tick = struct.unpack_from("<I", b)[0]
                if first_tick is None:
                    first_tick = tick
                elif tick != first_tick:
                    break
            elif rk == T.RK_WRECK and tick == first_tick:
                base_pool[struct.unpack_from("<H", b)[0]] = bytes(b[4:])
        break
    print(f"the first iteration's wreck records: {len(base_pool)}")
    tot = collections.Counter()
    prev_payload = None
    for kind, first, last, payload in bl:
        if kind != T.RB_SNAPSHOT:
            continue
        parts = collections.defaultdict(bytearray)
        wreck_new = bytearray()
        proj_live = bytearray()
        nproj = nlive = nwreck = ndiff = 0
        for rk, b in T.records(payload):
            g = GROUP.get(rk, "other")
            rb = rec_bytes(rk, b)
            parts[g] += rb
            if rk == T.RK_WRECK:
                nwreck += 1
                i = struct.unpack_from("<H", b)[0]
                if base_pool.get(i) != bytes(b[4:]):
                    ndiff += 1
                    wreck_new += rb
            elif rk == T.RK_PROJ:
                nproj += 1
                if struct.unpack_from("<I", b, 4)[0]:
                    nlive += 1
                    proj_live += rb
            elif rk == T.RK_PROJ_HEAD:
                proj_live += rb
        row = {g: (len(p), packed(bytes(p), "deflate"), packed(bytes(p), "zstd19"))
               for g, p in parts.items()}
        whole = (len(payload), packed(payload, "deflate"), packed(payload, "zstd19"))
        alt_w = packed(bytes(wreck_new), "deflate")
        alt_p = packed(bytes(proj_live), "deflate")
        delta = None
        if prev_payload is not None and __import__("common").zstandard:
            zs = __import__("common").zstandard
            d = zs.ZstdCompressionDict(prev_payload)
            delta = len(zs.ZstdCompressor(level=19, dict_data=d).compress(payload))
        prev_payload = payload
        print(f"\nsnapshot at {last}: whole raw {kb(whole[0])}, deflate {kb(whole[1])}, zstd19 "
              f"{kb(whole[2])}" + (f", zstd19 with the previous snapshot as its dictionary "
                                   f"{kb(delta)}" if delta else ""))
        for g, (r, dfl, z) in sorted(row.items(), key=lambda kv: -kv[1][1]):
            print(f"   {g:<14} raw {kb(r):>9} deflate {kb(dfl):>7} zstd19 {kb(z):>7}")
        print(f"   wrecks: {nwreck} records, {ndiff} differ from the first iteration's: deflate "
              f"{kb(alt_w)} instead of {kb(row['wrecks'][1])}")
        print(f"   projectiles: {nproj} records below hw, {nlive} with a weapon: deflate "
              f"{kb(alt_p)} instead of {kb(row['projectiles'][1])}")
        tot["deflate"] += whole[1]
        tot["zstd19"] += whole[2]
        tot["delta"] += delta or whole[2]
        tot["wreck saving"] += row["wrecks"][1] - alt_w
        tot["proj saving"] += row["projectiles"][1] - alt_p
    print("\ntotals:", {k: kb(v) for k, v in tot.items()})


if __name__ == "__main__":
    main()
