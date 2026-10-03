# The fork rules (S4c)

A recording has an original timeline, ID 0, and at most three live forks. Taking over
adds a fork; when three already exist it deletes the least recently used live fork,
excluding the timeline being played. The original is never an automatic eviction
candidate. Equal timestamps are resolved in physical segment order. `takeover
--dry-run` reports this choice; the real takeover scans again under its writer lock.
The prediction reserves nothing.

`tagpu_recfile.c` manages files only. It reads no engine memory, installs no byte
patch and creates no graphics object. The takeover still uses S4b's rebuild and
capture ordering ([the takeover](demo-recorder-s4-takeover.md)).

## Metadata and selection

`RB_TIMELINES` is block kind **6**, after `RB_MINIMAP` = 5. Format remains 4.
Its payload is a complete table of 16-byte little-endian rows:

| bytes | meaning |
|---|---|
| 0–3 | timeline ID, u32; `UINT32_MAX` is invalid |
| 4 | 1 live, 2 deleted |
| 5–7 | zero |
| 8–15 | last-used time, u64 UTC FILETIME |

Both readers validate its CRC, row size, state, reserved bytes and unique IDs.
The latest complete table wins. Before any table, a segment defaults live with
its header's `RT_WALLCLOCK`. A header appended after the last table defaults the
same way; a header preceding that table must have a row. An absent live ID is
invalid. There are at most 64 physical segments and 65 rows: compaction keeps a
highest-allocated-ID deleted row even if its segment is removed. A later fork
therefore cannot reuse an ID. Damage in the final indexed stream or snapshot
followed by a valid metadata block refuses mutation; it cannot be cut as a tear. Absent lower tombstones need not survive compaction.

The append writer writes its new header and the updated table before its first
stream block. Readers cannot select a deleted timeline, including ID 0, but can
use deleted ancestors in a live descendant's chain. Deleting the original is
allowed. Its header remains the format envelope even if no live chain needs its
stream. A file with no live timeline can be inspected but cannot play.

## Shared history, compaction and export

A worker retains every live segment and its ancestors. For each deleted ancestor
it retains whole blocks whose first iteration is before the greatest immediate
child fork point required by a live descendant. A stream block crossing that
point stays whole; recorded iterations are never rewritten. Its end becomes
`REC_END_PRUNED` = 12, naming the last retained stream iteration. Unused segments
are removed. Every retained block's packed CRC is checked before it is copied.
While the original is live, its header and simulation blocks remain byte-identical.

Export keeps only the chosen live timeline's chain, marks its ancestors deleted,
and applies the same prefix rule. IDs stay the recording's IDs, so an exported
fork is selected by its original ID. Export refuses to replace an existing file.
The root header and highest-ID tombstone are bookkeeping, even when not needed
for playback.

## File ordering and lifetimes

Every Impure recording writer opens the persistent `PATH.lock` file with share
mode zero before opening the recording. This includes new recordings, takeovers
and maintenance. The lock is never unlinked: two writers must not acquire two
successive lock objects at the same path. A takeover moves this handle alongside
its recording handle through the arm, ring and writer; each failure closes the
recording before releasing its lock. The writer releases it after its last write.

Maintenance scans with a read/write source handle sharing READ, copies into
`PATH.tmp`, flushes the completed temporary file, closes the source, then calls
`MoveFileExA(REPLACE_EXISTING | WRITE_THROUGH)`. The path lock remains held across
that close and replacement. This is the ordering invariant, rather than an
assumption about how quickly another writer runs. Impure addresses each recording
through its `replays` path; independently created hard-link aliases do not share
this path lock.

**Negative result, measured on the reference Wine setup:** replacing an open
destination failed with error 5, including a destination opened with share DELETE.
`ReplaceFileA` failed with error 32. Closing the destination made `MoveFileExA`
succeed even with the temporary file still open. The source handle alone cannot
protect the close-to-replace interval.

Other readers deny deletion sharing and can refuse replacement. Touch or delete
may still commit its metadata table; the result then explicitly says compaction
is deferred. A later playing-end touch or append-writer close retries. A failed
replacement leaves the old recording intact; a crash can leave a temporary file.
A source that cannot be written skips the touch with a log line (D5).

A playing and queued deletion reserve the canonical path, kernel file identity and timeline ID
under the worker publication lock. A reservation matches the timeline and either
the canonical path or file identity, so replacement cannot lose protection. A deletion of that playing is refused, and a
playing cannot claim a pending deletion. The playing claims before its metadata
scan and releases after its reader closes. A worker releases its reservation
only after the operation ends. This protects the current playing within this
process, including requests through a differently spelled path to the same file.

The bounded queue tracks eight worker thread handles. Exit seals publication and
waits for all workers. Recording writers enqueue their close compaction before
signalling their done events; exit waits for those events before sealing the
maintenance set. The coverage build flushes after both waits, and the harness
collects those final coverage files after stopping the process.

## Driving it

- `tacli replay forks INSTANCE FILE` lists physical segments, state, last-used time
  and the highest allocated ID without launching a game.
- `tacli replay forks INSTANCE FILE delete ID` deletes a live timeline.
- `tacli replay forks INSTANCE FILE export ID OUT` exports its chain. `OUT` is a
  simple file stem in the instance's `replays` directory.
- Mutations require a running instance, at any screen, and `FILE` must be in that
  instance's `replays` directory. They operate on that actual file, not an implicit
  copy returned to a caller's original path.
- The trigger forms are `forks "NAME" delete ID` and
  `forks "NAME" export ID "OUT"`. tacli prefixes the name with `@` and a
  32-digit hexadecimal request token; the result carries that token, so an older
  worker's result cannot complete a new command, including CRLF log lines. The common game-thread flip observer looks for
  these on every fifth frame, including menus. Seeks and dumps retain their
  existing in-play frame-visit ordering. Each request reports done or refused.
- `tacli replay takeover INSTANCE FILE --at T --player P --dry-run` predicts
  eviction offline. Without FILE it asks the running playing. Both refuse a child
  boundary or a time past the selected timeline's end. No takeover occurs.
- `tools/replay/tareplay.py forks battle [--cover]` exercises the rules. The
  landing tier also runs its X lane on `battle` and `stock-limits`, after the
  existing L, P, R, Q, K, F and W lanes when present.

## Verification and remaining work

The fork lane checks the live count and every live chain after each fork, touches
ID 2 to change eviction order, predicts and removes ID 1 on fork 4, forks ID 5
from ID 2, deletes ID 5 in another recording while this file's 5 plays,
refuses deletion of this playing, then deletes 2. It checks 2's retained
prefix and the reclaimed bytes, plays 5 across both boundaries, exports that
chain and compares both puppet dumps. It refuses overwrite of the export and
checks that deleting highest ID 5 is followed by allocation of ID 6. The live
original's segment hash is checked throughout.

**Verified on the reference Wine setup, 2026-10-02:** the latest complete
landing-tier run passed all eleven fork checks on each of the raised- and
stock-limits coverage builds, including deletion in a different recording while
timeline 5 plays and a real writer refusal with the isolated `replays` directory
made read-only. Permissions were restored, the game continued, and exit completed
before final coverage collection. Forks 1–3 remained live; touching
2 made fork 4 evict 1. Fork 5 from 2 evicted 3. After explicit deletion of 2,
physical IDs were `[0, 2, 4, 5]` with only `[0, 4, 5]` live; the export held
`[0, 2, 5]`, with only 5 live. Both the two-boundary playing and exported playing
were puppet-dump EQUAL. Removing 5 was followed by allocation of 6, with physical
and live IDs `[0, 4, 6]`.

The complete run graded DLL source at `2563e375`: 443 checks passed, none differed;
forest-circle ended early and could not run its later lanes. Its corrected fixture
rerun passed all 23 checks at the same requested ticks. Together these runs cover
all 18 fixtures: 466 passing checks and 54 round trips EQUAL, with zero residue
after poisoning. The full coverage gate reached 48 of the 49 changed functions
on every required build. Only `tagpu_recorder.c:ring_free`, the allocation-failure
cleanup before the writer starts, remains UNCOVERED on both builds. The owner
approved that exception for S4c on 2026-10-02 under OQ7, with the gap documented
and tolerances unchanged. The automated landing command did not record a passing
note; the landing records the owner's decision.

The review's file-invariant probes used the production reader and maintenance
source in a scratch Wine executable. Replacing a file while retaining an old
reservation identity refused both pending-play admission and active-play deletion
through the canonical path; the refusal preserved its request token. A corrupted
last stream followed by a valid metadata block refused touch with "damaged and
data follows it" and preserved the file's SHA-256. The same damaged payload at
the actual tail was cut and touched successfully. The tooling tests also cover
child-boundary, 64-segment and ID-exhaustion dry-run refusals.

Live rewind remains S4d. The browser's eviction confirmation and fork controls
are S5 work. S4b's mixed-side MAIN2 panel and the naval COB guard after seeks
remain separate work; these file rules do not close them.


### Harness ordering found by the landing tier

The takeover's four-second judge observation does not establish that the roster
belongs to the takeover. `tagpu_packet_pub.c:roster_log` emits full dumps every
five seconds; the first battle attempt read the loader's four commanders before
the next dump. The collected later dump still contained all three ARMCK builders
at slots 344–346. The order check therefore waits for a dump after the takeover
acknowledgement and its following `units:` header, and uses that complete dump.
The order driver still checks the recycled slot's expected type on the game thread.

A playing-end touch changes the recording's modification time. The browser's
`scan` sorts newest first and moves its existing index entries into list order;
`index_save` preserves that order. The W fixture owns exactly three files, all
passing the mod filter. Its second Watch resolves the filename in the returned
browser's saved index order instead of reusing the row number from before playback.

The first complete tier run had 440 passing checks, one naval-push sink-count
difference (`Stop`, 17 live versus 23 puppet over [451, 2611)), the stale battle
roster and two stale W rows. Its six `UNCOVERED` names were `recfile_exit`,
`tagpu_rec_exit`, `tagpu_replay_level_end`, `maintenance_end`, `fail` and `ring_free`.
The fixture worker used `tacli stop`, which sends SIGTERM and bypasses the game's
exit hook; copying files after that stop cannot collect the exit's coverage.
This run is evidence for the fork rules, not a passing landing gate. The naval
count difference remains visible; no exclusion or tolerance has been changed.

A separate coverage-build run of pre-S4c `main` (`84c0b957`) at the same
[451, 2611) window also failed the sink-count comparison: `0x4B08C0` from
`dll+0x1066A7`, `Stop` 12 live versus 17 puppet and `RequestState` 86 versus
91. Both puppet state dumps were EQUAL. This reproduces a count difference
before S4c; it does not establish its cause or close it.

The next full tier's forest-circle live game ended at 4380, before its requested
4710. Its last full roster held two ARM units and no CORE unit. Removing
commanders does not prevent a seat's last unit from dying. The fixture keeps one
stationary noncombat unit per seat away from the battle; `replay-end.json`
clears those units with the others. The corrected fixture's same ticks
2101/3361/3571/3601 passed all 23 checks, including the sink counts, three
round trips and fresh seek. Its recording ended at 4890 with zero corrections.


The harness quits collected runs through the game's menus. A live or puppet
level uses Options → Exit → Exit Game → Yes; an ended level uses ENDMSN's active
MainMenu button, then the main menu's Exit. A returned browser uses Back first.
It waits for that instance's process to disappear before collecting the final
coverage files and log. SIGTERM remains cleanup for a run that was not collected,
and `Worker.close` can hard-stop an owned instance after a failing run.

Scratch probes on the stock coverage build reached `tagpu_rec_exit`, `recfile_exit`
and `tagpu_replay_level_end` through the in-game exit, and reached writer `fail`
with a read-only directory. The injected `alt+f4` combo did not close the Wine
probe; that input is not the harness's exit route.
