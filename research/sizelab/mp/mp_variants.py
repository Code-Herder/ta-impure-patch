#!/usr/bin/env python3
"""mp_variants.py -- the contribution blocks under lighter alternatives to the merged world, each
deflated per block with the writer's settings:

  V0  today's records, recompressed alike;
  V1  without the recorder's own state packets (produced kinds 6 player, 7 roster, 8 units,
      2 orders, 11 explored), which duplicate its primary stream (mp_merge.py measured the units
      equal at every tick);
  V2  V1 without the seals and the proof tail (a footer instead);
  V3  V2 with the 16-byte metadata and 64-byte envelope reframed as 12 bytes (u32 application tick,
      u32 source tick, u8 origin index, u8 kind, u16 body length).

    mp_variants.py FILE...
"""
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402
from recmp_file import Contributions  # noqa: E402

LOCAL_STATE = {2, 6, 7, 8, 11}


def deflate(data):
    if not data:
        return 0
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def main(paths):
    for path in paths:
        sizes = []
        bl, _ = T.blocks(path, sizes)
        h = T.header(bl[0][3])
        actors = Contributions(h["multiplayer"]).actors
        index = {o: i for i, (o, _) in enumerate(actors)}
        tot = [0, 0, 0, 0]
        alone = {}
        real = sum(p + 32 for k, _, _, _, p in sizes if k == T.RB_CONTRIBUTIONS)
        for kind, _, _, raw in bl:
            if kind != T.RB_CONTRIBUTIONS:
                continue
            v = [bytearray() for _ in range(4)]
            kinds = {}
            at = 0
            while at < len(raw):
                ordinal, app, phase = struct.unpack_from("<QII", raw, at)
                span, origin, tick, seq, rk = struct.unpack_from("<IIIII", raw, at + 28)
                r = raw[at:at + 16 + span]
                v[0] += r
                if not (phase == 1 and rk in LOCAL_STATE):
                    v[1] += r
                    if rk != 4 and phase != 3:
                        v[2] += r
                        v[3] += struct.pack("<IIBBH", app, tick, index[origin], rk, span - 64) + r[80:]
                        kinds.setdefault(rk, bytearray()).extend(struct.pack("<IIBBH", app, tick, index[origin], rk, span - 64) + r[80:])
                at += 16 + span
            for i in range(4):
                tot[i] += deflate(bytes(v[i])) + (32 if v[i] else 0)
            for k, d in kinds.items():
                alone[k] = alone.get(k, 0) + deflate(bytes(d))
        print(f"{path}: contributions on disk {real / 1024:,.1f} KiB; V0 {tot[0] / 1024:,.1f}, "
              f"V1 {tot[1] / 1024:,.1f}, V2 {tot[2] / 1024:,.1f}, V3 {tot[3] / 1024:,.1f} KiB; "
              f"V3 kinds deflated alone {sum(alone.values()) / 1024:,.1f} KiB: " +
              ", ".join(f"{k} {v / 1024:,.1f}" for k, v in sorted(alone.items(), key=lambda kv: -kv[1])))


if __name__ == "__main__":
    main(sys.argv[1:])
