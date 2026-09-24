# A. Raised ceilings — the plan

## Summary

Nine of the engine limits TADR raises come into our stack, as one module over five landings. The
owner decided every choice below on 2026-09-23 **[DECIDED]**. **Landings 1 and 2 are done**
(2026-09-23): the effect pools, the module, the failure report and the stock-limits build; then the
unit limit, both `maxunits` keys, the restriction menu's sentinel, the pathfinder's budget and the
render design point. The module is
`tagpu_limits.h` and the limits block of `tagpu_patches.c`, which keeps it out of the thread-split
allow-list; how it works is [gpu-status §2.6b](../gpu-status.md). The disassembly behind each fact is in [the evidence pass](limits-evidence.md),
and the rules shared by every group are in [the port overview](overview.md#standing-rules-decided-2026-09-23).

Three findings shaped the plan:

- **Explosions and debris are not purely visual.** A death draws from the simulation's synced
  random numbers only while both pools have a free slot, so their size changes simulation outcomes.
- **Two pools move.** Explosions and flying pieces are relocated into new memory. Until our frame
  packet reads the new location, **we draw neither**, with no error.
- **The particle ceiling is two limits, not one:** 400 objects a layer, and a pool of 1000 objects
  in total. The pool is built by a C-runtime static initialiser.

## Scope and values

| Limit | Stock | Ours | What it touches | Landing |
|---|---|---|---|---|
| projectiles | 300 | 3000 | **simulation**: at the cap a shot and its damage are refused | L1 |
| explosions | 300 | 3000, moved to DLL memory | gates the synced random numbers in `0x421700` | L1 |
| flying pieces | 100 | 1000, moved to DLL memory | visual (no random-number draw depends on it) | L1 |
| debris records | 300 | 3000, moved to DLL memory | gates the synced random numbers in `0x421700` | L1 |
| units per player | default 250, clamp 500 | **1500, default and ceiling** | **simulation**; in a network game the host's value applies | L2 |
| pathfinding budget | 1333 | 66 650 | simulation, per owner (inferred); CPU per tick | L2 |
| particles | 400 per layer, 1000 objects | **measured** in the tier-1 battle | visual (C-runtime `rand` only) | L3 |
| sounds (`MixingBuffers`) | 8 | 128 | audio; a registry value, not a patch | L4 |
| composite buffer | 600² | 1280² | visual, and only in the GDI lane (Vulkan never shows it) | L4 |

**Deferred to their own plan:** unit-type IDs (512 → 16 000) and weapon IDs (256 → 4096). See
[the overview](overview.md#the-groups).

**The values** are TADR's, with two exceptions. Units are set to 1500 as both default and ceiling:
the owner's call ("go all in"), and TADR's shipped `totala.ini` value. Particles are measured,
because TADR's 20 480 per layer is a 51× jump that nobody has costed against our frame packet.
TADR's `EngineLimits.cpp` (the four pools) is one month old (`586d71a`, 2026-08-22); its
`LimitCrack.cpp` (units, pathfinding, particles, composite) dates from 2014.

## Decisions

**Multiplayer: TADR's behaviour.** In a network game each peer takes the host's unit limit, and we
follow it. Our own reads stay bounded. Past the frame packet's table ceiling (16 384 slots, 1638 a
player) the renderer draws what fits and logs the truncation. **One bound was added in landing 2:**
the game start's store of the host's value (`0x4973B5`) goes through the same [20, 1500] clamp as
the two `maxunits` keys. A same-build host sends at most that anyway, since its slider stops at
`ActualUnitLimit − 20`, so what is followed does not change. What the clamp closes is a DirectPlay
lobby launch, which writes `ActualUnitLimit` unclamped (`0x449D9B`): past 6553 a player the engine's
16-bit slot count `10·N + 1` wraps and the unit array is allocated too small. Every peer runs the
same clamp, so they agree.

**Unit limit: 1500 default and ceiling.**
- The three immediates at `0x491640` (default), `0x491659` (compare) and `0x491666` (clamp-to)
  become 1500. A `UnitLimit` in `totala.ini` `[Preferences]` can still lower it, down to the
  engine's floor of 20.
- Both `maxunits` keys stock stores unclamped are clamped to [20, 1500]: a saved game's
  `[Summary]` (`0x432646`) and a map's `.ota` `[GlobalHeader]` (`0x436037`, which writes the
  array's count directly and is what a campaign mission plays with).
- The render design point becomes 10 × 1500 + 1 = **15 001 slots** (`TAGPU_PK_DESIGN_SLOTS`), and
  every cap [gpu-status §2.86](../gpu-status.md) sized against it follows.
- `tacli`'s scenario schema ceiling `SCN_MAX_LIMIT` goes from 500 to 1500, and
  `scenario-format.md` says so.
- The restriction menu's "no limit" sentinel, 101 at `0x44CAFE`, becomes the unit limit, as TADR
  writes it: a cancelled menu writes the sentinel into the game as a real per-type cap
  (*Corrections*, below).

**One module, one table, one rule.** The limits block holds one site table: address, expected
bytes, replacement, name. It checks every entry, then writes all of them or none.
- It runs at `DLL_PROCESS_ATTACH`. `ddraw.dll` is `TotalA.exe`'s first static import
  (`dllmain.c:221`), so it runs before the exe's C-runtime static initialisers. The particle pool
  (`0x471C80`) needs that ordering; TADR's hook at `0x471C87` relies on the same loader rule.
- The visual-only sites (particles, composite) sit in the same table under the same rule. On a
  non-3.1 exe the simulation sites fail anyway.

**Moved pools live in DLL statics.** Explosions, flying pieces and debris records live for the
whole process, which is longer than stock's per-level block. That is the better lifetime for a
cross-thread reader. The frame packet reads the moved arrays directly, bounded by the same
compile-time constants that size them.

**The debris allocator is first-free, as stock's is.** TADR's round-robin keeps a position that
persists across games. Two peers with different game histories would then start from different
slots, and below the cap the order would differ from stock. A first-free scan over 3000 slots
costs little and keeps stock's assignment exactly.

**The projectile compaction frame** `0x49AE20` grows from `0x4C0` to `0x2EF0` bytes of stack,
committed by a stack probe a page at a time, as TADR does it. Correct by construction whatever
calls it. A C replacement over static arrays was the alternative, safe only because both callers
(`0x49BE53`, `0x49C8F4`) are on the game thread and the function is not reentrant; landing 1
checked both callers and took the probe, which needs neither fact.

**No runtime opt-out.** `make LIMITS=stock` builds `ddraw-stocklimits.dll` for the comparisons,
copied over an instance's `ddraw.dll` and launched with `tacli --keep-dll`. No lever file and no
store key exist for it.

## The failure report

If any site differs, the DLL writes nothing. At the first DirectDraw call (after the loader lock,
before the game window exists) it shows this MessageBox and exits. The same text goes to
`log\startup-failure.txt`. Ctrl+C copies a MessageBox's text on Windows and on Wine. The text is one
line per paragraph, so the box wraps it once; the owner reviewed it as it shows in the game.

```text
Title: Total Annihilation: Impure cannot start

Impure could not install its engine limits, so Total Annihilation will now close. Nothing was changed.

WHY
Impure raises the game's limits (units, projectiles, explosions...) by rewriting its code in memory, and it checks every place first. <why> Running anyway would let this game play by different rules from other players and break multiplayer without warning.

WHAT TO DO
- Use the original 3.1 TotalA.exe (the Steam copy is 3.1). Community patches such as 3.9.02 and TA: Escalation ship a modified exe.
- Or report it: press Ctrl+C to copy this message and paste it into a new issue at
github.com/Code-Herder/ta-impure-patch/issues
The same report is saved in log\startup-failure.txt

--- report ---
impure <commit> (<branch>)
exe TotalA.exe, <size> bytes
md5 <md5>
PE stamp <stamp>, known build: <name or none>
result: <k> of <n> sites differ, nothing written
<address> <site name>
  want <stock bytes>
  have <found bytes>
```

- **`<why>` has three forms**: an exe that is not 3.1; the 3.1 exe, changed in memory before we ran
  (another patch or loader); and Windows refusing the write.
- **The report block exists for whoever debugs it, person or agent.** It names our build and
  identifies the exe exactly. It lists every differing site, twelve in the box and all of them in
  `log\tagpu.log`. It prints no path, because a report is meant to be pasted publicly and an
  install path carries a user name.
- **`known build`** is looked up from a compiled-in table of md5s, which holds retail 3.1 alone.
- The draft said "the Steam and GOG copies are 3.1". The GOG copy was never checked, so the text
  names Steam only.

## The landings

**Landing 1 — the effect pools, the module and the failure report. Done 2026-09-23.**
Projectiles, explosions, flying pieces and debris records, 43 sites; the frame packet follows the
moved pools; the stock-limits `make` flag. What it proved, by running it:
- **Above the caps it works.** `scenarios/limits-flood.json`: the packet carried 687 projectiles,
  2439 explosions and 540 flying pieces, the same projectile count the engine held at that moment,
  with no table truncated.
- **Two peers agree.** A two-instance network game, each peer applying its own half
  (`limits-mp-west`, `limits-mp-east`): both passed every stock cap (projectiles 689 on each,
  explosions 1770 and 984), and when paused with the game's `Pause` both held the same 394 units at
  identical positions. A second fight of 630 units matched the same way.
- **It fails closed.** An exe copy with one cap byte changed got the dialog, the report file and
  the exit, with nothing written.
- **Below the caps: by construction, not by a trace.** The plan asked for a `tagpu_cobtrace`
  comparison with the stock-limits build. It cannot be run: two runs of the *same* stock build do
  not reproduce a fight past its first impact, even with both random seeds and the apply tick
  pinned (the engine map, *the engine's rates*), so the comparison would measure noise. The owner's
  point that TA replicates state rather than running lockstep settled it: what multiplayer needs is
  the two-peer agreement above, and what "stock below the caps" needs is that each rewritten site
  does what stock did. Two landing reviewers checked all 43 sites against the image and found none
  wrong.
- **One exception to "stock below the caps", by design.** The flying pieces' backing ring
  (`0x437A30`) is ten times larger, and a ring evicts by bytes, not by slots: stock can drop a
  piece with fewer than 100 alive, the raised build keeps it. The piece lands and adds its
  explosion, so the explosion count and the C-runtime `rand` stream differ from stock; the
  simulation's generator is reached only if the explosion pool fills. A tenfold slot count needs
  the tenfold ring, so this is the price of the raise, and TADR pays it too.
- **Not closed: the explosion tick's cost.** Its compaction (`0x4210E6`) is O(dead × live), about a
  hundred times stock's worst tick at 3000 records if a mass death expires together. Landing 2's
  tier-1 battle measures it.

**Landing 2 — units 1500, the `maxunits` clamps, the restriction sentinel, pathfinding 66 650 and
the design point 15 001. Done 2026-09-23.** Fifty-two sites now. What it proved, by running it:
- **The limit is what the engine holds.** With no `UnitLimit` key the game reads 1500; in the
  four-player skirmish `+0x37EEC` and `+0x37EE6` were 1500 and the unit array 15 001 slots. The
  two clamp stubs were read back out of the running process and disassembled.
- **Tier 1**, `scenarios/limits-tier1.json` (6000 kbots in a four-way melee on Town & Country,
  the shipped defaults, speed 20): all 6000 units and orders applied; the packet peaked at 5983
  units and 88 696 pieces, 2.96 MB of its 20 MB reserve, with no table truncated after the load's
  growth. Peaks: projectiles 215, explosions 1042, flying pieces 441, 755 particles in a frame.
  **The simulation held 57–60 ticks a second**: at these counts neither the 66 650 budget nor the
  explosion tick's quadratic compaction stalled it. The GPU frame stayed under 1 ms at p50. **The
  game thread published a median of 36 frames a second (21–59)**; how that frame splits between
  the engine and our publisher is not measured, because the publisher's histogram tops out at
  512 µs.

**Landing 3 — particles: the per-layer cap and the object pool.** The value is set from tier 1's
per-layer peaks plus headroom; `LAYER_OBJCAP`, `TAGPU_PK_MAX_PART` and `PART_SUBCAP` follow.

**Landing 4 — `MixingBuffers` 128 and the composite buffer.** `MixingBuffers` as an `impure.cfg`
key (read at `0x42FE4F` inside the registry loader `0x42F9A0`, which the store already observes);
the composite buffer proved by a GDI-lane comparison.

**Landing 5 — ten players.** `mp_lobby.sh` extended to N instances. Proof, tier 2: a 10-player
network game at 1500 each (15 000 units), the proof of the 15 001-slot design point, checked the
way landing 1 checked two peers: every peer applies its own units, then a paused roster on each.

L1 came first because it forced the module, the report and the packet changes into existence.
L2 comes before L3 because the particle measurement needs the raised unit limit.

## Open questions

- Whether a refused remote projectile changes damage on that peer
  ([evidence §1](limits-evidence.md#1-projectiles-300-3000-enginelimitscpp-addprojectilepatches)).
  Landing 1's two-peer check covered the same-build case, which is the contract.
- The particle headroom rule: decided in L3, from the data.
- The explosion tick's compaction near its 3000-record cap: tier 1 reached 1042 explosions without
  a stall; a denser fight could still find one.
- How the game thread's frame at 6000 units splits between the engine and our publisher.

## Corrections this plan made

- **`0x44CAFE` is not a unit-limit site** (`mov ecx,0x65`: 101 into the per-type battleroom table,
  the restriction menu's "no limit"). Corrected in [deep-tadr](../deep-tadr.md) on 2026-09-23.
  **But the sentinel is a real cap on one path** (landing 2, DISASSEMBLED): Cancel in the
  restriction menu writes the saved values, 101 included, back into the restriction store, the game
  copies them to `UnitDef+0x15A`, and the unit constructor `0x485F50` refuses a type's 102nd unit.
  A default network game never takes that path (landing 1's peers each created 450 of one type).
  The site is written as TADR writes it; the engine map, *The per-player unit cap*, has the chain.
- **There are two unclamped `maxunits` keys, not one.** The evidence found the saved game's
  (`0x432646`); landing 2 found the map's `.ota` (`0x436037`), which writes the array's own count.
- **The host's limit reaches the unit array at `0x4973B5`, not `0x449D9B`** (landing 2's review).
  `0x449D9B` is a lobby launch writing `ActualUnitLimit`, which bounds the host's slider; the game
  start copies the host record's word into the array's count. Landing 2 clamps that store.
- **The pathfinder's budget is spent a tick and shared among the players** (`0x40EB70`), its init
  is `0x40E9E0`, and `0x5119E8` is a ten-dword table, not one with the players' stride (landing 2's
  review).
- **`0x4912F5` is the process init, not a single-player one**, and runs before the ini read, so the
  game start is what carries `UnitLimit` into the array's count (landing 2's review).
- `tagpu_packet_pub.c` said "twelve more" particle cap sites; there are twenty (landing 1).
- The evidence named `0x420E50` as the flying-piece spawner; nothing calls it. The live spawner is
  `0x481140`, and the conclusion (the slot cap gates no simulation draw) holds through it. A piece
  explosion draws the simulation's generator eight times, not six (landing 1's review).
- The engine map said the effect arrays are simulation state that the draw passes only read, and
  did not record that the explosion cap and the debris records' fullness gate synced random-number
  draws. It does now (landing 1).
