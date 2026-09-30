# Demo recorder — completeness design

## Status

How the state recorder stays complete as Impure's simulation changes. **The declarations and
their build check, and the exclusion file with its check, are built** (the
[build order](#build-order)'s first step, two landings, 2026-09-29): every writable static of the
four simulation files is declared, and the link refuses one that is not; the bytes of the engine's
unit and player records that are not recorded as values are listed in one file, and the build
refuses a header that does not match it. The replay harness and self-verification are designed,
not built. The owner
chose **option B, declare and measure**, on 2026-09-29, after a design panel (four designs, two
judges) over the code as it stood; the product decisions it serves are on the
[product page](demo-recorder.md#completeness-and-verification), and the engine facts it rests on
are in the [state design evidence](demo-recorder-state-evidence.md). Addresses below come from that
panel's readers and were checked against the pristine binary by its judges unless marked
[INFERRED]. Every decision is taken ([below](#decisions-taken)). The rules are in CLAUDE.md
(*Simulation changes stay replayable*), less the replay gate, which lands with the harness
([below](#the-claudemd-section-and-landing-gate)).

## The problem

Impure's simulation grows fast. The fail-closed patch-site table held 156 sites on 2026-09-25, 284
on 2026-09-27, 358 with the COB core on 2026-09-28 and 402 on 2026-09-29, when the extra weapons'
44 sites joined it (MEASURED, instance logs); the extra weapons' two record observers and the data
keys' 15 sites install outside it. Much of the new state is invisible to a comparison
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
exclusion file, `tagpu/ddraw/replay-layout.tsv`, lists the bytes not recorded as values, one row
per field or bit range, each with a class, its engine writers and the evidence:

| Class | Meaning | Rows |
|---|---|---|
| `id:<table>` | a pointer, recorded and compared as its target's ID in `unit`, `type`, `weapon`, `player` or `bucket`; the ID is one more than the pointer's row in its table (so `NULL` is 0), never a field of the target, so a pointer to a dead unit keeps its ID | 13 |
| `object` | a pointer to an engine object recorded by a record of its own (the mover, the order lists, the COB object, the model, the AI, the sight counters…) | 10 |
| `cache` | not recorded; the engine recomputes it from recorded state, and it is compared after that recompute | 2 |
| `timer` | not recorded; on a clock a replay does not reproduce | 4 |
| `drop` | not recorded and not restored: the viewing machine's own state | 10 |

The rows start from the stock savers, the engine's own inventory of a record: the unit saver
`0x4876C0` and the player saver `0x4662F0`. A byte a saver writes raw is a value unless shown
otherwise; a byte it skips or converts needed a class. Every field, its writers and the evidence are
in the engine map, [*The unit and player records, byte by byte*](../exe-reverse-engineering.md#the-unit-and-player-records-byte-by-byte-the-savers-the-replay-layouts-rows-and-what-the-values-carry-disassembled-measured-2026-09-29).
`tools/replay-layout-gen.py` validates the file (known records, classes and tables, the field
inside its record, no bit in two rows, rows in order, every writer inside the executable's `.text`)
and generates `inc/tagpu_replay_layout.h`, which carries the file's SHA-256; the link runs
`tools/replay-layout-check.sh`, which re-derives the header and fails when the file changed without
it or the header was edited by hand, as `spirv-check.sh` does for shaders. A change to what is
excluded is therefore always a visible diff of the file, and each row is a loosening the owner
approves. Only the unit and player layouts are fixed by the retail executable: Impure's COB landing
grew the engine's COB object from `0x544` to `0x1144` bytes (`0x485D73`), and several pools now
live in the DLL.

**What restoring these records needs**, found while building the file; each is for the applier
the skirmish slice brings:

- **Caches are recomputed by the applier.** A paused frame runs neither the per-tick sight stamp
  `0x4827B0` nor the radar pass `0x467440`, so the applier calls what recomputes a cache before the
  comparison reads it — never the radar pass, whose results (`+0x110` bits 8–10 and 12, `+0xB0`)
  are recorded values the simulation reads between its passes. The reference chain `+0xA2` is rebuilt by linking every restored reference
  through `0x489690`, the air orders' payload references included. The player's sight grid
  geometry `+0x80..+0x8B` is never written: its length bounds `0x4816A0`'s fill of this process's
  grid.
- **The order of two lists is state.** `+0x8E` is the carrier's chain while `+0x86` is set and the
  sort bucket's list otherwise, and the simulation reads both in order (RepairPatrol's pick by the
  simulation RNG, the unloads' head). The applier rebuilds each bucket's head from `+0x82` and
  `+0x8E`: the head is the member no other member points to.
- **Some values go through engine functions, never a raw write:** the sight stamp `+0xF8`,
  `+0x7A`, `+0x7C` through the unstamp `0x481D50` and the stamp `0x482270`, since the counters
  hold what the old values stamped; `+0x10E` through `0x48B090` (*Decisions taken*, getter 75).
- **A running COB thread points into the unit array**: AutoAim hands the `Aim` script
  `&slot+0x4` as its completion callback, so a recorded COB state translates that pointer as a
  unit and a weapon slot.
- **The weapon slot's def pointer is an ID, resolved by the applier.** The stock restore instead
  writes the saved weapon ID into the shared weapon def (`0x487628`).
- **Ten player records, not eleven.** The engine builds and resets eleven; the eleventh is what an
  index of 10 (no player) reads, and nothing records or writes it. Slot 0 of the unit array has a
  NULL owner and is never a unit.
- **Players are keyed by the transport ID `+0x04`**, which the applier keeps distinct between
  active records, translating a recorded one through the recording's key: the death path turns the
  killer's player index into a transport ID and back (`0x44FFD0`, `0x44FE40`) into unit `+0xF4`.
  The controller `+0x73` changes in play (the console's AI toggle `0x4161F0` flips 1 and 2, a leave
  writes 0), so the recording carries each change, while a puppet host holds 3 for every puppet.
- **A seek sets the viewer's DisplayTimer `+0xF8` to GameTime**, as the level init does: the draw
  path raises it by 30 whenever GameTime has passed it and then refreshes the resource bar, so
  after a rewind it would stay ahead and the bar would freeze until GameTime caught up.
- **Other writers on a replay host**, for the puppet dispositions the skirmish slice decides (on a
  puppet the recorder is the only writer): the radar pass writes bits 8–10 and clears bit 12 on
  every live unit on the viewed player's pass (`0x4674B6`); the scoreboard `0x4948E0` compacts the
  ranks `+0x148` from the draw path while it is shown; and the receive timeout `0x453C20` reads
  every active type-3 record's `+0x1C` against the wall clock (experiment 1's puppets ran for
  minutes and were never dropped; whether a skirmish reaches it is not established). The
  recording carries the recording viewer's contact bits, so showing another perspective's radar
  in playback is decided there too.
- **Order nodes carry draw-path state**, for their own layout: the order-queue draw `0x439740`
  (DrawGameScreen → `0x48CC30` → `0x439B30`) writes a node's `+0x32`/`+0x34` and `+0x42` bit 21
  for a selected unit's queue, depending on the selection and on bit 9.

**Gaps stated, not closed.** The hover wave: a unit whose def has `+0x241` bit 12 (`canhover`) and
neither bit 19 nor bit 20 takes its altitude and tilt from a wave on the wall clock (`0x48A490`,
through `0x4B6340`), so a replay cannot recompute them and two peers of a network game compute
different values. The recording carries `+0x64`, `+0x68` and `+0x70` as values, which costs size for
every such unit on every tick; moving the wave onto GameTime would remove both the cost and the
stock nondeterminism, and is a simulation change of its own, a TODO in
[section B's items](sim-fixes.md#the-items). No network game was measured for the
file: the player rows' network fields rest on the disassembly alone.

The main block and the executable's `.data` (which holds the simulation RNG's seed, `0x51FC88`) are
captured, applied only through a positive list, and classified by the harness's null twin (below),
so presentation bytes such as the option word never trip anything.

### Impure's own state: declared where it is defined

Every source file on `thread-split.allow` carries a scope word: `sim` or `presentation`. Four
are `sim` — `tagpu_patches.c`, `tagpu_weapons.c`, `tagpu_datakeys.c` and `tagpu_cobtrace.c` — and
every other listed file is `presentation`, out of scope; only a source file can be `sim`, since a
header's statics belong to the objects that include it. In a `sim` file every writable static,
heap table and TLS slot carries a one-line declaration beside its definition
(`tagpu/ddraw/inc/tagpu_simreg.h`):

```c
static unsigned char s_quiet[DK_QUIET_SLOTS];
SIM_ROWS(s_quiet, SIM_STATE, SIM_ID_UNIT_SLOT, NULL);

static WSlot* g_side;             /* [rows][WPN_SIDE] */
static u32    g_side_rows;
SIM_HEAP(g_side, SIM_STATE, SIM_ID_UNIT_SLOT, WPN_SIDE * sizeof(WSlot), g_side_rows, side_layout);
```

Each declaration is a descriptor (its name `__FILE__ ":" var`, line, address, row size, row count,
live-count pointer, class, key, flags and layout) and a pointer to it in the linker section
`.rdata$simreg$m`. MinGW's linker sorts `.rdata$*` by name, so the markers `tagpu_simreg.c` puts in
`$a` and `$z` bracket every declaration: the recorder walks one list of all simulation state and no
central file names any of it. The walk runs once, in DllMain after the last simulation module has
installed (`tagpu_simreg_attach`): it checks each declaration's shape, refuses two declarations
of one byte, and logs one `simreg:` line with the count by class, the number of levers read and
the name of each one set, with the start of its text when a reader read it.

**The classes.** When unsure, `SIM_STATE`: a wrong STATE costs recording bytes, a wrong other class
costs a replay that silently differs.

| Class | What it holds | What the recorder does |
|---|---|---|
| `SIM_STATE` | what the simulation reads and a game changes, and display state that follows from simulation events | records it, restores it, verifies it; a table is carried in the self-check slice with its unit or player and remapped through its key at a takeover, with no code of its own |
| `SIM_CONTENT` | derived only from game data when it loads, constant until that data loads again | rebuilds it, does not record it; hashes it to catch a mismatch |
| `SIM_TRANSIENT` | nothing carried from one tick to the next: at every tick boundary each byte is back at its value from load (an empty queue, a free hold, a cleared flag) or is written before anything reads it (a scratch buffer) | the harness resets it to its value from load at a snapshot, as a fresh process holds it, and the round trip must still match |
| `SIM_PRIVATE` | diagnostics the simulation never reads, in one struct per feature (`s_hitDiag`, `g_diag`, …), so every use says so | ignores it |
| `SIM_INSTALL` | what Impure writes while installing — in DllMain, and in the start-up checks at the first DirectDraw call — and never during a game: trampolines, code pages, site tables, TLS indices, levers, locks | the harness hashes it after install and fails by name when a game changes it; locks (`SIM_LOCK`) are left out of that hash |

Neither "empty at every snapshot" nor "never read by a later tick" fits both kinds of transient
memory: a scratch buffer keeps its last bytes after the call that used it, and a free hold's zero
thread ID is read by every send that asks whether it holds one (`s_killHold`). Both hold what a
fresh process seeking to that tick holds, the value from load, so that is what the harness
writes. A queue left non-empty across a tick then fails the round trip, and so does scratch read
before it is written, whenever its stale bytes differ from the load's.

**The forms:** one row (`SIM_VAR`, and `SIM_VAR_L` with a layout); a static array (`SIM_ROWS`),
or its live prefix (`SIM_ROWS_LIVE`, counted by an `unsigned`); a pointer to rows `SIM_ALLOC` made
(`SIM_HEAP`, counted, and `SIM_BLOCK`, a fixed number); an engine pool the raised limits moved into
the DLL (`SIM_ENGINE`, in the engine's own record layout, so its bytes are described with the
engine records, not by a layout here); a TLS index whose class is the slot's content (`SIM_TLS`);
and three install forms, `SIM_LOCK`, `SIM_CODE` (executable memory an installer builds) and
`SIM_GUARD` (a reserved region no access may touch). The compiler checks what it can: a
`SIM_ROWS` of a pointer, a `SIM_HEAP` of an array, or a count that is not `unsigned` does not build.

**The key** says what a row index means, so a takeover can move rows with their units: none, unit
slot, unit type, player, weapon, or a DirectPlay player ID (the session's, not the recording's).

**Tables store IDs, not addresses, or declare their layout.** A row whose bytes are not all plain
values names the others: the engine's own index of something (`SIMF_ID`, a slot, a type index, a
player index or a weapon ID), recorded as it is; the address of what such an index names
(`SIMF_ADDR`, a def or a weapon record), recorded as the target's ID, one more than its row, so
never equal to the engine's index for the same target; with no key, other process memory that is
never a value; a
block the row owns (`SIMF_OWNED`, allocated through its table); a process-local value (`SIMF_LOCAL`,
a serial or a clock); a struct-of-arrays column and its count (`SIMF_COLUMN`, `SIMF_COUNT`); the
engine's weapon target (`SIMF_TARGET`, a unit slot when the second u16 is `0x8000`, else a ground
point); and a field whose address the engine holds (`SIMF_PINNED`). The last is the extra weapons'
side slots: a running aim script's thread record points at the slot's callback object, so the
recorder writes those rows in place and never moves one while a script may run
([extra weapons](../extra-weapons.md)). `DkUnit` declares `def` an address of its unit type, `pp`
an owned block and `serial` process-local.

**Everything else comes through the wrappers.** Memory belongs to a declaration. `SIM_ALLOC` and
`SIM_FREE` serve the memory its variable points at, from `VirtualAlloc`: a heap table's rows, a
TLS slot's buffer, code (executable) and a guard (reserved with no access). A heap table's pages
are write-watched (`MEM_WRITE_WATCH`), so the recorder images only the rows on pages written since
its last capture; a large table is best declared on the heap for that reason
([the skirmish slice](demo-recorder-skirmish-slice.md#the-capture)). `SIM_ALLOC_OWNED` and
`SIM_FREE_OWNED` serve a block one of its rows owns, from the process heap. They are separate
calls so that a heap table whose rows own blocks takes each from the right place; a call the
declaration does not own memory for gets NULL and a `simreg: FAILED` line naming it. Scratch that
a call frees before it returns comes from `tagpu_sim_scratch`. TLS indices come from
`SIM_TLS_ALLOC`, page changes from `tagpu_detour_write` and stubs from `tagpu_detour_stub` (freed by
`tagpu_detour_stub_free`). A lever — a file in the game folder whose presence or text changes
what the simulation runs — is read through `tagpu_sim_lever` or `tagpu_sim_lever_read` while
Impure installs. The registry keeps one entry per name, however many installers ask (three ask
for `tagpu_wirecheck.on`): whether it exists and, when a reader read its text, that text, its
length and a hash of it, so `tagpu_dmgdelay.on` reading `5` and reading `60` are two settings.
Four things log FAILED by name: a lever read after install, a lever found present and absent
while installing, one read with two texts, and a 33rd lever past the list's 32. A file that
tooling reads or writes is named inside `SIM_TOOL_FILE`.

**The build check**, `tools/sim-state-check.sh`, runs in the link recipe of whichever build is
linking, on the objects about to be linked, and fails before the link, naming the file and the
symbol; the landing gate builds both variants, since the simulation files differ between them by
`#ifdef`:

- **UNDECLARED**: a writable static (`nm` classes `b B d D C`) with no `simreg_` twin of the same
  name, counted (two function-local statics named `top` need two declarations; GCC's `.N` suffixes
  are stripped). It is a name match because GCC relocates file statics against the section symbol,
  so nothing pairs them by address.
- **STALE**: a declaration of something that is not a writable static of that object.
- **IMPORT**: a call to a function on its refused list: the C runtime's allocators and string
  duplicates, the Win32 heap, global, local, COM task and virtual memory, the native API under
  them, TLS and fiber-local slots, page protection, file mappings, `WriteProcessMemory`, and
  `GetProcAddress`, which would reach any of them around the list.
- **LEVER**: a string literal that is a whole `tagpu_` file name anywhere but directly inside
  `tagpu_sim_lever(`, `tagpu_sim_lever_read(` or `SIM_TOOL_FILE(`, with comments and `#include`
  lines removed first.

It exits 0 clean, 1 on an offender and 2 when it could not run (no `nm`, a missing object, a
header marked `sim`, and the like). **Verified 2026-09-29:** a probe object carrying one of each offender fails with
each named, the counted `top` included, while the wrapped lever beside them passes; a second probe
calling `_recalloc`, `_wcsdup`, `CoTaskMemAlloc`, `MapViewOfFile` and `WriteProcessMemory` fails
on all five; the three could-not-run cases exit 2; an undeclared static added to
`tagpu_cobtrace.c` stops the link. The registry's own refusals were run under Wine against a test
program: a lever read with a second text, a table allocation on a declaration that only owns
blocks, an owned block from a table whose rows own none, a variable declared twice and a lever
read after install each log their FAILED line, and the duplicate counts as malformed. Clean,
it reports **170 declarations in the raised build and 154 in the stock-limits build**, and a running
game's attach line agrees (MEASURED 2026-09-29, on Wine, in all 18 of the compatibility suite's
setups that load Impure):

```
simreg: 170 declarations (30 state, 5 content, 38 transient, 19 private, 78 install; 4 locks, 5 TLS indices, 3 heap tables, 6 code or guard pointers, 6 engine pools, 15 layouts), 0 malformed; levers: 7 read
```

The stock-limits build reads 154 (22 state, 3 content, 37 transient, 19 private, 73 install; 3
locks, 5 TLS indices, 5 code or guard pointers, 12 layouts), with no engine pools, since it raises no
limit (MEASURED 2026-09-29, on Wine).

**What the check does not prove**, by construction of its rules, and so what the review and later
the harness have to:

- that a declaration's class is right, or that a `SIM_TOOL_FILE` name is only tooling's: each is a
  claim the code makes;
- state kept in a file not marked `sim`, or in memory a `presentation` module allocates;
- a lever named through a macro, a string built at run time or a header;
- an allocator or page change the refused list does not name, or one reached through the engine:
  its own helpers or import slots called by address. The one such use is the engine's own
  protocol for its write-protected def array, which `sync_keys_unique` follows as the engine's
  writers do (open with `0x4D8780`, seal with `0x4D8710`; engine map, *The engine write-protects
  the def array*);
- a variable declared twice, which can hide an undeclared namesake from the count by name; the
  walk at attach refuses two declarations of one byte instead, so a run shows it;
- state Impure keeps inside engine-allocated objects (the grown COB object), which is not a DLL
  variable and needs a layout declaration that only the harness can catch missing;
- the contents of a block a row owns: the COB file records (`s_cobFiles`) chain through pointers
  and hold a program that `tagpu_cob_safe.c` allocates outside the wrappers. They are content,
  rebuilt when the scripts load, but no layout describes them;
- two content tables place their rows by a hash of a script's address — `s_cob`, the data keys'
  script cache, and `s_cobFiles`, the COB file records' buckets — so the same content sits in
  other rows in another process: their content hash has to be taken over the rows as a set, or
  the tables keyed by an ID;
- the engine pools the DLL holds (`SIM_ENGINE`) are declared, but their record layouts are not yet
  written anywhere the recorder reads.

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

**Effects only the owner's machine produces** are recorded where the engine produces them and
replayed through the same function. The nano spray is the first: its two emitters are called only
from order handlers in a local owner's unit tick, so a puppet never sprays by itself. The recorder
wraps the three handler call sites, so every handler call carries its unit, and hooks the two
emitters. Each emission is credited to the unit whose `QueryNanoPiece` (`0x43E400`) produced its
start point, and recorded as that unit's spray state: direction, target box, and the ticks it
sprayed on. An emission that no such query precedes is a named failure: it fails the harness, and a
player's recording logs and counts it. Playback calls the same emitter for the puppet, with the
start point from the puppet's own `0x43E400` and the recorded box bounded to the map first. The
engine facts are in the engine map's *The nano spray*
([engine map](../exe-reverse-engineering.md)).

**COB on a puppet has one writer.** In puppet mode COB's SET, ATTACH and DROP change no engine
state, and the recorder alone writes `+0x10E`, `+0x10F` and the carry links; it applies `+0x10E`
through `0x48B090`, which returns early on an unchanged byte, so each recorded change starts its
scripts once. Script starts are recorded only at the five broadcaster call sites: a start a script
issues never reaches them and runs again on the puppet from its parent, so recording it anywhere
else would double it. For the same reason a puppet's order nodes never carry `+0x42` bit
`0x400000`, whose destruction would start a second `StopBuilding` [INFERRED from the destructor,
`0x43A21E..0x43A25D`]. Getter 75 answers the recording machine's bit for the queried unit's seat.
The engine facts are in the engine map's *COB on a puppet*.

### Always-on self-verification

At each tick boundary the game thread copies a rotating slice of the real state — the next
thirtieth of the unit slots, one player record and 16 KB of declared rows — and the writer thread
compares it with the recorder's own copy of the world, so every unit slot is checked within 30
ticks. A mismatch writes the true values into the recording as a correction, counted by record,
table and offset in the file's end block, and the first sixteen are logged; in the harness it fails
the test. Capture happens at the unit-tick call `0x4954ED` (B2's stub) and is compared on the writer
thread. S1's capture, which includes this slice, costs about 80 µs a tick in a four-player game and
235–251 µs in a battle of 1,200 to 1,400 units (MEASURED,
[the skirmish slice](demo-recorder-skirmish-slice.md#measured)).

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

**No vacuous passes:** declared state tables are overwritten with `0xA5` before the rebuild, and
transient memory is reset to its values from load; one restore
per run puts units into different slots; a fresh process seeking to the same tick must match dump A,
and so must forward and backward seeks; a **null twin** pauses for the same time under scripted
camera and menu input without rebuilding, and the bytes that change there are presentation and are
masked.

**Also asserted:** puppet playback (equal root counts; sink counts by caller within a band, less an
owner-approved list of events expected to be absent); no COB-originated engine write on a puppet
(`0x48B090` from `0x480B52`/`0x480BEC`, `0x47DAC0` from `0x480BAE`, `0x48AAC0` from `0x4813A6`/`0x481422`),
each named by unit and opcode; no emission without its `QueryNanoPiece`; a 60-second takeover judged
like the compatibility suite's battle; zero self-verification corrections; the cost and size
budgets; and UNCOVERED, which fails a tier when a declared table, root, sink or feature is reached
by no run.

**Multiplayer:** never rebuild in place on a live peer, whose creates would broadcast. Every peer
pauses and dumps, and each recording is later seeked from a fresh process and compared with its own
peer's dump.

| Tier | Runs | Time [INFERRED from the compatibility suite: 572 s at six games] | When |
|---|---|---|---|
| Landing | the fixtures of the features the diff changes, each recorded, restored in place with the round trip and replayed on puppets; the set comes from the features' declarations beside the changed code, not from a list | a few scripted games, each at most 10 min | every landing that changes simulation code; a change whose feature names no fixture fails the landing by name |
| Nightly | every fixture; a 1v1 with AI; a four-AI skirmish with air, sea, transports and structures, with three restores and one slot-shuffled restore; a two-peer and a four-peer game; `limits-tier1` (4 × 1,500 = 6,000 units) with 20 restores; each compatibility setup; a game loaded from a save; a four-player AI hour for the budgets | about 90 min | nightly on `main`, from a timer on the reference setup built with the harness |
| Release | the nightly on the release DLL, plus `limits-tier2`: 10 peers, 15,000 units | about 30 min more | before every `v*` tag, beside the compatibility suite |

The landing run is scoped to the change because the broad tier holds about 28 of 32 cores and 11
of 12 GB of VRAM on the reference setup while it runs, and 38 landings touched the simulation files
in three weeks, 34 of them in one week at up to 10 a day (MEASURED 2026-09-29 from `main`'s reflog);
about a third of that week's were not simulation work but edits to the mixed `tagpu_patches.c`
[judged from their subjects and diffs]. Cross-feature breakage is caught the next night or at the
tag, and always-on self-verification names any mismatch in every recording in between.

Ten players exist only as a network game: a stock skirmish seats four (MEASURED,
[scenario format](../scenario-format.md)). Feasibility experiment 1 seated ten puppet players in a
skirmish level (MEASURED 2026-09-29,
[evidence](demo-recorder-state-evidence.md#the-puppet-host-measured)), which ten-player replay
rests on.

## Build order

1. **Before the first self-verification slice**, two landings:
   - **The declarations and the build check — built 2026-09-29.** The scope words in
     `thread-split.allow` (`thread-split-check.sh` reads the third column); `tagpu_simreg.h` and its
     wrappers; `tools/sim-state-check.sh` in the link recipe; every existing static of the four
     simulation files classified once, with no exemption list, the diagnostics among them moved into
     one struct per feature; layouts for the tables that carry pointers or IDs; the CLAUDE.md
     section, less its replay gate.
   - **The exclusion file — built 2026-09-29.** `replay-layout.tsv`, 39 rows from what the savers
     `0x4876C0` and `0x4662F0` write, the header generated from it, and
     `tools/replay-layout-check.sh` before the link.
2. **With the skirmish slice** ([its plan](demo-recorder-skirmish-slice.md) cuts it into five landings): capture at `0x4954ED` and the recording header's lever list (the
   registry's list), with the recorder that uses them, and self-verification over units, players and
   declared tables — **S1, landed 2026-09-29**; root and sink counting; the main block and `.data` with the null twin; puppet
   dispositions and fixtures; the one-writer rule and getter 75 per seat; the nano spray's recording
   and playback; the landing run and the nightly tier with its timer, and with the harness the
   CLAUDE.md replay gate and the landing command's Step 4b.
3. **Later:** multiplayer dumps, `limits-tier2`, and a check that every changed executable byte lies
   inside a range some installer published.

## The CLAUDE.md section and landing gate

The section is in CLAUDE.md (*Simulation changes stay replayable*) since the declarations landed,
and CLAUDE.md is its only copy. Two parts wait for the harness, since until it exists they could
not run and every simulation landing would stop at escalation reason 3:

- the bullet **"A landing that changes simulation code** runs the replay fixtures of the features
  it changes (recorded, restored in place with the round trip, replayed on puppets) and records a
  `replay-roundtrip:` note. A change whose feature names no fixture fails by name. The broad tier
  runs nightly on `main` and before every `v*` tag.";
- the landing command's **Step 4b**, requiring a current `replay-roundtrip:` note on `main..HEAD`,
  from the fixtures of the changed features, whenever the diff changes simulation code, the
  layout, the recorder or a fixture (three attempts, then escalation reason 3 if a game cannot
  start).

The landing command already carries the rest: its Step 3 build runs the check, and its Step 5
brief asks for each new class checked against the code, new direct sink calls and new engine
writes on puppets, and every loosening.

## Decisions taken

| Date | Decision |
|---|---|
| 2026-09-29 | **Option B, declare and measure.** The panel rejected a static-first registry with a lint over raw writes as brittle, and a single simulation API as too large a migration before the recorder exists; a measure-only design leaves the "remember to register" gap the owner ruled out. |
| 2026-09-29 | **Extra weapons is always on and fails closed**, like every other simulation feature; its switch and silent disarm go, so recordings carry no arming dimension. |
| 2026-09-29 | **`s_quiet` is recorded, and a replay shows what the game showed**: display state that follows from simulation events is recorded whenever the live game displayed it. |
| 2026-09-29 | **Effects only the owner's machine produces are recorded where the engine produces them and replayed through the same function.** The first case is the nano spray: two emitters (`0x4720D0`, reverse `0x472200`) called from 17 sites in 15 order handlers, which run only through the main-list order controller in a local owner's unit tick (DIS). Each emission is credited to the unit whose `QueryNanoPiece` (`0x43E400`) produced its start point: the handler's unit, or the repairing pad in `SelfRepair`. It is recorded as that unit's spray state: direction, target box, and the ticks it sprayed on. Playback calls the same emitter for the puppet, taking the start point from the puppet's own `0x43E400` and bounding the box to the map. An emission that no such query precedes is a named failure. Any other effect the harness finds missing on puppets gets the same treatment, never an imitation. Rejected: deriving sprays from recorded orders (15 handlers, each gated differently, wrong in a stalled economy) and accepting the gap. |
| 2026-09-29 | **Getter 75 answers on a puppet what the recording machine answered, per seat.** The recording keeps one bit per player, *simulated on the recording machine*, and in puppet mode getter 75 answers that bit for the queried unit's owner. Puppets therefore show the owner-only indicators the recording machine showed, drawn by the mods' own scripts. Two rules come with it, needed whatever 75 answers. **One writer in puppet mode:** COB's SET, ATTACH and DROP change no engine state on a puppet, and the recorder alone writes `+0x10E`, `+0x10F` and the carry links; it applies `+0x10E` through `0x48B090` so each recorded change starts its scripts once. **Takeover flips the seats, then re-creates the units:** only a create gives fresh COB, and Create's first step reads 75. Rejected: accepting 0, which silently drops Escalation's and Twilight's owner-only indicators (375 branches), and replaying per call, since no invariant pairs a recorded call with a puppet call. |
| 2026-09-29 | **Each landing runs the replay fixtures of the features it changes; the broad tier runs nightly and at every tag.** A landing that changes simulation code runs only the fixtures its changed features name beside their code, each recorded, restored in place with the round trip and replayed on puppets; a change whose feature names no fixture fails the landing by name. The broad tier (the skirmishes, the network games, restores at random ticks, the limits tiers) runs nightly on `main`, from a timer built with the harness, and before every `v*` tag beside the compatibility suite. Always-on self-verification names any mismatch in every recording in between. Rejected: the full tier on every landing that touches a simulation file (10–15 min of most of the reference setup, a third of them not simulation work), and the tier only when a landing declares new state (code-only changes such as B9 wait a day, and until the mechanism exists "declares new state" is a judgement). |
| 2026-09-29 | **The CLAUDE.md section and landing gate are adopted with the mechanism**, in the landing that brings the declarations and the build check, which classifies every existing static once, with no exemption list. Adopted now, the gate could not run (no harness: every simulation landing would stop at escalation reason 3), half the rules would have nothing to write against, and the `sim` word would break today's build. The draft is revised for decisions 1–3. Rejected: review-checked shape rules until then, which nothing enforces. |
| 2026-09-29 | **Install-time plumbing is a fifth class, `SIM_INSTALL`**, declared beside its definition like everything else: never recorded or restored; the harness hashes the class after install and fails by name when a game changes any of it. Locks get their own declaration (`SIM_LOCK`) and are the only part left out of that hash. Rejected: moving the plumbing out of the simulation files. |
| 2026-09-29 | **Four files are simulation code**: `tagpu_patches.c`, `tagpu_weapons.c`, `tagpu_datakeys.c` and `tagpu_cobtrace.c` are `sim`; every other file on `thread-split.allow` is `presentation`. |
| 2026-09-29 | **Step 1 is two landings.** The first brings the declarations, the wrappers, the build check and the CLAUDE.md section; the second, the exclusion file. Capture at `0x4954ED` and the recording header's lever list move to the skirmish slice, with the recorder that uses them; the CLAUDE.md replay gate and the landing command's Step 4b land with the harness. |
| 2026-09-29 | **The exclusion file's drop and timer rows are approved.** A player record's own network session and clocks are neither recorded as values nor compared: the transport ID `+0x04`, the host's number `+0x0C`, the message count `+0x10`, the load progress `+0x20`, the session flags `+0x21` and the controller as the local peer sees it `+0x73`, and the join time `+0x08`, the ping `+0x14`, the peer's GameTime `+0x18` and the last receive `+0x1C`. The recording carries the transport ID as the player's key and each change of the recorded controller, and a puppet host stamps `+0x18` itself. The viewer's own state is dropped — unit `+0x110` bits 4 (selected), 6–7 (two marks) and 22–25 (the build menu's page), and the player's DisplayTimer `+0xF8` — so a replay or a takeover shows the replay viewer's own selection and menus. **The viewed player's contact bits 8–10 are recorded**: the acquisition scan `0x40AA40` reads them every 30 ticks for every local seat, and between the radar pass's runs they are history, so a takeover restores what the recording's AI saw; a replay shows the recording viewer's radar picture. Rejected: recording the session fields, which differ legitimately between every peer and replay machine, and dropping the contact bits, after which a takeover resumes with different target lists. |

## Open items

Both for the skirmish slice:

1. **Does `0x48B090` play a sound?** A change of `+0x10E` bits 0 and 2 calls `0x47F780`, which
   queues a status line through `0x47FAD0`. Neither calls the sound entry `0x47F0C0` directly, the
   deeper calls of `0x47FD70` are not traced, and a source comment in `tagpu_patches.c` claims the
   chain. The answer decides whether a recorded `+0x10E` change applied on a puppet duplicates a
   recorded sound.
2. **Applying a recorded YARD_OPEN.** The live owner already passed `0x47D970`'s refusal, so the
   applier bypasses it and restamps as `0x47DAC0` does (`0x47C790`, `0x440A40`); neither function has
   been read.

Gaps stated, not closed: in a multiplayer recording, getter 75 per seat reproduces the recording
machine's view, so other players' owner-only indicators, which that machine never showed, are
absent. And a takeover's fresh COB resets what a mod keeps in COB statics and piece visibility
(for example Mayhem's upgraded metal extractor looks un-upgraded after a fork, while its attached
module and `+0x10E` are restored) [INFERRED from the scripts].
