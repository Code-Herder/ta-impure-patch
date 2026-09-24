# A′. Content IDs — the plan

## Summary

Two ceilings split out of [section A](raised-limits.md) come into our stack: **unit-type IDs**
(512 → 16 384) and **weapon IDs** (256 → 4096). So does the renderer cache that the first one makes
easy to overflow. That is three landings. The owner decided every choice below on 2026-09-24
**[DECIDED]**. **Nothing is built yet.** The disassembly behind each fact is in
[the evidence pass](limits-evidence.md), §8 and §9. The rules shared by every group are in
[the port overview](overview.md#standing-rules-decided-2026-09-23). Section A's own remaining item,
the composite scratch bound, is [landing 7 of that plan](raised-limits.md#the-landings) and lands
before these three.

Three findings shaped the plan:

- **Stock breaks at 512 real types because it lacks a count check.** The first type with ID 512
  sets a bit past a 64-byte category mask, a heap write out of bounds. Every other per-type table
  in the engine is already sized by its own count.
- **The weapon array sits inside the main game structure.** A weapon at ID 256 lands on the
  projectile pool's header. All 21 references to the array are known, so the array can move.
- **Three network messages carry the weapon ID as one byte.** Two have room to widen in place. The
  interceptor's `0x0E` does not.

## Scope and values

| Limit | Stock | Ours | What it touches | Landing |
|---|---|---|---|---|
| the Vulkan unit pass's caches | 512 models, 1024 (type, owner) streams a frame | sized from a bound the frame cannot exceed | visual: a frame over them refuses the whole unit draw | A′1 |
| unit-type IDs | 512 real types | **16 383 real types** (16 384 counting `None`) | **simulation as content**: category masks, the AI, selection | A′2 |
| a builder's build list | 30 entries, appended unbounded | bounded | a heap overrun at game load | A′2 |
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
- **The build list, fixed beyond TADR.** At game load, each builder's `[CANBUILD]` entries
  (`canbuild1..N`) go into a 0x3C-byte heap block the engine calls `TEMP UTYPE LIST`, which holds
  30 `u16` IDs (`0x42D971`). The append loop (`0x42DA46..0x42DA99`) stops only when a key is
  missing. Any builder with more than 30 entries overruns the block. This is a stock defect, but
  mods with thousands of types are the ones with long lists. The fix is always on. What happens
  past 30 entries is settled once the list's readers have been disassembled: either the block holds
  the list's real length, or the extra entries are refused with a log line.

### The unit pass's caches

**Their own landing, before the type raise**, so the raise never exposes them. `tagpu_posebake.h`
holds 512 models and 1024 (type, owner) streams. They must hold every entry one frame draws: an
entry evicted mid-frame makes the Vulkan pass refuse the whole frame. Stock content can already
reach the stream limit: ten players, each with more than 102 types on screen. Sixteen thousand
types make it likely. The caches are sized from a bound the frame cannot exceed. A frame draws no
more distinct models or streams than it has posed objects, and the frame packet already bounds
those. So the refusal becomes unreachable by construction.

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
  **The carrier is open** until the dispatcher is read: a message type of ours if its size table has
  a free entry, otherwise a tagged chat message, the way TADR carries its `0x0D` extension.

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

## The landings

**A′1 — the unit pass's caches. Planned.**

- The gate is a frame over each stock limit: more than 1024 (type, owner) streams on screen
  (ten players, each with more than 102 types), and more than 512 distinct models (units, wrecks
  and build ghosts together).
- On the previous build, the refusal counter shows the frame refused. On this build it is drawn,
  with no refusal.
- Review at `medium`: renderer code, no engine patch, no thread-sync change.

**A′2 — unit types, 16 384. Planned.**

- **Test content is generated locally and never committed**, like `tools/extra_weapons_fixture.py`.
  A generator writes the FBI text itself, naming stock models by name, plus one COB of ours
  compiled with tacob. Nothing from the game goes into the repository.
- **The ladder:**
  - 512 real types, stock's breaking point: the overrun on the previous build, clean on this one.
  - 16 383 real types, the ceiling: a type with an ID above 512 works in every mask reader —
    Ctrl-letter selection, an AI build line and Ctrl-Z.
  - 16 384 real types: the dialog and the exit, with nothing written.
- A builder with 31 and more build entries.
- A two-peer network game on the synthetic mod, building and fighting with types above 512.
- Memory and load time are measured at the ceiling. Every def loads its own model (`0x42D766`, no
  sharing). The estimate is 100–250 MB at 16 000 types in a 32-bit process, and it is untested.
- Review at `high`: byte patches, simulation as content.

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

## Open questions

- The `0x0E` companion's carrier (above).
- How a saved game stores a unit's type: by name or by ID.
- The unit-sync handshake with 16 383 defs sends one 14-byte subpacket per def per peer. How long a
  lobby takes is unmeasured.
- The build-menu records at `[main+0x391CB]` store `u16` IDs. They were skimmed, not read.
- Whether any COB path carries a weapon ID. It was not checked.
