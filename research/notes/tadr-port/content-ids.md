# A′. Content IDs — the plan

## Summary

Two ceilings split out of [section A](raised-limits.md) come into our stack: **unit-type IDs**
(512 → 16 384) and **weapon IDs** (256 → 4096). So does the renderer cache that the first one makes
easy to overflow. That is three landings. The owner decided every choice below on 2026-09-24
**[DECIDED]**. **A′1 and A′2 are done; A′3 is planned.** The disassembly behind each fact is in
[the evidence pass](limits-evidence.md), §8 and §9. The rules shared by every group are in
[the port overview](overview.md#standing-rules-decided-2026-09-23). Section A's last item, the
composite scratch bound, is [landing 7 of that plan](raised-limits.md#the-landings), done before
these three.

Three findings shaped the plan:

- **Stock breaks at 512 real types because it lacks a count check.** The first type with ID 512
  sets a bit past a 64-byte category mask, a heap write out of bounds. Every other per-type table
  in the engine is already sized by its own count.
- **The weapon array sits inside the main game structure.** A weapon at ID 256 lands on the
  projectile pool's header. All 21 references to the array are known, so the array can move.
- **Three network messages carry the weapon ID as one byte.** Two have room to widen in place. The
  interceptor's `0x0E` does not.
- **Memory bounds a large mod, not the type count.** Every type loads its own model and script,
  every game, at their file sizes: 10–12 KB a stock type, 17 KB for a clone of the Peewee and 7 KB
  for A′2's generated types (measured). On the reference setup a 32-bit process with the Vulkan
  stack loaded can still allocate 1.44 GB, and 16 383 of the generated types played two games in
  one process at 4K. A mod of heavy types may not fit, whatever the cap (evidence §8, *Memory*).
- **Found by the landing: the network join matches types by a weak checksum.** The unit sync keys
  each type on four byte-lane sums of its FBI, and two types with one key hold the battle room at
  SYNCHING for good. Stock content has no such pair; a mod of near-identical FBIs has thousands.

## Scope and values

| Limit | Stock | Ours | What it touches | Landing |
|---|---|---|---|---|
| the Vulkan unit pass's caches | 512 models, 1024 (type, owner) streams a frame | **done**: 30 721 each, the bound a frame cannot exceed, recycling from 512 / 1024 | visual: past them the frame's last objects were silently not drawn | A′1 |
| unit-type IDs | 512 real types | **done**: 16 383 real types (16 384 counting `None`) | **simulation as content**: category masks, the AI, selection | A′2 |
| a builder's build list | 30 entries, appended unbounded | **done**: the whole list | a heap overrun at game load | A′2 |
| a download file's menu entries | 5 a file, filled unbounded | **done**: as many as the files hold | a heap overrun at game load | A′2 |
| running out of memory | "Your hard disk may be full", then exit | an honest message, then the same exit | what a player reads | A′2 |
| the unit sync's keys | two types may share one, and the join never ends | unique, re-keyed at load | a network game's start | A′2 |
| the network join's pace | 4 types a lobby tick | 64 | the join's time | A′2 |
| weapon IDs | 256 | **4096** | **simulation, and the wire** | A′3 |

## Decisions

### Unit types

**The ceiling is 16 384 slots.** The category masks become 2048 bytes, which is 16 384 bits. That
is what TADR's widened masks actually hold: its nominal 16 000 has no mechanism behind it. It is
also the bound our own code already uses: `unit_type_index` in `tagpu_scenario.c` and the catalogue's
`MAX_DEFS`. Masks cost about 115 KB at a game start (about 56 of them), against about 4 KB stock.

**A mod over the ceiling is refused, and the game exits.** The menu-time loader counts the types
before it allocates the def array (`0x42AA65`). Over the ceiling, a dialog names the count and the
ceiling, and the game exits. No mask can be overrun. A mod this build cannot play then says so,
instead of playing as a different mod. The owner rejected dropping the extra types, because which
types are dropped depends on the order the engine walks the archives: a builder would lose menu
entries with no visible reason, and two peers whose files differ would drop different types.

**The masks widen in place.** TADR's 17 writes are the complete set: the five 16-dword loops and
three stack frames (evidence §8). Immediates become byte patches in the limits block's site table.
The frames of `0x406DB0`, `0x406E40` and `0x48BE00` grow through our branch stubs, and the engine's
code stays as it is. A 2048-byte frame fits inside one 4 KB page, so no stack probe is needed.
Ctrl-A, B, C and F need no site of their own: they only read masks, so widening the allocation
covers them.

**What rides in the same landing:**

- `mask_has` in `tagpu_weapons.c` reads `mask[type >> 5]` for a `u16` type taken from the unit's
  `+0xA6` with no bound. It is bounded by the mask's capacity.
- `WPN_MAXDEFS` (4096) follows the ceiling. Today a type past it silently keeps three weapons.
- **The build list, fixed beyond TADR.** This is the list of what each builder may build: the
  `canbuild1..N` keys of `gamedata\sidedata.tdf`'s `[CANBUILD]` section. Two writers fill it:
  - **At game load**, the engine reads each builder's entries into one shared 0x3C-byte heap block
    it names `TEMP UTYPE LIST`, which holds 30 `u16` IDs (`0x42D971`). The append loop
    (`0x42DA46..0x42DA99`) stops only when a key is missing. The builder then gets its own
    0x3C-byte copy of exactly 30 entries at def `+0x156` (`0x42DACA`, `rep movs` of 15 dwords),
    with the real count at `+0x152`.
  - **Later**, `0x42BE30` appends the builder's download-menu entries to the same list while the
    count is at most 30, so it can write entry 30, two bytes past the block: a stock off-by-one.

  The AI's pick (`0x40BDB0`) and its debug listing loop to the count. So a builder with more than
  30 entries overruns the shared block, and they read past its copy. Every other reader only tests
  the pointer, and no reader assumes 30 (evidence §8). This is a stock defect, but mods with
  thousands of types are the ones with long lists. Stock's longest list is exactly 30 (`corch`,
  `corcsa`), with no download entries.

  The fix is always on: **the builder keeps its whole list [DECIDED 2026-09-24]**.
  - The shared block grows as it fills.
  - Each copy holds `max(30, count)` entries.
  - The appender grows the copy itself when it runs out of room: a new block from `0x4D83B0`, the
    old one freed with `0x4D85A0`, and its cap of 30 removed. It runs on the loader thread before
    any reader exists.

  A builder with 30 entries or fewer, which covers every stock builder, gets stock's exact block.
  Clamping the count to 30 was rejected: a mod's builder, and the AI, would silently lose every
  option past the 30th.
- **The download-menu records, fixed beyond TADR [DECIDED 2026-09-24].**
  - The engine makes one 0xBD-byte record per `download\*.tdf` file (`0x42DCF0`, count at
    `[main+0x391C7]`, block at `[main+0x391CB]`), with room for five entries. The fill loop
    (`0x42DDD5..0x42DF0C`) never checks the count, so a file with six or more writes past its
    record, and the last file's past the heap block.
  - Stock files hold at most four.
  - The block is sized by the total number of entries across all files, and a large file continues
    into as many extra records as it needs. Its readers walk records and need no change: the
    build menu (`0x41AE0F`), the page count (`0x42DF72`), the downloadable check (`0x42E04B`) and
    the appender (`0x42BE30`).
  - Every entry the mod wrote appears, and stock's files get stock's exact records.
  - The owner rejected capping at five, which silently drops a mod's buttons.

**Memory [DECIDED 2026-09-24].** The cap bounds the masks. It cannot bound memory, which depends on
each mod's models (evidence §8, *Memory*).

- **Running out of memory shows an honest message.** The engine's handler (`0x49E700`, installed for
  the whole process) writes a crash dump to `ErrorLog.txt` and then shows "Out of memory! Your hard
  disk may be full" before it exits. It fires on any failed allocation. Ours keeps that path, the
  dump and the exit included, and replaces the text: the game is a 32-bit program that has used
  the memory it can address, and this game loaded N unit types.
  - A pre-flight estimate at startup was rejected. Fragmentation and the graphics driver's share
    are unknown, so an estimate could refuse a mod that fits, or pass one that does not.
- **The cap stays 16 384 whatever the ceiling test finds.** A′2 records how many of the test's
  types fit at each resolution, and the address space used, in this page and in gpu-status.
  - Lowering the cap to what passes was rejected: it holds only for content as light as the
    test's, and refuses lighter mods that would fit.
- **Models are not shared between types.** Types that name the same 3DO each load their own copy.
  - Sharing is feasible, but it saves nothing on stock content, where all 278 types name different
    models. It would also make two types show the same animated-texture frames, because the engine
    writes its frame cursors into the loaded model.
  - It is recorded as the lever to pull if a real mod runs out of memory (evidence §8).
- **Masks stay a fixed 2 KB.** A pathological mod with unique category strings on every type would
  make about 80 000 masks, up to 164 MB. That still fits the measured headroom, and the message
  covers it if it does not. Sizing masks by the loaded count was rejected: it turns five
  immediates into loads from a global, with an ordering to prove, for memory only such a mod uses.

**The lobby join [DECIDED 2026-09-24, and again after A′2 measured it].** When a player joins, the
unit-sync handshake sends one 14-byte message per type per peer, and the engine matches each with
a linear search (`0x46D755`, `0x46D9E3`, `0x46DA7A`, `0x46D906`). The plan was: at 5 seconds or less,
record the time; over 5, replace the four searches with a lookup keyed on `+0x13E`.

A′2 measured 20 s at 16 383 types, but not in the searches. The joiner sends its checksums four
types a lobby tick (`0x46DE8F`), and its cursor moved at a steady ~880 types a second while the host
kept up; a rejoin in the same processes, whose searches ran again over a fresh record, took under
2 s. The owner decided, on those numbers:

- **The pace rises to 64 types a tick** (`0x46DE91`, a raised-limits site). The first join at the
  ceiling then takes 12.5 s. What is left is the engine filling each type's value on first use
  (`0x42A610`, which reads the type's script and GUI files), which a lookup cannot shorten, so the
  searches stay the engine's.
- **Two types with one key are re-keyed at load**, a stock defect the test found (below), fixed in
  both builds.

**The unit sync's keys [DECIDED 2026-09-24].** The key at `+0x13E` is `0x4B6BA0`'s checksum of the
FBI file: four 8-bit lanes of byte sums and xors, not a CRC. The host keeps one entry a key in a
list that has to reach the type count, so two types with one key hold the battle room at SYNCHING
for good. The test's first FBIs, which differed only in their digits, gave 9 991 keys for 16 105
types, and the join never ended.

- After the menu-time load, every type whose key an earlier type holds takes the next value no
  type holds (`fix_sync_keys`, at the load's one call `0x42BD29`). Types that do not collide keep
  their keys, and the install's 278 names share none, so stock is unchanged.
- Peers with the same content compute the same keys. A type re-keyed on one peer only is reported
  not synced, which is what different content should get.
- The def array is write-protected after the load (the engine's `0x4D8710`), so the pass opens and
  seals it as the engine's own writers do.
- The owner chose this over documenting the limit: a mod built from copied FBIs can hit it at any
  type count, and the failure is a lobby that never starts, with no message.

**One limit outside our code.** TADR's `.tad` demo format keeps every unit-sync message in one
record with a 16-bit length (SRC, `Docs/saveformat.txt`), so recording a game breaks above about
2 340 types. The game is unaffected; the limit is documented, not fixed.

### The unit pass's caches

**Their own landing, before the type raise**, so the raise never exposes them. `tagpu_posebake.h`
held 512 models and 1024 (type, owner) streams, and they must hold every entry one frame draws.
Stock content reaches the stream limit: four skirmish players each with 257 or more types on
screen, or ten network players with 103. Sixteen thousand types make it likely. The caches are
sized from a bound the frame cannot exceed. A frame draws no more distinct models or streams than
it has posed objects, and the frame packet already bounds those: every unit its table holds, every
wreck, and a ghost for every queued build site plus the cursor's. So a mid-frame eviction becomes
unreachable by construction.

**What the old caches did past their size** was not the refusal this section first predicted. A
unit's hand-over record copies its entries' serials in `pd_record`, after every lookup of the
frame, so an entry evicted mid-frame hands its record the re-baked entry's serial rather than a
stale one, and the Vulkan pass's every-unit-or-none gate never fires. MEASURED: the frame's last
objects — every wreck and heap, and the last units of the last owner — were simply not drawn, with
no log line, while the caches re-baked 52 000 models and 239 000 streams in 35 seconds. Which step
drops them was not traced.

### Weapons

**The array moves.** `Weapons[]` becomes a DLL static of 4096 entries (0x115 bytes each, 1.1 MB)
for the life of the process, the same way landing 1 moved the explosion pool. The work is 12
operand swaps, 3 loop bounds, and a rewritten block in each of the two network receivers. The full
ID becomes `(pointer − base) / 0x115`, which is exact and bounded. The owner rejected TADR's
approach, which swaps a register so stock's address arithmetic lands in a second heap array. It
depends on 32-bit wrap-around and an aligned allocation, and it carries the defects listed in
evidence §9.

**A weapon with an out-of-range ID is skipped and logged.** That covers no `ID=` (read as −1) and
anything from 4096 up. Stock writes the first over the UI and input block, and the second past the
array. The loader refuses that one weapon and logs its name and ID. A unit that names it gets the
no-weapon entry, which is what stock already gives it: a stock weapon with no `ID=` can never be
found by name. An old mod with one malformed weapon still plays, minus the memory corruption.
**ID 0 stays legal**, because stock's own `NOWEAPON` uses it. A duplicate ID keeps stock's rule, the
later weapon wins, and is logged.

**The ID byte at weapon `+0x10A`.** Below 256 it stays exactly stock's value, the slot number
itself. From 256 up it is nonzero, because the AI's "armed" test reads it (`!= 0`). After this
landing no reader uses it as an index: the model path's byte index is replaced by the full slot
index.

**On the wire.** Every sender derives the ID from the weapon pointer. The packet sizes of `0x0D`
and `0x0F` do not change.

- **`0x0D` (weapon fired)** carries ID bits 8..11 in the high nibble of `+0x23`. Stock's four unit
  senders write that byte through `and dl,3`, so a stock-shaped packet decodes to its own ID. Our
  extra-weapons module uses bits 0..3. The meteor sender (`0x49DFA9`) leaves `+0x1A..+0x23`
  uninitialised, so it is patched to write them clean.
- **`0x0F` (feature hit)** carries bits 8..11 in bits 12..15 of the cell x. A cell is a 16.16 world
  position shifted right by 20, so no cell a projectile can reach is past 2047. Our sender still
  checks x and y against 0x1000.
- **`0x0E` (interceptor detonation)** has no room: its 14 bytes are the type, the target point and
  one ID byte. The receiver detonates the first local projectile that matches both, and the peer
  that owns that projectile applies its damage. **IDs below 256 keep sending the exact stock
  `0x0E`. IDs from 256 up send a companion message** carrying the target point and a `u16` ID. The
  stock receiver skips projectiles whose weapon is 256 or more, so the two messages can never match
  each other's projectiles. That makes the match exact by construction, and stock behaviour stays
  exact below 256. The owner rejected an in-place alternative, which put the high bits in the
  target's lowest fraction bits: it made two targets closer than 1/4096 px match, for every weapon.
  **Both receivers match on the index derived from the local projectile's weapon pointer**, not on
  the ID byte at `+0x10A`. A saved game writes that byte back on load (`0x487628`), so under a
  changed mod it can be stale.
  **The companion travels as a tagged `0x05` chat message [DECIDED 2026-09-24]**, the way TADR
  carries its `0x0D` extension:
  - **Why chat.** The engine, TADR's demo recorder and TAF's replay parser all split a packet into
    messages by each type's known length, so a type of our own would cost any of them the rest of
    the packet ([networking](../networking-lobbies.md)). With a chat message, a game on our DLL
    still records and replays, within the `.tad` limit above. It costs a 65-byte message instead
    of 14, and only when an interceptor with an ID of 256 or more fires.
  - **The layout.** `0x05`, a zero byte, our tag, then the payload. The tag is one outside TADR's
    set (`0x2B..0x31`, `0x60`).
  - **No display hook is needed.** The chat receiver `0x463CA0` returns at once for text that starts
    with a zero byte (`0x463CA7`): nothing is shown, logged or sounded. That is how TADR's tagged
    messages stay invisible.
  - **Our handler** is reached by pointing the dispatch table's `0x05` slot (`0x455F90`, data, no
    code bytes) at a stub that falls through to stock's `0x45522E`. It acts only on the game thread
    with the in-play handler (`0x499200`) installed, and drops the message otherwise. During a
    network load, the loader thread pumps messages too.

**Fixed beyond TADR, in the same landing.** We rewrite both ends of these messages anyway:

- **The `0x0F` sentinel clash.** The receiver reads the ID byte `0xFD`/`0xFE`/`0xFF` as "feature
  destroyed / burned / reclaimed". So a weapon with ID 253–255 that hits a feature makes every other
  peer destroy or reclaim it instead of damaging it. Our senders mark a sentinel with a flag bit in
  the cell x's free high bits, and the receiver treats `0xFD..0xFF` as a sentinel only when that bit
  is set. Stock content uses none of those IDs.
- **The `0x0D` receiver bounds the shooter index** at `+0x21`: a `u16` scaled by 0x118, with no
  bound in stock.
- **The `0x0F` receiver refuses a cell outside the map.** Stock goes on to read through the NULL
  that `0x481550` returns for one (`0x4244CF`).

### Both builds

The always-on fixes are in both builds, `make LIMITS=stock` included, as landing 6's are:

- the build list;
- the download records;
- the out-of-memory message;
- the unit sync's keys;
- skipping a weapon with a bad ID, bounded by that build's own array size;
- the `0x0F` sentinel;
- the receivers' two bounds.

Only the raises differ between the builds.

## The landings

**A′1 — the unit pass's caches. Done 2026-09-24.**

- **The size is the frame's bound**, `TAGPU_PB_FRAMEMAX` = `TAGPU_PK_MAX_UNITS` + `TAGPU_PK_MAX_WRECKS`
  + `TAGPU_PK_MAX_BUILDS` + 1 = 30 721 entries in each cache (24 577 in the stock build), and the
  Vulkan pass's vertex-buffer table holds one per entry of both.
- **A slot is never taken from an entry this frame has asked for.** Every lookup stamps its entries
  with the bake's own frame counter, advanced at each `tagpu_posebake_frame`; the eviction takes only
  an entry with an older stamp. A frame asks for at most the bound, and the table holds the bound,
  so a full table always has an unstamped entry.
- **The keep is where recycling starts**: 512 models and 1024 streams, stock's sizes. Below it a new
  entry takes a free slot; from it on, a new entry takes the slot of the least recently used entry
  this frame has not asked for. So the caches hold the busiest frame of the level, at least the
  keep, until the level change drops them, and a scene larger than the keep does not re-bake every
  frame. The vertex-buffer table recycles the same way but is not dropped at a level change: its
  entries from the last level are the first recycled.
- **The lookups are hash chains**, on the root and ghost flag for a model, the model's slot and the
  owner for a stream, and on the serial for a vertex buffer: a scan would have been O(units ×
  entries) a frame against a table of this size. A model's per-piece topology is its own block of
  `nparts` entries, so an entry costs its model's pieces, not the 256-piece ceiling's.
- **The Vulkan pass's retire grows** instead of holding 64 buffers: a full retire can no longer
  refuse an eviction, and when it cannot grow the table grows instead.
- **Measured**, Vulkan lane, Core Prime Industrial Area at 1280×1024 and zoom 0.25, four skirmish
  owners each with all 278 stock types plus every stock wreck and heap, 1426 objects in one view,
  paused right after the scenario applied:
  - the previous build: the wrecks, the heaps and the last units of the last owner are not drawn,
    nothing is logged, and the caches re-bake 51 929 models and 239 364 streams in 35 s;
  - this build: a frame held 598 models and 1414 streams, all 1426 objects are drawn, and
    nothing is re-baked once the scene is up: 598 model and 1423 stream bakes in the whole run, the
    9 streams past 1414 being ones baked in earlier frames and recycled;
  - a level change in the same process drops all 598 and 1414, streams first, and the next level
    re-bakes from the free slots with the vertex buffers of the first evicted through the retire.
- Review at `medium`: renderer code, no engine patch, no thread-sync change.

**A′2 — unit types, 16 384. Done 2026-09-24.**

- **The sites**: the 17 mask sites and the count check `0x42AA65` in the raised-limits table, and
  the join's pace `0x46DE91` with them (105 sites); the build list, the download records, the
  out-of-memory text and the unit sync's keys as engine fixes in both builds; `mask_has` bounded
  and `WPN_MAXDEFS` following the cap. The engine map has every address, *Unit-type slots* in the
  raised pools and four sections of *Engine defects we patch*.
- **The test content** is `tools/unittypes_fixture.py`: generated kbots on the Peewee's model, one
  small script compiled with tacob, a `CTRL_G` category, and options for a long `[CANBUILD]` list,
  a download file, an AI `Weight` line naming the highest type, padded scripts and raw keys. The
  engine loads a type only if its FBI carries the `Copyright` key (`0x42B0E2`), and each generated
  FBI ends in a tag that makes its unit-sync key unique. Nothing of the game's is committed.
- **Measured**, Core Prime Industrial Area:
  - IDs 512 and 543, stock's break: on the previous build Ctrl-Z returns to a corrupted address and
    an AI `Weight` line naming 543 faults during the load; on this build Ctrl+G and Ctrl-Z select
    every generated unit.
  - The ceiling, 16 383 real types: every mask reader works on a type at ID 16 383, including the
    AI line. The load takes 1.3 ms and 7 092 bytes a type, and two games in one process at
    1280×1024, 1920×1080 and 3840×2160 all load (gpu-status §2.6b has the table).
  - 16 384 real types: the dialog, and the exit on OK, with no `ErrorLog.txt`.
  - 1500 types each carrying a 1 MB script: our out-of-memory text, the engine's dump, the exit.
  - 40 `canbuild` keys and 12 download entries on ARMCOM: the whole list of 71, and 73 records.
    The previous build reads 59 entries over a 30-entry copy, and a 12-entry download file alone
    kills its load.
  - Two peers at 16 383 types with colliding keys: both re-key the same 6 114 types, and the join
    ends in 13 s. The peers fight with types from ID 279 to 16 383, and a Kbot Lab builds ID 16 383
    from its download page; paused, both hold the same slots, types and positions, with no spread.
    A commander's download button for a kbot arms nothing, in both builds; a factory's queues it.
  - Stock content: nothing re-keyed; the stock-limits build compiles.
- **Open**: a first join at the ceiling still takes 12.5 s, the engine's value checksum reading
  every type's files once per process.
- Review at `high`: byte patches, simulation as content, and a network-lobby path.

**A′3 — weapons, 4096. Planned.**

- Test weapons with IDs from 256 up are generated locally.
- **Single player:**
  - A unit firing weapon 4000: the projectile, the damage and the weapon's model drawn.
  - A weapon with no `ID=` and one at 5000: skipped and logged, with nothing written outside the
    array.
- **Two peers:**
  - Fire with IDs from 256 up agrees on both.
  - An interceptor from 256 up stopping a missile from 256 up and one below 256 whose IDs share a
    low byte, aimed at the same point: the right projectile detonates on both peers.
  - Feature hits by weapons 253–255 and from 256 up damage the feature on both peers.
- A stock-content network game shows no change.
- Review at `high`: byte patches, simulation and the wire.

**The order** is section A's landing 7 first, a gap in landed code. Then A′1, before the type raise
can expose the caches, then A′2, then A′3.

## Measured in the landings, decided now

Nothing is left for the owner. A′2 measured memory and load time at the ceiling (the cap stays) and
the lobby join, whose measured cause changed the fix (*The lobby join*, above).

Also recorded: the unit-selection dialog's pictures. The dialog loads one 4 KB picture a type
while it is open, 67 MB at the ceiling.

Settled on 2026-09-24 and not open:

- saved games store types by name;
- COB carries no type or weapon ID;
- the chat path is traced;
- the download records are read.
