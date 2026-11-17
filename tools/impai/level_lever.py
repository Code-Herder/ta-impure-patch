#!/usr/bin/env python3
"""level_lever -- write ImpAI's level and persona levers into an instance's game folder, for a replay fixture's
content (tools/replay/fixtures.json) and the levels' calibration.

    tools/impai/level_lever.py tagpu/instances/x/gamedir 1=hard [2=easy ...]
    tools/impai/level_lever.py tagpu/instances/x/gamedir --persona 1=60,90,45,10,3,15,3,4,2 [--persona ...]
    tools/impai/level_lever.py tagpu/instances/x/gamedir --income 1=2.5 [--income ...]
    tools/impai/level_lever.py tagpu/instances/x/gamedir --tune 1:attack_margin=150 [--tune ...]

writes `tagpu_impai_level.on` with the level items given, `tagpu_impai_persona.on` with the personas given, one a
line, and `tagpu_impai_income.on` with the income multipliers given: `<seat>=<m>`, m a decimal with at most three
places in 1..8, the seat's production times m (tagpu/ddraw/src/tagpu_income.c, which reads it at attach too and
honours it at a live skirmish level's init, any controller's seat). The DLL reads both once, at attach (tagpu/ddraw/src/tagpu_impai.c, THE TEST LEVERS), so they must be
in place before the game starts, and honours them at a live level's init and for a seat a takeover newly marks:
a seat the level lever names plays that level whatever the Difficulty, a seat the persona lever names carries
the decisions out by that persona in place of its level's row, and a puppet's level and persona come from its
recording. A level item is `<seat>=easy|normal|hard`; a persona is `<seat>=` and nine integers,
`<look_eco>,<look_mil>,<react>,<apm_eco>,<burst_eco>,<apm_mil>,<burst_mil>,<att_eco>,<att_mil>`, held to the rules
of impai_persona_ok (tagpu/ddraw/src/impai_react.c); the seat is one digit, as the DLL parses it. An item the DLL
would skip is refused here, so a lever never holds one. A game folder links the install's files one by one, so
a lever is never written through a path: whatever stands at its name is removed (a link, not its target) and the
file is created anew by an open that refuses to follow a link or to find anything there, so a link that appears
in between fails the open instead of taking the write.

`--tune` writes `tagpu_impai_tune.on`, one `<seat>:<name>=<int>` a line: a seat's plan constant set in place of the
DLL's hand-written value, for the automatic tuning (tune.py spsa). The names, ranges and integer encoding are
TUNE_PARAMS', the interface specified for the brain writer (.data/impai/tune-lever-spec.md); a DLL that does not
read the lever logs no `impai: tune` line, so a game that needs one could not run (bar.py).
"""
import argparse
import os
import re
import sys
from pathlib import Path

LEVER = "tagpu_impai_level.on"
PERSONA_LEVER = "tagpu_impai_persona.on"
INCOME_LEVER = "tagpu_impai_income.on"
TUNE_LEVER = "tagpu_impai_tune.on"
ITEM = re.compile(r"[0-9]=(easy|normal|hard)", re.IGNORECASE)
INCOME = re.compile(r"[0-9]=([0-9]+)(?:\.([0-9]{1,3}))?")
PERSONA = re.compile(r"[0-9]=([0-9]{1,6}(?:,[0-9]{1,6}){8})")
TUNE = re.compile(r"([0-9]):([a-z_]{1,31})=(-?[0-9]{1,6})")

# THE TUNE LEVER'S CONSTANTS: name -> (the hand-written value, the least, the most), each an integer; a constant the
# DLL holds as a float is in thousandths (SCALE 1000), every other in its own unit. Where each lives and why its
# range is what it is: .data/impai/tune-lever-spec.md. TUNE_ORDER names the pairs whose order must hold (the first
# below the second; for the raid squad at most), as impai_plan_ok is to refuse a plan breaking one.
TUNE_PARAMS = {
    # the military (IMPAI_PLAN, impai_mil.c)
    "attack_margin":   (100, -100, 400),
    "retreat_margin":  (-50, -300, 100),
    "wave_value":      (8000, 3000, 16000),
    "lead_in":         (2500, 1500, 4000),
    "lead_out":        (2000, 1200, 3500),
    "press_k":         (1500, 500, 3000),
    "press_hold":      (900, 300, 2700),
    "rally_px":        (384, 128, 1024),
    "scout_period":    (1800, 600, 5400),
    "task_timeout":    (2700, 900, 5400),
    "raid_min":        (3, 2, 5),
    "raid_max":        (6, 3, 10),
    "travel_cap":      (16200, 5400, 27000),
    "com_stay_margin": (500, 0, 1000),
    "aa_struct_v":     (1000, 250, 4000),
    "def_max":         (4, 0, 4),
    "def_v":           (600, 200, 2000),
    "dgun_reserve":    (250, 0, 600),
    # the economy (IMPAI_PLAN, impai_brain.c and impai_econ.c)
    "open_mex":        (2, 1, 4),
    "open_energy":     (2, 1, 4),
    "open_t":          (1800, 600, 3600),
    "factory_max":     (2, 1, 4),
    "factory_per_m":   (8000, 4000, 16000),
    "cons_max":        (12, 4, 20),
    "army_per_cons":   (2, 1, 6),
    "stall_pm":        (200, 50, 600),
    "hold_fac":        (1, 1, 4),
    "surplus_frac":    (800, 600, 950),
}
TUNE_SCALE = {n: 1000 for n in ("attack_margin", "retreat_margin", "wave_value", "lead_in", "lead_out", "press_k",
                                "com_stay_margin", "dgun_reserve", "factory_per_m", "surplus_frac")}
TUNE_ORDER = (("retreat_margin", "attack_margin", "below"), ("lead_out", "lead_in", "below"),
              ("raid_min", "raid_max", "at most"), ("stall_pm", "surplus_frac", "below"))

# impai_react.c: LOOK_MAX, APM_MAX, BURST_MAX; impai.h: IMPAI_IDLE_CAP, IMPAI_SQUADS
LOOK_MAX, APM_MAX, BURST_MAX, IDLE_CAP, SQUADS = 150, 600, 60, 32, 16


def persona_error(item: str) -> str | None:
    """Why the DLL would refuse the persona item, or None when impai_persona_ok accepts it."""
    m = PERSONA.fullmatch(item)
    if not m:
        return "is not <seat>= and nine integers"
    look_eco, look_mil, react, apm_eco, burst_eco, apm_mil, burst_mil, att_eco, att_mil = (
        int(v) for v in m.group(1).split(","))
    if any(not (15 <= look <= LOOK_MAX and look % 15 == 0) for look in (look_eco, look_mil)):
        return f"has a look outside the multiples of 15 in 15..{LOOK_MAX}"
    if react > min(look_eco, look_mil) - 15:
        return "reacts later than its shorter look less 15"
    for apm, burst in ((apm_eco, burst_eco), (apm_mil, burst_mil)):
        if apm > APM_MAX:
            return f"has a rate past {APM_MAX}"
        if (apm and not 1 <= burst <= BURST_MAX) or (not apm and burst):
            return f"has a burst outside 1..{BURST_MAX} with a rate, or one without"
    if att_eco > IDLE_CAP or att_mil > SQUADS:
        return f"has an attention past {IDLE_CAP} builders or {SQUADS} operations"
    return None


def income_error(item: str) -> str | None:
    """Why the DLL would refuse the income item (tagpu_income.c, lever_item), or None when it takes it."""
    m = INCOME.fullmatch(item)
    if not m:
        return "is not <seat>=<m>, m a decimal with at most three places"
    if not 1000 <= int(m.group(1)) * 1000 + int((m.group(2) or "").ljust(3, "0")) <= 8000:
        return "has a multiplier outside 1..8"
    return None


def tune_values(items: dict) -> dict:
    """Every TUNE_PARAMS constant's value with `items` ({name: int}) in place of the hand-written ones."""
    return {n: int(items.get(n, d)) for n, (d, _lo, _hi) in TUNE_PARAMS.items()}


def tune_error(seat: int, items: dict) -> str | None:
    """Why the DLL would refuse a seat's tune items {name: int}, or None when impai_plan_ok accepts the plan they
    make with the hand-written values of the rest."""
    if not 0 <= seat <= 9:
        return f"seat {seat} is not one digit"
    for n, v in items.items():
        if n not in TUNE_PARAMS:
            return f"names {n!r}, no constant of the lever"
        _d, lo, hi = TUNE_PARAMS[n]
        if not (isinstance(v, int) and lo <= v <= hi):
            return f"has {n}={v} outside {lo}..{hi}"
    vals = tune_values(items)
    for lo_name, hi_name, how in TUNE_ORDER:
        if vals[lo_name] > vals[hi_name] or (how == "below" and vals[lo_name] == vals[hi_name]):
            return f"has {lo_name} {vals[lo_name]} not {how} {hi_name} {vals[hi_name]}"
    return None


def tune_lines(seat: int, items: dict) -> list[str]:
    """The lever's lines of a seat's items, in TUNE_PARAMS' order."""
    return [f"{seat}:{n}={items[n]}" for n in TUNE_PARAMS if n in items]


def parse_tune(items: list[str]) -> dict:
    """{seat: {name: int}} of `--tune` items `<seat>:<name>=<int>`; ValueError naming the first item refused."""
    out = {}
    for item in items:
        m = TUNE.fullmatch(item)
        if not m:
            raise ValueError(f"{item} is not <seat>:<name>=<int>")
        seat, name = int(m.group(1)), m.group(2)
        if name in out.setdefault(seat, {}):
            raise ValueError(f"{item} names {name} a second time for seat {seat}")
        out[seat][name] = int(m.group(3))
    for seat, its in out.items():
        why = tune_error(seat, its)
        if why:
            raise ValueError(f"seat {seat} {why}")
    return out


def write_lever(gamedir: Path, name: str, items: list[str]) -> int:
    lever = gamedir / name
    lever.unlink(missing_ok=True)
    try:
        fd = os.open(lever, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    except OSError as e:
        print(f"level_lever: {lever}: {e.strerror}; refusing to write", file=sys.stderr)
        return 2
    with os.fdopen(fd, "w") as f:
        f.write("".join(f"{item}\n" for item in items))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    ap.add_argument("items", nargs="*", help="<seat>=easy|normal|hard")
    ap.add_argument("--persona", action="append", default=[], metavar="SEAT=9INTS",
                    help="<seat>=<look_eco>,<look_mil>,<react>,<apm_eco>,<burst_eco>,<apm_mil>,<burst_mil>,"
                         "<att_eco>,<att_mil>")
    ap.add_argument("--income", action="append", default=[], metavar="SEAT=M",
                    help="<seat>=<m>: the seat's production times m, 1..8")
    ap.add_argument("--tune", action="append", default=[], metavar="SEAT:NAME=INT",
                    help=f"<seat>:<name>=<int>, a plan constant: {', '.join(TUNE_PARAMS)}")
    args = ap.parse_args(argv)
    if not args.items and not args.persona and not args.income and not args.tune:
        print("level_lever: name a level item, a --persona, an --income or a --tune", file=sys.stderr)
        return 2
    if not args.gamedir.is_dir():
        print(f"level_lever: {args.gamedir} is not a folder", file=sys.stderr)
        return 2
    bad = [item for item in args.items if not ITEM.fullmatch(item)]
    if bad:
        print(f"level_lever: {', '.join(bad)} is not <seat>=easy|normal|hard", file=sys.stderr)
        return 2
    for item in args.persona:
        why = persona_error(item)
        if why:
            print(f"level_lever: --persona {item} {why} (impai_persona_ok)", file=sys.stderr)
            return 2
    for item in args.income:
        why = income_error(item)
        if why:
            print(f"level_lever: --income {item} {why} (tagpu_income.c)", file=sys.stderr)
            return 2
    try:
        tune = parse_tune(args.tune)
    except ValueError as e:
        print(f"level_lever: --tune {e} (impai_plan_ok)", file=sys.stderr)
        return 2
    if tune and write_lever(args.gamedir, TUNE_LEVER, [ln for s in sorted(tune) for ln in tune_lines(s, tune[s])]):
        return 2
    if args.items and write_lever(args.gamedir, LEVER, args.items):
        return 2
    if args.persona and write_lever(args.gamedir, PERSONA_LEVER, args.persona):
        return 2
    if args.income and write_lever(args.gamedir, INCOME_LEVER, args.income):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
