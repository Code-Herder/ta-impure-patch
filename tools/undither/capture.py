#!/usr/bin/env python3
"""Capture one 1080p engine screenshot per TA map environment.

The front-end menus drop keys, so navigation is a state machine: screenshot,
classify the screen against reference fingerprints, send the key that screen
needs, repeat. Once in-game we let the AI build for a while, park the camera on
a world position that has terrain under it, and take the 8bpp surface shot.

    python3 tools/undither/capture.py --out research/notes/assets/undither/shots
"""
import argparse, json, re, subprocess, sys, time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
TACLI = ROOT / "tools" / "tacli"
INST = "undither"

# (environment, map name, expansion) — map name must match the .ota missionname,
# which is what the SkirmishMap registry value is compared against.
MAPS = [
    ("Green planet", "Plains and Passes",         "Core Contingency"),
    ("Desert",       "Painted Desert",            "Total Annihilation"),
    ("Archipelago",  "East Indeez",               "Core Contingency"),
    ("Lava",         "Red Hot Lava",              "Total Annihilation"),
    ("Metal",        "Core Prime Industrial Area","Core Contingency"),
    ("Lunar",        "Moon Quartet",              "Core Contingency"),
    ("Ice",          "Polar Range",               "Core Contingency"),
    ("Red Planet",   "Red River",                 "Core Contingency"),
    ("Slate",        "Temblorian Mist",           "Core Contingency"),
    ("Lush",         "Lusch Puppy",               "Core Contingency"),
    ("Crystal",      "Crystal Maze",              "Core Contingency"),
    ("Acid",         "Acid Pools",                "Core Contingency"),
    ("Water World",  "Brain Coral",               "Core Contingency"),
    ("Wet Desert",   "Lake Shore",                "Core Contingency"),
    ("Urban",        "Assault on Suburbia",       "Core Contingency"),
]

REFS = ROOT / "tools" / "undither" / "screens"      # main.png / single.png / setup.png
SCREEN_KEY = {"main": "space", "single": "s", "setup": "return"}


def tacli(*args, **kw):
    return subprocess.run([str(TACLI), *args], capture_output=True, text=True, **kw)


def fingerprint(path):
    im = Image.open(path).convert("L").resize((64, 48), Image.BOX)
    return np.asarray(im, dtype=np.float32)


def classify(path, refs):
    fp = fingerprint(path)
    if fp.shape != (48, 64):
        return None
    best, score = None, 1e9
    for name, ref in refs.items():
        d = float(np.abs(fp - ref).mean())
        if d < score:
            best, score = name, d
    return best if score < 18.0 else None


def shot(dest):
    r = tacli("shot", INST, "-o", str(dest))
    return dest.exists() and r.returncode == 0


# the "Current Map" readout on the skirmish setup screen, in 640x480 front-end px
MAPFIELD = (44, 374, 262, 396)


def navigate_to_game(tmp, refs, mapfield_dest=None, tries=25):
    """Walk main menu -> single player -> skirmish setup -> Start."""
    for i in range(tries):
        if not shot(tmp):
            time.sleep(1.0)
            continue
        # in-game frames are the game resolution, the front end is always 640x480
        if Image.open(tmp).size != (640, 480):
            return True
        screen = classify(tmp, refs)
        if screen is None:
            time.sleep(1.0)
            continue
        if screen == "setup" and mapfield_dest is not None:
            # proof the registry map name took, kept for the verification strip
            Image.open(tmp).convert("RGB").crop(MAPFIELD).save(mapfield_dest)
        tacli("keys", INST, SCREEN_KEY[screen])
        time.sleep(1.6)
    return False


def local_player():
    """`me=N` only appears on the `units:` log line, not in roster --json."""
    out = tacli("log", INST, "-g", "units:").stdout
    hits = re.findall(r"me=(\d+)", out)
    return int(hits[-1]) if hits else 0


def own_commander(roster, me):
    """The local player's commander: own unit whose type ends in COM."""
    units = roster.get("units", [])
    mine = [u for u in units if u.get("owner") == me]
    for pool in (mine, units):
        for u in pool:
            if u.get("type", "").upper().endswith("COM"):
                return u
        if pool:
            return pool[0]
    return None


def capture_map(env, mapname, out_dir, build_seconds, refs):
    slug = env.lower().replace(" ", "-")
    tmp = Path("/tmp/undither_nav.png")
    tacli("stop", INST)
    time.sleep(1.0)
    r = tacli("launch", INST, "--res", "1920x1080", "--map", mapname,
              "--player", "1:0", "--player", "2:2:1:2", "--player", "3:0",
              "--los", "0", "--mapping", "0", "--json")
    if r.returncode != 0:
        return {"env": env, "map": mapname, "error": r.stderr.strip()[:300]}
    time.sleep(2.0)
    tacli("keys", INST, "tab")
    time.sleep(0.6)
    if not navigate_to_game(tmp, refs, out_dir / f"{slug}-mapfield.png"):
        return {"env": env, "map": mapname, "error": "menu navigation failed"}

    w = tacli("wait", INST, r"alive=[1-9]", "--timeout", "180")
    if w.returncode != 0:
        return {"env": env, "map": mapname, "error": "game never started"}

    tacli("keys", INST, "plus", "plus", "plus")          # speed the build up
    time.sleep(build_seconds)

    # the engine opens the view centred on the local player's commander and
    # nothing scrolls it while we send no input, so we just read the eye back
    eye = None
    hits = re.findall(r"eye=\((-?\d+),(-?\d+)\)", tacli("log", INST, "-g", "units:").stdout)
    if hits:
        eye = [int(hits[-1][0]), int(hits[-1][1])]

    dest = out_dir / f"{slug}.png"
    ok = shot(dest)
    tacli("stop", INST)
    if not ok:
        return {"env": env, "map": mapname, "error": "screenshot failed"}
    im = Image.open(dest)
    return {"env": env, "map": mapname, "file": dest.name, "size": list(im.size),
            "mode": im.mode, "eye": eye}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="research/notes/assets/undither/shots")
    ap.add_argument("--only", nargs="*", help="environment names to capture")
    ap.add_argument("--build-seconds", type=float, default=40.0)
    args = ap.parse_args()

    refs = {p.stem: fingerprint(p) for p in REFS.glob("*.png")}
    if not refs:
        sys.exit(f"no reference screens in {REFS}")
    out_dir = ROOT / args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    todo = [m for m in MAPS if not args.only or m[0] in args.only]
    results = []
    for env, mapname, expansion in todo:
        print(f"--- {env}: {mapname}", flush=True)
        r = capture_map(env, mapname, out_dir, args.build_seconds, refs)
        r["expansion"] = expansion
        print("   ", json.dumps(r), flush=True)
        results.append(r)

    manifest = out_dir / "manifest.json"
    old = json.loads(manifest.read_text()) if manifest.exists() else []
    byenv = {r["env"]: r for r in old}
    byenv.update({r["env"]: r for r in results})
    order = [m[0] for m in MAPS]
    manifest.write_text(json.dumps(sorted(byenv.values(), key=lambda r: order.index(r["env"])), indent=2))
    print(f"\nwrote {manifest}")


if __name__ == "__main__":
    main()
