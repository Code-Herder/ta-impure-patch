#!/usr/bin/env python3
"""Game regressions share the machine by declared lanes: the weighted run lock.

    runlock.py run --slots W [--vram G] [--suite S] --what TEXT [--resource NAME]... -- CMD ...
                                       CMD under W slots, the GPU memory of G games under suite
                                       S's cap and the named resources, waiting for them first
    runlock.py status                  who holds which slots, GPU memory, game seats and
                                       resources, and what is free

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
and takes LANE_SLOTS a lane, clamped to 1..BUDGET, and the GPU memory of each lane that renders
on the GPU (THE GPU MEMORY); a run that declares none takes the whole budget of both and so runs
alone. games() is how many games the run may play at once: its lanes, cut to the GPU memory there
is, and THE GAME SEATS hold every launch to it. Every tool declares its lanes: what else two runs
could meet on is kept apart by TAGS, for what a run makes for itself, and by RESOURCES, for what it
is given.

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

ADMISSION IS IN ARRIVAL ORDER, IN TWO QUEUES. A run that wants GPU memory first takes
runslots/tokenq, the TOKEN QUEUE, LOCK_EX, blocking; holding it, it reads the lock's figures (THE
BUDGET BELONGS TO THE LOCK) and takes its unit files ALL AT ONCE OR NOT AT ALL: it tries every one
it needs without waiting, and when one is held it lets go of those it took and tries again a round
later, so between two tries it holds no unit. With them all it lets the token queue go. It holds no
slot and not the slot queue meanwhile, so a run that wants no GPU memory is never held up by one
that waits for it. Every run -- one holding its GPU memory now, and one that wants none from the
start -- then takes runslots/queue, the SLOT QUEUE, LOCK_EX, blocking in the kernel's queue;
holding it, it takes free slots, its resources and tareplay.lock shared, keeps each one until it
has them all, and only then lets the slot queue go. Only a queue's holder takes what that queue
hands out, so a run that arrives behind a big one cannot take GPU memory or a slot before it, and
the big one is never starved by small ones. The waiters for a queue are woken in the order they
blocked and then race for it, so the order among them is arrival order to within that race; what
is guaranteed is that a queue's holder is served before every run behind it in that queue. The
retries are a liveness device: what a run holds is always a flock, and the bounds rest on the
flocks alone.

  * No deadlock. A run waits for one thing at a time, and never while holding what a run ahead of
    it waits for: the token queue's holder holds no unit while it waits for units (all at once or
    not at all) and holds no slot; a run waiting for the slot queue holds at most its GPU memory,
    which no run in the slot queue wants -- every run takes its units before it reaches that
    queue; the slot queue's holder waits only for slots, resources and tareplay.lock, held by
    admitted runs (and by a flock(1) chain), which end. So the slot queue's holder is admitted, the
    run behind it becomes the holder, every run that holds GPU memory is admitted, and the token
    queue's holder gets its units as those runs end.
  * No deadlock, mixed versions. Runlock code of before the GPU budget was memory counts GPU tokens
    in files runslots/vram-NN, which this code never takes; some versions of it gather them while
    holding the slot queue, keeping what they take. This code waits for them only as the token
    queue's holder, holding nothing (_others), and never wants what that code takes in the slot
    queue, so no cycle runs through both: a run of that code waits only for its own kind's tokens,
    slots, resources and tareplay.lock, held by runs that are admitted or are about to be, and the
    argument above holds for this code's runs beside it.
  * No starvation. Each queue serves its holder before the runs behind it, and its holder is
    served as the runs ahead of it end (above): a GPU run is behind only GPU runs for its memory,
    and behind every run that reached the slot queue first for its slots.
  * The budgets hold. A slot and a unit are each one flock(2) file held LOCK_EX by one run, so the
    slots held never sum past BUDGET and the units never past the files there are, gpu_budget() at
    the admission that made them; holding units while waiting for slots counts against the GPU
    budget like any held unit.

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

THE GPU MEMORY is bounded the way the slots bound the threads, in MiB. The budget is the device's
memory less reserve_mib, read from the device at each admission, with the figures from the LOCK'S
OWN CONFIGURATION, runslots/vram.cfg (below), which tools/runlock-vram.cfg seeds and which says
where they come from; it is never less than MIN_GAMES games at cap_mib, because a network game is
two games and a smaller card still has to run one. It is counted in units of GRAIN_MIB, each a
flock(2) file runslots/gpu-NNNN (287 units, 9184 MiB, on the reference setup). A run says what cap
its games run under -- its suite's (`suite`, tools/runlock-suites.cfg: what that suite's games were
measured to need), or the play defaults' cap_mib -- and takes, all at once, the units of its games
at that cap: each game's BOUND, the cap plus what the driver holds beside it (driver_mib), and a
margin, rounded up to whole units once a run (gpu_units). Runs with different caps mix freely: each
unit is held by one run, so the memory held never sums past the budget, and each run's games stay
inside the memory it holds. The bound holds by construction: a run with GPU memory publishes its
cap in RUNLOCK_HELD (`cap_mib`), every game launched inside the hold carries that cap in its store
(`gpumem=`, write_gpumem(), whatever launches it: tacli, and every harness that starts the game
itself), and the DLL refuses any device-local allocation that would take the game's own past it
(inc/tagpu_vkcap.h). A refusal that the renderer does not plan for fails the game in every suite
(talog.cap_refusals): a cap that is too small shows as a failure and never changes a result
quietly. A run without GPU memory publishes no cap, and its games run uncapped. A run that wants
more waits in the token queue holding neither a slot nor a unit; a run that wants none never waits
for one and never reads the figures, so a runslots/vram.cfg that cannot be read refuses only the
runs that want GPU memory. A run that renders without the GPU (llvmpipe, GDI) takes none. A
machine with no device memory to read -- no NVIDIA or amdgpu card -- has no budget; one whose
NVIDIA driver is there but does not answer refuses the run rather than run it unbounded.

THE GAME SEATS hold the games a run plays at once to games(), whatever tool plays them: every game
launched inside a run with GPU memory takes a seat, a flock(2) file runslots/seat-<tag>-NN with NN
under games(), held by the game's process itself (game_seat(), passed to it in `pass_fds`), and a
launch waits while every seat is held. A tool that starts more lanes than games() therefore waits
at the launch rather than puts a game on the GPU its run did not take memory for. A process the
game starts can inherit the seat too (wine's server), which only holds it longer. [INFERRED, not yet
measured: that a descriptor `wine TotalA.exe` inherits stays open for the game's life; `status`
lists the seats held, which shows it in a live run.] Seats are named by the run's tag, so their
files are a bounded set, and a game left running by a dead run holds its seat against the next run
of that tag: a game still on the GPU still counts. A lane that plays two
games at once takes its second seat holding its first; lanes that need more seats together than
games() can wait on each other, so a tool counts a network game as two games. Games that wait on
each other -- a network game's peers, which meet in one battle room -- need their seats together:
in a run whose games() is under their count the last launch waits for a seat the others hold while
they wait for it in the lobby, which never ends. Every such tool asks together() before its first
launch and refuses the game by name when the run may not play them all at once.

THE BUDGET BELONGS TO THE LOCK, NOT TO A TREE. Branches and worktrees at different commits share one
runslots directory and run side by side, so a budget each read from its own tree's file could
differ, and two of them together could put more games on the GPU than the device holds -- MEASURED
2026-11-13: `status` counted 11 tokens held of 9, a run of an older tree (11 tokens, a cap of 768)
holding vram-09 and vram-10 beside this tree's runs. So:
  * Every run reads the budget's figures from runslots/vram.cfg. When it is absent, the first run
    writes it from its tree's tools/runlock-vram.cfg, whole, by a hard link of a finished file, so
    it is written once and never seen half-written; a run whose tree's figures differ says so and
    uses the lock's. To change the budget, edit runslots/vram.cfg (or remove it, to be seeded
    again). A suite's cap is the tree's own (tools/runlock-suites.cfg): it is what that tree's
    renderer needs, and the budget counts each run at its own.
  * A unit file numbered at or above the budget is one only a run under another budget takes. The
    token queue's holder takes no unit while such a file is held: it waits, naming it, until that
    run ends, so it never adds a game to another budget's. `status` shows such units as held under
    another budget. A change to runslots/vram.cfg takes effect the same way: a smaller budget
    admits no unit until the runs holding the files past it end.
  * The cap a game runs under is the one in RUNLOCK_HELD, from the one read of the figures that
    sized its run's memory, made once the run holds the token queue: a budget changed while it
    waited for that queue is the one it is admitted under, and a game's cap always matches the
    memory its run holds. A hold that published no cap (runlock code of before the cap) names no
    cap for its games, which run uncapped: its tokens were sized on its own tree's figures,
    unknown here.
TRANSITION: runlock code of before this rule counts GPU tokens (runslots/vram-NN). This code takes
no GPU memory while any such token is held (_others), so it never adds to a device that code is
filling, but that code does not see this code's units and can still overfill the device beside
them; it cannot deadlock with this code (No deadlock, mixed versions). Every branch that launches
GPU runs must merge this before it runs beside another.

flock(1) ON tareplay.lock STILL EXCLUDES EVERY RUN. A run also holds tareplay.lock LOCK_SH, taken
once it has its slots; flock(1) takes the file LOCK_EX by default, so a hand chain under it waits
for every run and every run waits while it holds. Such a chain waits outside the queue: Linux grants
a shared lock beside a waiting exclusive one, so it can wait for as long as runs keep overlapping.
Hand chains therefore go through `runlock.py run`, which queues like the tools.

NESTING. The holder exports RUNLOCK_HELD, a JSON object of its slots, GPU memory (`gpu_mib`),
games, tag, resources, lock path and the GPU cap it was admitted under (none without GPU memory),
which its children inherit; a process that finds it, or that has a flock(1) ancestor on
tareplay.lock (which holds every slot and no GPU memory, so no cap), takes no slots: the outer
run's declaration covers its children, and its games take the outer run's seats. A nested run that asks for more games than its ancestor may play says so and runs under
the ancestor's. A resource the outer run did not name, a nested run takes without waiting -- waiting
there could wait on the queue's holder, which waits on the outer run's slots -- and Busy is raised
when another run holds it. A child started with an environment of its own carries RUNLOCK_HELD
over: without it, it would queue behind the run it belongs to and wait for that run's end.

HOLDERS. A run writes its line -- pid, slots, GPU memory, resources, start, what, directory -- into
each slot, unit and resource file it takes, after taking it (its units once it has them all), and
into the token queue's or the slot queue's file while it waits there as the holder; a game writes
its own into its seat.
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
MIN_GAMES = 2                   # the fewest games the GPU budget holds: one network game (THE GPU MEMORY)
GRAIN_MIB = 32                  # the GPU budget's unit, one flock(2) file (THE GPU MEMORY)
VRAM_CFG = Path(__file__).resolve().with_name("runlock-vram.cfg")
SUITES_CFG = Path(__file__).resolve().with_name("runlock-suites.cfg")
NVIDIA_S = 60                   # a hang bound on nvidia-smi, deciding nothing
POLL_S = 2                      # how often the run being admitted tries the slots again
SAY_S = 300                     # how often a waiting run says it is still waiting

_held = []                      # this process's hold: its slot and resource files, tareplay.lock
_outer = None                   # RUNLOCK_HELD before a nested hold took resources, for release()
_default_lock = None            # default_lock(), once asked
_said_differs = set()           # the locks whose figures this process has said its tree differs from


class Busy(RuntimeError):
    """A resource another run holds, asked for by a run nested in another (NESTING)."""


class BadFigures(ValueError):
    """A VRAM configuration file that cannot be read as one, its path in the message."""


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


def unit_file(d: Path, k: int) -> Path:
    return d / f"gpu-{k:04d}"


def _numbered(d: Path, prefix: str) -> list:
    """[(k, file)] of every `<prefix>-<k>` file there is, by number."""
    out = []
    for p in d.glob(f"{prefix}-*"):
        m = re.fullmatch(rf"{prefix}-(\d+)", p.name)
        if m:
            out.append((int(m.group(1)), p))
    return sorted(out)


def _others(d: Path, budget: int) -> list:
    """The GPU files a run holds that this run's budget of `budget` units does not count: a unit
    numbered at or past it (a run under another budget, THE BUDGET BELONGS TO THE LOCK) and a
    token of runlock code of before the GPU budget was memory (vram-NN, TRANSITION)."""
    return ([p.name for k, p in _numbered(d, "gpu") if k >= budget and _held_now(p)]
            + [p.name for _k, p in _numbered(d, "vram") if _held_now(p)])


def seat_file(d: Path, tag_: str, k: int) -> Path:
    return d / f"seat-{tag_}-{k:02d}"


VRAM_KEYS = ("reserve_mib", "cap_mib", "driver_mib", "margin_mib")
# The caps the DLL takes (inc/tagpu_vkcap.h, TAGPU_VKCAP_MIN_MIB and TAGPU_VKCAP_MAX_MIB): it logs
# any other `gpumem=` and runs UNCAPPED, so GPU memory sized on such a cap would bound nothing
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
                raise BadFigures(f"{path}: {key} is not one of {', '.join(VRAM_KEYS)}, or is "
                                 f"named twice")
            try:
                cfg[key] = int(value)
            except ValueError:
                raise BadFigures(f"{path}: {key} = {value.strip()!r} is not a whole number of "
                                 f"MiB") from None
    missing = [k for k in VRAM_KEYS if k not in cfg]
    if missing or any(v < 0 for v in cfg.values()):
        raise BadFigures(f"{path}: {', '.join(missing) or 'a negative figure'}: every one of "
                         f"{', '.join(VRAM_KEYS)} is a figure of MiB")
    if not CAP_MIN_MIB <= cfg["cap_mib"] <= CAP_MAX_MIB:
        raise BadFigures(f"{path}: cap_mib {cfg['cap_mib']} is not a cap the DLL takes "
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
        with open(tmp, "w") as f:
            f.write(VRAM_CFG.read_text())
            f.flush()
            os.fsync(f.fileno())            # never an empty file under the name after a power loss
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
    """The GPU memory cap of a game of a run that names no suite, MiB: the play defaults' (THE GPU
    MEMORY)."""
    return (cfg or vram_config())["cap_mib"]


def suite_cap(suite: str) -> int:
    """The GPU memory cap a game of `suite` runs under, MiB, from this tree's
    tools/runlock-suites.cfg: what that suite's games were measured to need, which is this tree's
    renderer's and so the tree's own figure (THE GPU MEMORY). BadFigures for a suite the file does
    not name, or a figure the DLL would not take."""
    caps = {}
    for line in SUITES_CFG.read_text().splitlines():
        key, eq, value = line.partition("=")
        if eq and not line.lstrip().startswith("#"):
            try:
                caps[key.strip()] = int(value)
            except ValueError:
                raise BadFigures(f"{SUITES_CFG}: {key.strip()} = {value.strip()!r} is not a whole "
                                 f"number of MiB") from None
    if suite not in caps:
        raise BadFigures(f"{SUITES_CFG}: no suite {suite!r} (it names {', '.join(sorted(caps))})")
    if not CAP_MIN_MIB <= caps[suite] <= CAP_MAX_MIB:
        raise BadFigures(f"{SUITES_CFG}: {suite} = {caps[suite]} is not a cap the DLL takes "
                         f"({CAP_MIN_MIB}..{CAP_MAX_MIB}): its games would run uncapped")
    return caps[suite]


def game_bound_mib(cfg: "dict | None" = None, cap: "int | None" = None) -> int:
    """B, the most device memory one harness game under the cap `cap` (None: cap_mib) holds, MiB:
    the cap on its own allocations and what the driver holds for it beside them."""
    cfg = cfg or vram_config()
    return (cap or cfg["cap_mib"]) + cfg["driver_mib"]


def game_mib(cfg: "dict | None" = None, cap: "int | None" = None) -> int:
    """What one game under the cap `cap` takes of the GPU budget, MiB: its bound and the margin."""
    cfg = cfg or vram_config()
    return game_bound_mib(cfg, cap) + cfg["margin_mib"]


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


def gpu_budget(path: "Path | None" = None, cfg: "dict | None" = None) -> "int | None":
    """The GPU budget of the lock of `path`, in GRAIN_MIB units, of the figures `cfg` when given
    (THE GPU MEMORY): the device's memory less reserve_mib, and never less than MIN_GAMES games at
    cap_mib; None where there is no device memory to bound."""
    total = device_total_mib()
    if total is None:
        return None
    cfg = cfg or vram_config(path)
    return max((total - cfg["reserve_mib"]) // GRAIN_MIB, gpu_units(MIN_GAMES, cfg))


def gpu_units(n: int, cfg: dict, cap: "int | None" = None) -> int:
    """The units `n` games under the cap `cap` take: their game_mib, rounded up once a run."""
    return -(-n * game_mib(cfg, cap) // GRAIN_MIB)


def gpu_games(budget: int, cfg: dict, cap: "int | None" = None) -> int:
    """The most games under the cap `cap` that `budget` units hold: the largest n with
    gpu_units(n) <= budget."""
    return budget * GRAIN_MIB // game_mib(cfg, cap)


def plan(lanes: "int | None", gpu: bool = True, path: "Path | None" = None,
         cfg: "dict | None" = None, suite: "str | None" = None) -> "tuple[int, int, int, int | None, int | None]":
    """(slots, GPU units, games, GPU budget in units, cap) for a run of `lanes` game lanes at once
    on the lock of `path` (of the figures `cfg` when given), its games on the GPU under `suite`'s cap
    (None: cap_mib) unless `gpu` is False; None lanes, the whole of both budgets."""
    budget = gpu_budget(path, cfg) if gpu else None
    if not budget:
        games = BUDGET // LANE_SLOTS if lanes is None else min(max(1, lanes), BUDGET // LANE_SLOTS)
        return (BUDGET if lanes is None else clamp(games * LANE_SLOTS)), 0, games, budget, None
    cfg = cfg or vram_config(path)
    cap = suite_cap(suite) if suite else cfg["cap_mib"]
    most = gpu_games(budget, cfg, cap)
    if most < 1:
        raise RuntimeError(f"a game of {suite or 'a run that names no suite'} under a cap of {cap} MiB "
                           f"takes {game_mib(cfg, cap)} MiB, more than the whole GPU budget "
                           f"({budget * GRAIN_MIB} MiB): no run of it is admitted")
    if lanes is None:
        return BUDGET, budget, most, budget, cap
    games = min(max(1, lanes), BUDGET // LANE_SLOTS, most)
    return clamp(games * LANE_SLOTS), gpu_units(games, cfg, cap), games, budget, cap


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


def _line(slots: int, what: str, resources=(), mib=0) -> str:
    """A holder's line: the run, and what it asks for in all -- what it holds of that at a moment is
    the count of files bearing the line (report)."""
    return (f"pid {os.getpid()}, a run of {slots} slot{'s' if slots != 1 else ''}"
            + (f" and {mib} MiB of the GPU" if mib else "")
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
    waiting for GPU memory, the run being admitted, each run with the number of slots it holds, the
    GPU memory held -- in all, of `budget` units when it is given, and by each run -- the GPU files
    held under another budget or by older runlock code, the game seats held, then each resource
    held. Every count is of the lock files themselves, read once here: a run's GPU memory is the
    unit files its line is in, whether it is admitted or still waiting, and the total is their
    sum."""
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
        lines.append(f"waiting for GPU memory: "
                     f"{_named(_read(tokenq) or 'a run that has not written its line yet')}")
    queue = d / "queue"
    if queue.exists() and _held_now(queue):
        lines.append(f"being admitted: {_named(_read(queue) or 'a run that has not written its line yet')}")
    lines += [f"{n:2} slot{'s' if n != 1 else ''} held by {_named(line)}" for line, n in runs.items()]
    by, other = {}, {}
    for k, p in _numbered(d, "gpu"):
        if _held_now(p):
            line = _read(p) or "a run that has not written its line yet"
            into = other if budget and k >= budget else by
            into[line] = into.get(line, 0) + 1
    old = {}
    for _k, p in _numbered(d, "vram"):
        if _held_now(p):
            line = _read(p) or "a run that has not written its line yet"
            old[line] = old.get(line, 0) + 1
    held = sum(by.values())
    if held or other or old or budget:
        lines.append(f"GPU memory: {held * GRAIN_MIB} MiB held"
                     + (f" of {budget * GRAIN_MIB}, {max(budget - held, 0) * GRAIN_MIB} free"
                        if budget else "")
                     + (f"; {sum(other.values()) * GRAIN_MIB} MiB more held under another budget"
                        if other else "")
                     + (f"; {sum(old.values())} GPU tokens of older runlock code held" if old else "")
                     + ("; no run takes GPU memory until they end" if other or old else ""))
    lines += [f"{n * GRAIN_MIB:5} MiB of the GPU held by {_named(line)}" for line, n in by.items()]
    lines += [f"{n * GRAIN_MIB:5} MiB of the GPU held under another budget by {_named(line)}"
              for line, n in other.items()]
    lines += [f"{n:2} GPU token{'s' if n != 1 else ''} of older runlock code held by {_named(line)}"
              for line, n in old.items()]
    seats = {}
    for p in sorted(d.glob("seat-*")):
        if _held_now(p):
            line = _read(p) or "a game that has not written its line yet"
            run = p.name.rsplit("-", 1)[0][5:]
            seats.setdefault(run, []).append(line)
    for run, held_by in seats.items():
        lines.append(f"{len(held_by):2} game seat{'s' if len(held_by) != 1 else ''} of run {run} held: "
                     + "; ".join(_named(ln) for ln in held_by))
    for p in sorted(d.glob("res-*")):
        if _held_now(p):
            lines.append(f"resource {p.name[4:]} held by "
                         f"{_named(_read(p) or 'a run that has not written its line yet')}")
    return free, lines


def _wait_for_queue(queue, path: Path, wanted: str, budget: "int | None") -> None:
    """A queue taken, blocking: the kernel queues the waiters. While it waits, a thread says so,
    with `wanted` and the holders out of a GPU budget of `budget` units, every SAY_S."""
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


def _try(p: Path, line: "str | None"):
    """`p` locked LOCK_EX without waiting and the holder's line written into it (none with None);
    None when another run holds it."""
    f = open(p, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        f.close()
        return None
    if line is not None:
        _write(f, line)
    return f


def _all_units(path: Path, units: int, budget: int, line: str, t0: float) -> list:
    """`units` of the `budget` GPU units, taken all at once or not at all, by the token queue's
    holder (ADMISSION IS IN ARRIVAL ORDER, IN TWO QUEUES): their open files. Between two tries it
    holds no unit, and it takes none while a GPU file this budget does not count is held
    (_others)."""
    d = slots_dir(path)
    said, told = None, None
    while True:
        # no unit is taken beside a run this budget does not count: this one's would add to its
        others = _others(d, budget)
        got = []
        if not others:
            for k in range(budget):
                # the line is written once the set is whole: a try let go leaves no file naming
                # this run as its holder
                f = _try(unit_file(d, k), None)
                if f is not None:
                    got.append(f)
                    if len(got) == units:
                        for g in got:
                            _write(g, line)
                        return got
            for f in got:                   # not all of them: none is kept while this run waits
                f.close()
        waited = time.monotonic() - t0
        if said is None or waited - said >= SAY_S or others != told:
            free, lines = report(path, budget)
            say(f"waiting for GPU memory ({d}): {units * GRAIN_MIB} MiB wanted at once, holding none "
                f"meanwhile"
                + (f"; {', '.join(others)} held by runs this budget of {budget * GRAIN_MIB} MiB does "
                   f"not count, so none is taken until they are let go" if others
                   else f"; {len(got) * GRAIN_MIB} MiB free")
                + f"; {waited / 60:.0f} min so far" + "".join(f"\n  {ln}" for ln in lines))
            said, told = waited, others
        time.sleep(POLL_S)


def _take(path: Path, what: str, resources: list, sizes, gpu: bool) -> "tuple[list, str, tuple]":
    """A GPU run's memory in the token queue, then every run's slots, resources and tareplay.lock
    shared in the slot queue (ADMISSION IS IN ARRIVAL ORDER, IN TWO QUEUES): the open files that
    hold them, the run's tag, and (slots, units, games, budget, cap) as `sizes(figures)` gave them.
    A GPU run reads the lock's figures once it holds the token queue, so its budget and its cap are
    those of the moment it takes its memory; any other run reads none (figures None)."""
    d = slots_dir(path)
    t0 = time.monotonic()
    cfg, units_, taken, named, shared, tokenq, queue = None, [], {}, {}, None, None, None
    try:
        if gpu and device_total_mib() is not None:
            tokenq = open(d / "tokenq", "a+")
            _wait_for_queue(tokenq, path, "GPU memory wanted", None)
            cfg = vram_config(path)
        slots, units, games_, budget, cap = sizes(cfg)
        line = _line(slots, what, resources, units * GRAIN_MIB)
        if tokenq is not None:
            if units:
                _write(tokenq, line)
                units_ = _all_units(path, units, budget, line, t0)
            tokenq.close()
            tokenq = None
        queue = open(d / "queue", "a+")
        _wait_for_queue(queue, path, f"{slots} slots wanted"
                        + (f", this run's {units * GRAIN_MIB} MiB of the GPU held" if units else ""),
                        budget)
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
                    f"taken, {free} free" + (f"; this run's {units * GRAIN_MIB} MiB of the GPU held"
                                             if units else "")
                    + (f"; waiting for {', '.join(wanted)}" if wanted else "")
                    + f"; {waited / 60:.0f} min so far" + "".join(f"\n  {ln}" for ln in lines))
                said = waited
            time.sleep(POLL_S)
    except BaseException:
        for f in [shared, tokenq, *taken.values(), *units_, *named.values()]:
            if f is not None:
                f.close()
        raise
    finally:
        if queue is not None:
            queue.close()
    waited = time.monotonic() - t0
    if waited >= 1:
        say(f"took the run lock ({slots} slots" + (f", {units * GRAIN_MIB} MiB of the GPU" if units else "")
            + f") after {waited / 60:.1f} min")
    return ([shared, *taken.values(), *units_, *named.values()], f"s{min(taken):02d}",
            (slots, units, games_, budget, cap))


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


def _hold(path: Path, what: str, wanted, sizes, resources=(), gpu: bool = True) -> bool:
    """`sizes(cfg)` gives (slots, GPU units, games, GPU budget, cap) of the lock's figures `cfg`
    (None: no GPU budget), asked only when this process takes them; a run with `gpu` reads the
    figures once it holds the token queue and publishes the cap of that read. `wanted` is the
    games asked for, None for all, said when an outer run's are fewer."""
    global _held, _outer
    if _held:
        return False
    resources = sorted(set(resources))
    outer = _env_held(path)
    if outer is None and _ancestor_holds(path):
        outer = {"slots": BUDGET, "gpu_mib": 0, "games": BUDGET // LANE_SLOTS, "tag": FLOCK_TAG,
                 "resources": [], "cap_mib": None}
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
    _held, tag_, (slots, units, games_, budget, cap) = _take(path, what, resources, sizes, gpu)
    _outer = None
    # a game of a run with GPU memory runs under the cap that memory was sized on; any other,
    # uncapped
    _publish(path, {"slots": slots, "gpu_mib": units * GRAIN_MIB, "games": games_, "tag": tag_,
                    "cap_mib": cap if units else None, "resources": resources})
    return True


def hold(tree: Path, what: str, lanes: "int | None" = None, resources=(), gpu: bool = True,
         suite: "str | None" = None) -> bool:
    """Hold the slots and GPU memory of `lanes` game lanes at once -- the whole of both with None,
    no GPU memory when `gpu` is False, each game's under `suite`'s cap (tools/runlock-suites.cfg;
    None, the play defaults' cap_mib) -- and each resource of `resources` until release() or the
    end of this process, waiting in the queue for them; games() then says how many games the run
    may play at once. True when this call took any; False when this process holds them already,
    directly or through RUNLOCK_HELD or a `flock(1)` ancestor. Inside another run it takes only the
    resources that run did not name, without waiting, and raises Busy for one another run holds."""
    path = lock_path(tree)
    if suite is not None:
        suite_cap(suite)                    # a suite the file does not name refuses before the wait
    return _hold(path, what, lanes, lambda cfg: plan(lanes, cfg is not None, path, cfg, suite),
                 resources, gpu)


def games() -> int:
    """The games this run may play at once (THE GPU MEMORY): its lanes, cut to its GPU memory."""
    held = _env()
    if held is None:
        raise RuntimeError("runlock.games() outside a run: hold() first")
    return held["games"]


def held_cap_mib() -> "int | None":
    """The GPU memory cap of the run this process is in, MiB, as its admission published it with its
    GPU memory (THE GPU MEMORY); None outside a run, in a run without GPU memory, and in a run of
    runlock code that published none -- whose tokens were sized on its own tree's figures, which
    this cannot know -- so its games run uncapped, as that code ran them."""
    held = _env()
    cap = held.get("cap_mib") if held is not None else None
    return cap if isinstance(cap, int) else None


def write_gpumem(gamedir: Path) -> "int | None":
    """The `gpumem=` line of the game store in `gamedir` (impure.cfg) set to the cap of the run this
    process is in, or removed when it has none (held_cap_mib), whatever launches the game: the
    DLL's cap is what the run's GPU memory was sized on, so a game launched inside a run with GPU
    memory and without this line would hold memory the budget does not count. Returns the cap."""
    cap = held_cap_mib()
    path = Path(gamedir) / "impure.cfg"
    lines = path.read_text().splitlines() if path.exists() else []
    keep = [ln for ln in lines if ln.partition("=")[0].strip().lower() != "gpumem"]
    if cap is not None:
        keep.append(f"gpumem={cap}")
    if keep != lines:
        path.write_text("\n".join(keep) + ("\n" if keep else ""))
    return cap


def game_seat(what: str):
    """A seat for one game of the run this process is in, an open file the game's process is to hold
    (pass it in `pass_fds`, then close it here): THE GAME SEATS. Waits while every seat of the run
    is held. None outside a run or in a run without GPU memory, whose games need none."""
    held = _env()
    if held is None or not held.get("gpu_mib") or not held.get("path"):
        return None
    d = slots_dir(Path(held["path"]))
    n, tag_ = held["games"], held["tag"]
    line = f"pid {os.getpid()}, a game: {what}, since {time.strftime('%Y-%m-%d %H:%M:%S')}\n"
    t0, said = time.monotonic(), None
    while True:
        for k in range(n):
            f = _try(seat_file(d, tag_, k), line)
            if f is not None:
                return f
        waited = time.monotonic() - t0
        if said is None or waited - said >= SAY_S:
            say(f"{what}: waiting for a game seat: the run ({tag_}) plays {n} game{'s' if n != 1 else ''} "
                f"at once on the GPU, and every seat is held; {waited / 60:.0f} min so far")
            said = waited
        time.sleep(POLL_S)


def together(n: int, what: str) -> "str | None":
    """Why `what`, whose `n` games wait on each other, cannot play in this run -- its games() is under
    `n`, so its last launch would wait for a seat forever (THE GAME SEATS) -- or None when it can.
    Asked before the first of them launches."""
    g = games()
    if g >= n:
        return None
    return (f"{what}: its {n} games wait on each other and this run may play {g} at once "
            f"(runlock.games()): its last launch would wait for a seat the others hold, so none is started")


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
    lanes = a.vram if a.vram is not None else -(-slots // LANE_SLOTS)

    def sizes(cfg):
        _s, units, games_, budget, cap = plan(lanes, cfg is not None, path, cfg, a.suite)
        return slots, units, (games_ if units else slots // LANE_SLOTS or 1), budget, cap
    if a.suite is not None:
        suite_cap(a.suite)
    try:
        _hold(path, a.what, lanes, sizes, a.resource, gpu=a.vram != 0)
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
    budget = gpu_budget(path)
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
    r.add_argument("--vram", type=int, default=None, metavar="G",
                   help="games on the GPU at once, each taking its cap's share of the GPU budget "
                        f"(default: one for each {LANE_SLOTS} slots), cut to the budget; 0 for a run "
                        "with no GPU")
    r.add_argument("--suite", default=None, metavar="NAME",
                   help="the suite whose games these are: their GPU memory cap is that suite's in "
                        "tools/runlock-suites.cfg (default: the play defaults' cap_mib)")
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
    except BadFigures as e:
        say(f"the GPU figures cannot be read, so no run that wants GPU memory is admitted: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
