# UI markers — selection rects, health bars, order & build-queue markers

*Static map of every engine-drawn per-unit marker in `DrawGameScreen 0x468CF0`:
who draws it, from what state, and exactly where it lands in the frame order —
so the native GL scene pass knows what its unit overdraw covers and what it must
re-draw itself. All addresses are VAs for our pristine build (ImageBase
`0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`); disassembly via
`objdump -d -M intel`, decompiler output from a scratch copy of
`tools/ghidra-projects/TA.gpr`. Frame anchors are from `terrain-depth.md` §3.*

Evidence tags as in `terrain-depth.md`: **[BINARY-VERIFIED]** = instructions/
decompiler output read for this build this session. **[CORPUS]** = TADR/`tamem.h`
name cross-checked against our bytes. **[INFERRED]** = my reading, not yet
runtime-confirmed. `main` = `*(void**)0x511DE8` (the `TAdynmemStruct`).

> **Status after G13d (2026-09-02): TAKEN.** Every marker on this page is now ours,
> and the engine's 8bpp surface inside the viewport is **99.98 % key with nothing left
> on it but the mouse cursor** (measured, 112 px of 630 784). `tagpu_markown.c` +
> `tagpu_mark.c`; what follows is still the RE this page always was, and it is what
> those two modules were written from — but read §6 first for which half of it we
> reproduce and which half we let the engine draw for us.
>
> The gate was not "draw markers", it was **free view zoom**. These were the last
> engine pixels anchored to a WORLD position; everything else in the frame is either
> ours or genuinely screen-space (side panel, minimap, top bar, chat, dialogs), which
> must stay at 1:1 at any zoom. A marker left engine-drawn is a marker frozen at the
> unzoomed projection — invisible at 1×, a ghost at anything else.

---

## Summary — the six things to know

1. **Only the selection rectangle interleaves with unit pixels.** It is drawn
   per-unit *immediately before that unit's `DrawUnit`* inside both sweeps
   (ground `0x4699EB`, airborne `0x469B8A`). Every other marker — health bars,
   group digits, order/waypoint/build-queue markers, range circles, the build
   cursor, the band box — draws **after all units** (between hook 8 `0x469BD7`
   and hook 9 `0x469D2C`, or after fog). [BINARY-VERIFIED]
2. The selection marker is a **vector rectangle, not a sprite**: the unit's 3DO
   model XZ bounding box at its lowest Y, **rotated by the unit's heading**, four
   `DrawLine 0x4BE950` calls in GUI colour 0xA (green). Gated on `main+0x37F2F`
   bit2 — console command **`SelBoxes`**, default ON (`or …,4` @ `0x430E84`).
3. **Health bars** (`DrawHealthBars 0x46A430`, sole caller `0x469CB9`): a
   35×5 px `DrawBar` rect 10 px below the unit's projected feet, for **every
   HotUnit owned by the watched player** — no damage or selection condition —
   gated only on `main+0x37F06` bit0 = the registry option **`damagebars`**.
   Enemy units never get engine bars. Group digits (`unit+0xAC`, drawn as text)
   live in the same block.
4. **Order/build-queue markers draw only while SHIFT is held** (hotkey id `0xF9`
   → `GetAsyncKeyState(VK_SHIFT)` via `KeyboardHotkeySampler 0x4C1B80`), plus
   always for the unit under the mouse cursor / camera-tracked unit. The walker
   `0x48CC30` → `0x439B30` iterates each unit's order list; a per-order-type mask
   (`*(0x512344)`, stride 0x19, `+0xC`) picks up to five drawers: build-site
   rect, marching dotted route line, target circle, target sprite, range
   circles. All of it lands **after units, before fog** (fog darkens it).
5. **The build-cursor footprint rect and the drag band box draw after fog**
   (`0x469EC5/0x469F1E`, `DrawTranspRectangle 0x4BF8C0` ×2, double outline) —
   they are never fog-darkened and always survive anything drawn in the world
   section.
6. For the native pass: if our GL unit overdraw replaces the engine's unit sweeps
   *in place* (between terrain and hook 8, per `terrain-depth.md` §6.5), every
   marker except the selection rectangle survives untouched. **Only the selection
   rectangle must be re-drawn by us** (or re-ordered); §5 has the full table.

---

## 1. The selection rectangle — `DrawUnitSelectBoxRect 0x46A530`

**Convention:** stdcall(OFFSCREEN* ctx, UnitStruct* unit), `ret 8` @ `0x46A605`.
[BINARY-VERIFIED]

**Exact call sites (the only two in the binary — full-image scan):**

| VA | Sweep | Condition |
|---|---|---|
| `0x4699EB` | ground row sweep (§3.3 of terrain-depth) | `(unit->stateMask&3)==1 && drawUnits && (stateMask&0x10)` — then `0x469A00 DrawUnit` follows if `unit+0x9A` (model) non-null |
| `0x469B8A` | airborne/non-ground sweep | `(stateMask&3)!=1 && (stateMask&0x10)` — then `0x469BA3 DrawUnit`; whole block gated on `drawUnits` at `0x469B0B` |

So the marker is painted **under the unit's own sprite and under everything
drawn later** (later rows' units, tall features, projectiles, aircraft). Both
selected ground and selected airborne units get it; the airborne one draws after
all ground content but still before that aircraft's own sprite.

**Body** [BINARY-VERIFIED, decompiled]:

```c
void DrawUnitSelectBoxRect(ctx, unit) {
    if (!(main->b37F2F & 4)) return;               // "SelBoxes" toggle, default ON
    model = MODEL_PTRS[unit->UnitID];              // *(main+0x14377)[u16 at unit+0xA6]
    FUN_004CB650(model, &min, &max, 0);            // whole-3DO-tree AABB, 16.16 model space
    pts[4] = { (min.x,min.y,min.z), (max.x,min.y,min.z),
               (max.x,min.y,max.z), (min.x,min.y,max.z) };   // flat rect at LOWEST model Y
    pos = { unit->XPos(0x6A) - eyeX<<16, unit->YPos(0x6E), unit->ZPos(0x72) - eyeY<<16 };
    FUN_00467A50(ctx, &pos, pts, &unit->rot /*+0x64, 3 angles*/);
}
```

- `0x4CB650` → `0x4CB6A0`: recursive `Model3DONode` walker (child/sibling links,
  vertex array at node+0x24, count node+0x4, piece offsets node+0x10/14/18)
  accumulating min/max over all vertices — **the box is the whole model's
  bounding box, not the footprint from the unit def**. [BINARY-VERIFIED]
- `0x467A50` (stdcall ×4, `ret 0x10`): rotates each corner by the unit's
  rotation via `0x4B6CC0(corner, out, &unit->rot)` (scratch buffers
  `*(main+0x14383)` rotated vecs, `*(main+0x14387)` screen points), projects with
  the standard rule — `sx = (rot.x+pos.x)>>16 + 0x80`,
  `sy = (pos.z − rot.z)>>16 − ((rot.y+pos.y)>>16)/2 + 0x20` — and draws the
  4-line loop `p0→p1→p2→p3→p0` with `DrawLine 0x4BE950`. [BINARY-VERIFIED]
- **Colour:** one byte, `main+0xDD5` = GUI palette colour **index 0xA** (the
  same entry the healthy health-bar fill uses — green).
  `GetGuiPaletteColor(ta,i) = *(u8*)(ta+0xDCB+i)` per `composite-buffer.md`.
- **Gate:** `main+0x37F2F` bit2. `0x37F2F` is a word of display/debug toggles
  flipped by internal console commands; the bit-2 toggler `0x416E00` is bound to
  the command name **"SelBoxes"** (`.data 0x50216C`, table entry `0x502100`),
  and game init sets the bit (`or byte[main+0x37F2F],4` @ `0x430E84`) so boxes
  are on by default. (Same word: bit3="TreeDeath" `0x416E30`, bit4="NoShake";
  TADR calls the word `SoftwareDebugMode`.) [BINARY-VERIFIED]

No other marker is attached to selection inside the sweeps — the "circle" of TA
is really this rotated square.

---

## 2. The health-bar block — hook 8 `0x469BD7` → hook 9 `0x469D2C`

Runs **after** the airborne sweep (`0x469BA3`), projectiles and sfx — i.e. over
every world sprite — and **before** fog `0x469D8E` (bars get fog-darkened at
cell granularity like everything else under the overlay). Block layout
[BINARY-VERIFIED]:

```
0x469BD7  hook 8: 0x471F90(ctx, 8)
0x469BDC  push 0xF9; 0x469BE1 call KeyboardHotkeySampler 0x4C1B80
          → keycode 0xF9 dispatches to the VK_SHIFT probe (index byte 05 @0x4C1D45,
            jump entry 0x4C1BB5 = GetAsyncKeyState(0x10))     ← SHIFT held?
0x469BFC  if held: call 0x48CC30(&ctx, main+0x142F3)          ← ORDER MARKERS (§3)
          (note: NOT gated on drawUnits — runs even when the unit block is skipped)
0x469C01  if (!drawUnits) goto 0x469D38  -- PAST hook 9, which is therefore
          NOT reached on such a frame. Only one of DrawGameScreen's four callers
          passes drawUnits=0: 0x4962C2, TA's movie recorder ("%s\MOVIE%03i",
          function 0x495E88, `xor ebx,ebx` @0x495EA1). 0x495C74 and 0x495E64
          pass a literal 1; 0x4969CC's ebx is set to 1 @0x4967CF. [BINARY-VERIFIED]
0x469C01  if (drawUnits) for each HotUnit (u16 indices at *(main+0x1435F),
          count main+0x14367; unit = *(main+0x14357) + idx*0x118):
  0x469C4A   if (!(main+0x37F06 & 1) && unit->0xAC == 0) continue;
  0x469C63   sx = (s16)unit->0x6C − eyeX + 0x80
             sy = (s16)unit->0x74 − eyeY − ((s16)unit->0x70 >> 1) + 0x20   // feet, alt-corrected
  0x469C97   if (!(main+0x37F06 & 1)) continue;               // "damagebars" master gate
  0x469CA6   if (unit->Owner(0x96)->id(+0x146) == watchedPlayer([esp+0x70]))
  0x469CB9       DrawHealthBars(ctx, unit, sx, sy + 0xA);     // bar centre 10px below feet
  0x469CC9   if (same owner && unit->0xAC != 0)
  0x469CF9       DrawTextCustomFont(ctx, {'0'+unit->0xAC, 0}, sx, sy + 0xE, -1);  // group digit
0x469D2C  hook 9: 0x471F90(ctx, 9)
```

### 2.1 `DrawHealthBars 0x46A430` [BINARY-VERIFIED]

**Convention:** stdcall(OFFSCREEN* ctx, UnitStruct* unit, int x, int y),
`ret 0x10`. Single caller in the whole image: `0x469CB9`.

```c
if (unit->Health /*s16 +0x108*/ <= 0) return;
RECT r = { x-0x11, y-2, x+0x11, y+2 };            // 35 x 5 px (DrawBar is edge-INCLUSIVE)
DrawBar(ctx, &r, guiCol[0]);                       // backing: GUI colour 0 (black)
r = { x-0x10, y-1, x-0x10 + Health*32/maxHP, y+1 };// fill: up to 33 x 3 px
// maxHP read as u32 at UnitDef+0x1FA (tamem: u16 nMaxHP + u16 data8) [CORPUS]
DrawBar(ctx, &r, Health > 2*maxHP/3 ? guiCol[0xA]  // green
              : Health >   maxHP/3 ? guiCol[0xE]   // yellow
              :                      guiCol[0xC]); // red
```

- `DrawBar 0x4BF6F0` = stdcall(OFFSCREEN*, RECT*, u8 colour) [CORPUS — TADR
  `ReloadBars.cpp` uses this exact signature]; clips to the context
  (`0x4BF620`) then `rep stos` fill in `0x4CCDEA`, **width = right−left+1,
  rows = bottom−top+1** (both edges inclusive). [BINARY-VERIFIED]
- **When a bar shows:** watched player's unit + on the HotUnits list (screen
  cull) + `Health > 0` + option bit. **Not** selection-gated, **not**
  damage-gated — full-HP units show a full green bar. Airborne units are in
  HotUnits like everyone else → **yes, aircraft get bars** (at
  feet-minus-alt/2 projection, i.e. under the sprite at its screen position).
  Enemy/allied units of other players: never.
- **Option:** `main+0x37F06` bit0 (the word `shadows-cloak.md` maps as TADR
  "GameOptionMask", whose bits 1..8 are AntiAlias/Shadow/…): bit0 is loaded from
  the registry value **`"damagebars"`** (`.data 0x504670`, read under key
  "Total Annihilation" at `0x42FD42`; if the value is missing the bit is forced
  0), and toggled live by a GUI handler at `0x496058`. [BINARY-VERIFIED]

### 2.2 Group digits [BINARY-VERIFIED draw, INFERRED semantics]

`unit+0xAC` (byte, inside tamem's `data9` blob) is the squad tag — 0 = none.
Drawn as a 1-character string `'0'+value` via `DrawTextCustomFont 0x4C14F0` at
`(sx, sy+14)`, same owner + damagebars gates as the bar. (The squad-assign
handler cluster at `0x4960xx` references sound/GUI id "SelectSquad"
`0x50951C`.) No other per-unit decal exists in the block.

---

## 3. Order, waypoint & build-queue markers — the SHIFT pass

### 3.1 Driver `0x48CC30` [BINARY-VERIFIED, decompiled]

stdcall(OFFSCREEN* ctx, void* viewStruct=main+0x142F3), `ret 8`, sole caller
`0x469BFC` (SHIFT-gated, see §2 layout). The **viewStruct** is the small camera
block at `main+0x142F3`: `+0` = `CameraToUnit` (UnitStruct*, tamem name), `+0x2C`
= eyeX (`main+0x1431F`), `+0x30` = eyeY (`main+0x14323`).

Walks **the watched player's unit range** (`PlayerStruct(main+0x1B63 +
watched(main+0x2A42)·0x14B)`: first unit ptr `+0x67`, last `+0x6B`, step 0x118).
For each unit with `stateMask & 0x10000000` (alive [CORPUS tamem]) and not
`& 0x4000` [INFERRED: excluded state, likely loaded/hidden]:

| Unit is… | mask | flag |
|---|---|---|
| the `CameraToUnit`, or its `UnitInGameIndex(+0xA8)` equals `main+0x37E9C` (tracked) or `main+0x2CBA` (unit under mouse cursor) | `0x1F` (all five drawers) | 1 |
| selected (`stateMask & 0x10`) | `0x1F` | 0 |
| anything else, **iff** one of the three special units above has `UnitDef+0x156` (`CANBUILD_ptr`) non-null — i.e. a *builder* is hovered/tracked | `0x01` (build spots only) | 1 |

→ `0x439B30(unit, mask, ctx, viewStruct, flag)`.

That last row is the "hover a constructor, see *everyone's* claimed build
sites" rule. Factory rally points and queued factory orders are just entries in
the factory's order list, so they come out of the same pass.

### 3.2 Walker `0x439B30` [BINARY-VERIFIED, decompiled]

`ret 0x14`. Chains a position `pos` initialised to the unit's own position
(`unit+0x6A/0x6E/0x72`, 16.16), then walks the order list: head `unit+0x5C`,
next link `node+0x4A`. Per node, order type byte `node+0x4` indexes the script/
order handler table `*(0x512344)` (TADR `COBScriptHandler_Begin`), **stride
0x19**; `entry+0xC` (u32) is the order type's marker-capability mask, ANDed with
the caller mask:

| bit | drawer | draws |
|---|---|---|
| 0 | `0x438C00` (`ADDR_DrawBuildSpotQueue`, TADR label) | queued build-site footprint rect |
| 1 | `0x4394E0` | target sprite (delegates to bit-3 drawer) **plus**, only when `flag==1`, the marching dotted route line |
| 2 | `0x4399F0` | circle around the order target |
| 3 | `0x439740` | animated sprite at the order target |
| 4 | `0x4390A0` | range circles — once per unit (guard flag) |

Each drawer receives `(ctx, viewStruct, node, &pos[, flag])` and **writes the
order's target into `pos`**, so consecutive orders chain unit → wp1 → wp2 …

Order-node fields used [BINARY-VERIFIED]: `+0x4` type, `+0xE` owning unit,
`+0x16` target UnitStruct* (0 = ground target), `+0x22/0x26/0x2A` target x/y/z
(16.16), `+0x32/0x34` cached last-seen target x/z (int16), `+0x36` build target
unit-type id, `+0x42` flags (bit `0x200000` = "last-seen cached"), `+0x46`
order-issue game time.

### 3.3 The five drawers

**Build-site rect `0x438C00`** [BINARY-VERIFIED]: for build orders
(`node+0x36` = target type): corners = target pos ± UnitDef footprint extents
(dwords at UnitDef `+0x15E/+0x162/+0x166/+0x16A/+0x172` — the load-time world
half-extents around tamem's `__X_Width`/`Z_Width`), projected with the target
tile's stored height. Draws **8 `DrawLine` segments in two colours**: an inner
rect that *grows over the first 10 game ticks* after the order is issued
(`t = clamp(gameTime(main+0x38A47) − node->issueTime, 0, 10)`, corner offsets
scaled `t/10`) plus outer edge lines. Colour pair by whether the issuing unit
(`node+0xE`) is currently selected: selected → GUI[3] + GUI[0xA]; not →
GUI[1] + GUI[9]. Chains `pos` to the site.

**Route dots `0x4394E0`** [BINARY-VERIFIED]: calls the bit-3 drawer first, then
iff `flag==1` (hovered/tracked unit only — merely-selected units do NOT get
route lines) draws the segment `pos → target` as GAF frames of the **`pathicon`
sequence (`*(main+0x148D3)`, tamem name)** via `CopyGafToContext 0x4B7F90`,
one every **0x30 (48) world px**, phase advancing 48 px per 30 ticks and frame
index cycling — the classic marching dotted order line.

**Target circle `0x4399F0`** [BINARY-VERIFIED]: 16 chords (`TurnZLookup
0x4B7123`/`TurnXLookup 0x4B70EF`, angle step 0x1000 of 0x10000) around the
target, radius = target unit's size field (`UnitDef+0x178`, upper half of
tamem `data_10` [INFERRED name]) or 0x20 for ground targets, colour GUI[0xC]
(red), flat at the target's altitude-corrected screen pos.

**Target sprite `0x439740`** [BINARY-VERIFIED]: resolves the target position —
live target unit if `UnitInPlayerLOS 0x465AC0(owner->player, target)`, else the
cached last-seen `node+0x32/0x34` — then alpha-blits
(`AlphaCompsteBuf2OFFScreen 0x4B8500`) an **animated cursor GAF**: sequence
`cursor_ary[entry+0x10]` (`*(main+0x1487F + idx*4)`, tamem `cursor_ary[0x15]`),
frame = `(gameTime / (2·seq.period)) % seq.count`. This is the pulsing
cross/crosshair at move/attack/patrol targets. Also (debug, `main+0x391BF`
only) per-weapon AoE/coverage + `attackrunlength` labelled circles.

**Range circles `0x4390A0`** [BINARY-VERIFIED]: normal play draws only
(a) the **cloak radius** (`mincloakdistance` def+0x208) around cloaked units
(`unit+0x10E & 4`) in GUI[0xF], and (b) for kamikaze units
(`UnitTypeMask_0(def+0x241) & 0x10000000`, `ExplodeAs` def+0x220): a
**pulsing circle** (radius animates with `gameTime%60` up to the weapon AoE/2,
`weapon+0xD6`) plus the `kamikazedistance`(def+0x218)/sight circle, GUI[0xC].
With the **`ShowRanges` console toggle** (`xor dword[main+0x391BF],1` @
`0x4194C5`, command name `.data 0x5022DC`) it instead draws *labelled* circles
for sight/radar/sonar/jammers/builddistance/maneuver/kamikaze (GUI[0xE]) and
weapon1-3 ranges (flashing GUI[4]/GUI[0xC] on `gameTime&1`), labels via
`DrawTextCustomFont`.

`DrawRangeCircle 0x438EA0` (symbol) is the shared circle rasteriser: segments
of `DrawLine`, each endpoint's y corrected by `max(centreY, GetPosHeight
0x485070(x,z))/2` — **circles follow the terrain**, with an optional label
drawn at a slot-dependent point on the circumference.

**Frame position of all of §3: after every unit and projectile, before fog** —
the engine's order markers are fog-darkened where they cross unlit cells, and
they overdraw health bars' screen area only where they happen to land later in
the block (they draw *before* the health-bar loop: markers first at `0x469BFC`,
bars after at `0x469CB9`, so **bars paint over order lines**). [BINARY-VERIFIED]

---

## 4. After fog — build cursor footprint & band box

`terrain-depth.md`'s frame map said "build-cursor rect … after fog"; confirmed,
with the exact mechanism [BINARY-VERIFIED]:

```
0x469D8E  fog 0x4848E0(ctx)                      (drawUnits-gated)
0x469DC2  gate: (main+0x2CC6 & 8)                        ← rect forced on (drag/band state)
          OR (main+0x2CC3 == 0xE                          ← mouse cursor mode 14 = build
              && IsPositionInRect 0x4B6720(main+0x37E27 viewport, mouse main+0x2C76/0x2C7A))
0x469E13  rect from world globals: x1=main+0x2C92, z1=main+0x2C9A (−h1/2, h1=main+0x2C96),
          x2=main+0x2C9E, z2=main+0x2CA6 (−h2/2, h2=main+0x2CA2); standard +0x80/+0x20 projection
0x469EC5  DrawTranspRectangle 0x4BF8C0(ctx, &rect, colour)          — outer
0x469F1E  DrawTranspRectangle(ctx, &rect inset by 1px, colour2)     — inner
```

Colours: build mode (`0x2CC3==0xE`) → both rects GUI[`(main+0x2CC6 & 0x40) ?
0xA : 4`] — bit6 = placement valid → green, else colour 4 (blocked). Other gate
(bit3 of `0x2CC6`) → outer GUI[0xF] (white), inner GUI[0] (black) — the
white/black double outline of the **drag band box** [mechanics BINARY-VERIFIED;
band-box identification INFERRED — the rect globals are world-anchored, which
matches the band box scrolling with the map]. `DrawTranspRectangle 0x4BF8C0`
(symbol) draws the 4 edges with `DrawLine2 0x4CC7AB`/`DrawLine` — a hollow
rect, two edges through the "transparent" line variant.

The rest of the post-fog tail is HUD, not world markers: spectate vcall
`0x469D80`, dialogs (`GetGameingType 0x435100` state 2/3 → `0x4C69C0` +
`DrawPopupF4Dialog 0x4948E0`), `DrawPopupButtomDialog 0x4689C0`,
`kDrawBps 0x468380` (gated `main+0x391C3`), `DrawChatText 0x464060`
(drawUnits-gated), the clock/frame debug text (`main+0x3923B` bit1,
`DrawTextCustomFont` at fixed x=0x83/0xBC, GUI colour 0xF), then side panel /
minimap (`0x46B900` ×9, GAF blits) down to present `0x46A3DB`. No further
unit-anchored decals exist. [BINARY-VERIFIED call survey]

---

## 5. SYNTHESIS — what our GL overdraw covers, what we must re-draw

Premise (from `terrain-depth.md` §6.5): our native pass replaces the world
content **between the terrain call and hook 8** and lets the engine keep drawing
everything from `0x469BD7` on. Under that split:

| Marker | Drawn at | Frame position | State to redraw from | Verdict |
|---|---|---|---|---|
| Selection rect | `0x4699EB`/`0x469B8A` → `0x46A530` | **interleaved** — before each unit, inside both sweeps | `stateMask&0x10` @unit+0x110; pos +0x6A/6E/72; rot +0x64; model AABB via `MODEL_PTRS[unit+0xA6]` (or our own mesh bounds); gate `main+0x37F2F`&4; colour GUI[0xA] | **must be re-drawn by us** (engine's is under our unit pixels and under wrong neighbours) |
| Health bar | `0x469CB9` → `0x46A430` | after all units, before fog | HotUnits list; Health u+0x108, maxHP def+0x1FA; owner u+0x96→+0x146 == main+0x2A42; option `main+0x37F06`&1; colours GUI[0,0xA,0xE,0xC]; pos = feet −alt/2 +(0,10) | **survives** our overdraw |
| Group digit | `0x469CF9` | same block | u+0xAC byte; same gates; text at feet+(0,14) | **survives** |
| Order route dots | `0x4394E0` (SHIFT pass) | after units, before fog (fog-darkened; bars paint over) | SHIFT key; order list u+0x5C (§3.2 fields); `pathicon` GAF main+0x148D3; hover ids main+0x2CBA/0x37E9C/CameraToUnit main+0x142F3 | **survives** |
| Order target sprite | `0x439740` | same | cursor GAF `main+0x1487F+idx·4`, idx = `(*(0x512344))[type·0x19+0x10]`; LOS cache node+0x32/42 | **survives** |
| Target circle | `0x4399F0` | same | target def+0x178 radius; GUI[0xC]; terrain heights (GetPosHeight) | **survives** |
| Build-site rect (queued) | `0x438C00` | same | node+0x36 type → footprint def+0x15E..0x172; issue time node+0x46; builder-selected colour switch | **survives** |
| Range circles (cloak/kamikaze/ShowRanges) | `0x4390A0` | same | def ranges 0x202..0x218, weapons; toggles main+0x391BF | **survives** |
| Build cursor footprint | `0x469EC5` | **after fog** | mode main+0x2CC3==0xE, valid bit main+0x2CC6&0x40, rect main+0x2C92..0x2CA6 | **survives** (also never fogged) |
| Band box (drag) | `0x469F1E` path | after fog | main+0x2CC6&8 + same rect globals | **survives** |
| Chat/clock/HUD/minimap | `0x469FCB`+ | after fog | — | **survives** |

Caveats for the native pass:

1. **If we instead composite at present time** (after `0x46A3DB`-side blits or
   in cnc-ddraw), *everything above* is under our unit pixels and the verdicts
   flip to "must re-draw" for every row above the build cursor. The in-place
   split is what keeps this table cheap.
2. Re-drawing the selection rect natively is ~40 lines: 4 model-space corners
   from our own mesh AABB at min-Y, rotate by unit yaw, project with
   `sx = wx − eyeX + 128, sy = wz − alt/2 − eyeY + 32`, 1-px lines, colour =
   `GetGuiPaletteColor(main, 0xA)` resolved through the game palette. Honour
   `main+0x37F2F` bit2 so `SelBoxes` still works.
3. If we want engine-parity layering, our redrawn select box must sit **under
   its own unit's pixels** and under later-row content — i.e. give it the same
   row-sort depth key as its unit minus an epsilon, not "always on top".
4. The SHIFT-order pass writes into the same colour offscreen *after* our
   content; nothing needs suppressing. But should we later take over fog, note
   the order markers are *expected* to be fog-darkened (they draw before
   `0x4848E0`), while the build cursor is not.
5. The health bar's `maxHP` divide reads a **dword** at def+0x1FA (tamem splits
   it as u16 nMaxHP + u16 data8) — replicate the dword read for parity with
   odd mods.

---

## 6. G13d — how we took them (2026-09-02)

Two mechanisms, because the markers split cleanly in two.

**Health bars are RE-DRAWN** (`tagpu_mark.c`), and `DrawHealthBars 0x46A430` is
detoured away. They cannot be captured, because the engine's loop walks **HotUnits —
a list culled to the UNZOOMED viewport** — so a captured bar layer would stop at the
1× rect and leave the outer ring of a zoomed-out view bare. Our walk is over the unit
array with the zoom's own effective rect, which is the same set at zoom ≥ 1 and a
superset below it. The arithmetic of §2.1 is reproduced exactly, including the
**unsigned** `(Health<<5)/maxHP` divide and the `maxHP/3` thirds; the colours go
through `gui[i] = *(u8*)(main+0xDCB+i)` rather than being used as palette indices.

> That last point caught a live bug in `tagpu_native.c`: its native selection rect had
> been emitting palette index **10** where the engine emits `gui[0xA]` = **233**. It
> drew the box in a dark colour and nobody saw it, because the engine was still
> painting its own green one over the top. Suppressing the engine's is what exposed it.

**Everything else is CAPTURED AND REPLAYED.** The order-marker pass alone is five
drawers over the order list, with a build rect that grows over ten ticks, a marching
dot phase, a last-seen LOS cache and `ShowRanges`' text labels — a lot of arithmetic
to get subtly wrong. So the engine draws it, into a scratch 8bpp buffer of ours
instead of into its frame, and we upload that buffer and draw it as one quad through
the same zoom transform the world uses. Parity is exact by construction, text
included. The capture is a pointer swap and nothing else: the OFFSCREEN is a stack
local in `DrawGameScreen`, so pointing its pixel base (+0x0C) at our own buffer for
the length of a block redirects every clipped blit inside it.

| Marker | Ours how | Engine side |
|---|---|---|
| Health bar | re-drawn from unit state | `0x46A430` prologue detour |
| Group digit | captured (window A) | rides the same block |
| Order markers, route dots, target sprite/circle, build-site rect, range circles | captured (window A) | ride the same block |
| Build-cursor footprint, drag band box | captured (window B) | the two `0x4BF8C0` call sites redirected |
| Selection rect | already native since G12b | `0x4699EB`/`0x469B8A` redirected through a **per-unit** test — a unit `tagpu_native_owns_unit` does not own keeps the engine's |

**Two capture windows, because fog divides them.** Window A is hook 8 `0x469BD7` →
hook 9 `0x469D2C` and is drawn with the fog rule applied (the engine's block runs
*before* `0x4848E0` and is darkened by it). Window B is the two `DrawTranspRectangle`
calls, drawn with fog off, because the engine never darkens the build cursor. Layer A
draws first, then our bars over it, then layer B — the engine's own order inside the
block (`0x469BFC` markers, `0x469CB9` bars, and the cursor after fog).

Six call-site redirects and one prologue detour. **No collision with the other passes**:
the redirects *call* `0x471F90` and `0x4BF8C0`, so whatever `fxown` and `terrown`
installed on those still runs.

**What the capture buffer holds under a blend.** `DrawTranspRectangle`'s transparent
edges and the order sprite's alpha composite read the destination. Ours is key-filled,
which is exactly what they already read out of the engine's frame today, because
`terrown` fills the viewport with that same key.

**That was originally called harmless, and it was not** [CORRECTED 2026-09-03]. What
those primitives *read* is unchanged; what they *write* is a function of it. The target
sprite (`0x439740`, the pulsing star at a move/attack waypoint) alpha-composites through
`AlphaCompsteBuf2OFFScreen 0x4B8500`, whose inner loop at `0x4CBF99..0x4CBFAC` is
`out = tab[(src << 8) | dst]` with the LUT pointer at `*(*(u32*)0x51FBD0 + 0xC0)`
(`0x4B6220` is just `mov eax,ds:0x51FBD0; ret`). [BINARY-VERIFIED] With `dst` = the key,
that is a blend against palette 254 — bright cyan — so the star rendered **teal** where
stock TA renders it olive over grass. Measured in its bounding box: 14 % of its pixels on
the cyan ramp, against 1 % after the fix.

**The fix: an identity LUT for the length of the capture.** `tab[(s<<8)|d] = s` for every
pair, installed at hook 8 and restored at hook 9, makes the composite a plain copy, so the
sprite lands in our buffer as its own palette indices. The pointer is a *global*, so
unlike the context's pixel base it can safely be restored from a later frame if a window
is ever abandoned. Inside the window it is the only alpha composite the engine reaches —
the route dots are a masked `CopyGafToContext 0x4B7F90` (and `fxown`'s detour on that leaf
only skips while the explosion pass is running, so the dots are untouched), the rects and
circles are `DrawLine`, and the health bars are ours. The replay then draws the sprite
**opaque**, which is a deliberate departure from stock's blend; re-blending it against our
own scene instead would now be a shader change, not another capture change.

### 6.1 Cost, and the one honest gap

Window A is not opened at all unless something can draw in it: the engine's own SHIFT
hotkey (`KeyboardHotkeySampler(0xF9)`, called through the engine's function so a
different keymap cannot make us disagree with it) or a watched HotUnit carrying a
squad tag with `damagebars` on. With `damagebars` off — the default when the registry
value is missing — the common frame costs nothing at all.

**The gap: the captured layers are clipped to the offscreen.** The engine's drawers clip
to the OFFSCREEN's own rect, so at zoom < 1 order markers and group digits stop while the
world carries on past them. Health bars, which are the always-on markers, do not have this
limit because they are re-drawn.

**G13f moved that edge but did not remove it.** `vpwide` widens the engine's addressable
viewport rect, and the same rect is what `DrawGameScreen` copies into the offscreen's clip
— so with `vpwide.on` armed the capture reaches the **surface** bound rather than the 1×
viewport (at 0.5× on a 1024×768 frame, screen `[288,863]×[192,575]` instead of
`[352,799]×[208,559]`; confirmed with a waypoint at `s=(318,542)` that used to be clipped).
`layer_begin` asks for the addressable rect for exactly this reason, and the intersection
with the context clip is what keeps it inside our scratch. Beyond the surface the engine
would have to draw at a negative position into a screen-sized buffer, which it cannot:
closing that last part means giving the capture window its own wider buffer and offsetting
the base so negative engine coordinates land inside it — `markown` already owns
`ctx[CTX_BASE]`, so it would also have to own `CTX_PITCH` and the clip fields. Possible,
and deliberately not done here.

**The input half of the same boundary IS closed.** G13e named it — the engine can only
*name* screen positions inside its own viewport, and a click outside it does nothing at all
(measured) — and G13f closes it: with `vpwide.on` a click in the ring selects and orders
like any other ([GPU status](gpu-status.html) §2.3b, §3.1). Without that arm the pre-G13f
behaviour stands and a ring click is *dropped* rather than landed on the wrong world point
(`tagpu_zoom.h`).

### 6.2 The capture runs ~83× per presented frame [MEASURED 2026-09-03]

The game thread and the GL thread are not in step, and they are not even close. With the
stack armed, almost everything in `DrawGameScreen` is skipped — terrain, units, features,
effects and fog are all ours — so the engine's frame is cheap and free-runs, while ours is
the slow half. Counted on a live skirmish at 1024×768: **9 300–10 200 hook 8 → hook 9
blocks per 120 presented frames, i.e. 78-85 captures for every frame the player sees.**
`cnc-ddraw` presents from its own thread (`ogl_render_main`), which leaves the primary
surface's critical section long before `tagpu_overlay_draw` runs, so the read is concurrent
with the game thread by design.

Two consequences, both measured with SHIFT held:

- **The publication must only ever be REPLACED, never emptied first.** `mark_hook8` used
  to `layer_clear` on the way in and republish at hook 9; that hole is open for the length
  of one capture, and at 80 captures a frame the GL thread landed in it **13 times in 120
  presents (~11 %)** — an order overlay that visibly flickered on and off the whole time
  SHIFT was down. Deciding "nothing this frame" *before* the capture and leaving the last
  publication standing otherwise takes it to **0 in 840**.
- **Two buffers are enough, and that was checked rather than assumed.** The writer
  alternates slots, so the buffer the GL thread is uploading is only reclaimed two
  publications later. Instrumented for the case where the slot about to be key-filled is
  the one the reader still holds: **0 in ~50 000 publications at 1024×768**. It is the
  `glTexSubImage2D` finishing inside two of the engine's blocks that makes that true, so it
  is the number to re-take if the layer ever grows much faster than the block does.

The post-fog window (build cursor, band box) has the same shape with one twist: its "there
was nothing to draw" is only knowable *in arrears*, because no `DrawTranspRectangle` came.
It is therefore decided at the **next** frame's hook 8, about the frame that just ended —
a one-block ghost where the old code had a hole most of a frame wide. Two residuals are
known and deliberately left: the block in which a drag ends still carries its last rect,
and the second of the two `DrawTranspRectangle` calls continues into the buffer the first
one published, so a present between them shows the outer outline without the inner.

---

## Appendix — addresses & offsets

### Functions

| VA | Role | Convention |
|---|---|---|
| `0x46A530` | **DrawUnitSelectBoxRect** (rotated model-AABB rect) | stdcall(ctx,unit), ret 8 @`0x46A605` |
| `0x467A50` | rotate+project 4 corners, draw 4 lines | stdcall(ctx,&pos,&pts4,&rot), ret 0x10 |
| `0x4CB650`→`0x4CB6A0` | 3DO tree AABB (min/max, 16.16) | (model,&min,&max,flag) |
| `0x4B6CC0` | rotate vector by unit rotation | (corner,out,&rot) |
| `0x46A430` | **DrawHealthBars** (one unit's bar) — sole caller `0x469CB9` | stdcall(ctx,unit,x,y), ret 0x10 |
| `0x4BF6F0` | **DrawBar** — clipped solid fill, edges inclusive (worker `0x4CCDEA`) | stdcall(ctx,RECT*,u8), [CORPUS TADR] |
| `0x4C14F0` | DrawTextCustomFont (group digit, labels, clock) | (ctx,str,x,y,-1) |
| `0x4C1B80` | KeyboardHotkeySampler — id `0xF9` → GetAsyncKeyState(VK_SHIFT) | (keyId), ret 4 |
| `0x48CC30` | order-marker driver (per player unit, mask/flag rules §3.1) — sole caller `0x469BFC` | stdcall(ctx,&main+0x142F3), ret 8 |
| `0x439B30` | order-list walker, dispatch by mask `(*(0x512344))[type*0x19+0xC]` | (unit,mask,ctx,view,flag), ret 0x14 |
| `0x438C00` | build-site rect (TADR `ADDR_DrawBuildSpotQueue`) | ret 0x14 |
| `0x4394E0` | route dots (pathicon GAF, flag-gated) + target sprite | ret 0x14 |
| `0x4399F0` | target circle (16 chords, GUI[0xC]) | ret 0x14 |
| `0x439740` | animated target sprite (`cursor_ary` GAF, alpha blit) + debug AoE circles | ret 0x14 |
| `0x4390A0` | per-unit range circles (cloak/kamikaze; all with ShowRanges) | — |
| `0x438EA0` | **DrawRangeCircle** (symbol) — terrain-following segmented circle + label | ret 0x1C |
| `0x465AC0` | UnitInPlayerLOS | — |
| `0x485070` | GetPosHeight (terrain height at world x,z) | — |
| `0x4B70EF`/`0x4B7123` | TurnXLookup / TurnZLookup (sin/cos LUT) | — |
| `0x4BF8C0` | **DrawTranspRectangle** — hollow rect (build cursor / band box) | (ctx,RECT*,colour) |
| `0x4BE950` | DrawLine; `0x4CC7AB` DrawLine2 | — |
| `0x416E00` | "SelBoxes" toggle (bit2 of main+0x37F2F); default set @`0x430E84` | — |
| `0x4194C5` | "ShowRanges" toggle (`xor dword[main+0x391BF],1`) | — |
| `0x42FD42` | load registry "damagebars" → main+0x37F06 bit0; live toggle `0x496058` | — |

### DrawGameScreen call sites (markers)

`0x4699EB` select box (ground) · `0x469B8A` select box (air) · `0x469BD7` hook 8
· `0x469BE1` SHIFT probe · `0x469BFC` order markers · `0x469CB9` health bar ·
`0x469CF9` group digit · `0x469D2C` hook 9 · `0x469D8E` fog · `0x469EC5`/
`0x469F1E` build-cursor/band-box rects · `0x469FCB` chat · `0x46A04A`/`0x46A061`
clock text.

### Data

| Where | What |
|---|---|
| `main+0xDCB` | GUI colour byte array (`GetGuiPaletteColor`); indices used: 0 backing/black, 1&9 build-spot (unselected), 3&0xA build-spot (selected), 4 build-blocked/flash, 0xA select box+healthy+build-OK, 0xC low-HP/target/kamikaze, 0xE mid-HP/ShowRanges, 0xF cloak/band-box/text |
| `main+0x142F3` | camera block: +0 `CameraToUnit`, +0x2C eyeX, +0x30 eyeY [CORPUS] |
| `main+0x14357` | unit array base (stride 0x118); `0x1435F/0x14367` HotUnits (u16 ids) / count |
| `main+0x14377` | `MODEL_PTRS` — Model3DONode* per unit type [CORPUS] |
| `main+0x1439B` | UnitDef array base (stride 0x249) |
| `main+0x1487F` | `cursor_ary[0x15]` GAF sequences (order-target sprites) [CORPUS] |
| `main+0x148D3` | `pathicon` GAF sequence (route dots) [CORPUS] |
| `main+0x2A42/0x2A43` | watched / local player id |
| `main+0x2C76` | mouse pos (POINT, world-space) [CORPUS]; `0x2CBA` unit-under-cursor id; `0x37E9C` tracked-unit id [INFERRED names] |
| `main+0x2C92..0x2CA6` | build/band rect: x1,h1,z1,x2,h2,z2 (world) |
| `main+0x2CC3` | mouse cursor mode (0xE = build); `0x2CC6` flags: bit3 rect-forced, bit6 placement-valid |
| `main+0x37F06` | GameOptionMask: **bit0 = damagebars**; (bit1 AA … per shadows-cloak.md) |
| `main+0x37F2F` | display/debug word: **bit2 = SelBoxes** (default on), bit3 TreeDeath, bit4 NoShake |
| `main+0x38A47` | GameTime (marker animation clock) |
| `main+0x391BF` | ShowRanges toggle |
| `0x512344` | → order/script handler table, stride 0x19: +0xC marker mask (bits per §3.2), +0x10 cursor_ary index |
| `PlayerStruct+0x67/+0x6B` | first/last unit ptr (order-marker walk); `+0x146` player id |
| UnitStruct | +0x5C order list head; +0x64 rotation; +0x6A/6E/72 pos (16.16); +0x92 UnitType; +0x96 owner PlayerStruct*; +0xA6 UnitID (type); +0xA8 UnitInGameIndex; +0xAC squad digit; +0x108 Health (s16); +0x10E cloak flags; +0x110 stateMask (0x10 selected, 0x10000000 alive) |
| UnitDefStruct | +0x156 CANBUILD_ptr; +0x15E..0x172 footprint extents; +0x178 size (circle radius); +0x1FA maxHP (read as u32); +0x202..0x218 ranges; +0x220 ExplodeAs; +0x241 UnitTypeMask_0 (bit28 kamikaze [INFERRED]) |
| Order node | +0x4 type; +0xE unit; +0x16 target unit; +0x22/26/2A target pos; +0x32/34 last-seen; +0x36 build type; +0x42 flags (0x200000 cached); +0x46 issue time; +0x4A next |
