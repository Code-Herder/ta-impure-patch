#!/usr/bin/env python3
"""roles_check -- ImpAI's threat table against the calibration runner's roles: the same rule on the
engine's memory and on the install's files.

    tools/impai/roles_check.py <log> [<log> ...] [--gamedir DIR | --setup NAME]

reads the `impai: role ...` lines the DLL writes at a level init under tagpu_impai_check.on
(tagpu/ddraw/src/tagpu_impai.c, impai_threat_build: the role classifier tasim_roles.c over the
engine filler tagpu_worldq_role_in, one line a unit type) from the logs given -- tagpu.N.log files
of one game; the last level init's lines are taken -- and runs `tasim_run roles` (the classifier
over the file filler tools/tasim/tasim_run.c, built by tools/tasim/native.py) on the install the
game played (--setup: the compat setup's folder the game played, built as the compatibility suite builds it). It fails unless both name the same unit types and agree, type by type, on the role,
the layer group, the value, the damage per tick against the ground, against air and against water,
the ground range, the role flags and the tech level, each exactly (the figures are doubles printed with 17
digits on both sides). The tech level is the classifier's walk over each builder's build options: the engine
filler's are its build menus merged over the sides the game loaded, the file filler's the same page rule over
the install's files merged over the sides its sidedata defines (tools/tasim/tasim_load.c, build_options), the
download files read by position as the engine reads them.

Exit 0 when every type agrees, 1 when one differs, 2 when the logs or the install cannot be read.
"""
import argparse
import json
import re
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools" / "tasim"))
import native  # noqa: E402

ROLE = re.compile(r"impai: role (\S+) (\S+) (\S+) value=(\S+) dps_ground=(\S+) dps_air=(\S+) "
                  r"dps_water=(\S+) range_ground=(-?\d+) flags=0x([0-9A-Fa-f]+) tech=(-?\d+)")
THREAT = re.compile(r"impai: threat ")
FLAG_NAMES = ("commander", "factory", "transport", "kamikaze", "antiweapons", "radar", "jammer", "mobile")


def engine_roles(logs):
    """{NAME: row} of the last level init's role lines; a `threat` line ends a level's block. The lines
    come in type order, and a block's first is type 0, the engine's reserved type (`None`, which no FBI
    defines and the one-type reload 0x42D1F0 skips: engine/memory.md), so it is left out."""
    levels, cur, first = [], {}, True
    for path in logs:
        for ln in Path(path).read_text(errors="replace").splitlines():
            m = ROLE.search(ln)
            if m and first:
                first = False
            elif m:
                name, role, layer, value, dg, da, dw, rg, flags, tech = m.groups()
                cur[name.upper()] = {"name": name, "role": role, "layer": layer, "value": float(value),
                                     "dps_ground": float(dg), "dps_air": float(da), "dps_water": float(dw),
                                     "range_ground": int(rg), "tech": int(tech),
                                     "flags": sorted(FLAG_NAMES[k] for k in range(8) if int(flags, 16) >> k & 1)}
            elif THREAT.search(ln):
                if cur:
                    levels.append(cur)
                cur, first = {}, True
    if cur:
        levels.append(cur)
    return levels[-1] if levels else {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("logs", nargs="+", type=Path)
    ap.add_argument("--gamedir", help="the install (default: tavfs's find_install)")
    ap.add_argument("--setup", help="a compat setup's folder (tools/compat/setups.json) instead of an install")
    a = ap.parse_args()
    if a.setup and a.gamedir:
        ap.error("--setup reads the setup's own folder: no --gamedir with it")
    try:
        eng = engine_roles(a.logs)
    except OSError as e:
        print(f"roles_check: {e}", file=sys.stderr)
        return 2
    if not eng:
        print("roles_check: no `impai: role` line: the check lever was not in place, or no level loaded")
        return 2
    rc, out, err = native.roles(a.gamedir, setup=a.setup)
    if rc:
        print(f"roles_check: tasim_run roles could not run ({rc}): {err.strip()[-300:]}")
        return 2
    files = {u["name"].upper(): u for u in json.loads(out)["units"]}
    fails = []
    for name in sorted(set(eng) - set(files)):
        fails.append(f"{name}: in the engine, not in the files")
    for name in sorted(set(files) - set(eng)):
        fails.append(f"{name}: in the files, not in the engine")
    roles = {}
    for name in sorted(set(eng) & set(files)):
        e, f = eng[name], files[name]
        f = dict(f, flags=sorted(f["flags"]))
        roles[e["role"]] = roles.get(e["role"], 0) + 1
        for key in ("role", "layer", "value", "dps_ground", "dps_air", "dps_water", "range_ground", "flags",
                    "tech"):
            if e[key] != f[key]:
                fails.append(f"{name}: {key} engine {e[key]!r} files {f[key]!r}")
    print(f"roles_check: {len(eng)} engine types, {len(files)} file types; roles "
          + ", ".join(f"{r} {n}" for r, n in sorted(roles.items())))
    for line in fails:
        print(f"  FAIL {line}")
    print(f"roles_check: {'FAIL' if fails else 'PASS'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
