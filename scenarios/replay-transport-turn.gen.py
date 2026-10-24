#!/usr/bin/env python3
"""Writes the `transport-turn` test group's scenario (tools/replay/fixtures.json),
scenarios/replay-transport-turn.json, and prints the group's steps, which fixtures.json carries.

    python3 scenarios/replay-transport-turn.gen.py

The class the group exercises: a carry link made in a unit's turn after that unit's own relink,
which the recorder keys at the next slot (tagpu_recorder.c, turn_put; tagpu_recfmt.h, THE CONTEST
ORDER) so that a playing places the carrier before it. A hover transport's TransportPickup runs
its boom's waits before each attach (BoomExtend, BoomToPad: wait-for-turn, wait-for-move); a
waiting thread is blocked on its piece's animation, which only DoScriptsNow's animation pass
0x4B1C00(dt) advances (it returns at once for dt 0), after the slice's threads have run
(0x4B0D75..0x4B0D88), so a wait that ends in the slice lets the thread run at the next pass of
that unit's threads: the next slice, or a run-now start earlier, in the same turn. The movement
pass 0x43DD20 makes one after the relink: its walk class 0x43DA70 (0x43DD5D) starts StartMoving
with run-now on a change of the walk state (DISASSEMBLED). A blocked unit makes the two meet by
construction: when its next cell is taken the relink clamps its position to its cell's edge and
sets the mover's bit 2 (0x43D912..0x43D9F7), which the walk class reads as stopped; the step the
cell frees, the relink moves it into it (0x43DA0F, 0x43DA41) and the walk class starts
StartMoving. So the attach lands after the relink whenever the car that blocks the transport
leaves in the step the boom's wait ends.

- COMBS. 40 combs in a row across Town & Country's flat band: an ARMFAV scout car, an ARMTHOVR
  hover transport behind it facing it and an ARMPW behind the transport, created cars first so
  that every car's turn comes before its transport's. The transport's load order (the scenario's)
  starts TransportPickup; its move order at GameTime 12 drives it into the car, 1 wu short of its
  next cell row, where it stays blocked from GameTime 35. Each car's move order comes at
  60 + i mod 5 and frees the transport's row in the car's own relink 7 steps later, so the
  transports unblock at steps 67 to 71, the window in which their first attach comes (MEASURED
  2026-10-24 in the recording of tools/replay/fixtures.json's run: 4, 9, 14, 6 and 7 of the 40
  attaches at steps 67 to 71, so the attach's step varies with the comb; 24 of the 40 transports
  moved into the freed row in the window, and the other 16 had stopped pushing and stayed). The
  combs whose two steps meet make the case: 6 and 4 links keyed their carrier's slot + 1 in two
  recordings.
- THE DUMPS. The group's `first` 91 and end 525 give the harness exactly 91, 121, 151 and 181,
  so the first dump comes 20 steps after the case, while the transports still stand in the cells
  they moved into.
- NO FIGHTING. Every unit has one owner and shootall is off; the AI seat owns a CORSOLAR in the
  far corner and nothing else, so it never acts and the level keeps running.

What the group cannot show: here an attach and its carrier's relink commute. The attach unlinks
the child from its bucket chain (0x47CB00 at 0x48AC5E) and pushes it onto the carrier's cargo
chain +0x8A (0x48AC70..0x48AC82), and calls no footprint writer (its calls: 0x47CB00, 0x47CB40,
0x4384A0, 0x48C9B0); the relink re-files only the carrier, and only on a change of its 128-wu
bucket (0x47CCDB). So a playing that made the attach before the relink reaches the same state:
MEASURED 2026-10-24, a build with the keying at the next slot removed passes every check of the
group. The order would show for a drop into the bucket its carrier has just entered, whose detach
pushes the child at that bucket's front (0x47CB40 at 0x48ACDC); no group makes one.
A drop comb built the same way (a transport 9 wu short of a cell row and past a 128-wu line it has
not been stamped across, its unload order standing and its car leaving in the drop's window) did
not make one: MEASURED 2026-10-24, its 22 drops all came without a relink of their carrier in
the step, since a hover transport drifts sideways by up to 14 wu while it creeps to the car and
pushes on it, so a sideways relink re-files its bucket before the row frees.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMBS = 40
X0, X_STEP = 1304, 128          # comb i's centre x: 16k + 8, on a cell centre for every footprint
Z_H = 4007                      # the transport's z: 1 wu short of its next cell row (16k + 8)
Z_B = Z_H + 48                  # the car's: its rear row the transport's next, 1 wu short of its own
Z_C = Z_H - 72                  # the cargo's, behind the transport
Z_GO_H, Z_GO_B = 4500, 4900     # where each moves
H_MOVE_AT = 12                  # the transports' move order
B_MOVE_AT, B_SPREAD = 60, 5      # comb i's car's move order: B_MOVE_AT + i mod B_SPREAD
SLOT0 = 2                       # the first unit's slot: clear_existing frees the commander's 1


def x_of(i):
    return X0 + i * X_STEP


def scenario():
    units = [{"id": f"car{i}", "type": "ARMFAV", "owner": 0, "pos": [x_of(i), Z_B], "facing": 0}
             for i in range(COMBS)]
    units += [{"id": f"bear{i}", "type": "ARMTHOVR", "owner": 0, "pos": [x_of(i), Z_H],
               "facing": 0, "orders": [{"cmd": "load", "target": f"pw{i}"}]}
              for i in range(COMBS)]
    units += [{"id": f"pw{i}", "type": "ARMPW", "owner": 0, "pos": [x_of(i), Z_C], "facing": 0}
              for i in range(COMBS)]
    units.append({"id": "keepalive", "type": "CORSOLAR", "owner": 1, "pos": [8000, 8000]})
    return {
        "format": "ta-scenario/1",
        "description": "The demo recorder's `transport-turn` test group. Written by "
                       "scenarios/replay-transport-turn.gen.py, whose header gives the design.",
        "setup": {
            "map": "Town & Country",
            "res": "1024x768",
            "clear_existing": True,
            "switches": {"shootall": False, "noshake": True},
            "players": [
                {"slot": 0, "controller": "human", "side": "arm", "color": 0},
                {"slot": 1, "controller": "ai", "side": "core", "color": 1},
            ],
        },
        "units": units,
    }


def order(at, slot, kind, x, z):
    return [at, "run", ["tools/tacli", "order", "{instance}", "move", "pos", str(x), str(z),
                        "--unit", str(slot), "--expect", kind]]


def steps():
    out = [order(H_MOVE_AT, SLOT0 + COMBS + i, "ARMTHOVR", x_of(i), Z_GO_H) for i in range(COMBS)]
    out += [order(B_MOVE_AT + i % B_SPREAD, SLOT0 + i, "ARMFAV", x_of(i), Z_GO_B) for i in range(COMBS)]
    return sorted(out, key=lambda s: s[0])


if __name__ == "__main__":
    (HERE / "replay-transport-turn.json").write_text(json.dumps(scenario(), indent=1) + "\n")
    json.dump(steps(), sys.stdout)
    print()
