# The map: terrain, features, footprints, fog and sight

The terrain pass, features and wrecks, the wind, the grid stamp and footprint contest, the fog grids and line of sight.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### Surface metal and the mex snap threshold

**SOURCE + DISASSEMBLED 2026-10-04.** Read for the
[resource-snapping design](../tadr-port/resource-snapping.md#metal-maps). The preferred image is
`pristine/TotalA.exe.pristine`, inspected with `i686-w64-mingw32-objdump -d -M intel` and a
section dump for the property name. These are stock reads/initialization, not Impure writes.

TADR's `v2026.6.8` `tamem.h` names the game-state dword at `+0xD30` `surfaceMetal` and the byte
at `FeatureStruct+7` `MetalValue`; the packed feature-grid record is 13 bytes. Its mex scorer
counts only footprint cells with `MetalValue > surfaceMetal`, rather than summing values. The
scorer and square-search selector are identical after whitespace normalization in `v2024.3.31`,
`v2026.6.8` and `v2026.8.6`. SOURCE.

| Address | Established fact |
|---|---|
| `0x4366B3..0x4366DB` | Pushes the property string at `0x504B24` (`SurfaceMetal`), calls the integer reader `0x4C46C0` at `0x4366BE`, and stores its result at game-state `+0xD30` at `0x4366DB`. DISASSEMBLED + string dump. |
| `0x4839B3..0x4839ED` | Loads the game-state pointer from main `+0x391E9` and the background dword from `+0xD30`. A negative value, or the header value held in `[esp+0x18]` below `0x2000`, selects zero; otherwise the dword's low byte initializes each grid record at `+7` (`0x4839E6`). The loop strides 13 bytes and counts the product of the two fields read at `[ebp+0x38]` and `[ebp+0x3C]` (`0x483986..0x48398D`). DISASSEMBLED. The header field's meaning is not classified by this inspection. |
| `0x435E09` | The game-state initialization stores `0xFFFFFFFF` at `+0xD30`. DISASSEMBLED. A cold-state value is not a loaded map's scoring threshold. |
| `0x40A5D0..0x40A602` | This function's initial arithmetic multiplies the background dword by definition footprint words `+0x14C` and `+0x14A`, doubles the product and stores it in a stack local. DISASSEMBLED. Its full role and callers were not traced here; this prefix is not evidence for a complete extractor-income formula. |
| `0x40C160..0x40C16C` | Another read of game-state `+0xD30`, compared against EAX by `cmp`/`jge`. DISASSEMBLED. The surrounding function's role and callers remain unclassified. |

The initialization establishes the baseline/grid relationship used by TADR's threshold test,
subject to the header condition above. Uniform cells equal to the baseline produce no snap
candidates; a uniform above-baseline interior produces equal-coverage candidates, with the
selector preferring pointer distance. These are source/disassembly deductions. Live metal-map
placement, feature-written metal values and the complete extractor-yield path were not verified
by this inspection.

### The demo recorder's features: the grid, the wreck pool and the calls that change them [DISASSEMBLED + MEASURED 2026-09-30]

S3a of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s3a-features-and-wrecks)
records the feature grid and the wreck pool every iteration, and each call into the feature code
that no other call contains, so that a replay can make the same call to the same end
(`tagpu_recorder.c`, *The features*; `tagpu_replay.c`, the feature section). This is what that
rests on. [The wreck pool](#the-wreck-pool-and-a-feature-paid-for-and-left-standing) has the pool's
functions and their behaviour on a full pool.

**The cell**, 13 bytes, row-major from `[main+0x14287]`, `W = [main+0x14233]` columns by
`H = [main+0x14237]` rows (`0x481550`):

| Offset | What |
|---|---|
| `+0x00`, `+0x02` | slot A and slot B, unit indices: the stamp `0x47CC30` files a unit by `+0x110 & 3`, 1 in A and 2 (airborne) in B, and a building's yard cells in A |
| `+0x04`..`+0x07` | the height, the 2×2 maximum and minimum, the metal |
| `+0x08` | the feature def: `0xFFFF` none, `0xFFFE` a footprint cell, `0xFFFD` the border, `0xFFFC` void. Every feature function treats a def from `0xFFFB` up as no feature (`0x4240ED`, `0x4244D3`, `0x4246EC`) |
| `+0x0A`, `+0x0B` | a footprint cell's rows and columns back to its anchor: the anchor is the cell less `(rows × W + columns)` cells (the destroy, `0x4246C6..0x4246E6`). An anchor whose flags have bit 0 set: its record's index. A GAF anchor without one: the damage it has taken, a `u16` (the damage, `0x42463F..0x424666`) |
| `+0x0C` | flags: bit 0 the anchor holds a record; bit 1 the yard mark, set by the stamp (`0x47CE32`) and cleared by the clear (`0x47D17C`); bit 2 the draw's, cleared and set again on every cell it draws, every frame (`0x4698A4..0x4698B0`, `0x469936`); bits 3–6 the feature's owner, 10 for none (the spawn, `0x423EF1..0x423F07`) |

**The pool's start.** The level load `0x421F20` allocates the pool (`0x18000` bytes in stock,
`0x421F29`), zeroes it, links every record to its neighbours, next `i + 1` and prev `i − 1`, with
−1 at both ends (`0x421F5F..0x421F94`), and sets the active and settled heads to −1 and the free
head to 0 (`0x421F54..0x421F5A`).

**The feature tick `0x424050`**, one caller (`0x495585`), after the unit tick, the projectiles,
the explosions and the players phase, in this order:

1. **The idle states.** For each def whose byte at `+0xFE` has bit 1 set, it steps the two
   sequence states at `+0xCC` and `+0xD8` with `0x4B8B90` (`0x424074..0x4240A1`). Only the draw
   reads them [INFERRED: no simulation read of `+0xCC..+0xE3` was found].
2. **The cursor** `main+0x14257`, a cell index, is decremented (stored at `0x4240AA`); when it
   goes below 0 it is set to `W × H − 1` (stored at `0x4240CC`) and reproduction is skipped for
   that tick (`0x4240A3..0x4240D2`, a jump to the list walk at `0x4241A8`). Otherwise the cursor's
   cell address is kept at `[esp+0x10]` (`0x4240E5`) for the source test below.
3. **Reproduction.** When the cursor's cell holds a def below `0xFFFB` whose anchor holds no record
   (a GAF feature at rest), it draws `0x4B6C30(100)` (`0x424113`), and when the draw is below the
   def's `+0xFC` it picks a cell near the cursor: the column is `cursor % W` and the row is
   `cursor / H` (`0x424137`, `0x424142`) — **the row divides by the height, not the width**, so on
   a map that is not square it picks a cell in the wrong row. Each coordinate then moves by
   `0x4B6C30(n) − n/2`, `n` the def's `+0xFD` (`0x424159`, `0x424167`). The spawn
   `0x423C50(cell, def, 0, 0, 10)` (`0x4241A3`) runs when that cell is on the map, the cursor's
   cell's slot A is 0 (`0x424189`) and the chosen cell's def is `0xFFFF` (`0x42418F`; nothing else
   about it is tested — not its units, height or water). The spawn takes the cursor cell's own def
   (`0x424197`) and owner `0xA`, and sends no network message: every peer reproduces from its own
   grid. `0x4B6C30(n)` returns 0 for `n` < 2 without stepping the generator, so `+0xFD` of 0 or 1
   makes no area draw. No stock feature has a nonzero `+0xFC`, so none reproduces, but the first
   draw is made for every GAF feature at rest the cursor passes; mods set it (TA Zero and TA
   Twilight use 1–2). The demo recorder replaces the 10-byte source test `0x424185..0x42418E`
   (`8B 4C 24 10 66 83 39 00 75 19`, the slot pointer's load, the compare at `0x424189` and its
   `jne 0x4241A8` at `0x42418D`)
   with a stub that asks `feat_source` (`tagpu_recorder.c`), which falls through to `0x42418F` or
   jumps to `0x4241A8`: a recording records the slot the recorder's tick read there, and a
   playing whose host keeps its own slots answers from it -- a multiplayer one, whose slots follow
   the owners' units, and a skirmish one whose stream holds its cells (format 16's `RECL_CELLS`).
   DISASSEMBLED.
   The keys: LoadFeature `0x4224B0` reads `reproduce` into the byte `+0xFC` (`0x422A13`,
   `0x422A1D`) and `reproducearea` into `+0xFD` (`0x422A27`, `0x422A31`), each with `atol`,
   truncated to a byte, not clamped, 0 when absent. LoadFeature takes a section from the first
   feature TDF enumerated that holds it, and the enumeration (`0x4BCB50("features", "*.tdf", −1,
   1)`, recursive) lists loose files under the game folder first, then the archives in mount order
   — `rev31.gp3`, `*.ccx`, `*.ufo`, `*.hpi` — so a `.ufo` takes over a name defined only in
   `totala1.hpi` (`tools/reproduce_fixture.py` relies on that). The index LoadFeature searches,
   `[0x511FB4]`, lives only during the level load: built by `0x4222E0` (`0x4918A2`), freed and
   zeroed by `0x4223E0` (`0x4918DE`; the store at `0x422452`), so LoadFeature called in a live
   game reads through NULL at `0x4224BB`. The cursor is set to 0 at every
   level load by LoadMap's last store (`0x483DAC`, `mov [ebp+0x5C],ebx` with `ebp` =
   `main+0x141FB`) and at process start (`0x41D956`): a level's first tick wraps it and examines
   no cell, tick k examines cell `W × H − k`, and cell `W × H − 1` is never examined. DISASSEMBLED
   (the enumeration and the cursor's start by a scouting pass, 2026-10-06).
4. **The active list**, every tick. A 3DO record (`+0xFE` bit 0 clear) with a velocity adds it to
   its position (`0x424230..0x424250`); then on the ground (`0x485140`) it stops there, velocity
   zeroed; below the sea level `main+0x1427F` it sinks, `+0x14` 0, `+0x18` `−0x2CCC`, `+0x1C` 0;
   otherwise it falls by the gravity `main+0x14263` (`0x424253..0x4242B0`). One without a velocity
   moves to the settled list (`0x4242B8..0x4242C0`). A GAF record, while burning (`+0x2F` bit 0) and
   on every third tick (`GameTime % 3`, `0x4241AE..0x4241CB`), makes its smoke, which draws the
   CRT's `rand` (`0x4E4870`, `0x424345`, `0x424379`) and calls the emitter `0x472810`
   (`0x4243CF`) — presentation; its sequences step (`0x4B8B90`: `0x4243DC`, `0x4243EB`,
   `0x42446E`, `0x42447D`); a burn ends with the destroy `0x42442F` and the burnt feature's spawn
   `0x424443`, the spread `0x4239C0` runs when the countdown `+0x2E` runs out (`0x424463`), and the
   swap `0x423710` when a death or reclaim sequence has ended (`0x424495`).

**The simulation RNG in the feature code** — the six calls of `0x4B6C30(n)` the recorder takes
(`RK_EV_DRAW`, the site's number in brackets):

| Call | Where | What it decides |
|---|---|---|
| `0x4234C2` (0) | the burn `0x4233A0` | the countdown `+0x2E`: `draw(FeatureDef +0xE8 / 2) + FeatureDef +0xE8 / 2` (`0x4234B8..0x4234DE`) |
| `0x423A38` (1) | the spread `0x4239C0`, the neighbours | `draw(100)` below the neighbour's `+0xFB`, when its `+0xFE` bit 4 is set: `0x4233A0(x, z, 0)` (`0x423A4D`) |
| `0x423B40` (2) | the spread, down the wind | the same test (`0x423B55`) |
| `0x424113` (3) | the tick, reproduction | `draw(100)` below `+0xFC` |
| `0x424159`, `0x424167` (4, 5) | the tick, reproduction | the column's and the row's offsets |

**The damage `0x4244B0(cell, x, z, weapon)`**, `ret 0x10`, callers the area damage `0x49A120`
(`0x49A626`) and the network handler (`0x455496`):

- it returns at once unless bit 3 of `SoftwareDebugMode` (`main+0x37F2F`) is **set**
  (`0x4244C2`), and that bit is set in a normal skirmish (MEASURED 2026-09-30: `0x000C`). So the
  bit the cheat table calls `invulnfeatures` makes features *vulnerable* when set;
- it skips a def whose word at `+0xFE` has bit 9 set (`0x4244F9`), the indestructible bit the
  destroy tests as `+0xFF` bit 1 (`0x424715`);
- in a network game (`0x435100` = 3), a player without `+0x97` bit 0 sends a `0x0F` with the
  weapon's ID byte to the host and returns (`0x424515..0x424581`);
- a flamable def (`+0xFE` bit 4) hit by a firestarter weapon (`+0x10B` nonzero) and holding no
  record burns: `0x4233A0(x, z, 0)` (`0x4245C6`);
- a 3DO feature's record takes the weapon's `+0xD4` damage into `+0x26` when the record's anchor
  is `(x, z)` (`0x4245F2..0x424613`); a GAF feature holding a record takes none (`0x4245D8`); a
  GAF feature at rest adds it to the cell's `+0x0A` (`0x424666`); either dies at the def's `+0xEA`:
  `FeatureDie(x, z, 0)` (`0x424628`, `0x42465A`).

It uses its `x` and `z` arguments for the burn and FeatureDie, and FeatureDie never checks that
`0x481550` found a cell (`0x423568..0x42358D`), so the cell and the two coordinates must name the
same place. A replay bounds all three.

**What each returns** (the recorder's `RK_EV_FEATURE_END`): the spawn a record's address, or 0;
the swap its spawn's (`0x4237BE`), 0 for no cell, and for a cell holding no feature the cell's
address with the def in its low half (`0x423734` → `0x4237C3`), which is no result; the destroy 0 when the cell is no feature after its footprint is followed back
(`0x4246F2`) or the def is indestructible and `force` is 0 (`0x42471E`), and 1 when it removed
the feature (`0x42482A`); FeatureDie, the burn and the damage end in their network sends (`0x451DF0`,
`0x451BC0`), so what they leave in `eax` is not a result.

**Every caller** of the seven functions (MEASURED over `.text`, `call` and `jmp`, and over the
image for a stored address):

| Function | Called from |
|---|---|
| the tick `0x424050` | `0x495585` |
| the spawn `0x423C50` | the console's feature command `0x4163D0` (`0x416412`, stored at file offset `0x100724`); the schema `0x423254`; the swap (`0x4237A3`, `0x4237BE`); the tick (`0x4241A3`, `0x424443`); the saved game's load `0x424C00` (`0x424FBF`, `0x425050`, `0x425180`); LoadMap (`0x483A7E`, `0x483ADE`, `0x483B38`); the corpse `0x486360` (`0x4863F9`, `0x486434`), whose one caller is the destructor (`0x486D6F`) |
| the destroy, `FEATURES_Destroy` | [its entry](memory.md#0x4246b0-features_destroy-the-wreck-path) lists them: the resurrect, BurnOne, the swap, the spawn's footprint, the tick, and BurnAll's clear of every feature |
| FeatureDie `0x423550` | the reclaim (`0x423965`); the damage (`0x424628`, `0x42465A`); the saved game's load (`0x42507E`, `0x42509D`); the network handler (`0x4554B0`, `0x4554E4`) |
| the burn `0x4233A0` | the spread (`0x423A4D`, `0x423B55`); the damage (`0x4245C6`); the saved game's load (`0x4250BB`); the network handler (`0x4554CA`) |
| the damage `0x4244B0` | the network handler (`0x455496`); the area damage (`0x49A626`) |
| the swap `0x423710` | FeatureDie (`0x4236F2`); the tick (`0x424495`) |

`0x423BF0(record)`, which destroys a record's anchor and spawns the def's `+0xF6` there, has no
caller and no stored address anywhere in the image.

**A unit's death and the feature code.** The destructor `0x4866D0` explodes the unit
(`0x49B000(unit, selfd)`, `ret 8`, at `0x486D50`; it builds the blast on its stack from the unit's
ExplodeAs `+0x220` or SelfDestructAs `+0x224` and calls `0x499EB0`), and only then spawns its corpse
(`0x486360` at `0x486D6F`); nothing between the two touches a feature (`0x486D55..0x486D6E` reads
the death record). The explosion's damage is computed only on the owner's peer: the owner gate
`0x49A01B..0x49A047` passes by an explosion whose owner's player record has `+0x73` = 3, which
then is a sound and a picture; otherwise the area damage `0x49A120` damages the features in the
blast at `0x49A626`. MEASURED 2026-10-05 on a native four-actor game recorded by one host: the
recorder's 7 deaths of its own units held 21 feature roots from `0x49A626` inside 6 of their
brackets, and the 17 deaths of the other peer's units none. The corpse is made on every peer. `0x49B000`'s other caller is kill-all
`0x486F10` at `0x486F9E`, outside the destructor.
The corpse helper `0x486360(unit, level, smoke)` is stdcall, `ret 0xC` (`0x486454`); the
destructor calls it only when the death record's corpse level, `+0xA`'s low nibble, is nonzero
(`0x486D55..0x486D6F`). It reads the unit's type `+0x92` (`0x486368`, `0x486404`), the cell
`+0x76`/`+0x78` (`0x4863BB`, `0x4863B7`), the position `+0x6A` (`0x4863D0`), the turn `+0x64`
(`0x4863F1`) and the owner `+0xFF` (`0x4863EB`, `0x486426`), and spawns through `0x423C50`
(`0x4863F9`, `0x486434`). A multiplayer demo recording carries that call as the root `FR_CORPSE`
with exactly those fields (`tagpu_recorder.c`, `root_args`), and a playing makes it through
`0x486360` on a stack record holding them, while the host's own destructor runs with the nibble
cleared (`tagpu_replay.c`, THE DEATHS ON A MULTIPLAYER LEVEL). DISASSEMBLED. Deaths do not nest: the cargo loop's hit
(`0x48680B`, `0x489BB0` → `0x489CE0`) marks a passenger dying (`+0x110` bit 14, `0x489EE2`), and
the unit tick's reaper destroys it later (`0x48AFD1`); no feature function and nothing the
destructor calls calls the destructor (its callers are `0x486679`, Send_UnitDeath, and the `0x0C`
case `0x455423`; Send_UnitDeath's are `0x4859A8`, `0x486244`, the console's kill of every unit of
a type `0x486E80` (`0x486EB2`, one caller `0x4174B8`) and of every unit `0x486ED0` (`0x486EF8`, one
caller `0x4164BD`), kill-all `0x486F10` (`0x486FB4`) and the reaper).

**What a playing's corpse call rests on** [DISASSEMBLED 2026-10-06] (`tagpu_replay.c`,
`FR_CORPSE` and `corpse_def_ok`; the root is recorded by an observer on `0x486360`'s entry).
- **The walk to the corpse def, `0x48636E..0x4863B1`.** The helper takes the unit def's `u16
  +0x1BC` (`0x48636E`), then, `level − 1` times while the def is below `0xFFFB` (`0x486384`), the
  FeatureDef's `u16 +0xF4` (`0x48639F`; FeatureDefs are `0x100` bytes apart from
  `[main+0x1426F]`), and spawns only when the last def is below `0xFFFB` (`0x4863B1`). The spawn
  tests a def only against `0xFFFF` and `0xFFFC` (`0x423C5A`, `0x423C76`) and then indexes
  FeatureDef by it (`0x423CD2`, `add esi,ebx` after `shl esi,8`), so every def the walk reads
  through, and the last, must name a feature def: the replay bounds them by the level's def count.
- **No callee keeps a pointer into the unit.** The position goes by address (`lea ebp,[edi+0x6A]`
  at `0x4863D0`), the turn too (`0x4863F1`, and `add edi,0x64` at `0x48642C` on the other branch).
  The spawn copies the position's 12 bytes into the record's `+0x08` (`0x423E28..0x423E38`) and
  the turn's 6 bytes into `+0x20` (`0x423E9B..0x423EA6`), and the owner byte reaches only the
  anchor cell's flag bits 3–6 (`and al,0xF; shl al,3` at `0x423EFB`). The spawn's smoke
  `0x472C50` (called at `0x423FE3`) builds an emitter of class `0x4FD638` through `0x4750B0`
  (`0x472C8E`) and calls its slot `+0x18`, `0x475150`, which copies the 12-byte point into the
  emitter's `+0x28` (`0x47515D..0x475178`). The helper's flare `0x472630` (`0x48644B`, with
  `(pos, 0xF, 0x384, 9)` when `smoke` is set, skipped after an underwater spawn that made a
  record, `0x486420`) builds class `0x4FD618`
  through `0x474CD0` and calls its slot `+0x18`, `0x474D50`, which copies the point into
  `+0x2C` (`0x474D5D..0x474D7C`). So a playing can pass the helper a record on its stack.
- **The two lookups bound themselves.** The cell lookup `0x481550(x, z)`, `ret 8`, returns NULL
  unless `0 ≤ x < W` and `0 ≤ z < H`, signed (`0x481551..0x481593`). The height `0x485070(pos)`,
  `ret 4`, takes the cell of the position's integer halves `>> 4` and returns −1 (`0x485136`)
  unless that cell and its +1 neighbours lie on the grid (`0x48508F..0x4850C4`).
- **The destructor's arguments.** It passes `level` as the death record's `+0xA` low nibble, so
  1..15, and `smoke` as 1 unless the high nibble is 7 (`0x486D55..0x486D6E`: `test al,0xF` at
  `0x486D58`, `jbe` past the call when 0, the low nibble taken at `0x486D69`). Its other four
  reads of `+0xA` take the high nibble alone (`0x4867DA`, `0x486880`, `0x486CA2`, `0x486D42`), so
  a playing that clears the low nibble removes the corpse and its flare and nothing else.
- **The unit's model `+0x9E`** is freed only by the destructor, after the corpse (`0x486D9E`), and
  written otherwise only by the create
  ([`FreeObjectState`'s callers](memory.md#0x45aaa0-freeobjectstate-the-object3do-destructor)).

**Reproduction's source test is the one value a feature call reads from outside the feature
world** [DISASSEMBLED 2026-10-06]. The spawn, the destroy and the tick read no other unit slot.
Area damage `0x49A120` keeps two sums of the damage it deals, by whether the victim's owner byte
`+0xFF` equals the shooter's `+0x66` (`add [esp+0x44]` at `0x49A407`, else `add [esp+0x30]` at
`0x49A40D`; both zeroed at `0x49A1D3..0x49A1D7`), and reads them only after its loops, at
`0x49A823..0x49A82E`, as arguments of `0x406F50` on the shooter's owner's AI object (`+0x74`,
the call at `0x49A837`): its feature branch (`0x49A626`) never reads them. The feature code's
draws of the simulation RNG are exactly the six in the table below: a scan of
`0x4232F0..0x4251CA`, which holds the feature functions through the saved game's load
`0x424C00` (whose one `ret 4` is `0x4251C8`), finds no other call of `0x4B6C30`.

**The footprint writers.** The stamp `0x47CC30(unit)`, the clear `0x47D0E0(unit)` and the re-claim
`0x47C790(unit)`, `ret 4` each, write a cell's unit slots and yard bit only inside the unit's
footprint, the cell `+0x76`, `+0x78` by the size `+0x7E`, `+0x80` (i16 each; the size is the def's
`+0x14A`, written by the create at `0x485AA4..0x485AAA` and the saved game's load at `0x487257`):
the stamp at `0x47CD31..0x47D03A`, the clear at `0x47D105..0x47D23B`, the re-claim at
`0x47C7C9..0x47CA8E` (a structure's footprint through its yardmap, any other unit's first cell). The stamp files the unit in a sort bucket (`+0x82`, `+0x8E` and the
bucket's head): a footprint that leaves the grid in the bucket at `[main+0x142B7]`, writing no cell
(`0x47CC5E..0x47CCA3`, `0x47D073..0x47D0CF`), any other in its position's (`0x47CCA9..0x47CD25`).
A structure (`+0x110` bit 29) is stamped into slot A through its yardmap with the yard bit, the
incumbent of a contested cell and the structure marked in `+0x110` bits 26 and 27 by whether the
incumbent's owner record is type 3 (`0x47CDCA..0x47CE1D`), and the stamp ends by recomputing the
cells' 2×2 height extremes `+0x05`, `+0x06` around the footprint (`0x483210`, from the heights
`+0x04` alone) and the passability (`0x440A40`); any other unit goes into slot A or B by `+0x110 & 3`
and nothing is called. The clear empties the slots that hold the unit by the same branches, a
structure's with its yard bits and then `0x483210` (`0x47D1DF`), and nothing for a unit in the
off-grid bucket (`0x47D0F7..0x47D0FF`); it tests nothing about the footprint's place. The re-claim
does not test the cell `0x481550` returns (`0x47C7D3`). **The clear nests the re-claim**: for a
unit with `+0x110` bit 26 it walks the buckets around (`0x47D2B9..0x47D2C4`, the walk `0x47E5C0`,
which calls `[cb]` at `0x47E69F` and `0x47E6EC`) with the callback object at `0x4FD660`, whose
function `0x47ED30` re-claims each unit it is handed (`0x47ED35`); MEASURED 2026-09-30, the
battle's playback called a writer inside another 1,200 times, the first of them this way. Nothing else nests: the three are called only directly (none is
stored as a pointer in the image), no direct path from one reaches another, and `0x4FD660` is passed
only by the clear, the saved game's `0x47DB20` (`0x47DB4B`, one caller `0x487698`) and `0x47CAC0`,
which nothing calls. The set-position `0x48A9F0` calls the clear and then the stamp (`0x48AA6B`,
`0x48AA9A`), from the cell it rounds the position to (`0x48AA00..0x48AA32`); with the same cell
and layer it writes the position alone (`0x48AA35..0x48AA68`). **A recorded footprint cell is the
set-position's rounding of the recorded position**: MEASURED 2026-10-12 in 3,640,526 of 3,640,526
mobile unit-iterations of an air war and a battle, so a playing that places the image's units
through the set-position stamps where the recording machine did. The battle's one differing slot
B of 2026-09-30 (GameTime 1280: the recording's unit 854, the host's 858, aircraft at the map's last
columns) is a contest decided by the puppet's type-3 rule or the stamp's edge bound, not a lead
[INFERRED]. Which of the two maintains a playing's slots: `tagpu_puppet.c`, *The footprint
writers*, and *The footprint contest's rule* below.

**A cell's slot names a live unit.** The destructor takes the unit off the grid at `0x48682D`
(`0x47CBD0`: the clear, then the unlink from its bucket, one caller) before it clears the alive bit,
`+0x110` bit 28, at `0x486DCE`; so between iterations a slot names a unit with bit 28 set. The
stamp's contest dereferences the incumbent's owner `+0x96` with no NULL test (`0x47CDCA`): a unit
holds one from its create (`0x485887`, `0x485902`; the other writers are `0x4AA004` and `0x4AA882`)
and the destructor leaves it, but a slot no unit has taken holds NULL there. The replay writes a
cell's slots only when each names a unit alive on the host (`cell_put`).

**A GAF record's two sequence states**, `+0x04..+0x0F` and `+0x10..+0x1B` (a `u16` frame, a `u16`
timer, a `u8` loop flag, the sequence at `+8`, written by `0x4B8B30`): the first is always in use,
the second — the shadow — only while `+0x2F` bit 2 is set. The burn and FeatureDie set the bit
exactly when they write the state and clear it otherwise (`0x42348E..0x423498`,
`0x4236B5..0x4236BF`); the tick steps the state (`0x4243E1`, `0x424473`) and the draw draws it
(`0x46A6E8`, behind the shadows option `[main+0x37F06]` bit 4 as well) only behind it. While it is
clear `+0x18` holds whatever the record's last tenant left: a corpse that was sinking leaves its
velocity `−0x2CCC` there (`0x424292`). The recorder images the pointer as no sequence while the bit
is clear (`REC_WRECK_SHADOW`, `tagpu_recfmt.h`), and the replay reads a sequence's count only
through the def's own field.

**Hazards in stock, recorded rather than fixed:**

- a corpse takes over the velocity its record's last tenant left, since a record is not cleared
  when it is freed: `0x486360` spawns a corpse on ground above the sea level `main+0x1427F` at
  `0x486434` and writes no velocity, and one below it at `0x4863F9` and then writes `+0x18` and
  `+0x1C` (`0x486416`, `0x48641D`, unless the unit def's `+0x241` bit 24 is set), leaving `+0x14`
  [DISASSEMBLED, not measured];
- reproduction's row divides by the height (above);
- a burn ends when its sequence does (the first state's sequence at `+0x0C` cleared,
  `0x4243F0`): the destroy `0x42442F` with `force` 0, then the spawn of the def's `+0xF6`. An
  indestructible def is refused by both (`0x42471E`, and the spawn's own clear), so an
  indestructible feature that burned would keep its record on the active list, retried every tick.
  The damage does not burn one (`0x4244F9`) but the spread's burn does not test the bit
  [DISASSEMBLED, not measured]; no stock feature is both flamable and indestructible (131 and 678
  of them, the install's feature TDFs, 2026-09-30).

**The observers** (`tagpu_recorder.c`, the install; the stub's contract is the event sites'):

| Entry | The bytes it takes (pristine) | Recorded |
|---|---|---|
| the tick `0x424050` | `A1 E8 1D 51 00` | a root; its bracket carries the cells that changed since the images |
| the spawn `0x423C50` | `83 EC 28 53 55` | the cell as `(x, z)`, the def, `v`, which of `pos` and `turn` it was given, and both |
| the destroy, `FEATURES_Destroy` | `53 8B 0D E8 1D 51 00` at [`0x4246B0`](memory.md#0x4246b0-features_destroy-the-wreck-path) | the cell, `force` |
| FeatureDie `0x423550` | `83 EC 08 53 55` | `(x, z, reclaimed)` |
| the burn `0x4233A0` | `8B 44 24 08 8B 4C 24 04` | `(x, z, flag)` |
| the damage `0x4244B0` | `8B 0D E8 1D 51 00` | the cell, `x`, `z`, the weapon's ID |
| the swap `0x423710` | `8B 44 24 08 8B 4C 24 04` | `(x, z, reclaimed)` |
| the stamp `0x47CC30` | `83 EC 18 53 55` | nothing: they mark the unit's footprint |
| the clear `0x47D0E0` | `83 EC 08 53 8B 1D E8 1D 51 00` | |
| the re-claim `0x47C790` | `53 55 56 8B 74 24 10` | |

The six draws are `call`s redirected to stubs of 17 bytes (`push [esp+4]; push site; call
feat_draw; ret 4`), which call `0x4B6C30` themselves unless a replay has armed the draw. The
destructor's explosion call `0x486D50` (`E8`, to `0x49B000`) is redirected the same way (`push
[esp+8]` twice; `call rec_blast; ret 8`): a frame while recording, inside which the area damage's
feature calls are roots, and on a replay's host the place the recording's are made.

MEASURED 2026-09-30, format 3, `scenarios/s3a-forest-fire.json` (two groups of fire-starting units
in the densest forest of Two Continents, a construction kbot reclaiming a wreck; 4,710 iterations):
`recorder: 50 event sites in`; 73 damage roots from `0x49A626` outside any death and 23 inside the
explosions of 7 of the 23 deaths, a reclaim's FeatureDie from `0x423965` and the scenario's own
spawn; 295 draws, all from the burn (76), the spread (161 to the neighbours, 2 down the wind) and
reproduction (56); 0 cells changed that no observer had marked. On a puppet host every root was
made, every draw taken as recorded, no cell or record left for its structure, and every dump equal
to the recording (*S3a, verified and measured* in the slice note).

### A NULL plot handed to `GetGridPosFeature 0x421E60`

`GetGridPosFeature 0x421E60` is `stdcall(plot)`, `ret 4`. It opens with `mov ecx,[esp+4]` /
`mov ax,[ecx+8]`, reading the plot's feature index with no test, and "no feature" is its
`or ax,0xFFFF` at `0x421E9C`. Three functions turn a place into a plot, and each can return NULL:

- `GetGridPosPLOT 0x481550` (a cell) and `0x4815A0` (a position) return NULL for a cell outside
  `main+0x14233 × main+0x14237`.
- `0x4815F0` (a position) returns NULL for a cell off the grid too. It also returns NULL for a cell
  **on** the grid that holds `0xFFFE`, a multi-cell feature's non-anchor cell, when the offset to
  the anchor in the cell's bytes `+0xB`/`+0xA` leads off the grid (`0x48164A..0x481682`). That is
  the only way a position on the map gets a NULL plot.

`0x421E60` has three callers:

| call | what | tests the plot |
|---|---|---|
| `0x47EAE3` | a lookup on the plot `0x4815A0` returns | yes, `0x47EADA` |
| `0x498F4F` | the hovered feature in the pointer→world function `0x498DA0` (one caller, `0x499221`), stored at `main+0x2CBC` | **no** |
| `0x40514A` | the target lookup of the resurrect order `Order_Resurrect 0x404DB0` (its own section below), through `0x4815F0` on the order's position, after the unit is made | **no** — but `0x4815F0` answers NULL only for a position off the grid or a `0xFFFE` cell whose offsets lead off it (`0x481602..0x48162A`, `0x48165A..0x48166A`), and the order's entry check has already found a feature at that position in the same call |

**How `0x498F4F` gets its plot.** `0x498DA0` turns the pointer into a world point: the viewport arm
is `eye + clamp(pos, L, R) − L`, and the minimap arm scales the pointer by `main+0x1422B`/`+0x1422F`
(§"The in-game mouse buttons"). `GetTPosition 0x484B50`, called at `0x498EF9`, clamps x and y to
the map's pixel size `main+0x14223`/`+0x14227` (`0x484B5D..0x484B88`). It then searches for the
terrain point in a window from `y & ~15` down 0x80 px (`0x484B94..0x484B9B`), plus interpolation,
so its answer is below `(y & ~15) + 144`: up to 143 px below the point it was handed. `0x498F2E`
stores that point's cell at `main+0x2C8E`, `0x481550` makes it a plot, and `0x498F4F` hands the
plot over.

**What keeps stock off it.** `main+0x1422B`/`+0x1422F` is not the map's size. It is the **scroll
extent**, the map's pixel width less 32 and its height less 128, written at the level load
(`0x4833C4`, `0x4833E0`). The engine's camera clamp `0x41C3C0` holds the eye in
`[0, extent − view]`, the viewport arm clamps the pointer to the rect, and the minimap's click
rect ends at `mmY + mmH − 1`. So every point `0x498DA0` produces has `y ≤ mapH − 129`, and
GetTPosition's answer is below `(mapH − 144) + 144 = mapH`, on the map. **The 128-px bottom margin
is exactly GetTPosition's search window, and nothing tighter protects this call.** Two Continents
measured it: map 10752 × 12800, extent 10720 × 12672.

**Where stock reaches it.** The margin holds only while `[0, extent − view]` is a range:

- **A map shorter than the viewport plus 128 px.** Then `extent − view` is negative. `0x41C40D`
  sets a negative eyeY to 0 and `0x41C42D..0x41C431` set an eyeY above `extent − view` to that
  negative value, so successive calls alternate the eye between the two. At eyeY = 0 the
  viewport's bottom row is past the extent. GetTPosition clamps y to `mapH − 1` and, where the
  terrain along the bottom edge is raised, can answer up to 127 px past the map: a NULL plot.
  [INFERRED from `0x41C40D..0x41C431` and GetTPosition; not run.] Retail's largest mode,
  1600×1200, has a 1136-px viewport. Of the 275 stock map files only `example.tnt` (1024 px, not in
  the skirmish list) is that short; the shortest skirmish map, Lava Run, is 1280 px tall with an
  extent of 1152 (their TNT headers). A custom map can be, and our DLL runs any resolution. MEASURED on Lava Run at 1920×1440 (a
  1376-px viewport) on a build without the patches: **the engine's terrain pass faults first**, on
  the first in-play frame, in the tile copy at `0x4CBE44`, called from `0x484110` inside
  `0x483FA0` (called at `0x468DB0`). That is the third defect below, "The terrain pass reads off
  its tile map", which is now bounded; the pointer's path to `0x421E60` on such a map was not run
  after it.
- **The debug-level console command `Edge`** (run level 4; table entry `0x50206C`, handler
  `0x416730`) rewrites both margins from its two arguments, 32 and 128 by default. A bottom margin
  under 128 lets the pointer's row leave the map. [INFERRED from the arithmetic; not run.]
- **`0x40514A`** hands over the plot `0x4815F0` returns for the order's position. 847 live
  resurrections never reached the branch a NULL plot would take (the resurrect order's section
  below); the guard covers it whether or not it is reachable.

**Our build.** Zoom > 1 lets the eye past the stock range, and vpwide's rect at zoom < 1 lets the
pointer past it, so both can hand `0x498DA0` a point beyond the extent. Two clamps keep it off
this call: `zoom_tpos_guard` (`tagpu_zoom.c`, the redirect of `0x498EF9`) and vpwide's replica of
`0x498DA0` (the redirect of `0x499221`). Both clamp the point to `[0, extent − 1]`, and **they work
because they read the scroll extent**: a clamp to the map's own size would leave 128 px in which
GetTPosition can answer off the map. MEASURED in experiment builds with `0x498EF9`'s clamp turned
off, at zoom 2.0 with the eye held at the bottom of our range (eyeY 12144). Without the patch, the
pointer on the bottom bar at (512, 760) faults at `0x421E64` reading `0x00000008`, return address
`0x498F54`. With the patch it does not fault and the simulation keeps ticking. The cell left at
`main+0x2C8E` is (125, 802), past the map's 800 rows, and the hovered feature reads `0xFFFF`.
vpwide's comment records the same fault once from an edge scroll at 0.5×, before its replica
clamped.

**The patch is a prologue detour on `0x421E60`**, eight stolen bytes (`tagpu_detour_land`):

```
mov  ecx,[esp+4]
test ecx,ecx
jnz  +7
or   ax,0xFFFF         ; the engine's own "no feature", as 0x421E9C
ret  4
mov  ax,[ecx+8]
jmp  0x421E68
```

- **The invariant:** `0x421E60` never dereferences NULL, whichever caller hands it the plot. No
  branch lands inside the eight stolen bytes [rel8/rel32 scan].
- **What it changes:** nothing for a non-NULL plot. For a NULL plot stock faults, so nothing the
  simulation reads differs except where stock would have crashed. `0x40514A` is on the resurrect
  order's path, so in a network game with a peer on stock, the stock peer crashes where the
  patched one goes on.
- **What it leaves:** the off-map cell `0x498F2E` stores at `main+0x2C8E` is untouched, as it is in
  stock under `Edge`. Its readers are not audited here.

### The terrain pass reads off its tile map when the view is larger than the map — `0x483FA0`, bounded at `0x484057`

**The pass.** `0x483FA0(OFFSCREEN* ctx)`: stdcall, `ret 4` at `0x4843B3`, one caller, `0x468DB0`
in `DrawGameScreen` [E8 scan]. It builds a window of 32-px cells from the eye and the view
[DISASSEMBLED `0x483FAC..0x484053`]:

| value | computed at | from |
|---|---|---|
| `col0`, `row0` = eye / 32 | `0x483FC2..0x483FD8`, `0x483FDC..0x483FF4` | the eye `main+0x1431F`/`+0x14323`, by `cdq; and edx,0x1F; add; sar 5`: a division that truncates toward zero, not a floor, for a negative eye. The stock path never runs on one: the window check at `0x484057` hands any window with an eye < 0 to `terrain_window_draw`, and the Vulkan terrain pass (`tagpu_terr.c`'s gather) takes a floor there ([terrain depth](../terrain-depth.html)) |
| `sx`, `sy` = eye − 32·cell | `0x483FF8`, `0x483FFD` (esi, edi) | negative for a negative eye |
| `ncols`, `nrows` = ⌈(view + s) / 32⌉ | `0x483FFF..0x484053` | the view `main+0x37E37`/`+0x37E3B` |
| stride = `main+0x14233` / 2 | `0x484057..0x48406D` | the map's width in 16-px cells |

Four loops then read the tile map `*(main+0x1428B)` at `row·stride + col` for every cell of
`[row0, row0+nrows) × [col0, col0+ncols)`: the left and right edge columns (pointer formed at
`0x4840A6`, reads at `0x4840EF`/`0x48412F`), the top and bottom edge rows (`0x4841A9`/`0x4841D6`,
reads at `0x484207`/`0x484247`) and the interior (`0x484345`, read at `0x48435D`). Each id becomes
`*(*(main+0x14283)+4) + id·0x400` with no compare. Edge cells go to the clipped GAF blit
`0x4B8150` (called at `0x484110`, `0x48415C`, `0x484228`, `0x484274`; its row copy is `0x4CBDD1`)
and interior cells to the unclipped `0x4C6E70` (called at `0x484370`; its copy is `0x4CBEF1`).

**The allocations** [DISASSEMBLED]. LoadMap `0x483610` allocates the tile map at
`0x48393C..0x483962` as `(pxH/32)·(pxW/32)` `u16` entries, from the map's own pixel size
`main+0x14223`/`+0x14227` (`[ebp+0x28]`/`[ebp+0x2C]`, written as the cell counts shifted left by
4 at `0x48388C..0x483898`). It stores the pointer at `0x483969` and copies the TNT's map data in.
The tile set is `count·0x400 + 8` bytes (`0x483B53..0x483B63`), laid out as
`{count, pixels = base + 8}` (`0x483B72`, `0x483B80`). So the stride equals the allocation's width,
and nothing in the pass compares a row with `pxH/32`: it never reads `main+0x14237` at all.

**Where stock is right.** The pass paints cell `(i, j)` of the window at
`(L − sx + 32j, T − sy + 32i)`. `ncols = ⌈(W + sx)/32⌉` carries the painted span to `L + W` or
past it for any `sx`, but the span starts at `L − sx`: the viewport's left column is painted only
when `sx ≥ 0`, and its top row only when `sy ≥ 0`. So the pass reads only map cells **and** paints
every viewport pixel exactly when `sx ≥ 0`, `sy ≥ 0` and the window lies inside the tile map.
With `col0` and `row0` truncated, that is `0 ≤ eye` and `eye + view ≤ 32·tiles` on each axis.
Every one of the 275 stock map files has an even number of 16-px cells on both axes (their TNT
headers), so `32·tiles` is the map's pixel size. Outside that, stock does one of two wrong things:

- **An eye in `(−32, 0)`** truncates to `col0 = 0` (or `row0 = 0`). Every read is on the map, but
  the window is drawn from `L − sx`, right of `L`: the viewport's leftmost `−sx` columns (or top
  `−sy` rows) are not painted and keep the last frame's pixels.
- **An eye at −32 or below** starts the window left of or above the map. `row0 < 0` reads before
  the tile map. `col0 < 0` reads the previous row's last cells, and on row 0 before the tile map.
  **An eye whose `eye + view` is past the map's far edge** runs the window off the end. Rows at
  and past `pxH/32` read past the end of the tile map. Columns at and past the stride read the
  next row's first cells, which stays inside the allocation on every row but the last.

An id read outside the allocation is whatever the heap holds there, and `pixels + id·0x400` can be
up to 64 MB past the tile set. That read is the fault.

**Where it is reached.** The camera clamp hands the pass such a window, and so did the zoom range
the BAR camera replaced.

- **The camera clamp** `0x41C3C0` holds the eye in `[0, extent − view]`, 32 px inside the map on
  X and 128 on Y. Where the view is larger than the extent that range is inverted, and the clamp
  alternates the eye between 0 and the negative `extent − view` ("Where stock reaches it"
  above). At the eye of 0 the window runs past the far edge only if the view is larger than the
  map; at the negative eye it starts before the map once that eye is −32 or less. So stock
  **reads off the tile map when the view is at least the map's width on X, or at least its height
  less 96 on Y**, and only leaves a strip unpainted when the view is in `(map − 32, map)` on X or
  `(map − 128, map − 96)` on Y. At HUD scale 100 % the view is the resolution less 128 × 64 (the
  side panel and the two bars); a HUD scale multiplies those margins. From the TNT headers: of the
  99 maps in the skirmish list, none reads off at 1920×1200 or below, retail's 1600×1200
  included. Lava Run (6400×1280) does at 1920×1440 and 2560×1440. At 3440×1440 six do: Lava Run
  by height, and Dark Side, The Desert Triad, Sherwood, Great Divide and Etorrep Glacier by width.
  At 3840×2160 twelve do: those six, plus The Pass, Coast To Coast, Shore to Shore, Show Down, Fox
  Holes and Slated Fate. On ten of the twelve the view is larger than the map on one axis, so the
  window is off the map at both of the clamp's eyes. Shore to Shore (2144 px tall) and Show Down
  (2176) hold the 2096-px view inside their height and are off the map only at the negative eye.
  Among the 275 map files, `example.tnt` also reads off at 1600×1200 and `cc01` at 1920×1080. None
  of the 275 lands in a strip-only band at 1024×768, 1600×1200, 1920×1080, 1920×1200, 1920×1440,
  2560×1440, 3440×1440 or 3840×2160.
  With `tagpu_zoom.on` (the play set) `0x41C3C0` is replaced by the camera's own clamp, which holds
  the eye at 0 where the engine's range inverts (`tagpu_zoom.c`, `zoom_eye_clamp` and
  `engine_range`), and the command apply clamps the eye into the same range at the top of every
  in-play draw. The pass then sees the eye at 0, so its window leaves the map only where the view
  is larger than the map, off the far edge, and no strip arises. Without `tagpu_zoom.on` the
  engine's clamp alternates as in stock.
- **Our camera's centre range** `[−W/2, map − W/2]` takes the eye below 0 at every left and top
  edge of any map, at any zoom, but it is in force only on draws whose terrain latch is up, and
  terrown takes the pass away on exactly those draws ([gpu-status](../gpu-status.html) §2.3c). Every
  other draw starts from an eye the apply clamped into `[0, extent − W]`, so the centre range
  never reaches the pass (MEASURED at the eight corner stops, the paragraph below the table). The
  zoom range the BAR camera replaced, `[−dx, extent − view + dx]` above zoom 1 with
  `dx = view/2 · (1 − 1/z)`, did reach it: at a left or top edge of any map the eye was negative,
  in `(−32, 0)` just past the edge and down to −448 × −254 at zoom 2 at 1920×1080, on every draw
  the engine's pass ran while zoomed in. The table's `09d7d55` rows were measured on that camera,
  and they are the evidence for the bound at a negative eye and for the strip.

MEASURED, each on the first in-play draw of a scenario load with the shipped play set (`--defaults`)
unless the row says otherwise. The pass's frame is decoded from the `ErrorLog.txt` stack dump.

| build | map, resolution | result |
|---|---|---|
| main `94e40ed` | Lava Run, 1920×1440 | access violation at `0x4CBE44` reading the tile. Return chain `0x4B82C2` ← `0x484115` ← `0x468DB5`. The frame: `row0 = −7` (eyeY −224 = extent 1152 − view 1376), `col0 = 73`, 57 × 43 cells on a 200 × 40-cell map, the tile-map pointer 2654 bytes before the array |
| main `94e40ed` | Lava Run, 3840×2160 | the same fault, from the right edge column (`0x484161`), `row0 = −29` |
| main `94e40ed` | Coast To Coast, 3840×2160 | the same fault, from the top edge row (`0x48422D`); `row0 = −6`, and 116 columns from `col0 = 0` on a map 105 cells wide |
| main `94e40ed` | Coast To Coast, 1920×1440 | no fault: the view, 1792×1376, fits the extent, 3328×1888 |
| main `94e40ed` | Dark Side, 3440×1440, the engine's terrain on every draw (`terrown.off`) | no fault. Past the map's right edge (screen x 3200..3439) the pass draws the next row's first cells |
| the BAR camera branch `b7c3f4a` | Lava Run, 1920×1440 | access violation at `0x4CBE44` from `0x484161` with `row0 = 0`. Its camera range holds the eye at 0 where the engine's inverts, and the window's last three rows (40..42) are past the end of the tile map |
| main `09d7d55` | Two Continents, 1920×1080, `terrown.off`, zoom 2, the eye held at −448 × −254 (the NW corner of the zoom-2 range) | access violation at `0x4CBE44` from the top edge row (`0x48422D`). The frame: `col0 = −14`, `row0 = −7`, 56 × 31 cells, stride 336 |
| main `09d7d55` | the same, the eye held at (−20, −20), (−20, 3000) and (3000, −20) | no fault. The viewport's 20-px strip at the left or top edge keeps the previous frame's ground: 0 of its 20 320 (left) or 35 840 (top) pixels are black |
| each of the above with the patch | the same | no fault. At the three eyes of −20 the strip is palette index 0: 20 319 of 20 320 pixels at the left edge and 34 288 of 35 840 along the top at the corner, the rest being sprites the engine draws over the ground afterwards |

**Our build at 1920×1080 zoomed out.** On Lava Run the view (1792×1016) fits the extent
(6368×1152), so the pass stays on the map. It runs only until our terrain pass owns the ground.
MEASURED on main with a temporary branch counter: 257–320 calls at the level's start, every one on
the stock path (57 × 32 cells on the 200 × 40 map). No further call was logged while the camera
was held at the four corners of its range, at 1× and at zoom 0.25 (the counter logs every 64th
call). `vpwide` widens only L/T/R/B, never W/H, and only on draws terrown has latched as ours
([gpu-status](../gpu-status.html) §2.3b), so the pass never sees the widened rect. The eye stays in
the 1× range `[0, 4576] × [0, 136]` at both zooms. The BAR camera branch's centre range, eye
`[−896, 5504] × [−508, 772]`, never reached the pass either: 385–448 calls at the level's start,
all on the stock path, and none at the eight corner stops. No fault anywhere in either build.

**The patch** is a jump at `0x484057`, over `mov eax,[ecx+0x14233]; mov ebx,[esp+0x5C]` (ten
bytes, ending at the `cdq` at `0x484061`). That is the first point where every value the window is
made of has been computed and nothing has been read through it:

```
pushad
push esp                      ; the pushad block; the pass's frame is 32 bytes above it
call terrain_window_on_map    ; cdecl: 1 = the window lies on the tile map
add  esp,4
test eax,eax
popad
jz   drawn
mov  eax,[ecx+0x14233]        ; the stolen pair
mov  ebx,[esp+0x5C]
jmp  0x484061                 ; the stock pass, unchanged
drawn:
jmp  0x4843AC                 ; the pass's epilogue: pop edi, esi, ebp, ebx; add esp,0x48; ret 4
```

`terrain_window_on_map` reads the pass's frame at `0x484057`: `ncols` at `[esp+0x10]`, `col0` at
`+0x14`, `nrows` at `+0x18`, T at `+0x1C`, L at `+0x20`, `row0` at `+0x24` and the OFFSCREEN at
`+0x5C`. It takes `sx`, `sy` and `main` from the saved esi, edi and ecx. It returns 1 exactly
where stock is right: `sx ≥ 0`, `sy ≥ 0`, the stride equals `pxW/32`, and
`[row0, row0+nrows) × [col0, col0+ncols)` lies inside the `pxH/32 × pxW/32` tile map. Otherwise
`terrain_window_draw` draws the same window itself. It fills the viewport `[L, L+W) × [T, T+H)`,
clipped to the OFFSCREEN's clip rect `+0x1C..+0x28`, with palette index 0. It then copies every
cell of the window that is on the map, and whose id is below the tile set's count, to
`(L − sx + 32j, T − sy + 32i)`, where stock puts it, with the same clip. Before it writes anything
it checks the inclusive clip rect against the surface's own width and height, `+0x00`/`+0x04`,
which SurfaceCreateNamed `0x4C69F0` stores at `0x4C6A2C`/`0x4C6A35` and sizes the pixels by: a
right column at or past the width, or a bottom row at or past the height, is refused, and nothing
is drawn.

- **The invariant:** for any eye and any view, the pass reads no tile-map entry outside
  `(pxW/32)·(pxH/32)` and paints every viewport pixel inside the clip rect. On our path it also
  reads no tile graphic outside `count·0x400`; the stock path trusts the ids ("What it leaves").
  The drawn window writes nothing outside the clip rect, and nothing at all when the rect is not
  inside the surface. `DrawGameScreen` sets that rect from the viewport rect (`0x4C6B10`, called
  at `0x468D85`), and the engine's own clipped blit holds every edge cell to it. No path we know
  hands the pass a rect outside its surface: SurfaceCreateNamed and `0x4C6A60` (the same fields
  over a buffer the caller owns) initialise it to `(0, 0, w − 1, h − 1)`; `0x468D85` sets the
  viewport rect, which lies inside the screen-sized OFFSCREEN, and when `vpwide` widens that rect
  its `vpw_setclip` clamps it to `w − 1`/`h − 1` from the same `+0x00`/`+0x04`.
  `0x4C6B10`'s other five callers (`0x46964F`, `0x469F95`, `0x495CAC`, `0x4A20A1`, `0x4A22DF`)
  are not audited. So the refusal is the bound, not a case any run has shown. Registers
  at `0x484057`: ecx (main), esi, edi and ebp (0) are live. eax and ebx are what the stolen pair
  loads, edx is overwritten by the `cdq` at `0x484061`, and the flags are set again at `0x48406B`
  before anything tests them. `0x484050`'s `je` lands on `0x484057`, the jump itself; no branch
  lands in `0x484058..0x484060` [rel8/rel32 scan].
- **What it changes:** nothing on a draw where stock is right, which includes every draw with
  the eye in `[0, extent − view]`. MEASURED on the stock control (no play pass) on Coast To Coast
  at 1920×1440, with the camera at the centre, at the far corner of its range (1536, 512) and at
  (0, 0). The branch counter saw 1 473 calls or more, every one on the stock path. Against main
  `09d7d55` the engine's frame is 0 px at the centre and at (0, 0), and 1 997 px at the far
  corner, every one of them the message line "Core has taken the lead with 1 kills" that the
  engine printed in one run and not the other.
- **Where the window leaves the map, it draws black where stock read off it.** MEASURED on Lava
  Run at 1920×1440 with the engine drawing the ground on every draw (`terrown.off`): 2 497 calls
  or more through the bound while the camera scrolled from x 4080 to 0, no fault, and exactly the 96 rows
  past the map's bottom black (screen y 1312..1407). The rows on the map are the stock pass's own
  pixels. At the same eye at 1920×1080, where the window is on the map and stock draws it, the
  shared 1792 × 1016 region differs only on a steam vent's animation, the unit and the pointer:
  860 px, none of them on the ground. On Dark Side at 3440×1440 the 240 columns past the right
  edge are black where stock drew the next row's cells, and the rest of the viewport matches stock
  except for 57 px of a GUI line at its top-left corner. With the play set the bound draws only the
  level's first draws, before our terrain pass takes the ground. MEASURED on main's camera before
  the BAR camera replaced it: on Lava Run at 1920×1440, 193–256 calls, first with `row0 = −7` (the
  engine's alternating eye) and then with `row0 = 0`. On the BAR camera the pass is never handed
  that eye ("What it leaves", below): on the merged build, Lava Run at 1920×1440 with
  `tagpu_terrown.off` so the engine draws the ground on every draw, the eye stayed in
  `[0, 4576] × [0, 0]` at every zoom, so `row0` is 0 on every window the bound draws there. The
  simulation reads nothing the pass draws.
- **Where stock leaves a strip, it paints it.** MEASURED on Two Continents at 1920×1080 with
  `terrown.off` at zoom 2, the eye held at (−20, −20), (−20, 3000) and (3000, −20): the strip is
  black (the table above), and the rest of the viewport is 0 px from main `09d7d55` at the first
  two eyes and 985 px at the third, all of them a steam vent's plume.
- **What it leaves:** on the stock path the interior cells still go through the unclipped
  `0x4C6E70`. They stay inside `[L, L+W) × [T, T+H)` for any sub-cell offset, and terrown's latch,
  not this patch, keeps an origin moved off the offscreen (vpwide's widened rect) away from them
  ("Two per-cell loops that differ" below). Only our path bounds the tile ids by the tile set's
  count: the stock path reads them unchecked, as LoadMap copies them in raw (`0x48397D`), so a
  map whose ids reach past its tile set still reads past it there. The route to a bad window is
  not patched, and the bound draws what it is handed: over a view larger than the map the window
  runs off it from any eye. With `tagpu_zoom.on` the camera's clamp and the command apply hold an
  inverted range at 0, so the pass is never handed the engine's alternating eye; without it, the
  engine's clamp alternates as in stock and the bound draws those draws.

### The wreck pool, and a feature paid for and left standing

`[DISASSEMBLED 2026-09-24, MEASURED the same day]` The pool at `main+0x1420B` (its allocation is
in the table above) holds a record for **every 3DO feature for its life** — each unit's corpse,
each heap — and for **every GAF feature while it plays its death or reclaim sequence**. Three
lists partition it, each record's `+0x00` next and `+0x02` prev being **signed** 16-bit (`movsx` at
`0x42331F`), so no index may reach `0x8000`: `main+0x14213` the active in-use list, `main+0x14217`
the settled one, `main+0x1421B` the free list (−1 = empty). `0x4232F0(idx, &list)` moves a record
onto a list, unlinking it from whichever head it was, and pushes it at the head (its next is the
old head, `0x42337B`, and the head becomes it, `0x42337E`); the spawn takes
the free list's head (`0x423DA8`). The free list is therefore LIFO, and the record a spawn takes
follows from the order of every earlier spawn and free — the reason a multiplayer playing makes
its corpses only through the recorder's roots, in recorded order. DISASSEMBLED. MEASURED with a census that reads the whole
pool and walks the three lists: with 8192 corpses placed, 0 active + 8192 settled + 0 free, every
record exactly once.

| address | what | the pool |
|---|---|---|
| `0x423550` `FeatureDie(x, y, reclaimed)` | stdcall, `ret 0xC`. Resolves a multi-cell feature's anchor and writes it back over its own `x`/`y` (`0x423580`). A 3DO feature, or a GAF one with no sequence for the event (FeatureDef `+0xC4/+0xC8` reclaim, `+0xBC/+0xC0` death), goes to `0x4236EF`: `push ebx/ebp/esi; call 0x423710`. Otherwise a cell already marked (flags bit 0) returns, and the rest takes a record, stores the def (`+0x2C`), `reclaimed` as bit 1 of `+0x2F`, x/y (`+0x28`/`+0x2A`) and the sequences, and marks the cell (`+0x0A` = the record, flags bit 0) | **pool empty: `0x42361D` sets `eax = count`, `0x423651` `jge 0x4236F7` returns having done nothing** — the fork's engine fix retargets that jge to a stub that reloads `x`/`y` from the arguments and jumps to `0x4236EF` |
| `0x423710` the swap `(x, y, reclaimed)` | stdcall, `ret 0xC`. A non-anchor cell (def `≥ 0xFFFB`) returns at once. The successor is FeatureDef `+0xF8` (`featurereclamate`) when `reclaimed`, else `+0xF4` (`featuredead`); a marked cell's record bit 1 overrides to `+0xF8`. Then `FEATURES_Destroy 0x4246B0` with force 0 and `0x423C50` for the successor. Force 0 refuses an indestructible def (FeatureDef `+0xFF` bit 1, `0x42471E`), and `0x423C50`'s own clear then refuses too (`0x423D71`), so **an indestructible feature is left as it was**. Two callers: `0x4236F2` (FeatureDie's no-sequence path) and `0x424495` (a sequence that has ended, in `0x424050`) | frees the record through `0x4246B0` |
| `0x423C50` `SpawnFeatureOnMap(plot, def, pos, turn, v)` | clears the footprint with `0x4246B0` on every occupied cell, then a 3DO def takes a record | **pool empty: `0x423DB9`/`0x423DDF` return 0 with nothing created** — a unit dies without a corpse, and the footprint was already cleared |
| `0x4233A0` `(x, y, flag)`, the burn: starts the feature's burn sequence, FeatureDef `+0xB4`, and draws its spread countdown `+0x2E` (`0x4234C2`; [the demo recorder's features](#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30)) | returns if the def has no `+0xB4` sequence or the cell is marked | pool empty: `0x42340D`/`0x423439` return — the feature does not burn |
| `0x4232A0` | a bare allocator: returns the free head, or the count for "none" | no `E8` caller |
| `0x424050` | the feature tick, one caller `0x495585` in the game tick; walks the **active** list only, moves a settled record to `+0x14217` (`0x4242B8`), and swaps a feature whose sequence has ended (`0x424495`); the whole of it: [the demo recorder's features](#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30) | — |
| `0x4237D0` the reclaim completion `(who, pos)` [the first argument INFERRED] | stdcall, `ret 8`, returns 1 when reclaimed. A GAF feature whose cell is marked returns 0 before anything is paid (`0x423892` the cell's flags bit 0, `0x423898` the def's `+0xFE` bit 0) — **but stock tests the targeted cell, and `FeatureDie` marks only the anchor**, so a reclaim that lands on any other cell of a multi-cell feature while its sequence plays is paid again; the fork's engine fix makes `0x423892` test the anchor ([below](#the-reclaim-paid-twice)). Pays FeatureDef `+0xEC` **energy** (`0x4238FF`, or `0x4238EF` on a branch that scales it by one of two constants chosen by `main+0x37EEE`, for a player whose `+0x73` is 2 [INFERRED: a computer player, by difficulty]) and `+0xF0` **metal** (`0x42395B`, both branches; the order is TADR's `FeatureDefStruct` and our `tagpu_cat.c`, and MEASURED: `Building15` reads energy 0, metal 2900, and pays 2900 metal), **then** calls `FeatureDie(x, y, 1)` (`0x423965`), then, in a network game (`0x435100` = 3), sends `0x0F, 0xFF, x, y` (6 bytes, `0x4239A4`) | — |
| `0x4244B0(cell, x, y, weapon)`, a feature taking damage; callers `0x49A626` (the area damage) and the network handler | calls `FeatureDie(x, y, 0)` at `0x424628`/`0x42465A` and sends subtype `0xFD`; [the demo recorder's features](#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30) | — |
| `0x424C00` the save-game loader's feature half (one caller, `0x43265A` in the game-load routine `0x432610`, which the border fix wraps: "A saved game's features on the map's border") | restores a feature's sequence with `FeatureDie(x, y, 1)` (`0x42507E`), `FeatureDie(x, y, 0)` (`0x42509D`) or `0x4233A0(x, y, 0)` (`0x4250BB`), then writes the record the cell's `+0x0A` names (`0x4250C0..0x4250F4`: `+0x26`, the low word of `+0x04`, `+0x2E`) **without checking that one was taken** | on a full pool an `Animating` record's GAF spawn still succeeds and takes no record, leaving `+0x0A` at 0 (`0x423EE9`); `FeatureDie` finds none (`0x42361B` → `0x42364C` → `0x423651`), and in stock the write lands on record 0, which on a full pool is live. A `3D` record refused on a full pool (`0x423DDF`/`0x423DE7`) writes through a word no spawn wrote — record 0 on the zeroed grid a load gets. Both patched ("A saved feature's state and the wreck record its cell names"). A save holds no more records than its own pool, so only a save loaded into a smaller pool gets there |
| `0x455xxx` the feature event handler | subtype `0xFF` → `FeatureDie(x, y, 1)` (`0x4554B0`), `0xFE` → `0x4233A0(x, y, 1)` (`0x4554CA`), `0xFD` → `FeatureDie(x, y, 0)` (`0x4554E4`) | the receiver's own pool decides |

**The defect.** With the pool empty, `FeatureDie` does nothing, and nothing tells its callers. The
reclaim has already paid, and the cell is left unmarked, so the same feature can be reclaimed
again, as often as the builder repeats it; a feature hit hard enough to die does not change either.
MEASURED on the build before the fix, Town & Country, the pool filled with 2048 one-cell corpses
(`armflea_dead`) until the engine refused the 2049th: a commander's reclaim of `Building15`
(2900 metal) raised the player's total metal produced (`PlayerStruct+0x8C+0x28`, a double) by
2900 and left the cell's def at 9, `Building15`; a second reclaim paid 2900 again. In the owner's
ten-player game at 1500 units a player, the free head read −1 on each of the three peers read.

**The fix** (`fix_feature_die_pool_full`, always on): the pool-full `jge` joins `0x4236EF` with `x`
and `y` reloaded from the arguments (`[esp+0x1C]`, `[esp+0x20]`; `ebx` still holds `reclaimed`).
MEASURED, same procedure: paid once, the cell's def 351 (`Buildingscar15`, not reclaimable), no
record, the free head still −1; a second reclaim paid nothing. Over the network with both pools
full, the host's reclaim swapped the building on both peers and paid only the host.

**What a full pool still changes.** The sequence is skipped, and with it the window in which the
cell is marked and further hits are ignored. MEASURED with a commander's D-gun on `Building15`: with
free records, the cell was marked (flags `0x54` → `0x55`) for 1.2 s and ended at `Building15b`;
with the pool full it ended at the scar, the successive frames' hits taking it through every stage.
Over the network each peer's own pool decides. A sender with a full pool walks a feature through
its stages, one `0xFD` event each, while a receiver with free records marks the cell on the first
and drops the rest at `0x423606`. In the other direction, a receiver with a full pool swaps at once
while the sender's cell stays marked for its sequence, and an event that receiver later sends about
the successor is dropped at the sender. Either way a feature can differ between peers once one pool
is full and another is not; with every pool full they agree, as measured. The successor itself can
be refused: a GAF feature whose successor is a 3DO feature is destroyed by the swap and the
successor refused at `0x423DDF`, so nothing takes its place. A full pool refuses corpses too
(`0x423C50`), which no fix can give back.

#### The reclaim paid twice

`[DISASSEMBLED, MEASURED 2026-09-24]` This one needs no full pool. `0x4237D0` refuses a GAF feature
that is playing its sequence by testing the flags of the cell the builder aimed at (`esi`, from
`0x481550` at `0x4237FE`; the test at `0x423892`), while `FeatureDie` marks only the anchor
(`0x42368B` stores the record in the anchor's `+0x0A`, `0x42368F..0x423695` sets its bit 0) and every
other cell keeps bit 0 clear (`0x423C50` at `0x423F4D`, `0x4246B0` at `0x4247BB`). A reclaim that
lands on any other cell while the sequence plays is paid in full, and its `FeatureDie` then returns
at `0x423606`. Builders ordered together onto one building finish in the same moment, so that is
the ordinary case, not a race. MEASURED on Town & Country with free records, two commanders ordered
onto `Building15` together (2900 metal): through its centre cell (312,128), **+5959** at the first
payout — twice 2900, and income; through its anchor (311,127), +3056 — once.

**The fix** (`fix_reclaim_mark_anchor`, always on): `0x423892`'s six bytes (`test`, `je`) become a
jump to a stub that resolves a `0xFFFE` cell to its anchor exactly as `0x423845..0x423862` does —
`cell − (row · width + col) · 13`, `row` and `col` the bytes at `+0x0A`/`+0x0B` — tests the anchor's
bit 0, and rejoins at `0x4238AD` (pay) or `0x423898` (the def's GAF test). It reads byte `+0x0C` of
the cell stock itself reads byte `+0x08` of at `0x423864`, so it forms no address stock does not.
The anchor's bit 0 is set by every sequence in play, not only a reclaim: `FeatureDie`'s death
sequence, and a burn (`0x4233A0` at `0x423468`). So the fix also refuses, through any cell, a
reclaim of a multi-cell feature that is burning or dying — the reclaim stock refuses through the
anchor. Where such a feature has no reclaim sequence, stock paid it once through another cell and
swapped it (`FeatureDie` takes `0x4235FC` before it reads the mark); that one reclaim is now
refused. The two builder callers (`0x404D66`, `0x414A4A`) ignore the result and return 5 either way, so a
refused builder ends its order as a paid one does [INFERRED: 5 as the order's done state].
Only `0x423884`'s `jne` branches to `0x423892`, and nothing into the five bytes after it. MEASURED
with the fix, same procedure: through the centre cell +3056 at the first payout and +3143 over
80 s, the anchor's +3140 — the building played its sequence with a record held and ended as
`Buildingscar15`. With the pool full as well: +3062 at the first payout, +3146 over 82 s, the scar
at once and the pool still 8192 of 8192. The scar is worth nothing (energy 0, metal 0), is not
reclaimable, and has no `featurereclamate` (−1).

**Negative results.** `0x422C71` (`mov edi,0x800`) is FeatureMask bit `0x800`, not the pool.
Nothing else writes or compares the pool's size: every reference to `main+0x1420B`, `+0x14213`,
`+0x14217` and `+0x1421B` was read, and a sweep of the functions that make them for `0x800`,
`0x7FF`, `0x18000`, `0x6000` and `0x17FD0` found only the eleven limits-table sites. TADR patches
none of this (its source as of `dcff5dd`).

### A saved game's features on the map's border — `0x4833B0` before `0x424C00`, wrapped at `0x43265A` [DISASSEMBLED + MEASURED 2026-09-25]

**The mask.** LoadMap's last write to the feature grid is `0x4833B0`, called at `0x483CF1` after
the feature join: over a fixed set of cells it writes `0xFFFD` on a cell that is EMPTY (`0xFFFF`) or
a footprint cell (`0xFFFE`) and leaves an anchor alone. The passes and their five stores are in
"LoadMap's feature placement, and a saved game" (below). It is the only producer of `0xFFFD` in
the grid: no other `0xFFFD` immediate in `.text` writes a def word (`0x446EAF`'s `mov ebx,0xfffd`
is a bit mask applied to `+0x9D` of another structure).

**What `0xFFFD` means to the engine** [DISASSEMBLED]. No reader tests `0xFFFD` alone; each treats it
as one of the values at `0xFFFB` and up, which means one of three things:

| meaning | readers |
|---|---|
| **blocked**: building and movement. Five readers share one rule: `0xFFFF` is free; a def below `0xFFFB` is blocked when FeatureDef `+0xFE` bit 6 is set, or when the def is at or past the def count; `0xFFFB`..`0xFFFD` are blocked; a `0xFFFE` cell is resolved to its anchor, where a def at `0xFFFB` and up reads as FREE and a def below goes straight to the bit-6 test, with no compare against the def count (in `0x47DE60`: `0x47DECA`/`0x47DECC`, `0x47DED0` → `0x47DED5`) | the yardmap placement test `0x47D2E0` (`0x47D57F`, fails at `0x47D80F`; callers `0x40A4CD`, `0x4198C6`, `0x47DBE5`); the footprint placement test `0x47DB70` (`0x47DC86`, fails at `0x47DD99`; nine callers, among them `0x402899`, `0x43D912`, `0x465321`, `0x481412`); per-cell passability for a movement class `0x47DE60(class, cell)` (`0x47DE7C`, returns 0); rect passability `0x47DFC0` (`0x47E090`), which the pathing refresh reaches through `0x47E1F0`; a transport's unload test `0x47E2D0(unit, pos)` (`0x47E449`; callers `0x40F417`, `0x40F57C`), which tests only the cells the player can see (`[main+0x14273]`) |
| **refused**: no feature may be placed | `SpawnFeatureOnMap 0x423C50`: its footprint loop (`0x423D5F`) hands every cell that is not `0xFFFF` to `FEATURES_Destroy 0x4246B0` (`0x423D6A`), which refuses a def at `0xFFFB` and up (`0x4246EC`, returns 0 at `0x4246F2`), and the spawn abandons (`0x423D6F`/`0x423D71` → `0x423DE7`, returns 0). The feature spread in the tick `0x424050` moves only onto a cell that is exactly `0xFFFF` (`0x42418F`), and only from a source cell with no unit on it (`0x424189`, which reads the source, `[esp+0x10]`) |
| **no feature**, the same as `0xFFFF` | `0x421DA0` and `GetGridPosFeature 0x421E60` (return `0xFFFF`); the order step `0x405141`; the AI's whole-map metal scan `0x40A7B0`; the cursor's feature word `0x4163A0`; the metal footprint `0x422040` (called once, at `0x483D7D`, after the mask); the burn `0x4233A0` (returns at `0x42353C`); `FeatureDie 0x423550` (returns at `0x4236F7`); the swap `0x423710`; the reclaim completion `0x4237D0`; the fire spread `0x4239C0`; the tick's one-cell sweep `0x424050`; `0x4244B0`; destroy-all `0x424840`; the save writer `0x424890`, which saves anchors only; the cursor chooser `0x43E490` and the order-for-target `0x43F0E0`; the feature draw in `DrawGameScreen` `0x468CF0` (`0x4698AA`, anchors only); the area reclaim scan `0x47EA40`; the projectile and feature-hit checks `0x49A120`, `0x49B284` |

So the mask closes the border to building, to movement and to feature spawns, and to nothing else.

**The pathing maps** [DISASSEMBLED]. 32 movement classes at `0x512358`, `0x20` bytes each:
`+0x00` present, `+0x04`/`+0x06` the class's footprint (`s16`, [INFERRED] from its use as the
refresh's margin), `+0x10`/`+0x14` the map's W and H,
`+0x18` the map — two bits a cell, `((H + 15) / 16) · W` dwords — and `+0x1C` the class tick.
`0x440940` builds them: for each present class it frees the old map, allocates the new one and
fills it (`0x440500`), and it runs once a load, at `0x4918E3` in `LoadGameData_Main`, after LoadMap
(`0x4918C0`): after the mask and before any saved feature is back. After that `0x440830` recomputes
them over a rectangle, each entry through `0x47E1F0`, and it has two kinds of caller:

- **grid changes**, through `0x440A40(xy, wh)` (stdcall, `ret 8`, `xy = x | y << 16`,
  `wh = w | h << 16`), which runs `0x440830` for each present class over
  `[x − fw, x + w] × [y − fh, y + h]` clamped to the map: `SpawnFeatureOnMap` (`0x424031`, with the
  anchor and FeatureDef `+0x94`), `FEATURES_Destroy` (`0x424822`), and the placement code at
  `0x47CEAD`, `0x47DB0A`, `0x47DB58`. The mask never calls it;
- **units**, over a unit's rectangle: `0x440A70` (`0x440AA3`, `0x440ACD`), `0x440AF0` (`0x440B45`,
  and `0x440B99` walking the unit array) and `0x440BE0` (`0x440BF9`). The per-cell rule reads the
  unit on the cell (`0x47E10C`) and compares its stamp, `[[unit]+0x26]`, with the class tick `+0x1C`
  (`0x47E121..0x47E13F`). `0x440940` rewrites `+0x10`, `+0x14` and `+0x18` but never `+0x1C`, which
  only `0x440B18` writes, so the class tick carries over from one load to the next in a process.

**The restore.** `0x424C00(tdf)` (stdcall, `ret 4`) has one caller, `0x43265A`, in the game-load
routine `0x432610`, which has one caller, `0x497B29` in the loader body (the condition is in
"LoadMap's feature placement, and a saved game"). `0x432610` restores, in order: `Players`
(`0x466050`), `Camera` (`0x41D2B0`), the features (`0x424C00`), `Metal`/`Plotmap` (`0x484D60`, the
save's byte into cell `+0x07` for every cell), `PlayerFeatures` (`0x484E80`, bits 3–6 of cell
`+0x0C`), `Mapping` (`0x484FA0`), `Units` (`0x486FD0`, at `0x432672`), `Meteor` (`0x438250`), then
`0x48FE60`. The features' TDF section maps the save's type
names to defs first (`Feature Type Names`, `0x424D23..0x424DE1`; a name not loaded goes to
`LoadFeature 0x4224B0`), then three loops place the records with `0x481550` and
`SpawnFeatureOnMap(cell, def, …, 0xA)`, and **none tests the spawn's result**:

| section | record | after the spawn |
|---|---|---|
| `Normal Features` (the spawn at `0x424FBF`) | 8 bytes: `x`, `y`, type, `+6` | `+6` into the cell's `+0x0A` (`0x424FC4..0x424FC9`) |
| `Animating Features` (`0x425050`) | 10 bytes: `x`, `y`, type, `+6`, `+8`, `+9` | by `+9`'s low nibble, the burn `0x4233A0(x, y, 0)` (`0x4250BB`), `FeatureDie(x, y, 0)` (`0x42509D`) or `FeatureDie(x, y, 1)` (`0x42507E`); then the wreck record the cell's `+0x0A` names takes `+6` at `+0x26` (`0x4250DF`), `+8` at `+0x04` (`0x4250E9`) and `+9`'s high nibble at `+0x2E` (`0x4250F4`) |
| `3D Features` (`0x425180`) | `0x1A` bytes: `x`, `y`, type, `+6`, the position and the turn (passed to the spawn by pointer) | the wreck record the cell's `+0x0A` names takes `+6` at `+0x26` (`0x4251A2`) |

The save writer `0x424890` sorts the anchors the same way: an `Animating` record for a GAF anchor
with its mark set (`0x4249CF` → `0x424A6E`), a `3D` record for a 3DO anchor (`0x4249DC`), a `Normal`
one for the rest. Metal and energy are the def's (FeatureDef `+0xEC`/`+0xF0`); no record carries
them.

**The defect.** On a saved game the grid is masked while it holds no feature, so a saved feature
whose anchor or any footprint cell the mask reaches is refused, where on the new game it stood:
the TNT's features are down before the mask there, and the mask leaves anchors. And the restore
carries on as though the spawn had succeeded: a refused `Animating` or `3D` record is written into
the wreck record that a word no spawn wrote names — LoadMap's grid comes from `0x4D83B0` (`W·H·13`
bytes at `0x48399D`, CRT malloc, filled only under the `-memset` command-line option: `setmemory`,
off by default, with the pattern `setvalue`, default `0xDEADBEEF`, `0x4D8200`/`0x4D8260`/`0x4D82C0`)
and its init loop `0x4839D5..0x4839ED` writes `+0x00`, `+0x02`, `+0x07`, `+0x08` and two bits of
`+0x0C` — which is a defect of its own, patched: "A saved feature's state and the wreck record its
cell names", below. MEASURED on the stock build, Two Continents (672 × 800 cells, 4 893 features),
a save at 14 s and its load: **51 features do not come back**, four in rows 0–2 — the trees at
(25, 0), (5, 1), (27, 2), (33, 2) — and 47 in rows 794–797, every one a one-cell feature that the
new game's mask had left standing, each cell reading `0xFFFD` after the load.

**The fix** (`fix_saved_features_border`, always on) retargets the call at `0x43265A` (its five
bytes compared first) to `features_restore_under_mask`. It opens the mask for the restore and
shuts it after: every cell holding `0xFFFD` is noted in a bitmap and set EMPTY, `0x424C00` runs
unchanged, and every noted cell that is then EMPTY or `0xFFFE` is set back to `0xFFFD` — the mask's
own rule over the mask's own cells, without recomputing which cells those are. Every grid change
the restore makes refreshes the pathing maps over the cells it changed (a spawn at `0x424031`, a
destroy at `0x424822`, both reading the mask open); the shut is the one change made without a
refresh, so each cell it rewrites is refreshed after it with `0x440A40(x | y << 16, 1 | 1 << 16)`,
which recomputes every class's entries over `[x − fw, x + 1] × [y − fh, y + 1]`: the entries whose
read region (`0x47E1F0`) holds that cell.

**The invariant**: the grid after the restore is the mask applied to the restored features, as a
new game's is the mask applied to the TNT's (a restored feature keeps its anchor on a masked cell
and loses its masked footprint cells; every other masked cell reads `0xFFFD` again), and the
pathing maps are that grid's. A map entry depends on the def words of its read region and on the
units standing there, and no unit is on the grid during the restore: the units are restored after
it (`0x486FD0`, at `0x432672`), and LoadMap's init zeroes every cell's `+0x00`. So an entry is
stale only if a cell of its region changed after its last computation. The open changes cells
without a refresh, but every opened cell ends either shut, and refreshed after the shut, or under a
feature a spawn placed, and refreshed over, after the open. It rests on a bound and an ordering.
The mask is opened only when the grid holds no feature (no def below `0xFFFB`, no `0xFFFE`) — the
state `0x424C00` is called in; a v2 map's void markers, `0xFFFC`, which LoadMap still places on a
saved game (`0x483ACA..0x483ADE`), are admitted — so no cell it opens is under a feature, every
anchor afterwards is a restored one, and nothing but the restore sees a cell open. The grid is read
as LoadMap sized it (`W · H`, `main+0x14233`/`+0x14237` refused outside 1..4096), and it is shut
only when its pointer and both counts are what they were before the restore. Everything runs on
the loader thread inside the level load, where the engine writes the grid and the pathing maps
itself. No main block, a grid out of range, a feature already down, or no memory for the bitmap
leaves the restore to stock, logged (`savedfeat: … the restore ran with the border mask shut`).
Identity on a new game, whose load never reaches `0x424C00`, and on every saved game with no
feature on a masked cell. The log line of a load it changes: `savedfeat: the restore ran with the
border mask open: 5938 cells opened, 5887 shut again and the pathing maps refreshed around each in
21 ms, 51 hold a restored feature's anchor` (Two Continents; 21–22 ms over two loads).

**MEASURED 2026-09-25**, Two Continents at 1024×768 on a private X server, a four-player
skirmish, with a diagnostic build (never committed) that writes the grid's 13-byte cells to a file
on a trigger:

- **Save and load**: 4 893 features at the save and after the load (4 894 with a wreck placed by
  `scenarios/one-wreck.json`), the 51 border features among them, and every def word identical;
  the grid is byte-identical with the grid at the save but for bit 2 of `+0x0C` on the cells the
  feature draw walked (`0x469936` sets it) where the camera stood before the save, which nothing in
  the restore writes. A save with no camera move since the game began was byte-identical in all
  13 bytes of all 537 600 cells.
- **The stock build's save, loaded by the fixed build**: byte-identical, all 13 bytes of every cell,
  with the stock build's grid at the moment it saved.
- **A new game** under the stock build and the fixed one: the def words identical cell for cell,
  and no `savedfeat:` line. The only bytes that differ are `+0x00`/`+0x01` and bit 1 of `+0x0C`
  under units that moved or were started between the two dumps.
- **The pathing maps**, by a diagnostic oracle that copies all 15 present classes' maps,
  recomputes the whole map with `0x440A40(0, W | H << 16)` and counts the dwords that change:
  nothing anywhere near a masked cell, on any load. 0 on a new game and on the first load of a
  process, under both builds. On a load after an earlier game in the same process — from inside it,
  or from the main menu after leaving it — 10 dwords of class 2 change at x 20–24, y 464–495, under
  the human player's commander, under the stock build and the fix alike.

**Multiplayer.** Neither fix is reached in a network game: the retail game has no saved multiplayer game.
`ARMOPT.GUI`'s handler `0x460CC0` greys `SAVEGAME` and `LOADGAME` in a game of type 3 (`cmp eax,3;
sete` at `0x460CF7` and `0x460D37`, then `0x4A1200` sets the gadget's `+0x13C` bit 0); the only
other writer of a save is the console's `Save` (the table row at `0x5020D8` → `0x417430`), at access
level 4, the debug level; and a load from the main menu rebuilds the game type from the save's
`Gametype` (`0x49267F` → `0x434AB0`).

**Not closed.**

- The class-2 difference after a load that is not the process's first is stock. [INFERRED] from
  the rule above: the class tick `+0x1C` survives the load, so the stamp test on a restored unit's
  cells reads a tick from the previous game.
- The `Normal Features` loop's write of `+6` into the spawn's cell (`0x424FC4..0x424FC9`) is a
  third write that tests nothing. Under the record gate's def test a stale `+0x0A` on a GAF anchor
  at rest or on an EMPTY cell is never taken for a record. But when the spawn is refused on another
  feature's footprint cell (`0xFFFE`), the write replaces that cell's offsets back to its anchor;
  with the same content a save's features do not overlap, so this needs the content changed between
  the save and the load. Not patched.

### A saved feature's state and the wreck record its cell names — `0x4250C0`, `0x425185` [DISASSEMBLED + MEASURED 2026-09-25]

**The defect.** After each `Animating Features` record's spawn (`0x425050`) and its sequence call
(`FeatureDie(x, y, 1)` at `0x42507E`, `FeatureDie(x, y, 0)` at `0x42509D`, the burn `0x4233A0` at
`0x4250BB`), the restore `0x424C00` writes the record's saved state into the wreck record that the
cell's `+0x0A` names (`0x4250C0..0x4250F4`: `+0x26` a word, `+0x04` a word — the sequence's frame,
inside the state `0x4B8B30` sets up at `+0x04` — and `+0x2E` the high nibble). After each `3D
Features` record's spawn (`0x425180`) it writes the record's `+6` word into `+0x26` of the record
`+0x0A` names (`0x425185..0x4251A2`). Neither block tests anything. `+0x0A` names the feature's own
record only while the feature holds one:

| the feature at the record's cell | `+0x0A` | who writes it |
|---|---|---|
| a 3DO anchor | its wreck record, for its whole life | `SpawnFeatureOnMap` at `0x423ED5`, after taking the record; with none left it abandons the spawn (`0x423DB9`/`0x423DDF`) |
| a GAF anchor whose sequence plays (cell `+0x0C` bit 0, the mark) | its wreck record, until the swap | `FeatureDie` at `0x42368B` (mark at `0x423695`); the burn at `0x42345C` (mark at `0x423468`) |
| a GAF anchor at rest | 0 | the spawn at `0x423EE9`; a `Normal Features` record's `+6` over it at `0x424FC9` |
| a footprint cell | the offsets back to the anchor | the spawn at `0x423F52` |
| an anchor `FEATURES_Destroy` removed — the def EMPTY | the index of the record it freed | nothing clears it: `0x4246B0` frees the record (`0x42476F`) and then clears the mark and sets the cell EMPTY (`0x424774..0x424780`); a spawn's own occupancy pass (`0x423D6A`) leaves such cells behind in the restore |
| a cell no spawn has written | what the allocator handed LoadMap | LoadMap's init loop `0x4839D5..0x4839ED` writes `+0x00`, `+0x02`, `+0x07`, `+0x08` and two bits of `+0x0C` |

So the saved state lands in a record the feature does not own whenever the record's feature did
not come back as it was saved.

- **Its spawn refused**: a void cell; a footprint past the map's edge (`0x423CF8`/`0x423D24`); an
  indestructible occupant (FeatureDef `+0xFF` bit 1: `FEATURES_Destroy` returns 0 at `0x42471E`);
  a masked cell before the border fix; a `3D` record's 3DO def with the pool empty (`0x423DDF` →
  `0x423DE7`). The write then goes through a word no spawn wrote. An anchor off the map is not on
  this list: `0x481550` hands the spawn NULL for it, and stock faults at `0x423D5F` before either
  site runs.
- **Its sequence not started**: on a full pool an `Animating` record's GAF spawn does NOT fail. It
  takes no record and leaves `+0x0A` at 0 (`0x423EE9`); `FeatureDie` then finds none (`0x42361B` →
  `0x42364C` → `0x423651`) and, under the `0x423651` fix, swaps the feature for its successor at
  once, and the burn returns (`0x42340D`/`0x423439`). The write lands on record 0, which on a full
  pool is live. A pool is full on a load when a save holds more records than the pool it is loaded
  into: a save from the raised build (8 192 records) loaded with stock limits (2 048).
- **A feature of the other kind** on the cell: the content changed between the save and the load (a
  name in `Feature Type Names` that now loads a 3DO def, `0x424D23..0x424DE1`), or, on a full pool,
  a swap's 3DO successor that destroyed a neighbour, freeing its record, and took it. An `Animating`
  write there puts a frame word over the 3DO record's `+0x04`, the pointer to its object state
  (`0x45A8D0`, from `0x423ECB`; freed by `0x45AAA0`), and a byte over `+0x2E`.

**What it hits.** MEASURED on Two Continents, with a diagnostic build (never committed) that logs,
at the restore's entry, how many cells have a nonzero `+0x0A`, and, on a trigger, empties the free
list (`main+0x1421B` = −1) for the restore, fills every record's `+0x04`, `+0x26` and `+0x2E` with
`0xA5`, compares the pool afterwards and puts it back:

- **The grid is zeroed**: 0 of 537 600 cells had a nonzero `+0x0A` word before the restore, on each
  of three saved loads, all from inside a running game. [INFERRED: the grid, 7 MB, is above the
  heap's threshold for a block of its own, which comes from `VirtualAlloc` zero-filled; the
  teardown frees it at `0x483E7E`, so every load gets a fresh one. Under `-memset` the grid is
  filled with the pattern instead, and the index is two of its bytes, far past the pool.]
- **So the stray write lands on record 0.** A save holding one wreck (`armlab_dead`, placed by
  `scenarios/one-wreck.json`) loaded with the free list empty: the 3D spawn was refused, and the
  build without this fix wrote the record's `+6` (0) over free record 0's `+0x26` — `A5 A5 → 00 00`,
  the only change in the pool of 8 192.
- Record 0 is the first record a load hands out, since the pool's free list starts there (the free
  head read 0 before both diagnostic loads). An `Animating` record whose sequence did not start
  overwrites the frame, `+0x26` and `+0x2E` of the feature that holds record 0; a refused `3D`
  record overwrites its `+0x26`. A grid block that reused freed memory would instead reach any
  record, or up to `0xFFFF · 0x30` bytes (3 MB) past the pool's base; no load measured got one.

**The fix** (`fix_restore_record_owner`, always on): a jump at `0x4250C0` and at `0x425185` over the
6-byte `mov edx,[0x511de8]` each block begins with (compared first, with the 6 bytes after it), to
a stub that pushes the site's kind and runs the block as stock only when
`restore_record_owned(cell, kind)` holds: the cell lies in the grid LoadMap sized, it is an anchor
below the def count `main+0x14253` of the record's own kind — for an `Animating` record FeatureDef
`+0xFE` bit 0 set (GAF) with the cell's mark set, for a `3D` record bit 0 clear (3DO) — and its
`+0x0A` is below the pool's count, read from the pool's allocation size (the dword at `0x421F2A`,
`0x18000` in stock, which the limits table rewrites at attach). That is the rule the save writer
sorts its records by (`0x4249CF` → `0x424A6E`, `0x4249DC`). Otherwise the stub leaves for the
loop's next record, `0x4250F7` or `0x4251A7`, where every register but `edi`, `ebx` and `ebp` is
reloaded. Branches into the sites land only on `0x4250C0` itself (`0x425065`, `0x425083`,
`0x4250A2`), none inside either stolen instruction [rel8/rel32 scan of `.text`]. Both sites are
rows of the fail-closed table, so both are written or neither is.

**The invariant**: the restore writes a saved state only into the wreck record that the feature on
the record's cell owns, of the kind the state was saved from — the record it took in this restore —
and never outside the pool. It rests on the engine's own ownership rule, a lifetime: an index in an
anchor's `+0x0A` is a live record exactly while the anchor is 3DO, or GAF and marked.
`FEATURES_Destroy` frees the record and, in the same call, clears the mark and sets the anchor
EMPTY; the swap frees through it (`0x423792`/`0x4237B1`); the index left in `+0x0A` is refused by
the def test. And it rests on a bound on every value read. A record whose feature did not come back
as saved loses its saved state, which has nowhere to go. The kind test cannot refuse a record of a
save loaded with the content it was saved with and a pool as large: the writer put each record in
the section its anchor's kind and mark chose, and the restore rebuilds that anchor.

**Why not the other shapes.** Gating the write on the spawn's success does not hold on the
full-pool path: an `Animating` record's GAF spawn succeeds and takes no record, and it is the
sequence call that takes one. With the pool full that call starts nothing, so the anchor's `+0x0A`
is the spawn's 0 and a success-gated write still lands on record 0. Bounding the index by the
pool's count alone keeps the write inside the pool, and still lands it on record 0, another
feature's.

**MEASURED 2026-09-25**, same diagnostic, Two Continents: the save with the wreck loaded with the
free list empty — without this fix: one free record written (record 0, `+0x26`); with it: the gate
refused the one record and no record was written. Loaded normally, the gate let the one `3D` record
through (`3D 1 allowed, 0 refused`), and the wreck's anchor read record 0 at the save and after the
load; the grid is as in "A saved game's features on the map's border". Nothing in the restore
writes bit 2 of `+0x0C`: the spawn writes bit 0 and bits 3–6 of the anchor (`0x423EF1`,
`0x423F07`) keeping bit 2 (`and cl,0x87` at `0x423F00`), and bit 0 of its footprint cells
(`0x423F55`). The kind test is argued from the save writer's rule above, not measured: it cannot
fire on a save loaded with its own content.

**Not closed.** `FeatureDie` follows a `0xFFFE` cell to its anchor (`0x423568..0x42358D`), so a
refused `Animating` record of mode 1 or 2 whose cell is an indestructible neighbour's footprint
starts or swaps that neighbour's sequence. That is stock; the gate skips only the write.

### The wind — `0x490C40`, its draws `0x490C5A` and the level load's call `0x491903` [DISASSEMBLED + MEASURED 2026-09-25]

**The updater.** `0x490C40()` takes no argument and returns with `ret`. It has exactly two callers:
the level load `LoadGameData_Main 0x4917D0` at `0x491903`, on the loader thread, and the tick
`0x495490` at `0x49558F`. `0x4917D0`'s only caller is `0x497581`. The updater compares `next`
(`main+0x37EC4`, a dword) with GameTime (`main+0x38A47`) (`0x490C45..0x490C53`). While
`next >= GameTime` it clears the changed flag `+0x37EE2` and returns (`0x490D87`). Otherwise it
draws three values:

| value | stock's draw | where |
|---|---|---|
| `next` `+0x37EC4` | `next += 30 · (5 + rand() · 10 / 0x8000)`, from the CRT's `rand 0x4E4870` through `_allmul 0x4E4400` and `_alldiv 0x4E4440` | `0x490C60..0x490C8E` |
| speed `+0x37EDA` (dword) | `min + simrand(max − min)`, where min is `main+0x1425B` and max is `main+0x1425F` (the map's minimum and maximum wind [INFERRED]; Two Continents reads 0 and 3000); the difference is a 32-bit `sub` at `0x490CA1` | `0x490C90..0x490CB7` |
| heading `+0x37ED8` (word) | `simrand(0x10000)`, drawn only when the speed is not 0 | `0x490CC8..0x490CDC` |

`simrand` is the sim RNG `0x4B6C30(n)`, which answers 0 for `n < 2` (`0x4B6C35`). The
updater then derives the components `+0x37ECC` and `+0x37ED4` from the speed and the heading
(`0x490CE8..0x490D35`), and the ratio `+0x37EDE`: the speed over `+0x37EC8`, capped at 1.0
(`0x490D3B..0x490D7B`). `+0x37EC8` is 5000, written only by the load (`0x4918ED`). It sets the changed flag to 1 (`0x490D7B`) and pops `esi` (`0x490D85`).

**The writers and readers.** The only writers of `+0x37EC4`, `+0x37EDA` and `+0x37ED8` are
`0x490C8E`, `0x490CB7`, `0x490CDC` and the load's `mov [ecx+0x37EC4],esi` at `0x4918FD`, which sets
`next` to 0: `esi` is 0 from `0x4917E5`. The components `+0x37ECC` and `+0x37ED4` are written only
by the updater (`0x490D09`, `0x490D35`). Besides the updater's own derivations, the readers are:

| reader | what it reads | what it does with it |
|---|---|---|
| the projectile pass `0x49B720`, called from the tick at `0x495513` | the three dwords from `+0x37ECC` (x, y, z; nothing writes `+0x37ED0` by name) | adds them to a projectile's position after its velocity, every tick (`0x49BC58..0x49BC62` → `0x49BD10..0x49BD2D`, and `0x49BD04..0x49BD2D`), on the branches of `0x49BC0C` and `0x49BCD9` (which weapon kinds take them was not traced) |
| the fire spread `0x4239C0` [role INFERRED], called from `0x424463` | `+0x37ECC` (`0x423AA1`) and `+0x37ED4` (`0x423AC1`) | scales each by `0x20000` (`_allmul 0x4E4400`, then `0x4E43D0` with 16) and adds it to a position |
| four effect handlers | `+0x37ECC` at `0x474B09`, `0x474FC9`, `0x475366`, `0x475626`, and `+0x37ED4` after each (`0x474B3A`, `0x474FFA`, `0x475391`, `0x475651`) | drift [INFERRED: smoke and particles] |
| `0x40156F`, in the resource step [INFERRED] | the ratio `+0x37EDE` | the ratio times the def's `WindGenerator` (`+0x1D2`, a float) is added to the player's energy income |
| `0x488F68` | the ratio | returns the same product, negated |
| `0x437910(unit)`, called from `0x48ADC4` | the changed flag, the heading, the speed | for a def with `WindGenerator > 0`, and only on a draw where the changed flag is set, calls the unit's script `SetDirection(heading)` and `SetSpeed(speed << 4)` through `0x4B0A70` (the names at `0x50500C` and `0x505000`) |
| `0x409B90` | `+0x37EC8` (5000) and the map's maximum `+0x1425F` | zeroes a wind generator's weight when the map's maximum is below half of 5000 [INFERRED: the AI's build weighting]; the drawn wind is not read |

So the wind is simulation state: it moves projectiles, and so where they land, it moves the fire
spread, it is each wind generator's income, and it is the argument of two COB calls that run on
every peer. These readers are what make the fix a simulation fix.

**Each peer draws its own.** The two engine RNGs are both local to a machine:
- the sim RNG `0x4B6C30` keeps its state at `0x51FC88`, one for the process, seeded by the loader
  `0x497180` from `QueryPerformanceCounter` (`0x49718C`, then `0x4B6CA0` at `0x49719D`);
- the CRT's `rand 0x4E4870` keeps its state per thread, at `+0x14` of the per-thread data
  `0x4EB0F0` returns, and `srand 0x4E4860` writes only the calling thread's (see *The C
  runtime's `rand` is a second, separate stream*, under the tacob value ids). The loader's `srand(time(0))` at `0x4971AE` seeds the loader thread's stream. The
  tick's draw at `0x490C60` runs on the game thread, whose stream WinMain seeds with
  `srand(time(0))` at `0x49E8BB`.

The updater runs on every peer, so each peer draws its own wind. The schedule `next` diverges on
every map, so the changed flag, and with it `0x437910`'s `SetSpeed` and `SetDirection`, fires on
different ticks on different peers. The heading diverges on every map whose speed is not 0: with
a range below 2 and a minimum above 0 the speed is the minimum on every peer, but `0x490CC8`
tests the speed, not the range, so the heading is still drawn. MEASURED on the previous build, two
peers on Two Continents (wind 0 to 3000) paused at GameTime 825:

| peer | next | speed | heading |
|---|---|---|---|
| host | 990 | 2525 | `0xF5C6` |
| joiner | 840 | 1498 | `0xC827` |

The load's call draws nothing: it finds `next` = 0 and GameTime 0 (`0x4971BB`). The first draw
is the first tick's.

**The fix: one wind for every peer.** `fix_wind` (`tagpu_patches.c`) writes four rows of the
fail-closed table that change code:
- a `jmp` at `0x490C5A` replaces `lea esi,[eax+0x37EC4]`, the draws' first instruction, after
  `0x490C51`'s `cmp ecx,edx` (`next` against GameTime), `0x490C53`'s `jae 0x490D87` and
  `0x490C59`'s `push esi`. It goes to a stub `pushad; call wind_draw; popad; mov eax,[0x511DE8];
  jmp 0x490CE8`, which leaves `eax` as stock's `0x490CE3` leaves it. After `0x490CE8`, `esi` is
  read only by the pop at `0x490D85`.
- a `call` at `0x491903` goes to a stub that calls `wind_seed` and then jumps to `0x490C40`.
- a `call` at `0x4982CA`, the loader thread's start, goes to a stub `pushad; call
  wind_session_capture; popad; jmp 0x4B6B20`, so `0x4B6B20` runs with stock's stack and returns
  to `0x4982CF`.
- a `jmp` at `0x4C98FD` replaces the engine's one `SetSessionDesc` call, nine bytes (`push 0;
  mov ecx,[esi]; push eax; push esi; call [ecx+0x7C]`, NOP-padded after the `jmp`), and goes to
  a stub `push eax; push esi; call wind_set_session_desc; jmp 0x4C9906`: `eax` is the descriptor
  `main+0x471` and `esi` the interface `main+0x4D9`, and the wrapper is `__stdcall` with two
  arguments, so it returns to `0x4C9906` with stock's stack and the `HRESULT` in `eax`
  (*The session, not the host*, below).

`wind_draw` makes stock's three draws, by stock's rules, from a splitmix64 generator of its own.
It uses neither engine RNG:
- `next += 30 · (5 + r % 10)`;
- the speed is `min + r % (max − min)`, or `min` when the 32-bit difference is below 2;
- the heading is `r % 0x10000`, drawn only when the speed is not 0.

Five more rows check the bytes the stubs rest on, and the fix adds nine rows in all:
- `0x490C40`, 26 bytes: the schedule test and its `push esi`;
- `0x490CE8`, 13 bytes: the loads where the draws rejoin;
- `0x4982C5`, 5 bytes: `push 0x497C70`, so the call hooked at `0x4982CA` is the loader's;
- `0x4C98D9`, 36 bytes: `lea eax,[esi+0x45D]` … `mov esi,[esi+0x4C5]; mov dword [eax],0x50`,
  so `eax` and `esi` at `0x4C98FD` are the descriptor and the interface;
- `0x4C9906`, 4 bytes: `test eax,eax; jne 0x4C9914`, where the wrapper rejoins.

**The seed.** `wind_seed` runs at every level load, so nothing crosses from one game to the next.
The engine's own network test chooses its path: `GameingState +0` (through `main+0x391E9`; `0x435100`
is `mov eax,[ecx]`) is 1 in a campaign, 2 in a skirmish and 3 in a network game, the dispatch the
game start makes at `0x4971C7`.
- **In a network game** the seed is FNV-1a 64 over DirectPlay's session instance GUID
  (`guidInstance`) and then the map's hash, and nothing local to a peer is read.
- **When DirectPlay's answer is refused** in a network game (no interface, `GetSessionDesc`
  failed, or it answered with a `dwSize` other than 0x50 or a null GUID), the seed takes the
  engine's copy `main+0x479` in its place when that is not null, counted (`network levels seeded
  from the engine's session copy`), and otherwise the map's hash alone, counted (`network levels
  seeded from the map alone`); both are logged with the refused answer's `HRESULT` and `dwSize`.
- **Outside a network game** the seed is `QueryPerformanceCounter`, as stock seeds its own RNG.

#### The engine's DirectPlay object — `main+0x14` [DISASSEMBLED 2026-09-25]

The executable imports `DPLAYX.dll` by ordinal only: 2 (`DirectPlayEnumerateA`, thunk `0x4FAFF0`),
4 (`DirectPlayLobbyCreateA`, `0x4FAFF6`) and 1 (`DirectPlayCreate`, `0x4FAFFC`, called at
`0x4CA667` and `0x4CA922`). The functions that use it trace their names through `0x4C9740`:
the strings `HAPINET_receivepacket` … `HAPINET_getplayeraddress` sit at `0x50B0AC..0x50B4A0`, one
more copy of `HAPINET_receivepacket` at `0x507264`. Each takes the object as its first stack
argument, and the engine passes `main+0x14` (`add edx,0x14` at `0x451203` before
`HAPINET_updategameinfo 0x4C9890`). Its fields, as offsets of `main`:

| `main+` | HAPINET `+` | what | written by |
|---|---|---|---|
| `0x451` | `0x43D` | the application GUID | `HAPINET_initmultiplay 0x4CA5D0`, from its argument |
| `0x461` | `0x44D` | the service provider's GUID, `DirectPlayCreate`'s first argument | `0x4CA5D0` |
| `0x471` | `0x45D` | a `DPSESSIONDESC2` (0x50 bytes), its `dwFlags` at `main+0x475` and its `guidInstance` at `main+0x479` | see below |
| `0x4D5` | `0x4C1` | the `IDirectPlay` from `DirectPlayCreate` | `0x4CA649` (zero), then `DirectPlayCreate` through its out pointer (`0x4CA667`); released and zeroed by `HAPINET_uninitmultiplay 0x4C9B70` (`0x4C9BC3`) |
| `0x4D9` | `0x4C5` | **the interface every call goes through**: an `IDirectPlay3A*`, or an `IDirectPlay2A*` when a lobby launched the game | `0x4CA64B` (zero); `QueryInterface(IID_IDirectPlay3A)` at `0x4CA684`, the IID at `0x4FCD78` (`{133EFE41-32DC-11D0-9CFB-00A0C90A43CB}`, as in `dplay.h`); the lobby's `Connect` at `0x4C9B59` (`IDirectPlayLobby` slot 3, `+0xC`, whose out parameter is an `LPDIRECTPLAY2`); `Close` (`0x4C9B92`), `Release` and zeroed at `0x4C9BA1`. `IDirectPlay2`'s table has 32 slots, `SetSessionDesc` (`+0x7C`) the last, and `GetSessionDesc` is slot 22 in it as in `IDirectPlay3`'s (`dplay.h` lines 561 and 677). Two engine calls go past it into `IDirectPlay3`'s own slots: `EnumConnections` (`+0x8C`, `0x4CA30D`) and `InitializeConnection` (`+0x98`, `0x4CA5B3`). The first always runs on a 3A: its function `0x4CA250` (one caller, `0x44471C`, in the provider screen's setup `0x444580` right after its `uninitmultiplay` at `0x4446F4`) calls `initmultiplay` first (`0x4CA2C2`) and returns unless it succeeded, and `initmultiplay` stores the `QueryInterface(IID_IDirectPlay3A)` result here and succeeds only on `S_OK` (`0x4CA686..0x4CA68C`). The second's wrapper `0x4CA590` tests nothing; its one caller is `0x450DBB` in `0x450D80`, which is called from `0x426D20` (no direct reference to it found) and from the state machine `0x426E80` on `main+0x2BBE` (`0x427AEA`, `0x427BF7`; called at `0x496BB5`). Whether that path can run while a lobby's `IDirectPlay2A` is here was not traced: after a lobbied launch the engine's calls stay inside the 2A's table only if it cannot [OPEN] |
| `0x4E1` | `0x4CD` | the `IDirectPlayLobby` | released and zeroed at `0x4C9BD9` |
| `0x4E5` | `0x4D1` | the lobby's `DPLCONNECTION*`, 0 when not lobbied | freed and zeroed at `0x4C9C08` |
| `0x4F1` | `0x4DD` | `dwMaxPlayers` for a new session (16, `0x4C9C31`) | |

The `HAPINET_*` functions read here test `+0x4C5` for NULL before calling through it, and its
writers are the four above: nothing else in the image writes `main+0x4D9`. `uninitmultiplay` has four callers (`0x4446F4`, `0x450E06`, `0x4523C8`, `0x46C1B2`),
`initmultiplay` one (`0x4CA2C2`); the thread they run on was not traced past the shell and the
network module [INFERRED: the game thread].

**The session description's writers.** The copy at `main+0x471` is written:
- by `HAPINET_createnewgame 0x4C9920`, on the host: zeroed (`0x4C995E`), filled, then
  `Open(DPOPEN_CREATE)` (vtable `+0x60`, `0x4C99DB`); then `GetSessionDesc` (`+0x58`) twice, once
  for the size (`0x4C99F8`, expecting `DPERR_BUFFERTOOSMALL 0x8877001E`) and once into a buffer,
  and on success its `guidInstance` is copied to `main+0x479` (`0x4C9A33..0x4C9A4C`). A failed
  `GetSessionDesc` leaves the GUID zero;
- by `HAPINET_joingame 0x4C9FD0`, on a joiner: zeroed, the GUID of the session to join (its
  by-value argument) stored at `main+0x479` (`0x4CA03A`), `Open(DPOPEN_JOIN)` (`0x4CA05A`), then
  `GetSessionDesc` (`0x4CA07D`) and its `guidInstance` copied again (`0x4CA0B8..0x4CA0CC`);
- by `HAPINET_createorjoinlobbygame 0x4C9A70`, when lobbied: the lobby's `DPLCONNECTION`
  `lpSessionDesc` copied whole (`0x4C9B4F`, `rep movs` of 0x14 dwords), on the host as on a joiner;
- by the network pump, on every peer but the host: a `DPSYS_SETSESSIONDESC` (`0x104`, dispatched
  at `0x454611..0x45461D`) is copied whole over it (`0x454689..0x454697`) when the local seat is
  not the host seat;
- `0x451180` sets `DPSESSION_JOINDISABLED` (0x20) in `main+0x475` when bit 4 of the local
  PlayerInfo's `+0x9B` is set (`0x4511D2..0x4511DA`), as does the caller at `0x454135`
  (`0x4540F0..0x4540F8`).

Nothing of the wind fix writes it: the fix's `SetSessionDesc` passes a copy of it (below).

**It reaches the session through `SetSessionDesc`, at one site.** `HAPINET_updategameinfo
0x4C9890` hands the whole copy, `guidInstance` included, to `SetSessionDesc` (`+0x7C`,
`0x4C9903`). It is the engine's only `SetSessionDesc`: the image has six calls through a vtable's
`+0x7C` (`call [reg+0x7C]`), and `0x4C9903` is the only one through `main+0x4D9` and the only one
with three arguments; `0x47C0CB`, `0x4B4FB8`, `0x4B5735`, `0x4B6069` and `0x4B60E1` push two, the
shape of `IDirectDrawSurface::SetPalette` [INFERRED]. No register call in the image takes its
target from a `+0x7C` load. `0x4C9890` has two callers: `0x451180` (`0x451208`; thirteen call
sites of its own: twelve at `0x444E48` … `0x44AB32`, the battle room's handlers [INFERRED], and
`0x497C0B`) and the pump `0x453D40` (`0x454135`, inside it). The function tests the interface
for NULL (`0x4C98A9`, to `0x4C9914`), fills `dwUser1`..`dwUser4` and `lpszSessionName`
(`0x4C98C2..0x4C98EB`), sets `dwSize` 0x50 (`0x4C98F7`), and returns 1 when the call answers
`DP_OK` (`0x4C990A`) and 0 otherwise (`0x4C9914`), `ret 0x1C`.

**Every load ends with one**, on the loader thread, in every game mode: `0x497BFF` sets bit 4 of
the local PlayerInfo's `+0x9B` and `0x497C0B` calls `0x451180`, which then sets
`DPSESSION_JOINDISABLED`. No branch in the loader body skips `0x497BFF..0x497C0B` and its one
`ret` is `0x497C6C`, so every load reaches the call, and it reaches DirectPlay whenever
`main+0x4D9` is set. MEASURED after a two-peer load: `main+0x475` reads `0x20` on both peers.
DirectPlay accepts it only from the session's host. An implementation that takes
`guidInstance` from it (Wine's `DP_SetSessionDesc` copies the whole descriptor) moves the
session to whatever the descriptor holds.

**The copy's GUID is read** by `SetSessionDesc` above; by `HAPINET_getgames 0x4C9E50`, which
copies it into the descriptor it hands `EnumSessions` (`0x4C9EC1`), in the shell's session list;
and through the getter `0x4CA9E0` (`add eax,0x465`) by `0x46C620` (called at twelve sites,
`0x41F897` … `0x46BC65`, and `0x4978D0` in the loader body, so this read also runs on the loader
thread), which passes it to `RIReport` of `reporter.dll` (the pointer `0x51E584`, resolved at
`0x46C07A` from the names at `0x507AF8` and `0x507AB8`) or to the stub `0x4CAA10` (`mov eax,1;
ret 0x28`). The retail install ships no `reporter.dll`. The other accesses in the copy's range
are other fields: `dwFlags` (`HAPINET_passwordrequired` at `0x4CA479` tests 0x400),
`dwMaxPlayers` (`main+0x499`, `0x447C8A`, `0x447D04`) and `dwUser2`/`dwUser3` (`main+0x4B5`,
`+0x4B9`: `0x44FA71`, `0x463177..0x463189`, `0x46353E..0x463599`).

`HAPINET_getcurrentplayers 0x4C9DD0` also calls `GetSessionDesc` (`0x4C9DFC`, `0x4C9E2A`) and
returns `dwCurrentPlayers`. `createnewgame` leaves `dwFlags` 0 (nothing writes it after the zeroing
at `0x4C995E`), so the sessions it creates have neither `DPSESSION_MIGRATEHOST` nor
`DPSESSION_KEEPALIVE`; a lobbied session takes the lobby's descriptor whole, flags included
(`0x4C9B4F`).

#### The session, not the host [DISASSEMBLED + MEASURED 2026-09-25]

**Why not the host's ID.** The host seat can change during the load. Until bit 1 of
`main+0x38D75` is set, the game thread runs the network pump `0x453D40` in its load loop
(`0x4984DD..0x49852E`, the call at `0x49852E`). The pump's leave case (`0x4550A0..0x4550D5`)
calls the player removal `0x452CC0` for a remote human even during the load. The removal clears
the seat's type (`0x463C60(0)` at `0x452E62`), sets its DirectPlay ID to −1 (`0x452E6D`), and, if
the leaver held the host bit and bit 2 of `main+0x2A44` is set, elects a new host: the human seat
with the highest ID (`0x452EE3..0x452FF8`). So a peer that handles a departure before its
`0x491903` would seed from the new host, one that handles it after from the old, and one in the
middle of the removal could read a set type beside an ID of −1. The winds would then differ for
the rest of the game, silently.

**The GUID has the property the host's ID lacks.** DirectPlay fixes a session's instance GUID when
the session is created, and every peer holds it from the moment it enters: a joiner names it to
`Open(DPOPEN_JOIN)` (`0x4CA03A`, `0x4CA05A`). No departure writes it, and the engine's host
election touches only its own seat records. The one engine call that could move it is
`SetSessionDesc`, under an implementation that takes `guidInstance` from it: the copy holds
DirectPlay's own GUID after a create or a join (`0x4C9A33`, `0x4CA0BB`), but a lobby's descriptor
after a lobbied launch (`0x4C9B4F`), and every peer makes the call at the end of every load, the
host in the battle room as well, after the joiners may have captured.

**So the engine's one `SetSessionDesc` passes the GUID DirectPlay holds.** The stub at `0x4C98FD`
hands the interface and the descriptor to `wind_set_session_desc`, which calls `GetSessionDesc`
through the same interface, validated as the capture validates it (below), copies the engine's
0x50-byte descriptor into its own frame, writes DirectPlay's GUID into that copy, counted when the
engine's differed (`made over another GUID`), and calls `SetSessionDesc` with it and stock's
flags 0 (`made`). With no valid answer it makes no call and returns the refusal (`withheld`),
which `0x4C9908` takes as it takes a failed call: `0x4C9890` returns 0. Every `SetSessionDesc`
the engine makes, in the battle room or the load, lobbied or not, on whichever thread reaches
it, then names the session's own GUID, whatever the implementation does with the field. The
engine's copy is not written, so its other readers above read what stock leaves there. The
`GetSessionDesc` runs right before stock's own `SetSessionDesc`, on the thread making it, so it
adds no DirectPlay concurrency stock does not already have. `ecx` and `edx` come back clobbered
as the COM call clobbers them, and `0x4C9906..0x4C9918` reads neither. No branch and no
absolute reference in the image lands inside `0x4C98FE..0x4C9905`. `0x4C98FD` is also where TADR's
`NullLpszPasswordInUpdateGameInfo` hooks, not ported ([the B evidence](../tadr-port/sim-fixes-evidence.md),
§3); the fail-closed table refuses two rows on one byte, so a port of it writes `lpszPassword` in
the wrapper's copy.

**How it is read.** `wind_session_capture` calls `GetSessionDesc` through `main+0x4D9` (vtable
`+0x58`, slot 22 of `IDirectPlay2A` and of `IDirectPlay3A` in `dplay.h`: `QueryInterface`,
`AddRef`, `Release`, then `AddPlayerToGroup` … `GetPlayerName`, then `GetSessionDesc`), into a
0x400-byte stack buffer or, when DirectPlay asks for more, a heap one, each zeroed first so an
answer that writes less than a descriptor cannot pass on a previous frame's bytes. It keeps the `guidInstance`
only when the answer's `dwSize` is 0x50 and the GUID is not null: Wine can answer `DP_OK` with a
zeroed descriptor. It runs on the **game thread**, from the stub at `0x4982CA`:
- `0x4982C3..0x4982CA` is `push ebp; push ebp; push 0x497C70; call 0x4B6B20`, the CRT's
  `_beginthread` over `CreateThread` (see *Thread creation*). `0x497C70` is referenced only by
  that push (`0x4982C6`, the image's one absolute reference to it), and the loader body
  `0x497180` is called only from `0x497CA1` inside it, so every seed at `0x491903` follows the
  capture made for its own load, and the thread's creation orders the capture's stores before
  anything the new thread reads.
- At that call the loader thread does not exist yet. Once it runs, stock lets it call DirectPlay
  concurrently with the game thread: the loader's wait pumps the network at `0x49727D` while the
  game thread's load loop pumps at `0x49852E`, and the loader makes the `SetSessionDesc` at
  `0x497C0B`. A call on the game thread before the loader starts adds no concurrency that stock
  does not already have.

The engine's copy `main+0x479` is the capture's second choice, taken only when DirectPlay's answer
is refused: the pump rewrites `main+0x471` whole from every `DPSYS_SETSESSIONDESC` on every peer but
the host (`0x454679`, `0x454689`), a lobbied host's copy is the lobby's (`0x4C9B4F`) rather than
DirectPlay's, and a host whose `GetSessionDesc` failed at creation holds zero. It is read at the
capture, on the game thread, and logged beside DirectPlay's answer. `GetSessionDesc` fails only
with no interface or no open session, and a peer with no open session exchanges no game traffic,
so its wind changes nothing another peer sees.

**The host seat, for the record.** `0x456850()` takes no argument and returns a seat in `al`, or 10
for none: the first seat, 0 to 9, whose type `+0x73` (records at `main+0x1B63`, stride `0x14B`)
is not 0 and whose PlayerInfo (the pointer at the record's `+0x27`) has bit 0 of `+0x97`. The
load waits for it only when bit 1 of the local record's `+0x21` is set: `0x49723B` tests it and
jumps to `0x4972BA` otherwise. The wait (`0x49724D..0x4972AB`) pumps the network through
`0x453D40` (`0x49727D`) and sleeps 50 ms while `0x456850` answers 10, or while the controlled seat's
PlayerInfo (`[record(main+0x2A42)+0x27]`, loaded at `0x49724D..0x497261`; `+0x2A42` is the
controlled player by *The order-marker chain*) reads `0xFF` at `+0x96`
or 0 at `+0x8F` (`0x49729A`, `0x4972A3`). From `0x4972BA` the load takes the map from the seat `0x456850`
answered at `0x497213` (`0x4972D6`, `0x435A20`), which past a skipped wait can be 10, and the unit
limit from the seat it answers at `0x4972DB` (`0x4972E8..0x4973B5`, skipped when it answers 10).
The same bit decides who damages a feature (`0x4244B0`, *Who sends a feature hit*). The seat index
differs from peer to peer: MEASURED on `23b6b8b`, 0 on the host and 1 on each joiner, with two
peers and with three.

**The host bit's writers** (PlayerInfo `+0x97`, bit 0):

| site | what |
|---|---|
| `0x45156C` | sets it on the controlled player's PlayerInfo (`main+0x2A42`) |
| `0x451334` | writes it on a new seat's PlayerInfo as the argument `bl` is (`xor bl,cl; and ebx,1; xor ebx,ecx`), set when the seat being added is the current host seat (`0x45126F..0x451334`) |
| `0x452FF8` | sets it on the seat the host election chooses, the first whose ID equals the one the loop at `0x452F89` searches for, when the host leaves |
| `0x45035D` | sets it the same way (the loop at `0x4502EE`), inside `0x450240`, which has no call, no jump and no absolute reference anywhere in the image: dead |
| `0x45193A`, `0x4280F3` | copy bit 1 of the lobby's `DPLCONNECTION` `dwFlags` (`[main+0x4E5]+4`, `DPLCONNECTION_CREATESESSION`) into it |
| `0x451943`, `0x428117` | clear it when there is no lobby connection (`main+0x4E5` is 0) |
| `0x46460F` | clears it, in a loop over the seat records (stride `0x14B`) |

The nine word writes at `0x418D16`, `0x418DD6`, `0x418E96`, `0x418F56`, `0x419016`, `0x4190E6`,
`0x419189`, `0x41922C` and `0x4192CF` rewrite the word with one of bits 1, 2, 3, 5 or 6 toggled
and bit 0 kept. No setter tests the seat's type, so an AI seat that carries the bit is the seat
`0x456850` returns. The seed no longer reads it.

**The map** is FNV-1a over the lower-cased stem of the TNT path, `GameingState +0x204`:
"Maps\Two Continents.TNT" on both peers, hashed as "two continents". `GameingState` is not NULL at
`0x491903`: the loader body dereferences it at `0x4971CD` (`0x435100` is `mov eax,[ecx]`) on the
only path into `0x4917D0` (`0x497CA1` → `0x497581`). Its writers are `0x434AB0(mode)`, which
stores a new object or NULL (`0x434B64`, `0x434B78`), and `0x434B90`, which frees it and stores
NULL (`0x434BE3`, called at `0x4917BF`).

**The ordering.** The GUID is captured on the game thread before the loader thread is created.
The seed is written on the loader thread before the loader's last store, bit 1 of `main+0x38D75`
(`0x497C62`). The tick reads it only after the game thread has seen that bit (`0x498342`).

**MEASURED on the new build.** The values in each row were read on every peer, and are identical
on every peer:

| build, game | seeded from | GameTime | next | speed | heading |
|---|---|---|---|---|---|
| `1a0c599`, game 1 | the ID `0x4961C0AB`, map hash `0x4934CBDE` | 795 / 796 | 930 | 1983 | `0x3E40` |
| `1a0c599`, game 1, later | | 2003 / 2006 | 2220 | 1027 | `0x2643` |
| `1a0c599`, game 2, same processes (the level counter reads 2) | `0x4962F926` | 795 | 1170 | 2632 | `0x6CAC` |
| `1a0c599`, game 3, joiner restarted (host level 3, joiner level 1) | `0x4964DA8F` | 796 | 960 | 556 | `0xABFA` |
| `23b6b8b`, two peers, Two Continents | host seat's `0x498AE486`, `0x4934CBDE` | 789 / 790 | 810 | 2745 | `0xB128` |
| `23b6b8b`, three peers, Town & Country (wind 25 to 5000) | host seat's `0x498FABE4`, `0x29FE9EA5` | 797 / 795 / 795 | 870 | 1598 | `0x387C` |
| `0885f46`, two peers, Two Continents | `{952E3FFB-DDEA-4B26-8BC6-210AD05BCACB}`, `0x4934CBDE`: seed `0x77A98479B8F3B818` | 1262 | 1350 | 1627 | `0xB693` |
| `0885f46`, three peers, Town & Country, the host killed after its seed | `{D3E2100E-59EB-45F3-8124-DA6E4518236B}`, `0x29FE9EA5`: seed `0x14CE9DAA1CF3E1B0` on all three | 1131 / 1137 (the two joiners) | 1530 | 2834 | `0x0616` |
| `ffadca6`, two peers, Two Continents | `{DE8782F9-D9D7-47A4-90D0-C09C60D4941D}`, `0x4934CBDE`: seed `0x0674C957184EEC35` | 265 / 320, running | 420 | 433 | `0x355C` |

Notes on the rows:
- On `1a0c599` the host was found by PlayerNum 1. That build's draws are the ones `23b6b8b` makes.
- The ratio `+0x37EDE` also agreed on every peer: `0x3ECB0F28` in game 1, `0x3F0C8B44` (0.549)
  and `0x3EA3A29C` (0.3196) on `23b6b8b`, `0x3EA69AD4` and `0x3F1119CE` with the session GUID.
- The components agreed as well: 5122 and 1976 with two peers, −3142 and −584 with three on
  `23b6b8b`; 3166 and 752, and −832 and −5606, with the session GUID.
- On every peer of the three session-GUID games the engine's copy `main+0x479` read the GUID
  `GetSessionDesc` returned. On `ffadca6` both answers passed the validation, that build's
  capture wrote no copy (its counter 0), none was seeded from the copy or the map alone (both
  0), and after the load the copy still held the session's GUID with `main+0x475` at `0x20`, the
  load's `SetSessionDesc` made.
- **The wrapped `SetSessionDesc`, `f44b5ee`'s code**, two peers on Two Continents: both seeded
  from `{181E3FD8-DEA0-42FE-AAB8-33E00D9AB262}` and `0x4934CBDE`, seed `0xE5210DF9515CC70E`, and
  on both the engine's copy agreed. At the seed the host had made 6 `SetSessionDesc` calls and
  the joiner 1, all from the battle room's handlers and the pump: the load's own call
  (`0x497C0B`) comes after the seed, since `0x497581` → `0x4917D0` → `0x491903` precede it in the
  loader body with no branch back. None was over another GUID and none withheld; in play they
  read 7 and 2, the load's call added on each, still 0 and 0. None was seeded from the copy or
  the map alone, and `main+0x475` read `0x20` on both. The draws were not compared on this
  build: its change is to the `SetSessionDesc` alone, and the seed is the draws' only input.
- **The killed host.** The host's `TotalA.exe` was killed (`kill -9`) 3 ms after its seed line,
  while the joiners loaded. Both joiners seeded from the same GUID and draw the same wind, and
  both went into play. DirectPlay never reported the departure: 115 s of game time later both
  joiners still held the host's seat with type 3 and its ID (`0x4AE1FCEC`), so the engine's
  removal `0x452CC0` never ran and this run does not exercise it. A killed process sends no
  `DestroyPlayer`, and a session `createnewgame` makes has no `DPSESSION_KEEPALIVE` (a lobbied
  one takes the lobby's flags). The two
  joiners' rosters also differed (one unit against two), which is stock's handling of a host lost
  at the start and nothing the seed reads.

In a skirmish on `23b6b8b` the log reads `seeded from the performance counter (game mode 2, not a
network game)`, and the wind draws (next 420, speed 1385 at GameTime 281). The wind's nine rows
and the yardmaps' three are all B6 adds to the table: on its own branch the install line read
`limits: installed 173 sites` (the 161 before it) and the stock-limits build `the simulation
fixes' 47 sites installed` (`f44b5ee`'s code); merged with B1–B5 (`2fc6528`) they read
`limits: installed 224 sites` and `the simulation fixes' 99 sites installed`, B5's 212 and 87 plus
the same twelve (MEASURED at launch, both builds).

### The grid stamp's last column and row — `0x47CC30`, `0x47CC8B`, `0x47CCA3`, `0x47CCA9` [DISASSEMBLED + MEASURED 2026-09-25]

**The stamp.** `0x47CC30(unit)` writes a unit's footprint into the feature grid's unit slots and
files it in a 128-px sort bucket. Its callers are `0x43DA41` (the motion relink, after its clear
`0x47D0E0`), `0x48610F`, `0x48630C`, `0x48AA9A` and `0x48B6B9`. It reads the footprint origin
`X = +0x76`, `Z = +0x78` and the footprint `fw`, `fh` from `+0x7E`. A unit with `X < 0` or `Z < 0`
(`0x47CC5E`, `0x47CC6B`), `X + fw ≥ W` (`0x47CC85`/`0x47CC8B` `jge`) or `Z + fh ≥ H`
(`0x47CCA1`/`0x47CCA3` `jge`) goes to `0x47D079`, which files it in the off-map bucket
`main+0x142B7` and stamps no cell. The last two are one past: a footprint that ends on the last
column or row, every cell of it on the map, is parked, and a parked unit holds no cell, so area
damage and the direct-hit test `0x49B090` never find it. Ground units never stand there (LoadMap masks the
border with `0xFFFD`); an aircraft over the east or south edge does.

**What `jg` needs** (settled before landing):

- The cell walk starts at `(Z·W + X)·13` (`0x47CD31..0x47CD4A`) and covers `fh` rows of `fw`
  cells: with `X + fw ≤ W` and `Z + fh ≤ H`, every cell is inside the grid. The clear `0x47D0E0`
  decides by the bucket (`0x47D0FD`) and walks the same cells. The re-claim `0x47C790` has **no map
  bound of its own**: it walks the footprint from `0x481550(X, Z)` with a stride of `W − fw` for
  any unit with `+0x110` bit 27 (`0x47C7A0`; bit 29 picks the walk, `0x47C7B4`, `0x47C8F3`). Bit
  27 is set by the stamp's cell loops, by the re-claim itself, by YARD_OPEN `0x47DAC0` (its one
  caller the COB setter, `0x480BAE`) only once `0x47D970` has found the footprint inside the grid
  (`1 ≤ X`, `1 ≤ Z`, `X + fw < W`, `Z + fh < H`, `0x47D991..0x47D9D8`), and by `0x47DB20`
  (`0x47DB40`), whose one caller is the saved game's restore (`0x487698`), on a unit its create
  has just stamped or parked [not established: whether a saved game can hold a parked unit].
  Every writer of `+0x76` on a stamped unit clears first (`0x43DA0F`, `0x48AA6B`, `0x48B685`);
  the other two, the create (`0x485BD3`) and the saved game's restore (`0x487243`), write a fresh
  unit that holds no stamp yet.
- **The sort bucket is taken from the position, not the footprint, with no bound**
  (`0x47CCA9..0x47CCDA`): column `x >> 23`, row `z >> 23`, index `row·cols + col`, 10 bytes a bucket
  from `main+0x1429F`. LoadMap sizes the grid `ceil(16W/128) × ceil(16H/128)` into `main+0x142A3`
  (columns) and `+0x142A7` (rows) at `0x482C84..0x482CA6`. Every writer of the footprint computes
  `X = (x − 8·fw + 8) >> 4` in pixels (`0x43D877..0x43D895`, `0x485BA3..0x485BC7`,
  `0x48AA24..0x48AA32`), so `X + fw ≤ W` gives `x < 16W − 8·fw + 8`: inside the bucket grid for
  `fw ≥ 1`, every stock footprint, and one bucket past its end for `fw = 0`. Stock itself forms
  column −1 for `fw = 0` and `x` in [−8, 0) at the west edge, whose linear index `row·cols − 1` is
  the previous row's last bucket — inside the array for `row ≥ 1` — and buckets are read by
  simulation code (`0x40F2E9`, `0x47E5DA`, …).

**MEASURED, previous build** (`scenarios/b1-offmap-edge.json`: two ARMATLAS, `fw` 3, hovering at the
east edge of Two Continents, `W` 672): the one at `X = 669` (cells 669–671) was in the off-map bucket
on every sample and held 150 of 150 HP for 40 s in range of an AI CORFLAK, which shot the one at
`X = 668` within 12 s.

**The fix** (`fix_last_cell`, simulation, fail closed): `jge` → `jg` at `0x47CC8B` and `0x47CCA3`, and
the 50-byte bucket block at `0x47CCA9` replaced by a jump to a stub that forms stock's index and
keeps it whenever the linear index lies inside the `rows·cols` grid, `[0, rows·cols)`; only an index outside has its column
clamped into `[0, cols − 1]` and its row into `[0, rows − 1]`. It leaves `edx` (the bucket), `eax`
(`[esi+0x82]`) and `[esp+0x20]` (y) as stock does and keeps `ebx` and `edi`, rejoining at
`0x47CCDB`. No branch from outside lands inside the block [rel8/rel32 scan; the four hits the
byte-level scan reports are operands]. **The invariant**: a unit is parked only when a cell of its
footprint is off the map, and every bucket index the stamp forms is inside the `rows·cols` grid —
stock's own whenever stock's is. (LoadMap allocates `(rows·cols + 7) & ~7` buckets,
`0x482CAE..0x482CBA`, so the grid is never larger than the block.) MEASURED on the
new build: the edge ARMATLAS was stamped (`+0x82` not the off-map bucket) and both were shot down
within 2 s.

### The footprint contest's rule — `0x47C856`, `0x47C95F`, `0x47CA3B`, `0x47CDD9`, `0x47CF0F`, `0x47CFE9` [DISASSEMBLED + MEASURED 2026-10-12]

**The six contests.** The re-claim `0x47C790` and the stamp `0x47CC30` contest a held cell the same
way at six sites, one per path (structure yardmap, slot A, slot B): `cmp dword [rec],0` / `je keep`,
then `cmp byte [rec+0x73],3` / `je take`, `rec` the incumbent's owner record `+0x96`. The newcomer
takes the cell only when the incumbent's record is active and of type 3 (remote): the re-claim at
`0x47C84D..0x47C856`, `0x47C956..0x47C95F`, `0x47CA32..0x47CA3B` (`je` = `74 26`), the stamp at
`0x47CDD0..0x47CDD9`, `0x47CF06..0x47CF0F`, `0x47CFE0..0x47CFE9` (`74 25`). Either way the unit
left without the cell gets `+0x110` bit 27 and the holder bit 26 (twelve `or` sites,
`0x47C85E..0x47D028`). The full list of `cmp byte [reg+0x73],3` in `.text` has 29 entries; these six
are the only ones in the footprint writers.

**The marks keep the grid consistent.** The clear `0x47D0E0` clears bit 27 always, before its
bit-26 test (`0x47D286..0x47D294`, `and ebx,0xF7FFFFFF`) and,
when bit 26 is set, clears it and walks the buckets around the footprint (`0x47D29C..0x47D2C4`; the
walk `0x47E5C0` hands every unit of an 8-cell bucket from `(x >> 3) − 1` to `((x + w) >> 3) + 1`, z
alike, whose footprint overlaps, to `0x47ED30`, which re-claims it). The re-claim acts only on bit
27 (`0x47C7A0..0x47C7A6`), clears it (`0x47C7AF`) and stamps again with the same contest. So a
holder's clear gives its cells back to the losers it marked. **What the stamp writes**: a mobile
unit whose footprint lies on the grid, filed by its layer, `+0x110 & 3` -- slot A for 1, slot B
for 2, no cell for 0 (a carried unit) or 3 (`0x47CEBC..0x47CEC2`, `0x47CF98..0x47CF9B`); a
footprint with x or z below 0, or x + w or z + h past the grid's columns or rows, is parked in the
off-map bucket and writes no cell (`0x47CC57..0x47CCA3`; the right and bottom edges are this
build's, with `fix_last_cell`'s `jg` at `0x47CC8B` and `0x47CCA3`). So every footprint cell of a
live mobile unit the stamp files names it in its layer's slot, or, while it carries bit 27, a unit
of the same layer -- or a structure, for a layer-1 unit, since the stamp files a structure in
slot A whatever its layer (`0x47CD5A` → `0x47CDA5`) -- whose footprint covers the cell and which
carries bit 26: the bit-26 unit loses that bit only in its own clear, whose walk re-claims for the
bit-27 units, or when its record is made anew -- the create's init clears both marks (`0x485CC5`,
`and eax,0xF3FFF7FF`) [DISASSEMBLED; MEASURED
2026-10-12 with no exception in 776 dumps, 284 of them live, holding 2,042 stamped bit-27 units].
Every clear ends with `0x440A70` (`0x47D2CA`).

**One slot per layer, so an overlap leaves a unit unnamed** [DISASSEMBLED 2026-10-23]. A cell's
slot A and slot B are one u16 each. A mobile unit's path tests the layer (`and eax,3` / `cmp eax,1`
/ `jne` at `0x47CEBC..0x47CEC2`, reached from the structure test `0x47CD5A` with bit 29 clear),
and the layer-1 loop reads the cell's slot A, `mov ax,word [edx]` at `0x47CEDA`: 0 takes the cell,
anything else is the incumbent's slot, whose unit record (`*(0x511DE8)+0x14357`, `0x118` bytes a
slot, `0x47CEE2..0x47CEFD`) gives its owner record `+0x96` at `0x47CF00`, tested at
`0x47CF06..0x47CF0F` as above. The keep path marks the incumbent bit 26 and the newcomer bit 27
and leaves the cell (`0x47CF11..0x47CF34`); the take path marks them the other way round and
writes the newcomer's slot `+0xA8` (the structure path's twin is `0x47CE00..0x47CE2A`). So where two
live mobile units of one layer overlap, one of the two is not named in the shared cell whatever
the order of the stamps, and only the marks say which one lost it -- the reason the replay's
occupancy check is the contest's (`tools/replay/tarec.py`, THE OCCUPANCY RULE). **On a playing
whose cells are the engine's** -- a multiplayer segment, and a skirmish segment that holds its
cells -- these writers are the marks' only writer but once: a takeover of a segment that holds its
cells writes the recording's marks at its refresh, beside the recording's slots and yard bits
(`tagpu_replay.c`, `tk_cells`). Otherwise the playing's unit image writes leave `+0x110`
bits 26 and 27 out (`tagpu_replay.c`, `U_MARKS`, `unit_put`, `states_put`). Each owner's image
carries its own machine's marks, for its own grid at its own times: written beside the host's
grid, they left a host contest unmarked (MEASURED 2026-10-23, the r2 four-seat recording at
GameTime 1016: cell (157, 82) named unit 10, which overlapped unit 1503 there, and none of the 43
units carried a mark); had the holder left first, its clear would have run no walk and the loser,
with no bit 27, no re-claim, so the cell would have stayed empty under it [INFERRED from the
writers above; not reached there, the loser left first, at 1025].

**An open yard gives nothing back.** YARD_OPEN `0x47DAC0` sets `+0x10F` bit 2 and the structure's
own bit 27 (`0x47DAD5..0x47DAF7`), re-claims for the structure alone (`0x47DAFD`) and refreshes the
passability (`0x47DB0A`); it calls no clear. The structure's re-claim walks its yardmap under the
mask `+0x10F` bit 2 selects (`0x47C804..0x47C817`: 2 open, 4 closed): a cell its byte sets is taken
or contested, and a cell it does not set is let go only if it names the structure
(`0x47C8AE..0x47C8B7`). So a unit that lost a yard cell to the closed structure keeps bit 27 and
gets no cell when the yard opens -- a holder's clear is the one walk that would re-claim for it.

**A lab can open its yard and make the nanoframe in it in the same tick** [DISASSEMBLED 2026-10-12].
YARD_OPEN's one caller is the COB setter (`0x480BAE`), so the yard opens in the lab's script slice:
the unit tick `0x48AD30` runs `DoScriptsNow(1)` `0x4B0D60` at `0x48ADEB`, and only after it, for an
owner of type 1 or 2, the order run `0x43B7C0` (`0x48AF98`). There the build handler BuildingBuild
`0x402640` waits for INBUILDSTANCE (`0x438700` at `0x4027CA`) -- which a lab's script sets in the
slice that opens the yard [INFERRED: the stock labs' order of the two] -- then asks
`QueryBuildInfo` (`0x4027FB`), tests the placement `0x47DB70` at `0x402899` (on failure it sleeps
15 ticks) and creates through `0x485F50` at `0x4028EA`. Nothing in the turn
stands between the slice's yard and the handler's create, so both can fall in one tick, the yard
first; slice-walkers measured them one tick apart (lab 502 opened at GameTime 2962, nanoframe 527
was made at 2963 on the yard's four free cells). The placement test does not see the yard: for a
mobile def (`+0x22F` ≠ 0, the path at `0x47DBF4`) it refuses only a footprint cell whose slot A
names another unit (`0x47DCFD..0x47DD0A`), so the order the cells were freed in decides the
create.

**So the recording carries the yard at its place, and a playing applies it there.** The recorder
observes YARD_OPEN's write `mov [esi+0x10F],bl` at `0x47DAF1` (6 bytes, `88 9E 0F 01 00 00`;
no branch in the file targets `0x47DAF2..0x47DAF6`), after the refusal `0x47D970` has passed
(`0x47DACC..0x47DAD3`) and with `bl` the new byte (`0x47DAD5..0x47DAEC`), and writes `RK_EV_YARD
{ID, the yard}` among the step's events, wherever the setter ran. A playing makes the same yard
through the re-claim `0x47C790` and `0x440A40` at that place in the events, so a create that
follows it in the step finds the cells the game's create found (`tagpu_replay.c`, *A RECORDED
YARD_OPEN*). A replay that makes units in slot order (the first step, a seek) writes each made
unit's yard as its create returns (`yard_put` in `level_units`), before the next create stamps
[MEASURED: before it, a seek at 3001 left 527 with bit 27 and its four cells empty; with it, every
round trip of slice-walkers, forest-circle and battle is EQUAL]. The image's yard goes in after
the positions wherever the host's still differs.

**A yard can also open inside its structure's own create, before its stamp** [DISASSEMBLED
2026-10-12]. The create `0x485F50` first runs the init `0x485A40` (`0x4860A0`), which writes the
footprint's size (`+0x7E`, `0x485AAA`) and cell (`+0x76`, `0x485BD3`) and clears `+0x10F`'s low
nibble, the yard among it (`0x485B3E..0x485B52`); then the COB setup `0x485D40` (`0x4860A6`),
which starts Create with run-now set (`0x485DE6`), so Create's first slice -- and a SET YARD_OPEN
in it, whose refusal now reads a valid footprint -- runs there; and only after it the placement
`0x48A870` (`0x486109`) and the stamp `0x47CC30` (`0x48610F`), whose yardmap path takes its mask
from `+0x10F` bit 2 (`0x47CD88..0x47CD9A`). CreateFromNetwork `0x4861D0` has the same order
(`0x4862BE`, then `0x486306` and `0x48630C`). So such a yard is stamped open, and setting it after
the create returned would stamp under the closed mask first. The recording carries the event
before the create's `RK_EV_CREATE` (the create is recorded at its exit, `0x4861B3`). The replayed
create runs with the puppet gates open (`tagpu_replay.c`, `make`), so its own Create makes the SET
again at the engine's place, as it makes the create's state byte; the recorded yard is held until
that create returns and compared with the yard the host's create left (`yard_check`), and a
difference fails the replay harness's *the yards a create opened* for that lane.

**A death's own Killed opens no yard inside its death** [DISASSEMBLED 2026-10-12]. The destructor
`0x4866D0` has two callers: Send_UnitDeath `0x4864B0` (`0x486679`, mode 1) and the `0x0C` network
receiver (`0x455423`, mode 0). Send_UnitDeath runs Killed as a query (`0x4B0BC0` at `0x4865C3`)
before it calls the destructor, so on a skirmish recording machine -- every death its own --
Killed's yard comes before the death's `RK_EV_DEATH`, on a unit still live. The destructor's own
Killed start (`0x4B0A70` at `0x486877`, after the footprint clear `0x47CBD0` -> `0x47D0E0` at
`0x48682D`) runs in mode 0 alone (`0x48684A..0x486853`), and its other script starts pass run-now
0: the order purge's teardown `0x43A1F0` starts TargetCleared through `0x489800` (`0x43A2B9`) and
StopBuilding at `0x43A251`, and its dispatch of event 2 to the order's handler (`0x43A21A`) reaches
handlers whose own starts pass run-now 0 -- BuildingBuild (`0x402653..0x402717`), SelfDestruct,
HelpBuild and VTOL_MobileBuild (returning at `0x41C110`), AirStrike and AirToGround, VTOL_Pickup's
states 4 and 5 (EndTransport, `0x4B0940` at `0x41148D` after `0x411479`). So no script of the dying
unit runs inside its own death's bracket. A yard inside another unit's death -- the Killed query of
a unit that death's explosion kills, the explosion's own starts (HitByWeapon `0x489F43`, TakeDamage
`0x489F8E`, RockUnit) passing run-now 0 -- is played after the whole destructor, just before that
unit's own death, as every event nested in a skirmish death is.

**THE YARD RULE** (`tagpu_recfmt.h`) follows from the two: a yard of a unit live at its place in
the step's events is that unit's yard there, written at that place; a yard of a unit not live is
its create's, and that create follows in the step; anything else -- a yard inside a feature call,
one naming the unit of a death still open, or one of a unit not live whose create does not follow
-- is malformed. The recorder holds a create's own yards and puts them just before the create
records itself, and keeps what the rule would refuse out of the file and counts it (the replay
harness fails any); the writer refuses a malformed record, and both readers (`tagpu_recreader.c`,
`tarec.py`) refuse the file. The playing compares every create's yard with the recording's: the
last yard its Create opened, or closed, since the init clears `+0x10F`'s low nibble
(`0x485B4A`, `0x485B52`) and nothing to the exit writes bit 2 but YARD_OPEN -- a Create refused
its yard on the recording machine (`0x47D970`) included. `tarec check` asks the same of the
recording: a unit made in the step shows the yard of its last yard event, or the closed one.

**The rule on a skirmish recording machine.** Its seats are type 1 (local human) and 2 (local AI)
(MEASURED, the controllers of both recordings the decision rests on), so the incumbent always keeps
a contested cell there. Every seat of a puppet level is type 3, so the same engine decides every
contest the other way. A playing that keeps the engine's cells (a skirmish recording that holds
them, `RECL_CELLS`) writes `90 90` over the six `je take` from its first step to its teardown, a
takeover or a return to now, each site only while it holds the retail bytes (`tagpu_puppet.c`,
*The contest's rule*). MEASURED 2026-10-12 on an air war, each rule's live game against its own
playing: under the type-3 rule 71 of 2,441 impacts differed (22 only live, 49 only played, 70 at
slot B); under the recording machine's 1 of 2,221, not at slot B (the known feature-path impact at
`0x49B3D2`), and every cell matched at both dumps; a battle 1 of 4,867, the same class.

**What reads the slots after a takeover.** The passability refresh `0x440A40(xz, wh)` calls
`0x440830` for each active context (`0x440A50..0x440A67`), which reaches the rect passability
`0x47DFC0` through `0x47E1F0` (`0x4408D5`; five calls `0x47E214..0x47E2B5`), and that reads a
cell's slot A at `0x47E10C`. A takeover writes the recording's slots, yard bits and marks and then
runs it over every context whole (`tagpu_replay.c`, `tk_cells`, `tk_live`).

### The order of a step's footprint and chain calls — the contest order, `0x48AD30`, `0x48A9F0`, `0x48AB70`, `0x47E5C0` [DISASSEMBLED 2026-10-23]

Who holds a contested cell, and which units carry the marks (`+0x110` bit 26 the holder, bit 27
the loser, *The footprint contest's rule* above), is a function of the order in which a step makes
the stamp `0x47CC30`, the clear `0x47D0E0` and the re-claim `0x47C790`, and of the order of the
bucket chains the re-claim walk reads. Where the recorder and a playing use this:
`tagpu_recfmt.h`, *THE CONTEST ORDER*; `tagpu_replay.c`, *THE CONTEST ORDER*.

**Every direct call of the three writers in the file**, by a call scan of `.text`; none of the
three, nor the set-position `0x48A9F0`, the relink `0x43D6D0`, the attach `0x48AB70` or its
wrapper `0x48AAC0`, is stored as an address anywhere in the file (a dword scan of the whole
file). The walk's functor is a stack object whose first dword is `0x4FD660`, written at
`0x47CACD`, `0x47D2B9` and `0x47DB29`.

| writer | call site (return) | what |
|---|---|---|
| stamp `0x47CC30` | `0x43DA41` (`0x43DA46`) | the relink `0x43D6D0`, movement |
| | `0x48610F` (`0x486114`) | the create `0x485F50` |
| | `0x48AA9A` (`0x48AA9F`) | the set-position `0x48A9F0` |
| | `0x48630C` (`0x486311`) | CreateFromNetwork `0x4861D0` |
| | `0x48B6B9` (`0x48B6BE`) | the `0x2C` receiver |
| clear `0x47D0E0` | `0x43DA0F` (`0x43DA14`) | the relink |
| | `0x47CBD6` (`0x47CBDB`) | the death's removal `0x47CBD0`, whose one caller is the destructor's `0x48682D` |
| | `0x48AA6B` (`0x48AA70`) | the set-position |
| | `0x48B685` (`0x48B68A`) | the `0x2C` receiver |
| re-claim `0x47C790` | `0x47DAFD` (`0x47DB02`) | YARD_OPEN `0x47DAC0`, whose one caller is the COB setter (`0x480BAE`) |
| | `0x47ED35` (`0x47ED3A`) | the walk's callback `0x47ED30`, inside a clear's walk (`0x47D2C4`), a re-claim's (`0x47CAD8`) or `0x47DB20`'s (`0x47DB4B`) |

**The set-position `0x48A9F0(unit, x, alt, z, layer)`**, stdcall `ret 0x14`: it rounds the cell,
`(x − w·2^19 + 2^19) >> 20` and the same for z (`0x48AA0C..0x48AA32`), and compares it with
`+0x76`/`+0x78` and the layer with `+0x110 & 3` (`0x48AA35..0x48AA56`). Unchanged, it writes
`+0x6A`/`+0x6E`/`+0x72` alone; changed, it calls the clear (`0x48AA6B`), writes the position, the
cell and the layer bits, then the stamp (`0x48AA9A`) and the sight stamp `0x4827B0` (`0x48AAA0`).
Either way it sets bit 16 (`0x48AAAC`). Its two callers: the relink's carried path `0x43D775` and
**Teleport**'s `0x406BC0` (order type 11, handler `0x406AA0`, *Teleport, order type 11* above),
which places *other* units in a box -- the one footprint call of stock code that is made in one
unit's turn on another unit. Just before it the handler makes the flare `0x471FD0(&unit+0x6A,
&target, 0x1E, 5)` at `0x406B9F` (stdcall `ret 0x10`), which copies both points into its particle
object (`0x4736F3..0x47371E`, the init at vtable `0x4FD588` + `0x18`) and keeps neither pointer.

**The relink `0x43D6D0` has one caller**, `0x43DD55` in the movement pass `0x43DD20`, whose
callers are the unit tick's `0x48AFAA` and the `0x2C` receiver's `0x48BA46`. It relinks only on a
cell or layer change (`0x43D898..0x43D8BF`: the clear `0x43DA0F` and the stamp `0x43DA41`, or the
set-position at `0x43D775` for a carried unit). So a unit relinks at most once a step, in its own
turn.

**After the relink, in the same turn, the unit's own scripts run.** The movement pass calls the
walk class `0x43DA70` next (`0x43DD5D`), which on a change of its walk state starts StartMoving,
StopMoving or the like through `0x4B0940` with run-now 1 (`push 1` at `0x43DAD5`, `0x43DAE3`,
`0x43DB02`, `0x43DB0D`, `0x43DB18`; the calls at `0x43DAF2` and `0x43DB27`), and then the water
state `0x43DB50` (`0x43DD65`), which starts setSFXoccupy through `0x4B0A70` with run-now 1
(`0x43DBDD..0x43DBE8`). A run-now start runs the script's threads at once (`0x4B0B86..0x4B0BA6`:
`0x4B0DA0(i, 0)` for i 0..7, then the pass at `0x4B0BA2`), every runnable thread of the unit among
them. The runner `0x4B0DA0(i, dt)` reads thread i's state (`+0x1C` of its 0xA4-byte record): high
byte 2 is blocked -- on a wait-for-move while the piece's move flag (`+0x28` of its animation
record) is set, on a wait-for-turn while its turn flag (`+0x10`) is, on a sleep until its counter,
less `dt`, reaches 0 (`0x4B0DD1..0x4B0E37`) -- and high byte 1 runs (`0x4B0E3D`). The animation
pass `0x4B1C00(dt)` clears those flags as each animation ends, and returns at once for `dt` 0 (`0x4B1C07`);
DoScriptsNow `0x4B0D60` calls it after its threads (`0x4B0D75..0x4B0D88`). So a wait that ends in a
unit's slice lets its thread run at the next pass of that unit's threads in the turn -- a run-now
start such as the walk class's -- or else at the next slice (DISASSEMBLED). A TransportPickup or
TransportDrop the order run started earlier (`0x406834`, `0x4069BF`, run-now 1 through `0x4B0A70`)
blocks on its boom's waits, so the unit's COB can attach or drop (`0x4813A6`, `0x481422` ->
`0x48AAC0`; the drop first asks `0x47DB70` at `0x481412`) or open a yard after its relink, and the
reaper's death (`0x48AFD1`) comes after it too. A blocked unit makes the walk class's pass and
the relink meet: when the relink finds the next cell taken (`0x47DB70` at `0x43D912`) it sets the
mover's `+0x2E` bit 2 and clamps the position to its cell's edge (`0x43D917..0x43D9F7`); the walk
class reads that bit as stopped (`0x43DA79`); the step the cell frees, the relink moves the unit
into it and the walk class starts StartMoving. MEASURED 2026-10-24 with
`scenarios/replay-transport-turn.gen.py`: hover transports blocked by scout cars attached their
cargo in that pass in 6 of 40 combs. The attach `0x48AB70` calls no footprint writer (its calls:
`0x47CB00`, `0x47CB40`, `0x4384A0`, `0x48C9B0`), and the stamp re-files its unit's bucket only on a
change of it (`0x47CCDB`), so an attach commutes with its carrier's relink; a detach, which pushes
the child at the front of its `+0x82` bucket (`0x48ACCC..0x48ACDC`), does not when that is the
bucket the carrier has just entered. The recorder keys
every footprint event made after the turn unit's own relink at the next slot, so a playing places
that unit before it (`tagpu_recorder.c`, `turn_put`; `tagpu_recfmt.h`, *THE CONTEST ORDER*, step
3b).

**The unit tick's turns** (`0x48AD30..0x48B022`): records 0..9 in order (`al` counts 0..9, `edx`
steps `0x14B`, `0x48B008..0x48B022`); for each active record of type 1, 2 or 3 with `+0x146` ≠ 10
(`0x48AD7A..0x48AD9C`) its block `[+0x67, +0x6B]` ascending by `0x118` (`0x48ADA2..0x48ADAA`,
`0x48AFD6..0x48AFDE`), a slot whose `+0xA6` is 0 skipped (`0x48ADB0`). In a skirmish block k
starts at slot k·each + 1 (`0x4854A0`, `0x4858BD`/`0x4858E0`), so **the turns ascend by slot over
the whole tick**. A turn runs `0x437910` (`0x48ADC4`), AutoAim `0x49E1A0` (`0x48ADDA`, type 1/2),
the slice site `0x48ADDF..0x48ADF0` (every turn passes it: its only branches in are AutoAim's
skips), two countdowns, then for an owner of type 1/2 (`0x48AEB5..0x48AECD`) `0x489BB0`
(`0x48AF32`), `0x41BD10` (`0x48AF92`), **the order runs** `0x43B7C0` (`0x48AF98`) and `0x43BAD0`
(`0x48AF9E`) and, with a mover, **the movement pass** `0x43DD20` (`0x48AFAA`) and `0x48A870`
(`0x48AFB0`); and for any owner, bit 14 → **Send_UnitDeath** `0x4864B0` (`0x48AFD1`). The
footprint and chain calls of a turn come from the slice (COB YARD_OPEN → `0x47DAC0` → the
re-claim), the order runs (BuildingBuild `0x402640` creates at `0x4028EA` and attaches at
`0x402948`, the transport handlers attach between `0x40F221` and `0x415843`, the build finish
`0x41B8D0` detaches at `0x41B9A1`, Teleport's set-position `0x406BC0`), the unit's own relink and
its death, in that order. Of the tick's direct calls only `0x43DD20` and `0x4864B0` reach a writer
in the direct-call graph [MEASURED with a call graph of `.text`]; the rest is indirect, through
the order handlers' tables.

**After the unit tick.** In the step `0x495490`, the unit tick at `0x4954ED` returns to `0x4954F2`,
and before the projectile tick's call at `0x495513` there is only a timing accumulator (`0x4B6560`
at `0x4954FE`, stores to `main+0x38D85`). Then the explosions (`0x495539`), the players phase
(`0x49555F`, whose Deathmatch respawn creates at `0x4653D9`) and the features. In the frame
`0x496790`: the main loop's network pump (`0x4968CB`), the step (`0x49693E`), the draw
(`0x4969CD`), then `0x4969D2`, where Impure's scenario applier runs. So every footprint call
outside the unit tick follows every turn of its capture-to-capture group.

**The walk `0x47E5C0` reads the chains in order.** It visits each bucket of its rectangle by its
head (`+0x6`, `0x47E652`), follows the chain through `+0x8E` (`0x47E6EE`, `0x47E6F8`), and calls
the functor on each unit and then on that unit's cargo through `+0x8A`/`+0x8E` (`0x47E6A1`,
`0x47E6EC`). So the bucket chains' order decides which unit owed a re-claim gets a freed cell
first. The chains' writers (a call scan): the stamp's re-file (`0x47CCD2..0x47CD31`: an unlink and
a push at the front, skipped for a carried unit, which gets `+0x82` alone), the attach `0x48AB70`
(its unlink `0x47CB00` has one caller, `0x48AC5E`, and its push `0x47CB40` one, `0x48ACDC`), and
the death's removal `0x47CBD0` (one caller, `0x48682D`; it calls the clear at `0x47CBD6`).
**`0x47CB60` has no caller in the file.** **`0x47DB20(unit)`** sets bit 27 and walks re-claims
(`0x47DB20..0x47DB5E`); its one caller is `0x487698` in the saved-game restore `0x487080`, reached
only from the loader `0x497180` (through `0x432610` at `0x497B29`) and the restore's own order
loader `0x43A420`: it never runs in a step.

**`0x48AAC0(child, parent, piece, layer)`** (`ret 0x10`), the attach's wrapper: it refuses a
child not alive (`+0x110` bit 28), a structure (bit 29) or one with cargo (`+0x8A`), and a parent
not alive, the child itself or carried (`+0x86`) (`0x48AAC7..0x48AB0D`), builds the `0x0A`
record from both units' `+0xA8`, sends it (`0x44FDB0`, `0x451DF0`) and applies it at `0x48AB62`.
Its 30 callers (a call scan): BuildingBuild `0x402948`; the transport handlers' twenty-two, `0x40F221`
to `0x415843`; the build finish `0x41B9A1`; COB's ATTACH `0x4813A6` and DROP `0x481422`;
the destructor's two detaches `0x4867CB`, `0x48681D`; `0x4871FF` in the saved-game restore; and
`0x488263`, in a function not mapped here. Since its tests are `0x48AB70`'s own, every call it
makes of `0x48AB70` acts. **An acting attach always changes a chain**: onto a carrier it pushes
the child at the front of the carrier's chain (`0x48AC70..0x48AC82`); a detach puts it at the
front of its bucket (`0x47CB40` at `0x48ACDC`), unlinked first from its carrier's chain or, for a
child no unit carries, from its bucket (`0x47CB00` at `0x48AC5E`) -- so a detach of an uncarried
unit, the build finish's included, re-files it at its bucket's head.

**How the recorder sees it.** An observer at the attach's entry, `mov edx,[0x511DE8]`
(`8B 15 E8 1D 51 00`; no rel8/rel32 in the file branches into `0x48AB71..0x48AB75`), reads the
record's seven bytes and, on slots bounded first, the refusals above, and writes an acting
attach's `RK_EV_LINK` at its place unless it is a death's (`tagpu_recorder.c`, `ob_link`). The
three writers' observers check each call's return against the table above
(`FOOT_SITES`): a relink's or a set-position's call is ordered only as the turn unit's own, or as
a Teleport move's. Teleport's call `0x406BC0` (`E8 2B 3E 08 00`) is redirected to a stub that
writes `RK_EV_PLACE` -- the unit, its position and layer before, and the call's position and layer
-- and then calls `0x48A9F0` itself (`tagpu_recorder.c`, `rec_teleport`); a playing makes the flare
and the set-position at that place, the flare counted as the handler's call `0x406BA4`
(`tagpu_recorder.c`, `tagpu_rec_teleport_flare`). A return the table does not hold, a turn out of
slot order, or a move of a unit that is not alive -- the handler's walk has no alive test -- is
counted (`recorder: N footprint calls a playing cannot put in their order`).

### Line of sight at the map's edge — `UnitInPlayerLOS 0x465AC0`, `PositionInPlayerMapped 0x408090` [DISASSEMBLED + MEASURED 2026-09-25]

**The function.** `0x465AC0(player, unit)`, `__stdcall`, `ret 8`, has seven callers: `0x40AB11`
(in the periodic acquisition `0x40AA40`), `0x439761`, `0x46AF3D`, `0x46B5B4`, `0x480EE5`,
`0x48BC56` and `0x494396`; our order markers call it too. A unit of the player's own is visible
(`0x465AD4`); one with `+0x10E` bit 2 is not. It tests up to four points of the unit's box in turn,
until one is visible, as [line of sight](../line-of-sight.md) describes — 16.16 dwords at `[esp+0x10]`
(x), `+0x14` (y) and `+0x18` (z), `def` the unit's type at `+0x92`: `(x + def+0x15E, y + def+0x16E,
z + def+0x166)` (`0x465AFD..0x465B3B`); the same with `x + def+0x176` (`0x465BDB..0x465BED`); then
`y − def+0x17A`, `z + def+0x17E` (`0x465C73..0x465C90`); and that with `x − def+0x176`
(`0x465D19..0x465D31`). Every point is placed at `col = x >> 5` and `row = (z − (y >> 1)) >> 5`
(32-px cells), projected by altitude, under unsigned bounds on the player's grid size (width `+0x80`,
height `+0x84`); outside them the point is not visible. What is read there depends on the mode, and
the shear is the same in every mode:

- **`LosType` bit 1** (`main+0x14281`; True line of sight, `LosType` 14): the player's LOS grid
  (`+0x7C`) inline — `0x465B6A..0x465B94`, `0x465C04..0x465C2E`, `0x465CA2..0x465CD2` and
  `0x465D46..0x465D67`, not visible at `0x465BB1`, `0x465C49`, `0x465CED`, `0x465D94`.
- **Without it** (Permanent 12, Circular 8, whose LOS grids are all lit): the first three points go to
  `PositionInPlayerMapped 0x408090(player, point)` (`0x465BBB`, `0x465C53`, `0x465CF7`), which places
  the point at `0x408095..0x4080C0` — not visible at `0x4080C1` — and reads the shared mapped grid
  `main+0x14273`, a `u16` a cell and a bit a player; the fourth goes to an inline copy of it,
  `0x465DA9..0x465DCA` (not visible `0x465DCB`, read `0x465DE0`). Inside `0x408090`, `esi` is pushed at `0x408094` and is free until the `pop esi` of either exit, which is what the stub over its row block uses.

The third read loads `dx` and `di`, the point's y and z words, and the fourth reads them again
(`0x465D46`, `0x465DA9`); the fourth takes its column in `ecx` from `0x465D3B`, in both modes.
`0x408090` has two more callers, each handing it a world point whose row means the same:
`0x407FBD` in `0x407E90`, the first method of the vtable `0x4FC9A0` (its destructor `0x407E70`
restores the base's `0x4FC980`) [INFERRED: an AI routine], which tests a probe point stepped 320 px
from a position along a heading it draws; and `0x49BF2F` in the projectile draw pass `0x49BE60`,
the local player's view of a projectile. Each of the two has an inline copy of the True-mode read
beside the call, with the same shear: `0x407F74..0x407F9B` (esi the point, ecx the player; read
`0x407F9C`, not visible `0x407FB7`) and `0x49BEE8..0x49BF0F` (ebp the point, eax the player; read
`0x49BF10`, not visible `0x49BF29`). The projectile pass still runs in play: `fxown`, which would
stop it, is not a play default, because the engine's frame is the golden source (`tagpu_opt.c`); it
also poses each model projectile it lets through (`0x49C127`).

**The defect.** The row is the box's north edge less half the box's absolute top, so a unit whose
top is more than twice its distance from the north edge — an aircraft at cruise altitude a few
tiles inside it, a unit on a hill beside it — samples a row above the grid and is invisible to every
other player, so nothing acquires it: at altitude 175 over ground at 84 the row is negative until
z is about 100. Underwater, `y < 0`, the same happens at the south edge. The AI's own acquisition
does not go through this test: an AI CORFLAK shot a human ARMATLAS hovering at z 40.

**MEASURED, previous build** (`scenarios/b1-los-shear.json`, True LOS: two AI ARMATLAS held on
station by re-issued patrols, `north` at z 40 and altitude 175, `control` at z 330, a human CORFLAK
between them and a human spotter beside `north`): the flak shot `control` at once and held its aim
there while `north` hovered on station for about 7 s at 150 of 150 HP, in range and inside the
spotter's lit radius; when the AI dragged it south past z 84 the flak swung north and shot it within
0.6 s.

**The census** [DISASSEMBLED 2026-09-25], by the data rather than by one instruction's shape. The
read is inlined far beyond `0x465AC0`, and a row bound reads a grid's height, so the census is every
instruction of `.text` that reads one in memory: the player's `+0x84` (its LOS grid's rows, and the
rows the shared mapped grid `main+0x14273` is read by), the sight grid's `main+0x14297`, and
`[r+8]` through a register that a `lea` of `main+0x1428F` or of `+0x7C` (or an `add r,0x7C`) set —
and every compare with a register loaded from one of them. **57 compares** read such a height: 50 memory
compares with a disp32 of `0x84` (`cmp r,[g+0x84]` 47, `cmp [g+0x84],r` 3), and 7 against the sight
grid's height (`0x482663` against `main+0x14297`, six through a pointer to `main+0x1428F`). The 13
compares with a register loaded from some `+0x84` are other structures' counts and loop bounds
(`0x428D48`, `0x428D6F`, `0x428DE8`, `0x47BE7D`, `0x48DFFA`, `0x48FE4E`, `0x48FEBE`, `0x48FF4D`,
`0x48FFAF`, `0x490495`, `0x490509`, `0x490569`, `0x4905C9`); no other load of such a height reaches
a compare (the rest are tests with zero, increments of the field and call arguments). **46** of the 57 are the read — a point
placed at `(z − h/2) >> 5` against one player's grid, then that player's LOS grid (`+0x7C`) or the
shared mapped grid at the player's bit — and 44 of them are patched: 16 in the table, 28 local. Two
are the dead `0x474B80`'s. Of the other eleven, three re-test the row the sight emitter stored, three
bound a ray's cell, two re-test a row already bounded, and three compare another structure's `+0x84`
with zero. The first census, of the `+0x84` compares alone, missed the sight emitter, whose bound
is `main+0x14297`.

**What it does not enumerate** — two forms, found by a second pass over each: a grid pointer
spilled to the stack and reloaded, and a height recomputed from the plot's rows, `main+0x14237 >> 1`.

- **Four spills.** The mapped stamp's (`0x4819AD`, `[esp+0x24]`), the removal's (`0x481DDA`) and the
  stamp's (`0x4822FA`, both `[esp+0x30]`) are reloaded on each ray's back edge (`0x481B89`,
  `0x481F76`, `0x482496`) and reach compares the census has, `0x481B0B`, `0x481F15`, `0x482435`. The
  sight grid's builder's (`0x482F2A`, `[esp+0x14]`, from `lea ebx,[edi+0x1428F]` at `0x482F21`)
  reaches two it has not. The builder sizes the grid at half the plot, fills each cell with
  `{0, 0xFF}`, then **projects every terrain tile to its sheared row**, `(z − h/2) >> 5` with `h` the
  tile's height byte (`0x482FFC..0x483002`), and writes it only inside the grid: `cmp ecx,-1` (signed,
  `0x483005`), then `0x48307C` and `0x4830C6` through the reloaded pointer. So **the sight grid is
  built in sheared space**, and that is the space the ray fan reads, from the own row as from stock's.
  A tile sheared off the grid loses only its occlusion, never anyone's sight — verdict: not the
  defect, stock.
- **Recomputed heights.** `main+0x14237 >> 1` reaches four compares — the circle's row clips in the
  mapped stamp (`0x481C28`, from `0x481988`), the removal (`0x481FF9`, through `[esp+0x44]` set at
  `0x481DA4`) and the stamp (`0x482519`, through `[esp+0x44]` set at `0x4822C4`), which clip a circle
  row by row; and `0x40D80A`, which bounds `0x40D817`'s scaled coordinate — and one scale, `0x466CB3`
  in `0x466C20`, a pixel of an image it fills [INFERRED: the minimap] to a cell. None is this read.

**A scan of the shear's arithmetic** (a halving subtracted from a z, then `>> 5`, following one
register copy) finds 39 computations: the sight grid builder's, and one feeding each of 38 of the 46;
the other eight reuse a `y >> 1` computed further up (`0x4658E0`'s second corner, `0x466DC0`'s dots,
`0x473A00`).

| compare | function | what the answer decides | grid | verdict |
|---|---|---|---|---|
| `0x407F94` | the AI probe `0x407E90` [INFERRED: an AI routine] | the probe point the routine keeps | LOS grid | sheared, patched: table (hand stub over `0x407F74..0x407F9B`) |
| `0x4080B9` | `PositionInPlayerMapped 0x408090` | whether a point is mapped for a player (Permanent, Circular) | mapped grid | sheared, patched: table (hand stub over `0x408095..0x4080C0`) |
| `0x43E69D` | the cursor picker `0x43E490` | the pointer sprite, and through `main+0x2CBE` what this player's left click orders | mapped grid | sheared, patched: local |
| `0x43E904` | the cursor picker | as above | mapped grid | sheared, patched: local |
| `0x43EBC6` | the cursor picker | as above | mapped grid | sheared, patched: local |
| `0x43ECDB` | the cursor picker | as above | mapped grid | sheared, patched: local |
| `0x43EE94` | the cursor picker | as above | mapped grid | sheared, patched: local |
| `0x43EFA9` | the cursor picker | as above | mapped grid | sheared, patched: local |
| `0x43F5D1` | the order resolver `0x43F0E0` | whether the target's cell is mapped for the local player, which steers the order it returns; among its 22 call sites are AI routines (`0x40804C` in `0x407E90`) and the unit order code (`0x405265`) | mapped grid | sheared, patched: table |
| `0x43F64D` | the order resolver | as above; stock overwrites the point's register (`esi`, `0x43F635`) before this test | mapped grid | sheared, patched: table (hand stub over `0x43F631..0x43F654`; it rejoins at `0x43F655`, or drops to stock's not-mapped exit `0x43F6AA`) |
| `0x43FC05` | the order resolver | as above | mapped grid | sheared, patched: table |
| `0x43FD1A` | the order resolver | as above | mapped grid | sheared, patched: table |
| `0x43FF85` | the order resolver | as above | mapped grid | sheared, patched: table |
| `0x4400AA` | the order resolver | as above | mapped grid | sheared, patched: table |
| `0x465942` | the feature LOS helper `0x4658E0`, first corner | whether `DrawGameScreen` draws a feature (`0x46990F`, `0x469A9C`) | LOS grid | sheared, patched: local |
| `0x46598A` | the feature helper, first corner | as above | mapped grid | sheared, patched: local |
| `0x465A17` | the feature helper, second corner: z is `bx` after `add bx,cx` (`0x4659F4`), less the `y >> 1` still in `esi` from `0x465934` | as above | LOS grid | sheared, patched: local |
| `0x465A63` | the feature helper, second corner (`esi` from `0x46597C`) | as above | mapped grid | sheared, patched: local |
| `0x465B8D` | `UnitInPlayerLOS 0x465AC0`, first point | whether a player sees a unit: the acquisition `0x40AA40` and six more callers | LOS grid | sheared, patched: table (hand stub over `0x465B6A..0x465B94`) |
| `0x465C27` | `UnitInPlayerLOS`, second point | as above | LOS grid | sheared, patched: table (`0x465C04..0x465C2E`) |
| `0x465CCB` | `UnitInPlayerLOS`, third point | as above | LOS grid | sheared, patched: table (`0x465CA2..0x465CD2`) |
| `0x465D60` | `UnitInPlayerLOS`, fourth point | as above | LOS grid | sheared, patched: table (`0x465D46..0x465D67`) |
| `0x465DC3` | `UnitInPlayerLOS`, fourth point, inline copy of `0x408090` | as above | mapped grid | sheared, patched: table (`0x465DA9..0x465DCA`) |
| `0x46725F` | the radar rebuild `0x466DC0`, its projectile dots (records `main+0x141F7`, stride `0x6B`; the row `[ebx+4] − ([ebx] >> 1)`, `0x4671E5..0x4671EB`) | whether a projectile's dot is drawn on the minimap | LOS grid | sheared, patched: local |
| `0x467294` | the radar rebuild | as above | mapped grid | sheared, patched: local |
| `0x467340` | the radar rebuild; `ebx` overwritten at `0x467315`, `0x467322` | as above | LOS grid | sheared, patched: local (z through `[esp+0x18]`) |
| `0x467375` | the radar rebuild; `ebx` overwritten at `0x467365` too | as above | mapped grid | sheared, patched: local (z through `[esp+0x18]`) |
| `0x46778A` | the view player's map build `0x467440`, its fifth loop, the sight loop `0x46770D..0x46782F` | a unit the view player sees gets `+0x110` bit 8, which the acquisition `0x40AA40` puts on every player's radar list | LOS grid | sheared, patched: table |
| `0x4677D0` | the view player's map build | as above | mapped grid | sheared, patched: table |
| `0x47360C` | the particle leaf `0x473590` | whether a particle is drawn | LOS grid | sheared, patched: local |
| `0x473657` | the particle leaf `0x473590` | as above | mapped grid | sheared, patched: local |
| `0x473A94` | the particle leaf `0x473A00` (called at `0x472FBB`): z is `bp`, less the `y >> 1` in `edx` from `0x473A3B` | whether a particle is drawn | LOS grid | sheared, patched: local |
| `0x473AD3` | the particle leaf `0x473A00` | as above | mapped grid | sheared, patched: local |
| `0x4741EC` | the particle leaf `0x474170` (fire) | whether a particle is drawn | LOS grid | sheared, patched: local |
| `0x474237` | the particle leaf `0x474170` | as above | mapped grid | sheared, patched: local |
| `0x474674` | the particle leaf `0x4745E0` (foam) | whether a particle is drawn | LOS grid | sheared, patched: local |
| `0x4746BB` | the particle leaf `0x4745E0` | as above | mapped grid | sheared, patched: local |
| `0x474BFC` | `0x474B80` | a particle's blit (`0x4B7F30`, `0x4B8500`) | LOS grid | sheared, **no caller**: no call or jump in `.text` reaches `0x474B80` and no pointer to it is in the image; left stock |
| `0x474C49` | `0x474B80` | as above | mapped grid | sheared, **no caller**; left stock |
| `0x47551E` | the particle leaf `0x475470`, slot 2 of the vtable `0x4FD618` (pointer at `0x4FD620`); `0x4750B0` beside it is a 0x35-byte constructor that installs the vtable `0x4FD638` | whether a particle is drawn | LOS grid | sheared, patched: local |
| `0x47556C` | the particle leaf `0x475470` | as above | mapped grid | sheared, patched: local |
| `0x47BE94` | `cmp [eax+0x84],ebx` in `0x47BDF0` [function start from the preceding padding] | another structure's `+0x84`, compared with zero | — | **not the shear**: no grid |
| `0x47D3B8` | the site test `0x47D2E0` | the build cursor's green or blocked (`main+0x2CC6` bit 6); it reads only when given a player (`0x47D366`), and only `0x4198C6` in the build cursor `0x4197D0` passes one — `0x40A4CD` and `0x47DBE5` pass 0 | mapped grid | sheared, patched: local |
| `0x47D41A` | the site test | re-tests the row `0x47D3B8` bounded (`eax` is not written in between) before reading the LOS grid | LOS grid | **not a separate shear**: it follows `0x47D3B8`'s row |
| `0x47D44E` | the site test | as `0x47D41A`, before reading the mapped grid | mapped grid | **not a separate shear**: it follows `0x47D3B8`'s row |
| `0x47F431` | positional sound `0x47F300` | whether a sound plays for this player | LOS grid | sheared, patched: local |
| `0x47F476` | positional sound | as above | mapped grid | sheared, patched: local |
| `0x4819B7` | the mapped stamp `0x481930` | re-tests the row the sight emitter stored (`0x481982`) | sight grid | **not a separate shear**: the emitter's row |
| `0x481B0B` | the mapped stamp | bounds one ray's cell, the stored centre plus a step | sight grid | **not the shear**: a ray's clip |
| `0x481DE4` | the removal `0x481D50` | re-tests the stored row (`0x481DB0`) | sight grid | **not a separate shear**: the emitter's row |
| `0x481F15` | the removal | bounds one ray's cell | sight grid | **not the shear**: a ray's clip |
| `0x482304` | the stamp `0x482270` | re-tests the stored row (`0x4822D0`) | sight grid | **not a separate shear**: the emitter's row |
| `0x482435` | the stamp | bounds one ray's cell | sight grid | **not the shear**: a ray's clip |
| `0x482663` | the sight emitter `0x4825B0` (below) | whether a unit stamps its sight at all, and where | sight grid (`main+0x14297`) | sheared, patched: table (`0x482615..0x48261E`) |
| `0x48307C`, `0x4830C6` (outside the 57: a spilled pointer) | the sight grid's builder | whether a terrain tile's height is written, at its sheared row | sight grid (through `[esp+0x14]`) | **not the defect**: the grid is built in sheared space; an off-grid tile loses only its occlusion |
| `0x48E9A8` | `cmp [ebp+0x84],ebx` in `0x48E010` [function start from the preceding padding] | another structure's `+0x84`, compared with zero | — | **not the shear**: no grid |
| `0x49BF08` | the projectile draw pass `0x49BE60` | whether the frame shows, and poses, a projectile | LOS grid | sheared, patched: local (hand stub over `0x49BEE8..0x49BF0F`; `ecx` is next written by the loop's top `0x49BEA3` through `0x49BF42`, or reloaded at `0x49C058`) |
| `0x4B4FF8` | `cmp [esi+0x84],edi` at the top of `0x4B4FF0` | another structure's `+0x84`, compared with zero | — | **not the shear**: no grid |

Readers of `main+0x14273` that no `+0x84` compare bounds — `0x40D817`, `0x466D01` and `0x47E37B`,
which halve a coordinate to scale it; `0x4816CC`, `0x481A48`, `0x481B58`, `0x481CAA`, `0x483E69`,
`0x484F8B`, `0x484FEE` and `0x485460` — index it by a cell or a scaled minimap coordinate, not by a
world point's row. `0x49C1B1`, a screen projection, reads neither it nor a grid. **The site test's
accumulator** `0x51E688` (zeroed at `0x47D2F4`, summed at `0x47D4E7`) is also read by AI code through
the getter `0x47C770`: at `0x40A4D6` and `0x40A4E5`, right after their own call of the site test
(`0x40A4CD`), which passes no player, so the test reads no grid and the fix cannot change what they
see; and at `0x40A76A`, after `0x47DB70`,
whose `[+0x22F]` path returns 1 without calling the site test — so that value can be one the build
cursor left, a per-peer leak stock already has. The fix changes the accumulator only
at the north edge (the footprint's sum instead of 0), no new kind of divergence, so the site
test stays local.

**The fix** (`fix_los_shear`, simulation, fail closed; `fix_los_local` and `fix_projectile_view`,
local): at every patched copy, when the sheared row is outside the grid and the point's own row,
`z >> 5`, is inside it, the own row is read; otherwise stock's answer stands, so a unit beyond the
map's edge stays unseen — TADR's margin there is a gameplay change, not a defect. **The invariant**:
the grid is read only at a column and a row inside it; exact whenever stock's row is inside.

- **Ten hand stubs** over whole blocks: the eight first found (43, 43, 49, 34, 34, 44, 40 and 40
  bytes), each computing stock's column and sheared row and leaving at stock's read (`0x465B95`,
  `0x465C2F`, `0x465CD3`, `0x465D68`, `0x465DE0`, `0x4080C7`, `0x407F9C`, `0x49BF10`) or its "not
  visible"; and `0x43F631..0x43F654` (36 bytes), the order resolver's second read, whose point
  register stock overwrites before its test, with z held on the stack. The third stub loads `dx` and
  `di` exactly as stock does, for the fourth; `ebp` after the third and `edx` after the fourth are
  dead on both exits. `ebx` is dead on both exits of the AI probe's copy (the read writes it at
  `0x407F9C`; past `0x408002` it is written at `0x40803E` or popped at `0x408081`) and of the
  projectile pass's (`0x49BF10` writes it; the other exit reaches the loop's top `0x49BEA3`, which
  writes it before any read, or its pop at `0x49C07D`). The tenth is the sight emitter's, below.
- **Thirty-four generic stubs** (7 in the table, 27 local) at lone row tests, `cmp r,[g+0x84]` and a
  `jae` or `jb` (8 bytes, or 12 with a near branch): the same compare and `jb` to stock's read; else
  `movsx r,<z>` — the instruction that loaded the point's z word for the shear — `sar r,5`, the
  compare again and `jae` to "not visible"; else the count and stock's read. The exits are decoded
  from the stock bytes; only `r` and the flags change, and each exit is reached with the carry stock
  reaches it with. **What each relies on is compared**: the bytes from the z load through the row
  test, the reload being exact only while nothing in between writes what it reads — for the table a
  compare-only row beside the branch row, for the local fix one compare per site before anything is
  written. The generator refuses a z that is not a `movsx` into `r` of its stated length, a z whose
  base, index or source is `r`, and a compare whose base is `r`. The radar's last two tests come
  after stock overwrites `ebx` (`0x467315`, `0x467322`, `0x467365`), so their stubs first take the
  pointer back from `[esp+0x18]`, where the loop keeps it (`0x4671C3`, `0x467425`), with `ebx` dead on
  both exits (reloaded at `0x4673A9`, cleared at `0x467381`); the whole loop `0x4671C0..0x46742E` is
  compared for them.

No branch from outside lands inside any replaced range or any compared span (a `rel8`/`rel32` scan
of `.text`: every hit is an operand byte, and the one literal equal to a site, `0x474674`, is the
text `"tFG"` in `.data`), and no absolute pointer into one is in the image. **Every stub counts**
the times it takes the own row, one interlocked `LONG` per function (seventeen), the second
`enginefix:` line printing the array's address and order — the path only the fix runs, so a count
above zero is that function's stub at work.

**The sight emitter** `0x4825B0(record)` (`__stdcall`, `ret 4`) is the observer's side of the same
defect. Its four live callers: `0x481836` in the all-units rebuild (after clearing every player's
grid, the loop from `0x481796` over the unit array zeroes each unit's height byte, under `LosType`
bit `0x2`, before emitting); `0x482824` in `0x4827B0`, which builds a unit's record on its stack —
the player `+0x96`, a pointer to the unit's stored column and row `+0x7A`/`+0x7C`, the def's `+0x202`
and `+0x170`, a pointer to the height byte `+0xF8`, and the unit's x, y, z (`+0x6A`, 16.16) with y
raised to at least the sea level `main+0x1427F` plus one — itself called at `0x43DA59`, `0x464DBF`,
`0x465053`, `0x48AAA0` and `0x48B6BF`; `0x482B5E` in `0x482AC0`, which builds the same record and
zeroes the height byte first (called at `0x486178` and `0x486312`); and `0x4829F4` in `0x482910`,
which builds a **temporary sight record** (below). `0x482868` is in `0x482830`, which no call or jump
reaches and no pointer in the image names. Under the ray fan
(`LosType` bit `0x4`) it takes the column `x >> 5`, a height `h` = the def's `+0x170` byte plus y's
integer part, clamped to [0, 255], and the row `(z − h/2) >> 5` (`0x482615..0x48261E`); if the stored
column and row are unchanged and `h` is within 5 of the height byte it returns
(`0x48261F..0x48262E`); otherwise it removes the old stamp through `0x481D50`
when the old height byte is non-zero and `LosType` bit `0x2` is set (`0x482634..0x482644`), stores the
column and row (`0x48264C`, `0x482652`), and bounds them (`0x48265B`, `0x482663`): inside, it stores
`h` in the height byte and stamps through `0x482270` (bit `0x2`) and `0x481930` (bit `0x1`); outside,
it zeroes the height byte and stamps nothing (`0x4826A9`). So an aircraft whose sheared row leaves the
grid near the north edge reveals nothing to its owner. The circle (bit `0x4` clear) places sight at
its corner, `(z >> 5) − (y >> 6)` less the circle's offset (`0x4826B7`), and the stamp clips each
row (`0x4824F8..0x482547`), so every row of the circle inside the grid is stamped.

**The temporary sight records** `main+0x1427B`, `main+0x14277` of them, at most 20 (`0x48292C`), 36
bytes each: `0x482910(point, sight, height, duration)` (`__stdcall`, `ret 0x10` at `0x482AAF`;
DISASSEMBLED `0x482910..0x482AAF`). It **refuses before writing anything** — no push, no store, the
count unchanged — when LosType bit 1 is clear (`and al,2` at `0x48291C`, `jne` at `0x482920`) or the
count is already 20 (`cmp eax,0x14` at `0x48292C`, `jge` at `0x48292F`); both jump to the `ret`.
Otherwise it fills record `count` of the block (`0x482935..0x482940`, `0x24` bytes a record): `+0x00`
the viewed player's record, `main+0x1B63 + 0x14B·[main+0x2A43]` (`0x482945..0x482972`), `+0x04` a
pointer to its stored words at `+0x20`, `+0x08` the sight word, `+0x0A` the height byte, `+0x0C` a
pointer to its height byte at `+0x0B`, `+0x10..+0x1B` the point copied (`0x48297E..0x48298D`) with y
raised to at least (the sea level `main+0x1427F` + 1) << 16 (`0x48299F..0x4829B0`), and the expiry
`GameTime + duration` at `+0x1C` (`0x4829C4`). It zeroes the height byte (`0x4829DE`) and stamps: under
the ray fan (LosType bit 2) through the emitter `0x4825B0(record)` (`0x4829F4`), otherwise as the
circle: the radius index sight/32 − 5 clamped to [0, the circle table `*(main+0x1485B)`'s count − 1]
(`0x4829FE..0x482A25`), the corner the point less the circle's offset from `0x4B7F30`
(`0x482A6A..0x482A85`), then `0x482270` and `0x481930` (`0x482A8E`, `0x482A94`). Both stamping paths
reach the count's increment (`0x482AA1..0x482AA9`); the refusals do not. **Its one engine caller is
the destructor** (`0x486748`, the only `E8` to it in `.text` and no absolute reference): only for
a unit whose `+0x110` bit 28 is set (the test at `0x486706` jumps to the epilogue, `0x486E59`, for
a dead slot) and whose owner record
(`+0x96`) has `+0x146` equal to `main+0x2A43` (`0x486716..0x48672A`); it pushes `0x3C`
(`0x486732`), the def's (`+0x92`) `+0x170` and `+0x202`, each sign-extended from a word
(`0x486737`, `0x48673E`), and the unit's `+0x6A`. It does not test LosType itself: the add does.
The expiry `0x482130` (its one call `0x495688`, once a call of the step `0x495490`, after its tick
loop) removes every record past its time through `0x481D50`
(`0x482161`), then compacts the table (`0x4821BF..0x48222D`): each kept record is copied down with
its pointers re-aimed at its new place, the stored words (`0x482220`) and the byte (`0x48222D`)
moved together.

- **The stamp and its removal read the stored words**: `0x482270` at `0x4822CD`/`0x4822D0`,
  `0x481930` at `0x48197F`/`0x481982`, `0x481D50` at `0x481DAD`/`0x481DB0`, each re-testing the
  column and row (`0x482304`, `0x4819B7`, `0x481DE4`). Every removal goes through `0x481D50`: the
  emitter's own (`0x482644`, when the old height byte is non-zero), the death removal `0x482090`
  (called at `0x486845`; it builds the unit's record as `0x4827B0` does and removes at `0x482104`),
  and the temporary records' expiry (`0x482161`). So a record whose stored words the bound refused
  removes nothing. Nothing else writes the stored words in the ray-fan mode: the create writes 0
  (`0x485C0A`, `0x485C0E`), a saved game's restore writes back the saved pair and height byte
  together (`0x48724D`, `0x4872BF`), the temporary records' compaction moves words and byte together,
  and the circle's placements (`0x4818C4`, `0x482766`, `0x48277C`, `0x482A7C`, `0x482BE8`, and
  `0x4828EC` in the uncalled `0x482830`) run only with bit `0x4` clear.
- **The fix** (in `fix_los_shear`, the table): the row is fixed once, at `0x482615`, before it is
  compared with the stored one and stored — so every stamp and its later removal use one row by
  construction. The stub keeps stock's order of work and changes only `edi`: it saves `eax` (the
  height difference `0x48262B` reads), computes the own row `z >> 5` from `edi` before the shear,
  then stock's sheared row; if that is outside `main+0x14297` and the own row inside, it takes the
  own row and counts; then `movsx ecx,[edx]` as stock and back to `0x48261F`. The whole span is
  compared with the patch: the point `0x4825D4..0x482614`, and from the landing point through the
  bound it mirrors, `0x48261F..0x48266A` — the compare with the stored words, the early-out, the
  removal and the store the argument rests on. The stub's length and its two `rel8`s follow the
  counter's length, `LOS_COUNT_LEN`. The count is the own row taken, not a stamp made: when the
  column bound `0x48265B` then refuses, nothing is stamped.

**Two properties of the projection** follow from the same arithmetic. The cursor picker's point is
not the hovered unit but the world point under the pointer (`main+0x2CAA`, filled by `0x484B50`),
whose sheared row is the pointer's own row in the world view — **MEASURED**: with the view at the
map's north edge (eye 116, 0), the pointer at game (500, 40), 8 px below the view's top, read the
world point (488, 85, 50), sheared row 0. So on the map its reads are inside the grid, and the cursor
copies change nothing there. And `z − y/2` is also where the unit is drawn [INFERRED from the
projection]: a unit whose sheared row is negative is drawn above the view's top, where the pointer
cannot reach it — the human cannot click the north atlas below; the fix lets their units acquire it,
not their pointer.

**MEASURED, previous builds.** With the first four reads patched, the same fixture: the flak aimed at
`north` first and shot it down on station at z 40.1 within 5 s, then `control`. Under Permanent line
of sight (`scenario load --los 0`, `LosType` read 12, so every point went through the mapped grid) the
same: `north` hit first at z 40.0 within 2.5 s and dead within 5 s, then `control`.

**MEASURED, every copy patched** (build `74fcfc2`, raised, True line of sight, the same fixture and
driver, one launch): the table installed 176 sites (the stock-limits build, to the menu: 50). All 43
patched sites were read back from the running process: each is a jump to a stub that disassembles
as intended, with stock's exits and its function's counter. The flak shot `control`, then `north` on
station (z 42.2, altitude 175), both within the first seconds. The counters:

| function | own row taken |
|---|---|
| `0x465AC0` | 2548 |
| `0x467440` | 4 |
| `0x49BE60` | 24 |
| `0x466DC0` | 2 |
| `0x474170` | 679 342 |
| `0x475470` | 2 395 955 |
| the other ten | 0 |

The particle counts are fire and smoke at altitude near the north edge, from the flak bursts and the
atlases' deaths; drawn at a negative row, they land above the view. **Not reached by this fixture**,
so resting on the disassembly and on the live decode alone: `0x408090` (True line of sight reads the
LOS grid inline), the AI probe `0x407E90`, the order resolver (the AI's targets were the human's
units, well inside the grid, and the human could not point at `north`), the cursor picker (above),
the site test, the feature helper, positional sound, and the leaves `0x473590`, `0x473A00` and
`0x4745E0`.

**MEASURED, the sight emitter** (`scenarios/b1-los-emit.json` on Two Continents, True line of sight:
a human ARMATLAS held by patrol at (400, 40), altitude 175, an AI CORSOLAR below it, the human's other
unit far south; the human's LOS map, 336 × 400 cells, read through the player record). Previous build
(`d87e506`): the atlas stores column 12, row −2, height byte 0, and rows 0–9 of the map are all 0 —
the atlas sees nothing. Build `3aaee36`: column 12, row 1 (its own), height byte 183, and 111 lit
cells in rows 0–9, the ray fan; the table installs 179 sites (the stock-limits build, to the menu:
53), every one of the 44 patched sites reads back from the running process as intended, and the
emitter's counter read 747. **Pairing**: the atlas then shuttled twelve times between z 250 and z 30
(the stored row moving between stock's in-bounds rows and the own row the fix takes) and was sent to
z 1100; with it at z 1055, rows 0–13 were all 0 and no cell of the map held 200 or more — a removal
without its stamp would wrap a cell's count below zero, a stamp without its removal would leave the
edge lit. A second game with four atlases, five ARMPW and an ARMFLEA shuttling by the north edge for
about a minute, then sent south, left lit only columns 16–29 of rows 0–8, the ARMFLEA's own stamp (it
did not move: stored at column 24, row 0, stock's row), and no cell at 200 or more. Build
`228fdee` (the whole span compared, the stub's `rel8`s from `LOS_COUNT_LEN`), one launch of the same
fixture: 179 sites installed, all 44 patched sites read back as intended — the emitter's two
branches landing after the count — the point `0x4825D4..0x482614` and `0x48261F..0x48266A` read
back equal to the retail exe, and the atlas again stored row 1 and lit 111 cells of rows 0–9.

## The screen fog grid — where it is allocated, and every cell the builder reads — mapped by us

[MEASURED 2026-09-09, this project — `objdump -d -M intel` of the pristine Steam build over
`0x483BB8..0x483CA6` and `0x4843C0..0x4848D6`, plus a live oracle: `tagpu_fogwide.c` replicates
the builder in C and, under `tagpu_fogwide_check.on`, rebuilds over the engine's own window and
compares. **0 differing bytes of 720 (30×24, 1024×768) and of 1972 (58×34, 1920×1080)**, 357 and
1555 non-zero cells respectively. The overlay that consumes the grid is `terrain-depth.md` §5.1;
what is new here is its **allocation** and the **cell-by-cell reads** of `0x4843C0`.]

### The allocation, at map load — `0x483BB8..0x483CA6`

The struct behind `*(main+0x1421F)` is built **once per map**, inside `LoadMap 0x483610`, from the
engine's viewport size — and nothing resizes it afterwards:

```
483bbf  esi = [main+0x37E37]          ; viewW      (1792 at 1920x1080)
483bc5  ebx = [main+0x37E3B]          ; viewH      (1016)
483c03  call 0x4B4F10 (16)            ; the {u16* buf; int cols; int rows; int cells} struct
483c28  [main+0x1421F] = it
        cols = viewW/32 + (viewW % 32 ? 3 : 2)      ; 483c1e..483c79
        rows = viewH/32 + (viewH % 32 ? 3 : 2)
483c84  cells = (cols*rows + 7) & ~7                ; ROUNDED UP TO A MULTIPLE OF 8
483c96  buf   = malloc(cells * 2)
```

Three consequences, all of which have bitten:

- **`cells` is the allocation, not `cols*rows`.** A reader that asserts `cells == cols*rows`
  accepts 1024×768 (30×24 = 720, already a multiple of 8) and **refuses 1920×1080** (58×34 = 1972
  against an allocated 1976). `tagpu_native.c` asserted exactly that until 2026-09-09, and the
  refusal is not a degraded fog — it clears `fogMode`, so at 1080p there was **no fog at all**: no
  black over unexplored ground, no grey band, the whole map drawn lit at every zoom.
- **The grid spans the 1x viewport and about two cells more**, for ever. It cannot be made to
  cover a zoomed-out view by asking it to: the size is a map-load decision.
- **The origin is not stored** — the builder recomputes it from the eye every time (below), so
  the grid cannot be re-anchored either without lying to the builder about `main+0x1431F`.

### The builder `0x4843C0` — void, no args, `ret` @ `0x4848D6`

Clears `cells*2` bytes, then walks the map cells `[col0, col0+cols) × [row0, row0+rows)`:

| Where | What it reads |
| --- | --- |
| `0x4843CD` | `main+0x2A43` — the **viewed** player id, whose sight the screen shows (not `+0x2A42`, the controlled one; see *The order-marker chain*, "`+0x2A42` is the controlled player"). `mask = 1 << id` |
| `0x4843F0` | `ebp = main + 0x1B63 + id*0x14B + 0x7C` — that player's LOS block: `{u8* counters; i32 w; i32 h}` at `+0`/`+4`/`+8` |
| `0x48442D..0x484485` | `col0 = eyeX/32 − (eyeX % 32 < 16)`, `row0` the same from `eyeY`. Equivalently **origin = `32·col0 + 16`** |
| `0x4844B9`/`0x4844C7` | `cx`/`cy` bounded against the LOS block's own `w`/`h`, **unsigned**, so a negative index is skipped |
| `0x4844D7` | `los[cy*w + cx]` — one byte, an overlap **counter**; 0 means out of LOS |
| `0x4844E7` | `LosType & 2` — the grey mask is written only in true-LOS mode |
| `0x4845A9` | `idx = (main[0x14233] * cy) / 2 + cx` into `*(main+0x14273)`, **u16** per tile, one **bit per player**. The row stride is `PLOT_C` *bytes*, i.e. the map is `PLOT_C/2` tiles wide; the allocation is `PLOT_C*PLOT_R/2` bytes (`0x483CF6`), so the last index used is exactly its last entry |

A cell that is dark ORs **a different corner bit into each of the four grid entries around it** —
entry `(cx−col0, cy−row0)` gets bit 1, `(cx−col0−1, cy−row0)` bit 2, `(…, cy−row0−1)` bits 4 and
8 — into byte `+1` for the out-of-LOS mask (`0x4844FF..0x4845A2`) and byte `+0` for the
unexplored one (`0x4845CC..0x484678`). The two blocks fall through, so a cell that is both sets
both. **The last column and the last row of any window are therefore short their right/bottom
corners**, because the cell that would supply them is outside the loop.

**That is a trap for anything that CLAMPS into the grid** [MEASURED 2026-09-10]. Bilinear
coverage over an entry whose right corners are 0 falls to 0 as you cross it, so a sampler that
clamps a world point past the grid onto `cols − ε` lands on corners nobody wrote and reads **no
fog** — the most dangerous answer available, because the caller then draws unexplored ground lit.
The engine never meets it: its own overlay paints the viewport only, and **the viewport's right
and bottom edges are always inside the grid's last column and row** — enumerated over the
allocation arithmetic above for every viewport from 64 to 16384 and every eye residue, the worst
slack is **1 px** (a 64-px viewport at `eye % 32 == 15`) and **16 px** for the negative eyes the
zoom's widened camera range produces. At the 1792×1016 viewport of a 1920×1080 screen — measured
`cols = 58`, `rows = 34` — it is `16 − r` or `48 − r` px horizontally and `24 − r` or `56 − r`
vertically, `r` being `eye % 32`. Ours *does* meet it, because a zoomed-out view reaches past the
grid by design, which is why `taFog` clamps to `uFogDim − 1.0` and not to the last cell
(`terrain-depth.md` §8a).

### The four border completions — `0x4846A1..0x4848CD`, and the index that is only right by luck

Off the map there is no cell to darken a corner, so each edge copies the corner bits it does have
outward: top `4→1, 8→2` (`0x4846AE`), bottom `1→4, 2→8` (`0x484731`), left `8→4, 2→1`
(`0x4847CA`), right `4→8, 1→2` (`0x48485A`); each pair is gated on `LosType & 2` for the grey
byte and unconditional for the black one, and **the four run in that order**, reading bits an
earlier one may have set.

The engine writes them into grid **row 0**, **row `rows−2`**, **column 0** and **column
`cols−2`**. Those are not the general answer — they are the straddling entries only while the
engine's own grid reaches no more than one cell past the map, which an eye in its own range
`[0, extent − W]` guarantees (`row0` is then 0 or −1). The entry that straddles an edge is the one whose corners are on the map on one side and
off it on the other:

| edge | gate | straddling entry | the engine's literal |
|---|---|---|---|
| top | `row0 < 0` | `gy = −row0 − 1` | `0` |
| bottom | `row0 + rows > PLOT_R/2` | `gy = PLOT_R/2 − 1 − row0` | `rows − 2` |
| left | `col0 < 0` | `gx = −col0 − 1` | `0` |
| right | `col0 + cols > PLOT_C/2` | `gx = PLOT_C/2 − 1 − col0` | `cols − 2` |

The two columns agree whenever the window overshoots by exactly one cell, which is the only case
an eye in the engine's own range produces. A window that reaches further must use the derived
index, or the completion lands rows out in open water and the shoreline entry keeps a half-set
mask. `fogwide`'s window always does; the engine's does whenever the camera's centre range has
put the eye past `[0, extent − W]`, and the native pass then draws from `fogwide`'s grid instead
(`tagpu_zoom_wide_fog`). The addresses stay inside the grid either way — each completion loops
over the grid's own `[0, cols)` or `[0, rows)` — so what goes wrong is the content, not memory.

Off the straddle the literal line lies wholly off the map, where every entry is zero, so the
engine's completion there is a no-op and the two builders differ only on the lines of that table:
the engine leaves the straddling line half-set, ours completes it. `fogwide`'s oracle
(`tagpu_fogwide_check.on`) therefore leaves both lines out of the comparison for each completion
whose gate is up and whose two indices differ, and reports them as `skipped=`.

### LoadMap's feature placement, and a saved game [DISASSEMBLED 2026-09-24, objdump of the pristine build]

The map edge's mirror draws the map as its TNT lays it out ([GPU status](../gpu-status.html) §2.90),
and **no moment of a saved game's load has that in the feature grid**. So the list is built from
the TNT itself, by LoadMap's own placement rules, at the one point every path reaches.

**LoadMap `0x483610`** runs on the loader thread; its one caller is `0x4918C0`. After
`0x421F20` has loaded the map's FeatureDefs (called at `0x4839F4`; `main+0x14253` is their count,
and `0x4224B0` only ever appends a def, so an index never changes meaning), it takes one of two
paths by the TNT's version, `[esp+0x18]` (`0x1020` or `0x2000`):

| path | records | always | a new game only |
| --- | --- | --- | --- |
| v1, `[esp+0x48]` | 8 bytes a cell: height `+0`, feature `u8` at `+2` | the height loop `0x483A14..0x483A31` | `0x483A3E`/`0x483A44`: `main+0x38D6B` non-zero jumps to the join; else every cell whose feature is below `[esp+0x50]` (`0xFC`) is spawned, `0x483A7E` |
| v2, `[esp+0x4C]` | 4 bytes a cell: height `+0`, feature `u16` at `+1` | the height loop `0x483AB9..0x483AF2`, which also spawns `0xFFFC` — the void marker — on every cell whose feature is `0xFFFC` (`0x483ACA..0x483ADE`) | `0x483B02`/`0x483B0A`: the same test; else every cell whose feature is below `[esp+0x50]` (`0xFFFB`) is spawned, `0x483B38`, and then `0x423160` adds the schema's own features (`0x483B4E`) |

Every spawn is `SpawnFeatureOnMap 0x423C50(cell, def, 0, 0, 0xA)` in row order, and **every branch
of both paths lands on `0x483B53`** (`mov edx,[esp+0x40]; shl edx,0xa`, seven position-independent
bytes whose flags the `shl` overwrites), before the TNT buffer is freed at `0x483BA6`. The frame
slots above are LoadMap's at that address. `main+0x38D6B` is the saved game being loaded: set at
`0x492655`, cleared at `0x4915F4`. `0x423160` walks the schema's list at `[main+0x391E9]+0xDBC`,
count `+0xDC0`, `0x88`-byte records — features the `.ota` places, which are not the TNT's.

**`SpawnFeatureOnMap 0x423C50`**: a footprint (`FeatureDef+0x94` × `+0x96`, 16-px cells, from the
anchor right and down) that runs past the map is refused. Then, footprint cell by footprint cell,
anything not empty (`0xFFFF`) goes to `FEATURES_Destroy 0x4246B0(cell, 0)`, and a refusal
abandons the spawn **keeping whatever it destroyed so far**. A 3DO def (`+0xFE` bit 0 clear) then
takes a wreck-pool record, and none left abandons it too. The anchor takes the def, every other
footprint cell `0xFFFE` with its offset back to the anchor in `+0x0A` (dz) and `+0x0B` (dx).
**`FEATURES_Destroy`** refuses a def at `0xFFFB` or above (the void and border markers) and, with
`force` 0, an indestructible def ([its entry](memory.md#0x4246b0-features_destroy-the-wreck-path)).

**A saved game's features come back later**, from the save: `0x424C00`, called at `0x43265A` from
the game-load routine `0x432610`, calls `LoadFeature 0x4224B0` (`0x424DE1`)
and places each record with `GetGridPosPLOT 0x481550` and `SpawnFeatureOnMap` (`0x424FBF`,
`0x425050`), then restores its sequence (the calls at `0x42507E`, `0x42509D`, `0x4250BB`, below).
**Between the two, `0x4833B0` masks the border** (called at `0x483CF1`, after the join)
[DISASSEMBLED 2026-09-24]. It writes the scroll extent first (`main+0x1422B`/`+0x1422F`, the map's
pixel size less `0x20` and `0x80`), then makes four passes over the grid. Every store is guarded by
the same pair, `cmp dx,0xffff; je` then `cmp dx,0xfffe; jne`, so it turns an EMPTY cell and a
footprint cell (`0xFFFE`) alike into `0xFFFD` and leaves an anchor alone: five stores, each after
its `0xFFFE` compare (`0x483439`, `0x483455`, `0x4834CC`, `0x483572`, `0x4835E8`).

| pass | cells | store |
| --- | --- | --- |
| 1 | columns W−2 and W−1 of every row (`+0x08` and `+0x15` of the cell at W−2), unconditionally | `0x483440`, `0x48345C` |
| 2 | down each column from the top, while the projected y, row·16 − height/2, is negative | `0x4834D3` |
| 3 | up each column from the bottom, while row·16 − height/2 is past the extent's height `main+0x1422F` — the cell ONE ROW ABOVE the one tested (`sub eax, W·13`) | `0x483579` |
| 4 | only when `[main+0x391E9]+0xD44` is set (a lava world [INFERRED]): every cell whose 2×2 minimum height `+0x06` is at or below the sea level `main+0x1427F` | `0x4835EF` |

A spawn onto `0xFFFD` is refused by `FEATURES_Destroy` (a def at `0xFFFB` and up). A new game's TNT
features are down when the mask runs, so their ANCHORS survive it, but a footprint cell of theirs
that the mask reaches becomes `0xFFFD`. A saved game's features are not down yet: the loader body
`0x497180` calls `LoadGameData_Main 0x4917D0` at `0x497581` — which reaches LoadMap at `0x4918C0`,
and the mask with it — and only later, at `0x497B29`, the game-load routine `0x432610` that restores
them, all on the loader thread. That call is conditional [DISASSEMBLED 2026-09-24,
`0x497AEF..0x497B45`]: it runs when the saved game's TDF `main+0x38D6B` is loaded (non-zero) and
`0x4B48F0` (a `thiscall` on that TDF, after `0x4B4560("summary")`) finds no `BetweenMissions` key
(the string at `0x504924`, `test eax,eax; jne 0x497B40`). With the key, the loader calls `0x488310`
and `0x41D1F0` instead and restores nothing; with no TDF at all (a new game) it asks `0x435100` of
`main+0x391E9` and, on 1, calls the same two. So **a saved feature whose footprint touches a masked
cell does not come back from a save at all** — a stock defect, patched: "Engine defects we patch",
"A saved game's features on the map's border", which has the readers of `0xFFFD`, the restore's
records and the measurements (on Two Continents, 51 of 4 893 features lost by the stock build, among
them the trees at (25, 0), (5, 1), (27, 2) and (33, 2)). The mirror does not depend on it (below).

**The mirror's list** (`tagpu_packet_pub.c` `mapfeat_at_load`, a stub at the join) replays the
paths above on a private `W × H` grid: v2's void markers first, then the TNT's features in row
order, with the spawn's refusals, its destroys, their resolution through the anchor offsets and
the pool count (`WR_COUNT`) as LoadMap applies them. What it keeps is every GAF anchor that
survives, row-major, with the anchor scan's height term from the grid's heights (written on both
paths before the join). **The rule: the mirror's map is the MAP FILE alone.** The schema's features
are not in it — the ones `0x423160` places on a new v2 game through `LoadFeature 0x4224B0` and
`SpawnFeatureOnMap 0x423C50` after the TNT's — and neither is the effect of those spawns on the
TNT's own: a TNT feature a schema spawn destroys stays in the list. Every join empties the hand-over
slot before it builds (`mapfeat_unpark`), so a level whose build fails has no list and a black edge,
never a list parked by an earlier, abandoned load.
MEASURED 2026-09-24 against the engine's own grid at the join, on a new game of each map (a
diagnostic build, not the shipped one): Two Continents 4 893 of 4 893, Pincushion 1 536 (68
spawns refused, as the engine refuses them), Lava Alley 800 (15 refused), Town & Country 6 206,
Eastside Westside 3 001, Core Prime 0 — identical cell by cell; Steel Jungle and King of the Hill
3 779 and 859 alike, the grid holding one and eight features more, which are the schema's. On a
saved game the list is the same as on the new one, by construction. **Its cost**, once a load on
the loader thread: 1.86–2.18 ms on Two Continents (672 × 800 cells), 6.89 ms on Seven Islands
(1280 × 1280, the largest stock map), built only when the session draws with Vulkan.

### The two fog lattices, and the minimap's four surfaces [VERIFIED 2026-09-12, landing 4b and 4c]

| what | where | lifetime, and the check that stands for it |
| --- | --- | --- |
| the fog overlay's call site | `0x4848E0`, called once from `0x469D8E` inside DrawGameScreen | **behind `test ebx,ebx / je` at `0x469D85`, and `ebx` there is the function's FIRST ARGUMENT** — reloaded from `[esp+0x228]` at `0x469BC9` against a frame of `sub esp,0x214` plus four pushes, i.e. the entry `[esp+4]`, which is `drawUnits`. The in-play call at `0x4969CD` passes 1 for both arguments, so this site runs on every in-play draw and an observer on it can be used as a per-draw liveness stamp [VERIFIED 2026-09-12] |
| the engine's screen fog grid | descriptor `*(main+0x1421F)` = `{u16* buf, i32 cols, i32 rows, i32 cells}` | built once per map by LoadMap (`0x483C03` / `0x483C96`) and only REWRITTEN by the builder `0x4843C0`; freed by the map-free `0x483DD0` inside the teardown cascade. **The relation is the check**: the builder rounds the cell count up to a multiple of 8 before allocating (`0x483C84`: add 7, and ~7), so `cells == ((cols*rows + 7) & ~7)` says the three numbers describe one block. The grid's world origin is `32·col0 + 16`, `col0` the builder's half-cell-rounded eye, stored nowhere — which is why the publisher keeps a build record of the eye each build read, off LosType bit 3 (row "who sets `main+0x14281` bit 3"), rather than taking the one the packet carries |
| the minimap's three 8bpp surfaces | `main+0x142DF` the fog base, `+0x142E3` the unshaded base, `+0x142DB` the fog+dots composite; each a `{i32 w, i32 h, i32 pitch, u8* base}` descriptor | the minimap build `0x4669B0`, from the level load at `0x4919C3`, stores all three once; `0x466AA0` frees and NULLS them inside the teardown cascade. A descriptor carries a base AND a pitch, so a torn one is a wild read and not a stale picture: the publisher cross-checks `w`/`h` across all three and refuses a pitch below the width or past 4096 |
| the minimap's box and view rect | `main+0x142E7`/`+0x142E9`/`+0x142EB`/`+0x142ED` the box the engine fitted it into (i16, ITS screen px), `+0x142CB` the view box (4 × i32, screen px, edges inclusive), `+0xDD9` its palette index | values. `+0x142F1` bit 1 is NOT a gate: it is `DrawMinimap 0x466B00`'s dirty flag and `0x466B16` clears it in the same breath |
| the level's minimap picture | `main+0x1426B`, a GAF frame (TED_GENERATED_PIC) | **alive for the WHOLE LEVEL** [CORRECTED 2026-09-12, objdump of the pristine build — this fork's own notes said it was alive only inside `BuildMinimapSurface 0x466780`, and that is false]. LoadMap loads it (`0x4838F5` → `0x4B8DA0`) and stores it at `0x483900`, or stores NULL at `0x483936` when the file is absent; `0x466780` reads it at `0x46684F` and does NOT null it; the only thing that frees it is `0x483DFE` (`MEM_Free 0x4D85A0`) inside `0x483DD0`, followed by the null at `0x483E0B` — and `0x483DD0`'s only caller is `0x491BB3`, inside the level teardown cascade `0x491B60`. So the packet's publisher can decode it itself on the level's first in-play draw, and that is what let the fork delete its last loader-thread observer |
| the cursor | `[0x51FBD0]+0x1B2` the sprite record — itself a GAF frame header — and `+0x1B6`/`+0x1BA` where it was last drawn (not copied since 2026-09-23: the packet carries the record alone, as `cur_rec`) | the record comes out of the cursor table, loaded once per session |

**The FeatureDef array grows one record at a time, and the count is written LAST.** `0x422520`
reallocs `main+0x1426F` to `(NumFeatureDefs + 1) · 0x100` (`0x422543`, through `0x4D84A0`), stores
the new base at `0x422558`, the caller fills the new record, and only then does `0x422DAC` write
`NumFeatureDefs + 1` back to `main+0x14253`. **So the count never describes more records than the
allocation holds**, and a live count read after a live base is a conservative bound on that base.
The teardown is the mirror: `0x42227D` frees the array, `0x42228B` nulls the base, `0x422299`
zeroes the count — base first, so a reader that tests the base never sees a live pointer with a
dead count. **[MEASURED FROM THE DISASSEMBLY 2026-09-12, a landing review]**: this is what licenses
`tagpu_feat.c` and `tagpu_scaffold.c` to bound a feature index by `min(live count, packet count)`;
the packet's alone belongs to the packet's level, and a held packet from a 442-def map would
authorise 442 records of the next map's 30-record array.

**`MODEL_PTRS` is the other order, and that is equally load-bearing.** `0x42D542` computes
`UNITINFOCount` into `main+0x1438F` from the unit-def array's extent (`(edi − main+0x1439B) /
0x249`), and only afterwards does `0x42D693` allocate `count · 4` and `0x42D6AA` store the base
into `main+0x14377`. **A non-NULL base therefore implies the count already describes it**, which is
why `tagpu_native.c`'s `model_root` may read the base live and bound by the packet's `udef_count`.
`0x42DCB6` frees the table and `0x42DCD8` nulls the base.

**The two kamikaze circles at `0x4390F0`, and which test gates which.** `0x439115` tests the def's
`+0x241` bit 28 and `0x439125` loads `def+0x220`, the ExplodeAs weapon; **both circles are skipped
only when one of those two fails**, and the second test is on the POINTER. With a weapon in hand,
`0x439137` takes its `u16 +0xD6` (AoE) and `0x43914B` halves it, and the pulse radius is
`clamp((GameTime % 60) · (AoE>>1) · 2 / 60, 8, AoE>>1)` — the clamps in that order at `0x439161`
and `0x439168`, so an `AoE>>1` under 8 ends at `AoE>>1` and not at 8, including 0. Execution then
falls through unconditionally to `0x439196`, which draws a second circle of radius `def+0x218`
(kamikazedistance) whenever the pointer at `[esp+0x18]` names a non-zero dword. **[ADDED
2026-09-12, a landing review]** `tagpu_order.c` returned early on the radius instead of the
pointer and so dropped that second circle for a weapon with `AoE>>1 == 0`; nothing in stock content
has one.

**What it costs, and the one thing that made it worth caching.** The whole publish is **p50 70 µs,
p99 322 µs** at 354 units, 5443 pieces, 35 wrecks and 760 anchors. The anchor scan alone was **168
of the first 200 µs**: the rect is the viewport at the zoom floor plus a margin, 465 × 273 cells
here, and 126 945 `u16` loads is what that costs. The grid is SIM STATE — the def index, the flags
nibble and the wreck index are written by the tick and by nothing else — so two publishes of one
tick over one rect must produce the same table, and the second reuses the first. Measured over one
interval: **735 scans against 4567 reuses**, which is the ratio 298 published frames a second
against 60 ticks predicts.

**Negative results.** No engine field is written by any of this. The `end` pointer is read once
per publish for the assertion and never used as a bound. `main+0x1421F`, the screen fog
descriptor, is deliberately NOT copied — it is landing 4b's, and the unit pass still reads it
itself under the fence. Neither is `main+0x14287`'s height byte for any cell that does not anchor
a feature: a unit's own ground height rides in its packet entry, and nothing else asked.
