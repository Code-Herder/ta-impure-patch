#!/usr/bin/env python3
"""randprobe_fixture — write test scripts whose commanders draw from the simulation RNG in their
first unit-tick script slice, for the demo recorder's level-start test (fixtures.json's
`randprobe`; tagpu_replay.c, THE FIRST STEP'S UNITS).

The stock ARMCOM and CORCOM scripts, read from the install at run time and decompiled with
tools/tacob, gain one thread that Create starts, RandProbe. Create runs with run-now inside the
create (0x485DE6), and the allocator 0x4B08C0 takes the lowest free record, so RandProbe's record
follows Create's and the create's run-now pass (0x4B09D4..0x4B09E2, every record with dt 0) reaches
it: its `sleep 34` (34 ms x 30 / 1000 = 1 tick) then holds it until the first unit tick's
DoScriptsNow(1) (0x48ADEB). There it draws twice from the simulation RNG 0x51FC88 (COB RAND,
0x4B15C9..0x4B15EE -> 0x4B6C30): which of RandTick0..RandTick3 it calls, and the period, 0.2 to
0.9 s, at which it calls it for the rest of the level, drawing nothing more. Every call allocates a
child at CALL's site (0x4B192D), which the counts see by name, so a playing whose first unit tick
draws from another RNG state than the recording machine's calls other scripts at other rates for
the whole level. Written into the game folder you point it at -- an instance's, never the
install's or the repository:

    tools/randprobe_fixture.py tagpu/instances/x/gamedir

They are loose files, scripts/armcom.cob and scripts/corcom.cob: a .ufo adds content but does not
override a path a stock archive already has, and a loose file does (file-formats.md §5, measured
with tools/tacob's pack). An instance's game folder links the install's files one by one, so a
scripts folder that is a link, which would write into the install, is refused.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from importlib.machinery import SourceFileLoader
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
datakeys = SourceFileLoader("datakeys_fixture", str(ROOT / "tools" / "datakeys_fixture.py")).load_module()

UNITS = ("armcom", "corcom")
TICKS = 4
PROBE = "\r\n".join(
    [f"RandTick{i}()\r\n{{\r\n}}\r\n" for i in range(TICKS)] +
    ["RandProbe()",
     "{",
     "\tvar pick, period;",
     "\tsleep 34;",
     f"\tpick = rand(0, {TICKS - 1});",
     "\tperiod = rand(2, 9);",
     "\twhile( TRUE )",
     "\t{"] +
    [f"\t\tif( pick == {i} )\r\n\t\t{{\r\n\t\t\tcall-script RandTick{i}();\r\n\t\t}}"
     for i in range(TICKS)] +
    ["\t\tsleep period * 100;",
     "\t}",
     "}",
     ""])


def probe(bos: str) -> str:
    """the script with RandProbe started last in Create, and the probe's scripts after it"""
    m = re.search(r"^Create\(\)\s*\{", bos, flags=re.M)
    if not m:
        sys.exit("randprobe_fixture: the script has no Create")
    end = bos.index("\n}", m.end())
    return bos[:end] + "\n\tstart-script RandProbe();" + bos[end:] + "\r\n" + PROBE


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gamedir", type=Path)
    a = ap.parse_args()
    gd = datakeys.gamedir()
    out = a.gamedir / "scripts"
    if out.is_symlink() or (out.exists() and out.resolve().parent != a.gamedir.resolve()):
        sys.exit(f"randprobe_fixture: {out} is not the game folder's own; nothing written")
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        for u in UNITS:
            bos, cob = tmp / f"{u}.bos", tmp / f"{u}.cob"
            tacob = [sys.executable, str(ROOT / "tools" / "tacob")]
            subprocess.run(tacob + ["decompile", u, "--gamedir", str(gd), "-o", str(bos)],
                           check=True, stdout=subprocess.DEVNULL)
            bos.write_text(probe(bos.read_text(encoding="latin-1")), encoding="latin-1")
            subprocess.run(tacob + ["compile", str(bos), "-o", str(cob),
                                    "-I", str(ROOT / "tools" / "tacob-include")],
                           check=True, stdout=subprocess.DEVNULL)
            (out / f"{u}.cob").write_bytes(cob.read_bytes())
    print(f"{out}: {', '.join(UNITS)} draw in their first slice")


if __name__ == "__main__":
    sys.exit(main())
