#!/usr/bin/env python3
"""extract.py -- pull the unit records out of a recording's stream into flat arrays.

    extract.py <file.imprec> <out.npz>

Per stream block, in order: every RK_TICK, RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE and RK_CORRECT,
as an op list (kind, tick index, slot, payload index). Delta values are kept flat. Also the
snapshots' unit images (for a seek-start check) and the block table.
"""
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402

OP_TICK, OP_UNIT, OP_DELTA, OP_GONE, OP_CORRECT = 0, 1, 2, 3, 4


def main(path, out):
    sizes = []
    bl, stop = T.blocks(path, sizes)
    assert stop is None, stop
    ops = []                # (op, block, a, b): tick: (gametime, 0); unit: (slot, img idx); ...
    images = []             # RK_UNIT images (82 u32)
    dslot, dmask, dvoff = [], [], []
    vals = []
    nvals = 0
    blocks = []             # (kind, first, last, raw, packed)
    snap_units = {}         # snapshot tick -> {slot: image}
    for bi, ((kind, first, last, payload), sz) in enumerate(zip(bl, sizes)):
        blocks.append(sz)
        if kind == T.RB_SNAPSHOT:
            d = {}
            for rk, b in T.records(payload):
                if rk == T.RK_UNIT:
                    d[struct.unpack_from("<H", b)[0]] = np.frombuffer(b[4:4 + T.REC_UNIT_IMAGE], "<u4").copy()
            snap_units[first] = d
            continue
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                ops.append((OP_TICK, bi, struct.unpack_from("<I", b)[0], 0))
            elif rk == T.RK_UNIT:
                ops.append((OP_UNIT, bi, struct.unpack_from("<H", b)[0], len(images)))
                images.append(np.frombuffer(b[4:4 + T.REC_UNIT_IMAGE], "<u4"))
            elif rk == T.RK_UNIT_DELTA:
                slot = struct.unpack_from("<H", b)[0]
                m = b[2:2 + T.REC_UNIT_BITS]
                w = b[T.REC_UNIT_DELTA_HEAD:]
                ops.append((OP_DELTA, bi, slot, len(dslot)))
                dslot.append(slot)
                dmask.append(m)
                dvoff.append(nvals)
                vals.append(w)
                nvals += len(w) // 4
            elif rk == T.RK_UNIT_GONE:
                ops.append((OP_GONE, bi, struct.unpack_from("<H", b)[0], 0))
            elif rk == T.RK_CORRECT:
                ops.append((OP_CORRECT, bi, 0, 0))
    ops = np.array(ops, dtype=np.int64)
    dmask = np.frombuffer(b"".join(dmask), np.uint8).reshape(-1, T.REC_UNIT_BITS)
    vals = np.frombuffer(b"".join(vals), "<u4")
    dvoff.append(nvals)
    images = np.array(images, dtype=np.uint32).reshape(-1, T.REC_UNIT_DWORDS)
    st = sorted(snap_units)
    np.savez(out, ops=ops, dslot=np.array(dslot, np.int32), dmask=dmask,
             dvoff=np.array(dvoff, np.int64), vals=vals, images=images,
             blocks=np.array(blocks, np.int64), snap_ticks=np.array(st, np.int64),
             **{f"snap_{t}_slots": np.array(sorted(snap_units[t]), np.int32) for t in st},
             **{f"snap_{t}_imgs": np.array([snap_units[t][s] for s in sorted(snap_units[t])],
                                           np.uint32).reshape(-1, T.REC_UNIT_DWORDS) for t in st})
    print(f"ops {len(ops)}, deltas {len(dslot)}, values {nvals}, images {len(images)}, "
          f"snapshots {st}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
