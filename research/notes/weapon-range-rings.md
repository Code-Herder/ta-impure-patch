# Weapon range rings — BAR's preview on TA's reach

*Outcome of the `/grill-me` interview, 2026-11-09. The browser mockup is built (`tools/tascene-ranges.js`,
`scenarios/tascene-ranges.json`); the DLL side waits for the owner's approval of it. Companion references:
`engine/render.md` (the stock ring `0x438EA0`), `engine/weapons.md` (the weapon record),
`tascene-design.md` (the lab), `bar-camera-port.md` (the camera whose distance the fades read).*

## Decisions locked

| Branch | Decision |
|---|---|
| When rings show | **BAR's four cases, as BAR shows them:** the selected units (up to 358), the unit under the cursor (ally or enemy), a defense while it is placed (its ring at the snapped cursor, and every allied defense ring of its class lit so the coverage merges), and the defenses of the enemy and of allies by class. Builders show their build range while a build command is active. |
| Weapons per unit | One ring per weapon, coloured by class (ground, AA, cannon, lrpc, build range; water, paralyzer and D-gun styles). **Identical rings collapse** (same class, style, range and ballistics) — BAR draws one per weapon, which differs only in the faint inner ring doubling. |
| The look | **BAR exactly:** per class a faint merged fill (attack rings only), a merged outline of the union (3 px attack, 5 px defense), faint 1.8 px per-unit inner rings dimmed by the selection size; BAR's colours, camera-distance fades, map-edge fade and mouse-proximity highlight; rings on the water surface. No arc wedges: TA weapon data has no turret arc. |
| Defense set | **Derived from data, not BAR's hand list:** a unit that cannot move (`BMcode=0`) with a weapon. Class: an interceptor is `nuke` at its `coverage`, `toairweapon` is `air`, otherwise the attack rule. Defaults as BAR: enemies ground, air, nuke, cannon; allies nuke only. |
| Fogged enemies | **Only what the engine shows:** a ring for an enemy structure the game currently draws for the player; no memory of defenses once out of sight (BAR keeps them until seen dead). |
| Controls | **All of BAR's:** alt+. / alt+, cycle a per-unit-type on/off bitmap over that type's rings (one unit selected; ally and enemy kept apart), "shift only", "cursor range", the defense classes for allies and enemies. Settings in a *Range rings* block of Visuals, stored in `impure.cfg`. |
| The reach | **TA's engine rule, not BAR's formula.** See below. |
| Ring edge | **The first point the engine refuses**, per direction: march outward on the height grid, then refine. Everything inside is in reach; a reachable valley beyond an unreachable crest is not drawn. |
| Renderers | **Vulkan replaces the stock rings:** the stock Shift + `+showranges` rings and `tagpu_order.c`'s redraw of them are deleted, with no "old mode". GDI keeps the stock engine behaviour. |
| Compute | **BAR's split:** the game thread publishes one small record per ring (centre, altitude, range, velocity, minbarrel, class, alpha); the vertex shader drapes it and runs the solver against our own copy of the height grid. The render thread reads no engine memory. |
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

## Gaps the mockup leaves open

- The shooter's altitude is the unit's position (`+0x6A`), as can-engage uses; can-aim's `from`
  may be a weapon piece position — not established.
- The water-weapon and above-sea gates of can-engage are not drawn: a non-water weapon's ring
  crosses water at the surface, a torpedo's crosses land.
- TA's stock anti-air towers (Defender, Pulverizer) are not `toairweapon` — they hit ground — so
  they draw ground rings. Only six stock weapons are anti-air-only (the flak guns and a few others).
- BAR's camera distance is stood in for by the ground the view shows: the distance at which BAR's
  45° camera shows as much height as this view does.
