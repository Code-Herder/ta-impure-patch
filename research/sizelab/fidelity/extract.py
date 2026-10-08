#!/usr/bin/env python3
"""extract.py -- every unit image of every iteration of a recording, as one dense array.

    extract.py <file.imprec> <outdir>

Walks the stream blocks with tarec.py's reader (the stream alone builds the world from its
first iteration), keeps each slot's 82-dword image (RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE) and,
at each iteration's end (the next RK_TICK), copies the images into
  outdir/units.u32   uint32 [T, S, 82]  (memmap; zero where the slot is dead)
  outdir/live.u8     uint8  [T, S]
  outdir/ticks.npy   the GameTime of each row
  outdir/meta.json   T, S
"""
import json
import os
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402


def main():
    path, out = sys.argv[1], Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    bl, stop = T.blocks(path)
    if stop:
        print("note:", stop)
    h = T.header(bl[0][3])
    S = h["slots"]
    # count the iterations first
    ticks = []
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                ticks.append(struct.unpack_from("<I", b)[0])
    NT = len(ticks)
    D = T.REC_UNIT_DWORDS
    units = np.lib.format.open_memmap(out / "units.npy", mode="w+", dtype=np.uint32,
                                      shape=(NT, S, D))
    live = np.zeros((NT, S), dtype=np.uint8)
    img = np.zeros((S, D), dtype=np.uint32)
    alive = np.zeros(S, dtype=np.uint8)
    row = -1
    bitw = np.arange(D)
    nd = 0
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                if row >= 0:
                    units[row] = img
                    live[row] = alive
                row += 1
            elif rk == T.RK_UNIT:
                slot = struct.unpack_from("<H", b)[0]
                img[slot] = np.frombuffer(b, dtype="<u4", count=D, offset=4)
                alive[slot] = 1
            elif rk == T.RK_UNIT_DELTA:
                slot = struct.unpack_from("<H", b)[0]
                bits = np.unpackbits(np.frombuffer(b, dtype=np.uint8, count=T.REC_UNIT_BITS,
                                                   offset=2), bitorder="little")[:D].astype(bool)
                vals = np.frombuffer(b, dtype="<u4", offset=T.REC_UNIT_DELTA_HEAD)
                img[slot, bitw[bits]] = vals
                nd += 1
            elif rk == T.RK_UNIT_GONE:
                slot = struct.unpack_from("<H", b)[0]
                img[slot] = 0
                alive[slot] = 0
    if row >= 0:
        units[row] = img
        live[row] = alive
    units.flush()
    np.save(out / "live.npy", live)
    np.save(out / "ticks.npy", np.array(ticks, dtype=np.int64))
    (out / "meta.json").write_text(json.dumps({"T": NT, "S": S, "deltas": nd,
                                               "first": ticks[0], "last": ticks[-1]}))
    print(f"{NT} iterations, {S} slots, {nd} deltas; GameTime {ticks[0]}..{ticks[-1]}")


if __name__ == "__main__":
    main()
