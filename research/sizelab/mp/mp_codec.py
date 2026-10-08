#!/usr/bin/env python3
"""mp_codec.py -- the units research's lossless frame (frame2.py: field lanes, per-unit predictor
switching, the engine integration rules) on the unit sequences mp_merge.py wrote, against today's
records, both deflated per block with the writer's settings. Every frame is decoded back from its
bytes and compared with the input at every iteration.

    mp_codec.py SLOTS NPZ...

A frame section carries the continuing units; the whole images of created units and the gone
records stay records (as frame2's run2.py has it), deflated per block beside the section.
"""
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

UNITS = Path(__file__).resolve().parents[1] / "units"
sys.path.insert(0, str(UNITS))
import sim  # noqa: E402
import frame2  # noqa: E402


def deflate(data):
    if not data:
        return 0
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def rec(kind, body):
    return struct.pack("<HH", kind, len(body)) + body + bytes(-len(body) % 4)


def run(path):
    z = sim.load(path)
    reset = set(int(t) for t in z["snap_ticks"])
    ops, images = z["ops"], z["images"]
    # today's records, per block
    base, side = {}, {}
    dmask, dvoff, vals, dslot = z["dmask"], z["dvoff"], z["vals"], z["dslot"]
    for op, bi, a, b in ops:
        if op == sim.OP_DELTA:
            body = struct.pack("<H", a) + dmask[b].tobytes() + b"\0\0\0" + vals[dvoff[b]:dvoff[b + 1]].tobytes()
            base.setdefault(int(bi), bytearray()).extend(rec(3, body))
        elif op == sim.OP_UNIT:
            r = rec(2, struct.pack("<HH", a, 0) + images[b].tobytes())
            base.setdefault(int(bi), bytearray()).extend(r)
            side.setdefault(int(bi), bytearray()).extend(r)
        elif op == sim.OP_GONE:
            r = rec(4, struct.pack("<HH", a, 0))
            base.setdefault(int(bi), bytearray()).extend(r)
            side.setdefault(int(bi), bytearray()).extend(r)
    baseline = sum(deflate(bytes(v)) for v in base.values())
    creates = sum(deflate(bytes(v)) for v in side.values())
    enc = frame2.Codec(lanes="field", rules=True, pred="win", reset_ticks=reset)
    dec = frame2.Codec(lanes="field", rules=True, pred="win", reset_ticks=reset)
    sections, checked, bad = [], 0, 0
    group, gkey = [], None

    def flush(group):
        nonlocal checked, bad
        if not group:
            return
        fr = [enc.encode_tick(tk) for tk in group]
        buf = enc.block_bytes(fr)
        sections.append(buf)
        got = dec.decode_block(buf, [(tk.gametime, tk.live, tk.created, tk.gone, tk.S) for tk in group])
        for tk, L in zip(group, got):
            lv = tk.live
            checked += 1
            if not np.array_equal(dec.ln.join(L[lv]), tk.S[lv]):
                bad += 1
    for tk in sim.ticks(z):
        if tk.block != gkey:
            flush(group)
            group, gkey = [], tk.block
        group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
    flush(group)
    frame = sum(deflate(s) for s in sections)
    return dict(npz=Path(path).name, records_writer=baseline, frame_writer=frame,
                creates_and_gone_writer=creates, frame_total=frame + creates,
                iterations=checked, mismatched_iterations=bad)


def main(argv):
    sim.SLOTS = int(argv[0])
    out = [run(p) for p in argv[1:]]
    for r in out:
        print(json.dumps(r))


if __name__ == "__main__":
    main(sys.argv[1:])
