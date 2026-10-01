# Demo recorder — the skirmish slice (the plan)

## Status

The [product page](demo-recorder.md#milestones-and-delivery-order)'s second milestone, planned
2026-09-29: recording, puppet watching, seeking, live rewind and takeover, forks, the Replays browser
and `tacli` automation, with the verification the
[completeness design](demo-recorder-completeness.md#build-order) lists as its step 2. Six landings,
each a finished unit verified by running it. **S1, the recorder, landed on 2026-09-29**: a
skirmish is recorded to a file when `tagpu_recorder.on` asks for it, and `tools/replay/tarec.py`
reads a recording back. **S2, watching, is two landings.** S2a landed on 2026-09-30: a recording
carries every mover, order list and PlayerInfo, and the events, each checked against the engine
(*S2a, verified and measured*). S2b landed on 2026-09-30: `tacli replay play` plays a recording
back on puppets, showing everything or one recorded player's view, and every recorded value is
equal to the recording at every checkpoint (*S2b, verified and measured*). **S3, seeking, is
three landings** (*S3 — seeking*). S3a landed on 2026-09-30: a recording carries the feature grid
and the wreck pool, and every change to them is replayed through the function that made it
(*S3a, verified and measured*). S3b, the globals and seeking, is next.

## How the slice is cut

Each landing adds one capability that can be verified by itself, and the ones after it build on it.

| Landing | Delivers | Verified by |
|---|---|---|
| **S1 — the recorder** | the capture at `0x4954ED`, the writer thread, the file, self-verification, and a tool that reads a recording back | self-verification over whole games; the file read back to the tick of a dump and compared with the dump byte for byte; the cost and the size measured |
| **S2a — what watching needs, recorded** | the objects a unit record points at (the mover, the order lists) and PlayerInfo, events at their roots, the nano spray, the header's game options, seats and weapon fingerprint | S1's checks over the new records; the events checked against the images |
| **S2b — watching** | the puppet host, playback, the one-writer rule, getter 75 per seat, `tacli replay play` | a recording played on puppets and compared with the recording at checkpoints, in full visibility and from the recording viewer's perspective with its radar picture; each event kind counted as played or not |
| **S3 — seeking** | the rebuild in place from a snapshot, features and wrecks, the main block and `.data`, the replay harness and its restore round trip, the CLAUDE.md replay gate and the landing command's Step 4b, the nightly tier | the round trip, with its rules against a vacuous pass; seek times against the target |
| **S4 — rewind, takeover and forks** | the timeline in a live skirmish, Back to now, Play from here, forks in the file, the takeover from a recording | the harness's takeover run, judged like the compatibility suite's battle; the fork rules |
| **S5 — the browser and the controls** | the Replays browser, the timeline bar, perspectives, camera follow, the overlays, the recording switch in the settings screen | the browser and the controls driven through `tacli ui`, with pictures |

**S1 and S2 record every value exactly, at every tick.** That is what makes their checks exact. The
product page's stream — ten samples a second in whole world units, interpolated in playback — is a
landing of its own after S2, once playback can show its error; S1 measures the exact stream's size
so that the gap to the budget is known before then. Until that landing a skirmish is recorded only
when asked, by the file `tagpu_recorder.on` in the game's folder, because the exact stream is too
large to write for every game (*Measured*). That landing turns recording on for every skirmish and
retires the file, and it packs the stream tighter as well: a scratch repacking of the exact stream
measured about 2.5 times smaller (*Measured*).

## S1 — the recorder

### What it records

At every iteration of the sim step in a skirmish, from the level's first iteration to its teardown,
the capture takes an **image** of every live unit record, of the ten player records (not the
eleventh, which nothing activates) and of the rows of every `SIM_STATE` declaration but two kinds:
the engine pools the raised limits moved into the DLL (`SIMD_ENGINE`), whose record layouts are not
written yet, and TLS slots, which are per thread. A skirmish is game mode 2, `+0` of the object
`[main+0x391E9]` points at (1 is a campaign and 3 a network game: `0x435100` is `mov eax,[ecx]`,
called with `ecx = [main+0x391E9]` at `0x41FB6C` and `0x4971C1`, DISASSEMBLED); a skirmish loaded
from a save is mode 2 too (run 3). A unit slot is live while bit 28 of its `+0x110` is set.

An image has the record's shape, and `inc/tagpu_replay_layout.h` — generated from the exclusion file
the owner controls — is all that shapes it. A byte of a value field is copied. An `id:<table>` row
holds its target's ID in place of the pointer: one more than the pointer's row in that table, by
the table's offset rule, so `NULL` is 0. A pointer outside its table and the off-map sort bucket get
IDs of their own (`REC_ID_BAD`, `REC_ID_OFFMAP`), the first counted and its first eight logged. The
bytes of `object`, `cache`, `timer` and `drop` rows are zero. A declared table's image follows its
layout the same way: an `SIMF_ADDR` field with an ID table holds the ID, and an address with none,
an `SIMF_LOCAL` value and the `SIMF_OWNED` and `SIMF_COLUMN` pointers are zero. So a record of zeros
images to zeros, and a row of zeros costs the ring nothing.

### The capture

`tagpu_recorder.c` runs first in B2's stub at the sim step's call of the unit tick, `0x4954ED`,
which then rebuilds the air pool and jumps to the tick. GameTime has just been incremented
(`0x4954C0`) and every phase of the previous iteration has run, so the capture sees the world
between two iterations, on the thread that owns it. A skirmish calls the step with 0 (`0x49693C`),
so the network pump inside it (`0x4954C8`) does not run: only a network game pumps there. It
names engine memory only to read it, each read bounded by the engine's own count
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-capture-where-it-runs-and-what-it-reads-disassembled-measured-2026-09-29)).
What it writes is its own: its shadows, the ring and the harness's dump.

It images only what may have changed, and puts only the changes into the ring:

- **A unit record** whose bytes equal the copy the last capture read is passed over. Within a
  recording an image is a function of its record's bytes alone, since the tables its IDs index — the
  unit array, the unit types, the players, the weapons, the buckets — do not move until the level
  ends, and that ends the recording. A record whose bytes moved is imaged, and its image's changed
  dwords go into the ring as a delta; a new unit, and a slot whose type changed, go whole; a death
  goes as a death.
- **A player record** is imaged every iteration, and its changed dwords go as a delta.
- **A declared table's rows** whose bytes may have moved are imaged, and each row's changed runs of
  bytes go as spans, bridging unchanged gaps of up to 8 bytes. A static table's rows are found by
  comparing its bytes with the copy the last capture read. A heap table's are the rows on the pages
  the OS says were written: `SIM_ALLOC` gives every heap table `MEM_WRITE_WATCH`, and
  `GetWriteWatch` answers and resets in one call. During play only the game thread writes these
  tables, the thread the capture runs on, so no write falls between the answer and the reset. Every
  row is imaged at the first capture, when a heap table moves or `GetWriteWatch` refuses it, and
  when a table's count takes rows in; a row of zeros is passed over without imaging, its image
  being zeros, like the shadow it is compared with.

The largest declared tables are heap tables for this reason: the build ghost's memo `s_ghost`
(16,384 rows of 52 bytes) and the transported explosions' marks `s_txMark` and `s_txKillMark`
(65,536 entries each). As static arrays their 1.3 MB was compared every iteration. Each is allocated
when its module installs and never freed, and every path that can run without that module tests it
for NULL. A capture that imaged every row of every table each iteration cost 3.6 ms; write-watched
heap tables, the byte comparisons and passing rows of zeros over bring it to about 80 µs in a
four-player game.

### Self-verification

Every iteration the capture also puts fresh images into the ring, taken straight from engine memory
whatever changed: the next thirtieth of the unit slots, live or not, so every slot is checked once
every 30 iterations; one player record, in turn; and the next 16 KB of declared rows, whole rows,
table after table. After applying the iteration's changes the writer compares them with its copy.
A difference writes the fresh image into the stream as a correction, counted by record, table and
offset in the end block (256 counters, then one for the rest), and the first 16 corrections of a
recording are logged with the two values. The change stream carries whole images, so a correction
is a defect in the recorder's own pipeline: the verification runs require none, and run 5 shows the
check is not vacuous.

### The file

One file per recording, `replays\<date> <time> <map>.imprec` in the game's folder, or its first free
` (2)`…` (9)` variant, written by the writer thread alone. It is `IMPREC\r\n` and then blocks: a
32-byte header — the block's kind, the first and last GameTime it covers, its raw and stored sizes, a
CRC-32 of the stored bytes and one of the header — and the payload, deflated with lodepng's
`lodepng_zlib_compress` when that is smaller, else stored. `inc/tagpu_recfmt.h` is the format:

- **the header block**: the format version, Impure's build, the game mode, the map, the first
  GameTime, the wall clock, the unit array's slots and the units each player may have, the unit
  types' count and a hash of their names, the players (index, controller, side, logo and name), the
  registry's levers and their text, the exclusion file's SHA-256, and the recorded declarations
  (index, class, key, row size, flags and name), so a reader can walk their images;
- **stream blocks** of whole iterations, each ending at the first iteration past 256 KB or at 300
  iterations;
- **a snapshot block** every 900 iterations (30 seconds at normal speed), the writer's whole copy.
  The interval is a placeholder: S3 chooses it by measurement against the seek target;
- **the end block**: why the recording ended, the last GameTime, the iterations, the corrections,
  the id rows found outside their tables, and the most the ring held.

A block is written whole or not at all, so a file cut short reads up to its last complete block, and
a file with no end block is incomplete.

### The game never waits

The ring is a single-producer, single-consumer byte ring of 16 MB, allocated when the recording
starts. The capture writes an iteration's records and then publishes its write count with an
interlocked exchange; the writer reads the count before the records and gives bytes back the same
way. 32 bytes are always kept back, so the capture can end a recording whatever the ring holds. If
an iteration's records do not fit, the capture drops them unpublished and ends the recording after
the last whole iteration, marked incomplete; a write error ends it the same way, through a flag the
writer sets and the capture reads.

The capture never waits for the writer. It signals the writer's event at the start, whenever a
megabyte has been published since its last signal, and at the end; the writer also wakes every
100 ms. A signal under Wine is a round trip to the wineserver, which a busy server can hold for
milliseconds, and signalling every iteration made the worst iteration 2 to 4 ms. What the writer may
read, and when it may free the ring, rest on the counts alone (`tagpu_recorder.h`, *The hand-over*).

Nothing on the game thread allocates once a recording has started but one thing: the extra weapons'
side table is allocated at the level's first unit with more than three weapons (`tagpu_weapons.c`),
and the capture then grows that table's shadow, once. If it cannot, the recording ends, marked
incomplete.

### Its lifetime

A recording starts at the first capture of a skirmish level, and only when it is asked for and
its end is certain. `tagpu_recorder.on` in the game's folder asks for it: the recorder looks for the
file once, at that capture, so it has to be there before the level starts, and removing it changes
the next level, not this one. The end is certain because `tagpu_reclaim`'s wrap of the teardown
`0x491B60` tells the recorder on the game thread, on entry, before the cascade frees the unit array
(`0x485980`, called at `0x491B95`), and without the wrap the recorder does not start. The end
record is the capture's last access to the ring; the writer then
finishes the file, frees the ring and exits. Each recording owns its ring and its writer, so a level
that starts while the previous file is still being finished takes a new pair.

A recording that has ended is complete once its writer has closed the file, and **the process's
exit waits for that**: the in-game menu's exit to Windows runs the teardown and then posts
`WM_DESTROY`, and the game leaves a moment later through the CRT's `ExitProcess`
([the engine map](../exe-reverse-engineering.md#the-demo-recorders-capture-where-it-runs-and-what-it-reads-disassembled-measured-2026-09-29)),
which would kill a writer still writing its last block. The fork hooks `ExitProcess` in the IATs of
the exe and of the DLLs in its folder, and its own exits, which that hook does not reach, call the
same wait, `tagpu_rec_exit`, before theirs: closing the window, `tagpu_refuse` (a saved game it
cannot load, a unit script that cannot continue) and the out-of-memory and unit-type refusals. It
waits for the writer of every recording that has ended, each of which sets an event once its file
is closed. A crash exit calls `TerminateProcess` and does not wait, since the crashed thread may
hold a lock a writer needs. A writer never waits for another thread, so the wait ends when the
files do. A recording still in progress at the exit — the window closed mid-game — has no end to
wait for, and its file reads to its last block, incomplete. A recording takes one of eight slots
for its event at its start and does not start without one, so its end always has a place to hand
it to.

Besides the teardown, a recording ends marked incomplete when the ring is full, the file cannot be
written, a table's growth finds no memory, the unit array moves or changes size or GameTime goes
back without a teardown, or the game stops being a skirmish; the rest of that level is not recorded. A load from
the in-game menu runs the teardown first, so the recording in progress ends complete and the loaded
level starts its own at its first iteration. A loaded game starts paused, and a paused game has no
iterations to record.

### Where the code is

- `src/tagpu_recorder.c`: the capture, the start and the end, and the harness's `dump` verb. It
  names engine memory, so it has a line in `thread-split.allow`, `publisher presentation`, approved
  by the owner on 2026-09-29: `presentation`, because in S1 the recorder holds no state the
  simulation reads. S2 revisits it for the puppet-mode flags COB reads.
- `src/tagpu_recwriter.c`: the writer thread. It is not on the list, so the build refuses an engine
  address in it, and "the writer never reads engine memory" holds by construction.
- `inc/tagpu_recorder.h`: the ring, the hand-over and the wake rule. `inc/tagpu_recfmt.h`: the ring's
  records and the file's blocks, shared by both files and mirrored by the tool.
- B2's stub at `0x4954ED` (`tagpu_patches.c`) calls the capture before the air pool's rebuild, and
  `tagpu_reclaim.c`'s teardown wrap calls the recorder's end first.
- The exit's wait: `fake_ExitProcess` (`winapi_hooks.c`, in `hook.c`'s kernel32 list, local
  modules only) and the fork's own exits — `SC_CLOSE` (`wndproc.c`), `tagpu_refuse.c`, and
  `hit_no_memory` and `lim_types_refused` (`tagpu_patches.c`) — call `tagpu_rec_exit` first.
- `tagpu_simreg`: a walk of the declarations and of the levers for the recorder, and
  `MEM_WRITE_WATCH` on every heap table `SIM_ALLOC` takes.
- `tools/replay/tarec.py`: reads a recording — `info` (the header, the blocks and how it ended),
  `check` (the stream against every snapshot the file carries) and `compare` (the stream to a
  dump's GameTime against the dump, byte for byte).
- The verb `dump <name>` in `tagpu_replay.trigger`, looked for every 32 captures: the capture that
  finds it first writes the live world's images to `replays\dump-<name>.bin`, a stored snapshot of
  the state that capture records.

The switch is a file, not a setting: `tagpu_recorder.on`, read with `tagpu_opt_on`, which
`tacli arm <instance> recorder.on` writes. It decides whether a game is recorded, not what the
simulation runs, so it is not a simulation lever and the header's lever list does not name it. The
product's opt-out is a setting, and the settings store's only writer is its menu
(`tagpu_settings.h`), so the setting comes with its menu row in S5.

### Verified by

Scripted games on Wine, on a private X display, in the raised build unless stated; each recording
read back with `tarec info`, `check` and, where the run took dumps, `compare`. Runs 1 to 5 ran on
S1 as the landing review read it. Runs 6 to 8 and the row after them ran on the build that lands
but for the file that asks for a recording, which changes only whether one starts; that build
changed every ID by one, and so every image, and added the exit's wait. Runs 9 and 10 ran on the
build that lands, run 10 with the two scratch changes its row names, and the last row on local
`main` after the landing:

| run | the game | what it showed |
|---|---|---|
| 1 | three stock AIs and an idle human seat on Core Prime Industrial Area, eight minutes, three dumps | 14,610 iterations and no correction; every dump equal (26, 39 and 64 live units), and all 16 snapshots equal to the stream |
| 2 | the experiments' battle on Town & Country, 1,428 units applied, two minutes, three dumps; run twice | no correction and every dump equal, 1,171 to 1,407 live units at the dumps; the cost below |
| 3 | run 1's game saved a minute in and loaded from the in-game menu, then played a minute, two dumps | the recording in progress ended complete at the load, at GameTime 1846; the loaded game started paused, and once running recorded from GameTime 1847; both dumps equal, no correction |
| 4 | run 1's game, the process killed with SIGKILL two minutes in | the file said incomplete and read to GameTime 3601, its four snapshots equal to the stream; copies cut inside a payload and inside a block header, and one with a torn block header, read to the block before the damage |
| 5 | run 1's game in a scratch build, not landed, that flips one byte of the writer's copy of a unit, a player and a declared table at GameTime 1000 | each correction named in the log and in the end block: the player at GameTime 1001, the unit at 1018, `g_def` row 0 `+0x8` at 1032; the dumps equal |
| — | run 1's game for 90 s in the stock-limits build | 22 tables, every dump equal, no correction |
| 6 | run 1's game left a minute in by the in-game menu's `EXIT`, `EXITGAME` and yes, in a scratch build, not landed, whose writer holds three seconds before finishing | the recording ended at the teardown; the exit logged that it waited for one file, the writer closed it, and the process went: the file complete, its dump equal, no correction |
| 7 | run 1's game ended by a defeat a minute in, and its window closed at once (`WM_DELETE_WINDOW`, which Wine turns into `SC_CLOSE`), in the same scratch build | the same: the exit waited for the file, complete, its dump equal |
| 8 | run 6 in the landing build | the writer had closed the file before the exit came, so nothing waited: complete, its dump equal |
| — | run 2's battle twice, and run 1's game for 90 s in the stock-limits build, all in the landing build | no correction and every dump equal: 1,099 to 1,404 live units at the battles' dumps; 22 tables in the stock-limits build |
| 9 | run 1's game without `tagpu_recorder.on`, then with it and left a minute in by the in-game menu | without it, `recorder: not recording: tagpu_recorder.on is absent` and no file; with it, recorded from GameTime 1 and complete at the teardown after 1,964 iterations, both snapshots equal to the stream, the dump equal, no correction |
| 10 | run 1's game ended by a defeat a minute in, and at once a refusal through `tagpu_refuse`, in a scratch build, not landed, whose writer holds fifteen seconds and whose frame hook refuses when a trigger file appears | the refusal came a second after the level's end and its box was closed a second later; the exit logged that it waited for one file, the writer closed it when its hold ran out, and the process went: the file complete after 2,130 iterations, its dump equal, no correction |
| — | runs 3 and 4 again on local `main` at `45f01501`, 15,001 unit slots | run 3: the recording in progress complete at the load at GameTime 1845 and the loaded game's recorded from 1846, both dumps equal (GameTime 2848 and 3776), no correction; run 4: incomplete, read to GameTime 3601, its four snapshots equal to the stream; the copy cut inside a payload read to 2101, and the copies cut and torn in a block header to 901 |

### Measured

The capture's cost per iteration, by `QueryPerformanceCounter` around it, on the reference setup
under Wine, in the build that lands:

| game | the first iteration | the rest, on average | the rest, at worst |
|---|---|---|---|
| run 1's game, a minute (runs 6 to 8), up to 14 live units | 2.3 to 2.9 ms | 84 to 91 µs | 204 to 552 µs |
| run 2's battle, twice, 1,099 to 1,404 live units at the dumps | 2.5 and 2.7 ms | 255 and 247 µs | 1.16 and 1.48 ms |
| run 1's game in the stock-limits build, 90 s | 0.6 ms | 90 µs | 373 µs |

S1 as the review read it measured the same rest: 78 µs on average over run 1's eight minutes (at
worst 760 µs), and 251 and 235 µs in two battles (at worst 583 and 610 µs). Its first iterations
took 4.3 to 4.8 ms: `NULL`'s ID was `0xFFFFFFFF`, so a row of zeros with an address in its layout
imaged to something else, and every such row went into the ring. The first iteration images every row of every table, the
extra weapons' 16,384 unit-type rows of 140 bytes among them; a row of zeros now costs it a
comparison and nothing in the ring. In the stock-limits build, whose unit-type tables are 512 rows,
it costs 0.6 ms.

The product page's budget is 0.5 ms on average and 1 ms at worst with 1,500 units. The average
meets it. The worst did not in three battle runs of five: 2.3 ms in a run while S1 was being
built, and 1.16 and 1.48 ms in the two above. The log names the costliest iteration's GameTime and
what it put into the ring, and the 1.48 ms one put 26 KB, about half what an average battle
iteration puts there: the stream averages 20 KB an iteration (75,590 KB over 3,840), and the
rotating check adds at least 18 KB and about 30 KB on average at the battle's 1,100 to 1,400 live
units, so the time was not the capture's own work. The cause is not found; the writer's wake
signal, a round trip to the wineserver under Wine that falls on about one battle iteration in
twenty, is a candidate, and the reference setup was running other work.

One cost falls outside the timer. Under Wine a write watch is a page protection, so after each
reset the simulation's first write to a watched page takes a fault in the engine's own code; a
scratch program measured 4.2 µs a fault under Wine 9.0 on the reference setup, 5.2 µs with three
other threads busy. The log counts those pages after the first iteration: 393 and 269 in the two
battles' 3,839 iterations, at most 3 in one, so under 20 µs in the worst iteration and about half
a microsecond on average; 6 to 9 in the four-AI games.

Size, the exact stream with every value at every tick: run 1's file is 2.0 MB for 8.1 minutes,
14.8 MB an hour at that rate — its stream deflated 3.4:1 to 1.7 MB, and its 16 snapshots, mostly
zero rows, add 0.26 MB — against the budget's 25 MB for a four-player game, which a longer game with
more units would pass. The two-minute battles came to 18.5 and 19.8 MB a minute in the reviewed
S1 and 20.3 and 17.7 MB a minute in the landing build (the stream deflates about 2:1 there),
1.1 to 1.2 GB an hour if sustained, against the budget's 100 MB for ten players with 1,500 units.
The ring held at most 645 KB of its 16 MB in the battles and 177 KB in the one-minute games; in
the reviewed S1 the first iteration's rows of zeros alone put about 1.2 MB into it.

A scratch repacking of the landing build's battle — one record an iteration rather than one a
unit, each value stored as its difference from the unit's last, and each field's differences stored
together — deflated to 13.8 MB, against 34.5 MB for the same stream as written: 2.5 times smaller,
6.5 MB a minute. The differences are most of that: one record an iteration alone gains 1.2 times.
Run 1's eight-minute stream, recorded by the reviewed S1, came out 2.7 times smaller. The repacked
stream was not decoded back, so this measures what the representation gains, not a format.

### Gaps S1 leaves

- **The objects a record points at** — the mover, the order lists, the COB object, the model, the AI
  object, the control groups, the sight counters and the resource accumulator — are not recorded:
  their rows are `object` and zero in the image. S2a brings the mover, the order lists and
  PlayerInfo, with layouts of their own (*S2*, the rows decided 2026-09-30).
- **Memory a declaration reaches through a pointer** is not recorded: the engine pools the DLL holds
  (`SIM_ENGINE`: explosions, particle systems and their auxiliary geometry), the blocks a row owns
  (`SIMF_OWNED`), and the columns of a struct-of-arrays table (`SIMF_COLUMN`: `s_hit`'s `stamp`,
  `freed` and `local`, one entry per unit slot, whose row alone is recorded).
- **The header** has no game options and no fingerprint of the weapon definitions: S2a. Alliances
  are values of the player records, so the stream carries them.
- **The worst iteration** met the 1 ms budget in two battle runs of five, and its cause is not
  found (*Measured*).
- **The recorder's memory** is not measured. It is bounded by construction — the 16 MB ring, and
  shadows and a copy sized by the unit array and the declared tables, fixed at the start but for the
  side table's one growth — against the product page's starting point of about 64 MB.
- **Events** (shots, script starts, sounds, the nano spray): S2a.
- **Features, wrecks, the main block and `.data`**: S3.
- **Network games** are not recorded: the multiplayer slice.

## S2 — watching

Planned 2026-09-30 from four research tracks, whose evidence landed with S2a:
[the mover and carried units](demo-recorder-s2-movers.md), [the order lists](demo-recorder-s2-orders.md),
[the events](demo-recorder-s2-events.md) and [the puppet host](demo-recorder-s2-host.md). S2 is two landings. **S2a** records
everything watching needs and verifies it against the engine; **S2b** plays a recording on a
puppet host and verifies it against the recording.

### S2a — what watching needs, recorded

- **The mover.** A mobile unit's movement state is one `0x2F`-byte object (the creates' `push 0x2F`,
  `0x4860C4`), made with the unit and freed with it. Its bytes therefore ride as a twelve-dword tail
  on the unit's image, and the unit image's `+0x00` says whether the unit has one. Three rows; every
  other byte is a value:
  - `+0x00`, the movement class object, `drop`: its kind depends on the machine, a local class on
    the recording machine and a remote proxy on a puppet host;
  - `+0x04`, the movement class, `cache`: written only by the constructor, from the unit's type;
  - `+0x2E` bits 3–7, `drop`: allocator leftovers that every writer masks out and nothing reads.

  It adds about 14 KB to a battle iteration (*S2a, verified and measured*): the moved-at stamp
  `+0x2A` holds GameTime on every tick a unit moves, and the speed and the velocity change with it.
- **The order lists.** Each unit's main list `+0x5C` and sub list `+0x60`, recorded as value images,
  a list whole whenever its image differs from the recorder's own previous one. A node image holds the action, state,
  events, deadline, the target as an ID, position, leash, the three parameters, flags, issue time,
  fired events, and the owned object's class code. A node's address is not recorded: playback
  rebuilds a puppet's lists from values. The node's rows:
  - `+0x0E` (the list's unit), `+0x1A` and `+0x1E` (the target reference's links): `cache`;
  - the draw's last-seen target position `+0x32`/`+0x34` and its valid bit `+0x42` bit 21: `drop`;
  - the four fields that arm owner code when a node is destroyed: `puppet0` and `puppet1`
    (*Decided*).

  A list longer than 4,096 nodes is a named failure that marks the recording broken from that tick.
  It is never truncated.
- **PlayerInfo**, the `0xB9`-byte block a player record points at (`+0x27`). It holds the side,
  logo, share bits, the watcher bit and the LOS bits. The radar pass and the draw read it, and the
  console's `Share*` commands change it in play. It is recorded as a record of its own.
- **Events at the engine's own roots.** Every call is recorded, nested ones included, with its
  step and its order in the step. A nested pair is inert on its second run by an engine test: a
  compare, an occupied slot, or an alive test. The roots:
  - **Create**, at `0x485F50`'s success exit `0x4861B3`, which sees the slot actually taken.
  - **Death**, at `0x4866D0`'s entry, with the victim's record as the destructor finds it. The
    reaper runs after the unit's own movement step, so image T is a step early for the wreck's cell
    and the blast.
  - **Shots**, from the `0x0D` packet of the five fire functions, with their network tests released
    while recording; and **interceptions**.
  - **Script starts** through the three broadcasters `0x456190`, `0x456200` and `0x456290`.
  - **Build done**, `0x41B8D0`, the only writer of the model's nanoframe flag.
  - **The state byte** `+0x10E`, as its new value, from `0x48B090`.
  - **Unit replies**, at `0x47F780` and `0x47F850` before their viewer test, for every player.
  - **The script starts only the owner's machine makes**: `HitByWeapon` and `TakeDamage` in the hit
    `0x489CE0`, `TransportPickup`, `TransportDrop` and `EndTransport` in the transport orders, and
    `TargetCleared`. A replay host applies no damage and runs no order handler, so it never makes
    these itself. `BeginTransport` is broadcast, so the broadcasters record it.
  - **The nano spray**, at its emitters (the completeness note's *Decisions taken*).
  - **A controller change.**
- **The header** gains:
  - the game options as the simulation ran them: the LosType bits, ActiveCommanderDeath, the
    difficulty and the start-position mode;
  - each seat's PlayerInfo and alliance row at the start;
  - the recording viewer, `+0x2A42` and `+0x2A43`;
  - the map's settings name `[main+0x29A0]+0x11C`, which is what `SkirmishMap` holds (`RT_MAP` is the
    TNT's stem, which a map can name differently);
  - the player count `+0x2A3C`;
  - a fingerprint of the weapon definitions.
- **Verified by:**
  - S1's checks over the new records: self-verification, and the dump compared byte for byte with
    the recording read back to its tick, now with every mover, order list and PlayerInfo;
  - the events checked against the images, offline: every unit that appears has its create, every
    unit that disappears its death, every `+0x10E` change its state event, and every finished build
    its build done;
  - the cost and the size, measured again.

#### S2a, verified and measured

On the reference setup under Wine, 2026-09-30, in four scripted games: an eight-minute skirmish of
four AIs on Core Prime Industrial Area (14,610 iterations), run 2's two-minute battle of 1,432 units
on Town & Country (3,840), and in the stock-limits build a three-minute skirmish of four AIs
(5,610) and a one-minute run of the battle (2,010). In each:

- no correction: the self-verification now covers every mover, order list and PlayerInfo;
- every dump EQUAL to the recording read back to its tick (three a game: 16, 27 and 42 live units in
  the eight-minute game, 1,400, 1,309 and 1,206 in the battle, 14, 19 and 27 and 1,412, 1,409
  and 1,400 in the stock-limits games), and every snapshot equal to the
  stream;
- every change between two images explained by the step's events (`tarec.py check`): no appearance
  without its create, no disappearance without its death, no `+0x10E` change without its state
  event, no build finished without its build done;
- no ID outside its table, no event refused off the game thread, no nano spray uncredited, and no
  order node whose owner or list flag disagreed with the list holding it.

The events of the battle: 1,430 creates, 1,433 deaths, 7,780 shots, 20,632 broadcast starts,
21,873 owner-only starts, 61 state bytes, 16,415 replies and 24 sprays; of the eight-minute game,
48 creates, 51 deaths, 84 broadcast starts, 48 builds done, 56 state bytes, 170 replies and 16,601
sprays. The stock-limits battle recorded 568 shots and 3,464 owner-only starts, so the released
network tests and the shot callouts run in that build too. No game intercepted a projectile.

**The capture's cost** grew with what it reads. In the battle it took 475 µs an iteration on
average (S1: 247 and 255) and 1.15 ms at worst, in the 37th iteration, while the scenario's 1,432
units were being placed (710 KB into the ring); in the eight-minute game 144 µs on average and
455 µs at worst, and in the stock-limits game 96 µs and 365 µs. The stock-limits battle's one
minute, with 1,400 to 1,412 units alive throughout, took 528 µs on average and 1.57 ms at worst.
The product page's budget is 0.5 ms on average and 1 ms at worst with 1,500 units: the battles'
averages sit at it (475 and 528 µs), and their worst iterations pass it.

**The size.** The battle came to 60.4 MB for two minutes, 30 MB a minute (S1: 17.7 to 20.3). Its
stream is 38 KB an iteration before deflating (S1: 20 KB): the order lists 4.1 KB, the events 0.7
KB, and the rest, about 14 KB, the mover tail [INFERRED: the growth the lists and the events leave;
PlayerInfo changed in no iteration after the first]. The eight-minute game came to 1.34 MB, whose
order lists are two thirds of the stream before deflating (6.7 of 10 MB) and deflate away: the
stream deflates 8:1. A list goes whole whenever a node changes, and the build and repair states,
self-repair, resurrection, capture and the reclaims re-arm their node's deadline `+0x0A` every tick
(`0x439E80`), so such a list goes
every tick. A delta of a list's dwords would gain little: the lists are about a tenth of the battle's
stream, and deflating removes their repetition in the smaller game. What the size needs is the
representation's change S1 measured (2.5 times smaller), the product page's open decision 2.

**PlayerInfo** is cheap: at most five of its `0xB9` bytes are nonzero in a seat of the battle, and
none changed in play. **The ring** held at most 1,059 KB of its 16 MB in the battle (S1: 645 KB).

#### What S2a leaves

- **An interception** has not been recorded by a game: none of the three had an interceptor.
- **The capture's cost** in a battle is at the 0.5 ms average budget (475 and 528 µs) and past the
  1 ms worst (1.15 and 1.57 ms). S1 took about half as long; what S2a's reads cost is not broken
  down. A one-minute game of eleven units on the landing build had one iteration of 2.6 ms that
  put 18 KB into the ring: S1's worst iteration, whose cause is not found (*Measured*).
- **The size**, 30 MB a minute in the battle: the representation (the product page's open
  decision 2).
- **The game options change in play** through the typed commands `LOSType` (`0x416690`), `LOS`
  (`0x416D50`) and `Mapping` (`0x416D80`): the header records them at the start, and the LosType
  word lives in the main block, which is S3's.
- **A seek from a snapshot** takes the events of the step after it from the stream block before
  it: the writer writes the snapshot at T after the block that ends with step T's events
  (`tagpu_recfmt.h`, `RB_SNAPSHOT`). S3 builds the seek.
- **An order node's owned object outlives the node's hold only for a type-3 owner**: that owner's
  air proxy (vtable `0x4FD9E0`) deletes the object an install hands it (`0x490A10`). A skirmish has
  no type-3 owner, and a puppet holds `+0x52` at 0 (`puppet0`), so it matters to the takeover (S4)
  and to the multiplayer slice, whose remote units have type-3 owners.

### S2b — watching

Built as planned, but for what *As built* marks. The engine facts are
[the engine map](../exe-reverse-engineering.md#the-puppet-host-its-gates-the-30-tick-deadline-and-pass-4-disassembled-measured-2026-09-30)'s,
the evidence [the host note](demo-recorder-s2-host.md)'s, and what the module writes
[GPU status](../gpu-status.md) §2.6f.

- **Opening a recording** goes through the engine's own SKIRMISH Start.
  `tacli replay play <instance> <recording> [--perspective RECORD]` writes the instance's skirmish
  settings from the header, copies the recording into `replays\`, writes the request file
  `tagpu_replay.play` (`play <name> [<record>]`), and clicks Start. Four sites are installed with
  the others, inert unless a request armed the level:
  - **The seat fill's call `0x47B092`.** It validates the request before any write: every puppet
    gate and the teardown wrap in, the format, the layout's hash, a skirmish, no network, the map's
    settings name, two to nine seats, as many players as seats, each seat's side and colour within
    this game's counts, the unit limit and the view. Then it seats each recorded
    player as type 3 through `0x464290` with its side, logo, name and alliance row, seats the
    viewer, points `+0x2A43` at the view, and disarms the victory and defeat tests (`0x4904B0`).
    On a failure the log names the reason and a normal skirmish starts. A recording from another
    build plays, and the log names both builds (D8).
  - **The settings persist's call `0x47B09C`.** It applies the recording's game options after
    `0x430F00` has written the player's own to the registry. The teardown restores them. From the
    level's first step, when the loader has read them, an observer on `0x430F00` puts the player's
    options in for the length of every save, so a save made during the playback (Options' OK at
    `0x45FD5F`, the damage-bars key at `0x496077`) writes the player's, and the save runs and
    returns unchanged.
    A save before the first step, which no known path makes, writes the recording's, and the
    teardown then saves again.
  - **The start units' calls `0x497963` and `0x497ACD`**, skipped.
  - **The first step**, at B2's stub `0x4954ED`: the level's counts, map, unit and weapon data and
    first GameTime against the header; then the start units through the create, the first image,
    the wind's seed and V's sight.
- **The viewer's seat** V is the lowest record the recording does not seat: type 3, with the watcher
  bit, holding `+0x2A42` for the whole level. Its PlayerInfo goes back as the Start's fill left it
  once the teardown `0x491B60` has returned, after the post-game table has left the watcher out:
  the seat activation does not clear the watcher bit, and a surrender to the main menu passes no
  reset (MEASURED).
  - Every path that selects or orders walks the controlled player's units, of which this seat has
    none, so the viewer cannot select or order a puppet unit.
  - Four gates cover the rest of the viewer's input: typed commands (`0x417B50`, every one
    refused), the debug keys (`0x4956C0`), the resource transfer (`0x464C60`) and saving
    (`0x4326B0`).
  - A recording that seats all ten records leaves no seat for the viewer, and is refused by name.
- **What the viewer sees.** Without `--perspective`, everything: V's sight counters are filled, V's
  bit is set in every cell of the explored map, and the radar pass marks every unit for a watcher.
  With `--perspective N`, `+0x2A43` is record N and the host's own radar pass computes N's radar
  picture, the recording's own where the recording showed N (`tarec info` names the recording's
  viewed player). V's sight is filled either way, so switching the perspective is `+0x2A43` and
  LosType bit 3 alone, for S5's selector.
- **The step.** B2's stub calls the host's unit tick, then the replay: the events of step T in
  recorded order, then the image of T+1, the world the recording machine's unit tick and the rest
  of its iteration T left. The rest of the host's iteration then reads what the recording
  machine's read. *As built:* the plan put the image before the host's unit tick, which left the
  sight stamps of 15–20 of 1,413 units a tick behind at every dump.
- **The apply** runs each step on the game thread. Every value is bounded before it becomes an
  index or a pointer. The completeness note's *What restoring these records needs* is its
  checklist.
  - **Units are placed with the engine's set-position `0x48A9F0`** at the recorded position and
    the altitude the movement pass had before its fix-up. Then the walk-class and occupancy parts
    `0x43DA70` and `0x43DB50` run, which start the walk and occupy scripts, and the fix-up's values
    go in. `+0x110` bits 2–3 and `+0x10A` are written only by those parts, and compared. The
    placement part `0x43D6D0` and the altitude fix-up `0x48A870` never run on a puppet.
  - **Players**: every record but V's, raw under the exclusion file, but the players phase's
    30-tick deadline `+0xF0`, which the host keeps (*as built*: written, it arrived already
    advanced, and the host never ran the viewed player's radar pass; the host note §2.3). An image
    goes in only with both names ended inside their 0x1E-byte fields, and a seat's PlayerInfo image
    only with its side and colour within this game's counts; the engine copies a name to its end and
    indexes a seat's side and colour tables unbounded, and itself leaves colours outside its count
    in the records it does not seat. Every seat's `+0x18` is GameTime each step, for
    the pacing gate.
  - **A unit's `+0x110`** takes the image's bits less the engine's and less the exclusion file's
    rows, so the viewer's selection, marks and build page are the viewer's own.
  - **Carry links** follow the recorded chains through the engine's attach `0x48AB70`, bounded
    first. They are never written raw.
  - **Order lists** are reconciled node by node against their images through the engine's
    constructor `0x43A0C0`, linker `0x43AD10`, reference set `0x489690` and destructor
    `0x43A1F0`, with every node's puppet rows held.
  - **Events are replayed through the functions that made them:** a create through `0x485F50`
    into the recorded slot; a death through C4's wrap of the destructor, after the victim's
    recorded fields; a shot through the receiver `0x49D270`; a script start through `0x4B0B00`,
    `0x4B0A70` or `0x4B0940`; a build done through `0x41B8D0`; a state change through `0x48B090`;
    a reply through `0x47F780` or `0x47F850`, whose own viewer test plays the watched seat's; a
    nano spray through `0x4720D0` or `0x472200`. What a create nests is recorded before the create
    and finds no unit; the replayed create makes it again. Each event is counted as played or not,
    and the log names the classes of those not played.
  - **A recorded YARD_OPEN** is applied after the positions, through the re-claim `0x47C790` and
    the passability refresh `0x440A40`.
  - **The wind** is seeded once, from the recording's first image, so every draw after it is the
    recording's.
- **The recording is the only writer.** On a puppet host, twelve prologue gates
  (`tagpu_puppet.c`), with the viewer's four above:
  - damage is refused at the apply `0x489CE0`, where every hit lands, for a puppet's unit
    (*as built*: the plan put it at B3's drop);
  - `Send_UnitDeath 0x4864B0` is refused for a puppet's unit, so bit 14 is written raw and a dying
    unit lives until its recorded death (*as built*: the plan had the reaper skip puppets);
  - COB's SET, ATTACH and DROP and the two replies are refused for a puppet's unit, except inside
    the replay's own create and reply;
  - the host's meteor shower does not run;
  - every order node on a puppet is held disarmed (`puppet0`, `puppet1`).
  - *As built:* Impure's per-slot hooks, B4's stamps and C4's marks, run on puppets unchanged,
    where the plan had them skip puppet slots. Each writes from the slot, GameTime and its own
    previous value, at the creates and deaths the replay makes at their recorded points, so its
    table agrees with the recording's; S2b writes no declared table but the wind's, and a hook
    that skipped puppets would have left its table differing.
- **Getter 75** answers, for each puppet seat, the recording machine's bit, from a per-seat table
  built from the header at the seat fill, declared `SIM_CONTENT` in `tagpu_puppet.c` and constant
  for the level. The recorder refuses to record a puppet host.
- **The dumps and the comparison.** `dump <name>` in `tagpu_replay.trigger` writes the live world
  with a header naming its tables, and `tools/replay/tarec.py compare --puppet` compares it with
  the recording read back to its GameTime: every unit, player and order list image and every
  declared table, paired by name, since each build numbers its tables in its own order; less the
  contact bits, V's record, the controllers and the viewer, and with the contact bits when the
  dump's viewed player is the recording's.

#### S2b, verified and measured

On the reference setup under Wine, 2026-09-30, a dump every 8 or 12 seconds of play, each compared
with the recording read back to its GameTime (`tarec.py compare --puppet`):

- **The 1,432-unit battle** on Town & Country (3,810 iterations), recorded by an earlier build of
  this landing, so also a recording played by another build (D8). In full visibility, and from
  record 0, the recording's own viewer: every one of the nine dumps inside the recording equal
  (GameTime 384 to 3,712, 1,413 live units down to 1), units, players, order lists and every
  declared table, and from record 0 the radar picture too. The events played kind by kind: 1,430
  creates, 1,433 deaths, 7,175 shots, 19,051 broadcast starts, 19,341 of 19,600 owner-only starts,
  1 build done, 37 of 62 state bytes, 16,780 of 16,805 replies and 459 sprays; not played, 25 state
  bytes and their 25 replies nested in a create, which the replayed create makes again, and 259
  TargetCleared starts nested in a death, which the destructor's order purge starts on the unit it
  destroys and a puppet's held nodes do not. Refused on puppets: 252 `Send_UnitDeath` calls, the
  reaper's on units the recording marked dying [INFERRED: its other callers need a network game or
  a type-1/2 owner], 26 SETs, 37 replies and 3,809 meteor showers, one an iteration; no hit reached the apply, since a puppet's shots do no damage by the impact's owner
  gate (B7).
- **The transport game** on Town & Country (3,090 iterations: two Atlases loading, dropping and
  reloading Peewees), recorded by the landing build, whose recorder took its replies through the
  reply gates: `tarec check` clean, and in full visibility and from record 0 every one of the ten
  dumps inside it equal (GameTime 288 to 2,880), the radar picture too from record 0. Every event
  played but one state byte and its reply, nested in a create; refused, 6 SETs, 7 replies and
  3,089 meteor showers.
- **The stock-limits build** recording a four-AI game on Core Prime Industrial Area (2,010
  iterations, 0 corrections, `tarec check` clean) and playing it in both views: every one of the six
  dumps inside it equal, the radar picture too from record 0; refused, 16 SETs, 8 replies and 2,009
  meteor showers.

**The landing review's fixes**, verified the same day. The battle again, in both views: every one
of the four dumps inside it equal (GameTime 800 to 3,200), the radar picture too from record 0, and
the same counts as above. The transport game with the player's registry set apart from the
recording's options (commander death 1 and start location 0 against the recording's 0 and 1): after
Options' OK during the playback the registry held the player's values, where a build without the
observer wrote the recording's (the same clicks, the store rewritten with 0 and 1); V's watcher bit
was set during the playback (`+0x9B` 0x54), and a build that logged the restore showed it put back 0
after the teardown of a surrender to the main menu, a path that runs no reset of the records.
The reader's check of an order list cut short at an iteration's end was verified by reading only:
no malformed file was built for it.

**What the checkpoints caught.** From record 0 the host at first showed a radar picture of its
own: every unit had the bits its create gives it, bit 9 on record 0's 352 and nothing on the other
1,061, where the recording had bit 8 on all of them. The host had never run the viewed player's
30-tick pass, whose deadline the apply wrote already advanced; it now leaves that deadline to the
host (the host note §2.3). And the dumps named their tables by index, so a recording from another
build compared the wrong tables; a dump now carries a header naming them.

### Decided 2026-09-30

- **S2 is two landings**, S2a and S2b.
- **The mover's and the order node's rows above are approved**, with a new class, `puppet0` and
  `puppet1`. Such a field is recorded and compared off a puppet. On a puppet it is held at 0
  (`puppet0`) or set (`puppet1`), and a takeover restores it. The class covers the order fields that
  arm owner code when a node is destroyed:
  - `+0x06` bit 1, the handler's own call, held 0;
  - `+0x42` bit 22, a `StopBuilding` owed, held 0;
  - `+0x52`, the handler's owned object, recorded as its class code, held 0;
  - `+0x42` bit 16, set when the destruction resets no weapon target, held set.

  A replayed death, attach or teardown then only unlinks and frees a puppet's nodes, and never
  runs owner code a second time.
- **On a puppet host the engine computes the contact bits for the viewed player.** The host's
  radar pass runs for `+0x2A43`. The recorded bits are kept for a takeover, and compared when the
  recording viewer's perspective is shown. This amends 2026-09-29's "a replay shows the recording
  viewer's radar picture": that perspective still shows it, computed by the engine from the same
  inputs, and every other perspective shows its own player's radar, as the product page has it.

- **`tagpu_replay.c` is `presentation`, `tagpu_puppet.c` `sim` and `tagpu_recreader.c`
  unlisted** (the owner, "two files"). The replay writes engine state only through the
  engine's functions and the recorded images, and keeps no simulation state of its own; the puppet
  table and the gates' own-calls flag are `tagpu_puppet.c`'s, declared there.

### What S2 leaves

- **Selecting a puppet unit**, its order markers and a factory's queue: S5's perspectives and
  overlays.
- **Features, wrecks and the footprint grid's contested cells**: S3. Until then a replay's features
  change only by the replayed deaths' wrecks [INFERRED: a host applies no damage and runs no order
  handler, so its trees do not burn or fall and nothing is reclaimed].
- **The path a mover's class object holds.** A takeover plans a new one from the orders (S4).
- **Recordings that seat all ten records.**
- **The declared tables are compared, not written**, but for the wind's generator, seeded at the
  first step. B4's stamps and C4's marks agree because their hooks run on puppets as they ran
  there; S3's restore writes the tables.
- **The playback's own cost** is not measured.
- **The replay's statics are outside the simulation-state check**, `tagpu_replay.c` being
  `presentation`: a takeover (S4) that resumes play from a playback has to account for them.
- **A network recording's controller changes** are not followed: getter 75 answers the header's
  controllers for the whole level. In a skirmish the one in-play change, the console's AI toggle,
  flips types 1 and 2, which getter 75 answers alike ([the host note](demo-recorder-s2-host.md)).

## S3 — seeking

Planned 2026-09-30, once S2 was in, from two passes over the disassembly: the feature grid and the
wreck pool with every root that changes them, and the main block and `.data` with what the
simulation reads across iterations. Their facts go into the engine map with the landing that uses
them.

### How S3 is cut

Three landings, as S2 was two, each a finished unit verified by running it:

| Landing | Delivers | Verified by |
|---|---|---|
| **S3a — features and wrecks** | the map grid's cells and the wreck pool recorded every iteration, with the events at their roots; their changes replayed on puppets through the functions that made them | S1's and S2b's checks with the features in them: every dump equal to the recording, live and on puppets; the capture's cost and size |
| **S3b — the globals, and seeking** | the main block's and `.data`'s simulation fields (a positive list) and the projectiles recorded, and both whole in every snapshot; the rebuild in place from a snapshot on a puppet host; `tacli replay seek` | a fresh process seeking to a dump's GameTime is equal to that dump; forward and backward seeks; seek times against the target |
| **S3c — the round trip and the harness** | the restore on a live skirmish, the rules against a vacuous pass and the null twin, `tools/replay/tareplay.py` with its fixtures and tiers, the CLAUDE.md replay gate, the landing command's Step 4b, the nightly tier | the round trip over the fixtures, none passing vacuously |

### S3a — features and wrecks

A recording carries the feature grid and the wreck pool, every call that changed them, and the
simulation RNG's draws those calls made; playback makes each change through the function that made
it. The engine facts are [the engine map](../exe-reverse-engineering.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30)'s.

**What the engine keeps.**

- **The grid**, `[main+0x14287]`: `W = [main+0x14233]` by `H = [main+0x14237]` cells of 13 bytes,
  row-major (`0x481550`), made per level by LoadMap (`0x48399D`) and freed at `0x483E7E`. It holds
  the features and the units' footprints together: the two unit slots, the height, the metal, the
  feature's def, an anchor's record or a footprint cell's way back to its anchor, and the flags.
- **The wreck pool**, `*(main+0x1420B)`: 2,048 records of `0x30` bytes in stock, 8,192 raised, on
  three lists linked by signed 16-bit indices: active `main+0x14213`, settled `+0x14217`, free
  `+0x1421B`. A record holds a 3DO feature for its whole life — a corpse, a heap — and a GAF feature
  while its burn, death or reclaim sequence plays. A free record's bytes are simulation input: a
  corpse starts with the velocity its record's last tenant left.
- **The writers.** A search of `.text` for every operand naming the grid, the pool, its heads or the
  cursor, and of every caller of the cell getters and the feature functions, finds in play: the
  features' bytes written by the spawn `0x423C50`, the destroy `0x4246B0`, FeatureDie `0x423550`,
  the burn `0x4233A0`, the damage `0x4244B0` and the swap `0x423710`, and the draw's flag bit 2;
  the unit slots and the yard bit by the stamp `0x47CC30`, the clear `0x47D0E0` and the re-claim
  `0x47C790`; the pool by those, the list move `0x4232F0`, the feature tick `0x424050` and the
  corpse `0x486360`.
- **The feature tick `0x424050`**, once an iteration at `0x495585`, after the unit tick, the
  projectiles, the explosions and the players phase, makes the features' only random decisions, at
  six calls of the simulation RNG `0x4B6C30`: reproduction, which draws for every GAF feature at
  rest its cursor passes (no stock feature reproduces), and the fire's spread and a burn's length.
  The rest is deterministic: the 3DO physics, the sequences' steps, a burn's end and a death's or
  reclaim's. Its smoke draws the CRT's `rand` and is presentation.
- **A death's explosion damages features on the owner's machine alone.** The owner gate
  `0x49A01B..0x49A047` passes by an explosion whose owner is a type-3 player, a picture; the corpse
  is made on every machine, after the explosion (`0x486D50`, then `0x486D6F`).

**What it records.**

- **The grid**, each cell's 13 bytes less the draw's bit 2: every cell at the level's first capture,
  handed to the writer whole through the ring (`RK_GRID`), then each cell that changed. The ten
  writers are observed at their entries, and each marks the cells it can write, a cell wider each
  way — a def's footprint at the spawn, the feature's at the destroy, FeatureDie, the burn, the
  damage and the swap, the unit's at the stamp, the clear and the re-claim — and the capture
  compares the marked cells with its copy. A rotating sweep compares a 64th of the grid each
  iteration as well: a changed cell that no writer marked is recorded when it finds it, late, and
  counted in the end record, which names a writer the search missed. Comparing the whole grid every
  iteration would be 7 MB on Two Continents, and a write watch would see the draw rewrite bit 2 on
  every drawn cell every frame.
- **The wreck pool**, compared whole each iteration (7 µs in stock and 26 µs raised, both copies
  cold, a scratch program on the reference setup): each record that changed, laid out by its def.
  A 3DO record's model instance is recorded as nothing (the replayed spawn makes its own), a GAF
  record's two sequence pointers as which of the def's sequences each is — the second, the shadow,
  only while the record uses it (`+0x2F` bit 2) and as none otherwise, since until then it holds
  what the record's last tenant left — and the rest raw. With it,
  the lists' three heads and the reproduction cursor, when any changed.
- **The calls that change them, at their roots.** A feature function called outside every other
  one is recorded at its entry with its arguments and the call's site, and at its return with its
  result where it has one (a record for the spawn and the swap, 0 or 1 for the destroy). The
  feature tick is a root at every call, and carries the cells that changed since the iteration's
  images, since reproduction reads a unit slot (`0x424189`). The six draws are recorded inside the
  root that made them. **A death is a bracket** from the destructor's entry to its return (one the
  capture's frame stack has no room for is not recorded, and ends the recording); the
  calls the destructor makes itself — the corpse — are its own and not recorded, and its explosion
  (`0x486D50`, redirected to a stub) is a frame inside which the area damage's calls are roots.
- **The header** carries `W`, `H`, the pool's size and the feature defs' count with a hash of their
  names; a capture that finds the grid, the pool or the count changed ends the recording
  (`INCOMPLETE: the features were not the ones it began with`). **Snapshots** carry every cell, every
  record that is not all zero, and the heads.

**Playback.**

- **The host decides nothing about features.** Seven more gates in `tagpu_puppet.c` refuse the
  feature tick and the six feature functions on a puppet level from its first step (before it the
  loader places the map's features through the spawn); each names the first call it refuses. A
  puppet's shots and deaths damage no feature by themselves (the owner gate), and its order
  handlers do not run.
- **Each root is made through its function where the recording made it**, with its arguments
  bounded — a cell by the grid, a def by the defs, a weapon by the weapons; FeatureDie and the
  damage index the grid with their `x` and `z` and never test the cell they get — and its draws
  armed, so the six calls take the recording's values while their site and argument agree. The
  tick's cells go in before the tick. **A death** is replayed as S2b replays it, with the feature
  gates open, so the host's destructor makes the corpse, and the roots its explosion made are made
  when the host's destructor has made its explosion, before the corpse. A result that differs, a
  draw not taken as recorded, a root out of bounds and a death whose explosion the host did not make
  are counted and the first named.
- **Then the images**: a cell's and a record's values go in raw; their structure — a cell's def,
  whether it holds a record and the record or its way back to the anchor, a record's links, anchor,
  def, flags and sequences, the lists' heads — is compared and never written, since only the
  engine's functions keep the grid, the pool and its lists consistent with each other. A value that
  indexes is bounded before it goes in: a unit slot by the unit array and by its unit being alive
  on the host, since the stamp's contest follows a slot to its unit's owner with no test
  (`0x47CDCA`); a sequence frame by the sequence's own count (as `0x4B8B30` bounds it), read
  through the def's field the recorded ID names and never through the record; the cursor by the
  grid's cells.
- **The unit slots and the yard bit come from the images alone.** The stamp, the clear and the
  re-claim still run on the host — for S2b's placements through the set-position `0x48A9F0`, the
  creates and deaths, and the host's own unit tick — because the stamp also files the unit in its
  sort bucket, which no image writes. Each call's writes to the grid are undone at its return: the
  recorder's observer at its entry hands the unit to `tagpu_puppet.c`, which saves the footprint,
  a cell wider each way, and puts the slots and the yard bit back. The three write only inside the
  footprint; the clear nests re-claims through its bucket walk's callback, so the saved frames are
  a stack, two deep in play. A deeper call or a footprint wider than 64 cells either way is counted
  and named. Without it the host's slots ran an iteration ahead of
  the recording's: an aircraft's recorded slot follows its position by an iteration, and the
  placement stamps it at the image's position. A recording plays only when the recorder's hooks
  are in, since the draws, the explosions and this undo are taken there.

**The comparison.** A dump carries the grid, the pool, its heads and the cursor, and
`tarec.py compare` compares them byte for byte less bit 2, live and with `--puppet`.

**Loosened, as the owner agreed on 2026-09-30:** four rows in `replay-layout.tsv` (the cell's draw
bit, the 3DO record's model instance, the GAF record's two sequence pointers as IDs; the second is
none while the record does not use it, when the engine does not read it), and two
things not recorded: the animating defs' idle phases (`FeatureDef +0xCC..+0xE3`, which the tick
steps and only the draw reads [INFERRED]) and the movement classes' passability, which is derived
from the grid and is not refreshed for cells an image writes (puppets do not pathfind).

**Not the stock save's shape.** The save `0x424890` and its load `0x424C00` carry what a new game
needs to look the same, not what makes it continue the same: no record indices, list order,
velocities, frame timers, the shadow sequence's frame, the countdown or the cursor.

#### S3a, verified and measured

MEASURED 2026-09-30 under Wine, format 3, scripted runs on a private X server:

- **After the landing review's fixes** (the layout's hash changed, so every fixture was recorded
  again): the forest fire, the battle, the carry links, a naval battle (`scenarios/naval-push.json`,
  213 deaths, 42,649 wreck records written) and the stock-limits game each recorded and played on
  puppets; every live and puppet dump EQUAL, 0 cells and 0 records left for their structure or
  for a bad value, every footprint write undone. The battle's capture: 564 µs on average, 1.64 ms
  at worst.

- **The fixture**, `scenarios/s3a-forest-fire.json`: six Rocko and two Zeus against six A.K. and
  four Storm in the densest forest of Two Continents, and a construction kbot reclaiming a
  laboratory wreck behind them, recorded for 4,710 iterations and ended by a scenario that clears
  the map. The recording: 4,808 feature roots — 4,710 ticks, 73 weapon impacts' damage outside a
  death and 23 inside the explosions of 7 of the 23 deaths, the reclaim's FeatureDie and the
  scenario's own spawn — and 295 draws (the burn's 76, the spread's 163, reproduction's 56); 0 cells
  changed that no writer had marked. `tarec.py check` explains every change of every step, the five
  snapshots equal the stream, and three dumps taken live are equal to the recording.
- **Played on puppets** (`tacli replay play`, full visibility) on the landing build, seven dumps at
  GameTime 640 to 4,576 are equal to the recording (`tarec.py compare --puppet`): the grid, the
  pool, its heads and the cursor, with the units. Every root was made and returned what it had,
  every draw was taken as recorded, every death's explosion made its calls, no cell or record was
  left for its structure or a value out of bounds, and all 2,295 footprint writes were undone. The
  host's own feature tick was refused 4,709 times and no other feature call was.
- **The first build diverged there**, at the first death whose explosion burnt a tree (GameTime
  1,379): it replayed a death's feature calls by replaying the death, and on the host the owner
  gate makes a puppet's explosion a picture. The explosion's calls are now roots.
- **The cost**, the same run: the capture took 135 µs an iteration on average and 351 µs at most,
  the first 10.5 ms. The stream was 10,209 KB raw and 1,302 KB deflated; 7,077 KB of it is cells,
  almost all the first iteration's whole grid, and 1,260 KB wreck records, a burning feature's
  record changing every tick. The five snapshots were 55 MB raw, each with every cell, and the
  file 4.1 MB.
- **S2b's three games again**, recorded with the features and played on puppets: the 1,432-unit
  battle on Town & Country (3,810 iterations: 5,749 feature roots, 106 draws, 1,432 deaths), the
  carry links (3,090 iterations) and the four-AI game in the stock-limits build (2,010
  iterations, the 2,048-record pool). Every check is clean, no cell changed that no writer had
  marked, and every dump is equal on puppets, five, six and five. The battle's first playback
  found one cell's slot B an iteration ahead of the recording, the cause of the footprint
  writers' undo (*Playback*); its next found 1,200 of them called inside a clear. With the undo
  stacked, the battle's playback undid 315,067 footprint writes and missed none.
- **The cost in the battle**: 569 µs an iteration on average and 2.21 ms at worst (GameTime
  3,305, 111 KB into the ring), against S2a's 475 µs and 1.15 ms; in the carry-links game 123 and
  324 µs, in the stock-limits game 118 and 642 µs. The battle came to 68.8 MB for two minutes
  (S2a: 60.4): its moving units' footprint cells are 33 MB of its 177 MB stream before deflating.

#### What S3a leaves

- **The capture's cost in a battle** grew further past the product page's budget of 0.5 ms on
  average and 1 ms at worst: 569 µs and 2.21 ms (S2a: 475 µs and 1.15 ms). The pool's comparison
  is 26 µs of it; the rest of the features' share is not broken down.

- **Every snapshot carries every cell**, 7 MB raw on Two Continents and 21 MB on Seven Islands;
  deflated, the fixture's five snapshots came to about 2.8 MB of its 4.1. S3b's seeking decides
  whether a snapshot needs them all.
- **Passability** is not refreshed for the cells an image writes (above).
- **A reproducing feature** would replay as recorded, reproduction's row bug included (the engine
  map); stock has none.
- **An indestructible feature that burned** would keep its record active for ever, in the
  recording and in its replay alike; stock has none (the engine map).

### S3b — the globals, and seeking

Planned in detail when S3a is in. From the research:

- **The positive list**, each field read by the simulation in a later iteration: GameTime
  `main+0x38A47`; the wind `main+0x37EC4..+0x37EE5` less `+0x37EC8`, with Impure's generator; the
  game end's countdown `main+0x39239` and flags `+0x3923B`; LosType bits 0–2 `main+0x14281`; the
  simulation RNG's seed `0x51FC88`; the meteor state (`0x5122E8`, `0x512318`, `0x51231C`,
  `0x512320`, `0x51232C`, `0x512330`, `0x512334`); the pathfinder's budget `0x5119E8..0x511A3B`;
  `0x51E6C4`; and `0x51E684`/`0x51E688`.
- **The projectiles**: the count `main+0x141F3` and the pool `*(main+0x141F7)`, `0x6B` bytes a record.
- **Both images whole in every snapshot**, for S3c's null twin: the main block (`0x3924D` bytes,
  allocated once by `0x41D920`) and `.data` (`0x501000..0x52B65C`).
- **The rebuild** on a puppet host, at the step: experiment 2's order, with S2b's appliers for every
  value, S3a's features, the positive list and the declared tables, and the projectiles,
  explosions and particles of the moment before cleared.
- **What a rebuild cannot restore**: the AI, pathfinder and objective objects, which live on the
  heap outside both images; and where the CRT's `rand` stands, which depends on how many frames were
  drawn, so the meteor shower (six draws in `0x437DE0`) and `0x490360`'s timer are not reproducible
  even between two stock peers.

### S3c — the round trip and the harness

Planned in detail when S3b is in: the [completeness note](demo-recorder-completeness.md#the-replay-test-harness)'s
harness, as decided on 2026-09-29.

## S4 and S5

Each is planned in detail when the landing before it is in. The table above is their scope; the
[completeness note](demo-recorder-completeness.md)'s [open items](demo-recorder-completeness.md#open-items)
are theirs.

## Open

- **Where recordings live and what they are called** (`replays\`, `.imprec`): chosen for S1, and
  shown by the browser in S5.
- **The stream's quantization, the snapshot interval and the crash promise** are the product page's
  open decisions 2, 3 and 5; the landings above measure what each needs.
- **`s_ghost`, the build ghost's piece masks,** is declared `SIM_STATE`, so it is recorded, though
  it is derived from the unit types and their scripts and only the draw reads it.
