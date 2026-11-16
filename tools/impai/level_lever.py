#!/usr/bin/env python3
"""level_lever -- write ImpAI's level and persona levers into an instance's game folder, for a replay fixture's
content (tools/replay/fixtures.json) and the levels' calibration.

    tools/impai/level_lever.py tagpu/instances/x/gamedir 1=hard [2=easy ...]
    tools/impai/level_lever.py tagpu/instances/x/gamedir --persona 1=60,90,45,10,3,15,3,4,2 [--persona ...]
    tools/impai/level_lever.py tagpu/instances/x/gamedir --income 1=2.5 [--income ...]

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
"""
import argparse
import os
import re
import sys
from pathlib import Path

LEVER = "tagpu_impai_level.on"
PERSONA_LEVER = "tagpu_impai_persona.on"
INCOME_LEVER = "tagpu_impai_income.on"
ITEM = re.compile(r"[0-9]=(easy|normal|hard)", re.IGNORECASE)
INCOME = re.compile(r"[0-9]=([0-9]+)(?:\.([0-9]{1,3}))?")
PERSONA = re.compile(r"[0-9]=([0-9]{1,6}(?:,[0-9]{1,6}){8})")

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
    args = ap.parse_args(argv)
    if not args.items and not args.persona and not args.income:
        print("level_lever: name a level item, a --persona or an --income", file=sys.stderr)
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
    if args.items and write_lever(args.gamedir, LEVER, args.items):
        return 2
    if args.persona and write_lever(args.gamedir, PERSONA_LEVER, args.persona):
        return 2
    if args.income and write_lever(args.gamedir, INCOME_LEVER, args.income):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
