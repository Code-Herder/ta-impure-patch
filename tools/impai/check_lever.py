#!/usr/bin/env python3
"""check_lever -- write ImpAI's test levers into an instance's game folder, for the replay
fixtures `impai-observe` and `impai-observe-perm` (tools/replay/fixtures.json).

    tools/impai/check_lever.py tagpu/instances/x/gamedir

writes an empty `tagpu_impai_check.on` and `tagpu_impaiov.on` naming seat 1. The DLL reads both
once, at attach (tagpu/ddraw/src/tagpu_impai.c), so they must be in place before the game starts:
every ImpAI turn then compares its line-of-sight rule with the engine's 0x465AC0, the call at
0x46556D runs the radar pass's copy beside the engine's, the level end writes the patches'
evidence, and seat 1's overlay is built and its digests logged (impai_check.py (b)). The seats ImpAI plays come from the scenario's `impai`
controllers (impure.cfg's `seatai`), not from here. A game folder links the install's files one
by one, so a lever path that is a link, which would write into the install, is refused.
"""
import argparse
import sys
from pathlib import Path

LEVERS = {"tagpu_impai_check.on": "", "tagpu_impaiov.on": "1\n"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gamedir", type=Path, help="the instance's game folder")
    args = ap.parse_args()
    if not args.gamedir.is_dir():
        print(f"check_lever: {args.gamedir} is not a folder", file=sys.stderr)
        return 2
    for name, text in LEVERS.items():
        lever = args.gamedir / name
        if lever.is_symlink():
            print(f"check_lever: {lever} is a link; refusing to write through it", file=sys.stderr)
            return 2
        lever.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
