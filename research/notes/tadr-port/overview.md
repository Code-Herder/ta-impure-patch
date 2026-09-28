# The TADR port

## Summary

TADR's `tdraw.dll` is roughly forty engine-feature modules for TA 3.1, which
[the merge exploration](../tadr-merge-exploration.md) sorts into five groups, A to E. The port
brings those features into the tagpu stack **as our own code**. This is the third route the
exploration describes (*rewrite it ourselves, review-and-improve each feature*), not adopting
`tdraw.dll` or vendoring its modules. Each feature group gets a plan page in this folder, then
landings. This page holds the rules every plan follows and the state of each group.

TADR's second DLL, the demo recorder (`tplayx.dll`, Delphi), patches the engine as well. The
exploration lists [the features of the recorder players actually run](../tadr-merge-exploration.md#the-shipped-features-by-port-group)
(build 3.9.2.416, not the unreleased 4.0 tree the source holds), sorted into the same groups, plus a
group F for the COB getters that only the recorder has.

The source is `vendor/TADR`, a gitignored clone of `github.com/tanvanman/TADR`, at `dcff5dd`
(2026-09-23). [The TADR deep dive](../deep-tadr.md) covers how that code works.

## Standing rules [DECIDED 2026-09-23]

These are the owner's rulings from the section-A grill. They apply to every group unless a plan
page records an exception.

1. **Multiplayer behaves as it does under TADR.** Every player is assumed to run the same build,
   and the game just plays. There is no handshake. A handshake is future work, not a
   prerequisite. Any multiplayer question a plan raises is answered with what TADR does today.
2. **Patches last for the whole process.** They are written once at `DLL_PROCESS_ATTACH` and never
   removed. TADR restores its bytes at detach, which buys nothing, since every other thread is
   gone by then.
3. **Fail closed.** A feature's sites are all checked against the stock 3.1 bytes before any is
   written. If one differs, nothing is written and the DLL exits with the report dialog (the text
   is in [the section-A plan](raised-limits.md#the-failure-report)). A player who is silently
   running stock rules is exactly the desync the same-build contract exists to prevent.
4. **No runtime opt-out for anything that changes the simulation.** To compare against stock, build
   a second DLL with a `make` flag and launch it with `tacli --keep-dll`.
5. **Our code, not theirs.** Features are re-implemented in C in our idiom (`patch_bytes`, the
   detour stubs). Every address is re-verified against `pristine/TotalA.exe.pristine` rather than
   copied. TADR is credited as prior art in the module header and in these notes. If a function
   ends up substantially ported, TADR's MIT notice goes beside it, per the roadmap's provenance
   rule.
6. **TADR's safety arguments are re-checked, not inherited.** CLAUDE.md's *Fixes must be safe by
   construction* applies to every ported feature. Values that come from the engine or the network
   are bounded before use. TADR's own claims are tested before we rely on them: its "visual-only"
   comment on the debris pool turned out to be half wrong
   ([evidence §4](limits-evidence.md#4-aux-debris-records-300-3000-addauxeffectpatches)).
7. **Stock behaviour stays exact below any raised limit.** Where stock behaviour can be kept
   exactly up to the old limit, it is, by construction. For example, the debris allocator stays
   first-free rather than switching to TADR's round-robin. Where it cannot, the plan says so: the
   flying pieces' ring allocator evicts by bytes, so its tenfold backing keeps pieces stock would
   drop ([the plan](raised-limits.md#the-landings)).

## The groups

| Group | What it is | State | Pages |
|---|---|---|---|
| A. Raised ceilings | projectiles, explosions, flying pieces, debris, units, pathfinding, particles, sounds, composite; and the wreck pool, beyond TADR | **done, all seven landings** 2026-09-24 (the effect pools, the module, the failure report; units 1500, pathfinding, the 15 001-slot design point; particles 20 480 a layer; sounds 32, the composite scratch 1280²; ten players at 1500 in one network game; the wreck pool 8192, and the fixes for a feature paid for and left standing and for a group reclaim paid once per builder, [fixed beyond TADR](raised-limits.md#fixed-beyond-tadr); and landing 7, the composite scratch's writers bounded by area and by the rows the rasterisers under them hold, growing the frame up to 2048² and falling back past it) | [plan](raised-limits.md), [evidence](limits-evidence.md) |
| A′. Content IDs | unit-type IDs 512 → 16 384, weapon IDs 256 → 4096, and the unit pass's caches the first one exposes | **done, all three landings** 2026-09-24: the unit pass's caches hold a frame's bound, 30 721 entries, and recycle from 512 / 1024; 16 384 unit-type slots, a mod past them refused at the count, the unit sync's keys made unique at load and the join's pace raised to 64 types a tick; weapon IDs 4096, the array a DLL static, `0x0D` and `0x0F` widened in place and `0x0E` joined by a companion message, with the `0x0F` sentinel clash and the receivers' bounds fixed in both builds | [plan](content-ids.md), [evidence §8–9](limits-evidence.md#8-unit-type-ids-512-16000-increaseunittypelimit-17-writes) |
| B. Simulation bug fixes | the stock engine defects TADR fixes, in any subsystem; its fixes to its own features, its gameplay changes and its diagnostics are not B | **done, all six landings** 2026-09-25, on local main after their reviews: damage (the victim caps, flak's divide, the map-edge off-by-one and line-of-sight shear), stacked aircraft, the wire's bounds, stale hits (an incarnation on the wire), the ghost commander, and loaders (wind, yardmaps, the save fallback, two UI divides); and **B7** 2026-09-26 with C3: a hit past the HP word, kill-outright sparing veterans, the radar's NULL read of a meteor's attacker, a meteor hitting once per peer. Sim fixes fail closed. Open: kill counts differ between peers | [plan](sim-fixes.md), [evidence](sim-fixes-evidence.md), [merge exploration §B](../tadr-merge-exploration.md#b-simulation-bug-fixes) |
| C. New data keys | weapon TDF flags, unit FBI keys, read through the engine's own TDF reader | **done, all four landings** 2026-09-25 and 2026-09-26 on local main after their reviews: the ghost's piece mask (the hides of `Create()`, `PreviewPieces=` as the override), the weapon keys (`nottoair`, `nottounderwater`, `surfacefire`, `notoverwater`/`notoverland`, `nomapweaponalert` as a silence), veterancy (`VeterancyThresholds=`, `VeterancyAccuracyBuffRate=`, "VetN"), and transported explosions (`TransportedExplodeAs=`, `TransportedSelfDestructAs=`, decided once a death and carried in the death record). TADR's key names and documented meanings; `Rotations=` and `reloadbar=` go to D with the features they switch on | [plan](data-keys.md), [evidence](data-keys-evidence.md), [merge exploration §C](../tadr-merge-exploration.md#c-new-data-keys) |
| D. UI / quality-of-life | megamap, ghost preview, rotation, drag-queue…; from the recorder, the camera, marker and income data behind tdraw's panels | not planned. The megamap stays excluded (the exploration's rule), and some items overlap our own renderer work, so the survey comes first. C's evidence pass already covers building rotation with `Rotations=` and the reload bars with `reloadbar=` ([Part 4 §1](data-keys-evidence.md#1-rotations-and-the-building-rotation-it-switches-on), [Part 2 §2](data-keys-evidence.md#2-reloadbar)) | [merge exploration §D](../tadr-merge-exploration.md#d-ui-quality-of-life) |
| E. Multiplayer / anti-abuse | start positions, vote-reject, share guard, anti-cheat…; from the recorder, the speed lock, autopause and ready, commander warp, `.take`, several AIs in one game, and whether allies share sight | not planned. Most of it needs the handshake that rule 1 defers; shared sight is the owner's decision | [merge exploration §E](../tadr-merge-exploration.md#e-multiplayer-anti-abuse), [the recorder's](../tadr-merge-exploration.md#e-game-control-unit-transfer-anti-cheat) |
| F. COB compatibility | the eight required `get` ids (32, 69–75), bounded execution capacity, validation and saved script state for the listed mods; Mayhem's inactive id 111 remains out of scope | **core landed 2026-09-28** on local main: all eight getters answered and asserted in-game, on both Wine peers of every listed mod; the full Wine suite passes on the landing DLL. Open: highest/recycled IDs, thread occupancy across the corpus, mod feature cases, dynamic multiplayer cases, shipped save fixtures and the Windows smoke checks | [plan and decisions](cob.md), [census](../tadr-merge-exploration.md#f-the-cob-getters) |

## Demo recorder

The [demo recorder](demo-recorder.md) page records the agreed multiplayer recording and solo
replay features, open architecture and performance decisions, and milestone progress. It is a
high-level feature record; detailed debugging and implementation evidence belong in separate
notes. The product decisions are recorded; recorder implementation and seek performance remain
unverified.

## Adding a page

Put a `.md` file in `research/notes/tadr-port/`. The wiki lists it in this section without further
setup. To choose its label and position in the sidebar, register it in `PAGES` in
`research/build_wiki.py`. Links are relative, and the same path works in the repo and on the
site: a sibling page is `raised-limits.md`, a top-level note is `../deep-tadr.md`.
