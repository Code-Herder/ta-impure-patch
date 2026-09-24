# A. Raised ceilings — the plan

## Summary

Nine of the engine limits TADR raises come into our stack, as one C module (`tagpu_limits.c`) over
five landings. The owner decided every choice below on 2026-09-23 **[DECIDED]**. As of that date
nothing is built. The disassembly behind each fact is in [the evidence pass](limits-evidence.md),
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

**Multiplayer: TADR's behaviour.** In a network game each peer takes the host's unit limit at
`0x449D9B`, unclamped, and we follow it. Our own reads stay bounded. Past the frame packet's table
ceiling (16 384 slots, 1638 a player) the renderer draws what fits and logs the truncation. Above
6553 a player, the engine's 16-bit slot count `10·N + 1` overflows; that is the engine's own
failure, exactly as under TADR.

**Unit limit: 1500 default and ceiling.**
- The three immediates at `0x491640` (default), `0x491659` (compare) and `0x491666` (clamp-to)
  become 1500. A `UnitLimit` in `totala.ini` `[Preferences]` can still lower it, down to the
  engine's floor of 20.
- The mission loader's `maxunits` key (`0x432646`, unclamped) is clamped to [20, 1500].
- The render design point becomes 10 × 1500 + 1 = **15 001 slots**. It is 10 241 today:
  `TAGPU_PK_DESIGN_SLOTS`, `tagpu_packet.h:449`. Every cap
  [gpu-status §2.86](../gpu-status.md) sized against it follows.
- `tacli`'s scenario schema ceiling `SCN_MAX_LIMIT` (`tools/tacli:3073`) goes from 500 to 1500.
  `scenario-format.md`'s statement that the cap can never exceed 500 is replaced.

**One module, one table, one rule.** `tagpu_limits.c` holds one site table: address, expected
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

**The projectile compaction frame** `0x49AE20` grows from `0x4C0` to `0x2EF0` bytes of stack.
TADR's naked stack probe does this correctly by construction. The alternative is a C replacement
over static arrays, which is safe only because the function is game-thread-only and not reentrant.
Landing 1 decides between them, after checking both callers.

**No runtime opt-out.** A `make` flag builds the stock-limits DLL under its own name for the
comparisons; it is launched with `tacli --keep-dll`. No lever file and no store key exist for it.

## The failure report

If any site differs, the DLL writes nothing. At the first DirectDraw call (after the loader lock,
before the game window exists) it shows this MessageBox and exits. The same text goes to
`log\startup-failure.txt`. Ctrl+C copies a MessageBox's text on Windows and on Wine. The owner
approved this draft and reviews the final wording in landing 1.

```text
Title: Total Annihilation: Impure cannot start

Impure could not install its engine limits, so Total
Annihilation will now close. Nothing was changed.

WHY
Impure raises the game's limits (units, projectiles,
explosions...) by rewriting its code in memory, and it
checks every place first. This TotalA.exe is not the
Total Annihilation 3.1 that Impure is built for.
Running anyway would let this game play by different
rules from other players and break multiplayer
without warning.

WHAT TO DO
- Use the original 3.1 TotalA.exe (the Steam and GOG
  copies are 3.1). Community patches such as 3.9.02 and
  TA: Escalation ship a modified exe.
- Or report it: press Ctrl+C to copy this message and
  paste it into a new issue at
  github.com/Code-Herder/ta-impure-patch/issues
  The same report is saved in log\startup-failure.txt

--- report ---
impure  <commit> (<branch>)
exe     TotalA.exe  <size> bytes
        md5 <md5>  PE stamp <stamp>
        known build: <name or none>
result  <k> of <n> sites differ, nothing written
  <address> <site name>
             want <stock bytes>  have <found bytes>
```

- **The report block exists for whoever debugs it, person or agent.** It names our build and
  identifies the exe exactly. It lists *every* differing site, not just the first. It prints no
  path, because a report is meant to be pasted publicly and an install path carries a user name.
- **`known build`** is looked up from a small compiled-in table of md5s: retail 3.1, and 3.9.02
  once measured.
- **Unverified: "the Steam and GOG copies are 3.1".** The Steam copy matches the retail 3.1 exe.
  The GOG copy has not been checked. Verify it or cut it before landing 1.

## Landings

| # | Content | Proof |
|---|---|---|
| L1 | the module and the failure report; projectiles, explosions, flying pieces and debris; the frame packet follows the moved pools; the stock-limits `make` flag | **below the caps nothing changes**: `tagpu_cobtrace` byte-identical between the stock-limits DLL and ours, on a scenario under every stock cap. **Above the caps it works**: a scenario past 300 projectiles and explosions, with the engine count equal to the packet count. **Two peers agree**: a two-instance game (`tools/mp_lobby.sh`) pushed past the caps, `tacli` rosters compared at the same tick. `high` review |
| L2 | units 1500 and the `maxunits` clamp; the design point to 15 001; `tacli` to 1500; pathfinding 66 650 | **tier 1**: a 4-player skirmish at 1500 each (6000 units; a skirmish seats four), measuring frame time and every pool's peak |
| L3 | particles: the per-layer cap and the object pool | the value is set from tier 1's per-layer peaks plus headroom; `LAYER_OBJCAP`, `TAGPU_PK_MAX_PART` and `PART_SUBCAP` follow |
| L4 | `MixingBuffers` 128 as an `impure.cfg` key (read at `0x42FE4F` inside the registry loader `0x42F9A0`, which the store already observes); the composite buffer | a GDI-lane comparison for the composite |
| L5 | `mp_lobby.sh` extended to N instances | **tier 2**: a 10-player network game at 1500 each (15 000 units), the proof of the 15 001-slot design point |

L1 comes first because it forces the module, the report and the packet changes into existence.
L2 comes before L3 because the particle measurement needs the raised unit limit. TA replicates
state and events rather than running in lockstep
([networking-lobbies](../networking-lobbies.md)). Comparing rosters on both peers is therefore the
agreement test; TA itself detects nothing.

## Open questions

- The GOG exe (see *The failure report*).
- `0x49AE20`: TADR's stack probe or a C replacement. Landing 1 checks both callers first.
- Whether a refused remote projectile changes damage on that peer
  ([evidence §1](limits-evidence.md#1-projectiles-300-3000-enginelimitscpp-addprojectilepatches)).
  L1's two-peer check covers only the same-build case, which is the contract.
- `0x44CAFE`: TADR writes the unit limit over the per-type value 101 there. If the engine treats
  101 as a real cap, then in a network game at 1500 a player no single unit type could pass 101.
  L2 settles it by building 102 of one type in a two-peer game, and writes the site as TADR does
  if the cap is real.
- The particle headroom rule: decided in L3, from the data.
- Pathfinding cost at 6000 units: tier 1 measures it.

## Corrections this plan owes

- **`0x44CAFE` is not a unit-limit site** (`mov ecx,0x65`: 101 into the per-type battleroom table;
  read as the "unrestricted" sentinel [INFERRED]). Corrected in [deep-tadr](../deep-tadr.md) on
  2026-09-23.
- `tagpu_packet_pub.c:997` says "twelve more" particle cap sites; there are twenty. Landing 1
  fixes it.
- The engine map says the effect arrays are simulation state that the draw passes only read. It
  does not record that the explosion and debris caps gate synced random-number draws. Landing 1
  adds that.
