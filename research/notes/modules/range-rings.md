# Weapon range rings

BAR's weapon range preview on the Vulkan renderer, on TA's own reach rule: the rings of the
selected units, of the unit under the cursor, of the builders while a building is armed, of the
building being placed, and of the defenses by side and class, each class's rings merged into the
outline of their union. Under them, and independent of them, BAR's sensor view: the radar, sonar and
jammer coverage of the viewed player and its allies, always, as dashed outlines of each kind's union
laid on the ground, by the engine's own emitter rule; and while a building that will emit is
placed, BAR's sensor preview of its coverage over the existing coverage. The rings replace the
engine's `ShowRanges` circles while they are on.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`. The decisions and the reach's derivation are the plan note's, [weapon range rings](../weapon-range-rings.html); the engine side is in the engine map: the weapon record and the ballistic reach `0x49A890` in [weapons](../engine/weapons.html), `UnitInPlayerLOS 0x465AC0` in [map](../engine/map.html), HotUnits, the activation byte `+0x10E` and the alliance row `+0x108` in [units](../engine/units.html), the radar pass `0x467440` in [recorder](../engine/recorder.html), the activation's script starts in [cob](../engine/cob.html), the placement fields in [orders](../engine/orders.html) and [render](../engine/render.html) (`0x4197D0`). Every field the module reads is a row of the [hook map](hook-map.html) §2.5; it writes no engine byte and installs no hook.

## The files

| file | thread | what |
|---|---|---|
| `tagpu_rangeown.c/.h` | game | which rings show, of what class, around which unit, and which sensor coverage — the view's and the preview's; every engine field resolved to a number in the packet; the Alt+. / Alt+, keys |
| `tagpu_packet.h`, `tagpu_packet.c`, `tagpu_packet_pub.c` (`fill_range`) | game / render | `TAGPU_PK_RANGE` (one 24-byte header) and `TAGPU_PK_RING` (32 bytes a ring, at most 4096, 128 KB) in the frame packet; the acquire's bounds |
| `tagpu_range.c/.h` | render | BAR's look: each ring's colour, widths, fades and highlight; the sensors' union outlines built as paths and laid on the ground, their dashes, fills and on/off animation; 64-byte instances, 32-byte path points (`TAGPU_RGPT`) and the draw list |
| `tagpu_range_comp.h` → `inc/spirv/tagpu_range_comp.spv.h` | GPU | the reach, a compute pass: one workgroup a ring |
| `tagpu_range_glsl.inc` | GPU | the disc and band vertex stages and their fragment stage, which read the reach and place the points (a sensor disc's extent cut, water test and the preview's travelling rings); the sensor paths' own pair `SVS`/`SFS`, a band along a path, dashed or solid |
| `tagpu_vk_range.c/.h` | render | the Vulkan pass: the per-slot reach cache, the dispatch, the stencil union, the per-slot path buffer |
| `tagpu_terr.c/.h` (`tagpu_terr_heights`) | render | hands the terrain pass's height mirror to a later reader on the render thread: the ground the sensor outlines are laid on |
| `tagpu_weapons.c` (`tagpu_weapons_type_weapon`) | game | a unit type's weapon `idx` (weapon1..3, then the extra-weapons record), NULL past the type's count; reads only and builds nothing |
| `tagpu_settings.c` | game, render | the keys below and `rangetoggle=` |
| `tagpu_menu.c` | game | the ten rows of the in-game Visuals panel (*Settings*) |
| `tagpu_shield.c` | game | routes Alt+. / Alt+, to the module on both paths, injected and real |
| `tagpu_order.c` | game | the engine's `ShowRanges` circles, drawn only while the rings are off |

`tagpu_rangeown.c` is listed in `thread-split.allow` as `publisher presentation`, a line the
owner approved: every engine word it reads is read on the game thread, inside the in-play draw,
and reaches the render thread only as numbers in the frame packet. It is presentation: nothing in it is read by the
simulation and nothing enters a recording.

## Which rings show (`tagpu_rangeown_fill`)

Inside the in-play draw's packet fill, on the game thread. The world is bounded first, as
`tagpu_route.c` bounds it: the viewed player `main+0x2A43` below ten, UNITINFOCount
`main+0x1438F` nonzero and inside the raised type limit, the unit array's begin and end pointers
a whole number of 0x118-byte records apart and agreeing with the slot count `main+0x14351`. A
unit is a slot `1..count−1` alive (`+0x110` bit 28) and not dying (bit 14); its def a row of
the def array; its weapons through `tagpu_weapons_type_weapon`, each a record of the weapon
array and not the no-weapon entry 0 (`tagpu_limits_weapon_index`).

**Shown means what the engine shows the viewed player**: a unit of the viewed player's own, or one
`UnitInPlayerLOS 0x465AC0(viewed player's record, unit)` passes — the test the HotUnits cull
takes at `0x48BC42..0x48BC56`, so a ring stands where the engine draws a unit and nowhere else.
That includes its refusal of a unit whose model is wholly under water and is not a sonar contact
(`0x465B38`). Allied units go through the same test for their rings; their sensor coverage takes
none (below).

**What is filled.** The weapon rings only under `ranges`; the sensors under their own two keys,
whatever `ranges` says. The fill runs while any of `ranges`, `rangesensor` and `sensorview` is on
(and the lane draws, *The lane*), and sets the header's `TAGPU_PK_RG_SVIEW` while `sensorview` is.

**Allied**, for the rings (`allied`): the viewed player itself, or an owner whose byte in the
viewed player's alliance row `+0x108 + owner` is 1. **For the sensors the alliance must hold both
ways** (`ally_both`): that byte, and the owner's own row toward the viewed player, `+0x108 +
viewer` of the owner's record (the owner bounded below ten first, since it indexes the player
array). The rows are one-way, and the radar pass shares contacts by the **owner's** row toward the
viewer (`0x4674BA..0x4674C5`) and then the owner's ShareRadar bit, PlayerInfo `+0x97` bit `0x40`
(`0x4674D2`); the viewer's row alone would draw the coverage of a player who allies nobody back.
The ShareRadar bit is not read: an ally's coverage shows whether or not it shares radar.

**The sensor layer** (`TAGPU_PK_RL_SENSOR`, drawn first, under everything below):

- **the view**, under `sensorview`, always: every unit of the viewed player **and of its allies both ways**
  — alive and not dying (`unit_at`) — in each of the four kinds whose distance, a signed word, is
  above 0: radar def `+0x204`, sonar `+0x206`, radar jam `+0x20A`, sonar jam `+0x20C` (classes
  `RC_RADAR` .. `RC_SONARJAM`). A unit **activated** (`+0x10E` bit 0) is published plain; a radar
  or sonar unit that is not is published flagged `TAGPU_PK_RF_OFF`, so the render thread can
  shrink its circle away (*The look*); a jammer that is not is not published. For the viewed
  player's own radar and sonar that is the radar pass's emitter test, `0x467521..0x467542` and the
  word tests after it ([recorder](../engine/recorder.html), `0x467440`). The allies' coverage and
  the jammers are counted by the same test, which is BAR's meaning rather than the pass's: the pass
  walks only the viewer's own emitters, so an ally's radar does not extend the viewer's own
  contacts (its *units* are shared as contacts when the ally shares radar, loop 1 of `0x467440`),
  and its jammer loop walks every *other* owner's activated jammers (`0x4675F5..0x467600`), so a
  player's jammers act in the other players' passes. **No line-of-sight test** for an ally's
  unit, and **an enemy's coverage is never published**, its jammers included. Moving units and
  aircraft count as structures do: activation is the engine's state, not the type's — a radar
  switched off with its button contributes nothing, a factory contributes its def's radar while
  its build order runs, which is when BuildingBuild `0x402640` keeps it activated (`0x4027AE`),
  and the VTOL order handlers set the bit, in flight [INFERRED] — an ARMPEEP standing on the
  ground reads 0 ([units](../engine/units.html), `+0x10E`; *How it is checked*);
- **the preview**, under `rangesensor`, only in placement — mode `main+0x2CC3` 14 with the pointer
  on the world, the type `main+0x2CC4` at the snapped site as the defense rings take it: the placed
  type's coverage, flagged `TAGPU_PK_RF_NEW`, when the type **will emit once built**: it switches
  on at completion (ActivateWhenBuilt, def `+0x241` bit 18, the bit the completion `0x41B8D0`
  reads at `0x41B9AC`) or the player can switch it on (onoffable, def `+0x245` bit 2, which the
  Activate handler `0x403010` reads at `0x40301A` and Deactivate `0x403040` at `0x40304A`). One
  record per kind whose distance is above 0. A factory is neither, so placing an air plant shows
  no radar though a working one emits it. With `sensorview` off, the existing coverage of the
  placed type's kinds is published by the view's rule for the preview alone; with it on, the
  view's records are the preview's existing coverage.

**The radius** is the one the pass tests. Sonar and both jammers: the distance. Radar:
`min(|R + 2·altitude|, max(R, sonar))` — the contact test is `d² < (R + 2y)²` (the square built
at `0x46755E..0x46757C`), but the bucket walk only offers units within `max(R, sonar)`
(`0x46757F..0x4675A3`), so the altitude bonus shows only past a larger sonar, which no stock unit
has. The coverage is a plain horizontal circle at the exact radius, not rounded to any cell: no
terrain hides anything (the walk's and the visitor's tests are `dx² + dz²`). The placed type's
altitude is the site's, an existing unit's its own. The placed type's records are emitted right
after its defense rings and the existing coverage last of everything, so a full table drops
existing coverage first.

**The defense layers**, drawn next:

- every structure (`+0x110` bit 29: the def's `bmcode` `+0x22F` is 0) with a weapon, shown, its
  rings filtered by side: an allied one by `rangeally`, an enemy one by `rangeenemy`, each a set
  of classes, allied by the rule above;
- **the building being placed**: with mode `main+0x2CC3` 14 and the pointer on the world
  (`main+0x2CC6` bit 1), the type `main+0x2CC4` gets its defense rings at the centre of the
  engine's own snapped site `main+0x2C92..+0x2CA6` (x and z the two corners' mean, the altitude the
  first corner's), and every class it has is added to the allied filter, so the allied defenses of
  those classes light up and the coverage merges.

**The attack layers**, under `rangeshift` only while `KeyboardHotkeySampler 0x4C1B80(0xF9)` says
Shift is down:

- the first 358 selected units (`+0x110` bit 4) in slot order, shown — BAR's
  `selectionDisableThreshold` 512 × 0.7; the rest show none;
- with `rangecursor`, the unit under the cursor `main+0x2CBA` when it is not selected and is
  shown. Not in mode 14: the in-play handler skips the hover query there (`0x499241` jumps past
  it), so the word is stale;
- in mode 14, the build ring of every unselected builder of the viewed player's own (def `+0x241`
  bit `0x40`).

A unit's rings carry the per-type choice of Alt+. / Alt+, (below), except the builders' build
rings in placement.

**A type's rings.** One per weapon, identical rings collapsing (class, style, range and
ballistics all equal). Attack: a `dropped` weapon (`+0x111` bit 8) or an `interceptor` (bit 30)
gives none; `toairweapon` (bit 17) is AA; a `ballistic` weapon (bit 1) is cannon, lrpc past 2600;
anything else ground, lrpc past 2600. Styles: `paralyzer` (bit 7) emp, `commandfire` (bit 26)
without `stockpile` (bit 28) the D-gun, `waterweapon` (bit 16) water. A builder with
`builddistance` `+0x212` above 0 adds its build ring. Defense, only for a type that cannot move:
an interceptor is a nuke ring at its `coverage` `+0xE0`, `toairweapon` an air ring, anything else
the attack rule's ground, cannon or lrpc; `dropped` gives none. The range is `+0xDC`, clamped to
32 767; a ballistic ring carries `+0x68` / 65 536 (world px a tick) and `+0xC8` (radians), and a
velocity outside (0, 4096] gives no ring, since the engine's solver reaches nothing with it.

**The header** carries the counts BAR's inner-ring dimming reads (the selected units shown, the
builders' build rings shown), the gravity `main+0x14263` as world px a tick squared (a value
outside (0, 65 536] in 16.16 publishes no rings at all), and the pointer's world point
`main+0x2CAA` while it is on the world. Each record's `uidx` is filled by `fill_range` from the
slot map `fill_world` wrote the same frame, so the render thread takes the unit's interpolated
position; a slot that walk did not reach keeps the record's own position.

**The acquire** (`tagpu_packet.c`) refuses a packet with more than one header, rings without it,
more than 4096 rings, a coordinate outside ±`TAGPU_PK_PLACE_COORD`, a range outside 1..32 767, an
enum past its last value, gravity outside (0, 1], or a ballistic ring with a velocity outside
(0, 4096] or a barrel angle outside ±3.2. A frame with more rings than the table holds sets
`TAGPU_PK_TRUNC_RING`.

## Alt+. and Alt+, (`tagpu_rangeown_key`)

BAR's `cycleUnitDisplay`: with exactly one unit selected, its type's rings on its side (ally or
enemy, kept apart) are a bitmap, and Alt+. adds one, Alt+, subtracts one, modulo 2ⁿ for a type
with n rings. The log says `range: NAME (ally) shows ring config N of M`; with no unit or several
selected, `range: alt+. needs exactly one selected unit`. The choice is kept by unit name and
saved as `rangetoggle=` — from the next fill, which does not wait for the store's lock, or at
the level's end, which does; a type back at every ring on both sides loses its entry, and an entry
whose ring count no longer matches the type is ignored.

The keys are taken in play only (from the level's first in-play draw to its end), with the rings
on and a lane that draws them (*The lane*, below), and with Alt held — `WM_SYSKEYDOWN` on the real path, `0x4C1B80(0xFB)` for an injected key.
**A taken key never reaches TA**, which would otherwise page the build menu with it (as
`tagpu_shield.c` states; the stock key handler's NEXT and PREV calls are `0x49608F` and
`0x496083`, but which key reaches them was not traced in the disassembly). `period` and `comma`
are tacli key tokens (`tagpu_input.c`), so `alt+period` drives it.

## The look (`tagpu_range.c`)

BAR's tables, ported from `gui_attackrange_gl4.lua` (the attack widget) and
`gui_defenserange_gl4.lua` (the defense widget): per class a colour, an outer and an inner width
in screen pixels, an outer and an inner alpha, a camera-distance fade and the segment count (128,
or 1024 for AA, lrpc and nuke). The attack widget draws a faint fill (0.12 of the colour's alpha
times the inner alpha); the defense widget none.

- **The camera distance** BAR's fades read is the view's height in world px over 2 tan(22.5°)
  (`camDist = (vh / zoom) / 0.82842712`): the distance at which BAR's 45° camera shows as much
  ground. An attack ring's outer line fades 2500 later than its inner one.
- **The mouse highlight**: a smoothstep from 48 to 64 world px between the pointer's world point
  and the ring's centre, five times as strong on a defense ring.
- **The inner rings dim** by the selection's size: `dim·0.1·n` with n the selected units shown
  (the builders shown for a build ring) clamped to 1..25, and the alpha times `min(1, 1/dim²)`.
- **The D-gun**: alpha × 3, clamped to 0.1..1, a fade 1000 later, and the radius × 1.05.
- **The map's edge**: a line fades to nothing over 50 world px outside the scroll extent.

The widths reach the target through the composite's device-pixel scale and the supersample, with
half a target pixel of rim each side.

### The sensors

BAR's sensor widgets on the engine's coverage rule: the view is `gui_sensor_ranges_radar.lua` and
`gui_sensor_ranges_sonar.lua` (their `sensor_ranges_los` shaders' colours) for radar and sonar and
`gui_sensor_ranges_jammer_preview.lua`'s always-shown layer for the jammers; the preview is
`gui_sensor_ranges_radar_preview.lua` without its terrain raycast, and sonar on its structure in
the sonar colours. `tools/tascene-ranges.js`'s `DASH`, `SENSOR_LINE`, `JAMMER`, `PREVIEW` and
`PULSE` are the same tables as `tagpu_range.c`'s. Sensors take no camera fade, highlight or
dimming. Every width, dash and speed is BAR's px of a screen times its `lineScale`,
(screen height in device px + 500) / 1300, so they hold on screen at every zoom.

**The outline of a kind is its union's, as paths.** `open_arcs` finds each circle's arcs that no
other disc of the kind covers (a circle inside another has none; of two identical ones the earlier
is kept, so the preview's own circle, first, keeps its arcs); `outline` chains them into closed loops — the next arc is the one of another circle
starting within 1 world px of where this one ends — so the dashes run on through a junction as
along any curve; `make_path` lays each loop on the ground as points (`TAGPU_RGPT`, 32 bytes): a
circle's 512 points a turn, 1024 past a radius of 1500, at least two an arc. A point carries its
world x and z, the surface altitude there (the sea's over water), its distance along the path on
the zoom-1 screen in game px (x, and z less half the altitude: the projection's own lift, so a
slope stretches the dashes as it stretches the ground), the band's direction (the mitre of its
two segments, pointing away from the point's own circle, lengthened to keep the band's width, at
most 1/0.35), and whether it shows: on the map, inside the scroll extent, and for a sonar kind
over water. **The ground is the terrain pass's height mirror**, read on the render thread through
`tagpu_terr_heights` — one byte a 16-px corner, bilinear as `GetPosHeight 0x485070` blends, the
nearest corner past the grid's last row and column. Its lifetime: the buffer is freed or replaced
only by `build_height`, which only `tagpu_terr_gather` reaches, and `tagpu_range_render` runs later
in the same frame on the same thread (`tagpu_native.c`), so it reads it whole (`tagpu_terr.h`).
With no grid nothing of the sensors is drawn, logged once (`range: no height grid on this
thread`). **The paths are rebuilt whole only when what they are made of moves** — the grid's
serial and size, the sea level, the scroll extent, `sensorview`, the placed type's kinds, and
every record's kind, flag, centre and radius, compared bit for bit — and only then does the
hand-over's `ptSerial` move; a frame whose sensors stand still re-uses them and uploads nothing.

**The dashes** are the route dots' level of detail (`tagpu_order.c`) on a path: every dash is the
same length on screen at every zoom, and where they stand is pinned to the map, on a lattice of
levels — level j a dash every s0·2ʲ along the path, each level every other dash of the one below,
s0 dividing the path's length evenly so every level closes round a loop, the coarsest putting 4..8
dashes along a path at least four spacings long. The zoom picks the level, its octave, held 0.65 of an octave past a boundary, so
the spacing on screen stays within about a factor of two; a level change fades the dashes it drops
or adds over 300 ms and moves none. Nine levels are drawn a frame. Radar and sonar: dashes 4 px
long every 11 px at level 0, a band 1.5 px wide, moving 9 px a second on screen; the jammers'
dashes 5 px every 11 px, moving 6 px a second the other way round. **The two clocks are BAR's**:
the radar and sonar dashes advance with the game (`tick`, a thirtieth of a second at normal speed),
so they stop on pause and run faster at a higher game speed; the jammers' dashes, the level fades
and the preview's travelling rings run on the wall clock. A step back in time, or one of more than
a second (30 ticks on the game's clock), moves nothing.

**The view.** Radar and sonar: a dashed band just **outside** each kind's union, radar green
(0, 1, 0) and sonar light blue (0.5, 0.7, 0.9) at alpha 0.16 and 0.17 times the dash's gain of 2.5.
The jammers: a **red fill** of the union — the placed jammer's (0.90, 0.22, 0.20), the existing
ones' (0.62, 0.35, 0.33) at 0.85 of its alpha 0.17 — one stencil bit a kind, so an overlap fills
once and the placed disc, drawn first, takes its own ground; each disc short of its circle by the
band's width, so **the fill stops where the band starts**; and a dashed band just **inside** the
union, (1.0, 0.32, 0.22) at alpha 0.8, 1.2 px of a 1080-high screen plus 0.89 world px wide. With
`sensorview` off a placed jammer draws its own fill and band alone.

**The preview**, per placed radar or sonar kind: the new disc's **sheet** (radar (0.22, 0.85,
0.50), sonar (0.40, 0.60, 0.85), alpha 0.2) with **rings travelling outward** across it — a glow
`pow(0.5 + 0.5·cos(2π(d − travelled)/180), 4.5) × 0.8` of the distance d from the centre that
raises the alpha and turns the colour toward the pulse's (radar (0.55, 1.0, 0.70), sonar (0.70,
0.85, 1.0)), travelling 85 world px a second; the existing coverage, where the new disc does not
reach, in the **duller** colour (radar (0.35, 0.62, 0.50), sonar (0.35, 0.48, 0.62)) at 0.55 of
the sheet's alpha; and a **solid outline**, 2 px of a 1080-high screen, just inside the union's
edge (radar (0.45, 1.0, 0.57) at 0.40, sonar (0.50, 0.70, 0.90) at 0.45), plus the new disc's own
arcs inside the existing coverage, as open paths. A placed jammer joins the jammer layer in the
brighter sheet colour.

**Going on and off.** A radar or sonar circle grows from nothing to its radius over 15 game ticks
when its unit switches on and shrinks to nothing over 15 when it switches off (`TAGPU_PK_RF_OFF`),
BAR's widgets' animation; a unit that is gone is dropped at once, and the placed type and the
jammers are not animated. The scale is kept per unit slot and kind on the game's clock — the
packet's tick plus the fraction of the next one since it started, a visual weight that never
reaches an index — so a paused game holds it. A slot not drawn on the frame before starts over,
growing from nothing if it is on, except when the whole layer is starting — the first frame, a new
level, another viewed player (the header's `viewer`: a replay's perspective switched, paused or
not), a jump in the game's time of more than 30 ticks or backwards, or the frame after one with no
sensor records (a frame with no ring records at all counts) — when every circle is at its end at
once.

**The draw order** within the layer: the jammers' fills and bands, the view's radar and sonar
dashes, then the preview's sheet, fill and outline; the weapon rings over all of it. **A sonar
kind shows over water only**, since the pass contacts a sonar target only at or below sea level
(`0x4678E6`): its fill per pixel (*The pass*), its band per path point. **Every sensor disc is cut
to the scroll extent**, the map's own edge, and a path's points past it do not show.

**The caps**: 512 circles an outline, 4096 arcs an outline, 256 paths and 65 536 points a frame
(`TAGPU_RG_MAXPT`). Past the circle cap the kind is not drawn at all (a union of only some of its
circles would draw edges inside the coverage); past the others some outlines are not drawn. Either
way the log says so once (`range: the
sensor outlines are past …`). The draw list holds 512 draws (`TAGPU_RG_MAXDRAW`), a path one each;
a draw past it is dropped without a log line.

## The reach — a compute pass (`tagpu_range_comp.h`)

One workgroup a ring, 64 invocations striding over its directions; each direction writes the
radius and the surface altitude there, `(r, y)`, into the slot's reach buffer, which every draw of
the ring reads. A non-ballistic ring is a circle of its range (× the radius scale) on the surface.
A ballistic ring walks the terrain: an 8-px march outward to the **first** point the engine
refuses, then eight halvings between the last kept and that point, so a reachable valley beyond an
unreachable crest is not drawn. The rule is the engine's ([weapons](../engine/weapons.html),
*The ballistic reach*): the distance horizontal and at most the range, and the solver `0x49A890`
— `D′ = v⁴ + 2g·dy·v² − g²d²` not negative and one root's angle in (minbarrel, π/4] — with `dy`
the shooter's altitude less the target's, a target on water standing on the sea surface.

**The ground** is sampled from the terrain pass's own R8 height grid, one byte a 16-px cell, which
`tagpu_vk_terr_height(slot, &w, &h, &serial)` hands over for the slot — a null view unless the
slot's set names that pass's own height image, larger than its 1×1 stand-in, and a copy has been
recorded into it; `serial` changes whenever the grid does. It is `GetPosHeight 0x485070`'s bilinear interpolation, in
float where the engine steps in integers and truncates; at the grid's last row and column it takes
the nearest cell, where `0x485070` answers −1 (it requires the cell and its +1 neighbours on the
grid, `0x48508F..0x4850C4`).

**The bound**: a job's writes land at `base + k` with `k` below its segment count, `base` and the
count come from the pass's own block allocator, and the shader refuses any write at or past the
buffer's length; the march is bounded by the range (at most 4096 steps).

**A ring's kind** is the job's third component: 0 a flat ring, 1 a ballistic one, 2 and up a
sensor disc (+1 water only, +2 the preview's sheet, which carries the travelling rings). A sensor
disc is a flat circle whose surface altitude is taken at the point held to the scroll extent (the
push constant `mapext`), where its rim is drawn. The sensor paths take no reach: their points are
laid by the render thread (*The sensors*).

## The pass (`tagpu_vk_range.c`)

**Recorded between the world and the markers**, in the world's render pass: after the units and
their build ghosts, before the selection rects, order markers and health bars. Its `prepare` runs
after the terrain pass's, since it binds the height grid that pass bound for the slot. No depth
test.

**The per-slot reach cache.** A ring is keyed by its centre, altitude, range, velocity,
minbarrel, kind (flat, ballistic or a sensor's), radius scale and segment count; a slot keeps each
ring's block from its last use while the key and what it was walked over — the grid's serial and
size, the sea level, gravity and the scroll extent — are the same, so a structure's ring is walked once and a moving unit's every frame. The
reach buffer is cut into blocks of 128 and 1024 entries; growing it starts a new buffer that keeps
nothing. The cache is safe to reuse because it is per slot: a slot's buffer is read and written
only by that slot's submissions, and the seam waits on the slot's fence before `prepare`.

**The path buffer.** Each slot keeps the sensor paths' points in a storage buffer at binding 2
(`SVS`'s `Path` block), grown by doublings from 4096 points, and copies the hand-over's points into
it only when its `ptSerial` differs from the one the slot holds, or the buffer is new.

**The read bound**: `prepare` refuses a draw list unless every draw's instances lie inside the
list and each instance belongs to a ring draw or a path draw, never both. A ring draw's instances
all carry the draw's own segment count, 128 or 1024, so the vertex stages read only inside the
reach block that count chose; a path draw (`TAGPU_RG_SLINE`) has at least one segment and fewer
than the hand-over's points, and each instance's first point is a whole number with
`first + segments + 1` within them, since `SVS` reads points `first + seg` and `first + seg + 1`.
The points themselves are bounded by `TAGPU_RG_MAXPT`. Only ring instances are walked for the
reach.

**The barriers**: before the dispatch, `TRANSFER | FRAGMENT_SHADER → COMPUTE_SHADER`, which chains
onto the terrain pass's height upload (its copy ends in a barrier whose second scope is the
fragment stage); and every frame, dispatch or not, `COMPUTE_SHADER → VERTEX_SHADER`, since a kept
block was written by an earlier submission.

**The union is the stencil's**, BAR's fallback path, for the fills and the weapon rings. Five
layers in order — the sensor layer, defense ally, defense enemy, attack ally, attack enemy. **The
sensor layer** has one stencil bit a kind: clear the stencil; per jammer kind its discs (1024
segments, each fan's rim held to the scroll extent) with `NOT_EQUAL` and `REPLACE`, the placed
one's first, then the kind's paths; the radar and sonar paths; then per placed radar or sonar kind
its discs, the new one's first, so the sheet takes its own ground and the duller fill only what is
left, then its paths. A path is one `SLINE` draw (pipeline `sline`, `SVS`/`SFS`, no stencil): two
triangles a segment between neighbouring points, each corner moved along the point's direction to
the band's two edges, so neighbouring segments share their edges and no pixel is drawn twice; a
one-target-pixel rim on both edges; in a dashed band the dash the fragment falls in, its level's
opacity from the vertex block. A segment with neither end shown has no area. **A water disc's land
is transparent, not discarded**: the fragment reads the height grid (binding 40) bilinearly at the
world point the pixel shows if the ground there is at sea level — exact over water, and a land
pixel that maps onto water lies within (altitude − sea) / 2 px south of it. Each weapon layer with
a merged class: clear the stencil;
each class's discs (a fan a ring) with `NOT_EQUAL` against its bit and `REPLACE`, writing the
fill once a pixel; the outer bands with `NOT_EQUAL` and `KEEP`, so a ring's outline survives only
outside every disc of its class; then the inner bands and the never-merged lrpc rings with no
stencil. The bit and masks are dynamic state, so the disc, outer and line pipelines serve every
class; with `sline` the pass has four. The vertex stages' block is 160 bytes: the view, the zoom,
the scroll extent and the sea, the dash levels' opacities and the finest and zoom's level, and the
travelling rings' spacing, distance travelled, power, strength and colours. The pass
clears the stencil once more after its last draw. **A device with no stencil-carrying depth format
draws no rings** (logged once); neither does a frame whose slot has no height grid bound.

Premultiplied over, the target's alpha kept.

## The lane

`tagpu_rangeown_on()` is 1 while `ranges` is on **and** the lane draws rings: the session's
backend is Vulkan (`g_ddraw.renderer == vk_render_main`, stored before the first draw and once
more by the hand-over to GDI) and the range pass has not refused. The pass refuses — a one-way
latch, `tagpu_rangeown_refuse()` — on a target with no stencil plane or a pipeline that would not
build, and its teardown clears it (`tagpu_rangeown_forgive()`) unless it leaves the pass refused,
so the next device answers again; the HUD's `tagpu_hudstate` latch has the same shape. What a
draw reads of the latch decides which picture the markers draw, never what memory is read. On a
lane that draws no rings nothing is filled — the sensors neither, so GDI and a device with no
stencil show no sensor view and no preview — Alt+. and Alt+, go to the engine, and the
`ShowRanges` circles below come back. `ranges=off` on a lane that draws keeps the sensors: the
fill still runs for them.

## The engine's `ShowRanges` circles (`tagpu_order.c`)

The order-marker port draws the engine's `ShowRanges` set — the per-unit circles of
`0x4390A0`'s labelled branch and the AoE and coverage circles `0x439740` draws at an order's
target — only while `main+0x391BF` is set **and the rings are off**
(`showRanges = flag && !tagpu_rangeown_on()`). With `ranges` on, the rings replace them; with
`ranges=off`, or on a lane that cannot draw rings, the set comes back as the engine draws it. A
frame with no height grid bound yet (the first frames of a level) draws neither. The normal-play limb is untouched: a
cloaked unit's `mincloak` circle and a kamikaze's circles draw either way ([markers](markers.html)).

## Settings

In `impure.cfg`, read every frame by the game thread, so no value needs a restart. **The in-game
Visuals panel has a row for each but `rangetoggle`**: *Range rings*, *Attack rings* (Always \|
Shift held), *Cursor range*, *Sensor ranges* (Off \| On), *Sensor preview* (Off \| On), and one row
a defense class — *Ground*, *Air*, *Nuke*, *Cannon*, *LRPC defense* — cycling None \| Enemy \|
Allied \| Both over the class's bit of `rangeenemy` (stage bit 1) and of `rangeally` (bit 2). The
seven rows of the rings' own options — *Attack rings*, *Cursor range* and the five classes — grey
while *Range rings* is Off; the two sensor rows are its neighbours, not its options, and never
grey with it. Restore defaults sets the defaults below. The front-end screen has none of them: its two columns
are full ([settings](settings.html) §2.12).

| key | default | values |
|---|---|---|
| `ranges` | `on` | `on`/`off` (`1`/`0`): the weapon rings; off brings the engine's `ShowRanges` circles back and leaves the sensors |
| `rangeshift` | `off` | the attack rings only while Shift is held |
| `rangecursor` | `on` | the rings of the unit under the cursor |
| `sensorview` | `on` | the sensor view: the radar, sonar and jammer coverage of the viewed player and its allies, always; independent of `ranges` |
| `rangesensor` | `on` | the sensor preview: the radar, sonar and jammer coverage of a building being placed; independent of `ranges` |
| `rangeally` | `nuke` | the allied defenses' classes: a comma list of `ground`, `air`, `nuke`, `cannon`, `lrpc`, or `none` |
| `rangeenemy` | `ground,air,nuke,cannon` | the enemy defenses' classes, the same spelling |
| `rangetoggle` | none | `NAME:n:ally:enemy,...`, the Alt+. map; at most 64 types, `n` 1..16, each mask inside 2ⁿ − 1 |

A class list with a word that is not a class keeps the default and is logged. Under
`tagpu_defaults.off` the store is ignored: the compiled defaults apply, so the rings and both sensor keys are on, and
an Alt+. choice lasts for the session.

## How it is checked

`tools/tacli` drives the game; the DLL's log carries a `range:` heartbeat every 300 frames —
`rings=` the records published, `sensor=` the sensor layer's (a circle shrunk to nothing not
counted), `paths=` and `pts=` the sensor paths and their points, then the defense and attack
layers' counts, the instances and draws. The scenarios:

- `scenarios/sensor-view.json` — Two Continents' eastern shore with true LOS. Player 0 (Arm): the
  radar towers `rad` (5300, 12300) and `rad2` (6100, 12650), the mobile radar `seer`, the radar
  plane `peep`, the mobile radar jammer `jam`, the radar jammer ship `sjam` (ARMSJAM) and the sonar
  jamming sub `scram` (ARMSCRAM) in the water, the commander and a construction kbot `ck` for the
  preview. Player 2 (Arm AI), the ally: the radar tower `arad` (6900, 12650), overlapping `rad2`'s
  so the two merge, and the sonar station `ason` in the water. Player 1 (Core AI), the enemy: the
  radar tower `crad` and the mobile radar jammer `spec` (CORSPEC), whose coverage is never drawn.
  **The alliance is not part of the scenario format**: it is the skirmish key `Player<N>AllyGroup`
  in the instance's registry store, written before the load — seats with equal groups start allied
  ([sim](../engine/sim.html), the skirmish settings' `+8`).
- `scenarios/sensor-preview.json` — Two Continents with true LOS: the site (6750, 11900) on the
  eastern hill, an Arm radar tower switched on when built (`radA`), a second one to switch off
  with its button (`radOff`), an air plant (`ap`) that emits its 500 of radar only while a build
  order runs, the commander (400 of radar and sonar), open water for a sonar station at
  (7700, 11500), and two Core solar collectors 1416 and 1464 from the site for the edge check.
  `scenarios/sensor-preview-built.json`, applied over it, puts a finished Arm radar tower at the
  site.
- `scenarios/tascene-ranges.json` — the browser lab's fixture, with a radar tower among the
  defenses.

**MEASURED 2026-11-11** on `scenarios/sensor-view.json` (the reference setup, Vulkan, 1920×1080,
true LOS), with `Player0AllyGroup` 1, `Player2AllyGroup` 1 and `Player1AllyGroup` 2 in the
instance's registry store (every seat read 5 before): the alliance rows `+0x108` read record 0
`01 00 01 …`, record 1 `00 01 00 …`, record 2 `01 00 01 …`. `+0x10E` bit 0 of the spawned sensor
units: `rad`, `rad2`, `arad`, `ason`, `seer`, `jam`, `sjam`, `scram`, `crad` and `spec` 1; `peep`
(an ARMPEEP on the ground) and `ck` 0. The heartbeat over the scene: `rings=11 sensor=10 paths=5
pts=3150 … inst=8 draws=8`; placing an ARMRAD, `rings=14 sensor=11 paths=7 pts=4323`; *Sensor
ranges* Off, `rings=1 sensor=0 paths=0`. Pictures: the own and the allied radars merged into one
outline, and no coverage around the Core radar `crad` or the Core jammer `spec`. **The clocks**:
paused (tick 10521 held), a crop of a radar dash changed in 0 px over 1.5 s and one of a jammer
dash in 668 px. **The animation**: `rad` switched off, its arc moved inward 47 px a tick at zoom
0.5 — 96 world px a tick, 1440 / 15 — stepping once a tick with smooth motion (`tagpu_lerp.on`) off.
**The preview**: its rings moved between two shots 0.3 s apart, and the sheet was gone after the
cancel. **Range rings Off**: the sensor view still drawn, *Sensor ranges* and *Sensor preview*
`grayed 0`, the rings' options greyed. **The replay viewer**: `--perspective 0` drew player 0's and
its ally's coverage as the live game does; `--perspective 1` only Core's (`sensor=4 paths=4`); the
default viewer, with full visibility and no perspective, drew no rings and no sensors at all (the
census's `range=0`).

**MEASURED 2026-11-10** on `scenarios/sensor-preview.json` (the reference setup, Vulkan), on the
preview alone (a build with no sensor view), reading `+0x10E` live:
the commander 1, `radA` 1, `radOff` 1 and 0 after its on/off button, the air plant 0, 1 with a
build queued (`0x09` while an ARMTHUND stalled), and 0 again both when an ARMPEEP finished and
when the queued build was cancelled with a right click. An ARMRAD placed at (6750, 11900) — the
engine's snapped site centre (6752, 11904) — published `sensor=4` (the new ring, `radA`, the air
plant while producing, the commander), 3 after the cancel; an ARMSONAR at (7696, 11504)
`sensor=2` (the new ring and the commander's sonar); an ARMAP or an ARMSOLAR placed `sensor=0`;
`rangesensor=off` 0. **The radius
against the engine**: with the finished radar at the site (`+0x10E` 1), `+0x110` of the CORSOLAR
1415.75 away read `0x30280121` (bit 8, the radar contact) and of the one 1464.02 away
`0x30280021`, on four reads 2 s apart; before the radar both read `0x30280021`.

## What is not closed

- **The fades are BAR's distances, untested past 1024×768**: at that size TA's zoom range never
  reaches the distances where BAR fades the inner rings; a larger screen fades them at low zoom,
  which has not been seen live.
- **The replay viewer's default view draws nothing**: with full visibility and no perspective
  it shows no rings and no sensors (measured, *How it is checked*); why the fill publishes nothing
  there is not established. With a perspective the sensors follow the viewed player and its
  allies (measured); the weapon rings' look in a replay has not been verified. Nothing of either is
  recorded.
- **Fill rate at low zoom**: with about a thousand discs on screen the remaining cost is their
  fill, not the walk.
- **The cold walk is not bounded per frame.** A slot's first frame, a height-grid change and a
  reach-buffer growth walk every ring at once: 1024 directions of up to 4096 steps for a
  long-range ring. The stress scenario (1358 short rings) was measured; hundreds of LRPC- or
  nuke-class rings in one dispatch were not, and could show as a hitch on a slow card.
- The plan note's own open questions: the shooter's altitude is the unit's position, not a weapon
  piece's; the water-weapon and above-sea gates of can-engage are not drawn.
- **The sensor animation steps once a tick with smooth motion off**: the scale reads the packet's
  tick plus the fraction of the next one, and with `tagpu_lerp` off a circle growing or shrinking
  was seen to move a whole tick's step at a time (47 px at zoom 0.5).
- **A radar's altitude never shows in stock**: the bonus `2·altitude` survives only past a larger
  sonar distance, which no stock unit has, so a radar plane's circle does not change with its
  height. A mod with such a unit would draw it; none has been tried.
- **The outlines are rebuilt every frame while any emitter moves** or a circle grows or shrinks:
  the rebuild is O(circles²) a kind (each circle's arcs against every other disc, and each arc's
  successor searched over all arcs), bounded by the caps (*The sensors*) but not measured with
  hundreds of moving emitters.
- **One union a kind**: two water jammers of different kinds — a radar jammer ship and a sonar
  jamming sub — draw as two outlines where they overlap, by design (the kinds jam different
  things).
- **The browser lab draws the existing coverage by type**: `tools/tascene-ranges.js` has no
  activation state, so it counts every own and allied unit whose type will emit, and aircraft — a
  radar switched off, a factory not producing and a plane on the ground included.
