#!/usr/bin/env python3
"""tourney.py -- full engine games between two seats, for ImpAI's tuning (tiers B and C of A3).

    tourney.py ids --set tierC --maps M1,M2,M3,M4,M5 [--out FILE]
    tourney.py register --dir DIR --tuned TABLE --hand TABLE --maps M1,..,M5
    tourney.py play --ids FILE --lanes L --out DIR            (waits on the bar harness, below)
    tourney.py judge --dir DIR RESULTS_DIR...                 tier C's verdict on its registered games
    tourney.py pieces --ids FILE --lanes L [--per-lane K]     the runner pieces of a game list
    tourney.py power [--n 400] [--alpha 0.01] [--sim 20000] [--seed 1]

A GAME is 1v1, ImpAI Hard against ImpAI Hard, seat A playing the candidate table and seat B the hand-written
one, to a commander's death or a cap, seated as the bar seats its games (research/notes/impai/bar.md).
A SET fixes the games' list, the cap and how a game at the cap is scored:
    tierB  the race's maps, 20 game minutes (GameTime 36 000), a game at the cap won by the side that keeps
           the larger share of the value it has built (the calibration's value rule), a tie to seat B;
    tierC  the bar's five maps, 45 game minutes (81 000), a game at the cap counted against seat A.
Every combination of map, side pairing (Arm/Arm, Arm/Core, Core/Arm, Core/Core) and start (seat A at the
map's first start or its second) is played `reps` times; a game's id names all four, so a piece is a list of
ids and any piece runs beside any other.

TIER C IS PRE-REGISTERED. `register` writes registration.json -- the 400 ids, the two tables' hashes, the
threshold -- before a game is played, and refuses (exit 1) when one exists with other tables or other ids;
`judge` counts only registered ids, refuses a game played with another table, and passes when seat A wins at
least THRESHOLD of them: the smallest k with P(X >= k) < 0.01 for X ~ Binomial(400, 1/2), 224 (a one-sided
binomial test of "the tuned table wins more than half"). Its power, exactly: 0.95 at a true win rate of 60 %,
0.81 at 58 %, 0.36 at 55 %, and a false pass at 50 % with probability 0.0093 (`power`, which also checks them
by simulation).

A RESULT, one JSON file a game, as the game runner writes it:
    {"id": ..., "outcome": "A" | "B" | "cap" | "could_not_run", "end_tick": int,
     "tables": {"A": "<hash>", "B": "<hash>"},            from each seat's `impai: table` line
     "value": {"A": {"built": v, "live": v}, "B": {...}}, at the end: what each side built, what it keeps
     "reason": "..."}                                      for could_not_run
"A" or "B" is the side whose opponent's commander died. A game that could not run is never an outcome: the
judge says "could not run" (exit 2) until it is played.

THE GAME RUNNER is the bar harness's lane (tools/impai/bar.py, A2b's b6), which does not exist yet: `play`
imports bar.py's `play_game(game: dict, lane: int, out_dir: Path) -> dict` -- one game of this module's game
spec on one lane, its result as above -- and says "could not run" (exit 2) while there is none. It holds the
run lock for L lanes (tools/runlock.py: W = 2 x L slots).

Exit 0 when the verdict passed (or the command ran), 1 when it failed or was refused, 2 when it could not
run."""
import argparse
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import tablefile  # noqa: E402

SIDES = [("arm", "arm"), ("arm", "core"), ("core", "arm"), ("core", "core")]
STARTS = 2
SETS = {
    #         maps  reps  cap ticks  at the cap   seconds a game a lane: [INFERRED] about 20 s of launch and
    #         300-800 ticks a second, no whole game measured yet
    "tierB": {"maps": 5, "reps": 1, "cap": 36000, "cap_rule": "value", "est_s": 90},
    "tierC": {"maps": 5, "reps": 10, "cap": 81000, "cap_rule": "against_A", "est_s": 180},
}
ALPHA = 0.01
TIER_C_GAMES = 400
PIECE_S = 600       # a piece ends in at most ten minutes at the inferred game length
REGISTRATION = "registration.json"


class TourneyError(Exception):
    pass


# ---- the statistics ----

def binom_tail(n: int, k: int, p: float) -> float:
    """P(X >= k) for X ~ Binomial(n, p), summed exactly in log space."""
    if k <= 0:
        return 1.0
    if k > n:
        return 0.0
    if p <= 0.0:
        return 0.0
    if p >= 1.0:
        return 1.0
    lp, lq = math.log(p), math.log1p(-p)
    terms = [math.lgamma(n + 1) - math.lgamma(i + 1) - math.lgamma(n - i + 1) + i * lp + (n - i) * lq
             for i in range(k, n + 1)]
    top = max(terms)
    return min(1.0, math.exp(top) * sum(math.exp(t - top) for t in terms))


def threshold(n: int, alpha: float = ALPHA) -> int:
    """The fewest wins of n that a one-sided binomial test against 1/2 calls significant: the smallest k with
    P(X >= k | n, 1/2) < alpha."""
    for k in range(n // 2, n + 2):
        if binom_tail(n, k, 0.5) < alpha:
            return k
    return n + 1


def upper_bound(wins: int, n: int, conf: float = 0.95) -> float:
    """The one-sided Clopper-Pearson upper bound on a win rate: the p at which wins or fewer of n has
    probability 1 - conf. 1 when every game was won."""
    if wins >= n:
        return 1.0
    lo, hi = 0.0, 1.0
    for _ in range(100):
        mid = (lo + hi) / 2
        if 1.0 - binom_tail(n, wins + 1, mid) > 1.0 - conf:     # P(X <= wins | mid) still too large
            lo = mid
        else:
            hi = mid
    return hi


def simulate(n: int, k: int, p: float, trials: int, seed: int) -> float:
    """The share of `trials` simulated sets of n games, each won with probability p, that reach k wins."""
    import numpy as np
    rng = np.random.Generator(np.random.PCG64(seed))
    wins = (rng.random((trials, n)) < p).sum(axis=1)
    return float((wins >= k).mean())


# ---- the games ----

def game_ids(set_name: str, maps) -> list:
    s = SETS[set_name]
    if len(maps) != s["maps"]:
        raise TourneyError(f"{set_name} plays {s['maps']} maps, not {len(maps)}")
    if len(set(maps)) != len(maps) or any("-" in m or not m for m in maps):
        raise TourneyError("the maps must be distinct names without '-'")
    return [f"{set_name}-{m}-{a}{b}-s{st}-{r:02d}"
            for m in maps for a, b in SIDES for st in range(STARTS) for r in range(s["reps"])]


def parse_id(gid: str) -> dict:
    try:
        set_name, m, sides, start, rep = gid.split("-")
        a, b = next((x, y) for x, y in SIDES if x + y == sides)
        assert set_name in SETS and start[0] == "s"
        return {"id": gid, "set": set_name, "map": m, "sides": {"A": a, "B": b}, "start": int(start[1:]),
                "rep": int(rep)}
    except (ValueError, AssertionError, StopIteration) as e:
        raise TourneyError(f"not a game id: {gid!r}") from e


def game_spec(gid: str, tables: dict) -> dict:
    """What the game runner plays: the map, each seat's side and controller, seat A's start, the cap."""
    g = parse_id(gid)
    s = SETS[g["set"]]
    g["seats"] = {seat: {"ai": "impai", "level": "hard", "table": str(tables[seat]["path"]),
                         "table_hash": tables[seat]["hash"]} for seat in ("A", "B")}
    g["cap"], g["cap_rule"] = s["cap"], s["cap_rule"]
    return g


def winner(result: dict, cap_rule: str) -> str:
    """'A' or 'B' for a played game; TourneyError for one that could not run or is malformed."""
    o = result.get("outcome")
    if o in ("A", "B"):
        return o
    if o == "cap":
        if cap_rule == "against_A":
            return "B"
        if cap_rule == "value":
            try:
                v = result["value"]
                share = {s: v[s]["live"] / v[s]["built"] if v[s]["built"] > 0 else 0.0 for s in ("A", "B")}
            except (KeyError, TypeError) as e:
                raise TourneyError(f"{result.get('id')}: a game at the cap carries no values") from e
            return "A" if share["A"] > share["B"] else "B"
        raise TourneyError(f"no cap rule {cap_rule!r}")
    if o == "could_not_run":
        raise TourneyError(f"{result.get('id')}: could not run: {result.get('reason', '')}")
    raise TourneyError(f"{result.get('id')}: no outcome {o!r}")


def read_results(dirs) -> dict:
    """Every result JSON under the directories, by id; a second result for one id is refused."""
    out = {}
    for d in dirs:
        for p in sorted(Path(d).rglob("*.json")):
            if p.name == REGISTRATION:
                continue
            r = json.loads(p.read_text())
            if not isinstance(r, dict) or "id" not in r:
                continue
            if r["id"] in out:
                raise TourneyError(f"two results for {r['id']}: {out[r['id']]['_path']} and {p}")
            r["_path"] = str(p)
            out[r["id"]] = r
    return out


# ---- tier C: the registration and the verdict ----

def register(d: Path, tuned: Path, hand: Path, maps) -> dict:
    tables = {"A": {"path": str(tuned), "hash": tablefile.read(tuned).hash_hex()},
              "B": {"path": str(hand), "hash": tablefile.read(hand).hash_hex()}}
    ids = game_ids("tierC", maps)
    if len(ids) != TIER_C_GAMES:
        raise TourneyError(f"tier C is {TIER_C_GAMES} games, not {len(ids)}")
    reg = {"set": "tierC", "n": len(ids), "alpha": ALPHA, "threshold": threshold(len(ids)),
           "cap": SETS["tierC"]["cap"], "cap_rule": "against_A", "maps": list(maps),
           "tables": {s: {"hash": tables[s]["hash"], "path": tables[s]["path"]} for s in tables}, "ids": ids}
    p = d / REGISTRATION
    if p.exists():
        old = json.loads(p.read_text())
        same = (old.get("ids") == ids and
                all(old["tables"][s]["hash"] == tables[s]["hash"] for s in ("A", "B")))
        if not same:
            raise TourneyError(f"{p} registers other tables or other games; a registration is never rewritten")
        return old
    d.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(reg, indent=1) + "\n")
    return reg


def judge(reg: dict, results: dict) -> dict:
    """The verdict on the registered games: {"status": "pass" | "fail" | "could_not_run", ...}."""
    ids = reg["ids"]
    missing = [i for i in ids if i not in results]
    unregistered = sorted(set(results) - set(ids))
    out = {"n": len(ids), "threshold": reg["threshold"], "missing": missing, "unregistered": unregistered,
           "wrong_table": [], "could_not_run": [], "wins": 0, "losses": 0, "capped": []}
    for gid in ids:
        r = results.get(gid)
        if r is None:
            continue
        tables = r.get("tables", {})
        if any(tables.get(s) != reg["tables"][s]["hash"] for s in ("A", "B")):
            out["wrong_table"].append(gid)
            continue
        if r.get("outcome") == "could_not_run":
            out["could_not_run"].append(gid)
            continue
        if r.get("outcome") == "cap":
            out["capped"].append(gid)
        if winner(r, reg["cap_rule"]) == "A":
            out["wins"] += 1
        else:
            out["losses"] += 1
    played = out["wins"] + out["losses"]
    out["p_value"] = binom_tail(played, out["wins"], 0.5) if played else 1.0
    if out["wrong_table"]:
        out["status"] = "fail"
    elif missing or out["could_not_run"]:
        out["status"] = "could_not_run"
    else:
        out["status"] = "pass" if out["wins"] >= reg["threshold"] else "fail"
    return out


# ---- pieces ----

def pieces(ids, lanes: int, per_lane: int = 0, set_name: str = None) -> list:
    """The games split into independent pieces of at most per_lane x lanes games each, a piece sized to end in
    PIECE_S at the set's inferred game length when per_lane is 0. Each holds W = 2 x lanes of the run lock."""
    import runlock
    if lanes < 1:
        raise TourneyError("a piece plays on at least one lane")
    set_name = set_name or parse_id(ids[0])["set"]
    est = SETS[set_name]["est_s"]
    k = per_lane or max(1, PIECE_S // est)
    size = k * lanes
    out = []
    for i in range(0, len(ids), size):
        chunk = ids[i:i + size]
        out.append({"piece": f"{set_name}-{i // size:03d}", "lanes": lanes, "W": runlock.slots_for(lanes),
                    "games": len(chunk), "est_s": math.ceil(len(chunk) / lanes) * est, "ids": chunk})
    return out


# ---- the commands ----

def cmd_ids(a) -> int:
    ids = game_ids(a.set, a.maps.split(","))
    text = "\n".join(ids) + "\n"
    if a.out:
        Path(a.out).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


def cmd_register(a) -> int:
    reg = register(Path(a.dir), Path(a.tuned), Path(a.hand), a.maps.split(","))
    print(f"tourney: registered {reg['n']} games of tables A {reg['tables']['A']['hash']} and "
          f"B {reg['tables']['B']['hash']}; pass at {reg['threshold']} wins")
    return 0


def cmd_judge(a) -> int:
    reg = json.loads((Path(a.dir) / REGISTRATION).read_text())
    v = judge(reg, read_results(a.results))
    print(f"tourney: {v['status']}: seat A won {v['wins']} of {v['wins'] + v['losses']} played "
          f"({len(v['capped'])} at the cap, counted against A), pass at {v['threshold']} of {v['n']}; "
          f"one-sided p = {v['p_value']:.4g}")
    for key in ("missing", "could_not_run", "wrong_table", "unregistered"):
        if v[key]:
            print(f"  {key}: {len(v[key])}: {' '.join(v[key][:12])}{' ...' if len(v[key]) > 12 else ''}")
    if a.json:
        Path(a.json).write_text(json.dumps(v, indent=1) + "\n")
    return {"pass": 0, "fail": 1}.get(v["status"], 2)


def cmd_play(a) -> int:
    try:
        import bar
        play_game = bar.play_game
    except (ImportError, AttributeError):
        print("tourney: could not run: no game runner -- tools/impai/bar.py's play_game (A2b's b6) plays "
              "a game of this module's spec on one lane")
        return 2
    import runlock
    from concurrent.futures import ThreadPoolExecutor
    ids = [x for x in Path(a.ids).read_text().split() if x]
    tables = {}
    for seat, path in (("A", a.table_a), ("B", a.table_b)):
        tables[seat] = {"path": Path(path), "hash": tablefile.read(path).hash_hex()}
    games = [game_spec(i, tables) for i in ids]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    runlock.hold(TREE, "tourney play " + " ".join(sys.argv[2:]), lanes=a.lanes)

    def one(job):
        lane, g = job
        r = play_game(g, lane, out)
        (out / f"{g['id']}.json").write_text(json.dumps(r, indent=1) + "\n")
        return r

    with ThreadPoolExecutor(max_workers=a.lanes) as pool:
        results = list(pool.map(one, [(i % a.lanes, g) for i, g in enumerate(games)]))
    bad = [r["id"] for r in results if r.get("outcome") == "could_not_run"]
    print(f"tourney: {len(results)} games, {len(bad)} could not run")
    return 2 if bad else 0


def cmd_pieces(a) -> int:
    ids = [x for x in Path(a.ids).read_text().split() if x]
    ps = pieces(ids, a.lanes, a.per_lane)
    for p in ps:
        print(f"{p['piece']}: {p['games']} games on {p['lanes']} lanes, W {p['W']}, ~{p['est_s']} s "
              f"[INFERRED game length]")
    if a.json:
        Path(a.json).write_text(json.dumps(ps, indent=1) + "\n")
    return 0


def cmd_power(a) -> int:
    k = threshold(a.n, a.alpha)
    print(f"tourney: {a.n} games, one-sided alpha {a.alpha}: pass at {k} wins "
          f"(P(X >= {k} | 1/2) = {binom_tail(a.n, k, 0.5):.4g}, at {k - 1}: {binom_tail(a.n, k - 1, 0.5):.4g})")
    for p in (0.50, 0.52, 0.55, 0.56, 0.58, 0.60, 0.62, 0.65):
        line = f"  true win rate {p:.2f}: passes with probability {binom_tail(a.n, k, p):.4f} exactly"
        if a.sim:
            line += f", {simulate(a.n, k, p, a.sim, a.seed):.4f} in {a.sim} simulated sets"
        print(line)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="tourney.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("ids", help="a set's game ids")
    s.add_argument("--set", choices=sorted(SETS), required=True)
    s.add_argument("--maps", required=True)
    s.add_argument("--out")
    s = sub.add_parser("register", help="pre-register tier C's games and tables")
    s.add_argument("--dir", required=True)
    s.add_argument("--tuned", required=True)
    s.add_argument("--hand", required=True)
    s.add_argument("--maps", required=True)
    s = sub.add_parser("judge", help="tier C's verdict on its registered games")
    s.add_argument("--dir", required=True)
    s.add_argument("--json")
    s.add_argument("results", nargs="+")
    s = sub.add_parser("play", help="play a list of games (needs bar.py's game runner)")
    s.add_argument("--ids", required=True)
    s.add_argument("--lanes", type=int, required=True)
    s.add_argument("--table-a", required=True)
    s.add_argument("--table-b", required=True)
    s.add_argument("--out", required=True)
    s = sub.add_parser("pieces", help="split a game list into runner pieces")
    s.add_argument("--ids", required=True)
    s.add_argument("--lanes", type=int, default=14)
    s.add_argument("--per-lane", type=int, default=0)
    s.add_argument("--json")
    s = sub.add_parser("power", help="the threshold, its power and its false-positive rate")
    s.add_argument("--n", type=int, default=TIER_C_GAMES)
    s.add_argument("--alpha", type=float, default=ALPHA)
    s.add_argument("--sim", type=int, default=0)
    s.add_argument("--seed", type=int, default=1)
    a = ap.parse_args(argv)
    try:
        return {"ids": cmd_ids, "register": cmd_register, "judge": cmd_judge, "play": cmd_play,
                "pieces": cmd_pieces, "power": cmd_power}[a.cmd](a)
    except (TourneyError, tablefile.TableError, OSError, json.JSONDecodeError) as e:
        print(f"tourney: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
