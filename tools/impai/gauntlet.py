#!/usr/bin/env python3
"""gauntlet.py -- the champion gauntlet: a candidate against a pool of earlier champions, ImpAI against ImpAI, so a
regression that only one earlier champion exposes shows. A member of the pool is its settings, played in one build,
or its code, a build of its own played in a network game.

    tools/impai/gauntlet.py run --dll DLL --cand ROW --pool ROW[,ROW...] --tag TAG [--blocks N] [--lanes L]
                                [--out DIR]
    tools/impai/gauntlet.py run --code --cand DLL --pool NAME=DLL[,NAME=DLL...] --tag TAG [--games N]
                                [--maps KEY,KEY...] [--cap T] [--at-once K] [--no-humans] [--out DIR]
    tools/impai/gauntlet.py judge DIR

WHY. A candidate that beats the last champion can still lose to an older one: strength among settings need not be
transitive. The paired margin test (margin.py, its THE SETTINGS) compares two settings against OTA AI; the gauntlet
plays the candidate against every champion of the pool directly.

THE SETTINGS of a seat are a persona row (bar.py's PERSONA_ROWS, the persona lever) or H, none: the DLL's Hard.
Tuned tables join them when the table lever's files are versioned (research/notes/impai/bar.md, *The champion
gauntlet*).

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
step a pool member (tools/tarun.py).

THE CODE GAUNTLET (`run --code`). The candidate and each pool member are builds: `--cand DLL` (or NAME=DLL; the
name is the file's stem otherwise) and `--pool NAME=DLL,...`. Against each member the candidate plays GAMES duels
(codeduel.py: one network game of two peers, each on its own ddraw.dll, judged from both peers' logs), an even
number: game 2j is pairing j with the candidate the host and game 2j + 1 the same pairing with it the joiner, so
each build sits on each peer, and so on each pair of starts, as often as the other. Pairing j is map j of MAPS, the
list gone round, with the side pairing of bar.py's BLOCK_SIDES (the candidate's side first) that is the map's
place plus the round, so every map meets every side pairing over four rounds of the maps. The cap is bar.py's
unless --cap names another.

THE MAPS default to DUEL_MAPS, four of the development pool's five, the pool the settings gauntlet plays. A duel
seats its two idle humans on a map's first two starts and the computer seats on the next two (codeduel.py, THE
MAP), so a map needs four, which every map of the bar's two pools has (ten each), and both seats must play from
those two. MEASURED 2026-11-17, a duel a map to GameTime 5 000 or 6 000 between two champions' builds: both seats
gave orders on Acid Foursome, Luschaven, Gasplant Plain and Lava Alley, and on the bar's Lava Mania, Pincushion,
Red Triangle and Plains and Passes; on Steel Jungle neither build gave one (two duels), and on Trout Farm the
seat on start 2 gave none. Those two maps are left out of the default; --maps takes any of bar.py's.
  - A game's points are the candidate's, as above: a game at the cap is scored by codeduel.py's value at the
    cap, which both peers must agree on (its THE VALUE AT THE CAP), through bar.outcome_of; as the joiner the
    candidate is seat B and its points are one less A's.
  - A game that could not run, or whose peers disagreed (a desync), is played once more; a member with a game
    still unscored is could not judge, and the run stops there.
  - A network game is not deterministic, so no game is paired with another: THE VERDICT is the same one-sided
    interval over the member's games.
  - The two peers' clocks run apart (codeduel.py, THE CLOCKS): the report gives each game's lead of the
    joiner's GameTime over the host's at its last round of looks, the ticks the joiner's seat has played more.
  - Each game is one codeduel.py process on its own hold of the run lock (two GDI lanes) and its own leased
    DirectPlay port; at most AT_ONCE (7: the lock's 28 slots over a game's 4) play at once, the next launched as
    one ends. A game past its deadline (duel_deadline: the cap at Normal speed and ten minutes) is stopped.
  - Both builds are copied into <out>/dlls/ at the first run and a run with another build under a name is
    refused. Every try of a game keeps its evidence in <out>/games/<id>/t<k>/ (codeduel.py's directory, its
    result.json), so `run` again takes each game's first scored try from the disk and plays only the games left:
    a run stopped inside a member loses no game it finished."""
import argparse
import json
import math
import shutil
import signal
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
import ladder  # noqa: E402

Z = 1.6449                      # one-sided 95 %
SD = 0.5                        # the largest spread of a game's points: THE VERDICT's width
LANES = 10
STATE = "gauntlet.json"
PASS, FAIL, COULD_NOT = 0, 1, 2
GAMES = 20                      # THE CODE GAUNTLET: duels a member, the settings gauntlet's two blocks of ten
DUEL_MAPS = ("acidfoursome", "luschaven", "gasplantplain", "lavaalley")    # THE MAPS
AT_ONCE = 7                     # duels at once: the run lock's 28 slots over a duel's 4
NORMAL_TICKS_S = 30             # a network game's Normal speed, the slowest a duel plays (codeduel.py, THE SPEED)
DUEL_SPARE_S = 600              # a duel's launch, lobby and leave, several times over


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


# ---- THE CODE GAUNTLET ----

def named_dll(item: str) -> tuple:
    """(name, path) of `NAME=DLL`, or of `DLL` named by its file's stem."""
    name, eq, path = item.partition("=")
    if not eq:
        name, path = Path(item).stem, item
    if not name or not path or any(c in name for c in "/, "):
        raise CouldNotJudge(f"{item!r}: NAME=DLL, the name without a slash, a comma or a space")
    return name, Path(path)


def fix_dlls(out: Path, dlls: dict) -> dict:
    """{name: its copy under <out>/dlls} of {name: path}, copied at the first run; CouldNotJudge when a name's
    copy is another build, or two names are one build (a duel needs two its logs tell apart)."""
    kept = {}
    for name, src in dlls.items():
        if not src.is_file():
            raise CouldNotJudge(f"{name}: no file {src}")
    (out / "dlls").mkdir(parents=True, exist_ok=True)
    for name, src in dlls.items():
        dst = out / "dlls" / f"{name}.dll"
        if not dst.exists():
            shutil.copyfile(src, dst)
        elif ladder.md5(dst) != ladder.md5(src):
            raise CouldNotJudge(f"{dst} is another build than {src}: one directory is one gauntlet")
        kept[name] = dst
    sums = {}
    for name, dst in kept.items():
        twin = sums.setdefault(ladder.md5(dst), name)
        if twin != name:
            raise CouldNotJudge(f"{twin} and {name} are one build: a duel is two")
    return kept


def duel_games(member: str, n: int, maps: list, cap: int, no_humans: bool = False) -> list:
    """A member's duels, in order (THE CODE GAUNTLET): [{id, member, map, sides, role, cap, no_humans}], `sides`
    the candidate's side then the member's, `role` h for the candidate the host and j the joiner."""
    if n < 2 or n % 2:
        raise CouldNotJudge(f"--games {n}: an even number, the candidate the host in half and the joiner in half")
    games = []
    for k in range(n):
        j = k // 2
        at, rnd = j % len(maps), j // len(maps)
        m, sides, role = maps[at], bar.BLOCK_SIDES[(at + rnd) % len(bar.BLOCK_SIDES)], "hj"[k % 2]
        games.append({"id": f"{member}-{j + 1:02d}{role}-{m}-{sides}", "member": member, "map": m, "sides": sides,
                      "role": role, "cap": cap, "no_humans": no_humans})
    return games


def duel_command(game: dict, dlls: dict, cand: str) -> list:
    """The codeduel.py that plays `game` into game["dir"]: the host's build is A's."""
    host, join = (cand, game["member"]) if game["role"] == "h" else (game["member"], cand)
    sides = game["sides"] if game["role"] == "h" else game["sides"][::-1]
    return [sys.executable, str(HERE / "codeduel.py"), "--a", str(dlls[host]), "--b", str(dlls[join]),
            "--out", str(game["dir"]), "--map", game["map"], "--sides", sides, "--cap", str(game["cap"]),
            *(["--no-humans"] if game["no_humans"] else [])]


def duel_deadline(cap: int) -> float:
    """The seconds a duel may play after its admission."""
    return cap / NORMAL_TICKS_S + DUEL_SPARE_S


class Duel:
    """A duel being played: its codeduel.py, which holds the run lock for itself, and its output."""

    def __init__(self, game: dict, dlls: dict, cand: str, logs: Path):
        logs.mkdir(parents=True, exist_ok=True)
        self.log = logs / f"{game['id']}.{Path(game['dir']).name}.log"
        with open(self.log, "a") as f:
            self.proc = subprocess.Popen(duel_command(game, dlls, cand), cwd=str(TREE), stdout=f,
                                         stderr=subprocess.STDOUT)

    def admitted(self) -> bool:
        """The run lock has admitted it: codeduel.py names its evidence once it holds its slots."""
        try:
            return "evidence: " in self.log.read_text(errors="replace")
        except OSError:
            return False

    def poll(self):
        return self.proc.poll()

    def signal(self, sig):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)


def play_duels(games: list, launch, clock=time.monotonic, sleep=time.sleep, at_once: int = AT_ONCE) -> list:
    """Every game of `games` played, at most `at_once` at a time and launched in order, the next as one ends:
    the ids of those stopped past their deadline."""
    waiting, live, overran = list(games), [], []
    while waiting or live:
        while waiting and len(live) < at_once:
            g = waiting.pop(0)
            live.append({"g": g, "h": launch(g), "admit": None, "termed": False})
            sleep(batch.LAUNCH_GAP)     # the run lock sees the games in their order
        sleep(batch.POLL)
        for d in list(live):
            if d["admit"] is None and d["h"].admitted():
                d["admit"] = clock()
            if d["h"].poll() is not None:
                live.remove(d)
            elif d["admit"] is not None and not d["termed"] and clock() - d["admit"] > duel_deadline(d["g"]["cap"]):
                d["h"].signal(signal.SIGTERM)
                d["termed"] = True
                overran.append(d["g"]["id"])
    return overran


def tries(gdir: Path) -> list:
    """A game's tries on the disk, in order: its t<k> directories."""
    return sorted((d for d in gdir.glob("t[0-9]*") if d.is_dir()), key=lambda d: int(d.name[1:]))


def duel_result(game: dict, games: Path):
    """(the candidate's points, the try, its result) of the game's first scored try, or (None, why, the last
    try's result or None)."""
    why, last = "not played", None
    for d in tries(games / game["id"]):
        try:
            r = json.loads((d / "result.json").read_text())
        except (OSError, ValueError):
            why = f"{d.name}: no result"
            continue
        who, said = bar.outcome_of(r)
        if who is not None:
            a = bar.POINTS[who]
            return (a if game["role"] == "h" else 1.0 - a), d.name, r
        why, last = f"{d.name}: {said}", r
    return None, why, last


def duel_rows(games: list, out: Path) -> list:
    """Each game as the state keeps it: its id, its try and what it gave."""
    rows = []
    for g in games:
        pts, t, r = duel_result(g, out)
        row = {k: g[k] for k in ("id", "map", "sides", "role")}
        if pts is None:
            rows.append(dict(row, points=None, why=t))
            continue
        at_cap = r["outcome"] == "cap" and not r.get("draw")
        clock = r.get("clock") or {}
        rows.append(dict(row, points=pts, **{"try": t}, outcome=r["outcome"], end_tick=r.get("end_tick"),
                         value={s: v["live"] for s, v in r["value"].items()} if at_cap else None,
                         ahead=clock["J"] - clock["H"] if len(clock) == 2 else None))
    return rows


def tally(rows: list) -> str:
    """A member's games in words: the candidate's wins, draws and losses, how many of them the cap's value
    scored, and its mean points on each peer."""
    w, d, l_ = (sum(1 for r in rows if r["points"] == x) for x in (1.0, 0.5, 0.0))
    caps = sum(1 for r in rows if r.get("value"))
    role = {x: [r["points"] for r in rows if r["role"] == x] for x in "hj"}
    mean = {x: f"{sum(v) / len(v):.3f}" if v else "none" for x, v in role.items()}
    return (f"{w} won, {d} drawn, {l_} lost; {caps} at the cap, scored by value; "
            f"as host {mean['h']}, as joiner {mean['j']}")


def load_code_state(out: Path, want: dict) -> dict:
    """The code gauntlet's state, made afresh or read back; CouldNotJudge when it names another gauntlet."""
    try:
        st = json.loads((out / STATE).read_text())
    except (OSError, ValueError):
        return dict(want, members={})
    if {k: st.get(k) for k in want} != want:
        raise CouldNotJudge(f"{out / STATE} is another gauntlet ("
                            + ", ".join(f"{k} {st.get(k)!r}" for k in want if st.get(k) != want[k])
                            + "): one directory is one gauntlet")
    return st


def code_report(st: dict) -> str:
    rows = [f"gauntlet of code: {st['cand']} against {', '.join(st['pool'])}, {st['games']} duels a member on "
            f"{', '.join(st['maps'])}, cap {st['cap']}"]
    for p in st["pool"]:
        v = st["members"].get(p)
        if not v:
            rows.append(f"  {st['cand']} against {p}: not played")
            continue
        rows.append(f"  {st['cand']} against {p}: {v['mean']:.3f} of {v['n']} games, 95 % one-sided "
                    f"[{v['lo']:.3f}, {v['hi']:.3f}]: {v['verdict']}")
        rows.append(f"    {tally(v['rows'])}")
        for r in v["rows"]:
            val = f", value A {r['value']['A']:.0f}, B {r['value']['B']:.0f}" if r.get("value") else ""
            clock = f", the joiner's clock {r['ahead']:+d}" if r.get("ahead") is not None else ""
            rows.append(f"    {r['id']} ({r['try']}): {r['outcome']} at {r['end_tick']}{val}{clock}: {r['points']}")
    return "\n".join(rows)


def run_code(a, launch=None, play=play_duels) -> int:
    import tarun
    out = Path(a.out)
    try:
        cand, cand_dll = named_dll(a.cand)
        items = [p for p in a.pool.split(",") if p]
        pool = dict(named_dll(p) for p in items)
        if not pool or len(pool) != len(items) or cand in pool:
            raise CouldNotJudge(f"--pool {a.pool!r}: one NAME=DLL or more, each name once and none the candidate's")
        import codeduel
        maps = [codeduel.map_of(m)[0] for m in a.maps.split(",") if m]
        if not maps:
            raise CouldNotJudge("--maps: one map or more")
        plan = {p: duel_games(p, a.games, maps, a.cap, a.no_humans) for p in pool}
        dlls = fix_dlls(out, {cand: cand_dll, **pool})
        # a state with no `no_humans` is a gauntlet of duels with the humans idle
        st = load_code_state(out, {"code": True, "cand": cand, "pool": list(pool), "games": a.games, "maps": maps,
                                   "cap": a.cap, "dlls": {n: ladder.md5(d) for n, d in dlls.items()},
                                   **({"no_humans": True} if a.no_humans else {})})
        if st.get("no_humans") and not a.no_humans:
            raise CouldNotJudge(f"{out / STATE} is another gauntlet (no_humans True): one directory is one gauntlet")
    except (CouldNotJudge, ValueError, OSError) as e:
        print(f"gauntlet: COULD NOT RUN: {e}")
        return COULD_NOT
    games = out / "games"
    launch = launch or (lambda g: Duel(g, dlls, cand, out / "logs"))
    jr = tarun.begin("gauntlet", [("gauntlet", [{"id": p} for p in pool])], title=f"gauntlet {cand} {out.name}",
                     pools={"main": 1}, gate=True)
    for p in pool:
        if p in st["members"]:
            jr.finish("gauntlet", p, "pass", st["members"][p]["verdict"])
    try:
        for p in pool:
            if p in st["members"]:
                continue
            jr.start("gauntlet", p)
            overran = []
            for again in (False, True):
                todo = [g for g in plan[p] if duel_result(g, games)[0] is None]
                if not todo:
                    break
                said = ""
                if again:
                    said = (" played once more ("
                            + "; ".join(f"{g['id']}: {duel_result(g, games)[1]}" for g in todo) + ")")
                for g in todo:
                    g["dir"] = games / g["id"] / f"t{len(tries(games / g['id'])) + 1}"
                    g["dir"].parent.mkdir(parents=True, exist_ok=True)
                print(f"gauntlet: {cand} against {p}: {len(todo)} of {len(plan[p])} duels{said}", flush=True)
                overran = play(todo, launch)
            rows = duel_rows(plan[p], games)
            lacking = [f"{r['id']}: {r['why']}" for r in rows if r["points"] is None]
            if lacking:
                why = "; ".join(lacking) + (f"; past their deadline: {', '.join(overran)}" if overran else "")
                jr.finish("gauntlet", p, "unrun", why)
                jr.end("unrun")
                print(f"gauntlet: COULD NOT JUDGE: {cand} against {p} lacks {len(lacking)} of its duels ({why}); "
                      f"`run` again goes on from them")
                return COULD_NOT
            st["members"][p] = dict(member_verdict([r["points"] for r in rows]), rows=rows)
            save_state(out, st)
            v = st["members"][p]
            jr.finish("gauntlet", p, "fail" if v["verdict"] == "loses" else "pass",
                      f"{v['mean']:.3f}: {v['verdict']}")
            print(f"gauntlet: {cand} against {p}: {v['mean']:.3f} of {v['n']}: {v['verdict']}", flush=True)
    except KeyboardInterrupt:
        jr.end("stopped")
        return COULD_NOT
    rc, line = overall(st["members"])
    print(code_report(st))
    print(f"gauntlet: {'PASS' if rc == PASS else 'FAIL'}: {cand} {line}")
    jr.end("pass" if rc == PASS else "fail")
    return rc


def judge_code(out: Path, st: dict) -> int:
    """The code gauntlet's verdicts read again from its games' results on the disk."""
    for p in st["pool"]:
        if p not in st["members"]:
            print(f"gauntlet judge: COULD NOT JUDGE: {st['cand']} against {p} not played")
            return COULD_NOT
        rows = duel_rows(duel_games(p, st["games"], st["maps"], st["cap"]), out / "games")
        lacking = [f"{r['id']}: {r['why']}" for r in rows if r["points"] is None]
        if lacking:
            print(f"gauntlet judge: COULD NOT JUDGE: {'; '.join(lacking)}")
            return COULD_NOT
        st["members"][p] = dict(member_verdict([r["points"] for r in rows]), rows=rows)
    rc, line = overall(st["members"])
    print(code_report(st))
    print(f"gauntlet judge: {'PASS' if rc == PASS else 'FAIL'}: {st['cand']} {line}")
    return rc


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
    if st.get("code"):
        return judge_code(out, st)
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
    r.add_argument("--dll", help="the one build a settings gauntlet plays")
    r.add_argument("--code", action="store_true", help="THE CODE GAUNTLET: the candidate and the pool are builds")
    r.add_argument("--cand", required=True, help=f"the candidate's settings: a persona row or {bar.HARD_ROW}; "
                                                 f"with --code its DLL, or NAME=DLL")
    r.add_argument("--pool", required=True, help="the earlier champions' settings, a comma list; with --code "
                                                 "NAME=DLL,...")
    r.add_argument("--games", type=int, default=GAMES, help=f"--code: duels a pool member, even (default {GAMES})")
    r.add_argument("--maps", default=",".join(DUEL_MAPS), help=f"--code: the maps, by bar.py's keys (default "
                                                               f"{','.join(DUEL_MAPS)})")
    r.add_argument("--cap", type=int, default=bar.CAP, help=f"--code: the GameTime a duel plays to (default {bar.CAP})")
    r.add_argument("--at-once", type=int, default=AT_ONCE, help=f"--code: duels at once (default {AT_ONCE})")
    r.add_argument("--no-humans", action="store_true", help="--code: every duel is codeduel.py's --no-humans")
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
    if a.code:
        if not 1 <= a.at_once <= AT_ONCE:
            p.error(f"--at-once {a.at_once}: 1 to {AT_ONCE}")
        return run_code(a, play=lambda games, launch: play_duels(games, launch, at_once=a.at_once))
    if not a.dll:
        p.error("--dll: a settings gauntlet plays one build")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
