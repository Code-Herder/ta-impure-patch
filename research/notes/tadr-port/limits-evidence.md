# A. Raised ceilings — the evidence pass

A read-only pass over the eleven ceilings of [the merge exploration's §A](../tadr-merge-exploration.md#a-raised-ceilings),
made 2026-09-23 to settle what each one touches before planning the port; the decisions it fed are
in [the plan](raised-limits.md). Sources: `pristine/TotalA.exe.pristine` (main checkout),
`i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd`. Tags: **DIS** = disassembled here, **SRC** = read in TADR's source,
**INF** = inferred, with the measurement that would settle it.

Two RNGs matter throughout. The **sim RNG** is `0x4B6C30` (Park–Miller, state `0x51FC88`, 129 call
sites; COB `rand` goes through it). **CRT `rand()`** is `0x4E4870` (MSVC LCG, per-thread state).

TA's network model ([networking-lobbies](../networking-lobbies.md)) is **state + event replication, not lockstep**. So a
difference in a local pool's fullness between two peers matters only where it changes something
every peer must agree on. That is established nowhere for the effect pools; see the per-item INF lines.

---

## 1. Projectiles 300 → 3000 (`EngineLimits.cpp` `AddProjectilePatches`)

**Layout (DIS).** Count `main+0x141F3`, base `main+0x141F7`, stride `0x6B`. `0x499A30` allocates
`0x7D64` (`push` operand at `0x499A32`) and clears with `mov ecx,0x1F59` / `rep stos` (operand at
`0x499A56`). It is called only from `0x4918B6` (level load). `0x499A80` frees the pool, called only
from `0x491C30` (teardown). **The pool is allocated per game.**

**TADR's writes (SRC; each checked against DIS):**

- The allocation and clear sizes above.
- Ten cap operands: `0x49B6F0 0x49B80A 0x49C9D2 0x49CC34 0x49CDF3 0x49D011 0x49D2BE 0x49D4B5
  0x49DD96 0x49DF24`. These are the operands of the ten `cmp …,0x12C` at `0x49B6EE 0x49B809
  0x49C9D1 0x49CC33 0x49CDF2 0x49D010 0x49D2BD 0x49D4B3 0x49DD95 0x49DF23`.
- The compaction function `0x49AE20`:
  - Its `sub esp,0x4C0` becomes a `jmp` to a naked stack probe, which then does `sub esp,0x2EF0`.
  - The second-array displacement `0x278` at `0x49AEB8` and `0x49AF39` becomes `0x20 + 2·3000`.
  - The epilogue `add esp,0x4C0` at `0x49AF7F` becomes `0x2EF0`.

The compaction frame (DIS) holds two i16 arrays: `[esp+0x20+i·2]` (the old index) and
`[esp+0x278+i·2]` (the new index). 0x258 bytes each is 300 entries. Indices are 16-bit
(`proj+0x67` is a WORD, read back with `movsx`), which is fine up to 32767.

**Missed sites: none found (DIS).** All 61 references to `+0x141F3`/`+0x141F7` walk the live count.
Every `0x12C` in `.text` was classified: the eleventh near this code, `0x49F442`, is a
command-line clamp `[30,300]` into `main+0x37F31` and has nothing to do with projectiles. The only
stack frame in the binary sized for 300 entries is `0x49AE20`'s. Also relocated by the compaction:
`main+0x142F7`, a projectile pointer that it fixes up.

**SIM (DIS).** `0x49B6E0`, the append, returns NULL when full, and the caller's projectile — with
its damage — does not exist. The cap sits on both the local fire paths and the receive path
(`0x49D2BD`, `0x49D4B3` are inside the `WEAPON_FIRED` receiver, which TADR's comments place at
`0x49D27E`).

- **INF:** whether a remote-created projectile that is refused changes damage on that peer depends
  on who computes damage (the `0x0B` damage packet suggests the firer or the victim). Settle it with
  two MP peers, one capped at 300 and one at 3000, a scenario with more than 300 projectiles in
  flight, and a comparison of unit HP (`tacli units`) on both peers.
- The projectile code also draws the sim RNG (`0x49B8D1`, `0x49B903`, `0x49D727`, `0x49D733`).

**Our code that must follow:**

- `inc/tagpu_engine.h:241` `PROJ_COUNT 300`, and the comments at `:234-240`.
- `src/tagpu_packet.h:328` `TAGPU_PK_MAX_PROJ 300u` and `:239-242` ("exactly 300 slots… the bound
  is the allocation itself").
- `src/tagpu_packet_pub.c:1070` (`np` clamped to `PROJ_COUNT`), `:1083`, `:921` (the scratch
  array), and the header comment at `:1066`.
- `src/tagpu_packet.c:534`: the packet validator rejects `n_proj > MAX`.
- `src/tagpu_weapons.c:40-41,712`: walks `n` with no cap, which is fine because it follows the count.

**Failure mode if we raise the cap and forget these:** the renderer silently draws only the first
300.

## 2. Explosions 300 → 3000 (`AddExplosionPatches`)

**Layout (DIS).** The count is `main+0x1491B` and the records are **inline** at `main+0x1491F`,
stride `0x54`. The sequence table `main+0x1AB8F` sits directly after the array: `4 + 300·0x54 =
0x6274`, and `0x420AA2` indexes it as `[edi+eax*4+0x6274]` with `edi` = pool base.

**TADR relocates the pool into a DLL static (SRC).** All seven base references are rewritten (DIS
confirms these seven are the only ones in `.text`): `0x420630` (the reset, in `0x420620`, whose
only caller is the level load `0x4919D2`), `0x420A36`/`0x420A3C` (the add `0x420A30`),
`0x420B35`/`0x420B3B` (inside `0x420B00`), `0x420F66` (inside `0x420F30`), and `0x421738` (the
piece explosion `0x421700`). The table load at `0x420AA2` becomes a call that reloads `main`. The
caps change at `0x420A44` and `0x421771` (the `cmp ecx,0x12C` at `0x420A42` and `0x42176F`).

**Missed sites: none found.** I scanned the four functions for other displacements relative to the
pool base: every other large displacement there is `main`-relative, not pool-relative.

**SIM-RNG-coupled (DIS).** `0x421700`, the piece explosion, loops over a unit's pieces. At
`0x42176F` it exits when explosions ≥ 300. Otherwise it allocates an aux record (§4) and **draws
the sim RNG eight times** a piece (`0x421851…0x4218D2`, then `0x421B39` and `0x421B62` on the same
path). So the explosion cap decides how many sim-RNG draws a death makes.

- **INF:** whether the sim-RNG stream must agree across peers. Stock peers can already differ in
  live-explosion count (remote events land later), which argues that it need not. Settle it by
  comparing `0x51FC88` (or `tagpu_cobtrace` rand lines) on two peers across a mass death, with the
  cap equal on both, then unequal.
- Single-player: the stream differs from stock, and no oracle cares.

**Per game:** the reset runs from the level load. With relocation, the backing becomes
process-lifetime static memory.

**Our code that must follow:**

- `inc/tagpu_engine.h` `OFF_NEXPL 0x1491B`, `OFF_EXPL 0x1491F`, `EXPL_COUNT 300` (around
  `:263-268`).
- `src/tagpu_packet.h:276-277,329` (`TAGPU_PK_MAX_EXPL 300u`).
- `src/tagpu_packet_pub.c:1061,1184-1190,922`.
- `src/tagpu_packet.c:535`.
- `src/tagpu_fx.c:27-28` (comment).

**Under relocation our gather reads the dead inline array.** The count stays 0, so the renderer draws
**no explosions at all**, silently. This is the conflict-audit item the merge note already names.

## 3. Model-effect ("flying piece") slots 100 → 1000 (`AddModelEffectPatches`)

**Layout (DIS).** 100 dwords at `0x511DF0..0x511F80`. `0x511F80` is **also** a pool-allocator
object (`this` for `0x4379B0` init and `0x437A30` alloc) with a 100 000-byte backing
(`push 0x186A0` at `0x4208FB`).

**TADR's writes (SRC, checked DIS):**

- Seven base operands (`0x420B08 0x420F38 0x421153 0x421172 0x4211A7 0x42165F 0x421680`) and six
  end operands (`0x420B28 0x420F53 0x421162 0x42118D 0x4211C3 0x42166D`). These cover **every**
  array use of the two addresses.
- The five `mov ecx,0x511F80` (`0x4205F0 0x420610 0x420900 0x420977 0x421691`) are the allocator's
  `this` and are **correctly left alone**.
- The backing grows ×10 (`0x4208FB`).
- The reset `0x42090A` (`mov ecx,0x64; xor eax,eax; mov edi,0x511DF0; rep stos`) becomes a call to
  a C reset.
- Missed sites: none.

**VISUAL as far as the RNG goes (DIS).** The slot allocator `0x421620` draws no sim RNG. The live
spawner `0x481140` (reached through the function table at `0x4FD6CC`) draws its six sim-RNG values
**before** calling `0x421620` at `0x48123C`, so the draw count does not depend on this cap.
`0x420E50`, which looks like a second spawner, has no caller or pointer anywhere in the image.
INF: that flying pieces deal no damage.

**Not stock below the cap (DIS).** The backing allocator `0x437A30` is a
ring that evicts the blocks it wraps over and nulls their owners' slots (`0x437A9D`, `0x437ACF`).
A ten-times ring evicts later, so the raised build keeps pieces stock would have dropped even with
fewer than 100 alive: more landings, more explosions, a different C-runtime `rand` stream.

**Per game:** init runs from `0x4919D2`, teardown from `0x491B9F`.

**Our code that must follow:**

- `inc/tagpu_engine.h` `VA_PSYS_BEGIN/END 0x511DF0/0x511F80`.
- `src/tagpu_packet.h:288-289,330` (`TAGPU_PK_MAX_DEBRIS 100u`).
- `src/tagpu_packet_pub.c:1158-1167,923`.
- `src/tagpu_packet.c:536`.
- `src/tagpu_fx.c:26,732`.

**Under relocation we read the dead static array and draw no flying debris,** silently.

## 4. Aux debris records 300 → 3000 (`AddAuxEffectPatches`)

**Layout (DIS).** 300 × `0x34` inline at `main+0x1AB9F`, free marker byte `0xFF`, initialised by the
level-load loop `0x420804…0x420882`. There are two first-free scans, both
`cmp eax,0x12C`:

- `0x420920`, a standalone allocator, which TADR replaces wholesale with a `jmp` to a round-robin C
  allocator. Nothing in the image calls it or points to it.
- An inline copy at `0x4217DE…0x4217FB`, which TADR replaces with `call alloc; jmp 0x421804`. That
  also bypasses the found-branch `0x421D79…0x421D90`.

TADR re-creates the stock initialiser in C: 8 vertices, 6 faces, `+0x1C = 0x502C28`, and face
companions at `0x502BF8+8k`. The stock loop still initialises the dead inline array, which is
harmless. **Missed sites: none** (all `+0x1AB9F` references are those four).

**SIM-RNG-coupled (DIS).** In `0x421700`, when the aux scan fails, `0x421802…0x421808` skips the
six sim-RNG draws. So aux fullness decides the draw count too. **TADR's own comment ("this pool is
visual-only, so allocation order cannot affect simulation determinism") is half right.** Order
does not matter; success does.

**Our code:** nothing reads `+0x1AB9F` directly. The debris gather follows `sys+0x2C`
(`tagpu_packet_pub.c:1158ff`), so a relocated record is reached by pointer. With TADR's scheme it is
process-lifetime static memory, which is a *better* lifetime than stock's.

## 5. Units per player 250/500 → 1500 (`LimitCrack.cpp:466-473`)

**Sites (DIS).**

- `0x49163F push 0xFA` (the ini default, operand `0x491640`), `0x491658 cmp eax,0x1F4` (operand
  `0x491659`) and `0x491665 mov eax,0x1F4` (operand `0x491666`). The result goes to `main+0x37EEC`.
- The process init `0x4912EE…0x491308` (in `0x491200`, before its own ini read) copies it to
  `+0x37EE6` and `+0x37EEA`; the game start is what carries the configured value into `+0x37EE6`.
- **In MP, the host's value is broadcast and used**, the unit array being `10·limit+1`: the game
  start writes `+0x37EE6` from the host record's `+0xA5` (`0x4973B5`), unclamped. A DirectPlay
  lobby launch writes `+0x37EEA` and `+0xA5` from `0x512D6C` (`0x449D87`, `0x449D9B`), also
  unclamped. Landing 2 clamps the store at `0x4973B5`.
- **Two unclamped side doors (DIS).** `0x432646`, in `0x432610`, writes `+0x37EEC` from the key
  **`maxunits`** (`0x4B4800`, default 0) of a saved game's `[Summary]`, read when the game's TDF at
  `main+0x38D6B` has no `BetweenMissions` key (`0x497B29`); TADR's `tdraw.txt:203` "unit limit
  reached in between single player missions" may be this path. `0x436037`, in the map loader
  `0x435DA0`, writes the array's own count `+0x37EE6` from the map `.ota`'s `[GlobalHeader]`
  `maxunits` (default 200). Every retail `.ota` sets 200 to 400. Landing 2 clamps both.

**`0x44CAFE` is NOT a per-player limit (DIS), whatever TADR's name for it, `MPUnitLimitAddr`,
says.** It is `mov ecx,0x65` (101), taken when
`0x46E330`'s per-unit-type lookup returns −1. The result is stored in the battleroom's per-type
table (`[0x5129B4]+i+0x5A`, `[0x5129C4]+i·4`), built per UnitDef from `def+0x186/+0x18A` and
sprintf.

- 101 is the "unrestricted" sentinel of the per-type restriction slider (0..100), and TADR
  replaces it with the unit limit so that "unrestricted" is not a cap of 101 a type.
- **Settled by disassembly (landing 2).** Cancel in the restriction menu (`0x44C6FC`) writes the
  saved per-type values, the 101 sentinel included, into the restriction store (`0x44C750` →
  `0x46E550`); `0x46E160` copies an enabled type's value to `UnitDef+0x15A` and sets `def+0x241`
  bit 23; the unit constructor `0x485F50` then counts the player's units of the type and refuses
  the one at the cap. Moving a slider past 100 stores −1 instead ("No Limit", `0x44BEC0`). A
  default network game does not take the Cancel path: landing 1's peers each created 450 of one
  type with none refused. Landing 2 writes the site as TADR does.

No other `0x1F4` in `.text` belongs to the unit limit; `0x40BBDF` is a resource clamp.

**SIM, per game, network-significant** (the per-player ID blocks; [networking-lobbies](../networking-lobbies.md)).

**Our code:**

- `src/tagpu_packet.h` `TAGPU_PK_DESIGN_SLOTS` and `TAGPU_PK_MAX_UNITS 16384`. The design point was
  10 241 before landing 2; 1500 makes 15 001, below the table ceiling, and landing 2 moved the
  design point and every cap asserted against it.
- `src/tagpu_scenario.c:68` `OFF_LIMIT 0x37EEA`.
- The order arena `MAXORD` and `WR_COUNT` (`inc/tagpu_engine.h:205`, `src/tagpu_feat.c:109`) are
  unit-scaled neighbours to re-check.

## 6. Pathfinding cycles 1333 → 66650 (`0x40EAD6`)

**DIS.** `0x40EAD3 mov dword [esi+0x48],0x535` is inside the pathfinder object's init `0x40E9E0`,
which sizes its own bitmaps from map dimensions. It is the only `0x535` in `.text`. Its one reader,
the per-tick `0x40EB70`, shares it among the players as credits (the engine map has the detail). TADR writes the
dword blindly (`SingleHook`, no expected-bytes check). **Missed sites: none.**

**SIM for the owner's units; per game (the init takes the map).**

- INF: in a state-replicated model each peer paths only its own units (movement travels in `0x2C`),
  so unequal budgets do not desync. Worth one MP measurement before it is relied on.
- The cost is CPU per tick (the budget caps the pathfinder's work a tick, shared among the players).

**Our code: none reads it.**

## 7. SFX (particle) vector 400 → 16000/20480 (`IncreaseSfxLimit`)

**DIS.**

- The twenty `cmp …,0x190` operands TADR writes (`SfxVectorLimitAry`) are exactly the twenty sites
  [the engine map](../exe-reverse-engineering.md) lists (`0x471183 … 0x472CD9`, `0x472BF2` against `ecx`). **Missed
  sites: none** (the other `0x190` hits, `0x44376C 0x46AA2F 0x46AB2B 0x489F06 0x489F1E 0x497F1A`, are
  outside the emitters).
- Second, separate ceiling: **the object pool `0x51E610`** is built by a CRT static initializer at
  `0x471C80` (`push 0x4C; push 0x3E8; mov ecx,0x51E610; call 0x470A90`; it registers atexit
  `0x471CA0`). That is **once per process**, and its capacity is fixed: `0x470C10` runs once, and
  the alloc `0x470EB0` **returns 0** when used == capacity. So stock is 1000 particle objects
  total, **not** 10 × 401.
- TADR raises the capacity to 10 × vector (163 840 or 204 800 × 0x4C ≈ 12–15 MB) through an
  inline hook at `0x471C87` that rewrites `[esp]`. This works only because ddraw.dll's `DllMain`
  runs before the exe's CRT init; landing 3 writes the `push` operand `0x471C83` instead, and the
  capacity read back live as 204 800 (MEASURED 2026-09-23).

**VISUAL (DIS).**

- The emitter range `0x470F00…0x472F00` calls CRT `rand` `0x4E4870` and **never** the sim RNG.
  Emitters are also called from the explosion *draw* `0x420B00`, via `0x421550`, per the engine map.
- Past the cap the emitter destroys the front object, the "401 steady state".

**Our code that follows it** (landing 3):

- The publisher's walk stops a layer at `TAGPU_LIM_SFX + 1` (`tagpu_packet_pub.c`), the engine's
  steady state; past it the layer is counted in `layerbad` and skipped whole, so the bound has to
  move with the cap. It did: the fixed `LAYER_OBJCAP 400` is gone.
- `TAGPU_PK_MAX_PART` (sub-particles, total) is 24 576, sized from tier 1's frame of 14 510. A
  frame that holds more keeps the same share of every layer (`thin=` in the heartbeat) instead of
  losing the top layers whole; `s_fxPartTrunc` stays as the table's own bound.
- **A ceiling this pass missed:** the effects pass's vertex buckets (`tagpu_fx.h`) were 65 536
  each, and tier 1 filled the sprite bucket at the opening volley. The two buckets particles land
  in are now sized from `TAGPU_PK_MAX_PART`.
- `PART_SUBCAP 4096` filters one object's sub-particles and did not move.

## 8. Unit-type IDs 512 → 16000 (`IncreaseUnitTypeLimit`, 17 writes)

**DIS, completed 2026-09-24 for [the A′ plan](content-ids.md), which raises the limit to 16 384.**

**How types get their IDs.** The per-game load (`0x42D4DC..0x42D653`) compacts and sorts the defs,
then numbers them `0..count−1`. Def 0 is `None`, so real types are `1..count−1`. The def array is
`(files+1)·0x249` bytes (`0x42AA65..0x42AA98`), sized by the files found, with no loader cap: no
`cmp …,0x200` anywhere in the loader. The type field is `u16`, at def `+0x21E` and unit `+0xA6`,
and many loops count in `u16`, so the hard ceiling is 65 535. Stock data holds 278 unit FBIs, so
`UNITINFOCount` is 279.

**The masks, and where stock breaks.**

- **The mask blocks.** Category bitmasks are 0x40-byte (512-bit) heap blocks. `0x488C50` gets or
  creates one in the name map at `0x51E6B0`, allocating it at `0x488CC2` (`push 0x40; call
  0x4B4F10`) and clearing it with `mov ecx,0x10`. They are freed only at teardown (`0x488BF0`, from
  `0x491C3A`).
- **Writers, with no bound:**
  - name → mask, `0x488E03`;
  - the FBI `Category` tokens, `0x488EC3` (in `0x488E70`, called from `0x42CC40`);
  - **every type into `ALL`**, `0x488F21`.
- **Readers:**
  - the badTarget/noChase masks at def `+0x231/+0x235/+0x239/+0x23D` (built at
    `0x42C028..0x42C0AB`), read at `0x4063E8`, `0x406493`, `0x4070A1/C3`, `0x407181`, `0x408AF2`,
    `0x40B94F`, `0x40BA0E`, `0x40FD8E` and `0x40FE39`;
  - the commander, `0x41C364`;
  - selection, at `0x48BFCE`, `0x48DA88`, `0x48DBA3`, `0x48DCDA` and `0x49347D`.
- **Stack masks, 64 bytes each.** The AI weight proc `0x406DB0` and limit proc `0x406E40` fill
  theirs through `0x488D30`, and they are read by `0x409DC0`/`0x409E90`, which loop to
  `UNITINFOCount`. Ctrl-Z has its own, at `0x48BE00` (called from `0x496413`).
- **Stock breaks at 512 real types** (`UNITINFOCount` 513). At game start, `0x42D6C2` → `0x42BF40`
  → `0x488E70` sets bit 512 in `ALL` and in each of the type's categories. That is a heap write past
  the 64-byte block, guaranteed for the first type with ID 512 or more. Further effects:
  - Ctrl-Z with such a unit selected ORs a bit into its own return address at `ESP+0x50`.
  - An AI line naming such a unit does the same in the AI procs.
  - An AI line naming a category ORs only 16 dwords, so those types are silently left out.
  - The exact symptom of the heap write is INF.

**TADR's writes (SRC, each checked against DIS).** All 17 stock byte sequences match the pristine
exe. TADR replaces each with a 5-byte `jmp` to a trampoline that holds the widened instruction and
the tail it displaced. No branch lands inside a span except at its first byte. With
`New = ((N/8/64)+1)·64`, which is 2048 for 16 000:

- the frames of `0x406DB0` (`0x406DB5`, `0x406DC9`, `0x406DFD`, `0x406E3A`) and `0x406E40`
  (`0x406E45`, `0x406E5D`, `0x406E64`, `0x406EB2`, `0x406ED6`), with every stack displacement
  moved;
- the allocation `0x488CC2` (`push New`);
- the five clear and OR counts `0x488CD3`, `0x488E3E`, `0x406DBE`, `0x406E52` and `0x48BE22`
  (`New/4` dwords);
- Ctrl-Z's frame, `0x48BE08` and `0x48BF1E`.

**The five 16-dword loops are exactly TADR's five** (`0x406DBD`, `0x406E51`, `0x488CD2`,
`0x488E3D`, `0x48BE21`). All 116 `mov r,0x10` in `.text` are now classified:

- The sites in `0x40BF49..0x40C10D` and the rest of `0x40xxxx` are 16.16 shift counts for the
  64-bit helpers `0x4E43D0`/`0x4E44F0`, and the same holds for those in `0x489…0x48C`.
- `0x40336A` is a clamp to 16.
- The rest are 16-byte `rep cmps`, the selection flag 0x10, or graphics.

**Ctrl-A, B, C and F need no site of their own.** Ctrl+letter runs `0x4963D8`, which formats
`CTRL_%c`, and `0x48BF30` only reads the heap mask. Ctrl-C adds `0x41C310`, which reads the
`Commander` mask, and Ctrl-F (`0x48D9A0`/`0x48DC30`) also only reads. They widen with the
allocation. TADR's §B changelog line about them comes from the same commit (`4546d82`) as the OR
loop and Ctrl-Z.

**Everything else indexed by type is sized by the count**, not by 512:

- the model table, `count·4` (`0x42D693`);
- the battleroom tables at `[0x5129B4]` (`count·0x62`), `[0x5129B8]`, `[0x5129C4]` and two more
  (`0x44C900..0x44CA20`);
- the AI's per-type vectors at `+0x81/+0xA1/+0xB1/+0xC1/+0xD1/+0xE1`, resized to the count
  (`0x4092F6`, `0x409470`).

Some structures use no type index at all. The restriction store and the unit sync are maps keyed
on the FBI's CRC at def `+0x13E` (`0x46E330`, `0x46D0E0`), and the per-player count of a type is a
scan (the unit constructor `0x485F50`).

On the wire, the unit create at `0x45605E` carries a `u16`. The `0x2C` builder (`0x48B710`)
bit-packs the type into as many bits as the count needs (`main+0x14393`, set at `0x42D65B`): stock
needs 9, 16 384 needs 15. The packet stays under 0x200 bytes (`0x48B7F6`), so fewer units fit in one
(INF).

**A separate stock overflow: the build list.** Each builder's entries under `[CANBUILD]`
(`canbuild%d`; the section is `sidedata.tdf`'s, INF from the names) are read into one shared
0x3C-byte heap block the engine names `TEMP UTYPE LIST` (`0x42D971`, through `0x4D83B0`), 30 `u16`
IDs.

- The append loop `0x42DA46..0x42DA99` stops only when a key is missing.
- The builder's own copy is a fresh 0x3C-byte block (`0x42DACA`, named `CANBUILD %s` after the
  unit) at def `+0x156`. It is filled by a `rep movs` of exactly 15 dwords, with the real count at
  `+0x152`.
- The shared block is freed after the last builder (`0x42DB07`).
- **Readers:** `0x4894FD..0x48951B` loops to the count at `+0x152`. The others (`0x4094B6`,
  `0x40ABAD`, `0x40BDDB`, `0x4143F9`, `0x41B8F7`, `0x43E828`, `0x43F7A0`, `0x468604`, `0x46887B`,
  `0x48CCBC..0x48CCE4`) are not classified yet.
- So a builder with more than 30 entries overruns the shared block and makes the counted reader
  read past its own copy. It depends on the length of the list, not on the number of types.

**SIM-relevant as content** (AI build targeting, categories). Static immediates, so any time before
use is fine.

**Our code:**

- `src/tagpu_weapons.c` `WPN_MAXDEFS 4096`: a type past it keeps three weapons, with a log line.
- `src/tagpu_weapons.c` `mask_has()` reads `mask[type >> 5]` with a `u16` type from the unit's
  `+0xA6` and no bound. Bounded masks are the A′ plan's.
- `src/tagpu_cat.c` `MAX_DEFS 16384` caps only the catalogue file.
- Already bounded: `tagpu_scenario.c` `unit_type_index` refuses a count above 16 384;
  `model_root` requires `n <= 0x10000`; the packet's `model_id`, `type_row` and `PK_BUILD.type` are
  `u16`, bounded by the engine's count (`tagpu_native.c`, `tagpu_packet_pub.c`).
- The Vulkan unit pass caches 512 models and 1024 (type, owner) streams a frame
  (`tagpu_posebake.h`), and a frame over either is refused whole. Stock content can reach the
  stream limit with ten players on screen. The A′ plan sizes them first.

## 9. Weapon IDs 256 → 4096 (`WeaponIdOverflow` + `WeaponFiredExt`, off by default)

**DIS, completed 2026-09-24 for [the A′ plan](content-ids.md).**

**The loader, `LoadWeaponTdf` `0x42E440`.**

- It reads `ID` with default −1 (`call 0x4C46C0` at `0x42E463`). With **no bound in either
  direction**, it then computes `ebp = main + id·0x115 + 0x2CF3` (`0x42E468..0x42E489`).
- `Weapons[256]` ends at `0x2CF3 + 0x11500 = main+0x141F3`, **exactly the projectile
  count/pointer**, so ID 256 corrupts the projectile pool's header, as TADR says.
- **ID −1** (a weapon with no `ID=`) writes over `main+0x2BDE..0x2CF2`, the UI and input block,
  which TADR does not mention: the mouse position, the hovered cell and feature, the build
  footprint, the mode byte `0x2CC3`, `BuildUnitID` `0x2CC4` and `0x2CC6`. Such a weapon is never
  found by name, because the lookup starts at slot 0.
- **The ID byte at weapon `+0x10A` is not the TDF's ID.** The load wipe `0x42E310` sets it to the
  slot's own index as a byte, and the loader never writes it. The model path indexes by that byte
  (`0x42ED50`, `0x42ED67`, `0x42ED74`, `0x42F340..0x42F387`). So an ID-less weapon, whose byte comes
  from `main+0x2CE8`, also overwrites some other weapon's model pointer and name.

**21 references to the array, a complete list.** A raw byte scan of `.text` for every displacement
in `[0x2CF3, 0x2E08)` and for `0x11500` found 18 displacements and 3 bounds. The scan covers the
region where objdump's sweep desyncs near `0x49C740`.

- **The displacements:**
  - `0x42CDE8`;
  - the wipe, `0x42E322/32A/332`;
  - the loader, `0x42E489`;
  - the model path, `0x42ECCA`, `0x42ED67`, `0x42ED74`, `0x42F364`, `0x42F380` and `0x42F387`;
  - the release, `0x42F3B3`;
  - the meteor weapon, `0x437CFD` and `0x437D19`;
  - the `0x0F` receiver, `0x455484`;
  - the `0x0D` receiver, `0x49D295` and `0x49D29F`;
  - the name lookup, `0x49E5D1`.
- **The bounds** are `cmp r,0x11500` at `0x42E33E`, `0x42F431` and `0x49E5EB`.
- **Why relocation is clean.** Every access is `[main + id + id·276 + disp]` through one register.
  At 12 sites the `mov reg,[0x511DE8]` feeds only the weapon address, so it can become a
  same-length immediate. The two receivers share `main` with other fields, so their blocks are
  rewritten instead. Nothing computes an ID from a pointer, and the name lookup's 7 callers store
  pointers.
- **Lifetime.** The array is refilled on every level load (`0x4918BB`) and released at teardown
  (`0x491C26` → `0x42F3A0`).
- **Prior art (SRC).** TADR's Recorder `WeaponsExpand.pas` and its commented-out
  `HardCodedValue.cpp` relocate through the same sites. The second omits both receivers, so it
  worked in single player only.

**The ID in memory and on the wire.** Unit defs, unit slots, projectiles (`+0x00`), feature defs
(`+0xE4`) and the meteor global `0x512328` all hold **pointers**. The `+0x10A` byte is read:

- as the "armed" test (`!= 0`): the AI at `0x40954D`, `0x409682` and `0x409940`, then `0x49E0C2`,
  and our `tagpu_weapons.c`;
- as the model path's index and loop bound, at `0x42EC99`;
- by save games, which store it as a dword at `0x487A1C` and write it back at `0x487628`, never
  indexing by it;
- by the wire senders:
  - **`0x0D`, weapon fired, 36 bytes.** The layout: `+1` start and `+0xD` target (three `i32`
    each), **`+0x19` the ID byte**, `+0x1A` flags, two `u16` at `+0x1B` and `+0x1D`, `+0x1F` the
    target unit, `+0x21` the shooter, and `+0x23` the weapon slot.
    - `+0x1A` defines only bit 0, the interceptor, and the senders merge it into an uninitialised
      stack byte.
    - `+0x23` from the four unit senders (`0x49D7E5`, `0x49DAD8`, `0x49DCAB`, `0x49DE6D`) is
      `and dl,3`, and our extra-weapons module uses bits 0..3. The meteor sender `0x49DFA9` leaves
      `+0x1A..+0x23` uninitialised. `0x499AB0` and `0x499BA0` have no callers.
    - The receiver indexes `Weapons[byte]` directly (`0x49D27B`) and does not bound the shooter
      index at `+0x21` (a `u16` scaled by 0x118): a stock hole.
  - **`0x0E`, interceptor detonation, 14 bytes:** the type, the target point (three 16.16 `i32`),
    and the ID byte at `+0xD`.
    - Area damage `0x49A120` sends two whenever an interceptor (weapon mask bit 30) catches a
      projectile, one for each projectile (`0x49A78C`, `0x49A7CD`).
    - The receiver `0x49AF90` detonates, through `0x499EB0(proj,0)`, the **first** projectile in
      the local pool whose target (`+0x28/+0x2C/+0x30`) and ID byte both match. With no match it
      does nothing.
    - `0x499EB0` kills the projectile and plays the explosion. If the projectile's owner is local
      (`+0x66`, player type not 3), it then applies the damage (`0x499CD0`, or area damage
      `0x49A120`).
    - **So a mismatch is simulation:** two weapons whose IDs share a low byte, aimed at one point,
      would detonate the wrong projectile, with its damage.
  - **`0x0F`, feature hit, 6 bytes:** the type, the ID or a sentinel at `+1`, then `u16` x and y.
    - The only real-weapon sender is `0x42454B`. The receiver `0x45544D` reads `0xFD`/`0xFE`/`0xFF`
      as sentinels: `FeatureDie(x,y,0)`, `0x4233A0(x,y,1)` and `FeatureDie(x,y,1)`. Anything else
      goes to `Weapons[byte]` → `0x4244B0`. So **a weapon with ID 253–255 hitting a feature is
      misread as a sentinel on every other peer**, a stock defect.
    - x and y are 16-pixel cells, bounded against the map by `0x481550`, which returns NULL outside
      it. `0x4244B0` then reads through that NULL at `0x4244CF`, so a malformed `0x0F` can crash
      the receiver.
    - The map's size is copied from the TNT header unclamped (`0x48367D`, `0x483684`). But
      positions are 16.16 `i32` shifted right by 20 (`0x4815A0`), so no reachable cell passes 2047.
  - `0x0B` and `0x0C` carry no weapon ID. COB was not checked.
- Weapon `+0xBC`, which TADR's Delphi layout calls "reserved": no displacement in the array scan,
  no loader store, and only unit pointers at `+0xBC` in the weapon code ranges. Unused, with high
  confidence; about 160 hits elsewhere are unclassified.

**Stock content (READ from the retail archives, counts only).** 195–198 unique weapons across
`totala1.hpi`, `rev31.gp3`, `ccdata.ccx`, `btdata.ccx` and five root `.ufo` files. Every section
has `ID=`, **the highest ID is 246**, and no ID or name is duplicated. **ID 0 is used once, by
`NOWEAPON`**, so it cannot be refused. No weapon uses 253–255.

**TADR's mechanism (SRC).**

- **The overflow array.** A hook at `0x42E468` replaces EAX for IDs in [256, 4096), so the stock
  arithmetic lands in a heap array. The array's distance from `&Weapons[0]` is aligned to an exact
  multiple of 0x115, and the whole trick depends on `main` not moving. **Corrected 2026-09-24:**
  this section used to say it works "because 0x115 is odd and so invertible"; the alignment is what
  it relies on.
- **Three more hooks:**
  - `0x49E5F3`, the name-not-found tail;
  - `0x42E310`, the wipe, which clears the overflow array;
  - a post-parse hook that stamps every overflow slot's ID byte `0xFF`.
- **`WeaponFiredExt`.** A hook at `0x451DF0` recovers the weapon from the shooter and `slot & 3`.
  For IDs of 256 and up it broadcasts a 65-byte `CHAT_05` (`05 00 2E`) carrying a `u16` ID and the
  whole 36-byte `0x0D`, and suppresses the original. Its receiver substitutes EDX at `0x49D27E`.
- **Defects (DIS+SRC; runtime effects INF):**
  - the model path writes an overflow weapon's model into `Weapons[255]`;
  - overflow slots are never freed;
  - ID < 0 is unhandled;
  - ID ≥ 4096 lands in slot 0, over the no-weapon entry;
  - meteor packets carry a garbage shooter, so resolving it reads outside the unit array;
  - `slot & 3` aliases our extra slots;
  - `0x0E` keys every overflow weapon as `0xFF`;
  - `0x0F` sends `0xFF` for one, so remote peers reclaim the feature instead of damaging it.
- **Peers.** A stock peer sees an empty chat and drops the fire.

**SIM + the wire.** Our extra-weapons work's `CRC_weapons` guard folds the per-file CRCs of the
weapon TDFs, `ID=` lines included, so it is independent of the ID's width. Our render and packet
code follows weapon pointers only. `OFF_WEAPON0` in `tagpu_weapons.c` follows the array. Two of the
five sender reads (`0x49DAD8`, `0x49DCAB`) sit inside existing extra-weapons splices.

## 10. Composite buffer 600² → 1280² (`0x458195`)

**DIS, completed 2026-09-24 for [landing 7](raised-limits.md#the-landings).**

**The frame.**

- `0x458180` (called from the model loader at `0x42D473`, once a level) does
  `push 0x258; push 0x258; push 0x506604; call 0x4B8E00`. It stores the frame at `+0x10` of the
  composite draw context `*(main+0x1437B)`: **one shared scratch frame, not the per-unit
  composite**. Landing 4 corrected this: the unit's own frame is the AABB's size, capped by the
  ring. It is the only such pair in `.text`, and TADR writes all ten bytes blindly.
- `0x4B8E00(name, w, h)` allocates `w·h·2 + 0x18` bytes through `0x4D83B0`. Its header:
  - `+0` `u16` width, `+2` `u16` height;
  - `+4`/`+6` the `s16` hotspot;
  - `+8` the transparent index;
  - `+0x10` the colour plane (base + 0x18), `+0x14` the depth plane (colour + w·h).

  The pitch is the header's width.
- **The header is not the allocation's record.** Every writer overwrites `+0..+8` with a unit's
  box, and nothing ever rewrites `+0x10`/`+0x14`. So `A = [+0x14] − [+0x10]` is the allocation's
  size in pixels, and what bounds a write is **area**, not either axis. Stock `A` is 360 000; at
  1280² it is 1 638 400.

**Four writers, and a fifth write.** All run on the game thread, reached only through DrawUnit
`0x45AC20` → `0x45AE6C` → `0x458810` → the blit `0x459200`. None compares with `A`, so the raise
moves an overrun threshold rather than bounding it.

- **The build-state copy `0x4589C0`.**
  - Called from `0x459608` every frame, for nanoframes, factories with cargo, and units with
    `+0x114` bit 0.
  - It sizes the box to the union of the model's box (`0x458310`), each cargo's box, and the unit's
    own frame, and writes the header at `0x458B8C`.
  - The blit then draws the unit's **body** from the scratch.
  - The cargo merge `0x4B90A0` refuses a negative origin but clips neither right nor bottom. It is
    safe only because the union provably contains each cargo box.
  - Hook: `0x458B87`, `8B 4B 10 F7 D8`.
- **The frame copy `0x45A470`.**
  - Called from `0x459338`, `0x4594DB` and `0x45958C`.
  - It copies the unit's own frame header and `w·h` bytes of each plane. The blit draws only the
    silhouette shadow from it.
  - Hook: the entry, `8B 44 24 04 53`.
- **The shadow build `0x45A790`.**
  - Called from `0x4592FE` and `0x45955B`, once for a completed structure, then cached in
    `Object3do+0x14`.
  - It clears `w·h` taken as 32-bit values while the header gets the 16-bit ones.
  - **The fifth write:** `0x4B9E60` compresses the colour plane **into the depth plane**
    (`0x45A85B`). At most `2h(w+1)` bytes, so the bound there is `2h(w+1) ≤ A`.
  - Hook: `0x45A7B9`, `8B 4D 10 66 8B 54 24 10`.
- **The 2× structure bake in `0x459830` / `0x459C70`.**
  - Taken for anti-aliasing (which the settings store pins on), the structure bit, and a nonzero
    mode.
  - The header is the unit's frame × 2 through a 16-bit `shl`, and it clears `4·w·h`.
  - Hooks: `0x459875` (`85 DB 0F 84 96 00 00 00`) and `0x459CB5` (`85 C0 0F 84 9A 00 00 00`),
    whose `je` targets are the engine's own 1× path.

All 15 references to `main+0x1437B` were read: besides these, only the constructor, the teardown
`0x4581C0`, and the ring's flush and free (`0x437C80`, `0x437C90`). Every other function called on
the scratch takes its size from the header.

**What sizes the box.** A box is the model's extent in world units, 1 unit = 1 pixel, and zoom
never enters. The ring (`ctx+0..+0xC`) is `page_round(2·W·H·1.3·f)` with `f = min(RAM_MB/16, 5)`
(`0x42D3E9..0x42D466`), 13·W·H at most. `0x437A30` refuses only a block larger than the whole ring,
so nothing keeps a box within `A`. 3DO vertices and COB `move` are unbounded, so no static proof
exists.

**Stock boxes**, computed from the retail 3DOs (all pieces at rest, the worst heading):

- The largest 1× is `cordev1`/`armdev1`, 184×239 = 43 976 pixels: 12 % of stock `A` and 2.7 % of
  1280².
- The largest 2× structure is 4 × 35 708 = 142 832 pixels: 40 % and 8.7 %.

**What each lane sees.** Only GDI presents the scratch. On Vulkan it reaches only the golden
source, and nothing of ours reads it on the render thread.

## 11. Simultaneous sounds 8 → 128 (`MixingBuffers`)

**DIS.** There is no byte patch: `0x42FE4F` in the registry loader reads REG `MixingBuffers`
(`0x4B69D0`, default 8) into the sound object `+0x2C` (`0x4CF210`, no clamp); `0x4310AB` saves it.
TADR only writes the registry value, from its Delphi launcher (`src/Launcher/settings.pas:520`).

**AUDIO only, per process.** Our impure.cfg store already observes this exact loader (`0x42F9A0`, in
the Visuals landing), so this is a store key.

**The engine's table is the bound, and it is 32** (landing 4, DISASSEMBLED). The sound object
tracks playing sounds in 32 slots (`+0x38` buffer, `+0xB8` sequence, `+0x138` looping flag;
every loop `cmp 0x20`). The play `0x4CF570` evicts while `+0x30 >= +0x2C`, then takes the first
empty slot, and with none returns without tracking the sound. So past 32 the eviction never fires
and the 33rd sound plays untracked, where the stop-all `0x4CF150` cannot reach it; a looping one
plays on. The eviction `0x4CF180` skips looping sounds and reads slot 32, past the table, when it
finds no victim, so a value below 2 is unsafe too. TADR's 128 and the "≥ 33 = unlimited" of the
community patch's notes both describe that untracked play. The store key is 32 by default and
one of 8, 16, 24 or 32.

## 12. Wreck records 2048 → 8192 (not a TADR limit; landing 6)

**DIS.** TADR does not touch this pool; no site of its source (`dcff5dd`) is near `0x421F20` or
`0x423550`. The pool at `main+0x1420B` is allocated for a level by `0x421F20`: `push 0x18000`
(`0x421F2A`), `mov ecx,0x6000` for the clear (`0x421F41`), the free-list loop's end `cmp eax,0x18000`
(`0x421F7A`) and the last record's next link at `[eax+0x17FD0]` (`0x421F97`). Every allocator's
"no record" is the count itself: `0x4232A0` (`0x4232B9`), the burn start `0x4233A0` (`0x42340E`,
`0x42343A`), `FeatureDie 0x423550` (`0x42361E`, `0x42364D`) and `SpawnFeatureOnMap 0x423C50`
(`0x423DBA`, `0x423DE1`). The links are signed 16-bit (`0x4232F0`), so the ceiling is 32 767.
Nothing else sizes the pool; the engine map's *The wreck pool* has the sweep.

**SIM.** A record is what lets a corpse exist and a GAF feature die or be reclaimed properly, so the
size is simulation state. An empty pool refuses corpses and, in stock, leaves a paid-for feature
standing — the defect the always-on fix at `0x423651` closes (the engine map, *Engine defects we
patch*).

**MEASURED.** The previous build's pool took exactly 2048 one-cell corpses, this build's exactly
8192 (scenario applies until the engine refused). 8192 is what the frame packet's wreck table holds
inside its 20 MB reserve at stock's worst wreck model, 19 pieces (`armscab_dead`; 265 of the 285
3DO features are one piece): the raise costs no address space. Ten peers at 1499 units each,
fighting on Town & Country for about 23 minutes of game time, held 5217 to 5268 records at the end,
read with every peer paused — two and a half stock pools, and 36 % of this one left free.

---

## Safety review of TADR's approach (against *Fixes must be safe by construction*)

- **Validation.** `EngineLimits` checks every site's stock bytes before writing (all-or-nothing, with
  rollback). The SEH `MemoryEquals` probe is harmless: the pages are the exe image, always mapped.
  **`LimitCrack` (units, pathfinding, unit types, SFX, composite) writes blindly:** `SingleHook::Hook`
  → `MemWriteWithBackup` with no expected-bytes compare (`hook/Hook.cpp:40-47`). On a non-3.1c exe it
  corrupts code silently. Our `patch_bytes` (`tagpu_patches.c:34`) already has the right shape.
- **Uninstall at DETACH.** `EngineLimits::Uninstall` and `delete NowCrackLimit` (`ddraw.cpp:338`)
  restore code bytes at process detach. Under ExitProcess every other thread is already gone, so it
  is not a race. It still buys nothing and points restored code back at pools the game no longer
  uses. Recommend process-lifetime patches with no uninstall.
- **The naked stack probe** (`ProbeProjectileCleanupStack`) is correct by construction: pages are
  touched at most 0x1000 apart, and `static_assert(PROJECTILE_CLEANUP_STACK_BYTES == 0x2EF0)` ties it
  to the limit. An alternative with no asm: replace `0x49AE20` with a C function over static arrays.
  It is safe because it is not reentrant and runs on the game thread only. INF: the two callers of
  `0x49AE20` must be checked to be game-thread.
- **Fail-closed.** `AbortIfInstallFailed` shows a MessageBox and exits, so a player whose patch
  failed cannot join a game with different limits. This is TADR's whole MP-safety argument.
  **There is no handshake:** nothing puts the limits on the wire except the unit limit, which the
  engine already broadcasts, and `DataShare->IniCRC`, which the recorder DLL exchanges (SRC,
  `LimitCrack.cpp:47`; the consumer is not traced here).
- **Relocation lifetime.** The relocated explosion, model-effect and aux pools are DLL statics, so
  they live for the process. That is safer than stock's per-level block for any cross-thread reader.
  The published tables still need the level fence only for the pointers inside the records.
- **The EAX-substitution trick (weapon IDs)** rests on 32-bit wrap arithmetic, and on `main` staying
  fixed across `LoadWeaponTdf`. It is by construction, not by timing, but it is opaque.
  An **ID < 0** entry is not handled.

## Multiplayer summary

- On the wire and engine-enforced: **unit limit only** (host → `+0x37EEA`, unclamped on receipt).
- A new wire message: **weapon IDs** (the `0x0D` u8 at `+0x19`, plus `WeaponFiredExt`).
- Everything else rides on "every player runs the same DLL", with fail-closed install as the guard.
- The unit-sync `0x1A` handshake covers UnitDef content, not limits.

## Summary table

| ceiling | sim / visual | per game? | TADR sites | missed sites | our code that must follow | risk |
|---|---|---|---|---|---|---|
| projectiles 300→3000 | **SIM** (damage-bearing object refused at cap) | yes (`0x499A30` from load) | alloc + clear, 10 caps, compaction frame (probe + 3 displacements) | none (61 refs, all `0x12C`, all frames) | `PROJ_COUNT`, `TAGPU_PK_MAX_PROJ`, packet validator, gather clamp | we silently draw 300; MP effect of refused remote projectiles unknown |
| explosions 300→3000 | cap gates **sim-RNG draws** in `0x421700` | reset per level; relocated to a static | 7 base refs + table reload + 2 caps | none | `OFF_NEXPL`/`OFF_EXPL`/`EXPL_COUNT`, `MAX_EXPL`, validator | **we draw no explosions** after relocation; RNG divergence |
| flying pieces 100→1000 | visual (no RNG on the slot path) | per level | 7 base + 6 end + backing + reset | none (the five `this` loads correctly kept) | `VA_PSYS_*`, `MAX_DEBRIS`, validator | **we draw no debris** after relocation |
| aux records 300→3000 | fullness gates **sim-RNG draws** | per level, relocated to a static | 2 allocators replaced, C re-init | none | none (pointer-followed) | TADR's "visual-only" comment is wrong on count |
| units/player 500→1500 | **SIM**, host-broadcast | per game | 3 immediates + `0x44CAFE` | **two `maxunits` paths (`0x432646`, `0x436037`), unclamped**; **`0x44CAFE` is a per-type sentinel (101), a real cap after a cancelled restriction menu** | design slots, 15 001 since landing 2; scenario `OFF_LIMIT` | beyond the design point: truncation |
| pathfinding 1333→66650 | sim, owner-local (INF) | per game (map init) | 1 dword, blind | none | none | CPU per tick |
| SFX vector 400→20480 | visual (CRT rand) | vector static; **object pool per process (static init)** | 20 caps + pool ×10 by hook | none | the walk's layer bound (drops whole layers), `MAX_PART`, the effects pass's sprite bucket; all follow since landing 3 | about 15 MB |
| unit types 512→16000 | sim as content | static immediates | 17 (masks, AI frames, ctrl-Z) | none (all 116 `mov r,0x10` classified; ctrl-A/B/C/F only read masks); the separate `CANBUILD` overflow `0x42D971` | `WPN_MAXDEFS 4096`, `mask_has` unbounded, the unit pass's caches | mods only; planned in [A′](content-ids.md) |
| weapon IDs 256→4096 | **SIM + wire** | per level (`0x4918BB`) | 4 hooks + chat-hijack packet | ID < 0 unguarded (stock); `0x0E` and `0x0F` still 8-bit; the model path | `OFF_WEAPON0`, two extra-weapons splices | needs every peer; off in mainline; planned in [A′](content-ids.md) |
| composite 600²→1280² | the unit bake's scratch, written on every lane, presented by GDI | one frame a level, static imm. | 1 × 10 bytes, blind | **four writers never compare with the allocation** (the build-state copy, the frame copy, the shadow build, the 2× bake), and the shadow's compression writes a fifth | none | 3.28 MB a level; the bound is [landing 7](raised-limits.md#the-landings) |
| MixingBuffers 8→128 | audio | per process (registry load) | none (launcher writes REG) | **the engine tracks 32; past it a sound plays untracked** | the impure.cfg store's loader observer, bounded to 32 | none |
| wreck records 2048→8192 (**not TADR's**) | **SIM**: corpses, and features dying or reclaimed | per level (`0x421F20`) | none — TADR leaves it | — | `WR_COUNT` ×2, `TAGPU_PK_MAX_WRECKS`, `PK_RESERVE`, `TAGPU_PD_MAXHAND` | a full pool refuses corpses; stock's paid-for feature left standing is fixed at `0x423651` (§12) |

**Notes this pass corrected**, both in landing 1: `tagpu_packet_pub.c` said "twelve more" `0x190`
sites where there are twenty, and the engine map now records that the explosion cap and the debris
records' fullness gate sim-RNG draws.

**Settled since, by landing 1** [MEASURED 2026-09-23]: both callers of `0x49AE20` are game-thread,
and the stack probe was taken anyway. Two peers on the same build agree past every effect cap: paused,
they held the same units at identical positions ([the engine map](../exe-reverse-engineering.md),
*The raised effect pools*). What a peer on a *lower* cap than its opponent does with a refused
remote projectile (§1) is still unmeasured; it is outside the same-build contract.
