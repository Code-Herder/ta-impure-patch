# Demo recorder — completeness design

## Status

**Design, not implementation.** How the state recorder stays complete as Impure's simulation
changes. The owner chose **option B, declare and measure**, on 2026-09-29, after a design panel
(four designs, two judges) over the code as it stood; the product decisions it serves are on the
[product page](demo-recorder.md#completeness-and-verification), and the engine facts it rests on
are in the [state design evidence](demo-recorder-state-evidence.md). Addresses below come from that
panel's readers and were checked against the pristine binary by its judges unless marked
[INFERRED]. Two decisions are still open ([below](#open-decisions)); the proposed CLAUDE.md text
is a draft until the owner adopts it.

## The problem

Impure's simulation grows fast. The fail-closed patch-site table held 156 sites on 2026-09-25, 284
on 2026-09-27 and 358 with the COB core on 2026-09-28 (MEASURED, instance logs); extra weapons' 46
sites and the data keys' 15 install outside it. Much of the new state is invisible to a comparison
of engine records: 14 of Impure's own tables with simulation meaning (`g_side`, `tagpu_weapons.c`;
`s_quiet`, `tagpu_datakeys.c`; `s_windState`, `tagpu_patches.c`; …), four wire tags of its own
(`0x49`–`0x4C`) and seven engine fields put to another use. Serializers already miss some of it:
savegames drop extra-weapon slots 4 and up ([extra weapons](../extra-weapons.md)). A recorder with a
fixed field list would go silently wrong at the next feature.

The owner's requirements: a gap must be **loud and named**, never silent; the design must **not be
brittle** (no hand-maintained field list, no check that raises false alarms on ordinary changes);
and **a replay shows what the game showed**.

## Option B: declare and measure

### Engine records: recorded whole by default

The unit records (stride `0x118`) and the player records (stride `0x14B`) are recorded whole. One
exclusion file, `tagpu/ddraw/replay-layout.tsv`, lists the bytes not recorded as values: pointers
(recorded as target IDs), caches, timers and takeover drops. Each row names its engine writers; it
starts from what the saver `0x4876C0` writes, and the build hash-checks it the way `spirv-check.sh`
checks shaders, so a change to it is a visible diff the owner approves. Only the unit and player
layouts are fixed by the retail executable: Impure's COB landing grew the engine's COB object from
`0x544` to `0x1144` bytes (`0x485D73`), and several pools now live in the DLL.

The main block and the executable's `.data` (which holds the simulation RNG's seed, `0x51FC88`) are
captured, applied only through a positive list, and classified by the harness's null twin (below),
so presentation bytes such as the option word never trip anything.

### Impure's own state: declared where it is defined

In a source file marked `sim` (a word added to its `thread-split.allow` line; files marked
`presentation` are out of scope), every table, static variable and thread-local slot carries a
one-line declaration next to its definition. The proposed shape:

```c
/* inc/tagpu_simreg.h (proposed) */
typedef enum { KEY_NONE, KEY_UNIT_SLOT, KEY_UNIT_TYPE, KEY_PLAYER, KEY_WEAPON } SimKey;
typedef enum { SIM_STATE,     /* recorded, restored, verified                */
               SIM_CONTENT,   /* rebuilt from game data at load; hashed      */
               SIM_TRANSIENT, /* empty whenever a snapshot is taken          */
               SIM_PRIVATE    /* diagnostics the simulation never reads      */
} SimClass;

typedef struct SimDesc {
    const char*     name;     /* "tagpu_datakeys.c:s_quiet": names every failure  */
    void*           addr;     /* the array, or &pointer for a heap table          */
    const unsigned* rows;     /* live row count (NULL = fixed)                    */
    unsigned        stride;   /* bytes per row                                    */
    SimKey          key;      /* what a row index means: how to remap at takeover */
    SimClass        cls;
    const struct SimField* layout; /* optional: which bytes are IDs               */
} SimDesc;

#define SIM_DECLARE(var, cls, key, stride, rowsptr) \
    static const SimDesc simdir_##var \
    __attribute__((used, section(".rdata$simreg$m"))) = \
    { __FILE__ ":" #var, (void*)&var, rowsptr, stride, key, cls, 0 }

/* in use */
static unsigned char s_quiet[DK_QUIET_SLOTS];
SIM_DECLARE(s_quiet, SIM_STATE, KEY_UNIT_SLOT, 1, NULL);
```

The descriptors land in one linker section, which MinGW's default script sorts by name
(`.rdata$simreg$a` … `$z`; the panel confirmed the ordering in a scratch link), so the recorder
walks one list of all simulation state with no central file to edit. A declared `SIM_STATE` table is
recorded, carried in the self-check slice with its unit or player, dumped by the harness and written
back at a takeover, remapped through its key, with no code of its own. `SIM_CONTENT` is hashed on its
value fields; `SIM_TRANSIENT` is asserted empty at snapshots; counters the simulation never reads go
in one `SIM_PRIVATE` struct per module.

- **Heap and thread-local tables** come only from a wrapper, `tagpu_simtab_alloc`, which registers
  what it creates.
- **Tables store IDs, not addresses**, or declare which bytes are IDs. Today `g_side`'s rows hold a
  COB thread handle and a `WeaponStruct*`, and `DkUnit` holds `def`, `pp` and a per-process serial;
  these get IDs or a layout.
- **The build check**, `tools/sim-state-check.sh`, lists each `sim` object's writable symbols with
  `nm` and requires a `simdir_` twin for each (a name match, since GCC relocates file statics
  against the section symbol); `malloc`, `VirtualAlloc` and `TlsAlloc` outside the wrapper are
  refused. It must depend on `$(OBJS)`: as an order-only prerequisite it would race the compiler.
  A failure names the object and the symbol.
- **One hole:** state Impure keeps inside engine-allocated objects (the grown COB object) is not a
  DLL variable. It needs a layout declaration, which only the harness can catch missing.

### Events

Events are counted where they start (roots: the creates `0x485F50`/`0x4861D0`, the destructor
`0x4866D0`, the script-start broadcasters `0x456190`/`0x456200`/`0x456290`, the fire functions,
build completion `0x41B8D0`) and where effects are produced (sinks, counted by caller: `0x420A30`,
`0x470EB0`, the COB lookup `0x4B08C0`, the sound entries `0x47F0C0`–`0x47F300`). Each function gets
one shared hook through the fail-closed table, so a clash with another feature's hook on the same
function (B5's on `0x4861D0`, B6's on `0x490C40`) is refused at install and names both. New effects
start through an engine root, never a direct call to a sink.

Each simulation feature declares its **puppet disposition** (runs on puppets, owner-only, or
independent) and names a **fixture** that exercises it. Owner-only must be enforced in code, since
puppets run COB (`0x48AD82..0x48ADEB`).

### Always-on self-verification

At each tick boundary the game thread copies a rotating slice of the real state (for example 50
unit records, one player record and their declared rows); the writer thread compares it with the
recorder's own copy of the world. At 50 units a tick, 1,500 units are checked within 30 ticks. A
mismatch writes the true values into the recording as a correction, names the field in the file's
metadata and logs one line per field; in the harness it fails the test. Capture happens at the
unit-tick call `0x4954ED` (B2's stub) and is diffed on the writer thread. Copying 1,500 unit
records takes about 6.5 µs on a host benchmark [INFERRED for the game; feasibility experiment 3
measures it].

## The replay test harness

`tools/replay/tareplay.py` reuses the compatibility suite's instance preparation, job pools, one
DirectPlay port per game and 0/1/2 exit codes; a `tagpu_replay.trigger` with the verbs `dump`,
`restore`, `seek` and `play` runs on the game thread.

**The restore round trip**, at seeded random ticks of a scripted game: pause and take dump A;
rebuild in place from the recording; let one paused frame recompute; take dump B and compare. The
comparison covers live units (matched by a recorder-issued ID, since B4's birth stamp is shared by
every unit created in the same tick), players, the main block and `.data`, features, wrecks, orders
and declared tables. Recorded bytes must match within the recording's quantum, caches after the
recompute, pointers as target IDs; only the takeover-drop list may differ. Anything else fails by
name, for example `restore@18231 unit[ARMCOM id 312] +0x108 hp 3120→3100 RECORDED`.

**No vacuous passes:** declared tables are overwritten with `0xA5` before the rebuild; one restore
per run puts units into different slots; a fresh process seeking to the same tick must match dump A,
and so must forward and backward seeks; a **null twin** pauses for the same time under scripted
camera and menu input without rebuilding, and the bytes that change there are presentation and are
masked.

**Also asserted:** puppet playback (equal root counts; sink counts by caller within a band, less an
owner-approved list of events expected to be absent); a 60-second takeover judged like the
compatibility suite's battle; zero self-verification corrections; the cost and size budgets; and
UNCOVERED, which fails a tier when a declared table, root, sink or feature is reached by no run.

**Multiplayer:** never rebuild in place on a live peer, whose creates would broadcast. Every peer
pauses and dumps, and each recording is later seeked from a fresh process and compared with its own
peer's dump.

| Tier | Runs | Time [INFERRED from the compatibility suite: 572 s at six games] | When |
|---|---|---|---|
| Landing | fixtures of the touched features; a 1v1 with AI; a four-AI skirmish with air, sea, transports and structures; a two-peer game; three restores and one slot-shuffled restore per skirmish | 10–15 min | simulation landings |
| Nightly | every fixture; `limits-tier1` (4 × 1,500 = 6,000 units) with 20 restores; each compatibility setup; a game loaded from a save; a four-peer game; a four-player AI hour for the budgets | about 90 min | nightly on `main` |
| Release | the nightly on the release DLL, plus `limits-tier2`: 10 peers, 15,000 units | about 30 min more | before every `v*` tag |

Ten players exist only as a network game: a stock skirmish seats four (MEASURED,
[scenario format](../scenario-format.md)). Replaying ten-player games needs feasibility
experiment 1.

## Build order

1. **Before the first self-verification slice:** the exclusion file with its build hash; the `sim`
   and `presentation` words in `thread-split.allow`; the declarations and the build check; IDs or
   layouts for the pointer-carrying tables; the recording header's lever list; capture at `0x4954ED`.
2. **With the skirmish slice:** self-verification over units, players and declared tables; root and
   sink counting; the main block and `.data` with the null twin; puppet dispositions and fixtures;
   the landing and nightly tiers.
3. **Later:** multiplayer dumps, `limits-tier2`, and a check that every changed executable byte lies
   inside a range some installer published.

## Draft CLAUDE.md section and landing gate

Not adopted yet (open decision 4). The draft:

```markdown
## Simulation changes stay replayable

The recorder keeps every byte of the engine's unit and player records except what
`tagpu/ddraw/replay-layout.tsv` excludes, plus all simulation state Impure declares. A
simulation change needs no recorder change unless `make` or the replay harness names one.

- **Declare state where you define it.** In a file marked `sim` in `thread-split.allow`,
  every static, heap table and TLS slot is `SIM_STATE`, `SIM_CONTENT`, `SIM_TRANSIENT`, or
  lives in the module's `SIM_PRIVATE` struct. Memory, TLS, page protection and levers come
  through the wrappers. When unsure, use `SIM_STATE`.
- **Store IDs, not addresses**, or declare the table's layout.
- **Start effects through an engine root**, never a direct sink call. Each simulation
  feature states its puppet behaviour and names its fixture.
- **A replay shows what the game showed**: display state that follows from simulation
  events is `SIM_STATE`, not `SIM_PRIVATE`.
- **Loosening is the owner's decision**: an exclusion, a `presentation` file, TRANSIENT or
  PRIVATE on memory the simulation reads, an expected-absent event, a tolerance. A
  difference the harness names is a bug in the change, never grounds for an exclusion.
- **A landing that touches a `sim` file, the layout, the recorder or a fixture** passes
  `make -C tagpu/ddraw replay-test` and records a `replay-roundtrip:` note.
```

The landing command would gain the build check in Step 3; a Step 4b requiring a current
`replay-roundtrip:` note on `main..HEAD` whenever the diff names a `sim` file, the layout, the
recorder or a fixture (three attempts at the landing tier, then escalation reason 3 if a game cannot
start); and three items in the Step 5 review brief: each new class checked against the code, new
direct sink calls, and every loosening.

## Decisions taken

| Date | Decision |
|---|---|
| 2026-09-29 | **Option B, declare and measure.** The panel rejected a static-first registry with a lint over raw writes as brittle, and a single simulation API as too large a migration before the recorder exists; a measure-only design leaves the "remember to register" gap the owner ruled out. |
| 2026-09-29 | **Extra weapons is always on and fails closed**, like every other simulation feature; its switch and silent disarm go, so recordings carry no arming dimension. |
| 2026-09-29 | **`s_quiet` is recorded, and a replay shows what the game showed**: display state that follows from simulation events is recorded whenever the live game displayed it. |
| 2026-09-29 | **Effects only the owner's machine produces are recorded where the engine produces them and replayed through the same function.** The first case is the nano spray: two emitters (`0x4720D0`, reverse `0x472200`) called from 17 sites in 15 order handlers, which run only through the main-list order controller in a local owner's unit tick (DIS). Each emission is credited to the unit whose `QueryNanoPiece` (`0x43E400`) produced its start point: the handler's unit, or the repairing pad in `SelfRepair`. It is recorded as that unit's spray state: direction, target box, and the ticks it sprayed on. Playback calls the same emitter for the puppet, taking the start point from the puppet's own `0x43E400` and bounding the box to the map. An emission that no such query precedes is a named failure. Any other effect the harness finds missing on puppets gets the same treatment, never an imitation. Rejected: deriving sprays from recorded orders (15 handlers, each gated differently, wrong in a stalled economy) and accepting the gap. |
| 2026-09-29 | **Getter 75 answers on a puppet what the recording machine answered, per seat.** The recording keeps one bit per player, *simulated on the recording machine*, and in puppet mode getter 75 answers that bit for the queried unit's owner. Puppets therefore show the owner-only indicators the recording machine showed, drawn by the mods' own scripts. Two rules come with it, needed whatever 75 answers. **One writer in puppet mode:** COB's SET, ATTACH and DROP change no engine state on a puppet, and the recorder alone writes `+0x10E`, `+0x10F` and the carry links; it applies `+0x10E` through `0x48B090` so each recorded change starts its scripts once. **Takeover flips the seats, then re-creates the units:** only a create gives fresh COB, and Create's first step reads 75. Rejected: accepting 0, which silently drops Escalation's and Twilight's owner-only indicators (375 branches), and replaying per call, since no invariant pairs a recorded call with a puppet call. |

## Open decisions

1. **Harness time per simulation landing.** The landing tier holds most of the reference setup for
   10–15 minutes: accept that, or run only the nightly when a landing declares nothing new.
2. **The CLAUDE.md section and landing gate above:** adopt the rule now, so new simulation code
   is written declaration-ready, or together with the mechanism in the first slice.
