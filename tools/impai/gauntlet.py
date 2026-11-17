#!/usr/bin/env python3
"""gauntlet.py -- the champion gauntlet: a candidate's settings against a pool of earlier champions' settings,
ImpAI against ImpAI in one build, so a regression that only one earlier champion exposes shows.

    tools/impai/gauntlet.py run --dll DLL --cand ROW --pool ROW[,ROW...] --tag TAG [--blocks N] [--lanes L]
                                [--out DIR]
    tools/impai/gauntlet.py judge DIR

WHY. A candidate that beats the last champion can still lose to an older one: strength among settings need not be
transitive. The paired margin test (margin.py, its THE SETTINGS) compares two settings against OTA AI; the gauntlet
plays the candidate against every champion of the pool directly.

THE SETTINGS of a seat are a persona row (bar.py's PERSONA_ROWS, the persona lever) or H, none: the DLL's Hard.
Tuned tables join them when the table lever's files are versioned (research/notes/impai/bar.md, *The champion
gauntlet*). Code against code -- two builds in one game -- is not this tool's: see the same section.

THE GAMES. Against each pool member P the candidate C plays bar.py's gauntlet pair `g:<C>.<P>` (C as A, P as B):
BLOCKS of its blocks on the development pool, every map from both starts and every side pairing (bar.py's THE
BLOCKS), so each settings row sits in both seats and on every start. A game's points are C's: a win 1, a draw or a
game at the cap with no lead in value 0.5, a loss 0 (bar.py's POINTS and outcome_of).

THE VERDICT, for each pool member, on C's mean points m over its n games: a one-sided Z (1.645, 95 %) interval
m -+ Z SD / sqrt(n), SD 0.5, the most a game's points can spread (a coin's), so the interval never claims more than
the games show, whatever share of them were draws, and a run of equal outcomes still has its width:
    beats   the interval's low end above one half: C is stronger;
    loses   its high end below one half: C is weaker -- the gauntlet FAILS;
    even    otherwise: no difference this many games can show.
The gauntlet passes when C loses to no pool member, and says which it beats and which it is even with. Exit 0
pass, 1 fail, 2 could not judge (a game missing, could not run, or at the cap and unscored).

HOW IT RUNS. Each pool member's pair is played as one piece a block, one `bar.py play` on LANES lanes, launched in
order through batch.py's play_stage (as ladder.py's looks are). The DLL is copied into the run's directory at its
first piece and a run with another DLL is refused, so one gauntlet is one build. The run's state is
<out>/gauntlet.json: the candidate, the pool, the blocks and each member's verdict as it is judged, so `run`
again with the same arguments goes on from the first member not judged; one with another candidate, pool or
blocks is refused. A member with a game that could not run is played once more; still lacking, the run stops,
could not judge. The results go to <out>/games/, each piece's log to <out>/logs/. It is one runs-viewer run with a
step a pool member (tools/tarun.py)."""
import argparse
import json
import math
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import bar  # noqa: E402
import batch  # noqa: E402
import ladder  # noqa: E402

Z = 1.6449                      # one-sided 95 %
SD = 0.5                        # the largest spread of a game's points: THE VERDICT's width
LANES = 10
STATE = "gauntlet.json"
PASS, FAIL, COULD_NOT = 0, 1, 2


class CouldNotJudge(Exception):
    pass


def settings(row: str) -> str:
    """`row` when a gauntlet pair takes it (a PERSONA_ROWS row or bar.HARD_ROW); CouldNotJudge otherwise."""
    if row != bar.HARD_ROW and row not in bar.PERSONA_ROWS:
        raise CouldNotJudge(f"no settings {row!r}: {bar.HARD_ROW} or one of {', '.join(bar.PERSONA_ROWS)}")
    return row


def member_verdict(points: list) -> dict:
    """THE VERDICT over C's points against one pool member: {n, mean, lo, hi, verdict}."""
    n = len(points)
    if not n:
        raise CouldNotJudge("no game")
    m = sum(points) / n
    half = Z * SD / math.sqrt(n)
    lo, hi = m - half, m + half
    v = "beats" if lo > 0.5 else "loses" if hi < 0.5 else "even"
    return {"n": n, "mean": m, "lo": lo, "hi": hi, "verdict": v}


def member_points(ids: list, games: Path):
    """(C's points game by game, None) over `ids`, or (None, why) when one is missing, could not run or is unscored."""
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
    return pts, None


def pieces(pair: str, games: Path, lanes: int, blocks: int) -> list:
    """A pool member's pieces: one a block of `pair`, in block order."""
    ids = bar.ids(pair, blocks=blocks)
    return [{"name": f"{pair}-b{b}", "setup": None, "gating": False, "lanes": lanes, "out": games,
             "ids": [g for g in ids if bar.block_of(g) == b]} for b in range(1, blocks + 1)]


def overall(members: dict) -> tuple:
    """(exit, line) of the gauntlet from its members' verdicts."""
    lost = [p for p, v in members.items() if v["verdict"] == "loses"]
    beat = [p for p, v in members.items() if v["verdict"] == "beats"]
    even = [p for p, v in members.items() if v["verdict"] == "even"]
    line = (f"beats {', '.join(beat) or 'none'}; even with {', '.join(even) or 'none'}; "
            f"loses to {', '.join(lost) or 'none'}")
    return (FAIL if lost else PASS), line


def report(state: dict) -> str:
    rows = [f"gauntlet: {state['cand']} against {', '.join(state['pool'])}, {state['blocks']} blocks a member"]
    for p in state["pool"]:
        v = state["members"].get(p)
        rows.append(f"  {state['cand']} against {p}: " + (
            f"{v['mean']:.3f} of {v['n']} games, 95 % one-sided [{v['lo']:.3f}, {v['hi']:.3f}]: {v['verdict']}"
            if v else "not played"))
    return "\n".join(rows)


def load_state(out: Path, cand: str, pool: list, blocks: int) -> dict:
    """The run's state, made afresh or read back; CouldNotJudge when it names another candidate, pool or blocks."""
    want = {"cand": cand, "pool": pool, "blocks": blocks}
    try:
        st = json.loads((out / STATE).read_text())
    except (OSError, ValueError):
        return dict(want, members={})
    if {k: st.get(k) for k in want} != want:
        raise CouldNotJudge(f"{out / STATE} is a gauntlet of {st.get('cand')} against {st.get('pool')} at "
                            f"{st.get('blocks')} blocks: one directory is one gauntlet")
    return st


def save_state(out: Path, st: dict):
    tmp = out / (STATE + ".tmp")
    tmp.write_text(json.dumps(st, indent=1) + "\n")
    tmp.replace(out / STATE)


def run(a, launch=None, play=ladder.play_look) -> int:
    import tarun
    out = Path(a.out)
    try:
        cand = settings(a.cand)
        pool = [settings(p) for p in a.pool.split(",") if p]
        if not pool or len(set(pool)) != len(pool):
            raise CouldNotJudge(f"--pool {a.pool!r}: one settings row or more, each once")
        dll = ladder.fix_dll(out, Path(a.dll))
        st = load_state(out, cand, pool, a.blocks)
    except (CouldNotJudge, ladder.CouldNotJudge, OSError) as e:
        print(f"gauntlet: COULD NOT RUN: {e}")
        return COULD_NOT
    games = out / "games"
    launch = launch or (lambda p: batch.Piece(p, dll, out / "logs"))
    jr = tarun.begin("gauntlet", [("gauntlet", [{"id": p} for p in pool])], title=f"gauntlet {cand} {out.name}",
                     pools={"main": 1}, gate=True)
    for p in pool:
        if p in st["members"]:
            jr.finish("gauntlet", p, "pass", st["members"][p]["verdict"])
    try:
        for p in pool:
            if p in st["members"]:
                continue
            pair = bar.gauntlet_pair(cand, p)
            ps = pieces(pair, games, a.lanes, a.blocks)
            ids = [g for x in ps for g in x["ids"]]
            jr.start("gauntlet", p)
            print(f"gauntlet: {pair}, {len(ids)} games")
            overran = play(ps, launch)
            pts, why = member_points(ids, games)
            if pts is None and not overran:
                print(f"gauntlet: {pair} played once more ({why})")
                overran = play(ps, launch)
                pts, why = member_points(ids, games)
            if pts is None:
                why = f"pieces past {batch.DEADLINE} s: {', '.join(overran)}" if overran else why
                jr.finish("gauntlet", p, "unrun", why)
                jr.end("unrun")
                print(f"gauntlet: COULD NOT JUDGE: {pair} lacks a game ({why}); `run` again goes on from it")
                return COULD_NOT
            st["members"][p] = dict(member_verdict(pts), pair=pair, ids=ids)
            st["dll"] = ladder.md5(dll)
            save_state(out, st)
            v = st["members"][p]
            jr.finish("gauntlet", p, "fail" if v["verdict"] == "loses" else "pass",
                      f"{v['mean']:.3f}: {v['verdict']}")
            print(f"gauntlet: {cand} against {p}: {v['mean']:.3f} of {v['n']}: {v['verdict']}")
    except KeyboardInterrupt:
        jr.end("stopped")
        return COULD_NOT
    rc, line = overall(st["members"])
    print(report(st))
    print(f"gauntlet: {'PASS' if rc == PASS else 'FAIL'}: {cand} {line}")
    jr.end("pass" if rc == PASS else "fail")
    return rc


def judge(out: Path) -> int:
    """The verdicts of a run's directory read again from its games (the state names the ids)."""
    try:
        st = json.loads((out / STATE).read_text())
    except (OSError, ValueError) as e:
        print(f"gauntlet judge: COULD NOT JUDGE: {e}")
        return COULD_NOT
    for p in st["pool"]:
        v = st["members"].get(p)
        if not v:
            print(f"gauntlet judge: COULD NOT JUDGE: {st['cand']} against {p} not played")
            return COULD_NOT
        pts, why = member_points(v["ids"], out / "games")
        if pts is None:
            print(f"gauntlet judge: COULD NOT JUDGE: {why}")
            return COULD_NOT
        st["members"][p] = dict(v, **member_verdict(pts))
    rc, line = overall(st["members"])
    print(report(st))
    print(f"gauntlet judge: {'PASS' if rc == PASS else 'FAIL'}: {st['cand']} {line}")
    return rc


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)
    r = sub.add_parser("run", help="play the candidate against each pool member, then judge")
    r.add_argument("--dll", required=True)
    r.add_argument("--cand", required=True, help=f"the candidate's settings: a persona row or {bar.HARD_ROW}")
    r.add_argument("--pool", required=True, help="the earlier champions' settings, a comma list")
    r.add_argument("--tag", required=True, help="the run's name under .data/impai/gauntlet/ (unless --out)")
    r.add_argument("--blocks", type=int, default=bar.GAUNTLET_BLOCKS, help="blocks a pool member (10 games each)")
    r.add_argument("--lanes", type=int, default=LANES)
    r.add_argument("--out", default=None)
    j = sub.add_parser("judge", help="the verdicts of a run's directory, from its games")
    j.add_argument("dir", type=Path)
    a = p.parse_args(argv)
    if a.verb == "judge":
        return judge(a.dir)
    a.out = a.out or str(ladder.common_data() / "impai" / "gauntlet" / a.tag)
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
