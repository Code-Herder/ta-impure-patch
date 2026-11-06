# Raised limits and the stock engine's defects

The always-on limits block and defect fixes.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note. Section numbers are those of the retired [GPU status](../gpu-status.md) page, which indexes them.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### 2.6b The raised engine limits (`tagpu_limits.h`, the limits block of `tagpu_patches.c`, always on) — the TADR port, landings 1–7, A′2 and A′3

**What it is.** TA's limits, raised the way TADR raises them, as our own C: projectiles
300 → 3000, explosions 300 → 3000, flying pieces 100 → 1000, debris records 300 → 3000, units a
player 250 (default) / 500 (ceiling) → 1500 / 1500, the pathfinder's budget 1333 → 66 650, and
particles 400 → 20 480 a layer from a pool of 1000 → 204 800 objects, the composite
scratch frame 600² → 1280², the wreck pool 2048 → 8192 records, which TADR does not raise
(landing 6; its full-pool defect is §2.6c's fourth fix), unit-type slots 512 → 16 384
([A′2](../tadr-port/content-ids.md)), with the network join's pace that the raise makes slow, and
weapon IDs 256 → 4096 ([A′3](../tadr-port/content-ids.md)), the array moved into the DLL and the ID
widened on the wire. The engine's simultaneous sounds are not a
site: they are the settings store's `mixingbuffers`, held to 32 (below).
Planned in [the TADR port](../tadr-port/raised-limits.md); every site is in the engine map, *The raised
effect pools* (its *Unit-type slots* and *Weapon IDs*) and *The per-player unit cap*.

| What | Sites | Mechanism |
|---|---|---|
| the projectile pool | `0x499A32`, `0x499A56`; ten caps `0x49B6F0` … `0x49DF24` | immediates ×10 |
| the projectile compaction frame | `0x49AE20` (`jmp` to a page-at-a-time stack probe), `0x49AEB8`, `0x49AF39`, `0x49AF7F` | a stub from `tagpu_detour_stub`, three displacements |
| the explosion pool | `0x420630`, `0x420A36`, `0x420A3C`, `0x420B35`, `0x420B3B`, `0x420F66`, `0x421738`; caps `0x420A44`, `0x421771` | `main`-relative operands become the address of `s_expl`; `0x420AA2` becomes a `call` to a stub that reads the sequence table from `main` |
| the flying-piece slots | seven base and six end operands (`0x420B08` … `0x42166D`); the backing `push` at `0x4208FB` | operands become `s_psys`'s bounds |
| the level's effect reset | `0x42090A` | a `call` to `lim_level_reset` in place of the `rep stosd` |
| the debris records | `0x4217DE` (16 bytes: `call` + `jmp 0x421804`), and `0x420920` (`jmp`), a stock copy of the scan that nothing calls | first-free C allocator over `s_aux` |
| units a player | `0x491640` (default), `0x491659` (compare), `0x491666` (clamp-to) | immediates → 1500; the floor 20 stays |
| the two `maxunits` keys | `0x432646` (a saved game's `[Summary]`), `0x436037` (a map's `.ota` `[GlobalHeader]`) | the 7-byte store becomes a `call` to a stub that clamps `eax` to [20, 1500] and stores it |
| the host's limit in a network game | `0x4973AE` (read), `0x4973B5` (store) | the read becomes `movzx eax, word [eax+0xA5]`, the store the same clamp stub |
| the restriction menu's "no limit" | `0x44CAFE` | 101 → 1500, so a cancelled menu caps no type below the player's own limit (Reset's 100 each, `0x44C62D`, is stock's and shown as 100) |
| the pathfinder's budget | `0x40EAD6` | 1333 → 66 650 |
| the particle layers | twenty compares, `0x471183` … `0x472CD9` (`0x472BF2` against `ecx`) | 400 → 20 480 |
| the particle pool | `0x471C83`, the `push` in the C runtime's static initializer `0x471C80` | 1000 → 204 800, built at that size because DllMain runs first |
| the composite scratch frame | `0x45819B` (width), `0x458196` (height), the two `push 0x258` in `0x458180` | 600 → 1280 each; one frame a level at `*(main+0x1437B)+0x10`, where it starts: §2.6c's composite fix bounds its writers and grows it |
| the wreck pool | `0x421F2A` (bytes), `0x421F41` (the clear's dwords), `0x421F7A` (the free-list loop's end), `0x421F97` (the last record's offset); the "no record" count at `0x4232B9`, `0x42340E`, `0x42343A`, `0x42361E`, `0x42364D`, `0x423DBA`, `0x423DE1` | 2048 → 8192 records of 0x30 (and the offsets with them); the fork's `WR_COUNT` and the packet's wreck table are the same constant |
| the category masks | the heap mask's allocation `0x488CC2` (a stub: `push 0x40` has no room for 2048), clear `0x488CD3` and OR `0x488E3E`; the stack masks' frames in the AI's `Weight` (`0x406DB5`, `0x406DBE`, `0x406DC9`, `0x406DFD`, `0x406E3A`), `Limit` (`0x406E45`, `0x406E52`, `0x406E5D`, `0x406E64`, `0x406EB2`, `0x406ED6`) and Ctrl-Z (`0x48BE08`, `0x48BE22`, `0x48BF1E`) | 0x40 → 2048 bytes; each frame's mask sits at its foot, so the entry, the clear count, the displacements above the mask and the release move, through stubs |
| the unit-type count | `0x42AA65`, before the def array is allocated from the count | a `call` to a stub that refuses more than 16 384 slots with a dialog and `ExitProcess(1)` |
| the network join's pace | `0x46DE91` | the joiner's unit-sync checksums 4 → 64 types a lobby tick |
| the weapon array's references | `0x42CDCD`, `0x42F3AB`, `0x49E5CB`, `0x437CF7`, `0x437D13`; bounds `0x42F431`, `0x49E5EB` | `mov reg,[main]` → an immediate of `s_weapons − 0x2CF3`; `0x11500` → `4096 · 0x115` |
| the weapon loader, wipe and model | `0x42E468` (the base, and the ID's bound, both builds), `0x42E31C` (the wipe's loop), `0x42EC99` (the model's loop), `0x42ED46` (the model's store, rewritten in place) | stubs that take the record from the full index; the ID byte `+0x10A` is the slot below 256 and nonzero above |
| `0x0D`, weapon fired | the five sends `0x49D859`, `0x49DB4D`, `0x49DD27`, `0x49DEEE`, `0x49DFF6` (both builds: the stock build only hands the packet to the demo recorder, §2.6e); the receiver's block `0x49D280` (both builds) | ID bits 8..11 in the high nibble of `+0x23`; the receiver bounds the shooter and target |
| `0x0F`, feature hit | the send `0x424575` and the receiver's dispatch slot `0x455FB8` (both builds) | ID bits 8..11 in bits 12..15 of the cell x; bit 11 marks a hit whose byte is a sentinel's |
| `0x0E`, interceptor detonation | the byte reads `0x49A78C`, `0x49A7CD` (both builds: the stock build reads the byte as stock does and hands the interception to the demo recorder, §2.6e), the sends `0x49A7A8`, `0x49A7E9`, the receiver's compare `0x49AFC9`, and the `0x05` dispatch slot `0x455F90` | below 256 stock's message; from 256 a companion chat message, `05 00 49`, the target point and a `u16` ID |

**The rule, one table and all or nothing.** `tagpu_limits_install()` runs from `DllMain` right
after `tagpu_apply_patches()`, before the exe's entry point, so no engine thread executes a site
while it changes. The same table holds the simulation fixes of §2.6c, in both builds: 267 sites in
all in the raised build (MEASURED 2026-09-26 from the log line), and the fixes alone in the
stock-limits build, where the weapon IDs' four sites join them. It
refuses a table in which two rows share a byte, reads every site (through `VirtualQuery`, never
assuming the page), compares all of them with **the exe file on disk**, and writes them only if
every one matches; a refused write puts back the file's bytes. The patches last for the process and are
never restored. The log line names the moved pools' addresses for `tacli peek`: `limits:
installed 267 sites, the simulation fixes' included -- …, units 1500 a player,
pathfinding 66650, particles 20480 a layer from a pool of 204800, composite 1280, wreck records
8192, unit types 16383, weapons 4096 at 0x…`; the stock-limits build logs `limits: stock build --
nothing raised (…); the simulation fixes' N sites installed`. Right before it, once both
installers have taken their stubs, `enginefix: the fixes' and limits' stubs take N bytes in P page(s)
of 4096` (§2.6c).

**The exe file on disk is the reference.** The comparison is made against the bytes of
`TotalA.exe` itself, read through the takeover's mapping of it (`tagpu_takeover_file_*`, §2.6d,
opened without `FILE_SHARE_WRITE` as that pass opens it). A site whose bytes in memory differ from
the file was rewritten by something that ran before Impure, which is what the table refuses over. A
site whose *file* differs from stock 3.1 is the mod's own build — a mod ships its engine changes in
its exe — and is refused just the same, because Impure's value there belongs to a fix that argues
from the stock bytes. The exception is a site marked `lim_file_ok`: the value stands alone (nothing
of ours reads it, no other site's argument rests on it) **and the site is one Impure raises in the
first place**, so the difference is accepted and Impure's own value goes in over it (`limits: N
site(s) hold a value of the mod's own in its exe file, at sites where the value stands alone`).
**One site is marked**: the pathfinding budget `0x0040EAD6`, a lone dword — stock 1333, the raised
build 66650, Escalation's exe file 1114. Escalation's own `TAESC.ini` sets
`AISearchMapEntries=66650`, which its TADR writes over that 1114 at every launch, so the exe's byte
is the unraised number its runtime replaces and Impure's 66650 is what the mod asks for; it also
keeps every Impure peer on one budget, so the port's open question about unequal budgets
(`limits-evidence.md` §6) is not relied on. Baked stock bytes alone cannot tell a rewrite from a
mod's own value, and comparing against them refused Escalation at its first changed site (MEASURED
2026-09-27). Where the file cannot be read for a site — no mapping, a writer holding the exe, or an
exe loaded away from its own `ImageBase` — that site falls back to the stock bytes and the log says
how many, since a mod's exe then refuses there.

**Fail closed.** If any site differs, nothing is written. At the first `DirectDrawCreate` or
`DirectDrawCreateEx` — outside the loader lock, before the game window exists —
`tagpu_limits_report()` shows a MessageBox titled *Total Annihilation: Impure cannot start* and
calls `ExitProcess`. The text says why in plain words (the exe is not 3.1; it is 3.1 but was
changed in memory first; Windows refused the write; or Impure failed on its own side, a table
overflow, two rows over one byte or a stub it could not allocate), what to do, and ends in a report block for
whoever debugs it: the impure commit and branch, the exe's name, size, md5 and PE stamp, whether
the md5 is a known build, and every differing site as `want` and `have` bytes (twelve in the box,
all of them in `log\tagpu.log`). It prints no path. The same text is written to
`log\startup-failure.txt`. `tagpu_log_dir()` gives the folder.

**The safety net.** The same first DirectDraw call re-reads every site once more (`lim_verify`),
now against the bytes Impure wrote: it comes after every DLL's `DllMain`, so a site that no longer
holds them was rewritten by a patcher that started after Impure. The report then says so — *changed
by another program … before a battle can crash*, `result: N of M sites rewritten after they were
installed`, each site's `want` being Impure's bytes — and the process ends before a game exists.
`s_limState` stays installed while it does: the engine code already points at the raised pools.
MEASURED 2026-09-26 (Wine, the suite's `mayhem-11.3.0-net`): Total Mayhem 11.3.0's 2024 TADR
with the takeover (§2.6d) switched off rewrites 17 of 265 sites — ten of the unit-type
relocations (the AI plan's Weight and Limit frames, the category mask at `0x488CC2`) and seven
sites of the fixes (`0x42DAC7`, `0x4954ED`, `0x486036`, `0x486DC1`, `0x4854A0`, `0x490C5A`,
`0x42CF5E`). A write made after that first call is outside the net
([the takeover](../compat/takeover.md), part 3).

**The stock build.** `make LIMITS=stock` builds `ddraw-stocklimits.dll` from objects with their
own suffix (`.stock.o`), with `TAGPU_LIMITS_STOCK` defined: nothing is raised and every accessor
returns the engine's own location. It is the comparison build, launched with `tacli launch
--keep-dll` after copying it over the instance's `ddraw.dll`. There is no runtime switch.

**What follows the pools.** The frame packet's publisher reads the explosions through
`tagpu_limits_expl_pool()` and the flying pieces through `tagpu_limits_psys_begin()`/`_end()`,
and `tagpu_weapons.c` the weapon array through `tagpu_limits_weapon0()`, never at a fixed address; `TAGPU_PK_MAX_PROJ`/`_EXPL`/`_DEBRIS` are the pool sizes, so an effect
table cannot truncate below the engine's own cap. `tagpu_fx.c`'s buckets are sized from the tables
they draw (`tagpu_fx.h`, and *The particles follow the layer cap* below): `SPRITES` holds a
projectile's two quads (its shadow blob and its frame), an explosion's one and the particle table;
`UNDER` the particle table; lines and flashes 65 536 vertices each. That is a size, not a bound by
construction: a composite GAF frame makes a quad per subframe and a lightning bolt up to 2 044 line
vertices. A quad or line that does not fit is dropped and logged (`fx: DROPPED … bucket-full`).

**The design point follows the unit limit.** The engine's unit array is `10 · N + 1` slots, so 1500
a player makes **15 001**: `TAGPU_PK_DESIGN_SLOTS`, and every cap sized from it (§2.86) grew with
it. `tagpu_packet_pub.c` asserts that the installed limit fits the design point, so raising
`TAGPU_LIM_UNITS` alone fails the build. The scenario harness holds a game that full too
(`SCN_MAX_UNITS`, `_ORDERS`, `_CLEAR` = 10 × `TAGPU_LIM_UNITS`; `tacli`'s `SCN_MAX_LIMIT` 1500,
`SCN_MAX_ENTITIES` 15 000). In a network game every peer takes the host's limit, through the same
[20, 1500] clamp as the `maxunits` keys (`0x4973B5`), so every writer of the array's count is held to
the design point; should a slot count still exceed it, the packet's tables truncate and say so.

**The particles follow the layer cap on our side too.** The publisher walks a layer up to the
engine's own steady state, `TAGPU_LIM_SFX + 1` (`tagpu_packet_pub.c`; past it the layer is counted
in `layerbad` and skipped). The packet's particle table is `TAGPU_PK_MAX_PART` = 24 576
sub-particles, one and a half times tier 1's frame of 14 510. **A frame that holds more is thinned,
not truncated:** the publisher counts every layer's sub-particles first and, when the total is past
the table, keeps the same share of each layer (a 16.16 accumulator, so the kept total cannot exceed
the table), counted in the heartbeat's `thin=`. A table that simply filled would lose the top
layers whole, because the walk runs bottom to top — the trails and smoke of layer 9 first.
MEASURED 2026-09-23 with the table forced to 2048 in tier 1: 670 thinned frames, the kept count
never past 2047, `trunc=0`, and every layer present in proportion (a frame of 8 776: layer 9 kept
2 024, layers 4 and 5 kept 8 and 15).
The effects pass's four vertex buckets no longer share one cap (`tagpu_fx.h`): each of the two
that particles land in holds the whole table, `UNDER` (layers 0–6) at 6 × 24 576 vertices and
`SPRITES` (layers 7–9, projectiles, explosions) at 6 × (2 × projectiles + explosions + 24 576);
lines and flashes keep 65 536. At 65 536 for all four, tier 1's opening volley filled `SPRITES` and refused 169 quads
(`fx: DROPPED … bucket-full`, which counts quads and lines, not vertices); on the landing's build
the same fight dropped none. **What that costs in address space**, in a process that is not
large-address-aware (the exe's characteristics are `0x10B`): the buckets' static arrays grow from
9.0 to 16.5 MiB, and a Vulkan slot's host-visible vertex buffer, which grows to a power of two and
never shrinks, can reach 32 MiB at the buckets' worst case, one a swapchain image. The engine's own
particle pool is 15.6 MB plus a 0.8 MB pointer stack, allocated once at startup.

**Tier 1, measured 2026-09-23** (`scenarios/limits-tier1.json`: four players at 1500, 6000 kbots on
Town & Country ordered onto the centre, 1920×1080, the shipped defaults, speed 20): the engine held
1500 a player and 15 001 slots; 6000 of 6000 units and orders applied; the packet peaked at **5983
units and 88 696 pieces**, 2.96 MB used of the 20 MB reserve, never truncated after the load's
growth. Effect peaks: projectiles 215 and explosions 1042 (the engine's counts, sampled at
~2 Hz), flying pieces 441 (at the heartbeat) and 755 particles in one frame (the packet's own
running maximum, under the stock particle caps). Under landing 3's raised particles the same fight
peaked at one layer of **13 529 objects**, a pool of **13 571** in use and **15 964** sub-particles
in a frame, with the simulation at 60 ticks a second throughout. **The sim held 57–60 ticks a second** outside the apply frame,
so neither the pathfinder's budget nor the explosion tick's compaction stalled it at these counts.
The GPU frame was 0.7–0.8 ms at p50 (`ftime`). **The game thread published 21–59 frames a
second, median 36**, and the publisher's own cost is past its histogram's 512 µs top at every
sample — how much of the game thread's frame is ours and how much is the engine's is not
measured.

**Unit types, measured 2026-09-24** (`tools/unittypes_fixture.py`: generated kbots on the Peewee's
model with one small script of ours, Core Prime Industrial Area). At the ceiling, 16 105 generated
types and the 278 stock: the scenario load at 1280×1024 took 43.1 s against stock's 21.7 s, 1.3 ms a
type, and the engine's live bytes (`[0x5289F8]`) grew by 108.9 MiB, 7 092 bytes a type. Two games in
one process at each resolution, the second started through the menu, in MiB (the address space
walked with `VirtualQueryEx` from inside the prefix):

| resolution | engine live, game 1 | engine live, game 2 | committed, game 1 | largest free region, game 1 |
|---|---|---|---|---|
| 1280×1024 | 173.9 | 165.6 | 426.1 | 83.6 |
| 1920×1080 | 184.1 | 177.5 | 441.1 | 62.4 |
| 3840×2160 | 267.3, the 102.8 MiB work ring included | 260.8 | 563.7 | 4.1 |

Without the mod, at 1280×1024: 65.0 MiB live, 308.9 committed. The free regions are small because
Wine reserves about 1.5 GiB of the 2 GiB and hands it out on demand; both games at 4K got their
ring out of it. A 32-bit process with the Vulkan stack can still allocate 1.44 GB
(limits-evidence §8), so a mod of heavier types fits less, and past that says so (§2.6c). 16 384
real types show the refusal and exit. A first two-peer join at the ceiling takes 20 s at stock's
pace and 12.5 s at ours.

**Weapon IDs, measured 2026-09-24** (`tools/weaponids_fixture.py`, Two Continents). Single player:
a tower firing weapon 4000 damages its target through the static's record 4000, and the weapon's
model is the stock one it copies, borrowed and drawn; the ID-less and 5000 weapons log their skip
and their towers stay unarmed. Two peers, the joiner's towers firing at the host's wrecks, solars
and interceptor: every feature hit reaches the host at its full ID; the host's interceptor 3000
caught 11 of the joiner's rockets 250 and 3322, one low byte and one target, and the joiner
detonated the right rocket each time, 6 through stock's `0x0E` and 5 through the companion.
Paused once the fire stopped, the peers held the same units at the same health and the same
feature cells. The extra-weapons module installs beside the raise: both are rows of the one table,
which refuses overlapping sites.

**Sounds: 32, through the store, not a site.** `mixingbuffers` in `impure.cfg` (renderers.md
2.10b) is 32 by default and one of 8, 16, 24 or 32. `tagpu_menu.c`'s `eng_push_mixing` writes it
into the sound object's `+0x2C` after every registry load (`0x42F9A0`), where the loader has just
stored the registry's `MixingBuffers` through a setter that takes anything. 32 is the engine's
own table: past it a sound plays untracked and a looping one escapes the stop-all (engine map,
*The sound object*). TADR writes 128. The engine saves the value back to the registry with its
other options, as it does the store's Gamma, into the registry: a tacli instance's own
registry store (§2.92), so a later control launch of that instance (`tagpu_defaults.off`, or the
stock DLL) reads 32 where stock's missing key gives 8. MEASURED 2026-09-23, tier 1 with sound on a null
device: 32 in use for the whole fight, never more; the store at 8 held 8.

**The composite scratch frame** is the unit bake's one shared frame a level (engine map, *The
composite scratch frame*), not the per-unit composite, which the ring caps. Four engine writers in
the blit size it to a unit and never compare with the allocation (the build-state copy, the frame
copy, the shadow build and the 2× structure bake, every frame and on every lane), so the raise
moves the box at which they write past it, from 600 × 600 to 1280 × 1280 (a quarter of each for
the 2× bake). It read back at 1280² after tier 1
on the Vulkan lane and at 600² on the stock build, and the GDI lane draws the nanoframe ladder the
same on both.

**Tier 2, measured 2026-09-24**: ten peers in one network game on Town & Country, 1500 a player,
15 000 units. Every peer held 15 001 slots and peaked at 14 991–15 000 alive; every raised pool
passed its stock cap on every peer (projectiles 731–854, explosions 2936–2966, particle objects
12 759–14 331, flying pieces 947–993); the simulation held 30 ticks a second, the full rate at a
network game's speed 10, on all ten. Paused after three minutes, the ten agreed on the same 7307
units in the same slots, and 95.8 % stood at the identical position on all ten; the moving rest
sat off their owner's copy by at most 40 ticks of their own top speed. Details in
[the plan](../tadr-port/raised-limits.md), landing 5.

**Landing 1, measured 2026-09-23** (the numbers are in the engine map): in single player the packet carried
687 projectiles, 2439 explosions and 540 flying pieces with no table truncated; in a
two-peer network game both peers passed every stock cap and, paused, held the same units at
identical positions; a copy of the exe with one site byte changed got the report, the file and
the exit, and the retail exe was untouched.

**Below the stock caps the raised build is not stock in one respect.** The flying pieces' ring
allocator `0x437A30` evicts the oldest blocks when it wraps and nulls their slots; its backing is
ten times larger here, so a piece stock would have evicted to make room lives on, lands and adds
its explosion. That changes the explosion count and the C-runtime `rand` stream, and reaches the
simulation's generator only if the explosion pool fills (it gates `0x421700`'s draws). The
stock-limits build is therefore a comparison build, not a proof of equality.

**What this landing does not close.**

- **The explosion tick's compaction is quadratic.** `0x4210E6..0x42113E` removes one dead record,
  shifts the whole tail down one record (`rep movsd`, `0x15` dwords) and rescans from the start,
  so a tick costs O(dead × live) record moves. At 3000 records with a mass death expiring together
  that is about a hundred times stock's worst tick. Not measured yet; landing 2's tier-1 battle
  measures the tick. TADR runs the same loop.
- **The install fails closed in any process that loads this DLL.** An exe that is not TA and calls
  `DirectDrawCreate` gets the same box and exits. By design: this `ddraw.dll` is built for
  `TotalA.exe` alone, and the report names the exe it met.
- **The effects models are sized for the raised pools**: the packet carries up to
  `TAGPU_PK_MAX_FXMODEL` (two a projectile, one a piece, one an explosion — 10 000 at the raised
  limits) and the unit pass's hand-over counts them in `TAGPU_PD_MAXHAND` (§2.89).
- Whether a peer on a lower cap than its opponent refuses projectiles the other fired, and what that
  does to damage. The contract is the same build on every peer, which is what was measured.
- The known-build table holds retail 3.1 alone; other builds report `known build: none`.
- **What 6000 units cost the game thread.** The median 36 publications a second at 4000–6000
  units is measured; its split between the engine's own frame and our publisher is not, because
  the publisher's histogram stops at 512 µs.
- **How long a remote unit lags its owner at stock's 500 a player.** At 1500 a player (tier 2,
  below) the moving ones sat up to 40 ticks of their own travel behind; stock's figure is not
  measured, so whether the raise stretches TA's update interval is open.
- **The particle layers erase by shifting.** The layer tick `0x471EB0` deletes a finished object and
  moves the tail down one slot, and an emitter at the cap drops the front the same way, so a
  removal costs the layer's length. At 13 529 objects in one layer the sim held 60 ticks a second;
  a layer held at the full 20 481, where every emission shifts the whole layer, was not reached.
  The level-end teardown `0x471DE0` empties each layer front first the same way, about n²/2 moves:
  some 2 × 10⁸ for a full layer. Not measured.
- **The composite scratch frame is not bounded.** A unit (with its cargo) whose screen box is past
  1280 × 1280, or a structure's past 640 × 640 under the 2× bake, still writes past the frame, as
  one past 600 × 600 or 300 × 300 does in stock. A bound has to test the box in all four writers
  (`0x4589C0`, `0x45A470`, `0x45A790`, and the 2× branch of `0x459830` / `0x459C70`, which can
  take the 1× path instead) and skip or fall back in each; not built. The ring that caps a unit's
  own frame does not help: at 800 × 600 it admits one of 3.1 million pixels.

### 2.6c The stock engine's own defects (`tagpu_patches.c`, always on)

**What it is.** Seven places where the retail image writes or reads memory it does not own (the
third also leaves a strip of the viewport unpainted, which the same patch paints; the fourth is the
composite scratch frame's five writers and the rasterisers' span tables under them; the fifth and
sixth are a builder's build list and the download menus' records, which a large mod reaches; the
seventh is a weapon's ID, which the loader and two network receivers take unbounded), one
where it takes a player's payment and does not deliver (a feature reclaimed or destroyed while the
wreck pool is full), one where it takes it twice (a feature reclaimed through a cell that is not its
anchor while it plays its sequence), one where a network game can never start (two unit types with
one unit-sync key), one where the peers disagree (a weapon with the ID 253–255 hitting a feature,
fixed with the seventh), two in a saved game's load (the features on the map's border are lost,
because the border mask runs before the restore; and a restored feature's state is written into a
wreck record it does not own when it did not come back as saved), the out-of-memory text, which
blames the disk, and four in combat ([section B's landing 1](../tadr-port/sim-fixes.md)): an explosion
hits a unit found past its twentieth victim, or a feature past its sixty-fourth, once per cell of it
in the blast; flak fired nearly straight up divides by zero; a unit whose footprint ends on the
map's last column or row is parked off the map, where nothing can hit it; and a unit whose altitude
is more than twice its distance from the north edge is off every other player's line-of-sight grid.
One more is [landing 2](../tadr-port/sim-fixes.md): an aircraft whose cells other aircraft hold takes
no splash at all (one that no in-rect slot names). And [landing 3](../tadr-port/sim-fixes.md): five network receivers take a unit index, a type or a slot delta off the wire with no bound. [Landing 4](../tadr-port/sim-fixes.md): a hit names its victim by slot and the owner reuses a freed slot at once, so a hit in flight lands on whatever unit the slot holds when it arrives. [Landing 5](../tadr-port/sim-fixes.md): a peer still loading refuses the others' first units, their commanders among them, until the round robin re-creates each. And five outside the battle ([landing 6](../tadr-port/sim-fixes.md)): each peer of a network game draws a wind of its own, a yardmap string is parsed past its end, the saved-game loader's order fallback walks one record past its table, the stockpile bar divides by a weapon's reload time without testing it, and a range circle of radius 1 divides by zero. [Landing 8](../tadr-port/sim-fixes.md): a peer counts a kill, the killer's Kills and a reclaim's credit only when its own copy of the victim reads finished, and a copy starts unfinished until its owner's round robin reaches it; and the builder's finish, `0x12`, scales two wire indices with no bound. Thirty fixes, built at every attach, in both builds, by `patch_engine_defects()` and then `patch_loader_defects()` at the end of
`tagpu_apply_patches()`. Each patch is the identity on every input stock handles correctly.

**Two classes.** A *simulation* fix — one whose absence would let a player silently compute
different shared state on an input stock does not fault on — hands its sites to the fail-closed
table of §2.6b, in both builds: they are compared and written with the rest of the table or not at
all, and a mismatch ends the process through the failure report. The seventeen such fixes are the
full wreck pool, the reclaim mark, the border features, the saved record's owner, whole build lists,
the download menus, the sync keys, the weapon IDs, the victim caps, the last column and row, the
line of sight, stacked aircraft, stale hits, the ghost commander, the kill counts, the wind and the yardmaps. A
*local* fix — a crash, a draw, a message or a malformed input — compares and writes its own sites
and is skipped alone: the sort buffer, the NULL plot, the terrain window, the composite scratch,
the out-of-memory text, flak's divides, the line of sight in local code, the projectile pass's
view, the network receivers' bounds, the builder's finish's indices, the saved-game order
fallback, the stockpile bar's divide, the range circle and the maps with no terrain. The engine map's table gives each
one's reason. The disassembly, callers, invariants and measurements are in the
engine map's *Engine defects we patch*; the register of defects is `binary-patches.md` §"Stock
engine defects we patch".

| VA | What it is | Mechanism |
|---|---|---|
| `0x469807..0x469825` | `DrawGameScreen`'s unit-sort append, which never tests a row's count and so can write unit pointers past the end of SORT_UNIT_LIST (`main+0x141FB`) | 31 bytes: a `jmp` to a 44-byte stub from `tagpu_detour_stub`, then NOPs up to the stock join `0x469826`. The stub files a unit only while `count[row] < (rows − row)·cap`, the slots left to the end of the allocation (`[edi+0x50]`, `[edi+0x54]` = `main+0x1424B`/`+0x1424F`). The whole stock block is compared first, then written with `tagpu_detour_write` |
| `0x421E60` | `GetGridPosFeature`, which reads `[plot+8]` with no NULL test (callers `0x498F4F`, `0x40514A` untested; `0x47EAE3` tests) | a prologue detour (`tagpu_detour_land`, 8 stolen bytes, compared first). A NULL plot returns `0xFFFF`, the engine's own "no feature", with the function's `ret 4` |
| `0x484057` | inside the terrain pass `0x483FA0`, where its window of 32-px cells (`row0`, `col0`, `nrows`, `ncols` from the eye and the view) is computed and the tile map `main+0x1428B` not yet read | `tagpu_detour_land` over 10 stolen bytes, compared first. The stub passes the pass's frame to `terrain_window_on_map`: a window stock gets right — `sx ≥ 0`, `sy ≥ 0` and inside the `pxH/32 × pxW/32` tile map (`main+0x14223`/`+0x14227`, the words LoadMap sized it with) — runs the stolen pair and resumes at `0x484061`; any other is drawn by `terrain_window_draw` (black, then each on-map cell whose id is below the tile set's count, inside the OFFSCREEN's clip rect, which must itself lie inside the surface's width and height at `+0x00`/`+0x04`) and leaves through the pass's epilogue `0x4843AC` |
| `0x423651` | `FeatureDie 0x423550`'s pool-full `jge 0x4236F7`, which returns without swapping the feature while its callers have already paid (the reclaim `0x4237D0`) or told the other peers | the `jge`'s 6 bytes retargeted (opcode `0x3D` at `0x42364C` compared too; its operand is the limits table's) to a stub that reloads `x`/`y` from the arguments and joins `0x4236EF`, the engine's own swap for a feature with no sequence |
| `0x423892` | the reclaim completion `0x4237D0`'s test that a GAF feature is already playing its sequence, which reads the flags of the cell the builder aimed at while `FeatureDie` marks only the anchor | `tagpu_detour_land` over the 6-byte `test`/`je`, compared first. The stub resolves a `0xFFFE` cell to its anchor exactly as `0x423845..0x423862` does, tests the anchor's bit 0, and rejoins at `0x4238AD` (pay) or `0x423898` (the def's GAF test) |
| `0x43265A` | the game-load routine `0x432610`'s one call to the saved-feature restore `0x424C00`, which runs after LoadMap's border mask `0x4833B0` has marked the border `0xFFFD`, so every saved feature touching a masked cell is refused by the spawn and lost | the call's 5 bytes retargeted, compared first, to `features_restore_under_mask`: when the grid holds no feature it notes and empties every `0xFFFD` cell, runs `0x424C00` unchanged, sets every noted cell that is then EMPTY or `0xFFFE` back to `0xFFFD`, and refreshes the pathing maps around each such cell with `0x440A40` over its 1 × 1 rectangle, the engine's own refresh for a grid change. It runs only when no feature and no unit is on the grid yet. Loader thread, inside the level load |
| `0x4250C0`, `0x425185` | the saved-feature restore `0x424C00`'s writes of an `Animating Features` record's state and a `3D Features` record's `+6` into the wreck record the cell's `+0x0A` names, made whether or not the feature came back and took one — record 0 on the zeroed grid a load gets, another feature's | `tagpu_detour_land` over the 6-byte `mov edx,[0x511de8]` each block begins with, compared first with the 6 bytes after it. The stub runs the block only when the cell is an anchor of the record's own kind that owns a record — for an `Animating` record GAF with its sequence mark set, for a `3D` record 3DO, the rule the save writer sorts its records by — and the index is below the pool's allocated count (`0x421F2A` / `0x30`); otherwise it leaves for the loop's next record (`0x4250F7`, `0x4251A7`). Loader thread, inside the level load |
| `0x458B87`, `0x45A470`, `0x45A7B9`, `0x459875`, `0x459CB5`; the calls at `0x459608` and `0x4596D8` | the composite scratch frame's writers — the build-state copy `0x4589C0`, the frame copy, the shadow build, the 2× bakes `0x459830`/`0x459C70` — each of which sizes `*(main+0x1437B)+0x10` to a unit with no compare against its area, and hands it to rasterisers whose span tables hold 800 or 2048 rows; and the cargo merge `0x4B90A0`, which paints a carried unit into it with no right or bottom clip | all seven sites compared first and written together or not at all (a failed write puts back the ones written). Each check stub runs a C check between `pushad` and `popad`: a need that fits runs the stolen bytes; one that does not grows the frame (the engine's own allocator path by hand, up to 2048 × 2048); a refused grow takes the writer's fallback — the unit not drawn that frame (a flag, and a wrapper on the call at `0x459608` that leaves through `0x4597D8`), a 1 × 1 key-pixel frame, or the bake's 1× path. The merge's call goes through a `jmp` stub to `scratch_merge`, a `__stdcall` that clobbers only what `0x4B90A0` does, and runs `0x4B90A0` only when the cargo's rectangle lies inside the scratch's header box and that box inside the area, and otherwise leaves the cargo out of that frame's composite. Levers `tagpu_scratch.stress` (which also points a freed frame's planes at `0x80000000`, where nothing is mapped), `tagpu_scratch.nogrow` |
| `0x42DA58`, `0x42DAC7`, `0x42BEAF`, `0x42BEC3` | a builder's build list: the game load's shared `TEMP UTYPE LIST` and each builder's copy at def `+0x156` hold 30 entries, the append and the download appender `0x42BE30` write past them | three `call`s to stubs that grow a block through `0x4D83B0` before the entry that would not fit (`bl_room`: 30, then powers of two from 64), a `jmp` past the copy's `rep movs`, the appender's cap NOPped |
| `0x42DD74`, `0x42DDF0`, `0x42DE12`, `0x42DF23`, `0x42DF35`, `0x42E0B9` | the download menus' records, one of five entries a file at `[main+0x391CB]`, filled with no cap | the block from a zeroing allocator; a file continues into records at the block's end, which grows; stock's count write NOPped; the next file's record by index; the page count reads the record count; the downloadable check reads the files' own records only |
| `0x49E700`, `0x49E7BD`, `0x49E7CD`, `0x49E7F4` | the allocator's new handler's text, "Your hard disk may be full" | a call at the handler's entry writes our text into a static; the three reads point at it |
| `0x42E468`, `0x49D280`, `0x424575`, `0x455FB8` | a weapon's ID: the loader `0x42E440` takes the record from it with no bound; the `0x0D` receiver `0x49D270` scales the `u16` shooter and target by 0x118 into the unit array with no bound, and indexes the shooter's slots by `+0x23` with none; the `0x0F` receiver `0x45544D` reads the bytes `0xFD`..`0xFF` as sentinels even when a weapon sent them, and reads through `0x481550`'s NULL for a cell off the map | four sites compared and written together: a `call` in place of the loader's `mov edx,[main]` that skips a weapon outside the array, or whose section name (copied to `+0` with no bound at `0x42E490`) does not fit its record; a `call` over the receiver's lookup that bounds both indexes and drops the message past them (the slot byte is bounded by the unit's own count at the extra-weapons module's splice `0x49D364`); a `call` over the `0x0F` send that marks a hit whose byte is `0xFD`..`0xFF` with bit 11 of x; the `0x0F` dispatch slot pointed at a stub that tells the hit from a sentinel by that bit and refuses a cell off the map |
| `0x49A0A9`, `0x49A109`, `0x49A262`, `0x49A5CE` | area damage `0x49A120`, whose two stack lists hold 20 units and 64 feature anchors and whose damage runs for a victim past them once per cell of it in the blast | both calls of `0x49A120` retargeted to `dmg_area`, which holds a frame of two hash sets (unit slots, anchor ordinals; 32 keys each before they grow) as a local on the calling thread's stack, makes it the thread's innermost frame through a TLS slot for the call, and puts the previous one back; the 72-byte unit block and the 71-byte feature block replaced by calls of `dmg_unit_seen` / `dmg_feature_seen` that answer skip (`0x49A415`, `0x49A62B`) or damage (`0x49A2AA`, `0x49A615`) from the innermost frame's sets, each index bounded by the unit array's count or `W·H` first |
| `0x49CF18`, `0x42F314`, `0x42F32E` | the ballistic fire `0x49CDE0`'s burnblow flight time, `idiv` by `v·cos(pitch)`, 0 within 0.35° of vertical; and its `div` by `w+0x68`, 0 for a ballistic weapon with `weaponvelocity` 0 | a `jmp` over the 9-byte `cdq; idiv ebp; mov edx,[main]` to a stub that divides as stock unless `ebp` is 0 and then takes `weapontimer` through `0x49CF29`, counted and logged; the loader's two calls of `0x49E010` routed through a stub that first gives such a weapon a `w+0x68` of 1, logged. Local |
| `0x47CC8B`, `0x47CCA3`, `0x47CCA9` | the grid stamp `0x47CC30`'s `X + fw ≥ W` and `Z + fh ≥ H`, which park a unit whose footprint ends on the last column or row | the two `jge` become `jg`; the 50-byte bucket block becomes a `jmp` to a stub that keeps stock's bucket index whenever its linear index is inside the `rows·cols` grid — stock's column −1 at the west edge is the previous row's last bucket, which simulation code reads — and clamps the column and row into the grid only for an index outside it; it rejoins at `0x47CCDB` |
| `0x49A664`, `0x49A415`, `0x47CF98`, `0x4954ED` | area damage `0x49A120` reads victims only from each cell's two unit slots, and the stamp `0x47CC30` gives a contested slot B to one aircraft, so an aircraft no in-rect slot names is never found | the walk's end (11 bytes) and the selector step (18) become `jmp`s to stubs that, after stock's walk, hand the aircraft stock missed to its own per-victim code at `0x49A24E` one at a time with the selector at 2, then run the displaced tail; candidates come from a pool of airborne slots rebuilt by a stub on the step's call of the unit tick (`0x4954ED`, which first runs the replay's capture-point write and the demo recorder's capture, §2.6e) and added to from the stamp's airborne path (`0x47CF98`), under a lock, each re-tested at hand-over (alive and airborne by slot B's rule, bit 29 in the mask so a bit-29 unit is excluded; in the grid, a real bucket; not dying, bit 14; uncarried; model present; in the blast's rect; not yet seen) with every index bounded; a failed grow of the list is the engine's out-of-memory exit (`eng_alloc_or_exit`) |
| `0x48ADDF..0x48ADF0` | the unit tick's script call `DoScriptsNow 0x4B0D60(+0x9A, 1)`, made in the unit's turn after every event earlier turns and its own AutoAim made: a playing's host gives it before the step's replayed events, so a start made before the slice on the recording machine reaches its first slice a tick late on a puppet, and a unit the step creates in a later slot gets none in that step | the 17 bytes become a `jmp` to `pushad; push esi; call unit_slice; add esp,4; popad; jmp 0x48ADF0`: the recorder's marker (`RK_EV_SLICE`, when the COB had a start at `0x4B0B00` or `0x4B0940` since the capture point), the replay's deferral to that marker, then the slice from C, counted at `0x48ADEB`; in the fail-closed table of both builds; flags and `eax`/`ecx`/`edx` dead at `0x48ADF0`, every register kept (§2.6e, *The script slice*) |
| `0x486877` | the destructor `0x4866D0`'s Killed for a death received from a peer (mode 0, severity `+9` above 0): `0x4B0A70` with run-now set, which runs every record of the dying unit's COB at `dt` 0, where the owner's `Send_UnitDeath` ran Killed as the query `0x4B0BC0` (`0x4865C3`), that one thread alone; a playing applies every recorded death as a peer's | the `call` retargeted to `dtor_killed` (`thiscall`, `ret 0x20` like `0x4B0A70`): inside a replayed death the recording machine made itself, the query with the record's severity; otherwise the stock start; in the fail-closed table of both builds (§2.6f) |
| `0x465B6A`, `0x465C04`, `0x465CA2`, `0x465D46`; `0x465DA9`, `0x408095`; `0x407F74`; `0x43F5D1`, `0x43F631`, `0x43FC05`, `0x43FD1A`, `0x43FF85`, `0x4400AA`; `0x46778A`, `0x4677D0`; `0x482615` | the altitude-sheared row `(z − y/2) >> 5` under an unsigned bound, off the grid for a point high up near the north edge: `UnitInPlayerLOS 0x465AC0`'s four points under True line of sight, and under Permanent or Circular `PositionInPlayerMapped 0x408090` (the first three) and its inline copy (the fourth); the copy inlined in the AI caller `0x407E90`, whose probe it decides; the order resolver `0x43F0E0`'s six; the view player's map build `0x467440`'s two, which mark the units that player sees for the acquisition; and the sight emitter `0x4825B0`, the observer's side: a unit whose sheared row is off the grid stamps no sight at all. The census — by the data, every compare that reads a grid's height in memory: 57, 46 of them this read; what it does not enumerate, and the sight grid's builder found apart, are stated there — is the engine map's *Line of sight at the map's edge* | table rows. The first eight blocks (43, 43, 49, 34, 34, 44, 40 bytes, and the order resolver's 36 at `0x43F631`, whose point register stock overwrites before its row test) become a `jmp` to a hand stub; the other seven lone row tests (`cmp r,[g+0x84]` and its `jae`/`jb`, 8 or 12 bytes) to a generic stub built from the site's stock bytes, with a compare-only row over the span from its z load to its test, which the stub relies on; the emitter's row computation `0x482615..0x48261E` to a hand stub that fixes the row before stock compares it with the stored one and stores it — so the stamp and its later removal use one row by construction — with compare-only rows over the point `0x4825D4..0x482614` and from the landing point through the bound, `0x48261F..0x48266A`. Each uses the point's own row `z >> 5` when the sheared row is off the grid and the own row is on it, otherwise keeps stock's answer, and counts the own row per function |
| `0x43E69D`, `0x43E904`, `0x43EBC6`, `0x43ECDB`, `0x43EE94`, `0x43EFA9`; `0x47D3B8`; `0x465942`, `0x46598A`, `0x465A17`, `0x465A63`; `0x46725F`, `0x467294`, `0x467340`, `0x467375`; `0x47360C`, `0x473657`, `0x473A94`, `0x473AD3`, `0x4741EC`, `0x474237`, `0x474674`, `0x4746BB`, `0x47551E`, `0x47556C`; `0x47F431`, `0x47F476` | the same read in local code: the cursor picker `0x43E490`, the build cursor's site test `0x47D2E0`, the feature helper `0x4658E0` (both corners), the radar rebuild `0x466DC0`'s projectile dots, the particle leaves `0x473590`, `0x473A00`, `0x474170`, `0x4745E0`, `0x475470`, positional sound `0x47F300` | each lone row test becomes a `jmp` to a generic stub with the same rule and count; the 27 are compared together, each from its z load through its test (the radar's whole loop `0x4671C0..0x46742E` for its dots), and written together or not at all (`fix_los_local`) |
| `0x49BEE8` | the projectile draw pass `0x49BE60`'s inline copy of the same read, the local player's view of a projectile | the 40-byte block becomes a `jmp` to a stub with the same own-row rule and count, compared and written alone. Local: a draw of the engine's frame, which still runs as the golden source (`fxown` is not a play default) |
| `0x42BD29` | the menu-time loader's one call, after which the unit sync's keys (def `+0x13E`, `0x4B6BA0`'s checksum of the FBI) may collide | a `call` to a stub that calls the loader and, when it returns 1, re-keys all but the first by name of each group of types sharing a key, to a hash of the name moved past every held value, opening and sealing the write-protected def array with `0x4D8780` / `0x4D8710` |
| **wire robustness (landing B3, local):** `0x4861F7` (`0x09`), `0x4553FE` (`0x0A`, at the dispatcher's case), `0x4866E5`+`0x486753` (`0x0C` + killer), `0x489CED` (`0x0B`), `0x48B92B`+`0x48B960`+`0x48B985`+`0x48B9AD`+`0x48BA05`+`0x48BA5E`+`0x48B40E`+`0x48B49C`+`0x48BA9F` (`0x2C`), `0x44E0DE`+`0x48B574` (the `0x2C`'s nested unit references: a move payload's target, the round robin's carrier), the length's capture in the receive (`0x453595`, `0x45361F`), the splitter's short messages (`0x463939`, `0x463B33`), and the diverged `0x0D` in `wpn_rx_fired` (`0x49D280`) | the network receivers index the unit array (stride `0x118`) by a `u16` off the wire with no bound, a `0x09` may name a slot outside its sender's block, the `0x0A` attach scales its child and parent unbounded, and the `0x2C` receiver also trusts a signed delta, an unbounded type, a missing move class, a NULL mover after a refused create (`0x48BA05`) and a NULL model object after one (`0x48B4A6`), a signed round-robin remainder, the player's block pointer, two unit references nested in its stream, and a bit reader with no end; the splitter loops for good on a message of length 0, and the pump dispatches from a freed buffer after a growth | each site is a `jmp` at a clean boundary (the `0x0C` destructor at `0x4866E5`, after stock's `push edi`; the receive's two statistics calls a `call`) to a stub that verifies the whole stock span first (all-or-nothing, `FIX_BYTES` on a mismatch), bounds the value by a pure predicate, and continues at the same address; the `0x2C` is parsed from a copy of itself in one static buffer (the receiver runs on the game thread only, by the dispatcher's gate) followed by 48 zero bytes (the most the engine reads past the last check is 229 bits) and its reader is bounded by the message's length before each read the stubs precede, the paired `0x2C` stubs carry the validated type in a game-thread static, and a misframed `0x2C` points the bit reader at a zero dword and jumps to `0x48BA28`; a nested reference past the array becomes 0, the engine's own no unit; a message too short to advance ends the splitter's walk through its own end for an unknown code, and the receive's note keeps the pump's message pointer on the buffer. `tagpu_wirecheck.on` checks the C predicates at attach (22/22 OK, 2026-09-25; 33 cases since the nested references and the `0x0A`, not yet run in the game), and the heartbeat's `wire:` section counts, per receiver, the records accepted and dropped, the morph/dcreate/rcreate/ghost oracles and `CreateFromNetwork`'s no-block refusals. The engine map's *network receivers* subsection has the site table |
| **stale hits (landing B4, simulation):** `0x486036` (first-free's free test), `0x486DC1` (the destructor's store of type 0), `0x48634F` (`CreateFromNetwork`'s success exit), `0x4854A0` (the unit array's allocation), `0x4653DE` (after the Deathmatch respawn's create `0x4653D9`), the sends `0x4560AE` (`0x09`), `0x489CB9` and `0x489CCD` (`0x0B`), the dispatch table's slots `0x455F90` (`0x05`), `0x455FA0` (`0x09`), `0x455FA8` (`0x0B`); compared, not written: `0x4553E9`, `0x455417`, `0x45605C`, `0x489CA8` | a `0x0B` names its victim by slot alone, and the owner's first-free allocator hands a slot freed by a death to its next create at once, so a hit in flight lands on the new unit on the owner and on every bystander | every row in the fail-closed table, both builds. The `0x09` and the `0x0B` leave inside a tagged `0x05` of `0x41` bytes (tags `0x4A`, `0x4B`) that carries the unit's incarnation in the same bytes — the owner's GameTime at the create, strictly increasing per slot, or for a copy the `0x2C` made a lower bound (bit 31) from the record whose block holds the slot (`+0x18`). The `0x05` slot's stub enters the stock `0x09`/`0x0B` handlers with their stock return addresses and the record in `eax`, so B3's bounds and counters see them unchanged, and mirrors the dispatcher's state gate (`[0x512BC0 + 4·type]`); a bare `0x09` or `0x0B` is counted and dropped. The owner applies a hit iff its unit's birth ≤ the stamp; a bystander refuses only a provably stale one (both exact and different, or an exact birth after the other side's bound) and applies the undecidable rest as stock. First-free (arg 8 = 0) is decided once, at the block's first free slot: an unheld one, else the one freed longest ago if in an earlier tick, never one freed this tick; a NULL at the Deathmatch respawn `0x4653D9`, which stock uses untested, takes the block's end and writes nothing, so the respawn's own countdown fires again six passes (about 180 ticks) later. The tables are sized from the engine's `u16` slot count and never freed. `tagpu_dmgdelay.on=K` (test only) holds every outgoing hit K ticks; `tagpu_wirecheck.on` unit-tests the rules at attach (23/23). The heartbeat's `hits:` section counts both sides. [sim-fixes.md](../tadr-port/sim-fixes.md) B4 has the argument, the engine map's *Unit identity on the wire* the addresses |
| **ghost commander (landing B5, simulation):** `0x497F5E` (the load state's first call), `0x49842F` (the in-play entry's call of the frame function), `0x45477F` (the dispatcher's refusal in state 5), `0x48BA00` (the dirty entry's `call 0x4861D0`); the hold rides in B4's `0x05` receiver (`0x455F90`); compared, not written: `0x497F54`, `0x497F64`, `0x497C5F`, `0x498348`, `0x48B9F5`, `0x4861D0`, `0x496790`, `0x454788`, `0x455F50`, `0x48B933` (the `0x2C` reader's slots), `0x453E84` (the sender's record) | the dispatcher passes a unit create only in state 6, and each peer creates its commander from its loader while still in state 5, so a peer whose load ends later refuses the others' commanders: each is missing on it until the round robin re-creates it (50 s at 1500, 16.7 s at 500), and one that moves first comes back through a dirty `0x2C` entry at the slot's stale position, `(0,0,0)` in a fresh array | every row in the fail-closed table, both builds. B4's receiver holds a carried `0x09` its gate refuses in state 5, per sender in arrival order (ten × 64, the overflow counted), under a lock, since the game thread and the loader both pump during a load; a `0x0C` refused in state 5 cancels the latest create held for its slot from its sender. `0x497F5E` empties the queue at the load's start. At `0x49842F`, after the game thread has read the loader's last store and before the frame function runs the first tick, the game thread replays each held create through B4's receiver past its state test and `CreateFromNetwork` (a stub that puts the sender in `edi` as the `0x09` case does, its return treated as `0x4553E9`), only from a sender that passes the dispatcher's sender test under the same DirectPlay id, into a slot that is empty or older by B4's stamps; in the catch-up ticks that follow and the frame function's own pump after them (`0x4968CB`), still in state 5, B4's receiver makes a refused create at once, in the pump it arrives in, where state 6 makes it, and a kill refused there marks a copy made before state 6 dying as the engine's ghost sweep does. The dirty create's stub reads the entry's move payload ahead of its decoder — ground: path point 0, the last node the unit reached (a straight move's origin until it is within 5 px of its goal); air: selector 2's x, y, z — only from B3's copy of the `0x2C`, under B3's reader, with each read inside the message's length — so B5 requires B3 armed and refuses the fail-closed table when it is not (B3 stays local for its own purpose) — and takes it only on the map, for the four disassembled move classes. `tagpu_ghostq.off` (test only) turns the queue and the kill note off; `tagpu_wirecheck.on` unit-tests the rules at attach (21/21). The heartbeat's `ghost:` section counts. [sim-fixes.md](../tadr-port/sim-fixes.md) B5 has the argument, the engine map's *A create refused during the load* the addresses |
| **kill counts (landing B8, simulation):** the death's send `0x48666D`; the dispatch slot `0x455FAC` (a bare `0x0C`); the send's entry `0x451DF0` (the hold's gate); the creates at `0x488700` (capture), `0x488462` (placed units), `0x405104` (resurrection), each through a holding wrapper; the flushes `0x488743`, `0x488791`, `0x4884AC`, `0x405119`, `0x405155`, `0x405164`; the carried `0x0C` rides in B4's `0x05` receiver (`0x455F90`), the create's state in B4's `0x4A` (`0x4560AE`, `0x48634F`); compared, not written: `0x486669`, `0x45541C..0x455427` (the `0x0C` case the carried death reproduces), `0x455428`, `0x488705..0x488742`, `0x488774..0x488790`, `0x488467..0x4884AB`, `0x405109..0x405118`, `0x405141..0x405154`, `0x40515A..0x405163`, `0x40516A..0x40522E`. **Local:** the `0x12` case `0x4555BA` | every peer's destructor counts a kill, the killer player's Kills and a reclaim's credit only when its own copy of the victim reads finished (`+0x104 == 0.0`), and a copy starts unfinished at HP 0 until its owner's round robin reaches it (26–37 s measured at 1500), so a young unit's kill counted on its owner's peer alone; the `0x12` case scales two wire indices unbounded | every row but the `0x12` bound in the fail-closed table, both builds. The `0x0C` leaves as a tagged `0x05` (`0x4C`, 65 bytes) with the victim's `+0x104` bits; the receiver applies the `0x0C`'s gate, hands a death refused in state 5 to B5 (whose dying mark in the catch-up ticks writes the carried fraction first), writes a valid fraction (a number in [0, 1]) into a live copy and enters the destructor as the `0x0C` case does. The `0x4A` carries its unit's `+0x104` and HP, written into the copy at `CreateFromNetwork`'s exit as the round robin writes them. The four callers that set a unit's state after its create hold their thread's messages from the create to a flush after their writes (a NULL create lets go at once; the queue holds 4 messages and 92 bytes, one create's direct calls send at most 3 and 74, and anything past it — a unit script's send — sends the queue in order first, its `0x4A` without a state, so the copy starts as stock's); tacli's scenario applier holds the same way. The `0x12` bound drops a record whose built or builder index is past the array. The heartbeat's own `kills:` line counts, after each `packet:` line. [sim-fixes.md](../tadr-port/sim-fixes.md) B8 has the argument and the measurements, the engine map's *Who counts a kill* the addresses |
| `0x490C5A`, `0x491903`, `0x4982CA`, `0x4C98FD` | the wind updater `0x490C40`'s three draws (the next change, the speed, the heading), which each peer makes from its own RNGs, seeded from its own clock: the game thread's CRT stream by WinMain (`0x49E8BB`), the sim RNG by the loader (`0x497180`) | nine rows of the fail-closed table: a `jmp` over the draws' first instruction to a stub that calls `wind_draw` (stock's rules, our own splitmix64 generator) and rejoins at `0x490CE8` with `eax` the main pointer; a stub on the loader thread's start (`call 0x4B6B20` at `0x4982CA`, on the game thread) that first reads DirectPlay's session instance GUID with `GetSessionDesc` (used only with `dwSize` 0x50 and a non-null GUID, else the engine's copy `main+0x479` when not null); a stub on the level load's call that seeds the generator first — in a network game (the engine's own test, `GameingState +0` = 3) from that GUID and a hash of the map's name, or from the hash alone when neither source has one, counted; outside one from the performance counter; a `jmp` over the engine's one `SetSessionDesc` call (nine bytes at `0x4C98FD`) to a stub that makes it with a copy of the engine's descriptor carrying the GUID DirectPlay holds then, validated the same way (no call without one), counted, so no `SetSessionDesc` the engine makes can move the session; `0x490C40` (26 bytes), `0x490CE8` (13), `0x4982C5` (5, `push 0x497C70`), `0x4C98D9` (36, the descriptor and the interface) and `0x4C9906` (4, the result test) compared |
| `0x42CF5E` | the unit-def parser `0x42BF40`'s yardmap fill, which steps past the `YardMap` string's NUL onto the stack when the key is missing or ends on an invalid char | three rows: a `jmp` through `fix_call_regs` into `yard_parse` (stock's allocation, table, skip and repeat rules; a cell past the string takes the last valid char or `o`), rejoining at `0x42D079`; the 32-byte key read `0x42CF3E` and `0x42D079` compared |
| `0x43A58D` | the saved-game order loader `0x43A420`'s fallback, which walks the order table one record past its end and keeps a count it did not find as the order's type | local, 4 bytes: `jbe` → `jb`, and the not-found `jmp` → `0x43A552`, "Ready" |
| `0x439D41` | the stockpile bar `0x439D20`'s `idiv` by the slot weapon's `+0xE4`, with the slot index unbounded | local: a `jmp` to a 38-byte stub that bounds the index to 0..2, tests the weapon for NULL and `+0xE4` for 0, and takes the function's `return 0` (`0x439D6B`) on any of them |
| `0x438EDE` | `DrawRangeCircle 0x438EA0`'s guard, which lets N = 0 segments (radius 1) through to `idiv` | local: the `jl` becomes a `jmp` to `jl 0x43904D; je 0x43908F; jmp 0x438EE4`, so N = 0 takes the radius-0 epilogue |
| `0x49697B`, `0x499226` | the frame's `call 0x48BAE0` (after the ticks and the keys, before the cull and the draw) and the in-play handler's first instruction after the mouse's world position (after the GUI's dispatch, before the head's first reader), while an order (`main+0x2CC3`: a build placement `0x0E`, or a command mode 2..9, `0xC`, `0xD`) outlives every unit its click would order | local (B9): an `E8` and an `E9` to stubs that call `order_check` under `pushad`, which, when nobody would take the armed order — for a placement the walk `0x419755..0x41976A`'s own test, for a command mode `0x48CF30`'s first test (a selected unit), both bounded — calls the right button's cancel `0x499100` — not while `main+0x531` is NULL, and not until the engine's modal test (`0x37EBE & 0x865` or `0x2BEE & 0xE0`) clears; then `0x48BAE0`, or the stolen `mov edx,[main]` and `0x49922C`; silent at run time |
| `0x4198C6` | the build cursor `0x4197D0`'s call of the site test `0x47D2E0`, which sees only built units, so a site queued over another queued site reads green | local: the `E8` becomes a call to `build_site_test`, which runs stock's test and answers no when the footprint shares a cell — one yardmap byte with bit 4, the other with bit 2 or 4 — with a queued site (descriptor mask bit 0, build type, ground position) of the controlled player's units, leaving out the orders the click would replace (no Shift, flag 4 clear) or cancel (Shift, within 16 units); `main+0x2CC6` bit 6 is the only output |
| `0x434D87`, `0x430B87` | the map list builder `0x434BF0` after its per-map tests (the `.ota` parse at `0x434D5F`, `0x436860`'s answer at `0x434D7F`), and the options loader's test of the saved `SkirmishMap` | local: a `jmp` to a stub that calls `map_list_terrain` under `pushad`, which asks `0x4290F0(out, "Maps", name, "TNT")` then `0x4BB5B0`/`0x4BB5D0` and hands the answer back in `eax`: no terrain goes to the builder's refusal `0x434EA9`, terrain to the stolen `mov eax,[esp+0x24]; or ecx,-1` and `0x434D8E`; and a `jmp` to a stub that, for a value that was read, asks the same of it with `.ota` appended (`saved_map_terrain`) and sends one with no terrain to the missing-value path `0x430B8D`. Names bounded to 255 bytes before the builder; counted |
| `0x4386B9`, `0x4386D8` | the unit reclaim's step `0x438650`: its product (workertime · `(kills + 5)/5` · the target's MaxHitPoints · 15), taken in 32 bits and read as an unsigned qword by `0x4386CC`'s `fild`, and its quotient's `_ftol` (`0x4E43A0`), which returns the low dword | two rows of the fail-closed table (B10, simulation, both builds): 23 bytes at `0x4386B9` become a `jmp` to a stub that calls the x87-free `b10_reclaim_product` under `pushad` (the product in 64 bits, saturated at `INT64_MAX`, into the same qword `[esp+8]`), runs the `fild` and goes on at `0x4386D0`; 7 bytes at `0x4386D8` become a `jmp` to the same `fdivp`, a clamp of st0 to 2147483647.0, the `call 0x4E43A0` and `0x4386DF`. Stock's step for every product below 2³², bit for bit; C3's keyed stub `0x43869D` is untouched and jumps to `0x4386B9`; silent at run time |
| **the weapon keys (TADR section C, landing C2, simulation):** `0x42E310` (the weapon load's entry), `0x49ABB0` (the can-engage test's entry), `0x43F1D4` (the order action's unit branch), `0x49B9EB` (the guidance's water test), `0x49E1FD` (AutoAim's fire gate); the store is filled at A′3's ID site `0x42E468` | not a defect: TADR's weapon keys `nottoair`, `nottounderwater`, `surfacefire`, `notoverwater` and `notoverland`, which the stock loader never reads | every row in the fail-closed table, both builds; each site runs the engine's own instructions for a weapon whose key byte is 0, so stock content runs stock's bytes. §2.97 has the whole landing |
| **transported explosions (TADR section C, landing C4, simulation):** `0x49B017` (the death explosion's pick), `0x48664B` (`Send_UnitDeath`, its record complete), `0x486679` (its call of the destructor), `0x4867CB` (the destructor's detach of its own unit when carried), `0x486810` and `0x48681D` (the destructor's cargo loop: a passenger, and its detach), `0x486F10` (kill-all's entry), `0x42B019` (the menu-time loader's store of `CRC_weapons`); B4's `0x4C` branch (`doDeath`) calls the destructor through the same wrap | not a defect: TADR's unit keys `TransportedExplodeAs` and `TransportedSelfDestructAs`, which the stock loader never reads; and two stock broadcasts inside kill-all for a departed player (the `0x0A` of each of the destructor's detaches), which let the first peer to remove a player decide the others' pictures of its passengers | every row in the fail-closed table, both builds; a type without the keys gets stock's pick, and a unit's sync value changes only for a type with a key naming a loaded weapon. §2.101 has the whole landing |
| **the extra weapons (`tagpu_weapons.c`, simulation, always on):** 19 entry hooks (`UNITS_StartWeaponsScripts` `0x49E070`, `AutoAim` `0x49E1A0`, the name helpers `0x43E2E0` `0x43E240` `0x43E1E0`, retaliation `0x406F80`, acquisition `0x4089A0`, the slot helpers `0x4897E0` `0x4898B0` `0x489800` `0x48A060` `0x48A0A0` `0x48A0F0` `0x48A160` `0x48A190`, `0x49ADF0`, `0x49AA80`, `0x49D120`, the def copy `0x42B370`), 21 splices (the loader `0x42CEF2`, the CRC fold `0x42B004`, the `0x0D` receiver's slot `0x49D364`, the fire-name and slot-index decodes, the target finder and position, the attack-ground order `0x4039BE`) and 4 byte patches; the record observers on `0x42D2E0` and `0x42BF40` are not rows | not a defect: `Weapon4..N` for a unit type, capacity 16, where stock has three | every row in the fail-closed table, both builds (`fix_extra_weapons`), and the launch is refused without the observers; a stock unit runs stock code through the hooks' trampolines, and a stock type's CRC is the retail one. The site list with each one's purpose: [extra weapons](../extra-weapons.html), *Implementation* |

**It writes no engine state.** The sort stub writes only the engine's own per-frame sort tables,
exactly as stock does for every slot inside the list. The plot guard writes nothing. The terrain
bound writes only the engine's offscreen, and only on a draw stock gets wrong: the viewport inside
the clip rect, which is what the pass is there to paint on every draw, and which stock on such a
draw paints from off the map or leaves holding the last frame. The feature swap writes nothing
itself: it sends a full pool's `FeatureDie` down the engine's own path, so the feature map changes
exactly as it does for a feature that has no sequence — which is simulation state, and the point:
stock leaves a paid-for feature standing. The reclaim's anchor test writes nothing either; it
refuses, through any cell of a feature whose sequence is in play (a reclaim, a death or a burn), the reclaim stock refuses through its anchor. None is in §2.5. `tagpu_zoom`'s
`0x498EF9` guard and `vpwide`'s `0x499221` replica still clamp the pointer's world point to the
scroll extent. With `0x421E60` guarded, those clamps are what keeps
the hover *right*. They are no longer what keeps the game alive. The composite scratch fix writes
the engine's scratch frame, which is the engine's to write: on a grow it allocates the new block,
copies the header and both planes, repoints `ctx+0x10` and frees the old one; on a fallback it
writes a 1 × 1 header and one pixel of each plane. It is drawing state, not simulation state, so it is
not in §2.5 either. The build list and the download records write the engine's own load-time heap
blocks, as stock does, only larger; what a builder and the AI then read past stock's 30 entries is
content. The out-of-memory text writes our static. The saved-feature border fix writes the border cells' def words and the pathing maps, both in §2.5, on the loader thread inside the level load: every cell it shuts again holds what LoadMap's mask put there, and the maps are refreshed by the engine's own call around each. The saved record's owner writes nothing; a record it refuses is not written at all. The unit sync's keys are engine state, and
content: the keys depend on the types' names and natural keys only, not on the order the files
were found in, so two peers with the same files compute the same keys and it changes nothing the
peers can disagree on; stock content, whose 278 names share no key, is not re-keyed. The weapon IDs'
fix writes no engine state, only message bytes: a skipped weapon is one the loader never writes, a
dropped message one the receiver never acts on, and a flagged hit is damage where stock's host
destroyed, burned or reclaimed the feature. In both builds the bytes are the `0x0D` it receives
(`+0x23` masked to its low nibble in the receive buffer before stock reads it) and the `0x0F`
hit's flag. The raised build adds the `0x0D` it sends (the ID's bits 8..11 in `+0x23`'s high
nibble, and the meteor's `+0x1A`..`+0x23`, which stock sends as the stack held them, cleared) and
from 256 up the `0x0E` (the companion goes out, and the stock message's type byte is zeroed so its
send is skipped). That is simulation, as content: stock's weapons carry neither an ID outside
0..246 nor the bytes `0xFD`..`0xFF`, so with stock content every message means what stock's does,
and is stock's byte for byte but for the raised build's cleared meteor tail. The victim caps
write nothing of the engine's: they skip a repeat victim, so a unit past the twentieth takes one
hit and a feature past the sixty-fourth one `0x4244B0` call, as the first twenty and sixty-four
already did. Their frames are locals of the wrapper on the calling thread's own stack, found through
a TLS slot, so each thread has its own; the `0x0E` receiver reaches area damage on the game thread
only (its mask 4 passes in net state 6 alone), so that costs nothing and needs no argument. A set that outgrows its frame's 32 keys takes a block
from `0x4D83B0`, freed when its call returns. Stacked aircraft write area damage's selector
`[esp+0x10]` (2, which stock never gives it) and `esi`, inside `0x49A120`, and otherwise only their
own pool and list; the damage a served aircraft takes is stock's own code on a victim stock missed. Flak's floor writes `w+0x68` of a ballistic weapon whose `weaponvelocity` is 0 (§2.5), at
load; no stock weapon has one. The last column and row change which bucket and cells the stamp
writes, the engine's own, for a unit stock parked. The line-of-sight fix writes nothing of the engine's, only its own
counters: it reads a grid at a row inside it — the player's LOS grid, or under Permanent and
Circular line of sight (and at every mapped-grid copy) the shared mapped grid `main+0x14273` at the
player's bit. At the sight emitter what it changes is then written by the engine: the row stored in
the unit (`+0x7A`/`+0x7C`), the height byte `+0xF8`, and the sight stamped from that row into the
owner's grids, whose later removal reads the same stored words. The projectile pass's view and the local copies write nothing either, and change only
which projectiles, particles and features the engine's frame draws, which sounds play, and the
cursor's sprite over a point. The wire bounds write the registers and stack words stock itself
sets on the path they continue (`esi`, a stock-stored `[esp+0x20]`, a player record's GameTime
`+0x18` as stock's `0x48B963` does), and on a stop one thing stock never writes there: the `0x2C` bit
reader in the receiver's frame (`[esp+0x10..0x18]`), pointed at a zero dword of ours so the
engine's own end-of-list reads a clear flag. The length note writes two TLS slots of ours per
received message.
The wind writes the engine's wind fields (`main+0x37EC4`, `+0x37EDA`,
`+0x37ED8`, §2.5) on the game thread, in the tick's call, where stock writes them, with values from
our generator where stock drew its own: that is simulation state, and the point — the projectile
pass and the fire spread move by it, and wind generators produce and animate by it. Its seed is
ours, written on the loader thread inside the level load; in a network game it reads only what
every peer shares and no departure changes: the game mode, DirectPlay's session instance GUID
(read on the game thread before the loader thread starts, else the engine's copy of it) and the
map's name. It writes nothing of the engine's session copy (`main+0x471`, §2.5): the engine's one
`SetSessionDesc` is made with a copy of it that carries the GUID DirectPlay holds, so no call the
engine makes can move the session. It reads no seat:
the host seat's index differs between peers, and its DirectPlay ID can change during the load. The yardmap fix writes each def's yardmap
(`def+0x14E`, §2.5), through the engine's own allocator and at stock's size, the same bytes as stock
for every string stock parses inside its NUL. The saved-game fallback, the stockpile bar and the
range circle write nothing: they change one register or take a branch.

**The log lines.** The first is `enginefix: sort-buffer end bound 0x469807 ARMED; NULL-plot guard 0x421E60
ARMED; terrain window bound 0x484057 ARMED; feature swap on a full wreck pool 0x423651 ARMED;
reclaim tests the anchor's mark 0x423892 ARMED; saved features restored under the border mask
(0x43265A) ARMED; a saved feature's state written only into the record it owns (0x4250C0 0x425185)
ARMED; composite scratch bound (0x4589C0 0x45A470 0x45A790
0x459830 0x459C70 0x4B90A0) ARMED; whole build lists (0x42DA58 0x42DAC7 0x42BEC3) ARMED; download
menus past five entries (0x42DCF0) ARMED; the out-of-memory text (0x49E700) ARMED; unique unit sync
keys (0x42BD29) ARMED; weapon IDs bounded, feature hits told from sentinels (0x42E468 0x49D280
0x455FB8 0x424575) ARMED; one hit a victim an explosion (0x49A0A9 0x49A109 0x49A262 0x49A5CE)
ARMED; flak's divides (0x49CF18 0x42F314 0x42F32E) ARMED; the map's last row and column (0x47CC8B
0x47CCA3 0x47CCA9) ARMED. Counters: unit repeats refused at 0x…, feature repeats refused at 0x…,
victims refused off their arrays at 0x…, list blocks run unwrapped at 0x…, flak fallbacks at 0x…`,
and the second `enginefix: line of sight at the map's edge, in the table (0x465B6A 0x465C04
0x465CA2 0x465D46 0x465DA9 0x408095 0x407F74; the sight emitter 0x482615; the order resolver 0x43F5D1 0x43F631 0x43FC05
0x43FD1A 0x43FF85 0x4400AA; the view player's map 0x46778A 0x4677D0) in the fail-closed table (the
limits line); local (the cursor …; the build cursor 0x47D3B8; the feature helper …; the radar's
projectile dots …; the particles …; sound …) ARMED; the projectile pass (0x49BEE8) ARMED. Own row taken at 0x…, one LONG per function:
0x465AC0 0x408090 0x407E90 0x43F0E0 0x467440 0x4825B0 0x49BE60 0x43E490 0x47D2E0 0x4658E0 0x47F300
0x473590 0x474170 0x4745E0 0x475470 0x473A00 0x466DC0`, then the stacked aircraft's `enginefix: stacked aircraft
served to area damage after its walk (…) in the fail-closed table (the limits line). Aircraft served, counted
at 0x…`. The stubs are reported later, by the limits install, once the raised build's weapon
sites have taken theirs too: `enginefix: the fixes' and limits' stubs take N bytes in P page(s) of
4096`. A simulation fix reads `in the
fail-closed table (the limits line)` in place of `ARMED`, and the counters are `tacli peek`
addresses (the first two count the repeat hits stock would have made), and every grow or refusal of the frame logs its own line
(`enginefix: composite scratch grown|refused (<reason>) for <writer>: N px in R rows asked, A held, P
now`, and `composite scratch merge refused: a WxH cargo at (x,y) is past the WxH frame`); a load
that re-keys logs its first eight types and a total (`enginefix: unit sync keys: N of M types
re-keyed`); a saved game's load logs what the border fix did (`savedfeat: the restore ran with the
border mask open: N cells opened, M shut again and the pathing maps refreshed around each in T ms,
K hold a restored feature's anchor`, or why it left the restore to stock). Landing 6's five fixes
and B9 log a line of their own, `enginefix: one wind for every peer (0x490C5A 0x491903 0x4982CA 0x4C98FD) …; yardmaps
parsed inside their string (0x42CF5E) …; the saved-game order fallback (0x43A58D) …; the
stockpile bar's divide (0x439D41) …; a range circle of radius 1 (0x438EDE) …; an order mode
disarmed with nobody to order (0x49697B 0x499226) …. Counters: levels
seeded at 0x…, network levels seeded from the engine's session copy at 0x…, SetSessionDesc calls
made, made over another GUID and withheld at 0x… 0x… 0x…, network levels seeded from the map alone
at 0x…, yardmaps filled past their string at 0x…`, with the same readings in place of `…`. The maps fix logs `enginefix: maps with no terrain -- left out of the map list (0x434D87) and never kept as the saved skirmish map (0x430B87) ARMED. Maps left out counted at 0x…, saved maps set aside at 0x…`. Each level load logs its wind seed (`enginefix:
wind: level N seeded from the session {…} and the map "…" (0x…): seed 0x…; the engine's copy
main+0x479 agrees: {…}; SetSessionDesc so far: M made, D with another GUID in the engine's
descriptor, W withheld` (or `differs: {…}`), `… is a network game and DirectPlay's answer was refused (0x…, dwSize N): seeded from
the engine's copy main+0x479 {…} and the map …`, `… is a network game with no session: …
seeded from the map "…" (0x…) alone (K so far)`, or `… seeded from the
performance counter (game mode M, not a network game), map "…"`), and a def whose yardmap is filled past its string logs `enginefix: yardmaps: <name>: N of
its M cells lie past its YardMap string and take 0xXX (K so far)`, the first 32 and then every
power of two. A
site that is not installed reads `SKIPPED (the bytes differ from the retail exe)`,
`SKIPPED (its stub could not be made)` or `SKIPPED (VirtualProtect of the site failed)` in
place of a local fix's `ARMED`. The local fixes that take stubs of their own (the sort buffer, the
NULL plot, the terrain window, the composite scratch) release them when their sites cannot be
written. Flak's divides, the projectile pass's view, the local line of sight, the stockpile bar,
the range circle, the table's fixes
and, in the raised build, the limits' weapon sites take theirs from `fix_code`: each stub contiguous
inside one page, a stub that does not fit the current page opening another, and every page kept
for the process, since a simulation fix's table either goes in whole or ends the process at the
report. MEASURED on the tree merged with B6 (`2fc6528`): 4 208 bytes in two pages in the raised build,
3 856 in one in the stock-limits build. MEASURED with B3 (2026-09-25): 3 984 bytes in one page in the raised build, B3's 480 among
them; the nested references' two stubs and the `0x0A`'s add 64 (B3's 544 by the stub lengths, 4 048 in the page, not yet read live). MEASURED on the B2 branch: 3 504 bytes in one page in the raised build (the engine fixes'
2 992 and the weapon sites' 512); 3 120 in the stock-limits build, whose weapon-ID stubs are fixes',
measured before the line moved to the limits install, which takes none there. A scratch build cut
to 0x300 a page spread the engine fixes' 2 992 over five pages and played the stack fixture the
same. There
is no switch: every one of these fixes is installed on every launch, `tagpu_defaults.off` included. A skipped weapon logs
`enginefix: weapon <name> has ID <id>, outside 0..<max>, and is skipped` or `enginefix: weapon
<name>... (ID <id>) has a name longer than its record, and is skipped`, a duplicate `enginefix:
weapon ID <id>: <name> replaces <name>`, and a dropped message or companion `enginefix: weapon
IDs: <why> (<a>, <b>)` (the first 32, and the first 32 companions on a budget of their own, since
out of play a companion drops by design; one the gate refuses names the weapon and the in-play
handler's slot).

**What it does not cover:**

- A first network join at 16 383 types still takes about 12.5 s: the joiner's pace is raised
  (§2.6b), and what is left is the engine filling each type's unit-sync value on first use
  (`0x42A610`, which reads the type's script and GUI files). A rejoin in the same processes takes
  under 2 s. The 64-a-tick pace was measured between two instances on one machine, not over a
  lossy or slow link; it sends the same messages as stock, sixteen times as fast.
- A peer without the key fix, in a game with a mod whose keys collide, stalls the room as a joiner,
  as stock does, and as the host gives the re-keyed types no verdict, so their availability can
  differ between the peers: every peer runs the same build. A restriction list saved (`*.LST`,
  keyed on the key) names a re-keyed type by its given key, the first free value from a hash of its
  name, which moves when the types around it change; the entry then applies to whichever type holds
  the key.
- A unit's own frame, which the 1× bakes draw into through the same rasterisers, is sized by the
  model alone, so a model more than 800 rows tall still overflows the 800-row span table, with the
  fix as in stock. No stock unit is near it (the tallest is 239 rows). The engine map's *The
  composite scratch frame and the rasterisers' span tables* has the measurement.

- A full wreck pool still skips the feature's sequence, and with it the window in which further hits
  are ignored, so a weapon that hits every frame can take a building through several damage stages
  at once, and a peer whose pool is not full can end on a different stage. It still refuses corpses
  (`0x423C50`). The engine map's *The wreck pool* has the measurements.
- `0x40514A`'s reachability with a NULL plot is not audited. The guard covers it whether or not it
  is reachable.
- The terrain pass is handed a window stock gets wrong where the view is larger than the scroll
  extent (the engine map's "Where it is reached"). The camera clamp `0x41C3C0` inverts its range
  there; with `tagpu_zoom.on` the camera's own clamp replaces it and, with the command apply, holds
  the eye at 0, so the window leaves the map only where the view is larger than the map. The
  camera's centre range takes the eye below 0 at every left and top edge, but it is in force only
  on draws whose ground is ours (§2.3c). The engine's pass draws the ground on a level's first
  draws before our terrain pass takes it (MEASURED on Lava Run at 1920×1440), on every draw with
  `terrown.off`, and on every draw under `renderer=gdi`, where none of our passes draws [INFERRED,
  not run]. The zoom range the BAR camera replaced, `[−dx, extent − view + dx]` above 1, handed
  it a negative eye at every left and top edge as well: MEASURED on Two Continents at 1920×1080
  at zoom 2 with `terrown.off`, the eye at the NW corner of that range, −448 × −254, faulted main
  `09d7d55` at `0x4CBE44`, and an eye of −20 left a 20-px strip at that edge holding the last
  frame; with the bound, no fault and the strip black. The route is not patched; the bound
  covers it. The skirmish maps and resolutions are in the engine map. On the stock path the
  interior cells still go through the unclipped `0x4C6E70`; terrown's latch, not this bound,
  keeps vpwide's widened rect away from it (§2.3b). Only our path bounds the tile ids by the tile
  set's count; the stock path reads them unchecked.
- The camera clamp `0x41C3C0`'s inverted range is not patched in the engine's bytes. With
  `tagpu_zoom.on` it is replaced and held at 0; without it the engine alternates the eye between 0
  and the negative bound as stock does, and the terrain bound draws those draws. The pointer's path to `0x421E60` on such a
  map was not run.
- The off-map cell `0x498F2E` leaves in `main+0x2C8E` is untouched. Its readers are not audited.
- The stock past-the-end write at 1× was not reproduced: it needs more than `cap` units in one of
  the sweep's last rows, the margin below the view [INFERRED].
- Flak's zero divisor was not reproduced on stock: acquisition never aimed a flak gun above 29.6°
  in the runs, so the stub rests on the disassembly and its fallback counter has read 0. Where
  acquisition cuts off was not disassembled.
- A unit beyond the map's north or south edge stays invisible to other players' line of sight, as
  stock; only a unit whose own row is on the grid is fixed.
- A stock unit with `fw = 0` near the east edge already indexes a sort bucket one past the grid and
  near the west edge one before it; the bucket clamp now covers both, and neither was measured.
- The saved-game order fallback, the stockpile bar's divide and the range circle were not run:
  no save lacks an order's name, no stock stockpile weapon has a zero reload, the `ShowRanges`
  cheat could not be typed under injected input, and no stock unit has a range of 1. All three rest
  on the disassembly.
- The wind was measured with two peers and with three, the three-peer game with the host killed
  right after its own seed. That run did not exercise the engine's player removal during the load:
  DirectPlay never reported the killed host (its seat still held its type and ID 115 s of game
  time later), so the departure argument rests on the construction — the seed reads the session's
  GUID, which no departure writes, captured before the load's pump runs. A peer whose
  `GetSessionDesc` answer is refused takes the engine's copy, and with a null copy the map alone,
  and can then draw a different wind from the others; no refusal was seen. `GetSessionDesc` fails
  only with no interface or no open session, and such a peer exchanges no game traffic.
- The `SetSessionDesc` wrapper was measured only where nothing differed: in a two-peer game the
  host made 7 calls and the joiner 2, none over another GUID and none withheld. A lobbied launch,
  the case whose descriptor can name another GUID (`0x4C9B4F`), was not run; the construction
  covers it, since every `SetSessionDesc` the engine makes, the battle room's included, goes
  through the one site the wrapper replaces.
- The wind no longer draws from the sim RNG `0x4B6C30` or the CRT's `rand`, so each peer's sequence
  of those two moves by the draws stock made. Both are seeded per peer from its own clock — the sim
  RNG by the loader (`0x497180`), the game thread's CRT stream by WinMain (`0x49E8BB`) — so
  neither sequence was shared between peers before.
