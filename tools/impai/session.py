#!/usr/bin/env python3
"""session.py -- the session tier of ImpAI's gate: a candidate DLL against the champion's, game by game, in minutes.

    tools/impai/session.py --test DLL --base DLL --tag TAG [--only NAME[,NAME...]] [--neutral [--state]
                           [--tree DIR]...] [--lanes L] [--out DIR]

THE GAMES. Every exploiter situation (exploit.SITUATIONS) and the confirm set, the bar games of confirm.txt
(regress.txt's format: an id, then its setup), each played once with each DLL from its own seed. --only narrows
them to the situations and bar ids it names (`air-rush` or `exploit-air-rush`, `bar-hota-troutfarm-ca-1`), so the
item a writer works on costs one or two games.

THE CHAMPION'S GAMES ARE PLAYED ONCE. BASE's games go to a folder named by BASE's content --
.data/impai/runs/session/base-<the first 12 hex of its SHA-256>/ in the main checkout, whatever --out -- beside a
copy of the DLL, and every later session against the same champion reads them there. TEST's go to
.data/impai/runs/session/<tag>/ (or --out), beside a copy of TEST; a folder that holds another DLL's games is
refused, since a tag names one build. In each folder a setup's games are in a directory of their own (`base` for
the base game) and a game already there is not played again (bar.py's RESUMING), so the same command twice plays
nothing. One session at a time plays a champion's games (a lock on its folder): a second waits, then reads them.

THE VERDICT, one line a game: the base's result, the test's, and a word. A bar game's category is batch.py's
reading of its result, best first: won; at the cap ahead in value; at the cap not ahead (a draw included); lost.
A situation's is exploit.py's verdict: pass; FAIL. A game's DECISIONS are its log's `impai:` lines that carry a
GameTime and no wall-clock field (diag.decisions).

    SAME     the two games' decisions are identical and their results equal
    BETTER   the test's category is above the base's
    WORSE    the test's category is below the base's
    PARTED   one category, but the games differ: the first GameTime at which their decisions part is named. An
             end at another tick, or another value at the cap, inside one category is reported, never judged
    UNRUN    a game of the two could not run, or kept no decision to compare

With --neutral, a refactor's proof that the same seeds give the same games, every game must be SAME, and the two
lines at the first parting of each game that is not are printed under it. With --state as well, the proof is
the brain's whole state, not the lines it happened to log: the two games' recordings must hold every ImpAI seat's
state equal at every tick (think.py's diff over s_mil, s_brain, s_job and s_known), a game whose state parts is
PARTED with the first tick and field named, and one whose recordings cannot be compared is UNRUN. A refactor
that changes a recorded struct names the tree that built the other DLL with --tree, as think.py takes it.

A situation's line ends with its FIGURES for both DLLs (exploit.py, THE FIGURES: the opponent's units through
the game, ImpAI's first attack and attacks, its army at its largest), so a change that moves a failing
situation without passing it shows. They are reported, never judged.

HOW IT RUNS. Each DLL's games not yet in its folder are played in pieces, one a setup, each piece a process of
its own (`session.py piece`, this tool's alone) that plays as diag.py plays: bar.play_games under the run lock,
on GDI with no draw, every game's log kept. The pieces are launched together and the run lock admits them as
its slots allow (batch.play_stage); six lanes a piece by default, so a first session's four pieces fit its 28
slots at once. A piece plays the bar games first and then the situations, shortest first, so a lane a short
situation frees takes the next. A piece that runs batch.DEADLINE seconds after its admission is stopped; a game
that could not run is played once more.

The table goes to stdout and <the test's folder>/summary.txt (and summary.json); each piece's log to its logs/.
It is one runs-viewer run with a step a game (tools/tarun.py). To read further into a game that parted:
`diag.py logs <the base's folder>/base <the test's folder>/base --grep REGEX --at T`.

Exit 0 no game WORSE (with --neutral: every game SAME), 1 otherwise, 2 a game could not run (or the session could
not start)."""
import argparse
import fcntl
import hashlib
import json
import os
import shutil
import signal
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import bar  # noqa: E402
import batch  # noqa: E402
import diag  # noqa: E402
import exploit  # noqa: E402

CONFIRM = HERE / "confirm.txt"
LANES = 6                       # a piece's lanes: two DLLs' base pieces and their setup pieces fill the lock's 28 slots
SAME, BETTER, WORSE, PARTED, UNRUN = "SAME", "BETTER", "WORSE", "PARTED", "UNRUN"
ROLES = {"base": "the champion's", "test": "the candidate's"}
QUOTE = 160                     # characters printed of a line at a parting


class SessionError(Exception):
    pass


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fixed(src: Path, dst: Path) -> Path:
    """`dst`, a copy of the DLL `src` made at the folder's first session, so a DLL rebuilt where it was built
    changes no game; SessionError when `dst` is another DLL. The copy appears whole or not at all (a rename), so
    a session stopped while copying leaves no part of one to be refused later."""
    if dst.exists():
        if digest(dst) != digest(src):
            raise SessionError(f"{dst.parent.parent} holds the games of another DLL than {src}: a folder's games "
                               f"are one build's (another --tag, or --out)")
        return dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    part = dst.with_name(f"{dst.name}.{os.getpid()}.part")
    shutil.copyfile(src, part)
    os.replace(part, dst)
    return dst


def items(only: list, confirm: Path, scenarios: Path) -> list:
    """The session's games, in the order they are reported: each situation, then each game of the confirm set, as
    {name, id, setup, situation (its name, None for a bar game), game (the spec bar.py plays)}; `only` keeps those
    it names. The situations' scenarios are written under `scenarios`."""
    its = []
    for n in exploit.SITUATIONS:
        g = exploit.situation_game(n, scenarios)
        its.append({"name": g["id"], "id": g["id"], "setup": None, "situation": n, "game": g})
    for gid, setup in batch.regress(confirm):
        its.append({"name": f"{gid} on {setup}" if setup else gid, "id": gid, "setup": setup, "situation": None,
                    "game": diag.bar_game(gid, None, setup)})
    if only:
        unknown = [n for n in only if not any(n in (i["id"], i["situation"]) for i in its)]
        if unknown:
            raise SessionError(f"--only {', '.join(unknown)}: no game of the session's; its games: "
                               + ", ".join(i["situation"] or i["id"] for i in its))
        its = [i for i in its if i["id"] in only or i["situation"] in only]
    return its


def play_order(its: list) -> list:
    """The bar games, then the situations from the shortest cap: the first lanes hold the long games and the
    shortest situations, and each lane a situation frees takes the next."""
    return sorted(its, key=lambda i: (bool(i["situation"]), i["game"]["cap"] if i["situation"] else 0))


def pieces(role: str, root: Path, dll: Path, its: list, lanes: int) -> list:
    """`role`'s pieces: for each setup, its games that `root` holds no played result of (bar.game_kept), on at most
    `lanes` lanes."""
    by_setup = {}
    for i in its:
        if not bar.game_kept(batch.where(root, i["setup"]), i["game"], False):
            by_setup.setdefault(i["setup"], []).append(i)
    return [{"name": f"{role}-{setup or 'base'}", "role": role, "setup": setup, "gating": False, "dll": dll,
             "items": play_order(left), "ids": [i["id"] for i in left], "lanes": min(lanes, len(left)),
             "out": batch.where(root, setup), "scenarios": root / "scenarios"} for setup, left in by_setup.items()]


def piece_command(piece: dict, dll: Path) -> list:
    """The command that plays `piece` with `dll`: this tool's `piece`."""
    cmd = [sys.executable, str(HERE / "session.py"), "piece", "--dll", str(dll), "--folder", str(piece["out"]),
           "--scenarios", str(piece["scenarios"]), "--lanes", str(piece["lanes"])]
    sits = [i["situation"] for i in piece["items"] if i["situation"]]
    ids = [i["id"] for i in piece["items"] if not i["situation"]]
    if sits:
        cmd += ["--situations", ",".join(sits)]
    if ids:
        cmd += ["--ids", ",".join(ids)]
    if piece["setup"]:
        cmd += ["--setup", piece["setup"]]
    return cmd


def play_piece(a) -> int:
    """One piece, in its own process: its bar games and its situations played into its folder (diag.play)."""
    try:
        games = [diag.bar_game(g, None, a.setup) for g in (a.ids or "").split(",") if g]
        games += [exploit.situation_game(n, Path(a.scenarios)) for n in (a.situations or "").split(",") if n]
        return diag.play(games, a.dll, Path(a.folder), a.lanes, a.setup, verb="session")
    except (bar.BarError, OSError, KeyError) as e:
        print(f"session piece: COULD NOT RUN: {e}")
        return 2


# ---- the verdict ----

def judged(item: dict, r: dict) -> tuple:
    """(category, what is said) of a game's result by its own judge -- a situation's exploit verdict, pass above
    FAIL; a bar game's batch.reading -- or (None, why) for a game that could not run."""
    if item["situation"]:
        v, why = exploit.judge_one(item["game"]["verdict"], r)
        if v == exploit.COULD_NOT:
            return None, why
        return (1, f"pass: {why}") if v == exploit.PASS else (0, f"FAIL: {why}")
    cat, text = batch.reading(r)
    return cat, text if cat is not None else f"could not run: {r.get('reason', '')}"


def state_parting(rec_base: Path, rec_test: Path, trees: list) -> tuple:
    """think.state_parting of two recordings, with what cannot be compared as (False, why)."""
    import think
    try:
        return think.state_parting(rec_base, rec_test, trees)
    except think.ThinkError as e:
        return False, f"state not compared: {e}"


def compare(item: dict, base: Path, test: Path, state=None) -> dict:
    """One game's row: {name, word, base, test (what is said of each game), parting (diag.parting's, None when the
    decisions agree or were not compared), figures ({role: exploit.figures}, a situation's alone), state (what
    `state` said of the two recordings' brain state: --state's)}. `state` is state_parting with its trees given,
    None when the brain state is not asked for."""
    row = {"name": item["name"], "id": item["id"], "setup": item["setup"], "parting": None, "figures": None,
           "state": None}
    cats, logs, results = {}, {}, {}
    for role, root in (("base", base), ("test", test)):
        folder = batch.where(root, item["setup"])
        results[role], log = diag.read_game(folder, item["id"])
        cats[role], row[role] = judged(item, results[role])
        if item["situation"] and cats[role] is not None:
            row["figures"] = dict(row["figures"] or {}, **{role: exploit.figures(
                results[role], log, folder / f"{item['id']}.imprec"
                if (folder / f"{item['id']}.imprec").is_file() else None)})
        logs[role] = diag.decisions(log)
        if cats[role] is not None and not logs[role]:
            # two logs with no decision in them agree in every line: no game is SAME on that
            cats[role], row[role] = None, f"{row[role]}; no decision line in its log"
    if cats["base"] is None or cats["test"] is None:
        row["word"] = UNRUN
        return row
    row["parting"] = diag.parting(logs["base"], logs["test"])
    ends = [(r.get("outcome"), r.get("end_tick"), r.get("value")) for r in results.values()]
    if cats["test"] != cats["base"]:
        row["word"] = BETTER if cats["test"] > cats["base"] else WORSE
    elif row["parting"] is None and ends[0] == ends[1] and row["base"] == row["test"]:
        row["word"] = SAME
    else:
        row["word"] = PARTED
    if state is not None:
        parted, row["state"] = state(*(batch.where(root, item["setup"]) / f"{item['id']}.imprec"
                                       for root in (base, test)))
        if parted is False:
            row["word"] = UNRUN
        elif parted is not None and row["word"] == SAME:
            row["word"] = PARTED
    return row


def line(row: dict, width: int) -> str:
    s = f"  {row['word']:<7} {row['name']:<{width}}  base {row['base']} | test {row['test']}"
    if row["word"] in (BETTER, WORSE, PARTED):
        s += (f"; first parting at GameTime {row['parting'][0]}" if row["parting"] else
              "; no decision line differs")
    if row.get("state"):
        s += f"; {row['state']}"
    if row.get("figures"):
        s += "; figures " + " | ".join(f"{role} {exploit.figures_text(f)}" for role, f in row["figures"].items())
    return s


def quoted(row: dict) -> list:
    """The two lines at a row's first parting, as --neutral prints them under it."""
    if not row["parting"]:
        return []
    return [f"      {role}: {(x if x is not None else '(no more lines)')[:QUOTE]}"
            for role, x in zip(("base", "test"), row["parting"][1:])]


def exit_of(rows: list, neutral: bool) -> int:
    """0 no game WORSE (with `neutral`: every game SAME); 1 one is; 2 none is, and one could not run."""
    if any(r["word"] == WORSE or neutral and r["word"] in (BETTER, PARTED) for r in rows):
        return 1
    return 2 if any(r["word"] == UNRUN for r in rows) else 0


def summary(a, roots: dict, rows: list, overran: list, seconds: float, rc: int) -> str:
    lines = [f"session {roots['test'].name}: TEST {a.test}, BASE {a.base} ({roots['base'].name})"
             f"{', neutral' if a.neutral else ''}; {seconds / 60:.1f} min"]
    width = max(len(r["name"]) for r in rows)
    for r in rows:
        lines.append(line(r, width))
        if a.neutral and r["word"] != SAME:
            lines += quoted(r)
    lines += [f"  past {batch.DEADLINE} s and stopped: {n}" for n in overran]
    lines.append(f"  results: base {roots['base']}, test {roots['test']}")
    count = ", ".join(f"{n} {w}" for w in (SAME, BETTER, PARTED, WORSE, UNRUN)
                      for n in [sum(r["word"] == w for r in rows)] if n)
    lines.append(f"session: {('PASS', 'FAIL', 'COULD NOT RUN')[rc]}: {count}")
    return "\n".join(lines)


# ---- the run ----

def run(a, launch=None, cache=None, clock=time.monotonic, sleep=time.sleep) -> int:
    """The session `a` names; `cache` is the folder the champions' folders are under (the main checkout's
    .data/impai/runs/session when none) and `launch` what plays a piece (a batch.Piece of piece_command)."""
    import tarun
    home = batch.common_data() / "impai" / "runs" / "session"
    try:
        for d in (a.base, a.test):
            if not Path(d).is_file():
                raise SessionError(f"no DLL {d}")
        roots = {"base": Path(cache or home) / f"base-{digest(Path(a.base))[:12]}",
                 "test": Path(a.out) if a.out else home / a.tag}
        dlls = {role: fixed(Path(getattr(a, role)), roots[role] / "dll" / f"{role}.dll") for role in roots}
        its = items([x for x in (a.only or "").split(",") if x], Path(getattr(a, "confirm", None) or CONFIRM),
                    roots["test"] / "scenarios")
    except (SessionError, bar.BarError, OSError, ValueError, KeyError) as e:
        print(f"session: COULD NOT RUN: {e}")
        return 2
    launch = launch or (lambda p: batch.Piece(p, p["dll"], roots["test"] / "logs", piece_command))
    jr = tarun.begin("session", [("session", [{"id": i["name"]} for i in its])],
                     title=f"session {roots['test'].name}", pools={"main": len(its)}, gate=True)
    t0 = time.time()
    for i in its:
        jr.start("session", i["name"])
    live, overran = [], []

    def started(p):
        live.append(launch(p))
        return live[-1]
    try:
        with open(roots["base"] / "lock", "w") as lock:
            # one session at a time plays a champion's games: a second waits here, then finds them played
            fcntl.flock(lock, fcntl.LOCK_EX)
            for again in (False, True):
                todo = {role: pieces(role, roots[role], dlls[role], its, a.lanes) for role in roots}
                if not again:
                    for role, ps in todo.items():
                        left = sum(len(p["ids"]) for p in ps)
                        print(f"session: {ROLES[role]} games ({roots[role].name}): {len(its) - left} of "
                              f"{len(its)} already played, {left} to play")
                if not todo["base"]:
                    fcntl.flock(lock, fcntl.LOCK_UN)
                ps = todo["base"] + todo["test"]
                if not ps:
                    break
                if again:
                    print("session: played once more, for their games that could not run: "
                          + ", ".join(p["name"] for p in ps))
                _failed, overran = batch.play_stage(ps, started, clock, sleep, lambda: None)
                if overran:
                    break
    except KeyboardInterrupt:
        for h in live:
            h.signal(signal.SIGINT)
        jr.end("stopped")
        return 2
    trees = getattr(a, "tree", None) or []
    state = (lambda x, y: state_parting(x, y, trees)) if getattr(a, "state", False) else None
    rows = [compare(i, roots["base"], roots["test"], state) for i in its]
    rc = exit_of(rows, a.neutral)
    for r in rows:
        bad = r["word"] == WORSE or a.neutral and r["word"] in (BETTER, PARTED)
        jr.finish("session", r["name"], "unrun" if r["word"] == UNRUN else "fail" if bad else "pass",
                  line(r, 0).strip())
    text = summary(a, roots, rows, overran, time.time() - t0, rc)
    print(text)
    (roots["test"] / "summary.txt").write_text(text + "\n")
    doc = {"verdict": ("pass", "fail", "unrun")[rc], "neutral": bool(a.neutral), "base": str(roots["base"]),
           "overran": overran, "seconds": round(time.time() - t0),
           "games": [dict({k: r[k] for k in ("name", "id", "setup", "word", "base", "test", "figures", "state")},
                          parting=r["parting"][0] if r["parting"] else None) for r in rows]}
    (roots["test"] / "summary.json").write_text(json.dumps(doc, indent=1) + "\n")
    jr.end(("pass", "fail", "unrun")[rc])
    return rc


def main(argv=None) -> int:
    sys.stdout.reconfigure(line_buffering=True)
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["piece"]:
        p = argparse.ArgumentParser(prog="session.py piece", description="one piece of a session, in its process")
        p.add_argument("--dll", required=True)
        p.add_argument("--folder", required=True)
        p.add_argument("--scenarios", required=True)
        p.add_argument("--situations", default=None)
        p.add_argument("--ids", default=None)
        p.add_argument("--setup", default=None)
        p.add_argument("--lanes", type=int, required=True)
        return play_piece(p.parse_args(argv[1:]))
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--test", required=True, help="the candidate DLL")
    p.add_argument("--base", required=True, help="the champion DLL: its games are played once, whatever the session")
    p.add_argument("--tag", required=True, help="the candidate's name: its folder under .data/impai/runs/session/")
    p.add_argument("--only", default=None, metavar="NAME,NAME",
                   help=f"only these games: a comma list of {', '.join(exploit.SITUATIONS)} and the confirm set's ids")
    p.add_argument("--neutral", action="store_true", help="every game must be SAME: a refactor's proof")
    p.add_argument("--state", action="store_true",
                   help="with --neutral: every game's recorded brain state must be equal at every tick as well")
    p.add_argument("--tree", action="append", default=[], metavar="DIR",
                   help="with --state: a source tree whose layout a recording may be read by (think.py's --tree)")
    p.add_argument("--lanes", type=int, default=LANES, help=f"lanes a piece (default {LANES})")
    p.add_argument("--out", default=None, help="the candidate's folder (default .data/impai/runs/session/<tag>)")
    a = p.parse_args(argv)
    if a.lanes < 1:
        p.error("--lanes: at least one")
    if a.state and not a.neutral:
        p.error("--state: with --neutral, whose proof it strengthens")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
