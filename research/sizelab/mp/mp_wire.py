#!/usr/bin/env python3
"""mp_wire.py -- what the unit codec would do to the unit contributions ON THE WIRE, where packets
travel uncompressed: each source's own sequence (mp_merge.py's owner_*.npz, one iteration per
source tick) encoded one tick per frame -- a packet decodable alone given the previous accepted
one, which the channel's gap-free sequence check guarantees -- against today's kind-8 bodies
(a u32 count, then per changed unit a 20-byte prefix and its changed dwords). Envelopes (64
bytes a packet) are not counted on either side. Every frame is decoded back and compared.

    mp_wire.py SLOTS NPZ...
"""
import json
import sys
from pathlib import Path

import numpy as np

UNITS = Path(__file__).resolve().parents[1] / "units"
sys.path.insert(0, str(UNITS))
import sim  # noqa: E402
import frame2  # noqa: E402


def run(path):
    z = sim.load(path)
    reset = set(int(t) for t in z["snap_ticks"])
    enc = frame2.Codec(lanes="field", rules=True, pred="win", reset_ticks=reset)
    dec = frame2.Codec(lanes="field", rules=True, pred="win", reset_ticks=reset)
    today = frames = ticks = sent = bad = 0
    full = 0
    for tk in sim.ticks(z):
        ticks += 1
        n = len(tk.dslots) + len(tk.created) + len(tk.gone)
        if n:
            sent += 1
            today += 4 + 20 * n + 4 * int(tk.dmask.sum()) + 4 * 82 * len(tk.created)
        tk = tk._replace(S=tk.S.copy(), live=tk.live.copy())
        s, R = enc.encode_tick(tk)
        buf = enc.block_bytes([(s, R)])
        got = dec.decode_block(buf, [(tk.gametime, tk.live, tk.created, tk.gone, tk.S)])[0]
        if not np.array_equal(dec.ln.join(got[tk.live]), tk.S[tk.live]):
            bad += 1
        if len(s) or tk.created or tk.gone:
            # a frame goes out only when something changed; created images and gones travel as
            # today's full/absent entries (20-byte prefix, plus 328 bytes for a full image)
            frames += len(buf) + 20 * (len(tk.created) + len(tk.gone)) + 4 * 82 * len(tk.created)
    return dict(npz=Path(path).name, ticks=ticks, packets_today=sent, today_body=today,
                codec_body=frames, ratio=round(today / frames, 2) if frames else None,
                mismatched_ticks=bad)


def main(argv):
    sim.SLOTS = int(argv[0])
    for p in argv[1:]:
        print(json.dumps(run(p)), flush=True)


if __name__ == "__main__":
    main(sys.argv[1:])
