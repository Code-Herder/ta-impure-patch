#!/usr/bin/env python3
"""handicap_row6 -- row 6 of the Difficulty handicap, a mobile energy producer, on Escalation.

    tools/impai/handicap_row6.py [--dll tagpu/ddraw/ddraw.dll] [--gpu]

Row 6 (`0x40167C`) is the economy pass's test of the owner of a mobile unit with a negative
EnergyUse that is active (+0x10E bit 0) or moving (+0x110 & 0xC), `0x401617..0x40167C`
(research/notes/engine/sim.md, Difficulty and the AI handicap). No retail unit has one, so
handicap_check.py cannot reach it; Escalation GOLD 10.2.0 has two, CORSFUS_UPGRADE and
ARMSFUS_UPGRADE (T3ESC.ufo, unitse/: canmove=1, MaxVelocity=1, EnergyUse=-1250).

The setup is the compatibility suite's `escalation` (tools/compat/tacompat.py, prepare_wine), the
instance `compat-escalation-<tag>-row6` (the run's tag, tools/runlock.py). Escalation's skirmish
seats two players, so the two branches take two launches, both at Easy, one at a time under the run lock: an idle ARM human in slot 0 and
a CORE computer seat in slot 1, played by OTA AI in the first launch and by ImpAI (`seatai=1`) in
the second. The launch is held (tagpu_hold.on), and two CORSFUS_UPGRADE units of seat 1 are
applied 150 wu either side of its commander, the western one ordered 300 wu south, so that one
of them meets the test by its motion should the applier create them switched off. The level is
advanced by exact ticks until row 6's count for the launch's branch is above 0, at most
ROWS_TICKS ticks, and the other branch's count must still be 0: record 1 is the only type-2
record, so a scaled count is OTA AI's and an exempt one ImpAI's. Each launch also checks that
record 1 is controller 2 and that the seat table marks it in the ImpAI launch only, and prints
each upgrade unit's +0x10E and +0x110, which say which of the two conditions it met.

The launches render with GDI (tagpu_gdi.on in the instance's game folder, which the run makes afresh
and removes) and take no GPU token; --gpu renders them with the default renderer and takes one.

Exit 0 when both launches pass, 1 on a failed check, 2 when a launch could not be run.
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools/compat"))
sys.path.insert(0, str(TREE / "tools/impai"))
import runlock  # noqa: E402
import tacompat as tc  # noqa: E402
from handicap_check import (ATTACH, LOAD_S, ROSTER_S, ROWS_TICKS, STEP, TACLI, Held,  # noqa: E402
                            counts, field)
from resource_regression import tacli_api  # noqa: E402

SETUP = "escalation"
SUFFIX = "-row6"
SEAT = 1                                # the computer seat's record
ROW = 6
UNIT = "CORSFUS_UPGRADE"
LISTED = UNIT[:12]                      # a roster line holds 12 characters of a name
UNITS = "*0x511DE8+0x14357:4"           # the unit array; 0x118 a unit (tagpu_engine.h)
UNIT_STRIDE = 0x118
SCALED, EXEMPT = 0, 1


def scenario(who):
    return {"format": "ta-scenario/1",
            "description": f"row 6 on Escalation: slot 1 played by {who}",
            "setup": {"map": "Two Continents", "res": "1024x768", "clear_existing": False,
                      "difficulty": "easy",
                      "players": [{"slot": 0, "controller": "human", "side": "arm", "color": 0},
                                  {"slot": SEAT, "controller": who, "side": "core", "color": 1}]}}


def upgrades(x, z):
    return {"format": "ta-scenario/1", "description": "two mobile energy producers of seat 1",
            "setup": {"clear_existing": False},
            "units": [{"id": "still", "type": UNIT, "owner": SEAT, "pos": [x + 150, z], "facing": 0},
                      {"id": "moving", "type": UNIT, "owner": SEAT, "pos": [x - 150, z], "facing": 0,
                       "orders": [{"cmd": "move", "to": [x - 150, z + 300]}]}]}


def roster(c, want, what):
    """The roster's units once `want` holds of them; the roster is logged every 5 s."""
    deadline = time.monotonic() + ROSTER_S
    while True:
        units = c.call("roster", "--json")["units"]
        if want(units):
            return units
        if time.monotonic() > deadline:
            raise OSError(f"the roster does not list {what} after {ROSTER_S} s")
        time.sleep(1)


def launch(api, name, who, out):
    """One launch with slot 1 played by `who`; the list of failed checks."""
    gamedir = api.Instance(name).gamedir
    lever = gamedir / "tagpu_hold.on"
    branch = SCALED if who == "ai" else EXEMPT
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        scn, units_scn = Path(tmp) / f"row6-{who}.json", Path(tmp) / "row6-units.json"
        scn.write_text(json.dumps(scenario(who)))
        lever.write_text("")
        try:
            # Escalation's skirmish loads its own map whatever the registry names
            r = subprocess.run([sys.executable, str(TACLI), "scenario", "load", name, str(scn),
                                "--defaults", "--restart", "--any-map"], capture_output=True,
                               text=True, timeout=LOAD_S)
            if r.returncode:
                raise OSError(f"scenario load: {(r.stdout + r.stderr).strip()[-400:]}")
            c = Held(name)
            c.hold()
            text = api.talog.run_text(gamedir).splitlines()
            where = next((m for m in map(ATTACH.search, text) if m), None)
            if where is None:
                raise OSError("no `impai: seam ... in; seats at ...` line: ImpAI is not in")
            table, hits_at = (int(g, 16) for g in where.groups())
            ctl, mark = c.values(field(SEAT, 0x73, 1), f"0x{table + SEAT:08X}:1")
            com = next(u for u in roster(c, lambda us: any(
                u["owner"] == SEAT and u["type"].upper() == "CORCOM" for u in us),
                "seat 1's commander") if u["owner"] == SEAT and u["type"].upper() == "CORCOM")
            units_scn.write_text(json.dumps(upgrades(*com["world"])))
            r = subprocess.run([sys.executable, str(TACLI), "scenario", "apply", name,
                                str(units_scn)], capture_output=True, text=True, timeout=300)
            if r.returncode:
                raise OSError(f"scenario apply: {(r.stdout + r.stderr).strip()[-400:]}")
            ups = [u for u in roster(c, lambda us: sum(u["type"].upper() == LISTED for u in us) == 2,
                                     f"two {UNIT}") if u["type"].upper() == LISTED]
            start = c.tick
            hits = counts(c, hits_at)[ROW]
            while not hits[branch] and c.tick - start < ROWS_TICKS:
                c.advance(STEP)
                hits = counts(c, hits_at)[ROW]
            base = c.values(UNITS)[0]
            state = c.values(*(f"0x{base + u['engine_index'] * UNIT_STRIDE + off:08X}:{n}"
                               for u in ups for off, n in ((0x10E, 1), (0x110, 4))))
        finally:
            try:
                subprocess.run([sys.executable, str(TACLI), "stop", name],
                               capture_output=True, text=True, timeout=300)
            finally:
                lever.unlink(missing_ok=True)
    out.append(f"  {who}: row {ROW} scaled/exempt at GameTime {c.tick}: {hits[0]}/{hits[1]}")
    for u, (active, motion) in zip(ups, zip(state[::2], state[1::2])):
        out.append(f"  {who}: {UNIT} {u['engine_index']}: +0x10E bit 0 = {active & 1}, "
                   f"+0x110 & 0xC = 0x{motion & 0xC:X}")
    if ctl != 2:
        fails.append(f"record {SEAT} is controller {ctl}, not 2")
    if mark != (who == "impai"):
        fails.append(f"the seat table marks record {SEAT} as {mark}, not {int(who == 'impai')}")
    if not hits[branch]:
        fails.append(f"row {ROW} sent no record down its "
                     f"{'scaled continuation' if branch == SCALED else 'unscaled branch'} by "
                     f"GameTime {c.tick}")
    if hits[1 - branch]:
        fails.append(f"row {ROW} sent {hits[1 - branch]} records down its "
                     f"{'unscaled branch' if branch == SCALED else 'scaled continuation'}, "
                     f"which no record of this launch takes")
    return fails


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw.dll",
                   help="the ddraw.dll under test")
    p.add_argument("--gpu", action="store_true",
                   help="render with the default renderer and take a GPU token, not GDI")
    a = p.parse_args()
    if not a.dll.is_file():
        print(f"handicap_row6: no DLL at {a.dll}", file=sys.stderr)
        return 2
    # one launch at a time; the instance carries the tag
    runlock.hold(TREE, "handicap_row6 on Escalation", 1, gpu=a.gpu)
    setup = tc.pick_setups([SETUP])[0]
    xv = tc.start_xvfb()
    failed = 0
    try:
        inst = tc.prepare_wine(setup, a.dll, xv.tacompat_display, suffix=SUFFIX)
        # tacompat parks tacli's store for a player's launch; this one is tacli's own
        parked = inst["gamedir"] / "tacli-state.off"
        if parked.is_dir():
            parked.rename(inst["gamedir"] / "tacli-state")
        gdi = inst["gamedir"] / "tagpu_gdi.on"
        gdi.unlink(missing_ok=True)         # unlinked first: a link is never written through
        if not a.gpu:
            gdi.write_text("")
        api = tacli_api()
        for who in ("ai", "impai"):
            out = []
            caps = api.talog.CapWatch(inst["gamedir"])      # this launch's refusals of the GPU memory cap
            try:
                fails = launch(api, inst["name"], who, out)
            except (AssertionError, OSError, StopIteration, subprocess.TimeoutExpired) as e:
                print("\n".join(out))
                print(f"handicap_row6 {who}: COULD NOT RUN: {e}")
                return 2
            # the launch has stopped: a refusal of the GPU memory cap anywhere in it fails it
            refused = caps.failure(inst["name"])
            if refused:
                fails.append(refused)
            print("\n".join(out))
            for f in fails:
                print(f"  {who}: FAIL {f}")
            print(f"handicap_row6 {who}: {'FAIL' if fails else 'PASS'}")
            failed += bool(fails)
    finally:
        tc.tacli("rm", tc.inst_name(SETUP, SUFFIX), "--force")
        tc.stop_xvfb(xv)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
