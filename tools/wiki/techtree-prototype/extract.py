#!/usr/bin/env python3
"""PROTOTYPE, throwaway -- the tech-tree prototype's data, one JSON per mod, from the player's own install.

    .venv-undither/bin/python tools/wiki/techtree-prototype/extract.py [stock prota mayhem escalation ...]

Reuses tools/wiki/gfx.py's engine rules (unit_index, read_sides, menu_of: the build menus exactly as the sidebar
reads them) and tools/wiki/sidepacks.py's setup overlay for the mods. Writes tools/wiki/mockup/gfx/techtree/<mod>/
(gitignored: derived from the game's files, never committed): tree.json and pics/<unit>.png, the build pictures.
"""
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
WIKI = HERE.parent
sys.path.insert(0, str(WIKI))
sys.path.insert(0, str(WIKI.parent / "compat"))
import gfx  # noqa: E402
import sidepacks  # noqa: E402
import tavfs  # noqa: E402
import tacompat  # noqa: E402
from PIL import Image  # noqa: E402

OUT = WIKI / "mockup" / "gfx" / "techtree"
EXTRA = ("description", "maxdamage", "category", "tedclass", "weapon1", "weapon2", "weapon3", "maxvelocity",
         "turnrate", "acceleration", "canfly", "floater", "upright", "init_cloaked", "stealth", "hidedamage",
         "damagemodifier", "cloakcost", "canmove", "canattack", "canguard", "canpatrol", "builddistance",
         "explodeas", "selfdestructas", "copyright", "version")
EXTRA_SETUPS = {"escalation": "escalation", "tazero": "tazero-alpha5", "twilight": "twilight-2.0b98"}


def fbi_extra(eng, rec):
    blob = eng.vfs.read_from(rec["fbi"], rec["where"])
    ui = next(s for s in tavfs.tdf_sections(blob) if s["_name"].lower() == "unitinfo")
    return {k: ui["keys"][k].strip() for k in EXTRA if k in ui["keys"]}


def weapon_names(eng):
    """Every weapons\\*.tdf section: {id lowered: display name}."""
    out = {}
    for path, where in eng.vfs.enumerate("weapons", "*.tdf", False):
        try:
            for s in tavfs.tdf_sections(eng.vfs.read_from(path, where)):
                out.setdefault(s["_name"].lower(), {"name": s["keys"].get("name", s["_name"]),
                                                    "range": gfx.atoi(s["keys"].get("range")),
                                                    "damage": gfx.atoi(next((x["keys"].get("default") for x in s["sub"]
                                                                             if x["_name"].lower() == "damage"), None))})
        except Exception:
            continue
    return out


def extract(eng, mod):
    out = OUT / mod
    (out / "pics").mkdir(parents=True, exist_ok=True)
    report = []
    units = gfx.unit_index(eng, report)
    sides = gfx.read_sides(eng, report)
    downloads = gfx.read_downloads(eng, report)
    pal = eng.open(eng.dirs["palettes"], eng.dirs["palette"], "PAL")[0]
    rgb = bytes(b for i in range(256) for b in pal[i * 4:i * 4 + 3])
    weapons = weapon_names(eng)
    cache, rows, tree_sides = {}, {}, []
    for s in sides:
        root = s["commander"].lower()
        if root not in units:
            continue
        menu = lambda t, p=s["prefix"]: [x for x in gfx.menu_of(eng, units, downloads, t, p, cache) if x in units]
        order, depth = gfx.side_walk(root, menu)
        tree_sides.append({"name": s["name"], "prefix": s["prefix"], "commander": root, "order": order,
                           "menus": {t: menu(t) for t in order if menu(t)}})
        for t in order:
            if t in rows:
                rows[t]["sides"].append(s["name"])
                continue
            u = units[t]
            x = fbi_extra(eng, u)
            ws = [weapons.get(x[k].lower(), {"name": x[k]}) | {"id": x[k]} for k in ("weapon1", "weapon2", "weapon3")
                  if x.get(k)]
            rows[t] = {"name": u["name"], "title": u["title"], "description": x.get("description", ""),
                       "sides": [s["name"]], "depth": depth[t], "structure": gfx.is_structure(u),
                       "mobile": gfx.is_mobile(u), "builder": gfx.is_builder(u), "canfly": bool(u["canfly"]),
                       "water": u["minwaterdepth"] is not None and u["minwaterdepth"] > 0,
                       "metal": u["buildcostmetal"], "energy": u["buildcostenergy"], "buildtime": u["buildtime"],
                       "health": gfx.atoi(x.get("maxdamage")), "speed": u["maxvelocity"],
                       "sight": u["sightdistance"], "radar": u["radardistance"], "workertime": u["workertime"],
                       "energymake": gfx.net_energy(u), "metalmake": u["metalmake"], "extracts": u["extractsmetal"],
                       "storage": [u["metalstorage"], u["energystorage"]], "category": x.get("category", ""),
                       "tedclass": x.get("tedclass", ""), "weapons": ws, "object": u["object"],
                       "cloaked": x.get("init_cloaked") == "1", "stealth": x.get("stealth") == "1",
                       "hidedamage": x.get("hidedamage") == "1", "damagemodifier": x.get("damagemodifier"),
                       "fp": [u["fx"], u["fz"]], "pic": None}
            hit = eng.load(eng.dirs["unitpics"], u["name"], "PCX")
            if hit:
                try:
                    w, h, px = gfx.read_pcx(hit[0])
                    im = Image.frombytes("P", (w, h), px)
                    im.putpalette(rgb)
                    im.convert("RGB").save(out / "pics" / f"{t}.png")
                    rows[t]["pic"] = f"pics/{t}.png"
                except ValueError:
                    pass
    (out / "tree.json").write_text(json.dumps({"mod": mod, "sides": tree_sides, "units": rows}, indent=0))
    # the 3D pack: every unit's model in tascene's format with its COB script (gfx.py write_units, owner 0), which
    # tools/wiki/serve.py's COB host opens by this folder's path under tools/wiki/mockup/gfx/
    report = []
    meshes = gfx.write_units(out / "pack", eng, {}, report, [rows[k]["name"] for k in rows])
    (out / "pack" / "pal.bin").write_bytes(bytes(b for i in range(256) for b in (*pal[i * 4:i * 4 + 3], 255)))
    (out / "pack" / "scene.json").write_text(json.dumps({"format": "ta-scene/1", "units": meshes, "report": report}))
    print(f"{mod}: {len(tree_sides)} sides, {len(rows)} units, "
          f"{sum(1 for r in rows.values() if r['pic'])} pictures, {len(meshes['meshes'])} models "
          f"({len(meshes['refused'])} refused) -> {out}")


def main(mods):
    retail = tavfs.find_install(None)
    by_name = {s["name"]: s for s in tacompat.SETUPS}
    setups = sidepacks.SETUPS | EXTRA_SETUPS
    for mod in mods or ["stock"]:
        if mod == "stock":
            extract(gfx.Engine(retail), mod)
            continue
        setup = by_name.get(setups.get(mod))
        if setup is None or any(tacompat.fixture_problems(a["fixture"]) for a in setup["add"]):
            print(f"{mod}: no setup or its fixtures are missing; skipped")
            continue
        with tempfile.TemporaryDirectory(prefix="techtree-") as tmp:
            tree = Path(tmp) / "game"
            sidepacks.setup_tree(setup, retail, tree)
            rec = sidepacks.RECORDS.get(mod, {})
            eng = gfx.Engine(tree, sidepacks.child(tree, tavfs.EXE_NAME), rec.get("folders"), rec.get("archive"))
            extract(eng, mod)


if __name__ == "__main__":
    main(sys.argv[1:])
