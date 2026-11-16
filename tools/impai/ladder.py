#!/usr/bin/env python3
"""ladder.py -- the handicap ladder: OTA AI's income multiplier at which ImpAI Hard falls to half the points.

    tools/impai/ladder.py run --dll DLL --tag TAG [--looks N] [--lanes L] [--out DIR]
    tools/impai/ladder.py fit DIR
    tools/impai/ladder.py simulate [--looks N] [--games G] [--slope S] [--tie T] [--runs R] [--seed K]

THE NUMBER. Once ImpAI wins every fair game, wins measure nothing; the ladder's number keeps moving. It is m*, the
multiplier on OTA AI's production (the income lever, tagpu/ddraw/src/tagpu_income.c, played as bar.py's handicap
pairs `h:<m>`) at which ImpAI Hard takes half the points on the bar's five maps: a game won 1, drawn 0.5, lost 0
(bar.py's POINTS, a game at the cap scored by value). A stronger ImpAI holds against a richer OTA AI, so m* rises.

THE METHOD: BISECTION, THEN A LOGISTIC FIT OVER EVERY GAME. Work in x = log2(m), the bracket [LO, HI] = [0, 3] (m 1
to 8, the lever's range). A look plays bar.py's two blocks of the pair at the bracket's midpoint (20 games: every
map from both starts, every side pairing), its multiplier rounded to the lever's three places; ImpAI's points of
at least one half move the bracket's low end up to it, less move the high end down. After the last look, every
game of every look is fitted with p(x) = 1 / (1 + exp((x - c) / s)), points as fractional outcomes, with a weak
prior on log s (median PRIOR_S, one unit of spread), which keeps the fit finite when the looks separate wins
from losses outright; m* = 2^c and its 90 % interval from the fit's curvature. The bracket's midpoint is the
placement, never the answer: SIMULATED (`simulate`, 300 runs a cell, x* uniform in [0.3, 2.7], ties 0.3), six
looks of 20 games put m* within 4 % at slope 0.3 (RMSE 0.057 in log2), 2 % at 0.15 (0.030) and 6.5 % at 0.5
(0.091), where the bracket's last midpoint alone is 0.091, 0.049 and 0.136; looks of one block (10 games) cost
half and give 0.078, 0.042 and 0.122 (research/notes/impai/bar.md, *The handicap ladder*, with the fixed grid
the method was chosen over).

HOW IT RUNS. Each look is two pieces, one `bar.py play` a block on LANES lanes -- one wave of ten games, which
ends well inside batch.py's DEADLINE (bar.py games at the cap took at most 346 s on the mirror's run) -- launched
in order through batch.py's play_stage, which stops a piece past its deadline. The DLL is copied into the run's
directory at its first look and a run whose copy differs is refused, so one ladder is one build (bar.py's resume
does not check the DLL). The run's state is <out>/ladder.json: every look's multiplier, ids and points, written
after each look, so `run` again goes on from the first look not finished, its finished games kept. A look with a
game that could not run, or a game at the cap not scored, is played once more; still lacking, the run stops,
could not judge. The results go to <out>/games/, each piece's log to <out>/logs/, the fit to stdout and
<out>/fit.txt. It is one runs-viewer run with a step a look (tools/tarun.py).

Exit 0 the fit found m* (inside or at the edge of the lever's range, said which), 2 could not judge."""
import argparse
import hashlib
import json
import math
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import bar  # noqa: E402
import batch  # noqa: E402

LO, HI = 0.0, 3.0               # the bracket in log2(m): m 1..8, the income lever's range
LOOKS = 6
LANES = 10                      # a block's ten games in one wave
PRIOR_S = 0.4                   # the fit's prior median slope, in log2(m) per logit unit
PRIOR_SD = 1.0                  # its spread, in log s
Z90 = 1.6449
STATE = "ladder.json"


class CouldNotJudge(Exception):
    pass


# ---- the method ----

def next_x(looks: list) -> float:
    """The next look's x: the midpoint of the bracket the finished `looks` leave, each {x, points}."""
    lo, hi = LO, HI
    for lk in looks:
        if lk["points"] >= 0.5:
            lo = max(lo, lk["x"])
        else:
            hi = min(hi, lk["x"])
    return (lo + hi) / 2


def mult_of(x: float) -> float:
    """The lever's multiplier for x, three places, inside 1..8."""
    return min(8.0, max(1.0, round(2.0 ** x, 3)))


def next_mult(looks: list) -> float | None:
    """The next look's multiplier, or None when the bracket has closed below the lever's resolution: a multiplier a
    look has already played is never played again, so a run never spends games on a look it has."""
    m = mult_of(next_x(looks))
    return None if any(lk["mult"] == m for lk in looks) else m


def p_win(x: float, c: float, s: float) -> float:
    z = (x - c) / s
    return 1.0 / (1.0 + math.exp(z)) if z < 700 else 0.0


def nll(c: float, u: float, xs: list, ys: list) -> float:
    """The negative quasi-log-likelihood of points `ys` at `xs` under (c, s = e^u), with the prior on u."""
    s = math.exp(u)
    tot = 0.5 * ((u - math.log(PRIOR_S)) / PRIOR_SD) ** 2
    for x, y in zip(xs, ys):
        q = min(1 - 1e-12, max(1e-12, p_win(x, c, s)))
        tot -= y * math.log(q) + (1 - y) * math.log(1 - q)
    return tot


def _minimize(f, start: tuple, step: float = 0.25, tol: float = 1e-9, iters: int = 4000) -> tuple:
    """Nelder-Mead on two parameters: the minimum's point."""
    pts = [list(start), [start[0] + step, start[1]], [start[0], start[1] + step]]
    vals = [f(*p) for p in pts]
    for _ in range(iters):
        order = sorted(range(3), key=vals.__getitem__)
        pts, vals = [pts[i] for i in order], [vals[i] for i in order]
        if abs(vals[2] - vals[0]) < tol and max(abs(pts[2][j] - pts[0][j]) for j in (0, 1)) < 1e-7:
            break
        cen = [(pts[0][j] + pts[1][j]) / 2 for j in (0, 1)]
        ref = [cen[j] + (cen[j] - pts[2][j]) for j in (0, 1)]
        fr = f(*ref)
        if fr < vals[0]:
            exp_ = [cen[j] + 2 * (cen[j] - pts[2][j]) for j in (0, 1)]
            fe = f(*exp_)
            pts[2], vals[2] = (exp_, fe) if fe < fr else (ref, fr)
        elif fr < vals[1]:
            pts[2], vals[2] = ref, fr
        else:
            con = [cen[j] + 0.5 * (pts[2][j] - cen[j]) for j in (0, 1)]
            fc = f(*con)
            if fc < vals[2]:
                pts[2], vals[2] = con, fc
            else:
                for i in (1, 2):
                    pts[i] = [pts[0][j] + 0.5 * (pts[i][j] - pts[0][j]) for j in (0, 1)]
                    vals[i] = f(*pts[i])
    best = min(range(3), key=vals.__getitem__)
    return tuple(pts[best])


def fit(xs: list, ys: list) -> dict:
    """The logistic fit over every game: {c, s, se} -- c the x of half the points, s the slope, se c's standard
    error from the curvature at the minimum (None when it is not positive definite)."""
    if not xs:
        raise CouldNotJudge("no game to fit")
    f = lambda c, u: nll(c, u, xs, ys)  # noqa: E731
    c, u = _minimize(f, (sum(xs) / len(xs), math.log(PRIOR_S)))
    h = 1e-4
    fcc = (f(c + h, u) - 2 * f(c, u) + f(c - h, u)) / h ** 2
    fuu = (f(c, u + h) - 2 * f(c, u) + f(c, u - h)) / h ** 2
    fcu = (f(c + h, u + h) - f(c + h, u - h) - f(c - h, u + h) + f(c - h, u - h)) / (4 * h * h)
    det = fcc * fuu - fcu * fcu
    se = math.sqrt(fuu / det) if det > 0 and fuu > 0 else None
    return {"c": c, "s": math.exp(u), "se": se}


def report(fr: dict, looks: list) -> str:
    """The fit's lines: m* and its interval, where it lies against the lever's range, and every look."""
    c, se = fr["c"], fr["se"]
    lines = [f"look {lk['k']}: x{lk['mult']:.3f} (log2 {lk['x']:.3f}): points {lk['points']:.3f} over "
             f"{lk['games']} games" for lk in looks]
    mid = 2.0 ** c
    band = (f", 90 % [x{2.0 ** (c - Z90 * se):.3f}, x{2.0 ** (c + Z90 * se):.3f}]" if se is not None
            else ", no interval (the fit's curvature is not positive)")
    where = (" -- above the lever's x8: ImpAI holds half the points at every multiplier played" if c > HI else
             " -- below x1: ImpAI takes less than half the points against OTA AI unhandicapped" if c < LO else "")
    lines.append(f"m* = x{mid:.3f}{band}; slope {fr['s']:.3f} log2 a logit unit{where}")
    return "\n".join(lines)


# ---- the cost, simulated ----

def play_sim(rng: random.Random, x: float, xs: float, s: float, tie: float, n: int) -> list:
    """n games' points at x when the true half point is xs: a tie takes `tie` of the mass, the mean kept."""
    p = p_win(x, xs, s)
    t = min(tie, 2 * min(p, 1 - p))
    out = []
    for _ in range(n):
        u = rng.random()
        out.append(1.0 if u < p - t / 2 else 0.5 if u < p + t / 2 else 0.0)
    return out


def simulate(looks: int, games: int, slope: float, tie: float, runs: int, seed: int = 1) -> dict:
    """The method run on simulated games: {rmse, p90} of c against the true x*, uniform in [0.3, 2.7], and the
    bracket midpoint's alone (mid_rmse) for comparison."""
    rng = random.Random(seed)
    err, mid = [], []
    for _ in range(runs):
        xs = rng.uniform(0.3, 2.7)
        done, gx, gy = [], [], []
        for k in range(looks):
            x = math.log2(mult_of(next_x(done)))
            pts = play_sim(rng, x, xs, slope, tie, games)
            gx += [x] * games
            gy += pts
            done.append({"k": k + 1, "x": x, "points": sum(pts) / games})
        err.append(fit(gx, gy)["c"] - xs)
        mid.append(next_x(done) - xs)
    rmse = lambda e: math.sqrt(sum(v * v for v in e) / len(e))  # noqa: E731
    a = sorted(abs(v) for v in err)
    return {"rmse": rmse(err), "p90": a[int(0.9 * (len(a) - 1))], "mid_rmse": rmse(mid)}


# ---- the run ----

def md5(p: Path) -> str:
    return hashlib.md5(p.read_bytes()).hexdigest()


def fix_dll(out: Path, dll: Path) -> Path:
    """The DLL copied into the run at its first look; CouldNotJudge when an earlier look's copy differs."""
    dst = out / "ddraw.dll"
    if dst.exists():
        if md5(dst) != md5(dll):
            raise CouldNotJudge(f"{dst} is another DLL than {dll}: a ladder is one build, fixed at its first look")
    else:
        out.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(dll, dst)
    return dst


def load_state(out: Path) -> dict:
    try:
        return json.loads((out / STATE).read_text())
    except (OSError, ValueError):
        return {"looks": []}


def save_state(out: Path, state: dict):
    tmp = out / (STATE + ".tmp")
    tmp.write_text(json.dumps(state, indent=1) + "\n")
    tmp.replace(out / STATE)


def look_points(ids: list, games: Path):
    """(points, None) of A's over the games `ids` whose results are in `games`; (None, why) when one is missing,
    could not run or is a game at the cap not scored."""
    pts = []
    for gid in ids:
        try:
            r = json.loads((games / f"{gid}.json").read_text())
        except (OSError, ValueError):
            return None, f"{gid}: no result"
        who, why = bar.outcome_of(r)
        if who is None:
            return None, f"{gid}: {why}"
        pts.append(bar.POINTS[who])
    return sum(pts) / len(pts), None


def pieces(pair: str, games: Path, lanes: int) -> list:
    """A look's pieces: one a block of `pair`, in block order."""
    ids = bar.ids(pair)
    return [{"name": f"{pair}-b{b}", "setup": None, "gating": False, "lanes": lanes, "out": games,
             "ids": [g for g in ids if bar.block_of(g) == b]} for b in range(1, bar.HANDICAP_BLOCKS + 1)]


def play_look(ps: list, launch) -> list:
    """The look's pieces played (batch.play_stage): the names of those stopped past the deadline."""
    _failed, overran = batch.play_stage(ps, launch, time.monotonic, time.sleep, lambda: None)
    return overran


def run(a, launch=None, play=play_look) -> int:
    import tarun
    out = Path(a.out)
    try:
        dll = fix_dll(out, Path(a.dll))
    except (CouldNotJudge, OSError) as e:
        print(f"ladder: COULD NOT RUN: {e}")
        return 2
    games = out / "games"
    launch = launch or (lambda p: batch.Piece(p, dll, out / "logs"))
    state = load_state(out)
    jr = tarun.begin("ladder", [("ladder", [{"id": f"look {k}"} for k in range(1, a.looks + 1)])],
                     title=f"ladder {out.name}", pools={"main": 1}, gate=True)
    looks = state["looks"]
    for lk in looks[:a.looks]:
        jr.finish("ladder", f"look {lk['k']}", "pass", f"x{lk['mult']:.3f}: {lk['points']:.3f}")
    try:
        while len(looks) < a.looks:
            k = len(looks) + 1
            mult = next_mult(looks)
            if mult is None:
                print(f"ladder: look {k} would play x{mult_of(next_x(looks)):.3f} again: the bracket is inside the "
                      f"lever's 0.001; the fit takes the {len(looks)} looks played")
                for j in range(k, a.looks + 1):
                    jr.finish("ladder", f"look {j}", "skipped", "the bracket closed")
                break
            pair = bar.handicap_pair(mult)
            ps = pieces(pair, games, a.lanes)
            ids = [g for p in ps for g in p["ids"]]
            jr.start("ladder", f"look {k}")
            print(f"ladder: look {k}: {pair}, {len(ids)} games")
            overran = play(ps, launch)
            points, why = look_points(ids, games)
            if points is None and not overran:
                print(f"ladder: look {k} played once more ({why})")
                overran = play(ps, launch)
                points, why = look_points(ids, games)
            if points is None:
                why = f"pieces past {batch.DEADLINE} s: {', '.join(overran)}" if overran else why
                jr.finish("ladder", f"look {k}", "unrun", why)
                jr.end("unrun")
                print(f"ladder: COULD NOT JUDGE: look {k} lacks a game ({why}); `run` again goes on from it")
                return 2
            looks.append({"k": k, "pair": pair, "mult": mult, "x": math.log2(mult), "points": points,
                          "games": len(ids), "ids": ids})
            state["dll"] = md5(dll)
            save_state(out, state)
            jr.finish("ladder", f"look {k}", "pass", f"{pair}: {points:.3f}")
            print(f"ladder: look {k}: {pair}: points {points:.3f}")
    except KeyboardInterrupt:
        jr.end("stopped")
        return 2
    rc = cmd_fit_dir(out)
    jr.end("pass" if rc == 0 else "unrun")
    return rc


def cmd_fit_dir(out: Path) -> int:
    """The fit over every game of the run's looks, printed and written to <out>/fit.txt."""
    looks = load_state(out)["looks"]
    xs, ys = [], []
    for lk in looks:
        for gid in lk["ids"]:
            r = json.loads((out / "games" / f"{gid}.json").read_text())
            xs.append(lk["x"])
            ys.append(bar.POINTS[bar.outcome_of(r)[0]])
    try:
        text = report(fit(xs, ys), looks)
    except CouldNotJudge as e:
        print(f"ladder: COULD NOT JUDGE: {e}")
        return 2
    print(text)
    (out / "fit.txt").write_text(text + "\n")
    return 0


def common_data() -> Path:
    """The main checkout's .data, which every worktree shares."""
    got = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--git-common-dir"], capture_output=True,
                         text=True).stdout.strip()
    return (TREE / (got or ".git")).resolve().parent / ".data"


def main(argv=None) -> int:
    sys.stdout.reconfigure(line_buffering=True)
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="play the looks not yet finished, then fit")
    r.add_argument("--dll", required=True, help="the build under test, ImpAI's")
    r.add_argument("--tag", required=True, help="the run's name: its directory under .data/impai/runs/ladder/")
    r.add_argument("--out", default=None, help="the run's directory (default .data/impai/runs/ladder/<tag>)")
    r.add_argument("--looks", type=int, default=LOOKS, help=f"looks in all (default {LOOKS})")
    r.add_argument("--lanes", type=int, default=LANES, choices=range(1, 15), metavar="1..14",
                   help=f"lanes a piece (default {LANES}, one wave of a block)")
    f = sub.add_parser("fit", help="fit the looks a run finished")
    f.add_argument("dir")
    s = sub.add_parser("simulate", help="the method's precision on simulated games")
    s.add_argument("--looks", type=int, default=LOOKS)
    s.add_argument("--games", type=int, default=10 * bar.HANDICAP_BLOCKS, help="games a look")
    s.add_argument("--slope", type=float, default=0.3)
    s.add_argument("--tie", type=float, default=0.3)
    s.add_argument("--runs", type=int, default=300)
    s.add_argument("--seed", type=int, default=1)
    a = p.parse_args(argv)
    if a.cmd == "run":
        a.out = a.out or str(common_data() / "impai" / "runs" / "ladder" / a.tag)
        return run(a)
    if a.cmd == "fit":
        return cmd_fit_dir(Path(a.dir))
    got = simulate(a.looks, a.games, a.slope, a.tie, a.runs, a.seed)
    print(f"looks {a.looks} x {a.games} games, slope {a.slope}, ties {a.tie}, {a.runs} runs: RMSE {got['rmse']:.3f} "
          f"log2 (x{2 ** got['rmse']:.3f}), 90th percentile {got['p90']:.3f}; the bracket's midpoint alone "
          f"{got['mid_rmse']:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
