#!/usr/bin/env python3
"""velocity_triples.py -- the ground velocity rule's inputs and outputs, as the engine computed them, out of a recording.

    python3 tools/tasim/velocity_triples.py RECORDING [--max N] > FILE

Prints "heading speed vx vz" lines, one per distinct (heading, speed) a moving ground unit had at the end of an
iteration, at most N (10000 by default): the unit's heading +0x66 (int16) and its mover's speed +0x20 and velocity
+0x08/+0x10 (16.16), from the recording's unit images (tools/replay/tarec.py, REC_UNIT_IMAGE: the unit record, then
its mover). tagpu/ddraw/tests/tasim_fixed_test.py --velocity FILE checks the core's rule on them (0x43CC20:
vx = -turn_x(heading, speed), vz = -turn_z(heading, speed)). These are numbers the engine computed, never a file of
the game.

A unit counts when its mover's vertical velocity +0x0C is 0 and its layer (+0x110 bits 0-1) is not 2, the air layer
[INFERRED from 0x47CC30's A/B split]: the air step 0x43D290 writes a damped velocity the ground rule does not give."""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "replay"))
import tarec  # noqa: E402

MOVER = 0x118


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("--max", type=int, default=10000)
    a = ap.parse_args(argv)
    h, bl, _ = tarec.load(a.recording)
    seen = {}

    def on_record(w, kind, body):
        if kind is not None or len(seen) >= a.max:
            return
        for img in w.units.values():
            if len(img) < MOVER + 0x24 or not any(img[MOVER:MOVER + 0x30]):
                continue
            heading = struct.unpack_from("<h", img, 0x66)[0]
            layer = struct.unpack_from("<I", img, 0x110)[0] & 3
            vx, vy, vz = struct.unpack_from("<iii", img, MOVER + 0x08)
            speed = struct.unpack_from("<i", img, MOVER + 0x20)[0]
            if speed > 0 and vy == 0 and layer != 2 and (heading, speed) not in seen:
                seen[(heading, speed)] = (vx, vz)
                if len(seen) >= a.max:
                    return

    tarec.stream_to(h, bl, on_record=on_record)
    for (heading, speed), (vx, vz) in seen.items():
        print(heading, speed, vx, vz)
    print(f"velocity_triples: {len(seen)} triples", file=sys.stderr)
    return 0 if seen else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
