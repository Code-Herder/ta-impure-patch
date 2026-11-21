#!/usr/bin/env python3
"""mp_agree -- do the two machines of a network game agree on the battle? Two peers, 1499 units each,
fought for a few minutes, paused, and each peer's units compared with the other's.

    tools/mp_agree.py --out DIR [--dll PATH] [--join-dll PATH] [--until T] [--tol W] [--engine-paths] [--gpu]

One two-player game on Town & Country (tools/impai/twopeers.py: the run's two instances on a private
display and a DirectPlay port leased for the game, MultiCommanderDeath 0), the 1500 unit limit; the host
applies scenarios/limits-tier2-p0.json and the joiner limits-tier2-p1.json, each its own army of Peewees,
Hammers and Rockos ordered onto the map's centre. At the host's GameTime T (5400 by default: three
minutes at 30 ticks a second) the host presses the game's own Pause, which pauses both peers, and each
peer's roster (`tacli roster`, the game thread's log of every live unit) is read twice, a dump apart;
the two reads of a peer must be equal, which shows the rosters were taken paused.

WHAT IT JUDGES, unit by unit, matched by engine slot (the same slot on every peer):
  - the same units are alive on both peers, each of the same type;
  - each unit belongs to the same player on both: a peer numbers the players from its own seat (its
    own units are owner 0), so the owners must map one to one between the peers, and every unit is
    its own peer's (owner 0) on exactly one of them;
  - each unit stands where its owner's copy stands, within W world units (128 by default). A remote
    copy is where its owner last reported it and the two pauses land a few ticks apart, so a unit
    moving at the pause can be off by its speed times those ticks: 40 ticks of the armies' fastest
    kbot is under 100. A stationary unit agrees exactly; the report gives the share that does.
--engine-paths writes tagpu_pathverify.on into both game folders, which puts the engine's own path scheduler
back in place of ours (our search runs beside it as an oracle and writes nothing): the same game with
stock pathfinding, for a comparison. Both peers log the pathfinder's lines (tagpu_pathlog.on).
Not judged: hit points, which a peer holds for its own units only (a remote copy reads 0), and kill
counts, which differ by design (.claude/skills/ta-drive/references/modules.md, "To compare peers").

THE RUN LOCK: two game lanes (tools/runlock.py) while the game plays. Both peers render with GDI
unless --gpu (twopeers.py, THE RENDERER). DIR gets result.json, each peer's rosters and run log,
and driver.log. Exit 0 when the peers agree, 1 when they do not, 2 when the game could not be run.
"""
import argparse
import json
import math
import re
import sys
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(TREE / "tools" / "impai"), str(TREE / "tools")]
import runlock      # noqa: E402
import twopeers     # noqa: E402

MAP = "Town & Country"
ARMIES = ("limits-tier2-p0", "limits-tier2-p1")
PAUSED = "*0x511DE8+0x38A51:1"          # the engine's pause byte (modules.md, "To compare peers")
ROSTER_S = 6                            # past the roster's 5 s dump (tagpu_packet_pub.c, ROSTER_DUMP_MS)
PATH_RX = re.compile(r"pathsched: (\d+) ticks: ([0-9.]+) searches a tick.*?searches carried past their tick (\d+)")


def compare(host: list, join: list, tol: float) -> dict:
    """The verdict on two rosters (each `tacli roster --json`'s `units`): {"problems": [...], ...}."""
    problems = []
    a = {u["engine_index"]: u for u in host if u.get("engine_index") is not None}
    b = {u["engine_index"]: u for u in join if u.get("engine_index") is not None}
    only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
    if only_a:
        problems.append(f"{len(only_a)} units alive on the host alone, slots {only_a[:10]}")
    if only_b:
        problems.append(f"{len(only_b)} units alive on the joiner alone, slots {only_b[:10]}")
    both = sorted(set(a) & set(b))
    if not both:
        problems.append("no unit alive on both peers")
    owners, offs, identical = {}, [], 0
    for s in both:
        ua, ub = a[s], b[s]
        if ua["type"] != ub["type"]:
            problems.append(f"slot {s}: {ua['type']} on the host, {ub['type']} on the joiner")
        if (ua["owner"] == 0) == (ub["owner"] == 0):
            problems.append(f"slot {s}: owner {ua['owner']} on the host and {ub['owner']} on the joiner -- "
                            f"its own peer's on {'both' if ua['owner'] == 0 else 'neither'}")
        if owners.setdefault(ua["owner"], ub["owner"]) != ub["owner"]:
            problems.append(f"slot {s}: the host's owner {ua['owner']} is the joiner's {ub['owner']} here "
                            f"and {owners[ua['owner']]} elsewhere")
        off = math.dist(ua["world"], ub["world"])
        offs.append(off)
        identical += off == 0
        if off > tol:
            mine = "host" if ua["owner"] == 0 else "joiner"
            problems.append(f"slot {s} ({ua['type']}, the {mine}'s): {off:.0f} world units apart "
                            f"({ua['world']} on the host, {ub['world']} on the joiner)")
    if len(set(owners.values())) != len(owners):
        problems.append(f"the owners do not map one to one: {owners}")
    offs.sort()
    return {"problems": problems, "host_alive": len(a), "join_alive": len(b), "both": len(both),
            "host_own": sum(1 for u in a.values() if u["owner"] == 0),
            "join_own": sum(1 for u in b.values() if u["owner"] == 0),
            "identical": identical, "off_p99": offs[int(len(offs) * 0.99)] if offs else 0,
            "off_max": offs[-1] if offs else 0, "owners": {str(k): v for k, v in owners.items()}}


def roster(tp, name) -> list:
    o = tp.tacli("roster", name, "--json", timeout=60)
    return json.loads(o[o.index("{"):])["units"]


def paused_rosters(tp, out: Path) -> tuple:
    """Each peer's roster taken while paused: two reads a dump apart that are equal, three tries."""
    for attempt in range(3):
        time.sleep(ROSTER_S)
        first = {n: roster(tp, n) for n in (tp.host, tp.join)}
        time.sleep(ROSTER_S)
        second = {n: roster(tp, n) for n in (tp.host, tp.join)}
        if first == second:
            for n, units in second.items():
                (out / f"roster-{n}.json").write_text(json.dumps(units))
            return second[tp.host], second[tp.join]
    raise twopeers.CouldNotRun("a peer's roster changed between two reads while the game was paused")


def path_line(log: str) -> str:
    if "pathverify: ARMED" in log:
        found = re.findall(r"pathverify: (\d+) searches: (\d+) equal, (\d+) DIFFERENT", log)
        return f"the engine's scheduler; the oracle compared {sum(int(f[0]) for f in found)} searches, " \
               f"{sum(int(f[2]) for f in found)} different"
    rows = PATH_RX.findall(log)
    ticks = sum(int(r[0]) for r in rows)
    searches = sum(int(r[0]) * float(r[1]) for r in rows)
    return f"{ticks} ticks, {searches:.0f} searches, {sum(int(r[2]) for r in rows)} carried past their tick"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw.dll")
    ap.add_argument("--join-dll", type=Path, help="the joiner's ddraw.dll, when it differs from the host's")
    ap.add_argument("--until", type=int, default=5400, help="the host's GameTime to pause at (5400)")
    ap.add_argument("--tol", type=float, default=128.0, help="world units a unit may stand off its owner's copy")
    ap.add_argument("--port", type=int, help="the DirectPlay port (leased when not given)")
    ap.add_argument("--engine-paths", action="store_true", help="the engine's own path scheduler (tagpu_pathverify.on)")
    ap.add_argument("--gpu", action="store_true", help="render both peers on the default renderer")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    runlock.hold(TREE, f"mp_agree: two peers of {len(ARMIES) * 1499} units, compared paused", lanes=2,
                 gpu=args.gpu)
    result = {"until": args.until, "tol": args.tol, "engine_paths": args.engine_paths}
    try:
        with twopeers.TwoPeers(args.out, args.dll, args.port, "ag", args.gpu, args.join_dll) as tp:
            limit = ("--unit-limit", "1500")
            levers = ("tagpu_pathlog.on",) + (("tagpu_pathverify.on",) if args.engine_paths else ())
            tp.launch(host_args=limit, join_args=limit, levers=levers)
            tp.battle_room(MAP)
            tp.start()
            for name, army in zip((tp.host, tp.join), ARMIES):
                tp.tacli("scenario", "apply", name, army, "--timeout", "300", timeout=360)
            tp.play_to(args.until)
            tp.tacli("keys", tp.host, "shift", "pause")
            deadline = time.monotonic() + 60
            while not all(tp.peek(n, PAUSED)[0] == 1 for n in (tp.host, tp.join)):
                if time.monotonic() > deadline:
                    raise twopeers.CouldNotRun("the game's Pause did not pause both peers in 60 s")
                time.sleep(1)
            result["gametime"] = {"host": tp.gametime(tp.host), "join": tp.gametime(tp.join)}
            host, join = paused_rosters(tp, args.out)
            result.update(compare(host, join, args.tol))
        if tp.refused:
            raise twopeers.CouldNotRun("; ".join(tp.refused))
        for name in (tp.host, tp.join):
            log = args.out / f"{name}.log"
            result[f"path_{name[-1]}"] = path_line(log.read_text(errors="replace")) if log.exists() else "no log"
    except twopeers.CouldNotRun as e:
        result["could_not_run"] = str(e)
        (args.out / "result.json").write_text(json.dumps(result, indent=1))
        print(f"mp_agree: could not run: {e}")
        return 2
    (args.out / "result.json").write_text(json.dumps(result, indent=1))
    r = result
    print(f"mp_agree: paused at GameTime {r['gametime']['host']} (host) / {r['gametime']['join']} (joiner); "
          f"{r['both']} units on both peers ({r['host_own']} the host's, {r['join_own']} the joiner's); "
          f"{r['identical']} at the identical position, off by {r['off_p99']:.0f} at the 99th percentile, "
          f"{r['off_max']:.0f} at most")
    print(f"mp_agree: pathfinder, host: {r.get('path_H')}; joiner: {r.get('path_J')}")
    for p in r["problems"][:40]:
        print(f"mp_agree: DISAGREE {p}")
    print("mp_agree: " + ("the peers agree" if not r["problems"] else f"{len(r['problems'])} disagreements"))
    return 1 if r["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
