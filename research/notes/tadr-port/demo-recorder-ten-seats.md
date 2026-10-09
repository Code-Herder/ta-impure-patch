# Demo recorder, stage 5: ten recorded seats

Status: **design, 2026-10-06; implemented 2026-10-06 on the stage branch (§4, both commits of
§5); §5's runs 2, 3, 4 and 6 made 2026-10-06, run 5 (the pictures) 2026-10-07.** Stage 5 of the
[multiplayer slice](demo-recorder-multiplayer-slice.md) ("Still open after stage 4: ten seats
without a spare viewer seat"). It supersedes decision D2 of the
[puppet host note](demo-recorder-s2-host.md) (§1.3: refuse a ten-seat recording) and corrects §1.4
("record 10 is not a seat"), which stays true of the engine and stops being true of a puppet level.
Engine facts are DISASSEMBLED against `pristine/TotalA.exe.pristine` unless marked; the table at
the end lists them.

## 1. The problem

A playback seats every recorded seat as a type-3 puppet and puts the viewer V in a record of its own:
`+0x2A42` (controlled) names V, so the viewer's input walks V's empty unit block and reaches no
puppet (s2-host §1.2), and V's sight counters, filled with 1, plus V's bit in every explored-map
entry give full-map visibility (s2-host §2.9). Today V is the lowest index the recording does not
seat, so a recording that seats all ten records is refused (`validate_header`, "it seats all ten
records, which leaves the viewer none"), and so are `tarec.viewer_of` and tacli's
`REPLAY_SEATS_MAX = 9`. Ten seats are the common case of a full multiplayer game.

## 2. Decision: V is record 10, on every playback

The engine constructs **eleven** player records, not ten: the constructor loop runs 11 times
(`0x41D97E..0x41D997`, `ebx` = 0xB, each through `0x463BE0`, which allocates its PlayerInfo); the
players reset `0x4644D0` clears all eleven; the player teardown `0x464A00` frees all eleven
(`0x464A00..0x464A9B`, `jbe main+0x2851`, record 10's address). Record 10 is the engine's
"no player": `+0x146` = 10, no unit block, skipped by every players loop. It is a whole record with
a whole lifetime, and no recording carries it (`replay-globals.tsv`'s `players` row covers records
0–9 only).

| candidate | verdict |
|---|---|
| **(b) an eleventh record** | **Chosen.** Record 10 exists, has the engine's own constructor and destructor, and is outside everything a recording carries, so nothing recorded is ever overwritten with viewer state. |
| (c) an index outside 0..9 | The same as (b) for index 10. Index 11 is not a record: `0x1B63 + 11·0x14B` = `main+0x299C`, the branch selector, the settings pointer `+0x29A0`, the start positions and `+0x2A3C..+0x2A45` themselves (`+0x2A42` would be "record 11" + 0xA6). |
| (a) a borrowed recorded seat with sink gates | Rejected. The viewer's input would act on a puppet's units unless every sink is gated — orders, selection, build, self-destruct, give, groups: a list, and the owner rejects list-driven designs. Full visibility needs a record whose counters are all 1 and whose explored bit is set everywhere, and a borrowed seat's counters, `+0x146` bit and PlayerInfo are recorded and compared, which the materializer must not overwrite. It needs (b) anyway. |

**V = record 10 for every playback, not only ten-seat ones** (owner decision O1): one path, which
every existing fixture then exercises; the alternative keeps two.

## 3. What record 10 needs, and what the engine gives it

- **Index safety of the engine's own calls.** `0x464290(10, t)` writes only inside record 10: the five
  11-byte rows `+0x108/+0x113/+0x11E/+0x129/+0x134` and their element `[+0x108+10]`,
  `[+0x113+10]`; `+0x146`, `+0x147`, `+0x148` = 10 (`0x46434D`, `0x46436B`, `0x464353`);
  `[rec+0]` = 1 (`0x464364`); `+0x74` = 0 without a free (`0x46434A`). `0x464700(rec)` creates an AI
  object and calls `0x40B320(+0x146)`, which writes the ten-entry table `0x5119C0[i]` (`0x40B340`),
  **unless** `[rec+0]` ≠ 0 and `+0x73` = 3 (`0x46489D..0x4648A5`). So record 10 is activated as type 3
  through `0x464290` before `0x464700` ever sees it: that ordering is what keeps `0x5119C0[10]` unwritten.
- **Buffers.** `0x464700` allocates the sight counters `+0x7C` (frees the old one first,
  `0x464855`; size from the map, `0x46482B..0x464866`), the group table `+0x78` through `0x480190`
  (`0x464898`; it does **not** free an old one, `0x4801A1..0x4801B1`) and `+0xEC` when NULL
  (`0x4647CF..0x4647EA`). `0x464A00` frees all three for records 0–10 and zeroes the pointers
  (`0x464A2D`, `0x464A3C`→`0x4801F0`→`0x480239`, `0x464A76..0x464A7E`). The level init
  `0x464990` calls `0x464700` for records 0–9 only (`edi` = 10, `0x464994`), from `0x4919C8` alone, in
  `LoadGameData_Main` `0x4917D0` on the loader thread (`0x497581`).
- **The players phase loops 0–9** (`0x4655A8..0x4655AF`). For the viewed record it runs the minimap
  rebuild `0x466DC0` every tick (`0x465065..0x465072`) and, on its 30-tick pass (`0x46507D..0x465092`),
  the radar pass `0x467440` and the minimap fog `0x466C20` (`0x465565..0x465572`). With record 10
  viewed none of these runs: the host must run them (§4.4).
- **Unbounded reads of the viewed record.** `0x466C20` reads the viewed record's counters with no
  bound or NULL test (`0x466CA4..0x466D25`); the loader calls it through `0x4816A0(1)` at `0x497AE5`.
  So `+0x2A43` may name record 10 only while record 10's counters exist.
- **The block walkers.** All 35 reads of a record's first-unit pointer `+0x67` were classified. Every
  walk of the controlled or viewed block tests `first > last` before the first unit
  (`ja` at `0x41C345`, `0x467516`, `0x48BE3F`, `0x48CA80`, `0x48D2A5`, `0x48D454`, `0x48D4AC`, `0x48D505`,
  `0x48D561`, `0x48D67E`, `0x48DD40`). The nine reads with no such test are AI-object cursors (`0x4089F8`,
  `0x40ED1B`), the unit-slot searches over a record argument (`0x48B88E`, `0x48B960`, `0x48B988`,
  `0x48BAAD`; [INFERRED] the argument is a new unit's owner, never 10) and three reads of other
  structures (`0x4838D8`, `0x49AF2A`, `0x4C15A8`). The unit ID cycle `0x48C190` walks the controlled
  record's ID range `[+0x6F, +0x71]`. It turns ID 0 into a NULL unit and reads it (`0x48C1EF..0x48C213`),
  so the range must not include 0. The level init gives blocks to records 0–9 only (`0x48592B`).
- **Loops that would have touched V and do not touch record 10.** The level start `0x465E30` seeds
  resources for records 0–9 from the skirmish settings (`0x465F87`, `cmp edi,0xD84`); the post-game table
  `0x41DC20` lists records 0–9 (`0x41DEC5`) and skips `+0x146` = 10 (`0x41DD37`); the pacing gate walks
  0–9; the sight rebuild `0x4816A0` refills the counters of records 0–9 only (`0x481707..0x48177A`).
- **Ten-entry tables indexed by `+0x2A42`.** The only one found is the reporter's `[0x51E574]`,
  read at `0x46C856`/`0x46C88A` in `0x46C810`. Its one caller `0x463EBE` is behind GameingState 3
  (`0x463EAF..0x463EB7`), and a playback level is GameingState 2 (`validate_engine`; live rewind refuses
  a network game). `0x46C620` passes `+0x2A42` as a value to the stub `0x4CAA10` (`mov eax,1; ret 0x28`)
  and, when `reporter.dll` is loaded (`0x51E58C` ≠ 0, set only by `0x46C00F` inside `0x46BF30`, reached
  only through `0x443718` behind GameingState 3 at `0x4436EC..0x4436F4`), to its export `[0x51E584]`
  (`0x46C7CC..0x46C7E7`). MEASURED 2026-10-06: the retail install on the reference setup ships no
  `reporter.dll`. A residual sweep for spilled indexes is the scout batch S5-A (§8).

## 4. The design

New names, all in `tagpu/ddraw/src/tagpu_replay.c` unless stated:
`#define REC_VIEWER 10` in `inc/tagpu_recfmt.h` beside `REC_PLAYERS`, whose comment becomes "the
records a game seats and a recording carries; the eleventh, REC_VIEWER, is the engine's no-player
record, which a puppet level seats the viewer in". Statics: `s_vrec` (record 10's `PL_STRIDE` bytes
as the level's first seating found them, and a `taken` flag), `s_vbuf` (record 10 was activated for
this level's viewer; read once by the loader hook), `s_vready` (`viewer_init` has run this level),
`s_vframe` (GameTime of `viewer_frame`'s last run).

### 4.1 Seating (the Start: S-a's fill and the browser's Watch, game thread, before the loader thread exists)

- `validate_header`: drop the `nseats >= REC_PLAYERS` refusal; `*viewer = REC_VIEWER`. The reader
  already bounds every seat index below 10 and the seat count to 10 (`tagpu_recreader.c`, RT_PLAYER).
- `validate_engine` gains `viewer_free(ta)`: refuse unless record 10 is the engine's free record:
  `+0x73` = 0, `+0x74` NULL, live count `+0x144` = 0, `+0x78`, `+0x7C` and `+0xEC` NULL, PlayerInfo
  `+0x27` non-NULL. Also refuse when `0x51E58C` ≠ 0 ("the network reporter is loaded"): its export
  would receive index 10, which no longer names a seat. The buffers test runs at the Start only (a live
  rewind may find buffers its earlier rewind made, §4.6).
- `seat_level`: take `s_vrec` (if not taken) and `s_vinfo` (as today) from record 10; then
  `viewer_seat(ta, side)`, shared with `rw_prepare`:
  `E_ActivateSeat(10, 3)`; the watcher bit, and the **side and colour** (`+0x95`, `+0x96`) of the
  header's controlled seat, or of its first seat when the controlled record is not seated, both
  checked by `validate_engine` against the engine's counts. **The colour is a bound, not a
  nicety:** the players reset gives every record its own index as its colour (`0x46461E`, records
  0..10), so record 10 holds colour 10, one past the ten frames of the colour sequence
  `main+0x148DB`; with V viewed, the resource bar draws V's colour badge (`0x468E4B` → `0x4690AB`
  → `0x467C00`, which takes the frame at `0x467C24` with no bound) and the span read pixel data
  at NULL (MEASURED 2026-10-06: an access violation at `0x4C74C7` within seconds of a full-map
  playback; the dword past the ten frames was `0x0001000A` on two processes). The fixtures' P, Q
  and R legs all play a perspective, so only the browser's Watch (W), which plays the full map,
  exercised it; the **empty block** `+0x67` = 1, `+0x6B` = 0 (first above last: no address
  at all, so a walker that missed the test would fault at once instead of acting on a seat's unit) and
  `+0x6F` = `+0x71` = 1 (the ID cycle then reads slot 1 only, which every unit array has, 10·each + 1
  slots, and finds its selected bit clear because every setter walks the controlled block). Then
  `s_vbuf = 1`.
- Drop `seatv[v] = 1`: V is not a puppet seat (the table is ten entries, and every gate keys on a
  unit's owner, never 10). `+0x2A42` = 10; `+0x2A43` = the requested seat, or 10 for the full map.
  `+0x2A43` = 10 before the load is safe because the first reader of the viewed record's counters
  (`0x4816A0` at `0x497AE5`) comes after the loader hook (§4.2), as V's own did when V's `0x464700` ran
  in the same loop.

### 4.2 The level init (loader thread): record 10's buffers

Retarget the call at `0x4919C8` (`E8 C3 2F FD FF`, to `0x464990`; check the bytes before writing)
to `tagpu_replay_level_players`:

```c
void __cdecl tagpu_replay_level_players(void)   /* loader thread, LoadGameData_Main */
{
    E_PlayersInit();                             /* 0x464990: records 0-9 */
    if (s_vbuf) viewer_buffers();                /* 0x464700(record 10) */
}
```

`viewer_buffers` checks `[rec+0]` = 1 and `+0x73` = 3 (the two values `0x46489D..0x4648A5` test),
and `+0x78`/`+0x7C` NULL (so the one call leaks nothing), calls `0x464700`, then checks the three
pointers. On any failure it moves `+0x2A42` and `+0x2A43` to the lowest seat, which has counters, and sets
a flag so the first step refuses ("the viewer's record could not be prepared"); the level stays empty.

The comment must carry three invariants. ORDERING: `s_vbuf` is written on the game thread before the
loader thread is created (`0x4982C5..0x4982CA`) and cleared at the level's end, after it has exited.
LIFETIME: record 10 now gets its buffers at the same point of the load as records 0–9, and loses them
at the same point (`0x464A00` at `0x491BA9`), so from the loader's first reader to the teardown,
record 10 is shaped as V's record is today. BOUND: the activation as type 3 before this call is what
keeps `0x5119C0[10]` unwritten. `thread-split.allow`'s line for `tagpu_replay.c` names this
loader-thread entry beside the allocator callback.

### 4.3 The first in-play frame or the first step, whichever comes first: `viewer_init`

The frame callback `0x496790` runs the steps (`0x495490`, possibly none on a frame) before the keys
(`0x496971`) and the draw call `0x4969CD`, so a level's first draw can precede its first step. Call
`viewer_init(ta)` at the top of `tagpu_replay_frame` (before its `s_first` return) and at the start of
the first step. It runs once (`s_vready`) and does this:
`sight_fill(ta, REC_VIEWER)` (counters 1, bit 10 everywhere), record 10's pass `+0xF0` = the next
multiple of 30 above GameTime, then `minimap_fog`. ORDERING: both callers run inside `0x496790`,
the in-play callback, which runs only once the loading loop has seen the loader's last write (bit 1
of `main+0x38D75`, set at `0x497C5F`, tested at `0x498342`). The puppet table's `playing` flag rests
on the same fact. The first draw then shows the full map.

In `play(first)`, delete the two memsets of V's energy and metal and their comment. Its reasoning
("the level start reads them from V's settings entry") is false for record 10: `0x465E30` stops at 9,
and `0x464700` zeroes `+0x8C`/`+0x98` (`0x46476B`, `0x46477D`) and storage `+0xA4`/`+0xA8`
(`0x46478F`, `0x464795`).

### 4.4 Every frame: the viewed work the players phase no longer does

Restructure `tagpu_replay_frame` as its present body followed by `viewer_frame(ta)`, which runs on
every exit:

```c
/* record 10 viewed: the players phase's work for the viewed record (0x465065..0x465072,
   0x465565..0x465572), which its loop over 0-9 never reaches */
if (!tagpu_puppet_on() || (unsigned char)ta[OFF_VIEWED] != REC_VIEWER || now == s_vframe) return;
s_vframe = now;
E_MinimapRebuild();                       /* 0x466DC0 */
if (pass <= now) { pass = 30 * (now / 30 + 1); E_RadarPass(); E_MinimapFog(); }   /* +0xF0 */
```

At the frame visit it shows exactly what the draw would show after the frame's last tick: the
rebuild reads the projectile pool, which `0x49B720` moves after the step's hook (`0x495513`). Gating
on GameTime skips paused frames, as stock does. Several 30-tick boundaries in one frame run the pass
once [INFERRED to be equivalent: the pass recomputes the contact bits from the present state]. The
comment must carry the LIFETIME: `+0x2A43` = 10 only while record 10's buffers exist (§4.2, §4.6), and
the teardown wrap's entry ends puppet mode before `0x464A00` frees them.

### 4.5 Seeks, sight change, takeover

- `sk_deadlines`: also write record 10's `+0xF0` (the next multiple of 30) and `+0xF8` (now); the loop
  over 0–9 no longer has a V branch.
- `sight_change`: accept seat 10 (`seat > REC_VIEWER` refuses); the full-map card's request
  (`s_viewer + 1`) already names it. `eye_put`'s `+0x146` check holds for record 10 (10 = 10).
- `mapped_put`: V's bit is record 10's `+0x146`, 10 by `0x46434D`; unchanged code.
- The takeover check: drop `acq[s_viewer]` (`0x5119C0` has ten entries; record 10's is never written,
  §3); keep the `+0x74` test. TK4a's `E_ActivateSeat(s_viewer, 0)` is index-safe for 10 (§3); it
  writes PlayerInfo `+0x94` = 0, which `s_vinfo` puts back.
- Remove the now-dead `== s_viewer` tests over 0–9 (`owner_holds`, `players_put`'s skip, the `kp`
  test, `pace`, `tagpu_replay_players`): records 0–9 are all applied raw from the recording, as
  every unseated record already is.

### 4.6 Live rewind

`rw_prepare` uses V = 10 through `validate_header`. Drop `((void**)ACQ_TABLE)[v]` and `seats[v] = 1`.
Take `s_vrec` if not taken (and require the buffers NULL then), call `viewer_seat`, then
`0x464700` when `+0x7C` is NULL (require `+0x78` NULL with it). A second rewind in the same level
reuses the buffers, as today.

### 4.7 The level's end

`tagpu_replay_level_after` (after `0x491B60` has returned, so `0x464A00` has freed and zeroed record
10's buffers) restores `s_vrec` whole (its buffer pointers were NULL when taken, which matches the
freed state) and `s_vinfo` as today. When `+0x2A42` or `+0x2A43` holds 10, it writes 0 to each, the
players reset's values (`0x46451B`, `0x46452D`), so no ten-entry table can be indexed with 10
outside a level that seats record 10. `tagpu_replay_level_end` clears `s_vbuf`, `s_vready` and
`s_vframe`.

### 4.8 Impure's own readers of the indexes (game thread, in the in-play draw)

- `tagpu_fogwide.c` `fogw_source`: `if (id > 9) return 0` leaves a full-map playback without the
  wide fog grid when zoomed out. Change the bound to the eleven records (`id > REC_VIEWER`). The mask
  `1 << id` stays inside the u16 entry. The comment ("ten players") states the eleven records and that
  record 10's counters exist whenever it is viewed (§4.4).
- `tagpu_gui_hook.c` `HUD_NPLAYERS 10` (the HUD poison and the chrome repaint): with record 10 viewed,
  the debt is never paid. Make it the eleven records the engine's own readers index (`0x468E6C` reads
  `+0x146` of the viewed record; `0x467D70` its PlayerInfo side), and fix the "PlayerStruct[10]"
  comments.
- Checked and unchanged: `tagpu_order.c` (unbounded index, but `ptr_ok(1)` and `first <= last` skip
  the empty block); `tagpu_scenario.c` `live_selected` (refuses 10: no selection, which is right);
  `tagpu_packet_pub.c` `player_logo` (−1 for 10; the ghost's fallback owner is clamped by
  `face_texframe`, `tagpu_render3do.c`, and V never builds); `tagpu_mark.c`, `tagpu_feat.c`,
  `tagpu_native.c` (owner comparisons, the same for V at any index).

### 4.9 Lists and tools

- `replay-globals.tsv`: a `leave` row for record 10, `main 0x2851 331` (`viewerrec`), reason: the replay viewer's
  record on a puppet host; on a recording machine it is the engine's no-player record. Without it the
  images gate fails on the bytes `viewer_frame` writes (`+0xF0`). **A loosening, owner decision O2.**
  Any edit changes the globals hash, so recordings made by earlier builds are refused (none is
  published).
- `tools/replay/tarec.py`: `VIEWER = VIEWER_BIT = 10`; `viewer_of` goes; `compare` masks bit 10
  (`mapped_hold`); `puppet_view` no longer blanks a recorded record (drop its `viewer` argument).
  `tools/replay/recmp_compare.py` the same.
- `tools/tacli`: `REPLAY_SEATS_MAX = 10`; the refusal text loses "since the viewer takes a record of
  its own".
- `tools/replay/fixtures.json`: the fixture `tenseat` (§5). If the `SIM_FEATURE` fixture string takes
  a list, name it in `puppet_host`'s (`tagpu_puppet.c`). Correct the `PuppetTable` comment
  ("the record is a puppet seat, V included").
- The file comment of `tagpu_replay.c` ("V is chosen below 10") and the notes the docs pass corrects:
  s2-host §1.3, §1.4, §8 D2; the multiplayer slice's "still open"; the engine map (§9 facts).

## 5. Landing and verification

**One landing, two commits:** (1) V moves to record 10 for every playback (§4.1–4.8, the tsv row,
the tools); (2) the ten-seat refusal goes (validate_header, tacli) with the `tenseat` fixture. One
review at **high**: it adds a byte patch, writes engine state from the loader thread and changes what
the render-feeding readers bound. Each run below is a game of 10 minutes or less.

1. **Build.** `make -C tagpu/ddraw -j$(nproc)`; the globals header regenerated from the tsv.
2. **The ten-seat fixture**, `tenseat`: `load` `replay-tenseat-load.json` (human and nine AIs on Core
   Prime Industrial Area; recording MEASURED by the coordinator 2026-10-06, 951 iterations, 0
   corrections), `end` 1800, a step `[900, "type", "+los"]` (true LOS from mid-game: `los_react` then
   `sight_fill(10)`), `watch: true`, `forks: true`, `takeover: {"player": "controlled", "checks":
   ["judge", "line", "fork"]}`. Expect: P logs "the viewer at record 10"; the puppet compare passes
   with all ten records compared (no blanked record); the images gate passes with record 10 under
   its `leave` row; W's Sight switches seat 0 ↔ full map ("sight is seat 10"); W's second Watch in the
   same process seats again (record 10 restored and free); K's takeover deactivates record 10 and the
   fork plays.
3. **Every existing playback moves V**, so re-run `battle` (rewind U lane through `rw_prepare` with
   V = 10, forks, takeover), `battle-los` (true LOS, `+los`), `secondary-orders` and
   `stock-secondary-orders` (viewer clicks and `ctrl+d` on a puppet host stay inert), `stock-limits`
   (stock DLL, Watch, rewind), `takeover-end`, `b7-radar-hail` (owner-10 projectiles, R2), each through
   `tools/replay/tareplay.py run`.
4. **Multiplayer.** `tools/replay/recmp_play_test.py` (four actors, V = 10). Ten seats: a two-peer game
   with eight hosted AIs on Core Prime Industrial Area, recorded and played as the four-actor test
   is ([INFERRED] that the battleroom seats eight AIs). It covers the materializer and the arena order
   with ten seats on two instances (owner decision O4).
5. **Pictures**, one full-map playback on the private display at 1024x768, Vulkan: zoomed out, the
   wide fog matches the seat view's extent (§4.8); the minimap's unit and projectile dots move between
   two frames 1 s apart (`viewer_frame`); with `hudscale` 150, the bars and chrome redraw.
6. **R1's size**: count the recorded deaths whose victim image has killer byte `+0xF4` = 10, in the
   `tenseat` and `battle` recordings (`tarec`).

MEASURED 2026-10-06 on the reference setup:

- **Run 2:** `tenseat` passes every lane: P logs "10 seats, the viewer at record 10", the images
  gate passes with record 10 under its `leave` row, W's Watch and its cards, the forks, and K's
  takeover.
- **Run 3:** `battle`, `battle-los`, `secondary-orders`, `stock-secondary-orders`, `stock-limits`,
  `takeover-end`, `b7-radar-hail` and `perspective` pass.
- **Run 4:** the four-actor network recording is EQUAL in full view and from record 1, its chat
  EQUAL. The ten-seat game was recorded from five Impure peers with one AI each (owner decision O4,
  revised: the battleroom allows one AI per human), twice, the host recording and the joiners
  leaving first: every peer opened the channel (one lost port race reopened, F1 of the slice note),
  bound 8/8, and the host's file completed. Played back it logs "10 seats, the viewer at record
  10"; its dumps at 300 and 1000 are EQUAL in full view, from record 0 and from a fresh seek to
  1000.
- **Run 6:** in §6, R1.
- **Run 5 (the pictures)**, MEASURED 2026-10-07 on the landing gate's ten-seat recording (one
  human and nine AIs on Core Prime Industrial Area), 1024x768, Vulkan, on a private Xvfb:
  - the wide fog, paused at GameTime 157, zoom 0.456 and 0.25: even to every viewport edge, no
    unshaded band (platform brightness 106 top, 108 bottom, 114 left, 103-111 centre at 0.25);
  - the minimap while playing: unit dots move (12 and 48 of its 128x128 pixels changed over 1 s),
    all ten seat colours present; projectile dots unchecked, the recording having none in flight
    at the times sampled;
  - hudscale 150 while playing: 0 pixels changed outside content in the top, side and bottom bars;
  - `--perspective 0`, paused at 1499, zoom 0.25: colour only in the commander's sight circle
    (chroma 13.9 against 0.0 elsewhere); never-visited ground is grey, not dark, because the game
    starts Mapped.

  The run found two minimap faults, both fixed: the view box kept its 1x size through a paused
  zoom (the zoom apply now rescales it when the level changes, `gpu-status.md`), and after a paused
  seek in a seat's view the minimap showed no unit until play resumed (the seek's R14d and a Sight
  change now rebuild the picture; MEASURED 2026-10-07 on 2acc422e: the box 23x16 at 0.456 and 41x29
  at 0.25 while paused, in the replay and in a paused live skirmish alike, and seat 0's one dot right
  after a paused seek to 1500 and after each paused Sight switch). The player cards of that run
  showed 9 of the 10 seats at 100 % and 5 at hudscale 150. The replay HUD that replaced them
  scrolls its column by whole items: MEASURED 2026-10-09, a ten-seat, five-team recording at the
  HUD's 150 % scrolled from item 0 to 4 by its arrows and by the wheel
  ([The replay HUD in game](../hud-in-game.md)).

## 6. Residuals and gaps

- **R1, the music cue: closed by a gate.** The destructor adds 5 to the music's combat ring (the
  call `0x494FF0` at `0x4869EB`) whenever a death's killer byte `+0xF4` equals `+0x2A42` (the
  compare at `0x4869DD`). With record 10 controlled that matched most deaths: MEASURED 2026-10-06,
  `+0xF4` = 10 on 906 of 1448 deaths in the battle fixture, every death in the ten-seat fixture, and
  3 of 35 in a four-actor network recording, so the playback leaned to the battle music. On a puppet
  level the call site returns without the cue (`tagpu_replay.c`, THE DEATH CUE), as the cue never
  fired for a viewer before it had record 10. The hit cue `0x489DE6` compares owners (`+0xFF`), which
  are never 10. Owner decision O3, revised.
- **R2, owner-10 explosions.** Record 10 is type 3 on a puppet level, so the explosion's owner gate
  (`0x49A03F..0x49A047`) skips area and direct damage for owner-10 projectiles, as it does for
  puppets'. Every damage sink is gated on a puppet level (`tagpu_puppet.c`), so nothing the recording
  carries changes [INFERRED; items 2–3 compare it]. After TK4a, record 10 is type 0 again.
- **Gap:** spilled-index uses of `+0x2A42`/`+0x2A43` beyond the classifier's window (scout batch S5-A).
- **Gap:** a ten-peer network game (ten instances) is not run here; O4.
- **Fallback:** an allocation failure in §4.2 leaves an empty level, which is the engine's own
  out-of-memory behaviour for records 0–9.

## 7. Owner decisions (answered 2026-10-06)

- **O1.** V is record 10 on **every** playback: one path, exercised by every fixture.
- **O2. Accepted:** the `leave` row for record 10 (`main+0x2851`, 0x14B bytes) and the compares'
  explored-map mask moving from V's bit to bit 10. The images gate no longer checks record 10. On a
  recording machine, its only writes in play are, [INFERRED], the destructor's kill and loss counters
  (`0x4868C1`, `0x486906`, `0x486A33`) for deaths credited to player 10, which nothing displays or
  tests (§3). In exchange, records 0–9 are all compared, where today V's record is blanked.
  Recordings made before the change are refused; none is published.
- **O3. Gate it** (R1, revised 2026-10-06 once item 6 measured it on most deaths): the death cue is
  skipped on a puppet level.
- **O4.** A ten-seat network recording from five Impure peers and five AIs (revised 2026-10-06: the
  battleroom seats one AI per human, MEASURED, so two peers and eight AIs cannot exist); a true
  ten-peer game only on demand or with the release suite. The viewer's seat does not depend on the peer count.

No escalation applies: every fix rests on a bound or an ordering stated above, and every verification
can run.

## 8. Open sweep

**S5-A, for a scout:** follow every load of `main+0x2A42`, `main+0x2A43`, or a controlled or viewed
record's `+0x146..+0x148` through register moves, stack spills and callee arguments to the end of
its function. Report each use as an index into anything other than the player records (`main+0x1B63`,
stride 0x14B), with the table's entry count and whether index 10 stays inside it. §3's
classifier looked 30 instructions ahead and found only the reporter's. The sweep does not block the
implementation; the landing review should have its answer.

## 9. Facts

| fact | address | how |
|---|---|---|
| eleven records constructed, PlayerInfo each | `0x41D97E..0x41D997` → `0x463BE0` | DISASSEMBLED |
| the reset clears eleven, then `+0x2A42` = `+0x2A43` = 0 | `0x4644D7..0x46450F`, `0x46451B`, `0x46452D` | DISASSEMBLED |
| the player teardown frees `+0x7C`, `+0x78`, `+0xEC`, AI for records 0–10 | `0x464A00..0x464A9B` | DISASSEMBLED |
| `0x464290(i,t)`: rows inside the record, `+0x146..+0x148` = i, `[rec+0]` = 1, `+0x74` = 0, PlayerInfo `+0x94` = t when t ≠ 3 | `0x4642BE..0x464364` | DISASSEMBLED |
| `0x464700` skips AI and `0x40B320` only for `[rec+0]` ≠ 0 and type 3 | `0x46489D..0x4648CD` | DISASSEMBLED |
| `0x464700` allocates `+0x7C` (freeing the old), `+0x78` (not freeing), `+0xEC` if NULL; zeroes resources and storage | `0x464855`, `0x464898`/`0x4801A1`, `0x4647CF`; `0x46476B..0x464795` | DISASSEMBLED |
| `0x401070(10)` stores record 10's address only | `0x401070..0x4010A6` | DISASSEMBLED |
| level init loops records 0–9; its only call site | `0x464994`; `0x4919C8` | DISASSEMBLED |
| load order: level init, sight rebuild, MAIN2 from the viewed side, controlled PlayerInfo `+0x9B` bit 0x10, players phase, load bit 1 | `0x497581`, `0x497AE5`, `0x497B56`, `0x497BE3..0x497BFF`, `0x497C10`, `0x497C5F` | DISASSEMBLED |
| loader thread started; loading loop waits on bit 1 | `0x4982C5..0x4982CA`; `0x498342..0x49834D` | DISASSEMBLED |
| players phase loops 0–9; viewed-only rebuild, radar pass, fog | `0x4655A8`; `0x465065..0x465072`, `0x465565..0x465572` | DISASSEMBLED |
| step order: unit tick hook, projectiles, `0x420F30`, players phase | `0x4954ED`, `0x495513`, `0x495539`, `0x49555F` | DISASSEMBLED |
| `0x465E30` and the post-game table stop at 9; the table skips `+0x146` = 10 | `0x465F87`; `0x41DEC5`, `0x41DD37` | DISASSEMBLED |
| controlled/viewed block walkers test `first > last` | §3 list | DISASSEMBLED |
| the ID cycle reads only the IDs in `[+0x6F, +0x71]`; ID 0 is NULL | `0x48C1CA..0x48C2FD` | DISASSEMBLED |
| `0x4801F0` frees and zeroes `+0x78` | `0x4801F0..0x480239` | DISASSEMBLED |
| reporter: table read by `+0x2A42` behind GameingState 3; stub `0x4CAA10`; DLL export | `0x46C856`, `0x463EAF..0x463EBE`; `0x4CAA10`; `0x46C7E7` | DISASSEMBLED |
| no `reporter.dll` in the retail install | the reference setup, 2026-10-06 | MEASURED |
| explosion's type-3 owner gate | `0x49A01D..0x49A047` | DISASSEMBLED |
| music event 5 on killer = controlled | `0x4869D1..0x4869EB` | DISASSEMBLED; the meaning of `+0xF4` [INFERRED] |
| ten records seated in a skirmish; recording plays with 0 corrections | `replay-tenseat-load.json` | MEASURED 2026-10-06 (coordinator) |

## 10. As built

What the build settled that §4 left open, in `tagpu_replay.c` unless stated:

- `s_vbuf` is one interlocked word with four states: OFF, ARMED (the seating, with the lowest
  recorded seat copied beside it in `s_vlowest`), READY and FAILED (the loader hook). The first
  step's `first_checks` refuses anything but READY at the Start ("the viewer's record could not be
  prepared"), and `viewer_init` runs only on READY, so a live rewind, which never arms the hook,
  fills record 10 through the seek's R14 and sets its deadlines through R15.
- The hook at `0x4919C8` is installed with the replay's other call sites, all or none, before the
  fill's site: no Start can seat record 10 without it.
- `viewer_seat` and `viewer_take` (record 10 kept whole, once a level) are shared by the seating
  and `rw_prepare`; `viewer_free` is `validate_engine`'s, with the buffers test at a Start only.
- `tagpu_fogwide.c` and `tagpu_gui_hook.c` bound the index with constants of their own, 11
  (`PLAYER_RECS`, `HUD_NPLAYERS`), since neither includes the recording format header.
- The level's end zeroes both `+0x2A42` and `+0x2A43` when either names record 10.
- Record 10 is `main+0x2851` (§2, `0x464A0C`); the `leave` row is `viewerrec`.
