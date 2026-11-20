#!/usr/bin/env python3
"""Game regressions share the machine by declared lanes: the weighted run lock.

    runlock.py run (--slots W | --lanes N) [--vram G] [--suite S] --what TEXT [--resource NAME]...
            -- CMD ...                 CMD under W slots, or the slots of N game lanes (THE LANE
                                       WEIGHTS), the GPU memory of G games under suite S's cap and
                                       the named resources, waiting for them first
    runlock.py run --sim N --what TEXT -- CMD ...
                                       CMD on N places of the sim pool, at idle priority and each
                                       process under the pool's memory bound (THE SIM POOL)
    runlock.py make [-j N] [--what TEXT] -- MAKE_ARGS ...
                                       make -jK MAKE_ARGS under at most N slots (8 by default), K
                                       the slots it was granted (BUILDS)
    runlock.py status                  who holds which slots, GPU memory, game seats, resources
                                       and sim-pool places, the sim games playing and the places
                                       yielding, what is free, and what the load guard says

All act on the lock of the repository the current directory is in.

WHY. A landing gate alone peaks at a load of about 24 on the reference setup's 32 threads, so two at
once oversubscribe it, every game runs slower, and the checks bounded in wall-clock or CPU time
fail for the load alone (MEASURED 2026-10-12: four gates at once took 34 min each against 11 alone,
at a load average of 63-87, and failed two such checks). Every tool that plays games for a
regression -- tareplay.py's game commands, tacompat.py wine, wiki_regression.py's game lanes,
recmp_play_test.py and the single-game checks -- holds slots of one machine budget while its games
play, so a run that would take the machine past the budget waits instead of sharing the CPU. The
games are not the only load: at a load average of 81 on 32 threads, with every run at about 40 %
of its speed, the games used about 9 cores and the rest was the wiki gate's offline checks (8 and
more), builds at -j 32 and work the lock cannot see (MEASURED 2026-11-13, 14:50). So every
CPU-heavy offline phase of the test tooling and every build a harness starts holds slots too, one a
worker it runs (OFFLINE WORK, BUILDS), and an admission leaves room for the load no run of the lock
accounts for (THE LOAD GUARD).

THE BUDGET is BUDGET slots, 28 of the reference setup's 32 threads (the owner's figure,
2026-11-12). A slot stands for one thread of load. A run declares the game lanes it plays at once
and the renderer they draw with, and takes the renderer's weight a lane (THE LANE WEIGHTS),
clamped to 1..BUDGET, and the GPU memory of each lane that renders on the GPU (THE GPU MEMORY); a
run that declares none takes the whole budget of both and so runs alone. games() is how many games
the run may play at once: its lanes, cut to the GPU memory there is, and THE GAME SEATS hold every
launch to it. Every tool declares its lanes: what else two runs could meet on is kept apart by
TAGS, for what a run makes for itself, and by RESOURCES, for what it is given.

THE LANE WEIGHTS are the slots one game lane takes, by the renderer its games draw with:
lane_slots(renderer), from this tree's tools/runlock-lanes.cfg, which says how each figure was
established. A run that takes no GPU memory draws with GDI unless it says otherwise (hold(...,
gpu=False), `runlock.py run --vram 0`); any other draws with Vulkan, and so does a run on llvmpipe,
which takes no GPU memory and says renderer="vulkan". Every conversion between lanes and slots goes
through lane_slots(): plan(), and so hold() and the slots every tool sizes its pieces by,
`runlock.py run --lanes`, and lanes_in(), the games a run of given slots may play. Its properties:
  * The budgets hold whatever the weights. The slots held never sum past BUDGET by the flocks alone
    (THE SLOTS), so a weight decides only how many lanes a run's slots admit. Two trees whose
    figures differ size their own runs differently and share the one budget.
  * A figure is the whole lane's measured cost rounded UP to a whole slot -- a GDI lane's 1.13 to
    1.54 threads takes two (MEASURED, tools/runlock-lanes.cfg) -- so the lanes a run's slots admit
    want no more threads than the slots stand for, and THE LOAD GUARD is left the load the lock does
    not see. Rounded down, the budget's own lanes would pass the machine's threads with no other
    work at all, and past them a lane's load grows with its wait for a CPU: 28 GDI lanes at one
    slot each took the 1-minute load to 61 on the reference setup's 32 threads, and 21 of them ran
    1.5 to 1.7 runnable threads a lane against 0.8 to 0.9 of CPU (MEASURED 2026-11-13).
  * Nothing a run concludes depends on it, as nothing depends on the guard. The tools that play GDI
    lanes judge in game ticks; their wall-clock bounds -- tareplay's ACK_S and STALL_S, a launch's,
    handicap_check's roster wait -- end a game as one that could not run, and the one bound that
    fails a check, netroom_check's LABEL_S, gives a name crossing DirectPlay on one machine 60 s.

OFFLINE WORK. A phase that plays no game and keeps the CPU busy -- the wiki gate's offline checks,
tareplay's offline pool (tarec's dumps and compares), the tasim checks and trace reads, predfit's
cross-validation -- holds one slot for each worker process or thread it runs at once, in the same
admission as games: offline() for a phase of its own, or hold(..., workers=W) for one that runs
beside the run's game lanes in one process. It asks for a range: at most W, at least the least it
can run on (one worker, or its heaviest task's weight). The queue's holder takes W slots when they
are free, else what is free and the load guard allows, never fewer than the least, so a phase that
arrives beside a running gate starts on what is left rather than waiting for the gate's end. Its
worker count is cut to the slots granted, as a run's game lanes are cut to games(): Workers.n, or
workers() inside a hold(). A run may move its own game slots to its workers once no game will play
on them again (Workers.widen; tareplay.py, THE LENDING): it holds them already, and the widened
grant never passes slots(). A phase whose tasks differ in weight -- a simparity shard
keeps three hosts busy -- runs each task on Workers.share(weight), a weighted semaphore over the
grant that admits tasks in the order they ask. Inside a run (NESTING) a phase takes no slot and is granted
min(W, the run's workers): the run's own slots. A process may hold a Workers beside its hold() --
the wiki gate takes its offline phase's first and then queues for its game lanes' -- because a
Workers, once held, waits on nothing: its tasks are leaves that end, and the process lets it go
when the last one has, whatever its other hold is waiting for (No deadlock, below).

BUILDS. `runlock.py make [-j N] -- ARGS` runs `make -jK ARGS` under an offline hold of at most N
slots and at least one, K the slots granted; inside a run it takes none and K is min(N, the run's
workers). Every build a harness starts goes through it, and so may a hand's. A build is short and
its speed-up flattens long before 32 jobs, so N defaults to BUILD_JOBS, 8.

THE HOST MEMORY. A slot also carries host memory: slot_mib(), the machine's memory (MemTotal) less
the lock's reserve_mib, shared by the BUDGET slots -- 1534 MiB on the reference setup's 63 437 MiB
less 20 GiB. The reserve is the lock's own runslots/mem.cfg, which tools/runlock-mem.cfg seeds as
the GPU figures are seeded (THE BUDGET BELONGS TO THE LOCK, NOT TO A TREE). The slots held never
sum past BUDGET (THE SLOTS), so the memory they carry never sums past MemTotal less the reserve, and
the runs keep inside it as long as each slot's work keeps inside its slot's memory. Its properties:
  * A job bound by construction. forked() runs each job of an offline phase in a process forked
    for it alone, on the slots that carry the memory its caller says it needs, and holds the
    process to them with RLIMIT_DATA (bind()): an allocation past them fails in that process --
    MemoryError, which forked() answers by running the job again on twice the slots, up to the
    phase's grant -- and never takes another run's memory or the reserve's. RLIMIT_DATA counts
    every private writable mapping of the process, resident or not and inherited from its parent
    or not, so the bound holds however the job grows; it leaves out shared memory, the stack and
    the page cache, which the kernel reclaims. refs build's trace reader runs on it
    (calib.lane_jobs, each lane sized by calib.lane_mib).
  * Work measured under a slot. A phase whose worker stays inside one slot's memory takes a slot
    a worker, unbound, as before. Each figure is MEASURED 2026-11-14 and is a figure, not a bound:
    tareplay's tarec processes, 1.2 GiB at most (`check` on the replay gate's largest recording,
    137 MiB); the wiki gate's offline checks, 1.0 GiB at most a process (a stagecheck shard;
    sidecheck 0.9, a simcheck or simparity shard 0.5 with simparity's hosts 0.2 each, textparity
    0.1); a GDI game lane, 0.35 GiB (the game, its X server and its driver) against its two slots'
    3 GiB, beside a run's own driver of up to 2.5 GiB. A phase that grows past its slot moves onto
    forked().
  * A forked job inherits its parent's mappings and its bound counts them, so its parent stays
    lean: devpy looks numpy up rather than loading it, since numpy's OpenBLAS reserves a buffer a
    thread of the machine as it loads (bind()).
  * The owner's processes go last. Every process the lock admits, and all it starts, is the
    kernel's first choice of victim (oom_score_adj OOM_SCORE_ADJ, oom_first()): should memory run
    out anyway, the kernel kills a run and not the desktop. That is the second line, not the
    bound.

THE LOAD GUARD. The slots bound only the work the lock sees. At admission the queue's holder reads
the 1-minute load average L (/proc/loadavg) and counts H, the slots other runs hold, each a thread
of load the lock accounts for. What it does not account for -- the owner's own work, a build run
outside the lock, a holder that runs more than its slots -- is L - H when that is positive, so the
machine's load once this run runs its R slots is about max(L, H) + R. The holder takes no more
slots than keep that at or under LIMIT, load_per_thread (tools/runlock-load.cfg) times the
machine's threads; a run that cannot have its least waits in the queue, keeping what it took, as it
does for a slot. Its properties:
  * No double count. A run just admitted is counted in H before its load reaches L, and a run whose
    games idle under its slots counts H, not more. Runs admitted in the same second are no
    exception: the slot queue admits one at a time and each takes its slots before it lets the
    queue go, so the next holder's H has them whatever L shows yet. A run that has just ended still
    shows in L for a minute or two while its slots are free (the 1-minute average decays with a
    time constant of a minute), so the guard errs late after a run ends: it delays an admission,
    never refuses one.
  * What it cannot see while L lags H. For the minute or so in which L has not yet caught up with
    runs just admitted, L - H is short of the work outside the lock, or below zero, so max(L, H)
    hides that work: an admission then can take the machine past LIMIT by up to the outside load.
    The slots do not depend on it (A scheduling rule, below), and the lane weights, rounded up,
    keep the lock's own lanes inside their slots.
  * Progress. When no other process holds a slot, the queue's holder is admitted whatever the load:
    the load is then none of the lock's, and waiting on it could wait forever. Every run behind the
    holder waits behind it, so the slots others hold are admitted runs', which end, and once they
    have the holder is admitted. The holds of the process itself and of its ancestors count in H for
    the load but not here, so a process queuing for its second hold never waits on its first, nor a
    child on the hold of the parent that waits for it. The guard therefore adds no
    deadlock and no starvation, and it never takes a slot back from a run that holds one.
  * A scheduling rule, not a correctness one. The slots never sum past BUDGET by the flocks alone,
    and what a run concludes never depends on the guard: it decides only when a run starts, and how
    many workers a flexible phase starts with. A load that cannot be read, or a load configuration
    that cannot be, leaves the guard out and the lock as it was without it.
  * The sim pool is not counted. L is read less the sim games playing, never below zero (THE SIM
    POOL): a game is one runnable thread of a process that yields every CPU to an engine game, so a
    sim batch never delays an engine run's admission. The count is of the game files held, each by
    the game's own process for exactly its life (THE YIELD), so it never passes the sim processes
    running: a place held between games, and a `run --sim` command's processes, are not discounted
    and count as load outside the lock. While L lags games just started, the discount hides up to
    that many threads of other load for a minute or so, as max(L, H) does for runs just admitted.

THE SIM POOL is a second budget, outside the BUDGET slots, for the coarse simulator's batches
(tools/impai/coarse): hundreds of games, each one process that keeps one thread busy, holds about
36 MB resident and takes no GPU (MEASURED 2026-11-20 on the reference setup: 34-36 MB resident 10 s
into a game). Inside the slots such a batch was granted what was free at its admission, 1 to 14
slots beside other sessions' engine runs, and each slot carried 1534 MiB for a 36 MB process. A
batch takes PLACES of the pool instead: sim_pool(tree, what, most, least), offline()'s drop-in, or
`runlock.py run --sim N`. Its figures are tools/runlock-sim.cfg's (floor, cap, pool_mib), read by
the tree that takes the place, each with how it was decided. Its properties:
  * The size. The pool holds threads - S places, threads the machine's (32 on the reference setup)
    and S the engine slots held at that moment, never under floor (8) nor over cap (24) (sim_size):
    24, 24, 16, 8, 8 places at S = 0, 8, 16, 24, 28. A batch is granted up to `most` of what the size
    at its admission leaves beside the places other batches hold, waits while it leaves fewer than
    its `least`, and keeps its places to its end; the games it starts on them follow the size as it
    is when each starts (THE YIELD), so places held can pass the size an engine run's arrival leaves
    and the games started after it cannot.
  * The places are flock(2) files, runslots/sim-NN, each held LOCK_EX by one batch as a slot is held
    by one run: a batch that dies frees its places with no cleanup step, and a place is counted by
    asking its own lock, so a stale holder is never counted. Only the holder of runslots/simq, the
    SIM QUEUE, takes a place; a batch takes it LOCK_EX, blocking in the kernel's queue, so batches
    are served in arrival order as the slot queue serves runs. Holding it, a batch counts every
    place file held, whatever its number, and takes no more than the size less that count: the
    places held never pass the size at the last admission that took one, and never the cap, by the
    files the cap names. The holder tries again every POLL_S while what is left is under its least,
    a liveness device as the slot queue's is: the bound is the flocks and the one holder.
  * Nothing of the slots. The pool takes no slot, GPU unit, resource or tareplay.lock and neither the
    slot queue nor the token queue, and an engine run takes no place: an engine run's admission
    changes only by the load the guard reads (THE LOAD GUARD). A batch waits only for places, held
    by batches, which end, so the pool adds no deadlock to the lock's.
  * Priority. Once admitted, every thread of the batch's process is put in the kernel's idle
    scheduling class (SCHED_IDLE, idle_first(); it needs no privilege), which every thread and
    process it starts afterwards inherits: an engine game wins every contest for a CPU against it.
    It is also the kernel's first choice of victim (oom_first), as every process the lock admits
    is. A process in the idle class takes no engine hold (IdleEngineRun): an unprivileged process
    cannot leave the class (MEASURED 2026-11-20 on the reference setup: a batch's process asking
    for SCHED_OTHER was refused, EPERM), so the engine games and workers it would start would lose
    every CPU to the runs they are counted beside.
  * Memory. The pool is charged pool_mib flat, not slots' worth, and it is held out of the lock's
    reserve (runslots/mem.cfg, THE HOST MEMORY): the slots carry what they carried, and what the
    reserve leaves the owner's programs is the reserve less pool_mib. Each process a batch starts
    for a place runs held to place_mib(), pool_mib // cap, by RLIMIT_DATA, set before its program
    starts (Workers.game() and Workers.argv(); `run --sim` binds its command and everything that
    starts), so a batch that runs one process a place keeps the pool's processes inside pool_mib.
    RLIMIT_DATA counts a
    process's private writable mappings, touched or not, not what is resident (bind()): a coarse
    game needs 128 MiB of it, which the owner's figures give a place, and a game under too small a
    bound exits 2 by name (tools/runlock-sim.cfg, MEASURED). The batch's own process,
    which took the places and starts its games -- the coarse driver, a thread and its stack a
    game -- is neither bound nor charged: one a batch.
  * No GPU: a sim batch plays no engine game, and `run --sim` takes no --vram, --suite or resource.
  * Nested. A batch inside an engine run (RUNLOCK_HELD, or a flock(1) chain) takes no place and is
    granted min(most, the run's workers), as offline() is, at the run's priority. A batch inside a
    sim batch (RUNLOCK_SIM, which Workers.environ() gives a batch's processes) takes no place and is
    granted min(most, that batch's places). Neither yields (THE YIELD): its game() waits for nothing.
  * THE YIELD. A batch starts each game through Workers.game(), which waits until the batch has a
    place of its own that plays no game and is numbered below the pool's size as it is then -- the
    engine slots held read again, each asked of its own lock -- and starts the game's one process on
    that place: the process takes runslots/simgame-NN, NN the place's number, LOCK_EX before its
    program starts (BOUND_CODE) and holds it for its life, so the kernel lets it go when the game
    ends however it ends. So while an engine run holds its slots, a game starts only on a place
    numbered below the size it leaves, a place is one batch's (its flock) and a game file one
    process's (its flock): the games started after the engine run's admission, with those still
    playing on the places below the size, never number more than that size, across all batches,
    whatever each batch computes. A place at or above the size
    starts no game until the size grows past it again; the batch's other places go on. A game file
    held by a dead batch's game holds up the next game on that number until the game ends, so the
    bound counts it too. Its properties:
      - A throughput policy over the places' bound, not a correctness one. Games in play are never
        killed: a game started before an engine run's admission -- one whose start read the size
        before the run's last slot was taken -- plays to its end beside the run, about 10 s and up
        to about 65 s for a stalemate, and that is THE YIELD's residual.
      - No wait on a poll's timing. A game waiting for the size re-reads it every POLL_S, as the
        queues' holders do: that bounds how soon a batch widens again, never what it may start.
        Its batch says so every SAY_S.
      - No starvation, and no delay to an engine run. An engine run takes no place and no game
        file. A batch whose places are all at or above the size waits and resumes once the engine
        run ends; its games wait only on the engine slots, which end, and on game files, whose
        processes end.
      - `run --sim N` does not yield: its command is one process the lock cannot see into, and its
        processes hold no game file, so the load guard does not discount them either. Nor does a
        process a batch starts through argv() alone, outside game().
  * Runlock code of before the pool does not see it. No file of the pool is one that code takes or
    counts, so the two never meet on a lock; but that code's load guard counts a batch's processes
    in L as work outside the lock, so while a batch plays it can DELAY an engine run that code
    admits -- by up to the cap's 24 threads of load. That is a delay, never an over-admission: the
    guard decides only when a run starts, and the slots bound the rest. It ends once every tree that
    runs engine regressions has merged this.
  * THE RESIDUAL. The idle class decides which thread a CPU runs, not what the other hardware
    thread of the same core does. A sim process on the sibling thread of a core an engine game runs
    on shares that core's execution units and caches and slows the game. MEASURED 2026-11-20 on the
    reference setup, one run each: `tareplay.py run` of resource-locked, resource-plain,
    resource-refusals, stock-sweep-basic, battle and stock-limits, 28 slots, played 442 s alone,
    507 s beside a batch of the floor's 8 places (15 % longer) and about 677 s beside a batch of the
    cap's 24 admitted before it that kept all 24 playing (53 % longer); all 494 checks passed in
    each. The batch slowed too: 1.1 games a second on 24 places against 2.2 alone. THE YIELD brings
    a full batch down to the size an engine run's arrival leaves as its games end, so an engine run
    of every slot plays beside the floor's 8 games once the games in play at its admission have
    ended. The pool lowers what a batch costs an engine run and is not an invariant.

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
    queue's holder gets its units as those runs end. A run that queues holding a Workers of its own
    (OFFLINE WORK) holds slots that go when that phase's tasks end, which wait on no run, so they
    count among the slots of admitted runs; the load guard waits only on those (THE LOAD GUARD).
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
(inc/tagpu_vkcap.h). A refusal that the renderer does not plan for fails the game by name in every
suite, and in every tool that takes GPU memory with a hold of its own: on the lines its waits read
and on the game's whole log once the game has stopped, every launch and rotation included
(talog.cap_failure, talog.CapWatch). A cap that is too small shows as a failure and never changes
a result quietly. A command run under `run` is the exception: nothing reads its games for refusals
but the command itself. A run without GPU memory publishes no cap, and its games run uncapped. A run that wants
more waits in the token queue holding neither a slot nor a unit; a run that wants none never waits
for one and never reads the figures, so a runslots/vram.cfg that cannot be read refuses only the
runs that want GPU memory. A run that renders without the GPU (llvmpipe, GDI) takes none. A
machine with no device memory to read -- no NVIDIA or amdgpu card -- has no budget; one whose
NVIDIA driver is there but does not answer refuses the run rather than run it unbounded.

A MEASUREMENT'S CAP. RUNLOCK_CAP_MIB=N in a run's environment replaces its suite's cap and cap_mib
alike (cap_override): the run takes the memory of its games at N and publishes N, so its games run
at N, the budget counting them at N -- what `tools/vramproof.py watch --cap N` sets, so that a
suite's peak is measured under a cap above it rather than cut at the cap being measured, and no
tracked file changes for it. write_gpumem refuses a launch where it is set and the run's published
cap is another: a run admitted before it was set, whose games would be measured at the wrong cap.

THE GAME SEATS hold the games a run plays at once to games(), whatever tool plays them: every game
launched inside a run with GPU memory takes a seat, a flock(2) file runslots/seat-<tag>-NN with NN
under games(), held by the game's KEEPER (start_game(), A SEAT IS LET GO AFTER ITS GAME), and a
launch waits while every seat is held. A tool that starts more lanes than games() therefore waits
at the launch rather than puts a game on the GPU its run did not take memory for. Seats are named
by the run's tag, so their files are a bounded set. A game left running by a dead run holds its seat
against the next run of that tag, and its seat holds up every run that wants GPU memory until it
ends, since the dead run's units no longer count it (A DEAD RUN'S GAMES). A lane that plays two
games at once takes its second seat holding its first; lanes that need more seats together than
games() can wait on each other, so a tool counts a network game as two games. Games that wait on
each other -- a network game's peers, which meet in one battle room -- need their seats together:
in a run whose games() is under their count the last launch waits for a seat the others hold while
they wait for it in the lobby, which never ends. Every such tool asks together() before its first
launch and refuses the game by name when the run may not play them all at once.

A SEAT IS LET GO AFTER ITS GAME. A seat bounds the games on the GPU only if it is held for as long
as its game holds device memory, so it is let go by a process that outlives every process of the
game, never by the game's own exit:
  * Why not the game's own descriptor. Every process of a Wine game inherits the seat: the game,
    the prefix's wineserver, services.exe, explorer.exe, winedevice.exe, plugplay.exe, svchost.exe,
    rpcss.exe and dplaysvr.exe all held it (MEASURED 2026-11-13, the descriptors of each seat
    sampled every 15 s through a compatibility run). A dying process closes its descriptors in
    exit_files and runs each file's release, the GPU driver's included, in its deferred fputs
    (exit_task_work), before exit_notify makes it reapable [INFERRED from Linux's do_exit and fput
    as known, not re-read for this and not measured]; the order of the seat's release and the
    device's within that is not fixed, and whichever process of the prefix holds the seat last --
    the wineserver, which `wineserver -k` ends without waiting for its clients -- ends in no order
    with the game's release. In those samples three games had closed their descriptors, the seat
    held by the wineserver alone, while the game was not yet reaped.
  * THE KEEPER. start_game() runs the game under a keeper, a process of its own (keep()) that holds
    the seat and is the game's parent. It makes itself the child subreaper of everything below it
    (prctl PR_SET_CHILD_SUBREAPER), so a process of the game whose parent ends -- the wineserver,
    which forks itself off, and Wine's services -- becomes the keeper's child, not init's, and it
    reaps its children until waitpid(-1) answers ECHILD. Every process below the keeper that is not
    yet reaped is its child or the child of another such process, so ECHILD means every process the
    game started has been reaped, each after the releases of its files. Then it waits for the
    prefix's wineserver (`wineserver -w`), which covers a server this game did not start, and only
    then ends, which lets the seat go. The keeper is in a session of its own and outlives the
    launcher: tacli's launch ends at once, and a run killed outright leaves its keepers to their
    games (A DEAD RUN'S GAMES). The game holds the seat as well, so a keeper killed outright
    (SIGKILL, which it cannot pass on and which nothing here sends it) leaves the seat to the game's
    own descriptors. SIGTERM, SIGINT and SIGHUP are passed on to the game and SIGUSR1 kills it:
    KeptGame's terminate() and kill(). The holder line of a seat names the keeper's pid, live for as
    long as the seat is held.
  * THE GAP: the driver can free a game's memory after the release. Read from NVIDIA's kernel
    interface (the reference setup's 595.99.02, nv.c and nv-lock.h): the control device's release,
    `nvidia_ctl_close`, frees its clients (`rm_cleanup_file_private`) within the release; a GPU
    device's, `nvidia_close`, does so only when its wait for the open's completion
    (`wait_for_completion_interruptible`) and for the system power lock
    (`nv_down_read_interruptible`, a trylock loop that gives up on a pending signal) both succeed,
    and otherwise queues `nvidia_close_deferred` on its kernel thread `nv_queue` and returns. So the
    close is deferred only with a signal pending in the dying thread while the open has not
    completed (no memory was allocated through it yet) or while a system suspend or resume holds
    the power lock for writing. What `rm_cleanup_file_private` does inside the driver's core is in
    its binary, unread. The invariant the bound would need -- a game's device memory is free once
    its processes are reaped -- is therefore not established, and no driver interface says when a
    client's memory is free. margin_mib does not cover it: it is what the device holds for a game
    outside every process (13-16 MiB a game, tools/runlock-vram.cfg), not a whole game left behind.
    A game seated while a deferred close still holds its predecessor's memory can find the device
    full and takes the road a full device takes (the Vulkan backend note, *A pass refused its
    memory*). The gap is the driver's: it is stated here, not closed.

A DEAD RUN'S GAMES still hold device memory the dead run's units no longer count: a game's keeper
holds its seat, never its run's GPU units, and outlives the run in a session of its own, so a run
killed outright lets its units go while its games still play. So a seat is counted as its run's
only while that run lives: each run has an id (pid and start, in RUNLOCK_HELD and in its holder
line), its games' seats carry it, and a held seat is a live run's when the slot file its tag names
(sNN: slot NN, the lowest the run holds, for its whole hold) is held by the run of that id
(orphaned_seats). Any other held seat -- a dead run's game, or a seat whose line names no run --
is among the files the token queue's holder's budget does not count (_others): it takes no unit
while one is held, and waits, naming it, until the game's keeper lets it go. A seat of a live run
is counted by that run's units.

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
games, offline workers (`workers`), tag, run id, resources, lock path and the GPU cap it was
admitted under (none without GPU memory), which its children inherit; a process that finds it, or that has a flock(1) ancestor on
tareplay.lock (which holds every slot and no GPU memory, so no cap), takes no slots: the outer
run's declaration covers its children, and its games take the outer run's seats. A nested run that
asks for more games than its ancestor may play says so and runs under the ancestor's. A nested run
that wants its games on the GPU inside a run that took no GPU memory is refused (NotCarried): its
games would take no seat and carry no cap, on the GPU and counted nowhere. Under a flock(1) chain,
which no run plays beside, its games run uncapped as the chain's always have. A resource the outer
run did not name, a nested run takes without waiting -- waiting there could wait on the queue's
holder, which waits on the outer run's slots -- and Busy is raised when another run holds it. A
child started with an environment of its own carries RUNLOCK_HELD over: without it, it would queue
behind the run it belongs to and wait for that run's end.

HOLDERS. A run writes its line -- pid, slots, GPU memory, resources, start, what, directory, run id
-- into each slot, unit and resource file it takes, after taking it (its units once it has them
all), and into the token queue's or the slot queue's file while it waits there as the holder; a
sim batch writes its own into each place it takes and into the sim queue's file likewise, and a
sim game's process its own, with its batch's run id, into its game file; a
game's keeper writes its own, with its run's id, into its seat, naming its own pid, which lives as
long as the seat is held. `status` asks each file's own lock whether it is held and reads the line
only to name the holder;
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
import collections
import contextlib
import ctypes
import fcntl
import hashlib
import json
import math
import os
import re
import resource
import select
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

ENV = "RUNLOCK_HELD"
CAP_ENV = "RUNLOCK_CAP_MIB"     # a measurement's cap (A MEASUREMENT'S CAP)
KEEPER_ENV = "RUNLOCK_KEEPER"   # the game a keeper starts (A SEAT IS LET GO AFTER ITS GAME)
BUDGET = 28                     # slots (THE BUDGET)
RENDERERS = ("gdi", "vulkan")   # what a game lane draws with, each a weight (THE LANE WEIGHTS)
FLOCK_TAG = "fx"                # the tag under a flock(1) ancestor (TAGS)
MIN_GAMES = 2                   # the fewest games the GPU budget holds: one network game (THE GPU MEMORY)
GRAIN_MIB = 32                  # the GPU budget's unit, one flock(2) file (THE GPU MEMORY)
VRAM_CFG = Path(__file__).resolve().with_name("runlock-vram.cfg")
SUITES_CFG = Path(__file__).resolve().with_name("runlock-suites.cfg")
LOAD_CFG = Path(__file__).resolve().with_name("runlock-load.cfg")
LANES_CFG = Path(__file__).resolve().with_name("runlock-lanes.cfg")
MEM_CFG = Path(__file__).resolve().with_name("runlock-mem.cfg")
SIM_CFG = Path(__file__).resolve().with_name("runlock-sim.cfg")
SIM_ENV = "RUNLOCK_SIM"         # a sim batch's places, which its processes inherit (THE SIM POOL)
OOM_SCORE_ADJ = 1000            # the kernel's first choice of victim: every process the lock admits (THE HOST MEMORY)
BUILD_JOBS = 8                  # the slots `runlock.py make` asks for by default (BUILDS)
NVIDIA_S = 60                   # a hang bound on nvidia-smi, deciding nothing
POLL_S = 2                      # how often the run being admitted tries the slots again
SAY_S = 300                     # how often a waiting run says it is still waiting

_held = []                      # this process's hold: its slot and resource files, tareplay.lock
_outer = None                   # RUNLOCK_HELD before a nested hold took resources, for release()
_default_lock = None            # default_lock(), once asked
_said_differs = set()           # the locks whose figures this process has said its tree differs from


class Busy(RuntimeError):
    """A resource another run holds, asked for by a run nested in another (NESTING)."""


class NotCarried(RuntimeError):
    """A nested run that wants GPU memory inside a run that took none (NESTING)."""


class BadFigures(ValueError):
    """A VRAM configuration file that cannot be read as one, its path in the message."""


class BadLanes(ValueError):
    """A lane-weights file that cannot be read as one, its path in the message (THE LANE WEIGHTS)."""


class BadSim(ValueError):
    """A sim-pool configuration file that cannot be read as one, its path in the message (THE SIM
    POOL)."""


class IdleEngineRun(RuntimeError):
    """An engine hold asked for by a process in the idle scheduling class (THE SIM POOL)."""


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
    """The GPU files held that this run's budget of `budget` units does not count: a unit numbered
    at or past it (a run under another budget, THE BUDGET BELONGS TO THE LOCK), a token of runlock
    code of before the GPU budget was memory (vram-NN, TRANSITION) and a dead run's game's seat
    (A DEAD RUN'S GAMES)."""
    return ([p.name for k, p in _numbered(d, "gpu") if k >= budget and _held_now(p)]
            + [p.name for _k, p in _numbered(d, "vram") if _held_now(p)]
            + orphaned_seats(d))


RUN_RX = re.compile(r"\[run ([0-9]+-[0-9]+)\]")


def _run_of(line: str) -> "str | None":
    """The run id a holder's line carries (_line, game_seat), None for a line that names none."""
    m = RUN_RX.search(line)
    return m.group(1) if m else None


def orphaned_seats(d: Path) -> list:
    """The held seats whose run holds no slot (A DEAD RUN'S GAMES): a seat is its live run's only
    when the slot file its tag names (sNN: slot NN, the lowest a run holds) is held by the run the
    seat's line names. A seat whose line names no run -- one runlock code of before the run id
    wrote, or one being taken this moment, its line not yet written -- counts as a dead run's,
    which can only make a run wait more."""
    out = []
    for p in sorted(d.glob("seat-*")):
        if not _held_now(p):
            continue
        m = re.fullmatch(r"seat-s(\d+)-\d+", p.name)
        run = _run_of(_read(p))
        if m and run and int(m.group(1)) < BUDGET:
            slot = slot_file(d, int(m.group(1)))
            if _held_now(slot) and _run_of(_read(slot)) == run:
                continue
        out.append(p.name)
    return out


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


def _lock_cfg(shared: Path, seed: Path, read, what: str) -> dict:
    """The figures of the lock's own copy `shared` of a configuration, read by `read`, seeded from
    this tree's `seed` when it is absent -- whole, by a hard link of a finished file, so it is
    written once and never seen half-written -- and said once a process when this tree's figures
    differ from the lock's (THE BUDGET BELONGS TO THE LOCK, NOT TO A TREE). `what` names the budget
    in what is said."""
    if not shared.exists():
        figures = read(seed)                        # a file that is not whole seeds nothing
        tmp = shared.with_name(f".{shared.name}.{os.getpid()}.{threading.get_ident()}")
        with open(tmp, "w") as f:
            f.write(seed.read_text())
            f.flush()
            os.fsync(f.fileno())            # never an empty file under the name after a power loss
        try:
            os.link(tmp, shared)                    # written once: a second seeder finds it there
            say(f"seeded {shared} from {seed} ({figures}): the {what} of every run on this lock")
        except FileExistsError:
            pass
        finally:
            tmp.unlink()
    cfg = read(shared)
    try:
        mine = read(seed)
    except (OSError, ValueError) as e:
        mine = str(e)
    if mine != cfg and shared not in _said_differs:
        _said_differs.add(shared)
        say(f"this tree's {seed.name} ({mine}) is not the lock's {shared} ({cfg}): the runs on "
            f"this lock share the lock's, and this run uses it")
    return cfg


def vram_config(path: "Path | None" = None) -> dict:
    """The figures of the lock's runslots/vram.cfg (the lock of `path`, else default_lock()),
    seeded from this tree's tools/runlock-vram.cfg when it is absent; said once a process when this
    tree's figures differ from the lock's."""
    return _lock_cfg(shared_vram_cfg(path or default_lock()), VRAM_CFG, read_vram_cfg, "GPU budget")


MEM_KEYS = ("reserve_mib",)


def read_mem_cfg(path: Path) -> dict:
    """The figures of a host memory configuration file, MEM_KEYS, each a whole number of MiB; a file
    that lacks one, names one twice or one it does not know, is refused, as read_vram_cfg refuses."""
    cfg = {}
    for line in Path(path).read_text().splitlines():
        key, eq, value = line.partition("=")
        if eq and not line.lstrip().startswith("#"):
            key = key.strip()
            if key not in MEM_KEYS or key in cfg:
                raise BadFigures(f"{path}: {key} is not one of {', '.join(MEM_KEYS)}, or is named twice")
            try:
                cfg[key] = int(value)
            except ValueError:
                raise BadFigures(f"{path}: {key} = {value.strip()!r} is not a whole number of "
                                 f"MiB") from None
    missing = [k for k in MEM_KEYS if k not in cfg]
    if missing or any(v < 0 for v in cfg.values()):
        raise BadFigures(f"{path}: {', '.join(missing) or 'a negative figure'}: every one of "
                         f"{', '.join(MEM_KEYS)} is a figure of MiB")
    return cfg


def shared_mem_cfg(path: Path) -> Path:
    """The lock's own host memory configuration (THE HOST MEMORY)."""
    return slots_dir(path) / "mem.cfg"


def mem_config(path: "Path | None" = None) -> dict:
    """The figures of the lock's runslots/mem.cfg, seeded from this tree's tools/runlock-mem.cfg when
    it is absent, as vram_config's are."""
    return _lock_cfg(shared_mem_cfg(path or default_lock()), MEM_CFG, read_mem_cfg, "host memory budget")


def host_total_mib() -> "int | None":
    """The machine's memory, MiB (/proc/meminfo's MemTotal), None where it cannot be read."""
    try:
        with open("/proc/meminfo") as f:
            return next(int(ln.split()[1]) for ln in f if ln.startswith("MemTotal:")) >> 10
    except (OSError, StopIteration, ValueError, IndexError):
        return None


def slot_mib(path: "Path | None" = None) -> "int | None":
    """The host memory one slot carries, MiB (THE HOST MEMORY): the machine's memory less the lock's
    reserve_mib, shared by the BUDGET slots; None where the machine's memory cannot be read. A
    reserve that leaves no memory a slot is refused (BadFigures)."""
    total = host_total_mib()
    if total is None:
        return None
    reserve = mem_config(path)["reserve_mib"]
    per = (total - reserve) // BUDGET
    if per < 1:
        raise BadFigures(f"reserve_mib {reserve} leaves no memory for a slot of the machine's {total} MiB")
    return per


def slots_for(mib: int, per: int) -> int:
    """The slots that carry `mib` MiB when a slot carries `per` (THE HOST MEMORY): the least whole
    number, at least one."""
    return max(1, -(-int(mib) // per))


def oom_first() -> None:
    """This process, and every process it starts from now on, the kernel's first choice of victim
    when memory runs out (oom_score_adj OOM_SCORE_ADJ, which children inherit): never the owner's
    desktop. Raising it needs no privilege; where /proc cannot be written it is left as it is."""
    try:
        with open("/proc/self/oom_score_adj", "w") as f:
            f.write(str(OOM_SCORE_ADJ))
    except OSError:
        pass


def bind(mib: "int | None") -> None:
    """This process's private writable memory held to `mib` MiB by RLIMIT_DATA, soft and hard, so an
    allocation past it fails here -- Python raises MemoryError -- instead of taking the host's
    memory, and so does any process it starts; and its BLAS, OpenMP and MKL work on one thread
    (THE HOST MEMORY). None binds no memory.

    RLIMIT_DATA counts every private writable mapping, resident or not, and what a forked process
    inherits from its parent with it: so it bounds the process's anonymous memory however it grows,
    and a reservation never touched counts too. numpy's OpenBLAS reserves a buffer a thread of the
    machine when it loads -- 1 GiB on the reference setup's 32 threads, none of it resident
    (MEASURED 2026-11-14) -- so a bound process runs that work on one thread, as its slots stand
    for one thread each. Set here, it holds for a numpy loaded afterwards, never for one the parent
    had loaded before it forked: a bound job's parent loads none (forked)."""
    oom_first()
    for var in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ.setdefault(var, "1")
    if mib:
        cap = int(mib) << 20
        _soft, hard = resource.getrlimit(resource.RLIMIT_DATA)
        if hard != resource.RLIM_INFINITY:
            cap = min(cap, hard)
        resource.setrlimit(resource.RLIMIT_DATA, (cap, cap))


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


def cap_override() -> "int | None":
    """The cap RUNLOCK_CAP_MIB names for every game of a run admitted under it (A MEASUREMENT'S
    CAP), None when it is not set; BadFigures for a figure the DLL would not take."""
    value = os.environ.get(CAP_ENV)
    if value is None:
        return None
    try:
        cap = int(value)
    except ValueError:
        raise BadFigures(f"{CAP_ENV}={value!r} is not a whole number of MiB") from None
    if not CAP_MIN_MIB <= cap <= CAP_MAX_MIB:
        raise BadFigures(f"{CAP_ENV}={cap} is not a cap the DLL takes "
                         f"({CAP_MIN_MIB}..{CAP_MAX_MIB}): its games would run uncapped")
    return cap


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


def lane_weights() -> dict:
    """{renderer: the slots one game lane drawing with it takes}, from this tree's
    tools/runlock-lanes.cfg (THE LANE WEIGHTS); BadLanes for a file that misses a renderer of
    RENDERERS, names one twice or one it does not know, or gives a figure that is not a whole
    number of slots in 1..BUDGET."""
    keys = {f"lane_slots_{r}": r for r in RENDERERS}
    weights = {}
    for line in LANES_CFG.read_text().splitlines():
        key, eq, value = line.partition("=")
        if not eq or line.lstrip().startswith("#"):
            continue
        key = key.strip()
        if key not in keys or keys[key] in weights:
            raise BadLanes(f"{LANES_CFG}: {key} is not one of {', '.join(keys)}, or is named twice")
        try:
            weights[keys[key]] = int(value)
        except ValueError:
            raise BadLanes(f"{LANES_CFG}: {key} = {value.strip()!r} is not a whole number of "
                           f"slots") from None
        if not 1 <= weights[keys[key]] <= BUDGET:
            raise BadLanes(f"{LANES_CFG}: {key} = {weights[keys[key]]} is not a lane's slots "
                           f"(1..{BUDGET})")
    missing = [k for k, r in keys.items() if r not in weights]
    if missing:
        raise BadLanes(f"{LANES_CFG}: no {', '.join(missing)}: every renderer's lane has a weight")
    return weights


def renderer_of(gpu: bool, renderer: "str | None" = None) -> str:
    """What a run's game lanes draw with (THE LANE WEIGHTS): `renderer` when given, else Vulkan for
    a run that takes GPU memory and GDI for one that takes none; ValueError for a renderer not in
    RENDERERS."""
    return _known(renderer or ("vulkan" if gpu else "gdi"))


def _known(renderer: str) -> str:
    if renderer not in RENDERERS:
        raise ValueError(f"runlock: renderer {renderer!r} is not one of {', '.join(RENDERERS)}")
    return renderer


def lane_slots(renderer: str) -> int:
    """The slots one game lane drawing with `renderer` takes (THE LANE WEIGHTS): every conversion
    between lanes and slots goes through this."""
    return lane_weights()[_known(renderer)]


def lanes_in(slots: int, renderer: str) -> int:
    """The game lanes of `renderer` that `slots` slots hold, at least one."""
    return max(1, slots // lane_slots(renderer))


def plan(lanes: "int | None", gpu: bool = True, path: "Path | None" = None,
         cfg: "dict | None" = None, suite: "str | None" = None,
         reserve: int = 0, renderer: "str | None" = None) -> "tuple[int, int, int, int | None, int | None]":
    """(slots, GPU units, games, GPU budget in units, cap) for a run of `lanes` game lanes at once
    on the lock of `path` (of the figures `cfg` when given), its games on the GPU under `suite`'s cap
    (None: cap_mib), or the cap_override() when one is set, unless `gpu` is False; None lanes, the
    whole of both budgets. Each lane takes the weight of `renderer` (None: renderer_of(gpu); THE
    LANE WEIGHTS). The games leave `reserve` slots of the budget to the run's offline workers
    (OFFLINE WORK)."""
    per = lane_slots(renderer_of(gpu, renderer))
    budget = gpu_budget(path, cfg) if gpu else None
    most_lanes = max(1, (BUDGET - reserve) // per)
    if not budget:
        games = most_lanes if lanes is None else min(max(1, lanes), most_lanes)
        return (BUDGET if lanes is None else clamp(games * per)), 0, games, budget, None
    cfg = cfg or vram_config(path)
    cap = cap_override() or (suite_cap(suite) if suite else cfg["cap_mib"])
    most = gpu_games(budget, cfg, cap)
    if most < 1:
        raise RuntimeError(f"a game of {suite or 'a run that names no suite'} under a cap of {cap} MiB "
                           f"takes {game_mib(cfg, cap)} MiB, more than the whole GPU budget "
                           f"({budget * GRAIN_MIB} MiB): no run of it is admitted")
    if lanes is None:
        return BUDGET, budget, (min(most, most_lanes) if reserve else most), budget, cap
    games = min(max(1, lanes), most_lanes, most)
    return clamp(games * per), gpu_units(games, cfg, cap), games, budget, cap


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


def _line(slots: int, what: str, resources=(), mib=0, run: "str | None" = None,
          least: "int | None" = None) -> str:
    """A holder's line: the run, and what it asks for in all -- `least` to `slots` slots for an
    offline phase (OFFLINE WORK); what it holds of that at a moment is the count of files bearing
    the line (report) -- and the run's id, which its games' seats carry too (orphaned_seats)."""
    asked = f"{least} to {slots}" if least is not None and least != slots else f"{slots}"
    return (f"pid {os.getpid()}, a run of {asked} slot{'s' if slots != 1 else ''}"
            + (f" and {mib} MiB of the GPU" if mib else "")
            + (f" and {', '.join(resources)}" if resources else "")
            + f", since {time.strftime('%Y-%m-%d %H:%M:%S')}, {what}, from {Path.cwd()}"
            + (f" [run {run}]" if run else "") + "\n")


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
    orphans = set(orphaned_seats(d))
    if held or other or old or orphans or budget:
        lines.append(f"GPU memory: {held * GRAIN_MIB} MiB held"
                     + (f" of {budget * GRAIN_MIB}, {max(budget - held, 0) * GRAIN_MIB} free"
                        if budget else "")
                     + (f"; {sum(other.values()) * GRAIN_MIB} MiB more held under another budget"
                        if other else "")
                     + (f"; {sum(old.values())} GPU tokens of older runlock code held" if old else "")
                     + (f"; {len(orphans)} game{'s' if len(orphans) != 1 else ''} of no live run "
                        f"on the GPU" if orphans else "")
                     + ("; no run takes GPU memory until they end" if other or old or orphans
                        else ""))
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
            seats.setdefault(run, []).append(_named(line) + (" [OF NO LIVE RUN: it holds up every "
                                                             "run that wants GPU memory]"
                                                             if p.name in orphans else ""))
    for run, held_by in seats.items():
        lines.append(f"{len(held_by):2} game seat{'s' if len(held_by) != 1 else ''} of run {run} held: "
                     + "; ".join(held_by))
    for p in sorted(d.glob("res-*")):
        if _held_now(p):
            lines.append(f"resource {p.name[4:]} held by "
                         f"{_named(_read(p) or 'a run that has not written its line yet')}")
    return free, lines


LOAD_KEYS = ("load_per_thread",)
_said_load = set()              # the load configurations this process has said it cannot read


def load_limit() -> "float | None":
    """THE LOAD GUARD's LIMIT: load_per_thread of tools/runlock-load.cfg times the machine's
    threads; None, said once, when the file cannot be read as one, which leaves the guard out."""
    try:
        cfg = {}
        for line in LOAD_CFG.read_text().splitlines():
            key, eq, value = line.partition("=")
            if eq and not line.lstrip().startswith("#"):
                if key.strip() not in LOAD_KEYS or key.strip() in cfg:
                    raise ValueError(f"{key.strip()} is not one of {', '.join(LOAD_KEYS)}, or is named twice")
                cfg[key.strip()] = float(value)
        if set(cfg) != set(LOAD_KEYS) or not cfg["load_per_thread"] > 0:
            raise ValueError(f"load_per_thread is a positive figure, and the file names {sorted(cfg)}")
    except (OSError, ValueError) as e:
        if str(LOAD_CFG) not in _said_load:
            _said_load.add(str(LOAD_CFG))
            say(f"{LOAD_CFG} cannot be read ({e}): the load guard is left out, and runs are "
                f"admitted on their slots alone")
        return None
    return cfg["load_per_thread"] * (os.cpu_count() or 1)


def loadavg() -> "float | None":
    """The machine's 1-minute load average, None where /proc/loadavg cannot be read."""
    try:
        return float(Path("/proc/loadavg").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def _lineage() -> set:
    """This process's pid and its ancestors'."""
    pids, pid = {os.getpid()}, os.getppid()
    while pid > 1 and pid not in pids:
        pids.add(pid)
        try:
            stat = Path(f"/proc/{pid}/stat").read_text()
        except OSError:
            break
        pid = int(stat.rsplit(")", 1)[1].split()[1])      # the field after the command is the ppid
    return pids


def _guard(d: Path, mine) -> "tuple[int | None, str]":
    """THE LOAD GUARD for the queue's holder, which holds the slots `mine` (their numbers): the most
    slots it may hold once admitted, None for no bound past its own, and why, for its waiting line.
    The slots held by other runs are counted as load the lock accounts for; when none of them is
    held by a process other than this one or one of its ancestors, there is no bound (Progress): a
    holder that waits for this process's end must never be what this process waits for."""
    held, others, own = 0, False, _lineage()
    for k in range(BUDGET):
        p = slot_file(d, k)
        if k in mine or not p.exists() or not _held_now(p):
            continue
        held += 1
        m = re.match(r"pid (\d+),", _read(p))
        others = others or not (m and int(m.group(1)) in own)
    if not others:
        return None, "no other process holds a slot"
    limit, load = load_limit(), loadavg()
    if limit is None or load is None:
        return None, "no load guard"
    # the sim games playing come off the load: their processes yield every CPU to an engine game
    # (THE SIM POOL). The games, not the places: a game file is held by its game's process for its
    # life alone (THE YIELD), so the discount never passes the sim processes running; and never
    # below zero, so a pool never adds room for an engine run
    playing = games_playing(d)
    seen = max(load - playing, 0.0)
    room = math.floor(limit - max(seen, held))
    return room, (f"the 1-minute load {load:.1f}"
                  + (f" less the {playing} sim game{'s' if playing != 1 else ''} playing, {seen:.1f},"
                     if playing else "")
                  + f" beside {held} slots held by other runs leaves room for "
                    f"{max(room, 0)} under the load guard's {limit:g}")


def _wait_for_queue(queue, path: Path, wanted: str, budget: "int | None", pool: bool = False) -> None:
    """A queue taken, blocking: the kernel queues the waiters. While it waits, a thread says so,
    with `wanted` and the holders out of a GPU budget of `budget` units -- or, for the sim queue
    (`pool`), the sim pool's lines -- every SAY_S."""
    try:
        fcntl.flock(queue, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return
    except BlockingIOError:
        pass
    stop = threading.Event()

    def remind():
        t0 = time.monotonic()
        while True:
            if pool:
                lines, ahead = sim_report(path), "another batch ahead in the sim queue"
            else:
                free, lines = report(path, budget)
                ahead = f"{free} slots free, another run ahead in the queue"
            say(f"waiting for the run lock ({path.parent / 'runslots'}): {wanted}, {ahead}; "
                f"{(time.monotonic() - t0) / 60:.0f} min so far" + "".join(f"\n  {ln}" for ln in lines))
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


def _take(path: Path, what: str, resources: list, sizes,
          gpu: bool) -> "tuple[list, str, str, tuple]":
    """A GPU run's memory in the token queue, then every run's slots, resources and tareplay.lock
    shared in the slot queue (ADMISSION IS IN ARRIVAL ORDER, IN TWO QUEUES): the open files that
    hold them, the run's tag, its id (pid and start, unique on the machine) and (slots, units,
    games, budget, cap), the slots those it was granted. `sizes(figures)` gives (least, most,
    units, games, budget, cap): the run takes `most` slots when they are free, else what is free
    and the load guard allows, never fewer than `least` (OFFLINE WORK, THE LOAD GUARD).
    A GPU run reads the lock's figures once it holds the token queue, so its budget and its cap are
    those of the moment it takes its memory; any other run reads none (figures None).
    IdleEngineRun in a process of the idle scheduling class, before any wait (THE SIM POOL)."""
    if idle():
        raise IdleEngineRun(f"{what}: this process runs in the idle scheduling class, a sim batch's "
                            f"(THE SIM POOL), which it cannot leave, so the engine games and workers "
                            f"it would start would lose every CPU to the runs counted beside them: an "
                            f"engine run is started outside a sim batch")
    d = slots_dir(path)
    t0 = time.monotonic()
    run = f"{os.getpid()}-{time.time_ns()}"
    cfg, units_, taken, named, shared, tokenq, queue = None, [], {}, {}, None, None, None
    try:
        if gpu and device_total_mib() is not None:
            tokenq = open(d / "tokenq", "a+")
            _wait_for_queue(tokenq, path, "GPU memory wanted", None)
            cfg = vram_config(path)
        least, most, units, games_, budget, cap = sizes(cfg)
        wanted = f"{most} slots" if least == most else f"{least} to {most} slots"
        line = _line(most, what, resources, units * GRAIN_MIB, run, least)
        if tokenq is not None:
            if units:
                _write(tokenq, line)
                units_ = _all_units(path, units, budget, line, t0)
            tokenq.close()
            tokenq = None
        queue = open(d / "queue", "a+")
        _wait_for_queue(queue, path, f"{wanted} wanted"
                        + (f", this run's {units * GRAIN_MIB} MiB of the GPU held" if units else ""),
                        budget)
        _write(queue, line)
        said = None
        while True:
            room, why = _guard(d, taken)
            upto = most if room is None else min(most, max(room, 0))
            if upto >= least:
                for k in range(BUDGET):
                    if len(taken) >= upto:
                        break
                    if k not in taken:
                        f = _try(slot_file(d, k), line)
                        if f is not None:
                            taken[k] = f
                # the guard's bound lowered since a slot was taken: the highest go, the tag stays
                for k in sorted(taken, reverse=True)[:max(len(taken) - upto, 0)]:
                    taken.pop(k).close()
            for r in resources:
                if r not in named:
                    f = _try(resource_file(d, r), line)
                    if f is not None:
                        named[r] = f
            if least <= len(taken) <= upto and len(named) == len(resources):
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
                missing = [r for r in resources if r not in named]
                say(f"waiting for the run lock ({d}): {len(taken)} of the {wanted} wanted taken, "
                    f"{free} free" + (f"; this run's {units * GRAIN_MIB} MiB of the GPU held"
                                      if units else "")
                    + (f"; waiting for {', '.join(missing)}" if missing else "")
                    + (f"; {why}" if room is not None and upto < least else "")
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
    slots = len(taken)
    oom_first()                 # admitted: this process and all it starts go before the owner's (THE HOST MEMORY)
    waited = time.monotonic() - t0
    if waited >= 1 or slots < most:
        cut = "" if slots == most else (f"; {why}" if upto < most else "; the rest are held by other runs")
        say(f"took the run lock ({slots} slots" + (f" of the {most} asked" if slots < most else "")
            + (f", {units * GRAIN_MIB} MiB of the GPU" if units else "")
            + f") after {waited / 60:.1f} min{cut}")
    return ([shared, *taken.values(), *units_, *named.values()], slot_tag(min(taken)), run,
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


def _hold(path: Path, what: str, wanted, sizes, resources=(), gpu: bool = True,
          workers_of=lambda slots, games_: 0, renderer: str = "vulkan") -> bool:
    """`sizes(cfg)` gives (least, most, GPU units, games, GPU budget, cap) of the lock's figures
    `cfg` (None: no GPU budget), asked only when this process takes them; a run with `gpu` reads the
    figures once it holds the token queue and publishes the cap of that read. `wanted` is the
    games asked for, None for all, said when an outer run's are fewer. `workers_of(slots, games)`
    is the offline workers a run granted `slots` publishes (OFFLINE WORK). `renderer` is what the
    run's lanes draw with, which sizes the games a flock(1) chain's slots hold (NESTING)."""
    global _held, _outer
    if _held:
        return False
    resources = sorted(set(resources))
    outer = _outer_hold(path, renderer)
    if outer is not None:
        if (gpu and "path" in outer and not outer.get("gpu_mib") and not outer.get("tokens")
                and device_total_mib() is not None):
            # its games would take no seat and no cap: on the GPU, uncounted
            raise NotCarried(f"{what} wants its games on the GPU inside a run that took no GPU "
                             f"memory ({outer['tag']}): they would run uncapped and uncounted; "
                             f"declare the GPU lanes in the outer run, or render without the GPU")
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
    _held, tag_, run, (slots, units, games_, budget, cap) = _take(path, what, resources, sizes, gpu)
    _outer = None
    # a game of a run with GPU memory runs under the cap that memory was sized on; any other,
    # uncapped
    _publish(path, {"slots": slots, "gpu_mib": units * GRAIN_MIB, "games": games_,
                    "workers": workers_of(slots, games_), "tag": tag_, "run": run,
                    "lane_slots": lane_slots(renderer),
                    "cap_mib": cap if units else None, "resources": resources})
    return True


def _outer_hold(path: Path, renderer: str) -> "dict | None":
    """The hold an ancestor has on `path` (NESTING): RUNLOCK_HELD's, or a flock(1) chain's, which
    holds every slot and no GPU memory, and so the games of `renderer` they hold; None when there is
    none."""
    outer = _env_held(path)
    if outer is None and _ancestor_holds(path):
        outer = {"slots": BUDGET, "gpu_mib": 0, "games": lanes_in(BUDGET, renderer), "workers": BUDGET,
                 "lane_slots": lane_slots(renderer), "tag": FLOCK_TAG, "resources": [], "cap_mib": None}
    return outer


def hold(tree: Path, what: str, lanes: "int | None" = None, resources=(), gpu: bool = True,
         suite: "str | None" = None, workers: int = 0, renderer: "str | None" = None) -> bool:
    """Hold the slots and GPU memory of `lanes` game lanes at once -- the whole of both with None,
    each lane the weight of `renderer` (None: renderer_of(gpu); THE LANE WEIGHTS), no GPU memory
    when `gpu` is False, each game's under `suite`'s cap (tools/runlock-suites.cfg; None, the play
    defaults' cap_mib) -- and each resource of `resources` until release() or the
    end of this process, waiting in the queue for them; games() then says how many games the run
    may play at once. `workers` asks, beside the lanes, for a slot for each of that many offline
    workers the run runs while its games play: at least one, at most `workers`, as many as are free
    (OFFLINE WORK); workers() then says how many it may run. True when this call took any; False
    when this process holds them already, directly or through RUNLOCK_HELD or a `flock(1)`
    ancestor. Inside another run it takes only the resources that run did not name, without
    waiting, and raises Busy for one another run holds."""
    path = lock_path(tree)
    if suite is not None:
        suite_cap(suite)                    # a suite the file does not name refuses before the wait
    cap_override()                          # so does a measurement's cap the DLL would not take
    renderer = renderer_of(gpu, renderer)
    per = lane_slots(renderer)              # and lane weights that cannot be read
    workers = max(0, workers)
    whole = lanes is None

    def sizes(cfg):
        # a run that wants GPU memory on a machine with none to bound still draws with its renderer
        slots, units, games_, budget, cap = plan(lanes, cfg is not None, path, cfg, suite,
                                                 reserve=min(workers, 1), renderer=renderer)
        if whole:
            return slots, slots, units, games_, budget, cap
        most = min(BUDGET, slots + workers)
        return min(most, slots + 1), most, units, games_, budget, cap

    def workers_of(slots, games_):
        # the slots past the game lanes' are the workers'
        if not workers:
            return 0
        return min(workers, slots - games_ * per) if whole else slots - clamp(games_ * per)
    return _hold(path, what, lanes, sizes, resources, gpu, workers_of, renderer)


def workers() -> int:
    """The offline workers the run this process is in may run at once (OFFLINE WORK): what its hold
    granted them beside its game lanes, or, in a run that declared none, its slots."""
    held = _env()
    if held is None:
        raise RuntimeError("runlock.workers() outside a run: hold() first")
    return held.get("workers") or held["slots"]


def slots() -> int:
    """The slots the run this process is in holds: its games' and its offline workers' together, or,
    in a run that declared no workers, its games' alone, which workers() then shares."""
    held = _env()
    if held is None:
        raise RuntimeError("runlock.slots() outside a run: hold() first")
    return held["slots"]


def game_slots() -> int:
    """The slots each game of the run this process is in takes: its renderer's lane weight (THE
    LANE WEIGHTS), as its hold recorded it."""
    held = _env()
    if held is None:
        raise RuntimeError("runlock.game_slots() outside a run: hold() first")
    return held.get("lane_slots") or lane_slots("vulkan")


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
    memory and without this line would hold memory the budget does not count. Returns the cap.
    RuntimeError, and the store untouched, when a measurement's cap is set and the run's is another
    (A MEASUREMENT'S CAP)."""
    cap = held_cap_mib()
    want = cap_override()
    if want is not None and cap is not None and cap != want:
        raise RuntimeError(f"{CAP_ENV}={want}, but this run was admitted under a cap of {cap} "
                           f"MiB (set after its hold, or inherited from an outer run): its game "
                           f"would be measured at {cap}, so it is not launched")
    path = Path(gamedir) / "impure.cfg"
    lines = path.read_text().splitlines() if path.exists() else []
    keep = [ln for ln in lines if ln.partition("=")[0].strip().lower() != "gpumem"]
    if cap is not None:
        keep.append(f"gpumem={cap}")
    if keep != lines:
        path.write_text("\n".join(keep) + ("\n" if keep else ""))
    return cap


def game_seat(what: str):
    """A seat for one game of the run this process is in, an open file for start_game's keeper to
    hold (THE GAME SEATS). Waits while every seat of the run is held. None outside a run or in a run
    without GPU memory, whose games need none."""
    if not seated():
        return None
    held = _env()
    d = slots_dir(Path(held["path"]))
    n, tag_ = held["games"], held["tag"]
    line = _seat_line(os.getpid(), "a game being launched", what)
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


def slot_tag(k: int) -> str:
    """The tag of a run whose lowest held slot is `k` (TAGS)."""
    return f"s{k:02d}"


def tags_held_by(path: Path, pids) -> set:
    """The tags of the runs on the lock of `path` whose holder is one of `pids` (TAGS: each run's
    lowest held slot, the holder its line names)."""
    d, tags, seen = slots_dir(path), set(), set()
    for k in range(BUDGET):
        p = slot_file(d, k)
        if not p.exists() or not _held_now(p):
            continue
        m = re.match(r"pid (\d+),", _read(p))
        if m and int(m.group(1)) in pids and m.group(1) not in seen:
            seen.add(m.group(1))
            tags.add(slot_tag(k))
    return tags


def seats_of(pid: int, path: Path) -> set:
    """The tags of the runs on the lock of `path` whose seats process `pid` has open: a game's own
    process holds its seat beside its keeper (A SEAT IS LET GO AFTER ITS GAME)."""
    prefix = str(slots_dir(path).resolve() / "seat-")
    tags = set()
    try:
        fds = os.listdir(f"/proc/{pid}/fd")
    except OSError:
        return tags
    for fd in fds:
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        if target.startswith(prefix):
            tags.add(target[len(prefix):].rsplit("-", 1)[0])
    return tags


def seated() -> bool:
    """Whether a game launched by this process takes a seat: inside a run with GPU memory."""
    held = _env()
    return bool(held is not None and held.get("gpu_mib") and held.get("path"))


def _seat_line(pid: int, who: str, what: str) -> str:
    """A seat's holder line, carrying the run's id (orphaned_seats)."""
    run = (_env() or {}).get("run")
    return (f"pid {pid}, {who}: {what}, since {time.strftime('%Y-%m-%d %H:%M:%S')}"
            + (f" [run {run}]" if run else "") + "\n")


def start_game(argv, what: str, **popen):
    """`argv`, a game, started as every harness starts one: inside a run with GPU memory, under a
    keeper holding a seat of the run (A SEAT IS LET GO AFTER ITS GAME), as a KeptGame; anywhere
    else, where a game takes no seat, as the subprocess.Popen of `argv` itself. `popen` is what
    Popen takes (cwd, env, the streams, start_new_session). Waits for a seat like game_seat()."""
    seat = game_seat(what)
    if seat is None:
        return subprocess.Popen(argv, **popen)
    try:
        game = KeptGame(argv, seat, **popen)
        # the holder `status` names is the keeper, which holds the seat for as long as it is held
        _write(seat, _seat_line(game.pid, "a game's keeper", what))
        return game
    finally:
        seat.close()


class KeptGame:
    """A game started under its keeper (start_game), answering for the game as subprocess.Popen
    does: `pid` is the keeper's, the root of every process of the game; poll(), wait() and
    `returncode` are the game's own end, which the keeper says the moment it reaps the game, before
    the rest of the game's processes are gone and its seat with them; terminate() and kill() reach
    the game through the keeper. `keeper` is the keeper's Popen: it ends once the seat is let go."""

    def __init__(self, argv, seat, env=None, **popen):
        self._status = None
        self.args, self.returncode, self.game_pid = list(argv), None, None
        self._failed, self._said = None, b""
        r, w = os.pipe()
        own = {k: v for k, v in os.environ.items() if k != "WINEPREFIX"}
        # the game's command and environment travel in one variable, so the keeper's command line
        # names no TotalA.exe and its environment no prefix: no listing of a prefix's processes or
        # of the games counts the keeper as one
        own[KEEPER_ENV] = json.dumps({"argv": [str(a) for a in argv],
                                      "env": dict(os.environ if env is None else env)})
        try:
            self.keeper = subprocess.Popen(
                [sys.executable, "-I", str(Path(__file__).resolve()), "keep", str(seat.fileno()),
                 str(w)],
                env=own, pass_fds=(seat.fileno(), w), **popen)
        except BaseException:
            os.close(r)
            raise
        finally:
            os.close(w)
        self._status = r
        self.pid = self.keeper.pid
        while self.game_pid is None and self._failed is None and self._status is not None:
            self._hear(None)
        if self.game_pid is None:
            code = self.keeper.wait()
            raise OSError(f"{self.args[0]}: its keeper did not start it: "
                          f"{self._failed or f'the keeper ended with {code}'}")

    def _hear(self, timeout) -> bool:
        """What the keeper has said within `timeout` s (None: until it says something or ends);
        False when it said nothing."""
        if self._status is None:
            return False
        ready, _, _ = select.select([self._status], [], [], timeout)
        if not ready:
            return False
        data = os.read(self._status, 4096)
        if not data:
            self._close()
            if self.returncode is None and self.game_pid is not None:
                # the keeper ended without saying the game's end: it was killed, and its own end is
                # all there is to say
                self.returncode = self.keeper.wait()
            return False
        self._said += data
        while b"\n" in self._said:
            line, _, self._said = self._said.partition(b"\n")
            word, _, rest = line.decode(errors="replace").partition(" ")
            if word == "started":
                self.game_pid = int(rest)
            elif word == "failed":
                self._failed = rest
            elif word == "exit":
                self.returncode = int(rest)
                self._close()
        return True

    def _close(self):
        if self._status is not None:
            os.close(self._status)
            self._status = None

    def poll(self) -> "int | None":
        while self.returncode is None and self._hear(0):
            pass
        return self.returncode

    def wait(self, timeout=None) -> int:
        end = None if timeout is None else time.monotonic() + timeout
        while self.returncode is None:
            left = None if end is None else end - time.monotonic()
            if left is not None and left <= 0:
                raise subprocess.TimeoutExpired(self.args, timeout)
            self._hear(left)
        return self.returncode

    def terminate(self) -> None:
        """SIGTERM to the game, through its keeper."""
        if self.poll() is None:
            self.keeper.send_signal(signal.SIGTERM)

    def kill(self) -> None:
        """SIGKILL to the game, through its keeper (SIGUSR1 asks it): never to the keeper, whose end
        lets the seat go."""
        if self.poll() is None:
            self.keeper.send_signal(signal.SIGUSR1)

    def wait_released(self, timeout=None) -> int:
        """Until the keeper has let the seat go: every process of the game reaped and the prefix's
        wineserver gone. The keeper's exit code."""
        return self.keeper.wait(timeout)

    def __del__(self):
        self._close()


PR_SET_CHILD_SUBREAPER = 36             # <linux/prctl.h>


def keep(argv) -> int:
    """The keeper (A SEAT IS LET GO AFTER ITS GAME), `runlock.py keep SEAT_FD STATUS_FD` with the
    game in RUNLOCK_KEEPER: starts the game, says `started <pid>` on STATUS_FD and `exit <code>`
    once it has reaped the game, and holds SEAT_FD until every process below it has been reaped
    and the prefix's wineserver has exited. Exits with the game's code."""
    seat, status = int(argv[0]), int(argv[1])
    spec = json.loads(os.environ.pop(KEEPER_ENV))
    os.set_inheritable(status, False)

    def tell(text):
        try:
            os.write(status, f"{text}\n".encode())
        except OSError:
            pass                # the launcher has gone: the seat is kept all the same

    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        tell(f"failed prctl(PR_SET_CHILD_SUBREAPER): {os.strerror(ctypes.get_errno())}")
        return 127
    game = {"pidfd": None, "pending": []}

    def forward(signum, _frame):
        sig = signal.SIGKILL if signum == signal.SIGUSR1 else signum
        if game["pidfd"] is None:
            game["pending"].append(sig)
            return
        try:
            # through the pidfd: once the game is reaped it names no process, never a reused pid
            signal.pidfd_send_signal(game["pidfd"], sig)
        except ProcessLookupError:
            pass
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGUSR1):
        signal.signal(s, forward)
    try:
        proc = subprocess.Popen(spec["argv"], env=spec["env"], pass_fds=(seat,))
    except OSError as e:
        tell(f"failed {e}")
        return 127
    game["pidfd"] = os.pidfd_open(proc.pid)            # the game is not reaped yet: it is this pid
    for sig in game["pending"]:
        forward(sig, None)
    tell(f"started {proc.pid}")
    code = 0
    while True:
        try:
            pid, st = os.waitpid(-1, 0)
        except ChildProcessError:
            break               # no child left, so no process below this one in any state
        if pid == proc.pid:
            code = proc.returncode = os.waitstatus_to_exitcode(st)
            tell(f"exit {code}")
    if spec["env"].get("WINEPREFIX"):
        try:
            subprocess.run(["wineserver", "-w"], env=spec["env"], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            say(f"the keeper of {spec['argv'][0]}: no `wineserver -w` ({e}); every process the game "
                f"started has been reaped all the same")
    return code if code >= 0 else 128 - code


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
    if a.sim is not None:
        return _run_sim(a)
    # --vram 0 declares lanes without the GPU, which draw with GDI (THE LANE WEIGHTS)
    renderer = "gdi" if a.vram == 0 else "vulkan"
    per = lane_slots(renderer)
    slots = clamp(a.slots if a.lanes is None else a.lanes * per)
    path = lock_path(Path.cwd())
    lanes = a.vram if a.vram is not None else (a.lanes or -(-slots // per))

    def sizes(cfg):
        _s, units, games_, budget, cap = plan(lanes, cfg is not None, path, cfg, a.suite,
                                              renderer=renderer)
        return slots, slots, units, (games_ if units else lanes_in(slots, renderer)), budget, cap
    if a.suite is not None:
        suite_cap(a.suite)
    cap_override()
    try:
        # the command's offline work runs on its slots: it declared them all
        _hold(path, a.what, lanes, sizes, a.resource, gpu=a.vram != 0,
              workers_of=lambda granted, games_: granted, renderer=renderer)
    except (Busy, NotCarried) as e:
        say(f"run: {e}")
        return 2
    for fd in fds():
        os.set_inheritable(fd, True)
    try:
        os.execvp(a.cmd[0], a.cmd)
    except OSError as e:
        say(f"run: {a.cmd[0]}: {e}")
        return 127


def _run_sim(a) -> int:
    """`run --sim N`: the command on N places of the sim pool, N clamped to the cap, at idle
    priority and held to the pool's memory bound with everything it starts (THE SIM POOL); inside
    an engine run or a sim batch it takes none, as sim_pool() does. The command is one process the
    lock cannot see into, so it does not yield (THE YIELD) and its processes hold no game file."""
    if a.vram is not None or a.suite is not None or a.resource:
        say("run: --sim takes no --vram, --suite or --resource: a sim batch plays no engine game")
        return 2
    w = _sim_pool(lock_path(Path.cwd()), a.what, a.sim, a.sim)
    for fd in w.fds():
        os.set_inheritable(fd, True)
    if w.mib is not None:
        bind(w.mib)             # the command and all it starts, each process held to a place's memory
    try:
        os.execvpe(a.cmd[0], a.cmd, w.environ())
    except OSError as e:
        say(f"run: {a.cmd[0]}: {e}")
        return 127


class Workers:
    """An offline phase's hold (OFFLINE WORK), from offline(): `n`, the workers it may run at once;
    fds(), the descriptors a worker process inherits so that it holds the slots with this process
    until it exits (A HOLD THAT ENDS BEFORE THE PROCESS); environ(), a worker process's
    environment, carrying the hold in RUNLOCK_HELD so that a run started inside it takes none of
    its own (NESTING); share(weight), a task's turn on the grant; widen(k), the grant raised by
    slots the process holds already; release(), also on leaving a `with`. Inside another run it
    holds no file and its environment is the outer run's. A sim batch's (sim_pool) carries its
    places in RUNLOCK_SIM instead; game() is a game's turn on them, and argv(cmd) starts a process
    of it under the pool's memory bound, `mib` (THE SIM POOL, THE YIELD). `pool`, a batch's own
    places: the lock's directory `d`, the `floor` and `cap` its size is kept within, the places'
    numbers `places`, its `run` id and `what` it is."""

    def __init__(self, n: int, files=(), held: "dict | None" = None, var: str = ENV,
                 mib: "int | None" = None, pool: "dict | None" = None):
        self.n, self._files, self._held, self._var, self.mib = n, list(files), held, var, mib
        self._cv = threading.Condition()
        self._free, self._turns = n, collections.deque()
        self._pool, self._playing, self._game_turns = pool, set(), collections.deque()
        self._yielding = None           # when the batch last said its next game waits for the size; None while it does not

    def fds(self) -> tuple:
        return tuple(f.fileno() for f in self._files)

    def environ(self, base=None) -> dict:
        env = dict(os.environ if base is None else base)
        if self._held is not None:
            env[self._var] = json.dumps(self._held)
        return env

    def argv(self, cmd) -> list:
        """`cmd` as a process of this phase is started: in a sim batch, under a program that sets
        RLIMIT_DATA to `mib` MiB and then execs `cmd`, so the bound holds from the first allocation
        of `cmd`'s program and for all it starts (THE SIM POOL); elsewhere, `cmd` as it is. The
        process that calls this stays unbound."""
        cmd = [str(c) for c in cmd]
        if self.mib is None:
            return cmd
        return [sys.executable, "-I", "-c", BOUND_CODE, str(self.mib), "", "", *cmd]

    @contextlib.contextmanager
    def game(self):
        """One game of a sim batch, its turn held while the `with` runs (THE YIELD): it waits until
        this batch has a place that plays no game and is numbered below the pool's size now, and
        yields argv(cmd) for the game's one process, which holds that place's game file for its
        life and runs under the pool's memory bound. Games are let in in the order they ask, on the
        lowest such place. Outside a batch that holds places of its own -- inside an engine run, or
        nested in a batch -- it waits for nothing and yields argv."""
        if self._pool is None:
            yield self.argv
            return
        p = self._pool
        turn = object()
        with self._cv:
            self._game_turns.append(turn)
            try:
                k = self._place(turn)
            except BaseException:
                self._game_turns.remove(turn)
                self._cv.notify_all()
                raise
            self._game_turns.popleft()
            self._playing.add(k)
            self._cv.notify_all()
        line = f"a game of a sim batch on place {k}, {p['what']} [run {p['run']}]"
        try:
            yield lambda cmd: [sys.executable, "-I", "-c", BOUND_CODE, str(self.mib),
                               str(game_file(p["d"], k)), line, *[str(c) for c in cmd]]
        finally:
            with self._cv:
                self._playing.discard(k)
                self._cv.notify_all()

    def _place(self, turn) -> int:
        """The place game() starts its game on, once `turn` heads the games' queue and a place of
        this batch's plays no game and is numbered below the size; called holding the condition.
        The size is read again every POLL_S, a liveness device: what a game may start on is decided
        by the read, never by when it is made."""
        p = self._pool
        while True:
            if self._game_turns[0] is not turn:
                self._cv.wait()
                continue
            engine = engine_held(p["d"])
            size = sim_size(machine_threads(), engine, p["floor"], p["cap"])
            idle_places = [k for k in p["places"] if k not in self._playing]
            below = [k for k in idle_places if k < size]
            if below:
                if self._yielding is not None:
                    say(f"{p['what']}: the sim pool is {size} places now ({engine} engine slots held): "
                        f"this batch starts games on {sum(k < size for k in p['places'])} of its "
                        f"{len(p['places'])} places")
                    self._yielding = None
                return below[0]
            if idle_places:             # a place is free and only the size keeps the game back
                now = time.monotonic()
                if self._yielding is None or now - self._yielding >= SAY_S:
                    over = sum(k >= size for k in p["places"])
                    say(f"{p['what']}: the sim pool is {size} places now ({engine} engine slots held), "
                        f"so {over} of this batch's {len(p['places'])} places start no game until it "
                        f"grows; the games in play finish")
                    self._yielding = now
            self._cv.wait(POLL_S)

    @contextlib.contextmanager
    def share(self, weight: int = 1):
        """`weight` of the grant -- the processes or threads a task keeps busy, cut to `n` -- held
        while the `with` runs. Tasks are let in in the order they ask: a heavy task at the head
        waits for its weight, and the lighter ones behind it wait with it, so none is passed over
        for good."""
        w = min(max(1, weight), self.n)
        turn = object()
        with self._cv:
            self._turns.append(turn)
            try:
                self._cv.wait_for(lambda: self._turns[0] is turn and self._free >= w)
            except BaseException:
                self._turns.remove(turn)
                self._cv.notify_all()
                raise
            self._turns.popleft()
            self._free -= w
            self._cv.notify_all()
        try:
            yield w
        finally:
            with self._cv:
                self._free += w
                self._cv.notify_all()

    def widen(self, k: int) -> None:
        """`k` more workers at once, on slots this process already holds for something else that
        no longer needs them -- a run's game slots once no game will play on them (tareplay.py,
        THE LENDING). It takes nothing from the queue, so what it adds must be held already."""
        with self._cv:
            self.n += k
            self._free += k
            self._cv.notify_all()

    def release(self) -> None:
        """This process's descriptors of the slots closed: they go once no worker process given
        fds() still runs. Safe to call twice, and from two threads (the wiki gate's)."""
        files, self._files = self._files, []
        for f in files:
            f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


def offline(tree: Path, what: str, most: int, least: int = 1) -> Workers:
    """The slots of an offline phase of at most `most` workers at once and at least `least`, each
    clamped to 1..BUDGET, waited for in the queue like any run's (OFFLINE WORK): a Workers whose
    `n` is the workers granted. Inside a run it takes none and is granted min(`most`, the run's
    workers)."""
    return _offline(lock_path(tree), what, most, least)


def _offline(path: Path, what: str, most: int, least: int = 1) -> Workers:
    most = clamp(most)
    least = min(clamp(least), most)
    # a run nested in the phase takes no GPU memory (NotCarried), so its games draw with GDI
    outer = _outer_hold(path, "gdi")
    if outer is not None:
        return Workers(min(most, outer.get("workers") or outer["slots"]))
    files, tag_, run, (slots, *_rest) = _take(path, what, [],
                                              lambda cfg: (least, most, 0, 0, None, None), gpu=False)
    return Workers(slots, files, {"slots": slots, "gpu_mib": 0, "games": lanes_in(slots, "gdi"),
                                  "workers": slots, "tag": tag_, "run": run, "cap_mib": None,
                                  "resources": [], "path": str(path)})


# ---- THE SIM POOL --------------------------------------------------------------------------------

SIM_KEYS = ("floor", "cap", "pool_mib")
# Workers.argv's and Workers.game's program, `mib file line cmd...`: RLIMIT_DATA set, soft and hard,
# under any lower hard limit it inherits; for a game (`file` not empty), its game file taken LOCK_EX,
# waiting, its holder line written, and the descriptor made inheritable, so the game's own process
# holds it from before its program starts to its exit (THE YIELD); then the game's program exec'd in
# its place, so the bound is the game's from its first allocation
BOUND_CODE = ("import fcntl, os, resource, sys\n"
              "b = int(sys.argv[1]) << 20\n"
              "h = resource.getrlimit(resource.RLIMIT_DATA)[1]\n"
              "b = b if h == resource.RLIM_INFINITY else min(b, h)\n"
              "resource.setrlimit(resource.RLIMIT_DATA, (b, b))\n"
              "if sys.argv[2]:\n"
              "    f = open(sys.argv[2], 'a+')\n"
              "    fcntl.flock(f, fcntl.LOCK_EX)\n"
              "    f.seek(0)\n"
              "    f.truncate()\n"
              "    f.write('pid %d, %s\\n' % (os.getpid(), sys.argv[3]))\n"
              "    f.flush()\n"
              "    os.set_inheritable(f.fileno(), True)\n"
              "os.execvp(sys.argv[4], sys.argv[4:])\n")


def sim_config() -> dict:
    """This tree's sim-pool figures, tools/runlock-sim.cfg (THE SIM POOL): `floor` and `cap`, whole
    numbers of places with 1 <= floor <= cap, and `pool_mib`, a whole number of MiB, at least a MiB
    a place. BadSim, naming the file, for one that cannot be read, lacks a figure, names one twice
    or one it does not know, or breaks those rules: a batch is refused before it waits."""
    cfg = {}
    try:
        text = SIM_CFG.read_text()
    except OSError as e:
        raise BadSim(f"{SIM_CFG}: {e}") from None
    for line in text.splitlines():
        key, eq, value = line.partition("=")
        if eq and not line.lstrip().startswith("#"):
            key = key.strip()
            if key not in SIM_KEYS or key in cfg:
                raise BadSim(f"{SIM_CFG}: {key} is not one of {', '.join(SIM_KEYS)}, or is named twice")
            try:
                cfg[key] = int(value)
            except ValueError:
                raise BadSim(f"{SIM_CFG}: {key} = {value.strip()!r} is not a whole number") from None
    missing = [k for k in SIM_KEYS if k not in cfg]
    if missing:
        raise BadSim(f"{SIM_CFG}: no {', '.join(missing)}: the pool has every one of {', '.join(SIM_KEYS)}")
    if not 1 <= cfg["floor"] <= cfg["cap"]:
        raise BadSim(f"{SIM_CFG}: floor {cfg['floor']} and cap {cfg['cap']} are not places with "
                     f"1 <= floor <= cap")
    if cfg["pool_mib"] < cfg["cap"]:
        raise BadSim(f"{SIM_CFG}: pool_mib {cfg['pool_mib']} is less than a MiB for each of the "
                     f"cap's {cfg['cap']} places")
    return cfg


def machine_threads() -> int:
    """The machine's threads, which the sim pool's size is taken from (THE SIM POOL)."""
    return os.cpu_count() or 1


def sim_size(threads: int, engine: int, floor: int, cap: int) -> int:
    """The sim pool's places while `engine` of the BUDGET slots are held on a machine of `threads`
    threads: threads - engine, never under `floor` nor over `cap` (THE SIM POOL)."""
    return min(cap, max(floor, threads - engine))


def place_mib(cfg: dict) -> int:
    """The memory one process of a place is held to, MiB: the pool's, shared by the cap's places,
    so the cap's processes never hold more than pool_mib (THE SIM POOL)."""
    return cfg["pool_mib"] // cfg["cap"]


def place_file(d: Path, k: int) -> Path:
    return d / f"sim-{k:02d}"


def game_file(d: Path, k: int) -> Path:
    """The file the game playing on place `k` holds for its life (THE YIELD)."""
    return d / f"simgame-{k:02d}"


def games_playing(d: Path) -> int:
    """The sim games playing now, each game file asked of its own lock: never more than the sim
    processes running, since each is held by its game's own process alone (THE YIELD)."""
    return sum(1 for _k, p in _numbered(d, "simgame") if _held_now(p))


def engine_held(d: Path) -> int:
    """The BUDGET slots held now, each asked of its own lock: S of the sim pool's size."""
    return sum(1 for k in range(BUDGET) if slot_file(d, k).exists() and _held_now(slot_file(d, k)))


def places_held(d: Path, mine=()) -> int:
    """The sim-pool places held now but those numbered in `mine`, each asked of its own lock, every
    place file counted whatever its number: a tree whose cap is larger names more of them."""
    return sum(1 for k, p in _numbered(d, "sim") if k not in mine and _held_now(p))


def idle() -> bool:
    """Whether this thread runs in the kernel's idle scheduling class."""
    return hasattr(os, "SCHED_IDLE") and os.sched_getscheduler(0) == os.SCHED_IDLE


def idle_first() -> None:
    """Every thread of this process put in the kernel's idle scheduling class (SCHED_IDLE), which
    every thread and process each of them starts afterwards inherits (THE SIM POOL). OSError when
    the kernel refuses it.

    A pass sets each listed thread that is not idle; the passes end with one that finds every
    listed thread idle. Every thread then is idle by construction: one created after that pass's
    listing was created by a thread that was idle by then -- a thread made idle in an earlier pass
    became so before the listing, and one the listing found idle was never otherwise -- and a new
    thread takes its creator's class."""
    while True:
        changed = False
        for tid in os.listdir("/proc/self/task"):
            try:
                if os.sched_getscheduler(int(tid)) != os.SCHED_IDLE:
                    os.sched_setscheduler(int(tid), os.SCHED_IDLE, os.sched_param(0))
                    changed = True
            except ProcessLookupError:
                pass                    # the thread has ended since the listing
        if not changed:
            return


def _env_sim(path: Path) -> "dict | None":
    """The sim batch RUNLOCK_SIM says an ancestor holds on `path`, None when it names none there."""
    try:
        held = json.loads(os.environ.get(SIM_ENV, ""))
    except ValueError:
        return None
    return held if isinstance(held, dict) and held.get("path") == str(path) and "places" in held else None


def _sim_line(most: int, least: int, what: str, run: str) -> str:
    """A sim batch's holder line, in the shape of a run's (_line): its pid first, which `status` marks
    stale once it is gone."""
    asked = f"{least} to {most}" if least != most else f"{most}"
    return (f"pid {os.getpid()}, a sim batch of {asked} place{'s' if most != 1 else ''}, since "
            f"{time.strftime('%Y-%m-%d %H:%M:%S')}, {what}, from {Path.cwd()} [run {run}]\n")


def _take_places(path: Path, what: str, most: int, least: int, cfg: dict) -> "tuple[dict, str]":
    """`least` to `most` places of the sim pool, taken in the sim queue (THE SIM POOL): their open
    files by number and the batch's run id. Only the sim queue's holder takes a place, and it takes
    no more than the size less every place held when it counts them, so the places held never pass
    that size; it touches no slot and neither of the slot lock's queues."""
    d = slots_dir(path)
    t0 = time.monotonic()
    run = f"{os.getpid()}-{time.time_ns()}"
    line = _sim_line(most, least, what, run)
    wanted = f"{most} places of the sim pool" if least == most else f"{least} to {most} places of the sim pool"
    taken, simq = {}, None
    try:
        simq = open(d / "simq", "a+")
        _wait_for_queue(simq, path, f"{wanted} wanted", None, pool=True)
        _write(simq, line)
        said = None
        while True:
            size = sim_size(machine_threads(), engine_held(d), cfg["floor"], cfg["cap"])
            upto = min(most, size - places_held(d, taken))
            if upto >= least:
                for k in range(cfg["cap"]):
                    if len(taken) >= upto:
                        break
                    if k not in taken:
                        f = _try(place_file(d, k), line)
                        if f is not None:
                            taken[k] = f
            # the size fell since a place was taken (an engine run arrived): the highest go
            for k in sorted(taken, reverse=True)[:max(len(taken) - max(upto, 0), 0)]:
                taken.pop(k).close()
            if least <= len(taken):
                break
            waited = time.monotonic() - t0
            if said is None or waited - said >= SAY_S:
                say(f"waiting for the sim pool ({d}): {wanted}, {max(upto, 0)} to be had now; "
                    f"{waited / 60:.0f} min so far" + "".join(f"\n  {ln}" for ln in sim_report(path)))
                said = waited
            time.sleep(POLL_S)
    except BaseException:
        for f in taken.values():
            f.close()
        raise
    finally:
        if simq is not None:
            simq.close()
    waited = time.monotonic() - t0
    if waited >= 1 or len(taken) < most:
        say(f"took {len(taken)} place{'s' if len(taken) != 1 else ''} of the sim pool"
            + (f" of the {most} asked" if len(taken) < most else "") + f" after {waited / 60:.1f} min")
    return taken, run


def sim_pool(tree: Path, what: str, most: int, least: int = 1) -> Workers:
    """The places of a sim batch of at most `most` processes at once and at least `least`, each
    clamped to 1..cap, waited for in the sim queue (THE SIM POOL): offline()'s drop-in, a Workers
    whose `n` is the places granted. Once admitted, this process and all it starts run at idle
    priority and go first when memory runs out; each game is started inside the Workers' game(),
    which holds it back while the pool's size leaves the batch no place for it (THE YIELD), and runs
    under the pool's memory bound. Inside an engine run it takes no place and is granted
    min(`most`, the run's workers), as offline() is; inside a sim batch, min(`most`, that batch's
    places)."""
    return _sim_pool(lock_path(tree), what, most, least)


def _sim_pool(path: Path, what: str, most: int, least: int = 1) -> Workers:
    cfg = sim_config()                  # a configuration that cannot be read refuses before the wait
    most = min(max(1, most), cfg["cap"])
    least = min(max(1, least), most)
    inner = _env_sim(path)
    if inner is not None:
        return Workers(min(most, inner["places"]), held=inner, var=SIM_ENV, mib=inner.get("place_mib"))
    outer = _outer_hold(path, "gdi")
    if outer is not None:
        return Workers(min(most, outer.get("workers") or outer["slots"]))
    taken, run = _take_places(path, what, most, least, cfg)
    try:
        idle_first()
    except OSError as e:
        for f in taken.values():
            f.close()
        raise RuntimeError(f"{what}: the kernel refused the idle scheduling class ({e}): a batch at "
                           f"normal priority would contest the engine games' CPUs, so it is not "
                           f"run") from None
    oom_first()
    mib = place_mib(cfg)
    return Workers(len(taken), taken.values(), {"places": len(taken), "place_mib": mib, "run": run,
                                                "path": str(path)}, var=SIM_ENV, mib=mib,
                   pool={"d": slots_dir(path), "floor": cfg["floor"], "cap": cfg["cap"],
                         "places": sorted(taken), "run": run, "what": what})


def sim_report(path: Path) -> list:
    """The sim pool's lines for `status` (THE SIM POOL): its size now and why, the places held and
    free, the games playing and the places yielding (THE YIELD), its memory, the load guard's
    discount, the batch waiting in the sim queue and each batch with the places it holds, its games
    playing and its places yielding. Every count is of the lock files themselves, asked of each
    lock; a game is its batch's by the run id its line names."""
    d = slots_dir(path)
    try:
        cfg = sim_config()
    except BadSim as e:
        return [f"sim pool: {e}; no batch is admitted"]
    threads, engine = machine_threads(), engine_held(d)
    size = sim_size(threads, engine, cfg["floor"], cfg["cap"])
    by = {}
    for k, p in _numbered(d, "sim"):
        if _held_now(p):
            by.setdefault(_read(p) or "a batch that has not written its line yet", []).append(k)
    games = {k: _read(p) for k, p in _numbered(d, "simgame") if _held_now(p)}
    held = sum(len(ks) for ks in by.values())
    over = sum(k >= size for k in games)
    yielding = sum(k >= size for ks in by.values() for k in ks)
    lines = [f"sim pool: {size} places now ({threads} threads less the {engine} engine slots held, "
             f"kept within {cfg['floor']}..{cfg['cap']}), {held} held, {max(size - held, 0)} free; "
             f"{len(games)} game{'s' if len(games) != 1 else ''} playing, {over} of them on places at "
             f"or above the size, finishing; {yielding} place{'s' if yielding != 1 else ''} held at or "
             f"above the size start no game; {cfg['pool_mib']} MiB out of the reserve, "
             f"{place_mib(cfg)} MiB a process; idle priority; the load guard takes the {len(games)} "
             f"game{'s' if len(games) != 1 else ''} playing off the load"]
    simq = d / "simq"
    if simq.exists() and _held_now(simq):
        lines.append(f"waiting for the sim pool: {_named(_read(simq) or 'a batch that has not written its line yet')}")
    for line, ks in by.items():
        run = re.search(r"\[run ([^\]]+)\]", line)
        playing = sum(1 for g in games.values() if run and f"[run {run.group(1)}]" in g)
        lines.append(f"{len(ks):2} place{'s' if len(ks) != 1 else ''} of the sim pool held by {_named(line)}; "
                     f"{playing} game{'s' if playing != 1 else ''} playing, "
                     f"{sum(k >= size for k in ks)} yielding")
    return lines


class OverBound:
    """forked()'s result for a job past the most memory its phase was granted: `mib`, that bound."""

    def __init__(self, mib: int):
        self.mib = mib

    def __repr__(self):
        return f"OverBound({self.mib})"


class JobFailed(RuntimeError):
    """A job of forked() that raised, or whose process ended without its result -- killed."""


def _forked_job(conn, fn, job, mib) -> None:
    bind(mib)
    try:
        out = ("done", fn(job))
    except MemoryError:
        out = ("memory", None)
    except BaseException:
        import traceback
        out = ("raised", traceback.format_exc())
    try:
        conn.send(out)
    except MemoryError:
        conn.send(("memory", None))         # the result's pickle passed the bound; its buffer is gone
    conn.close()


def forked(tree: Path, what: str, fn, jobs: list, mib_of, most: int) -> list:
    """fn(job) for each of `jobs`, each in a process forked for it alone and bound to the slots that
    carry mib_of(job) MiB, at most `most` at once (THE HOST MEMORY): the results in the jobs' order.

    The jobs run on one offline phase (OFFLINE WORK) of at least the heaviest job's slots and at most
    the `most` heaviest's together, and are started in their order as the grant has room for each
    one's slots, so a heavy job at the head waits for its room and none is passed over. A job whose
    process passes its bound is run again on twice the slots, up to the whole grant; past that its
    result is an OverBound. A job that raises, or whose process ends without its result, raises
    JobFailed once the jobs started with it have been ended. A process forked for one job alone
    starts from the parent's memory, never another job's heap, and holds the phase's slots with the
    parent until it exits (A HOLD THAT ENDS BEFORE THE PROCESS). Where the machine's memory cannot be
    read, each job takes one slot, unbound."""
    import multiprocessing
    from multiprocessing.connection import wait
    if not jobs:
        return []
    per = slot_mib(lock_path(tree))
    weight = [min(slots_for(mib_of(job), per), BUDGET) if per else 1 for job in jobs]
    heaviest = sorted(weight, reverse=True)
    ctx = multiprocessing.get_context("fork")
    results, live, queue = [None] * len(jobs), {}, collections.deque(range(len(jobs)))
    with offline(tree, what, min(BUDGET, sum(heaviest[:max(1, most)])), heaviest[0]) as phase:
        grant = phase.n
        free = grant
        try:
            while queue or live:
                while queue and len(live) < max(1, most) and min(weight[queue[0]], grant) <= free:
                    i = queue.popleft()
                    weight[i] = min(weight[i], grant)
                    r, w = ctx.Pipe(duplex=False)
                    p = ctx.Process(target=_forked_job, args=(w, fn, jobs[i], weight[i] * per if per else None),
                                    daemon=True)
                    p.start()
                    w.close()           # the job's end of its pipe is its process's alone: EOF when it ends
                    live[r] = (i, p)
                    free -= weight[i]
                for r in wait(list(live)):
                    i, p = live.pop(r)
                    try:
                        kind, value = r.recv()
                    except EOFError:
                        p.join()
                        how = f"killed by signal {-p.exitcode}" if p.exitcode < 0 else f"exit {p.exitcode}"
                        raise JobFailed(f"{what}: job {i} ended without its result ({how})") from None
                    finally:
                        r.close()
                    p.join()
                    free += weight[i]
                    if kind == "raised":
                        raise JobFailed(f"{what}: job {i} raised:\n{value}")
                    if kind == "memory" and per and weight[i] < grant:
                        say(f"{what}: job {i} passed its {weight[i] * per} MiB: run again on "
                            f"{min(2 * weight[i], grant) * per} MiB")
                        weight[i] = min(2 * weight[i], grant)
                        queue.appendleft(i)
                    else:
                        results[i] = OverBound(weight[i] * per if per else None) if kind == "memory" else value
        finally:
            for _i, p in live.values():
                p.kill()
                p.join()
    return results


def cmd_make(a) -> int:
    path = lock_path(Path.cwd())
    w = _offline(path, a.what or f"make {' '.join(a.cmd)}", a.jobs)
    for fd in w.fds():
        os.set_inheritable(fd, True)
    argv = ["make", f"-j{w.n}", *a.cmd]
    try:
        os.execvpe(argv[0], argv, w.environ())
    except OSError as e:
        say(f"make: {e}")
        return 127


def cmd_status(a) -> int:
    path = lock_path(Path.cwd())
    budget = gpu_budget(path)
    free, lines = report(path, budget)
    print(f"runlock: {free} of {BUDGET} slots free ({path.parent / 'runslots'})"
          + ("" if budget else "; no GPU budget: no device memory reads"))
    for ln in lines:
        print(f"  {ln}")
    try:
        per = slot_mib(path)
        print(f"  host memory: {per} MiB a slot, {per * BUDGET} MiB in all (THE HOST MEMORY)" if per
              else "  host memory: the machine's cannot be read; a forked job takes a slot, unbound")
    except BadFigures as e:
        print(f"  host memory: {e}")
    room, why = _guard(slots_dir(path), ())
    print(f"  load guard: {why}" + ("; the next run is admitted on its slots alone" if room is None else ""))
    for ln in sim_report(path):
        print(f"  {ln}")
    return 0


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv[:1] == ["keep"]:
        return keep(argv[1:])               # start_game's keeper, not a command for a hand
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n", 2)[2])
    sub = ap.add_subparsers(dest="cmd_name", required=True)
    r = sub.add_parser("run", help="a command under W slots, after waiting for them")
    size = r.add_mutually_exclusive_group(required=True)
    size.add_argument("--slots", type=int, metavar="W", help=f"slots to hold, clamped to 1..{BUDGET}")
    size.add_argument("--lanes", type=int, metavar="N",
                      help="game lanes played at once: the slots of N lanes of their renderer, GDI "
                           "with --vram 0 and Vulkan otherwise (tools/runlock-lanes.cfg)")
    size.add_argument("--sim", type=int, metavar="N",
                      help="places of the sim pool, outside the slots, clamped to 1..its cap "
                           "(tools/runlock-sim.cfg): the command and all it starts at idle "
                           "priority, each process under the pool's memory bound")
    r.add_argument("--vram", type=int, default=None, metavar="G",
                   help="games on the GPU at once, each taking its cap's share of the GPU budget "
                        "(default: --lanes, or the Vulkan lanes the slots hold), cut to the budget; "
                        "0 for a run with no GPU, whose lanes draw with GDI")
    r.add_argument("--suite", default=None, metavar="NAME",
                   help="the suite whose games these are: their GPU memory cap is that suite's in "
                        "tools/runlock-suites.cfg (default: the play defaults' cap_mib)")
    r.add_argument("--what", required=True, help="what the run is, for the holder line")
    r.add_argument("--resource", action="append", default=[], metavar="NAME",
                   help="a resource no other run may hold meanwhile, instance-<name> for the "
                        "tacli instance <name>; repeatable")
    r.add_argument("cmd", nargs=argparse.REMAINDER, help="-- and the command")
    r.set_defaults(fn=cmd_run)
    m = sub.add_parser("make", help="make under the slots of its jobs, -j the slots granted")
    m.add_argument("-j", "--jobs", type=int, default=BUILD_JOBS, metavar="N",
                   help=f"the most slots, and so make's -j, clamped to 1..{BUDGET} (default "
                        f"{BUILD_JOBS}); fewer when fewer are free, at least one")
    m.add_argument("--what", default=None, help="what the build is, for the holder line")
    m.add_argument("cmd", nargs=argparse.REMAINDER, help="-- and make's arguments")
    m.set_defaults(fn=cmd_make)
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
    except BadLanes as e:
        say(f"the lane weights cannot be read, so no run is admitted: {e}")
        return 2
    except BadSim as e:
        say(f"the sim pool's figures cannot be read, so no sim batch is admitted: {e}")
        return 2
    except IdleEngineRun as e:
        say(str(e))
        return 2


if __name__ == "__main__":
    sys.exit(main())
