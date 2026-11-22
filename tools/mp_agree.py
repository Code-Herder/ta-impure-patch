#!/usr/bin/env python3
"""mp_agree -- do the two machines of a network game agree on the battle? Two peers, 1499 units each,
fought for a few minutes, paused, and each peer's units compared with the other's.

    tools/mp_agree.py --out DIR [--dll PATH] [--join-dll PATH] [--until T] [--tol W] [--engine-paths] [--gpu]
                      [--marches N [--march-every TICKS]]

One two-player game on Town & Country (tools/impai/twopeers.py: the run's two instances on a private
display and a DirectPlay port leased for the game, MultiCommanderDeath 0), the 1500 unit limit; the host
applies scenarios/limits-tier2-p0.json and the joiner limits-tier2-p1.json, each its own army of Peewees,
Hammers and Rockos ordered onto the map's centre. At the host's GameTime T (5400 by default: three
minutes at 30 ticks a second) the host presses the game's own Pause, which pauses both peers, and each
peer's roster (`tacli roster`, the game thread's log of every live unit) is read twice, a dump apart;
the two reads of a peer must be equal, which shows the rosters were taken paused.

WHAT IT JUDGES, unit by unit, matched by engine slot (the same slot on every peer):
  - the same units are alive on both peers, each of the same type -- but for a DEATH IN FLIGHT: a unit gone
    on its owner and alive on the other peer, whose death the other peer has not received when it pauses
    (the owner alone decides a death, and the two pauses land a few ticks apart). Those are counted and
    reported, and more than DEATHS_IN_FLIGHT of them is a disagreement (MEASURED 2026-11-20: 0 or 2 a run,
    a lost death message would read as one every run); a unit alive on its owner and gone on the other
    peer is a disagreement;
  - each unit belongs to the same player on both: a peer numbers the players from its own seat (its
    own units are owner 0), so the owners must map one to one between the peers, and every unit is
    its own peer's (owner 0) on exactly one of them;
  - each unit stands where its owner's copy stands, within W world units (256 by default). A remote
    copy is where its owner's last entry for it put it: a unit moving at the pause trails by its speed
    times the ticks its entry waited, the message's way and the two pauses' skew. The unit stream
    (tagpu_patches.c, unit_stream) sends a block's dirty units in turn, in up to four messages a tick,
    so an entry waits one turn of the walk at most: MEASURED 2026-11-21 at most 4 ticks for a whole
    army ordered at once, the copies 100 world units off at most there and unit for unit alike in the
    fight. A stationary unit agrees exactly; the report gives the share that does.
--marches N plays no fight: each peer's whole army is ordered to move as one, N times, --march-every ticks
apart (150), between points apart from the other army, and the game is paused 300 ticks after the last. Every
unit of a block is dirty in the tick of a march, the unit stream's worst case; the report's `unit stream`
lines give the longest wait.
--engine-paths writes tagpu_pathverify.on into both game folders, which puts the engine's own path scheduler
back in place of ours (our search runs beside it as an oracle and writes nothing): the same game with
stock pathfinding, for a comparison. Both peers log the pathfinder's lines (tagpu_pathlog.on) and the unit
stream's queue (tagpu_netlog.on: the units left dirty after each tick's message, and how long a unit waited
for its entry), which the report sums.
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
# --marches: the two points each peer's whole army is ordered between, apart from the other's so that
# nothing fights and every unit lives: a march makes every unit of a block dirty in one tick
# A march is a player's own: Ctrl+A, the camera on the point, a right click in the middle of the view --
# the engine's order path for every selected unit (tacli's `order --sel` reaches 512). The points are empty
# ground the army never reaches between two marches. The camera's pin is applied by a later frame, so the
# click waits for it; a run whose marches did not take is told by its unit stream (MARCH_DIRTY).
MARCH = (((7400, 2300), (5300, 2500)), ((4000, 6900), (5600, 7900)))
VIEW_MIDDLE = ("576", "384")
# a march that took leaves most of a block dirty after its tick's messages: at least this many units
MARCH_DIRTY = 700
DEATHS_IN_FLIGHT = 32
PAUSED = "*0x511DE8+0x38A51:1"          # the engine's pause byte (modules.md, "To compare peers")
ROSTER_S = 6                            # past the roster's 5 s dump (tagpu_packet_pub.c, ROSTER_DUMP_MS)
STREAM_RX = re.compile(r"netstream: record \d+, (\d+) ticks of a block of \d+ units: sent ([0-9.]+) a tick "
                       r"\(most (\d+)\); left dirty ([0-9.]+) a tick \(most (\d+)\); .*?: (\d+) units, mean ([0-9.]+) "
                       r"ticks, most (\d+), 30 ticks or more (\d+)")
TICK_MOST_RX = re.compile(r"netstream: record .*a tick's most: \d+ bytes in (\d+) messages")
PATH_RX = re.compile(r"pathsched: (\d+) ticks: ([0-9.]+) searches a tick.*?searches carried past their tick (\d+)")


def compare(host: list, join: list, tol: float) -> dict:
    """The verdict on two rosters (each `tacli roster --json`'s `units`): {"problems": [...], ...}."""
    problems = []
    a = {u["engine_index"]: u for u in host if u.get("engine_index") is not None}
    b = {u["engine_index"]: u for u in join if u.get("engine_index") is not None}
    in_flight = 0
    for here, there, name, other in ((a, b, "host", "joiner"), (b, a, "joiner", "host")):
        alone = sorted(s for s in set(here) - set(there))
        mine = [s for s in alone if here[s]["owner"] == 0]     # alive on its owner, gone on the other peer
        in_flight += len(alone) - len(mine)
        if mine:
            problems.append(f"{len(mine)} units of the {name}'s alive on the {name} and gone on the {other}, "
                            f"slots {mine[:10]}")
    if in_flight > DEATHS_IN_FLIGHT:
        problems.append(f"{in_flight} units gone on their owner and alive on the other peer, more than "
                        f"{DEATHS_IN_FLIGHT} deaths in flight")
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
            "deaths_in_flight": in_flight,
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


def stream_line(log: str) -> str:
    rows = [tuple(float(x) for x in r) for r in STREAM_RX.findall(log)]
    if not rows:
        return "no netstream lines"
    ticks = sum(r[0] for r in rows)
    waited = sum(r[5] for r in rows)
    return (f"sent {sum(r[0] * r[1] for r in rows) / ticks:.1f} a tick (most {max(r[2] for r in rows):.0f}); "
            f"left dirty {sum(r[0] * r[3] for r in rows) / ticks:.1f} a tick (most {max(r[4] for r in rows):.0f}); "
            f"a unit's wait for its entry: mean {sum(r[5] * r[6] for r in rows) / max(waited, 1):.1f} ticks, "
            f"most {max(r[7] for r in rows):.0f}, {sum(r[8] for r in rows):.0f} of {waited:.0f} waits 30 ticks or more")


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
    ap.add_argument("--tol", type=float, default=256.0, help="world units a unit may stand off its owner's copy")
    ap.add_argument("--port", type=int, help="the DirectPlay port (leased when not given)")
    ap.add_argument("--engine-paths", action="store_true", help="the engine's own path scheduler (tagpu_pathverify.on)")
    ap.add_argument("--gpu", action="store_true", help="render both peers on the default renderer")
    ap.add_argument("--marches", type=int, default=0,
                    help="order each peer's whole army to move, this many times back to back, in place of the fight")
    ap.add_argument("--march-every", type=int, default=150, help="ticks between two marches (150)")
    ap.add_argument("--host-as-stock", action="store_true",
                    help="the host joins as stock does (tagpu_modprint_stock.on): it holds no play revision and must send one "
                         "message a tick, which fails the run if it sends more")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    runlock.hold(TREE, f"mp_agree: two peers of {len(ARMIES) * 1499} units, compared paused", lanes=2,
                 gpu=args.gpu)
    result = {"until": args.until, "tol": args.tol, "engine_paths": args.engine_paths}
    try:
        with twopeers.TwoPeers(args.out, args.dll, args.port, "ag", args.gpu, args.join_dll) as tp:
            limit = ("--unit-limit", "1500")
            levers = ("tagpu_pathlog.on", "tagpu_netlog.on") + (("tagpu_pathverify.on",) if args.engine_paths else ())
            tp.launch(host_args=limit, join_args=limit, levers=levers,
                      host_levers=("tagpu_modprint_stock.on",) if args.host_as_stock else ())
            tp.battle_room(MAP)
            tp.start()
            for name, army in zip((tp.host, tp.join), ARMIES):
                tp.tacli("scenario", "apply", name, army, "--timeout", "300", timeout=360)
            if args.marches:
                at = tp.gametime(tp.host) + 60
                for k in range(args.marches):
                    tp.play_to(at + k * args.march_every)
                    for name, points in zip((tp.host, tp.join), MARCH):
                        x, z = points[k % 2]
                        tp.tacli("eye", name, str(x), str(z))
                        time.sleep(1)
                        tp.tacli("keys", name, "ctrl+a")
                        tp.tacli("click", name, "--right", *VIEW_MIDDLE)
                        print(f"mp_agree: march {k + 1} at GameTime {tp.gametime(tp.host)}, {name} to {x},{z}",
                              flush=True)
                args.until = at + args.marches * args.march_every + 300
                result["until"] = args.until
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
            text = log.read_text(errors="replace") if log.exists() else ""
            result[f"path_{name[-1]}"] = path_line(text) if text else "no log"
            result[f"stream_{name[-1]}"] = stream_line(text) if text else "no log"
            result[f"tick_messages_{name[-1]}"] = max((int(n) for n in TICK_MOST_RX.findall(text)), default=0)
            if args.marches:
                most = max((int(r[4]) for r in STREAM_RX.findall(text)), default=0)
                if most < MARCH_DIRTY:
                    raise twopeers.CouldNotRun(f"the marches did not take on {name}: at most {most} units were left "
                                               f"dirty after a tick's messages, under {MARCH_DIRTY}")
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
          f"{r['off_max']:.0f} at most; {r['deaths_in_flight']} deaths in flight")
    print(f"mp_agree: pathfinder, host: {r.get('path_H')}; joiner: {r.get('path_J')}")
    print(f"mp_agree: unit stream, host: {r.get('stream_H')}; a tick's most messages {r.get('tick_messages_H')}")
    print(f"mp_agree: unit stream, joiner: {r.get('stream_J')}; a tick's most messages {r.get('tick_messages_J')}")
    if args.host_as_stock and r.get("tick_messages_H") != 1:
        r["problems"].append(f"the host holds no play revision and sent {r.get('tick_messages_H')} messages in a tick, not 1")
    for p in r["problems"][:40]:
        print(f"mp_agree: DISAGREE {p}")
    print("mp_agree: " + ("the peers agree" if not r["problems"] else f"{len(r['problems'])} disagreements"))
    return 1 if r["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
