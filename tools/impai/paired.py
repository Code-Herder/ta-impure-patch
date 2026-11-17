#!/usr/bin/env python3
"""paired.py -- the paired value bar: two arms of the same bar games, paired by seed, judged on ImpAI's army metal
lost and enemy metal killed against OTA AI.

    tools/impai/paired.py BASE TEST [--pairs N]

BASE and TEST are directories of M1's per-game measurements, searched recursively: every `*.json` under them is
one recording's measurement as `.data/impai/runs/a3/tools/measure.py` writes it (`run_all.sh` beside it runs it
over a list of recordings). A measurement names its recording (`rec`), and the bar's result for that game is the
`<id>.json` beside the recording (bar.py, A GAME), which carries the seed the game played. BASE is the arm without
the change, TEST the arm with it, each played by `bar.py play` on the same game ids.

THE SEAT-GAMES: every ImpAI seat of a game whose other seats are all OTA AI. ImpAI against ImpAI is left out: there
both seats change, so one seat's kills move with the other's change and the pair no longer isolates the seat's own.

THE PAIRING, by seed: a TEST seat-game pairs with the BASE seat-game of the same game id and seat. The id fixes the
seed (bar.py, THE SEED), and each arm's result states the seed it played; a pair whose two seeds differ, a
seat-game with no partner, a recording the measurement did not read to its end (`stop` set), an id measured twice
in one arm, or fewer than N pairs, and the bar COULD NOT JUDGE.

THE MEASURES of a seat-game, from the measurement's deaths of finished units from GameTime 30 on (the scenario
applier replaces every commander at GameTime 1, which is no fight; a unit still being built is no army):
  lost    the metal cost of the seat's army units killed by another player. A death with no killing player -- the
          level's kill-all, a self-destruction -- is no loss to an enemy.
  killed  the metal cost of the other players' army units and constructors the seat killed.
Each is the measurement's own class and cost (measure.py, the roles table's `cost`).

THE BAR (the owner's decision of 2026-11-14), over at least N paired seat-games (60 by default), each the mean of
the pairs' differences TEST - BASE with a 95 % percentile bootstrap interval (RESAMPLES resamples of the pairs from
a fixed seed, so a set of games always gets the same verdict):
  (a) lost falls: the interval's upper bound is below 0;
  (b) killed falls by no more than 10 %: the interval's lower bound is above -KILLED_MARGIN x BASE's mean killed.
Reported beside it and not judged: each arm's means, the exchange (killed - lost), and the differences by level.
Exit 0 when (a) and (b) hold, 1 when either fails, 2 when the bar could not judge."""
import argparse
import json
import random
import sys
from pathlib import Path

PAIRS = 60
RESAMPLES = 10000
BOOT_SEED = 1
KILLED_MARGIN = 0.10
FIRST_T = 30            # deaths before this are the scenario applier's replacement of the commanders
IMPAI, OTA = "impai", "ai"


class CouldNotJudge(Exception):
    pass


def seat_measures(deaths, seat: int) -> dict:
    """lost and killed of `seat` over one measurement's deaths (THE MEASURES)"""
    lost = killed = 0.0
    for d in deaths:
        if d["t"] < FIRST_T or not d["fin"]:
            continue
        kp = d["kp"]
        if d["owner"] == seat and d["cls"] == "army" and kp is not None and kp != seat:
            lost += d["metal"]
        if d["owner"] != seat and kp == seat and d["cls"] in ("army", "cons"):
            killed += d["metal"]
    return {"lost": lost, "killed": killed}


def ota_seats(seats: dict) -> list:
    """the ImpAI seats of a game whose other seats are all OTA AI (THE SEAT-GAMES)"""
    out = []
    for s, v in seats.items():
        others = [o for k, o in seats.items() if k != s]
        if v.get("ai") == IMPAI and others and all(o.get("ai") == OTA for o in others):
            out.append(int(s))
    return sorted(out)


def read_arm(root: Path) -> dict:
    """{(game id, seat): {lost, killed, level, seed}} over every measurement under `root`"""
    files = sorted(root.rglob("*.json"))
    if not files:
        raise CouldNotJudge(f"no measurement under {root}")
    out, seen = {}, {}
    for f in files:
        try:
            m = json.loads(f.read_text())
            gid, seats, deaths, rec = m["id"], m["seats"], m["deaths"], m["rec"]
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise CouldNotJudge(f"{f}: not a measurement ({e})")
        if m.get("stop") not in (None, "None"):
            raise CouldNotJudge(f"{f}: the recording was not read to its end ({m['stop']})")
        if gid in seen:
            raise CouldNotJudge(f"{gid} is measured twice in {root}: {seen[gid]} and {f}")
        seen[gid] = f
        game_json = Path(rec).with_suffix(".json")
        try:
            seed = json.loads(game_json.read_text())["seed"]
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise CouldNotJudge(f"{gid}: no seed in its result {game_json} ({e})")
        if seed is None:
            raise CouldNotJudge(f"{gid}: its result {game_json} names no seed")
        for s in ota_seats(seats):
            out[(gid, s)] = dict(seat_measures(deaths, s), level=seats[str(s)].get("level"), seed=seed)
    return out


def pair(base: dict, test: dict, want: int) -> list:
    """the paired keys, in order; CouldNotJudge on a partner missing, a seed differing or fewer than `want`"""
    alone = sorted(set(base) ^ set(test))
    if alone:
        names = " ".join(f"{g}:{s}" for g, s in alone[:12]) + (" ..." if len(alone) > 12 else "")
        raise CouldNotJudge(f"{len(alone)} seat-games have no partner in the other arm: {names}")
    keys = sorted(base)
    differ = [k for k in keys if base[k]["seed"] != test[k]["seed"]]
    if differ:
        raise CouldNotJudge(f"{len(differ)} pairs played different seeds: "
                            + " ".join(f"{g}:{s}" for g, s in differ[:12]))
    if len(keys) < want:
        raise CouldNotJudge(f"{len(keys)} paired seat-games against OTA AI, the bar reads {want}")
    return keys


def boot(xs: list, n: int = None, seed: int = BOOT_SEED) -> tuple:
    """the 95 % percentile bootstrap interval of the mean of xs"""
    n = n or RESAMPLES
    rng = random.Random(seed)
    k = len(xs)
    means = sorted(sum(rng.choice(xs) for _ in range(k)) / k for _ in range(n))
    return means[int(0.025 * n)], means[int(0.975 * n)]


def mean(xs: list) -> float:
    return sum(xs) / len(xs)


def judge(base_dir: Path, test_dir: Path, want: int = PAIRS) -> int:
    try:
        base, test = read_arm(base_dir), read_arm(test_dir)
        keys = pair(base, test, want)
    except CouldNotJudge as e:
        print(f"paired: COULD NOT JUDGE: {e}")
        return 2
    print(f"paired: {len(keys)} seat-games against OTA AI, paired by seed; means a seat-game, TEST - BASE with "
          f"its 95 % interval")
    res = {}
    for f in ("lost", "killed"):
        d = [test[k][f] - base[k][f] for k in keys]
        lo, hi = boot(d)
        res[f] = (mean(d), lo, hi)
        print(f"  {f:6} base {mean([base[k][f] for k in keys]):8.0f}  test {mean([test[k][f] for k in keys]):8.0f}"
              f"  diff {mean(d):+8.0f} [{lo:+.0f}, {hi:+.0f}]")
    d = [(test[k]["killed"] - test[k]["lost"]) - (base[k]["killed"] - base[k]["lost"]) for k in keys]
    lo, hi = boot(d)
    print(f"  exchange (killed - lost) diff {mean(d):+.0f} [{lo:+.0f}, {hi:+.0f}]")
    for lv in sorted({base[k]["level"] for k in keys}, key=str):
        kk = [k for k in keys if base[k]["level"] == lv]
        dl = [test[k]["lost"] - base[k]["lost"] for k in kk]
        dk = [test[k]["killed"] - base[k]["killed"] for k in kk]
        (ll, lh), (kl, kh) = boot(dl), boot(dk)
        print(f"  {lv}: {len(kk)} seat-games, lost {mean(dl):+.0f} [{ll:+.0f}, {lh:+.0f}], killed {mean(dk):+.0f} "
              f"[{kl:+.0f}, {kh:+.0f}]")
    floor = -KILLED_MARGIN * mean([base[k]["killed"] for k in keys])
    a = res["lost"][2] < 0
    b = res["killed"][1] > floor
    print(f"  (a) lost falls: upper bound {res['lost'][2]:+.0f} below 0: {'PASS' if a else 'FAIL'}")
    print(f"  (b) killed within {KILLED_MARGIN:.0%}: lower bound {res['killed'][1]:+.0f} above {floor:+.0f}: "
          f"{'PASS' if b else 'FAIL'}")
    print(f"paired: {'PASS' if a and b else 'FAIL'}")
    return 0 if a and b else 1


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("base", type=Path, help="the measurements of the arm without the change")
    p.add_argument("test", type=Path, help="the measurements of the arm with it")
    p.add_argument("--pairs", type=int, default=PAIRS, help=f"the paired seat-games the bar reads (default {PAIRS})")
    a = p.parse_args(argv)
    return judge(a.base, a.test, a.pairs)


if __name__ == "__main__":
    sys.exit(main())
