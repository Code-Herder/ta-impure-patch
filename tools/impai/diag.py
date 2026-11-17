#!/usr/bin/env python3
"""diag.py -- one-call diagnosis: play a few games of ImpAI's bar, or read finished ones, and print only the lines
asked for.

    tools/impai/diag.py run --dll D [--dll D2] (--ids ID[,ID...] | --situation NAME[,NAME...]) [--cap T]
                            [--grep REGEX]... [--lines N] [--at T[,T...]] [--near W] [--max M] [--lanes L]
                            [--out DIR]
    tools/impai/diag.py logs DIR [DIR2] [--grep REGEX]... [--lines N] [--at T[,T...]] [--near W] [--max M]
    tools/impai/diag.py build [--export NAME] [--define K=V]...

RUN plays the games as bar.py plays (bar.play_games: under the run lock, on GDI with no draw, each game from its
own seed), the bar's ids or the exploiter suite's situations (exploit.situation_game), on each DLL into a folder of
its own -- DIR/dll1 and DIR/dll2, DIR by default .data/impai/runs/diag/run-<time> -- and then prints what LOGS
prints over them. A game already in its folder is not played again (bar's RESUMING), so a second call with other
patterns reads the same games.

LOGS prints, for each game of DIR (each `<id>.json` of bar's results) and, with DIR2, each game of both:
    the result line     the winner (bar's outcome: A, B or cap), the end tick and the seed, or why it could not run
    the matched lines   each `impai:` line of the game's log that a --grep pattern matches, the first N (--lines)
                        of each pattern, a count of the rest; no --grep, the count of the game's impai lines alone
    the first parting   with two folders, the first GameTime at which the two games' matched lines differ, and the
                        two lines there (every matched line compared, not only the N printed)
--at keeps only lines whose GameTime (`t=` or `gametime=`) lies within --near ticks (default 300) of one of the
GameTimes named; a line with none is then left out. The whole print is held to --max lines (default 60), and what
the cap left out is said in its last line.

BUILD makes a scratch debug DLL of this tree, as it is on disk (uncommitted edits included), into
.data/impai/runs/diag/<NAME>/ddraw.dll, without touching tagpu/ddraw's own objects or DLLs: tagpu/ddraw is copied
(its sources and generated headers, never its objects or DLLs) to <NAME>/tree/tagpu/ddraw beside links to the
tree's tools, art, mods and unditherer, which the build reads, and built there with `tools/runlock.py make`. Each
--define K=V goes into that copy's config.mk as `CPPFLAGS += -DK=V`; a build whose defines differ from the copy's
last starts from no object. It prints the DLL's path, or the compiler's errors and warnings alone. Exit: 0, 1 (the
build failed, or a game could not run), 2 (could not run: an argument, a folder)."""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
DIAG = TREE / ".data" / "impai" / "runs" / "diag"
MAX_LINES = 60
PER_PATTERN = 10
NEAR = 300
TIME = re.compile(r"\b(?:t|gametime)=(\d+)")
# what the scratch copy of tagpu/ddraw leaves out: build products, which the scratch build makes for itself
SKIP = re.compile(r"\.(o|d|dll|map|exe|log)$")
LINKED = ("tools", "art", "mods", "unditherer")


class DiagError(Exception):
    pass


def gametime(line: str):
    m = TIME.search(line)
    return int(m.group(1)) if m else None


def near(line: str, at: list, w: int) -> bool:
    t = gametime(line)
    return t is not None and any(abs(t - x) <= w for x in at)


def selected(log: str, greps: list, at=None, w=NEAR) -> list:
    """`log`'s impai lines that a pattern of `greps` matches, in the log's order, those near `at` alone when it
    names GameTimes."""
    lines = [ln for ln in log.splitlines() if ln.startswith("impai:")]
    if at:
        lines = [ln for ln in lines if near(ln, at, w)]
    return [ln for ln in lines if any(re.search(g, ln) for g in greps)]


def matched(log: str, greps: list, at=None, w=NEAR) -> dict:
    """{pattern: [line, ...]}: selected's lines by the pattern that matches each (a line under every one it
    matches)."""
    lines = selected(log, greps, at, w)
    return {g: [ln for ln in lines if re.search(g, ln)] for g in greps}


def parting(la: list, lb: list):
    """(GameTime, line of a, line of b) at the first of two games' selected lines that differ; None when they
    agree. The GameTime is the earlier of the two lines' own."""
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else None
        y = lb[i] if i < len(lb) else None
        if x != y:
            ts = [gametime(v) for v in (x, y) if v is not None and gametime(v) is not None]
            return (min(ts) if ts else None, x, y)
    return None


def result_line(r: dict) -> str:
    if r.get("outcome") in (None, "could_not_run"):
        return f"COULD NOT RUN: {r.get('reason', 'no result')}"
    win = {"A": "A won", "B": "B won", "cap": "at the cap"}.get(r["outcome"], r["outcome"])
    return f"{win}, end {r.get('end_tick')}, seed {r.get('seed')}"


def read_game(folder: Path, gid: str):
    """(result, log text) of game `gid` in bar's results folder `folder`."""
    try:
        r = json.loads((folder / f"{gid}.json").read_text())
    except (OSError, ValueError):
        r = {"outcome": None, "reason": f"no result {gid}.json in {folder}"}
    try:
        log = (folder / f"{gid}.tagpu.log").read_text(errors="replace")
    except OSError:
        log = ""
    return r, log


def game_ids(folder: Path) -> list:
    return sorted(p.name[:-len(".json")] for p in folder.glob("*.json")
                  if not p.name.endswith(".scenario.json"))


def report(folders: list, greps: list, per: int = PER_PATTERN, at=None, w=NEAR, most: int = MAX_LINES) -> list:
    """The lines LOGS prints over `folders` (one, or two to compare), at most `most`, the last saying what was
    cut."""
    out = []
    names = [f.name for f in folders]
    if len(set(names)) < len(names):
        names = [f"{f.parent.name}/{f.name}" for f in folders]
    ids = game_ids(folders[0])
    if len(folders) > 1:
        ids = [g for g in ids if g in set(game_ids(folders[1]))] or ids
    if not ids:
        return [f"diag: no game results in {folders[0]}"]
    for gid in ids:
        seen = []
        for k, f in enumerate(folders):
            r, log = read_game(f, gid)
            tag = f" [{names[k]}]" if len(folders) > 1 else ""
            out.append(f"== {gid}{tag}: {result_line(r)}")
            m = matched(log, greps, at, w)
            seen.append(selected(log, greps, at, w))
            if not greps:
                n = sum(1 for ln in log.splitlines() if ln.startswith("impai:"))
                out.append(f"   {n} impai lines (--grep to print some)")
            for g, ls in m.items():
                out.extend(f"   {ln}" for ln in ls[:per])
                if len(ls) > per:
                    out.append(f"   ... {len(ls) - per} more lines of /{g}/")
                elif not ls:
                    out.append(f"   (no line matches /{g}/)")
        if len(folders) > 1 and greps:
            p = parting(*seen)
            if p is None:
                out.append("   the two agree in every matched line")
            else:
                t, x, y = p
                out.append(f"   FIRST PARTING at GameTime {t}:")
                out.append(f"     {names[0]}: {x if x is not None else '(no more lines)'}")
                out.append(f"     {names[1]}: {y if y is not None else '(no more lines)'}")
    if len(out) > most:
        cut = len(out) - (most - 1)
        out = out[:most - 1] + [f"diag: {cut} more lines cut at --max {most} (narrow --grep or --at, or raise --max)"]
    return out


def csv_ints(s):
    return [int(x) for x in s.split(",") if x] if s else None


def run_games(a) -> int:
    import bar
    out = Path(a.out) if a.out else DIAG / f"run-{time.strftime('%Y%m%d-%H%M%S')}"
    if bool(a.ids) == bool(a.situation):
        raise DiagError("name the games with --ids or --situation, one of the two")
    if a.situation:
        import exploit
        games = [exploit.situation_game(n, out / "scenarios") for n in a.situation.split(",") if n]
        if a.cap:
            games = [dict(g, cap=a.cap) for g in games]
    else:
        games = [dict(bar.parse_id(g), cap=a.cap or bar.CAP, setup=None, values=[]) for g in bar.read_ids(a.ids)]
    folders = []
    rc = 0
    missing = [d for d in a.dll if not Path(d).is_file()]
    if missing:
        raise DiagError(f"no DLL {missing[0]}")
    for k, dll in enumerate(a.dll):
        f = out / f"dll{k + 1}"
        pa = argparse.Namespace(verb="diag", dll=dll, lanes=a.lanes, setup=None, gpu=False, draw=False,
                                keep_all=False, no_curtail=True)
        rc = max(rc, bar.play_games(games, pa, f))
        folders.append(f)
    print(f"diag: {len(games)} game(s) on {len(a.dll)} DLL(s) in {out}")
    print("\n".join(report(folders, a.grep, a.lines, csv_ints(a.at), a.near, a.max)))
    return 1 if rc else 0


def copy_ddraw(src: Path, dst: Path) -> None:
    """tagpu/ddraw's sources and generated files into `dst`, build products left out, files gone from the source
    removed; a file whose size and time agree is not copied again, so make sees its time unchanged."""
    keep = set()
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for n in files:
            if SKIP.search(n) or rel == Path(".") and n == "config.mk":
                continue
            s, d = Path(root) / n, dst / rel / n
            keep.add(d)
            st = s.stat()
            if d.exists() and d.stat().st_size == st.st_size and int(d.stat().st_mtime) == int(st.st_mtime):
                continue
            shutil.copy2(s, d)
    for root, _dirs, files in os.walk(dst):
        for n in files:
            d = Path(root) / n
            if not SKIP.search(n) and d not in keep and d.name != "config.mk":
                d.unlink()


def compiler_lines(text: str) -> list:
    return [ln for ln in text.splitlines() if re.search(r"\b(error|warning)\b|undefined reference", ln)]


def build(a) -> int:
    name = a.export or f"build-{time.strftime('%Y%m%d-%H%M%S')}"
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
        raise DiagError(f"--export {name!r}: a folder name of letters, digits, '.', '_' and '-'")
    defines = []
    for d in a.define:
        k, _, v = d.partition("=")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k):
            raise DiagError(f"--define {d!r}: K=V with K a C identifier")
        defines.append(f"-D{k}={v}" if _ else f"-D{k}")
    home = DIAG / name
    tree = home / "tree"
    ddraw = tree / "tagpu" / "ddraw"
    ddraw.mkdir(parents=True, exist_ok=True)
    for n in LINKED:
        link = tree / n
        if not link.is_symlink():
            link.symlink_to(TREE / n)
    copy_ddraw(TREE / "tagpu" / "ddraw", ddraw)
    mk = "".join(f"CPPFLAGS += {d}\n" for d in defines)
    cfg = ddraw / "config.mk"
    if not cfg.exists() or cfg.read_text() != mk:
        # an object made under other defines is stale, and make cannot see a flag change
        for root, _dirs, files in os.walk(ddraw):
            for n in files:
                if n.endswith((".o", ".d")):
                    (Path(root) / n).unlink()
        cfg.write_text(mk)
    commit = subprocess.run(["git", "-C", str(TREE), "describe", "--match=NeVeRmAtCh", "--always", "--dirty"],
                            capture_output=True, text=True).stdout.strip() or "UNKNOWN"
    r = subprocess.run([sys.executable, str(TREE / "tools" / "runlock.py"), "make", "--", "-C", str(ddraw),
                        f"COMMIT={commit}-diag", "ddraw.dll"], cwd=TREE, capture_output=True, text=True)
    dll = ddraw / "ddraw.dll"
    if r.returncode != 0 or not dll.exists():
        lines = compiler_lines(r.stdout + r.stderr) or (r.stdout + r.stderr).splitlines()[-20:]
        print("\n".join(lines[:MAX_LINES]))
        print(f"diag build: FAILED (make exit {r.returncode}); the tree is {ddraw}")
        return 1
    shutil.copy2(dll, home / "ddraw.dll")
    print(home / "ddraw.dll")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)

    def extraction(q):
        q.add_argument("--grep", action="append", default=[], metavar="REGEX",
                       help="print the impai lines this matches (repeatable)")
        q.add_argument("--lines", type=int, default=PER_PATTERN, help="lines printed of each pattern")
        q.add_argument("--at", default=None, metavar="T,T", help="only lines near these GameTimes")
        q.add_argument("--near", type=int, default=NEAR, help="ticks either side of each --at")
        q.add_argument("--max", type=int, default=MAX_LINES, help="lines printed in all")
    r = sub.add_parser("run", help="play games on one or two DLLs, then print their lines")
    r.add_argument("--dll", action="append", required=True, help="a DLL; twice to compare two")
    r.add_argument("--ids", default=None, help="bar ids: a comma list, a file of ids, or a pair's name")
    r.add_argument("--situation", default=None, help="exploit situations, a comma list")
    r.add_argument("--cap", type=int, default=None, help="the GameTime the games end at")
    r.add_argument("--lanes", type=int, default=2)
    r.add_argument("--out", default=None)
    extraction(r)
    lg = sub.add_parser("logs", help="print the lines of finished games")
    lg.add_argument("dirs", nargs="+", type=Path, help="bar's results folder; two to compare")
    extraction(lg)
    b = sub.add_parser("build", help="a scratch debug DLL of this tree")
    b.add_argument("--export", default=None, metavar="NAME", help="the build's folder under .data/impai/runs/diag")
    b.add_argument("--define", action="append", default=[], metavar="K=V", help="a C define (repeatable)")
    a = p.parse_args(argv)
    try:
        for g in getattr(a, "grep", []):
            re.compile(g)
        if a.verb == "run":
            if len(a.dll) > 2:
                raise DiagError("--dll at most twice")
            return run_games(a)
        if a.verb == "logs":
            if len(a.dirs) > 2:
                raise DiagError("one folder, or two to compare")
            missing = [d for d in a.dirs if not d.is_dir()]
            if missing:
                raise DiagError(f"no folder {missing[0]}")
            print("\n".join(report(a.dirs, a.grep, a.lines, csv_ints(a.at), a.near, a.max)))
            return 0
        return build(a)
    except (DiagError, re.error, ValueError) as e:
        print(f"diag: COULD NOT RUN: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
