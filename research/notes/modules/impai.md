# ImpAI in the engine

ImpAI's seam, its seat table, the seat choice `seatai`, what each of its seats knows and the self-checks that hold that to the engine, the Difficulty handicap's exemption and the console's `AI` command (`tagpu_impai.c`, `tagpu_impai.h`; the engine-free half `impai.h`, `impai_know.c`; the engine's reads `tagpu_worldq.c`). The stock AI still plays every seat: ImpAI observes the seats it is given and takes them out of the handicap. The plan, the engine facts it rests on and what ImpAI will do are [entering the game](../impai/engine-seam.html) and [ImpAI for the player](../impai/player.html).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`). `main` = `*(void**)0x511DE8`; the player records are at `main+0x1B63`, stride `0x14B`, ten of them; `+0x73` is the controller (1 human, 2 AI, 3 remote or a replay puppet).

## The seat choice: `seatai`

`impure.cfg`'s key `seatai` is `none`, or the seats ImpAI plays as digits 0–9 separated by commas (`seatai=1,3`): bit i for settings entry i, which is record i on every path that seats a skirmish ([sim](../engine/sim.html), *Seating a level*). Anything else logs `settings: seatai: <text> not understood - no seat plays ImpAI` and reads as none. `tagpu_settings_seatai` answers it under `tagpu_defaults.off` too, as the engine honours its own SKIRMISH registry keys under every launch. Nothing in the DLL writes the key: tacli and the player do. **Every tacli launch writes it** (`write_harness_store`, and the remote launch): the seats a scenario names `impai` (`tacli scenario load`), or `none`, so no launch plays an earlier launch's seats ([the scenario format](../scenario-format.html)).

**A saved game does not carry the choice.** The save holds the records' controllers, not the seat table, so a reloaded game seats ImpAI from the `seatai` in force when it loads, and the seats its level-init line names can differ from the game that was saved. The save's players restore `0x466050` also writes `+0x73` (`0x4662C3`) after the level init, so the line can name a seat that is not controller 2 in play; every reader re-tests controller 2, so such a mark does nothing.

## The install

At attach, after `tagpu_reclaim_init` and `tagpu_replay_init`, whatever the levers, and only when the level's two edges are ours:

- replay's play sites are in (`tagpu_replay_level_init_in`), among them the level init's `PLAYERS` call `0x4919C8`, retargeted to `tagpu_replay_level_players`, the only caller of ImpAI's level init;
- the teardown wrap `0x491B60` is in (`tagpu_reclaim_level_tracked`), the only caller of ImpAI's level end.

Then it compares twenty-one sites with their stock bytes: the seam `0x465031`, the `AI` command's two calls `0x4162F4` and `0x4162FE`, and the eighteen handicap sites. Only when every one matches does it build the stubs, in one `SIM_CODE` page, and write all twenty-one, putting back every site it wrote if a write fails. Success logs `impai: seam 0x465031, the AI command 0x4162F4 and 0x4162FE and 18 handicap sites in; seats at 0x<addr>, handicap counts at 0x<addr>` (the addresses `handicap_check.py` peeks). Otherwise nothing is written, ImpAI is unavailable, and the attach and every level log `impai: unavailable: <why>`, a mismatch as `<VA> not stock (<bytes>)`. The sites' rows are in [the hook map](hook-map.html), *The AI seat*.

**The test lever `tagpu_impai_check.on`**, read at attach, turns on the self-checks (*The self-checks*, below). With it, once every site above is in, the install also retargets the radar pass's call `0x46556D` (`E8 CE 1E 00 00`, byte-matched) and logs `impai: check: line of sight in every ImpAI turn, radar at 0x46556D`, or why the radar check is out. Without it `0x46556D` stays stock.

## The seam and the wrapper

The players phase's `call 0x408C40` at `0x465031` calls `impai_ai_tick`, `__fastcall(obj, edx)`: ECX arrives as its first argument and it pops nothing, so the site's stack is unchanged, and `EBX`, `ESI`, `EDI` and `EBP`, which the players phase uses after the call, are callee-saved. It runs an ImpAI seat's observation (*What a seat knows*) and then **always calls `0x408C40`**, so the stock AI plays every seat and the recorder's observer on `0x408C40`'s entry sees each call. A turn counts when all of these hold:

- **the game thread** (`GetCurrentThreadId()` against `g_ddraw.gui_thread_id`), which excludes the loader thread's one players phase `0x497C10`;
- **the bound**: `rec = *(obj+0)` lies at `main+0x1B63 + i·0x14B`, `i` below 10, by range and exact division, before anything is read through it;
- the record is seated with controller 2, the test `0x408C40` makes before it plans;
- the seat table marks record `i`, and `i` is not a puppet seat.

## What a seat knows

Each ImpAI seat keeps, for every unit slot, what it may know of the unit there: `s_known[slot].seat[i]`, 8 bytes (`impai.h`, `IMPAI_SEEN`): the type and owner last identified (`0xFFFF` and `0xFF` when never), the classes it holds the slot by now (`how`: own, line of sight, shared radar, radar, sonar), and the 32-px block last known. An all-zero row is empty. **The information rule**: a row changes only through what the seat observes, so a death or a slot reuse the seat cannot see changes nothing it knows, and no engine fact reaches a decision through a row. A row with no class left is a **ghost**: the last thing known, kept at its block until the seat sees that block again, when it is emptied.

**One turn** (`impai_observe`, then the engine-free `impai_know_tick` over the engine's WORLDQ, `tagpu_worldq.c`), at the seat's place in the players phase, after the step's unit tick, projectiles and explosions and before the seat's own sight emits, so the view is the start of the seat's turn at this tick; nothing after the players phase in the step writes a position, the alive bit, an owner, a type or cloak ([sim](../engine/sim.html), the step):

1. **The line-of-sight slice**: `OBS_SLICE` (2048) slots from the seat's cursor, wrapping. `observe` answers the first class that holds: **own** (the owner byte is the seat); **line of sight**, `0x465AC0`'s rule for the seat — not cloaked, not submerged, then the four points of the unit's box ([map](../engine/map.html), `0x465AC0`) at the live `fix_los_shear` row, read in the seat's counters under True line of sight and in the mapped grid **with the seat's own bit** `rec+0x146` otherwise (the engine's own reads take the viewed player's bit whatever record asks; the stamp writes every record's); **shared**, loop 1 of the radar pass for the seat (the owner's record allies the seat and shares radar) — never in a skirmish, where ShareRadar cannot be set. Own and line of sight identify the type and owner; shared gives the block alone. **A submerged unit is never identified**: the engine sees one only by `+0x110` bit 9, the viewed player's sonar; the seat knows it through its own sonar.
2. **The radar pass**: started when idle and `GameTime % 30 == (3·seat + 1) % 30`, so two seats never start on one tick, and continued every tick until it completes, at most `RADAR_VISITS` (32768) target visits a tick, rounded up to the end of the emitter or jammer in progress. It is `0x467440`'s five loops for the seat ([recorder](../engine/recorder.html), `0x467440`; loop 4, the same for every viewer, left out), walking the engine's own bucket chains as `0x47E890` does, since a unit's bucket can lag its position and a carried unit is in none. Its answer, a nibble a slot in `s_radar_bits[seat]` (radar, sonar, jammed, kept), is published into the rows when it completes: a contact takes the radar and sonar classes, and the block when no class of now holds it; a slot the pass did not find loses them. Under load a pass grows staler; a tick's work does not grow.

The knowledge (`s_known`, keyed by unit slot, `TAGPU_PK_DESIGN_SLOTS` rows of 80 bytes), each seat's cursor and pass (`s_seatst`) and the passes' bits (`s_radar_bits`) are `SIM_STATE`, cleared at every level init; a puppet's come from its recording. The work counters are `SIM_PRIVATE`: each level end logs `impai: work seat=<i> ticks=<n> refused=<n> slots_max=<n>/<bound> visits_max=<n>/<bound>`, a tick's largest count against its bound (the slice plus a pass's publish, one observe a slot; the budget plus one walk, which reads each slot's unit once at most). `refused` counts turns whose view would not open (a bound of `tagpu_worldq_open` failed).

## The self-checks

Under `tagpu_impai_check.on` (`SIM_PRIVATE` counts, logged at the level end):

- **Line of sight**, in every ImpAI turn: the rule against the engine's `0x465AC0` for every live unit of another owner that is not submerged, at one instant on one thread. Under True line of sight for the seat's own record (`impai: check los seat=<i> evaluated=<n> mismatch=<n>`); in the other modes, where `0x465AC0` reads the viewed player's bit whatever record it is given, for the local record `main[0x2A43]`, where both read the same bit (`impai: check los-local …`).
- **Radar**, at `0x46556D`: the engine's pass for the viewed record, then ImpAI's copy for the same record, unbudgeted, and every live unit's bits 8 and 9 compared — bit 10 too where loop 1 cleared it (`impai: check radar passes=<n> evaluated=<n> mismatch=<n>`).

The first mismatch of each is logged with its slot. `tools/impai/impai_check.py logs --los true|permanent <logs>` judges a game's lines: every check `mismatch=0`, at least 10 000 line-of-sight comparisons (each seat's under True, the local record's under Permanent) and 50 radar passes, and every `work` line within its bounds with `refused=0`.

## The seat table

`s_seat[10]` is `SIM_STATE`, keyed by player record.

- **The level init** (`tagpu_impai_level_init`), on the loader thread inside `0x4919C8`, right after the engine's players init `0x464990`, clears the table on every level, live or puppet. On a live level it marks each record that is seated with controller 2 and whose `seatai` bit is set, logging `impai: seats 1,2 (impure.cfg) of 4 records` (or `seats none`); a record with controller 3 (a network game) marks none and logs why. On a puppet level it marks nothing: the recording restores the table at the level's first step, on the game thread, and between the two clears the recorder is its only writer. A recording that carries no row of the table leaves it clear.
- **The level end** (`tagpu_impai_level_end`), on the game thread in the teardown `0x491B60`'s entry, after the recorder has closed the level's recording: it logs `impai: calls seat=<i> n=<turns> gametime=<t>` for each marked seat, `impai: handicap rows scaled/exempt gametime=<t>: 1=<s>/<e> … 18=<s>/<e>` and the observation's `work` and `check` lines (*What a seat knows*), and **clears the table**. So nothing between two levels reads a mark: the next load's loader thread, the only thread that can reach a site before that load's level init (its early pump `0x49727D`, in a network load), is created afterwards by the game thread (`0x4982CA`).
- **Why a game-thread reader sees the whole table**: the level init runs before the loader's last store `0x497C62`, and the game thread runs no step until it has read that store's bit (`0x498342`), the ordering `tagpu_replay.c`'s *THE VIEWER'S BUFFERS* rests on.
- **The recording's header** says which AI played each seat: `RT_PLAYER`'s byte 15 is `tagpu_impai_seat_ai(i)`, 1 when the record is active, controller 2 and marked, else 0 — also in a recording made before the seat choice. The reader refuses a byte above 1. It is written at the header's first capture, after the level init, or at a fork's after the takeover's restore.

## The handicap exemption

The seventeen Difficulty type tests and the on-hit commander purge `0x406FC3` ([sim](../engine/sim.html), *Difficulty and the AI handicap*, has the table with each site's unscaled branch U and scaled continuation S) each become a 5-byte `jmp` to a stub of its own (the rest of the site NOPs). A stub:

1. runs the site's own compare, copied from the site, and its `jne` to U, so a record stock does not scale is not scaled;
2. for a type-2 record, bounds it as the seam does: an offset from record 0 below `0xCEE` (the engine's own loop bound, `0x46602D`) that divides by `0x14B`;
3. when the seat table marks the record, increments the row's exempt count and jumps to U; else increments its scaled count and jumps to S.

It restores `EAX`, `ECX` and `EDX`, touches no x87 register (zero to two values are live on the FPU stack at a site; none at rows 13, 14 and 18: `0x464B88` and `0x464CB8` empty it with `fstp st(0)` before a call, and `0x406F80` holds no x87 value) and calls nothing; the `inc` writes only the flags, which are dead at all 36 targets. Seat 2 marked and seat 1 not, a scaled count is an OTA AI seat's and an exempt one an ImpAI seat's. The counts (`s_diag.hits[18][2]`) are `SIM_PRIVATE` diagnostics, cleared at the level init; nothing in the simulation reads them.

**The ordering.** The table is written by the level init on the loader thread inside `0x4919C8`, before every loader-thread path that reaches a site after it (`0x497B29`, `0x497B40`, `0x497C10`) and before the game thread's first step; then only by the recording's restore and the `AI` command, both on the game thread between steps, and by the level end's clear. The one path before the level init, the loader's early pump in a network load (a removed player's units destroyed: scout I1 of the ImpAI plan, function-level reachability), reads the table the previous level's end cleared, so every type-2 record takes S there. On a puppet every recorded record is type 3, which the site's own compare sends to U as stock does.

**Writing `main+0x37EEE` around calls instead is rejected**: the sites are reached from the economy pass, transfers (including the network's), reclaim, building and unit death, and no one bracket covers them.

## The `AI` command

`0x416280`, the console's `AI` (row `0x501FD0`, mask 4: cheats), sets the type of the active record its argument names through `0x463C60` at one of two calls: type 1 becomes 2 at `0x4162F4` (`push 2`), types 2 and 3 become 1 at `0x4162FE` (`push 1`). Both calls come to `impai_ai_command`, `__fastcall(rec, edx, type)` (the record in `ECX`, the type on the stack, popped by the callee as `0x463C60`'s `ret 4` does), which clears the record's mark, bounded as above and never on a puppet, logs `impai: the AI command makes record <i> type <t>: ImpAI no longer plays it` when there was one, and calls `0x463C60`. So `AI` never leaves a seat to ImpAI: on an ImpAI seat it makes the seat a human's, and a second `AI` hands it to the stock AI; on a human's seat it hands it to the stock AI, as in stock, even one a takeover played as an ImpAI seat. It runs on the game thread, or on the loader thread through a profile script line in `0x4648E0`, inside the players init `0x464990`, before the level init clears the table and marks the record if `seatai` names it ([recorder](../engine/recorder.html), the `AI` command's entry).

## The replay features and their tests

Each a region of `tagpu_impai.c`:

| feature | puppet | what |
|---|---|---|
| `impai_observe` | `SIM_PUPPET_OWNER` | the seam, the wrapper, what a seat knows, the self-checks, the level end |
| `impai_seats` | `SIM_PUPPET_OWNER` | the level init, the `AI` command, the header's accessor |
| `impai_handicap` | `SIM_PUPPET_RUNS` | the site table, the stub generator, the install |

- **`impai-observe` and `impai-observe-perm`** play four records on Core Prime Industrial Area (the human seat idle, three computer seats, 1 and 2 `impai`, 3 OTA AI) under true and permanent line of sight. Each writes the check lever (`tools/impai/check_lever.py`), so its games run both self-checks; `impai_check.py` judges their logs.
- **`impai-handicap`** plays Town & Country at Easy, seat 1 OTA AI and seat 2 ImpAI, both CORE. At GameTime 300 and 330 it applies each seat's reclaims (`impai-handicap-reclaim-1.json`, `-2.json`): a CORE construction kbot ordered to reclaim a wreck (rows 11 and 12) and another ordered to reclaim a radar tower of its own seat (row 17). At 360 the human seat gives each computer seat 100 energy and 100 metal (`+give`, rows 13 and 14). At 4200 it turns cheats on and types `+ai 2` twice (`0x4162FE`, then `0x4162F4`). A takeover of the idle human seat runs the `ai` check.
- **The exit test** `tools/impai/handicap_check.py <instance>`: one held launch of `impai-handicap.json` at each of Easy, Medium and Hard, under the run lock. At GameTime 61, where the two commanders' own terms are the whole income: Easy, OTA AI's energy and metal income exactly half of ImpAI's; Medium, 0.7 of it within 1e-6; Hard, equal; the eighteen sites read `E9`, and the table marks record 2 and not record 1. Then the same reclaims and gives, advanced until rows 11, 12, 13, 14 and 17 have each counted both a scaled and an exempt record, and the economy rows 1 to 8 together both; it prints all eighteen rows' counts. Last, the two `+ai 2`: controller 1 with no mark, then controller 2 with no mark.

**What no test exercises**: rows 9 and 10 (the build-cancel and unbuild refunds), 15 and 16 (the Deathmatch respawn), 18 (the on-hit purge), the economy rows the commanders do not reach (the exit test prints which), the network paths to rows 13, 14 and 17, and the loader thread's profile-script `AI`. They come from the same table and generator as the measured rows and are byte-checked at attach.
