#!/usr/bin/env python3
"""impai_fixture — write ImpAI's seat lever into an instance's game folder, for the replay
fixtures `impai-observe` and `impai-observe-perm` (tools/replay/fixtures.json).

    tools/impai_fixture.py tagpu/instances/x/gamedir 1,2

writes `tagpu_impai.on` holding the seats given: `ai` (every record whose controller is 2 at the
level's start) or a comma-separated list of record indexes 0..9. The DLL reads it once, at attach
(tagpu/ddraw/src/tagpu_impai.c), so it must be in place before the game starts. A game folder
links the install's files one by one, so a lever path that is a link, which would write into
the install, is refused.
"""
import argparse
import re
import sys
from pathlib import Path

LEVER = "tagpu_impai.on"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    ap.add_argument("seats", help="`ai`, or record indexes 0..9 separated by commas")
    args = ap.parse_args()
    if args.seats != "ai" and not re.fullmatch(r"[0-9](,[0-9])*", args.seats):
        print(f"impai_fixture: {args.seats!r} is neither `ai` nor a list such as 1,2",
              file=sys.stderr)
        return 2
    if not args.gamedir.is_dir():
        print(f"impai_fixture: {args.gamedir} is not a folder", file=sys.stderr)
        return 2
    lever = args.gamedir / LEVER
    if lever.is_symlink():
        print(f"impai_fixture: {lever} is a link; refusing to write through it", file=sys.stderr)
        return 2
    lever.write_text(args.seats + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
