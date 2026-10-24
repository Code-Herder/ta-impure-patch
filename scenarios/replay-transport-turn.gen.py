#!/usr/bin/env python3
"""Writes the `transport-turn` test group's scenario (tools/replay/fixtures.json),
scenarios/replay-transport-turn.json, and prints the group's steps for fixtures.json.

    python3 scenarios/replay-transport-turn.gen.py

PROBE: a comb of hover transports, each blocked by a scout car that leaves at its own tick.
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMBS = 32
X0, X_STEP = 1304, 128          # comb i's centre x: 16k + 8, on a cell centre for every footprint
Z_H = 4007                      # the transport's z: 1 wu short of its next cell row (16k + 8)
Z_B = Z_H + 48                  # the car's: its rear row the transport's next, 1 wu short of its own
Z_C = Z_H - 72                  # the cargo's, behind the transport
Z_GO_H, Z_GO_B = 4500, 4900     # where each moves
H_MOVE_AT = 12                  # the transports' move order
B_MOVE_AT = 34                  # comb 0's car's; comb i's at B_MOVE_AT + i


def x_of(i):
    return X0 + i * X_STEP


def scenario():
    units = [{"id": f"car{i}", "type": "ARMFAV", "owner": 0, "pos": [x_of(i), Z_B], "facing": 180}
             for i in range(COMBS)]
    units += [{"id": f"bear{i}", "type": "ARMTHOVR", "owner": 0, "pos": [x_of(i), Z_H],
               "facing": 180, "orders": [{"cmd": "load", "target": f"pw{i}"}]}
              for i in range(COMBS)]
    units += [{"id": f"pw{i}", "type": "ARMPW", "owner": 0, "pos": [x_of(i), Z_C], "facing": 180}
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


def steps():
    out = [[H_MOVE_AT, "order", {"unit": ["ARMTHOVR", i],
                                 "args": ["move", "pos", str(x_of(i)), str(Z_GO_H)]}]
           for i in range(COMBS)]
    out += [[B_MOVE_AT + i, "order", {"unit": ["ARMFAV", i],
                                      "args": ["move", "pos", str(x_of(i)), str(Z_GO_B)]}]
            for i in range(COMBS)]
    return out


if __name__ == "__main__":
    (HERE / "replay-transport-turn.json").write_text(json.dumps(scenario(), indent=1) + "\n")
    json.dump(steps(), sys.stdout)
    print()
