# Terrain, features & the depth story — the rest of the frame

*Static map of everything in `DrawGameScreen 0x468CF0` that is **not** the per-unit
path: the terrain tile blit, the feature passes and their exact interleave with
units, the fog-of-war overlay, and a definitive answer to "what per-pixel depth
exists in this engine". Produced for the native-resolution GL scene-pass design.
All addresses are VAs for our pristine build (ImageBase `0x400000`, md5
`8e74a1dffa1f5988624c52048f5b20cd`); disassembly via `objdump -d -M intel` on the
PE (`.text` VA→file = `VA−0x400C00`), decompiler output from a scratch copy of
`tools/ghidra-projects/TA.gpr` (613 corpus symbols + `tamem.h` structs applied).*

Evidence tags as in `frame-composition.md`: **[BINARY-VERIFIED]** = instructions/
decompiler output read for this build this session. **[CORPUS]** = TADR/`tamem.h`
name cross-checked against our bytes. **[INFERRED]** = my reading, not yet
runtime-confirmed. `main` = `*(void**)0x511DE8` (the `TAdynmemStruct`).

---

## Summary — the five things to know

1. **`0x418310` is NOT the terrain draw** — it is the map **debug overlay** (gated
   on `main+0x14280` mapDebugMode / `DAT_00511DD0`, normally a no-op). The real
   terrain pass is **`0x483FA0`**: a flat, colour-only blit of pre-rendered
   **32×32-px 8bpp tiles** indexed by a `u16` tile map. **No height, no depth, no
   LOS enters the terrain draw** — the "3D" look of cliffs is baked into the tile
   art at map-compile time. [BINARY-VERIFIED]
2. **Terrain height exists as data, not as pixels**: one byte per **16×16-px tile**
   (`FeatureStruct.height`, grid at `*(main+0x14287)`, stride 0xD). Everything that
   *positions* things — features, LOS stamps, unit binning caps, the debug grid —
   projects it with the engine's one rule `screenY = worldZ − h/2`. It never
   reaches a pixel buffer.
3. **The screen offscreen has NO depth plane.** The 8-bit z-merge `0x4B90A0` has
   exactly **one caller** (`0x4596D8`, the cargo merge inside blit `0x459200`) and
   merges **composite GAFFrame → composite GAFFrame** only. Every sprite lands on
   the screen through colour-keyed colour-only copies. There is no per-pixel depth
   truth at screen level anywhere in the engine. [BINARY-VERIFIED]
4. **Scene ordering is a strict painter's algorithm keyed on the 16-px map-tile
   row** (§3): flat features (def Height<10) → per-row [ground units, then tall
   features] top-to-bottom → projectiles/explosions → airborne units → fog.
   Terrain can never occlude a unit; only *tall features* can, purely by row order.
5. **Fog of war is a screen-space overlay pass `0x4848E0`** drawn after all world
   sprites: per 32-px screen cell, solid black (unexplored), a per-pixel **shade-
   LUT remap of already-drawn pixels** (explored, out of LOS), and GAF edge sprites
   for soft borders. Driven by two runtime maps we can read: the per-player **LOS
   counter map** (byte per 32-px tile, `PlayerStruct+0x7C`) and the shared
   **MAPPED bitmask** (`u16` per 32-px tile, `*(main+0x14273)`). [BINARY-VERIFIED]

---

## 1. The map in memory — what LoadMap builds

The TNT loader **`FUN_00483610`** (contains TADR's `LoadMap_Addr 0x483638` label)
parses the map's `.TNT` (version dword `0x2000` for retail TA; `0x1020` legacy) and
builds every runtime store, each with Cavedog's own allocation tag string
[BINARY-VERIFIED — all names read from `.data`]:

| Alloc tag | Ptr stored at | Element / size | Contents |
|---|---|---|---|
| `TILE_MAP` | `*(main+0x1428B)` | `u16` per **32-px tile**; `(Wpx/32)·(Hpx/32)·2` bytes | index of the 32×32 tile graphic for each map cell |
| `TILE_SET` | `*(main+0x14283)` | header `{u32 count; u8* pixels}` + `count·0x400` bytes | the tile graphics; tile *i*'s 1024 pixels at `*(TILE_SET+4) + i·0x400`, 8bpp, row-major 32-wide |
| `PLOT_MEMORY` | `*(main+0x14287)` | `FeatureStruct` (0xD bytes) per **16-px tile**; `W·H·0xD` | the per-tile game grid: occupancy, **height**, features (§1.1) |
| `MAPPED_MEMORY` | `*(main+0x14273)` | `u16` per **32-px tile**; `(W/2)·(H/2)·2` | bit *p* set ⇔ player *p* has explored the tile |
| `SORT_UNIT_LIST` | `*(main+0x141FB)` | `rows·cap·4` bytes | per-row unit-pointer buckets for the sweep (§3) |
| `SORT_INDICES` | `*(main+0x141FF)` | `rows·4` | per-row write cursors into SORT_UNIT_LIST |
| `SORT_LINE_COUNT` | `*(main+0x14203)` | `rows·2` (`u16`) | per-row unit counts |
| fog grid | `*(main+0x1421F)` | `{u16* buf; int cols; int rows; int cells}` struct; `cells·2` bytes | screen-space fog cell grid, 2 bytes per 32-px **view** cell (§5) |
| `TED_GENERATED_PIC` | `*(main+0x1426B)` | composite GAFFrame | the minimap picture from the TNT |
| `EYEBALL_MEMORY` | `*(main+0x1427B)` | 0x2D0 bytes | minimap viewport bookkeeping |

Dimensions & constants set alongside [BINARY-VERIFIED]:
`main+0x14233/0x14237` = map W/H in **16-px tiles** (`FeatureMapSizeX/Y`);
`main+0x14223/0x14227` = map W/H in pixels (`= tiles<<4`); `main+0x1427F` =
SeaLevel (byte, height units); `main+0x1423B/0x1423F` = view W/H in 16-px tiles,
`main+0x14243/0x14247` = view W/H in 32-px tiles; sweep bucket dims
`main+0x1424B = viewTilesX+0xC` (row capacity/columns swept) and
`main+0x1424F = viewTilesY+0x20` (rows swept — 16-tile margin above *and* below).
`FeatureDef` array (stride **0x100**) at `*(main+0x1426F)`, wreckage records
(stride **0x30**) at `*(main+0x1420B)`, scratch feature-unit at `*(main+0x1420F)`.

### 1.1 `FeatureStruct` — the 16-px tile record (stride 0xD) [CORPUS `tamem.h`, byte-confirmed]

| Off | Type | Field | Notes |
|---|---|---|---|
| +0x00 | u16 | occupyingUnitNumber | grounded unit/building on tile |
| +0x02 | u16 | airborneUnitNumber | the one airborne unit on tile |
| **+0x04** | **u8** | **height** | **THE terrain heightmap** (0–255, world-Y units; screen projection = −h/2 px) |
| +0x05 | u8 | maxHeight2x2 | max of the 2×2 patch starting here (built by `0x483210`) |
| +0x06 | u8 | minHeight2x2 | min of the 2×2 patch (ditto) |
| +0x07 | u8 | MetalValue | |
| +0x08 | u16 | FeatureDefIndex | `<0xFFFB` = anchor of a live feature; `0xFFFF` none, `0xFFFE` extension tile (`+0x0A/0x0B` = Dy/Dx back to anchor), `0xFFFD` masked-off border (set by `LoadMap_PLOT3 0x4833B0` for tiles the projection pushes off-map + lava tiles), `0xFFFC` void |
| +0x0A | u16 | wreckage index **or** Dy/Dx | when `+0x0C bit0` set: index into the 0x30-stride wreckage array at `*(main+0x1420B)` |
| +0x0C | u8 | flags | bit0 = wreckage present; **bit2 = "tall feature deferred to row sweep"** (recomputed every frame, §3); bits3–6 = last-seen player nibble (0xA = none) [INFERRED for the nibble] |

`LoadMap_AverageHeightMap 0x483370` → `0x483210(x0,y0|x1,y1)` fills +0x05/+0x06 as
min/max over the 2×2 neighbourhood — the same helper is callable for a sub-rect
(used after deformation). [BINARY-VERIFIED]

---

## 2. The terrain pass — `0x483FA0` [BINARY-VERIFIED]

**Convention:** stdcall, 1 arg `[esp+4] = OFFSCREEN* ctx`, `ret 4` @ `0x4843B3`
(the function body runs to `0x4843B3` — the `0x4840xx` partial-tile code belongs to
it). Called once per frame at `0x468DB0`, immediately followed by the debug
overlay `0x418310` (stdcall(ctx), `ret 4` @ `0x418BA8`) at `0x468DBA`.

What it does — nothing but a grid blit:

```c
// decompiled core (FUN_00483fa0), annotated
tileX0 = eyeX >> 5;  fracX = eyeX & 0x1F;      // eyeX = main+0x1431F, eyeY = main+0x14323
tileY0 = eyeY >> 5;  fracY = eyeY & 0x1F;      // 32-px tiles; frac = sub-tile scroll
cols   = (viewW(main+0x37E37) + fracX + 31) >> 5;   // +1 col/row when scrolled mid-tile
rows   = (viewH(main+0x37E3B) + fracY + 31) >> 5;
rowStride = FeatureMapSizeX/2;                  // = TILE_MAP width in 32-px tiles

// edge columns/rows (partially visible): stack-built GAF descriptor
//   {W=0x20,H=0x20,HotX=0,HotY=0,Compressed=0, ptr = tileGfx} → clipped blit 0x4B8150
// full tiles:
for (row...) for (col...) {
    u16 idx  = TILE_MAP[row*rowStride + col];               // *(main+0x1428B)
    u8* gfx  = *(u32*)(*(main+0x14283)+4) + idx*0x400;      // TILE_SET pixels
    FUN_004C6E70(ctx, screenX, screenY, gfx);               // unclipped 32×32 copy
}
// screenX = viewportLeft(main+0x37E27=128) + col*32 − fracX
// screenY = viewportTop (main+0x37E2B=32)  + row*32 − fracY
```

Key facts:
- **Colour plane only.** `0x4C6E70` → `0x4CBEF1` and the clipped path `0x4B8150` →
  `0x4CBDD1` are raw 8bpp copies — no colour key even (tiles are opaque), no depth
  read or write, no shading. The terrain repaints the whole viewport every frame,
  which is why the offscreen never needs a clear: **terrain is the frame's
  implicit "far plane"**.
- **No height/LOS participation.** Neither `FeatureStruct.height` nor any LOS map
  is consulted. Cliff faces, shadows, water — all pre-painted into the 32×32 tile
  art by the map compiler. Fog darkening happens later, over the finished scene
  (§5).
- The debug overlay `0x418310` (mapDebugMode 1–4) is the *proof* the height grid
  is authoritative: it draws the tile lattice with every vertex at
  `y − height/2` from `FeatureMap[..].height`, plus pathfinding/LOS diagnostics.
  [BINARY-VERIFIED]

---

## 3. Feature passes & the exact unit/feature interleave

`DrawGameScreen`'s world composition, with call-site VAs [BINARY-VERIFIED]:

```
0x468DB0  terrain 0x483FA0(ctx)                          ← backdrop, whole viewport
0x468DBA  debug overlay 0x418310(ctx)                    ← usually no-op
          … HUD top-bar block (only when panel state changed) …
          SORT bucket reset + hot-unit binning loop      ← §3.1
0x469849/55/61   particle layers 0/1/2  0x471F90(ctx, n)   ← wake foam (2)
          FLAT-FEATURE PRE-PASS                          ← §3.2   (defHeight < 10)
0x469964/70      particle layers 3/4                       ← feature smoke (4)
          THE INTERLEAVED ROW SWEEP                      ← §3.3
            0x469A00   DrawUnit 0x45AC20  (ground units of row)
            0x469ABB   feature 0x46A610   (tall features of row)
0x469AFD  layer 5 (trail puffs);  0x469B18 layer 6 (nanolathe spray)
0x469B22  projectiles 0x49BE60(ctx)      0x469B2C  explosions/sfx 0x420B00(ctx)
0x469B38  layer 7 (bubbles)
0x469BA3  DrawUnit 0x45AC20              ← SECOND sweep: airborne/non-ground units
0x469BD7  layer 8 … health bars over HotUnits … 0x469D2C layer 9 (impact/damage smoke, fire)
0x469D80  watch-player vcall 0x417F30    ← spectate overlay (vtable+0x28), not fog
0x469D8E  FOG OVERLAY 0x4848E0(ctx)      ← §5
          … build-cursor rect, dialogs, chat, HUD, minimap …
0x46A3DB  present 0x4C63A0
```

(`0x471F90(ctx, n)` walks the layer-`n` vector at `*(main+0x38D77) + n·0x10`,
calling `vtbl+8` (draw) on each object. These are **not** empty plugin slots: they
are the particle sfx — smoke, fire, wake foam, nanolathe spray — filed into ten
layers by their emitters, so the layer number is their draw depth. Seen live: 2 =
wake foam (under everything), 4 = feature smoke, 5 = rocket-trail puffs, 6 = the
nanolathe spray (over ground units, under projectiles), 7 = submarine bubbles, 9 =
impact/damage smoke and fire (over everything but the fog). Full RE and the native
pass that owns them: effects.md §7. [BINARY-VERIFIED, LIVE-VERIFIED 2026-09-02])

### 3.1 Binning — who sorts on what

Hot-unit list build `0x48BAE0` (screen/LOS cull → `HotUnits`/`NumHotUnits`,
`main+0x1435F/0x14367`) then the row-binning loop in `DrawGameScreen`:

```c
row = ((int16)unit->pos.z.int /*unit+0x74*/ − eyeY) >> 4  + 0x10;   // 16-px tile rows,
if (0 <= row < numRows(main+0x1424F))                               // +16-row top margin
    SORT_UNIT_LIST[row].append(unit);   // cursor *(main+0x141FF)[row], count (u16)(main+0x14203)[row]
```

**A unit's sort key is the 16-px map-tile row of its world Z (its feet)** — the
altitude term is *not* applied. (`0x48BAE0` uses height only for view-culling: it
caps the projection altitude at the tile height via `Position2GridPlot` →
`tile+0x04`.) [BINARY-VERIFIED]

### 3.2 Flat-feature pre-pass (before every unit)

Double loop over the visible tile rect (rows `eyeY>>4 − 16`, count
`viewTilesY+0x20`; cols `eyeX>>4 − 10`, count `viewTilesX+0xC`) over
`FeatureMap`:

```c
tile->flags &= ~4;                                   // clear the deferred bit — every frame
if (tile->FeatureDefIndex < 0xFFFB) {                // anchor of a live feature
    def = FeatureDef + idx*0x100;                    // *(main+0x1426F)
    if (def->Height /*+0xFA*/ < 10) {
        if (!(def->FeatureMaskHi & 8)  /*+0xFF: LOS-gated flag*/
            || seenNibble==localPlayer
            || FUN_004658E0(player, col,row, def->FootprintX, def->FootprintZ, tile->height))
            FUN_0046A610(ctx, tile, col, row);       // draw NOW — under all units
    } else tile->flags |= 4;                         // TALL → defer to the row sweep
}
```

So **features whose def `Height < 10` (metal patches, splats, rocks) are part of
the backdrop**; taller ones re-enter the painter's queue per row. [BINARY-VERIFIED]

### 3.3 The interleave — the exact ordering rule

One loop, rows top→bottom over the same rect:

```c
for (row = firstRow; row < firstRow+numRows; row++) {
    for (unit in SORT_UNIT_LIST[row])                 // ← 1. UNITS of this row first
        if ((unit->stateMask & 3) == 1 && drawUnits) {
            if (unit->stateMask & 0x10) DrawUnitSelectBoxRect(ctx, unit);
            if (unit->modelPtr /*+0x9A*/) DrawUnit(ctx, unit);        // call site 0x469A00
        }
    for (col over visible cols) {                     // ← 2. then TALL FEATURES of this row
        tile = FeatureMap + (row*W+col)*0xD;
        if (tile->flags & 4)
            if (!LOS-gated || seen || FUN_004658E0(...))
                FUN_0046A610(ctx, tile, col, row);                    // call site 0x469ABB
    }
}
```

**The rule, exactly** [BINARY-VERIFIED]:
- Sort key = **16-px map-tile row**. Unit key = `floor(worldZ/16)` (feet).
  Feature key = **its anchor tile row** (top-left of footprint; extension tiles
  are `0xFFFE` and never drawn).
- Rows are painted **back (top) to front (bottom)**; later = in front.
- **Within one row: all units first, then all features** — a tall feature
  overdraws a unit standing on the same tile row; a unit one row lower overdraws
  that feature. Within-row unit order = HotUnits order (unit-array index);
  feature order = left→right.
- Ground units only (`stateMask&3 == 1`). Everything else (airborne, in-build
  states) draws in the **second, un-rowed sweep at `0x469BA3`** — after
  projectiles and explosions, so aircraft pass over everything. (This is why the
  G4 tracer only ever saw site A `0x469A00` for the commander.)
- **No depth values are involved anywhere in this ordering.** It is pure paint
  order over colour-keyed 2D blits.

### 3.4 Feature drawing — `0x46A610` [BINARY-VERIFIED]

**Convention:** stdcall, 4 args `(OFFSCREEN* ctx, FeatureStruct* tile, int tileX,
int tileY)`, `ret 0x10` (exits `0x46A71E/0x46A76B/0x46A849`).

Projection — footprint-centred, height-corrected by the **average of the 2×2
anchor-corner tile heights**:

```c
def = FeatureDef[tile->FeatureDefIndex];             // stride 0x100
sx = tileX*16 + def->FootprintX*8 + 128 − eyeX;      // +128/+32 = viewport origin
sy = tileY*16 + def->FootprintZ*8 + 32  − eyeY
     − (tile[0,0].h + tile[0,1].h + tile[1,0].h + tile[1,1].h) / 8;   // = avgH/2
```

Three bodies:
1. **3D wreckage** (`tile->flags&1` and `def->FeatureMask & 1` clear): copies the
   wreckage record (`*(main+0x1420B) + idx*0x30`: `+4` = `Object3doStruct*`,
   `+8/+0xC/+0x10` = pos) into the **scratch feature-unit** `*(main+0x1420F)` and
   calls **`DrawUnit(ctx, fakeUnit)`** (call site `0x46A762`) — dead-unit wrecks
   go through the full composite path, own depth plane and all.
2. **Animated GAF wreckage** (`flags&1`, mask bit0 set): shadow
   `GAFGetCurrentFramePtr(wreck+0x10)` then body `(wreck+4)`, both via
   `CopyGafToContext` at `(sx,sy)`.
3. **Normal features**: GAF frames resolved from the def — static:
   `GAF_SequenceIndex2Frame(*(def+0xAC), 0)` body / `*(def+0xB0)` shadow;
   animating (mask bit1): `GAFGetCurrentFramePtrAddr(def+0xCC)` body /
   `(def+0xD8)` shadow. Shadow drawn first, only if shadows enabled
   (`main+0x37F06 & 0x10`); alpha vs plain per mask bits 2 (body) / 3 (shadow):
   `AlphaCompsteBuf2OFFScreen 0x4B8500` vs `CopyGafToContext 0x4B7F90`.

**Feature graphics are ordinary GAF frames drawn colour-keyed, colour-only —
features carry NO depth values into anything.** The only depth a feature can ever
touch is inside path 1, where the wreck's own composite has the standard per-model
plane. The visibility helper `FUN_004658E0(player, tx,ty, footX,footZ, h)`
(stdcall, `ret 0x18`) samples LOS/MAPPED at the two projected footprint corners
(`(x·16)>>5`, `(y·16 − h/2)>>5`). [BINARY-VERIFIED]

### 3.5 Projectiles `0x49BE60` and sfx `0x420B00` — not fog

Despite `frame-composition.md`'s earlier guess, these are **the weapon-fx
passes**, both stdcall(ctx) `ret 4`:
- `0x49BE60` walks the projectile array (`count main+0x141F3`, base
  `*(main+0x141F7)`, stride 0x6B): lasers as `DrawLine 0x4BE950` pairs, models via
  `0x46BAE0`, sprite weapons/contrails as GAF frames, ground shadow blob via
  `AlphaCompsteBuf2OFFScreen` — each LOS-gated per projectile (LOS byte map or
  `PositionInPlayerMapped 0x408090`). [BINARY-VERIFIED]
- `0x420B00` updates+draws the particle systems (`DAT_00511DF0..0x511F80` list →
  `0x421550`) and the explosion array **inline at `main+0x1491F`** (count
  `main+0x1491B`, stride 0x54 = `ExplosionStruct`): flashes via `0x4B8EC0`
  (shade-table blit through `*(TAProgram+0xC8)`), debris via `0x46BAE0` + GAF.
  All colour-only. [BINARY-VERIFIED]

---

## 4. The depth story — destination z does not exist

The question "what initialises the destination z-plane the unit z-merge tests
against" dissolves under the bytes:

- **`0x4B90A0` has exactly one call site in the whole binary: `0x4596D8`**, inside
  blit `0x459200`'s attached/cargo-unit merge. [BINARY-VERIFIED — full-image scan]
- Both of its operands are **composite `GAFFrame*`s** (header hotspots at
  `+4/+6`, colour plane `+0x10`, depth plane `+0x14`): it merges a *child unit's
  composite* into the *parent unit's composite*, pixel test
  `dst_depth ≤ src_depth + bias` (0=far, 255=near, bias = elevation delta —
  as pinned in `composite-buffer.md §4`).
- The destination depth plane is therefore the **parent composite's** plane,
  initialised to 0x00 (far) by allocator `0x437BE0` on every rebuild and populated
  by the rasterisers (`0x459830/0x459C70` via `GAF_DrawTransformed`, depth =
  `vertexY + 0x32 (+0x4B)`). Its lifetime = the composite's (per-`Object3do`,
  persistent, rebuilt on move/animation).
- **Every sprite→screen path is colour-only**: terrain (`0x4CBEF1/0x4CBDD1`),
  features/units (`CopyGafToContext 0x4B7F90` → `0x4CBE70`), alpha variants
  (`0x4B8500` → `0x4CBF2C` shade-table), fog (§5). The `OFFSCREEN` context
  (12 dwords; `+0x08` pitch, `+0x0C` pixel base, clip rect `+0x1C..0x28` set by
  `0x4C6B10`) has no depth member at all.

**Conclusion: per-pixel depth exists only *inside* one unit's (or one wreck's)
composite sprite, resolving piece-vs-piece and cargo-vs-carrier. Between objects
and against terrain, the entire game is ordered by the §3 painter's rules.**
Terrain never occludes a unit (a unit "behind" a cliff still draws on top of the
cliff art); only a tall feature on a lower row can cover it.

---

## 5. Fog of war — `0x4848E0` + the LOS maps

### 5.1 The overlay pass `0x4848E0` [BINARY-VERIFIED]

stdcall(ctx) `ret 4` @ `0x484B48`, called at `0x469D8E` after all world sprites.
Lazy: if `LosType(main+0x14281) & 8` clear, first rebuild the **screen fog grid**
via `0x4843C0` and set bit 3. Then per 32-px screen cell of
`fg = *(main+0x1421F)` (`{u16* buf; cols; rows; cells}`, cell = 2 bytes
`[b0 = unexplored-corner mask, b1 = out-of-LOS-corner mask]`):

| Cell value | Drawn as |
|---|---|
| `b0 == 0xF` (all 4 corners unexplored) | `DrawBar` solid rect, GUI colour 0 (black) |
| `b1 == 0xF` (explored, all corners out of LOS) | full-cell **darken**: `0x4BFE10` — every pixel remapped `p = shadeLUT[p]` through `*(TAProgram+0xCC)` — or, with `main+0x37F06 & 0x40` (dither option), `0x4BFF20` — checkerboard pixels forced to palette index 0 |
| `b1 in 1..0xE` | LOS **edge sprite**: GAF `*(main+0x1486F + ((x+y+scrollParity)&3)*4)`, frame `b1−1` (14 corner-combination shapes, 4 phase variants) blitted as a **mask**: where sprite ≠ colour-key, dest pixel is LUT-remapped (`0x4B86E0`) or dither-blacked (`0x4B88D0`) |
| `b0 in 1..0xE` | unexplored **edge sprite**: GAF `*(main+0x1485F + ..&3)*4)`, frame `b0−1`, plain `CopyGafToContext` (black art, keyed) |

So: **per-32-px-tile decision, per-pixel application, and the darkening is a
palette-index remap of pixels already in the frame** — not an overdraw of new
content. Gated on `TAProgram+0xF1 & 1` (shading capability).

### 5.2 The screen fog grid builder `0x4843C0` [BINARY-VERIFIED]

void, no args, `ret` @ `0x4848D6`. Clears the grid, then for each visible 32-px
map cell (origin = rounded `eyeX>>5, eyeY>>5`):
- if the **local player's LOS byte == 0** and true-LOS mode (`LosType&2`): OR
  corner bits (1/2/4/8) into `b1` of the up-to-4 neighbouring cells;
- if the **MAPPED bit for the local player == 0**: same into `b0`.
Plus map-border corner completion. The result is the corner-mask smoothing that
picks the 14 edge shapes.

**Invalidation** (clears bit 3 of `LosType`, forcing a rebuild): every scroll
path (`0x41C54B..0x41D485`, ~17 sites in the scroll/minimap code), every LOS stamp
add/remove that changes the local player's maps (`0x481911, 0x481D2D, 0x481D73,
0x482293`), map load (`0x483CB7`), and `Game_SetLOSState`. In practice the grid
rebuilds nearly every frame something moves. [BINARY-VERIFIED]

### 5.3 The LOS maps themselves — what to read at runtime

All grids are **32-px tiles**, dims `LOSW = FeatureMapSizeX/2`,
`LOSH = FeatureMapSizeY/2`:

- **Per-player LOS counter map** — `PlayerStruct+0x7C` (`PlayerStruct` array at
  `main+0x1B63`, stride **0x14B**, 10 players; local id byte `main+0x2A43`,
  watched-player id `main+0x2A42`). One **byte per tile, an overlap COUNTER**
  (not a bitmask): each unit's stamp `0x482270` **increments** every tile under
  its LOS circle; the removal twin decrements. Non-zero ⇒ in LOS. Dims at
  `+0x80/+0x84`, byte count at `+0x88`. LOS circles are **GAF frames**
  (sequence `*(main+0x1485B)`, frame ≈ `sightDistance>>5 − 5`, colour-key =
  outside), stamped at the **projected** position: tile `(x>>5, (z − max(alt,
  (SeaLevel+1)<<16)/2)>>5)` minus frame hotspot — LOS is a screen-space disc,
  altitude-corrected like everything else. [BINARY-VERIFIED]
- **MAPPED (explored) map** — `*(main+0x14273)`, **u16 per tile, bit p = player p
  has seen it**, stamped by `0x481930` with the same GAF circle.
  `Game_SetLOSState 0x4816A0` re-primes both on mode change (fill 0xFF/0x00 per
  `LosType` bits 0/1) and re-stamps every unit.
- **Raycast-LOS terrain grid** — `*(main+0x1428F)` (`LOS_GridBuf`, cols/rows at
  `main+0x14293/0x14297`): 2 bytes per 32-px cell of terrain min/max height,
  consulted only in `LosType&4` mode (true LOS with terrain blocking) by the ray
  walker in `0x482270`/`0x4825B0`. Built at load (`0x482C20`). [BINARY-VERIFIED
  reads; builder INFERRED from call site]
- `LosType` u16 at `main+0x14281`: bit0 = mapping on (start unexplored), bit1 =
  true LOS (else explored==visible), bit2 = terrain-blocking raycast LOS, bit3 =
  screen fog grid valid. [BINARY-VERIFIED]

---

## 6. SYNTHESIS — depth truth for the native-res GL scene pass

**What per-pixel depth truth exists in the engine today:**

| Layer | Engine depth | Where |
|---|---|---|
| Terrain | **none** — implicit farthest plane, repainted whole every frame; height baked into tile art | draw `0x483FA0`; data `TILE_MAP/TILE_SET` |
| Features | **none** — colour-keyed GAF blits; ordering = anchor-tile row in the §3 sweep | `0x46A610` |
| 3D wrecks | 8-bit plane inside their own composite (piece-vs-piece only) | via `DrawUnit` at `0x46A762` |
| Units | 8-bit plane inside the per-unit composite (`0=far,255=near`, `depth = pieceY+0x32`); **never compared across units or against the scene** | `0x459200`/`0x4B90A0` |
| Projectiles/sfx/fog | none | §3.5, §5 |

**What we must synthesise for a full-scene GL pass that reproduces stock
occlusion at native resolution — and where to read the inputs live:**

1. **Terrain = backdrop at far depth.** Match the engine exactly by rendering (or
   letting cnc-ddraw upscale) the tile layer at depth=FAR. No terrain fragment
   may ever occlude a unit — that is engine behaviour, not a shortcut. If we want
   *better*-than-engine (cliffs occluding), the height field is
   `FeatureMap[ty*W+tx].height` (`*(main+0x14287)`, stride 0xD, byte +4, 16-px
   grid) with `screenY = z − h/2`; but that changes gameplay-visible info —
   default to engine-faithful.
2. **Scene ordering key**: give every object a depth derived from its **16-px
   sort row**, e.g. `depth = worldZfeet >> 4` for units (`unit+0x74` int16) and
   `anchorRow` for features, with the §3 tie-breaks (flat features behind
   everything; units before features within a row; airborne pass above all ground
   content; projectiles between ground and air). A single scalar
   `rowKey*2 + isFeature` reproduces stock layering exactly in a GL depth test.
   Within one unit we already own real per-pixel depth from our rasteriser
   (engine formula `modelY + 0x32` if we ever hand planes back).
3. **Feature sprites**: source GAFs are reachable via
   `FeatureDef = *(main+0x1426F) + idx*0x100` (`+0xAC/+0xB0` static sequences,
   `+0xCC/+0xD8` anim-state for animating defs, `+0x94/+0x96` footprint,
   `+0xFA` height, `+0xFE` mask). Position with the §3.4 formula (avg 2×2 tile
   height /2). Wrecks: records at `*(main+0x1420B)` (stride 0x30) with a live
   `Object3doStruct*` at +4 — renderable by our existing 3DO path.
4. **Fog/LOS at native res**: do NOT sample the engine's screen fog grid (32-px
   blocks, view-sized, rebuilt lazily) — sample the *source* maps per fragment
   for smooth native-res fog:
   - visible: `PlayerStruct(main+0x1B63 + localId(main+0x2A43)·0x14B) + 0x7C`
     byte map, `>0` ⇒ lit; tile = `(wx>>5, (wz − alt/2)>>5)`.
   - explored: `*(main+0x14273)` u16, bit `localId`.
   - Match stock look: explored-dark = shade-LUT `*(TAProgram+0xCC)` applied to
     the composed colour (or just N% darken in linear GL); unexplored = black.
   - Respect `LosType(main+0x14281)` bit1: in "mapped" mode everything explored
     is fully lit.
5. **Draw-order hooks**: the scene pass slots cleanly between the terrain call
   (`0x468DB0` ret) and the fog call (`0x469D8E`) — everything in between is what
   we would replace; fog, HUD, dialogs and present stay engine-side. All
   world→screen maths reuse the one projection
   `sx = wx − eyeX + 128, sy = wz − alt/2 − eyeY + 32`
   (eye at `main+0x1431F/0x14323`, viewport rect `main+0x37E27..0x37E33`).
6. **Stability caveats**: `FeatureMap` tile flags bit2 is *recomputed by the
   engine's own pre-pass each frame* — if we suppress the engine feature passes we
   must not depend on it; classify tall-vs-flat ourselves from `def->Height<10`.
   The SORT_* buckets are engine-internal scratch (view-sized, indices relative
   to `eyeY>>4 − 16`) — read-only useful for parity checks, not as our source of
   truth (walk HotUnits or the unit array instead).

---

## Appendix — address & offset tables

### Functions

| VA | Role | Convention |
|---|---|---|
| `0x483FA0` | **terrain tile blit** (body to `0x4843B3`) | stdcall(OFFSCREEN*), ret 4 |
| `0x418310` | map **debug overlay** (mapDebugMode/`0x511DD0` gated) | stdcall(OFFSCREEN*), ret 4 @`0x418BA8` |
| `0x4C6E70` → `0x4CBEF1` | full 32×32 tile copy (colour-only) | — |
| `0x4B8150` → `0x4CBDD1`/`0x4CC51D` | clipped GAF-descriptor blit (edge tiles; raw/RLE) | stdcall, ret 0x10 |
| `0x46A610` | **feature draw** (3 bodies: 3D wreck → DrawUnit @`0x46A762`; GAF wreck; normal GAF) | stdcall(ctx,tile,tx,ty), ret 0x10 |
| `0x4658E0` | feature LOS/MAPPED visibility test | stdcall ×6 args, ret 0x18 |
| `0x49BE60` | projectile pass | stdcall(ctx), ret 4 |
| `0x420B00` | explosions/particles pass (`0x421550` per-system) | stdcall(ctx), ret 4 |
| `0x4B90A0` | 8-bit z-merge — **sole caller `0x4596D8`** (cargo merge) | composite→composite only |
| `0x4848E0` | **fog overlay** | stdcall(ctx), ret 4 @`0x484B48` |
| `0x4843C0` | screen fog-grid rebuild (corner masks) | void, ret @`0x4848D6` |
| `0x4BFE10` / `0x4BFF20` | full-cell darken: shade-LUT remap / checkerboard black | — |
| `0x4B86E0` / `0x4B88D0` | edge-sprite darken (mask→LUT / mask→dither) | — |
| `0x483610` | **TNT map loader** (TADR label `LoadMap_Addr` = `0x483638`) | builds §1 stores |
| `0x483370`→`0x483210` | (re)build min/max 2×2 heights | — |
| `0x4833B0` | `LoadMap_PLOT3` — mask off-projection border tiles (`0xFFFD`) | — |
| `0x482270` / `0x481930` | LOS counter stamp / MAPPED bit stamp (GAF circle `*(main+0x1485B)`) | — |
| `0x4816A0` | `Game_SetLOSState` — re-prime both maps + restamp all units | — |
| `0x482AC0` | `UNITS_RebuildLOS` — per-unit stamp refresh | — |
| `0x471F90` | particle-layer draw walker (layer n at `*(main+0x38D77)+n·0x10`; smoke/fire/wake/nano — effects.md §7) | (ctx, n), `stdcall` `ret 8` |
| `0x48C190` | get watched/next-selected unit (debug + spectate) | — |

### DrawGameScreen call sites (world section)

`0x468DB0` terrain · `0x468DBA` debug · `0x46990F/0x469920/0x46992F` flat-feature
pre-pass (LOS-check / gated draw / plain draw) · `0x469A00` DrawUnit ground ·
`0x469A9C/0x469ABB` tall-feature LOS/draw · `0x469B22` projectiles · `0x469B2C`
sfx · `0x469BA3` DrawUnit airborne · `0x469D80` spectate vcall · `0x469D8E` fog ·
`0x46A3DB` present.

### `main+…` (TAdynmemStruct) data offsets

| Offset | What |
|---|---|
| `0x141F3/0x141F7` | projectile count / array (stride 0x6B) |
| `0x141FB/0x141FF/0x14203` | SORT_UNIT_LIST pool / row cursors / row counts (u16) |
| `0x1420B` | wreckage array (stride 0x30: +4 Object3do*, +8..+0x10 pos) |
| `0x1420F` | scratch feature-unit (UnitStruct*) for 3D wreck draw |
| `0x1421F` | screen fog grid `{u16* buf, cols, rows, cells}` (2 B/32-px view cell) |
| `0x14223/0x14227` | map W/H pixels |
| `0x14233/0x14237` | map W/H in 16-px tiles (`FeatureMapSizeX/Y`) |
| `0x1423B/0x1423F` | view W/H in 16-px tiles; `0x14243/0x14247` in 32-px tiles |
| `0x1424B/0x1424F` | sweep cols (viewTilesX+0xC) / rows (viewTilesY+0x20) |
| `0x1426B` | minimap composite (`TED_GENERATED_PIC`) |
| `0x1426F` | FeatureDef array (stride 0x100; +0x94/96 footprint, +0xAC/B0 GAF seqs, +0xCC/D8 anim states, +0xFA Height, +0xFE mask) |
| `0x14273` | **MAPPED_MEMORY** u16/32-px tile, bit=player explored |
| `0x1427B` | EYEBALL_MEMORY (minimap viewport) |
| `0x1427F` | SeaLevel byte; `0x14280` mapDebugMode; `0x14281` **LosType** u16 (b0 map, b1 LOS, b2 raycast, b3 fog-grid-valid) |
| `0x14283` | **TILE_SET** `{u32 count; u8* pixels}`; graphics at `*(+4) + idx·0x400` |
| `0x14287` | **FeatureMap/PLOT_MEMORY** — 0xD-stride per-16-px-tile grid, **height byte at +4** |
| `0x1428B` | **TILE_MAP** u16 tile index per 32-px tile |
| `0x1428F/0x14293/0x14297` | raycast-LOS terrain grid (2 B/32-px cell) + dims |
| `0x1431F/0x14323` | eyeX/eyeY (px) |
| `0x1435F/0x14367` | HotUnits / NumHotUnits |
| `0x1485B` | LOS-circle GAF sequence; `0x1485F+4n`/`0x1486F+4n` fog edge GAF variants (n=0..3) |
| `0x1491B/0x1491F` | explosion count / inline array (stride 0x54) |
| `0x2A42/0x2A43` | watched / local player id |
| `0x1B63 + id·0x14B` | PlayerStruct: **+0x7C LOS counter map**, +0x80/+0x84 dims, +0x88 size, +0x146 player id (via PlayerInfo path in stamps) |
| `0x37E27..0x37E33` | viewport rect on the offscreen (left=128, top=32, right, bottom) |
| `0x37E37/0x37E3B` | view W/H px; `0x37E1B` OFFSCREEN template ptr |
| `0x37F06` | gfx options (bit4 shadows, bit6 dithered fog) |
| TAProgram `+0xC8/+0xCC` | shade LUTs (explosion-shadow / **fog darkening**); `+0xF1` bit0 shading enabled |

### Runtime confirmations & corrections (G12a, 2026-08-31)

The scaffold module (`tagpu_scaffold.c`, Phase D) reproduces §3's sweep live and confirmed:

- **Sweep geometry verified at runtime**: at 640×480 the live reads give viewport (128,32),
  view 512×416 (the bottom 32-px HUD bar is part of the rect — viewH = H−64, not H−32),
  sweep 44×58 = `viewTilesX+0xC` × `viewTilesY+0x20`, exactly the §1 formulas.
- **§3.2's height gate byte-confirmed**: `cmp BYTE [def+0xFA], 0xA` at `0x4698CF`; tall
  features get `tile->flags |= 4` at `0x469936`. Footprints are **signed** words
  (`movsx` at `0x4698F2/0x469900`).
- **The viewport offsets are BAKED as constants in the draw paths**: feature projection
  `0x46A610` computes `(tileX + 8) << 4` and `(tileY + 2) << 4` (the `+128/+32` as
  hard immediates), it does NOT read `main+0x37E27`. Consistent with
  [resolution](resolution.html): left=128/top=32 are constants engine-wide; only
  right/bottom derive from the mode.
- **Junk FeatureDef entries exist**: on Painted Desert, tiles reference def indices
  (21, 22, 28) whose 0x100-stride entries hold garbage in `+0x90..+0x9F` (invalid GAF
  sequence pointers, wild footprints). Any def-array walker MUST range-guard footprint/
  pointer fields — an unclamped footprint loop hung the render thread for minutes
  ([INFERRED: these are defs beyond the map's real def count, or non-feature records
  aliased into the array]).
- **Painted Desert has no tall features at all**: whole-map census — 433 anchors, every
  real def Height ∈ {0, 5}. On that map nothing ever occludes a unit; occlusion testing
  needs a tree map.

### Corrections to earlier notes
- `frame-composition.md`: "terrain 0x418310" → the terrain pass is **0x483FA0**;
  `0x418310` is the debug overlay. "fog of war 0x420B00/0x49BE60" → those are the
  **sfx and projectile passes**; fog is **0x4848E0** (after health bars, before
  build-cursor/UI). "features 0x46A610 in the row sweep" → features draw in TWO
  places: flat pre-pass (`0x469920/2F`) and per-row deferred (`0x469ABB`).
