#!/usr/bin/env python3
"""arttest.py -- run tests/arttest.exe under Wine and compare it with the PNGs in art/ui/.

    make -C tagpu/ddraw arttest && tagpu/ddraw/tests/arttest.py

The exe prints every frame the DLL's tagpu_art.c inflates; this prints the same from the
PNGs through tools/artbake.py's reader and mapping. Equal line sets mean every frame the
DLL carries is the art in git, index for index. Exit 0 when they are equal.

WINEPREFIX defaults to the main checkout's `wineprefix/`."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools"))
import artbake  # noqa: E402


def expected():
    entries, _ = artbake.bake(REPO / "art/ui", REPO / "unditherer/ta-palette.json")
    out = []
    for style, name, frames in entries:
        for f in frames:
            code, w, h, hx, hy, key = __import__("struct").unpack("<HHHhhB", f[:11])
            hsh = 2166136261
            for b in f[12:]:
                hsh = ((hsh ^ b) * 16777619) & 0xFFFFFFFF
            out.append(f"{style} {name} {code} {w} {h} {hx} {hy} {key} {hsh:08x}")
    return out


def main():
    env = dict(os.environ)
    common = Path(subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "--git-common-dir"],
                                          text=True).strip())
    env.setdefault("WINEPREFIX", str((REPO / common).resolve().parent / "wineprefix"))
    env.setdefault("WINEDEBUG", "-all")
    got = subprocess.run(["wine", str(HERE / "arttest.exe")], env=env, capture_output=True,
                         text=True, timeout=120).stdout.replace("\r", "").splitlines()
    want = expected()
    if got == want:
        print(f"arttest: OK, {len(want)} frames equal")
        return 0
    for line in sorted(set(want) - set(got))[:20]:
        print("missing:", line)
    for line in sorted(set(got) - set(want))[:20]:
        print("unexpected:", line)
    print(f"arttest: FAILED ({len(got)} printed, {len(want)} expected)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
