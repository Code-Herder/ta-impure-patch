#!/usr/bin/env python3
"""cobcorpus.py -- the COB corpus gate: every unit script of every compatibility setup through the wiki's COB host.

    .venv-undither/bin/python tools/wiki/cobcorpus.py [--gamedir <retail TA install>] [--json FILE] [SETUP ...]

The rule is the plan's decision 9 (research/notes/ingame-wiki-cob-plan.md): every compatibility setup's COB corpus
runs through the core as a gate, and a refusal of a script the real engine runs is a core bug to fix, never a
quarantine to accept. For each setup of tools/compat/setups.json (or the SETUPs named):

1. **Its files as the engine mounts them.** The setup's game folder as tools/compat/tacompat.py's `overlay` lays it
   (tools/wiki/sidepacks.py `setup_tree`: the retail install, the setup's files on top), read through tools/wiki/
   gfx.py's `Engine` over tools/wiki/tavfs.py: loose first, then the archives in the executable's mount order. A
   setup whose Community Patch Loader renames the main archive in memory takes the archive sidepacks.py's `RECORDS`
   gives the setup that ships the same dplayx.dll, since the loader's list is that DLL's.
2. **Its unit types**: gfx.py `unit_index`, every `units\\*.fbi` by its UnitName, the first file listed winning a
   name; each resolved as the wiki's asset load resolves it (gfx.py `resolve_unit`: `units\\<UnitName>.FBI` by path,
   its Objectname's `objects3d\\<name>.3DO`, `scripts\\<UnitName>.COB`), the posed order and each primitive's visible
   flag from gfx.py `posed_order`, as serve.py hands them to the host.
3. **Each type through the host alone** (tagpu/ddraw/src/tagpu_wikicob.c, one host a type, bound as serve.py binds
   it, the library tools/tacob builds): one unit spawned (Create run-now), and over TICKS slices the calls of the
   plan's calls table, each through the entry the engine uses: an extractor's SetSpeed with one word,
   ActivateWhenBuilt's Activate, a mobile unit's StartMoving and MoveRate1 then StopMoving, a mobile builder's
   StartBuilding(0) and StopBuilding. Two calls go past what the wiki makes today, because the engine makes them and
   a type the gate never starts them on would pass untested: the state setter's Deactivate and Activate on every
   type (a player's on and off), and a structure builder's StartBuilding and StopBuilding with no word (the state
   setter's, 0x48B14E, 0x48B169). The note (`tagpu_wikicob_note`) of a quarantined type is its refusal.

What is not a host refusal is listed apart, never counted as one: a type whose FBI, model or script cannot be loaded
at all (the engine's level load would stop, or the type has no script to run), and a model past WIKICOB_PIECES
primitives, which the host's bound, not the core, refuses.

Setups whose types resolve to the same scripts, posed orders and calls -- most differ only by TADR's or the loader's
files -- are run once, and the summary says which setup's run each one shares.

A setup whose compat fixtures are not installed is named "not installed" and skipped, never silently. Exit 0 when no
setup's script is refused, 1 naming each refused type (setup, type, note), 2 when the gate cannot run (an unknown
setup, no retail install)."""
import argparse
import ctypes as C
import hashlib
import json
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "compat"))
import gfx  # noqa: E402
import serve  # noqa: E402
import sidepacks  # noqa: E402
import tacompat  # noqa: E402
import tavfs  # noqa: E402

TICKS = 600                 # twenty seconds of game time at 30 slices a second: every call below and its animation
PIECES = 256                # WIKICOB_PIECES
NOW, LATER, ARGS = 0, 1, 2  # WIKICOB_NOW, _LATER, _ARGS
SPOT = (1024 << 16, 1024 << 16)     # the unit's centre in 16.16 world units, inside any map
METAL_CELL = 3              # the metal of each footprint cell the extractor's SetSpeed sums (cell metal + 1)

# The wiki's calls over the run, each at its tick, before that tick's slice: (tick, what, who). `who` names the kind
# of type the wiki makes the call for; the calls table of the plan gives each one's entry.
TIMELINE = (
    (15, "activate", "any"),            # the state setter's Activate: nothing when ACTIVATION is already set
    (30, "move", "mobile"),             # StartMoving then MoveRate1, run-now (0x43DAF2)
    (60, "build", "builder"),
    (240, "unbuild", "builder"),
    (270, "stop", "mobile"),            # StopMoving, run-now (0x43DB27)
    (300, "deactivate", "any"),
    (360, "activate", "any"),
    (390, "build", "builder"),
    (510, "unbuild", "builder"),
)


def dll_md5(setup):
    for dest, src in tacompat.overlay(setup):
        if dest.lower() == "dplayx.dll":
            return tacompat.md5_file(src)
    return None


def loader_archive(setup):
    """The main archive a setup's Community Patch Loader mounts instead of the executable's pattern, or None."""
    mine = dll_md5(setup)
    if mine is None:
        return None
    by_name = {s["name"]: s for s in tacompat.SETUPS}
    for mod, rec in sidepacks.RECORDS.items():
        if dll_md5(by_name[sidepacks.SETUPS[mod]]) == mine:
            return rec["archive"]
    return None


def extractor_speed(cells):
    """SetSpeed's word for an extractor on `cells` footprint cells of METAL_CELL metal: the sum of cell metal + 1,
    kept as a sign-extended 16-bit word (0x437866..0x437902; sim.js `extractorSpeed`)."""
    v = cells * (METAL_CELL + 1) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def kinds(rec):
    """Which of the wiki's calls the type gets, from its FBI's figures (gfx.py `unit_record`)."""
    mobile = rec["bmcode"] != 0 and rec["maxvelocity"] > 0
    return {"mobile": mobile, "builder": gfx.is_builder(rec), "extractor": rec["extractsmetal"] > 0,
            "awb": bool(rec["activatewhenbuilt"]), "cells": max(1, rec["fx"]) * max(1, rec["fz"])}


def resolve(eng):
    """(types, apart): types [(name, cob, visible, kinds)] the host runs, apart {name: why} those it cannot be given."""
    index = gfx.unit_index(eng, [])
    types, apart = [], {}
    for key in sorted(index):
        rec = index[key]
        name = rec["name"]
        try:
            _label, root, _hidden, _uncached, _facts = gfx.resolve_unit(eng, name, [])
        except gfx.Miss as e:
            apart[name] = f"cannot be loaded: {e}"
            continue
        script = eng.open(eng.dirs["scripts"], name, "COB")
        if script is None or not script[0]:
            apart[name] = f"no script: no {eng.dirs['scripts']}\\{name}.COB"
            continue
        posed = gfx.posed_order(root, script[0])
        if len(posed) > PIECES:
            apart[name] = f"the host's bound: the model has {len(posed)} primitives, WIKICOB_PIECES is {PIECES}"
            continue
        types.append((name, script[0], bytes(int(v) for _k, v in posed), kinds(rec)))
    return types, apart


def corpus_key(types, apart):
    h = hashlib.sha256()
    for name, cob, vis, k in types:
        h.update(json.dumps([name.lower(), k], sort_keys=True).encode() + cob + b"\0" + vis + b"\0")
    h.update(json.dumps(sorted((n.lower(), w) for n, w in apart.items())).encode())
    return h.hexdigest()


class Runner:
    def __init__(self):
        self.lib = serve.library()
        self.fact = (serve.Fact * 1)(serve.Fact(1, 0, SPOT[0], SPOT[1], 0.0, 100))

    def count(self, r):
        """Tally one call's answer: 1 started, 0 nothing started (the script lacks it, or the state setter's value
        was already set), -1 faulted."""
        self.made += 1
        self.started += r == 1
        return r

    def call(self, h, script, how, args=()):
        a = (C.c_int32 * 4)(*args)
        return self.count(self.lib.tagpu_wikicob_call(h, 1, script.encode(), how, len(args), a, self.fact, 1))

    def activate(self, h, on):
        return self.count(self.lib.tagpu_wikicob_activate(h, 1, on))

    def run(self, cob, visible, k):
        """(note, calls made, calls started) of the type's run: the note its quarantine left, "" when its script
        ran through."""
        self.made = self.started = 0
        arr = (serve.Type * 1)()
        arr[0].cob = C.cast(C.c_char_p(cob), C.c_void_p)
        arr[0].ncob = len(cob)
        arr[0].pieces = len(visible)
        arr[0].visible = C.cast(C.c_char_p(visible), C.c_void_p)
        err = C.create_string_buffer(256)
        h = self.lib.tagpu_wikicob_new(1, arr, 1, err, 256)
        if not h:
            raise RuntimeError(err.value.decode("latin-1"))
        try:
            if self.lib.tagpu_wikicob_spawn(h, 0, self.fact, 1) != 1:
                raise RuntimeError("the host gave the unit no handle")
            if k["extractor"]:
                self.call(h, "SetSpeed", ARGS, [extractor_speed(k["cells"])])
            if k["awb"]:
                self.activate(h, 1)
            events = iter(TIMELINE)
            nxt = next(events, None)
            for tick in range(TICKS):
                while nxt and nxt[0] == tick:
                    self.event(h, nxt[1], nxt[2], k)
                    nxt = next(events, None)
                self.lib.tagpu_wikicob_slice(h, self.fact, 1)
            return self.lib.tagpu_wikicob_note(h, 0).decode("latin-1"), self.made, self.started
        finally:
            self.lib.tagpu_wikicob_free(h)

    def event(self, h, what, who, k):
        if who != "any" and not k[who]:
            return
        if what in ("activate", "deactivate"):
            self.activate(h, what == "activate")
        elif what == "move":
            self.call(h, "StartMoving", NOW)
            self.call(h, "MoveRate1", NOW)
        elif what == "stop":
            self.call(h, "StopMoving", NOW)
        elif what == "build":
            # a mobile builder's StartBuilding carries the target's bearing (0x4385C7), a factory's none (0x48B14E)
            if k["mobile"]:
                self.call(h, "StartBuilding", ARGS, [0])
            else:
                self.call(h, "StartBuilding", LATER)
        elif what == "unbuild":
            self.call(h, "StopBuilding", LATER)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="cobcorpus.py", description=__doc__.split("\n")[0])
    ap.add_argument("setups", nargs="*", help="the setups to run (default: every one of tools/compat/setups.json)")
    ap.add_argument("--gamedir", help="the retail install the setups lay their files over (default: TA3DO_GAMEDIR, "
                                      "else Steam's default library)")
    ap.add_argument("--json", help="also write every setup's types, refusals and the types set apart to this file")
    args = ap.parse_args(argv)
    by_name = {s["name"]: s for s in tacompat.SETUPS}
    unknown = [n for n in args.setups if n not in by_name]
    if unknown:
        print(f"cobcorpus.py: no setup {', '.join(unknown)} (tacompat.py list)", file=sys.stderr)
        return 2
    retail = tavfs.find_install(args.gamedir)
    if not retail.is_dir() or not sidepacks.child(retail, tavfs.EXE_NAME):
        print(f"cobcorpus.py: no retail install with {tavfs.EXE_NAME} at {retail} (--gamedir)", file=sys.stderr)
        return 2
    runner = Runner()
    runs = {}                   # corpus key -> (the setup that ran it, its result)
    out, refused, ran = [], [], 0
    t0 = time.monotonic()
    for name in args.setups or [s["name"] for s in tacompat.SETUPS]:
        setup = by_name[name]
        missing = sorted({a["fixture"] for a in setup["add"] if tacompat.fixture_problems(a["fixture"])})
        if missing:
            print(f"{name}: not installed: fixtures {', '.join(missing)} missing (tacompat.py fetch); skipped")
            out.append({"setup": name, "skipped": f"fixtures {', '.join(missing)} missing"})
            continue
        t = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="cobcorpus-") as tmp:
            tree = Path(tmp) / "game"
            sidepacks.setup_tree(setup, retail, tree)
            exe = sidepacks.child(tree, tavfs.EXE_NAME)
            eng = gfx.Engine(tree, exe, None, loader_archive(setup))
            types, apart = resolve(eng)
        key = corpus_key(types, apart)
        if key in runs:
            first, res = runs[key]
            shared = first
        else:
            got = {n: runner.run(cob, vis, k) for n, cob, vis, k in types}
            res = {"types": len(types) + len(apart), "scripts": len(types),
                   "calls": sum(g[1] for g in got.values()), "started": sum(g[2] for g in got.values()),
                   "refused": {n: g[0] for n, g in got.items() if g[0]}, "apart": apart}
            runs[key] = (name, res)
            shared = None
            ran += 1
        for n, note in sorted(res["refused"].items()):
            refused.append((name, n, note))
        print(f"{name}: {res['types']} types, {res['scripts']} scripts through the host, "
              f"{res['started']} of {res['calls']} calls started, {len(res['refused'])} refused, "
              f"{len(res['apart'])} set apart" + (f" (the same scripts as {shared}, run once)" if shared else "")
              + f" [{time.monotonic() - t:.1f} s]")
        for n, note in sorted(res["refused"].items()):
            print(f"  REFUSED {n}: {note}")
        for n, why in sorted(res["apart"].items()):
            print(f"  apart {n}: {why}")
        out.append({"setup": name, "shares": shared, **res})
    print(f"cobcorpus.py: {len(out)} setups, {ran} distinct corpora run, {len(refused)} refusals "
          f"in {time.monotonic() - t0:.1f} s")
    if args.json:
        Path(args.json).write_text(json.dumps(out, indent=1) + "\n")
    sys.stdout.flush()
    for s, n, note in refused:
        print(f"REFUSED {s} {n}: {note}", file=sys.stderr)
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())
