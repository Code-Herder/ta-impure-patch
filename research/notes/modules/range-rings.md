# Weapon range rings

BAR's weapon range preview on the Vulkan renderer, on TA's own reach rule: the rings of the
selected units, of the unit under the cursor, of the builders while a building is armed, of the
building being placed, and of the defenses by side and class, each class's rings merged into the
outline of their union. They replace the engine's `ShowRanges` circles while they are on.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`. The decisions and the reach's derivation are the plan note's, [weapon range rings](../weapon-range-rings.html); the engine side is in the engine map: the weapon record and the ballistic reach `0x49A890` in [weapons](../engine/weapons.html), `UnitInPlayerLOS 0x465AC0` in [map](../engine/map.html), HotUnits in [units](../engine/units.html), the placement fields in [orders](../engine/orders.html) and [render](../engine/render.html) (`0x4197D0`). Every field the module reads is a row of the [hook map](hook-map.html) §2.5; it writes no engine byte and installs no hook.

## The files

| file | thread | what |
|---|---|---|
| `tagpu_rangeown.c/.h` | game | which rings show, of what class, around which unit; every engine field resolved to a number in the packet; the Alt+. / Alt+, keys |
| `tagpu_packet.h`, `tagpu_packet.c`, `tagpu_packet_pub.c` (`fill_range`) | game / render | `TAGPU_PK_RANGE` (one 24-byte header) and `TAGPU_PK_RING` (32 bytes a ring, at most 4096, 128 KB) in the frame packet; the acquire's bounds |
| `tagpu_range.c/.h` | render | BAR's look: each ring's colour, widths, fades and highlight; 64-byte instances and the draw list |
| `tagpu_range_comp.h` → `inc/spirv/tagpu_range_comp.spv.h` | GPU | the reach, a compute pass: one workgroup a ring |
| `tagpu_range_glsl.inc` | GPU | the disc and band vertex stages and their fragment stage; they read the reach and place the points |
| `tagpu_vk_range.c/.h` | render | the Vulkan pass: the per-slot reach cache, the dispatch, the stencil union |
| `tagpu_weapons.c` (`tagpu_weapons_type_weapon`) | game | a unit type's weapon `idx` (weapon1..3, then the extra-weapons record), NULL past the type's count; reads only and builds nothing |
| `tagpu_settings.c` | game, render | the keys below and `rangetoggle=` |
| `tagpu_menu.c` | game | the eight rows of the in-game Visuals panel (*Settings*) |
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
(`0x465B38`). Allied units go through the same test.

**The defense layers**, drawn first:

- every structure (`+0x110` bit 29: the def's `bmcode` `+0x22F` is 0) with a weapon, shown, its
  rings filtered by side: an allied one by `rangeally`, an enemy one by `rangeenemy`, each a set
  of classes. Allied means the viewed player, or a byte 1 in the viewed player's alliance row
  `+0x108 + owner`;
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

## The pass (`tagpu_vk_range.c`)

**Recorded between the world and the markers**, in the world's render pass: after the units and
their build ghosts, before the selection rects, order markers and health bars. Its `prepare` runs
after the terrain pass's, since it binds the height grid that pass bound for the slot. No depth
test.

**The per-slot reach cache.** A ring is keyed by its centre, altitude, range, velocity,
minbarrel, ballistic flag, radius scale and segment count; a slot keeps each ring's block from its
last use while the key and what it was walked over — the grid's serial and size, the sea level and
gravity — are the same, so a structure's ring is walked once and a moving unit's every frame. The
reach buffer is cut into blocks of 128 and 1024 entries; growing it starts a new buffer that keeps
nothing. The cache is safe to reuse because it is per slot: a slot's buffer is read and written
only by that slot's submissions, and the seam waits on the slot's fence before `prepare`. **The
read bound**: `prepare` refuses a draw list in which any instance of a draw has a segment count
other than the draw's own, 128 or 1024, so the vertex stages read only inside the block that count
chose.

**The barriers**: before the dispatch, `TRANSFER | FRAGMENT_SHADER → COMPUTE_SHADER`, which chains
onto the terrain pass's height upload (its copy ends in a barrier whose second scope is the
fragment stage); and every frame, dispatch or not, `COMPUTE_SHADER → VERTEX_SHADER`, since a kept
block was written by an earlier submission.

**The union is the stencil's**, BAR's fallback path. Four layers in order — defense ally,
defense enemy, attack ally, attack enemy — and per layer with a merged class: clear the stencil;
each class's discs (a fan a ring) with `NOT_EQUAL` against its bit and `REPLACE`, writing the
fill once a pixel; the outer bands with `NOT_EQUAL` and `KEEP`, so a ring's outline survives only
outside every disc of its class; then the inner bands and the never-merged lrpc rings with no
stencil. The bit and masks are dynamic state, so three pipelines serve every class. The pass
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
lane that draws no rings nothing is filled, Alt+. and Alt+, go to the engine, and the
`ShowRanges` circles below come back.

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
Shift held), *Cursor range*, and one row a defense class — *Ground*, *Air*, *Nuke*, *Cannon*, *LRPC
defense* — cycling None \| Enemy \| Allied \| Both over the class's bit of `rangeenemy` (stage bit
1) and of `rangeally` (bit 2). The seven rows under *Range rings* grey while it is Off, and
Restore defaults sets the defaults below. The front-end screen has none of them: its two columns
are full ([settings](settings.html) §2.12).

| key | default | values |
|---|---|---|
| `ranges` | `on` | `on`/`off` (`1`/`0`): the rings; off brings the engine's `ShowRanges` circles back |
| `rangeshift` | `off` | the attack rings only while Shift is held |
| `rangecursor` | `on` | the rings of the unit under the cursor |
| `rangeally` | `nuke` | the allied defenses' classes: a comma list of `ground`, `air`, `nuke`, `cannon`, `lrpc`, or `none` |
| `rangeenemy` | `ground,air,nuke,cannon` | the enemy defenses' classes, the same spelling |
| `rangetoggle` | none | `NAME:n:ally:enemy,...`, the Alt+. map; at most 64 types, `n` 1..16, each mask inside 2ⁿ − 1 |

A class list with a word that is not a class keeps the default and is logged. Under
`tagpu_defaults.off` the store is ignored: the compiled defaults apply, so the rings are on, and
an Alt+. choice lasts for the session.

## What is not closed

- **The fades are BAR's distances, untested past 1024×768**: at that size TA's zoom range never
  reaches the distances where BAR fades the inner rings; a larger screen fades them at low zoom,
  which has not been seen live.
- **The replay viewer** carries the rings (nothing in them is recorded; a playing's in-play draw
  fills them as a live game's does [INFERRED]), but what it shows has not been verified.
- **Fill rate at low zoom**: with about a thousand discs on screen the remaining cost is their
  fill, not the walk.
- **The cold walk is not bounded per frame.** A slot's first frame, a height-grid change and a
  reach-buffer growth walk every ring at once: 1024 directions of up to 4096 steps for a
  long-range ring. The stress scenario (1358 short rings) was measured; hundreds of LRPC- or
  nuke-class rings in one dispatch were not, and could show as a hitch on a slow card.
- The plan note's own open questions: the shooter's altitude is the unit's position, not a weapon
  piece's; the water-weapon and above-sea gates of can-engage are not drawn.
