#!/usr/bin/env python3
"""Game regressions share the machine by declared lanes: the weighted run lock.

    runlock.py run --slots W [--vram T] --what TEXT [--resource NAME]... -- CMD ...
                                       CMD under W slots, T GPU tokens and the named resources,
                                       waiting for them first
    runlock.py status                  who holds which slots, tokens and resources, and what is free

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
2026-11-12). A slot stands for one thread of load. A run declares the game lanes it plays at once
and takes LANE_SLOTS a lane, clamped to 1..BUDGET, and a GPU token for each lane that renders on the
GPU (THE VRAM TOKENS); a run that declares none takes the whole budget of both and so runs alone.
games() is how many games the run may play at once: its lanes, cut to the tokens there are. Every
tool declares its lanes: what else two runs could meet on is kept apart
by TAGS, for what a run makes for itself, and by RESOURCES, for what it is given.

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

ADMISSION IS IN ARRIVAL ORDER, IN TWO QUEUES. A run that wants GPU tokens first takes
runslots/tokenq, the TOKEN QUEUE, LOCK_EX, blocking; holding it, it takes free token files and keeps
each one until it has them all, and only then lets the token queue go. It holds no slot and not the
slot queue while it does, so a run that wants no token is never held up by one that waits for the
GPU. Every run -- one holding its tokens now, and one that wants none from the start -- then takes
runslots/queue, the SLOT QUEUE, LOCK_EX, blocking; holding it, it takes free slots, its resources
and tareplay.lock shared, keeps each one until it has them all, and only then lets the slot queue
go. Only a queue's holder takes what that queue hands out, so a run that arrives behind a big one
cannot take a token or a slot before it, and the big one is never starved by small ones: it gathers
what it wants as the runs ahead of it end. The waiters for a queue are woken in the order they
blocked and then race for it, so the order among them is arrival order to within that race; what is
guaranteed is that a queue's holder is served before every run behind it in that queue.

  * No deadlock. Every run takes in one order: the token queue, its tokens, the slot queue, its
    slots, resources and tareplay.lock; it never waits for an earlier one while holding a later
    one, and no run waits for tokens while it holds a slot. The token queue's holder waits only
    for token files, which are held by runs that have their tokens: each of those is admitted, and
    ends, or waits in the slot queue. The slot queue's holder waits only for slots, resources and
    tareplay.lock, held by admitted runs (and by a flock(1) chain, which ends): it is admitted, and
    the run behind it becomes the holder. So every run that holds tokens is admitted, and the
    token queue's holder gets them as those runs end.
  * No starvation. Each queue serves its holder before the runs behind it, and its holder is
    served as the runs ahead of it end (above): a GPU run is behind only GPU runs for its tokens,
    and behind every run that reached the slot queue first for its slots.
  * The budgets hold. A slot and a token are each one flock(2) file held LOCK_EX by one run, so the
    slots held never sum past BUDGET and the tokens never past the files there are, vram_tokens()
    at the admission that made them; holding tokens while waiting for slots counts against the
    token budget like any held token.

RESOURCES are what a run is given and cannot rename: the tacli instance its caller names
(`instance-<name>`), a fixed output folder. Each is a flock(2) file runslots/res-<name>, taken
LOCK_EX in the same admission as the slots, all or none: a run naming instance X waits for any
other run on X, and two runs on different instances go together. Two names that differ only in
characters a file name cannot carry share a file, which can only make runs wait more.

TAGS name what a run makes for itself. tag() is `s<NN>`, the lowest slot the run holds: no two live
runs share it, since a slot has one holder, and later runs reuse it, so the instances a tool names
with it are a bounded set that each run replaces rather than one that grows. A tool removes its
tagged instances by name before it makes them; whatever bears the tag of a slot it holds is a dead
run's. A process with a flock(1) ancestor on tareplay.lock has the tag FLOCK_TAG: no weighted run is
live beside it. DirectPlay ports are leased the same way, per game, by tools/dpport.py.

THE VRAM TOKENS bound the GPU's memory the way the slots bound its threads. There are
(device total - reserve_mib) // game_mib() of them, at least MIN_TOKENS, the total read from the
device at each admission and the figures from the LOCK'S OWN CONFIGURATION, runslots/vram.cfg
(below), which tools/runlock-vram.cfg seeds and which says where they come from (9 tokens on the
reference setup). A token is one game's BOUND plus a margin, and the bound holds by construction:
the run publishes the cap it was admitted under in RUNLOCK_HELD (`cap_mib`), tacli writes that cap
into the store of every game it launches inside the hold (`gpumem=`, held_cap_mib()), and the DLL
refuses any device-local allocation that would take the game's own past it (inc/tagpu_vkcap.h);
game_bound_mib() adds what the driver holds outside those allocations. Each is a flock(2) file
runslots/vram-NN, taken LOCK_EX in the token queue before the run joins the slot queue, so the
tokens held never sum past the budget, a run that wants more waits in the token queue holding no
slot, and a run that wants none never waits for one. A run's tokens are its lanes on
the GPU, cut to the budget; games() is that number, and a tool plays no more games on the GPU at
once than it says. A run that renders without the GPU (llvmpipe) takes none. A machine with no
device memory to read -- no NVIDIA or amdgpu card -- has no budget and no tokens; one whose NVIDIA
driver is there but does not answer refuses the run rather than run it unbounded. MIN_TOKENS is two
because a network game is two games and a smaller card still has to run one.

THE BUDGET BELONGS TO THE LOCK, NOT TO A TREE. Branches and worktrees at different commits share one
runslots directory and run side by side, so a budget each read from its own tree's file could
differ, and two of them together could put more games on the GPU than the device holds -- MEASURED
2026-11-13: `status` counted 11 tokens held of 9, a run of an older tree (11 tokens, a cap of 768)
holding vram-09 and vram-10 beside this tree's runs. So:
  * Every run reads the figures from runslots/vram.cfg. When it is absent, the first run writes it
    from its tree's tools/runlock-vram.cfg, whole, by a hard link of a finished file, so it is
    written once and never seen half-written; a run whose tree's figures differ says so and uses
    the lock's. To change the budget, edit runslots/vram.cfg (or remove it, to be seeded again).
  * A token file numbered at or above the budget is one only a run under another budget takes. The
    token queue's holder takes no token while such a file is held: it waits, naming it, until that
    run ends, so it never adds a game to another budget's. `status` shows such tokens as held under
    another budget. A change to runslots/vram.cfg takes effect the same way: a smaller budget
    admits no token until the runs holding the files past it end.
  * The cap a game runs under is the one in RUNLOCK_HELD, published from the same file at the
    admission that sized its token, so a game's cap always matches the token it holds.
TRANSITION: runlock code without this rule reads its own tree's file and neither reads the lock's
nor looks past its own budget, so it can still overfill the device beside a run of this code; the
guard above only stops this code from adding to it. Every branch that launches GPU runs must merge
this before it runs beside another.

flock(1) ON tareplay.lock STILL EXCLUDES EVERY RUN. A run also holds tareplay.lock LOCK_SH, taken
once it has its slots; flock(1) takes the file LOCK_EX by default, so a hand chain under it waits
for every run and every run waits while it holds. Such a chain waits outside the queue: Linux grants
a shared lock beside a waiting exclusive one, so it can wait for as long as runs keep overlapping.
Hand chains therefore go through `runlock.py run`, which queues like the tools.

NESTING. The holder exports RUNLOCK_HELD, a JSON object of its slots, tokens, games, tag, resources,
lock path and the GPU cap it was admitted under, which its children inherit; a process that finds it, or that has a flock(1) ancestor on
tareplay.lock (which holds every slot), takes no slots: the outer run's declaration covers its
children. A nested run that asks for more games than its ancestor may play says so and runs under
the ancestor's. A resource the outer run did not name, a nested run takes without waiting -- waiting
there could wait on the queue's holder, which waits on the outer run's slots -- and Busy is raised
when another run holds it. A child started with an environment of its own carries RUNLOCK_HELD
over: without it, it would queue behind the run it belongs to and wait for that run's end.

HOLDERS. A run writes its line -- pid, slots, tokens, resources, start, what, directory -- into each
slot, token and resource file it takes, after taking it, and into the token queue's or the slot
queue's file while it waits there as the holder.
`status` asks each file's own lock whether it is held and reads the line only to name the holder;
a line whose pid is gone is marked stale, the file then held by a process that inherited the lock
(below).

A HOLD THAT ENDS BEFORE THE PROCESS: release() closes this process's descriptors of the lock. A
lock belongs to the open file description, not to the descriptor, so a child started with fds() in
`pass_fds` holds the slots too, until it exits: they go when the last of them closes them, and a
release while such a child still plays cannot let another run in. Nothing here ever unlocks
explicitly (LOCK_UN), which would drop the lock for those children as well. `runlock.py run`
execs its command with the descriptors inheritable, so the command is the holder.
"""

import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ENV = "RUNLOCK_HELD"
BUDGET = 28                     # slots (THE BUDGET)
LANE_SLOTS = 2                  # slots a game lane takes (LANE_SLOTS, above)
FLOCK_TAG = "fx"                # the tag under a flock(1) ancestor (TAGS)
MIN_TOKENS = 2                  # the fewest GPU tokens: one network game (THE VRAM TOKENS)
VRAM_CFG = Path(__file__).resolve().with_name("runlock-vram.cfg")
NVIDIA_S = 60                   # a hang bound on nvidia-smi, deciding nothing
POLL_S = 2                      # how often the run being admitted tries the slots again
SAY_S = 300                     # how often a waiting run says it is still waiting

_held = []                      # this process's hold: its slot and resource files, tareplay.lock
_outer = None                   # RUNLOCK_HELD before a nested hold took resources, for release()
_default_lock = None            # default_lock(), once asked
_said_differs = set()           # the locks whose figures this process has said its tree differs from


class Busy(RuntimeError):
    """A resource another run holds, asked for by a run nested in another (NESTING)."""


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


def resource_file(d: Path, name: str) -> Path:
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_")
    if len(safe) > 80 or not safe:
        safe = safe[:70] + "-" + hashlib.md5(name.encode()).hexdigest()[:8]
    return d / f"res-{safe}"


def token_file(d: Path, k: int) -> Path:
    return d / f"vram-{k:02d}"


def _token_files(d: Path) -> list:
    """[(k, file)] of every token file there is, any budget's, by number."""
    out = []
    for p in d.glob("vram-*"):
        m = re.fullmatch(r"vram-(\d+)", p.name)
        if m:
            out.append((int(m.group(1)), p))
    return sorted(out)


def _past_budget(d: Path, budget: int) -> list:
    """The token files numbered at or past `budget` that a run holds: a run under another budget's
    (THE BUDGET BELONGS TO THE LOCK, NOT TO A TREE)."""
    return [p.name for k, p in _token_files(d) if k >= budget and _held_now(p)]


VRAM_KEYS = ("reserve_mib", "cap_mib", "driver_mib", "margin_mib")
# The caps the DLL takes (inc/tagpu_vkcap.h, TAGPU_VKCAP_MIN_MIB and TAGPU_VKCAP_MAX_MIB): it logs
# any other `gpumem=` and runs UNCAPPED, so a token sized on such a cap would bound nothing
CAP_MIN_MIB, CAP_MAX_MIB = 64, 1 << 20


def read_vram_cfg(path: Path) -> dict:
    """The figures of a VRAM configuration file, VRAM_KEYS, each a whole number of MiB; a file that
    lacks one, or names one twice or one it does not know, is refused rather than read as a
    smaller budget."""
    cfg = {}
    for line in Path(path).read_text().splitlines():
        key, eq, value = line.partition("=")
        if eq and not line.lstrip().startswith("#"):
            key = key.strip()
            if key not in VRAM_KEYS or key in cfg:
                raise ValueError(f"{path}: {key} is not one of {', '.join(VRAM_KEYS)}, or is "
                                 f"named twice")
            cfg[key] = int(value)
    missing = [k for k in VRAM_KEYS if k not in cfg]
    if missing or any(v < 0 for v in cfg.values()):
        raise ValueError(f"{path}: {', '.join(missing) or 'a negative figure'}: every one of "
                         f"{', '.join(VRAM_KEYS)} is a figure of MiB")
    if not CAP_MIN_MIB <= cfg["cap_mib"] <= CAP_MAX_MIB:
        raise ValueError(f"{path}: cap_mib {cfg['cap_mib']} is not a cap the DLL takes "
                         f"({CAP_MIN_MIB}..{CAP_MAX_MIB}): its games would run uncapped")
    return cfg


def default_lock() -> Path:
    """The lock of the repository this file is in: the one its tools hold."""
    global _default_lock
    if _default_lock is None:
        _default_lock = lock_path(Path(__file__).resolve().parent)
    return _default_lock


def shared_vram_cfg(path: Path) -> Path:
    """The lock's own VRAM configuration (THE BUDGET BELONGS TO THE LOCK, NOT TO A TREE)."""
    return slots_dir(path) / "vram.cfg"


def vram_config(path: "Path | None" = None) -> dict:
    """The figures of the lock's runslots/vram.cfg (the lock of `path`, else default_lock()),
    seeded from this tree's tools/runlock-vram.cfg when it is absent; said once a process when this
    tree's figures differ from the lock's."""
    global _said_differs
    shared = shared_vram_cfg(path or default_lock())
    if not shared.exists():
        seed = read_vram_cfg(VRAM_CFG)              # a file that is not whole seeds nothing
        tmp = shared.with_name(f".vram.cfg.{os.getpid()}.{threading.get_ident()}")
        tmp.write_text(VRAM_CFG.read_text())
        try:
            os.link(tmp, shared)                    # written once: a second seeder finds it there
            say(f"seeded {shared} from {VRAM_CFG} ({seed}): the GPU budget of every run on this lock")
        except FileExistsError:
            pass
        finally:
            tmp.unlink()
    cfg = read_vram_cfg(shared)
    try:
        mine = read_vram_cfg(VRAM_CFG)
    except (OSError, ValueError) as e:
        mine = str(e)
    if mine != cfg and shared not in _said_differs:
        _said_differs.add(shared)
        say(f"this tree's {VRAM_CFG.name} ({mine}) is not the lock's {shared} ({cfg}): the runs on "
            f"this lock share the lock's, and this run uses it")
    return cfg


def cap_mib(cfg: "dict | None" = None) -> int:
    """The GPU memory cap a harness game runs under, MiB: tacli writes it as `gpumem=` for every
    launch inside a hold, and the DLL's allocations never sum past it (THE VRAM TOKENS)."""
    return (cfg or vram_config())["cap_mib"]


def game_bound_mib(cfg: "dict | None" = None) -> int:
    """B, the most device memory one harness game holds, MiB: the cap on its own allocations and
    what the driver holds for it beside them."""
    cfg = cfg or vram_config()
    return cfg["cap_mib"] + cfg["driver_mib"]


def game_mib(cfg: "dict | None" = None) -> int:
    """A GPU token, MiB: one game's bound and the margin over it."""
    cfg = cfg or vram_config()
    return game_bound_mib(cfg) + cfg["margin_mib"]


def device_total_mib() -> "int | None":
    """The device memory of the largest GPU, in MiB: nvidia-smi's, else the amdgpu driver's
    counters; None on a machine with neither, which has no device memory to bound."""
    if shutil.which("nvidia-smi"):
        try:
            r = subprocess.run(["nvidia-smi", "--query-gpu=memory.total", "--format=csv,noheader,nounits"],
                               capture_output=True, text=True, timeout=NVIDIA_S)
            totals = [int(v) for v in r.stdout.split()]
        except (OSError, ValueError, subprocess.TimeoutExpired) as e:
            raise RuntimeError(f"runlock: nvidia-smi did not give the device's memory ({e}): a run is "
                               f"not admitted without its GPU budget") from e
        if r.returncode or not totals:
            raise RuntimeError(f"runlock: nvidia-smi did not give the device's memory "
                               f"({(r.stderr or r.stdout).strip()[:200]}): a run is not admitted "
                               f"without its GPU budget")
        return max(totals)
    if Path("/dev/nvidiactl").exists():
        raise RuntimeError("runlock: an NVIDIA device and no nvidia-smi to read its memory: a run is "
                           "not admitted without its GPU budget")
    totals = []
    for p in Path("/sys/class/drm").glob("card*/device/mem_info_vram_total"):
        try:
            totals.append(int(p.read_text()) >> 20)
        except (OSError, ValueError):
            pass
    return max(totals, default=None)


def vram_tokens(path: "Path | None" = None, cfg: "dict | None" = None) -> "int | None":
    """The GPU tokens there are on the lock of `path`, of the figures `cfg` when given (THE VRAM
    TOKENS); None where there is no device memory to bound."""
    total = device_total_mib()
    if total is None:
        return None
    cfg = cfg or vram_config(path)
    return max(MIN_TOKENS, (total - cfg["reserve_mib"]) // game_mib(cfg))


def plan(lanes: "int | None", gpu: bool = True, path: "Path | None" = None,
         cfg: "dict | None" = None) -> "tuple[int, int, int, int | None]":
    """(slots, tokens, games, token budget) for a run of `lanes` game lanes at once on the lock of
    `path` (of the figures `cfg` when given), on the GPU unless `gpu` is False; None lanes, the
    whole of both budgets."""
    budget = vram_tokens(path, cfg) if gpu else None
    if lanes is None:
        return BUDGET, budget or 0, budget or BUDGET // LANE_SLOTS, budget
    games = min(max(1, lanes), BUDGET // LANE_SLOTS)
    if budget:
        games = min(games, budget)
    return clamp(games * LANE_SLOTS), games if budget else 0, games, budget


def instance_resource(name: str) -> str:
    """The resource of the tacli instance `name` (RESOURCES)."""
    return f"instance-{name}"


def clamp(slots: int) -> int:
    return min(BUDGET, max(1, slots))


def say(text: str) -> None:
    print(f"runlock: {text}", file=sys.stderr, flush=True)


def _env() -> "dict | None":
    """RUNLOCK_HELD as this process inherited or set it, None when there is none."""
    try:
        held = json.loads(os.environ.get(ENV, ""))
    except ValueError:
        return None
    return held if isinstance(held, dict) and "tag" in held else None


def _env_held(path: Path) -> "dict | None":
    """The hold RUNLOCK_HELD says an ancestor has on `path`, None when it names none there."""
    held = _env()
    return held if held is not None and held.get("path") == str(path) else None


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


def _line(slots: int, what: str, resources=(), tokens=0) -> str:
    """A holder's line: the run, and what it asks for in all -- what it holds of that at a moment is
    the count of files bearing the line (report)."""
    return (f"pid {os.getpid()}, a run of {slots} slot{'s' if slots != 1 else ''}"
            + (f" and {tokens} GPU token{'s' if tokens != 1 else ''}" if tokens else "")
            + (f" and {', '.join(resources)}" if resources else "")
            + f", since {time.strftime('%Y-%m-%d %H:%M:%S')}, {what}, from {Path.cwd()}\n")


def _write(f, line: str) -> None:
    f.seek(0)
    f.truncate()
    f.write(line)
    f.flush()


def _held_now(p: Path, mode=fcntl.LOCK_SH) -> bool:
    """Whether another open file holds a lock on `p` that conflicts with `mode`, asked of the lock
    itself. The probe holds `mode` for no longer than the call: the run being admitted that tries
    the same file meanwhile finds it taken and tries again on its next round."""
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


def report(path: Path, budget: "int | None" = None) -> "tuple[int, list[str]]":
    """(free slots, a line for each holder): tareplay.lock held exclusively by flock(1), the run
    waiting for GPU tokens, the run being admitted, each run with the number of slots it holds, the
    GPU tokens held -- in all, of `budget` when it is given, and by each run -- then each resource
    held. Every count is of the lock files themselves, read once here: a run's tokens are the token
    files its line is in, whether it is admitted or still waiting, and the total is their sum."""
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
    tokenq = d / "tokenq"
    if tokenq.exists() and _held_now(tokenq):
        lines.append(f"waiting for GPU tokens: "
                     f"{_named(_read(tokenq) or 'a run that has not written its line yet')}")
    queue = d / "queue"
    if queue.exists() and _held_now(queue):
        lines.append(f"being admitted: {_named(_read(queue) or 'a run that has not written its line yet')}")
    lines += [f"{n:2} slot{'s' if n != 1 else ''} held by {_named(line)}" for line, n in runs.items()]
    by, other = {}, {}
    for k, p in _token_files(d):
        if _held_now(p):
            line = _read(p) or "a run that has not written its line yet"
            into = other if budget and k >= budget else by
            into[line] = into.get(line, 0) + 1
    held, past = sum(by.values()), sum(other.values())
    if held or past or budget:
        lines.append(f"GPU tokens: {held} held" + (f" of {budget}, {max(budget - held, 0)} free"
                                                   if budget else "")
                     + (f"; {past} more held under another budget, so no run takes a token until "
                        f"they end" if past else ""))
    lines += [f"{n:2} GPU token{'s' if n != 1 else ''} held by {_named(line)}" for line, n in by.items()]
    lines += [f"{n:2} GPU token{'s' if n != 1 else ''} held under another budget by {_named(line)}"
              for line, n in other.items()]
    for p in sorted(d.glob("res-*")):
        if _held_now(p):
            lines.append(f"resource {p.name[4:]} held by "
                         f"{_named(_read(p) or 'a run that has not written its line yet')}")
    return free, lines


def _wait_for_queue(queue, path: Path, wanted: str, budget: "int | None") -> None:
    """A queue taken, blocking: the kernel queues the waiters. While it waits, a thread says so,
    with `wanted` and the holders out of `budget` GPU tokens, every SAY_S."""
    try:
        fcntl.flock(queue, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return
    except BlockingIOError:
        pass
    stop = threading.Event()

    def remind():
        t0 = time.monotonic()
        while True:
            free, lines = report(path, budget)
            say(f"waiting for the run lock ({path.parent / 'runslots'}): {wanted}, {free} slots "
                f"free, another run ahead in the queue; {(time.monotonic() - t0) / 60:.0f} "
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


def _try(p: Path, line: str):
    """`p` locked LOCK_EX without waiting and the holder's line written into it; None when another
    run holds it."""
    f = open(p, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        f.close()
        return None
    _write(f, line)
    return f


def _take_tokens(path: Path, tokens: int, budget: int, line: str) -> list:
    """`tokens` of the `budget` GPU tokens, in the token queue (ADMISSION IS IN ARRIVAL ORDER, IN TWO
    QUEUES): the open files that hold them. Holds no slot while it waits."""
    d = slots_dir(path)
    t0 = time.monotonic()
    vram = {}
    queue = open(d / "tokenq", "a+")
    try:
        _wait_for_queue(queue, path, f"{tokens} GPU tokens wanted", budget)
        _write(queue, line)
        said, past, told = None, [], []
        while True:
            # no token is taken beside a run under another budget: this one's would add to its
            past = _past_budget(d, budget)
            for k in range(budget if not past else 0):
                if len(vram) == tokens:
                    break
                if k not in vram:
                    f = _try(token_file(d, k), line)
                    if f is not None:
                        vram[k] = f
            if len(vram) == tokens:
                break
            waited = time.monotonic() - t0
            if said is None or waited - said >= SAY_S or past != told:
                free, lines = report(path, budget)
                say(f"waiting for GPU tokens ({d}): this run has {len(vram)} of the {tokens} it wants "
                    f"and no slot"
                    + (f"; {', '.join(past)} held under another budget than this lock's {budget}, so "
                       f"no token is taken until they are let go" if past else "")
                    + f"; {waited / 60:.0f} min so far" + "".join(f"\n  {ln}" for ln in lines))
                said, told = waited, past
            time.sleep(POLL_S)
    except BaseException:
        for f in vram.values():
            f.close()
        raise
    finally:
        queue.close()
    return list(vram.values())


def _take(path: Path, slots: int, tokens: int, budget: "int | None", resources: list,
          what: str) -> "tuple[list, str]":
    """`tokens` of the `budget` GPU tokens in the token queue, then `slots` slots, every resource of
    `resources` and tareplay.lock shared in the slot queue (ADMISSION IS IN ARRIVAL ORDER, IN TWO
    QUEUES): the open files that hold them, and the run's tag."""
    d = slots_dir(path)
    line = _line(slots, what, resources, tokens)
    t0 = time.monotonic()
    vram = _take_tokens(path, tokens, budget, line) if tokens else []
    taken, named, shared = {}, {}, None
    try:
        queue = open(d / "queue", "a+")
    except BaseException:
        for f in vram:
            f.close()
        raise
    try:
        _wait_for_queue(queue, path, f"{slots} slots wanted"
                        + (f", this run's {tokens} GPU tokens held" if tokens else ""), budget)
        _write(queue, line)
        said = None
        while True:
            for k in range(BUDGET):
                if len(taken) == slots:
                    break
                if k not in taken:
                    f = _try(slot_file(d, k), line)
                    if f is not None:
                        taken[k] = f
            for r in resources:
                if r not in named:
                    f = _try(resource_file(d, r), line)
                    if f is not None:
                        named[r] = f
            if len(taken) == slots and len(named) == len(resources):
                shared = open(path, "a+")
                try:
                    fcntl.flock(shared, fcntl.LOCK_SH | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    shared.close()
                    shared = None
            waited = time.monotonic() - t0
            if said is None or waited - said >= SAY_S:
                free, lines = report(path, budget)
                wanted = [r for r in resources if r not in named]
                say(f"waiting for the run lock ({d}): {len(taken)} of the {slots} slots wanted "
                    f"taken, {free} free" + (f"; this run's {tokens} GPU tokens held" if tokens else "")
                    + (f"; waiting for {', '.join(wanted)}" if wanted else "")
                    + f"; {waited / 60:.0f} min so far" + "".join(f"\n  {ln}" for ln in lines))
                said = waited
            time.sleep(POLL_S)
    except BaseException:
        for f in [shared, *taken.values(), *vram, *named.values()]:
            if f is not None:
                f.close()
        raise
    finally:
        queue.close()
    waited = time.monotonic() - t0
    if waited >= 1:
        say(f"took the run lock ({slots} slots" + (f", {tokens} GPU tokens" if tokens else "")
            + f") after {waited / 60:.1f} min")
    return [shared, *taken.values(), *vram, *named.values()], f"s{min(taken):02d}"


def _take_now(path: Path, resources: list, what: str) -> list:
    """Each of `resources` for a run nested in another, without waiting (NESTING): the open files,
    or Busy naming the first another run holds."""
    d = slots_dir(path)
    line = _line(0, what, resources)
    got = []
    for r in resources:
        f = _try(resource_file(d, r), line)
        if f is None:
            for g in got:
                g.close()
            raise Busy(f"{r} is held by another run ({_named(_read(resource_file(d, r)))}); a run "
                       f"nested in another never waits for a resource its outer run did not name")
        got.append(f)
    return got


def _publish(path: Path, held: dict) -> None:
    os.environ[ENV] = json.dumps({**held, "resources": sorted(held["resources"]), "path": str(path)})


def _hold(path: Path, what: str, wanted, sizes, resources=()) -> bool:
    """`sizes(cfg)` gives (slots, tokens, games, token budget) of the lock's figures `cfg`, asked
    only when this process takes them, from the one read whose cap the run publishes; `wanted` is
    the games asked for, None for all, said when an outer run's are fewer."""
    global _held, _outer
    if _held:
        return False
    resources = sorted(set(resources))
    outer = _env_held(path)
    if outer is None and _ancestor_holds(path):
        outer = {"slots": BUDGET, "tokens": 0, "games": BUDGET // LANE_SLOTS, "tag": FLOCK_TAG,
                 "resources": [], "cap_mib": cap_mib(vram_config(path))}
    if outer is not None:
        if wanted is not None and wanted > outer["games"]:
            say(f"{what} wants {wanted} games at once inside a run that may play {outer['games']}: "
                f"it plays under those")
        extra = [r for r in resources if r not in outer["resources"]]
        if not extra:
            if _env() is None:
                _publish(path, outer)
            return False
        _held = _take_now(path, extra, what)
        _outer = os.environ.get(ENV)
        _publish(path, {**outer, "resources": outer["resources"] + extra})
        return True
    cfg = vram_config(path)
    slots, tokens, games_, budget = sizes(cfg)
    cap = cap_mib(cfg)
    _held, tag_ = _take(path, slots, tokens, budget, resources, what)
    _outer = None
    _publish(path, {"slots": slots, "tokens": tokens, "games": games_, "tag": tag_, "cap_mib": cap,
                    "resources": resources})
    return True


def hold(tree: Path, what: str, lanes: "int | None" = None, resources=(), gpu: bool = True) -> bool:
    """Hold the slots and GPU tokens of `lanes` game lanes at once -- the whole of both with None,
    no tokens when `gpu` is False -- and each resource of `resources` until release() or the end of
    this process, waiting in the queue for them; games() then says how many games the run may play
    at once. True when this call took any; False when this process holds them already, directly or
    through RUNLOCK_HELD or a `flock(1)` ancestor. Inside another run it takes only the resources
    that run did not name, without waiting, and raises Busy for one another run holds."""
    path = lock_path(tree)
    return _hold(path, what, lanes, lambda cfg: plan(lanes, gpu, path, cfg), resources)


def games() -> int:
    """The games this run may play at once (THE VRAM TOKENS): its lanes, cut to the GPU tokens."""
    held = _env()
    if held is None:
        raise RuntimeError("runlock.games() outside a run: hold() first")
    return held["games"]


def held_cap_mib() -> "int | None":
    """The GPU memory cap of the run this process is in, MiB, as its admission published it (THE
    VRAM TOKENS), else read from the lock it names (a run of runlock code that published none);
    None outside a run."""
    held = _env()
    if held is None:
        return None
    if isinstance(held.get("cap_mib"), int):
        return held["cap_mib"]
    return cap_mib(vram_config(Path(held["path"]) if held.get("path") else None))


def holding() -> bool:
    """Whether this process is inside a run: its own hold, or one it descends from."""
    return _env() is not None


def tag() -> str:
    """This run's tag (TAGS), held by this process or one it descends from."""
    held = _env()
    if held is None:
        raise RuntimeError("runlock.tag() outside a run: hold() first")
    return held["tag"]


def fds() -> tuple:
    """The descriptors of the hold this process took, () when it took none: a child given them in
    `pass_fds` holds the slots with this process until it exits (A HOLD THAT ENDS BEFORE THE
    PROCESS)."""
    return tuple(f.fileno() for f in _held)


def release() -> None:
    """End this process's hold: its descriptors closed and RUNLOCK_HELD put back to what it was
    before (none, or the outer run's), so the processes it starts from now on wait like any other
    run. The slots themselves go once no child given fds() still runs. Nothing when this process
    took none."""
    global _held, _outer
    if not _held:
        return
    if _outer is None:
        os.environ.pop(ENV, None)
    else:
        os.environ[ENV] = _outer
    for f in _held:
        f.close()
    _held, _outer = [], None


def cmd_run(a) -> int:
    if not a.cmd:
        say("run: no command after --")
        return 2
    slots = clamp(a.slots)
    path = lock_path(Path.cwd())

    def sizes(cfg):
        budget = vram_tokens(path, cfg)
        tokens = min(budget, a.vram if a.vram is not None else -(-slots // LANE_SLOTS)) if budget else 0
        return slots, tokens, tokens or slots // LANE_SLOTS or 1, budget
    try:
        _hold(path, a.what, a.vram if a.vram is not None else -(-slots // LANE_SLOTS),
              sizes, a.resource)
    except Busy as e:
        say(f"run: {e}")
        return 2
    for fd in fds():
        os.set_inheritable(fd, True)
    try:
        os.execvp(a.cmd[0], a.cmd)
    except OSError as e:
        say(f"run: {a.cmd[0]}: {e}")
        return 127


def cmd_status(a) -> int:
    path = lock_path(Path.cwd())
    budget = vram_tokens(path)
    free, lines = report(path, budget)
    print(f"runlock: {free} of {BUDGET} slots free ({path.parent / 'runslots'})"
          + ("" if budget else "; no GPU budget: no device memory reads"))
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
    r.add_argument("--vram", type=int, default=None, metavar="T",
                   help="GPU tokens, one a game on the GPU at once (default: one for each "
                        f"{LANE_SLOTS} slots), cut to the budget; 0 for a run with no GPU")
    r.add_argument("--what", required=True, help="what the run is, for the holder line")
    r.add_argument("--resource", action="append", default=[], metavar="NAME",
                   help="a resource no other run may hold meanwhile, instance-<name> for the "
                        "tacli instance <name>; repeatable")
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
