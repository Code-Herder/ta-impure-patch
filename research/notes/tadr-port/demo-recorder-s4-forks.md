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

**The claims.** A playing and a queued deletion reserve the canonical path, kernel file identity
and timeline ID under the worker publication lock. There are two claims (`recfile.c`, `s_claim`):
the timeline that plays, and the one a switch moves to while its second reader is open beside the
first (*Watching another timeline in place*). A claim matches the timeline and either the
canonical path or file identity, so replacement cannot lose protection. A deletion of a claimed
timeline is refused, and a claim of a timeline whose deletion is queued is refused. The timeline
a playing chooses is claimed before the scan it is chosen from and released after its reader
stops playing it; a worker releases its reservation only after the operation ends. This protects
the timelines a playing plays within this process, including requests through a differently
spelled path to the same file. A reader that only reads -- the HUD's walker -- claims nothing:
what keeps its bytes is the file's lifetime (*Watching another timeline in place*).

The bounded queue tracks eight worker thread handles. Exit seals publication and
waits for all workers. Recording writers enqueue their close compaction before
signalling their done events; exit waits for those events before sealing the
maintenance set. The coverage build flushes after both waits, and the harness
collects those final coverage files after stopping the process.

## Tickets and the HUD's route

`recfile_request` returns the request's **ticket**, never 0, or 0 when it is refused before it is
queued (a bad argument, no memory, every worker busy, the process exiting). Its outcome waits in a
ring of `REC_FILE_RESULTS` = 16 slots, filled in the order the requests finish: `recfile_result`
answers 0 while the job is queued or running, 1 done, -1 refused with the worker's reason, -2
unknown. A job is given a ticket only once it has a worker's slot; the deletion of a timeline a
playing claims is a queued request whose outcome, `timeline N is in use`, is in the ring at once.
**The ordering, under the queue's lock:** the worker writes the result before its job leaves the
pending set, so from its issue a ticket is pending or in the ring at every instant the lock is
free, until 15 completions after its own: a result outlives 15 completions after its own. Each slot
names its ticket, so a ticket the ring no longer holds reads unknown, never another request's
outcome. `recfile_completed` counts the finished requests without the lock, for a reader
that wants to know some file may have changed.

The replay HUD deletes and exports a fork of the playing's file through
`tagpu_replay_fork_op` (`inc/tagpu_replay.h`): refused at once with a line and an outcome
(`replay: NAME: the HUD's fork request refused: <why>`) when the level plays no recording, a live
rewind owns it, the operation is unknown, the export's stem is not a file stem, or four requests
already wait; else queued with its ticket kept. The frame visit polls the tickets (`fork_poll`),
logs `replay: NAME: the HUD's fork request T done: <why>` (or `refused`), makes it the last
outcome (`tagpu_replay_result`, kind FORK) and refreshes the HUD's timelines. The trigger's
`hud fork delete <id>` and `hud fork export <id> "<stem>"` are the tests' way to it. Deleting the
<<<<<<< HEAD
timeline that plays is a switch first (*Watching another timeline in place*); the forks trigger's
deletion of it, which no switch precedes, is refused as in use.
=======
timeline that plays is refused as in use. An export runs while a playing reads the file: MEASURED
2026-10-23 on `battle`, the HUD's export of fork 4 during fork 5's playing holds `[0, 4]`.
>>>>>>> main

**The HUD's timelines** come from a cache the game thread refreshes at the first iteration, a
rewind's open, a takeover's plan and the frame visit after any file job finished, never per frame:
its scan is the one the trigger's dry run already ran on the playing reader (`segs_scan`, which
keeps the reader's place). Each row carries the timeline's `controlled` seat, its header's
RT_OPTIONS byte 18, which the recorder takes from `main+0x2A42`: in a fork, the player it was
played as. `tagpu_replay_timelines_serial` grows by one at every change of the rows or the axis,
the playing's end included, so a caller (the takeover's picker) knows when to read them and plan
again. READ: the takeover's `tk_live` stores that seat in `+0x2A42`, then `tk_end` arms the
fork, whose header `header_build` writes at its first capture point. MEASURED 2026-10-23 by the
X lane's `hud state` check on `battle`: forks 2, 4 and 5 each name the player taken over as. The axis runs from the root's first GameTime to the greatest last of a live timeline.

## Watching another timeline in place

`tagpu_replay_watch_timeline(id)` (`inc/tagpu_replay.h`, *THE SWITCH*) moves a playing to another
timeline of its file without leaving the level. The request refuses at once, with a line and a
SWITCH outcome, when the level plays no recording, a rewind owns it, a takeover waits, a deletion
waits for its own switch, `id` already plays, or the HUD's timelines have no live row `id`; else
it waits for the frame visit, the latest replacing one not yet taken. There, before any pending
seek, `tl_switch` takes it in one of two ways:

- **Re-chained** (`recr_rechain`), when every segment the reader has read and the iteration it
  holds are on `id`'s chain too: its chain is replaced and nothing else -- its world, decoder,
  segment and file position stay, and so does the host. **The invariant, structural:** a reader's
  state is a function of the blocks of `chain[0..cur]` it has applied up to `w.tick`; two chains
  sharing `chain[0..cur]` hold that segment's iterations identically, since a chain decides only
  where reading *leaves* a segment (`cover_end`); `recr_tick` consults the chain only in its
  boundary test before a read; and `w.tick` is at or before the new chain's end of the current
  segment. So afterwards the reader is one that had played `id` from the start. `recr_timeline`
  and `recr_rechain` choose a chain through one walk, `chain_of`. Two states are not shared, and
  the seek serves them: a level held at a boundary, whose step played its iteration's events and
  read no images after them, so the reader is not where a playing of the new chain stands; and a
  playing that has stopped (`s_over`: a boundary's seek that failed, a read that failed), which
  only a seek starts again. The line:
  `replay: NAME: watching timeline N from GameTime G (shared history: no rebuild)`.
- **Sought**, otherwise: a second reader of the same file opened on `id`'s chain, and THE SEEK on
  it in SWITCH mode to the host's GameTime + 1, clamped to the chain (`seek_run` with `sw`). THE
  SEEK's invariant makes the result what a playing of `id` holds there. The first reader is not
  moved. **The invariant, an ordering:** every refusal of `seek_run` comes before its first host
  write (R1), and in SWITCH mode it calls no `sk_back`; so a refused switch closes the second
  reader and puts the first back, and the playing goes on as before -- except what every refused
  seek leaves (the eye's lead, the cleared click overlay), and in a multiplayer playing the
  materializer's rule, which `mp_follow` sets back from the first reader's segment. **The
  residual:** in a multiplayer playing, a refusal from the second reader's materializer
  (`mp_follow` on it failing) is sticky (`mp_fail`) and stops the playing, as it stops it after
  any refused seek; restoring it would need the first reader's materializer rebuilt whole by
  `mp_follow`, which is not shown. The request
  the switch's seek replaced (a boundary's, or the user's) is given back on a refusal, and the
  user's is asked again on the new chain when the switch is done. The line: `replay: NAME:
  watching timeline N at GameTime T (sought from G in M ms)`, after the seek's own.

**The claims' order.** The new timeline is claimed before the re-chain's scan or the second
reader opens, and the left one released after its reader closes (re-chained: once the reader
plays the new one), so the timeline that plays is never deleted, through the window in which two
readers are open. **The file's lifetime:** the second reader opens the playing's path while the
first holds it open, and an open reader's file can be neither replaced nor deleted (readers share
no DELETE; a compaction's replacement fails on an open file, above), so both name one file;
`recr_same` checks the two handles. A deletion writes only a metadata block after the file's
last, so any reader of a deleted timeline, the walker's included, reads blocks nothing
rewrites.

**The in-level stamps.** A timeline's last use is stamped in the playing when it is left -- at a
switch, and for the last one at the level's end (`used_stamp`, `s_used`, oldest first, at most 65
rows). The order is the mockup's "watched least recently". `recr_append_point` takes each
timeline's last use as the later of the file's and the stamp's, both for the eviction and for the
rows the fork writes, so the takeover's plan (the HUD's "replaces F<n>", the trigger's dry run)
and the real eviction (`tk_open`) come from one function and one table. The watched timeline
stays excluded from eviction. **The touch list:** the level's end sends one touch
(`recfile_touch`) with every stamp, the last timeline first and then the others in the order they
were left; the worker sets each live one's last use to the later of the file's and the row's,
skips one absent or deleted, and logs `touched timeline N` for one, `touched timelines A, B, C`
for several.

**Deleting the watched timeline** (`tagpu_replay_fork_op`, DELETE of the timeline that plays) is a
switch first, to the nearest live timeline up the chain from the leaf's parent; with none it is
refused ("the last live timeline of this chain plays"), and while another switch or a takeover
waits ("another request is waiting"). The deletion is queued once the switch has released the
claim, so the worker's in-use test passes by construction, and its ticket goes in a slot the
request kept free; a refused switch makes the deletion's outcome refused ("could not leave
timeline N"). The recorder's rule is unchanged: a deletion of a claimed timeline is still refused.

**The tests: X2,** a step of the X lane right after fork 5, on a copy of the file (live
timelines 0, 2, 4 and 5), one launch held at fork 5's first iteration:
1. 5 -> 2 by the seek path, then a round trip at +30 whose dumps compare EQUAL (the playing after
   the switch equals a rebuild of chain 2 in place), and `hud state` watching 2 on `[0, 2]`;
2. a HUD seek before both forks, then 2 -> 4 re-chained with no seek line, watching 4 on `[0, 4]`;
3. the dry run names timeline 5 for eviction where the file's own times would name 2;
4. the re-chained reader's own world at fork 2's GameTime + 30, before 4's fork -- where the chain
   it left would have crossed into 2 -- and past 4's boundary, both dumps EQUAL to a fresh seek
   of 4;
5. a switch to the evicted timeline 1 refused, the dumps before and after it EQUAL;
6. the deletion of the watched 4: the switch to 0 (a seek, past 4's fork), the worker's deletion,
   the HUD's outcome, live `[0, 2, 5]`;
7. the level's end: `touched timelines 0, 5, 2`, and the three last-use times later than before.

**Measured 2026-10-23** on the coverage build, `battle`, X2 alone (`--x2-only`) on the file the
lane's run made (T5 = 902, F2 = 901, F4 = 1501, base 601): 5 -> 2 sought at 903 in 2032 ms, the
round trip at 933 with 26 tables poisoned, residue 0, its dumps EQUAL; 2 -> 4 re-chained from
GameTime 600 with no seek; the dry run named timeline 5 (the file's own times would name 2); the
dump at 1531 past 4's fork EQUAL to a fresh seek of 4; the refused switch's dumps EQUAL; the
deletion of 4 by way of 0, sought at 1532 in 2987 ms, the deletion's compaction deferred while
the playing held the file (error 5, the replacement refused, as *File ordering and lifetimes*
says); the touch `touched timelines 0, 5, 2`, compacting to three segments once the playing had
closed. Every function the switch adds or changes was reached by X2's coverage file. In the
whole lane's run of the same day, whose GameTimes the commit picks (base 811, F2 1111, F4 1711):
5 -> 2 sought in 2466 ms, 2 -> 4 re-chained from 810, the re-chained reader's dump at 1141 and
the one past 4's fork both EQUAL to a fresh seek of 4, 4 -> 0 sought in 3333 ms, and every other
check of the lane passing.
`codectest outcomes` plays `recr_rechain`'s edges offline: a reader at a fork's `fork_at - 1`
re-chains onto it and its next read is that fork's boundary; at `fork_at` it does not and reads on;
a reader that has read nothing, one that has read its end, and one in a sibling fork's segment
are not shared and stay as they were; an absent timeline is refused.

**Gaps.** No test makes `seek_run` refuse after the second reader is in place (its refusals need
an unreadable file or a level that is not the counts'), so the restoration is reached by no test;
its argument is the structural one above. Of the refusals at the frame visit, X2 reaches none:
a claim refused because the timeline's deletion is queued, and a re-chain or a second reader
refused by the file (`k < 0`, a timeline the cache still lists live), each need a deletion to
finish between the request's look at the cache and the visit, which no test can order; the
refusal X2 plays is the request's own. X2 plays a single-player file: the multiplayer lines of
the restoration (`mp_follow`) are reached by no test; a ten-seat X2 would reach them. Two things
fixed from the first header stay as a switch finds them, as a boundary crossing leaves them:
getter 75's `local[]` (`seat_level`) and the viewer's side art. The HUD's walker (markers, stats)
of the left timeline is joined, with its bounded wait on the game thread, at the HUD's next fill
after a switch, as at any timeline change, until the lanes' landing polls it; its reads are of
blocks no deletion rewrites, in a file no compaction can replace while it is open. A touch
whose recording the prune has deleted since the playing ended leaves a `.lock` beside the
missing file: `recfile_lock` opens the lock `OPEN_ALWAYS` before the recording is found absent.
The lock is empty and harmless, and left: the touch could delete it through its own exclusive
handle, as the prune deletes one, which it does not do.

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
- `tacli replay timeline INSTANCE N` switches the running playback to timeline N (the trigger's
  `timeline N`) and prints the switch's line, `shared` or `sought` in `--json`; exit 1 refused.
  `tacli replay timelines INSTANCE` lists the playing's timelines as its HUD sees them (the
  trigger's `hud state`): parent, fork point, last, live, the seat played as, and the chain.
- `tacli replay takeover INSTANCE FILE --at T --player P --dry-run` predicts
  eviction offline. Without FILE it asks the running playing. Both refuse a child
  boundary or a time past the selected timeline's end. No takeover occurs.
- `tools/replay/tareplay.py forks battle [--cover]` exercises the rules. The
  landing tier also runs its X lane on `battle` and `stock-limits`, after the
  existing L, P, R, Q, K, F and W lanes when present. While timeline 5 plays the lane
  compares `hud state` with the file (every row's id, parent, fork point, last and state,
  each fork's controlled seat, the axis, the chain's span and its marks), checks the in-game
  prediction against the file's, a refused seek's outcome, and the HUD's route: the
  deletion of the evicted 1 refused by the file worker, an export of 4 holding `[0, 4]`, and
  the two outcomes in turn. X2 runs on a copy after fork 5 (*Watching another timeline in place*);
  `tareplay.py forks battle --x2-only FILE` plays it alone on a saved `x2.imprec`, for a
  debugging loop, never as a gate.

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

The takeover's judge observation does not establish that the roster
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
