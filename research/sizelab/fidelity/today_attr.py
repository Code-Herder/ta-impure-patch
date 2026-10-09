#!/usr/bin/env python3
"""today_attr.py -- what the motion dwords cost in today's format: each stream block's unit
deltas deflated as recsize.py deflates a category (the writer's settings), then again with
groups of dwords removed from every delta (their bits cleared, their values dropped, a delta
left empty dropped whole).

    today_attr.py <file.imprec>
"""
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402

D = T.REC_UNIT_DWORDS
POSHD = [0x64 // 4, 0x68 // 4, 0x6C // 4, 0x70 // 4, 0x74 // 4]       # turn, x, alt, z, (fx)
MOVER = [0x120 // 4, 0x124 // 4, 0x128 // 4, 0x138 // 4, 0x13C // 4, 0x140 // 4, 0x144 // 4]
DERIV = [0x78 // 4, 0x7C // 4, 0xF8 // 4]                               # fz/sx, sz/fw, sight h
SETS = {"none": [], "position+turn": POSHD, "+mover": POSHD + MOVER,
        "+derived": POSHD + MOVER + DERIV, "mover alone": MOVER}


def deflate(b):
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(b) + c.flush())


def main():
    bl, _ = T.blocks(sys.argv[1])
    tot = {k: [0, 0] for k in SETS}
    drop = {k: np.zeros(D, bool) for k in SETS}
    for k, v in SETS.items():
        drop[k][v] = True
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        parts = {k: bytearray() for k in SETS}
        for rk, b in T.records(payload):
            if rk != T.RK_UNIT_DELTA:
                continue
            bits = np.unpackbits(np.frombuffer(b, np.uint8, T.REC_UNIT_BITS, 2),
                                 bitorder="little")[:D].astype(bool)
            vals = np.frombuffer(b, "<u4", offset=T.REC_UNIT_DELTA_HEAD)
            idx = np.nonzero(bits)[0]
            for k in SETS:
                keep = ~drop[k][idx]
                if not keep.any():
                    continue
                nb = bits.copy()
                nb[idx[~keep]] = False
                bm = np.packbits(np.concatenate((nb, np.zeros(T.REC_UNIT_BITS * 8 - D, bool))),
                                 bitorder="little").tobytes()
                body = b[:2] + bm + bytes(T.REC_UNIT_DELTA_HEAD - 2 - T.REC_UNIT_BITS) + \
                    vals[keep].astype("<u4").tobytes()
                parts[k] += struct.pack("<HH", rk, len(body)) + body
        for k in SETS:
            tot[k][0] += len(parts[k])
            tot[k][1] += deflate(bytes(parts[k]))
    base = tot["none"][1]
    for k, (raw, pk) in tot.items():
        print(f"{k:<16} raw {raw / 1e6:8.1f} MB  deflated {pk / 1e6:7.2f} MB  "
              f"removed {(base - pk) / 1e6:6.2f} MB")


if __name__ == "__main__":
    main()
