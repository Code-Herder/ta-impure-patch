# Weapon range rings — BAR's preview on TA's reach

*Outcome of the `/grill-me` interview, 2026-11-09, and the decision record since. The browser
mockup (`tools/tascene-ranges.js`, `scenarios/tascene-ranges.json`) is the reference, and the DLL
is built on the Vulkan lane: the module as it is now is [range rings](modules/range-rings.html).
Companion references: `engine/render.md` (the stock ring `0x438EA0`), `engine/weapons.md` (the
weapon record and the ballistic reach `0x49A890`), `tascene-design.md` (the lab),
`bar-camera-port.md` (the camera whose distance the fades read).*

## Decisions locked

| Branch | Decision |
|---|---|
| When rings show | **BAR's four cases, as BAR shows them:** the selected units (up to 358), the unit under the cursor (ally or enemy), a defense while it is placed (its ring at the snapped cursor, and every allied defense ring of its class lit so the coverage merges), and the defenses of the enemy and of allies by class. Builders show their build range while a build command is active. |
| Weapons per unit | One ring per weapon, coloured by class (ground, AA, cannon, lrpc, build range; water, paralyzer and D-gun styles). **Identical rings collapse** (same class, style, range and ballistics) — BAR draws one per weapon, which differs only in the faint inner ring doubling. |
| The look | **BAR exactly:** per class a faint merged fill (attack rings only), a merged outline of the union (3 px attack, 5 px defense), faint 1.8 px per-unit inner rings dimmed by the selection size; BAR's colours, camera-distance fades, map-edge fade and mouse-proximity highlight; rings on the water surface. No arc wedges: TA weapon data has no turret arc. |
| Defense set | **Derived from data, not BAR's hand list:** a unit that cannot move (`BMcode=0`) with a weapon. Class: an interceptor is `nuke` at its `coverage`, `toairweapon` is `air`, otherwise the attack rule. Defaults as BAR: enemies ground, air, nuke, cannon; allies nuke only. |
| Fogged enemies | **Only what the engine shows:** a ring for an enemy structure the game currently draws for the player; no memory of defenses once out of sight (BAR keeps them until seen dead). |
| Controls | **All of BAR's:** alt+. / alt+, cycle a per-unit-type on/off bitmap over that type's rings (one unit selected; ally and enemy kept apart), "shift only", "cursor range", the defense classes for allies and enemies. Stored in `impure.cfg` (`ranges`, `rangeshift`, `rangecursor`, `rangesensor`, `rangeally`, `rangeenemy`, `rangetoggle`). As built, every key but `rangetoggle` is a row of the in-game Visuals panel, which became two columns for them (the front-end screen's columns are full, so it has none). |
| The reach | **TA's engine rule, not BAR's formula.** See below. |
| Ring edge | **The first point the engine refuses**, per direction: march outward on the height grid, then refine. Everything inside is in reach; a reachable valley beyond an unreachable crest is not drawn. |
| Renderers | **Vulkan replaces the stock rings while the rings are on** (owner's decision 2026-11-09, reversing the interview's "deleted, no old mode"): `tagpu_order.c` draws the engine's `ShowRanges` set (`+showranges`, the labelled per-unit circles and the AoE/coverage circles at an order's target) only while `ranges` is off, so `ranges=off` brings it back as the engine draws it. The cloak and kamikaze circles of normal play are untouched. GDI keeps the stock engine behaviour and draws no rings. |
| Compute | **The game thread decides, the GPU walks:** the game thread publishes one 32-byte record per ring (centre, altitude, range, velocity, minbarrel, class, layer, style) and the render thread reads no engine memory. The reach is **a compute pass**, one workgroup a ring and one invocation a direction, over the terrain pass's own height grid; the draw stages only read its result. **A per-slot cache** keeps a ring's walk while its inputs and the grid, sea and gravity are unchanged, so a structure is walked once and a moving unit every frame. (The first build walked the terrain in the vertex stage, per vertex, every frame.) |
| Sensor preview | **BAR's sensor preview, placement only** (grill of 2026-11-09): while a building is placed whose type will emit once built — it switches on at completion (ActivateWhenBuilt) or the player can switch it on (onoffable) — its radar, sonar, radar-jam and sonar-jam coverage is drawn, per kind the sheet under the new ring, the viewed player's **existing** coverage of that kind as a muted fill, and one outline of their union; jammers dashed. **Existing coverage is the engine's emitter test**, not the type: the viewer's own units, alive, not dying and activated (`+0x10E` bit 0), with the kind's distance above 0 — so a radar switched off counts for nothing and a factory counts while it produces. For radar and sonar that is the radar pass's own test; for the jammers it is BAR's meaning, since the pass's jammer loop walks the other owners' jammers (`0x4675F5..0x467600`). **The radius is the radar pass's** (`0x467440`; radar capped by the bucket walk at max(radar, sonar)), a flat circle with no terrain. **Sonar is clipped to water**, where the pass finds sonar targets. **Under the weapon rings**, its own layer. **No preview on GDI**, as no rings. The setting `rangesensor` (on by default), the Visuals row *Sensor preview*. Allied coverage is not drawn: the engine's allied radars do not extend the viewer's. |
| Replays | Rings show in the replay viewer as BAR shows them to spectators. Presentation only: nothing enters the recording. |
| Mockup | The tascene lab (`rings=1`), on a real map with real weapon data, interactive, running the same algorithm — the reference the DLL is compared against. |

## The reach  [DISASSEMBLED 2026-11-09]

- **Range is horizontal.** Can-aim `0x49AA80` and can-engage `0x49ABB0` compare `dx² + dz²` with
  `range²` (weapon `+0xDC`, a dword); no altitude term.
- **A ballistic weapon (flag bit 1) must also pass the solver `0x49A890(dx, dy, dz, v, minbarrel)`.**
  `dy` = shooter altitude − target altitude, `d` = the horizontal distance, `v` = weapon `+0x68`
  (weaponvelocity × 65536/30: world units per tick in 16.16), `g` = `main+0x14263` (8155 in every
  measured game: 112 × 65536/900). With `D′ = v⁴ + 2·g·dy·v² − g²·d²`, the target is unreachable
  when `D′ < 0`. The roots `s± = (d²(v² + g·dy) ± d²√D′) / (2(d² + dy²))` are `v²·cos²θ`; each
  becomes `acos(√s / v)` — `0x4E67F0` is the CRT `acos` (it computes `atan2(√(1−x²), x)`), so a
  downhill pitch counts by its size — and a root ≤ 0 is π/2. An angle is accepted when
  `minbarrel < angle ≤ π/4`.
- **Consequences.** On flat ground a cannon reaches `v²/g` or `range`, whichever is less
  (Guardian: 1094 of its 1250). Downhill it reaches further, uphill less, never past `range`.
  The 45° cap also refuses targets close below a cliff, so the reachable set has an inner hole
  under a steep drop; the ring's first-refusal edge then sits at the cliff.
- **The gravity's source** is `[INFERRED]` to be the map's OTA `gravity` (112 on Two Continents,
  giving 8155); no store to `main+0x14263` was found by displacement.

## As built, and measured

The DLL side is built on the Vulkan lane: the game thread's half `tagpu_rangeown.c`, the look
`tagpu_range.c`, the compute pass `tagpu_range_comp.h` and the Vulkan pass `tagpu_vk_range.c`
([range rings](modules/range-rings.html) states them). Measured on the reference setup at
1024×768 with vsync off, on `scenarios/range-rings-stress.json` loaded with `--los 0` — 1000
CORPUN and 400 ARMMART, 358 of them selected, 1358 rings:

| | zoom 1.0 | zoom 0.25 |
|---|---|---|
| no rings (main's DLL) | 1260–1620 fps | 248–300 fps |
| the walk per vertex, every frame (the first build) | 137–169 fps | 83–102 fps |
| the compute pass with its cache | 647–695 fps (none selected) / 646 (Ctrl+A) | 104–167 fps |
| the compute pass, cache forced off (an A/B build: every ring walked every frame) | 575 fps | 134–140 fps |

Under a real move order only 127–151 of the 1358 rings were walked again per frame. A picture of
the compute build against the per-vertex build differed in 168 px at 6 % fuzz, the HUD counters.
The cost left at zoom 0.25 is fill, about a thousand discs. A live check on 2026-11-09 of
`b7-dgun` with `+showranges` and Shift held: `ranges=off` draws the stock set exactly as main's
DLL does, and `ranges=on` only the rings. Earlier in-game checks against the mockup: the enemy
cannon ring's kinks match to the pixel; the Guardian at zoom 0.5, 0.3 and 1.6; the commander's
D-gun ring at 252 with its laser merged and its build ring at 60; the hover union; the LLT's
placement ring with the builders' rings; Alt+. / Alt+, cycling 7 → 0 → 1 → 0 → 7 → 6.

**The sensor preview's stock types** (a read of the 278 stock FBIs over the retail install,
2026-11-09): the structures that will emit are the radar towers ARMRAD and CORRAD (1440) and their
advanced pair (2880), the sonar stations ARMSONAR (1180), CORSONAR (1223) and the advanced pair
(2250) — ActivateWhenBuilt and onoffable all — and the torpedo launchers (440–650 of sonar), which
are ActivateWhenBuilt, so placing one shows its sonar. The aircraft plants (500–1200 of radar) and
the Annihilator (1200) are neither, so placing them shows none; a plant emits only while it
produces. No stock structure jams; the six radar jammers and the one sonar jammer are mobile. What
was measured in game: [range rings](modules/range-rings.html), *How it is checked*.

## Gaps still open

- **GDI draws no rings.**
- **The fades at larger screens**: at 1024×768 TA's zoom range never reaches BAR's fade
  distances; a larger screen fades the inner rings at low zoom, which has not been seen live.
- **The replay viewer's display** of the rings has not been verified.
- **Fill rate at low zoom** with about a thousand discs.
- **The ground at the grid's edge**: the walk takes the nearest cell on the height grid's last row
  and column, where `GetPosHeight 0x485070` answers −1, and interpolates in float where the engine
  truncates in integer steps.
- **The sensor preview's jammer look** has not been seen in game: no stock structure jams.
- **The lab draws existing sensor coverage by type**: `tools/tascene-ranges.js` has no activation
  state, so a switched-off radar and an idle factory count there and not in the DLL.
- The shooter's altitude is the unit's position (`+0x6A`), as can-engage uses; can-aim's `from`
  may be a weapon piece position — not established.
- The water-weapon and above-sea gates of can-engage are not drawn: a non-water weapon's ring
  crosses water at the surface, a torpedo's crosses land.
- TA's stock anti-air towers (Defender, Pulverizer) are not `toairweapon` — they hit ground — so
  they draw ground rings. Only six stock weapons are anti-air-only (the flak guns and a few others).
- BAR's camera distance is stood in for by the ground the view shows: the distance at which BAR's
  45° camera shows as much height as this view does.
