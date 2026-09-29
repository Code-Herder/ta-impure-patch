# Demo recorder — the skirmish slice (the plan)

## Status

The [product page](demo-recorder.md#milestones-and-delivery-order)'s second milestone, planned
2026-09-29: recording, puppet watching, seeking, live rewind and takeover, forks, the Replays browser
and `tacli` automation, with the verification the
[completeness design](demo-recorder-completeness.md#build-order) lists as its step 2. Five landings,
each a finished unit verified by running it. **S1 is in progress**; nothing of the slice has landed.

## How the slice is cut

Each landing adds one capability that can be verified by itself, and the ones after it build on it.

| Landing | Delivers | Verified by |
|---|---|---|
| **S1 — the recorder** | the capture at `0x4954ED`, the writer thread, the file, self-verification, and a tool that reads a recording back | self-verification over whole games; the file read back to the tick of a dump and compared with the dump byte for byte; the cost and the size measured |
| **S2 — watching** | the objects a unit record points at (the mover, the order nodes), events at their roots, the puppet host, playback, the one-writer rule, getter 75 per seat, the nano spray | S1's checks over the new records; a recording played on puppets and compared with the recording at checkpoints; root counts equal and sink counts within a band |
| **S3 — seeking** | the rebuild in place from a snapshot, features and wrecks, the main block and `.data`, the replay harness and its restore round trip, the CLAUDE.md replay gate and the landing command's Step 4b, the nightly tier | the round trip, with its rules against a vacuous pass; seek times against the target |
| **S4 — rewind, takeover and forks** | the timeline in a live skirmish, Back to now, Play from here, forks in the file, the takeover from a recording | the harness's takeover run, judged like the compatibility suite's battle; the fork rules |
| **S5 — the browser and the controls** | the Replays browser, the timeline bar, perspectives, camera follow, the overlays, the recording switch in the settings screen | the browser and the controls driven through `tacli ui`, with pictures |

**S1 and S2 record every value exactly, at every tick.** That is what makes their checks exact. The
product page's stream — ten samples a second in whole world units, interpolated in playback — is a
landing of its own after S2, once playback can show its error; S1 measures the exact stream's size
so that the gap to the budget is known before then.

## S1 — the recorder

### What it records

At every iteration of the sim step in a skirmish (the game mode, `+0` of the object `[main+0x391E9]`
points at, is 2; 1 is a campaign and 3 a network game: `0x435100` is `mov eax,[ecx]`, called with
`ecx = [main+0x391E9]` at `0x41FB6C` and `0x4971C1`, DISASSEMBLED), from the level's first iteration
to its teardown, the capture takes an **image** of every live unit record, of the ten player records
(not the eleventh, which nothing activates) and of the rows of every `SIM_STATE` declaration.

An image has the record's shape, and `inc/tagpu_replay_layout.h` — generated from the exclusion file
the owner controls — is all that shapes it. A byte of a value field is copied. An `id:<table>` row
holds its target's ID in place of the pointer, by the table's offset rule; `NULL`, and a pointer
outside its table, get IDs of their own, the second counted. The bytes of `object`, `cache`, `timer`
and `drop` rows are zero. A declared table's image follows its layout the same way: an `SIMF_ADDR`
field with an ID table holds the ID, and an address with none, an `SIMF_LOCAL` value and an
`SIMF_OWNED` pointer are zero.

The capture compares each image with the game thread's previous one and puts the changed spans into a
ring; a slot whose unit was created, died or changed type goes whole. The writer thread keeps its own
copy of the world, built from the ring alone, and writes the file from that copy. It never reads
engine memory.

### Self-verification

Every iteration the capture also puts a rotating slice into the ring: the images of the next 50 unit
slots, of one player record and of the declared rows keyed by those slots. After applying the
iteration's changes the writer compares the slice with its copy. A mismatch writes the true image as
a correction, counts it by record and offset in the file's end block, and logs one line the first
time each record and offset differs. Since the change stream carries whole images, a mismatch is a
defect of the recorder's own pipeline, and S1's runs require none.

### The file

One file per recording, `<date> <time> <map>.imprec` in `replays\` in the game's folder, written by
the writer thread alone:

- **a header block**: the format version, Impure's build, the game options, the map, the content's
  fingerprints (the counts and hashes of the unit and weapon definitions), the players (name, side,
  colour, controller, alliances), the registry's lever list, the exclusion file's SHA-256, and the
  registry's declarations (name, row size, rows), so a reader can walk the tables' images;
- **stream blocks**, each a run of iterations, deflated with lodepng's `lodepng_zlib_compress`,
  which the build already carries, and checked by a CRC-32 over the block's header and payload;
- **a snapshot block** every 30 seconds of game time, the writer's whole copy. The interval is a
  placeholder: S3 chooses it by measurement against the seek target;
- **an end block**: the last tick, whether the recording is complete and why not, and the
  correction counts.

A block is written whole or not at all, so a file cut short by a crash reads up to its last complete
block.

### The game never waits

The ring is a single-producer, single-consumer byte ring of fixed size, allocated when the recording
starts. The capture writes an iteration's records and then publishes the write index (a release);
the writer reads the index (an acquire) before the records. The space an end record needs is kept
back, so the capture can always end a recording. If an iteration's records do not fit, the capture
ends the recording after the last whole iteration and marks it incomplete; a write error ends it the
same way, through a flag the writer sets and the capture reads. Nothing on the game thread waits,
sleeps or allocates once the recording has started.

### Its lifetime

A recording starts at the first capture of a skirmish level and ends at the teardown `0x491B60`,
where `tagpu_reclaim`'s wrap tells the recorder on the game thread. The capture's last act is the end
record; the game thread then drops the recording, and the writer finishes the file, frees the ring
and exits. Each recording owns its ring and its writer, so a game started while the previous file is
still being finished takes a new pair. A file the process leaves without an end block stays readable
to its last complete block, and its missing end block says it is incomplete.

### Where the code goes

- `src/tagpu_recorder.c`: the capture, called on the game thread from B2's stub at `0x4954ED` — one
  hook on the site, which now calls the capture and then the air pool's rebuild; the start and the
  end; the harness's `dump` verb. It names engine memory, so it has a line in `thread-split.allow`,
  `publisher presentation`: approved by the owner on 2026-09-29. `presentation`, because in S1 the
  recorder holds no state the simulation reads; S2 revisits it for the puppet-mode flags COB reads.
- `src/tagpu_recwriter.c`: the writer thread. It is not on the list, so the build refuses an engine
  address in it, and "the writer never reads engine memory" holds by construction.
- `inc/tagpu_recfmt.h`: the ring's records and the file's blocks, shared by both files and mirrored
  by the tool.
- `tagpu_simreg`: a walk of the declarations for the recorder, and the lever list for the header.
- `tools/replay/tarec.py`: reads a recording — its header and blocks, the world at any tick, and a
  comparison with a dump.
- The verb `dump <name>` in `tagpu_replay.trigger`, the harness's first: at the next capture, the
  live world's images to `replays\dump-<name>.bin`, in a snapshot's format.
- `impure.cfg`'s `recording`, on by default: the product's opt-out, whose menu entry comes with S5.

### Verified by

Scripted games of at most ten minutes each, on Wine, on a private X display:

1. four AIs for eight minutes: no correction, with every unit slot checked every 30 iterations, and
   dumps at three ticks, each equal byte for byte to the file read back to that tick;
2. the experiments' 1,432-unit battle in the raised build: the capture's cost per iteration, on
   average and at worst, against 0.5 and 1 ms, and the ring's high-water mark;
3. a skirmish loaded from a save, recorded from the load;
4. the process killed mid-game: the file reads to its last complete block and says incomplete;
5. a scratch build that corrupts one byte of the writer's copy: the correction names it, which shows
   the check is not vacuous (the scratch build is not landed);
6. run 1's size per hour, for the record against the budget.

### Gaps S1 leaves

- **The objects a record points at** — the mover, the order lists, the COB object, the model, the AI
  object, the control groups, the sight counters and the resource accumulator — are not recorded:
  their rows are `object` and zero in the image. S2 brings the mover and the order nodes, with layouts
  of their own: new records in `replay-layout.tsv`, so new rows for the owner.
- **The engine pools the DLL holds** (`SIM_ENGINE`: explosions, particle systems and their auxiliary
  geometry) and **the blocks declarations own** have no layouts yet.
- **Events** (shots, script starts, sounds, the nano spray): S2.
- **Features, wrecks, the main block and `.data`**: S3.
- **Network games** are not recorded: the multiplayer slice.

## S2 to S5

Each is planned in detail when the landing before it is in. The table above is their scope; the
[completeness note](demo-recorder-completeness.md)'s *What restoring these records needs* is the
applier's checklist for S2 and S3, and its [open items](demo-recorder-completeness.md#open-items) are
theirs. Rules S2 inherits from the notes:

- the puppet host seats every recorded player as a remote seat, disarms the victory and defeat
  tests, stamps each puppet's `+0x18` every tick, and steps only the movement pass's three
  presentation parts, writing the mover's layer and its `+0x2E` bit 2
  ([state evidence](demo-recorder-state-evidence.md#recording-and-replay-measured));
- events are played through the engine's own receivers, with every index bounded first;
- on a puppet the recorder is the only writer of the state the recording carries.

## Open

- **Where recordings live and what they are called** (`replays\`, `.imprec`): chosen for S1, and
  shown by the browser in S5.
- **A release cut between S1 and S2** would write recordings that nothing can open yet.
- **The stream's quantization, the snapshot interval and the crash promise** are the product page's
  open decisions 2, 3 and 5; the landings above measure what each needs.
