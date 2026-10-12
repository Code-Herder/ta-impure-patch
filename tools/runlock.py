"""One game regression at a time: the run lock.

A landing gate alone peaks at a load of about 24 on the reference setup's 32 threads, so two at
once oversubscribe it, every game runs slower, and the checks bounded in wall-clock or CPU time
fail for the load alone (MEASURED 2026-10-12: four gates at once took 34 min each against 11 alone,
at a load average of 63-87, and failed two such checks). Every tool that plays games for a
regression -- tareplay.py's game commands, tacompat.py's suites, recmp_play_test.py -- holds this
lock for the whole run, so a second run waits for the first instead of sharing the CPU with it.

THE LOCK is flock(2) on `tareplay.lock` in the repository's common git directory, the one every
worktree and every scratch worktree of this repository shares. The kernel drops it when the holding
process exits, however it exits, so a crashed run leaves no stale lock. `flock(1)` on the same file
takes the same lock, which is how a script chain of games queues with the tools.

A process inside a run that already holds it does not take it again: the holder exports
RUNLOCK_HELD naming the file, which its children inherit, and a process whose ancestor is a
`flock(1)` on the file holds it through that ancestor.
"""

import fcntl
import os
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


def _holder(path: Path) -> str:
    try:
        text = path.read_text().strip()
    except OSError:
        text = ""
    return text or "a flock(1) chain or an older tool"


def hold(tree: Path, what: str) -> None:
    """Take the run lock for the rest of this process, waiting while another run holds it."""
    global _held
    path = lock_path(tree)
    if _held is not None or os.environ.get(ENV) == str(path) or _ancestor_holds(path):
        return
    f = open(path, "a+")
    waited, said = 0.0, -SAY_S
    while True:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if waited - said >= SAY_S:
                print(f"runlock: waiting for the run lock ({path}), held by {_holder(path)}; "
                      f"{waited / 60:.0f} min so far", file=sys.stderr, flush=True)
                said = waited
            time.sleep(POLL_S)
            waited += POLL_S
    if waited:
        print(f"runlock: took the run lock after {waited / 60:.1f} min", file=sys.stderr,
              flush=True)
    f.seek(0)
    f.truncate()
    f.write(f"pid {os.getpid()}, {what}, from {Path.cwd()}, since {time.strftime('%H:%M:%S')}\n")
    f.flush()
    _held = f
    os.environ[ENV] = str(path)
