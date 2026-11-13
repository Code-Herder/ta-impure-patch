#!/usr/bin/env python3
"""econ_check -- ImpAI's economy op held to the engine's own record, in one held launch.

    tools/impai/econ_check.py <instance> [--scenario NAME]

Each ImpAI seat logs once a level `impai: econ seat=<i> t=<t> pass=<p> metal=... energy_out=...`:
what WORLDQ's economy op read of the seat's player record in its turn at GameTime t
(tagpu/ddraw/src/tagpu_impai.c, THE SEAT'S VIEW). The turn writes the line only when the record's
next 30-tick block is not due in that step (pass = the record's +0xF0, above t), and the economy
pass 0x401360 runs only inside that block, after the turn in the same iteration of the players
phase. So at the end of step t no pass has written the record since the line was read, whatever
the seat's phase, and a peek held there reads the same fields: both readings are of one tick.

The launch (default scenarios/impai-observe.json, seats 1 and 2 ImpAI) is held (tagpu_hold.on),
advanced to GameTime FROM - 1, then one tick at a time; after each tick every new econ line whose t
is the held tick is compared field by field with one peek of the record's eight floats (energy +0x8C,
production +0x90, request +0x94, metal +0x98, production +0x9C, request +0xA0, storage +0xA4 energy
and +0xA8 metal), each read as the op reads it -- finite, else 0 -- and held to the line's value as a
float32, bit for bit. Exit 0 when every seat the level init names compared equal, 1 on a difference
or a seat with no line within SPAN ticks, 2 when the launch could not be run.
"""
import argparse
import json
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools"))
import runlock  # noqa: E402
from resource_regression import TACLI, Check, tacli_api  # noqa: E402

LOAD_S = 900                            # a launch to a live map, a cold prefix included
FROM = 590                              # the line's first tick (tagpu_impai.c)
SPAN = 60                               # ticks the lines may take: two decisions past a due pass
REC = 0x1B63                            # record 0 in main; 0x14B a record (tagpu_engine.h)
STRIDE = 0x14B
FIELDS = (("metal", 0x98), ("energy", 0x8C), ("metal_max", 0xA8), ("energy_max", 0xA4),
          ("metal_in", 0x9C), ("energy_in", 0x90), ("metal_out", 0xA0), ("energy_out", 0x94))
LINE = re.compile(r"impai: econ seat=(\d+) t=(\d+) pass=(\d+) " +
                  " ".join(rf"{name}=(\S+)" for name, _ in FIELDS))
SEATS = re.compile(r"impai: seats ([0-9,]+|none) \(impure\.cfg\)")


def f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def as_op(u32):
    """A record float as the economy op answers it: itself when finite, else 0."""
    x = struct.unpack("<f", struct.pack("<I", u32))[0]
    return u32 if math.isfinite(x) else 0


def run(api, instance, scenario, out):
    gamedir = api.Instance(instance).gamedir
    lever = gamedir / "tagpu_hold.on"
    had = lever.exists()
    fails, seen, want = [], {}, set()
    lever.write_text("")
    try:
        r = subprocess.run([sys.executable, str(TACLI), "scenario", "load", instance, str(scenario),
                            "--defaults", "--restart"], capture_output=True, text=True,
                           timeout=LOAD_S)
        if r.returncode:
            raise OSError(f"scenario load: {(r.stdout + r.stderr).strip()[-400:]}")
        c = Check.__new__(Check)
        c.name = instance
        c.lockstep()
        c.hold()
        if c.tick < FROM - 1:
            c.advance(FROM - 1 - c.tick)
        text = api.talog.run_text(gamedir).splitlines()
        named = next((m.group(1) for m in map(SEATS.search, text) if m), None)
        if named is None or named == "none":
            raise OSError(f"the level init names no ImpAI seat ({named})")
        want = {int(s) for s in named.split(",")}
        start = len(c.log)
        while set(seen) < want and c.tick < FROM + SPAN:
            c.advance(1)
            c.log += [ln.rstrip() for ln in c.reader()]
            for ln in c.log[start:]:
                m = LINE.search(ln)
                if not m or int(m.group(1)) in seen:
                    continue
                seat, t, due = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if t != c.tick:
                    fails.append(f"seat {seat}: its line is of GameTime {t}, read at {c.tick}")
                    seen[seat] = None
                    continue
                specs = [f"*0x511DE8+0x{REC + seat * STRIDE + off:X}" for _, off in FIELDS]
                vals = [int(v.split()[0]) for v in c.snapshot(*specs)]
                diff = []
                for k, (name, _) in enumerate(FIELDS):
                    line_bits = f32_bits(float(m.group(4 + k)))
                    eng = as_op(vals[k])
                    if line_bits != eng:
                        diff.append(f"{name}: line {m.group(4 + k)}, engine "
                                    f"{struct.unpack('<f', struct.pack('<I', eng))[0]!r}")
                seen[seat] = diff
                out.append(f"  seat {seat}: GameTime {t}, next pass {due}: "
                           + ("every field equal" if not diff else "; ".join(diff)))
                fails += [f"seat {seat}: {d}" for d in diff]
            start = len(c.log)
    finally:
        try:
            subprocess.run([sys.executable, str(TACLI), "stop", instance],
                           capture_output=True, text=True, timeout=300)
        finally:
            if not had:
                lever.unlink(missing_ok=True)
    for seat in sorted(want - set(seen)):
        fails.append(f"seat {seat}: no `impai: econ` line by GameTime {FROM + SPAN}")
    return fails


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("instance")
    p.add_argument("--scenario", default="impai-observe.json",
                   help="a scenario in scenarios/ with ImpAI seats (default impai-observe.json)")
    a = p.parse_args()
    api = tacli_api()
    runlock.hold(TREE, f"econ_check on {a.instance}")
    out = []
    try:
        fails = run(api, a.instance, TREE / "scenarios" / a.scenario, out)
    except (AssertionError, OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as e:
        print("\n".join(out))
        print(f"econ_check: COULD NOT RUN: {e}")
        return 2
    print("\n".join(out))
    for f in fails:
        print(f"  FAIL {f}")
    print(f"econ_check: {'FAIL' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
