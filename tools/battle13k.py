#!/usr/bin/env python3
"""The large-game performance case, played on this tree's ddraw.dll: scenarios/zombie-kingdom-13k.json --
13 500 units of ten seats on the map [M] Zombie Kingdom under Total Mayhem 11.3.0.

  tools/battle13k.py --display 391 --out DIR    measure: the fight under the frame profiler (tagpu_prof.on) with
                                                the pathfinder's log (tagpu_pathlog.on). DIR gets the profile's
                                                CSVs, the game's logs and path.log, the log's `pathsched:` lines
  tools/battle13k.py --display 0 --watch        show: the same battle unshielded, left running for a person

THE MAP is a third party's and is not in the repository: `--map` or TA_BATTLE13K_MAP names a copy of
`[M] Zombie Kingdom.ufo`. The mod's folder is the compatibility suite's (tools/compat/tacompat.py, mayhem-11.3.0).
THE DISPLAY: the client is 1920x1080 (TA_BATTLE13K_RES names another, e.g. 3840x2160). A measurement belongs on a private X server
(`Xvfb :391 -screen 0 1920x1080x24 -noreset -nolisten tcp`), never on a desktop in use.
THE RUN LOCK: a measurement holds two lanes of it (tools/runlock.py) while its game plays -- the game thread, the
render thread and the pathfinder's helpers are busy at once. A shown game is a person's own play and holds none.
THE BATTLE DOES NOT PLAY THE SAME TWICE: compare ranges of runs, never one run against one
(research/notes/modules/pathfinder.md, Measurements).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TREE / "tools" / "compat"), str(TREE / "tools")]
import runlock      # noqa: E402
import tacompat     # noqa: E402

SETUP = "mayhem-11.3.0"
SCENARIO = "zombie-kingdom-13k"
RES = os.environ.get("TA_BATTLE13K_RES", "1920x1080")   # the client's size; the X server must hold it
TACLI = str(TREE / "tools" / "tacli")
# what a measurement arms in the game's folder, and a shown game must not find there
LEVERS = ("tagpu_prof.on", "tagpu_prof_sample.on", "tagpu_pathlog.on", "tagpu_pathverify.on", "tagpu_candverify.on")


def tacli(*argv, timeout=900):
    r = subprocess.run([sys.executable, TACLI, *argv], capture_output=True, text=True, timeout=timeout)
    last = (r.stdout + r.stderr).strip().splitlines()[-1:]
    print("$ tacli", " ".join(argv), "->", r.returncode, *last, flush=True)
    if r.returncode != 0:
        raise SystemExit(f"battle13k: tacli {argv[0]} failed")
    return r


def ls_line(name):
    out = subprocess.run([sys.executable, TACLI, "ls"], capture_output=True, text=True, timeout=120).stdout
    return next((line for line in out.splitlines() if line.split()[:1] == [name]), "")


def start(name, *launch_flags):
    """the game up in the battle: the scenario's load launches it again, so a lever read at attach stays armed
    from before the first launch to after the second"""
    tacli("arm", name, "gdi.on=off")
    for lever in ("fx", "feat"):
        tacli("arm", name, f"{lever}.off=off")
    tacli("launch", name, "--defaults", "--res", RES, *launch_flags)
    tacli("scenario", "load", name, SCENARIO, "--defaults", "--restart", "--res", RES,
          "--load-timeout", "300", "--timeout", "600")


def measure(name, gamedir, out, seconds, verify):
    log = gamedir / "log"
    prof = gamedir / "tagpu_prof.on"
    (gamedir / "tagpu_pathlog.on").write_text("")
    if verify:
        (gamedir / "tagpu_pathverify.on").write_text("")
    prof.write_text("")                  # the profiler's hook goes in at attach
    try:
        start(name)
        prof.unlink()
        time.sleep(3)
        prof.write_text("")
        time.sleep(seconds + 1.5)
        prof.unlink()
        time.sleep(2.0)                  # the lever is polled every 0.5 s, then the CSVs are flushed and closed
        for f in list(log.glob("tagpu_prof_*.csv")) + list(log.glob("tagpu*.log")):
            shutil.copy2(f, out / f.name)
    finally:
        for lever in LEVERS:
            (gamedir / lever).unlink(missing_ok=True)
        tacli("stop", name)
    lines = [line for f in sorted(out.glob("tagpu*.log"), key=lambda p: p.stat().st_mtime)
             for line in f.read_text(errors="replace").splitlines() if "pathsched" in line]
    (out / "path.log").write_text("\n".join(lines) + "\n")
    subprocess.run([sys.executable, str(TREE / "tools" / "profsum.py"), str(out / "tagpu_prof_game.csv")])
    print(f"battle13k: {len(lines)} pathsched lines in {out / 'path.log'}")


def watch(name, gamedir):
    for lever in LEVERS:
        (gamedir / lever).unlink(missing_ok=True)
    start(name, "--no-shield")
    if " OPEN " not in ls_line(name) + " ":
        tacli("shield", name, "off")
    print(f"battle13k: {name} is up, unshielded; this process ends with the game", flush=True)
    while "running" in ls_line(name):
        time.sleep(5)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--display", type=int, required=True, help="the X display's number")
    ap.add_argument("--map", help="[M] Zombie Kingdom.ufo (else TA_BATTLE13K_MAP)")
    ap.add_argument("--out", type=Path, help="a measurement's folder")
    ap.add_argument("--seconds", type=float, default=125, help="how long the fight is profiled (125)")
    ap.add_argument("--verify", action="store_true", help="measure under tagpu_pathverify.on")
    ap.add_argument("--watch", action="store_true", help="show the battle instead of measuring it")
    args = ap.parse_args()
    ufo = Path(args.map or os.environ.get("TA_BATTLE13K_MAP", ""))
    if not ufo.is_file():
        ap.error("the map is not in the repository: --map or TA_BATTLE13K_MAP names [M] Zombie Kingdom.ufo")
    if not args.watch and not args.out:
        ap.error("a measurement needs --out DIR")
    if not args.watch:
        runlock.hold(TREE, "battle13k: the 13 500-unit battle under the frame profiler", lanes=2)
        args.out.mkdir(parents=True, exist_ok=True)
    setup = next(s for s in tacompat.SETUPS if s["name"] == SETUP)
    inst = tacompat.prepare_wine(setup, TREE / "tagpu" / "ddraw" / "ddraw.dll", args.display,
                                 "-13k-watch" if args.watch else "-13k")
    lease = tacompat.as_test_instance(inst)      # the game's DirectPlay port, held while this process lives
    try:
        name, gamedir = inst["name"], Path(inst["gamedir"])
        shutil.copy2(ufo, gamedir / ufo.name)
        if args.watch:
            watch(name, gamedir)
        else:
            measure(name, gamedir, args.out, args.seconds, args.verify)
    finally:
        lease.close()


if __name__ == "__main__":
    main()
