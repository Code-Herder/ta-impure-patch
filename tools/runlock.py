#!/usr/bin/env python3
"""Game regressions share the machine by declared lanes: the weighted run lock.

    runlock.py run --slots W --what TEXT -- CMD ...    CMD under W slots, waiting for them first
    runlock.py status                                  who holds which slots, and how many are free

Both act on the lock of the repository the current directory is in.

WHY. A landing gate alone peaks at a load of about 24 on the reference setup's 32 threads, so two at
once oversubscribe it, every game runs slower, and the checks bounded in wall-clock or CPU time
fail for the load alone (MEASURED 2026-10-12: four gates at once took 34 min each against 11 alone,
at a load average of 63-87, and failed two such checks). Every tool that plays games for a
regression -- tareplay.py's game commands, tacompat.py wine, wiki_regression.py's game lanes,
recmp_play_test.py and the single-game checks -- holds slots of one machine budget while its games
play, so a run that would take the machine past the budget waits instead of sharing the CPU.
Offline checks take none (CLAUDE.md, "Game regressions share a 28-slot budget").

THE BUDGET is BUDGET slots, 28 of the reference setup's 32 threads (the owner's figure,
2026-11-12). A slot stands for one thread of load. A run declares the game lanes it plays
at once and takes LANE_SLOTS a lane, clamped to 1..BUDGET; a run that declares none takes the whole
budget and so runs alone. A tool declares lanes only when two of its runs, or it and another tool,
cannot meet on anything but the CPU: its games on instances it holds for the run (tareplay's
Worker), on a private X server, on no fixed DirectPlay port. A tool that plays on a fixed instance
name, a fixed port or the instance its caller names declares none.

LANE_SLOTS is 2 [INFERRED from load, not measured a lane]: the landing gate at its default -j 12
peaks at a load of about 24 (MEASURED 2026-10-12), two threads a lane with the lane's share of the
offline checks; tareplay's offline pool (offline_workers) and the wiki gate's --offline-jobs are
sized on the same two threads a game. A 200v200 battle is heavier: tacompat's six battles held a
load of 28 (MEASURED 2026-09-28), nearer five a lane.

THE SLOTS are flock(2) files, runslots/slot-00 .. slot-27, beside `tareplay.lock` in the
repository's common git directory, the one every worktree and every scratch worktree shares. A run
holds each of its slots LOCK_EX, so no slot is ever held by two runs and the slots held can never
sum past BUDGET: that is the bound, and no wait or poll is part of the argument for it. The kernel
drops a lock when the last descriptor of its open file description closes, however the process
ends, so a crashed run frees its slots and leaves no state to clean.

ADMISSION IS IN ARRIVAL ORDER. A run first takes runslots/queue, LOCK_EX, blocking; holding it, it
takes free slots and keeps each one until it has all it wants, and only then lets the queue go.
Only the queue's holder takes slots, so a run that arrives behind a big one cannot take a slot
before it, and the big one is never starved by small ones: it gathers its slots as the runs ahead
of it end. The waiters for the queue are woken in the order they blocked and then race for it, so
the order among them is arrival order to within that race; what is guaranteed is that the queue's
holder is admitted before every run behind it. Only the queue's holder waits while it holds slots,
and every other holder is an admitted run, which ends: admission cannot deadlock.

flock(1) ON tareplay.lock STILL EXCLUDES EVERY RUN. A run also holds tareplay.lock LOCK_SH, taken
once it has its slots; flock(1) takes the file LOCK_EX by default, so a hand chain under it waits
for every run and every run waits while it holds. Such a chain waits outside the queue: Linux grants
a shared lock beside a waiting exclusive one, so it can wait for as long as runs keep overlapping.
Hand chains therefore go through `runlock.py run`, which queues like the tools.

NESTING. The holder exports RUNLOCK_HELD as "<slots>:<lock path>", which its children inherit; a
process that finds it, or that has a flock(1) ancestor on tareplay.lock (which holds every slot),
takes no slots: the outer run's declaration covers its children. A nested run that asks for more
than its ancestor holds says so and runs under the ancestor's slots. A child started with an
environment of its own carries RUNLOCK_HELD over: without it, it would queue behind the run it
belongs to and wait for that run's end.

HOLDERS. A run writes its line -- pid, slots, start, what, directory -- into each slot file it
takes, after taking it, and into the queue file while it is being admitted. `status` asks each
slot's own lock whether it is held and reads the line only to name the holder; a line whose pid is
gone is marked stale, the slot then held by a process that inherited the lock (below).

A HOLD THAT ENDS BEFORE THE PROCESS: release() closes this process's descriptors of the lock. A
lock belongs to the open file description, not to the descriptor, so a child started with fds() in
`pass_fds` holds the slots too, until it exits: they go when the last of them closes them, and a
release while such a child still plays cannot let another run in. Nothing here ever unlocks
explicitly (LOCK_UN), which would drop the lock for those children as well. `runlock.py run`
execs its command with the descriptors inheritable, so the command is the holder.
"""

import argparse
import fcntl
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ENV = "RUNLOCK_HELD"
BUDGET = 28                     # slots: the reference setup's 32 threads less four (THE BUDGET)
LANE_SLOTS = 2                  # slots a game lane takes (LANE_SLOTS, above)
POLL_S = 2                      # how often the run being admitted tries the slots again
SAY_S = 300                     # how often a waiting run says it is still waiting

_held = []                      # this process's hold: its slot files and tareplay.lock, shared


def lock_path(tree: Path) -> Path:
    """`tareplay.lock` in the common git directory of the repository `tree` belongs to."""
    common = subprocess.run(["git", "-C", str(tree), "rev-parse", "--path-format=absolute",
                             "--git-common-dir"], capture_output=True, text=True,
                            check=True).stdout.strip()
    return Path(common) / "tareplay.lock"


def slots_dir(path: Path) -> Path:
    d = path.parent / "runslots"
    d.mkdir(exist_ok=True)
    return d


def slot_file(d: Path, k: int) -> Path:
    return d / f"slot-{k:02d}"


def slots_for(lanes: "int | None") -> int:
    """The slots a run of `lanes` game lanes at once takes; with None, the whole budget."""
    return BUDGET if lanes is None else clamp(lanes * LANE_SLOTS)


def clamp(slots: int) -> int:
    return min(BUDGET, max(1, slots))


def say(text: str) -> None:
    print(f"runlock: {text}", file=sys.stderr, flush=True)


def _env_held(path: Path) -> int:
    """The slots RUNLOCK_HELD says an ancestor holds on `path`, 0 when it names none there."""
    count, _, held = os.environ.get(ENV, "").partition(":")
    return int(count) if held == str(path) and count.isdigit() else 0


def _ancestor_holds(path: Path) -> bool:
    """True when an ancestor process is `flock(1)` holding `path`."""
    pid = os.getppid()
    while pid > 1:
        try:
            argv = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
            stat = Path(f"/proc/{pid}/stat").read_text()
        except OSError:
            return False
        if argv and Path(argv[0].decode(errors="replace")).name == "flock" and \
                str(path).encode() in argv:
            return True
        pid = int(stat.rsplit(")", 1)[1].split()[1])      # the field after the command is the ppid
    return False


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    return True


def _line(slots: int, what: str) -> str:
    return (f"pid {os.getpid()}, {slots} slot{'s' if slots != 1 else ''}, since "
            f"{time.strftime('%Y-%m-%d %H:%M:%S')}, {what}, from {Path.cwd()}\n")


def _write(f, line: str) -> None:
    f.seek(0)
    f.truncate()
    f.write(line)
    f.flush()


def _held_now(p: Path, mode=fcntl.LOCK_SH) -> bool:
    """Whether another open file holds a lock on `p` that conflicts with `mode`, asked of the lock
    itself. The probe holds `mode` for no longer than the call: the run being admitted that tries
    the same slot meanwhile finds it taken and tries again on its next round."""
    try:
        with open(p, "a+") as f:
            fcntl.flock(f, mode | fcntl.LOCK_NB)
    except BlockingIOError:
        return True
    return False


def _read(p: Path) -> str:
    try:
        return p.read_text(errors="replace").strip()
    except OSError:
        return ""


def _named(line: str) -> str:
    """`line`, marked stale when the pid it names is gone."""
    m = re.match(r"pid (\d+),", line)
    if m and not _alive(int(m.group(1))):
        return f"{line} [STALE: pid {m.group(1)} is gone; a process it passed the lock to holds it]"
    return line


def report(path: Path) -> "tuple[int, list[str]]":
    """(free slots, a line for each holder): tareplay.lock held exclusively by flock(1), the run
    being admitted, then each run with the number of slots it holds."""
    d = slots_dir(path)
    runs, free = {}, 0
    for k in range(BUDGET):
        p = slot_file(d, k)
        if not p.exists() or not _held_now(p):
            free += 1
            continue
        line = _read(p) or "a run that has not written its line yet"
        runs[line] = runs.get(line, 0) + 1
    lines = []
    if path.exists() and _held_now(path):
        lines.append("tareplay.lock held exclusively, by a flock(1) chain: no run is admitted")
    queue = d / "queue"
    if queue.exists() and _held_now(queue):
        lines.append(f"being admitted: {_named(_read(queue) or 'a run that has not written its line yet')}")
    lines += [f"{n:2} held by {_named(line)}" for line, n in runs.items()]
    return free, lines


def _wait_for_queue(queue, path: Path, slots: int) -> None:
    """The queue taken, blocking: the kernel queues the waiters. While it waits, a thread says so
    every SAY_S."""
    try:
        fcntl.flock(queue, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return
    except BlockingIOError:
        pass
    stop = threading.Event()

    def remind():
        t0 = time.monotonic()
        while True:
            free, lines = report(path)
            say(f"waiting for the run lock ({path.parent / 'runslots'}): {slots} slots wanted, "
                f"{free} free, another run ahead in the queue; {(time.monotonic() - t0) / 60:.0f} "
                f"min so far" + "".join(f"\n  {ln}" for ln in lines))
            if stop.wait(SAY_S):
                return
    t = threading.Thread(target=remind, daemon=True)
    t.start()
    try:
        fcntl.flock(queue, fcntl.LOCK_EX)
    finally:
        stop.set()
        t.join()


def _take(path: Path, slots: int, what: str) -> list:
    """`slots` slots and tareplay.lock shared, in arrival order (ADMISSION IS IN ARRIVAL ORDER):
    the open files that hold them."""
    d = slots_dir(path)
    line = _line(slots, what)
    t0 = time.monotonic()
    taken, shared = {}, None
    queue = open(d / "queue", "a+")
    try:
        _wait_for_queue(queue, path, slots)
        _write(queue, line)
        said = None
        while True:
            for k in range(BUDGET):
                if len(taken) == slots:
                    break
                if k in taken:
                    continue
                f = open(slot_file(d, k), "a+")
                try:
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    f.close()
                    continue
                _write(f, line)
                taken[k] = f
            if len(taken) == slots:
                shared = open(path, "a+")
                try:
                    fcntl.flock(shared, fcntl.LOCK_SH | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    shared.close()
                    shared = None
            waited = time.monotonic() - t0
            if said is None or waited - said >= SAY_S:
                free, lines = report(path)
                say(f"waiting for the run lock ({d}): {len(taken)} of the {slots} slots wanted "
                    f"taken, {free} free; {waited / 60:.0f} min so far"
                    + "".join(f"\n  {ln}" for ln in lines))
                said = waited
            time.sleep(POLL_S)
    except BaseException:
        for f in [shared, *taken.values()]:
            if f is not None:
                f.close()
        raise
    finally:
        queue.close()
    waited = time.monotonic() - t0
    if waited >= 1:
        say(f"took the run lock ({slots} slots) after {waited / 60:.1f} min")
    return [shared, *taken.values()]


def _hold(path: Path, slots: int, what: str) -> bool:
    global _held
    if _held:
        return False
    has = _env_held(path) or (BUDGET if _ancestor_holds(path) else 0)
    if has:
        if slots > has:
            say(f"{what} wants {slots} slots inside a run that holds {has}: it runs under those")
        return False
    _held = _take(path, slots, what)
    os.environ[ENV] = f"{slots}:{path}"
    return True


def hold(tree: Path, what: str, lanes: "int | None" = None) -> bool:
    """Hold the slots of `lanes` game lanes at once -- the whole budget with None -- until
    release() or the end of this process, waiting in the queue for them. True when this call took
    them; False when this process holds them already, directly or through RUNLOCK_HELD or a
    `flock(1)` ancestor."""
    return _hold(lock_path(tree), slots_for(lanes), what)


def fds() -> tuple:
    """The descriptors of the hold this process took, () when it took none: a child given them in
    `pass_fds` holds the slots with this process until it exits (A HOLD THAT ENDS BEFORE THE
    PROCESS)."""
    return tuple(f.fileno() for f in _held)


def release() -> None:
    """End this process's hold: its descriptors closed and RUNLOCK_HELD taken out of its
    environment, so the processes it starts from now on wait like any other run. The slots
    themselves go once no child given fds() still runs. Nothing when this process took none."""
    global _held
    if not _held:
        return
    os.environ.pop(ENV, None)
    for f in _held:
        f.close()
    _held = []


def cmd_run(a) -> int:
    if not a.cmd:
        say("run: no command after --")
        return 2
    _hold(lock_path(Path.cwd()), clamp(a.slots), a.what)
    for fd in fds():
        os.set_inheritable(fd, True)
    try:
        os.execvp(a.cmd[0], a.cmd)
    except OSError as e:
        say(f"run: {a.cmd[0]}: {e}")
        return 127


def cmd_status(a) -> int:
    path = lock_path(Path.cwd())
    free, lines = report(path)
    print(f"runlock: {free} of {BUDGET} slots free ({path.parent / 'runslots'})")
    for ln in lines:
        print(f"  {ln}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 2)[2])
    sub = ap.add_subparsers(dest="cmd_name", required=True)
    r = sub.add_parser("run", help="a command under W slots, after waiting for them")
    r.add_argument("--slots", type=int, required=True,
                   help=f"slots to hold, clamped to 1..{BUDGET}: {LANE_SLOTS} a game played at once")
    r.add_argument("--what", required=True, help="what the run is, for the holder line")
    r.add_argument("cmd", nargs=argparse.REMAINDER, help="-- and the command")
    r.set_defaults(fn=cmd_run)
    sub.add_parser("status", help="who holds which slots").set_defaults(fn=cmd_status)
    a = ap.parse_args(argv)
    if getattr(a, "cmd", None) and a.cmd[0] == "--":
        a.cmd = a.cmd[1:]
    try:
        return a.fn(a)
    except subprocess.CalledProcessError:
        say("not inside a checkout of this repository: the lock lives in its common git directory")
        return 2


if __name__ == "__main__":
    sys.exit(main())
