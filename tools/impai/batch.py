#!/usr/bin/env python3
"""batch.py -- the per-batch loop: every gate a brain batch must pass, in one command.

    tools/impai/batch.py --test DLL --base DLL --tag TAG [--mode notworse|improve] [--margin-lanes L]
                         [--gate-held-out] [--out DIR]

THE GATES, in order; the first that fails fails the batch (research/notes/impai/testing.md, *Per brain batch
and at milestones*):

    regression  the games of regress.txt, the brain's known-hard games: every one a win for ImpAI
    hota        the 40 hota games of the broad check (games 1 and 2 of each map and side pairing): every one a win
    margin      the paired margin test, TEST against BASE (margin.py run; NOT-WORSE unless --mode): accepted
    mods        the fair mods: MOD_IDS on each setup of MOD_SETUPS, 12 games: every one a win

and one check that reports and judges nothing, played whatever the gates do:

    held-out    HELD_OUT, the eight p:L5 games OTA AI won on Steel Jungle and Lava Alley at 57d39d71 (round 3):
                each game's outcome and whether ImpAI reached the cap ahead in value. With --gate-held-out every
                one must be a win and it gates, after the mods.

A win is the outcome "A": ImpAI's opponent gone before the cap. A game at the cap is not a win, whatever its
value; a game's stalls are reported, not judged.

HOW IT RUNS. Every game but the margin test's is played with TEST's DLL, through `bar.py play --dll`, in
pieces: each piece one `bar.py play` of one setup's games, at most one game a lane, so a piece is one wave of
games and ends well inside DEADLINE. The regression set, the hota games and the mods play beside each other --
a game in both the regression set and the hota games is played once -- and the held-out games beside them; every
piece is launched at once and the run lock admits them as its slots allow. A piece that runs DEADLINE seconds
after its admission is stopped (SIGTERM, bar.py's stop), and its gate could not run. FAIL FAST: once a game of
a gate has ended in anything but a win, the gate has failed, every gating piece still playing is stopped
(SIGINT) and the margin test is not played; the held-out piece plays on. A piece with a game that could not run
is played once more (bar.py's resume plays only those). The margin test is played only once every other gate
has passed, since it is the dearest. A margin test with no decision at the end of its order is no pass.

Both DLLs are copied into the run's directory first, so a DLL rebuilt elsewhere changes no game, and a
directory whose copies differ from the DLLs given is refused (margin.fix_dlls). The results go to
<out>/<setup or base>/, the margin test to <out>/margin/, each piece's log to <out>/logs/, and the one-screen
summary to stdout and <out>/summary.txt (and summary.json). It is one runs-viewer run with a step a gate
(tools/tarun.py); its games and the margin test's run inherit its id (TARUN_RUN).

Exit 0 every gate passed, 1 a gate failed, 2 a gate could not run (or the batch could not start)."""
import argparse
import json
import math
import os
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
import margin  # noqa: E402

REGRESS = HERE / "regress.txt"
HOTA = [g for g in bar.ids("hota") if g.rsplit("-", 1)[1] in ("1", "2")]
MOD_SETUPS = ("loader+tadr-ota", "392+tadr-2026.8.6", "prota-4.8")
MOD_IDS = ("bar-nota-pincushion-ac-1", "bar-nota-pincushion-ac-2", "bar-nota-lavamania-ca-1",
           "bar-nota-lavamania-ca-2")
# the p:L5 games of round 3 at 57d39d71 (.data/impai/runs/r3/57d39d71/pL5) that OTA AI won on the two maps where
# ImpAI drew or lost to it: one outright, seven by value at the cap
HELD_OUT = ("bar-p:L5-lavaalley-aa-8", "bar-p:L5-lavaalley-ac-2", "bar-p:L5-lavaalley-ca-4",
            "bar-p:L5-lavaalley-cc-6", "bar-p:L5-steeljungle-ac-7", "bar-p:L5-steeljungle-ac-8",
            "bar-p:L5-steeljungle-ca-1", "bar-p:L5-steeljungle-ca-2")
GATES = ("regression", "hota", "margin", "mods")
HELD = "held-out"
BASE_LANES = 7                  # a piece's lanes on the base game: two pieces fill the lock's 28 slots
SETUP_LANES = 4                 # on a mod setup: one wave of its four games
DEADLINE = 600                  # seconds a piece may play after its admission
POLL = 5
PASS, FAIL, UNRUN = "pass", "fail", "unrun"
EXIT = {PASS: 0, FAIL: 1, UNRUN: 2}


def regress(path: Path = REGRESS) -> list:
    """[(id, setup)] of the regression set: a line is an id and its setup (none for the base game); # comments."""
    out = []
    for ln in Path(path).read_text().splitlines():
        words = ln.split("#", 1)[0].split()
        if words:
            bar.parse_id(words[0])
            out.append((words[0], words[1] if len(words) > 1 else None))
    if len(set(out)) != len(out):
        raise bar.BarError(f"{path}: a game listed twice")
    return out


def gate_games(path: Path = REGRESS) -> dict:
    """{gate: [(id, setup)]} of every gate that plays bar games, the held-out check's included."""
    return {"regression": regress(path), "hota": [(g, None) for g in HOTA],
            "mods": [(g, s) for s in MOD_SETUPS for g in MOD_IDS], HELD: [(g, None) for g in HELD_OUT]}


def where(out: Path, setup) -> Path:
    return Path(out) / (setup or "base")


def pieces(games: dict, out: Path) -> list:
    """The pieces, in the order they are launched -- the run lock admits in that order: the gating games of each
    setup, each played once, in gate order, cut at the setup's lanes, and the held-out games, a piece of their own;
    the mod setups' pieces after the first base piece and the held-out piece after the second, so they play beside
    the base game's rather than after all of them."""
    seen, by_setup = set(), {}
    for gate in ("regression", "hota", "mods"):
        for g in games[gate]:
            if g not in seen:
                seen.add(g)
                by_setup.setdefault(g[1], []).append(g[0])
    base, setups = [], []
    for setup, ids in by_setup.items():
        lanes = SETUP_LANES if setup else BASE_LANES
        for k in range(0, len(ids), lanes):
            (setups if setup else base).append(
                {"name": f"{setup or 'base'}-{k // lanes + 1}", "setup": setup, "gating": True,
                 "ids": ids[k:k + lanes], "lanes": lanes, "out": where(out, setup)})
    held = [g for g, _s in games[HELD]]
    held_piece = {"name": HELD, "setup": None, "gating": False, "ids": held, "lanes": len(held),
                  "out": where(out, None)}
    return base[:1] + setups + base[1:2] + [held_piece] + base[2:]


# ---- the play ----

class Piece:
    """A piece being played: its `bar.py play`, its log, and whether the run lock has admitted it."""

    def __init__(self, piece: dict, dll: Path, logs: Path):
        logs.mkdir(parents=True, exist_ok=True)
        self.log = logs / f"{piece['name']}.log"
        cmd = [sys.executable, str(HERE / "bar.py"), "play", "--ids", ",".join(piece["ids"]),
               "--lanes", str(piece["lanes"]), "--dll", str(dll), "--out", str(piece["out"])]
        if piece["setup"]:
            cmd += ["--setup", piece["setup"]]
        with open(self.log, "a") as f:
            self.proc = subprocess.Popen(cmd, cwd=str(TREE), stdout=f, stderr=subprocess.STDOUT)

    def admitted(self) -> bool:
        """The run lock has admitted it: it said so, or a lane of it started (the lock says nothing of an
        admission that did not wait)."""
        try:
            text = self.log.read_text(errors="replace")
        except OSError:
            return False
        return "took the run lock" in text or " on Xvfb " in text

    def poll(self):
        return self.proc.poll()

    def signal(self, sig):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)


def results(games, out: Path) -> dict:
    """{(id, setup): its result, or None}."""
    got = {}
    for gid, setup in games:
        try:
            got[(gid, setup)] = json.loads((where(out, setup) / f"{gid}.json").read_text())
        except (OSError, ValueError):
            got[(gid, setup)] = None
    return got


def lost(rs: dict) -> list:
    """The games of `rs` that ended in anything but a win."""
    return [g for g, r in rs.items() if r and r.get("outcome") in ("B", "cap")]


def verdict(games, out: Path) -> tuple:
    """(PASS | FAIL | UNRUN, detail) of a gate whose games have all been played: FAIL at a game not won, UNRUN at
    a game with no result or one that could not run."""
    rs = results(games, out)
    bad = lost(rs)
    if bad:
        return FAIL, f"{len(rs) - len(bad)} of {len(rs)} won; not won: " + ", ".join(_name(g) for g in bad)
    unrun = [g for g, r in rs.items() if not r or r.get("outcome") != "A"]
    if unrun:
        return UNRUN, f"{len(unrun)} could not run: " + ", ".join(_name(g) for g in unrun[:6])
    stalls = sum(bool(any((r.get("stalls") or {}).values())) for r in rs.values())
    return PASS, f"{len(rs)} of {len(rs)} won" + (f" ({stalls} with a stall)" if stalls else "")


def _name(g) -> str:
    return g[0] + (f" on {g[1]}" if g[1] else "")


def held_out(games, out: Path) -> list:
    """The held-out check's lines: each game's outcome, and at the cap A's value over B's and whether A reached
    it ahead."""
    lines = []
    for (gid, _s), r in results(games, out).items():
        if not r or r.get("outcome") in (None, "could_not_run"):
            lines.append(f"{gid}: could not run")
        elif r["outcome"] == "A":
            lines.append(f"{gid}: won at {r.get('end_tick')}")
        elif r["outcome"] == "B":
            lines.append(f"{gid}: lost at {r.get('end_tick')}")
        else:
            v = r.get("value") or {}
            a, b = ((v.get(s) or {}).get("live") for s in ("A", "B"))
            if r.get("draw") or a is None or b is None:
                lines.append(f"{gid}: at the cap, no value to compare")
            else:
                ratio = a / b if b else math.inf
                lines.append(f"{gid}: reached the cap {'ahead' if a > b else 'not ahead'}, value A/B {ratio:.2f}")
    return lines


def play_stage(ps: list, launch, clock, sleep, failed_now) -> tuple:
    """Every piece of `ps` launched at once and watched to its end: (the first gate failed, if any; the pieces
    that ran past DEADLINE). A piece past DEADLINE after its admission is stopped with SIGTERM; once failed_now()
    names a gate, every gating piece still playing is stopped with SIGINT."""
    live = [dict(p, h=launch(p), admit=None, end=None, termed=False) for p in ps]
    failed, overran = None, []
    while any(p["end"] is None for p in live):
        sleep(POLL)
        for p in live:
            if p["end"] is not None:
                continue
            if p["admit"] is None and p["h"].admitted():
                p["admit"] = clock()
            if p["h"].poll() is not None:
                p["end"] = clock()
            elif p["admit"] is not None and clock() - p["admit"] > DEADLINE and not p["termed"]:
                p["h"].signal(signal.SIGTERM)
                p["termed"] = True
                overran.append(p["name"])
        if failed is None:
            failed = failed_now()
            if failed:
                for p in live:
                    if p["gating"] and p["end"] is None:
                        p["h"].signal(signal.SIGINT)
    return failed, overran


def play_margin(a, out: Path) -> int:
    """The margin test (margin.py run) into <out>/margin: its exit code."""
    with open(Path(out) / "logs" / "margin.log", "a") as f:
        return subprocess.call([sys.executable, str(HERE / "margin.py"), "run", "--base", str(a.base), "--test",
                                str(a.test), "--mode", a.mode, "--out", str(Path(out) / "margin"),
                                "--lanes", str(a.margin_lanes)], cwd=str(TREE), stdout=f, stderr=subprocess.STDOUT)


MARGIN = {margin.ACCEPT: (PASS, "accepted"), margin.REJECT: (FAIL, "rejected"),
          margin.COULD_NOT: (UNRUN, "could not judge"), margin.UNDECIDED: (FAIL, "no decision: no pass")}


def run(a, launch=None, play_margin=play_margin, clock=time.monotonic, sleep=time.sleep) -> int:
    import tarun
    out = Path(a.out)
    try:
        games = gate_games(Path(getattr(a, "regress", None) or REGRESS))
        dlls = margin.fix_dlls(out, Path(a.base), Path(a.test))
    except (bar.BarError, margin.CouldNotJudge, OSError) as e:
        print(f"batch: COULD NOT RUN: {e}")
        return EXIT[UNRUN]
    (out / "logs").mkdir(parents=True, exist_ok=True)
    launch = launch or (lambda p: Piece(p, dlls["test"], out / "logs"))
    gate_held = getattr(a, "gate_held_out", False)
    steps = list(GATES) + [HELD]
    jr = tarun.begin("batch", [("batch", [{"id": s} for s in steps])], title=f"batch {out.name}",
                     pools={"main": len(steps)}, gate=True)
    t0 = time.time()
    for s in ("regression", "hota", "mods", HELD):
        jr.start("batch", s)
    ps = pieces(games, out)

    def failed_now():
        for gate in ("regression", "hota", "mods"):
            if lost(results(games[gate], out)):
                return gate
        return None
    states = {}
    try:
        failed, overran = play_stage(ps, launch, clock, sleep, failed_now)
        if not failed and not overran:
            again = [p for p in ps if any(r is None or r.get("outcome") == "could_not_run"
                                          for r in results([(g, p["setup"]) for g in p["ids"]], out).values())]
            if again:
                print("batch: played once more, for their games that could not run: "
                      + ", ".join(p["name"] for p in again))
                failed, overran = play_stage(again, launch, clock, sleep, failed_now)
    except KeyboardInterrupt:
        jr.end("stopped")
        return EXIT[UNRUN]
    for gate in ("regression", "hota", "mods"):
        states[gate] = verdict(games[gate], out)
        if failed and gate != failed and states[gate][0] == UNRUN:
            states[gate] = (UNRUN, f"stopped: {failed} failed")
    for name in overran:
        print(f"batch: the piece {name} played past {DEADLINE} s and was stopped")
    held = held_out(games[HELD], out)
    held_state = verdict(games[HELD], out) if gate_held else ("done", f"{len(held)} games, reported")
    for gate in ("regression", "hota", "mods"):
        jr.finish("batch", gate, states[gate][0], states[gate][1])
    jr.finish("batch", HELD, held_state[0], held_state[1])
    gating = ("regression", "hota", "mods") + ((HELD,) if gate_held else ())
    states[HELD] = held_state
    if all(states[g][0] == PASS for g in gating):
        jr.start("batch", "margin")
        try:
            rc = play_margin(a, out)
        except KeyboardInterrupt:
            jr.end("stopped")
            return EXIT[UNRUN]
        states["margin"] = MARGIN.get(rc, (UNRUN, f"exit {rc}"))
        jr.finish("batch", "margin", states["margin"][0], states["margin"][1])
    else:
        states["margin"] = ("skipped", "an earlier gate did not pass")
        jr.finish("batch", "margin", "skipped", states["margin"][1])
    order = list(GATES) + ([HELD] if gate_held else [])
    worst = (FAIL if any(states[g][0] == FAIL for g in order) else
             UNRUN if any(states[g][0] not in (PASS,) for g in order) else PASS)
    text = summary(a, out, states, held, overran, time.time() - t0, worst, gate_held)
    print(text)
    (out / "summary.txt").write_text(text + "\n")
    (out / "summary.json").write_text(json.dumps({"verdict": worst, "gates": states, "held_out": held,
                                                  "overran": overran, "seconds": round(time.time() - t0)},
                                                 indent=1) + "\n")
    jr.end({PASS: "pass", FAIL: "fail"}.get(worst, "unrun"))
    return EXIT[worst]


def summary(a, out, states, held, overran, seconds, worst, gate_held) -> str:
    lines = [f"batch {out.name}: TEST {a.test}, BASE {a.base}, margin {a.mode}; {seconds / 60:.1f} min"]
    for g in GATES:
        st, detail = states[g]
        lines.append(f"  {g:<10} {st.upper():<7} {detail}")
    lines.append(f"  {HELD:<10} {states[HELD][0].upper():<7} {'gates' if gate_held else 'reports, judges nothing'}")
    lines += [f"    {ln}" for ln in held]
    lines += [f"  past {DEADLINE} s and stopped: {n}" for n in overran]
    lines.append(f"  results: {out}")
    lines.append(f"batch: {worst.upper()}")
    return "\n".join(lines)


def common_data() -> Path:
    """The main checkout's .data, which every worktree shares."""
    got = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--git-common-dir"], capture_output=True,
                         text=True).stdout.strip()
    return (TREE / (got or ".git")).resolve().parent / ".data"


def main(argv=None) -> int:
    sys.stdout.reconfigure(line_buffering=True)
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--test", required=True, help="the candidate DLL: every game but the margin test's BASE arm")
    p.add_argument("--base", required=True, help="the champion DLL: the margin test's BASE")
    p.add_argument("--tag", required=True, help="the run's name: its directory under .data/impai/runs/batch/")
    p.add_argument("--out", default=None, help="the run's directory (default .data/impai/runs/batch/<tag>)")
    p.add_argument("--mode", choices=sorted(margin.MODES), default="notworse", help="the margin test's mode")
    p.add_argument("--margin-lanes", type=int, default=6, choices=range(1, margin.MAX_LANES + 1),
                   metavar=f"1..{margin.MAX_LANES}", help="the margin test's lanes an arm")
    p.add_argument("--gate-held-out", action="store_true",
                   help="the held-out games gate (every one a win) instead of reporting")
    a = p.parse_args(argv)
    a.out = a.out or str(common_data() / "impai" / "runs" / "batch" / a.tag)
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
