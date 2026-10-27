#!/usr/bin/env python3
"""sidepacks.py -- the wiki mockup's sidebar packs, one for each mod its selector names that a compat setup installs.

    .venv-undither/bin/python tools/wiki/sidepacks.py                 # every pack
    .venv-undither/bin/python tools/wiki/sidepacks.py stock prota     # these ones
    .venv-undither/bin/python tools/wiki/sidepacks.py --gamedir <retail TA install> [--out <folder>]

For each mod it lays out the setup's game folder as tools/compat/tacompat.py's `overlay` gives it -- the retail
install's files, then the setup's on top, a later name replacing an earlier one without case -- as symlinks in a
temporary folder, and runs `gfx.py --side-only` on it, which writes tools/wiki/mockup/gfx/side/<mod>/ (gitignored:
it is derived from the game's files). Then it loads each pack's layout into tools/wiki/mockup/sim.js's checkLayout
under node, the bound the mockup applies before it uses one.

This mapping is test tooling (spec section 2.5); the picture rule in gfx.py names no mod. `unknown` has no pack: the
mockup gives it the fixture layout.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "mockup" / "gfx"
SIM = HERE / "mockup" / "sim.js"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools" / "compat"))
import tavfs  # noqa: E402
import tacompat  # noqa: E402

# wiki.js MODS -> tools/compat/setups.json
SETUPS = {"stock": "retail", "mayhem": "mayhem-11.3.0", "prota": "prota-4.8", "twilight": "twilight-2.0b98",
          "escalation": "escalation", "tazero": "tazero-alpha5"}

# INTERIM (owner question Q-O5): what a mod's Community Patch Loader rewrites in the engine's memory at DllMain,
# before the engine reads it (the Q6 scout answer), and which the executable file therefore does not show. Each
# value is copied from the loader's resource list in the setup's dplayx.dll (MEASURED, `strings`); peek P1 reads the
# live strings in game, and this record must equal them. TA Zero and Escalation rename their folders in the
# executable file itself, which gfx.py reads.
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
            cmd = [sys.executable, str(HERE / "gfx.py"), "--side-only", "--mod", mod, "--gamedir", str(tree),
                   "--exe", str(exe), "--out", str(out_root)]
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
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
