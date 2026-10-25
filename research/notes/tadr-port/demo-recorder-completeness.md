# Demo recorder — completeness design

## Status

How the state recorder stays complete as Impure's simulation changes. **The declarations and
their build check, and the exclusion file with its check, are built** (the
[build order](#build-order)'s first step, two landings, 2026-09-29): every writable static of the
simulation files is declared, and the link refuses one that is not; the bytes of the engine's
unit and player records that are not recorded as values are listed in one file, and the build
refuses a header that does not match it. Self-verification is built with the recorder
([S1](demo-recorder-skirmish-slice.md#self-verification)), and the main block and `.data` are
recorded through a positive list ([S3b](demo-recorder-skirmish-slice.md#s3b-the-globals-and-seeking));
the replay harness is built — the puppet round trip and `tools/replay/tareplay.py` with its
fixtures ([S3c-1](demo-recorder-skirmish-slice.md#s3c-1-one-writer-for-the-tables-and-the-round-trip)),
the counts, the null twin's images gate and the address scan
([S3c-2](demo-recorder-skirmish-slice.md#s3c-2-counted-on-puppets-and-the-images-gate)), and the
features beside the code, the coverage build and the landing and broad tiers
([S3c-3](demo-recorder-skirmish-slice.md#s3c-3-fixtures-beside-the-code-and-the-gate)). The owner
chose **option B, declare and measure**, on 2026-09-29, after a design panel (four designs, two
judges) over the code as it stood; the product decisions it serves are on the
[product page](demo-recorder.md#completeness-and-verification), and the engine facts it rests on
are in the [state design evidence](demo-recorder-state-evidence.md). Addresses below come from that
panel's readers and were checked against the pristine binary by its judges unless marked
[INFERRED]. Every decision is taken ([below](#decisions-taken)). The rules are in CLAUDE.md
(*Simulation changes stay replayable*), the replay gate among them, and the landing command's
Step 4b enforces it ([below](#the-claudemd-section-and-landing-gate)).

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
grew the engine's COB object from `0x544` to `0x1144` bytes (`0x485D73`); the current
ten-record runtime uses `0x158C` bytes. Several pools now live in the DLL. COB execution
history is still reconstructed, rather than recorded, under the existing contract;
a larger native allocation does not change that replay limitation.

**What a skirmish stream holds.** The exclusion file says which bytes are recorded at all; a
second table, `tagpu/ddraw/replay-hold.tsv`, says which of the recorded unit and main-order-list
bytes a skirmish recording's stream may carry at an earlier value between two refreshes (every 300
ticks), each row a loosening with the readers it has on a playback host and its evidence. Every
byte no row names is exact at every tick, and at a refresh every byte is: *The exact stream* below
says how the harness proves it. `tools/replay-hold-gen.py` validates the table and generates
`inc/tagpu_rechold.h`, checked at the link like the layout's header, and each recording carries the
masks it was written with (`RT_HOLD`).

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
  `+0x8E`: the head is the member no other member points to. On a multiplayer level no recorded
  order is the truth, since each owner's image carries its own machine's, and the order is the
  host's (demo-recorder-multiplayer-slice.md, *The sort buckets' order*).
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
  The controller `+0x73` changes in play (the console's AI toggle `0x416280` flips 1 and 2, a leave
  writes 0), so the recording carries each change, while a puppet host holds 3 for every puppet.
- **A seek sets the viewer's DisplayTimer `+0xF8` to GameTime**, as the level init does: the draw
  path raises it by 30 whenever GameTime has passed it and then refreshes the resource bar, so
  after a rewind it would stay ahead and the bar would freeze until GameTime caught up.
- **Other writers on a replay host**, for the puppet dispositions the skirmish slice decides (on a
  puppet the recorder is the only writer): the radar pass writes bits 8–10 and clears bit 12 on
  every live unit on the viewed player's pass (`0x4674B6`); the scoreboard `0x4948E0` compacts the
  ranks `+0x148` from the draw path while it is shown. The receive timeout `0x453C20`, which reads
  every active type-3 record's `+0x1C` against the wall clock, is never reached in a skirmish: its
  one caller `0x455F64` is inside the network pump `0x453D40`, which returns at once unless
  `main+0x2A44` bit 0 is set (`0x453D4B`). On a puppet host the engine computes the contact bits
  for the viewed player (*Decisions taken*, 2026-09-30), and in the recording's own viewed player
  a playing then writes the recording's bits 8–10 after every step (2026-10-23; `tagpu_replay.c`,
  THE RECORDED VIEW'S CONTACTS).
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

**The main block and the executable's `.data`** (which holds the simulation RNG's seed,
`0x51FC88`) cannot be recorded or applied whole: `main` sits a GetTickCount pad into its
allocation, the block holds heap pointers, and `.data` holds the main pointer itself. So one
positive list, `tagpu/ddraw/replay-globals.tsv`, names every byte of the two a recording carries
and how a puppet host treats it — written every step, held (`hold`, for the takeover and the live
in-place restore that comes with it) or written with the engine's own reaction (LosType) — and the build checks the header
generated from it. **A field the simulation's steps write goes on the list** unless it is the
viewer's own or a cache of the draw; removing a row or making one `hold` is a loosening. What keeps
the list honest is that **a dump carries both images whole**: `tools/replay/tarec.py compare
--images` names every differing byte of two dumps that no row covers, so a simulation global the
list misses shows up by name, as a missing unit field does. It is a report until the harness's null
twin (below) sorts those bytes into presentation and the rest, and makes it a gate. The projectile
pool is a record class of the exclusion file, `proj`, whose pointers are IDs.

### Impure's own state: declared where it is defined

Every source file on `thread-split.allow` carries a scope word: `sim` or `presentation`. Five
are `sim` — `tagpu_patches.c`, `tagpu_weapons.c`, `tagpu_datakeys.c`, `tagpu_cobtrace.c` and
`tagpu_puppet.c` — and every other listed file is `presentation`, out of scope; only a source file can be `sim`, since a
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

**Restored at a seek.** A seek writes every recorded table back into the host's declaration of
the same name, stride, class, key, flags and layout — the recording carries a CRC-32 of each
declaration's layout (`tagpu_simreg_layout_hash`) — and skips any other. The write is the inverse
of the capture's image, by the layout alone: plain bytes and `SIMF_ID`, `SIMF_TARGET` and
`SIMF_PINNED` fields from the image, a `SIMF_ADDR` with an ID kind translated back and bounded,
and `SIMF_LOCAL`, `SIMF_OWNED`, `SIMF_COLUMN`, an `SIMF_ADDR` of no ID kind and a heap table's row
count left as the host holds them. **A sim file whose tables carry an invariant their layout
cannot express provides its `_restored()`**, which re-establishes it on the game thread before
anything runs: `tagpu_patches_restored()` (the air pool's triple under its lock, the hit delay
queue's head and count, the ghost sweep's count) and `tagpu_weapons_restored()` (each weapon
definition row's count and pointers, each cached piece). Each names the reader its bound protects.
Between two seeks the tables hold what the host's own code writes, which differs from the
recording where that writer is gated on a puppet host (`s_quiet`); S3c sorts each table's writers.

**What playback leaves to the host's engine, and where that stops holding.** Playback's own masks
(`masks_init`, `tagpu_replay.c`; the recording carries the bytes) leave a few engine bytes the host
derives from the same history as the recording machine — a unit's sight stamp's fields `+0x7A`,
`+0x7C`, `+0xF8`, and the state byte `+0x10E`, which the state events carry — to the host, which
writes them itself. The water state `+0x10A` is out of the raw write too, and since format 23 the
playing writes it from each image of a unit with a mover (its one script, `setSFXoccupy`, is a
recorded start) and from the create event; the bookkeeping below then finds it equal. Three things cut that history, and in each the replay sets
them from the recording rather than masking more: a seek (R8b: all three, the stamp through the
engine's removal and stamp so the sight counters stay exact); a create in a slot dead at T (the
create event carries the water state its unit took from its slot); and a unit the recording machine
made after its players phase, which the host meets one step early (`books_put`, at every capture
point, counting each write it explains as `made` and noting any other). An unexplained write is a
finding, never grounds for a mask ([the slice
note](demo-recorder-skirmish-slice.md#s3b-the-globals-and-seeking)).

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

**MEASURED 2026-10-05: native cosmetic pools after seek.** The resource landing's whole
rerun passes 1479 checks and 36 image gates; the unchanged `stock-secondary-orders` control
alone fails 514 unlisted image bytes in native explosions, debris and flying-piece slots.
Live/ordinary playback at T451 have 15 active explosions, fresh seek has zero; every unit,
order, effect-call, paused-state and restoration check passes. A focused fresh-worker rerun
reproduces 469 failing bytes in those same pools. `seek_run` calls `tagpu_effects_clear`;
the native paths clear the count, reinitialise debris and zero flying-piece slots. The raised
engine-pool declarations are also excluded by `tagpu_simreg_recorded` until their layouts
are implemented. These paths are unchanged from the landing's base.

**Owner-approved known gap, 2026-10-06.** Only `stock-secondary-orders`, on its native
300-projectile build, enables `images-gate --stock-secondary-cosmetics`. Otherwise failing
unlisted bits within exactly `main[0x1491B,0x1AB8F)`, `main[0x1AB9F,0x1E88F)` and
`.data[0x511DF0,0x511F80)` are reported as `known-gap`, with their accepted byte count.
The address scope is 41204 bytes; it is not the measured number of differences. Adjacent bytes,
other fixtures and raised-limit recordings retain the strict gate. No recorder layout,
positive globals row, unit/player/order comparison, paused comparison, restore check or
root/sink effect comparison changes. The saved whole-run and focused dumps pass only with
this explicit option; the default still fails their original 514 and 469 bytes. Six portable
policy tests cover the bounds and fixture/build/command restrictions. Native and raised
cosmetic-pool snapshot layouts remain separate work; this exception does not establish that
pool fullness is irrelevant to simulation.

The subsequent full landing run passes 1480 checks, 519 exact paused comparisons and all
37 image gates, with coverage reaching all 11 changed function/region requirements. Its
native control's T301 checkpoint accepts zero cosmetic bytes and also passes the default
strict comparator. That checkpoint does not erase the earlier reproduced T451 gap. All
17 resource image gates remain strict. The passing note is on `cfe3771f` (2026-10-06).

### Events

Events are counted where they start (roots: the creates `0x485F50`/`0x4861D0`, the destructor
`0x4866D0`, the script-start broadcasters `0x456190`/`0x456200`/`0x456290`, the fire functions,
build completion `0x41B8D0`) and where effects are produced (sinks, counted by caller: `0x420A30`,
`0x470EB0`, the COB lookup `0x4B08C0`, the sound entries `0x47F0C0`–`0x47F300`). Each function gets
one shared hook through the fail-closed table, so a clash with another feature's hook on the same
function (B5's on `0x4861D0`, B6's on `0x490C40`) is refused at install and names both. New effects
start through an engine root, never a direct call to a sink.

**A script slice is an event when a start precedes it.** The unit tick's `DoScriptsNow(1)` is
the only clock a COB thread has, so where the slice falls among the step's events decides when a
start takes its first slice. The recorder writes `RK_EV_SLICE {ID}` just before the slice of any
unit whose COB object had an engine start (`0x4B0B00` or `0x4B0940`) since the capture point, and
a playing gives the slice at that marker instead of in the host's unit tick (*COB slices a playing
gives one tick late*, below).

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
player's recording logs and counts it. Playback calls the same emitter for the puppet with the
recorded point, box and layer, the layer bounded by the ten particle layers first. The
engine facts are in the engine map's *The nano spray*
([engine map](../exe-reverse-engineering.md)). **The owner's COB queries** are the second case:
`QueryBuildInfo`, `QueryNanoPiece`, `QueryTransport` and `QueryLandingPad` are made only by order
handlers, and a query runs a thread of the unit's COB to its end at once, so the recorder writes
`RK_EV_QUERY {ID, the query}` where the engine makes each, and a playing makes the same `0x4B0BC0`
at that place with the engine's arguments
([the engine map](../exe-reverse-engineering.md#the-owners-cob-queries-disassembled-2026-10-12)).
The fifth `QueryLandingPad` call, `0x41189C`, is in a function nothing calls.

**COB on a puppet has one writer.** In puppet mode COB's SET, ATTACH and DROP change no engine
state, and the recorder alone writes `+0x10E`, `+0x10F` and the carry links; it applies `+0x10E`
through `0x48B090`, which returns early on an unchanged byte, so each recorded change starts its
scripts once. Script starts are recorded at the five broadcaster call sites, and where the engine
starts a script that only the owner's machine does: `HitByWeapon` (`0x489F43`) and `TakeDamage`
(`0x489F8E`) in the hit, which a replay host never applies, `TransportPickup` (`0x406834`),
`TransportDrop` (`0x4069BF`), `BeginTransport` (`0x4113EF`) and `EndTransport` in the transport
orders, `setSFXoccupy` (`0x43DBE8`) in the water state `0x43DB50`, and `TargetCleared`.
`BeginTransport` runs at once on the recording machine while its broadcast, which a remote copy
starts later, is not recorded; and a puppet never runs `0x43DB50`, whose callers run only on the
owner's machine or a network peer: the playing writes `+0x10A` from each image (the
[engine map](../exe-reverse-engineering.md), *BeginTransport is recorded at its start*). A start a
script issues reaches neither and runs again on the puppet from its parent, so recording it would
double it. For the same reason a puppet's order nodes are
held disarmed, so that destroying one runs no owner code: `+0x06` bit 1 clear (the handler's own
call), `+0x42` bit `0x400000` clear (a second `StopBuilding`, `0x43A21E..0x43A25D`), `+0x52` NULL
(the handler's owned object), and `+0x42` bit `0x10000` set, without which the destructor resets the
unit's weapon targets through `0x489800` (`0x43A2AB..0x43A2B9`): the `puppet0` and `puppet1` rows of
2026-09-30. Getter 75 answers the recording machine's bit for the queried unit's seat.
The engine facts are in the engine map's *COB on a puppet*.

**A takeover ends a puppet level by design.** It is the one writer of engine state on a puppet host
besides the recorder, and it writes only inside the seek's TAKEOVER mode (`seek_run`): the seat
types, the AI's objects, the difficulty, the controlled and viewed players, the viewer's
deactivation, the deselection, the passability refresh and the objective. When it returns the
puppet table is off and the playing is closed, so the level is a live skirmish from T, recorded as a
new timeline ([the takeover note](demo-recorder-s4-takeover.md)).

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

### Terms

The harness has four nouns. They are the words the notes, the instructions and the runs pane use:

| term | what it is |
|---|---|
| **scenario** | a game setup in `scenarios/`: the map, the players, the units placed. A building block; several test groups can load the same one. |
| **test group** | one scripted game and the tests run on it: a scenario to load, actions at given GameTimes (apply another scenario, type a chat line, press keys, order a unit), the GameTime it ends at, and which tests it wants. Each is an entry of `tools/replay/fixtures.json`. |
| **test** | one way of playing a test group: one game, in a fresh process, on one worker. Every test group has **record**; the others play what record recorded. |
| **checks** | the comparison, offline and on no worker, of what a test group's tests produced; it starts when the group's last test ends and gives the group its verdict. |

A **worker** is one game instance with its private X server; `-j` sets how many tests play at once,
from any mix of test groups. The tests:

| test | id | starts from | plays |
|---|---|---|---|
| **record** | `L` | the test group's scenario | the script live, and records it |
| **puppet** | `P` | record's recording | from the start, every unit driven only by the recording |
| **roundtrip** | `R` | record's recording | as puppet, restoring the state in place from the recording at points on the way |
| **seek** | `Q` | record's recording | from one point, in a fresh process that played nothing before it |
| **takeover** | `K` | record's recording | to a point, then hands control back to live play, recording the new branch |
| **follow** | `F` | takeover's recording | from just before the takeover, across it |
| **forks** | `X` | record's recording | branches timelines off it, forks of forks, deletions, the chains played |
| **rewind** | `U` | the `battle` test group's scenario | the script live with rewinds, as its own recording |
| **watch** | `W` | record's recording | opened as a player opens it, through the Replays browser and the HUD |

Record runs first because every other test but rewind plays its file; follow waits for takeover.
In a gate a test group plays record, puppet, roundtrip and seek, with takeover and follow, forks,
rewind or watch where its entry says so (`procs`); a `cost` test group plays record alone,
`tareplay.py measure` record and puppet, and `tareplay.py takeover` record, takeover and follow. **The code and the older text still say fixture and lane**:
`fixtures.json`, `fx`, `LANE_ORDER`, the tests by their one-letter id, `SIM_FEATURE`'s `fixtures_`,
`tareplay.py run <fixture>`. A fixture is a test group and a lane a test; they are renamed in one
pass with the code. "Lane" elsewhere, in the recording codec (`tagpu_reclanes.h`) and the
renderer, is unrelated, and so is a "fixture" that means a synthetic test input (a C unit test's).

### What it runs

`tools/replay/tareplay.py` runs test groups in parallel, as the compatibility suite runs games:
one tacli instance and one private X server per worker, nothing stopped by name, and exit 0
(every check passed), 1 (a named failure) or 2 (something could not run). Its core is built
([S3c-1](demo-recorder-skirmish-slice.md#s3c-1-one-writer-for-the-tables-and-the-round-trip),
which lists the processes, the checks and the test groups): each test group of
`tools/replay/fixtures.json` is recorded live, played on puppets from the start, played again with
three round trips, and sought from a fresh process, and every dump is compared with the recording
or with its pair. The trigger `tagpu_replay.trigger` is read on the game thread by the replay's
frame visit, before the in-play draw and on paused frames too; it knows `dump <name>` (the next
capture point), `dump <name> at <T>` (the capture point of GameTime T exactly, whatever the speed or
pauses, since a puppet host's speed keys change nothing), `dump <name> now` (a frame dump, at
once), `seek <T> [dump <name>]`, `hud seek <T>` (the replay HUD's timeline's own seek entry,
`tagpu_replay_seek_request`, which takes no dump), `roundtrip <name> at <T>`, `turbo on` and `turbo
off`, `pause at <T>` and `resume` (*Test speed* below).

**The exact stream.** A skirmish recording's stream is lossy from format 13 on
(`tagpu_reclossy.c`): the bytes `replay-hold.tsv` names may keep an earlier value between two
refreshes, so a puppet's dump is compared with the lossy file and a live dump is not. The L and K
lanes launch with the test lever `tagpu_rec.exact`, under which the writer follows every stream
block with the exact records it filtered (`RB_EXACT`). L's dumps are compared with that exact
stream (`tarec compare --exact`), and `tarec lossy-check` walks the two streams side by side and
fails by name unless, at every iteration, every byte the hold masks leave clear is equal, every
held byte is the stream's previous value or the exact one, the iteration's nano sprays -- the
stream's and the ones its spray sessions imply -- are the exact stream's as a multiset and each
unit's in order, the two worlds are equal at every refresh and every refresh sits on its grid and
ends its block. That clause is Python's; the DLL's own reader meets the same check in
`codectest.py sprays` (the Windows build links `tagpu_recreader.c`), which compares every
iteration's sprays with the exact stream's, read in order and after a seek to each snapshot, and
`codectest.py spraycases` runs it on built files, including the ones the reader must refuse. So lossy-check proves the lossy
stream against the exact one at every iteration; the exact stream is proved against the live game
only where something compares them, at L's dumps and by the writer's own verification
([self-verification](demo-recorder-skirmish-slice.md#self-verification)), not at every iteration. K's dump
is compared with the fork's exact stream, F's with its lossy stream, and timeline 1 is
lossy-checked; with `RECL_CELLS` lossy-check also requires every cell's shown bytes exact, its
held slots and yard bit the stream's last or the exact ones, and the tick's input runs dropped
only where they differ in those alone. A puppet's straight dumps take their cells' slots and yard
bits from the exact stream; a seek's are checked by the occupancy rule (*Decisions taken*,
2026-10-12). The fixtures with a takeover launch every process with `tagpu_rec.refresh 30`
(*Taking over*: a takeover of a segment that holds something starts at a refresh). **A hold row is a loosening**, the owner's
decision like an exclusion: it names its readers on a playback host and its evidence, and a reader
found there that shows the byte removes the row. The table's rows, the owner's (2026-10-11), hold
the three weapon slots' aim and target state, the unit's resource ledger's accumulators (`+0xBC`
to `+0xC8`, `+0xD4` to `+0xE0`) and the main order list's handler state (`+0x05`, `+0x06`, `+0x0A`,
`+0x2E`, `+0x3E`, `+0x4E`, `+0x52` and most of `+0x42`); a slot's distance, read by the shot builder
on a playback host, and the node flags a playback host reads stay exact. On the heavy hour they take
the file from 42.15 to 30.53 MB (MEASURED offline, the writer's path at a refresh every 300 ticks;
[size](demo-recorder-size.md#l5-the-lossy-plan-measured-offline)). The stream also leaves out the nano sprays its spray sessions imply,
which every reader makes again with the same bytes after the iteration's events (format 15,
`RK_SPRAY_SESSION`; the order of the sprays inside an iteration is the decision of 2026-10-11
below).

**One run at a time.** Every `tareplay.py` command that plays games -- `run`, `measure`,
`landing` (not `--check`), `broad`, `takeover`, `forks`, `rewind` -- first takes the machine's run
lock (`tools/runlock.py`, flock(2) on `tareplay.lock` in the common git directory), so a second
run on the machine waits for the first; the kernel drops the lock when its holder exits. One gate
alone peaks at a load of about 24 of the reference setup's 32 threads; four at once took 34 min
each against 11 alone and failed checks bounded in time for the load alone (MEASURED 2026-10-12).

**Test speed.** Tick-driven runs default to internal game speed 20 (2x normal),
within the retail setter's range. `--game-speed 10` selects normal speed for
`run`, `measure`, `landing`, `broad`, `takeover`, `forks` and `rewind`. Every launch
configures only the stopped worker's private registry store through `tacli
registry`; its acknowledged dword is checked before launching. Cost fixtures and W's
speed/settings UI test use normal speed. The mouse-button ring's indices depend on when a dump
falls against the viewer's clicks, and the null twin cannot classify every bit of them. MEASURED
2026-10-09, resource-geo-upgrade at speed 20: one run in three failed the images gate at its first
point, GameTime 451, a dump taken as a selection click of its script reached the engine, on one
byte: 0x51F4B2, the ring's read index, 9 live against 6 played and sought. The images gate counts a
bit as the viewer's only when the twin's own dumps differ there, and a click is two ring entries,
its press and its release (`0x4C2E30`, from the window procedure's `0x201..0x206` arms), so the
twin's whole clicks move the read index by even steps of a ring of 20 and never change its low
bit; 9 against 6 differs in that bit. DISASSEMBLED 2026-10-09: the indices and the GUI's copy of
the popped event are viewer input state the simulation never computes from (the
[engine map](../exe-reverse-engineering.md#gui-only-writes-found-by-the-resource-replay-fixtures)
has the writers, the readers and the one route from the step), so they are `leave` rows of
`replay-globals.tsv`, the owner's (2026-10-09): `mousehead` (`0x51F4AE`), `mousetail`
(`0x51F4B2`), `guimsg` (`main+0x565`), `guidblclick` (`main+0x569`) and `guipopwparam`
(`main+0x56D`), four bytes each, as `keyhead` and `keytail` are for the keyboard ring. The fixture
runs at the default speed. The target speed is
reported, and a passing landing note records it. Armed round trips bound the scheduler's batch
before the step caches it, making the frame after T−1 available even when a frame would otherwise
run several ticks. The guard is on the game thread and affects only puppet pacing while a round
trip is armed; an unavailable guard refuses the round trip. The
[engine map](../exe-reverse-engineering.md#the-scheduler-the-clock-and-the-test-pacing-disassembled-measured-2026-10-08)
gives the scheduler's disassembly and the measured tick rates.

**The turbo.** Speed 20 runs about 60 ticks a second because the clock steps 30 times a second
and grants each step two ticks; the frames, ~180 a second in play, are mostly idle. A test
launch's `tagpu_turbo.on` (its text k, 1 to 5, empty for 1; honoured only with the per-instance
registry store, refused with a line otherwise) arms the turbo: once the trigger says `turbo on`
the observer at `0x49692D` writes k into DeltaTime every frame, so the level runs k ticks a frame
as fast as frames come, until `turbo off` or the level's end. The step itself is unchanged — the
same per-tick function, called k times. The harness (`--turbo K`, default 1, 0 for none) sends
`turbo on` once a lane's dumps and round trips are armed and acknowledged, on L, P, R, F, K, X and
U; not on Q, which plays one tick, nor W, whose ticks pass between its interface steps; never on a
cost fixture, which measures the game's own pacing. The recorder's ring never blocks the capture,
so a turbo frame runs no tick while the ring holds more than half its 16 MiB: the step is then
skipped whole, and every batch is still k ticks (*the writer's back-pressure* in tagpu_replay.c;
without it the stock-limits battle at 314 ticks a second filled the ring by GameTime 1003).
MEASURED 2026-10-08 at k = 1: a light L lane about 1,500 ticks a second, P and R 200–385 (the
puppet's write is the cost), the battle's lanes 64–230 with up to 1,652 frames held for the
writer, against ~60 at speed 20.

**Why k = 1.** The step's tail — `0x428BD0`, `0x428BE0`, `0x428BF0` (each a bare `ret`), the chat expiry `0x463EF0`
and the temporary sight records' expiry `0x482130` — runs once a call of `0x495490`, not once a
tick, so where the batches fall changes what units see and so what they do. Speed 20 mixes batches
of two with an odd one after a slow frame, and the mix differs run to run; a fixed k makes every
batch the same on every run. MEASURED 2026-10-08 (tenseat taken over at 931 as player 0, dumped
at 2731): of two pairs of takeovers at speed 20 one parted in 1,328 unit fields; pairs at a fixed
k (2, then 1) agreed in everything but the meteor shower's two points (*Open items*); a takeover at
speed 20 against one at k = 2 differed in 1,272 and 1,383 unit fields. k = 1 is the batch a player
at normal speed gets. The lever keeps 1..5: five is the scheduler's own clamp (`0x4953F5`), so no
batch is one stock pacing never produces.

**Ticks, not polls.** No check is decided by the wall clock: a loaded machine runs every lane
slower, never differently. The held lanes (L, P, R, F, K, X and U; `held_lane`) launch with
`tagpu_hold.on`, honoured on a test launch only, and **the launch's hold** (`tagpu_replay.c`, THE
LAUNCH'S HOLD) bounds the level's first batch to one tick at `0x49692D` and sets the pause bit at
the frame visit after it: a skirmish is held at GameTime 1 with its scenario applied on the held
frames (the applier hook `0x4969D2` lies on the paused path; MEASURED 2026-10-09, `resource-mex`
held at 1 with its 8 units applied), a playing at its first step — or, when the launch's seek is
already pending there, once the seek has rebuilt the level at T−1 — and a takeover or seek asked
for later runs on the held frames and leaves the level held at T−1, so nothing a lane arms can
pass before its answer. From there a lane
acts at GameTimes: the trigger's `pause at <T>` sets the pause bit at the frame visit after step
T (the same observer bounds the batch so the step stops at T exactly), `resume` clears it, and
**the hold keeper** at the pause test `0x496920` puts the bit back before every test while a pause
of the DLL's is taken, so neither the Pause key nor the in-game menu's OK lets a held level run.
An L step — keys, a chat line, an order, a scenario — is given while the level is held at its T,
then the level resumes; the twin (Tlast + 30) and the end scenario (at the fixture's end) are held
ticks too. A `run` step, a script, is handed the level held at its tick with the turbo on,
advances it by exact ticks itself (`pause at` and `resume`, `resource_regression.py`'s IN
LOCKSTEP) and hands it back held; a later step whose T it ran past could not run, named. K's
takeover returns held at T−1 (`tacli replay takeover` returns once the hold's line is logged),
arms its AI window, judges the units Impure draws 240 ticks after T, gives its order and chat
line there, and ends at its dump's GameTime; X's forks end at their dump's GameTime. U holds the
level at 601 and 1501, 1 (mod 30) as every dump of the harness is (the radar pass's step, O14),
and its two judges watch 3,600 ticks each, the units read 240 ticks in, the window and its boxes
throughout. A wait on a one-frame answer is bounded by 60 s (`ACK_S`); a wait on a running level
by 120 s of GameTime standing still (`STALL_S`, `Stall`): each catches a hung or dead game, never
an outcome. A peek the running game has not answered counts as no progress and nothing more: a
seek's rebuild holds the game thread for one frame, and two fork crossings' seeks in the battle's X
lane held it past `tacli peek`'s own 10 s at a load average of 63–87 (MEASURED 2026-10-12), so no
shorter bound stands in for `STALL_S`.

**A seek's work, not its time.** Every seek logs its work line (`sk_work_log`): the iterations
the reader read and where it built from, the blocks it decoded to get there, the image's units
live, made, refused, killed and left alive, and the game thread's CPU time (`GetThreadTimes`)
beside the wall's. The harness checks against the file (`work_verdict`) every seek of R, Q and F,
every takeover and seek of X's forks, K's takeover, and U's return to now and fork. Of U's seeks
inside an open rewind (to T1, T1 + 300 and T2) only the first one's CPU time is reported
(`rewind_works`), and none is checked. **W1**, exact, the reader started where the file's index
says it must (`tarec.seek_start`, the reader's own choice: the last snapshot of the chain's part
more than 123 ticks before the target, else the first iteration of the chain's first segment
whose part reaches 123 ticks before it) and read no more than from there to its target; **W2**,
exact, every live unit made or refused, none left alive by the kills, and none refused by a
takeover, a return to now or a fork; **W3**, exact, the blocks the reader decoded -- their count,
their bytes in the file and unpacked -- are the file's own from there to the target
(`tarec.seek_blocks`): the snapshot, after its segment's first stream block when the reader did
not hold that segment's base, then in each segment of the chain every stream block holding an
iteration it read, each once. A chained snapshot counts every snapshot of its chain from the key,
each read and unpacked through `block_body` (`snapshot_chain`). A sectioned block counts as one
block whatever its run: its header's packed and raw, the chunk and the container it unpacks to,
not the run's window; a seek starts only where a run does (after a snapshot, at a segment's first
iteration), so it decodes every run it reads from its `RBF_FRAME_RESET` block, and `seek_blocks`
refuses a file where it would join one in its middle, as the reader does. Q's and F's seeks run in a fresh process after its playing's first
step (a seek asked for earlier waits for it), which holds the root's base, so for them the file
says whether a base is read: only for a snapshot in a later segment of the chain. No check reads a
time. The game thread's CPU time is reported
only, because it grows with contention for the core: an SMT sibling made fixed work cost ×1.5
(MEASURED 2026-10-09, `tagpu_replay.c`, THE SEEK'S WORK), the all-core clock lowers it further,
and the battle's return to now at 1,501, the same 1,500 iterations and about 1,420 units each
time, took 3,990–5,250 ms of it across the lanes of one gate run (MEASURED 2026-10-12). Wine
9.0's `GetThreadTimes` counts kernel time and not `Sleep`, in 10 ms steps (MEASURED 2026-10-09).

**The gap W3 leaves.** It bounds what a seek reads, not what reading each record costs: a change
that made the reader or the seek's visitor slower per record, with the same blocks, passes the
gate. And it counts only the blocks `recr_seek` decodes through `block_body`, so two reads stay
outside the count: the segment scan `segs_scan` at the top of every `recr_seek`, which reads the
header and timeline blocks through `body_get` and grows with the file and its forks, and a
multiplayer seek's contribution blocks, which `mp_read` decodes through `body_get` too. The seek
and takeover times of the [state design](demo-recorder.md) (*Playback and seeking*) are targets
the gate does not check.

**The test render profile.** Every game the harness launches writes `style=custom`, `assets=0`,
`light=0`, `shadowres=512` and `ss=1` into the worker's `impure.cfg`, and a 20 fps present cap
(`tagpu_presentcap.on`, test launch only): the Classic++ look keys off, the engine's own draw and
its shadow switch as a player has them. A configuration players do not run, accepted for the gate
by the owner (2026-10-08).

MEASURED 2026-10-05 on the reference setup: at internal speed 20,
`secondary-orders` passes all 23 checks, `wpn-badtgt --cover` all 33 (W starting
at 10), and `takeover battle --cover` all ten reported checks, including fresh
executed AI orders and exact fork playback. The normal-speed
`secondary-orders` control passes 22 checks and fails its images inventory at
the `main+0x531` GUI pointer, which `replay-globals.tsv` now leaves with the viewer
(`guitop`, the owner's decision, 2026-10-06). Simulation runs
near 60 ticks/s at 20 and 30 ticks/s at 10; startup, pauses and comparison work
prevent inferring a whole-harness speedup from these runs.

**Games and checks.** `-j` (default 8, at most 16) is the number of games at once: each worker
plays one lane at a time. A free worker takes the queued lane first in the order X, U, K, L, W, R,
P, F, Q — the forks, rewind and takeover lanes are the longest chains and cannot start before
their fixture's L — and the L lanes are queued fixtures with those lanes first, then the longer
games. Every check runs beside the games on an offline pool (the cores less two per game, no more
processes than GiB available, at nice 10 so that the games' timed checks measure the game): a
fixture's checks start the moment its last lane ends, and the forks and rewind lanes hand their
file checks over and go on to their next game. A check handed over reads only files no later step
writes — a copy, where the forks lane goes on to change its file. Results are read back in the
order they were asked for, and the first failure in that order is the one reported, before any
failure the lane met later, so the summary lists the same checks in the same order with the same
verdicts as running each in its place. The decode cache keeps a file per segment
(`reccache.py`), so a file a takeover appended to decodes only its new segment. Launching a game
reads which other instances are running from one process listing (`tools/tacli`, `place_window`):
a `pgrep` per instance folder took 16.8 s at every launch with 506 folders.

MEASURED 2026-10-08 on the reference setup, `--cover`, the gate's seeds. Before, the landing gate
at `-j 12` (39 fixtures) took 52 min: 31 min of games, then 20.5 min of checks in one thread after
the last game (battle's alone 285 s), with battle's X lane 1278 s — each of its nine file checks
re-decoded the whole file (24 s) and checked each live timeline in turn — and its X, U and K lanes
queued behind every fixture's L for 7 min. After, alone at `-j 8`: battle 10.5 min end to end (X
262 s, U 427 s, its checks 67 s after its last lane), stock-limits and tenseat together 8.5 min
(X 230 and 227 s, U 409 s; a P, R or Q lane 25–35 s shorter, the launch's wait), every check's
name, order and verdict the same as the gate's. The whole gate at `-j 8` projects to about
21 min from those lane times (a simulation of the queue, which reproduces the 31 min of games
from the gate's own lane times) [INFERRED until the next landing runs it].

**Live rewind (U).** A fixture with `"rewind": true` also runs the
[rewind L/P/Q checks](demo-recorder-s4-rewind.md) in `tarewind.py`. Battle and
stock-limits use the battle on their respective DLLs. `rewind_steps` declares
additional scenario steps for the U lane's live game, so edits to
those scenarios select these fixtures for the landing tier. The current extra
step supplies AI builders before the comparison window, without giving them
construction orders. Fresh planning and normal dispatch are required per AI seat
after return-to-now and fork; construction is reported only. `tareplay.py rewind battle [--stock] [--cover]` runs this
lane independently. Its fresh seek uses `seek-frame T dump NAME`, which pauses
and dumps in the seek's frame visit. The approved scope excludes unseated sight
counters under D2 and reports/accepts VM START/CALL child counts across
return-to-now. Seated sight, root counts and puppet-write checks remain intact;
see *Rewind comparison scope* below. The U lane also uses the engine's `+kill 9`
with developer cheats temporarily enabled to disable objectives on an unused
seat, verifies return-to-now preserves that
switch, and verifies a fork arms objectives under the takeover policy.

**The player's way in (W).** A fixture with `"watch": true` in `fixtures.json` — `stock-limits` on
the stock-limits build and `wpn-badtgt` on the raised one, so the landing tier's coverage holds
both builds, and `perspective` and `tenseat` — runs a fifth process on its recording,
`watch_lane` in `tareplay.py`, which starts the playing as a player does and drives it through
the replay HUD, clicking its controls in device space with `tacli hud`
([The replay HUD in game](../hud-in-game.md), *Input*). From the main menu's REPLAYS it puts three files in the browser,
dated so their rows are known and each Watch checked against the file the browser logs it took:
a copy whose header names another `replay-layout.tsv` hash, which Watch's own checks refuse
(`watch_refuse`); a copy whose skirmish settings name a map no archive has, which the Start's
terrain check refuses after the seats are exchanged (`watch_undo` puts them back); each refusal's
notice is answered OK. Then the recording itself is watched; the HUD's `speed` changes the speed;
the in-game Options' OK (Tab, PREFS, PREV — the settings save `0x430F00`) must leave the
instance's registry store at the player's `gamespeed` while the playing keeps the HUD's; `play`
pauses (GameTime and the pause word are read to hold still); the trigger `hud seek <T1>` seeks
through the timeline's own entry; then, paused (`watch_hud`), `sight:0` twice (the first row's
seat, then the playing's viewer back, the clock and the camera unchanged), and on the row of the
seat the recording's human commanded `follow:` (`follow seat`), `follow:` again (`free cam`) and
`jump:` (`jump to seat`), then `cursors` off and on, the paused clock unchanged throughout; dump
`w<T1>` is armed at T1 and `play` resumes; `leave` and Yes return to the browser with the player's
speed back in the game and in the store; a second Watch plays it again, and its coverage file
carries the first level's teardown. `w<T1>` is compared with the recording by `tarec compare
--puppet`, as P's, R's and Q's dumps are, and must be EQUAL. The one S5 function no fixture reaches
is `watch_refuse_engine`, the notice for the engine's CD check `0x41D6A0`, which cannot refuse in
this exe: its first test reads `0x50289C`, 1 in the file and written by no instruction
(DISASSEMBLED), and returns `'.'`.

**The round trip runs on a puppet host** (decided 2026-10-01, [below](#decisions-taken)). At the
frame visit after step T−1 of a playing it takes frame dump A, puts the static `SIM_TRANSIENT`
declarations back to their values from the playing's first tick boundary, rebuilds the level in
place to T with the poison check, and takes frame dump B; `tarec.py compare-dumps` compares A and B
strictly — units and their order lists, players, declared tables, features and wrecks, the globals,
the projectile pool, the explored map and every player's sight counters — and A against B must be
EQUAL, with no exception: with nothing between them, a difference is the rebuild's. The seek makes
the viewed seat's temporary sight records again, so the viewed record's counters compare strictly
too, and the units' contact bits: the R process views the recording's viewed player, in which the
seek runs the radar pass and then takes `+0x110` bits 8–10 from the recording, since no image
reproduces the pass a playing holds (the skirmish slice's O14). So a round trip agrees at every
T, not only at the pass's step. The live in-place restore, which a
recording machine cannot do without the takeover's own steps, is built and verified with the
takeover in S4. Inside the rebuild, just before its table write (R12), every declaration the
recorder records is filled with `0xA5` outside the bytes its layout keeps, and a byte the write must
cover that still holds `0xA5` is named as residue and put back; no `0xA5` outlives R12.

**No vacuous passes:** the poison check and the TRANSIENT reset above, inside the round trip; a
fresh process seeking to the same tick must match, as must forward and backward seeks (S3b); the
**null twin** and the **images gate**; and the **address scan**. The slot-shuffled restore is
dropped: slots are the recording's identity everywhere (an ID is a row plus one), no product path
re-creates a unit in another slot, and a raw address restored raw compares equal, so a shuffle
cannot see an address recorded as a value; the scan looks for one instead.

- **The null twin.** In the live process, past `l<Tlast>`, tareplay pauses the game, takes frame
  dump `ln1`, then does what a viewer does without changing the game — the arrow keys a second
  each way, one wheel notch in and out at the map's centre, the cursor over the map and over the
  panel, the in-game menu (Tab, `ARMOPT.GUI`) opened — and takes `ln2` with the menu still open
  (its OK resumes the game). It also samples twelve paused views: GUI-consumed clicks through
  the event ring, explicit cursor positions and distinct camera positions. A click returns the
  injected pointer to screen centre; each sample therefore moves it separately and requires
  the native GUI's x/y/message echo before dumping. Zero, the largest in-range power of two
  minus one, and the last screen coordinate exercise every valid coordinate bit. A thirteenth
  view follows fixture-specific native selection; resource-refusals selects RSNRACK and requires
  the command panel to display its type. Each additional dump must
  have the same GameTime, and its carried simulation records must equal `ln1`. Returning to
  the same eye/ring index at the second endpoint alone would hide viewer-only writes.
- **The images gate** (`tarec.py images-gate`). Every bit of the main block and `.data` that no
  row of `replay-globals.tsv` covers, where the live game at T1 differs from the playing at T1 or
  from the fresh seek to T1, is in this order **presentation** (the twin's paused dumps differ there),
  **host-variant** (the playing and the round-trip process differ there at T1), **load-constant**
  (unchanged across [T1, Tlast] on the recording machine and on the puppet host), or a **FAIL**,
  named by address, `IMAGE_MAP` group and values and labelled *written during play on the
  recording machine only*, *written on the puppet host only*, *the seek leaves it otherwise* or
  *both write it, differently*. A bit equal at both ends of [T1, Tlast] on each machine counts as
  unwritten in play: the window cannot see a write undone inside it. `replay-globals.tsv`'s
  **`leave`** class (carried `leave`, puppet `-`) names every byte the recording deliberately does
  not carry, with its reason: the viewer's own (the camera, the GUI, the message log, the music), a
  wall clock, process state a fresh process differs in (the heaps, the allocators, the libraries'
  and the C runtime's state), a cache of the draw, or a level constant. Each is a loosening and the
  owner's decision; `replay-globals-gen.py` accepts the rows and generates nothing for them.
  The resource landing's first approved rows name exactly twelve viewer bytes: active GUI pointer
  `main+0x531` (4), latest GUI mouse-event clock `main+0x561` (4), and current bitmap-font
  pointer `0x51F524` (4). The generated recorded simulation slots remain identical.
  Coordinates retain their existing classification, and orders retain their existing comparisons. Ownership and native writers
  are established in the engine map's resource section.
  On 2026-10-05 the owner additionally approved the two four-byte keyboard-ring
  indices, enqueue `0x51F48E` and dequeue `0x51F492`. Native keyboard and GUI
  input consume this local ring; the completed-Moho replay had drained indices
  9/9 live and 0/0 on playback and seek. Only those eight index bytes receive
  leave rows; builds, orders, unit/player state and effect checks retain their
  existing comparisons. The engine map records the disassembled stores and readers.
  The owner then approved exactly ten further UI bytes: native mouse button/modifier
  wParam `main+0x55D` (4), saved signed gadget status `main+0x11E7` (4), and the
  displayed unit type `main+0x37E9E` (2). Their disassembled writers own GUI input
  and command-panel state. Frozen native probes observe Shift flags 0/4 and
  ARMCK/ARMACK/CORCK types 32/4/165. The gadget-latch probe did not reach its
  paired comparisons; its ownership evidence is disassembly. These eight rows
  total 30 bytes; the 60 recorded globals slots / 78 dwords remain identical.
  The subsequent full landing run must still pass every ordinary state, effect,
  restoration, seek, image and reach check.
- **The address scan** (`tarec.py scan`). A dump carries Impure's DLL's base and size (`RT_DLL`)
  and each recorded table's layout (`RT_FIELDS`). Every recorded table's plain fields — all but
  `SIMF_ID`, `SIMF_ADDR`, `SIMF_OWNED`, `SIMF_LOCAL`, `SIMF_COLUMN` and `SIMF_OPAQUE`, each aligned
  dword of a table without a layout — fail when their value lies in the main block, the unit array,
  the unit defs, the projectile pool, the feature grid, the wreck pool, the explored map or the
  DLL; inside a pool of records only at a record's start. The globals image fails against the
  engine's ranges only, and the unit, player and projectile images' bytes outside
  `replay-layout.tsv`'s rows are tested against the engine's ranges and reported, not failed:
  stock code stores no address of Impure's DLL there, and their aligned dwords straddle pairs of
  16-bit fields whose value lands in the DLL's 77 MB about 1.8 % of the time (the
  [engine map](../exe-reverse-engineering.md#the-demo-recorders-counts-and-the-steps-phases-sinks-impacts-emitters-and-the-images-gates-map-disassembled-measured-2026-10-01)).
  **`SIMF_OPAQUE`** declares a plain value whose bits are arbitrary — a random generator's state, a
  hash — recorded and compared as any other and passed over by the scan, which cannot tell such a
  word from an address (`s_windState`, `tagpu_patches.c`). The globals image's counterpart is the
  `opaque` value class of `replay-globals.tsv` (its optional tenth column), which the simulation RNG
  `rngseed` (`0x51FC88`) carries: its dword is recorded, written on a puppet and compared byte for
  byte as before, and the scan passes over it. It knows the class only through this checkout's
  list, so a dump made with another one is scanned whole, and there the seed can still land in the
  main block, about one in 9,000 [INFERRED: the block's `0x3924D` bytes among the generator's 2^31
  values].

**Also asserted:**

- **The counts** (`tarec.py compare-counts`, against `tagpu/ddraw/replay-absent.tsv`). Every dump
  carries how often each root, sink and observed site ran since the level's first capture point,
  keyed (site, caller, aux) — the caller the return address, `dll+offset` inside Impure's DLL, the
  aux a sink's script name hashed — counted on the level's game thread only and never while a
  seek rebuilds. A live game's counts are compared with its playing's, each as the change between
  its two dumps over [T1, Tlast). **Roots** — the create's exit, the destructor (a live slot only),
  the build done, the state setter (a call that changes the byte only), the two replies, the two
  nano emitters and the seven feature functions — compare per site, **exactly**. **Sinks** — the
  explosion add, the COB thread allocation, the four sound entries, the projectile init, the three
  shot paths, the impact and ten particle emitters — compare per (site, caller, aux) within the
  file's **band**, 5 % of the larger or 2 calls, whichever is more, less its rows: `absent` and
  `extra` rows (an all-or-nothing difference the owner accepted, each with its reason) and `same`
  rows (one effect the engine reaches through another path on a puppet host — a shot built by the
  fire function live and by the shot receiver on a puppet — whose listed callers are compared
  summed). The receiver builds from the shot event's start, which is the one the live build used
  for every fire function: `0x49D9C0` sends the shooter's centre in its packet, so the recorder
  records that function's build start from the send's frame ([engine map](../exe-reverse-engineering.md#what-a-shots-packet-says-of-its-start-0x49d9c0-0x49d270-disassembled-measured-2026-10-09)); a
  replayed shot that starts elsewhere meets other cells in the collision test `0x49B090`, which
  is what moved battle-los's slot-B impacts by three. The band is the owner's standard: a replay is a good approximation of what the game
  showed, not a tick-exact copy; the roots stay exact. A key table that overflows fails by name.
  The owner-approved bounded COB flare allowance applies only to
  `(0x472630, 0x420AE6, 0x4812D3)`: after its own band fails, the corresponding
  parent `(0x420A30, 0x4812D3, 0)` must be counted in all four dumps and pass its
  ordinary band. Both differences must have the same direction, the flare
  difference must not exceed the parent difference, all four counts must be
  nonnegative, and each flare count must not exceed its parent count. Accepted
  cases report both pairs. This is a count tolerance based on at most one flare
  per parent, not proof identifying the individual missing calls; see
  [the measured case](demo-recorder-s4-rewind.html#the-naval-flare-count).
- **The script slice's count root** `0x48ADEB`, counted by `tagpu_slice_give` with aux 0 in the
  unit tick, 1 at the recorded marker, 2 at a playing's step's end: compared per aux, exactly, like
  every root. A live game's aux 1 and 2 are 0; a playing's aux 2 is a debt its marker did not pay.
- **`--cob-exact`** (a fixture's `"cob": "exact"`): the COB thread allocation's VM children, the
  START (`0x4B18C0`) and CALL (`0x4B192D`) callers keyed by script name, compare exactly instead of
  within the band. It runs only on the playing's lane against the live one (not across a rewind,
  whose VM children `--rewind` accepts). The gate's fixtures that carry COB set it:
  `slice-walkers`, `battle`, `tenseat` and `codec-decline`.
- **No COB engine write on a puppet**: SET's ACTIVATION and ARMORED (`0x48B090`, returning to
  `0x480B57`, `0x480BF1`), SET's YARD_OPEN (`0x47DAC0`, `0x480BB3`), ATTACH and DROP (`0x48AAC0`,
  `0x4813AB`, `0x481427`), counted on a puppet's unit outside the replay's own calls; the first 16
  logged by unit, owner and GameTime. The playing's, the round trips' and the fresh seek's dumps
  must hold 0.
- **No native owner-only start on a puppet**: the fifteen calls `tagpu_recorder.c`'s
  `START_SITES` observes, counted at each call. On a puppet level only the replay starts those
  scripts, through `0x4B0A70` itself; the playing's, the round trips' and the fresh seek's dumps
  must hold 0 at every site ([why](demo-recorder-multiplayer-slice.md#exactly-once-private-children)).
- **No native owner's query on a puppet**: the six calls `QUERY_SITES` observes (`0x4027FB`,
  `0x4113B1`, `0x411A35`, `0x411AA6`, `0x411BF5`, `0x411CF1`; `tarec.py`'s `OWNER_QUERIES`), counted
  at each call. Only the replay asks those queries on a puppet, through `0x4B0BC0` itself, so the
  same dumps must hold 0 at every site.
- No emission without its `QueryNanoPiece`; zero self-verification corrections (`tarec.py check`).
- **UNCOVERED.** The landing tier fails `UNCOVERED <file>:<function>` for a changed function none
  of the fixtures it ran reached, on the coverage build; the broad tier fails it for a feature none
  of its fixtures reached, a root or sink site no run counted, and a recorded table no recording
  changed between T1 and Tlast. The owner decides each landing that cannot reach a change (OQ7).
  A line that only a test program compiles is no game's to reach: a block under `#ifdef` of a
  macro in `tafeat.py`'s `TEST_ONLY` — `RECF_TEST_COPY`, the record frames' `copy`, which only
  the codec's self-test calls and only its command lines define — is in no DLL, and the landing
  tier asks for none of its functions. A path the DLL does carry but no fixture's game takes gets a
  test lever and a fixture instead: the codec's decline (`tagpu_rec.decline`, `codec-decline` and
  `stock-codec-decline`,
  [the slice note](demo-recorder-skirmish-slice.md#the-file)), which forces the encoder's first
  decline branch only; its decline in mid-block and its declines after the walk stay unreached.
- **A feature root is made at its phase of the step.** The recorder stamps each feature root with
  where in the step it was made — the unit tick, the projectile tick, or from the explosion tick
  on — and a puppet host makes it at the same point of its own step, so the host's projectiles meet
  the features the recording machine's met. The cells' unit slots of the next image go in before
  the host's projectile tick for the same reason. A simulation change that makes feature roots in a
  new place of the step needs a phase there, or the impact counts name it. The cost and size budgets
are reported, not failed, until the quantized stream lands, measured on a 10-minute four-AI game
with MB/h extrapolated; no tier runs that game yet. A 60-second takeover judged like the compatibility suite's battle comes
with S4.

**Multiplayer:** never rebuild in place on a live peer, whose creates would broadcast. Every peer
pauses and dumps, and each recording is later seeked from a fresh process and compared with its own
peer's dump. Not in S3c.

| Tier | Runs | When |
|---|---|---|
| Landing | the fixtures the changed features name in their `SIM_FEATURE` declarations, each recorded, played on puppets, restored in place by the round trip and sought from a fresh process, on the instrumented coverage build, with a check that they reached every changed function; a changed line no declaration covers or a changed function no fixture reached fails by name | every landing that changes a gated file (the landing command's Step 4b) |
| Broad | every fixture with three round trips, `limits-tier1` with 20, and a ten-minute four-AI game for the cost and size (reported), on the coverage build; UNCOVERED over the tier | a command, `tareplay.py broad`, not a schedule: on demand after a run of simulation landings, and as the release tier, `broad --release DLL` on the release DLL without reach, before every `v*` tag beside the compatibility suite |

Both tiers are built ([S3c-3](demo-recorder-skirmish-slice.md#s3c-3-fixtures-beside-the-code-and-the-gate));
`tareplay.py run` runs named fixtures. The broad tier's own entries, `fixtures.json`'s `broad`,
are `limits-tier1` (4 × 1,500 = 6,000 units) with 20 round trips and `cost-four-ai`, a 10-minute
four-AI game for the cost and size; no landing runs them. Not in S3c: network games and
per-peer dumps, `limits-tier2` (10 peers, 15,000 units), the compatibility setups and a game loaded
from a save. The landing run is scoped to the change because the broad tier holds about 28 of 32
cores and 11 of 12 GB of VRAM on the reference setup while it runs, and 38 landings touched the simulation
files in three weeks, 34 of them in one week at up to 10 a day (MEASURED 2026-09-29 from `main`'s
reflog); about a third of that week's were not simulation work but edits to the mixed
`tagpu_patches.c` [judged from their subjects and diffs]. Cross-feature breakage is caught by the
broad run before the tag, and always-on self-verification names any mismatch in every recording in
between.

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
2. **With the skirmish slice** ([its plan](demo-recorder-skirmish-slice.md) cuts it into landings): capture at `0x4954ED` and the recording header's lever list (the
   registry's list), with the recorder that uses them, and self-verification over units, players and
   declared tables — **S1, landed 2026-09-29**; the mover, the order lists, PlayerInfo and the
   events at their roots — **S2a, landed 2026-09-30**; the feature grid and the wreck pool —
   **S3a, 2026-09-30**; the main block and `.data` through the positive list, both whole in every
   dump, and the projectiles — **S3b, 2026-10-01**; puppet dispositions and fixtures; the one-writer rule and getter 75 per seat; the nano spray's recording
   and playback; the harness's core, the puppet round trip and one writer for the declared tables —
   **S3c-1, 2026-10-01**; root and sink counting, the null twin and its images gate, and the
   address scan — **S3c-2, 2026-10-01**; the features beside the code, the coverage build, the
   landing and broad tiers, and with them the CLAUDE.md replay gate and the landing command's
   Step 4b — **S3c-3, 2026-10-01**.
3. **Later:** multiplayer dumps, `limits-tier2`, and a check that every changed executable byte lies
   inside a range some installer published.

## The CLAUDE.md section and landing gate

The section is in CLAUDE.md (*Simulation changes stay replayable*), and CLAUDE.md is its only copy.
Its replay gate is the bullet **"A landing that changes simulation code runs its replay
fixtures"**: `tools/replay/tareplay.py landing` runs the fixtures the changed features name in their
`SIM_FEATURE` declarations — recorded, played on puppets, restored in place by the round trip and
sought from a fresh process, on the coverage build — checks that they reached every changed
function, and records a `replay-roundtrip:` note; a changed line no declaration covers, or a
changed function no fixture reached, fails by name, and landing it anyway is the owner's decision.
The broad tier is a command, not a schedule. The landing command enforces it as **Step 4b**, the
replay gate, between the documentation pass and the review: when the diff touches a gated file it
passes on `tareplay.py landing --check` (a passing note on a commit R, nothing gated changed in
`R..HEAD`), and otherwise runs `tareplay.py landing` on HEAD, three attempts at a failure, a game
that cannot start at all being escalation reason 3. Its Step 3 build runs the state and feature
checks, and its Step 5 brief asks for each new class checked against the code, new direct sink
calls and new engine writes on puppets, every loosening, and each new or moved `SIM_FEATURE`
region with whether its fixtures reach it.

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
| 2026-09-30 | **The mover's and the order node's rows are approved, with the class `puppet0`/`puppet1`**, for the skirmish slice's S2a: the mover's class object `+0x00` and `+0x2E` bits 3–7 `drop`, its movement class `+0x04` `cache`; the order node's list unit `+0x0E` and reference links `+0x1A`/`+0x1E` `cache`, and the draw's last-seen cache `+0x32`, `+0x34` and `+0x42` bit 21 `drop`. A `puppet0`/`puppet1` field is recorded and compared off a puppet, held at 0 or set on a puppet, and restored by a takeover: the node's `+0x06` bit 1, `+0x42` bit 22 and `+0x52` held 0, and `+0x42` bit 16 held set, so destroying a puppet's node only unlinks and frees it. |
| 2026-09-30 | **On a puppet host the engine computes the contact bits for the viewed player**: the host's radar pass runs for `+0x2A43`, the recorded bits are kept for a takeover and compared when the recording viewer's perspective is shown. This amends 2026-09-29's "a replay shows the recording viewer's radar picture": that perspective shows it, computed by the engine from the same inputs, and every other perspective shows its own player's radar, as the product page has it. Rejected: skipping the host's radar pass and writing the recorded bits, which shows the recording viewer's radar in every perspective, full visibility included. |
| 2026-10-01 | **The round trip runs on a puppet host; the live in-place restore comes with the takeover** (OQ1). This amends 2026-09-29's harness: the recording machine holds iteration k at the frame visit after step k while the world holds step k's result, so a live rebuild runs step k a second time against what no recording carries (each seat's AI and acquisition objects, the per-thread `rand`, the order nodes' handler objects), and making that safe is the takeover's own work (S4). The puppet round trip rebuilds in place with nothing between its two dumps, and the whole-image comparison the live one was for comes from the fresh-process seek of the images gate (S3c-2). Rejected: the live restore now, which would build half the takeover inside the harness. |
| 2026-10-01 | **The slot-shuffled restore is dropped** (OQ2) for an address-shaped-value scan of every plain-value field in a dump (S3c-2). Slots are the recording's identity everywhere, no product path re-creates a unit in another slot, and a raw address restored raw compares equal, so the shuffle could not see what it was for. |
| 2026-10-01 | **The `0xA5` fill and the TRANSIENT reset run inside the puppet round trip**, checked in memory, and no `0xA5` outlives the rebuild. As built (S3c-1), the residue test covers only the bytes the write must cover: outside the layout's keep, in the rows it writes, of declarations whose value it does not keep whole. |
| 2026-10-01 | **No scheduled nightly tier** (N1). The broad tier is a command, `tareplay.py broad`, required before every `v*` tag (with `--release` on the release DLL) and run on demand after a burst of simulation landings; no timer on the reference setup. This amends 2026-09-29's "the broad tier runs nightly". |
| 2026-10-01 | **The cost and size budgets are reported, not failed**, until the quantized stream lands (OQ5); the four-AI hour becomes a 10-minute four-AI game with MB/h extrapolated. **The broad tier leaves out** network games, `limits-tier2`, the compatibility setups and the saved game for S3c; the takeover run comes with S4 (OQ6). **The landing tier runs on the instrumented coverage build, the release tier on the release DLL** (OQ8). Network-only regions (OQ7) and the CLAUDE.md wording (OQ9): below. |
| 2026-10-01 | **A replay is a good approximation of what the game showed, not a tick-exact copy** (after S3c-2's first runs): debris that flies differently and particles in the right place are accepted. So the sinks compare within a **band of 5 % or 2 calls** per key and the roots stay exact (OQ3a, revised from 0); the shot builders `0x49C9C0`, `0x49CDE0`, `0x49CC20`, the meteor stone's init `0x49C740` and Killed compare by **caller-equivalence classes**, each exact within the band (OQ3b); **rows** for the eight owner-only aim and piece queries, the console key's click and the refused script indexes (OQ3c–e), for the flying pieces' explosions, smoke and fire (OQ3f: rows, not the RNG-at-EXPLODE recording) and for COB `EMIT_SFX`'s emitters; **the COB one-tick lag (A5) and the walk class after an unload (A6) are accepted residuals**; the lost impacts (A7) are fixed by the step's phases. The LEFT OUT prose becomes `leave` rows with its labels corrected, new `leave` rows name process state, wall clocks and the viewer's and the draw's state, and the message log is `leave` (OQ4a–d). **The scan** tests the engine's records and the globals image against the engine's ranges only, and Impure's tables against every range less fields declared `SIMF_OPAQUE` (OQ10). This revises the batch's first answer, band 0. Rejected: recording the simulation RNG at each EXPLODE so that debris flies identically, a recording feature for a cosmetic difference. |
| 2026-10-01 | **The declared tables have one writer on a puppet host** (O13, closed with S3c-1): the recording writes every table it carries at every capture point and every frame visit after a step, so a host's own write between two tick boundaries is overwritten at the next. |
| 2026-10-01 | **A seek makes the viewed seat's temporary sight records again** (O9 reversed for the seek, before S3c-3 landed): from the recorded deaths before T through the engine's add `0x482910`, then the engine's expiry, so a round trip and a fresh seek compare the viewed record's sight counters and contact bits strictly and `compare-dumps` folds nothing. The cap of 20 (a refusal counted and logged) and the engine's once-a-frame expiry are accepted residuals. A recorded LosType change still drops them on a playing. Rejected: naming O9 in the fresh seek's check too, which would mask contact bits 8–10 whenever the viewed seat lost a unit under true LOS in the 62 steps before T. |
| 2026-10-01 | **A landing no fixture can reach goes to the owner each time** (OQ7): a changed function in a region no skirmish fixture reaches — network-only code, error paths, the saved game — fails `UNCOVERED`, and landing it is the owner's call for that landing. Rejected: a standing exemption list. |
| 2026-10-01 | **The replay gate's wording is adopted as written** (OQ9): the CLAUDE.md bullet and the landing command's Step 4b, as built with S3c-3. |
| 2026-10-02 | **A takeover starts every order node from the constructor's state** (S4's D1, the s2-orders decision 8): `+5`, `+6`, `+0xA`, `+0x4E` and `+0x52` as the node's constructor leaves them, except the measured adoption of in-progress builds (BuildingBuild and MobileBuild at state 3 when their target is set). Every handler starts from that state, so it is the one safe by construction; keeping the recorded `+5` would resume handlers mid-state with `+0x52` NULL, and the destructor's event-2 dispatch for `+6` bit 1 is a named crash path. Orders restart: a patrol leg, an attack's acquisition, a reclaim. |
| 2026-10-02 | **The takeover checks' expected differences** (S4's D2, a loosening approved by the owner): the order-node fields of D1; the player records outside the fork's seats (the viewer seat and the records the seat fill deactivated); the controllers, `RK_VIEWER` and PlayerInfo `+0x94` where the takeover changed a seat's role; and the contact bits only where the viewed seats agree (the existing `compare --puppet` rule). Any other difference is a bug. |
| 2026-10-02 | **A fork is always recorded**, whatever `tagpu_recorder.on` says (S4's D3); a takeover whose file cannot be appended is refused before the rebuild, with the reason. **B4's per-slot columns are recorded** (D4, landing S4a). **The fork rules' last-used times live in the recording** (D5), appended when a watching session ends and skipped with a log line when the file cannot be written. |
| 2026-10-08 | **The harness runs a test-only turbo** (*Test speed*): k ticks a frame, k = 1 by default and 1..5 in the lever; a live lane acts at GameTimes by `pause at <T>` and the K and U judges are bound to ticks; every game gets the test render profile (a 20 fps present cap, `shadowres=512`). **The meteor shower's strike points after a takeover stay an open gap** (*Open items*). |
| 2026-10-09 | **No check of the gate is decided by the wall clock** (*Ticks, not polls*): a test launch's hold starts its level held (its pause bit and one-tick first batch written from `tagpu_replay.c`, a `presentation` file, on test launches only: a loosening the owner approved); a seek's cost is its game thread's CPU time, bounded by 4,000 ms with SMT and clock contention named as the residual; the fixture worlds change once, deterministically, as scenarios now land at GameTime 1; the compat suite's soaks and `--watch` count game ticks or presented frames (SECONDS × 30) with a 120 s stall bound. |
| 2026-10-11 | **A recording does not keep the order of the nano sprays inside one iteration** (OD5 of the lossy plan, a loosening the owner approved): a spray a unit's spray session implies is left out of the stream and made again by every reader after the iteration's events, units ascending. Every spray stays exact: the same ID, point, box and layer in the same iteration, the point the unit's own earlier spray's, moved by exactly the unit's position change when it follows the unit. Why the order can go: the emitters `0x4720D0`/`0x472200` change no simulation state and draw only the C runtime's rand, which a playing does not reproduce, and the order of a step's emissions decides only their particles' order in the layer -- the draw order and which one a full layer destroys first -- and, when the particle pool `0x51E610` that every emitter shares (1000 objects in stock) runs out within the step, which emission makes no particle (the engine map's *The nano spray*); the owner confirmed OD5 covers it, 2026-10-12. `tarec lossy-check` compares each iteration's sprays as a multiset and each unit's in order. Rejected: zeroing the point, which the game shows (the offline plan's step, −3.9 MB on a stream whose cells were already held, against −2.72 MB for the exact sessions over the unit and order rows). |
| 2026-10-11 | **A takeover's AIs read the recording's contacts at T** (O14 for takeovers): when P is the recording's viewed record, the takeover restores `+0x110` bits 8–10 from the recording after its radar pass, as the seek does, so the fork's first acquisition scans read the contacts the recording held at T, as the recording's AIs read them until its next pass; a multiplayer recording's takeover keeps its own pass's bits (below). Rejected: masking bits 8–10 in `fork_view` for every fork, a loosening of D2 (d) that would leave the fork's AIs reading a picture the parent never held; and steering the harness off the ticks after a scripted step, which hides the gap. |
| 2026-10-12 | **A5 and the in-game create lag are fixed by construction, not accepted** (the owner, after the level-start lag was traced to the same one-slice COB lag): a recorded slice marker, in order among the step's creates and starts, and the replay giving each marked unit its slice there, a format bump (format 20, *COB slices a playing gives one tick late*). This supersedes A5's acceptance in the 2026-10-01 row, which stays as history. |
| 2026-10-12 | **The slice marker's scope and its check** (the owner): in a multiplayer recording the marker covers the recording machine's own units only (M1: the starts the ledger orders keep their lag, and other owners' units are unchanged); and **`--cob-exact`** -- the VM's START and CALL children of `0x4B08C0` compared exactly, with no band -- runs on every gate fixture that carries COB, `slice-walkers`, `battle` and `tenseat`. Anything it catches is a replay gap to close by construction, never a band, an exclusion or an expected-absent row. |
| 2026-10-12 | **An owner-only COB query `--cob-exact` names is recorded and made again, not accepted** (the owner's standing decision for the slice marker, applied to `slice-walkers`' `QueryBuildInfo`, live 9 against puppet 0): `RK_EV_QUERY` carries `QueryBuildInfo` and `QueryNanoPiece` where the engine makes them, a playing makes each through `0x4B0BC0` at its place, and `QueryNanoPiece`'s `absent` row of 2026-10-01 goes, a tightening. AutoAim's aim queries keep their `absent` rows of 2026-10-01. |
| 2026-10-12 | **A failure only under load is a bug in the check** (the owner's rule): a peek the running game has not answered is no progress, so `STALL_S` is the one bound on a running level. **Approved by the owner (2026-10-12):** the seek's W3 is an exact count of the blocks the reader decoded against the file's own, with the game thread's CPU time reported only and the gap that leaves accepted (*A seek's work, not its time*); and bits 1 and 2 of the pause word `main+0x38A51`, the pacing gate's lag flag and its live speed below the chosen one, are left (`replay-globals.tsv`, `pacingbits`, a `leave` row: bit 2 has no reader, bit 1 only the corner icon and the turbo). |
| 2026-10-12 | **A skirmish recording holds its cells' unit slots and yard bit, and its playing's engine stamps them by the recording machine's contest rule** (OD4, option a; format 16's `RECL_CELLS`, *The cells held* in [making recordings smaller](demo-recorder-size.md)). On such a playing the grid and its contest marks, `+0x110` bits 26 and 27, have one writer, the three footprint writers: the playing writes neither, and the six contest `je`s are `90 90` from its first step to its end, so a contested cell keeps its incumbent as on the recording machine. A takeover writes the recording's slots, yard bits and marks at its refresh and refreshes the passability. **The loosening accepted with it**: after a seek -- the Q lane, a round trip's B dump, the dumps after one (`rt<T>c`), W, F and the forks lanes -- the slots, yard bits and marks are checked by the occupancy rule (`tarec.occupancy`, `--seek`): every footprint cell of a live mobile unit the stamp files (layer 1 or 2, the footprint on the grid by its bound with `fix_last_cell`) names it in its layer's slot, or, while this one carries the loser's mark, bit 27, names there a live unit of the same layer -- or a structure, which the stamp files in slot A whatever its layer, for a layer-1 unit -- whose footprint holds the cell and which carries the holder's mark, bit 26; every named slot is a live unit whose footprint holds the cell: a mobile unit of that slot's layer, or a structure in slot A (which cells of its footprint a structure holds follows its yardmap under the current mask, which a dump does not carry, so that is not checked); every yard bit lies in a live structure's footprint. A straight playing's slots are compared byte for byte with the exact stream's (`RB_EXACT`), or at a refresh. Nothing else is loosened. The residual: where the playing orders two contesting footprint calls otherwise than the game (deaths, creates and placements in slot order), the holder can differ. **Not decided**: the marks on a multiplayer playing, which still writes them from the recording beside the engine's slots, and its contests, which still follow the type-3 rule. |
| 2026-10-13 | **A step's walk start and water start may play in the other order on a puppet** (owner-accepted, with the walk-class residual A5/A6 of 2026-10-01, after B2's review): the recorded `setSFXoccupy` plays among the step's events, before the placement `0x48A9F0` and the walk class `0x43DA70` the images run, where the engine's `0x43DD20` runs placement `0x43DD55`, walk `0x43DD5D`, water `0x43DD65`; its run-now pass `0x4B0B86..0x4B0BA6` reads the previous step's position. Recording the walk class stays a separate idea for later (*Not closed by the marker*). |
| 2026-10-13 | **The transport, pad and waterline COB entries follow the owner's machine** (the follow-up to the slice marker the owner asked for): `QueryTransport` and `QueryLandingPad` join the recorded queries, `BeginTransport` and `setSFXoccupy` the recorded owner starts (format 23); the `transport-pad` and `waterline` test groups drive them in both builds. A tightening of the 2026-09-30 D2 (`BeginTransport` left to its broadcast, which a playing starts one slice late). The walk class's starts are not recorded (*Not closed by the marker*: the size). |
| 2026-10-23 | **On a multiplayer playing the contest marks are the host's footprint writers' alone, and its contests keep the engine's own rule** (the owner; this extends the 2026-10-12 row's exception and decides what that row left "Not decided"). The grid's unit slots and yard bits on a multiplayer segment are already the host engine's (`s_engine_cells`); `unit_put` and `states_put` now choose their masks by `s_engine_cells` too, so the playing writes neither `+0x110` bit 26 nor 27 there and the stamp, the clear and the re-claim are the marks' one writer, as on a skirmish segment that holds its cells. Why: each owner's image carries its own machine's marks, for its own grid at its own times, so writing them beside the host's grid left a host contest unmarked (MEASURED at GameTime 1016 of the r2 four-seat recording: cell (157, 82) named unit 10 and none of the 43 units carried a mark), and the clear's hand-back to a loser (`0x47D29C..0x47D2C4`) and the re-claim's (`0x47C798..0x47C7A6`) act only on marks. The contests keep the type-3 rule: every puppet seat is type 3, so the newcomer takes a held cell, as each owner's machine gives a cell up to a remote unit; no single rule is the composed world's. **The loosening accepted with it**: in a multiplayer segment `recmp_compare.py` (its dumps and `--fork`'s first iteration) and `tarec compare-dumps --seek` check the unit slots, yard bits and marks by the occupancy rule of the 2026-10-12 row, whose contest form is now the rule's only form, and take the dump's marks into the expected world: the marks are no longer compared with the owners' images, and a loser's cell may name the holder that took it. Why: two live mobile units of one layer that overlap share a cell's one slot, so the clause "every footprint cell of a live mobile unit names it" cannot hold there whatever the order (the r2 recording's pair 10 and 1503, iterations 1015..1024, 10 of 1,346), and a seek's slot-order creates give the cell to the other unit than the playing's history does. Two straight playings of one multiplayer file are still compared byte for byte. The 2026-10-12 row's "Not decided" sentence stays as history. |
| 2026-10-23 | **A straight playing makes the step's footprint and chain calls in the recording machine's order** (format 26, the fix of the 2026-10-12 row's residual for every playing but a seek; `tagpu_recfmt.h`, *THE CONTEST ORDER*, and [the engine map](../exe-reverse-engineering.md), *The order of a step's footprint and chain calls*): the recording machine makes the stamp, the clear and the re-claim in its unit tick's turns, ascending by slot, and the calls from outside the tick after them, so the recording carries the turn a top-level footprint event was made in (`RK_EV_TURN`, only where no reader infers it) every acting attach `0x48AB70` at its place (`RK_EV_LINK`) and every unit Teleport's handler moves, at its set-position call `0x406BC0` (`RK_EV_PLACE`), which a playing makes there with the handler's flare `0x471FD0` before it, counted as the handler's own call -- Teleport being an order handler, which runs on the owner's machine alone (the `teleport` test group, and `stock-teleport` on the stock-limits build). On a skirmish segment that holds its cells the playing reads image T+1 before it plays step T's events and, before each footprint event of key K, places every unit below slot K at its next recorded position through the set-position, so the host makes the same calls in the same order on the same state, and every cell's holder, both marks and every chain follow by induction. What the recording machine did that no key orders -- a turn out of slot order, a footprint or attach call from a site the recorder does not know, a Teleport move of a unit that is not alive -- is counted in the recording, and the harness fails any (*L: the footprint calls no playing can order*). On the playing's side the harness fails a step the order could not hold, a refused Teleport move or carry link, and on a recording that holds its cells it could not run a playing that applied steps and played none in the order, and fails one whose steps played in the order and held out of it do not add up to the steps it applied -- every such step is counted as one or the other (`co_engage`), so a playing the order skipped, for every step or for some, cannot pass (*P: the contest order played*, and R's and Q's). **Nothing is loosened**: the 2026-10-12 row's "Nothing else is loosened" stays true, and the seek's residual is unchanged -- a seek still makes its units in slot order, and its cells are still checked by the occupancy rule. MEASURED 2026-10-23: the `contest-waves` test group, whose P lane fails on format 25 in 4 runs of 4, passes L, P and R; its recording has 26 steps where a unit of a lower slot moves beside a footprint event of a later turn, and two playings are EQUAL to it at each of those steps' T+1; the `teleport` test group's 12 moves play at their place, its counts EQUAL with the flare live 8 and puppet 8 (`sink 0x471FD0 from 0x406BA4`), and its recording counts no unordered call. An event made in turn s after s's own relink -- the walk class's and the water state's run-now COB passes run after it (0x43DD5D, 0x43DD65), and the reaper's death -- is keyed s+1, so a playing places s before it: MEASURED 2026-10-24, slice-walkers keyed 2 events so, both a unit's death after its own relink, its P and R EQUAL; carry-links, transport-pad, yard-lab, contest-waves and teleport keyed none; and the `transport-turn` test group (`scenarios/replay-transport-turn.gen.py`) keys attaches so: 40 hover transports, each blocked by a scout car that leaves in the window of its TransportPickup's first attach, so the step the car frees the row the transport relinks into it and its walk class's StartMoving pass runs the pickup's thread, whose boom wait ended in that step's slice, to its attach -- MEASURED 2026-10-24, `tareplay.py run transport-turn`: 6 links keyed their carrier's slot + 1, the recorder's count of events made after their turn unit's relink 6, 0 unordered, P and R EQUAL at 91, 121, 151 and 181, the contest order played on P, R and Q, all 53 checks passed (4 such links in an earlier recording of the same combs). What it cannot show: an attach and its carrier's relink commute (the attach moves the child between chains and writes no cell, the relink re-files only the carrier, on a bucket change), so a build with the next-slot keying removed passes the group as well; the order would show for a drop into the bucket its carrier has just entered (the detach pushes the child at that bucket's front, `0x47CB40` at `0x48ACDC`), which no group makes. A drop comb built the same way did not make one either (MEASURED 2026-10-24: 22 drops, none in a step its carrier relinked in, since a hover transport drifts sideways by up to 14 wu while it creeps to the car and pushes on it, and a sideways relink re-files its bucket before the row frees): the drop's order stays unmeasured. A key is RCS_OTHER's and sets no events flag, so the lossy filter takes the step's first key, link or move as the end of the iteration's images, as it takes the first event (`tagpu_reclossy.c`, *A REFRESH'S WRITES*), and the writer ends the file where a stream block puts a record between a key and its event. MEASURED 2026-10-24: a filter that ended the images at the first event alone wrote a refresh's cells and lists between a key and its event, which every reader refuses (a contest key before `RK_CELLS`, kind 33) -- `tenseat` at GameTime 181, `forest-circle` at 1501 and `stock-limits`, wherever a refresh's first event was a keyed one; that filter run again over `forest-circle`'s exact records (`RB_EXACT`) makes the refusal at 1501, this one makes a stream `tarec check` reads to its end, every change explained, and so for `tenseat`'s. |
| 2026-10-23 | **The recorded view's contact bits are the recording's** (the owner, amending D1 of 2026-09-30 in that one perspective; not a loosening): when the host views the recording's own viewed player, a playing writes every unit's `+0x110` bits 8–10 from the image it holds after every step, at the capture point and the frame visit (`contacts_put`), as the seek and a change of view already did after their pass. The engine's pass there did not have the recording machine's inputs: it saw the units made or killed after the recording machine's pass of the same step (the frame tail's applier, the SHARE give), and in multiplayer each remote unit at its owner's position. Every other perspective keeps the engine's pass, as D1 has it. **The residual accepted with it** (a known display gap, no reorder and no format change): in any other perspective the radar picture covers a unit made after a pass step's players phase up to 30 ticks early, and in multiplayer follows the owners' positions; under true LOS the minimap's fog shows such a unit's sight one pass early in every view [INFERRED]. The `contact-waves` test group carries the class. |

The owner approved one additional UI-only count exception on 2026-10-05: the native
immediate-command click (`0x41A0F1` → `0x47F1A0("immediateorders", 0)`, return `0x41A0F6`),
including Stop. The recording player's click is absent on puppets, as the console click already
is. Command orders, reclaim/build effects and carried simulation state remain checked.

The owner approved another on 2026-10-25: the multi-building placement's `oktobuild`
(`tagpu_place.c`, its commit plays `0x47F1A0(0x509660, 0)` once a gesture that added or removed an
order, as the stock click plays it at `0x498FA4`). The row is `0x47F1A0 dll * absent`: the
recording player's own mouse, which a puppet host never makes. Its caller column `dll` matches
every caller inside Impure's DLL, so the menu's and the Replays browser's sounds (`tagpu_menu.c`,
`tagpu_replays.c`) fall under it too. The orders the gesture makes are recorded and checked; the
test group that carries them is `place-basic` ([quality-of-life](quality-of-life.md), *What
Impure does*).

Resource fixtures may name a `preflight` command before the live recording: native frontend
Undo assertions or an engine catalogue refresh for changed private test content. Its failure
fails the fixture; the match is then recorded and checked through the usual L/P/R/Q lanes.

An owned replay worker invalidates its cached engine catalogue whenever it replaces fixture
content. The next scenario therefore validates against its own launch, rather than rejecting
a private unit because the previous fixture did not supply it. The sequential stock-resource-mex
then wpn-badtgt regression on one worker passes all 84 replay checks, including 28 exact paused
comparisons and both strict image gates.

The native COB trace and the recorder's allocator counter both require the pristine
`0x4B08C0` entry. Enabling trace for a recorded lane prevents that counter from installing,
and the count gate rejects it as not counted. Resource preflights therefore exercise trace
with the recorder disabled, save their coverage bitmap, stop the native game, and restore
the trace lever before L. Playback, restoration and seek use the ordinary counter setup.

## Open items

For the skirmish slice: one, being looked at on its own. In the `contest-waves` test group a create
made at a frame's tail, one tick before a drawn GameTime, differed in `+0x110` bit 8 -- a radar
contact bit -- on P and R (MEASURED 2026-10-23 while that test group was built); it is not a
footprint or contest difference, and the contest order does not touch it. A recorded YARD_OPEN (`RK_EV_YARD`, written at `0x47DAF1` past
`0x47D970`'s refusal) of a live unit is applied at its place in the step, restamped as `0x47DAC0`
does (`0x47C790`, `0x440A40`); one a structure's own Create opens, before the create's stamp
(`0x485D40` at `0x4860A6`, the stamp at `0x48610F`), is made again by the replayed create and
checked against the recording, a difference failing the harness (the engine map's *An open yard
gives nothing back* and *THE YARD RULE*). The test groups `yard-lab` and `stock-yard-lab`
(`scenarios/replay-yard-lab.json`, to GameTime 900, the raised and the stock-limits build) reach
the live yard in every run, so the yard code's coverage rests on no AI's choice: the human seat's
ARMLAB is selected with a native click at GameTime 120 and its ARMPW build button pressed through
the build panel (`tacli ui click ARMPW`, the engine's own button path), and the lab's build scripts
open its yard at 184, the Peewee is made inside the open yard at 185, is built at 668, and the yard
closes at 820 (MEASURED 2026-10-13, both builds, the same ticks). No test group reaches a Create
that opens its own yard: a stock lab opens it from its build scripts, after its create.

Answered while planning S2 (2026-09-30): **`0x48B090` plays unit replies, never a network sound.**
A change of `+0x10E` bit 0 queues reply kind 3 or 4 and of bit 2 kind 14 or 15 through `0x47F780`
(`0x48B110`, `0x48B135`, `0x48B178`, `0x48B1AA`), for the viewed player's live units only. The queue's
consumer `0x47F680`, called once per main-loop pass (`0x4999A6`), plays a reply through `0x47FD70`
and `0x4D0640` by file name. Nothing on that path calls the sound entry `0x47F0C0`, whose four callers
are `0x45562D`, `0x47F1FA`, `0x47F274` and `0x47F31A`, so no `0x13` is sent. S2 records the replies
and makes the engine's own reply calls inert on a puppet host, so a recorded `+0x10E` change applied
there plays nothing twice.

**A takeover does not reproduce the meteor shower's strike points** (open; the owner's ruling,
2026-10-08: a known gap, not fixed now). The meteor tick draws its strike points `0x512334` and
`0x512320` from the CRT's `rand`, as the forced shower `0x438070` draws `0x512334`, whose state is per thread and in neither image, and on the game
thread the draw path draws from the same stream every frame
([engine map](../exe-reverse-engineering.md#the-scheduler-the-clock-and-the-test-pacing-disassembled-measured-2026-10-08)).
A takeover does not restore that state, so two takeovers of one recording at one T part there and
nowhere else: MEASURED 2026-10-08, tenseat taken over at 931 and dumped at 2731, at a fixed turbo
k and at speed 20 alike. Closing it needs the stream's state recorded and restored at a takeover,
or the shower's draws moved off the shared stream.

**A takeover takes the viewed record's contact bits from the recording** (decided by the owner,
2026-10-11: the fork's AIs read the recording's contacts at T). The seek and a skirmish recording's
takeover alike run
the viewed record's radar pass on T's world (R14b) and then, when P is the recording's viewed
record (a takeover views P from TK1 on), put `+0x110` bits 8–10 back from the recording through
`rw_contacts`, since the pass a playing holds read its step's globals mid-step and no image
reproduces it (the skirmish slice's O14: the tenseat fixture's LOS command at GameTime 900 leaves
images 901..930 with LosType 6 and the contacts a LosType-4 pass set). Those bits are the fork's AI
input: the acquisition scan `0x40AA40` puts every non-allied unit with bit 8 on each local seat's
radar list (`0x40AB3F..0x40AB5C`), and the acquisition records TK1 makes start with `+0xED` 0
(`0x40918B`, `0x4092DC`), so each scans in iteration T's players phase, before the next radar pass,
and reads the recorded bits as the recording's AIs read them until its next pass. The bound is
`rw_contacts`' (a slot below the array's count, live in the image, whose host slot holds a live unit
of the image's type; only the three masked bits written); the ordering is that nothing after R14b
writes bits 8–10 before the capture point of T (R14c, TK4a's `0x48BD00` clears bits 4, 6 and 7
only, `0x466DC0` reads, then the tick loop to `0x4954ED`: one level of callees DISASSEMBLED, the
rest MEASURED 2026-10-11 — tenseat taken over at 901 and 915, `compare-fork` EQUAL with 23 and 25
live units). The harness takes tenseat over at 901, the tick after its typed LOS command
(`fixtures.json`'s `at`), so every run checks it. The residuals:

- **P is not the recording's viewed record.** The recorded bits are another record's scan, so the
  pass's bits stand, and the radar lists the fork's AIs build from P's picture as of T stand until
  their next scan at T+30, where a playing viewed as P holds its last pass's. D2 (d) masks bits 8–10
  there.
- **A multiplayer recording.** Its takeover does not restore: `rw_contacts`' bound is a live unit of
  the image's type, not its identity, and the primary stream's image of a slot can lag a same-type
  reuse the composed world already holds, which would hand the new unit an earlier one's contacts
  as the fork's AI input. So the pass's bits stand, computed on the composed world at T, and the
  O14 gap stays for multiplayer takeovers ([its takeover](demo-recorder-multiplayer-slice.md#the-takeover-of-a-multiplayer-recording)).
- **Bit 12 and `+0xB0`** stay the takeover's own pass's: its fourth loop sets them through the fresh
  acquisition records (`0x4676A1..0x4676E8`), and D2 (d) masks bit 12 always.

**Battle-los's ground and feature impacts differ by a few calls, cause unknown** (open, inside the
band). The impact `0x499EB0` from the collision test's feature, ground and water path
(`0x49B3D2`) counted 6191 live against 6194 on the playing by GameTime 3031 (MEASURED 2026-10-09,
before the shot start was recorded from the build), and 2516 against 2517 over [1021, 2281)
after it, with slot A and slot B equal. One candidate, not traced to a shot [INFERRED]: within one
phase the recording's feature roots go in after the host's whole projectile call, so a later
projectile can still meet a tree an earlier impact felled on the recording machine
([skirmish slice](demo-recorder-skirmish-slice.html)).

**COB slices a playing gives one tick late** (closed for A5 and in-game creates by the slice
marker, format 20). The unit tick's `DoScriptsNow(1)` at `0x48ADEB` is the only clock a COB thread
has, so a unit whose slice falls on the other side of the unit tick on a puppet runs its scripts
one tick off, and a looping script such as a walker's `MotionControl` stays re-phased for the rest
of the unit's life. The level's own units are made before the host's first unit tick, which closes
the level-start case (the [engine map](../exe-reverse-engineering.md), *COB on a puppet*: tenseat's
walk/walklegs/stand counts reproduced call for call, MEASURED 2026-10-12). **The slice marker**
closes two more: the recorder writes `RK_EV_SLICE` before the slice of a unit whose COB had an
engine start since the capture point, and a playing skips that unit's slice in the host's unit tick
and gives it at the marker, after the recorded events that preceded it -- a unit created during
play in a later slot of a block not yet finished (its Create start), and AutoAim's starts (A5:
TargetCleared, the aim script, FirePrimary, RockUnit), all made in the unit's turn before its
slice. Why every start a slice can depend on is seen: outside a slice a COB object's threads are
made only through `0x4B0B00` and `0x4B0940` (the engine map, *The COB module's entries*). The
playing's bookkeeping (`tagpu_replay.c`, THE SCRIPT SLICES) gives each owed slice once: at its
marker, at a multiplayer own create's CREATE_END, or at the step's end, counted and noted; a create
or a death in the slot voids the debt.

**A replayed death's Killed** is the owner's query too. The recording machine runs a dying unit's
Killed through `Send_UnitDeath`'s query (`0x4865C3` → `0x4B0BC0`), that one thread at `dt` 0; a
playing applies every death as a peer's, and the destructor's peer path starts Killed with run-now
set (`0x486877`), which runs every record of the dying unit -- an aim script whose turn had
finished ran on and started RestoreAfterDelay, the puppet 1 to 12 ahead under `--cob-exact`
(MEASURED 2026-10-12: battle 7833/7837, codec-decline 452/453, every engine start equal per
script). Inside a replayed death the recording machine made itself, Impure makes the query
instead (`tagpu_patches.c`, THE KILLED SCRIPT OF A REPLAYED DEATH), so both sides count Killed at
`0x4B0C4D`. Its corpse argument is 0 on a puppet, where the recording machine passed an
uninitialised local: a Killed that reads it before writing it can differ [INFERRED: no stock
script seen to].

Not closed by the marker:

- **The walk class's run-now pass** can run a start the AI made after the unit tick a tick early,
  the COB side of *Events outside a deferred bracket carry no phase*
  ([skirmish slice](demo-recorder-skirmish-slice.md)).
- **A seek or a round trip** starts COB from Create: a rebuilt unit's threads are not the ones it
  had.
- **The walk class after an unload (A6)**, unchanged.
- **A step's walk start and water start in the other order** (owner-accepted 2026-10-13 with A5
  and A6, *Decisions taken*). On the recording machine the movement pass `0x43DD20` places the
  unit (`0x43D6D0` at `0x43DD55`), then runs the walk class (`0x43DA70` at `0x43DD5D`), then the
  water state (`0x43DB50` at `0x43DD65`, whose `setSFXoccupy` start is at `0x43DBE8`). On a puppet
  the recorded `setSFXoccupy` plays among the step's replayed events, and the unit's placement
  (`0x48A9F0`) and its walk class's start come later, at the images (`unit_put`). So in a step in
  which a unit changes both, a puppet starts `setSFXoccupy` before `StartMoving` or `StopMoving`
  where the recording machine started it after; and `setSFXoccupy`'s start runs now (its third
  argument 1, `0x43DBDD`), so the run-now pass `0x4B0B86..0x4B0BA6` gives the unit's threads their
  `dt` 0 run while the unit still stands at the previous step's position. Recording the walk class
  would close it; that is a separate idea for later.
- **Mid-tick reads.** A slice reads unit and world fields (GET, the `+0xFA`/`+0xFB` countdowns).
  The live slice read the tick as far as the unit's turn; a host-tick slice reads the capture
  image plus the host's tick so far, a deferred one that plus the events before its marker
  [INFERRED: not seen].
- **The order of COB RAND draws against the shared simulation RNG.** A slice's RAND draws from
  the simulation RNG `0x4B6C30` (the call at `0x4B15E0`), whose state `0x51FC88` every other draw
  steps too. On the recording machine the order runs' controllers (`0x43B951` in `0x43B7C0`,
  `0x43BB33` in `0x43BAD0`), the order handlers and the aim path (`0x49D727`, `0x49D733`) draw
  between slices; a puppet makes none of those draws, and the slice marker moves a marked unit's
  slice, with its draws, after the host-tick slices and the events before its marker. So a
  script's RAND can read another value than the recording machine's [INFERRED: not measured]. Not
  a regression of the marker: the capture point writes the RNG's state from the recording every
  step (`replay-globals.tsv`, `rngseed`), so a difference lasts one step; open.
- **The walk class's starts stay the playing's own** (`0x43DA70` at the images: `StartMoving`,
  `StopMoving`, `MoveRate1..3`), which is where *the walk class's run-now pass* and A6 come from.
  Recording them where the recording machine makes them, as `setSFXoccupy` is (format 23), would
  close both, at a cost the owner has not taken: MEASURED on `battle` at format 20, 36,752
  `StartMoving`, 36,538 `StopMoving` and 84 `MoveRate2` starts in 2,851 ticks, about 26 events a
  tick. A step in which a unit changes both its walk class and its water state runs the
  `setSFXoccupy` thread at its place and again at `dt` 0 in the image's walk start, the same
  run-now class.
- **A unit dying before its turn** in the same tick, only through an earlier turn's immediate
  kill [INFERRED: a reclaim of a live unit]: live it gets no slice, a puppet gives it one in the
  host's tick.
- **A start made inside a slice** by a COB SET a puppet replays as an event (`RK_EV_STATE` →
  Activate): live the new thread may run in the same slice when its record comes later
  [INFERRED: the runner's record order]; on a puppet it runs at the next slice.
- **Multiplayer**: markers cover the recording machine's own units (M1); starts the ledger orders
  keep their lag, and another owner's units are unchanged
  ([multiplayer slice](demo-recorder-multiplayer-slice.md)).

Gaps stated, not closed: within a multiplayer segment, getter 75 per seat reproduces the
recording machine's view, so other players' owner-only indicators, which that machine never
showed, are absent; across a segment the answers follow each segment's controllers, so a
takeover timeline of a multiplayer file, every seat local on the machine that recorded it,
shows every seat's (`mp_follow`, `tagpu_puppet_locals`). And a takeover's fresh COB resets what a mod keeps in COB statics and piece visibility
(for example Mayhem's upgraded metal extractor looks un-upgraded after a fork, while its attached
module and `+0x10E` are restored) [INFERRED from the scripts].


### Rewind comparison scope

The owner approved two S4 amendments on 2026-10-03: D2 also excludes unseated
records' sight counters; the U lane accepts VM START/CALL child-allocation count
differences across return-to-now via `compare-counts --rewind`, reporting each
pair. Seated sight counters, root counts and puppet-write checks remain intact.
COB execution state is reconstructed, not preserved. Ordinary count comparisons
retain their existing policy. See [S4 rewind](demo-recorder-s4-rewind.html).

The resource frontend preflight temporarily disables frame packets to require menu reload
without a renderer consumer; it restores that lever before recording the match.
