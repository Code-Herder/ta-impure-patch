#!/usr/bin/env python3
"""sidepacks.py -- the wiki mockup's sidebar packs, one for each mod its selector names that a compat setup installs.

    .venv-undither/bin/python tools/wiki/sidepacks.py                 # every pack
    .venv-undither/bin/python tools/wiki/sidepacks.py stock prota     # these ones
    .venv-undither/bin/python tools/wiki/sidepacks.py --gamedir <retail TA install> [--out <folder>]

For each mod it lays out the setup's game folder as tools/compat/tacompat.py's `overlay` gives it -- the retail
install's files, then the setup's on top, a later name replacing an earlier one without case -- as symlinks in a
temporary folder, and runs `gfx.py --side-only --stages` on it, which writes tools/wiki/mockup/gfx/side/<mod>/ and
the mod's entry stage packs in its stage/ (gitignored: they are derived from the game's files). Then it loads each
pack's layout into tools/wiki/mockup/sim.js's checkLayout under node, the bound the mockup applies before it uses
one, and on the stock pack checks the cast rule's stock identity (`stock_identity`): the rule is general, and this is
what pins it.

This mapping is test tooling; the picture rule and the cast rule in gfx.py name no mod. `unknown`
has no pack: the mockup gives it the fixture layout.
"""

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "mockup" / "gfx"
ENTRIES = REPO / "art" / "wiki" / "entries"
SIM = HERE / "mockup" / "sim.js"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "compat"))
import tavfs  # noqa: E402
import tacompat  # noqa: E402

# wiki.js MODS -> tools/compat/setups.json
SETUPS = {"stock": "retail", "mayhem": "mayhem-11.3.0", "prota": "prota-4.8", "twilight": "twilight-2.0b98",
          "escalation": "escalation", "tazero": "tazero-alpha5"}

# What a mod's Community Patch Loader rewrites in the engine's memory at DllMain, before the engine reads it (the Q6
# scout answer), and which the executable file therefore does not show (owner question Q-O5). Each value is copied
# from the loader's resource list in the setup's dplayx.dll (MEASURED, `strings`). CHECKED 2026-10-27 against peek
# P1, the live strings at 0x502820, 0x503360, 0x505FE4 and 0x503730 read at the main menu of each setup: Mayhem
# guiM, gamedata, unitpicM, downloadsM; ProTA guiP, gamedatP, unitpicsP, downloadP; equal to this record, and
# commongui (0x50925C) unrenamed in both. P1 did not read the archive pattern at 0x5028CC, so `archive` is still the
# loader's list alone. TA Zero (ZGui, ZGameDat, ZUnitPic, ZBuildMenu) and Escalation (guiE, gamedatE, unitpicE,
# downloadsE) rename their folders in the executable file itself, which gfx.py reads; P1 read the same names live.
RECORDS = {
    "mayhem": {
        "folders": {"gui": "guiM",              # [00100E23] patch=4D: 0x502823, "guis" -> "guiM"
                    "unitpics": "unitpicM",     # [001045EB] patch=4D: 0x505FEB, "unitpics" -> "unitpicM"
                    "download": "downloadsM"},  # [Settings] DownloadPath=downloadsM, written to 0x503730
        # [Settings] Gp3FileName=may%s.GP3 at 0x5028CC, and [00100ED8] patch=68 65 6D turns its "31" into "hem"
        "archive": "mayhem.GP3",
    },
    "prota": {
        "folders": {"gui": "guiP",              # [00100E20] patch=67 75 69 50: 0x502820 "guiP"
                    "unitpics": "unitpicsP",    # [001045E4] patch=75 6E 69 74 70 69 63 73 50: 0x505FE4 "unitpicsP"
                    "gamedata": "gamedatP",     # [00101960] patch=67 61 6D 65 64 61 74 50: 0x503360 "gamedatP"
                    "download": "downloadP"},   # [Settings] DownloadPath=downloadP, written to 0x503730
        "archive": "ProTA.gp3",                 # [Settings] Gp3FileName=ProTA.gp3, written to 0x5028CC
    },
}


def child(folder: Path, name: str):
    """The entry of `folder` named `name` without case, or None."""
    low = name.lower()
    return next((c for c in folder.iterdir() if c.name.lower() == low), None)


def mirror(src: Path, dst: Path):
    """`src`'s tree under `dst`: real folders, every file a symlink to the original."""
    dst.mkdir(parents=True, exist_ok=True)
    for e in src.iterdir():
        if e.is_dir():
            mirror(e, dst / e.name)
        else:
            (dst / e.name).symlink_to(e.resolve())


def place(tree: Path, dest: str, src: Path):
    """The setup's file at `dest` (a path in the game folder), each component matched without case as Windows does;
    an existing file of that name is replaced."""
    parts = [p for p in dest.replace("\\", "/").split("/") if p]
    cur = tree
    for part in parts[:-1]:
        hit = child(cur, part)
        if hit is None or not hit.is_dir():
            hit = cur / part
            hit.mkdir()
        cur = hit
    old = child(cur, parts[-1])
    if old is not None:
        if old.is_dir() and not old.is_symlink():
            shutil.rmtree(old)
        else:
            old.unlink()
    (cur / parts[-1]).symlink_to(src.resolve())


def setup_tree(setup, retail: Path, tree: Path):
    mirror(retail, tree)
    for dest, src in tacompat.overlay(setup):
        place(tree, dest, src)


def check_layout(pack: Path):
    """sim.js's checkLayout on the pack's layout under node: (ok, message)."""
    node = shutil.which("node")
    if node is None:
        return False, "node is not installed: the layout was not checked"
    js = ("const S = require(process.argv[1]); const p = JSON.parse(require('fs').readFileSync(process.argv[2]));"
          "try { S.checkLayout(p.layout); console.log('checkLayout: ok'); }"
          "catch (e) { console.log(e.message); process.exit(1); }")
    r = subprocess.run([node, "-e", js, str(SIM), str(pack)], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


# An entry unit's figure and the value an absent key stands for: what the bound entry takes from the casting. The
# casting carries every FBI's EnergyMake (a construction kbot's 8 among them), and the sims read energyMake only as a
# geothermal plant's strength, so it is compared where the entry states it and nowhere else.
FIGURES = (("footprint", None), ("buildTime", 0), ("workerTime", 0), ("buildDistance", 0), ("maxVelocity", 0),
           ("canstop", 0), ("canreclamate", 0), ("activatewhenbuilt", 0), ("extracts", 0), ("vent", False))


def stock_identity(pack):
    """The stock pack's first side ([SIDE0], whose units the wiki's entries author) cast for cast: every entry unit's
    `cast` is bound to that unit's own type, with the entry's figures and name, and builds the casts the entry says it
    builds. Binding stock's entries to stock's casting then changes nothing, which is the check that keeps the
    general rule honest (gfx.py `side_casting`). Returns the differences."""
    casting = pack["casting"][0]["casts"]
    bad = []
    for f in sorted(ENTRIES.glob("*.json")):
        e = json.loads(f.read_text())
        units = e["units"]
        used = {v.get("cast") for v in units.values()}
        for t, u in units.items():
            at, c = f"{e['id']} {t}", u.get("cast")
            if c not in casting:
                bad.append(f"{at}: its cast {c!r} is none of {', '.join(casting)}")
                continue
            got = casting[c]
            if got is None:
                bad.append(f"{at}: the stock casting leaves {c} unbound")
                continue
            if got["type"].lower() != t.lower():
                bad.append(f"{at}: the stock {c} is {got['type']}")
                continue
            figures = FIGURES + ((("energyMake", None),) if "energyMake" in u else ())
            for key, absent in figures:
                if got[key] != u.get(key, absent):
                    bad.append(f"{at}: {key} {got[key]!r} in the casting, {u.get(key, absent)!r} in the entry")
            # an entry's `vent` without `g` makes every footprint cell G
            fx, fz = got["footprint"]
            every = [[x, z] for x in range(fx) for z in range(fz)]
            g = sorted(map(list, u["g"])) if "g" in u else every if u.get("vent") else []
            if sorted(got["g"]) != g:
                bad.append(f"{at}: G cells {got['g']} in the casting, {g} in the entry")
            if got["title"] != u.get("name"):
                bad.append(f"{at}: named {got['title']!r} in the casting, {u.get('name')!r} in the entry")
            have = sorted(d for d in got["builds"] if d in used)
            want = sorted(units[b]["cast"] for b in u.get("builds", []) if b in units and "cast" in units[b])
            if have != want:
                bad.append(f"{at}: builds {have} of the entry's casts in the casting, {want} in the entry")
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(prog="sidepacks.py", description=__doc__.split("\n")[0])
    ap.add_argument("mods", nargs="*", help=f"the packs to build (default: all of {', '.join(SETUPS)})")
    ap.add_argument("--gamedir", help="the retail install the setups lay their files over (default: TA3DO_GAMEDIR, "
                                      "else Steam's default library)")
    ap.add_argument("--out", help=f"write here instead of {OUT.relative_to(REPO)}/")
    args = ap.parse_args(argv)
    mods = args.mods or list(SETUPS)
    unknown = [m for m in mods if m not in SETUPS]
    if unknown:
        print(f"sidepacks.py: no setup for {', '.join(unknown)} (the mods: {', '.join(SETUPS)})", file=sys.stderr)
        return 2
    retail = tavfs.find_install(args.gamedir)
    by_name = {s["name"]: s for s in tacompat.SETUPS}
    out_root = Path(args.out) if args.out else OUT
    failed = 0
    for mod in mods:
        setup = by_name[SETUPS[mod]]
        missing = sorted({a["fixture"] for a in setup["add"] if tacompat.fixture_problems(a["fixture"])})
        if missing:
            print(f"{mod}: setup {setup['name']} needs fixtures {', '.join(missing)} (tacompat.py fetch); skipped")
            failed += 1
            continue
        with tempfile.TemporaryDirectory(prefix="sidepack-") as tmp:
            tree = Path(tmp) / "game"
            setup_tree(setup, retail, tree)
            exe = child(tree, tavfs.EXE_NAME)
            cmd = [sys.executable, str(HERE / "gfx.py"), "--side-only", "--stages", "--mod", mod,
                   "--gamedir", str(tree), "--exe", str(exe), "--out", str(out_root)]
            rec = RECORDS.get(mod)
            if rec:
                cmd += ["--folders", ",".join(f"{k}={v}" for k, v in rec["folders"].items()), "--archive",
                        rec["archive"]]
            r = subprocess.run(cmd, capture_output=True, text=True)
        print(f"{mod} ({setup['name']}): " + (r.stdout + r.stderr).strip().replace(str(out_root), "gfx"))
        if r.returncode != 0:
            failed += 1
            continue
        ok, msg = check_layout(out_root / "side" / mod / "side.json")
        print(f"  {msg}")
        failed += not ok
        if mod == "stock":
            bad = stock_identity(json.loads((out_root / "side" / mod / "side.json").read_text()))
            print("  stock identity: " + ("ok" if not bad else "FAILED\n    " + "\n    ".join(bad)))
            failed += bool(bad)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
