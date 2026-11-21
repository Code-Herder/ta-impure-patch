#!/usr/bin/env python3
"""write_lever -- write empty lever files into an instance's game folder, as a replay fixture's
`content` command (tools/replay/fixtures.json):

    tools/replay/write_lever.py tagpu/instances/x/gamedir tagpu_pathverify.on

The DLL reads a lever at attach, so the files must be in place before the game starts. A game
folder links the install's files one by one, so a lever path that is a link, which would write
into the install, is refused; so is a name that is not a plain `tagpu_*.on`.
"""
import argparse
import re
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    ap.add_argument("levers", nargs="+", help="lever file names, tagpu_*.on")
    args = ap.parse_args()
    if not args.gamedir.is_dir():
        print(f"write_lever: {args.gamedir} is not a folder", file=sys.stderr)
        return 2
    for name in args.levers:
        lever = args.gamedir / name
        if not re.fullmatch(r"tagpu_\w+\.on", name):
            print(f"write_lever: {name} is not a lever's name", file=sys.stderr)
            return 2
        if lever.is_symlink():
            print(f"write_lever: {lever} is a link; refusing to write through it", file=sys.stderr)
            return 2
        lever.write_text("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
