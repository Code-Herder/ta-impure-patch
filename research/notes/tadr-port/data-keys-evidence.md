# C. New data keys — the evidence pass

A read-only pass made 2026-09-25 over every key of
[the merge exploration's §C](../tadr-merge-exploration.md#c-new-data-keys), to settle what each one
does in TADR, what stock does where it acts, which peer decides it, and whether TADR's
implementation holds by construction. It was made in four parts, one per area, and a census of
Escalation's content (Part 5). The decisions it fed are in [the plan](data-keys.md); each part
records the owner's answer where it raised a question. Sources: `pristine/TotalA.exe.pristine` (main checkout), disassembled with
`i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd`; the retail archives read
through `tools/hpipack.py` for stock-content counts (counts and names only; nothing extracted into
the repo). Every address was re-read in the disassembly; where TADR's number is quoted it was
checked, not copied. Tags: **DIS** = disassembled here, **SRC** = read in TADR's source or ours,
**INF** = inferred, with the measurement that would settle it.

TA's network model ([networking-lobbies](../networking-lobbies.md)) is **state and event
replication, not lockstep**. Every key here is decided on one peer: targeting and firing on the
unit's owner, a hit's damage on the firer's peer, a death explosion on the dying unit's owner. So a
peer that lacks a key plays different rules for its own units without desyncing the others, which
is the case rule 3 exists for.

## What the pass found

| item | kind | where it goes |
|---|---|---|
| the weapon-key store: one byte a weapon, indexed by the validated weapon ID, written at A′3's loader site | mechanism | [C2](data-keys.md#the-landings) · Part 1 §1 |
| the unit-key store: one record a type, validated by def pointer, written at the game-start loader | mechanism | C3 · Part 3 §1 |
| **the lobby sync already covers the keys' content**: a unit's sync value is a checksum of its whole FBI, and a weapon section's is its raw text, unknown keys included; only keys that name *another* file need a fold | engine fact | every landing · Part 1 summary, Part 3 §1 |
| `nottoair` (TADR gates acquisition only, against its doc) | sim, flagged content | C2 · Part 1 §2 |
| `nottounderwater` | sim, flagged content | C2 · Part 1 §3 |
| `surfacefire` (TADR's jumps froze the game once; its code also engages aircraft) | sim, flagged content | C2 · Part 1 §4 |
| `notoverwater` / `notoverland` (TADR's gate skips the per-tick target read and reopens the stale-target class) | sim, flagged content | C2 · Part 1 §5 |
| `nomapweaponalert` (TADR deletes the hit; the doc promises a silence) | display, built as a silence | C2 · Part 2 §1 |
| `VeterancyThresholds` / `VeterancyAccuracyBuffRate` (TADR's cap of 25 makes a unit immortal, even to kill-outright; two lists divide by zero; one capture formula missed) | sim, keyed types | C3 · Part 3 §2 |
| `TransportedExplodeAs` / `TransportedSelfDestructAs` (TADR's passenger list is timing; no fold) | sim, keyed types | C4 · Part 3 §3 |
| `PreviewPieces=`, over **our ghost showing pieces `Create()` hides, on 21 of 126 stock structures** | draw | C1 · Part 4 §2 |
| `PreviewObject3D=` | draw | parked · Part 4 §3 |
| `PreviewFaceOpponent=` (needs a COB getter no stack here has; leaks fogged positions) | draw | waits for rotation (D) · Part 4 §4 |
| `Rotations=` (the switch for building rotation: sim + wire, ~25–30 sites; TADR's fails by construction five ways) | a D feature | group D · Part 4 §1 |
| `reloadbar` (TADR's full value ignores health and veterancy; our marker pass is its home) | display, a D feature | group D · Part 2 §2 |
| weapon `ID=` 0–4095 | done | [A′3](content-ids.md) |
| what Escalation's content uses: every `surfacefire` weapon is a water weapon; every veterancy list has 5 entries; `PreviewFaceOpponent` rests on the recorder's COB getters | census | settles the plan's open questions · Part 5 |
| a hit above 32 767 wraps the HP word; kill-outright spares a veteran above 24 000 HP; a NULL read at `0x4673B1`; meteors per peer | stock defects | B7 · Part 3 §2, Part 2 adjacent |

---

## Part 1. The weapon keys' mechanism and the four targeting flags

A read-only pass made 2026-09-25 over TADR's weapon-key mechanism (`WeaponTdfHook.{h,cpp}`,
`WeaponTags.{h,cpp}`) and the four targeting flags built on it: `nottoair` (`NotToAir.cpp`),
`nottounderwater` and `surfacefire` (`SurfaceFire.cpp`), and `notoverwater` / `notoverland`
(`TerrainFireGate.cpp`), with their install in `ddraw.cpp` and the flying-unit test that
`AreaDamageOverflow.cpp` reuses. Sources: `pristine/TotalA.exe.pristine` (main checkout),
disassembled with `i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd`; the retail
archives, read in memory through `tools/hpipack.py`'s reader for the stock-content counts (counts
and names only; nothing extracted). Every address was re-read in the disassembly. Tags: **DIS** =
disassembled here, **SRC** = read in TADR's source, **INF** = inferred, with the measurement that
would settle it.

### Summary

| # | item | what TADR does | class and mixed-build consequence | TADR's design by construction? | recommendation |
|---|---|---|---|---|---|
| 1 | **The mechanism**: one reader for every weapon key, and a per-weapon tag store | One inline hook in the per-section loader at `0x42E4AB` reads each registered key with `0x4C46C0` (`GetInt(key, 0) & 1`) and **assigns** a tag word, keyed by the weapon's pointer: a 256-entry array for the main block, a hash map for its heap overflow. Hooks for a flag install **lazily**, from inside the TDF load, the first time a loaded weapon carries it. | Load-time, game thread, every peer. The stock unit sync already covers the keys' **content** (the answer below). It cannot tell two builds apart. | **Mostly.** The pointer key is sound for its two arrays. The lazy install patches code from the game thread mid-load, which our rule 2 does not allow, and makes the patched state depend on the content. | **Port the idea, not the store.** A DLL table, one byte per weapon, indexed by `tagpu_limits_weapon_index(w)`: exact and bounded for both builds, since the weapon array is contiguous in both (A′3). It is written at the loader's ID site, which we already own in both builds (`0x42E468`), and cleared where the record's name is wiped. Every site is installed at attach and fails closed. |
| 2 | **`nottoair`**: the weapon cannot acquire a flying unit | Sets **bit 31 of the engine's own mask** `+0x111` and rejects a flying target (`+0x110 & 3 == 2`) at `0x49AD07`, inside the can-engage test's non-water path | **Sim, decided by the owner.** Acquisition, retaliation and orders run on the unit's own peer, and the shots go out as `0x0D`, so a peer lacking the flag plays stock rules for its own units while every peer sees the same shots. That is the rule-3 case, not a divergence of replicated state. | **The bit is sound** (DIS: the loader writes bits 0..30 and nothing reads bit 31). **The claim is not**: it gates acquisition only, so a target that takes off after acquisition is still fired at. It is also never cleared: the loader rewrites each named bit, and the flag is only ever OR-ed in. | **Port as the exact mirror of the engine's own `toairweapon`**, tested wherever stock tests that flag: the can-engage verdict, and the order action's target branch (`0x43F1D4`). Use our table, not bit 31. Whether it also gates the fire is **the owner's call** (Q1). |
| 3 | **`nottounderwater`**: the weapon cannot engage a fully submerged unit (top ≤ sea) | One hook at the water path's range convergence `0x49AC47` | Sim, decided by the owner, as #2 | **Yes for the verdict.** For a non-water weapon it is exactly the rule stock already applies at `0x49ACF7` (DIS). | **Port as a post-filter on the can-engage verdict:** reject when the target's top ≤ sea, for both paths (the identity on the non-water path). |
| 4 | **`surfacefire`**: a `waterweapon` may engage surface units | Five hooks: two water-path rejects (`0x49AC0F`, `0x49AC20`) redirected to the range check `0x49AC47`; can-aim `0x49AB18` → success; the order action `0x43F24F` → allow; the projectile's guidance `0x49B9EB` → the steering path | Sim, decided by the owner for the engage and the order. **The guidance runs on every peer for every projectile**, but only the owner's peer computes damage (DIS `0x49A01D..0x49A047`), so a peer without it draws a torpedo falling where the owner's is steering: a wrong picture, not wrong state. | **No.** (a) The redirects already froze the game once (a `surfacefire`+`nottounderwater` ping-pong that TADR fixed by moving one target). (b) **The code accepts flying targets** unless `nottoair` is set, where the doc promises surface units. (c) The can-aim hook is a no-op for `waterweapon=1` (DIS `0x49AB26`), and for `waterweapon=0` it silently skips the submerged-firer check and the ballistic reach. (d) Its comment calls the order action's shooter a submarine. The flag it tests is `canhover`. | **Port as a verdict filter plus the two genuine sites:** in the filter, a rejected water-path verdict becomes stock's own range test (exact to TADR's bypass); the order action's hover reject (`0x43F24F`); guidance (`0x49B9EB`). **Do not port** the can-aim hook. The flag means nothing without `waterweapon` and is logged at load. Flying targets are excluded unless the owner decides otherwise (Q2). |
| 5 | **`notoverwater` / `notoverland`**: the firer does not aim or fire while over water / over land | One hook at `0x49E1D6` in AutoAim's slot loop, after the reload decrement, that jumps to the next slot | Sim, decided by the owner (AutoAim runs only for local units, DIS `0x48ADCE..0x48ADDA`) | **No: the gate sits before the per-tick target read.** `0x48A1E0` is where a weapon drops a dead target (DIS `0x48A295..0x48A2E0`). Skipping it while the weapon is suppressed keeps a dead target's slot index for as long as the suppression lasts, so B4's two-tick hold never reaches it, and the weapon can resume on the slot's next unit. | **Port with the gate moved after the target read**, to `0x49E1FD` (six position-independent bytes). The target is then dropped as stock drops it, the reload still counts, and nothing aims or fires. The same gate goes into the extra-weapons C loop. |

**The sync-coverage answer: yes for content, no for builds.** A weapon section's stored CRC is
`0x4B6BA0` taken over **the section's raw text**, from its start to the byte before `}` (DIS: the
parser `0x4C3E40` on `}` at `0x4C4165..0x4C4188` and `0x4C4264..0x4C4273`; the setter
`0x4C43F0`). It therefore covers every key in the section, known or not. That CRC is folded
into `CRC_weapons` (`+0x146`) for `weapon1..3`, `explodeas` and `selfdestructas` (`0x42AE29`,
`0x42AEA0`, `0x42AF17`, `0x42AF8D`, `0x42B004`), and by our module for `weapon4..N`. `CRC_weapons`
goes into the unit-sync value at `+0x142` (`0x42A610`). The host compares each key's value across
peers (`0x46DA7A..0x46DAA0`) and marks a disagreement. The effect was measured for the extra
weapons: the type silently leaves the game on both peers (see
[extra-weapons](../extra-weapons.md#multiplayer-who-computes-what-and-the-guard)). So **two peers
whose weapon TDFs differ only in `nottoair=1` lose every unit type armed with that weapon**. The
four lanes make `=1` versus `=0` always detected, because the sum lane moves by one. **Not
covered:** two peers with identical files but different builds (rule 1: same build assumed), and
a weapon that no unit names in a weapon slot, `explodeas` or `selfdestructas`, such as a map
weapon (Part 2's `nomapweaponalert`).

**The three findings that matter most:**

1. **Every targeting decision about a unit goes through one verdict, and every fire through
   one loop.** The can-engage test `0x49ABB0` has 11 callers (DIS): acquisition (`0x40B914` in
   the target finder), retaliation (`0x4070D0`, `0x407125`, `0x40714F`), the attack cursor
   (`0x43E59E`) and the order handlers (`0x402302`, `0x4035D6`, `0x4037D7`, `0x406461`,
   `0x40FE07`, `0x413908`). A weapon's fire function (`+0x60`, set only by `0x49E010`) is called
   at exactly one place, `0x49E43C` in AutoAim. The `0x0D` receiver creates projectiles
   directly. So our flags are **a pure filter on one verdict plus one gate in one loop**, and
   each is the identity for a weapon whose tag byte is 0. No stock weapon carries a key (READ:
   0 of 198), which makes rule 7 hold by construction.
2. **TADR's fire gate reopens the stale-target class.** See #5. Our position keeps the
   per-tick read.
3. **TADR's `surfacefire` is built from jumps between the engine's own branch targets**, and has
   already frozen the game once because of it. A filter on the verdict cannot loop.

---

### 0. The model this part rests on (DIS)

**The weapon loader.** `0x42E310` runs once per level load (its one call is `0x4918BB`). It wipes
each record's name and ID byte (`0x42E31C..0x42E343`), enumerates `Weapons\*.tdf` (`0x503934`,
`0x4BCA30`) and calls the per-section loader **`0x42E440(ctx)`** (`stdcall`, `ret 4`) for every
top-level section (`0x42E3C9..0x42E3ED`). `0x42E440` reads `ID` (`0x42E463`), takes the record
from it (A′3's site `0x42E468`), copies the name, then reads each field. **The mask `+0x111` is
assembled one bit at a time by read-modify-write, never cleared first:** bit 0 `lineofsight`,
1 `ballistic`, 2 `shellweapon`, 3 `beamweapon`, 4 `vlaunch`, 5 `meteor`, 6 `noradar`,
7 `paralyzer`, 8 `dropped`, 9 `startsmoke`, 10 `endsmoke`, 11 `soundtrigger`, 12 `guidance`,
13 `tracks`, 14 `unitsonly`, 15 `groundbounce`, 16 `waterweapon`, 17 `toairweapon`,
18 `smoketrail`, 19 `turret`, 20 `selfprop`, 21 `propeller`, 22 `noexplode`, 23 `burnblow`,
24 `twophase`, 25 `cruise`, 26 `commandfire`, 27 `noautorange`, 28 `stockpile`,
29 `targetable`, 30 `interceptor` (`0x42E5E4..0x42EC05`). **Bit 31 is written by no load and
read by no code:** every access to `+0x111`, and to its only indexed alias `main+0x2E04`
(`0x49D295`), tests a bit from 0 to 30. The loader ends with `0x49E010(weapon)` (`0x42F314`,
`0x42F32E`; its only callers). That call sets the fire function `+0x60` from the mask: turret
`0x49D580`, vlaunch `0x49DB70`, lineofsight or selfprop `0x49D9C0`, dropped `0x49DD60`. A
weapon A′3 skips leaves through `0x42F333`, past that call.

**The TDF readers.** A section node keeps every `key=value` it parsed in a sorted vector of
8-byte `{key, value}` pairs at `[+0x19, +0x1D)`. `0x4C4630(key)` (value), `0x4C46C0(key, default)`
(integer, `atoi` `0x4E4F70`) and `0x4C4760` (float) binary-search it with **`_stricmp`**
(`0x4F8A70`). Keys the engine does not know are kept and are readable, and case does not
matter. The engine reads a boolean as `0x4C46C0(key, 0)` then `and eax,1`. TADR does the same.

**Who decides.** AutoAim `0x49E1A0` runs only for a unit whose player's `+0x73` is 1 or 2, local
human or AI (`0x48ADCE..0x48ADDA`). The detonation skips all damage when the projectile's player
is remote, `+0x73 == 3` (`0x49A01D..0x49A047`, before the area-damage call `0x49A0A9`). So a
remote projectile is a picture. Section B's evidence (Part 1 §0) has the rest: the owner's peer
computes hits and sends `0x0B`; orders run on the owner's peer.

**The can-engage test `0x49ABB0(unit, slot, target)`** (`stdcall`, `ret 0xC`; the weapon is
`[unit + slot·0x1C + 0x10]`):

- **Water path** (weapon bit 16): reject a target that is not a `floater` (def `+0x241` bit 19)
  and whose `y` (`+0x70`) is above sea (`0x49ABF9..0x49AC0F`). Reject a `canhover` target (bit 12)
  whose `y + height/2` (def `+0x170`) is above sea (`0x49AC1B..0x49AC3B`). Then the range test,
  squared distance ≤ `range²` (`+0xDC`) (`0x49AC47..0x49ACA2`).
- **Non-water path:** reject when the shooter's top is at or below sea (`0x49ACAF..0x49ACD2`), or
  the target's top is (`0x49ACE0..0x49ACF9`). A `toairweapon` rejects a target not flying,
  `+0x110 & 3 != 2` (`0x49AD07..0x49AD1A`). Then the ballistic reach (`0x49A890`) and the range.

**The trajectory test `0x49AA80(unit, from, to, slot)`** checks range, then a `waterweapon`
passes (`0x49AB26`), then the shooter's top must be above sea, then the ballistic reach. Its
callers are `0x408035`, `0x43E5C2` and AutoAim's `0x49E3C8`.

**AutoAim's slot loop** (`0x49E1BB..0x49E559`, three slots). A present slot (`state & 2`) first
decrements its reload `+0x14` (`0x49E1C8`). It then reads its target through **`0x48A1E0`**
(`0x49E1E1`), and clears the aiming bit when there is none. It needs a fire function
(`0x49E1F2`), aims (turret: the aim script `0x4B0A70` and the `0x10` send `0x456200`), and at
reload 0 runs `0x49AA80`. An out-of-range target sets `unit+0xBB |= 0x10` and keeps its target.
It then checks the stockpile or the per-shot energy and metal, fires through `[weapon+0x60]`
(`0x49E43C`), and sets the reload. **`0x48A1E0` is where a dead target is dropped:** a unit target
whose slot type `+0xA6` is 0 gets the slot's target cleared (`0x48A2AF`) and the
`TargetCleared` script started (`0x48A2E0`).

**Re-validation** (`0x4089A0`, a slice of a player's units each call): for each slot it tests
allies (the player's `+0x108` table), the slot's `badTargetCategory` (def `+0x231 + slot·4`) and
a paralyzer against target `+0x10E` bit 4 (role INF), and re-finds with `0x40B7B0` if the target
is gone. **It does not ask `0x49ABB0` about the current target.** Once acquired, a target is
kept whatever it does next, as long as it lives and stays an enemy. This is how stock's
`toairweapon` behaves too.

**The order action `0x43F0E0`** is called from the order handlers (`0x401F2F`, `0x402E3D`,
`0x4064F5`, `0x40804C` …). It names the action for **weapon 0**. On a position, a
`toairweapon` refuses (`0x43F17E`). On a unit, a `toairweapon` refuses a target that is not
flying (`0x43F1D4..0x43F1EB`). A submerged target (top < sea) needs weapon 0 or slot 1 to be a
`waterweapon` (`0x43F21B..0x43F23C`). A `canhover` shooter against a surface target is refused
when weapon 0 is a `waterweapon` (`0x43F240..0x43F26A`).

**Stock content (READ, 198 weapons by ID):** 16 `waterweapon`, all `selfprop` (torpedoes, depth
charges, and the seaplane and torpedo-bomber weapons `ARMAIR_TORPEDO`, `ARMSEAP_WEAPON1/2` …);
6 `toairweapon` (the flak and the two AA third weapons); 4 `dropped` (the bombs). **No section
carries any of the seven new keys.**

---

### 1. The mechanism: `WeaponTdfHook` and `WeaponTags`

#### What TADR does (SRC)

`WeaponTdfHook` places one inline hook at **`0x42E4AB`** (`lea ecx,[ebp+0x20]; push ecx; mov
ecx,ebx`, 6 bytes; DIS: `ebx` is the section `ctx` from `0x42E447`, `ebp` the record from
`0x42E489`, and no branch lands inside). It runs every registered handler with `{ctx, weapon}`.
`WeaponTags` binds keys to tag bits and evaluates all of them on every section. It **assigns**
the word rather than OR-ing it, so a tag is always exactly what this load's TDF says. It stores
the word by the record's pointer: `g_baseTags[256]` for the main block, and a
`std::unordered_map` for `WeaponIdOverflow`'s heap slots. `OnFirstUse(tag, fn)` runs `fn`, which
installs the flag's hooks, **from inside the router, the first time a loaded weapon carries the
tag**. `ddraw.cpp` constructs all three modules in every build.

#### Class, and rule 7

Load-time, on the loader thread (the weapon load `0x4918BB` is inside `LoadGameData_Main
0x4917D0`), every peer. It changes nothing by itself. Rule 7 holds by
construction, since stock content carries none of the keys.

#### TADR's safety argument, re-checked

- **The key.** TADR notes that `WeaponTypedef::ID` is not unique under its overflow array, and
  keys by pointer instead. That is sound for its two arrays. In our stack the array is one
  contiguous block in both builds: `main+0x2CF3` for 256, or A′3's DLL static for 4096.
  `tagpu_limits_weapon_index(w)` returns `(w − base) / 0x115` only when the offset is a whole
  record inside the build's count, and −1 otherwise. **The index is exact and bounded by the
  array's own range**, with no hash map and no pointer key.
- **"Slots a later load leaves unpopulated are unreachable anyway".** This is true today (the
  wipe empties the name, and weapons resolve by name), but it is an argument about who looks,
  not a bound. The engine's own fields in such a record are stale too, since the wipe clears only
  `+0` and `+0x10A`. The by-construction version clears the tags where the name is cleared.
- **The lazy install.** It is safe in practice, because the TDF load precedes every combat tick.
  But it patches code from the game thread, mid-load, and the patched state then depends on
  which content was loaded. **Our rules 2 and 3 replace it:** every site is written at attach,
  checked all-or-nothing, and fails closed. The cost it avoided is one table lookup per decision.
- **`nottoair` reads bit 31 straight out of `eax`.** The bit really is free (§0). But the loader
  never clears it, and `NotToAir` only ever sets it. The table is the uniform place, and it keeps
  the engine's mask the engine's.

#### Overlap

- **A′3's loader bound `wpn_loader_id` (`0x42E468`)** runs in both builds for every section. It
  has the validated `id` in hand, and `ebx` is the section's `ctx` there (DIS: set at `0x42E447`,
  untouched until `0x42E4AF`). That is the natural reader: the tag is written for exactly the
  records the engine writes, and a skipped weapon writes neither.
- **A′3's wipe `wpn_wipe` (`0x42E31C`)** exists in the raised build only. The stock build needs
  the same site in order to clear its 256 entries.
- **`tagpu_weapons.c`** already reads FBI keys through `0x4C4630` and folds `weapon4..N` into
  `CRC_weapons`. Its C bodies for slots ≥ 3 (`my_CheckUnitWeapon`, `my_Trajectory3`, `my_AutoAim`)
  are the second implementation every gate must reach (§§2–5).

#### Proposed our-design

- **`u8 s_wkey[TAGPU_LIM_WEAPONS]`**, one bit per flag (5 here; Part 2 adds `nomapweaponalert`
  and `reloadbar`), read only through `wkey_of(w) = (i = tagpu_limits_weapon_index(w)) < 0 ? 0 :
  s_wkey[i]`. **The bound is the index function's:** no value from engine memory reaches the table
  unchecked, and anything that is not a record of this build's array reads 0, which is stock.
- **Written in `wpn_loader_id`** after the ID and name checks, in both builds:
  `s_wkey[id] = Σ (0x4C46C0(ctx, key, 0) & 1) << bit`. It is assigned, so a later weapon with the
  same ID replaces the earlier one's tags exactly as it replaces its record.
- **Cleared at the wipe**, in both builds: the raised build's `wpn_wipe`, and a new stock-build
  site at the same place.
- **An ordering, not a lock.** The writes run on the loader thread, inside the level's load
  (`0x4918BB`, in `LoadGameData_Main 0x4917D0`), and every reader on the game thread after it: the
  load completes before that level's first in-play draw, the ordering every game-thread read of
  the engine's own weapon array rests on. The render thread never reads the table. Part 2's reload bar would carry its bit in the frame packet instead.
- **Load-time diagnostics, per weapon, logged:** `surfacefire` without `waterweapon` (it has no
  effect), and both `notoverwater` and `notoverland` (the weapon can never fire).

#### How to test

Counters: `wkey: loaded N weapons, M tagged (bits …)`. Load the fixture below and check each
tagged weapon's byte through a `tacli` query or the log. Then load stock content: M = 0, and every
gate counter stays 0 through a skirmish.

---

### 2. `nottoair`

#### What TADR does, and what stock does

SRC: at load, `nottoair` sets mask bit 31. At `0x49AD07` (`test eax,0x20000`, the non-water
path's `toairweapon` test; `eax` = mask, `ebx` = target), a set bit 31 with a flying target jumps
to the reject `0x49AD1C`. DIS: that is the exact place and the exact test stock uses for
`toairweapon`, inverted. Stock tests `toairweapon` in **two** decisions: can-engage (`0x49AD07`)
and the order action on a unit (`0x43F1E1`; `0x43F17E` for a position). TADR gates only the
first.

#### Class, and rule 7

**Sim, decided by the owner.** Every caller of `0x49ABB0` runs on the unit's own peer. A peer
without the flag lets its own units acquire aircraft with that weapon. Its shots then reach every
peer as `0x0D` and its hits as `0x0B`, so replicated state does not diverge, but that player
plays different rules. That is the rule-3 case, so the install fails closed. Rule 7: the
identity for untagged weapons.

#### TADR's safety argument, re-checked

- The bit is free (DIS, §0), and never cleared (§1).
- **"Cannot target or fire at flying units" is an overclaim.** The gate is acquisition-only.
  AutoAim fires at whatever target it holds whenever `0x49AA80` passes, and `0x49AA80` has no
  air test. Re-validation (`0x4089A0`) never re-asks `0x49ABB0` (DIS, §0). So a landed aircraft
  acquired by a `nottoair` weapon is still fired at after it takes off. Stock's `toairweapon`
  has the same property the other way round.
- The order action is not gated. A `nottoair` unit (weapon 0) offers the attack cursor on an
  aircraft, and the attack order's own `0x49ABB0` then refuses the target. What the order does
  next (chase, or drop it) is INF, settled by giving the order and watching it.

#### Overlap

`my_CheckUnitWeapon` (slots ≥ 3) re-implements `0x49ABB0` in C, so the filter must run there too.
When the module is armed it owns `0x49ABB0`'s entry (7 bytes, `X_CHECK`), and TADR's inner site
`0x49AD07` would never be reached for slots ≥ 3.

#### Proposed our-design

- **A verdict filter on `0x49ABB0`:** `v' = wkey_engage(w, t, v)`, with `nottoair && (t+0x110 & 3)
  == 2 → 0`. It is the engine's own airborne test, reading only the fields the body it wraps
  has just read.
- **One owner of `0x49ABB0`'s entry, decided once at attach.** With the extra-weapons module
  armed, `my_CheckUnitWeapon` applies the filter to every slot: `o_Check` for slots 0..2 and its
  C body for 3..N. Unarmed, the key module installs its own entry detour on the same 7 bytes:
  `v = o_Check(…)`, then the filter. Both byte-match the same stock bytes, and exactly one is
  written.
- **The order action mirrors `toairweapon`:** a splice at `0x43F1D4` (`mov eax,[edi+0x110]`, 6
  bytes; the only branch that reaches it lands on its first byte, `0x43F17C`). A flying target
  with a `nottoair` weapon 0 goes to the order action's refusal `0x4401DC`. Otherwise it
  re-executes and continues.
- **Q1 (owner):** also refuse to *fire* at a held target that is flying, in the AutoAim gate of §5
  (hold, or clear the target)? A mirror of `toairweapon` says no, and TADR's doc says yes.

#### How to test

Fixture: two Light Laser Towers, one firing an `ARM_LIGHTLASER` clone with `nottoair=1`, and
aircraft passing over both, one peer. Count `0x0D` per tower from the log. Expected: the flagged
tower never acquires a flying unit and still hits ground units. Repeat with the weapon as
`weapon5` of a ten-weapon tower (armed module). Order a flagged unit to attack an aircraft: no
attack cursor, if the mirror is taken. For Q1, a landed seaplane acquired, then taking off.

---

### 3. `nottounderwater`

#### What TADR does, and what stock does

SRC: a 6-byte hook at the water path's range convergence `0x49AC47` (`mov ecx,[eax+0x72]; mov
eax,[eax+0x6A]`). It rejects a target whose `y + height` (`+0x70`, def `+0x170`) is ≤ sea, and
sends it to `0x49AC3B`, not `0x49AC0F`. DIS: the non-water path already rejects exactly that
(`0x49ACE0..0x49ACF9`), so the flag matters only to a `waterweapon`. There it removes subs (and
anything whose top is under water) from what the weapon can engage.

#### Class, rule 7, TADR's argument

The same as §2. The argument holds, but its choice of reject target is what prevented an infinite
loop with `surfacefire` (§4).

#### Proposed our-design

In the same filter: `nottounderwater && top(t) ≤ sea → 0`. On the non-water path it is the
identity, since stock returned 0 there already. **Q3 (owner, small):** mirror it in the order
action's submerged branch (`0x43F21B`: weapon 0 counts as unable), or leave the order path stock
as TADR does. Note that the order action and `0x49ABB0` disagree at top == sea (`jge` against
`jg`, DIS). That is stock, harmless, and recorded.

#### How to test

A torpedo boat firing an `ARM_TORPEDO` clone with `nottounderwater=1`, against a submarine and a
ship. It never acquires the sub, and it still engages the ship.

---

### 4. `surfacefire`

#### What TADR does, and what stock does

SRC, five hooks:
- **`0x49AC0F`** (water-path reject 1: not a floater, above sea) and **`0x49AC20`** (reject 2: a
  hover above sea by half its height) jump to the range check `0x49AC47`, unless `nottoair` is
  set and the target is flying.
- **`0x49AB18`** in `0x49AA80` jumps a `surfacefire` weapon to success `0x49AB9E`.
- **`0x43F24F`** in the order action allows weapon 0 when the shooter is a hover and the target
  is on the surface.
- **`0x49B9EB`** in the projectile engine: in the selfprop flight window (`[p+0x46] > GameTime`),
  stock gives a `waterweapon` projectile above sea gravity and no steering (`0x49B9F2..0x49BA11`,
  DIS). TADR sends it to the steering path `0x49BA16` instead.

#### Class, and rule 7

The engage and the order are sim, decided by the owner, as in §2. **The guidance runs on every
peer**, for the owner's projectile and for every remote copy created from `0x0D`. The owner's
copy decides the hit, and the remote copies are pictures (DIS: `0x49A01D..0x49A047`). A peer
without the flag draws the torpedo falling where the owner's is steering. That breaks the same-
build contract; replicated state is unaffected. Rule 7: the identity for untagged weapons.

#### TADR's safety argument, re-checked

- **By jumps between the engine's own branch targets.** `surfacefire`+`nottounderwater` looped
  `0x49AC47 → 0x49AC0F → 0x49AC47` forever, which is a freeze. TADR fixed that by retargeting one
  reject. A design in which a wrong target means a hang is safe only by care, not by
  construction.
- **Flying targets.** The bypass takes any target the water path rejected for being above sea,
  and that includes aircraft. Only `nottoair` holds them back. The doc says "surface units above
  sea level, such as hovercraft or land units near the shoreline".
- **The can-aim hook.** DIS: `0x49AB18` tests `waterweapon` and goes to success on its own at
  `0x49AB26`. For the documented use (`waterweapon=1`) the hook changes nothing. For
  `surfacefire` without `waterweapon`, it skips the check that the shooter is above water and
  the ballistic reach `0x49A890`, which is undocumented.
- **The order action's comment** calls the shooter "a submarine (`UnitTypeMask_0 & 0x1000`)".
  Bit 12 of def `+0x241` is `canhover` (the FBI bit map in
  [shadows-cloak](../shadows-cloak.md)). The hook does what the code says, and only the comment
  is wrong.

#### Proposed our-design

- **In the filter:** `v == 0 && surfacefire && waterweapon(w) && !flying(t) → v = range_ok(u, t,
  w)`, stock's own test at `0x49AC47..0x49ACA2`. The water path returns 0 only for reject 1,
  reject 2 or range, so this is exactly TADR's bypass, minus flying targets (Q2). `nottoair` and
  `nottounderwater` are applied after it, in one function, so no ordering between them can loop.
- **The order action:** TADR's `0x43F24F` (`test [esi+0x111],edi`, 6 bytes, no branch inside).
  A `surfacefire` weapon 0 goes to the allow path `0x43F27E`, which does not come back.
- **Guidance:** `0x49B9EB` (`test eax,0x10000`, 5 bytes, `esi` the weapon). A `surfacefire`
  weapon goes to `0x49BA16`.
- **Not ported:** the can-aim hook. `surfacefire` on a weapon without `waterweapon` is logged at
  load and has no effect.
- **Q2 (owner):** may a `surfacefire` weapon engage aircraft? Recommended no: surface means not
  airborne, and `nottoair` then has nothing to add.

#### How to test

A ship firing a `CORE_TORPEDO` clone with `surfacefire=1`, against a hovercraft on the water, a
land unit on the shore, a ship and an aircraft over water. Expected: it engages the hover, the
shore unit and the ship, not the aircraft, and the torpedo steers above water. INF: whether a
torpedo running above the water surface actually strikes a land unit. The terrain branch at
`0x49B37F..0x49B3CD` treats water weapons differently. Settle it by watching the fixture hit.
Two peers, for the picture only: the joiner's copy of the owner's torpedo should steer too.

---

### 5. `notoverwater` / `notoverland`

#### What TADR does, and what stock does

SRC: an 8-byte hook at **`0x49E1D6`** (`mov eax,[esp+0x10]; lea ecx,[esp+0x28]`, both
ESP-relative; the one branch that reaches it, `0x49E1CF`, lands on its first byte). It reads the
terrain under the firer with `GetPosHeight` `0x485070`, which bounds the cell to the map and
returns −1 off it (DIS `0x48508F..0x4850C4`, `0x485139`). Then `h ≤ sea` is water. A weapon
carrying the flag for that terrain jumps to the next slot, `0x49E541`. Off the map it does not
gate.

#### Class, and rule 7

**Sim, decided by the owner.** AutoAim runs for local units only (DIS `0x48ADCE..0x48ADDA`). A
suppressed slot sends no `0x10` and no `0x0D`. Rule 7: the identity for untagged weapons.

#### TADR's safety argument, re-checked

The claims hold for where the gate sits: the reload decrement at `0x49E1C8` has run, and no aim
script starts, so the turret holds. No `0x0D` goes out and nothing is spent. **But the gate skips
the per-tick target read `0x48A1E0`** (DIS §0), which is the only place AutoAim drops a dead unit
target. While the weapon is suppressed, a target that dies keeps its slot index in the weapon's
slot. The index is held by slot (`0x48A1E0` reads `+0xA6` of the slot, DIS `0x48A295`). B4's
two-tick hold exists so that exactly this read sees a freed slot empty once, and a suppression
that outlasts the hold defeats it. The weapon then resumes on whatever unit the slot holds next.
That is the stale-target class (section B, Part 1 §1), reopened by where the gate sits.

#### Overlap

`my_AutoAim` runs **every** slot of a unit with more than three weapons in C, including slots
0..2 (`count > 3`). Units with three or fewer take the stock body. So the gate is needed at two
places: the stock splice, and the C loop at the same step, after `E_TargetPos` and the fire
function check.

#### Proposed our-design

- **Gate at `0x49E1FD`** (`mov eax,[ebx+0x111]`, 6 bytes, position-independent, no branch
  inside). That is after the target read (`0x49E1E1`) and the fire-function check (`0x49E1F2`), so
  a dead target is dropped every tick exactly as stock drops it. `wkey_fire_suppressed(w, unit)`
  → `0x49E541`, else re-execute and continue at `0x49E203`. **The invariant it rests on:** the
  per-tick target read runs for every present slot every tick, suppressed or not.
- **Terrain:** `0x485070` on the firer's position, as TADR. Off the map (−1): not suppressed,
  TADR's rule. Both flags together: logged at load (§1).
- **The same predicate in `my_AutoAim`**, at the same step.
- If Q1 is taken, the fire-time `nottoair` joins this gate.

#### How to test

A bomber whose `ARMBOMB` clone carries `notoverwater=1`, ordered to attack a target across a
coastline. It releases over land only, and its reload keeps counting. A torpedo bomber with
`notoverland=1` for the mirror. For the stale-target property, kill a suppressed weapon's target
while its firer is over water (`tacli` scenario): with the gate at `0x49E1FD` the slot's target
is 0 on the next tick. TADR's position keeps it. One peer suffices, since the decision reads the
firer's position, which the firer's own peer owns.

---

### Engine facts this part established (for `exe-reverse-engineering.md`)

- `0x42E440(ctx)`, the per-section weapon loader: the mask bits 0..30 are assigned by
  read-modify-write, one key each (the list in §0), and **bit 31 is written and read by
  nothing**. `0x49E010(weapon)` sets the fire function `+0x60` from the mask; its only callers
  are `0x42F314` and `0x42F32E`; the fire function is called only at `0x49E43C`.
- **A TDF section's stored CRC (`+0x25`) is `0x4B6BA0` over the section's raw text** (`0x4C4165`,
  `0x4C4264`, `0x4C43F0`), so it covers keys the engine does not read.
- `0x4C4630` / `0x4C46C0` / `0x4C4760`: a binary search of the section's sorted `{key, value}`
  vector `[+0x19, +0x1D)` under `_stricmp` `0x4F8A70`. Unknown keys are readable.
- `0x49ABB0` and `0x49AA80` as in §0, with their callers. `0x43F0E0`, the order action, tests
  weapon 0 only.
- AutoAim runs only for local owners (`0x48ADCE..0x48ADDA`). `0x48A1E0` drops a dead target and
  runs `TargetCleared`. Re-validation `0x4089A0` never re-asks `0x49ABB0`.
- **The detonation skips damage for a remote projectile** (`0x49A01D..0x49A047`).
- `0x49B9C2..0x49BA11`: a selfprop `waterweapon` above sea falls under gravity
  (`main+0x14263`) and does not steer.
- The order action (`top ≥ sea` = surface) and `0x49ABB0` (`top > sea` to engage) disagree at
  equality.

### What this part did not establish

- What an attack order does when `0x49ABB0` refuses its target (chase, or drop). Settle it by
  ordering a flagged unit onto an aircraft.
- Whether an above-water torpedo strikes a land unit (the terrain branch `0x49B37F..0x49B3CD`), and
  therefore how useful `surfacefire` is against the shore. Settle it with the §4 fixture.
- The roles of the order handlers `0x4021F0`, `0x4035D0`, `0x406300`, `0x40FBE0` and `0x4138A0`
  (INF: attack-order states). Only their use of `0x49ABB0` matters here.
- The stack offset of the section `ctx` at the `0x49E010` call sites. It was not needed, because
  the reader sits at `0x42E468`.
- The unit sync takes a weapon's CRC from the **first** weapon file with that section name,
  while the record is the last loaded with its ID. For duplicate section names across files the
  two can differ. That is a stock gap, not measured.

### Questions for the owner

1. **`nottoair` at fire time.** Mirror stock's `toairweapon` exactly (acquisition and the order
   action only; TADR's behaviour), or also refuse to fire at a held target that has taken off?
   Recommended: the mirror.
2. **`surfacefire` and aircraft.** TADR's code engages aircraft unless `nottoair` is set, where
   its doc promises surface units. Recommended: never aircraft.
3. **The order cursor.** Mirror each flag where stock tests its counterpart (`nottoair` ↔
   `toairweapon`, `surfacefire` ↔ the hover's `waterweapon` refusal, `nottounderwater` in the
   submerged branch), or only where TADR does (`surfacefire`)? Recommended: the mirrors.
4. **Install.** Always on in both builds, fail closed (rule 3), and TADR's lazy install not
   ported (rule 2)? Recommended: yes. The cost is one bounded lookup per decision.

#### Decided 2026-09-25

The owner's general rule settles 1–3: **where TADR's documentation and its code disagree, the
documentation is the meaning, unless real content relies on the code** (the plan's
[contract](data-keys.md#the-contract)).

1. **`nottoair` holds at fire time too.** The documentation says the weapon "cannot target or fire
   at flying units". A held unit target that is flying is dropped at the fire gate the way
   `0x48A1E0` drops a dead one, and acquisition picks again through the filtered verdict.
2. **`surfacefire` never engages aircraft**, and it acts on water weapons only, as documented:
   every one of Escalation's 11 `surfacefire` weapons is a water weapon (Part 5), so no content
   relies on TADR's non-water behaviour.
3. **The mirrors.** Each flag applies to the order action wherever stock tests its counterpart,
   since the documentation says "cannot target".
4. **Yes.** Every site is written at attach in both builds and fails closed. The lazy install is not
   ported.

---

## Part 2. Alerts and display: `nomapweaponalert` and `reloadbar`

A read-only evidence pass made 2026-09-25 over the two weapon keys of
[the merge exploration's §C](../tadr-merge-exploration.md#c-new-data-keys) that are about what the
player is told rather than what the simulation does. Sources: `pristine/TotalA.exe.pristine`,
disassembled with `i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd`
(`src/DDraw/ZeroDamageMapWeapons.cpp`, `ReloadBars.cpp`, `ProjectileMap.cpp`,
`AreaDamageOverflow.cpp`, `ddraw.cpp`, and the recorder's `GUIEnhancements.pas`, `Colors.pas`,
`UnitInfoExpand.pas`); the retail archives read through `tools/hpipack.py` for counts, names and
flag keys only (nothing extracted into the repo). Every address below was re-read in the
disassembly; where TADR's number is quoted it has been checked, not copied. Tags: **DIS** =
disassembled here, **SRC** = read in TADR's source or in section B's notes, **INF** = inferred,
with the measurement that would settle it.

### Summary

| # | item | what TADR does | class and mixed-build consequence | TADR's design by construction? | recommendation |
|---|---|---|---|---|---|
| 1 | **`nomapweaponalert`** (`ZeroDamageMapWeapons.cpp`, 3509ac3) | For a projectile with **no attacker** whose weapon carries the key and has default damage `+0xD4` = 0, it skips the **whole** area-damage call at `0x49A0A7` and the projectile's minimap dot at `0x467206` (to `0x467406`). | **As TADR built it: sim, for flagged content.** A zero-damage hit is not inert in stock (DIS): it sets the victim's recently-hit counter `+0xFA` that order code reads, sends event `0x10` to the unit's listeners, interrupts an AI owner's builder, stores the hit kind `+0xF5`, sends the `0x0B` to a remote owner, and hits features. Skipping the call removes all of it, so a peer without the key computes different shared state. **As we would build it: display only.** | The lookups are sound (a set keyed on the weapon pointer). **The design is not what its documentation says**: "suppress the alert" is achieved by deleting the hit. Its hook region `0x49A0A7..0x49A0AD` contains the area-damage call that TADR's own `AreaDamageOverflow` and our B1 `dmg_area` both patch (an install-order dependency TADR documents). | **Port as a silence, not a deletion.** Keep the damage call. Gate the one emission site of the "Under Attack" notification (`0x4071D8`, and the same call in our extra-weapons `my_Retaliate`) on the hit record itself: no attacker, a weapon kind, **amount 0**, and the level's meteor weapon carrying the key. The minimap dot is already stock's `noradar=1` (bit 6, one reader): document it, or let the loader imply it for a flagged weapon (owner). |
| 2 | **`reloadbar`** (`ReloadBars.cpp`, bd5119e) | Under the health bar of each locally owned unit, a 34×4 bar for the slowest-reloading tagged weapon of slots 0–2, full = the weapon's `reloadtime` ticks `+0xE4`, drawn at `0x469CB1` with the engine's `DrawBar` inside `__try/__except`. | **Local display.** No engine state is written; the owner's peer is the only one whose slot counters are live for its units. Mixed builds: nothing differs but the picture. | **No, on three counts.** Its full value is `+0xE4`, but the engine stores `+0xE4 × (100…120 % by health) × (100…70 % by veterancy)` (`0x49E468..0x49E4EE`), so a damaged unit's bar sits empty for up to a sixth of the reload and a veteran's starts part-full. The bar overlaps the group digit (`sy+0x0E`). The `__try` is a probe, not a bound. It never sees slots 3..N. | **Port through the frame packet into our marker pass**, in the health bars' loop and under their gates. Take the full value where the engine makes it: record the stored reload at `0x49E4EC..0x49E4F1` (and at our own extra-weapons store), per unit slot and weapon slot; publish `left` and `full` on the game thread; the render thread draws from the packet only. The bar's fill is `(F − left) / F` with `F = max(full, left, 1)`, in [0, 1] by construction. |

**The three findings that matter most:**

1. **Stock's "map weapon" is the meteor shower, and its projectiles are the only ones with no
   attacker.** A map's OTA schema names a `meteor=1` weapon; the shower spawns it from the sim tick
   with CRT `rand()`. The projectile initialiser gives it attacker NULL and owner index 10, and a
   receiver of its `0x0D` does the same. No stock map weapon has zero damage, so the key changes
   nothing on stock content (rule 7 holds trivially).
2. **The alert is one call, and the hit that raises it is fully described by its `0x0B` record.**
   "Under Attack" (notification 2) is emitted at exactly one site, `0x4071D8`, inside the one
   function `0x489CE0` calls with every applied hit, local or received. So a silence can be
   decided from the record — attacker, amount, kind — on every path, without touching the damage
   code B1 wraps.
3. **The engine already computes the exact reload length; TADR approximates it.** The slot
   counter is set once per shot, from reload time, health and kills, and counts down one per tick.
   Recording that store is the exact denominator. Deriving it later from the unit's current health
   drifts whenever the unit is hit mid-reload.

---

### 0. The model this part rests on (DIS)

**Map weapons are meteors.** The OTA schema reader reads `MeteorWeapon` (string `0x504B00`,
through `0x4C48C0` at `0x436747`), `MeteorRadius` (int, `0x4C46C0` at `0x43676A`) and
`MeteorDensity`, `MeteorDuration`, `MeteorInterval` (float, `0x4C4760` at `0x436780`,
`0x436796`, `0x4367AC`). If the name is empty or a float is 0, it falls back to the `[Default]`
section of `gamedata\meteor.tdf` (`0x438320`, called at `0x4367F4`/`0x436809`; a bogus default
prints `"Hey, hoser!  The default meteor shower data was bogus!"`, `0x5050A8`, at `0x438427`).
`0x437D40`/`0x437D50` set the shower's enable word `0x51232C` to 1/0, and `0x437D60` copies the
name to `0x5122F0` and the tick values to `0x512310..0x512338`. At level load (`0x4919BE`),
`0x437CD0` resolves the name with `0x49E5B0` into `0x512328` and **keeps it only if the weapon has
`+0x111` bit 5**, the weapon TDF's `meteor=1` (loader `0x42EB45..0x42EB71`); otherwise it stores
`main+0x2CF3`, weapon record 0 (`0x437CFD`, `0x437D19`; A′3 relocates both into its static).

**The shower.** `0x437DE0`, called from the sim tick at `0x495594`, picks positions with the CRT's
`rand()` `0x4E4870` (the per-thread `holdrand` LCG, `×214013 + 2531011`) and spawns through
`0x49DF10(weapon=[0x512328], start, target, 1)` at `0x438044`. The `+meteor` cheat (`0x438070`)
turns the same shower on. `0x49DF10` takes a pool slot (stock refuses at 300, `0x49DF23`; L1
raises it), initialises it with `0x49C740(proj, weapon, start, 0, GameTime, 0)` and, when
`main+0x2A44` bit 0 is set, broadcasts a `0x0D` (`0x49DFF6`; its `+0x1A..+0x23` are uninitialised
stack in stock, cleared by A′3 in the raised build).

**No attacker ⇔ meteor.** `0x49C740` stores the attacker at `+0x52` and its owner byte at `+0x66`;
with attacker NULL it stores `+0x52 = 0`, `+0x66 = 10` (`0x49C853..0x49C857`). Of its eight call
sites, six pass a unit; the two that pass 0 are the meteor spawn `0x49DF7D` and the `0x0D`
receiver's meteor branch `0x49D307`, which is taken when the message's weapon has bit 5
(`0x49D2A6`). Nothing later writes `+0x52` in a projectile: `0x49C880`, called when a unit dies
(`0x4867B5`), destroys that unit's projectiles with `+0x60 ≠ 0` and leaves the others' `+0x52`
pointing at the dead slot. (The other `+0x52` writers in the image belong to other structures,
`0x4388FA`, `0x43A2A4`…, with vtables.)

**The detonation.** `0x499EB0(proj, unit)` plays the explosion and sound, then damages only when the
projectile's owner record `main+0x1B63 + 331·[proj+0x66]` is empty or not a remote player
(`[+0x73] ≠ 3`, `0x49A01B..0x49A047`). For a meteor that record is index 10, `main+0x2851`, the
331 bytes after `Players[10]` (INF what they hold at run time; see *not established*). Then an area
of effect above 16, or no direct-hit unit, goes to area damage `0x49A120` (`0x49A0A9`); otherwise the
direct hit `0x499CD0(proj, unit, 1.0)` and the shooter's bookkeeping `0x406F50`, a no-op with no
attacker (`0x406F54..0x406F59`).

**A hit on a unit.** `0x499CD0` takes the damage from the weapon's per-type table `+0x64`, else its
default `+0xD4`; scales it; adds the shooter's veterancy (none without a shooter); applies the
`doubleshot`/`halfshot` cheats; and calls `0x489BB0(attacker, victim, amount, kind, heading)` with
kind 1, or 2 for a `paralyzer` weapon (`+0x111` bit 7, `0x499E20..0x499E33`). **There is no
zero test anywhere on that path.** `0x489BB0` builds the 9-byte record
`{0x0B, victim +0xA8, attacker +0xA8 or 0, amount, heading, kind}`, applies it locally
(`0x489C89` → `0x489CE0`) and sends it to the victim's owner when that owner is remote and the kind
is not `0xB` (`0x489C8E..0x489CCD`; section B's model: the attacker's peer computes the hit, the
victim's owner applies it). `0x489CE0` has two callers, that one and the dispatcher's `0x0B` case
`0x455412`. For any kind but `0xA` (heal) and `0xB` it runs, in order:
- `0x467950(victim)`: `+0xFA = 0xF0`, the recently-hit counter. It is decremented per unit tick
  (`0x48ADF0..0x48ADFC`), drives the victim's minimap-dot blink (`0x466EB9`), and is read by order
  code (`0x404BFD`, `0x404D17`, `0x404ECA`, `0x405079`, `0x4149FC`; at `0x404BFD` it is the argument
  of `0x4B6C30`, a random draw). **So it is simulation state.**
- `0x406F80(attacker, victim, amount)`, its only call (`0x489DA2`; no literal reference in the
  image): `0x4897B0(victim, 0x10)`, event `0x10` to each listener on the unit's list `+0xA2`; for an
  AI owner (`+0x73 == 2`) of a type with `+0x245` bit 12, a reaction timer and `0x439EB0`; with an
  attacker, retaliation per slot (`0x48A060`); and last **the notification** below.
- `+0xF5 = kind`, and with an attacker `+0xF0`/`+0xF4`.

**The notification.** `0x47F850(unit, index, text)` (`ret 0xC`) requires the unit **not** in the
on-screen list (`0x48BCB0` over `main+0x1435F`, count `+0x14367`), owned by `main+0x2A43`, alive
(`+0x110` bit 28) and not bit 14, then calls `0x47FAD0` on the queue `[0x51E68C]` (eight 17-byte
entries), which plays the index's sound and shows its text and is rate-limited per index by
`GameTime` (`0x47FAF0`; the next time is written at `0x47FEFD`). The table at `0x5086E8`, 24 bytes an
index, names index 2 `underattack` / `"Under Attack"` (`0x508714`, `0x508718`) with an interval
of 20 (×30 ticks). **Index 2 is passed at exactly one site in the image, `0x4071D8`** (a scan of
all 83 calls of `0x47F780`/`0x47F850`; `0x47F7E0` has no caller; `0x47FAD0` is called only from
the three wrappers). There, at the end of `0x406F80`: `0x438BE0(victim) & 0x80` clear, and the
victim's `+0xF4 ≠ +0xFF` or `+0xF5 == 1` (`0x4071B0..0x4071D1`). With attacker NULL `0x406F80`
jumps straight there (`0x406FFC`).

**The minimap dot.** The radar rebuild `0x466DC0` runs on the game thread every sim tick (`0x464F80`
at `0x49555F`, for the local player at `0x465072`) and walks the projectile pool
(`0x4671A0..0x46742F`). At `0x467206..0x46721D`: a weapon with `+0x111` bit 29 or 30 (`targetable`,
`interceptor`) takes the coloured-marker branch `0x467300`; otherwise **bit 6, the weapon TDF's
`noradar=1`** (loader `0x42EB6A..0x42EB8D`), skips the projectile (`jne 0x467406`); otherwise
`DrawPoint 0x4BEE60` at `0x4672F6` when the point is in the local player's sight or radar or the
projectile is the local player's. A scan of every `+0x111` load followed by a bit test or shift
finds **no other reader of bit 6**. TADR's hook at `0x467206` exits to the same `0x467406`.

**Our minimap is the engine's.** `tagpu_gui_surf.c` (13.6) draws our 252-px base masked by the
engine's fog and takes everything drawn on top — unit dots, radar arcs and `DrawPoint`'s points —
from the engine's composite `+0x142DB`, with no dot replay. Whatever the engine decides to draw
reaches our screen with no renderer change.

---

### 1. `nomapweaponalert`

#### What TADR does, and what stock does

TADR (SRC) registers a weapon-TDF handler that puts the weapon's address in a set when
`nomapweaponalert & 1`, and installs two hooks unconditionally, in every build:
- `0x49A0A7`, 7 bytes (`push ebp; push esi; call 0x49A120`): if the projectile (`esi`) has
  `AttackerUnitPtr == NULL`, its weapon has `Damage (+0xD4) == 0` and is in the set, jump to
  `0x49A0AE`, **skipping area damage altogether**.
- `0x467206`, 8 bytes: the same test on `ecx`, jumping to the next projectile `0x467406`.
It also hides the projectile from TADR's own megamap (`ProjectileMap.cpp`), which we exclude.

The stated purpose (tdraw.txt): "suppresses the under-attack message, alarm sound, and
minimap/megamap projectile alert dots … The projectile itself still appears in the world", for
harmless weather.

Stock (DIS, §0): a harmless meteor hit still runs the whole hit path. **The skip removes more than
the doc says**: the event to the unit's listeners, the AI builder's interruption, the recently-hit
counter and its random draw in the order code, the stored kind, the `0x0B` to a remote owner, the
feature hits `0x4244B0` (and a `firestarter` weapon's ignition with them, INF), and, for an
`interceptor` weapon, the interception tail (`0x49A66F`). It does **not** remove the
direct-hit path: a flagged weapon with area of effect ≤ 16 that lands on a unit goes to
`0x499CD0` at `0x49A062` and still alerts.

**Stock content.** The retail weapons with `meteor=1` are `METEOR` (ID 225), `HAILSTORM` (226) and
`EARTHQUAKE` (227, also `noradar=1`); none carries `targetable` or `interceptor`, and all three
have a nonzero default damage. 31 of the 275 retail OTA files name a `MeteorWeapon` (11 `METEOR`,
9 `HAILSTORM`, 11 `EARTHQUAKE`), and `gamedata\meteor.tdf`'s default names `METEOR`. The only
weapon whose every damage entry is 0 is `NOWEAPON`. So the key, as TADR defines it, matches no
stock weapon.

#### Class, and rule 7

- **TADR's design is sim for flagged content.** Our operational test (a peer without the fix would
  silently compute different shared state): the recently-hit counter feeds a random draw in the
  order code, the listener event and the AI reaction change what units do, and a feature's fate
  changes. So a port of the skip would have to fail closed, like section B's sim fixes.
- **A silence is local.** The notification's queue, its text and sound, and its per-index next
  time are UI state (INF that nothing in the simulation reads `[0x51E68C]` or `0x5086EC`; a
  reference scan of both settles it). A peer without the key alerts where one with it does not, and
  computes the same state.
- **Rule 7** holds either way on stock content, which the key cannot match.

#### TADR's safety argument, re-checked

- The set is keyed on the weapon's address, filled at weapon load before any detonation, read on
  the game thread: sound, if heavier than a bit test.
- The harmless test reads the weapon's **default** damage. A weapon with default 0 and a per-type
  damage for some units is silenced even where it hurts. The hit's own amount is the exact test.
- **The hook site is shared.** `0x49A0A7..0x49A0AD` contains `0x49A0A9`, the area-damage call that
  TADR's `AreaDamageOverflow` rewrites first (its comment at `AreaDamageOverflow.cpp:53` names the
  install-order dependency) and that our B1 `dmg_area` wraps (`tagpu_patches.c` on
  `worktree-tadr_port_b`). A port of the skip would sit inside B1's patch.
- It is installed in every build, so every detonation pays the hook even with no flagged weapon.

#### Overlap with our stack

- **B1** (built, not landed) wraps both calls of `0x49A120`; the design below touches neither.
- **Extra weapons**: `tagpu_weapons.c` detours `0x406F80` at its entry (`"Retaliate"`, 8 bytes)
  and, for a unit with more than three weapons, runs `my_Retaliate`, which issues the same
  notification itself (`E_Reaction(victim, 2, 0)`, `0x47F850`). The gate must cover that call too.
- **A′3**: `0x512328` points into the DLL's weapon static in the raised build; the meteor's `0x0D`
  tail is cleared there. Every reader of ours takes a weapon's index from its pointer, bounded by
  the static.
- **The minimap** needs no work (§0): our screen follows the engine's dot decision.
- **The flag itself** comes from the weapon-key mechanism of Part 1: one bit per weapon ID, bounded
  by A′3's 4096.

#### Proposed our-design, safe by construction

**A silence of notification 2 for a harmless hit from no one, decided from the hit record.**

- **The frame.** At `0x489DA2` replace `call 0x406F80` (5 bytes, `E8 D9 D1 F7 FF`) with a call of a
  `__stdcall` of the same shape that keeps the record (`edi`, the record `0x489CE0` is applying):
  `prev = g_hit; g_hit = silent(rec); call 0x406F80; g_hit = prev`. `0x406F80` has this one caller,
  so `g_hit` is set for its whole dynamic extent; saving and restoring it makes any nesting under
  `0x406F80` (`0x4897B0`'s listeners, `0x48A060`, `0x439EB0`) restore the outer answer by
  construction. `0x489DA7`, the branch target after the call, is untouched.
- **The gate.** At `0x4071D8` replace `call 0x47F850` (5 bytes, `E8 73 86 07 00`) with a call of
  `alert_under_attack(unit, 2, 0)`, which returns without calling when `g_hit` says silent, else
  calls `0x47F850`. `0x4071D3` and `0x4071DD`, the branch targets around it, are untouched. Our
  `my_Retaliate` calls the same function instead of `E_Reaction(victim, 2, 0)`.
- **`silent(rec)`** is true exactly when: the attacker word `rec+3` is 0; the kind `rec+8` is 1 or
  2 (the two weapon kinds `0x499CD0` passes); the amount `rec+5` is 0; and the level's meteor weapon
  `[0x512328]`, validated as a record of the weapon array (offset from the base a multiple of
  `0x115`, index below the count), has the key's bit. Attacker-less weapon-kind records come only
  from meteor projectiles (§0), and a level has one meteor weapon, so the record path and the
  local path answer alike; on a received `0x0B` the record is all a receiver has, and it is enough.
- **The invariant:** the silence changes only whether notification 2 is queued; every hit is
  applied exactly as stock applies it. `g_hit` is written and read only on the game thread: the
  local path runs inside the sim tick, and the dispatcher's `0x0B` case runs there in play (section
  B's B5 measurement: the dispatcher's state table `0x512BC0` drops `0x0A..0x12` during loading, at
  `0x455F50`; re-check it for `0x0B` when this lands).
- **Install:** local, so skip-and-log, not fail-closed. Both sites byte-checked; a mismatch leaves
  stock alerts and logs the reason. A counter `silenced`/`passed` on the `enginefix:` line.
- **The dot:** nothing to hook. Either document that `noradar=1` hides the dot (stock since 1997;
  `EARTHQUAKE` uses it), or have the weapon loader set bit 6 for a weapon carrying
  `nomapweaponalert=1` and `meteor=1`, which is exactly as if the TDF said `noradar=1` (bit 6 has
  one reader, the dot). Owner's choice (Q2).
- **The residual:** a unit-fired `meteor=1` weapon (content only) is turned attacker-less on remote
  peers by the `0x0D` receiver's bit-5 branch; its harmless hits there follow the level's meteor
  weapon's key, not its own. It can only miss or keep an alert.

#### How to test

- Fixture (scratch, built with `tools/hpipack.py`, never committed): a copy of `HAILSTORM` under a
  new name and a spare ID with `damage=0` and `nomapweaponalert=1`, named by a test map's
  `MeteorWeapon`; a second copy without the key. Units parked off screen under the shower.
- Previous build vs new, one peer: the count of queued notification 2 (a peek of the queue, or the
  gate's counter) — stock alerts, the new build is silent with the key and alerts without it. The
  hit is unchanged: `+0xFA` set to `0xF0`, `+0xF5` the kind, and an AI builder under the shower
  interrupted in both builds.
- Two peers (`a2net0`/`a2net1`), because the record may arrive from the other peer: the victim's
  owner stays silent whether the hit was computed locally or received.
- A zero-damage weapon with a per-type damage for one unit type: that type still alerts.

#### Questions for the owner

- **Q1. Silence or inert?** The design above only silences. TADR's skip also makes the hit inert
  (no listener event, no AI builder interruption, no recently-hit scatter, no feature hits); that is
  a sim change for flagged content and would fail closed, inside B1's site. Recommend silence.
- **Q2. The minimap dot:** document stock's `noradar=1`, or imply it at load for a flagged
  `meteor=1` weapon (TADR content then looks as it does under TADR)?
- **Q3. "Harmless" = the hit's amount is 0** (recommended), rather than TADR's default damage 0?
- **Q4. The victim's dot still blinks** for a harmless hit (the counter is sim state and stays). A
  display-only suppression needs a per-unit "last hit was silent" mark and a hook in the radar
  rebuild at `0x466EB9`. Wanted (the visual-residuals rule), or accepted?

#### Decided 2026-09-25

By the documentation rule (the plan's [contract](data-keys.md#the-contract)):

- **Q1: a silence.** The hit is applied exactly as stock applies it.
- **Q2: the key hides the dot itself.** The loader sets bit 6 (`noradar`) for a weapon that carries
  the key, has `meteor=1` and has default damage 0, which is what `noradar=1` in the file would do.
- **Q3: "harmless" is the weapon's default damage of 0**, as documented, not the hit's amount. A
  weapon with default 0 and a per-type damage for some units is silenced where it hurts them too,
  as TADR's documentation says.
- **Q4: the blink is suppressed**, display only, under the "under-attack" the documentation
  promises to silence.

---

### 2. `reloadbar`

#### What TADR does, and what stock does

TADR (SRC): a weapon-TDF handler records the weapon's address and name when `reloadbar & 1`. A hook
at `0x469CB1` (6 bytes, inside `DrawGameScreen`'s bar loop, after the owner compare `0x469CA6` and
before `DrawHealthBars 0x46A430`) draws, when health bars are on (`GameOptionMask & 1`), the unit is
not a nanoframe and its owner is the local human player: among slots 0–2, the tagged weapons that are
not `stockpile` (`+0x111` bit 28) with `+0xE4 ≠ 0`, the one with the largest `+0xE4`; `cur =
+0xE4 − slot+0x14` (0 if the counter exceeds it); a 34×4 rect at `(sx ± 17, sy+0x0A+3 ..
sy+0x0A+7)` in `gui[0]`, filled to `32·cur/max` in `gui[144 − min(6, 100·cur/max/15)]`, through
`DrawBar 0x4BF6F0`, the whole draw inside `__try/__except`.

Stock (DIS):
- **The counter** is the slot's `+0x14` (unit `+0x18`, `+0x34`, `+0x50`). Written at `0x49E099`
  (zeroed by `0x49E070`, which the three unit-create paths call: `0x485F04`, `0x4860AC`,
  `0x4862C4`), `0x49E1D2` (the weapon tick `0x49E1A0` decrements it by one a tick, stopping at 0)
  and `0x49E4EE` (set after a shot). Read at `0x49E3AE` (the fire gate) and `0x46AC6E` (below).
- **The value set after a shot** (`0x49E468..0x49E4EE`): with `m = min(kills +0xB8 / 5, 5)`,
  `a = 0x78 − HP·20/maxHP` (100 at full health, up to 120) and `b = (100 − 6m)·[weapon+0xE4]/100`,
  the counter is `a·b/100`. **Stockpile weapons skip it** (`0x49E447..0x49E463`: they decrement the
  stock and jump to `0x49E4F2`).
- **`+0xE4`** is `reloadtime × 30.0`, truncated, as a u16 (`0x42E546..0x42E561`; the constant at
  `0x4FD250`).
- **The only stock reload display** is indirect: the unit info panel snapshots the three counters
  of weapons with `+0xE4 > 30` (`0x46AC5B..0x46AC83`) into its 60-byte compare block at
  `main+0x37E60` to decide whether to redraw (`0x46ACDB`); what it draws from them is not
  established here. Its stockpile progress is `0x439D20` (section B's HUD divide).
- **Retail content**: 28 of 198 weapons reload in 5 s or more, 12 in 10 s or more, 8 in 30 s or
  more; 8 are `stockpile`. None carries `reloadbar`.

The recorder has an older, separate reload bar (`GUIEnhancements.pas`): an ini threshold
`MinWeaponReload`, a `customreloadbar` FBI key, a veterancy correction, and stockpile progress in its
place. It is not in `tdraw.dll` and not in scope.

#### Class, and rule 7

**Local display.** Nothing in the engine is written by the bar; the store record in the design
below writes only DLL memory, in the same place the engine writes its own value. A peer without it
computes identical shared state. **Rule 7:** stock content carries no key, so no bar appears.

#### TADR's safety argument, re-checked

- **The denominator is inexact.** `+0xE4` is the reload at full health and no kills. A unit near
  death gets up to `1.2 × +0xE4`, so TADR's `cur` is clamped to 0 for up to the first sixth of the
  reload; a veteran of level 5 gets `0.7 × +0xE4`, so its bar starts 30 % full.
- **The `__try/__except`** swallows a fault in the draw: a probe, not a bound (CLAUDE.md).
- **The bounds that do hold:** the slot index is 0..2 by construction; the colour indices are
  inside `main+0xDCB`, which is the full 256-byte `guipal` LUT (`0x4AC7D0`).
- **Placement:** the group digit is drawn at `sy+0x0E` (`0x469CF9`), inside TADR's bar rows.
- **Slots 3..N** of our extra-weapons module are invisible to it.
- The tag test hashes the weapon's name per unit per frame, beside the pointer set.

#### Overlap with our stack

- **The health bars are ours.** On Vulkan the engine's `DrawHealthBars` is skipped (`markown`) and
  `tagpu_mark.c` redraws them from the frame packet, on the render thread, for units whose owner is
  `pk->local_player` (`0x2A43`, the bar loop's byte) when `pk->game_opt & 1` (`damagebars`), with the
  body's interpolated anchor and a device-grid snap. The reload bar belongs in that loop, under the
  same gates, anchor and snap, so it cannot part from the health bar.
- **The packet** carries each live unit as a 100-byte `TAGPU_PK_UNIT` (`tagpu_packet.h`) filled by
  `fill_unit` on the game thread, and the whole `guipal` LUT (`pk->gui_col`). Two u16 fields make
  the reload bar: `reload_left`, `reload_full`; four bytes an entry, 60 KB a packet slot at the
  15 001-slot design point.
- **Extra weapons:** slots 3..N live in the module's rows (`g_side`, `slot_ptr`), and the module
  sets their counter itself with the same formula (`tagpu_weapons.c`, the port of the fire path).
  The publisher asks the module for them when it is armed.
- **The GDI lane** (`renderer=gdi`) is the engine's own drawing: no reload bar there (section B's
  rule for that lane).

#### Proposed our-design, safe by construction

- **The key** through Part 1's weapon-key mechanism: one bit per weapon ID.
- **The full value, recorded where the engine makes it.** At `0x49E4EC..0x49E4F1` (`add edx,eax;
  mov [esi-7],dx`, 6 bytes; the only branch into the neighbourhood lands on `0x49E4F2`, which the
  patch ends before) a jump to a stub that performs both instructions and then stores `dx` in a DLL
  table `full[unit slot][weapon slot]`. The unit slot is `(edi − units_begin) / 0x118`, remainder 0
  and below the engine's slot count; the weapon slot `((esi − 0x1B) − (edi + 4)) / 0x1C`, remainder
  0 and below 3. An index that fails is not recorded and is counted. Our extra-weapons store records
  into the same table for its rows. Game thread only, like the store it follows.
- **The publisher** (game thread, `fill_unit`): for a unit of the local player that is not a
  nanoframe, the tracked slot is the tagged, non-stockpile slot whose weapon has the largest
  `+0xE4` (TADR's choice, and stable before the first shot; ties: the lowest slot); it publishes
  `left` = the slot's counter and `full` = the table's value.
  A weapon pointer is validated as a record of the weapon array before its flags are read.
- **The fill is bounded by construction:** `(F − left) / F` with `F = max(full, left, 1)`, in [0, 1]
  whatever the table holds. A slot's counter is zeroed at every unit create and set only by the
  recorded store, so a recycled slot shows "ready" until its first shot rewrites the table; a
  counter the table never saw (a restored game, INF) shows an empty bar until it reaches 0, never
  an overfull or negative one.
- **The render thread** reads only the packet (the frame packet's rule): the bar is two quads in the
  health bars' bucket, whose vertex capacity grows by them.
- **Install:** local, skip-and-log. Without the store stub the publisher falls back to `+0xE4`
  (TADR's value), logged.

#### How to test

- Fixture (scratch): a unit whose `weapon1` is a copy of a long-reload weapon with `reloadbar=1`,
  fighting a static target.
- Numbers first: the published `left`/`full` against the slot's counter read on the game thread
  (`tacli peek`), at full health, at half health (full = about `1.1 × +0xE4`) and after five kills
  (full = `0.94 × +0xE4`). The bar is empty at the shot and full at the instant the fire gate opens
  (`left` reaches 0).
- Pictures: `import -window` crops of the bar over several frames, at 1× and zoomed, checking it
  stays locked to the health bar and clear of the group digit.
- A unit with two tagged weapons picks the slower one; a stockpile weapon never shows a bar.

#### Questions for the owner

- **Q5. The full value:** record the engine's own store (recommended; one small stub in the fire
  path, writing DLL memory only), or TADR's `reloadtime` alone (a bar that stalls empty on a
  damaged unit and starts part-full on a veteran)?
- **Q6. Placement and look:** TADR's rows collide with the group digit. Below the digit, or a thinner
  bar between the two? TADR's `gui[144..138]` ramp, or one colour?
- **Q7. Which weapons:** the key only (TADR, and rule 7), or also a player setting that bars stock
  long-reload weapons? Stockpile weapons excluded, as TADR does?
- **Q8. Which units:** the health bars' set (the local player's, `damagebars` on). On a peer only the
  owner's counters are live, so a spectator's view of another player's units would show "ready"
  (INF, see below); gate on the owner being a local player as well?

#### Decided 2026-09-25

**`reloadbar` goes to group D with the reload bars it switches on**, not to C: the owner routed
every key that only enables a D feature to that feature. Q5–Q8 stand for D's plan, with this
section as its evidence.

---

### Adjacent findings (not §C items)

- **A NULL read in the minimap for an attacker-less `targetable` or `interceptor` projectile
  (DIS).** The marker branch `0x467300` reads the attacker's owner at `0x4673B1` (`mov
  ecx,[ebx+0x48]`, i.e. `proj+0x52`; `cmp [ecx+0xFF],al`) whenever the point is outside the local
  player's sight. A meteor weapon with `targetable=1` or `interceptor=1` (content only; no retail
  weapon) faults every peer's radar rebuild on its first out-of-sight flight. A stock defect for
  section B's register: bound it by skipping the owner test when `+0x52` is 0.
- **Meteor showers in a network game (INF).** Every peer runs the shower from its own `rand()` and,
  with `main+0x2A44` bit 0, broadcasts each meteor, and whether a peer computes damage for its copy
  depends on the record at `main+0x2851`. Whether a unit takes one hit or one per peer is not
  established: a two-peer measurement on a retail meteor map settles it (group E or B).
- **Stock `noradar=1` is only the minimap dot** (bit 6, one reader). Worth a line in the weapon-key
  reference Part 1 writes.

### Engine facts this part established (for `exe-reverse-engineering.md`)

- The weapon TDF's flag keys and their `+0x111` bits (loader `0x42E5DF..0x42EBE6`, each a
  `0x4C46C0` read then `and 1; shl n`): `noautorange` 27, `soundtrigger` 11, `guidance` 12,
  `tracks` 13, `unitsonly` 14, `groundbounce` 15, `waterweapon` 16, `toairweapon` 17, `smoketrail`
  18, `turret` 19, `selfprop` 20, `propeller` 21, `noexplode` 22, `burnblow` 23, `twophase` 24,
  `cruise` 25, `commandfire` 26, `stockpile` 28, `targetable` 29, `interceptor` 30, `beamweapon` 3,
  `shellweapon` 2, `dropped` 8, `vlaunch` 4, `meteor` 5, `noradar` 6, `paralyzer` 7, `startsmoke`
  9, `endsmoke` 10. `reloadtime` → `+0xE4` u16 ×30 (`0x42E561`).
- Meteors: OTA reader `0x43673D..0x4367AC`, default reader `0x438320` (`gamedata\meteor.tdf`,
  `[Default]`), enable `0x437D40`/`0x437D50` → `0x51232C`, store `0x437D60` (`0x5122F0`,
  `0x512310..0x512338`), resolve `0x437CD0` at level load `0x4919BE` (bit 5 required, else record
  0) → `0x512328`, tick `0x437DE0` from `0x495594`, spawn `0x49DF10` (one caller, `0x438044`),
  cheat `0x438070`.
- `0x49C740(proj, weapon, start, target, time, attacker)`, `ret 0x18`: attacker NULL →
  `+0x52 = 0`, `+0x66 = 10`. Eight callers; the attacker-less two are `0x49DF7D`, `0x49D307`.
- `0x489BB0(attacker, victim, amount, kind, heading)`, `ret 0x14`, the `0x0B` builder, 12 callers;
  weapon hits pass kind 1, or 2 for `paralyzer`; no zero-amount test. `0x489CE0` → `0x467950`
  (`+0xFA = 0xF0`), `0x406F80` (one caller `0x489DA2`), `+0xF5 = kind`.
- The notification path: `0x47F780`/`0x47F7E0`/`0x47F850` → `0x47FAD0` on `[0x51E68C]`; table
  `0x5086E8`, stride 24, index 2 = `underattack`, interval 20 s; `0x47F850` requires the unit off
  screen (`0x48BCB0`, `main+0x1435F`/`+0x14367`). Index 2's one emission site `0x4071D8`.
- `+0xFA`: set `0x467954`, decremented `0x48ADFA`, read by the radar blink `0x466EB9` and by
  `0x404BFD`, `0x404D17`, `0x404ECA`, `0x405079`, `0x4149FC`.
- The radar rebuild's projectile loop `0x4671A0..0x46742F`, the dot `0x4672F6`, the `noradar` skip
  `0x46721A`, the marker branch `0x467300` and its NULL read `0x4673B1`.
- The slot reload counter's writers `0x49E099` (`0x49E070`, from `0x485F04`, `0x4860AC`,
  `0x4862C4`), `0x49E1D2`, `0x49E4EE`; its formula `0x49E468..0x49E4EE`; the stockpile skip
  `0x49E447`; the info panel's snapshot `0x46AC5B..0x46AC83` → `main+0x37E60`.

### What this part did not establish

- **What `main+0x2851` holds in play** (the owner record of index 10), and so which peers compute a
  meteor's damage. Settle: peek it in a single-player and a two-peer game with a meteor map.
- **Whether a meteor hit arrives once or once per peer** at the victim's owner (above).
- **That nothing in the simulation reads the notification queue or its next-time table.** Settle:
  a reference scan of `[0x51E68C]` and `0x5086E0..0x5086FF`.
- **Whether the dispatcher refuses `0x0B` while loading** exactly as section B measured for `0x09`
  and `0x2C`. Settle: the same state-table read, for code `0x0B`.
- **Other writers of the slot counter** reached through addressing the scan does not match (COB,
  a restored game). Settle: a write watch on one slot's `+0x14` through a game save and load.
- **What the info panel draws from its reload snapshot.** Not needed for either design.
- **Whether `0x2A43` can name a player whose units this peer does not own** (a spectator or a
  replay), where the reload counters would not be live.

---

## Part 3. Unit FBI keys: veterancy, transported explosions, and the unit-key store

A read-only pass made 2026-09-25 over TADR's unit-definition keys that change the simulation and
over the mechanism that reads them. Sources: `pristine/TotalA.exe.pristine`, disassembled with
`i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd`: `src/DDraw/UnitDefExtensions.{h,cpp}`,
`VeterancyHack.{h,cpp}`, `TransportedExplosions.cpp`, `tamem.h`, `ddraw.cpp`, `tdraw.txt`; our
`tagpu_weapons.c`; the retail archives read through `tools/hpipack.py` (counts and names only,
nothing extracted). Tags: **DIS** = disassembled here, **SRC** = read in TADR's source, **INF** =
inferred, each with the measurement that would settle it. Every address below was re-read in the
disassembly; where TADR's number is quoted it was checked, not copied.

### Summary

| # | item | what TADR does | class and mixed-build consequence | TADR's design by construction? | recommendation |
|---|---|---|---|---|---|
| 1 | **The unit-key store** (`UnitDefExtensions`) | Hooks the game-start FBI loader at `0x42BF97`, reads every registered key through the engine's own reader (`0x4C46C0` int, `0x4C4760` float, `0x4C48C0` string) with a default, and keeps `vector<vector<>>` side tables keyed by def `+0x21E` | the mechanism, not a rule | **Mostly.** Setters bound the type index below 65 536 and getters size-check. But strings are kept raw and parsed at every use, and nothing checks the hook site's bytes | **Port the shape, not the code:** one always-on reader at the same loader, fixed-size parsed records per def index, validated by def pointer (the `def_rec` rule `tagpu_weapons.c` already uses) |
| 2 | **`VeterancyThresholds`**, **`VeterancyAccuracyBuffRate`** (`VeterancyHack`) | Replaces the engine's `kills/5`, `min(·,5)` and `kills/12` at eight sites with a per-type threshold list and rate. The damage-taken level is capped at 25 and the reload level at 16 | **sim.** Every effect is decided on one peer and the result travels as an event (damage in `0x0B`, a shot in `0x0D`). A mixed build therefore applies different rules to different players; it does not desync state. The default reproduces stock exactly for every sim effect (DIS) | **No.** The 25-level cap makes a unit **immortal**, to weapons and to every engine "kill outright" (self-destruct, player defeat, death in a transport). Two parse paths divide by zero on mod data. One of the two capture formulas is missed | **Port, with bounds of our own** (below). Owner questions on the damage-taken ceiling, the kill-outright calls and the display |
| 3 | **`TransportedExplodeAs`**, **`TransportedSelfDestructAs`** (`TransportedExplosions`) | Swaps the death-explosion weapon at `0x49B017` when the unit is carried at death, or was a passenger of a transport that just died. The second case is found through a pointer list expired by per-tick health heuristics | **sim** on the dying unit's owner's peer, which is the only peer that computes the blast's damage; draw on every other peer. The named weapons are **not** in any sync value, so two peers whose TDF for that weapon differs pass the lobby | **No.** The passenger list is a timing heuristic. The site check fails *open*. There is no CRC fold | **Port, by construction:** carried at death is read in the same destructor call; a passenger of a destroyed transport is the engine's own death **kind 6**, which travels on the wire. Fold both names into `CRC_weapons` |

**The findings that matter most:**

1. **Every veterancy effect in combat is computed on the firer's peer.** `0x499EB0` applies a
   projectile's damage only when the projectile's owner is not remote (`0x49A043`), and both the
   dealt and taken multipliers live inside that path. Reload, accuracy and tracking run in the
   owner's weapon tick. The `0x0D` receiver builds the projectile from the packet's vectors and
   does not re-roll the spread. **Kill counts are replicated by the death event:** every peer's
   destructor increments the attacker named in the `0x0C` record.
2. **TADR's damage-taken cap is a bound that lands on zero.** Stock multiplies incoming damage by
   `(25 − L)·4 %`. At TADR's L = 25 that is 0. The engine's own "kill outright" calls pass 30 000
   through the same multiplier: self-destruct, player defeat, the cargo of a dying transport, and
   kinds 4 and 9. So a unit at 25 levels survives all of them. Even at L = 24 they deal 1 200.
3. **"Died with its transport" is already on the wire.** The destructor kills each passenger with
   damage kind **6**, and nothing else in the image passes 6 (1 of 12 callers of `0x489BB0`). The
   kind is stored at `+0xF5`, sent in `0x0B` and put in the passenger's own `0x0C`. TADR's passenger
   list re-derives a fact the engine already carries.
4. **Sync coverage.** The unit sync's key (`+0x13E`) is `0x4B6BA0` over the **whole FBI file**. So
   two peers whose FBIs differ only in a C key hold different keys, and the type drops out of that
   game on both, silently: stock's own behaviour for any FBI difference. A `units\<name>.OVR` can
   override the key; stock ships none. The **weapons** a transported key names are in no sync value
   unless we fold them.

---

### 0. The model this part rests on (DIS)

**Kills.** A unit's kill count is the **u16 at `+0xB8`**. Every engine reader zero-extends it
(`xor ecx,ecx; mov cx,[…+0xB8]`), so stock treats it as 0…65 535. It is written in four places:

| site | what |
|---|---|
| `0x485C76` | create: zeroed together with `+0xBA` |
| `0x4869CA` | the destructor `0x4866D0`: `inc word [attacker+0xB8]`, the only increment |
| `0x4871C1` | saved-game restore: set from the record |
| `0x487857` | saved-game writer (`0x4876C0`, caller `0x432A01`): read into the record |

The increment runs in the destructor, which runs on **every peer**: in mode 1 from `Send_UnitDeath
0x4864B0` on the owner, and in mode 0 from the `0x0C` case on the others. The destructor takes the
attacker from the record (`rec+7` → victim `+0xF0`, `0x486753..0x486778`) and the killer's player
from `rec+3` through `0x44FE40` (→ `+0xF4`, `0x486787`). It counts the kill only when all three hold:

- the attacker is non-NULL;
- the victim is finished: `+0x104 == 0.0`, the same test that gates the corpse at `0x4865D2`
  [role INFERRED: `+0x104` is the build fraction still owed];
- the victim's owner `+0xFF` differs from `+0xF4`.

`Send_UnitDeath` writes the attacker's index into the record at `0x48663F`. So **the kill count is
replicated by the `0x0C` event**, and every peer computes the same increment from the same record.
[INF: `+0x104` is read from each peer's own copy of the victim. Measure: two peers, a paused
skirmish, compare `+0xB8` of every live unit on both.]

**Every 16-bit access to `+0xB8`** in the image is one of 17 (DIS):

| site | function | reads kills as | TADR hooks it? |
|---|---|---|---|
| `0x489BFA` | `0x489BB0`, damage taken | `min(k/5, 5)` | yes (`takeDamageProc`) |
| `0x499DB5` | `0x499CD0`, damage dealt | `min(k/5, 5)`, the shooter's | yes (`dealDamageProc`) |
| `0x49E46F` | `0x49E1A0`, the weapon tick's reload | `min(k/5, 5)` | yes (`reloadTimeBuffProc`, at `0x49E468`) |
| `0x48A324` | target lead, in the aim path | `k > 5` | yes (`aimBuffProc`) |
| `0x49D6E0` | `0x49D580`, the weapon class's fire method (vtable `+0x60`, stored at `0x49E024`) | `k/12` | yes (`accuracyBuffProc`, at `0x49D6EA`) |
| `0x4043D8` | the capture order's tick | `k/5`, **unbounded** | yes (`captureCostProc`) |
| `0x43869D` | `0x438650`, the capture duration (callers `0x40483D`, `0x414C86`) | `(k+5)/5` | **no** |
| `0x46B2C8`, `0x46B306`, `0x46B338` | the unit panel `0x46AEE0` | "N kill(s)", and "Veteran" at k ≥ 5 | yes (`drawKillsProc`) |
| `0x467CCF`, `0x467CF1` | `0x467CB0`, a second panel | as above | yes (`devDrawKillsProc`) |
| `0x46AC0D` | `0x46A860` (caller `0x469615`) | copied into a local record [role INFERRED: the panel's state; no send found] | no |
| `0x485C76`, `0x4869CA`, `0x4871C1`, `0x487857` | the writers above | — | — |

**Who computes each effect.**

- **Damage dealt and taken.** `0x499EB0` applies a projectile's damage (`0x499CD0` for a direct
  hit, `0x49A120` for area) only after `0x49A043` finds that the projectile's owner player (record
  `+0x66`) is not remote (`+0x73 != 3`). The dealt multiplier reads the shooter (`proj+0x52`). The
  taken multiplier sits inside `0x489BB0`, which that path calls. So both are computed on the
  **firer's peer**, and the taken one reads the victim's kills as the firer's peer holds them: the
  replicated counter above. `0x489BB0` applies locally first (`0x489C89`), then sends `0x0B` when
  the victim's owner is remote (`0x489C99`), with the damage as a **word** (`0x489C71`).
- **Reload, accuracy and tracking.** These run in the owner's weapon tick `0x49E1A0`. The fire
  method is called through `[weapon class+0x60]` at `0x49E43C`.
- **The `0x0D` receiver** `0x49D270` builds its projectile directly (`0x49D307` → `0x49C740`) and
  copies the packet's 12-byte vectors (`0x49D30C..0x49D31C`). It never calls the fire method, so
  the spread is rolled once, on the firer, with that peer's RNG (`0x4B6C30`).
- **Capture.** The capture order runs on the capturer's owner's peer and reads the target's
  replicated kills.

**A mixed build therefore never desyncs through veterancy.** It applies different rules to
different players' units, which is what the same-build contract exists to prevent, so the feature
is **sim** and fails closed (rule 3). This is the same shape section B's Part 2 found for combat.

**Death and transports.**

- **`0x49B000(unit, selfd)`** (`stdcall`, `ret 8`) is the death explosion. It picks
  `def+0x224` (SelfDestructAs) when `selfd` is set, else `def+0x220` (ExplodeAs) (`0x49B017..0x49B021`).
  It builds a projectile record at the unit's position whose owner is `unit+0xFF` and whose
  attacker `+0x52` is 0, and calls `0x499EB0` (`0x49B074`). So **its damage is computed only on the
  dying unit's owner's peer**, and is a picture on every other peer.
- **Its callers:**
  - the destructor at `0x486D50`, when the record's severity `rec+9 > 0` and the unit is finished,
    with `selfd = (rec kind == 3)`;
  - kill-all-for-player `0x486F10` at `0x486F9E`, for **non-local** units, with `selfd = 1`.
- **`unit+0x86` is the transporter** (non-NULL while the unit is carried) and **`+0x8A` the first
  passenger**. The destructor detaches the dying unit from its transporter (`0x4867BA`, then
  `0x48AAC0` at `0x4867CB`) **before** its explosion at `0x486D50`. So by the time `0x49B017`
  runs, `+0x86` is already NULL: "carried at death" must be read before the detach.
- **The cargo loop, `0x4867D0..0x48682A`.** For each passenger, it calls `0x489BB0(attacker, passenger, 30000, kind)`
  with kind **3** if the transport self-destructed (`rec` kind 3) and **6** otherwise
  (`0x4867DA..0x4867EC`), then detaches it. The loop is in the destructor, so it runs on every
  peer.

**Damage kinds (arg 4 of `0x489BB0`)**, from all 12 callers (DIS):

| kind | callers | what |
|---|---|---|
| 1 or 2 | `0x499E37` | a weapon (2 = paralyser) |
| 3 | `0x402147`, `0x486F94`, and the cargo of a self-destructing transport (`0x48680B`) | self-destruct; player defeat |
| 4 | `0x4886A4`, `0x4887D0` | [role INFERRED] |
| 5 | `0x404981`, `0x414B8D` | [role INFERRED: capture] |
| **6** | **`0x48680B` only** | a passenger of a dying transport |
| 9 | `0x402701`, `0x41BC49` | [role INFERRED] |
| 0xA | `0x41BDC7` | heal: skips every multiplier (`0x489BBD`) |
| 0xB | `0x48AF32` | never sent (`0x489C9F`) |

Seven callers pass damage **30 000**: `0x402147`, `0x402701`, `0x41BC49`, `0x48680B`, `0x486F94`,
`0x4886A4` and `0x4887D0`, which are kinds 3, 4, 6 and 9. Kinds 1/2, 5, 0xA and 0xB pass a
computed value. 30 000 is the engine's "kill outright" value; the armour multiplier already
exempts it (`0x489BD1`: `cmp edi,0x7530; jge`), and the veterancy multiplier does **not**.

**How the kind travels.**

- `0x489CE0` refuses a victim that is dead or already pending death (`0x489D45`, `0x489D50`), and
  otherwise stores the kind at **`+0xF5`** (`0x489DAC`).
- It subtracts the damage from the HP **word** (`0x489EB5`). At or below 0 it sets pending death
  (`0x4000`) only when the owner is local (`0x489EE2`), and clamps to 0 otherwise (`0x489EF1`).
- The reaper passes `+0xF5` to `Send_UnitDeath` (`0x48AFC9`), which writes it to the `0x0C`
  record's high nibble (`0x48661C`). The `0x0B` carries it too, at `rec+8` (`0x489C85`).

So on the passenger's owner's peer, a kind-6 hit that kills sets pending death at once. Every later
hit is refused, `+0xF5` stays 6, and the passenger's `0x0C` tells every peer "kind 6". **Pending
death is never cleared in play:** the only explicit clear of bit `0x4000` is the saved-game restore
(`0x487454`), so a pending unit is certain to be reaped. [DIS for explicit clears. INF that no
wholesale rewrite of `+0x110` drops it; the create path masks other bits.]

**The two FBI loaders.**

- **Menu time, `0x42A8D0`.** It computes the sync key `+0x13E` over the whole file
  (`0x42AB78..0x42ABBC`; the engine map has the checksum), and folds weapon1..3 and explodeas
  (`0x42AF23..0x42AF97`) and selfdestructas (`0x42AF99..0x42B019`) into `CRC_weapons +0x146`. Each
  term is the named weapon's TDF-section CRC (`[section+0x25]`), the value `weapon_tdf_crc`
  reproduces.
- **Game start, `0x42BF40`.** The game-start loader `0x42D2E0` numbers each def (`+0x21E` =
  index, `0x42D702`) and then calls `0x42BF40` for it (`0x42D722`). The loader has one other
  caller, `0x42D1F0` at `0x42D269`, for a single def: the console command `Reload`, whose handler
  `0x417490` calls it at `0x4174BE` on the game thread (the engine map's `0x42D2E0` section).
  TADR's hook `0x42BF97` is the instruction after the loader finds `[UNITINFO]`: a clean 5-byte
  `push 0x5119B8`, with `ebp` = def and `[esp+0x14]` = the TDF context. Our extra-weapons loader
  hook is at `0x42CEF2` in the same function.

**Stock content** (the retail archives): 278 FBIs; **no `.OVR` file**; no FBI uses any key of this
part; 6 transports (`ARMATLAS`, `CORVALK`, `ARMTSHIP`, `CORTSHIP`, `ARMTHOVR`, `CORTHOVR`); both
commanders explode as `COMMANDER_BLAST`. `CORKROG` (29 918 HP) is the only unit above 24 000 HP,
and 2 weapons deal 30 000 (the two disintegrators).

---

### 1. The unit-key store (`UnitDefExtensions`)

#### What TADR does, and what stock does

`UnitDefExtensions` (SRC) is a registry of `(key, type, default)`. `LoadUnitDefs`, run at
`0x42BF97`, reads every registered key for the def being loaded. The values go into
`vector<vector<int|double|string>>` indexed by def `+0x21E`. The setters refuse an index of
65 536 or more and grow the vectors, and the getters size-check and fall back to the default.
`VeterancyHack`, `TransportedExplosions`, `buildghost` and the rotation code register keys on it.
Stock reads no such keys. An unknown key in an FBI is simply never asked for, which is what makes
this portable: the engine's own reader answers for any key.

#### Class, and rule 7

It is the mechanism, not a rule; each key's class is its consumer's. The one sim-relevant property
is **when** it reads: at game start, on the loader thread, for every def in play, before any tick
(`0x42D2E0`, called from `LoadGameData_Main 0x4917D0`, numbers and then loads each def); and once
more on the game thread for one type, at the console's `Reload` (`0x42D1F0`, read for C1).

#### TADR's safety argument, re-checked

- **The index is sound but the values are not.** `+0x21E` equals the def's index for the whole
  game, since it is written just before the load. But strings are stored raw and parsed at every
  use. `VeterancyHack::getThresholds` does a `std::map<std::string>` lookup per hit, in the damage
  path, and inserts on first use, mid-game. Parse errors therefore surface in combat (§2).
- **The site is not checked.** `InlineSingleHook` writes without comparing the stolen bytes.
  Rule 3 requires a byte check and a fail-closed install.
- **Nothing records which values a peer read.** A key's text is covered by the sync key (below),
  and that is enough for keys whose *meaning* is local to the FBI.

#### Sync coverage (DIS): would two peers differing only in `VeterancyThresholds` pass?

**No, and neither would stock for any FBI edit.** `+0x13E` is `0x4B6BA0` over the FBI's bytes,
read whole (`0x42AB78` opens the file, `0x42AB8C` sizes it, `0x42ABA8` reads it, `0x42ABB3` sums
it). A key present on one peer and absent on the other changes the file, so the two peers
announce different keys. The type then syncs on neither and is dropped from the game (the flag
`0x800000` at `+0x241` is cleared, `0x46E1FE`), with no message: the "vanished at start" A′2
measured.

Three limits:

- `0x4B6BA0` is four 8-bit lanes, so two differing FBIs *can* collide. An edit that swaps two digits
  keeps lanes 0 and 1 and only moves lanes 2 and 3, so accidental collisions are improbable, not
  impossible.
- A `units\<name>.OVR` with a `Compatability` value replaces the key (`0x42ABCB..0x42AC43`) and
  would hide the difference; stock ships none.
- The value `+0x142` does not cover the FBI at all. It is script, GUIs, download TDF and
  `CRC_weapons`.

So **no fold is needed for keys whose meaning lives in the FBI text** (thresholds, rate). **A fold
is needed for keys that name another file's content** (§3).

#### Overlap

- `tagpu_weapons.c` already reads FBI keys: at the game-start loader, `cb_loader` at `0x42CEF2`
  (weapon4..N), and at menu time, `cb_crc_weapons` at `0x42B004`, which folds them into
  `CRC_weapons` **only while the module is armed**.
- It keys `g_def[]` by def index, and `def_rec()` answers NULL when the stored def pointer differs,
  which makes a stale row read as stock only at a def address that changed: a slot whose FBI the
  loader skips or fails to open keeps an earlier game's row at a repeated address (the engine
  map's `0x42D2E0` section).
- It hooks the def copy `0x42B370` to carry records with their type.
- A′2 made 16 384 type slots (`TAGPU_LIM_TYPES`) and found the def array write-protected after the
  load. Our records live in the DLL, so that does not bite.

#### Proposed our-design

- **One always-on module owns the unit keys** (not the extra-weapons module, which is armed per
  game).
- **Reader: `0x42BF97`,** byte-checked and failing closed. It is a different site from
  `tagpu_weapons`' `0x42CEF2`, so no bytes are shared.
- **Records:** for every def the loader visits, write a fixed-size POD record. Write one **whether
  or not the FBI has any key** (defaults = stock). That alone does not make every in-play row
  fresh: the def array can come back at the same address in the next game, and the loader skips a
  slot with no FBI (`0x42D71A`) or stops before `0x42BF97` when the open fails, so the row must
  also be emptied at each unit-data load and reset at the loader's entry, as C1 builds it
  ([C1, as built](data-keys.md#c1-as-built)). The record:
  `{ const char* def; u8 flags; u8 nthr; u16 thr[32]; u16 accrate; i16 wpn_texp; i16 wpn_tsd; }`.
- **Read through the engine's reader:** `0x4C48C0` for strings, `0x4C46C0` for ints. Parse once,
  here.
- **Lookup: the `def_rec` rule.** The index is `(def − base) / 0x249`, bounded by
  `TAGPU_LIM_TYPES`, and the record is valid only when its `def` equals the def asked about;
  otherwise it answers stock. That is a bound plus a validation, never a timing argument.
- **A flag bit says "this type carries a key".** Every consumer runs the **engine's own original
  instructions** when it is clear, so an unkeyed type executes stock's bytes: rule 7 by
  construction, not by equal arithmetic.
- **Folds for keys that name other content** go in a separate menu-time site, **`0x42B019`**. That
  is the engine's final store of `+0x146`, 6 bytes, `[esp+0x28]` = the UNITINFO context after
  three pushes. It is not `tagpu_weapons`' `0x42B004`, and XOR order does not matter.

#### How to reproduce and test

Pattern: `tools/weaponids_fixture.py`, a generated `.ufo` of clones.

- A clone with each key, one with none, and malformed ones.
- `tacli peek` the record through a debug verb.
- Check the log for every rejected value.
- Game start twice in one process with different mods loaded, to check that no stale row survives:
  a type present only in game 1 must read stock if its slot is reused in game 2.

#### Questions for the owner

None beyond §2–§3.

---

### 2. `VeterancyThresholds` and `VeterancyAccuracyBuffRate` (`VeterancyHack`)

#### What TADR does, and what stock does

Stock (DIS). With **L = min(k/5, 5)** unless noted:

| effect | site | stock formula |
|---|---|---|
| damage taken | `0x489BF3..0x489C34` | `dmg · (25 − L)·4 / 100`, i.e. −4 % a level, floor 80 %; skipped for heal |
| damage dealt | `0x499DAE..0x499DEC` | `dmg · (100 + 6L) / 100`, only when the projectile has a shooter |
| reload | `0x49E468..0x49E4EE` | `reload(+0xE4) · (100 − 6L)/100 · (120 − 20·HP/maxHP)/100` → slot `+0x14` |
| target lead | `0x48A324` | on when `k > 5` |
| accuracy | `0x49D6C2..0x49D711` | spread = `weapon+0x104 + 2048·(1 − HP/maxHP)`, divided by `k/12` when that exceeds 1 |
| capture, tick | `0x4043D5..0x404407` | `· (10 + k/5)/10`, with k/5 **unbounded** |
| capture, duration | `0x438650` | `workertime · (k+5)/5 · …` |
| display | panels | "Veteran" at k ≥ 5 |

TADR (SRC) replaces `k/5` with `L = |{t ∈ thresholds : t ≤ k}|` (an `upper_bound`) at the tick,
taken, dealt, reload and display sites, caps taken at **25** and reload at **16**, and changes the
panel text to "VetN". For capture it uses `getUnboundedVetLevel`: beyond the last threshold it
extrapolates with the last gap, or `k / t₀` for a single threshold. For accuracy it uses
`k / rate`, and rate ≤ 0 disables the buff. The defaults are "5 10 15 20 25" and 12.

#### Class, and rule 7

**Sim**, owner-local in effect: the firer's or the owner's peer decides and the result is
replicated (§0). Fail closed.

**The default reproduces stock exactly for every sim effect (DIS, formula by formula).**

- Taken, dealt and reload with N = 5 give `min(k/5, 5)`.
- Lead `k > t₀` is `k > 5`.
- Accuracy `k/12`.
- Capture: `upper_bound` equals `k/5` for k ≤ 25, and `5 + (k − 25)/5 = k/5` beyond.

Only the display text differs from stock for every unit. **Rule 7 is easier than that:** with a
"carries a key" bit (§1), unkeyed types never enter our code.

#### TADR's safety argument, re-checked

- **The damage-taken cap is a bound that lands on zero.** `(25 − L)` must stay non-negative, and
  TADR stops exactly at 0. At L = 25 no hit does damage. The multiplier also applies to the engine's
  30 000 kill-outright calls (§0: kinds 3, 4, 6 and 9), so a unit at 25 levels
  survives self-destruct, its player's defeat, the death of its transport, and kinds 4 and 9.
  At L = 24 each of those deals 1 200 (4 %).
  - Stock has the same shape at its own ceiling: at L = 5 a kill-outright deals 24 000, and
    `CORKROG` has 29 918 HP. That is a stock quirk [INF: measure a vet-5 Krogoth's self-destruct].
  - TADR turns it into immortality for any unit whose list has 25 entries.
- **The reload cap is a real bound.** `100 − 6L > 0` holds up to L = 16, and 16 is its largest
  value (4 % of the reload, 25× the fire rate). At 17 the product is negative, stored into a u16,
  and the weapon stops firing for ~65 000 ticks.
- **Parse defects (SRC):**
  - `stoul` accepts a negative token and wraps it, and accepts values above 65 535 that no u16
    count reaches.
  - A non-increasing list breaks `upper_bound`'s precondition.
  - **Two paths divide by zero** in the capture tick:
    - "5 10 10": `rate = t[N−1] − t[N−2] = 0` once `k > 10`;
    - "0": `k / t₀`.
    
    Both are reachable from mod data on the first capture of a veteran, so a crash comes from data.
- **Signed kills.** `tamem.h` declares `Kills` as `short`, so from 32 768 kills TADR's levels differ
  from stock's zero-extended ones. That is unreachable in play, but it is not the identity.
- **One capture formula is missed.** `0x438650`, which sets the order's `+0x36` at the start of a
  capture (`0x40483D`, `0x414C86`), still uses stock's `(k+5)/5`. The tick uses TADR's levels, so a
  keyed type's capture mixes the two [roles of `+0x36`/`+0x3A` INFERRED: total and progress].
- **Hot-path allocation.** `getThresholds` inserts into a map on first use mid-game, on the game
  thread (every hooked site runs there), so this is not a race. It is still a parse in the damage
  path.
- **Damage past the HP word.** The dealt multiplier is uncapped, and `0x489CE0` subtracts the damage
  as a **word** and tests the result as signed (`0x489EB5..0x489EC4`). A hit above 32 767 can wrap:
  for 39 000 against a 3 000-HP unit, `3000 − 39000 mod 65536 = 29 536`, and the unit survives.
  - Stock reaches this only at L = 5 with a 30 000 weapon (×1.3 = 39 000).
  - TADR's uncapped levels reach it with far smaller weapons.
  
  [INF: whether the disintegrator's hit goes through `0x499DB5` with its shooter set. Measure: a
  vet-5 commander's D-gun on a 3 000-HP unit, on the previous build.] If it does, **it is a stock
  defect for section B**, and a saturation at the word makes every multiplier here safe by
  construction.

#### Overlap

- **Extra weapons.** `tagpu_weapons.c:892–898` re-implements the reload formula in C, for units with
  more than three weapons (`my_AutoAim` replaces `0x49E1A0` for them). The level must come from the
  same function there as at `0x49E468`.
  - Its fire callbacks are the engine's, so accuracy (`0x49D6EA`) and lead (`0x48A324`) cover every
    slot.
  - Its splices `cb0.b`/`cb0.c` at `0x49D63B`/`0x49D6A3` sit 0x47 bytes before the accuracy site and
    do not overlap it.
- **Section B.** B1's victim caps and B2's stacked air change which units the area walk
  (`0x49A120`) passes to `0x499CD0`, not the multiplier inside it. B4's incarnation drops stale
  `0x0B`/`0x0C` records upstream of the kill increment. No shared bytes.

#### Proposed our-design, safe by construction

`vet_level(rec, kills, effect)`. `kills` is read as u16, like stock. `L` is the count of
thresholds ≤ kills, found by a binary search over at most 32 entries. Then **one bound per effect,
each stated at its site:**

- **Taken:** L ≤ min(N, `CAP_TAKEN`), where `CAP_TAKEN` ≤ 24 keeps `(25 − L) > 0`. In addition,
  for damage ≥ 30 000, use stock's `min(k/5, 5)`. This follows the engine's own sentinel, the one
  the armour path already exempts, so **a kill-outright kills whatever stock kills** (owner question
  1).
- **Dealt:** L ≤ N. Saturate the product at 32 767, the HP word's range, for keyed types. The
  stock-wide wrap is B's question.
- **Reload:** L ≤ min(N, 16), the largest level with `100 − 6L > 0`.
- **Lead:** `kills > thr[0]`.
- **Accuracy:** `kills / rate` with rate 1…65 535. 0 = off. Anything else is malformed.
- **Capture, both sites** (`0x4043D8` and `0x438650`): the unbounded level with the last gap. The
  gap is ≥ 1 by the parse rule below. Cap the level at **13 107**, stock's own maximum
  (65 535 / 5), so no product exceeds what stock already computes.
- **Display:** local, skip-and-log if its bytes differ (owner question 3).

**Parse at load, reject whole.** 1…32 tokens, each a decimal in 1…65 535, strictly increasing. Any
violation keeps the stock table for that type and logs `datakeys: <unit> VeterancyThresholds
rejected: <why>`. The same rule applies to the rate. No partial lists and no token skipping.

**Sites:** `0x489BFA`, `0x499DB5`, `0x49E468`, `0x48A324`, `0x49D6EA`, `0x4043D8`, `0x43869D`,
`0x46B306`, `0x467CCF`, plus the C copy in `tagpu_weapons`. Each is byte-checked. The sim sites
fail closed.

#### How to reproduce and test

The fixture clones one ground unit into:

- (a) no key;
- (b) `VeterancyThresholds=1 2 3 … 30;` with `VeterancyAccuracyBuffRate=1;`;
- (c) malformed variants: `0`, `5 5`, `10 5`, `-3`, `1.5`, 33 tokens.

Kills are set with a peek-write lever or earned in a scenario. For each effect, compare the
previous build (a scratch checkout, `--keep-dll`) with the new one:

- (a) is identical everywhere;
- (b) gives the expected numbers: reload ticks read from slot `+0x14` after a shot, the victim's HP
  delta for dealt and taken, `+0x36`/`+0x3A` for capture;
- (c) logs a rejection and plays stock.

Kill-outright: a (b) unit at 30 levels self-destructs, is carried in a transport that is shot down,
and belongs to a defeated player, and **must die** every time. Two peers (`a2net0`/`a2net1`):
after a paused fight, every unit's `+0xB8` is equal on both.

#### Questions for the owner

1. **The damage-taken ceiling.** Choose among:
   - stock's 5 (−20 %);
   - TADR's 25 (immortal at 25);
   - 24 (4 %);
   - another value.
   
   And do the engine's kill-outright calls (30 000) keep stock's level? I recommend they do, so that
   self-destruct, player defeat and death in a transport kill every unit that stock kills.
2. **The reload ceiling:** keep TADR's 16 (4 % reload)?
3. **The panel text:** "VetN" for every unit, as TADR does and which changes stock's "Veteran", or
   only for types that carry the key?
4. **Capture:** port both formulas (TADR misses `0x438650`) with stock's level ceiling of 13 107?

#### Decided 2026-09-25

1. **25 levels for weapon hits, stock's level for kill-outright.** A weapon hit follows TADR's
   documented −4 % a level up to 25 levels, so a unit at 25 takes no weapon damage. A call of
   30 000 or more (self-destruct, player defeat, a dying transport's cargo, kinds 4 and 9) uses
   stock's `min(kills/5, 5)`, so it kills every unit stock kills.
2. **16**, decided without asking: it is a bound, the largest level with `100 − 6L > 0`.
3. **"VetN" on every unit**, as TADR. Display only.
4. **Both capture formulas, the level capped at 13 107**, decided without asking: it fixes TADR's
   miss and keeps every product inside what stock already computes.

A malformed list or rate is rejected whole and the type plays stock for that key, with the file,
the key and the reason logged ([the plan](data-keys.md#bad-values)).

**For section B** (added as B7): a hit above 32 767 wrapping the HP word, and kill-outright sparing a
veteran above 24 000 HP, are stock defects, measured first.

---

### 3. `TransportedExplodeAs` and `TransportedSelfDestructAs` (`TransportedExplosions`)

#### What TADR does, and what stock does

Stock (DIS, §0): the death explosion is `def+0x220` or `+0x224`, chosen at `0x49B017` by `selfd`,
which the destructor derives from the death kind (3 = self-destruct). The detach at `0x4867CB`
runs before it, so the explosion never knows the unit was carried. A transport's passengers die of
kind 6 (kind 3 if it self-destructed) and explode as their own `ExplodeAs`. That is the "Commander
bomb": a carried commander's `COMMANDER_BLAST` at the crash site.

TADR (SRC) hooks four sites:

- **`0x4867BA`** records the dying unit in one global when `+0x86` is set (carried at death);
- **`0x486D55`** clears that global;
- **`0x4867DA`** appends each passenger of a dying transport to a vector `{ptr, slot, type, HP}`.
  The vector is pruned once a tick: a changed identity, or HP that is positive and differs from the
  captured value, removes the entry;
- **`0x49B017`** replaces the pick with `Transported*As` when the unit is currently carried, is the
  global, or is found in the vector. The name is resolved with `0x49E5B0` at every death, and an
  unresolved name logs and keeps stock.

#### Class, and rule 7

**Sim** on the dying unit's owner's peer: the blast's damage is computed there only (`0x49A043`).
On other peers the choice is a picture. Without the key the pick is untouched, so rule 7 holds by
construction. A mixed build gives different blasts to different players' units and does not
desync. Fail closed.

#### TADR's safety argument, re-checked

- **The passenger vector is timing.** Its entries live until a tick observes the passenger's HP
  change. A passenger that survives the 30 000 (a veteran: §2's multiplier, or `CORKROG` at vet 5)
  with unchanged HP stays listed, and explodes as `Transported*As` whenever it later dies. A slot
  recycled to the same type between two ticks passes the identity check.
- **The global is an ordering inside one call.** Set at `0x4867BA`, consumed at `0x49B017` in the
  same destructor call: sound as long as no destructor nests between the two points. No call there
  reaches `0x4866D0`: `0x489CE0` only sets pending death, and `0x49B000` comes after. But this is a
  property we checked, not one the design guarantees.
- **It fails open.** `ValidateHookSites` disables the feature on a byte mismatch, where rule 3 exits.
- **There is no CRC fold.** The weapon a key names is in no unit's `CRC_weapons` unless some unit
  also uses it as weapon1..3 or explodeas. So two peers whose TDF for that weapon differs sync
  clean.
- **It misses a case.** A passenger already pending death when its transport dies (both killed in
  one tick, the passenger's slot above the transport's) is in the vector, so TADR covers it. A
  kind-3 death (the transport self-destructed) is covered by the vector too.

#### Overlap

- `tagpu_weapons`' `cb_crc_weapons` folds at `0x42B004` when armed. The fold proposed here goes to
  `0x42B019` (§1).
- The destructor is B4's territory (the incarnation check on `0x0C`) and B3's (the bound on its
  indices). Our reads come after both.
- The cargo-detach "Option A/B" that B rejected is unrelated: we read the loop, we do not change it.

#### Proposed our-design, safe by construction

A per-slot byte array `died_carried[unit slots]`, bounded by the unit array's count. It is cleared
at level load, and each entry is **set and consumed inside the lifetime of one death**:

- **Carried at death.** At `0x4867BA`, before the detach: if `+0x86 != 0`, set
  `died_carried[slot]`. This is an ordering within one call, and keyed by slot, so no other unit's
  death can clobber it, whatever nests.
- **A passenger of a dying transport.** At `0x486D42`, where the destructor reads `rec+0xA`: if the
  kind is **6**, set `died_carried[slot]`. The engine sets kind 6 in exactly one place (§0), and the
  record carries it, so every peer agrees with no list and no expiry.
- **A passenger already dying, or of a self-destructing transport** (kind 3). In the cargo loop,
  after each passenger's `0x489BB0`: if the passenger is now pending death, set
  `died_carried[passenger slot]`. On the owner's peer, pending death is certain to be reaped (§0),
  so the entry cannot outlive the unit. On other peers pending is never set, so there is no entry,
  and their picture falls back to the kind (a named residual, draw only).
- **The pick.** At `0x49B017`: `carried = (+0x86 != 0) || died_carried[slot]`. The first test
  covers kill-all's `0x486F9E`, which detonates before any detach. Clear the entry, then pick the
  record's pre-resolved weapon when the key is set and `carried`, else stock's.
- **Names resolved at load** through the engine's `0x49E5B0`. A weapon ID in 1…4095 (A′3's static)
  is kept in the record. An unresolved name keeps stock and logs once, at load.
- **The fold.** At menu time, at `0x42B019`, fold each named weapon's section CRC into
  `CRC_weapons`, with a mix distinct from stock's terms and from `cb_crc_weapons`' slot mix (a
  rotation and constant of its own). Then a key naming the same weapon as `ExplodeAs` cannot cancel
  stock's term. Unkeyed FBIs fold nothing, so stock sync values are unchanged.

#### How to reproduce and test

The fixture: a commander clone with `TransportedExplodeAs=` and `TransportedSelfDestructAs=` naming
a small blast. Cases:

- (a) the clone, carried by `ARMTSHIP` (deck units can be hit), killed by area damage;
- (b) the clone in `ARMATLAS` (`tacli order load`), the transport killed;
- (c) the transport and the clone killed by one blast, both slot orders;
- (d) the transport self-destructs;
- (e) the clone's player is defeated while it is carried.

Measure the HP of a ring of units around the site, and the log line naming the weapon, on the
previous and the new build. Two peers for (b): the damage matches on both, and the pictures are
logged on each. The fold: two peers whose TDF for the named weapon differs. The previous build
syncs clean; the new one drops the type.

#### Questions for the owner

5. The third rule (a passenger already dying when its transport dies, or its transport
   self-destructs) is exact on the owner's peer and a picture elsewhere. Port it, or leave those
   deaths stock?

#### Decided 2026-09-25

5. **Port it, and carry "died carried" on the wire.** The `0x0C` has no spare bit. Its byte
   `rec+0xA` is `kind << 4 | corpse` (DIS `0x486605..0x486621`). The low nibble is the local
   `[esp+0x10]`: 1 for kind 7, the out-value of the `Killed` script query `0x4B0BC0` (`0x4865C3`,
   [role INFERRED: the corpse choice]), and 0 for an unfinished unit or kinds 4, 5 and 9. So the passenger's owner
   sends the flag in a companion message beside the passenger's `0x0C`, the pattern B4 builds for
   `0x09` and `0x0B`, and every peer draws the same blast. This landing (C4) comes after B4.

---

### Engine facts this part established (for `exe-reverse-engineering.md`)

- **Kills:** unit `+0xB8`, u16, zero-extended by every reader. The 17 accesses and their roles are
  in the §0 table.
- **The only kill increment is `0x4869CA`.** It is in the destructor, on every peer. The attacker
  comes from `rec+7`, and the three conditions are an attacker, `+0x104 == 0`, and `+0xFF != +0xF4`.
  Kills are saved at `0x487857` and restored at `0x4871C1`.
- **The veterancy formulas** at `0x489BF3`, `0x499DAE`, `0x49E468`, `0x48A324`, `0x49D6C2`,
  `0x4043D5`, and `0x438650` (whose callers are `0x40483D` and `0x414C86`). The display strings are
  `"kills"` `0x507548`, `"kill"` `0x507540`, `"Veteran"` `0x507538`, and `"%d %s - %s"` `0x50752C`.
- **`0x49A043`** is the gate that computes a projectile's damage only for a non-remote owner.
- **The `0x0D` receiver** builds its projectile from the packet (`0x49D307`) and does not call the
  fire method.
- **The weapon class's fire method** is `[class+0x60]` (`0x49D580` stored at `0x49E024`), called at
  `0x49E43C`.
- **`0x49B000(unit, selfd)`** is the death explosion. Its callers are `0x486D50` and `0x486F9E`, its
  owner is `unit+0xFF`, and it has no attacker.
- **`0x486F10(player)`** kills all of a player's units: 30 000 of kind 3 for local units, a
  detonation plus `Send_UnitDeath(u, 3)` for the others.
- **Transport links:** `+0x86` is the transporter and `+0x8A` the first passenger. The destructor
  detaches at `0x4867CB` and runs its cargo loop at `0x4867D0..0x48682A` (kind 3 or 6, damage
  30 000).
- **`0x489BB0`'s arguments** are `(attacker, victim, damage, kind, byte)`. Its 12 callers and their
  kinds are in the §0 table; heal skips the multipliers, and armour exempts damage of 30 000 or more.
- **`0x489CE0`** stores the kind at `+0xF5` (`0x489DAC`), subtracts from the HP word (`0x489EB5`),
  and sets pending death for local owners only (`0x489EE2`).
- **The kind travels.** The reaper passes `+0xF5` to `Send_UnitDeath` (`0x48AFC9`), which writes the
  `0x0C` kind nibble (`0x48661C`). The `0x0B` kind is at `rec+8`.
- **The only explicit clear of pending death** is the saved-game restore, `0x487454`.
- **The game-start FBI loader** `0x42BF40` is called per def at `0x42D722`, after `+0x21E` = index
  at `0x42D702`, and once more from `0x42D1F0` (`0x42D269`).
- **`CRC_weapons`' explodeas and selfdestructas folds** are at `0x42AF23..0x42AF97` and
  `0x42AF99..0x42B019`.

### What this part did not establish

- Whether kill counts are equal on every peer in practice: `+0x104` is each peer's own copy.
  Measure with two peers and a paused census.
- Whether a hit above 32 767 reaches the HP word (the disintegrator at vet 5). This one goes to
  section B.
- The roles of damage kinds 4, 5 and 9, and of `0x46A860`, and the capture order's
  `+0x36`/`+0x3A`.
- Whether `+0x86` is set on a non-owner peer for a remote passenger, which only affects the
  picture.
- Whether a loaded saved game runs `0x42D2E0`. `tagpu_weapons` relies on it; measure with a
  keyed type across a save and a load.

---

## Part 4. Building placement and the build preview: `Rotations=` and the `Preview*` keys

A read-only pass made 2026-09-25 over the section-C keys that belong to placing a structure and
previewing it. Sources: `pristine/TotalA.exe.pristine` (main checkout), disassembled with
`i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd` — `unitrotate.{h,cpp}`,
`buildghost.{h,cpp}`, `tahook.cpp`, `UnitDefExtensions.cpp`, `tdraw.txt`, and the recorder's
`COB_extensions.pas`; our `tagpu_native.c` (the build ghost) and gpu-status §2.23. Stock-content
counts come from the retail archives read through `tools/hpipack.py` (counts and names only), with
the piece hides taken by `tools/ta3do`'s `hidden_at_create` rule. Tags: **DIS** = disassembled
here, **SRC** = read in TADR's source or ours, **INF** = inferred, with the measurement that would
settle it. Every address below was re-read in the disassembly.

### Summary

| # | item | what TADR does | class and mixed-build consequence | TADR's design by construction? | recommendation |
|---|---|---|---|---|---|
| 1 | **`Rotations=`** (a string of `S`/`E`/`N`/`W`, default `S`) | The per-type opt-in for TADR's **building rotation**: the `/` key cycles the cursor's facing, and the order, the create, the footprint, the yardmap and the `0x09` heading follow it. The key does nothing without that feature (22 inline hooks in `unitrotate.cpp`, more in `tahook.cpp`, `MegamapTAStuff.cpp`, `dialog.cpp`, `ChallengeResponse.cpp`; 16 commits, four of them fixes to itself). | **sim + wire.** The footprint, the grid-origin cell and the occupancy stamp decide placement and pathing on every peer; the facing travels in `0x09`. A peer without it builds its copy south-facing and silently stamps different cells — fail closed by section B's test. | **No.** It rotates by **writing the shared, write-protected unit def** for the duration of engine calls, closed by return-address thunks with 8-deep stacks that skip silently on overflow; it **infers** a live unit's rotation by rounding its heading, which the engine jitters by up to ±90°; it keys orders by pointer with no lifetime; **remote peers bake the rotated building unrotated**, because the network create is not wrapped; and its rotated-yardmap cache outlives the yardmaps, which the engine frees at every level teardown. | **Do not port the key into C.** It is group D's building rotation, and that is a sim feature with a wire change, about 25–30 sites. Plan it as its own D landing, ours by design (below), or not at all. The owner decides. |
| 2 | **`PreviewPieces=`**, **`PreviewPiecesS/E/N/W=`** | A per-type whitelist of piece names for the ghost. Without it, TADR hides pieces by a **name heuristic** (`flare`, `flash`, `muzzle`, `fire`, `flame`, `wake`). | **draw, local.** Nothing on the wire; nothing any peer computes. Skip and log. | Mostly. The name list is parsed into a set and matched with a 63-char cap. The heuristic is a guess, and it misses 3 of the 126 stock structures. | **Port as an override on top of a by-construction rule our ghost lacks today: the pieces the unit's own `Create()` hides.** 21 of 126 stock structures' ghosts currently show muzzle flares or hidden parts. The per-facing variants go with rotation (D). |
| 3 | **`PreviewObject3D=`** | Loads `objects3d/<name>.3DO` through the engine's 3DO loader, from the ghost's draw path, on first use, and draws the ghost from it. | **draw, local.** | Partly. It is safe on TADR's single thread; in our stack the ghost draws on the render thread, which must not call engine loaders. It resolves a file path from FBI text with no check of the name. | **Park unless a mod the owner plays uses it.** If ported: the game thread loads it at level load and frees it at teardown, the name is bounded and has no path separators, and the render thread gets a template pointer under `model_root`'s contract. |
| 4 | **`PreviewFaceOpponent=`** | Overrides the preview's facing to the cardinal toward the nearest enemy commander ("each enemy's first unit slot"), when the cursor is within a selected builder's `builddistance`. | **draw, local**, but it reads **enemy positions through fog**. | **No.** The builder-radius gate limits where you can scan *from*, not what you learn: the answer depends on the commander's true position anywhere on the map. "First slot = commander" is a heuristic: the slot is reused after the commander dies. | **Do not port.** Its premise is a `Create()` script that turns toward the nearest enemy commander, and that needs the recorder's COB getters: Escalation's scripts scan every unit id (`MIN_ID`..`MAX_ID`, `UNIT_ALLIED`, then stock `UNIT_XZ`; Part 5). Stock COB has none of them and our stack does not implement them. Revisit only with the COB extensions ([merge exploration §F](../tadr-merge-exploration.md#f-the-cob-getters)). |
| 5 | TADR's ephemeral-piece filter vs **our ghost's "every piece visible"** | (the heuristic of item 2) | draw, local | n/a | Folded into item 2: read `Create()`'s hides at level load, per type, on the game thread. |

**The findings that matter most:**

1. **`Rotations=` is not a data key; it is the switch for a D feature that changes the simulation
   and the wire.** Porting the key alone does nothing, and TADR's implementation fails "safe by
   construction" in five independent ways (§1c). The one that matters for multiplayer is DIS:
   `CreateFromNetwork 0x4861D0` bakes the new unit's footprint and stamps its occupancy
   (`0x4862B8`, `0x48630C`) before TADR's return thunk copies the heading. So every remote copy
   of a rotated E/W building carries the unrotated footprint. When the yardmap readers see the
   rotated heading later, they read a quarter-turned yardmap against unrotated dimensions.
2. **Our own build ghost shows pieces the building's script hides.** `ghost_pieces` marks every
   template node visible (`tagpu_native.c`, "EVERY PIECE IS MARKED VISIBLE"), and **21 of the 126
   stock structures** hide a piece that has faces at `Create()`. Muzzle flares on ARMLLT, ARMHLT,
   ARMGUARD, CORHLT, CORFLAK and 11 more; ARMAP's radar dish; ARMVULC's shell; CORMOHO's
   `dingle1`/`dingle2`/`rotary`. That is a visual residual in a shipped play default. The fix
   by construction is the rule `tools/ta3do` already applies: walk `Create()`'s prologue and
   collect its `HIDE`s. `PreviewPieces=` then becomes a mod override on top of it, not the only
   remedy.
3. **The heading is not an identity.** The spawn writes
   `unit+0x66 = 0x8000 − BuildAngle/2 + rand(BuildAngle)` (`0x485BD6..0x485C06`, DIS), and 5
   stock structures have `BuildAngle > 0x4000` (ARMLLT, CORLLT, CORSOLAR at 32768; ARMVULC,
   CORBUZZ at 29096). For those, a south build lands more than 45° off south about half the time.
   TADR's `RotationFromHeading` would read it as E or W if the type allowed it. Any rotation
   design must carry the facing explicitly, per unit, with a lifetime.

---

### 0. The model this part rests on (DIS)

**The spawn `0x485A40(unit, pos by value, flag)`** is shared by the owner's create and the
network create. It sets the def pointer and the alive bit, then:

- **bakes the footprint into the unit:** `0x485AA4 mov ecx,[def+0x14A]` / `0x485AAA mov [unit+0x7E],ecx`.
  That is one dword, so `footX` goes to `+0x7E` and `footZ` to `+0x80`.
- **derives the grid-origin cell from it:** `0x485B99..0x485BD3`,
  `x = (pos.x − footX·2^19 + 0x80000) >> 20`, likewise z, stored at `+0x76` / `+0x78`.
- **writes the rotation words:** bank `+0x64 = 0` (`0x485C02`) and
  `heading +0x66 = rand(BuildAngle) + 0x8000 − BuildAngle/2` (`0x485BD6..0x485C06`; `BuildAngle`
  is `def+0x210`, `rand` is `0x4B6C30`).

**The owner's create `0x485F50`** runs `0x485A40` (`0x4860A0`), `0x485D40`, `0x49E070`, `0x437840`,
the mobile-only movement object and `heading = BuildAngle` (`0x4860E6`/`0x4860ED`), `0x48A870`,
the occupancy stamp `0x47CC30` (`0x48610F`), and then broadcasts `0x09` through `0x456050`
(`0x486115`). None of the five callees between the spawn and the send stores to `+0x64` or `+0x66`
in its own body (read to each one's first `ret`; their callees were not walked).

**`0x09` carries the orientation, and no receiver reads it.** `0x456050` builds 0x17 bytes:
`{0x09, type +0xA6 @1, index +0xA8 @3, position +0x6A (12) @5, bank/heading/pitch +0x64 (6) @0x11}`.
`CreateFromNetwork 0x4861D0` reads `rec+1`, `rec+3` and `rec+5..+0x10` and nothing past them. It
calls the spawn itself (`0x4862B8`). For a mobile def (`+0x22F == 1`) it sets
`heading = def+0x210` (`0x4862FA`/`0x486301`). For a structure it keeps its **own** random jitter.
Then it stamps (`0x48630C`). So stock peers already disagree on a structure's exact heading, which
is cosmetic because every stock structure faces south ± its `BuildAngle`. The full-state writer
`0x48B200` (the `0x2C` round robin) sends type, `+0x108`, the build fraction, `+0x10E`,
`+0x110 & 3`, the transporter index and `+0xF9`. It sends no heading.

**The yardmap is read from the def, and its dimensions from the unit.** The yardmap is allocated
by the unit loader as `footZ · footX` bytes, for `bmcode == 0` only
(`0x42CF5E..0x42CF7A`, into `def+0x14E`; `0x42CF32` zeroes it first). The runtime readers
`Unit_UpdateYardmap 0x47C790`, `RebuildFootPrint 0x47CC30`, `ClearOccupancy 0x47D0E0` and
`CanCloseOrOpenYard 0x47D970` (called by `YardOpen 0x47DAC0`) read the unit's `+0x7E`/`+0x80` and
`+0x76`/`+0x78`, and the **def's** yardmap pointer (`0x47C7FE`, `0x47CD82`, `0x47D166`, `0x47DA47`).
The **placement testers take a def, not a unit**, and read its footprint and yardmap: `0x47D2E0`
(`0x47D2FA`, `0x47D4ED`), `0x47DB70` (`0x47DB7F`) and `0x47D820` (`0x47D82D`, `0x47D8C2`), which
`TestBuildSpot 0x4197D0` calls at `0x4198FE`. The other `+0x14E` hits in the image are not
unit-def reads: `0x4A26xx` walks a 0x15B-stride GUI gadget array.

**A unit type's COB lives for a level.** The level's unit-data load `0x42D2E0` (called from
`0x4918CA`) builds `MODEL_PTRS`, and in the same per-type loop builds `scripts\<name>.COB`
(`0x42D8E5`, strings `0x503740`/`0x503EC8`), loads it through `0x4B2450`, and stores it at
`def+0x18E` (`0x42D8F4`). The level teardown's `0x42DB90` (called at `0x491C21`) frees it (`0x4B2540`, `0x42DC3C`) and
zeroes it (`0x42DC41`), just after it frees the type's yardmap (`0x42DC18..0x42DC2B`).
The spawn's COB setup `0x485D40` reads it (`0x485D64`, `0x485DA5`, `0x485DB8`). The in-memory header
is TADR's `CobHeader`: the file's counts, then its offsets turned into pointers in place, the two
name tables entry by entry, the entry-point table's entries left as code offsets in dwords (DIS
`0x4B24A7..0x4B2527`, read for C1; [the engine map](../exe-reverse-engineering.html), *The loaded
script*).

### 1. `Rotations=` and the building rotation it switches on

#### 1a. What TADR does, and what stock does

Stock has no player-chosen facing: every structure is created at `0x8000 ± BuildAngle/2`, and
neither the order nor the wire carries a choice. TADR adds one (SRC, `unitrotate.cpp`):

- **The choice:** `/` (or modifier+wheel, or a click on a build-menu button's edge chevron) cycles
  `m_rotation` over the facings the build type's `Rotations=` allows (`IsRotationAllowed`, `S`
  always allowed).
- **The order:** while `GUI_IssueMobileBuildOrder 0x419670` runs (a return thunk scopes it), every
  order allocated at `0x43ADCB` (after `cmalloc(0x56)` in `ORDERS_NewSubOrder2Unit`) is tagged with
  the rotation in `std::unordered_map<void*, int>`, keyed by the order's address.
- **The placement preview:** `TestBuildSpot 0x4197D0`'s entry swaps the build def's footprint
  words and yardmap pointer every frame the cursor is up. `DrawBuildSpotQueue 0x438C00` swaps the
  def's six `lFpBnd` dwords for one queued order's rect.
- **The owner's create:** `Order_MobileBuild 0x403A20` / `Order_VTOL_MobileBuild 0x413D80` swap
  the def for the whole handler (the target snap, the area-clear test and `ApplyYardmap` all read
  it before the create). `0x403D5B`/`0x41409B` arm `m_pendingHeading`, and the `0x456050` entry
  adds `rotation·0x4000` to the new unit's heading just before `0x09` goes out.
- **The re-creation paths:** give `0x488570` (heading from the `0x14` message's `+0x11`, SRC),
  resurrect `0x404DB0` (heading from the wreck record's `+0x22`), and load `0x48718E` (heading at
  `[esp+0x71]`, the save record's `+0x39`). Each arms `g_pendingCreateRotation`, and the
  `0x485F50` entry swaps the def around the create.
- **The receiver:** the `0x4861D0` entry stashes the record and hooks the return, which copies
  `rec+0x13` into the new structure's heading.
- **The runtime readers:** `0x47C790`, `0x47CC30`, `0x47D0E0`, `0x47D970` and `0x47DAC0` round
  the unit's live heading to a quarter turn, gate on `bmcode == 0` and `IsRotationAllowed`, and
  swap the def's yardmap pointer to a rotated copy for one call.
- **Teardown:** `0x491B60`'s entry restores any swap, so the engine never frees TADR's `new[]`
  copy.

#### 1b. The key cannot be ported without the feature

`Rotations=` only answers "may this type face E/N/W". With no facing to choose, no order that
carries one and no create that honours one, the value is never read. Of TADR's readers
(`IsRotationAllowed`, `IsRotatableStructure`), every caller is inside the rotation feature or its
UI (SRC). No stock FBI carries the key, so reading it and storing a 4-bit mask per type is the
identity on stock content. That part is trivial, and it is not the work.

#### 1c. TADR's safety argument, re-checked

1. **A shared, sealed def is written for the duration of engine calls.** `SwapUnitInfoFootprintWords`,
   `SwapUnitInfoFpBndXZ` and `ApplyYardmapRotationTo` write `def+0x14A/0x14C`, the six `lFpBnd`
   dwords and `def+0x14E` through `WriteProcessMemory`, falling back to `VirtualProtect`. The engine
   seals that array after the load (engine map, `0x42B328`). Correctness then needs **every**
   reader inside the window to want the rotated value, and every window to close. TADR's own
   history shows both failing. The staircase yardmap (`ed71666`) was the cursor preview's swap
   leaking into the next create. The "venom" crash (`577a22d`) was a re-entered
   `Order_MobileBuild` returning to its inner caller, because the envelope kept its return
   address in one global. The fix is a stack of 8 frames. The yardmap readers keep another stack
   of 8, and at depth 8 **they skip the swap silently**
   (`if (g_yardmapFrameTop >= YARDMAP_FRAME_STACK_SIZE) return 0;`, `unitrotate.cpp:1413`), which
   is an unrotated read and a wrong stamp. Section B's lesson applies: order handlers re-enter
   through `0x43A21A`, so no detour may keep a return address or per-call state in a global.
2. **Rotation is inferred from a jittered heading.** `RotationFromHeading` rounds `heading − 0x8000`
   to the nearest quarter. The spawn's jitter is uniform over `[−BuildAngle/2, BuildAngle/2)`
   (§0), so a south build is read as E or W whenever the jitter reaches ±0x2000, which needs
   `BuildAngle > 0x4000`. Five stock structures qualify: ARMLLT, CORLLT and CORSOLAR at 32768
   (±90°, so about half of their builds), and ARMVULC and CORBUZZ at 29096. Stock is safe only
   because its `Rotations=` defaults to `S`. A mod that gives any of those types `Rotations=SENW`
   gets rotated yardmap reads on south-built units whose footprint was baked unrotated. The gate
   (`IsRotationAllowed`) is a filter on the type, not an identity for the unit.
3. **Order tags have no lifetime.** The map is keyed by `OrderStruct*` and never cleared when an
   order is freed. A reused address inherits the old rotation, and the only defence is the same
   type filter ("ignore stale entries … if the unit type doesn't even allow this rotation").
4. **Remote peers bake the building unrotated (mechanism DIS, consequence INF).**
   `CreateFromNetwork 0x4861D0` runs the spawn and the stamp inside its own body (§0) and never
   reaches `0x485F50`'s entry hook. TADR's thunk writes the heading only after it returns. On
   every other peer, a rotated ARMVP (8×6) therefore has `+0x7E = 8, +0x80 = 6`, a grid origin
   derived from them, and an unrotated first stamp. On the next yardmap-reader call, the rotated
   heading swaps in the 6×8 copy, which the reader then strides as 8 wide. *Settle with:* two
   peers, a rotated-E ARMVP on one of them, and a dump of the other peer's occupancy cells and
   `+0x7E/+0x76` for that slot.
5. **The rotated-yardmap cache outlives the yardmaps it copies (mechanism DIS + SRC).** Every
   level teardown frees each type's yardmap: `0x491B60` calls `0x42DB90` at `0x491C21`, which
   frees and zeroes `def+0x14E` for every type (`0x42DC18..0x42DC2B`), along with the model, the
   COB and the build list. The count is then zeroed, so the menu-time loader `0x42A8D0` rebuilds
   the defs and their yardmaps for the next game. TADR's teardown hook restores the active swaps,
   and that part of its comment is right. But `m_yardmapCache` is keyed by type index and is freed
   only in the destructor, never at a teardown (SRC: `FreeGameData_Entry_Proc` clears the swaps;
   `CBuildGhost::OnGameTeardown` clears only the ghost's caches). In a second game of one session,
   `GetRotatedYardmap` therefore hands out copies of the first game's yardmaps, under indices that
   may now name other types. `ClearYardmapRotation` then writes the first game's **freed** pointer
   (`origYardmap`) into the new def. That leaves a dangling `def+0x14E`, which later reads use and
   which the next teardown frees a second time. *Settle with:* two games in one session, a
   rotatable type rotated in the second, and the def's yardmap pointer compared before and after
   one reader call.
6. **The receiver's heading copy is unbounded but harmless**: it writes a word. The resurrect
   lookup bounds its map cells (`CaptureResurrectFeature`), which is right. The build ghost indexes
   `MODEL_PTRS[BuildUnitID]` with no bound against `UNITINFOCount` (`buildghost.cpp:895`). Our
   `model_root` does bound it.

#### 1d. What section B already said

B's Part 3 §7 filed TADR's four rotated-unit fixes (`ed71666`, `b306cbf`, `fa27e3e`, `577a22d`) as
"TADR fixing its own code, out of scope; we have no unit rotation", and kept one lesson: handlers
re-enter. B's Part 3 §1 split resurrection's rotation half off as rotation-only. B6's bounded
yardmap parse (`JunkYardmapFix`, `0x42CF5E`) is a prerequisite for any rotated-yardmap table: a
rotated copy is only as sound as the `footX·footZ` bytes it is built from.

#### 1e. Proposed our-design (for whichever plan takes rotation)

Every piece is a bound, a lifetime or an ordering; nothing writes the def.

- **The facing is data, per unit, with the slot's lifetime.** A DLL table `rot[slot]` (2 bits,
  sized to the unit array's own bound). It is written at every create path from that path's
  source, cleared in the destructor `0x4866D0` and zeroed with the array by the level load
  `0x4854A0`. It is never inferred from the heading.
- **The bake takes it.** At `0x485AA4`, swap the two words when `rot[slot]` is odd. The grid origin
  (`0x485B99..`) and every later reader of `+0x7E/+0x80` then follow by construction, and so do
  the pathing and the four unit-based yardmap readers' dimensions.
- **The yardmap is looked up, not swapped.** Replace the four unit-based loads of `def+0x14E`
  (`0x47C7FE`, `0x47CD82`, `0x47D166`, `0x47DA47`) with `yardmap(unit)`: the def's own pointer, or
  the rotated copy from a per-type table. The table has the yardmaps' own lifetime: it is built on
  the game thread after the menu-time loader's success exit, and cleared in the level teardown
  before `0x42DB90` frees the originals (`0x491C21`). Every copy is `footX·footZ` bytes, the size
  the loader allocated. A type with a dimension ≤ 0 is refused.
- **The placement testers get the rotation as an argument, not through a global.** Wrap
  `0x47D2E0`, `0x47DB70` and `0x47D820` (and the handlers' own snap and clear reads in
  `0x403A20`/`0x413D80`) in C wrappers that call the original through a trampoline, with the
  rotation in a context that the wrapper pushes and pops around the real `call`. Nesting then
  follows the call stack, and no return address is ever patched.
- **The order carries it with the order's lifetime.** A field in the order the engine allocates
  (0x56 bytes, `0x43ADCB`), if a spare one exists, *INF: read the order's readers*. Otherwise a
  table keyed by the order and cleared at the order's free.
- **The wire carries it explicitly.** Not by rounding `rec+0x13`: a tagged 2-bit field in `0x09`'s
  bank word (`rec+0x11`), which the spawn always writes as 0 and no stock receiver reads (§0), or
  a companion message as in A′3 and B4. The receiver sets `rot[slot]` **before** it calls the
  spawn at `0x4862B8`. Give (`0x14`), resurrect, load and, INF, the `0x2C` round-robin create
  (`0x48B497`, whose full state carries no heading) each get the same treatment.
- **Draw:** the build ghost stands at `0x8000 + r·0x4000`, and the order pass's squares
  (`tagpu_order.c`) swap the footprint for odd `r`. The cursor square follows `TestBuildSpot`'s
  own corners.

#### 1f. Size, and the routing

About **25–30 engine sites**: the bake (1); the unit-based yardmap reads (4); the three
placement testers plus the two build handlers' own reads (about 5 wrappers); the create paths —
the build ×2, the network create, give, resurrect, load, and the round-robin create, INF (7); the
order's allocation and free (2); and the wire field in `0x09` and in `0x14`'s receive. Plus two
DLL tables, the input binding and the build-menu affordance (a GUI item), our ghost's heading and
the order squares. **Class: sim + wire, fail closed** (rule 3; section B's operational test: a
peer without it silently stamps different cells). Stock content is untouched by construction,
because no stock FBI carries `Rotations=`.

**Recommendation: C does not take `Rotations=`.** It is the opt-in for a D feature of about
landing L2's size (52 sites), with a wire change, so it wants its own plan page and its own
multiplayer test (two peers). Nothing else in C depends on it. `PreviewPiecesS/E/N/W=` and
TADR's whole preview-facing logic depend on it and go with it.

#### 1g. How to reproduce and test (if planned)

- **The remote-peer defect in TADR (§1c.4):** the stock 8×6 ARMVP rotated E on peer 1; on
  peer 2, `tacli peek` the slot's `+0x7E/+0x80/+0x76/+0x78` and the occupancy cells under it.
- **Ours:** the same fixture on two peers must give identical `+0x7E..+0x80`, origin and cells on
  both. The 18 non-square stock structures (for example ARMVP 8×6, ARMHP 8×7, CORMAKR 4×3) and the
  5 square structures whose yardmap changes under a quarter turn (ARMLAB, ARMAVP, CORLAB 6×6,
  CORVP 7×7, CORGANT 9×9) are the regression set. Units must path around the rotated door
  cells on both peers.
- **The jitter trap:** CORSOLAR built south, many times, with `Rotations=SENW` in a test FBI.
  None may read as rotated.

#### 1h. Questions for the owner

1. Is building rotation wanted at all? If so, is it a D landing of its own, or part of D's survey?
2. Should the wire use `0x09`'s bank word (no new message, readable by same-build peers only), or
   a companion message?
3. When a mod's `Create()` script turns a structure's base piece (TADR's documented limitation),
   the model and the footprint disagree. Leave that as a documented limitation?

**Routed 2026-09-25:** the owner sent `Rotations=` to group D with building rotation. These three
questions stand for D's plan, with this section as its evidence.

### 2. `PreviewPieces=` and the pieces `Create()` hides

#### 2a. What TADR does, and what we do

TADR draws its ghost from the raw 3DO template, with no script run. So by default it hides any
piece whose name contains `flare`, `flash`, `muzzle`, `fire`, `flame` or `wake`
(`IsEphemeralEffectPieceName`). `PreviewPieces=` replaces that with an exact whitelist: names
split on whitespace, `,` and `;`, lower-cased, and matched against each node's name with a
63-char cap. The per-facing `PreviewPiecesS/E/N/W=` win over it for that facing. Children of a
skipped piece are still walked (SRC, `buildghost.cpp:398-440, 590-690`).

Our ghost (`tagpu_native.c`, `ghost_pieces`, SRC) walks the same template on the render thread,
parents first, and **marks every node visible**. The code says so and names it a known
deviation. Measured against the stock scripts with `ta3do`'s rule, the deviation is real on
**21 of 126 structures**, and on 82 of the 278 unit types overall (the ghost only draws
structures). Each of those hides a piece **that has faces** in the first straight-line run of
its `Create()`. Examples: ARMLLT `flare` (3 faces), ARMHLT `flash1`/`flash2`, ARMGUARD
`flare1`/`flare2`, ARMAP `radar`, ARMVULC `shell`, CORMOHO `dingle1`/`dingle2`/`rotary`. TADR's
name heuristic agrees with the script on 123 of the 126 structures and misses those last three,
whose hidden pieces are not named like effects. Across the 126 it never hides a piece the script
keeps.

#### 2b. Class

Draw, local: skip and log. Nothing on the wire, and no peer's state depends on it. It is not a
simulation change, so rule 4 does not bind it, and it rides the ghost's existing lever.

#### 2c. Proposed our-design

- **The default is the script's own hides, read, not run.** At level load on the game thread
  (after `0x42D2E0` has stored each type's COB at `def+0x18E`), or lazily in the packet publisher
  the first time a type is published for a ghost: walk `Create()`'s prologue with `ta3do`'s
  conservative rule and stop at the first opcode whose length is not certain. That means
  `HIDE`/`SHOW` with one operand and the listed fixed-length opcodes (`tools/ta3do`,
  `COB_PROLOGUE`). Bounds: the method count, the piece count and the bytecode length from the
  COB's own header; the entry point below the code length; each piece operand below the piece
  count. Name compares are capped.
- **`PreviewPieces=`, when set, replaces the default for that type**, as in TADR. The list is
  parsed from a bounded FBI string, the key reader being Part 3's shared mechanism.
- **The result is a bitmask in the ghost's own walk order**, computed by the **same** walk
  function `ghost_pieces` uses, so the order cannot drift. It carries its piece count, and the
  render side applies it only when the count matches, falling back to all visible and counting
  the mismatch. It travels to the render thread through the packet (the ghost's `PK_BUILD` path),
  never as a render-thread read of engine or loader state. The table's lifetime is the level:
  cleared at the teardown the render thread is already held out of.
- **The per-facing variants** only exist with rotation (§1); they go to D with it.

#### 2d. How to test

The ghost of ARMLLT, ARMHLT, ARMAP and CORMOHO at the cursor, compared with the finished building
(`import -window` pairs; the ghost at `alpha=1.0` so the silhouette reads): the flares, the dish
and the dingles are gone from the ghost and match the finished building. A regression over the
105 structures whose `Create()` hides no piece with faces must stay pixel-identical. Then an FBI in a scratch
`.ufo` with `PreviewPieces=base` must draw just the base.

#### 2e. Questions for the owner

1. Fix our ghost's hidden-piece residual (21 stock structures) as part of C, with the key as an
   override? Or separately, as a ghost fix, with the key after it?

#### Decided 2026-09-25

**One landing, C's first (C1):** the hides read from `Create()` are the default mask, and
`PreviewPieces=` replaces it for its type. One mask, one walk, one packet path. No file change is
needed for stock content. The per-facing variants go to D with rotation.

### 3. `PreviewObject3D=`

**What TADR does (SRC, `buildghost.cpp:889-929`).** It lower-cases the FBI value and builds
`objects3d/<name>.3DO` with the engine's path helper. It loads the file through the engine's own
3DO open, parse and texture-match, and caches the root by name, negative results included. The
cache is dropped at teardown without freeing anything: the entries "hold TA-owned pointers".
Whether the engine frees such a model at the level teardown is **INF**. Every other preview key
then applies to the override model's piece names. It does not check the name, so a value with
`..\` or a path separator names a file outside `objects3d` [SRC; whether the engine's path helper
and archive lookup accept it is INF].

**In our stack** the ghost draws on the render thread, which must never call an engine loader
(allocation, texture tables). A port is: the game thread loads the model at level load, frees it
at the teardown under `tagpu_reclaim`'s wrap, keys our bake by the template it loaded (posebake
already keys by root pointer, and the cache must not outlive the template: INF, check that
posebake flushes at the level boundary), bounds the name, and refuses separators. **No stock
content uses it.** Its purpose (upgrade-style units whose model bundles variants) is largely what
§2's `Create()` rule and `PreviewPieces=` already cover. **Recommendation: park it**, unless a mod
the owner plays uses it.

### 4. `PreviewFaceOpponent=`

**What TADR does (SRC, `buildghost.cpp:1020-1115`).** When a selected, completed local builder has
the cursor inside its `builddistance`, it takes the nearest enemy player's *first unit slot* (the
`PlayerStruct` block pointer), snaps the direction to a cardinal, and overrides the preview's
facing, bypassing `Rotations=`.

- **The premise is not in stock.** It serves a `Create()` script that turns the structure toward
  the nearest enemy commander. Stock COB has no getter that can name another player's unit. The
  shipped recorder adds ids that can (`MIN_ID`..`MAX_ID` to walk every unit, `UNIT_ALLIED` to pick
  enemies), and Escalation's scripts use exactly those (Part 5). `UNIT_NEAREST` (177) is an id of
  the recorder's unreleased 4.0 tree. Nothing in our stack implements COB extensions.
- **The leak is not closed.** The builder-radius gate restricts where the cursor may be, not what
  is read. The answer depends on the true position of the nearest enemy commander, fogged or not,
  anywhere on the map. Hovering yields the commander's cardinal direction for free and repeatably.
  The script leaks the same bit too, but only once the structure exists.
- **"First slot = commander" is not an identity.** The allocator reuses the lowest free slot
  (section B, Part 1 §0), so after the commander dies the preview faces whatever that player
  builds next.
- **In our stack** it would be computed by the packet publisher on the game thread (it reads
  replicated unit positions), with only the chosen facing sent to the render thread, so the
  frame-packet rule is not the obstacle.

**Recommendation: do not port.** Revisit only if the COB extensions are ever ported. At that
point, choose between using only commanders the local player can see (no leak, and a preview
that may disagree with the script) and TADR's rule (a documented leak).

### Engine facts this part established (for `exe-reverse-engineering.md`)

| address | fact |
|---|---|
| `0x485AA4`/`0x485AAA` | the spawn bakes `def+0x14A` (footX, footZ as one dword) into `unit+0x7E` (`+0x80`) |
| `0x485B99..0x485BD3` | the grid-origin cell `+0x76/+0x78 = (pos − foot·2^19 + 0x80000) >> 20` |
| `0x485C02`, `0x485BD6..0x485C06` | bank = 0; heading = `rand(BuildAngle) + 0x8000 − BuildAngle/2` (`0x4B6C30`) |
| `0x4860A0..0x486115` | `0x485F50`'s order: spawn, `0x485D40`, `0x49E070`, `0x437840`, (mobile: movement object, heading = `def+0x210` at `0x4860ED`), `0x48A870`, stamp `0x47CC30`, send `0x09` |
| `0x456050` | `0x09` = 0x17 bytes, `{09, type@1, index@3, pos(12)@5, bank/heading/pitch@0x11}` |
| `0x4861D0` | reads `rec+1/+3/+5..+0x10` only; spawn at `0x4862B8`; mobile heading `0x4862FA`/`0x486301`; structures keep the receiver's own jitter; stamp `0x48630C` |
| `0x48B200` | the full-state entry: type, `+0x108`, build fraction `+0x104`, `+0x10E`, `+0x110&3`, transporter index, `+0xF9`; no heading |
| `0x47C7FE`, `0x47CD82`, `0x47D166`, `0x47DA47` | the four unit-based yardmap reads of `def+0x14E` (dims from `unit+0x7E/+0x80`, origin from `+0x76/+0x78`) |
| `0x47D2FA`/`0x47D4ED`, `0x47DB7F`, `0x47D82D`/`0x47D8C2` | the def-based placement testers' footprint and yardmap reads; `0x47D820` called from `TestBuildSpot` at `0x4198FE` and from `0x47DE4A` |
| `0x42CF2A..0x42CF7A` | the yardmap: zeroed, then allocated `footZ·footX` bytes for `bmcode == 0` only |
| `0x42D2E0` (from `0x4918CA`), `0x42D8E5..0x42D8F4` | the level's unit-data load: `scripts\<name>.COB` through `0x4B2450` into `def+0x18E`; read by `0x485D40` |
| `0x42DB90` (from the level teardown at `0x491C21`) | opens the def array (`0x4D8780`), then for every type frees and zeroes the model (`MODEL_PTRS`), the yardmap (`0x42DC18..0x42DC2B`), the COB (`0x4B2540` at `0x42DC3C`, zeroed at `0x42DC41`) and the build list (`+0x156`/`+0x152`). The yardmap's lifetime is therefore the menu-time load to that game's teardown |
| `0x403A20`, `0x413D80`, `0x404DB0`, `0x488570`, `0x419670`, `0x4197D0`, `0x438C00` | function entries (nop-padded): the two build handlers (no direct callers; dispatched), resurrect, give, issue-build, `TestBuildSpot`, `DrawBuildSpotQueue` |

### What this part did not establish

- **The remote-peer footprint defect in TADR (§1c.4)**: the mechanism is DIS; the stamped cells on
  a second peer were not measured.
- **A spare field in the 0x56-byte order** for the facing, and the order's free site.
- **Whether the `0x2C` round-robin create (`0x48B497`) can create a structure**, and from which
  position. The heading is not in `0x48B200`, but the caller's position source was not read.
- **Whether the engine frees a model TADR loads through `PreviewObject3D`**, and whether our
  posebake flushes its template-keyed caches at the level boundary.
- **The capture path** (TADR's `fa27e3e` lists it) was not traced.
- **TADR's stale yardmap cache in a second game (§1c.5)**: the frees and the missing clear are
  DIS and SRC; the dangling pointer was not observed.
- **Mod usage** of any of these keys: no stock FBI uses them, and no mod content was read.

---

## Part 5. What Escalation's content uses

A census made 2026-09-25 of TA: Escalation GOLD 10.2.0, the largest mod written against these
keys, to settle the contract's exception (content that relies on TADR's code rather than its
documentation). Source: the distribution's RAR (`vendor/taesc/`, gitignored; sha256 `a9873e55…`),
its HPI archives read with `tools/hpipack.py`, a tolerant TDF parser, and `tools/tacob` for two
scripts. Counts and names only; nothing from it is in the repo, and nothing in it was run.

**Escalation does not load stock data folders.** Its hex-edited `TotalA.exe` reads `weaponE\*.tdf`,
`unitsE`, `gamedatE` and the other renamed folders, never `Weapons\`. So it does not run on our
stack as shipped: our patches are checked against the stock 3.1 exe, which reads the stock folders.
What the census gives is the shape of real content written for these keys.

| key | uses | shape |
|---|---|---|
| `surfacefire` | 11 weapon sections | **all 11 carry `waterweapon=1`.** Four D-guns (`DGUN_*`: `lineofsight`, `beamweapon`, `commandfire`, `weapon3` of the commanders and decoys, which walk on land and on the seabed); five sub-launched `vlaunch`+`selfprop` missiles fired from submerged submarines (`VSPAM_UW`, `VLAUNCH_SUB_*`, `NUKE_SUB_*`), all five also `nottounderwater`; two beams no unit names. 10 of 11 carry `nottoair`; `DGUN_DECOY_CORE` does not, where its three siblings do. |
| `nottoair` | 73 weapon sections | only surface-attack weapons (cannons, torpedoes and depth charges, rockets, nukes, sub missiles, three of the four D-guns); never with `toairweapon`. 43 of the 161 units carrying one also carry an AA weapon, nearly always `weapon3`. |
| `nottounderwater` | 5 | the five sub-launched missiles above. |
| `VeterancyThresholds` | 197 FBIs | **every list has exactly 5 entries**, strictly increasing, one of four tier lists (`10…50`, `20…100`, `50…250`, `100…500`). None malformed. The other 352 FBIs take the default. |
| `VeterancyAccuracyBuffRate` | the same 197 | 24 / 48 / 120 / 240, one per tier. None zero or negative. |
| `PreviewPieces` | 15 FBIs | the whole-model list; no per-facing variant. |
| `PreviewFaceOpponent` | 12 FBIs | long-range defences (Big Bertha, Intimidator, Guardian, Punisher …). Their scripts find the enemy through the recorder's extended COB getters (`GET` 69, 70, 73, 74, walking every unit ID; `tools/tacob` on `ARMBRTHA.cob` and `CORINT.cob`), which stock COB and our stack do not have. |
| `Rotations` | 0 | only in comments, in two FBIs. |
| `notoverwater`, `notoverland`, `nomapweaponalert`, `reloadbar`, `TransportedExplodeAs`, `TransportedSelfDestructAs`, `PreviewPiecesS/E/N/W`, `PreviewObject3D` | 0 | — |
| weapon `ID=` | 240 sections | 0..243; none above 255. |

**What it settles.**

- **`surfacefire` follows its documentation**: every use is a water weapon, so no content relies
  on TADR's non-water behaviour, and the can-aim hook it would need is not ported. The two shapes
  C2 must work for are the ones Escalation uses: a D-gun that is a water beam weapon, fired by a
  commander on land at land units and from the seabed at surface units; and a `vlaunch` missile
  from a submerged submarine that must steer once above water (the guidance at `0x49B9EB`).
- **"Never aircraft" changes one Escalation weapon.** Under TADR's code `DGUN_DECOY_CORE`, whose
  siblings all say `nottoair=1`, can be ordered onto an aircraft; here it cannot. That follows the
  documentation and the author's pattern, and it is recorded rather than excepted.
- **Veterancy's 25-level ceiling touches no Escalation unit.** With five thresholds a list reaches
  five levels. Escalation's changelog records the ceiling as "up to 5 levels (up to 25 levels in
  future version)".
- **`PreviewPieces=` is live content** (C1's override), and **`PreviewObject3D=` has none**: it stays
  parked.
- **`PreviewFaceOpponent=` depends on the recorder's COB extensions**, not only on rotation: the
  buildings it previews turn by getters our stack does not implement.
- Side findings, not C's: 4 FBIs name weapons no file defines, and 17 `explodeas` or
  `selfdestructas` values name missing weapons.
