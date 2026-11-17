#!/usr/bin/env python3
"""tune.py -- ImpAI tuned in the engine: the response table's tier A search, tier B race and tier C confirmation,
the levels' persona rows on one line, and SPSA over the plan constants.

    tune.py tierA init --dir D --base TABLE --pictures FILE [--seed S] [--pop 10] [--gens 18] [--sigma 0.3]
    tune.py tierA next --dir D [--lanes L]       write the current piece's fights; say what it is
    tune.py tierA play --dir D --lanes L         play them (waits on the composition fights, below)
    tune.py tierA ingest --dir D RESULTS         take a piece's margins; advance the search
    tune.py tierA report --dir D                 each generation's best and spread
    tune.py tierA top --dir D --n 4 --out DIR    the best fully-scored tables, for tier B

    tune.py tierB init --dir D --hand TABLE --maps M1,..,M5 TABLE...   the race's 12 candidates
    tune.py tierB next --dir D [--lanes L] [--gpu]    the round's games, as runner pieces
    tune.py tierB ingest --dir D RESULTS_DIR...  take a round's games; drop; say who is left
    tune.py tierB report --dir D

    tune.py confirm register --dir D --tuned TABLE --hand TABLE --maps M1,..,M5   (tourney.py register)
    tune.py confirm judge --dir D RESULTS_DIR...                                  (tourney.py judge)

    tune.py plan [--lanes L] [--gpu]             every tier's runner pieces, for the runner

    tune.py spsa init --dir D --objective O --params P,.. [--seed S] [--iters N] [--start X] [--handicap h:<m>]
                      [--c C] [--a A]
    tune.py spsa run --dir D --dll DLL [--lanes L]   play every iteration not yet played; resumes
    tune.py spsa report --dir D

    tune.py levels-line run --dir D --dll DLL --level normal|easy [--normal-row R] [--looks 6] [--lanes 7]
    tune.py levels-line report --dir D
    tune.py levels-line simulate [--looks 6] [--games 20] [--slopes 0.05,0.1,0.2] [--runs 300] [--aim 0.95]

L defaults to LANES, the GDI lanes the run lock's whole budget holds (tools/runlock-lanes.cfg). Run with the shared
venv's interpreter (numpy). The design: research/notes/impai/brain.md, Adaptation.

TIER A searches the counter part of the table -- the eight response gains and the type choice's `counter`, nine
numbers -- by CMA-ES (cmaes.py) in a box around the base table: each number within its scale of its base value,
the scale being the value's magnitude or 1, whichever is larger. A candidate is scored on composition fights:
the army it buys for a value budget (impai_buy, the DLL's own function) against an enemy picture, fought in C1's
arena; its score is the mean, over the fights it played, of its margin minus the base table's margin on the
same picture and budget (paired, so the pictures' spread cancels). SUCCESSIVE HALVING: each generation's
candidates first play the same SCREEN fights (40 of the 120 picture-budget pairs, a permutation drawn per
generation from the seed); a candidate continues to all 120 only when its screen mean is within one standard
error of the difference from the generation's best, sqrt(se_c^2 + se_best^2), the best always continuing. Its
fitness is its mean over what it played. The incumbent's margins are played once, first.

STATE. Everything lives in D: state.json (the search, written whole after every ingest, so a reboot loses at most
the piece being played), tables/<hash>.tbl, jobs/<piece>.json (the fights a piece plays) and the results the
runner writes. A piece is one phase: the incumbent's 120 fights, then per generation its screen (pop x 40) and
its continuation (continuing x 80). The pieces of tier A follow one another; resume anywhere with `next`.

A JOB is {"piece", "lanes", "W", "fights": [{"id", "table", "table_hash", "picture", "budget"}]}; a RESULTS file
is {"fights": [{"id", "margin"} | {"id", "could_not_run": reason}]}, margin the calibration's (side A, the
candidate's army, keeping value share minus side B's: tools/tasim/metrics.py combat). A fight that could not run
is never scored: `ingest` refuses the piece until every fight has a margin.

THE COMPOSITION FIGHTS are c3's: `duelgen.py --compose` writes a fight's scenario from impai_buy's army against
the picture, through the fight library (A2a's tools/impai/fightlib.c, which must export impai_buy and the classes
beside impai_fight_predict, so the buy's type choice reads the predictor's pair rate and exponential), and
`calib.py engine --mode reload` plays them. Until they exist `play` says "could not run" (exit 2); `next` and
`ingest` work on any runner's results.

TIER B races the four best tier-A tables and two variants of each -- air_from_t, tech_m and the air classes'
base shares (the surface classes' rescaled to keep the base total) all times 1.25, and all times 0.75 -- each
against the hand table, in rounds in which every candidate still in plays each of tier B's 40 map, side and
start combinations once (tourney.py's tierB set: 20 minutes, the value rule at the cap). After a round a
candidate whose one-sided 95 % Clopper-Pearson upper bound on its win rate is below 50 % is dropped. The race
ends with one candidate left or once BUDGET_B games are played; the tuned table is the remaining candidate of
best win rate. If none remains, tier A's objective is what is wrong (its proxy rewards something games do not),
and the race says so.

THE RENDERER. The games tier B and C name are `tourney.py play`s, which render with GDI and take no GPU token;
`--gpu` on `tierB next` and `plan` puts `--gpu` in the commands they write, for a debugging run on the default
renderer. A result is compared only with results of its own renderer (research/notes/modules/impai.md).

SPSA (spsa.py) tunes the brain's plan constants, which reach a seat through the tune lever `tagpu_impai_tune.on`
(level_lever.TUNE_PARAMS, each named constant a coordinate over its range: PlanSpace; the DLL's side of it is
specified for the brain writer in .data/impai/tune-lever-spec.md, and until a DLL reads it every game could not
run). Each iteration plays one theta+ and theta- on the SAME games (common random numbers: the same ids, so the
same seeds, each arm's results apart under games/<k>-<arm>/), a new block of games each iteration. The objective O:

    margin          the arms paired on each of the margin test's block of games (margin.py's measure, the time to
                    win: +1 where theta+ was better, -1 worse, 0 a tie), the mean the difference
    handicap        A's points against OTA AI on its production times m (`h:<m>` ids, m the champion's m*)
    mirror          A's points against ImpAI Hard with no lever (`g:H.H` ids)

THE PIECES of an iteration are each arm's games of each role, at most SPSA_PIECE a piece, played by batch.py's
pieces under the run lock, each stopped past its ten minutes; a game that could not run plays its pieces once
more, and an iteration still missing one stops the run, unrun. The state (spsa.json: the configuration, the
search, every iteration with its settings and its difference) is written whole after each iteration, so a run
stopped anywhere resumes at the iteration it was in, and the same seed is the same run. A run is fixed to the DLL
it began on and is one runs-viewer run, a step an iteration. A plan it finds is accepted against the champion's
by the paired margin test, `margin.py run --test-tune ITEMS` on the champion's DLL.

THE LEVELS' LINE (`levels-line`) chooses Normal's and Easy's persona rows in one dimension: harshness t in [0, 1]
on a fixed line (line_row) from L0b, the lever's harshest row (t = 0), to Hard's (t = 1), every axis relaxing as t
grows. Normal's row is the strongest Hard beats in at least LEVEL_WIN of the mirror's games while it still takes
0.90 of the points against OTA AI at Hard; Easy's the strongest Normal's row beats so while it takes 0.50.
- THE LOOKS, the ladder's method (ladder.py): bisection on t, each look the mirror's two blocks (`m:` ids, the row on
  B; Easy's `n:`, Normal's row on A) at the bracket's midpoint, past the blocks the confirmation plays; a look where
  the stronger side won at least LEVEL_WIN of its games moves the low end up. A look whose row a look has played
  ends the looks (the bracket is inside the line's rounding).
- THE ORDER CHECK after every look: bisection and fit both need the line monotone in strength, so a pair of looks
  whose stronger row lost more, by more than LINE_MONO_Z standard errors, stops the run, NOT MONOTONE.
- THE FIT over every look's games, the ladder's logistic with its prior on the slope in t units: t90, where the
  stronger side's chance is LEVEL_WIN, and THE AIM, the row chosen where the fit puts it at LINE_AIM: at t90 itself
  the 100-game confirmation could pass only about half the time (90 of 100 at a true 0.90), so the row is chosen a
  little inside the bound (SIMULATED, `simulate`: at 0.95 the confirmation passes in 81 to 91 % of runs). A t90
  below 0: no point on the line meets the mirror's bound.
- THE CONFIRMATIONS at the row chosen: the mirror's 100 games (its run blocks; at least 90 won) and the calibration's
  40 against OTA AI (`p:` ids, the row on A; at least the floor). A row below the floor means no point on the line
  meets both bounds, since every lower t is weaker on a monotone line: the question goes back to the owner.
The state (line.json) is written after each look; a stopped run resumes where it stopped. One runs-viewer run.

Exit 0 when the command ran, 1 when it failed or was refused, 2 when it could not run; `levels-line run` exits 0
when a row met both bounds, 1 when none on the line does, the line is not monotone or the confirmation's mirror
refused the row, 2 when it could not run."""
import argparse
import hashlib
import json
import math
import random
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import bar  # noqa: E402
import batch  # noqa: E402
import cmaes  # noqa: E402
import ladder  # noqa: E402
import level_lever  # noqa: E402
import margin  # noqa: E402
import runlock  # noqa: E402
import spsa  # noqa: E402
import tablefile  # noqa: E402
import tourney  # noqa: E402

SCREEN = 40                 # fights a candidate is first scored on
FIGHT_S = 10                # lane-seconds a group fight by reload [INFERRED: the duel's 3-5 scaled by its units]
ROUND_GAMES = 40            # tier B: games a candidate plays a round
BUDGET_B = 960              # tier B: games the race may play
VARIANT = 0.25              # tier B: the variants' step
LANES = runlock.lanes_in(runlock.BUDGET, "gdi")    # the run lock's whole budget of GDI lanes


class TuneError(Exception):
    pass


def save_json(p: Path, obj) -> None:
    tmp = p.with_name(p.name + ".tmp")
    tmp.write_text(json.dumps(obj, indent=1) + "\n")
    tmp.replace(p)


def put_table(d: Path, t: tablefile.Table) -> dict:
    (d / "tables").mkdir(parents=True, exist_ok=True)
    h = t.hash_hex()
    p = d / "tables" / f"{h}.tbl"
    if not p.exists():
        tablefile.write(p, t)
    return {"path": str(p), "hash": h}


# ---- tier A: the vector and the table ----

def tier_a_space(base: tablefile.Table):
    """The nine numbers tier A searches, their base values and their scales."""
    names = [f"gain:{r}" for r in tablefile.ROWS] + ["counter"]
    centre = np.array(list(base.gains) + [base.counter], float)
    scale = np.maximum(np.abs(centre), 1.0)
    return names, centre, scale


def tier_a_table(base: tablefile.Table, x) -> tablefile.Table:
    """The table at normalised point x, projected onto the box."""
    _, centre, scale = tier_a_space(base)
    v = centre + np.clip(np.asarray(x, float), -1.0, 1.0) * scale
    t = base.copy()
    t.gains = [float(g) for g in v[:-1]]
    t.counter = float(v[-1])
    return t


# ---- successive halving ----

def mean_se(xs):
    xs = np.asarray(xs, float)
    if len(xs) == 0:
        return 0.0, math.inf
    if len(xs) == 1:
        return float(xs[0]), math.inf
    return float(xs.mean()), float(xs.std(ddof=1) / math.sqrt(len(xs)))


def halving(screen: dict) -> list:
    """The candidates that continue, from each one's screen scores {cand: [diff, ...]}: the best mean, and every
    other whose mean is within one standard error of the difference from it. Sorted."""
    stats = {c: mean_se(v) for c, v in screen.items()}
    best = max(stats, key=lambda c: (stats[c][0], -c))
    mb, sb = stats[best]
    return sorted(c for c, (m, s) in stats.items()
                  if c == best or mb - m <= math.sqrt(s * s + sb * sb))


# ---- tier A: the search's state ----

class TierA:
    def __init__(self, d: Path):
        self.d = Path(d)
        self.s = json.loads((self.d / "state.json").read_text())
        self.base = tablefile.read(self.s["base"]["path"])

    @staticmethod
    def init(d: Path, base_path: Path, keys: list, seed: int, pop: int, gens: int, sigma: float) -> "TierA":
        d = Path(d)
        if (d / "state.json").exists():
            raise TuneError(f"{d} holds a search already")
        if len(keys) < SCREEN or len(set(keys)) != len(keys):
            raise TuneError(f"the pictures give {len(keys)} distinct fights; a screen needs {SCREEN}")
        d.mkdir(parents=True, exist_ok=True)
        base = tablefile.read(base_path)
        names, _, _ = tier_a_space(base)
        es = cmaes.CMAES(len(names), pop, seed, sigma)
        s = {"seed": seed, "gens": gens, "keys": keys, "base": put_table(d, base), "names": names,
             "cma": es.state(), "phase": "incumbent", "gen": 0, "incumbent": None, "ys": None,
             "cands": None, "order": None, "history": []}
        save_json(d / "state.json", s)
        return TierA(d)

    def save(self) -> None:
        save_json(self.d / "state.json", self.s)

    def es(self) -> cmaes.CMAES:
        return cmaes.CMAES.from_state(self.s["cma"])

    def _ask(self) -> None:
        es = self.es()
        ys = es.ask()
        self.s["cma"] = es.state()          # the generator's state after the draw: resuming never redraws
        self.s["ys"] = ys.tolist()
        self.s["cands"] = []
        for y in ys:
            ref = put_table(self.d, tier_a_table(self.base, cmaes.CMAES.project(y)))
            self.s["cands"].append({"table": ref, "diffs": {}, "continued": False})
        rng = np.random.Generator(np.random.PCG64([self.s["seed"], self.s["gen"]]))
        self.s["order"] = [self.s["keys"][i] for i in rng.permutation(len(self.s["keys"]))]

    def piece_name(self) -> str:
        p = self.s["phase"]
        return "incumbent" if p == "incumbent" else (p if p == "done" else f"g{self.s['gen']:02d}-{p}")

    def job(self, lanes: int = LANES) -> dict:
        """The current piece's fights; the generation's candidates are drawn on its first call."""
        p = self.s["phase"]
        if p == "done":
            raise TuneError("the search is done")
        if p == "screen" and self.s["cands"] is None:
            self._ask()
            self.save()
        fights = []
        if p == "incumbent":
            for k in self.s["keys"]:
                fights.append(self._fight("inc", self.s["base"], k))
        else:
            keys = self.s["order"][:SCREEN] if p == "screen" else self.s["order"][SCREEN:]
            for c, cand in enumerate(self.s["cands"]):
                if p == "continue" and not cand["continued"]:
                    continue
                for k in keys:
                    fights.append(self._fight(f"g{self.s['gen']:02d}-c{c:02d}", cand["table"], k))
        job = {"piece": f"tierA-{self.piece_name()}", "lanes": lanes, "W": runlock.plan(lanes, gpu=False)[0],
               "est_s": math.ceil(len(fights) * FIGHT_S / lanes), "fights": fights}
        (self.d / "jobs").mkdir(exist_ok=True)
        save_json(self.d / "jobs" / f"{job['piece']}.json", job)
        return job

    @staticmethod
    def _fight(prefix: str, table: dict, key: str) -> dict:
        picture, budget = key.rsplit("@", 1)
        return {"id": f"{prefix}-{key}", "table": table["path"], "table_hash": table["hash"],
                "picture": picture, "budget": float(budget)}

    def ingest(self, results: dict) -> str:
        """A piece's margins {fight id: margin}; every fight of the current piece must have one. The phase it
        moved to."""
        want = [f["id"] for f in self.job()["fights"]]
        missing = [i for i in want if i not in results]
        if missing:
            raise TuneError(f"{len(missing)} fights of the piece have no margin, e.g. {missing[:3]}")
        bad = [i for i in want if not isinstance(results[i], (int, float)) or not math.isfinite(results[i])]
        if bad:
            raise TuneError(f"{len(bad)} fights' margins are not numbers, e.g. {bad[:3]}")
        p = self.s["phase"]
        if p == "incumbent":
            self.s["incumbent"] = {k: float(results[f"inc-{k}"]) for k in self.s["keys"]}
            self.s["phase"] = "screen"
        else:
            keys = self.s["order"][:SCREEN] if p == "screen" else self.s["order"][SCREEN:]
            for c, cand in enumerate(self.s["cands"]):
                if p == "continue" and not cand["continued"]:
                    continue
                for k in keys:
                    cand["diffs"][k] = float(results[f"g{self.s['gen']:02d}-c{c:02d}-{k}"]) - self.s["incumbent"][k]
            if p == "screen":
                go = halving({c: list(cand["diffs"].values()) for c, cand in enumerate(self.s["cands"])})
                for c in go:
                    self.s["cands"][c]["continued"] = True
                self.s["phase"] = "continue"
            else:
                self._tell()
        self.save()
        return self.s["phase"]

    def _tell(self) -> None:
        es = self.es()
        means = [mean_se(list(c["diffs"].values()))[0] for c in self.s["cands"]]
        es.tell(np.array(self.s["ys"]), [-m for m in means])
        full = [i for i, c in enumerate(self.s["cands"]) if c["continued"]]
        best = max(full, key=lambda i: (means[i], -i))
        self.s["history"].append({
            "gen": self.s["gen"], "best": means[best], "best_table": self.s["cands"][best]["table"]["hash"],
            "spread": float(np.std(means)), "continued": len(full), "sigma": es.sigma,
            "fights": SCREEN * len(means) + (len(self.s["keys"]) - SCREEN) * len(full),
            "cands": [{"table": c["table"], "mean": m, "fights": len(c["diffs"])}
                      for c, m in zip(self.s["cands"], means)]})
        self.s["cma"] = es.state()
        self.s["gen"] += 1
        self.s["ys"] = self.s["cands"] = self.s["order"] = None
        self.s["phase"] = "done" if self.s["gen"] >= self.s["gens"] else "screen"

    def top(self, n: int) -> list:
        """The n best tables scored on every fight, best first, each table once."""
        full = len(self.s["keys"])
        seen, out = set(), []
        rows = [c for h in self.s["history"] for c in h["cands"] if c["fights"] == full]
        for c in sorted(rows, key=lambda c: -c["mean"]):
            if c["table"]["hash"] not in seen:
                seen.add(c["table"]["hash"])
                out.append(c)
        return out[:n]


def read_margins(path: Path) -> dict:
    r = json.loads(Path(path).read_text())
    out, cnr = {}, []
    for f in r.get("fights", []):
        if "could_not_run" in f:
            cnr.append(f["id"])
        else:
            out[f["id"]] = f.get("margin")
    if cnr:
        raise TuneError(f"could not run: {len(cnr)} fights, e.g. {cnr[:3]}; play them again")
    return out


# ---- tier B: the race ----

def variants(t: tablefile.Table, f: float) -> tablefile.Table:
    v = t.copy()
    v.air_from_t *= f
    v.tech_m *= f
    air = [i for i, k in enumerate(tablefile.CLASSES) if k in tablefile.AIR]
    surface = [i for i in range(len(v.base)) if i not in air]
    air_sum, surf_sum = sum(v.base[i] for i in air), sum(v.base[i] for i in surface)
    if air_sum > 0 and surf_sum > 0:
        moved = air_sum * (f - 1)
        for i in air:
            v.base[i] = min(1.0, v.base[i] * f)
        k = max(0.0, (surf_sum - moved) / surf_sum)
        for i in surface:
            v.base[i] *= k
    return v


class TierB:
    def __init__(self, d: Path):
        self.d = Path(d)
        self.s = json.loads((self.d / "state.json").read_text())

    @staticmethod
    def init(d: Path, hand: Path, maps, tops) -> "TierB":
        d = Path(d)
        if (d / "state.json").exists():
            raise TuneError(f"{d} holds a race already")
        tourney.game_ids("tierB", maps)
        d.mkdir(parents=True, exist_ok=True)
        cands = []
        for i, p in enumerate(tops):
            t = tablefile.read(p)
            for tag, table in (("", t), ("p", variants(t, 1 + VARIANT)), ("m", variants(t, 1 - VARIANT))):
                cands.append({"name": f"t{i}{tag}", "table": put_table(d, table), "wins": 0, "games": 0,
                              "alive": True})
        s = {"hand": put_table(d, tablefile.read(hand)), "maps": list(maps), "round": 0, "played": 0,
             "cands": cands, "rounds": [], "done": False}
        save_json(d / "state.json", s)
        return TierB(d)

    def save(self) -> None:
        save_json(self.d / "state.json", self.s)

    def round_dir(self) -> Path:
        return self.d / "rounds" / f"r{self.s['round']:02d}"

    def games(self) -> dict:
        """The round's games by candidate: every candidate still in, against the hand table, on each combination
        once, the ids' repetition number the round's, so no two rounds share a game id."""
        if self.s["done"]:
            raise TuneError("the race is over")
        base = [g[:-2] + f"{self.s['round']:02d}" for g in tourney.game_ids("tierB", self.s["maps"])]
        out = {}
        for c in self.s["cands"]:
            if c["alive"]:
                tables = {"A": c["table"], "B": self.s["hand"]}
                out[c["name"]] = [tourney.game_spec(gid, tables) for gid in base]
        return out

    def pieces(self, lanes: int, gpu: bool = False) -> list:
        """The round as runner pieces, one a candidate (one table pair a `tourney.py play`, `--gpu` with `gpu`),
        W the run lock's slots of `lanes` lanes on their renderer (tourney.pieces); each writes its results into its candidate's directory of the round."""
        rd = self.round_dir()
        rd.mkdir(parents=True, exist_ok=True)
        out = []
        for name, games in self.games().items():
            ids = rd / f"{name}.ids"
            ids.write_text("\n".join(g["id"] for g in games) + "\n")
            c = next(c for c in self.s["cands"] if c["name"] == name)
            for p in tourney.pieces([g["id"] for g in games], lanes, set_name="tierB", gpu=gpu):
                p["piece"] = f"tierB-r{self.s['round']:02d}-{name}-{p['piece'][-3:]}"
                p["cmd"] = ["tools/impai/tourney.py", "play", "--ids", str(ids), "--lanes", str(lanes),
                            "--table-a", c["table"]["path"], "--table-b", self.s["hand"]["path"],
                            "--out", str(rd / name)] + (["--gpu"] if gpu else [])
                out.append(p)
        return out

    def ingest(self) -> dict:
        """The round's results, from its candidates' directories; every game must have one."""
        rd = self.round_dir()
        by = {c["name"]: c for c in self.s["cands"]}
        played = 0
        for name, games in self.games().items():
            results = tourney.read_results([rd / name]) if (rd / name).is_dir() else {}
            missing = [g["id"] for g in games if g["id"] not in results]
            if missing:
                raise TuneError(f"{name}: {len(missing)} games of the round have no result, e.g. {missing[:3]}")
            for g in games:
                r = results[g["id"]]
                if r.get("tables", {}).get("A") != g["seats"]["A"]["table_hash"] or \
                        r.get("tables", {}).get("B") != g["seats"]["B"]["table_hash"]:
                    raise TuneError(f"{name}: {g['id']} was played with other tables")
                by[name]["games"] += 1
                by[name]["wins"] += tourney.winner(r, "value") == "A"
                played += 1
        self.s["played"] += played
        dropped = []
        for c in self.s["cands"]:
            if c["alive"] and tourney.upper_bound(c["wins"], c["games"]) < 0.5:
                c["alive"] = False
                dropped.append(c["name"])
        alive = [c for c in self.s["cands"] if c["alive"]]
        self.s["rounds"].append({"round": self.s["round"], "games": played, "dropped": dropped,
                                 "standing": {c["name"]: [c["wins"], c["games"]] for c in self.s["cands"]}})
        self.s["round"] += 1
        if len(alive) <= 1 or self.s["played"] >= BUDGET_B:
            self.s["done"] = True
        self.save()
        return self.verdict()

    def verdict(self) -> dict:
        alive = [c for c in self.s["cands"] if c["alive"]]
        if not alive:
            return {"done": self.s["done"], "tuned": None, "alive": []}
        best = max(alive, key=lambda c: (c["wins"] / max(1, c["games"]), c["games"], -self.s["cands"].index(c)))
        return {"done": self.s["done"], "tuned": best["name"] if self.s["done"] else None,
                "table": best["table"] if self.s["done"] else None, "alive": [c["name"] for c in alive],
                "played": self.s["played"]}


# ---- the plan: every tier's runner pieces ----

def plan(lanes: int, gpu: bool = False) -> list:
    """Every tier's engine runs as runner pieces, W the run lock's slots of the games played at once on their
    renderer (runlock.plan), in the order they depend on one another: tier A's pieces one after another, tier B's within a round in any order, tier C's in any order."""
    W = runlock.plan(lanes, gpu=False)[0]
    WG = runlock.plan(lanes, gpu=gpu)[0]           # tiers B and C: tourney.py plays, on the GPU with `gpu`
    out = [{"tier": "A", "piece": "tierA-incumbent", "lanes": lanes, "W": W, "fights": 120,
            "est_s": math.ceil(120 * FIGHT_S / lanes), "after": None,
            "cmd": f"tools/impai/tune.py tierA play --dir DIR --lanes {lanes}"}]
    prev = "tierA-incumbent"
    for g in range(18):
        for phase, n in (("screen", 10 * SCREEN), ("continue", 5 * 80)):
            name = f"tierA-g{g:02d}-{phase}"
            out.append({"tier": "A", "piece": name, "lanes": lanes, "W": W, "fights": n,
                        "est_s": math.ceil(n * FIGHT_S / lanes), "after": prev,
                        "cmd": f"tools/impai/tune.py tierA play --dir DIR --lanes {lanes}",
                        "note": "continue's fight count is the halving's, about half the population"
                        if phase == "continue" else None})
            prev = name
    est_b = math.ceil(ROUND_GAMES / lanes) * tourney.SETS["tierB"]["est_s"]
    out.append({"tier": "B", "piece": "tierB-r00", "lanes": lanes, "W": WG, "games": 12 * ROUND_GAMES,
                "pieces": 12, "games_a_piece": ROUND_GAMES, "est_s_a_piece": est_b,
                "after": prev, "cmd": f"tools/impai/tune.py tierB next --dir DIR --lanes {lanes}{' --gpu' if gpu else ''} names each "
                                      "piece's tourney.py play; then tools/impai/tune.py tierB ingest --dir DIR",
                "note": f"later rounds: the candidates still in x {ROUND_GAMES}, until one is left or "
                        f"{BUDGET_B} games"})
    per_piece_c = (tourney.PIECE_S // tourney.SETS["tierC"]["est_s"]) * lanes
    out.append({"tier": "C", "piece": "tierC", "lanes": lanes, "W": WG, "games": tourney.TIER_C_GAMES,
                "pieces": math.ceil(tourney.TIER_C_GAMES / per_piece_c), "games_a_piece": per_piece_c,
                "after": "tierB, then tune.py confirm register",
                "cmd": f"tools/impai/tourney.py play --ids PIECE --lanes {lanes} --table-a TUNED --table-b HAND "
                       "--out DIR/games" + (" --gpu" if gpu else "")})
    return out


# ---- the commands ----

def cmd_tier_a(a) -> int:
    if a.sub == "init":
        keys = json.loads(Path(a.pictures).read_text())["keys"]
        keys = [f"{k['picture']}@{int(k['budget'])}" for k in keys]
        TierA.init(Path(a.dir), Path(a.base), keys, a.seed, a.pop, a.gens, a.sigma)
        print(f"tune: tier A in {a.dir}: {len(keys)} fights a candidate, population {a.pop}, {a.gens} generations")
        return 0
    t = TierA(Path(a.dir))
    if a.sub == "next":
        if t.s["phase"] == "done":
            print("tune: tier A is done")
            return 0
        j = t.job(a.lanes)
        print(f"tune: piece {j['piece']}: {len(j['fights'])} fights on {j['lanes']} lanes, W {j['W']}, "
              f"~{j['est_s']} s [INFERRED fight cost]; jobs/{j['piece']}.json")
        return 0
    if a.sub == "play":
        print("tune: could not run: the composition fights need duelgen.py --compose and the fight library's "
              "impai_buy (c3, on A2a's fightlib.c); `next` writes the piece's fights, `ingest` takes their margins")
        return 2
    if a.sub == "ingest":
        phase = t.ingest(read_margins(Path(a.results)))
        print(f"tune: tier A now at {t.piece_name() if phase != 'done' else 'done'}")
        return 0
    if a.sub == "report":
        for h in t.s["history"]:
            print(f"gen {h['gen']:2d}: best {h['best']:+.4f} ({h['best_table']}), spread {h['spread']:.4f}, "
                  f"{h['continued']} continued, {h['fights']} fights, sigma {h['sigma']:.3f}")
        print(f"tune: phase {t.piece_name()}")
        return 0
    if a.sub == "top":
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        for i, c in enumerate(t.top(a.n)):
            dst = out / f"top{i}.tbl"
            dst.write_bytes(Path(c["table"]["path"]).read_bytes())
            print(f"{dst}: {c['table']['hash']} mean {c['mean']:+.4f}")
        return 0
    return 2


def cmd_tier_b(a) -> int:
    if a.sub == "init":
        b = TierB.init(Path(a.dir), Path(a.hand), a.maps.split(","), [Path(p) for p in a.tables])
        print(f"tune: tier B in {a.dir}: {len(b.s['cands'])} candidates")
        return 0
    b = TierB(Path(a.dir))
    if a.sub == "next":
        ps = b.pieces(a.lanes, a.gpu)
        for p in ps:
            print(f"{p['piece']}: {p['games']} games on {p['lanes']} lanes, W {p['W']}, ~{p['est_s']} s: "
                  + " ".join(p["cmd"]))
        save_json(b.round_dir() / "pieces.json", ps)
        return 0
    if a.sub == "ingest":
        v = b.ingest()
        if v["done"] and v["tuned"] is None:
            print("tune: every candidate was dropped: tier A's proxy is what is wrong")
            return 1
        print(f"tune: still in: {' '.join(v['alive'])}" + (f"; the tuned table is {v['tuned']}" if v["done"] else ""))
        return 0
    if a.sub == "report":
        for r in b.s["rounds"]:
            print(f"round {r['round']}: {r['games']} games, dropped {' '.join(r['dropped']) or 'none'}")
        for c in b.s["cands"]:
            print(f"  {c['name']:5s} {c['wins']:3d}/{c['games']:3d} {'in' if c['alive'] else 'out'} {c['table']['hash']}")
        return 0
    return 2


def cmd_confirm(a) -> int:
    if a.sub == "register":
        return tourney.main(["register", "--dir", a.dir, "--tuned", a.tuned, "--hand", a.hand, "--maps", a.maps])
    return tourney.main(["judge", "--dir", a.dir, *a.results])


def cmd_plan(a) -> int:
    ps = plan(a.lanes, a.gpu)
    print(json.dumps(ps, indent=1))
    return 0


# ---- SPSA (tune.py spsa) ----

SPSA_STATE = "spsa.json"
SPSA_PIECE = 10             # games a piece: one block, at most ten lanes, inside the ten minutes (bar.md, the mirror)
NONE_AT = 0.98              # PERSONA SPACE: a budget or attention coordinate from here up is no limit
OBJECTIVES = ("margin", "handicap", "mirror")


class PersonaSpace:
    """THE PERSONA SPACE: the nine integers of a persona row as nine coordinates in [0, 1], 0 the lever's harshest
    and 1 Hard's (L5: looks 15 and 30, no reaction, no limits), so the box holds every row the lever accepts from
    the harshest to Hard's and every point of it decodes to one (impai_persona_ok's rules hold by construction):
    the looks in steps of 15 (economy 150..15, military 150..30), the reaction a share of the most the looks allow
    (the shorter less 15) in steps of 15, each budget's rate on a log scale from 2 (economy) or 3 (military) to
    APM_HI and none from NONE_AT, its burst 1..BURST_HI (none with no rate), each attention on a log scale from 1
    to the lever's most and none from NONE_AT."""
    names = ("look_eco", "look_mil", "react", "apm_eco", "burst_eco", "apm_mil", "burst_mil", "att_eco", "att_mil")
    APM = {"apm_eco": (2, 120), "apm_mil": (3, 240)}
    BURST_HI = 12
    ATT = {"att_eco": level_lever.IDLE_CAP, "att_mil": level_lever.SQUADS}
    LOOK = {"look_eco": 15, "look_mil": 30}

    def __init__(self):
        self.dim = len(self.names)

    @staticmethod
    def _log(x: float, lo: float, hi: float) -> int:
        return int(round(lo * (hi / lo) ** (min(x, NONE_AT) / NONE_AT)))

    @staticmethod
    def _unlog(v: float, lo: float, hi: float) -> float:
        return min(1.0, max(0.0, math.log(v / lo) / math.log(hi / lo))) * NONE_AT

    def decode(self, x) -> str:
        x = [min(1.0, max(0.0, float(v))) for v in x]
        v = {}
        for i, n in enumerate(("look_eco", "look_mil")):
            steps = (150 - self.LOOK[n]) // 15
            v[n] = 150 - 15 * int(round(x[i] * steps))
        most = min(v["look_eco"], v["look_mil"]) - 15
        v["react"] = 15 * int(round((1.0 - x[2]) * most / 15))
        for n, b, i in (("apm_eco", "burst_eco", 3), ("apm_mil", "burst_mil", 5)):
            if x[i] >= NONE_AT:
                v[n] = v[b] = 0
            else:
                v[n] = self._log(x[i], *self.APM[n])
                v[b] = 1 + int(round(x[i + 1] * (self.BURST_HI - 1)))
        for n, i in (("att_eco", 7), ("att_mil", 8)):
            v[n] = 0 if x[i] >= NONE_AT else self._log(x[i], 1, self.ATT[n])
        return ",".join(str(v[n]) for n in self.names)

    def encode(self, row: str) -> list:
        v = dict(zip(self.names, (int(t) for t in row.split(","))))
        x = [(150 - v["look_eco"]) / (150 - 15), (150 - v["look_mil"]) / (150 - 30)]
        most = min(v["look_eco"], v["look_mil"]) - 15
        x.append(1.0 - v["react"] / most if most > 0 else 1.0)
        for n, b in (("apm_eco", "burst_eco"), ("apm_mil", "burst_mil")):
            x.append(1.0 if v[n] == 0 else self._unlog(v[n], *self.APM[n]))
            x.append(0.0 if v[n] == 0 else (v[b] - 1) / (self.BURST_HI - 1))
        for n in ("att_eco", "att_mil"):
            x.append(1.0 if v[n] == 0 else self._unlog(v[n], 1, self.ATT[n]))
        return [min(1.0, max(0.0, t)) for t in x]


class PlanSpace:
    """THE PLAN SPACE: the tune lever's constants named, each a coordinate in [0, 1] over its range
    (level_lever.TUNE_PARAMS), decoded to the lever's integers. A decoded point that breaks one of TUNE_ORDER's
    orders is mended by moving the first of the pair to the nearest value that keeps it, whether or not it is
    tuned, so every point decodes to a plan impai_plan_ok accepts."""

    def __init__(self, names):
        bad = [n for n in names if n not in level_lever.TUNE_PARAMS]
        if bad or not names:
            raise TuneError(f"the tune lever has no constant {', '.join(bad)}" if bad else "name a constant to tune")
        self.names = tuple(names)
        self.dim = len(self.names)

    def decode(self, x) -> dict:
        vals = level_lever.tune_values({})
        for n, t in zip(self.names, x):
            _d, lo, hi = level_lever.TUNE_PARAMS[n]
            vals[n] = int(round(lo + min(1.0, max(0.0, float(t))) * (hi - lo)))
        for a, b, how in level_lever.TUNE_ORDER:
            top = vals[b] - 1 if how == "below" else vals[b]
            if vals[a] > top:
                vals[a] = top
        hand = level_lever.tune_values({})
        return {n: v for n, v in vals.items() if n in self.names or v != hand[n]}

    def encode(self, items: dict) -> list:
        vals = level_lever.tune_values(items)
        return [(vals[n] - lo) / (hi - lo) for n in self.names for _d, lo, hi in [level_lever.TUNE_PARAMS[n]]]


def tune_item(items: dict) -> str:
    return ",".join(f"{n}={v}" for n, v in items.items())


def spsa_roles(obj: str, k: int, handicap: str = None) -> dict:
    """{role: the game ids} iteration k plays for each arm: the same ids for theta+ and theta- (common random
    numbers), a new block each iteration, so no iteration's games are another's: the margin's the paired margin
    test's block k (cycling through its order); the handicap's block k + 1 of `h:<m>`; the mirror's of `g:H.H`."""
    def block(pair):
        return [g for g in bar.ids(pair, blocks=k + 1) if bar.block_of(g) == k + 1]
    if obj == "margin":
        bs = margin.blocks()
        return {"pairs": bs[k % len(bs)]}
    if obj == "handicap":
        return {"points": block(handicap)}
    if obj == "mirror":
        return {"points": block(bar.gauntlet_pair(bar.HARD_ROW, bar.HARD_ROW))}
    raise TuneError(f"no objective {obj!r}: {', '.join(OBJECTIVES)}")


def spsa_settings(arm: dict) -> dict:
    """{"persona": [], "tune": [...]}: the `bar.py play` items that put an arm's plan constants on ImpAI's seat A."""
    return {"persona": [], "tune": [f"A={tune_item(arm)}"]}


def spsa_points(results: dict, seat: str = "A"):
    """(the mean points of `seat` over the results, None) or (None, why) when one is missing or not scored."""
    pts = []
    for gid, r in results.items():
        if r is None:
            return None, f"{gid}: no result"
        who, why = bar.outcome_of(r)
        if who is None:
            return None, f"{gid}: {why}"
        pts.append(bar.POINTS[who] if seat == "A" else 1.0 - bar.POINTS[who])
    return sum(pts) / len(pts), None


def spsa_diff(obj: str, plus: dict, minus: dict):
    """(J(theta+) - J(theta-), detail) of an iteration from each arm's {role: {id: result}}, or (None, why). The
    margin's is the paired comparison of the two arms on each id (margin.py's measure, a game's time to win: +1
    where theta+ was better, -1 worse, 0 a tie), its mean; the others' a value of each arm."""
    if obj == "margin":
        signs = []
        for gid, rp in plus["pairs"].items():
            rm = minus["pairs"].get(gid)
            if rp is None or rm is None:
                return None, f"{gid}: no result"
            try:
                signs.append(margin.sign(margin.time_to_win(rm), margin.time_to_win(rp)))
            except margin.CouldNotJudge as e:
                return None, str(e)
        return sum(signs) / len(signs), {"better": signs.count(1), "ties": signs.count(0), "worse": signs.count(-1)}
    jp, why = spsa_points(plus["points"])
    if jp is None:
        return None, why
    jm, why = spsa_points(minus["points"])
    if jm is None:
        return None, why
    return jp - jm, {"plus": {"J": jp}, "minus": {"J": jm}}


def md5(p: Path) -> str:
    return hashlib.md5(Path(p).read_bytes()).hexdigest()


class SpsaRun:
    """A run's directory D: its state (SPSA_STATE: the configuration, the search, every iteration played, written
    whole after each), the DLL it is fixed to, and its games under games/<k>-<arm>/, the arms' games apart since
    they share their ids."""

    def __init__(self, d: Path):
        self.d = Path(d)
        self.state = json.loads((self.d / SPSA_STATE).read_text())
        cfg = self.state["config"]
        self.obj = cfg["objective"]
        self.space = PlanSpace(cfg["params"])
        self.search = spsa.SPSA.from_state(self.state["search"])

    @staticmethod
    def init(d: Path, objective: str, seed: int, iters: int, start=None, params=None, handicap=None,
             c: float = 0.15, a: float = None) -> "SpsaRun":
        d = Path(d)
        if (d / SPSA_STATE).exists():
            raise TuneError(f"{d} holds a run already: `spsa run` resumes it")
        if objective not in OBJECTIVES:
            raise TuneError(f"no objective {objective!r}: {', '.join(OBJECTIVES)}")
        space = PlanSpace(params or ())
        if start and level_lever.tune_error(1, start):
            raise TuneError(f"the start {start} {level_lever.tune_error(1, start)}")
        x0 = space.encode(start or {})
        if objective == "handicap" and not (handicap and bar.HANDICAP_RX.fullmatch(handicap)):
            raise TuneError("the handicap objective names its pair, `h:<m>` (the champion's m*, ladder.py)")
        search = spsa.SPSA(space.dim, seed, iters, x0, a=a, c=c)
        d.mkdir(parents=True, exist_ok=True)
        state = {"config": {"objective": objective, "seed": seed, "iters": iters,
                            "params": list(space.names), "handicap": handicap},
                 "search": search.state(), "iterations": [], "dll": None}
        save_json(d / SPSA_STATE, state)
        return SpsaRun(d)

    def save(self):
        self.state["search"] = self.search.state()
        save_json(self.d / SPSA_STATE, self.state)

    def settings(self, x):
        return self.space.decode(x)

    def pieces(self, k: int, plus, minus, lanes: int) -> list:
        """Iteration k's pieces: each arm's games of each role, at most SPSA_PIECE games a piece."""
        cfg = self.state["config"]
        out = []
        for arm, settings in (("plus", plus), ("minus", minus)):
            for role, ids in spsa_roles(self.obj, k, cfg.get("handicap")).items():
                items = spsa_settings(settings)
                for j in range(0, len(ids), SPSA_PIECE):
                    out.append({"name": f"k{k:03d}-{arm}-{role}-{j // SPSA_PIECE}", "setup": None, "gating": False,
                                "lanes": min(lanes, SPSA_PIECE), "ids": ids[j:j + SPSA_PIECE],
                                "out": self.d / "games" / f"k{k:03d}-{arm}", "arm": arm, "role": role, **items})
        return out

    def results(self, ps: list) -> dict:
        """{arm: {role: {id: result or None}}} of the pieces' games."""
        got = {"plus": {}, "minus": {}}
        for p in ps:
            for gid in p["ids"]:
                try:
                    r = json.loads((Path(p["out"]) / f"{gid}.json").read_text())
                except (OSError, ValueError):
                    r = None
                got[p["arm"]].setdefault(p["role"], {})[gid] = r
        return got


def spsa_play(ps: list, launch) -> list:
    """The pieces played (batch.play_stage): the names of those stopped past the deadline."""
    _failed, overran = batch.play_stage(ps, launch, time.monotonic, time.sleep, lambda: None)
    return overran


def spsa_run(d: Path, dll: Path, lanes: int, launch=None, play=spsa_play) -> int:
    """THE RUN: every iteration not yet played, each played whole and told to the search before the next, the
    state written after each, so a run stopped anywhere resumes at the iteration it was in (its games kept,
    bar.py's game_kept). An iteration with a game that could not run or a cap not scored plays its pieces once
    more; still missing, the run stops, unrun, exit 2. One runs-viewer run, a step an iteration."""
    import tarun
    run = SpsaRun(d)
    dll = Path(dll)
    if run.state.get("dll") and run.state["dll"] != md5(dll):
        print(f"tune spsa: COULD NOT RUN: {dll} is another DLL than the one the run began on ({run.state['dll']})")
        return 2
    run.state["dll"] = md5(dll)
    run.save()
    launch = launch or (lambda p: batch.Piece(p, dll, run.d / "logs"))
    iters = run.search.iters
    jr = tarun.begin("tune", [("spsa", [{"id": f"iteration {k}"} for k in range(iters)])],
                     title=f"spsa {run.obj} {run.d.name}", pools={"main": 1}, gate=True)
    for it in run.state["iterations"]:
        jr.finish("spsa", f"iteration {it['k']}", "pass", f"diff {it['diff']:+.3f}")
    try:
        while not run.search.done():
            k = run.search.k
            xp, xm, delta = run.search.ask()
            plus, minus = run.settings(xp), run.settings(xm)
            ps = run.pieces(k, plus, minus, lanes)
            jr.start("spsa", f"iteration {k}")
            print(f"tune spsa: iteration {k}: {sum(len(p['ids']) for p in ps)} games, theta+ {plus}, theta- {minus}")
            overran = play(ps, launch)
            diff, detail = spsa_diff(run.obj, *(run.results(ps)[a] for a in ("plus", "minus")))
            if diff is None and not overran:
                print(f"tune spsa: iteration {k} played once more ({detail})")
                overran = play(ps, launch)
                diff, detail = spsa_diff(run.obj, *(run.results(ps)[a] for a in ("plus", "minus")))
            if diff is None:
                why = f"pieces past {batch.DEADLINE} s: {', '.join(overran)}" if overran else detail
                jr.finish("spsa", f"iteration {k}", "unrun", why)
                jr.end("unrun")
                print(f"tune spsa: COULD NOT RUN: iteration {k} lacks a game ({why}); `spsa run` goes on from it")
                return 2
            theta = run.search.theta.tolist()
            step = run.search.tell(diff)
            run.state["iterations"].append({"k": k, "theta": theta, "delta": delta.tolist(), "plus": plus,
                                            "minus": minus, "diff": diff, "detail": detail,
                                            "step": step.tolist(), "after": run.settings(run.search.theta)})
            run.save()
            jr.finish("spsa", f"iteration {k}", "pass", f"diff {diff:+.3f}")
            print(f"tune spsa: iteration {k}: diff {diff:+.3f}; now {run.settings(run.search.theta)}")
    except KeyboardInterrupt:
        jr.end("stopped")
        return 2
    jr.end("pass")
    print(spsa_report(run))
    return 0


def spsa_report(run: "SpsaRun") -> str:
    """The run so far: each iteration's difference and the plan constants the search holds now."""
    cfg = run.state["config"]
    lines = [f"spsa {run.obj}: {run.search.k} of {run.search.iters} iterations, seed {cfg['seed']}"]
    for it in run.state["iterations"]:
        lines.append(f"  {it['k']:3d}  diff {it['diff']:+.3f}  {json.dumps(it['detail'], sort_keys=True)}")
    now = run.settings(run.search.theta)
    lines.append(f"now: {now}; accepted against the champion by `margin.py run --test-tune {tune_item(now)}`")
    return "\n".join(lines)


def cmd_spsa(a) -> int:
    if a.sub == "init":
        start = {n: int(v) for n, _, v in (kv.partition("=") for kv in (a.start or "").split(",") if kv)}
        params = [p for p in (a.params or "").split(",") if p]
        run = SpsaRun.init(Path(a.dir), a.objective, a.seed, a.iters, start=start, params=params,
                           handicap=a.handicap, c=a.c, a=a.a)
        print(f"tune spsa: {a.dir}: {a.objective}, {run.search.iters} iterations, starting at "
              f"{run.settings(run.search.theta)}")
        return 0
    if a.sub == "run":
        return spsa_run(Path(a.dir), Path(a.dll), a.lanes)
    print(spsa_report(SpsaRun(Path(a.dir))))
    return 0


# ---- THE LEVELS' LINE (tune.py levels-line) ----

LINE_STATE = "line.json"
LEVEL_WIN = 0.90            # the levels: the stronger side wins at least this share of the mirror's games
LINE_LOOKS = 6
LINE_BLOCKS = 2             # a look's blocks of the mirror: 20 games
LINE_LANES = 7              # a piece's lanes: two pieces at once hold the lock's 28 slots
LINE_PRIOR_S = 0.08         # the fit's prior median slope, in t a logit unit
LINE_MONO_Z = 2.576         # a look pair breaks the line's order past this z (one-sided 0.5 %)
LN9 = math.log(9.0)         # p(t) = LEVEL_WIN where (t - c) / s = -ln 9
LINE_AIM = 0.95             # THE AIM: the row chosen is where the fit puts the stronger side's chance here
LEVELS = {"normal": {"pair": bar.MIRROR, "floor": 0.90},       # the row's points against OTA AI at Hard, at least
          "easy": {"pair": bar.NORMAL_MIRROR, "floor": 0.50}}


def line_row(t: float) -> str:
    """THE LINE: the persona row at harshness t in [0, 1], the persona space's diagonal from L0b (t = 0, every
    coordinate 0, the lever's harshest) to L5 (t = 1, Hard's), so every axis relaxes as t grows and every point is a
    row impai_persona_ok accepts. The raw integers are not interpolated: a budget or attention of 0 is no limit,
    so a straight line from L0b's 2 actions a minute to Hard's 0 would pass through 1, harsher than either end."""
    return PersonaSpace().decode([min(1.0, max(0.0, t))] * len(PersonaSpace.names))


def line_bracket(looks: list) -> tuple:
    """(lo, hi) the finished looks leave: a look where the stronger side won at least LEVEL_WIN of its games moves
    lo up to its t, any other moves hi down."""
    lo, hi = 0.0, 1.0
    for lk in looks:
        if lk["wins"] >= LEVEL_WIN * lk["games"]:
            lo = max(lo, lk["t"])
        else:
            hi = min(hi, lk["t"])
    return lo, hi


def line_next(looks: list):
    """(t, row) of the next look, the bracket's midpoint; None when its row is one a look has played, the bracket
    closed below the line's resolution."""
    lo, hi = line_bracket(looks)
    t = (lo + hi) / 2
    row = line_row(t)
    return None if any(lk["row"] == row for lk in looks) else (t, row)


def line_fit(ts: list, ys: list, aim: float = LINE_AIM) -> dict:
    """THE FIT over every game: p(t) = 1 / (1 + exp((t - c) / s)), p the stronger side's chance of a win, with the
    ladder's prior on log s (median LINE_PRIOR_S); t90, where p is LEVEL_WIN, = c - s ln 9; t_aim, where p is
    `aim`, = c - s ln(aim / (1 - aim)), and its standard error by the delta method on the curvature at the minimum
    (None where that is not positive definite)."""
    if not ts:
        raise TuneError("no game to fit")
    f = lambda c, u: ladder.nll(c, u, ts, ys, LINE_PRIOR_S)  # noqa: E731
    c, u = ladder._minimize(f, (sum(ts) / len(ts), math.log(LINE_PRIOR_S)), step=0.1)
    h = 1e-4
    fcc = (f(c + h, u) - 2 * f(c, u) + f(c - h, u)) / h ** 2
    fuu = (f(c, u + h) - 2 * f(c, u) + f(c, u - h)) / h ** 2
    fcu = (f(c + h, u + h) - f(c + h, u - h) - f(c - h, u + h) + f(c - h, u - h)) / (4 * h * h)
    det = fcc * fuu - fcu * fcu
    s = math.exp(u)
    se = None
    if det > 0 and fcc > 0:
        icc, iuu, icu = fuu / det, fcc / det, -fcu / det          # the inverse of the curvature
        g = (1.0, -s * math.log(aim / (1 - aim)))                  # d t_aim / d (c, u)
        var = g[0] ** 2 * icc + g[1] ** 2 * iuu + 2 * g[0] * g[1] * icu
        se = math.sqrt(var) if var > 0 else None
    return {"c": c, "s": s, "t90": c - s * LN9, "t_aim": c - s * math.log(aim / (1 - aim)), "aim": aim, "se": se}


def line_breaks(looks: list) -> list:
    """THE ORDER CHECK: the look pairs (t_i < t_j, two rows) where the stronger side won a larger share at the
    stronger row t_j, by more than LINE_MONO_Z standard errors of the difference. On a line monotone in strength
    none is expected; one stops the run, since the bisection and the fit both rest on the order."""
    out = []
    for a in looks:
        for b in looks:
            if a["t"] < b["t"] and a["row"] != b["row"]:
                pa, pb = a["wins"] / a["games"], b["wins"] / b["games"]
                p = (a["wins"] + b["wins"]) / (a["games"] + b["games"])
                se = math.sqrt(max(p * (1 - p), 1e-9) * (1 / a["games"] + 1 / b["games"]))
                if (pb - pa) / se > LINE_MONO_Z:
                    out.append((a["t"], pa, b["t"], pb))
    return out


def line_game_ids(level: str, k: int) -> list:
    """Look k's ids (from 1): blocks past the mirror's run set, which the confirmation plays, so no look shares a
    seed with it or with another look."""
    pair = LEVELS[level]["pair"] + bar.NORMAL_ROW
    first = bar.MIRROR_BLOCKS + LINE_BLOCKS * (k - 1) + 1
    want = set(range(first, first + LINE_BLOCKS))
    return [g for g in bar.ids(pair, blocks=first + LINE_BLOCKS - 1) if bar.block_of(g) in want]


def line_items(level: str, role: str, row: str, normal_row: str = None) -> list:
    if role == "ota":
        return [f"A={row}"]
    return ([f"A={normal_row}"] if level == "easy" else []) + [f"B={row}"]


def line_pieces(name: str, ids: list, items: list, out: Path, lanes: int) -> list:
    return [{"name": f"{name}-{j // SPSA_PIECE}", "setup": None, "gating": False, "lanes": lanes,
             "ids": ids[j:j + SPSA_PIECE], "out": out, "persona": items, "tune": []}
            for j in range(0, len(ids), SPSA_PIECE)]


def line_read(ids: list, out: Path):
    """({id: who won}, None) of the games, or (None, why) when one is missing, could not run or is not scored."""
    got = {}
    for gid in ids:
        try:
            r = json.loads((out / f"{gid}.json").read_text())
        except (OSError, ValueError):
            return None, f"{gid}: no result"
        who, why = bar.outcome_of(r)
        if who is None:
            return None, f"{gid}: {why}"
        got[gid] = who
    return got, None


def line_play(ps: list, launch, play, out: Path, ids: list):
    """The pieces played, and once more when a game lacks a result: (results, None) or (None, why)."""
    overran = play(ps, launch)
    got, why = line_read(ids, out)
    if got is None and not overran:
        print(f"tune levels-line: played once more ({why})")
        overran = play(ps, launch)
        got, why = line_read(ids, out)
    if got is None:
        return None, f"pieces past {batch.DEADLINE} s: {', '.join(overran)}" if overran else why
    return got, None


def line_run(d: Path, dll: Path, level: str, normal_row: str = None, looks: int = LINE_LOOKS,
             lanes: int = LINE_LANES, launch=None, play=spsa_play) -> int:
    """THE RUN of a level (the module's docstring, THE LEVELS' LINE): the looks, the fit, the confirmations.
    Exit 0 a row met both bounds, 1 none on the line does (or the line is not monotone, or the confirmation's
    mirror refused the row: the owner's question), 2 could not run; a stopped run resumes where it stopped."""
    import tarun
    d, dll = Path(d), Path(dll)
    st_path = d / LINE_STATE
    if level not in LEVELS:
        raise TuneError(f"no level {level!r}: normal or easy")
    if level == "easy":
        normal_row = bar.PERSONA_ROWS.get(normal_row or bar.NORMAL_ROW, normal_row)
        why = level_lever.persona_error(f"1={normal_row}")
        if why:
            raise TuneError(f"Normal's row {normal_row} {why}")
    try:
        st = json.loads(st_path.read_text())
    except (OSError, ValueError):
        st = {"level": level, "normal_row": normal_row if level == "easy" else None, "dll": None, "looks": []}
    if (st["level"], st["normal_row"]) != (level, normal_row if level == "easy" else None):
        raise TuneError(f"{d} is a run of {st['level']} (Normal's row {st['normal_row']}), not of {level}")
    if st["dll"] and st["dll"] != md5(dll):
        print(f"tune levels-line: COULD NOT RUN: {dll} is another DLL than the one the run began on")
        return 2
    d.mkdir(parents=True, exist_ok=True)
    st["dll"] = md5(dll)
    save_json(st_path, st)
    launch = launch or (lambda p: batch.Piece(p, dll, d / "logs"))
    games = d / "games"
    floor = LEVELS[level]["floor"]
    jr = tarun.begin("tune", [("levels-line", [{"id": f"look {k}"} for k in range(1, looks + 1)]
                               + [{"id": "confirm"}])],
                     title=f"levels-line {level} {d.name}", pools={"main": 1}, gate=True)
    try:
        for lk in st["looks"]:
            jr.finish("levels-line", f"look {lk['k']}", "pass", f"t {lk['t']:.3f}: {lk['wins']}/{lk['games']}")
        while len(st["looks"]) < looks:
            k = len(st["looks"]) + 1
            nxt = line_next(st["looks"])
            if nxt is None:
                print(f"tune levels-line: look {k} would play a row again: the bracket is inside the line's "
                      f"resolution; the fit takes the {k - 1} looks played")
                for j in range(k, looks + 1):
                    jr.finish("levels-line", f"look {j}", "skipped", "the bracket closed")
                break
            t, row = nxt
            ids = line_game_ids(level, k)
            out = games / f"look-{k}"
            jr.start("levels-line", f"look {k}")
            print(f"tune levels-line: look {k}: t {t:.4f}, row {row}, {len(ids)} games")
            got, why = line_play(line_pieces(f"look-{k}", ids, line_items(level, "mirror", row, normal_row), out,
                                             lanes), launch, play, out, ids)
            if got is None:
                jr.finish("levels-line", f"look {k}", "unrun", why)
                jr.end("unrun")
                print(f"tune levels-line: COULD NOT RUN: look {k} lacks a game ({why}); `run` again goes on from it")
                return 2
            wins = sum(1 for w in got.values() if w == "A")
            st["looks"].append({"k": k, "t": t, "row": row, "ids": ids, "wins": wins, "games": len(ids)})
            save_json(st_path, st)
            jr.finish("levels-line", f"look {k}", "pass", f"t {t:.3f}: {wins}/{len(ids)}")
            print(f"tune levels-line: look {k}: the stronger side won {wins} of {len(ids)}")
            breaks = line_breaks(st["looks"])
            if breaks:
                st["verdict"] = "not monotone"
                save_json(st_path, st)
                jr.end("fail")
                print(line_report(st))
                return 1
        ts = [lk["t"] for lk in st["looks"] for _ in range(lk["games"])]
        ys = [1.0 if w == "A" else 0.0 for lk in st["looks"]
              for w in line_read(lk["ids"], games / f"look-{lk['k']}")[0].values()]
        st["fit"] = line_fit(ts, ys)
        if st["fit"]["t90"] < 0.0:
            st["verdict"] = "below the line"
            save_json(st_path, st)
            jr.finish("levels-line", "confirm", "skipped", "no point on the line meets the mirror's bound")
            jr.end("fail")
            print(line_report(st))
            return 1
        t = min(1.0, max(0.0, st["fit"]["t_aim"]))
        row = line_row(t)
        st["chosen"] = {"t": t, "row": row}
        save_json(st_path, st)
        jr.start("levels-line", "confirm")
        mids = bar.ids(LEVELS[level]["pair"] + bar.NORMAL_ROW)
        oids = bar.ids(bar.CALIBRATION + bar.NORMAL_ROW)
        mout, oout = games / "confirm-mirror", games / "confirm-ota"
        ps = (line_pieces("confirm-mirror", mids, line_items(level, "mirror", row, normal_row), mout, lanes)
              + line_pieces("confirm-ota", oids, line_items(level, "ota", row), oout, lanes))
        overran = play(ps, launch)
        mgot, why = line_read(mids, mout)
        ogot, owhy = line_read(oids, oout)
        if (mgot is None or ogot is None) and not overran:
            print(f"tune levels-line: the confirmation played once more ({why or owhy})")
            overran = play(ps, launch)
            mgot, why = line_read(mids, mout)
            ogot, owhy = line_read(oids, oout)
        if mgot is None or ogot is None:
            why = f"pieces past {batch.DEADLINE} s: {', '.join(overran)}" if overran else (why or owhy)
            jr.finish("levels-line", "confirm", "unrun", why)
            jr.end("unrun")
            print(f"tune levels-line: COULD NOT RUN: the confirmation lacks a game ({why}); `run` again goes on")
            return 2
        mwins = sum(1 for w in mgot.values() if w == "A")
        opts = sum(bar.POINTS[w] for w in ogot.values()) / len(ogot)
        st["confirm"] = {"mirror_wins": mwins, "mirror_games": len(mgot), "ota_points": opts,
                         "ota_games": len(ogot), "floor": floor}
        mirror_ok = mwins >= LEVEL_WIN * len(mgot)
        st["verdict"] = ("chosen" if mirror_ok and opts >= floor else
                         "the mirror refused the row" if not mirror_ok else "below the floor")
        save_json(st_path, st)
        jr.finish("levels-line", "confirm", "pass" if st["verdict"] == "chosen" else "fail",
                  f"{mwins}/{len(mgot)}, OTA AI {opts:.3f}")
        jr.end("pass" if st["verdict"] == "chosen" else "fail")
        print(line_report(st))
        return 0 if st["verdict"] == "chosen" else 1
    except KeyboardInterrupt:
        jr.end("stopped")
        return 2


def line_report(st: dict) -> str:
    level = st["level"]
    who = "Hard" if level == "normal" else f"Normal's row {st['normal_row']}"
    lines = [f"levels-line {level}: the row {who} beats in at least {round(100 * LEVEL_WIN)} % of the mirror's games "
             f"and that scores {LEVELS[level]['floor']:.2f} against OTA AI at Hard"]
    for lk in sorted(st["looks"], key=lambda x: x["t"]):
        lines.append(f"  look {lk['k']}: t {lk['t']:.4f}  {lk['row']:<28}  {who} won {lk['wins']:2d} of {lk['games']}")
    breaks = line_breaks(st["looks"])
    lines.append("the order: " + ("BROKEN, " + "; ".join(f"t {a:.3f} won {pa:.2f} < t {b:.3f} won {pb:.2f}"
                                                         for a, pa, b, pb in breaks)
                                 + ": the line is not monotone in strength, so neither the bisection nor the fit "
                                   "holds; the question goes back to the owner" if breaks else
                                 "every look's share falls as t grows, within the noise (monotone as measured)"))
    fr = st.get("fit")
    if fr:
        band = f" +- {ladder.Z90 * fr['se']:.3f} (90 %)" if fr.get("se") else ""
        lines.append(f"the fit: t90 {fr['t90']:.4f}, the aim {fr['aim']:.2f} at t {fr['t_aim']:.4f}{band}, slope "
                     f"{fr['s']:.4f}")
    v = st.get("verdict")
    if v == "below the line":
        lines.append(f"NO ROW: {who} wins less than {round(100 * LEVEL_WIN)} % even at t = 0, the lever's harshest "
                     f"row: no point on the line meets the mirror's bound; the question goes back to the owner")
    if st.get("confirm"):
        c, ch = st["confirm"], st["chosen"]
        lines.append(f"the row at t {ch['t']:.4f}: {ch['row']}: {who} won {c['mirror_wins']} of {c['mirror_games']}; "
                     f"against OTA AI it took {c['ota_points']:.3f} of the points over {c['ota_games']}")
        lines.append({"chosen": f"CHOSEN: {ch['row']}",
                      "below the floor": f"NO ROW: at the largest t {who} still beats {round(100 * LEVEL_WIN)} %, the "
                                         f"row takes {c['ota_points']:.3f} < {c['floor']:.2f} against OTA AI, and every "
                                         f"lower t is weaker on a monotone line: no point on the line meets both "
                                         f"bounds; the question goes back to the owner",
                      "the mirror refused the row": f"NOT CHOSEN: the confirmation's mirror gave {who} "
                                                    f"{c['mirror_wins']} of {c['mirror_games']}: the fit placed the "
                                                    f"row too strong"}[v])
    return "\n".join(lines)


def line_simulate(looks: int, games: int, slope: float, runs: int, seed: int = 1, aim: float = LINE_AIM) -> dict:
    """The method on simulated games, as ladder.simulate: the true t90 uniform in [0.15, 0.85], the stronger side's
    chance p(t) logistic of slope `slope`; each look plays `games` games at the bracket's midpoint (rows as the
    line rounds them: a row played again closes the bracket). {rmse, p90, mid_rmse} of t90 against the truth, the
    mean true win share at the t chosen and the share of runs whose 100-game confirmation passes (90 of 100)."""
    rng = random.Random(seed)
    err, mid, p_at, conf = [], [], [], []
    for _ in range(runs):
        t90 = rng.uniform(0.15, 0.85)
        c = t90 + slope * LN9
        done, ts, ys = [], [], []
        for k in range(looks):
            nxt = line_next(done)
            if nxt is None:
                break
            t, row = nxt
            p = 1.0 / (1.0 + math.exp((t - c) / slope))
            wins = sum(rng.random() < p for _ in range(games))
            ts += [t] * games
            ys += [1.0] * wins + [0.0] * (games - wins)
            done.append({"k": k + 1, "t": t, "row": row, "wins": wins, "games": games})
        fr = line_fit(ts, ys, aim)
        err.append(fr["t90"] - t90)
        lo, hi = line_bracket(done)
        mid.append((lo + hi) / 2 - t90)
        pt = 1.0 / (1.0 + math.exp((min(1.0, max(0.0, fr["t_aim"])) - c) / slope))
        p_at.append(pt)
        conf.append(sum(math.comb(100, j) * pt ** j * (1 - pt) ** (100 - j) for j in range(90, 101)))
    rmse = lambda e: math.sqrt(sum(v * v for v in e) / len(e))  # noqa: E731
    a = sorted(abs(v) for v in err)
    return {"rmse": rmse(err), "p90": a[int(0.9 * (len(a) - 1))], "mid_rmse": rmse(mid),
            "p_at": sum(p_at) / len(p_at), "confirm": sum(conf) / len(conf), "games": looks * games + 140}


def cmd_levels_line(a) -> int:
    if a.sub == "run":
        return line_run(Path(a.dir), Path(a.dll), a.level, a.normal_row, a.looks, a.lanes)
    if a.sub == "report":
        print(line_report(json.loads((Path(a.dir) / LINE_STATE).read_text())))
        return 0
    for s in a.slopes:
        r = line_simulate(a.looks, a.games, s, a.runs, a.seed, a.aim)
        print(f"slope {s:.3f}: t90 rmse {r['rmse']:.4f} (90 % within {r['p90']:.4f}; the bracket alone "
              f"{r['mid_rmse']:.4f}); the true win share at the t chosen {r['p_at']:.3f}; the 100-game mirror "
              f"confirms {100 * r['confirm']:.0f} % of runs; {r['games']} games a level")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="tune.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    ta = sub.add_parser("tierA").add_subparsers(dest="sub", required=True)
    s = ta.add_parser("init")
    s.add_argument("--dir", required=True)
    s.add_argument("--base", required=True)
    s.add_argument("--pictures", required=True)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--pop", type=int, default=10)
    s.add_argument("--gens", type=int, default=18)
    s.add_argument("--sigma", type=float, default=0.3)
    for name in ("next", "play"):
        s = ta.add_parser(name)
        s.add_argument("--dir", required=True)
        s.add_argument("--lanes", type=int, default=LANES)
    s = ta.add_parser("ingest")
    s.add_argument("--dir", required=True)
    s.add_argument("results")
    s = ta.add_parser("report")
    s.add_argument("--dir", required=True)
    s = ta.add_parser("top")
    s.add_argument("--dir", required=True)
    s.add_argument("--n", type=int, default=4)
    s.add_argument("--out", required=True)
    tb = sub.add_parser("tierB").add_subparsers(dest="sub", required=True)
    s = tb.add_parser("init")
    s.add_argument("--dir", required=True)
    s.add_argument("--hand", required=True)
    s.add_argument("--maps", required=True)
    s.add_argument("tables", nargs="+")
    s = tb.add_parser("next")
    s.add_argument("--dir", required=True)
    s.add_argument("--lanes", type=int, default=LANES)
    s.add_argument("--gpu", action="store_true",
                   help="the tourney.py plays it names render on the default renderer, for debugging, taking a "
                        "GPU token a lane; without it they render with GDI and take none")
    s = tb.add_parser("ingest")
    s.add_argument("--dir", required=True)
    s.add_argument("results", nargs="+")
    s = tb.add_parser("report")
    s.add_argument("--dir", required=True)
    tc = sub.add_parser("confirm").add_subparsers(dest="sub", required=True)
    s = tc.add_parser("register")
    s.add_argument("--dir", required=True)
    s.add_argument("--tuned", required=True)
    s.add_argument("--hand", required=True)
    s.add_argument("--maps", required=True)
    s = tc.add_parser("judge")
    s.add_argument("--dir", required=True)
    s.add_argument("results", nargs="+")
    sp = sub.add_parser("spsa").add_subparsers(dest="sub", required=True)
    s = sp.add_parser("init")
    s.add_argument("--dir", required=True)
    s.add_argument("--objective", required=True, choices=OBJECTIVES)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--iters", type=int, default=40)
    s.add_argument("--start", default=None, help="name=int,...: where the search starts in place of the "
                                                  "hand-written values")
    s.add_argument("--params", required=True, help="the plan constants tuned, comma-separated")
    s.add_argument("--handicap", default=None, help="handicap: the pair h:<m> played, m the champion's m*")
    s.add_argument("--c", type=float, default=0.15)
    s.add_argument("--a", type=float, default=None)
    s = sp.add_parser("run")
    s.add_argument("--dir", required=True)
    s.add_argument("--dll", required=True)
    s.add_argument("--lanes", type=int, default=SPSA_PIECE)
    s = sp.add_parser("report")
    s.add_argument("--dir", required=True)
    ll = sub.add_parser("levels-line").add_subparsers(dest="sub", required=True)
    s = ll.add_parser("run")
    s.add_argument("--dir", required=True)
    s.add_argument("--dll", required=True)
    s.add_argument("--level", required=True, choices=sorted(LEVELS))
    s.add_argument("--normal-row", default=None, help="easy: Normal's row, the stronger side of its mirror")
    s.add_argument("--looks", type=int, default=LINE_LOOKS)
    s.add_argument("--lanes", type=int, default=LINE_LANES)
    s = ll.add_parser("report")
    s.add_argument("--dir", required=True)
    s = ll.add_parser("simulate")
    s.add_argument("--looks", type=int, default=LINE_LOOKS)
    s.add_argument("--games", type=int, default=LINE_BLOCKS * 10)
    s.add_argument("--slopes", type=lambda v: [float(x) for x in v.split(",")], default=[0.05, 0.1, 0.2])
    s.add_argument("--runs", type=int, default=300)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--aim", type=float, default=LINE_AIM)
    s = sub.add_parser("plan")
    s.add_argument("--lanes", type=int, default=LANES)
    s.add_argument("--gpu", action="store_true",
                   help="the tourney.py plays it names render on the default renderer, for debugging, taking a "
                        "GPU token a lane; without it they render with GDI and take none")
    a = ap.parse_args(argv)
    try:
        return {"tierA": cmd_tier_a, "tierB": cmd_tier_b, "confirm": cmd_confirm, "plan": cmd_plan,
                "spsa": cmd_spsa, "levels-line": cmd_levels_line}[a.cmd](a)
    except (TuneError, tourney.TourneyError, tablefile.TableError, bar.BarError, OSError,
            json.JSONDecodeError) as e:
        print(f"tune: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
