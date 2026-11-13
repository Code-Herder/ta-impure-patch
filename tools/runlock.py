#!/usr/bin/env python3
"""One game regression at a time: the run lock.

A landing gate alone peaks at a load of about 24 on the reference setup's 32 threads, so two at
once oversubscribe it, every game runs slower, and the checks bounded in wall-clock or CPU time
fail for the load alone (MEASURED 2026-10-12: four gates at once took 34 min each against 11 alone,
at a load average of 63-87, and failed two such checks). Every tool that plays games for a
regression -- tareplay.py's game commands, tacompat.py's suites, recmp_play_test.py -- holds this
lock while its games play, so a second run waits for the first instead of sharing the CPU with it.
Most hold it for the whole run; wiki_regression.py's gate takes it only for its game lanes and runs
its offline checks before and beside them (CLAUDE.md: builds and offline checks take no lock).

THE LOCK is flock(2) on `tareplay.lock` in the repository's common git directory, the one every
worktree and every scratch worktree of this repository shares. The kernel drops it when the holding
process exits, however it exits, so a crashed run leaves no stale lock. `flock(1)` on the same file
takes the same lock, but writes no holder line, so a run waiting behind it is told the line of
whoever held the lock last. A chain of games scripted by hand queues through the wrapper instead:

    tools/runlock.py run --what "the sweep, then calib-applier" -- sh -c 'cmd1 && cmd2'

which takes the lock, writes the holder line, runs the command with RUNLOCK_HELD set (so the
tools inside it do not wait for their own chain), and exits with the command's status.

THE HOLDER LINE names the holding process by its pid and its start time (field 22 of
/proc/<pid>/stat, clock ticks since boot), so a waiting run can tell a live holder from a line left
by one that has exited: a reader calls the line stale when no process with that pid and start time
is running, which a recycled pid cannot fake.

A process inside a run that already holds it does not take it again: the holder exports
RUNLOCK_HELD naming the file, which its children inherit, and a process whose ancestor is a
`flock(1)` on the file holds it through that ancestor.

A HOLD THAT ENDS BEFORE THE PROCESS: release() closes this process's descriptor of the lock. The
lock belongs to the open file description, not to the descriptor, so a child started with
fileno() in `pass_fds` holds it too, until it exits: the lock goes when the last of them closes it,
and a release while such a child still plays cannot let another run in. Nothing here ever unlocks
explicitly (LOCK_UN), which would drop it for those children as well.
"""

import argparse
import fcntl
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ENV = "RUNLOCK_HELD"
POLL_S = 2                      # how often a waiting run tries again
SAY_S = 300                     # how often a waiting run says it is still waiting

_held = None                    # the open lock file: the lock lives as long as this process


def lock_path(tree: Path) -> Path:
    """`tareplay.lock` in the common git directory of the repository `tree` belongs to."""
    common = subprocess.run(["git", "-C", str(tree), "rev-parse", "--path-format=absolute",
                             "--git-common-dir"], capture_output=True, text=True,
                            check=True).stdout.strip()
    return Path(common) / "tareplay.lock"


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


def _start_time(pid: int):
    """The process's start time in clock ticks since boot, None when no process has that pid."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    return int(stat.rsplit(")", 1)[1].split()[19])       # field 22; the split starts at field 3


HOLDER_RX = re.compile(r"^pid (\d+)(?: start (\d+))?, ")


def holder(path: Path) -> str:
    """Who holds the lock, as its holder line says, or why the line cannot be believed: a line
    whose pid and start time name no running process is stale, and the lock is then held by
    something that wrote no line (a `flock(1)` chain, or an older tool). A line with no start
    time, which a tool from before the start time writes, is believed while its pid runs."""
    try:
        text = path.read_text().strip()
    except OSError:
        text = ""
    if not text:
        return "a process that wrote no holder line (a flock(1) chain or an older tool)"
    m = HOLDER_RX.match(text)
    if m:
        started = _start_time(int(m.group(1)))
        if started is not None and (m.group(2) is None or started == int(m.group(2))):
            return text
    return (f"a process that wrote no holder line (a flock(1) chain or an older tool); "
            f"the last line is stale, its process has exited: {text}")


def hold(tree: Path, what: str) -> bool:
    """Take the run lock until release() or the end of this process, waiting while another run
    holds it. True when this call took it; False when this process holds it already, directly or
    through RUNLOCK_HELD or a `flock(1)` ancestor."""
    global _held
    path = lock_path(tree)
    if _held is not None or os.environ.get(ENV) == str(path) or _ancestor_holds(path):
        return False
    f = open(path, "a+")
    waited, said = 0.0, -SAY_S
    while True:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if waited - said >= SAY_S:
                print(f"runlock: waiting for the run lock ({path}), held by {holder(path)}; "
                      f"{waited / 60:.0f} min so far", file=sys.stderr, flush=True)
                said = waited
            time.sleep(POLL_S)
            waited += POLL_S
    if waited:
        print(f"runlock: took the run lock after {waited / 60:.1f} min", file=sys.stderr,
              flush=True)
    f.seek(0)
    f.truncate()
    f.write(f"pid {os.getpid()} start {_start_time(os.getpid())}, {what}, from {Path.cwd()}, "
            f"since {time.strftime('%H:%M:%S')}\n")
    f.flush()
    _held = f
    os.environ[ENV] = str(path)
    return True


def fileno():
    """The descriptor of the lock this process took, None when it took none: a child given it in
    `pass_fds` holds the lock with this process until it exits (A HOLD THAT ENDS BEFORE THE PROCESS)."""
    return _held.fileno() if _held is not None else None


def release() -> None:
    """End this process's hold: its descriptor closed and RUNLOCK_HELD taken out of its
    environment, so the processes it starts from now on wait like any other run. The lock itself
    goes once no child given fileno() still runs. Nothing when this process took no lock."""
    global _held
    if _held is None:
        return
    os.environ.pop(ENV, None)
    _held.close()
    _held = None


def run(tree: Path, what: str, argv) -> int:
    """`argv` under the run lock: taken (or already held by this process's chain), the holder line
    written with this process's pid, the command run with RUNLOCK_HELD in its environment, and its
    exit status returned. The command is given the lock's descriptor as well, so a wrapper killed
    while it plays leaves the command holding the lock until it ends (A HOLD THAT ENDS BEFORE THE
    PROCESS), never playing unlocked."""
    if not argv:
        raise SystemExit("runlock run: no command after `--`")
    hold(tree, what)
    fd = fileno()
    try:
        return subprocess.run(argv, pass_fds=(fd,) if fd is not None else ()).returncode
    except OSError as e:
        print(f"runlock run: {argv[0]}: {e}", file=sys.stderr)
        return 127


def main(args=None) -> int:
    ap = argparse.ArgumentParser(prog="runlock.py", description="The machine's run lock.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run a command under the run lock, waiting for it if held")
    r.add_argument("--what", required=True, help="what the holder line says the run is")
    r.add_argument("argv", nargs=argparse.REMAINDER, help="-- the command and its arguments")
    h = sub.add_parser("holder", help="say who holds the run lock now, or that nobody does")
    for q in (r, h):
        q.add_argument("--tree", type=Path, default=Path(__file__).resolve().parents[1],
                       help="the repository whose lock it is (default: this script's)")
    a = ap.parse_args(args)
    tree = a.tree
    if a.cmd == "run":
        argv = a.argv[1:] if a.argv[:1] == ["--"] else a.argv
        return run(tree, a.what, argv)
    path = lock_path(tree)
    with open(path, "a+") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_SH | fcntl.LOCK_NB)
            print("nobody holds the run lock")
        except BlockingIOError:
            print(f"held by {holder(path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
