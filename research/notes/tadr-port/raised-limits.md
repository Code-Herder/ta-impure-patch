# A. Raised ceilings — the plan

## Summary

Nine of the engine limits TADR raises come into our stack, as one module over five landings. The
owner decided every choice below on 2026-09-23 **[DECIDED]**. **All five landings are done**
(2026-09-23 to 2026-09-24): the effect pools, the module, the failure report and the stock-limits
build; the unit limit, both `maxunits` keys, the restriction menu's sentinel, the pathfinder's
budget and the render design point; the particles; sounds and the composite scratch frame; and ten
players in one network game. **A sixth landing goes beyond TADR**: the wreck pool, raised and made
safe when full, because the raised unit limit fills it — [Fixed beyond TADR](#fixed-beyond-tadr).
The module is
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
| pathfinding budget | 1333 | 66 650 | simulation: a budget a tick, shared among the players; CPU per tick | L2 |
| particles | 400 per layer, 1000 objects | 20 480 per layer, 204 800 objects, **checked against** the tier-1 battle | visual (C-runtime `rand` only) | L3 |
| sounds (`MixingBuffers`) | 8 | **32**, a store key (8, 16, 24 or 32) | audio; a registry value, not a patch; the engine tracks 32 | L4 |
| composite scratch frame | 600² | 1280² | the unit bake's shared scratch, written on every lane and presented only by GDI's | L4 |
| wreck records | 2048 | **8192** — not a TADR value | every 3DO wreck, and every GAF feature playing its death or reclaim sequence; **simulation**: a full pool refuses corpses | L6 |

**Deferred to their own plan:** unit-type IDs (512 → 16 000) and weapon IDs (256 → 4096). See
[the overview](overview.md#the-groups).

**The values** are TADR's, with two exceptions, and one limit TADR does not raise at all (the wreck
pool, below). Units are set to 1500 as both default and ceiling:
the owner's call ("go all in"), and TADR's shipped `totala.ini` value. Sounds stop at 32, not
TADR's 128, because the engine's table of playing sounds has 32 slots and past it a sound plays
untracked (landing 4). Particles were measured
before they were set, because TADR's 20 480 per layer is a 51× jump that nobody had costed against
our frame packet; tier 1 put one layer at 13 529 objects, so TADR's value stands (landing 3).
TADR's `EngineLimits.cpp` (the four pools) is one month old (`586d71a`, 2026-08-22); its
`LimitCrack.cpp` (units, pathfinding, particles, composite) dates from 2014.

## Fixed beyond TADR

Issues this port found and fixed that TADR does not: an engine defect the raised limits made easy
to reach, and one found beside it that any game reaches. TADR, which raises the same limits,
carries both unchanged (checked against its source as of `dcff5dd`, 2026-09-20).

| Issue | What a player saw | Fix | Landing |
|---|---|---|---|
| **A reclaimed feature is paid for and left standing** | In a big battle, reclaiming a building on Town & Country paid its metal but left the building, and it could be reclaimed again, as often as the builder repeated it. A destroyed building did not change either. | The wreck pool is raised from 2048 to 8192 records, and when it is full the feature is swapped at once instead of not at all | L6 |
| **A building reclaimed by a group is paid for once per builder** | Several builders ordered together onto a Town & Country building were each paid its full metal. Nothing looked wrong: the building went once. | The reclaim's "already being reclaimed" test reads the building's anchor cell, the one the engine marks, instead of the cell the builder aimed at | L6 |

**The defect** ([the engine map, *The wreck pool*](../exe-reverse-engineering.md#the-wreck-pool-and-a-feature-paid-for-and-left-standing)). The engine keeps 2048 wreck records for a
level. A 3DO wreck (every unit's corpse, every heap) holds one for as long as it lies there, and a
GAF feature such as a Town & Country building holds one while it plays its reclaim or collapse
sequence. `FeatureDie 0x423550` starts that sequence; with no free record it returns having done
nothing. Its callers have already acted: the reclaim `0x4237D0` has paid the feature's metal, and
in a network game it tells every peer, whose own `FeatureDie` does the same nothing. Stock needs
2048 corpses on the map at once to get there; ten players at 1500 units do it in the first minutes
of a battle. In the owner's ten-player game the pool was full on each of the three peers read.

**The fix, in two parts.**

- **Always on, in both builds: a full pool swaps the feature now.** The pool-full branch joins the
  engine's own path for a feature with no sequence (`0x4236EF`), so a feature that is paid for is
  replaced by its successor — the scar for a reclaim, the next damage stage for a collapse — exactly
  as one with no sequence is. That path has two exceptions of its own, which stock shares for every
  feature without a sequence: an indestructible feature is not removed, and a successor that is a
  3DO feature needs a record, so on a full pool nothing takes the old feature's place.
  The one thing a full pool still costs is the sequence, and with it the immunity it gives: while a
  collapse plays, the cell is marked and further hits are ignored, so a weapon that hits in frame
  after frame (the D-gun) takes a building one stage per sequence. Without the sequence it can take
  it through several stages at once. Stock with a full pool cannot destroy the building at all.
- **Raised builds: 8192 records.** Not a TADR value; TADR leaves the pool at 2048. The engine's own
  ceiling is 32 767 (its list links are signed 16-bit), and 8192 is what the frame packet's wreck
  table holds inside its existing 20 MB reserve at stock's worst wreck model (19 pieces), so the
  raise costs no address space. What a full pool refuses that the fix cannot give back is new
  corpses: `0x423C50` creates no 3DO feature without a record, so a unit dies without leaving one.

**The reclaim paid twice** ([the engine map](../exe-reverse-engineering.md#the-reclaim-paid-twice)).
Found by landing 6's review, and independent of the pool. While a building plays its reclaim
sequence the engine marks its anchor cell, and a reclaim of a marked building is refused before it
pays — but the refusal tests the cell the builder aimed at, not the anchor. A group ordered onto a
building finishes in the same moment, so every builder that aimed at any other cell is paid in
full. The fix (always on, in both builds) makes the test read the anchor: the second builder is
refused exactly as one aimed at the anchor always was.

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

**Landing 3 — particles: the per-layer cap and the object pool. Done 2026-09-23.** Seventy-three
sites now: the twenty layer compares and the pool's capacity. What it proved, by running it:

- **The headroom rule, and the value it gave.** A cap must hold the largest layer tier 1 reaches
  with half again to spare. A scratch build at TADR's values put one layer at **13 529 objects**
  at the opening volley (the pool's used count 13 571); 13 529 × 1.5 = 20 294, inside TADR's
  20 480, so TADR's value stands. The pool is ten layers' worth, TADR's 204 800, so a layer's
  own cap is what binds.
- **Our side follows it.** The publisher walks a layer to `TAGPU_LIM_SFX + 1`; the packet's
  particle table is 24 576, one and a half times tier 1's 14 510 sub-particles in a frame. The
  scratch run showed a third ceiling nobody had listed: the effects pass's sprite bucket filled
  (65 532 vertices) and refused 169 quads, so the two buckets particles land in are now sized from
  the particle table. `PART_SUBCAP`, a filter on one object's sub-particles, did not need to move.
- **Past the table, thinned rather than cut** (landing 3's review): the walk runs bottom to top, so
  a full table used to lose the top layers whole, and the raise makes a full table reachable. The
  publisher now counts first and keeps the same share of every layer. With the table forced to
  2048 in tier 1: 670 thinned frames, never more than 2047 kept, nothing truncated, every layer
  present in proportion.
- **On the landing's build**, the same fight: 73 sites installed, the pool's capacity read back at
  204 800, 15 964 sub-particles in a frame with nothing truncated, no layer refused, no vertex
  dropped, the smoke drawn, and the simulation at 60 ticks a second.

**Landing 4 — sounds and the composite scratch frame. Done 2026-09-24.** Seventy-five sites now,
and one store key. What it proved, by running it:

- **Sounds are 32, not 128.** The sound object tracks at most 32 playing sounds (`+0x38`, `+0xB8`,
  `+0x138`); `MixingBuffers` is where it starts evicting, and past 32 the eviction never fires and
  a 33rd sound plays untracked, a looping one beyond the stop-all's reach. So `mixingbuffers` in
  `impure.cfg` is 32 by default and one of 8, 16, 24 or 32, pushed into `+0x2C` after every
  registry load, where the loader has just stored the registry's value unchecked. Tier 1 with
  sound on a null device: 32 in use throughout the fight, never more; the store at 8 held 8.
- **The composite scratch is one frame a level, not one a unit**, and four engine writers in the
  blit size it to a unit without comparing with the allocation: the build-state copy, the frame
  copy, the shadow build and the 2× structure bake (the landing's two reviews found the middle
  two). The raise to 1280² moves the size at which they write past it; it is not a bound. The
  frame read back at 1280² on the Vulkan lane, where the copy writes it every frame, and 600² on
  the stock build.
- **The GDI lane draws the same picture**: the nanoframe ladder on both builds differs only inside
  the nanoframes, by exactly as much as two frames of one run a second apart.

**Landing 5 — ten players. Done 2026-09-24.** `mp_lobby.sh` takes a host and up to nine joiners,
and `scenarios/limits-tier2-p0` … `p9` are one army each. Tier 2, on the landing-4 build: ten
instances in one network game on Town & Country (it seats ten), 800 × 600 each, every peer applying
its own 1499 units, the 1500 cap with its commander. What it proved, by running it:

- **The design point holds on every peer.** All ten read a unit array of 15 001 slots and a cap of
  1500; every apply created 1499 of 1499; the peers' own counts peaked at 14 991 to 15 000 alive (the
  fight had begun on the first armies before the last arrived).
- **Every raised pool passed its stock cap on every peer**, sampled from the engine's own counts
  for three minutes: projectiles 731–854 at their peak (stock 300), explosions 2936–2966 (stock
  300, the raised cap 3000), particle objects 12 759–14 331 in use (stock pool 1000), and the
  flying pieces 947–993 in a frame (stock 100). The simulation held **30 ticks a second on every
  peer**, the full rate of a network game's speed 10.
- **The peers agree when paused.** After three minutes of fighting, all ten held the same **7307**
  units in the same engine slots with the same types, and each army the same count on every peer
  (owners renumbered by each peer's own seat). **6998 (95.8 %) stood at the identical position on
  all ten.** The other 309, all moving, sat off their owner's own copy by at most 40 ticks of their
  own top speed (p99 14): a remote unit is where its owner last reported it, and the pause itself
  landed on ticks 6440 to 6455. No peer disagreed about which units exist.
- **A trap it found**: with the lobby's default commander death ("game ends"), one dead commander
  wiped its peer's army on every peer, which looks exactly like a dropped player. The rehearsal lost
  one that way; tier 2 ran with commander death off on every peer (`ActiveCommanderDeath`,
  `main+0x37EF6`, read as 0 on all ten).

**Landing 6 — the wreck pool, beyond TADR. Done 2026-09-24.** Eighty-six sites now, and two
engine fixes in the always-on group ([Fixed beyond TADR](#fixed-beyond-tadr)). Every test fills the
pool for real, with corpses, rather than faking an empty free list: scenario applies of 1024
one-cell `armflea_dead` each, on cells no map feature touches, until the engine refuses. What it
proved, by running it:

- **The pool is the size we say.** The previous build's pool took exactly **2048** corpses and
  refused the 2049th; this build's took exactly **8192** and refused all of the ninth thousand. A
  census of the whole pool, read in one pass and walked offline, found the free list and the two
  in-use lists covering all 8192 records exactly once.
- **The bug, reproduced on the previous build.** With the pool full, a commander reclaimed
  `Building15` (2900 metal): the player's total metal produced rose by 2900 and the building stayed.
  A second reclaim of the same building paid 2900 again.
- **The fix, on this build**, same procedure: paid once, and the cell held `Buildingscar15` (not
  reclaimable) with no record attached while the free list stayed empty — the immediate swap. A
  second reclaim paid nothing.
- **Over the network.** Two peers, both pools full: the host's reclaim swapped the building on
  both, the joiner through the event the host sends; only the host was paid.
- **With free records nothing changed.** On a fresh game the reclaim still ended in the scar, and a
  D-gun shot at a second `Building15` marked its cell (a record held) for 1.2 s, then left
  `Building15b`, as stock does. With the pool full the same shot left the scar: the chained stages
  described in [Fixed beyond TADR](#fixed-beyond-tadr).
- **Our side follows it.** With 8192 corpses on the map, the packet carried 3729 wrecks in one
  frame with no record index refused and no table truncated.
- **How much of 8192 a big battle uses.** Ten peers at 1499 units each on Town & Country, the
  landing 5 fixtures, fighting for about 23 minutes of game time (tick 41 442): **5217 to 5268
  records in use** on the ten peers, the rest free — two and a half stock pools, so stock's was
  full long before, and this one never filled. The census was taken with every peer paused, since
  a read spread over seconds of a live battle tears the lists; each peer's lists covered its 8192
  records exactly once. On every peer no wreck was refused or truncated (`woob=0`, the wreck
  table's truncation 0); the packet's truncations (11 to 14 a peer) all came by tick 825, while
  15 000 units spawned and the slots grew — the load-time case of gpu-status §2.86.
- **The reclaim paid twice, found by the review.** On this build, free records, two commanders
  ordered together onto `Building15`: through its centre cell the player gained +5959 at the first
  payout (twice 2900), through its anchor +3056. With the anchor fix, through the centre: +3056,
  and +3143 over 80 s against the anchor's +3140 — paid once, the building playing its sequence and
  ending as the scar. With the pool full as well, through the centre: +3146 over 82 s, the scar at
  once, the pool still 8192 of 8192 — the two fixes together.

L1 came first because it forced the module, the report and the packet changes into existence.
L2 comes before L3 because the particle measurement needs the raised unit limit.

## Open questions

- Whether a refused remote projectile changes damage on that peer
  ([evidence §1](limits-evidence.md#1-projectiles-300-3000-enginelimitscpp-addprojectilepatches)).
  Landing 1's two-peer check covered the same-build case, which is the contract.
- The explosion tick's compaction at its 3000-record cap: tier 2 held 2936–2966 explosions on
  every peer at the full 30 ticks a second of a network game's speed 10; at single player's speed
  20 (60 ticks) that count is not measured.
- How far a remote unit lags its owner at stock's 500 a player: tier 2's up-to-40-ticks at 1500 is
  measured, stock's is not, so whether the raise stretches the update interval is open.
- How the game thread's frame at 6000 units splits between the engine and our publisher.
- Whether peers disagree about a feature in practice when one pool is full and another is not. The
  engine map shows how they can, in both directions; corpses are made on every peer from the synced
  deaths, so the pools should fill together, but a sequence holds its record for its own length on
  each peer. A full pool still refuses corpses (landing 6).

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
- **The 600² frame is not the per-unit composite's cap** (landing 4). The unit's own frame is the
  AABB's size, capped by the context's ring; 600² is the draw context's one shared scratch. The composite notes and
  the evidence's §10 said otherwise, and so did its claim that the frame is made per object and
  matters only to the GDI lane: the build-state copy writes it on every lane.
- **The bit the 2× bake tests is the structure bit**, not "under construction" (build-state.md §1
  had measured it; its 2× paragraph still said nanoframes).
- **TADR's 128 sounds is past the engine's table** (landing 4).
- The engine map said the effect arrays are simulation state that the draw passes only read, and
  did not record that the explosion cap and the debris records' fullness gate synced random-number
  draws. It does now (landing 1).
