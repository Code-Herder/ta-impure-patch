#!/usr/bin/env python3
"""tune.py -- ImpAI's response table tuned in the engine: tier A's search, tier B's race, tier C's confirmation.

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

Exit 0 when the command ran, 1 when it failed or was refused, 2 when it could not run."""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import cmaes  # noqa: E402
import runlock  # noqa: E402
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
    s = sub.add_parser("plan")
    s.add_argument("--lanes", type=int, default=LANES)
    s.add_argument("--gpu", action="store_true",
                   help="the tourney.py plays it names render on the default renderer, for debugging, taking a "
                        "GPU token a lane; without it they render with GDI and take none")
    a = ap.parse_args(argv)
    try:
        return {"tierA": cmd_tier_a, "tierB": cmd_tier_b, "confirm": cmd_confirm, "plan": cmd_plan}[a.cmd](a)
    except (TuneError, tourney.TourneyError, tablefile.TableError, OSError, json.JSONDecodeError) as e:
        print(f"tune: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
