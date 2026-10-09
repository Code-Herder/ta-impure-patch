#!/usr/bin/env python3
"""tarun -- what a gate or a game is doing right now, for the runs viewer (.claude/skills/ta-runs).

A RUN is one invocation that plays or checks something: a tareplay tier, a tacompat suite, one
tacli instance's game, a landing. It is planned up front as items (fixtures, setups) of steps
(lanes, stages), and each step goes planned -> running -> done | pass | fail | unrun | skipped.
`done` is a step that ended with no verdict of its own (a lane whose checks run later); `unrun`
is one that could not run, or never started because the run stopped.

    tarun.py begin TOOL [--title T] [--jobs N] STEP...   a run driven from the shell: its steps
                                                         in order, or N at once with --jobs
    tarun.py step  RUN STEP start|done|pass|fail|unrun|skipped [--detail TEXT]
    tarun.py end   RUN pass|fail|unrun|stopped
    tarun.py ls                                          this session's runs

RUN is a run id, or a tool name: the newest open run of that tool in this session and worktree.

The files live outside the repository (TARUN_DIR, default $XDG_DATA_HOME/ta-runs):

    sessions/<session>/<run>.json   the run as it stands, replaced whole on every change: written
                                    beside and renamed into place, so a reader never sees half
    history.jsonl                   every finished step's seconds, one line each, appended

<session> is CLAUDE_CODE_SESSION_ID, which Claude Code exports to every shell it starts, its
subagents' included; a run started outside Claude Code is filed under `terminal`.

LIVENESS. A run names the process whose life is its own -- the tool's, the game's for a tacli
instance, the Claude Code session's (CLAUDE_PID) for a run these commands drive -- with that
process's start time from /proc, so a reader tells a run that died from one still going by the
process itself, never by how long the file has been quiet, and a reused PID is not mistaken for it.

NESTING. A run started while TARUN_RUN names one (a process a gate started) is that run's child;
otherwise, one started while this session and worktree have a run open that these commands drive
(a landing) is filed under that run's running step. A tacli call under a gate writes nothing: the
gate's own step already names the instance it plays on.

THE ESTIMATE. Each step's expected seconds are the median of its last five finished runs with the
same tool, item, step and variant (the run's speed settings); a running step is left whichever is
longer of that less its elapsed time and, with GameTime readings, their rate's time to its target. The remaining steps are then scheduled onto the run's
pools (a pool is the workers that play one kind of step: tareplay's -j games, tacompat's network
games) in the order the tool queues them, each after the steps it waits for. A step with no history
counts as zero, and the estimate then says it is a lower bound; with nothing known it is absent.

The journal never fails the tool that writes it: an error disables it for the rest of the run and
is said once on stderr. Nothing the tool reads is ever written here.
"""
import argparse
import fcntl
import functools
import itertools
import json
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

VERSION = 1
ENDED = {"done", "pass", "fail", "unrun", "skipped"}
STEP_STATES = {"start"} | ENDED
RUN_STATES = {"pass", "fail", "unrun", "stopped"}
KEEP_HISTORY = 5                # finished runs the median is taken over
PROGRESS_EVERY = 2.0            # seconds between GameTime-only rewrites of one run
PRUNE_DAYS = 14                 # a session folder untouched this long goes
SEQ = itertools.count(1)        # a run id is unique per process even within one second


def root() -> Path:
    if os.environ.get("TARUN_DIR"):
        return Path(os.environ["TARUN_DIR"]).expanduser()
    base = os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share"
    return Path(base) / "ta-runs"


def session_id() -> str:
    return os.environ.get("CLAUDE_CODE_SESSION_ID") or "terminal"


def session_dir(sid=None) -> Path:
    return root() / "sessions" / (sid or session_id())


def pid_start(pid):
    """The process's start time in clock ticks since boot (/proc/<pid>/stat field 22), or None
    when it is gone. The comm field may hold spaces and parentheses, so fields are counted from
    the last ')'."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except (OSError, ValueError):
        return None
    return int(stat.rsplit(")", 1)[1].split()[19])


def alive(doc) -> bool:
    pid, start = doc.get("pid"), doc.get("pid_start")
    return bool(pid) and start is not None and pid_start(pid) == start


def where(cwd=None):
    """(worktree folder, branch) of the checkout cwd is in, or (None, None)."""
    try:
        out = subprocess.run(["git", "-C", str(cwd or os.getcwd()), "rev-parse", "--show-toplevel",
                              "--abbrev-ref", "HEAD"], capture_output=True, text=True,
                             timeout=10).stdout.split("\n")
        return (out[0] or None), (out[1] or None)
    except (OSError, subprocess.SubprocessError, IndexError):
        return None, None


def write_json(path: Path, doc):
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    tmp.write_text(json.dumps(doc, separators=(",", ":")))
    os.replace(tmp, path)


def read_runs(sid=None):
    """Every run file of the session, newest first, skipping one that does not parse."""
    runs = []
    d = session_dir(sid)
    for p in d.glob("*.json") if d.is_dir() else ():
        try:
            runs.append(json.loads(p.read_text()))
        except (OSError, ValueError):
            continue
    return sorted(runs, key=lambda r: r.get("started", 0), reverse=True)


class SessionLock:
    """The session folder's lock, for a read-modify-write of a run another process may also edit
    (a run these commands drive, a tacli instance's). A gate writes its own file alone."""

    def __init__(self, sid=None):
        self.path = session_dir(sid) / ".lock"

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(self.path, "a")
        fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        self.f.close()


def load_history(tool):
    """{(item, step, variant): [seconds, ...]} of the tool's finished steps, oldest first."""
    got = {}
    try:
        with open(root() / "history.jsonl") as f:
            for line in f:
                try:
                    h = json.loads(line)
                except ValueError:
                    continue
                if h.get("tool") == tool:
                    got.setdefault((h["item"], h["step"], h.get("variant", "")), []).append(
                        h["seconds"])
    except OSError:
        pass
    return got


def append_history(entry):
    with open(root() / "history.jsonl", "a") as f:
        f.write(json.dumps(entry, separators=(",", ":")) + "\n")


def prune():
    """Session folders nobody has written to for PRUNE_DAYS. history.jsonl stays: it is what every
    later estimate is made of."""
    cut = time.time() - PRUNE_DAYS * 86400
    base = root() / "sessions"
    for d in base.iterdir() if base.is_dir() else ():
        try:
            newest = max([d.stat().st_mtime] + [p.stat().st_mtime for p in d.iterdir()])
        except OSError:
            continue
        if newest < cut:
            shutil.rmtree(d, ignore_errors=True)


def guarded(fn):
    """A method of the journal: whatever goes wrong in it switches the journal off, never the
    tool that called it."""
    @functools.wraps(fn)
    def call(self, *a, **k):
        if self.off:
            return None
        try:
            return fn(self, *a, **k)
        except Exception as e:  # noqa: BLE001 -- the journal must not cost the run
            self.off = True
            print(f"tarun: journal off for this run: {type(e).__name__}: {e}", file=sys.stderr)
            return None
    return call


def schedule(doc, history, now):
    """(seconds left, lower bound) for the run, or (None, False) when nothing is known: the
    steps left, laid onto the run's pools in queue order after the steps each waits for."""
    pools = doc.get("pools") or {}
    variant = doc.get("variant", "")
    # each bounded pool's workers, by when each frees up; a pool of 0 (or none named) never waits
    free = {p: [] for p, cap in pools.items() if cap}
    finish, todo, unknown, known = {}, [], False, False

    # a step with no history of its own (a new fixture, the first run of a variant) borrows the
    # median of the same step over the items that have one, each item counted once
    by_step = {}
    for (i, step, v), past in history.items():
        if v == variant and past:
            by_step.setdefault(step, []).append(statistics.median(past[-KEEP_HISTORY:]))

    def expect(item, s):
        past = history.get((item, s["id"], variant))
        if past:
            return statistics.median(past[-KEEP_HISTORY:])
        lent = by_step.get(s["id"])
        return statistics.median(lent) if lent else None

    for item in doc["items"]:
        for s in item["steps"]:
            key = (item["id"], s["id"])
            if s["state"] in ENDED:
                finish[key] = 0.0
            elif s["state"] == "running":
                # the larger of what history leaves and what the GameTime rate leaves: history
                # knows the lane's tail after its last GameTime (dumps, collection, the exit),
                # the rate knows a lane running slower than it ever has
                e = expect(item["id"], s)
                guesses = [] if e is None else [max(0.0, e - (now - s["started"]))]
                g = s.get("gt_seen")
                if g and s.get("gt_target") and g[1] > g[0] and s["gt"] < s["gt_target"]:
                    rate = (g[1] - g[0]) / max(1e-3, g[3] - g[2])       # GameTime a second
                    guesses.append((s["gt_target"] - s["gt"]) / rate)
                left = max(guesses) if guesses else None
                if left is None:
                    unknown, left = True, 0.0
                else:
                    known = True
                finish[key] = left
                if s.get("pool", "main") in free:
                    free[s.get("pool", "main")].append(left)
            else:
                todo.append((item["id"], s))
    for p in free:
        free[p] += [0.0] * max(0, pools[p] - len(free[p]))
    while todo:
        ready = [(i, s) for i, s in todo if all((i, a) in finish for a in s.get("after", ()))]
        if not ready:
            return (None, False) if not known else (max(finish.values(), default=0.0), True)

        def start_at(i, s):
            dep = max([finish[(i, a)] for a in s.get("after", ())], default=0.0)
            workers = free.get(s.get("pool", "main"))
            return max(dep, min(workers)) if workers else dep
        i, s = min(ready, key=lambda r: (start_at(*r), r[1].get("order", 0)))
        t = start_at(i, s)
        e = expect(i, s)
        if e is None:
            unknown, e = True, 0.0
        else:
            known = True
        finish[(i, s["id"])] = t + e
        workers = free.get(s.get("pool", "main"))
        if workers:
            workers[workers.index(min(workers))] = t + e
        todo.remove((i, s))
    if not known:
        return None, False
    return max(finish.values(), default=0.0), unknown


class Run:
    """One run's journal. A step is (item, step id); an item-less run has one item, ""."""

    def __init__(self, doc, history, owned):
        self.doc, self.history, self.owned = doc, history, owned
        self.path = session_dir(doc["session"]) / f"{doc['id']}.json"
        self.lock = threading.RLock()
        self.off = False
        self.wrote = 0.0
        self.steps = {(i["id"], s["id"]): s for i in doc["items"] for s in i["steps"]}

    @guarded
    def start(self, item, step, **fields):
        with self.lock:
            s = self.steps[(item, step)]
            for stale in ("detail", "seconds", "gt", "gt_seen"):    # a step started again
                s.pop(stale, None)
            s.update(state="running", started=time.time(), ended=None, **fields)
            self.write()

    @guarded
    def progress(self, item, step, gt):
        """A GameTime reading of a running step: written at most every PROGRESS_EVERY seconds."""
        with self.lock:
            s = self.steps.get((item, step))
            if not s or s["state"] != "running":
                return
            now = time.time()
            seen = s.get("gt_seen")
            s["gt"] = gt
            # (first GameTime, latest, when first, when latest): the rate the estimate extrapolates
            s["gt_seen"] = [gt, gt, now, now] if not seen else [seen[0], gt, seen[2], now]
            if now - self.wrote >= PROGRESS_EVERY:
                self.write()

    @guarded
    def finish(self, item, step, state, detail=None):
        assert state in ENDED, state
        with self.lock:
            s = self.steps[(item, step)]
            now = time.time()
            s.update(state=state, ended=now)
            if detail:
                s["detail"] = detail
            if s.get("started"):
                s["seconds"] = round(now - s["started"], 1)
                # a step that could not run says nothing about how long the next one takes
                if state != "unrun":
                    entry = {"tool": self.doc["tool"], "item": item, "step": step,
                             "variant": self.doc.get("variant", ""), "seconds": s["seconds"],
                             "at": round(now)}
                    append_history(entry)
                    self.history.setdefault((item, step, entry["variant"]), []).append(
                        s["seconds"])
            self.write()

    @guarded
    def never(self, item, steps, detail):
        """Steps the tool will not start after all: each one still planned ends unrun. A step
        already started or ended, or not in the plan, is left as it is."""
        with self.lock:
            now = time.time()
            for step in steps:
                s = self.steps.get((item, step))
                if s and s["state"] == "planned":
                    s.update(state="unrun", ended=now, detail=detail)
            self.write()

    @guarded
    def checks(self, item, passed, failed=(), unrun=()):
        """An item's checks: the count that passed and, by name, every one that did not."""
        with self.lock:
            it = next(i for i in self.doc["items"] if i["id"] == item)
            it["checks"] = {"pass": passed, "fail": list(failed), "unrun": list(unrun)}
            self.write()

    @guarded
    def note(self, **fields):
        """Run-level facts: `summary` (where the tool's own report is), `last` (a tacli
        instance's latest command)."""
        with self.lock:
            self.doc.update(fields)
            self.write()

    @guarded
    def end(self, state):
        assert state in RUN_STATES, state
        with self.lock:
            now = time.time()
            for s in self.steps.values():
                if s["state"] in ("planned", "running"):
                    s.update(state="unrun", ended=now)
            self.doc.update(state=state, ended=now)
            self.write()
        if self.owned and os.environ.get("TARUN_RUN") == self.doc["id"]:
            del os.environ["TARUN_RUN"]

    def write(self):
        now = time.time()
        for it in self.doc["items"]:
            it["state"] = item_state(it)
        left, lower = (None, False) if self.doc.get("ended") else schedule(self.doc, self.history,
                                                                            now)
        self.doc["eta"] = None if left is None else {"at": now, "seconds": round(left),
                                                     "lower_bound": lower}
        self.doc["updated"] = now
        self.path.parent.mkdir(parents=True, exist_ok=True)
        write_json(self.path, self.doc)
        self.wrote = now


def item_state(it):
    states = {s["state"] for s in it["steps"]}
    c = it.get("checks") or {}
    if "fail" in states or c.get("fail"):
        return "fail"
    if "running" in states:
        return "running"
    if states <= {"planned"}:
        return "planned"
    if "planned" in states:
        return "running"
    if "unrun" in states or c.get("unrun"):
        return "unrun"
    return "pass" if states & {"pass"} or c else "done"


class Null:
    """The journal when there is none: every call does nothing."""
    off = True
    doc = {}

    def __getattr__(self, name):
        return lambda *a, **k: None


NULL = Null()


def parent_for(sid, top):
    """The run a new one is filed under: TARUN_RUN's, else the newest open run that these
    commands drive in this session and worktree, at its running step."""
    named = os.environ.get("TARUN_RUN")
    runs = read_runs(sid)
    if named:
        return {"run": named, "step": None}
    for r in runs:
        if r.get("adopts") and not r.get("ended") and r.get("worktree") == top and alive(r):
            running = [s["id"] for i in r["items"] for s in i["steps"] if s["state"] == "running"]
            return {"run": r["id"], "step": running[-1] if running else None}
    return None


def begin(tool, items, *, kind="", title="", pools=None, variant="", owner="process", pid=None,
          gate=False, adopts=False, **fields):
    """A new run, planned. `items` is [(item id, [step, ...])], each step a dict with `id` and
    optionally `after` (step ids of the same item it waits for), `pool` and `order` (its place in
    the tool's queue, lower first). `owner` names whose life the run's is: `process` (this one),
    `instance` (a game, `pid`), `session` (Claude Code's, CLAUDE_PID, else the calling shell).
    A `gate` passes its id to every process it starts (TARUN_RUN); a run that `adopts` (one these
    commands drive) takes in the runs this session starts in its worktree while it is open.
    Returns NULL when the journal cannot be opened."""
    try:
        sid = session_id()
        top, branch = where()
        if pid is None:
            pid = (os.getpid() if owner == "process" else
                   int(os.environ.get("CLAUDE_PID") or os.getppid()))
        now = time.time()
        rid = (f"{time.strftime('%Y%m%d-%H%M%S', time.localtime(now))}-{tool}-{os.getpid()}"
               f"-{next(SEQ)}")
        doc = {"v": VERSION, "id": rid, "session": sid, "tool": tool, "kind": kind,
               "title": title or " ".join([tool, kind]).strip(), "cwd": os.getcwd(),
               "worktree": top, "branch": branch, "owner": owner, "pid": pid,
               "pid_start": pid_start(pid), "parent": parent_for(sid, top),
               "pools": pools or {"main": 1}, "variant": variant, "adopts": adopts,
               "started": now,
               "ended": None, "state": "running", "eta": None,
               "items": [{"id": item, "steps": [dict({"state": "planned", "pool": "main"}, **s)
                                               for s in steps]} for item, steps in items],
               **fields}
        try:
            prune()
        except OSError:
            pass
        run = Run(doc, load_history(tool), owned=gate)
        run.write()
        if gate:
            os.environ["TARUN_RUN"] = rid
        return run
    except Exception as e:  # noqa: BLE001 -- the journal must not cost the run
        print(f"tarun: no journal for this run: {type(e).__name__}: {e}", file=sys.stderr)
        return NULL


def edit(rid, fn, sid=None):
    """fn(run) on a run another process may also be editing, under the session's lock."""
    with SessionLock(sid):
        return edit_locked(rid, fn, sid)


def edit_locked(rid, fn, sid=None):
    """edit() for a caller already holding the lock: a second flock of the same file from this
    process would wait on its own first one."""
    doc = json.loads((session_dir(sid) / f"{rid}.json").read_text())
    run = Run(doc, load_history(doc["tool"]), owned=False)
    fn(run)
    return run


def find(name, sid=None, open_only=True):
    """A run id, or the newest open run of the tool `name` in this session and worktree."""
    if (session_dir(sid) / f"{name}.json").exists():
        return name
    top, _ = where()
    for r in read_runs(sid):
        if r.get("tool") == name and r.get("worktree") == top and not (open_only and
                                                                       r.get("ended")):
            return r["id"]
    return None


# ---- tacli: one instance's game is a run --------------------------------------------------

def instance_run(name, sid=None):
    for r in read_runs(sid):
        if r.get("tool") == "tacli" and r.get("instance") == name and not r.get("ended"):
            return r
    return None


def tacli_launched(name, pid, display=None, remote=False):
    """`tacli launch` outside any gate: the game's own run, until `tacli stop` or its end. A
    remote instance's game is on another machine, so the session's life stands in for its own."""
    if os.environ.get("TARUN_RUN"):
        return
    try:
        with SessionLock():
            old = instance_run(name)
            if old and not remote and old.get("pid") == pid:
                return
            if old:                     # the previous game of the name ended unseen
                edit_locked(old["id"], lambda r: r.end("unrun"))
        run = begin("tacli", [("", [{"id": "game", "instance": name, "display": display}])],
                    kind="instance", title=f"tacli {name}" + (" (remote)" if remote else ""),
                    owner="session" if remote else "instance", pid=None if remote else pid,
                    instance=name, last="launch")
        run.start("", "game", instance=name, display=display)
    except Exception as e:  # noqa: BLE001 -- the journal must not cost the command
        print(f"tarun: {type(e).__name__}: {e}", file=sys.stderr)


def tacli_command(name, words):
    """Any other tacli command on an instance whose game is a run: its latest command."""
    if os.environ.get("TARUN_RUN") or not name:
        return
    try:
        r = instance_run(name)
        if r:
            said = list(words)
            if name in said:                # the row names the instance already
                said.remove(name)
            edit(r["id"], lambda run: run.note(last=" ".join(said)[:120]))
    except Exception as e:  # noqa: BLE001
        print(f"tarun: {type(e).__name__}: {e}", file=sys.stderr)


def tacli_stopped(name):
    if os.environ.get("TARUN_RUN"):
        return
    try:
        r = instance_run(name)
        if r:
            def stop(run):
                run.finish("", "game", "done")
                run.end("pass")
            edit(r["id"], stop)
    except Exception as e:  # noqa: BLE001
        print(f"tarun: {type(e).__name__}: {e}", file=sys.stderr)


# ---- the command line ---------------------------------------------------------------------

def cmd_begin(args):
    steps = []
    for k, sid in enumerate(args.steps):
        s = {"id": sid, "order": k}
        if not args.jobs and k:
            s["after"] = [args.steps[k - 1]]
        steps.append(s)
    run = begin(args.tool, [("", steps)], title=args.title or "", owner="session",
                pools={"main": args.jobs or 1}, adopts=True)
    if run is NULL:
        return 2
    print(run.doc["id"])
    return 0


def cmd_step(args):
    rid = find(args.run)
    if not rid:
        print(f"tarun: no open run {args.run!r} in this session and worktree", file=sys.stderr)
        return 2

    def act(run):
        if ("", args.step) not in run.steps:
            raise KeyError(args.step)
        if args.state == "start":
            run.start("", args.step)
        else:
            run.finish("", args.step, args.state, args.detail)
    try:
        edit(rid, act)
    except KeyError:
        print(f"tarun: {rid} has no step {args.step!r}", file=sys.stderr)
        return 2
    return 0


def cmd_end(args):
    rid = find(args.run)
    if not rid:
        print(f"tarun: no open run {args.run!r} in this session and worktree", file=sys.stderr)
        return 2
    edit(rid, lambda run: run.end(args.state))
    return 0


def cmd_ls(args):
    mark = {"planned": ".", "running": ">", "done": "+", "pass": "+", "fail": "X",
            "unrun": "?", "skipped": "-"}
    for r in read_runs():
        state = r["state"] if r.get("ended") else ("running" if alive(r) else "died")
        eta = r.get("eta")
        left = (f"  ~{max(0, eta['seconds'] - (time.time() - eta['at'])) / 60:.0f} min"
                f"{' or more' if eta['lower_bound'] else ''}" if eta else "")
        print(f"{r['id']}  {r['title']}  {state}{left}")
        for it in r["items"]:
            steps = " ".join(f"{s['id']}{mark.get(s['state'], '?')}" for s in it["steps"])
            fails = (it.get("checks") or {}).get("fail") or []
            print(f"   {it['id'] or '-':20} {it['state']:8} {steps}"
                  + (f"  FAIL {'; '.join(fails)}" if fails else ""))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 2)[2])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("begin", help="a run driven from the shell; prints its id")
    b.add_argument("tool")
    b.add_argument("steps", nargs="+", metavar="STEP")
    b.add_argument("--title")
    b.add_argument("--jobs", type=int, help="the steps play N at once instead of in order")
    b.set_defaults(func=cmd_begin)
    s = sub.add_parser("step", help="a step of a run starts or ends")
    s.add_argument("run")
    s.add_argument("step")
    s.add_argument("state", choices=sorted(STEP_STATES))
    s.add_argument("--detail")
    s.set_defaults(func=cmd_step)
    e = sub.add_parser("end", help="a run ends")
    e.add_argument("run")
    e.add_argument("state", choices=sorted(RUN_STATES))
    e.set_defaults(func=cmd_end)
    ls = sub.add_parser("ls", help="this session's runs")
    ls.set_defaults(func=cmd_ls)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
