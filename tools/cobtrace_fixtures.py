#!/usr/bin/env python3
"""Run the nine tacob gate scenarios and keep their traces as fixtures.

For each class scenario (scenarios/cob-*.json) this arms `tagpu_cobtrace.on`
with the class's unit type as the filter and `tagpu_native.on=all` (the pose
oracle lives in the native unit pass), loads the scenario into one tacli
instance, lets the behaviour run, drops `tagpu_posedump.on` once, waits a
little more, stops the game and copies out:

    <out>/<name>/cobtrace.log   every S/R/X/K/D line the type produced
    <out>/<name>/posedump.txt   the one posedump block (tick= idx= header first)
    <out>/<name>/apply.json     tacli's load report (engine indices, positions)

The trace contract and the nine scenarios: research/notes/tacob-design.md.
Run from the tree whose ddraw.dll you want measured (tacli pins that tree's
build). Usage:

    tools/cobtrace_fixtures.py                 # all nine, instance `cobfx`
    tools/cobtrace_fixtures.py kbot death      # a subset
    tools/cobtrace_fixtures.py --instance c2 --out /tmp/fx --keep
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
TACLI = HERE / "tacli"

#            name        type       run s  after-posedump s
CLASSES = [("kbot",     "ARMPW",     8,  3),
           ("tank",     "ARMSTUMP",  8,  3),
           ("building", "ARMWIN",    8,  3),
           ("death",    "ARMPW",     3,  3),
           ("fighter",  "ARMHAWK",  10,  3),
           ("gunship",  "ARMBRAWL", 10,  3),
           ("bomber",   "ARMTHUND", 12,  3),
           ("ship",     "CORBATS",  10,  3),
           ("sub",      "CORSUB",   10,  3)]


def tacli(*args, timeout=400):
    p = subprocess.run([str(TACLI), *args], cwd=str(ROOT), text=True,
                       capture_output=True, timeout=timeout)
    if p.returncode != 0:
        sys.stderr.write(p.stdout + p.stderr)
        raise SystemExit(f"tacli {' '.join(args)} failed ({p.returncode})")
    return p.stdout


def gamedir(inst):
    for i in json.loads(tacli("ls", "--json")):
        if i["name"] == inst:
            return pathlib.Path(i["gamedir"])
    raise SystemExit(f"no instance {inst}")


def last_posedump(log_text):
    """The final posedump block: its tick= header and the p-lines after it."""
    lines = [l for l in log_text.splitlines() if l.startswith("posedump:")]
    start = max((i for i, l in enumerate(lines) if l.startswith("posedump: tick=")), default=None)
    return "\n".join(lines[start:]) + "\n" if start is not None else ""


def peek(inst, *specs):
    """tacli peek: one line per spec, `<spec> @0x<addr> = <decimal> (0x<hex>)`."""
    out = {}
    for line in tacli("peek", inst, *specs).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] in specs:
            try:
                out[parts[0]] = int(parts[-2]) if parts[-1].startswith("(0x") else int(parts[-1])
            except ValueError:
                pass
    return out


def follow(inst, utype, engine_index):
    """Park the camera on the traced unit: the pose oracle dumps the first unit
    the native pass draws, and aircraft and ships have left the spawn view by
    the time the behaviour has happened. The roster lists on-screen units only,
    so the position is read from the unit array by the engine index tacli's
    load report gave us: `main+0x14357` is the array, stride 0x118, the slot IS
    the in-game index (measured 2026-09-07: slot 1 was posedump's idx=1 unit),
    x and z are 16.16 at +0x6A and +0x72."""
    try:
        base = peek(inst, "*0x511DE8+0x14357:4").get("*0x511DE8+0x14357:4")
        if base and engine_index is not None:
            u = base + engine_index * 0x118
            sx, sz, si = f"0x{u:X}+0x6A:4", f"0x{u:X}+0x72:4", f"0x{u:X}+0xA8:2"
            r = peek(inst, sx, sz, si)
            if r.get(si) == engine_index and sx in r and sz in r:
                tacli("eye", inst, str(r[sx] >> 16), str(r[sz] >> 16))
                time.sleep(0.5)
                return
    except SystemExit:
        pass
    roster = json.loads(tacli("roster", inst, "--json"))
    for u in roster.get("units", []):
        if u.get("owner") == 0 and u.get("type", "").upper() == utype:
            x, y = u["world"]
            tacli("eye", inst, str(x), str(y))
            time.sleep(0.5)
            return
    print(f"   (no live {utype} found — camera left where it was)", flush=True)


def run_one(inst, name, utype, run_s, after_s, out):
    print(f"== {name} ({utype})", flush=True)
    tacli("arm", inst, f"cobtrace.on={utype}", "native.on=all", "posedump.on=off")
    report = tacli("scenario", "load", inst, f"cob-{name}", "--restart", "--json")
    idx = None
    try:
        for u in json.loads(report).get("units", {}).values():
            if u.get("owner") == 0 and u.get("type", "").upper() == utype:
                idx = u.get("engine_index")
    except ValueError:
        pass
    time.sleep(run_s)
    follow(inst, utype, idx)
    tacli("arm", inst, "posedump.on")
    time.sleep(after_s)
    g = gamedir(inst)
    trace = (g / "tagpu_cobtrace.log").read_text(errors="replace")
    pose = last_posedump((g / "tagpu.log").read_text(errors="replace"))
    tacli("stop", inst)
    d = out / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "cobtrace.log").write_text(trace)
    (d / "posedump.txt").write_text(pose)
    (d / "apply.json").write_text(report)
    kinds = {}
    for l in trace.splitlines():
        kinds[l[:1]] = kinds.get(l[:1], 0) + 1
    print(f"   {sum(v for k, v in kinds.items() if k != '#')} lines "
          + " ".join(f"{k}={kinds[k]}" for k in "SRXKD" if k in kinds)
          + (f", posedump {pose.count(chr(10))} lines" if pose else ", NO posedump"), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="class names to run (default: all nine)")
    ap.add_argument("--instance", default="cobfx")
    ap.add_argument("--out", default=str(ROOT / "research/notes/evidence/cobtrace"))
    ap.add_argument("--keep", action="store_true", help="leave the instance's arm files in place")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    todo = [c for c in CLASSES if not a.names or c[0] in a.names]
    unknown = set(a.names) - {c[0] for c in CLASSES}
    if unknown:
        raise SystemExit(f"unknown class {sorted(unknown)}; choose from {[c[0] for c in CLASSES]}")
    for name, utype, run_s, after_s in todo:
        run_one(a.instance, name, utype, run_s, after_s, out)
    if not a.keep:
        tacli("arm", a.instance, "cobtrace.on=off", "native.on=off", "owndraw.on=off", "posedump.on=off")
    print(f"fixtures in {out}")


if __name__ == "__main__":
    main()
