#!/usr/bin/env python3
"""Writes the `contest-waves` test group's scenarios (tools/replay/fixtures.json):
scenarios/replay-contest-walkers.json and replay-contest-wave-{a,b,c}.json.

    python3 scenarios/replay-contest.gen.py

The class the group exercises: on the recording machine a unit relinks in its own turn of the
unit tick, and the scenario applier creates at the frame tail after the step (0x4969D2), so a
create that lands on the cells a walker has just moved into finds them held and loses the
contest; a playing that makes the step's creates before its relinks gives the same cells the
other way round (tagpu_puppet.c, THE CONTEST'S RULE). A contest needs a walker that changed cell
in the very step of the create and whose footprint, one cell wider, meets the create's, so the
group is built to make that happen at every wave rather than now and then:

- LANES. 160 ARMFAV, a fast 2x2 scout car, of the human seat in 16 lanes 48 wu apart (three
  cells: a footprint and one free cell), ten to a lane, each patrolling its own lane north to the
  band's far end and back. A lane is a column, not a crowd around one point: 72 ARMPW patrolling
  to one point jammed there, 1 to 10 of them relinking in a wave's step, and gave a candidate at
  5 of 38 waves; the lanes give 7 to 31 relinks and a candidate at 24 of 38 (MEASURED 2026-10-23
  on main 361ed8f1, the images of one recording each).
- WAVES. Each wave creates one ARMPW on every lane, in a row across the band, so every lane's
  traffic meets a create; the three waves put the row at three depths of the band. Each wave's
  units are sent west to a parking point off the band, so the band stays as it was for the next,
  and the fixture's unit limit (1000) holds the 160 walkers and every wave's 16.
- NO FIGHTING. Every unit has one owner and shootall is off; the AI seat owns a CORSOLAR in the far
  corner and nothing else, so it never acts and the level keeps running.

A cell, a mark or a bucket chain a playing gets wrong stays wrong until the units it names move
on, so the waves start early and run to the last GameTime the harness can draw (T = 1 mod 30,
tareplay.py gametimes), each two ticks before one: on main, with waves from 299, a playing of one
recording differed at 34 of the 38 GameTimes the harness can draw, in +0x110 bits 26/27, cell
holders and the bucket chain +0x8E (MEASURED 2026-10-23, dumps armed at every one). Two ticks,
not one: with each wave one tick before T, the playing differed at T in +0x110 bit 8 on every
unit of that wave and on no other unit (P and R at 301 and 721, MEASURED 2026-10-23), a
difference of the contact bits the radar pass writes, which is not the class this group is for.
"""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
X0, LANES, LANE_STEP = 4320, 16, 48             # the lanes' centre, count and spacing (wu)
Y_NEAR, Y_FAR, PER_LANE = 3300, 4300, 10        # the band's ends and each lane's walkers
WAVES = {"a": (3550, 3500), "b": (3800, 3800), "c": (4050, 4100)}  # row depth, parking point y
PARK_X = 3200


def lane_x(i):
    return X0 + round((i - (LANES - 1) / 2) * LANE_STEP)


def walkers():
    spacing = (Y_FAR - Y_NEAR) // PER_LANE
    groups = [{
        "id": f"lane{i}",
        "owner": 0,
        "at": [lane_x(i), Y_NEAR + (PER_LANE - 1) * spacing // 2],
        "pattern": {"kind": "line", "spacing": spacing, "angle": 90},
        "composition": [{"type": "ARMFAV", "count": PER_LANE}],
        "facing": 0,
        "orders": [{"cmd": "patrol", "to": [lane_x(i), Y_FAR]}],
    } for i in range(LANES)]
    return {
        "format": "ta-scenario/1",
        "description": "The demo recorder's `contest-waves` test group: 160 ARMFAV of the human "
                       "seat patrolling 16 lanes across a band of Town & Country, and an AI seat "
                       "that owns one CORSOLAR and nothing else. Written by "
                       "scenarios/replay-contest.gen.py, whose header gives the design.",
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
        "groups": groups,
        "units": [{"id": "keepalive", "type": "CORSOLAR", "owner": 1, "pos": [8000, 8000]}],
    }


def wave(name, y, park_y):
    return {
        "format": "ta-scenario/1",
        "description": f"A wave of the demo recorder's `contest-waves` test group: one ARMPW on "
                       f"each of the 16 lanes at y {y}, sent west to ({PARK_X}, {park_y}). "
                       f"Written by scenarios/replay-contest.gen.py, whose header gives the "
                       f"design.",
        "setup": {"clear_existing": False},
        "units": [{"id": f"w{name}{i}", "type": "ARMPW", "owner": 0, "pos": [lane_x(i), y],
                   "facing": 0, "orders": [{"cmd": "move", "to": [PARK_X, park_y]}]}
                  for i in range(LANES)],
    }


def write(name, doc):
    (HERE / name).write_text(json.dumps(doc, indent=1) + "\n")


if __name__ == "__main__":
    write("replay-contest-walkers.json", walkers())
    for name, (y, park_y) in WAVES.items():
        write(f"replay-contest-wave-{name}.json", wave(name, y, park_y))
