#!/usr/bin/env python3
"""level_lever -- write ImpAI's level lever into an instance's game folder, for a replay fixture's content
(tools/replay/fixtures.json).

    tools/impai/level_lever.py tagpu/instances/x/gamedir 1=hard [2=easy ...]

writes `tagpu_impai_level.on` with the items given, one a line. The DLL reads it once, at attach
(tagpu/ddraw/src/tagpu_impai.c, THE TEST LEVERS), so it must be in place before the game starts, and honours it
at a live level's init and for a seat a takeover newly marks: a seat it names plays that level whatever the
Difficulty, and a puppet's level comes from its recording. Each item is `<seat>=easy|normal|hard`, the seat one
digit, as the DLL parses it; an item it would skip is refused here, so the lever never holds one. A game folder
links the install's files one by one, so the lever is never written through a path: whatever stands at its name
is removed (a link, not its target) and the file is created anew by an open that refuses to follow a link or
to find anything there, so a link that appears in between fails the open instead of taking the write.
"""
import argparse
import os
import re
import sys
from pathlib import Path

LEVER = "tagpu_impai_level.on"
ITEM = re.compile(r"[0-9]=(easy|normal|hard)", re.IGNORECASE)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    ap.add_argument("items", nargs="+", help="<seat>=easy|normal|hard")
    args = ap.parse_args()
    if not args.gamedir.is_dir():
        print(f"level_lever: {args.gamedir} is not a folder", file=sys.stderr)
        return 2
    bad = [item for item in args.items if not ITEM.fullmatch(item)]
    if bad:
        print(f"level_lever: {', '.join(bad)} is not <seat>=easy|normal|hard", file=sys.stderr)
        return 2
    lever = args.gamedir / LEVER
    lever.unlink(missing_ok=True)
    try:
        fd = os.open(lever, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    except OSError as e:
        print(f"level_lever: {lever}: {e.strerror}; refusing to write", file=sys.stderr)
        return 2
    with os.fdopen(fd, "w") as f:
        f.write("".join(f"{item}\n" for item in args.items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
