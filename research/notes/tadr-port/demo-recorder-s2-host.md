# Demo recorder S2 — the puppet host (evidence)

## Status

**Desk research, 2026-09-30**, for the [skirmish slice](demo-recorder-skirmish-slice.md)'s S2, with the owner's decisions in that note's *Decided 2026-09-30*. S2a records what its header and applier need. **S2b built the puppet host on this page**, and where the build departed from it or proved it wrong, the section now says what was built and why: the radar pass's 30-tick deadline (§2.3), the damage gate at the apply rather than the sender (§2.6), the reaper and bit 14 (§2.7), the explored map's size (§2.9), and the wind (row 13).

It covers five tasks:

1. other writers on a replay host, and full visibility;
2. the viewer's own input;
3. getter 75 per seat;
4. applying a recorded YARD_OPEN;
5. opening a recording.

**The executable.** Every engine address is in the pristine retail exe (`pristine/TotalA.exe.pristine` in the main checkout, base `0x400000`). It was read with `i686-w64-mingw32-objdump -d -M intel` and raw section reads.

**The source.** Impure's source was read at the worktree's HEAD (`45f01501`).

**Evidence tags.**

- **DISASSEMBLED**: read in this research, with addresses.
- **MEASURED**: cited from the note that measured it.
- **INFERRED**: not proved.

**Terms.**

- **Recorded**: the S1 stream carries it (a `value`, `timer` or similar row of `tagpu/ddraw/replay-layout.tsv`, or a declared table).
- **Puppet host**: a skirmish level whose recorded seats are type 3.
- **Puppet seat**: one of those recorded seats.
- **V**: the viewer's own seat (§1).

---

## 0. The answers in brief

**Task 1: other writers.**

- **The radar pass** runs on a puppet host. It is either display, or skipped:
  - As display, the applier holds the recorded contact bits for takeover and does not write them live.
  - Skipped, every perspective shows the recording viewer's radar.
  - This is decision D1 (§8), the owner's. D1 amends the 2026-09-29 sentence "a replay shows the recording viewer's radar picture".
- **The scoreboard's rank compaction** runs as a presentation write. The reconcile restores the recorded rank at each boundary.
- **The receive timeout** is unreachable in a skirmish. This closes the open question.
- **The sweep of other writers** found several more:
  - The unit tick's reaper runs for puppets, so bit 14 is a death trigger.
  - The fire spread and the meteor shower compute damage on the host.
  - A replayed transport death damages its cargo through the destructor's cargo loop.
  - The footprint contest inverts on a puppet host.
- **All computed damage** is stopped by one sink gate at the hit sender `0x489BB0`, for puppet victims.
- **Full visibility** needs the viewer's own seat V, which the recording does not use, with three fills:
  - V's watcher bit;
  - V's sight counters, filled with 1;
  - V's explored bit in every cell.

  None of these writes recorded state.

**Task 2: the viewer's input.** With `main+0x2A42` = V for the whole level, every path that selects units or issues orders is confined by the engine to V's unit block, which is empty. That covers clicks, keys, build menus, groups, self-destruct and giving units away. Four puppet-mode gates cover the rest:

- typed commands, at `0x417B50` (not the executor `0x4B7900`);
- the debug keys, at `0x4965E1`;
- the resource transfer `0x464C60`;
- the save writer `0x4326B0`.

**Task 3: getter 75.** Getter 75 is Impure's `cob_get` in `tagpu_patches.c`, reached by an E9 at `0x480770`. On a puppet host it answers from a per-seat table built from the header's recorded controllers (1 or 2 answers 1). The table is written at the seat fill, before the loader thread exists. For a skirmish recording the answer is 1 for every seat.

**Task 4: YARD_OPEN.** Three functions are involved:

- `0x47D970` is a grid-bounds test plus a slot-A occupancy test.
- `0x47DAC0` sets `+0x10F` bit 2, sets `+0x110` bit 27, and calls the re-claim `0x47C790` and then the passability refresh `0x440A40`.

The recipe for applying a recorded YARD_OPEN:

1. Bound the unit and its footprint.
2. Skip the occupancy half of the refusal.
3. Write bit 2 and bit 27.
4. Call `0x47C790`, then `0x440A40`.
5. Let the reconcile write `+0x110` last.

The contest outcome in the grid can still differ from the recording until S3 records the grid.

**Task 5: opening a recording.** The engine's own entry is the SKIRMISH Start branch. The puppet host needs:

- **a request file**, `tagpu_replay.play`, read once at the seat fill;
- **sites installed at DLL install**:
  - `0x47B092`, the seat fill;
  - `0x47B09C`, the settings persist;
  - `0x497963` and `0x497ACD`, the start units;
- **the applier's step** at `0x4954ED`;
- **the teardown**.

`tacli replay play` sets the instance's registry store and clicks Start. The header must add:

- the game options as the simulation ran them;
- each seat's PlayerInfo;
- the recording viewer's `+0x2A42` and `+0x2A43`;
- the settings' map name;
- `+0x2A3C`;
- a weapon fingerprint.

---

## 1. The viewer's own seat

### 1.1 What it is

V is the lowest record index in 0–9 that the recording does not seat. A skirmish recording lists its seats in `RT_PLAYER` (`tagpu_recorder.c` `header_build`: every record with a non-zero `+0x73` at the start).

**Seating.** V is seated at the seat fill as type 3 through the engine's seat activation `0x464290(V, 3)` [DISASSEMBLED, `0x464290..0x4644BE`]. That call:

- zeroes the record's five 11-byte arrays (`+0x108`, `+0x113`, `+0x11E`, `+0x129`, `+0x134`);
- sets `+0x73` = 3 and leaves PlayerInfo `+0x94` alone for type 3 (`0x46432C..0x46433A`);
- sets the first dword to 1, `+0x146`/`+0x147`/`+0x148` = V and `+0x04` = V;
- clears PlayerInfo `+0x9B` bit `0x20`;
- names nothing for type 3 in a skirmish. Game type 2 names only types 1 and 2 (`0x46444C..0x464494`).

**V's settings.**

- PlayerInfo `+0x9B` bit `0x40` is set: the watcher bit, as the network watcher switch sets it at `0x46569D`.
- `main+0x2A42` = V for the whole level.
- `main+0x2A43` = V for full visibility, or a recorded seat for a perspective (§2.9).

**What V's record never gets.**

- **No start unit.** The loader creates none, because the start-unit sites skip every record on a puppet host (§6.4).
- **No apply and no comparison.** The applier never writes V's record and the playback comparison skips it. S1 records all ten records every iteration (`REC_PLAYERS` = 10, `cap_players`), and V's recorded image is an empty record, so applying it would un-seat the viewer.
- **The records no one sits in are applied like the seats.** Every record but V's is written raw
  under the exclusion file, not only the recorded seats: the level start `0x465E30` writes the start
  energy and metal (`+0x8C`/`+0x98`) of all ten records from the skirmish settings
  (`0x465EF5..0x465F1A`), and the host's settings for an unseated record are its own menu's, not
  the recording's [DISASSEMBLED; MEASURED: in the battle, records 5–9 differed at `+0x8C` and
  `+0x98` until the apply wrote them].
- **No pacing stamp needed.** The pacing gate `0x495230` takes the smallest `+0x18` among active type-3 records whose live count `+0x144` is above 0 (`0x4952CF..0x495303`, engine map). V never has a unit, so it never holds the host back.

**V's block.** V has an empty unit block of its own. The level init gives contiguous ranges of `main+0x37EE6` slots to all ten records, active or not:

- The loop runs to `cmp edi,0xA` at `0x48592B`, writing `+0x67`/`+0x6B` at `0x4858BD`/`0x4858E0`.
- In a skirmish the order is pointer order, from `0x485940`'s non-network branch.
- [DISASSEMBLED; the engine map's *Replay player names and unit-block ordering* says the same: "The loop does not omit spectators".]

### 1.2 Why selection and orders are inert by construction

Every path that selects units or issues orders walks the controlled player's block `[+0x67, +0x6B]` for units with `+0x110` bit 4 [DISASSEMBLED]:

- the order issuer `0x48CF30`, whose block walk is at `0x48CFAC..0x48CFC5`. Its callers are:
  - the key handler (`0x496497`);
  - the order buttons (`0x41A490`, `0x419BE0`);
  - `Assign` (`0x416366`);
  - the mouse (`0x498F70`, `0x499100`);
- `0x419670` → `0x43AFC0` (`0x41972A..0x419755`);
- the build menu `0x419B00` → `0x43B0B0` (from `0x41AA00`);
- the selected-units collector `0x48CA20`, used by the share dialog `0x4933E0` and by self-destruct:
  - the key handler collects the selection at `0x496436`;
  - it looks up the order type `SELFDESTRUCT` (`0x49643B` → `0x438760`);
  - it cancels an existing order through `0x439E30`/`0x439F80`, or issues a new one through `0x48CF30` at `0x496497`;
- the selection functions `0x48BD00..0x48D9A0`;
- the group assign `0x48D920` (`0x495FA4`) and group select `0x48D9A0` (`0x495FFD`).

V's block holds no unit, so nothing can ever be selected. No order, build, self-destruct, give or group reaches a puppet. The bound is the engine's own: the controlled record's block.

The drag box `0x48C390` clears bits 4, 6 and 7 on every unit. Those are the drop-class bits `selected` and `marks`: the viewer's own state.

This holds while `+0x2A42` = V. §3.4 lists its writers.

### 1.3 The limit: a ten-seat recording [NEW]

The engine map says the SKIRMISH seat count `main+0x38D81` is 4 by default: "That count is why a skirmish seats four players". The default is right, but the count can be 3 to 10.

**The screen's cheat handler** [DISASSEMBLED]. The SKIRMISH screen's handler `0x47B9F0` has no direct caller; it is a GUI handler.

- It matches the name suffixes `*III` … `*X` (strings at `0x508470..0x508498`) to the counts 3–10 (`0x47B9F7..0x47BB08`).
- It persists the count (`0x430F00` at `0x47BB3D`; `0x432B80` writes `NumSkirmishPlayers`).
- It stores the count at `0x47BB47`.
- It re-reads the settings (`0x42F9A0`) and plays the sound `SkirmishCheat` (`0x47BB7D`).
- It is a hidden SKIRMISH-screen cheat. Its key sequence was not traced.

**The registry read has no bound** [DISASSEMBLED]. The read of `NumSkirmishPlayers` at `0x4307B4..0x4307DD` stores the value whatever it is: both range tests (`0x4307C1`, `0x4307C5`) branch to the same store. So tacli's registry store can set any count.

**A count above 10 overruns.** The seat fill loops to that count (`0x47A8A4..0x47A8BE`) and would write past the ten records [INFERRED from the loop]. The count is input the puppet host bounds itself.

**Consequence.** A skirmish recording can seat all ten records and leave no index for V. Record 10 cannot serve (§1.4). **Decision D2** (§8):

- S2 refuses a ten-seat recording, with a log line that names the reason.
- The multiplayer slice designs the full house, where ten seats are common: a borrowed seat with sink gates on the order issuers and selection, or more records.

### 1.4 Record 10 is not a seat

- The level init assigns blocks to indices 0–9 only (`0x48592B`), so record 10 has none.
- Every player loop treats `+0x146` = 10 as no player:
  - the players phase;
  - `0x4816A0` (`0x48172A`);
  - the loader (`0x497958`, `0x497ABD`);
  - `0x41DD37`.
- The AI toggle refuses an index of 10 or more (`0x41629A`).
- B7 marks shooterless projectiles with owner 10.

[DISASSEMBLED]

---

## 2. Task 1: other writers on a puppet host

### 2.1 Where they run

**The skirmish frame branch** of `0x496790` (from `0x496918`) runs, in order:

1. the pacing gate `0x495230` (`0x496928`);
2. the sim step `0x495490(0)` (`0x49693C`);

Unless `main+0x37EBE` bit 0 or the pause bit `+0x38A51` bit 0 is set, it then runs:

3. the keys `0x495E90` and the mouse `0x41CE90`;
4. the hot units `0x48BAE0`;
5. `DrawGameScreen` `0x468CF0` (`0x4969CD`);
6. the scenario tick site `0x4969D2`.

**Each iteration of the sim step** [DISASSEMBLED; engine map line ~720]:

| order | call | address | what |
|---|---|---|---|
| 1 | GameTime++ | `0x4954C0` | |
| 2 | `0x48AD30` | `0x4954ED` | the unit tick. B2's stub is here (the capture, then the air rebuild), and so is S2's applier. |
| 3 | `0x49B720` | `0x495513` | projectiles |
| 4 | `0x420F30` | `0x495539` | explosions |
| 5 | `0x464F80` | `0x49555F` | the players phase |
| 6 | `0x424050` | `0x495585` | features, and the fire spread |
| 7 | `0x415B30` | `0x49558A` | steps the GAF sequences of `main+0x148EB` through `0x4B8B90` |
| 8 | `0x490C40` | `0x49558F` | the wind |
| 9 | `0x437DE0` | `0x495594` | meteors |
| 10 | `0x41CA10` | `0x495599` | the camera |
| 11 | `0x471EB0` | `0x4955BF` | particles |
| 12 | `0x466580` | `0x4955E5` | the radar blink |
| — | `0x457D30`, `0x4618A0` | | network only |

After the loop come `0x428BD0`, `0x428BE0` and `0x428BF0` (empty), then `0x463EF0` (the chat lines), then `0x482130` (sight-record expiry).

### 2.2 Dispositions

The disposition keys:

- **SKIP**: gated on a puppet host.
- **DISPLAY**: let run; it writes only host display state.
- **AGREEING**: let run. It writes recorded state with what the recording machine's same code wrote from the same inputs. The reconcile restores the recorded value at each boundary, and a difference is a bug the harness names.
- **UNREACHABLE**: shown not to run on a puppet host.

| # | Writer | Reached on a puppet host? | What it writes | Disposition |
|---|---|---|---|---|
| 1 | The radar pass `0x467440`, the viewed record's 30-tick pass | Yes: one caller, `0x46556D` in the players phase, for `bl == +0x2A43`. Returns if `+0x2A3C` < 2. | **Loop 1** (`0x46749F..0x46750C`): every live unit. Clears bit 12 (`0x4674B6`), then sets bits 8–9 when the owner is the viewed player, or is allied with ShareRadar (info `+0x97 & 0x40`), or the viewed record is a watcher (info `+0x9B & 0x40`, `0x4674DB..0x4674EB`); otherwise clears 8–10. **Loops 2–3**: radars and jammers (bucket walk `0x47E890`, callbacks `0x4FD55C`/`0x4FD558`/`0x4FD554`). **Loop 4** (`0x467690..0x4676FD`): only type-1/2 owners, so never on a puppet host: `+0xB0` = GameTime + 90 and bit 12. **Loop 5** (`0x467710..0x467829`): sight test for the viewed player → bit 8. | **D1** (§2.3) |
| 2 | The scoreboard `0x4948E0` | Yes: `0x469F65` in `DrawGameScreen`, game types 2 and 3, while shown or held | Compacts the ranks `+0x148` of active type-1–3 players when no one holds rank i (`0x494DC0..0x494E41`, store `0x494E27`) | Let run. Compaction preserves order, and the reconcile writes the recorded rank at every boundary. No simulation reader of `+0x148` other than the destructor's rank update `0x486B2D..0x486C17`. The comparison happens at the boundary, after the apply. |
| 3 | The receive timeout `0x453C20` | **No** | Reads `+0x1C` (its dialog `0x453A50`, callback `0x453640` installed at `0x453B9A`, read at `0x453839`) | **UNREACHABLE**: the one caller `0x455F64` is inside the pump `0x453D40`, which returns unless `main+0x2A44` bit 0 is set (`0x453D4B..0x453D5C`). All seven pump callers go through that test. The seat fill refuses a request when the bit is set (§6.4). |
| 4 | The players phase `0x464F80`, per active type-1–3 record with `+0x146` ≠ 10 | Yes, for every puppet seat and V | See the rows below | as below |
| 4a | `0x408C40` (AI) | Only when `+0x74` is set, and type 3 has NULL (`0x46434A`) | — | UNREACHABLE |
| 4b | `0x40B2C0(i)` | Returns for a NULL `0x5119C0[i]` [DISASSEMBLED earlier; the engine map says type 3 has no `0x5119C0` object] | — | UNREACHABLE |
| 4c | Sight re-stamp `0x4827B0` of every live unit in the block (`0x465046..0x465063`) | Yes | Unit `+0xF8`/`+0x7A`/`+0x7C` and the owner's counters | AGREEING, because the apply runs after the unit tick and before this phase ([the events note](demo-recorder-s2-events.html) §1.0): the stamp reads image T+1's positions, as the recording machine's read the positions its own tick left. The applier writes these only through the position recipe. |
| 4d | The 30-tick gate: `+0xF0 += 30` (`0x465077..0x465092`) | Yes | Player `+0xF0` (recorded) | AGREEING |
| 4e | The end tests (`0x46516D`/`0x46586F`) and the Deathmatch respawn (`0x4651B2..0x4654EF`, which creates a unit for `+0x2A42` through `0x485F50` at `0x4653D9`) | For `+0x2A42` = V | Victory, defeat, a unit | Disarmed by `0x4904B0` in the seat fill; the respawn is behind the defeat test (`0x465174`) [MEASURED in the engine map] |
| 4f | The economy `0x401360` | No: types 1/2 only (`0x465539..0x46553F`) | — | UNREACHABLE |
| 4g | Minimap `0x466DC0` / `0x466C20` | Viewed record | Display | DISPLAY |
| 5 | The unit tick `0x48AD30`, every slot of a type-1–3 record | Yes | See the rows below | as below |
| 5a | `0x437910` at `0x48ADC4` | Yes, when def `+0x1D2` > 0 and `main+0x37EE2` (wind changed) is set | Starts the COB scripts `SetDirection(+0x37ED8)` and `SetSpeed(+0x37EDA << 4)` | Let run: COB presentation. Its inputs agree with the recording (row 13). |
| 5b | `+0xFA`/`+0xFB` countdowns; `+0xF6`/`+0xF7` HP% every 30 ticks | Yes | Recorded values | AGREEING |
| 5c | Bit 4 cleared on unselectable units (`0x48AE13..0x48AE59`) | Yes | Drop class | DISPLAY |
| 5d | The local block `0x48AED3..0x48AFB5`: water damage `0x48AF32`, auto-heal `0x41BD10` at `0x48AF92`, order controllers `0x43B7C0`/`0x43BAD0`, movement `0x43DD20`, `0x48A870` | No: types 1/2 only (`0x48AEC4..0x48AECD`) | — | UNREACHABLE |
| 5e | **The reaper** (`0x48AFB9..0x48AFD1`): bit 14 → `Send_UnitDeath 0x4864B0(unit, +0xF5)` | **Yes, for every owner** | A death | §2.7 |
| 6 | Area damage from projectile impacts | Owner gate `0x49A01B..0x49A047` (B7-patched): damage only when the projectile's owner record is empty or not remote. Puppet shots are type 3, so no damage; owner 10 computes. | Unit HP | Covered by the sink gate (§2.6) |
| 7 | **The fire spread** `0x4239C0` (features `0x424050` at `0x424463`) | **Yes** (sim RNG `0x4233A0`) | `0x49A0C0(weapon, pos)` at `0x423BE0` builds a zeroed projectile with owner 10 (`0x49A0EB`) and calls area damage `0x49A120` **directly** (`0x49A109`), bypassing the owner gate | The sink gate stops the unit damage; the burning itself is feature state (S3). **D5** |
| 8 | **The meteor shower** `0x437DE0` (`0x495594`, no host test) | **Yes** (sim RNG) | Stones of owner 10 compute damage (B7: `+0x62` = 0 when spawned) | **SKIP** the shower on a puppet host; the recording's stones come as events. **D5**. Built as a gate at `0x437DE0`'s entry, whose one caller is `0x495594`. |
| 9 | **The destructor's cargo loop** in `0x4866D0` (`0x4867D0..0x48682A`) | **Yes**, whenever the applier replays a carrier's death through the destructor | For each carried unit: `0x489BB0(+0xF0, cargo, 30000, 3 or 6, 0)`, then `0x48AAC0(cargo, 0, −1, 1)` (unload: carry links) | The sink gate stops the damage. The unload writes recorded carry links, so the apply orders the carried units' fate before the carrier's death, and the loop then finds `+0x8A` empty. Shared with the carried-units research. |
| 10 | Kill-all on a commander's death `0x486F10` (`0x4866B3`) | **No**: only for an active type-1/2 owner (`0x48669A..0x4866A3`) | — | UNREACHABLE [MEASURED in the engine map] |
| 11 | The hit sender `0x489BB0(attacker, victim, amount, type, x)` | Via rows 6, 7 and 9 | Applies **locally for every owner** (`0x489C89` → `0x489CE0`), then forwards to a type-3 owner (`0x489C8E..0x489CC4`) through `0x451DF0`, which sends nothing without the network bit (`0x451EAB`); `ret 0x14` | **The sink gate**, at the apply (§2.6) |
| 12 | AI: AutoAim, loop 4 of the radar pass | No: types 1/2 | — | UNREACHABLE |
| 13 | The wind `0x490C40` | Yes | Main-block wind fields. Impure's draws at `0x490C5A` come from `s_windState` (`tagpu_patches.c` ~9199–9300, `SIM_VAR(s_windState, SIM_STATE)`). | AGREEING. S2b seeds `s_windState` once, at the first step, from the recording's first image (`wind_seed`, found by its name in the header), so every draw after it is the recording's; the table then compares equal at every dump. The main-block fields are S3's. |
| 14 | `0x415B30` (the GAF sequences), `0x466580` (radar blink), `0x471EB0` (particles), `0x41CA10` (camera), `0x463EF0` (chat lines `+0x2A40`) | Yes | Display and animation (main block) | DISPLAY |
| 15 | `0x482130` sight-record expiry through `0x481D50` | Yes | Main-block sight records | AGREEING [INFERRED; S3] |
| 16 | The destructor's rank update (`0x486B2D..0x486C17`) and kill counts, for applied deaths | Yes | `+0x148`, kills | AGREEING |
| 17 | The elimination message `0x47BD70` (`0x486E54`, at `+0x144` = 0 in a skirmish) | Yes | A chat line | Let run: the game showed it. The probe filtered it only during rebuilds. |
| 18 | Typed commands, debug keys, the resource transfer, the save | Yes | Many things | §3.2 |

### 2.3 The radar pass and the contact-bit decision (D1)

The recording carries the contact bits `+0x110` bits 8–10 as values: the decision of 2026-09-29, because the AI's acquisition scan `0x40AA40` reads them at a takeover. On a puppet host the radar pass writes the same bits for the host's `+0x2A43`, on that record's 30-tick pass, which runs after the apply at `0x4954ED` in the same step.

The two writers fight. After a radar tick the draw shows the host's picture; from the next boundary it shows the recorded one again. That is a flicker every 30 ticks between two radar pictures.

Loop 1 also clears bit 12 on every unit on every pass, and bit 12 is recorded. Loop 4, which sets bit 12 and `+0xB0`, never runs on a puppet host.

**D1** (§8), the owner's:

- On a puppet host, contact bits 8–10 and 12 are display state.
- The radar pass computes them for the chosen perspective.
- The applier keeps the recorded bits in its model for takeover and forks, and does not write them live.
- The playback comparison masks them.

**The pass runs only if the host keeps the 30-tick deadline itself.** The pass is called from the viewed record's 30-tick block (`0x46556D`, with the minimap fog `0x466C20` after it), which runs when the record's `+0xF0` has reached GameTime (`0x46507D..0x465089`) and then adds 30 (`0x46508F`). The applied image of T+1 carries `+0xF0` as the recording machine's players phase of T left it, already advanced, so a host that wrote it would never find a seat's pass due, and never ran the radar pass for a recorded perspective: every unit kept the bits its create gave it (`0x485C2C..0x485C48`: bits 8–9 cleared, bit 9 set when the owner is the viewed record). MEASURED 2026-09-30: from the battle recording's viewer, record 0, every one of 1,413 units lacked bit 8 at every dump. S2b therefore leaves `+0xF0` to the host's players phase and compares it: both machines' deadlines start at the load's GameTime (`0x464700`) and step by 30 whenever GameTime reaches them, and the host plays one recorded iteration a GameTime. The only other writer, `0x466050`, is the saved game's loader (its one caller `0x43264E`). V's own pass always ran, since V's record is not applied.

What this buys: full visibility and every perspective show the engine's own radar picture. The recording viewer's perspective shows the same picture the recording machine did, whenever the host's inputs match. Those inputs are positions, alliances, radar and jammer units, and PlayerInfo share bits, and PlayerInfo is not recorded (§6.6) [INFERRED].

The alternative is to skip the radar pass for puppets and apply the recorded bits. Then every perspective shows the recording viewer's radar picture, and full visibility shows blips only where that viewer had them.

Optionally, after a perspective switch, call `0x467440` once on the game thread at the next boundary, so the picture follows at once rather than within 30 ticks. The call reads `+0x2A43` and writes only the bits D1 makes display.

### 2.4 The scoreboard

The compaction is a presentation-driven write of a recorded value on the recording machine too. It runs only when that viewer had the scoreboard shown or held. So `+0x148`'s recorded history depends on the recording viewer's UI.

On the host, the reconcile restores the recorded rank at each boundary. The order shown is the same, because compaction keeps order.

The alternative is to suppress the store at `0x494E27` on a puppet host. The scoreboard would then show gaps in rank. Not recommended.

### 2.5 The receive timeout (closes the note's open question)

A skirmish never reaches it.

- `0x453C20` has one caller, `0x455F64`, inside the pump `0x453D40`.
- The pump returns at `0x453D4B..0x453D5C` unless `main+0x2A44` bit 0 is set.
- The pump's seven callers are `0x41F80B`, `0x434EAF`, `0x44A69C`, `0x4954C8` (the sim step, only when its argument is set), `0x4968CB`, `0x49727D` and `0x49852E`.

**Where the bit is set and cleared.**

- It is set only in menu code: `0x426D34`, `0x42700C`, `0x4277DF`, `0x427B02`, `0x427C0F` and `0x428478`.
- It is cleared at `0x41EFEC`, `0x450E10` (in `0x450DD0`, also reached from the teardown's jump `0x491C54`), `0x4523D2`, `0x496AB4` and `0x496B38`.

[DISASSEMBLED]

The seat fill refuses a request while the bit is set, so a puppet host is a skirmish by construction. The `+0x1C` timer row needs no host stamp.

### 2.6 Damage: one sink gate

**The gate, as built.** At the entry of the apply `0x489CE0(rec)` (`ret 4`), on a puppet host, return when the record's victim, the `u16` at `rec+1`, names a unit of a puppet seat: bounded first to `1..(last−first)/0x118` of the unit array, then its `+0xFF` < 10 and `seat[+0xFF]`. The desk design put it at the entry of the sender `0x489BB0`; the apply is where every hit lands, the sender's included (`0x489C89`), and the only other direct caller is the pump's `0x0B` case (`0x455412`), with Impure's own stubs that jump to it (B4's companion). The sender's forward to a type-3 owner sends nothing in a skirmish (`0x451DF0` tests the network bit at `0x451EAB`).

**The invariant.** On a puppet host no computed hit lands on a puppet.

**Coverage.** Every caller of `0x489BB0`, which reaches the apply through `0x489C89` [DISASSEMBLED]:

- order handlers: `0x402147`, `0x402701`, `0x404981` and `0x414B8D`;
- `0x41BC49` in `0x41BA60`, and `0x41BDC7` in `0x41BD10`, both reached from order handlers and the local block;
- the cargo loop `0x48680B`;
- the kill-all `0x486F94`;
- the give `0x4886A4`/`0x4887D0`;
- water damage `0x48AF32`;
- area damage per victim `0x499E37`.

On a puppet host only the cargo loop and area damage reach puppet victims; area damage is reached through the fire spread and stones. The gate covers them, and any source not listed here.

**Recorded hits.** If S2 replays hits, they enter at the apply `0x489CE0` as the pump's case does (`0x455412`), not through the sender.

**What the gate does not cover.** Features are damaged through `0x4244B0` (`0x49A626`), not through the sender. They are S3's.

**The meteor shower** is also skipped at `0x495594` on a puppet host. Its stones would fall where the host's RNG puts them, not where the recording's fell. **D5.**

### 2.7 The reaper and bit 14

The unit tick's reaper runs for every owner type. A unit with `+0x110` bit 14 calls `Send_UnitDeath 0x4864B0`. For a type-3 owner, that runs the local destructor `0x4866D0(msg, 1)` without a broadcast (`0x48664B..0x48666D`) and without the kill-all (`0x48667E..0x4866B3`).

So the recorded bit 14, written raw by the reconcile, kills the puppet at its next unit tick. Deaths that happen inside a tick never show bit 14 at a boundary; for example, the units a commander's kill-all hits later in slot order. So S2 needs one route for deaths, owned by the events research:

- **(a)** Deaths recorded at their root and replayed through `0x4866D0(msg, 0)`, with bit 14 kept out of the reconcile; or
- **(b)** A proof that the reaper route plus the "gone" records never doubles a death.

The host's constraint: **bit 14 is a death trigger on puppets**, so the reconcile must not write it unless the plan chooses (b).

**As built (S2b): route (a), with the trigger removed instead of the bit.** Deaths are replayed at their recorded point through the destructor (C4's wrap, `tagpu_tx_replay_death`), and bit 14 is written raw like any value. The reaper's `Send_UnitDeath 0x4864B0(unit, …)` (`ret 8`) is refused at its entry for a puppet's unit, so a unit the recording marks dying lives until its recorded death, in the step it died in there. MEASURED 2026-09-30 on the 1,432-unit battle: 252 reaper calls refused, every dump equal, bit 14 included. The gate covers every caller: the reaper `0x48AFD1`, the kill-all `0x486EB2`, `0x486EF8` and `0x486FB4`, CreateFromNetwork's occupant `0x486244`, and the scenario tooling's `UNITS_KillUnit`. The last, the teardown's destroy-all `0x485980` (`0x4859A8`, called at `0x491B95`), runs after the teardown wrap has ended the puppet mode, so it destroys every unit as ever.

### 2.8 The footprint contest inverts on a puppet host [NEW consequence]

The stamp `0x47CC30` and the re-claim `0x47C790` resolve a contested cell the same way. The newcomer takes the cell when the incumbent's record has a set first dword and `+0x73` = 3; otherwise the incumbent keeps it. The sites [DISASSEMBLED]:

- in the stamp: the yardmap slot A at `0x47CDCA..0x47CE23`, and the slot-B contest at `0x47CFDA..0x47D03A` (engine map);
- in the re-claim: `0x47C847..0x47C8A9`, `0x47C950..0x47C9B2` and `0x47CA2C..0x47CA8E`.

**The inversion.**

- In a skirmish recording, every incumbent is local, so the first stamp keeps the cell.
- On a puppet host, every incumbent is type 3, so the last stamp takes it.

**Consequences.**

- Which unit holds a contested cell depends on the order of stamps, and the host's order is not the recording's.
- Bits 26 and 27 are recorded and restored by the reconcile, so the unit records agree while the grid may not.
- S2 does not record the grid (the main block is S3), so the harness cannot see the difference.
- The grid's slots are what area damage and the direct-hit test `0x49B090` read. That is irrelevant on a host that computes no damage, but it matters at a takeover or fork.

**Open for S3:** record the feature grid's unit slots.

### 2.9 Full visibility without writing recorded state (F3), and perspectives

The fills run at the host's first sim step, after the loader's `0x4816A0(1)` at `0x497AE5`:

1. V's PlayerInfo `+0x9B` |= `0x40`. This is done at the seat fill already.
2. V's sight counters: fill the `[rec_V+0x88]` bytes at `[rec_V+0x7C]` with 1.
   - This is the value `0x4816A0` writes into every type-1–3 record's counters when true LOS is off (`0x481734..0x481767`: u8 cells, the length at `+0x88`).
   - Bound: the length the engine computed at load. It is the `sightgeom` row, class `cache`, which "bounds 0x4816A0's fill".
3. The explored map `[main+0x14273]`: set bit `1 << V` in each of its u16 cells.
   - The map is `(W·H·2) >> 2` bytes, W·H/4 cells, with W = `+0x14233` and H = `+0x14237`: the size the allocation (`0x483CF6..0x483CFF`) and `0x4816A0`'s fill (`0x4816B5..0x4816D2`) use, and the saved game's loader `0x484FA0` accepts.
4. Clear LosType bit 3 so the draw rebuilds the fog grid. `0x4848E0` builds it from `+0x2A43`'s counters when bit 3 is clear.

**Why none of this is recorded state.**

- V's record is outside the apply and the comparison.
- PlayerInfo is not recorded in S1.
- The counters are an `object` row.
- The explored map is main-block state. S3's comparison must mask V's column, or re-set it after a restore.

**It holds for the level.** Stamps write only their owner's counters and explored bits, and V owns no unit. The other callers of `0x4816A0` are all gated or unreachable:

- the typed commands `LOSType` `0x416690`, `LOS` `0x416D50`, `Mapping` `0x416D80`, `NowISee` `0x417540` and `MakePoster` `0x417600`/`0x495A30`: gated, §3.2;
- the network watcher switch `0x4656CE`: game type 3 only, `0x465643..0x465651`;
- the Deathmatch respawn `0x4654EF`: behind the disarmed defeat test;
- the loader: before the first step.

Impure calls `0x4816A0` nowhere (source grep).

**What the viewer sees.**

- **Every unit but cloaked ones.** The sight test `0x465AC0` hides a cloaked unit from everyone but its owner.
- **Submerged units** need contact bit 9. The radar pass gives it for everything when the viewed record is a watcher, under D1.

**Other effects of the watcher bit** [DISASSEMBLED]:

- It keeps V off the scoreboard (`0x494BF3`, `0x494E06`).
- It keeps V off the share lists (`0x4935CB`, `0x49394C`), the TAB menu (`0x495120`) and the name list (`0x41DD43`).
- It changes a draw at `0x46A16B` [INFERRED: the controlled player's resource bar is skipped].
- It **refuses the game-speed keys**: `0x496550` and `0x4965AA` jump past `0x490DF0`. Playback speed therefore needs Impure's own control (D7).

**A perspective of seat N.**

- Set `+0x2A43` = N and clear LosType bit 3. This is what the engine's `View` command does at `0x416BC1`.
- Under D1, one radar pass follows at once.
- N's counters and explored bits are kept by the host's stamps of N's units from applied positions (AGREEING).

---

## 3. Task 2: the viewer's own input

### 3.1 Inert by the empty controlled block (§1.2)

These all act on the controlled block's selected units, and there are none:

- map clicks that order (`0x41CE90` → `0x498F70`/`0x499100` → `0x48CF30`);
- every selection (click, drag box, type select, select-all: `0x48BD00..0x48D9A0`);
- the order keys and order buttons;
- ON/OFF, which are orders (`0x403010`, `0x403040`);
- the build menu and build placement (a selected builder through `0x419B00`);
- self-destruct (§1.2);
- groups;
- the next-unit cycle `0x48D4D0`;
- giving units away (the share dialog `0x4933E0` gives the selection, through `0x48CA20`);
- `Capture` (a local unit's order, `0x4046C5`).

### 3.2 Inert by a puppet-mode gate (four sites)

**1. Typed commands: gate `0x417B50(line, mask)` (stdcall, `ret 8`), and return 0.**

Its callers:

- the chat send `0x493E36`: lines starting with `+`, mask 1, or 7 when `main+0x37F2F` bit 1 (cheats) is set;
- the key handler `0x496393`: mask −1, when cheats are on.

That refuses every command the viewer can type. The console table at `0x501D38..0x502138` has entries of `{name, handler, level}`; the levels are 1 (normal), 2 (cheat) and 4 (debug). It includes:

| command | handler | level | note |
|---|---|---|---|
| `Give` | `0x416BD0` | 1 | |
| `Logo` | `0x4168D0` | | |
| `Selectable` | `0x416460` | | |
| `Share*` / `SetShare*` | `0x418CD0..0x419400` | | |
| `View` | `0x416B50` | 2 | |
| `LOS` / `LOSType` / `Mapping` / `NowISee` | | | |
| `Meteor` | `0x417760` | | |
| `DoubleShot` / `HalfShot` | | | |
| `AI` | `0x416280` | 4 | |
| `Control` | `0x416AB0` | | |
| `Kill` | `0x4164B0` | | |
| `IWin` / `ILose` | | | |
| `Assign` | `0x416310` | | |
| `BurnAll` / `BurnOne` | | | |
| `Save` | `0x417430` | | |
| `SeaLevel` / `TreeDeath` / `Feature` | | | |
| `Include` | `0x4177E0` | | runs `debugdat\%s.txt` |
| `ReloadAIProfiles` | `0x4174D0` | | |

[DISASSEMBLED: the table dumped from `.data`]

**Not at the executor `0x4B7900`.** The level init runs the map's AI script through it:

- `0x4919C8` → `0x464990` → `0x4648E0` (the map's script, or `ai\default.txt`);
- then → `0x4B7A30` → `0x4B7900`, with mask −1 (`0x464932..0x464937`).

A later takeover to an AI seat needs that script.

**The display commands.** Among the refused commands are harmless display ones: `Clock`, `Gamma`, `ScreenChat`, `ShowRanges`, `Shadow` and similar. Allowing them would take a list. **D4** recommends refusing all.

**2. The debug keys: gate `0x4956C0(key)` (`ret 4`), whose one caller is `0x4965E1`.**

- It is reached only when `main+0x3923B` bit 1 is set. That is the SoftwareDebugMode word `tacli switches` writes.
- `0x495749..0x495794` sets bit 14 (dying) on the unit under the cursor.
- `0x49579E` fills every type-1–3 player's current resources from storage.

**3. The resource transfer: gate `0x464C60`.**

Its callers:

- `Give` (`0x416C8D`);
- the SHARE dialog `0x4934B0` (`0x49362C`), opened from the TAB menu through `0x4936F0`;
- two network-only callers (`0x4558D6`, `0x457E78`).

It takes from `+0x2A42` (= V) and deposits into the recipient's accumulator `[+0xEC]+0x18`. That is an `object` row, not recorded, and a takeover's economy would drain it.

**4. The save: gate `0x4326B0(path, desc, x)` (`ret 0xC`).**

- Its callers are the console `Save` (`0x41747B`) and the save dialog (`0x493049`).
- ARMOPT disables SAVEGAME and LOADGAME only when the game type is 3 (`0x460CF7`, `0x460D37`), and a puppet host is type 2.
- The load action `0x492360` accepts game types 1 and 2 (engine map).
- So a saved puppet host would load as a skirmish with type-3 seats that nothing stamps. Refuse it.

### 3.3 Left running: the viewer's presentation and playback controls

- Pause: playback pause.
  - The pause key toggles `main+0x38A51` bit 0 (`0x4960AA..0x4960BC`), then sends the new state (`0x4960DA` → `0x451DF0`) [INFERRED: the send does nothing in a skirmish].
  - The options menu `0x460CC0` (`0x4961B7`) sets `main+0x37EBE` bit 0 (`0x4961C1`), which stops the sim step while the menu is open.
- The camera and its bookmarks `0x41D3B0`/`0x41D3F0`, and the minimap.
- The scoreboard.
- Chat and TALK `0x494050` (skirmish chat is local lines).
- Hover info `0x48BAE0`.
- Build-menu pages (the `buildpage` bits: drop class).
- The TAB menu. Its lists exclude watchers; SHARE ends at the gated transfer.
- Game speed: refused by the engine for a watcher (§2.9).
- Alliances change only through network code (`0x452960`) [DISASSEMBLED earlier in this research].

### 3.4 `+0x2A42` and `+0x2A43`: every writer [DISASSEMBLED]

These are every byte store with displacement `0x2A42` or `0x2A43`. The only wider stores nearby are word stores at `+0x2A40`, which cover `+0x2A40..+0x2A41` (`0x448D2C`, `0x463C95`, `0x463CE9`, `0x463CFF`, `0x463F3C`, `0x463F53`, `0x4646F2`).

| writer | stores | reached how | on a puppet host |
|---|---|---|---|
| the seat fill | `0x47A7D2`, `0x47A7DE` | Start | ours |
| `Control` | `0x416B25`, `0x416B38` | typed | gated |
| `View` | `0x416BC1`, `+0x2A43` only | typed | gated |
| the players reset `0x4644D0` | `0x46451B`, `0x46452D` | `0x42734F`, `0x427D73` and `0x4518AC`: menu and network paths | not in play |
| the saved-game restore `0x466050` | `0x466084`, `0x466097` | `0x432610`, from the loader's saved-game branch `0x497B29` | a load tears the level down first |

Stores through a computed address are not excluded by this scan.

**So `+0x2A42` = V holds for the level by construction, once typed commands are gated.**

---

## 4. Task 3: getter 75 per seat

**Where it lives.**

- It is `tagpu_patches.c` `cob_get` (lines 11161–11199), installed by `cob_get_sites` as an E9 at `0x480770`, over the stock getter's `mov eax,[ecx+0x540]`.
- Keys other than 32 and 69–75 go to the stock getter through the trampoline `s_cobNativeGet`, which rejoins at `0x480776`.
- Key 75 bounds the target with `cob_live_unit`:
  - the id below u16 `main+0x14351`;
  - the live bit set;
  - `+0xA8` equal to the id;
  - the owner `+0xFF` below 10.
- It then answers `+0x73 ∈ {1, 2}` for the owner's record.

**What it answers today.**

- **On a puppet host**, every seat is type 3, so it answers 0 [MEASURED: `demo-recorder-state-evidence.md`].
- **On a skirmish recording machine**, every seat is 1 or 2, so it answered 1 for every unit.
- **The console AI toggle `0x416280`** flips 1 and 2, so it does not change the answer. It is the only in-play controller change in a skirmish.

**How playback answers the recording machine's bit.** A per-seat table in the puppet module:

- **`seat[10]`**: this record is a puppet seat.
- **`local[10]`**: the recording machine's bit, from `RT_PLAYER`'s controller. 1 or 2 gives 1; 3 or 4 gives 0, for the multiplayer slice.
- **`cob_get` key 75**: `if (seat[owner]) return local[owner];` and otherwise the stock rule. `owner` is bounded below 10 by `cob_live_unit` before the table is indexed.
- **V**: `seat[V] = 1`, `local[V] = 0`. V owns no unit.

**Ordering.**

- The table is written at the seat fill on the game thread, before the loader thread exists, and cleared at the level's teardown.
- COB reads it on the game thread in play, and on the loader thread during Create's first slice. There are no loader-thread creates on a puppet host, but the ordering covers them anyway.
- **Takeover** follows the approved rule: flip the seats (`seat[s] = 0`, `+0x73` = the recorded controller), then re-create the units. Create's first step then reads 75 from the live controller.
- **Recorded controller changes** (multiplayer) update `local[]` at the boundary where the applier applies them.

S1 does not yet record controller changes. The decision says the recording carries each one, but S1 has only the header. A skirmish needs none for getter 75; a takeover does, to know which seats are AI.

**Its class (D3).** The simulation reads it, but it is the host's configuration, derived from the header, not the recording's state. A restore from a recording must never write it.

Recommendation:

- Declare it in a new puppet module (a `sim` file), with a class that is rebuilt and never recorded or restored.
- `SIM_CONTENT`'s shape fits: derived at level start and constant until the teardown or a takeover.
- This keeps `tagpu_recorder.c` `presentation`.
- It is the owner's call, because it is a non-STATE class on memory the simulation reads.

**The recorder must refuse a puppet host.** `tagpu_rec_capture` starts on any skirmish while `tagpu_recorder.on` exists. It must refuse to start while puppet mode is on, or it would record the playback as a new skirmish.

---

## 5. Task 4: applying a recorded YARD_OPEN (open item 2)

### 5.1 The functions [DISASSEMBLED unless marked]

**`0x47D970(unit, open)`: the refusal.** stdcall, `ret 8`; its one caller is `0x47DACC`.

It returns 0 unless the footprint lies strictly inside the grid:

- the footprint is X = `+0x76` and Z = `+0x78` (read as one dword at `0x47D98E`), fw = `+0x7E`, fh = `+0x80`;
- 1 ≤ X (`0x47D991`);
- 1 ≤ Z (`0x47D9A4`);
- X + fw < W = `main+0x14233` (`0x47D9C1`);
- Z + fh < H = `main+0x14237` (`0x47D9D6`).

Then:

- The mask is `open ? 2 : 4` (`0x47D9DE..0x47D9F8`).
- It walks each footprint cell in row-major order. The yardmap index k counts from 0 into `[def+0x14E]`, with def = `unit+0x92`, read unconditionally (`0x47DA3D..0x47DA51`).
- For a cell where `yard[k] & mask`, the cell's slot-A word must be 0 or the unit's own `+0xA8` (`0x47DA5F..0x47DA72`); otherwise it returns 0. Cells are 13 bytes from `[main+0x14287]`.
- If every cell passes, it returns 1.

**`0x47DAC0(unit, open)`: YARD_OPEN.** stdcall, `ret 8`; its one caller is the COB setter at `0x480BAE`.

1. If the refusal answers 0, it does nothing (`0x47DAD3`).
2. `+0x10F` bit 2 = `open & 1` (`0x47DAD5..0x47DAF1`).
3. `+0x110 |= 0x8000000`, bit 27 (`0x47DAE6..0x47DAF7`).
4. `0x47C790(unit)` (`0x47DAFD`).
5. `0x440A40(dword +0x76, dword +0x7E)` (`0x47DB0A`).

**`0x47C790(unit)`: the re-claim.** stdcall, `ret 4`.

Its callers are `0x47DAFD` and `0x47ED35`. `0x47ED35` is in `0x47ED30`, the bucket-walk callback stored at `0x4FD660`. The clear `0x47D0E0` passes that callback to `0x47E5C0` at `0x47D2C4` (when the cleared unit had bit 26), and so does the saved game's `0x47DB20` (`0x47DB4B`).

It acts only when bit 27 is set, and clears it first (`0x47C7A0..0x47C7B9`).

**With bit 29 (the yardmap path):**

- It walks the footprint from `0x481550(X, Z)` with a row stride of W − fw cells.
- The mask is `+0x10F` bit 2 ? 2 : 4 (`0x47C804..0x47C814`).
- A named cell that is empty is taken (`0x47C8A2`).
- A named cell that is held is contested (`0x47C828..0x47C89C`):
  - An incumbent whose record has a set first dword and `+0x73` = 3 loses. The incumbent gets bit 27 and the unit bit 26, and the unit takes the cell.
  - Any other incumbent keeps the cell. The incumbent gets bit 26 and the unit bit 27.
- A cell the mask does not name, and the unit holds, is released (`0x47C8AE..0x47C8B7`).

**Without bit 29:**

- Every footprint cell: slot A when `+0x110 & 3` = 1 (`0x47C8F3..0x47C9DC`), slot B otherwise (`0x47C9E3..0x47CAB9`).
- The same contest applies.

**Also:**

- The re-claim has no map bound of its own (engine map).
- A named cell the unit already holds leaves it with bits 26 and 27 on either branch.

**`0x440A40(XZ, fwfh)`: the passability refresh.** stdcall, `ret 8`.

Its callers are the stamp (`0x47CEAD`), YARD_OPEN (`0x47DB0A`), the saved game's `0x47DB58`, and the features' `0x424031`/`0x424822`.

- It walks 32 contexts of `0x20` bytes at `0x512358..0x512758`.
- For each context whose first dword is set, it calls `0x440830(ctx, XZ, fwfh)` (`0x440A59`).
- That recomputes the context's 2-bit entries over the rect grown by one cell each way. The rect is clamped to the context's width `[ctx+0x10]` and height `[ctx+0x14]`. Values come from `0x47E1F0(ctx, col, row)` (`0x4408D5`) and go into `[ctx+0x18]`, 16 cells a dword.
- It is a cache over the grid, bounded by its own clamps.

[INFERRED: the movement classes' passability maps. `0x440420` finds a context by comparing `[ctx+0]` with a name through `0x4F8A70`.]

**`0x47DB20(unit)`: the saved game's restamp.** Its one caller is `0x487698`.

1. It sets bit 27 on the unit.
2. It re-claims every unit in the rect's buckets (`0x47E5C0` with the `0x47ED30` callback).
3. It calls `0x440A40`.

**The stamp's yardmap path** (`0x47CD5A..0x47CEB2`) uses the same mask from `+0x10F` bit 2. It also does two things YARD_OPEN does not do:

- It ORs 2 into `[cell+0xC]` for yard bytes with bit 0 (`0x47CE2D..0x47CE32`).
- It calls `0x483210` on the rect grown by one (`0x47CEA0`).

**Byte writers of `+0x10F`:**

- YARD_OPEN, `0x47DAF1` (bit 2);
- the COB setter, `0x480B7A`, `0x480B9E` and `0x480BD9` (bits 0, 1 and 3);
- `0x485B52` in `0x485A40` [INFERRED: the create's init];
- the saved-game restore, `0x4872F3..0x487346`.

**In play, bit 2 changes only through `0x47DAC0`.**

### 5.2 The recipe

The recipe runs for each live puppet unit whose recorded `+0x10F` bit 2 differs from its live bit. It runs at the boundary, in the applier's step at `0x4954ED`: after the position recipe of the same boundary, and before the reconcile's final write of `+0x110`.

1. **Bound, since the recording is data.** Nothing is written on a failure, which is logged by unit and field.
   - The unit is live and its id matches, as `cob_live_unit` checks.
   - `+0x92` is the engine's def for the unit's recorded type, and the type index is below UNITINFOCount.
   - `+0x110` bit 29 is set.
   - `def+0x14E` is non-NULL. The refusal reads it unconditionally, so a live YARD_OPEN implies it.
   - The unit's fw and fh equal its def's footprint, so the yardmap holds the fw·fh bytes the walk reads. The def's footprint offsets are to be named in the plan.
   - The footprint lies strictly inside the grid, as `0x47D970` tests it.
2. `+0x10F = (+0x10F & ~4) | (recorded & 4)`.
3. `+0x110 |= 0x8000000`.
4. `0x47C790(unit)`.
5. `0x440A40(*(u32*)(unit+0x76), *(u32*)(unit+0x7E))`.
6. The reconcile's write of `+0x110` from the recorded image comes after, for every unit. So bits 26 and 27 end as recorded, both this unit's and those of the incumbents the contest touched.

**The occupancy half of the refusal is not applied.** The owner passed it mid-tick, against a grid the host does not hold at the boundary. Refusing here would leave the unit's bit and the grid disagreeing.

**`+0x10F` bit 2 joins `+0x10E`** as a byte the reconcile never writes raw. The COB setter's own call at `0x480BAE` is inert on a puppet, under the one-writer rule. The harness asserts that no `0x47DAC0` comes from `0x480BAE` on a puppet.

### 5.3 What it cannot reproduce

The holder of a contested cell (§2.8).

**Why the order is "positions, then yard changes."** A worked case:

- **Live:** the owner closes its yard at tick T (mask 4). A unit then steps onto a newly held cell during T. The building, being local, keeps the cell. The mover gets bit 27 and the building bit 26.
- **On the host, positions first:** the mover's stamp takes the still-free cell. The re-claim then contests a type-3 incumbent and takes the cell. The holder and the bits are the same as live.
- **Reversed:** the mover's stamp would take the cell from the type-3 building.

The general case needs the grid recorded (S3).

---

## 6. Task 5: opening a recording

### 6.1 The engine's entry: the SKIRMISH Start branch

The engine map's *Seating a level* section describes the branch. It was confirmed here.

**The Start branch** (`0x47AED2..0x47B0BC`, game thread) runs, in order:

1. `+0x2A3C` = the AI entries + 1.
2. The map's `.ota` through `0x435A20`, named by `[main+0x29A0]+0x11C` (`0x47AF77`).
3. Its checks:
   - at least one AI and one human entry;
   - the map's player limit `0x437300`;
   - `0x479760`.
4. `+0x2A3C` recounted.
5. **The seat fill `0x47A760`** (`0x47B092`).
6. `0x41DA30`.
7. **`0x430F00`** (`0x47B09C`).
8. `main+0x2BC0` = 2 and `0x491C80(0x14)`.

**`0x430F00` is the settings persist** [NEW role]. It writes every setting to the registry (`0x430F12..0x43172F`), including:

- `SkirmishCommanderDeath` (`+0x108`), `SkirmishMapping` (`+0x10C`), `SkirmishLineOfSight` (`+0x110`), `SkirmishLOSType` (`+0x114`) and `SkirmishLocation` (`+0x118`);
- `SkirmishDifficulty` and `SkirmishMap`;
- `Skirmish\Player%d{Controller,Side,Color,AllyGroup,Metal,Energy}`.

Anything written into the settings before it is persisted into the player's registry.

**The loader** (`0x497180`, loader thread) then reads the settings:

- `+0x37EE6 = +0x37EEC` (`0x4973C1..0x4973CD`);
- ActiveCommanderDeath, and LosType bits 0, 1 and 2 from `+0x10C`, `+0x110` and `+0x114` (`0x4973E0..0x497450`);
- `+0x118` (`0x49790E`).

It then:

- creates the start units, `0x496EE0(seat, start)` at `0x497963` and `0x497ACD`. These also rewrite PlayerInfo `+0x95`/`+0x96` from the entry;
- runs `0x465E30`;
- runs `0x4816A0(1)` (`0x497AE5`).

**The level init `0x464990`** (from `0x4919C8`) allocates each seat's sight grid (`0x464700`) and runs the AI script (`0x4648E0`).

**Why this entry.** The Start branch loads exactly a skirmish from settings the host can set. The puppet changes sit at four call sites around it. The alternatives:

- **The saved-game load** needs a save file, which the recording is not, and its restore has known gaps (engine map).
- **The restart** `0x4997A0` re-seats from the same settings.
- **The network path** makes a game of type 3.

### 6.2 How a skirmish is started today

**tacli `scenario load`:**

1. Writes the instance's registry store: `SkirmishMap`, and `Skirmish\PlayerN{Controller,Side,Color,Metal,Energy}`. `AllyGroup` is not exposed.
2. Clicks SINGLE → Skirmish → Start through `tacli ui` gadgets.
3. Sets Mapping and LineOfSight by gadget stage. The registry does not decide them (MEASURED 2026-09-02, per tacli).
4. Waits for `units: alive=N`.
5. Confirms the loaded TNT from GameingState `+0x204`.

**`tagpu_scenario.c` starts nothing.** It applies a compiled scenario into a running level: the trigger is read on the present path, and the apply runs at the tick site `0x4969D2`.

**Experiment 1's probe** patched the Start's call sites at run time from the present path. That is not a design to keep.

### 6.3 The request

**The file.** `tagpu_replay.play` (`SIM_TOOL_FILE`) holds one line, `play <name>`. The name follows `dump`'s rule (letters, digits, `-` and `_`) and names `replays\<name>.imprec`.

**One reader, one time.** The seat-fill stub at `0x47B092` reads it, on the game thread, in the Start handler, before the loader thread exists. It is consumed at read whatever the outcome.

**Why not a verb in `tagpu_replay.trigger`.** That file's reader is the recorder's `dump_poll`. It runs every 32 captures during a level, deletes the file every time, and refuses every verb but `dump`. Two readers of one file at two times race by design.

**Why not a lever.** Levers are read while Impure installs, and one read after install is logged by name (`tagpu_simreg.h`). A lever is constant for every game and is listed in every recording's header. A replay is chosen per level.

**A stale request** (for example, from a crashed tacli run) makes the next Start in that folder a replay. tacli removes the file on its own failure paths. The instance folders are tacli's, not the owner's install.

### 6.4 The sites

These are installed at DLL install like the other simulation sites. They fail closed, and are inert unless a request armed the level.

**S-a: `0x47B092`**, the E8 to `0x47A760`, bytes `E8 C9 F6 FF FF`. The stock fill always runs first. Then, if a request is present, validate, in this order, before any write:

1. The name, and that the file opens.
2. The header parses within bounds (every TLV length inside the block), with:
   - the format equal to this build's;
   - `RT_LAYOUT` equal to this build's `TAGPU_REPLAY_LAYOUT_SHA256`;
   - `RT_MODE` equal to skirmish.
3. `main+0x2A44` bit 0 clear, and `0x435100` = 2.
4. The map. The Start has just read the `.ota` named by `[main+0x29A0]+0x11C` (`0x47AF64..0x47AF77`), before this site, so the map cannot be changed here. That name must equal the header's settings map name (§6.6 item 5).
   - For a recording without that field, compare it with `RT_MAP` instead [INFERRED: equal for maps whose TNT carries the map's name].
   - Where GameingState `+0x204` is written was not found in the `.ota` reader `0x435DA0`, so the TNT-stem check waits for S-d.
5. The seats:
   - every index below 10, and distinct;
   - every controller 1 or 2;
   - at least two;
   - no more than nine, so that V exists (D2).
6. The settings count `main+0x38D81` bounded to 10 before it is trusted (§1.3).
7. The unit defs' count and hash against `RT_UNITDEFS`, if UNITINFO is loaded by now. Otherwise this check waits for S-d (§9).
8. The build: per D8.

**On any failure:** log `replay: not playing <name>: <reason>`. A normal skirmish starts, and tacli reports it.

**On success, seat the level:**

1. Each seat s: `0x464290(s, 3)`. Then:
   - PlayerInfo `+0x95`/`+0x96` = the header's side and logo;
   - the name into `+0x2B` and `+0x49`, bounded to `0x1E`;
   - the header's alliance row into `+0x108..+0x112` (§6.6), because `0x464290` zeroes it.
2. V: `0x464290(V, 3)`, then PlayerInfo `+0x9B |= 0x40`.
3. Any other record the stock fill activated as 1 or 2: `0x464290(r, 0)`, as the fill does for an off entry.
4. `+0x2A42` = `+0x2A43` = V.
5. `+0x2A3C` = the header's player count.
6. Disarm: `0x4904B0` on `[main+0x391ED]`. The `.ota` read at `0x47AF77` armed it, and a skirmish's loader does not re-read it [DISASSEMBLED + MEASURED, engine map].
7. `seat[]`, `local[]` and puppet mode on.

**S-b: `0x47B09C`**, the E8 to `0x430F00`. Call the persist first. Then, on a puppet level:

- save the settings fields and write the recording's options:
  - LosType bits into `+0x10C`, `+0x110` and `+0x114`;
  - ActiveCommanderDeath into `+0x108`;
  - `+0x118`;
  - the difficulty `main+0x37EEE`;
- write the units each into `+0x37EEC`, which the loader copies into `+0x37EE6`, bounded by this build's own unit limit.

Everything written here is restored at the teardown. So the player's registry and SKIRMISH screen keep their settings.

**D6** chooses between this and verifying only: tacli sets the options by gadget, and the stub refuses a mismatch.

**S-c: `0x497963` and `0x497ACD`**, the E8s to `0x496EE0` (stdcall, `ret 8`). On a puppet level, skip the call for every record. That means:

- no start units;
- PlayerInfo `+0x95`/`+0x96` keep the header's values;
- the random branch has already taken its position (`0x497AC6..0x497AC8`).

The stub pops the two arguments.

**S-d: the applier's step at `0x4954ED`** (B2's stub; the capture is inert on a puppet level).

On the first step:

1. The checks that need the loaded level:
   - `+0x37EE6` = `RT_UNITS_EACH`;
   - u16 `main+0x14351` = `RT_SLOTS`;
   - the TNT stem at GameingState `+0x204` = `RT_MAP`, computed by the same code the recorder used (`map_stem`);
   - the unit defs, if not already checked at S-a.

   On a mismatch, nothing is applied, the reason is logged, and the level stays empty. That is a visible failure.
2. F3 (§2.9).
3. The first apply.

On every step: stamp `+0x18` = GameTime for each puppet seat after the apply. The pacing gate counts only seats with live units, and those exist only after the applier's creates. The probe stamped at the tick site `0x4969D2` every frame, which also works.

**S-e: the level's teardown** (the wrap the recorder uses). Puppet mode off, the tables cleared, and the settings fields restored.

A load that fails before a level exists must also end the mode. The plan names the hook (§9).

**S-f: the recorder.** It refuses to start on a puppet level.

**Also:** the sink gate (§2.6), the meteor shower skip, and the four input gates (§3.2).

### 6.5 `tacli replay play <instance> <recording>`

1. Read the header with the tool that mirrors `tagpu_recfmt.h`.
2. Refuse ten seats (D2).
3. Write the instance's registry store:
   - `SkirmishMap` = the settings' map name (§6.6);
   - `NumSkirmishPlayers` = the highest seat + 1, at most 10;
   - for each seat: `PlayerNController` = the recorded controller, and `Side` and `Color`;
   - every other entry: `Controller` 0.

   The Start's own checks (a human, an AI, the map's limit, the alliance groups) passed when the game was recorded.
4. Copy the recording into the game folder's `replays\`, and write `tagpu_replay.play`.
5. Launch, then click SINGLE → Skirmish → Start (the `scenario load` path).
6. Wait for `replay: playing <name>` or `replay: not playing <name>: <reason>`. Not `units: alive`: a puppet host has no unit until the first apply.

### 6.6 What the header must carry beyond S1's

**1. The game options as the simulation ran them:**

- the LosType word's bits 0–2 (`main+0x14281`);
- ActiveCommanderDeath `main+0x37EF6`;
- the difficulty `main+0x37EEE`;
- the start-position mode, settings `+0x118`.

S1's `RT_UNITS_EACH` is `+0x37EE6`. Record the derived main-block values, not the menu's, because they are what ran. S3 will record the main block too.

**2. Each seat's PlayerInfo**, the `0xB9`-byte block at record `+0x27`:

- `+0x94` type, `+0x95` side, `+0x96` logo;
- `+0x97` share bits: the radar pass reads ShareRadar, and the economy at a takeover reads them too;
- `+0x99`;
- `+0x9B` flags, with the watcher bit;
- `+0x9C` LOS bits;
- `+0xA5`.

The console's `Share*`/`SetShare*` (level 1) change it in play, so S2 should record PlayerInfo as a pointed-to object with rows of its own (the owner's rows), with the header carrying its start image.

**3. The seats' alliance rows `+0x108..+0x112` at the start**, so the seat fill restores what `0x464290` zeroes. These are values of the records; the header copy makes the seating exact before the first apply.

**4. The recording viewer:** `+0x2A42` and `+0x2A43` at the start, and their changes (`View` and `Control` on the recording machine). It is the default perspective. The contact bits in the stream follow `+0x2A43` at each radar pass.

**5. The settings' map name** `[main+0x29A0]+0x11C`: what `SkirmishMap` must hold. `RT_MAP` is the TNT stem, which a map can name differently.

**6. The player count `+0x2A3C`.** It can be derived in a skirmish; carry it exactly.

**7. A weapon-definition fingerprint.** This is the known gap.

**8. Controller changes** as a record kind. This belongs to the stream, not the header. It is the approved decision, and S1 has only the header's controller.

**Not needed:**

- starting metal and energy, and `AllyGroup`: values of the records, which the first apply writes;
- the seat count `+0x38D81`.

---

## 7. New facts and corrections for the notes

1. **The AI toggle is `0x416280`, not `0x4161F0`** (`demo-recorder-completeness.md`, "the console's AI toggle `0x4161F0`").
   - `0x4161F0` is a network-statistics helper called by `NetStats` (`0x417570`).
   - `0x416280` is `AI`, level 4, table row `0x501FD0`. It calls `0x463C60(2)` when the record's type is 1 and `0x463C60(1)` otherwise, so a type-3 record becomes 1 (`0x4162CF..0x4162FE`).
2. **The receive timeout is unreachable in a skirmish.** This closes the completeness note's open question (§2.5).
3. **The skirmish seat count is 3–10** (§1.3). The count is registry input read without a bound. A recording can seat all ten records.
4. **`0x430F00` is the settings persist** (§6.1). Its role is not stated in the engine map.
5. **YARD_OPEN, the refusal, the re-claim and the passability refresh** (§5.1). Open item 2 can be closed with §5.2's recipe.
6. **The typed-command entry is `0x417B50`.** The executor `0x4B7900` also runs the level's AI script (§3.2).
7. **The save writer is `0x4326B0`.** ARMOPT's save and load buttons are disabled only for game type 3 (§3.2).
8. **The complete writer list of `+0x2A42` and `+0x2A43`** (§3.4).
9. **The hit sender applies every computed hit locally before forwarding it** (`0x489C89`). The destructor's cargo loop deals 30 000 to each carried unit (§2.2, rows 9 and 11).
10. **The footprint contest inverts on a puppet host** (§2.8).
11. **The watcher bit's non-network effects**, including the refusal of the game-speed keys (§2.9).
12. **The level init `0x464990`** allocates sight grids and runs the AI script (`0x4648E0`).
13. **The console table** (`0x501D38..0x502138`), with levels 1, 2 and 4 (§3.2).

---

## 8. The decisions

Taken 2026-09-30: D1 by the owner as recommended (the slice note's *Decided 2026-09-30*); D2 to D8 as recommended, in the slice note's *S2b*.


| | Decision | Taken |
|---|---|---|
| **D1** | The contact bits on a puppet host | Display state: the radar pass computes them for the chosen perspective; the applier holds the recorded bits for takeover; the comparison masks them. This amends 2026-09-29's "a replay shows the recording viewer's radar picture": that perspective still shows it, computed by the engine. |
| **D2** | Ten-seat recordings | S2 refuses them by name; the multiplayer slice designs the full house. |
| **D3** | The puppet table's class | Rebuilt from the header, never recorded or restored (`SIM_CONTENT`'s shape), in a new `sim` puppet module; the recorder stays `presentation`. |
| **D4** | Typed commands on a puppet host | Refuse all, at `0x417B50`. No allow-list for display commands. |
| **D5** | The fire spread and the meteors | The sink gate at `0x489BB0` makes both harmless to puppets. Skip the shower, whose stones come from the recording as events. Let features burn by the host's RNG until S3 records features: a visible difference named as a gap. |
| **D6** | Game options | The DLL applies the header's options after the persist and restores them at the teardown, rather than tacli setting them by gadget. |
| **D7** | Playback speed | V is a watcher, so the engine refuses the speed keys. Playback speed is Impure's own control. |
| **D8** | A recording from another build | Refuse on a layout or data-fingerprint mismatch. Log a warning on a build mismatch alone, because a puppet host computes nothing a build changes, but a takeover would. |

---

## 9. Not established, and open

- **When UNITINFO is loaded**, before or after the Start's seat fill. This decides where the unit-defs check goes.
- **Where GameingState `+0x204` (the TNT path) is written.** It is not in the `.ota` reader `0x435DA0`, so the TNT-stem check sits at S-d.
- **Readers of `+0x2A3C`** beyond the radar pass's "fewer than 2". Also `0x479760`'s test (the engine map: role not traced).
- **The draw `0x46A16B`**: what a watcher skips; the resource bar is [INFERRED]. Also which HUD elements read `+0x2A42` and would show V's zero resources.
- **The explored map** is monotonic (set only) [INFERRED].
- **The death route** (§2.7) belongs to the events research. So does whether recorded hits are replayed at all.
- **The cargo loop's order** (§2.2 row 9) is shared with the carried-units research. It is not established whether a mod's unit survives 30 000 damage.
- **The grid's contested slots** (§2.8, §5.3): S3.
- **Features burning on the host** diverge until S3 (D5).
- **Cloaked units** stay hidden under full visibility. Showing them would need a presentation patch of the sight test for V.
- **A level load that fails after the seat fill** must also clear puppet mode. The hook is not identified.
- **The key sequence** that triggers the SKIRMISH seat-count cheat `0x47B9F0` was not traced.
- **The def's footprint field offsets**, for the YARD_OPEN bound, are to be named in the plan.
- **Stores through computed addresses** are not excluded from §3.4's writer list.
