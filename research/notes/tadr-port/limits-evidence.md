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

**DIS.**

- Category bitmasks are 0x40-byte (512-bit) heap blocks, made in `0x488CC2` (`push 0x40; call
  0x4B4F10`, cleared with `mov ecx,0x10`), keyed in the map at `0x51E6B0`.
- TADR widens:
  - that allocation and its clear count;
  - the OR loop in `0x488E3D` (`mov edx,0x10`);
  - two AI parse procs' stack masks (`0x406DB5…0x406E3A`, `0x406E45…0x406ED6`, frame and
    argument displacements, clear counts `0x406DBD`/`0x406E51`);
  - ctrl-Z `0x48BE08/0x48BE21/0x48BF1E`.
- I checked the other 64-byte allocation (`0x48E392`: an object with a vtable, not a mask) and all
  97 `mov e?x,0x10` in `.text`. The ones in `0x489…0x48C` (`0x489BDF`, `0x48A38A…0x48A6D4`,
  `0x48CE39/49`) are shift counts for the 64-bit helpers `0x4E43D0`/`0x4E44F0`, not mask loops.
  `0x40BF49…0x40C10D` and others in `0x40xxxx` (AI) were **not** individually classified.
- **Open:** ctrl-A/B/C are listed under TADR's §B fixes ("truncation of IDs ≥ 512"), so they are
  masks this item does *not* widen. Settle it by classifying the remaining AI-range `mov ecx,0x10`
  sites before trusting it.
- No loader cap on the type count was found (no `cmp …,0x200` in the loader). Stock simply
  overflows the 64-byte masks past 512 types.

**SIM-relevant as content** (AI build targeting, categories). Static immediates; any time before
use is fine.

**Our code:**

- `src/tagpu_weapons.c:30` `WPN_MAXDEFS 4096`: refuses and logs types beyond, gracefully.
- `src/tagpu_cat.c:47-51` `MAX_DEFS 16384`.
- `src/tagpu_native.c:99` / `tagpu_packet_pub.c:297,427`: the `UNITINFOCount` bound follows the
  engine's own count, so it needs nothing.

## 9. Weapon IDs 256 → 4096 (`WeaponIdOverflow` + `WeaponFiredExt`, off by default)

**DIS.**

- `0x42E463` reads `ID` with default −1 (`0x4C46C0`). Then, with **no bound either way**,
  `ebp = main + id·0x115 + 0x2CF3` (`0x42E46E…0x42E489`; the stride arithmetic works out to 277).
- `Weapons[256]` ends at `0x2CF3 + 0x11500 = main+0x141F3`, **exactly the projectile
  count/pointer**. So ID ≥ 256 corrupts the projectile pool header, as TADR says. **ID < 0** (a
  weapon with no `ID=`) writes below `main+0x2CF3`, which TADR does not mention.
- The name lookup loops 256 (`0x49E5EB cmp esi,0x11500`).

**TADR's mechanism (SRC).**

- The hook at `0x42E468` substitutes EAX so that the stock address arithmetic lands in a heap
  overflow array. It works because 0x115 is odd and so invertible mod 2³², and it depends on `main`
  not moving during a load.
- It hooks the "name not found" tail `0x49E5F3` and the load wipe `0x42E310`.
- On the wire, `0x0D WEAPON_FIRED` carries the weapon ID as **one byte at payload `+0x19`** (SRC:
  `WeaponFiredExt.h`, not re-disassembled here). `WeaponFiredExt` re-sends the whole 36-byte `0x0D`
  plus a u16 ID inside a hijacked 65-byte `CHAT_05` (msgId `0x2E`).
- Unpatched peers see an empty chat and **silently drop the fire event**.

**SIM + a new wire message.** Our extra-weapons work's `CRC_weapons` guard covers weapon *slots*,
not IDs.

## 10. Composite buffer 600² → 1280² (`0x458195`)

**DIS.**

- `0x458180` (called from the model loader at `0x42D473`, once a level) does
  `push 0x258; push 0x258; push 0x506604; call 0x4B8E00` and stores the frame at `+0x10` of the
  composite draw context `*(main+0x1437B)`: **one shared scratch frame, not the per-unit
  composite** (landing 4 corrected this; the unit's frame is the AABB's size, capped by the ring).
- It is the only such pair in `.text`. TADR writes all ten bytes blindly.
- Four writers in the blit (the build-state copy `0x4589C0`, the frame copy `0x45A470`, the shadow
  build `0x45A790`, the 2× structure bake in `0x459830` / `0x459C70`) size it to a unit and never
  compare with the allocation, so the raise moves an overrun threshold
  rather than bounding it. The writers run on every lane (landing 4 read its header after a Vulkan
  fight), so this is memory safety everywhere, and only GDI presents the result.
- The engine map's *The composite scratch frame* has the addresses and the measurements.

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
| unit types 512→16000 | sim as content | static immediates | 17 (masks, AI frames, ctrl-Z) | ctrl-A/B/C (in §B); AI-range `0x10` sites unclassified | `WPN_MAXDEFS 4096` refuses | mods only |
| weapon IDs 256→4096 | **SIM + wire** | load-time hook | 3 hooks + chat-hijack packet | ID < 0 unguarded (stock) | `OFF_WEAPON0` users; `CRC_weapons` does not cover IDs | needs every peer; off in mainline |
| composite 600²→1280² | the unit bake's scratch, written on every lane, presented by GDI | one frame a level, static imm. | 1 × 10 bytes, blind | **four writers never compare with the allocation** (the build-state copy, the frame copy, the shadow build, the 2× bake) | none | 3.28 MB a level |
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
