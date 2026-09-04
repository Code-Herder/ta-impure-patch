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
   art at map-compile time. [BINARY-VERIFIED] **Since G13b it is ours** — §7 has
   the native pass, the two detours that take it, and the compositing model that
   owning a full-viewport layer forced.
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
   **Since G13b this pass is ours too** — it has to be, because its shade remap
   would rewrite the terrain key fill; the one thing its skip path must keep is the
   lazy grid rebuild every native pass samples (§7.2).

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
`main+0x14223/0x14227` = map W/H in pixels (`= tiles<<4`) — **note these are
*not* the fog grid's cols/rows; those are fields inside the struct behind the
pointer at `main+0x1421F`** (§5.2); `main+0x1427F` =
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

**The lattice geometry** [BINARY-VERIFIED 2026-09-02, G13c]. The grid origin is
a *half-cell* offset from the map cells: `col0 = eyeX/32 − 1` when `eyeX % 32 <
16`, else `eyeX/32` — i.e. `col0 = floor((eyeX − 16)/32)`, and the overlay draws
cell `(col,row)` at `vp + (±16 − eye%32) + col·32`, so entry `(gx,gy)` covers
world x `[32·(col0+gx) + 16, +32)`. Equivalently **origin = `32·col0 + 16`**, and
a grid *corner* sits exactly at a map cell's *centre*. The four bits are those
four surrounding cells:

| bit | corner | map cell |
|---|---|---|
| 1 | top-left | `(col0+gx, row0+gy)` |
| 2 | top-right | `(col0+gx+1, row0+gy)` |
| 4 | bottom-left | `(col0+gx, row0+gy+1)` |
| 8 | bottom-right | `(col0+gx+1, row0+gy+1)` |

so one dark map cell sets a *different* bit in each of the four grid entries
around it, and `0xF` needs all four cells dark. This half-cell offset is why
per-fragment source-map sampling is ~16 px out of register (§6 item 4).

**The grid is not a lazy cache in practice** — measured stable and in step with
the frame at 25 s apart with the camera parked; the ~17 invalidation sites mean
it rebuilds whenever anything moves. Reading it every frame is correct.

**A caution about the source maps.** With the grid and MAPPED read *in the same
frame*, at the same cells, using the stride this section documents, they
disagree: on Two Continents MAPPED reported explored for cols 80..98 of row 20
while the grid (and the screen) had only 92..96 lit. The grid matched the
screenshot exactly; MAPPED did not, and re-reading it at other strides did not
reconcile them either. Unresolved — and it does not matter for rendering, since
the grid is what the engine draws. Do not build a fog rule on `main+0x14273`
without re-checking it against the drawn frame first.

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

**The four bits are independent, and "fog is on" is NOT bit 0** [LIVE-VERIFIED
2026-09-02, G13b]. `LineOfSight` cycle stage 1 with the SKIRMISH *mapping* option
off gives **`LosType = 14`** (bits 1,2,3 — true LOS, raycast, grid valid; mapping
clear), and the grid in that state reads, per cell `[b0, b1]`:

```
00 0F 00 0F 00 07 00 03 00 03 00 01 00 00 …
```

— **`b0` all zero** (nothing is unexplored, because mapping is off) while **`b1` is
fully populated** (plenty is out of LOS). So the engine paints a live grey band with
`LosType & 1 == 0`. Any shader gate of the form `if (LosType & 1)` drops the whole
rule in this configuration; the correct gate is simply *"the grid exists"*, since
the builder already encodes an inactive mode as an all-zero mask. This cost G13c a
latent bug that only became visible when G13b suppressed the engine's own overlay
(§7.5).

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

**Where that stands now (G13a, 2026-09-02).** Rows 2 and 3 of the table are history for
the native pass: `tagpu_feat.c` draws every feature itself and **writes the depth** row
2 says the engine has none of, and the 3D wrecks of row 3 are drawn by the native unit
pass. The full leaf-level RE — the three bodies of `0x46A610`, the exact LOS gate, the
sweep-rect clamps and the ownership detour — is on its own page,
[Features](features.html); §3 below remains the map of *when* the engine draws them.
The consequence for this section: synthesis item 2's "single scalar `rowKey*2 +
isFeature`" is now real code, and the [scene-depth scaffold](native-res-design.html) that
stood in for it is superseded as the occluder.

**What we must synthesise for a full-scene GL pass that reproduces stock
occlusion at native resolution — and where to read the inputs live:**

1. **Terrain = backdrop at far depth.** ***Done — G13b, §7 below.*** Match the
   engine exactly by rendering (or
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
4. **Fog/LOS at native res**: ~~do NOT sample the engine's screen fog grid —
   sample the *source* maps per fragment~~. **SUPERSEDED 2026-09-02 — this advice
   was wrong, and the G13c gate reversed it. Sample the grid.** Per-fragment
   source-map sampling was implemented in all four passes and does *not*
   reproduce the overlay: measured on Two Continents, MAPPED (`*(main+0x14273)`,
   u16, bit `localId`, stride `mapW16/2`) reports **explored across a whole band
   the engine paints solid black**, so the discard never fires and features and
   units get drawn over unexplored ground. The grid at `*(main+0x1421F)` matches
   the drawn frame exactly, cell for cell, at the same instant. Two independent
   reasons the source maps cannot work by themselves:
   - the grid lattice is offset **half a cell**: a grid corner sits at a map
     cell's *centre* (`origin = 32·col0 + 16`, `col0` = the builder's rounded
     `eye>>5`), so a per-cell test lands ~16 px off; and
   - the overlay's shape comes from **4-bit corner masks**, feathered by 14 edge
     sprites — a per-cell boolean cannot express it.
   What actually works (`tagpu_glsl.h`, G13c): upload the grid as an RG8 texture
   — its two bytes *are* `(b0, b1)`, so it uploads with no conversion — and take
   bilinear coverage over the four corner bits, thresholded at 0.5. That gives
   the 14 shapes for free (`0xF` → everywhere, `0x3` → the top half, a lone
   corner → its quadrant), with a clean edge where the engine dithers one.
   - The grey darken **is** the shade-LUT `*(TAProgram+0xCC)`, applied to the
     palette **index** before the palette fetch — not an N% darken of the
     composed colour, which comes out visibly too dark on tree canopies
     (measured: 19 768 mismatched px vs 97 with the real remap).
   - `LosType(main+0x14281)` bit1 needs no shader flag: the builder only writes
     `b1` when it is set, so in "mapped" mode the grey mask is simply all-zero.
   - Units and effects are **hidden**, not darkened, in grey — the engine draws
     no unit it cannot currently see; terrain, features and wreckage stay and
     are remapped. (Verified: an enemy solar on grey ground disappears while its
     flattened terrain footprint remains.)
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

## 7. Owning the terrain — G13b [LIVE-VERIFIED 2026-09-02]

`tagpu_terr.c` reproduces §2 and `tagpu_terrown.c` takes the draw. The drawing half
is as small as §2 makes it look; **the gate is the compositing model, and owning a
full-viewport layer inverts it.**

### 7.1 The pass

Armed by `tagpu_terr.on` (tokens `log`, `passive`, `over`, `key=N`). It rides the
native pass's frame like the feature pass, walks the same grid §2 walks, and emits
one quad per visible cell. Three things fall out of §2 exactly as predicted:

- **Terrain is the frame's far plane**, so it draws at depth key `0.10` — under the
  flat-feature band (`0.40`) and under particle layers 0..2 (`0.30`), i.e. under
  everything, and it writes depth so nothing has to be ordered against it again.
- **Water animates for free.** It is palette cycling and the native pass already
  re-uploads the live palette every frame; sampling it is the whole implementation.
- **The engine's arithmetic is reproduced, not corrected.** `cdq; and edx,0x1f;
  add; sar 5` is a division *toward zero*, and `cols` is `ceil((viewW+fracX)/32)`
  with the remainder test the engine does at `0x48403E`. `div32_trunc`/`ceil32`
  carry a comment saying so, because a floor-based "fix" would be wrong for a
  negative eye.
- **The grid is watertight, at every zoom.** [ANALYSED 2026-09-03] Column *c*'s
  right edge is emitted as `float(N) + 32` and column *c+1*'s left edge as
  `float(N + 32)`, where `N = vpL + c*32 - fracX` is an int; those are the **same
  float** for every `N` a viewport can produce (`|N|` far below 2²⁴), and the vertex
  shader's scale-about-the-centre is a pure function of that position — so two
  neighbours can never disagree about where their shared edge is. Plain
  rasterisation therefore **cannot** open a gap between tiles — that much is
  still true, and it is worth knowing because a coloured hairline in the world
  looks exactly like the tile-seam artefact that 2D-drawn-in-3D is famous for.

  **The sentence that used to follow it was wrong, and it cost this project the
  interior-crack hunt.** It read: *atlas bleed is out for the same kind of
  reason — interpolated `u` stays inside `[u0, u1)` for any fragment centre
  inside the quad, so a fragment cannot reach the neighbouring cell however
  small the quad gets.* The half-open interval is the error. A fragment centre
  can land **exactly on** the quad's far edge, not merely inside it, and then
  the interpolated `u` is exactly `u1` — the one value `[u0, u1)` excludes — and
  `GL_NEAREST` resolves `floor(u1·W)` to the first texel of the **next** atlas
  cell. Geometry being watertight is what makes this possible rather than what
  rules it out: the two quads agree about the edge to the bit, so a sample point
  sitting on it is a real case rather than a rounding accident. §7.6's fifth
  failure mode is that bug, reproduced and fixed.

**The atlas is built once per map**, not per frame: `LoadMap` builds `TILE_SET` and
nothing changes it afterwards. A `GL_TEXTURE_2D_ARRAY` is not viable — Two Continents
has **5062 tiles** against the usual 2048-layer cap — so it is one `GL_R8` texture of
32×32 cells on a **34-texel pitch**, 64 per row: **2176×2720 for 5062 tiles (5.9 MB)**,
sized from the count and capped at `GL_MAX_TEXTURE_SIZE`. The spare texel on each side
is the cell's own outermost row/column repeated — a guard rail, not padding, and §7.6's
fifth failure mode is why. A map change is the `TILE_SET` pointer or its count moving;
measured live (before the pitch changed), switching Two Continents → Anteer Strait **in
the same process** rebuilt it for 7051 tiles.

**Fog needed one change, and only one.** Terrain is now the bottom layer, so where the
overlay paints an unexplored cell **solid black it must paint black rather than
discard** — there is nothing behind it any more. That is `TAGPU_GLSL_FOG_TERRAIN` in
`tagpu_glsl.h`, taking the black from palette index 0 (the engine's `DrawBar` GUI
colour 0) rather than assuming `vec3(0)`. Otherwise the G13c rule drops in unchanged:
terrain **darkens, never hides**, in grey.

### 7.2 Taking the draw — and why the skip path is not empty

One prologue detour, `tagpu_detour.c`'s shape, on

```
0x483FA0   83 EC 48 | 8B 0D E8 1D 51 00      sub esp,0x48; mov ecx,[0x511DE8]
```

— **nine** stolen bytes (the first instruction boundary at or past five), resuming at
`0x483FA9`, with the four bytes past our `jmp` NOPped. `tagpu_detour_leaf` grew a
`nst` range of 5..16 for it.

But **the terrain repaint is why the engine's offscreen never needs clearing** (§2).
Skip it outright and the overlays the engine still draws land on last frame's garbage.
So the skip path is not a bare `ret 4`: it calls back into C and **fills the viewport
rect of the offscreen with one palette index, the KEY** — `base = ctx+0x0C`,
`pitch = ctx+0x08`, clamped to the context's own inclusive clip rect `ctx+0x1C..0x28`.
`tagpu_detour_leaf_call` is that variant (`pushad`, `push [esp+0x24]` = the callee's
first stack arg, `call fn`, `add esp,4`, `popad`, `ret n`).

**The fog overlay goes with it**, second detour, same flag:

```
0x4848E0   83 EC 2C | 53 | 55                sub esp,0x2C; push ebx; push ebp
```

Two reasons: its grey band is a shade-LUT remap of pixels *already in the frame*, so it
would rewrite the key fill into flat grey blobs — and every one of them would read as
"the engine drew something here"; and we have reproduced the overlay exactly since G13c,
so drawing it twice is wrong anyway. **But its first act is the lazy rebuild of the
screen fog grid we sample**, and dropping that would freeze the grid and take G13c's
parity with it. The skip path replicates those five lines and nothing else:

```
4848f2  test byte [esi+0x14281],bl   ; bl = 8
4848f8  jne  0x484911
4848fa  call 0x4843c0                ; rebuild the grid
484904  or   word [eax+0x14281],bx   ; LosType |= 8
```

### 7.3 The inverted composite

Until G13b our passes only ever covered *sprites*, so the composite rule was "drop our
empty pixels and let the engine's frame show" (`tagpu_native.c`, `CFS`). Terrain covers
the whole viewport, so that rule would hide everything the engine still draws inside
it — health bars over HotUnits (`0x469BD7`), nanoframe wireframes, the build-cursor
rect, chat, dialogs, the spectate overlay.

**The key inverts it in one line.** Every index in the viewport other than the key is,
by construction, something the engine drew *after* our fill, so:

```glsl
if (uKey >= 0) {
  ivec2 p = clamp(ivec2(uv * vec2(uSurfSz)), ivec2(0), uSurfSz - 1);
  if (int(texelFetch(uSurf, p, 0).r * 255.0 + 0.5) != uKey) discard;
}
```

— we discard **our** fragment there and the engine's own already-drawn frame shows
through. No second full-screen draw and no palette plumbing: cnc-ddraw already holds
the 8bpp frame as an `R8` **index** texture whose texel *(x,y)* is game pixel *(x,y)*
(`g_ogl.surface_tex_ids[tex_index]`, plumbed through `TAGPU_FRAME.surface_tex`).
`texelFetch`, not `texture()` — the filter state on that texture belongs to cnc-ddraw
and may be linear, and interpolated palette indices are garbage. `uKey < 0` is the
pre-G13b behaviour exactly.

Two ordering rules keep it honest:

- **`uKey` is armed only once the engine has actually run a key-filled frame.** On the
  frame the skip is first set, the engine's surface still holds a real terrain blit;
  inverting on that would hide the whole world for a frame. `tagpu_terrown_filled()`
  is cleared by `set_skip()` and set by the fill.
- **`tagpu_native_frame` must never return early while we own the terrain.** The
  engine's frame is a flat key fill and only the composite turns it back into a
  picture; the gather hands the draw back on every bail, so `nterr == 0` also means
  the engine is painting terrain again.

**Choosing the key.** Index 0 is not free — the fog's own solid black is `DrawBar` GUI
colour 0. The default is **254**, and it is not a guess: the engine's 16-entry GUI
colour table at `main+0xDCB` reads
`00 04 02 06 D5 05 CB 55 5A 09 E9 20 D3 FD C2 FF` — it uses `0xFD` and `0xFF` and
**leaves `0xFE` in the gap between them**. Verified on captured frames too: index 254
appears nowhere in the panel, minimap, top bar, chat, build panel or selection boxes.
`key=N` in `tagpu_terr.on` moves it if a mod's UI ever collides.

**And it is LOUD, which is a diagnostic asset.** `palettes/palette.pal` ends in the
Windows system tail — `249..255` = pure red, green, yellow, blue, magenta, cyan, white —
so index 254 is `(0,255,255)`, **bright cyan**. Neither it nor 253 (`(255,0,255)`,
magenta) occurs in the tile art: across 350+ captured frames of Two Continents, **zero**
pixels equalled either colour.

So `key=N` is the **first** move when hunting a leak, not the last: set `key=253` and
see whether the artefact changes colour. If it does it is the fill; if it does not it is
ours — and both answers arrive in one frame. Do it *before* measuring anything, because
with the default cyan key a "how cyan is this pixel" test also fires on **ocean and
dithered water**, which is a trap worth naming; magenta is the colour Two Continents has
no opinion about. This is how §7.6's fourth failure mode was pinned.

### 7.4 Verified

`terr1` on Two Continents (`feat-forest`) and Anteer Strait, 1024×768.

| Check | Result |
|---|---|
| **Drawing parity** (`terr.on="log over"`, ours drawn over the engine's, features muted) | Fog off: **0** differing pixels in every terrain-only band; the only differences in the whole frame are our own native units (panel below). Fog on, measured by dilating the difference mask 8 px to exclude the boundary: 90.6 % of the viewport survives that and **0** of it differs |
| **Sub-tile scroll** | exact at `frac=(0,0)`, `(6,8)` and `(25,31)`; outside the fog edge, 289 px of 625 306 differ (0.046 %) and they hug the boundary. No seams, no grid pattern. **The whole-viewport figure read 0.91 % at all four cameras** — a constant across unrelated positions is a feature of the frame, not a geometry error (field-notes "Verification discipline" §4) |
| **Ownership** | the engine's 8bpp surface inside the viewport is **99.33–99.98 % key** depending on how much UI is up — **no terrain left at all**; the remainder is the cursor and the overlays listed below |
| **The inversion** | engine chat text, the self-destruct countdown, `PAUSED`, the green selection box, the build panel and the mouse cursor all survive **inside** the viewport, over our terrain |
| **Fog** | engine's own overlay vs ours at the same camera: **99.06–99.39 %** of the viewport agrees on lit-vs-grey (`feat-forest`), **99.48 %** of pixels identical at a fully-fogged map corner, mean abs diff **0.32/255**. The disagreement is a 2–4 px band on the boundary — the engine dithers its edge sprites, we threshold cleanly (G13c, deliberate). **No double-darkening** where the engine's overlay is still live: mean grey-band luminance **62.21** (engine drawing its own) vs **61.77** (ours) — a second remap would roughly halve it |
| **Map change, in-process** | Two Continents → Anteer Strait rebuilt the atlas 2048×2560/5062 → 2048×3552/7051, dims 336×400 → 289×292 |
| **Map corner** | at the far corner the grid ends exactly on the last cell, `off-map=0`, no garbage tiles, terrain to the viewport edge |
| **Disarm** | `terr.on=off` restores the engine's terrain **and** the fog overlay; zero key pixels left |
| **Watchdog** | `tagpu_overlay.off` stops the beat; after 90 frames `terrown` restores both and the frame is a normal engine draw. The flushes run *before* the overlay's own early return, which is why this works at all |
| **Ownership flips** | six `passive`↔owned transitions, capturing after each: **zero** key pixels on screen every time, black at its 0.03–0.04 % baseline (dark terrain). The hand-back frame is covered by §7.6 |
| **Patch absent** | `terr.on` armed with `terrown.on` missing: `cells=0`, the log says why, and the frame is a normal engine draw — the pass does not paint over the overlays it cannot let through |
| **Key change, live** | `key=200` end to end: the engine's fill becomes palette index 200 and the composite follows it, GL frame unchanged |
| **Stress** | 200v200 at sim +3, terrain + features + units + wrecks + effects all native: **59.7 fps** measured over 20 s off the 60-frame `terr:` cadence — terrain replaces a CPU blit of the whole viewport with 667 quads, so it is not a cost |

<figure><img src="assets/shots/terr-parity.png" alt="terrain parity: engine, ours, difference"><figcaption>Drawing parity. LEFT the engine's own terrain on its 8bpp surface, MIDDLE ours drawn over it in the same frame, RIGHT the difference — <strong>only our native units</strong>. Every terrain-only band of the viewport differs by zero pixels.</figcaption></figure>

<figure><img src="assets/shots/terr-ownership.png" alt="terrain ownership: the key fill and our frame"><figcaption>Ownership and the inverted composite. LEFT the engine's 8bpp surface while we own the draw — 99.9 % one palette index, the key, with only the cursor and the engine's own overlays left in it (99.3 % once a build panel and chat are up). RIGHT our GL frame of the same run.</figcaption></figure>

<figure><img src="assets/shots/terr-fog-ab.png" alt="fog: engine overlay vs ours"><figcaption>Fog, with the engine's overlay suppressed and ours in its place. LEFT the engine drawing its own terrain and its own <code>0x4848E0</code>, RIGHT ours. 99 % of the viewport agrees on lit-vs-grey; the disagreement is the 2–4 px dithered edge band.</figcaption></figure>

### 7.5 One thing G13b had to fix in G13c

`fogMode` was `LosType & 3` and every pass gated the whole fog rule on **bit 0** — which
is only the **MAPPING** option, not "fog is on". Under **true LOS without mapping**
(`LosType=14`: `LineOfSight` cycle stage 1 with the mapping option off, a reachable
skirmish setting) the engine's grid carries a live grey mask while our shaders skipped
the rule entirely. Harmless while the engine still drew its own overlay on top of its
own terrain; **fatal once G13b suppresses that overlay**, because then nothing draws the
grey band at all. The gate is now "the grid uploaded": what the overlay paints is
decided entirely by the grid bytes, and an inactive mode is already an all-zero grid.

### 7.6 Failure modes — three the review found, one that shipped, and the reported one

Review found three ways the key fill could reach the screen; play found a fourth the
day after; and the fifth — the interior crack this whole section was opened for — turned
out not to involve the key at all. **If you are here because something in the world is
the wrong colour, read the fourth and fifth first.** All five are closed, and the shape
of the fixes is worth keeping:

- **Emitting without owning.** Our terrain is opaque and covers the whole viewport, so
  drawing it while the composite is *not* inverting hides every engine overlay. The pass
  therefore emits only when it owns the draw, when `over` asks for it explicitly, or on
  the single hand-back frame below — and when `tagpu_terrown.on` was not armed at DLL
  attach it emits nothing and says so, rather than silently blanking the overlays.
- **The hand-back frame.** Arming is guarded (`filled` gates the inversion until a
  key-filled frame exists) but *dis*arming is the mirror hazard: the engine's frame is
  already key-filled when the skip drops. So the gather reads `filled` **before** it
  touches the skip and emits terrain for that one last frame, in the same call that
  releases it — the composite stops inverting and our own terrain covers the fill.
- **A screen that never calls `0x483FA0`.** The fill bumps a sequence; when it stalls for
  30 presents the composite stops inverting. Otherwise a non-world screen drawn by the
  game thread would be blacked out by a key test it can never satisfy.

Two more rules make the remainder harmless: **the key test is scoped to the viewport
rect** (outside it the engine's frame is UI we never filled, so a UI pixel that happens
to *be* index 254 can never be mistaken for our fill), and **inside it our fragment is
composited over BLACK, not over the engine's frame** — so a hard bail, or an atlas too
large for `GL_MAX_TEXTURE_SIZE`, degrades to black and never to key colour. Verified
across six ownership flips: **zero** key pixels on screen — a test with a blind spot,
which is the next paragraph.

#### A fourth mode, found in play and not by review: a fraction of the key

[LIVE-VERIFIED 2026-09-03] That second rule used to be narrower — "paint black when the
pixel is **entirely** empty" — and every other pixel was emitted as `c` and left to
`glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` over the engine's frame. **That is correct
only when `c.a` is 1.** The FBO is premultiplied and cleared to `(0,0,0,0)`, and the 2×
downsample averages covered samples with uncovered ones along any edge terrain does not
reach, so a part-covered pixel came out as `c.rgb + (1 - c.a) * key`.

The symptom was a **bright cyan hairline the full width (and height) of the world**: the
map's own boundary is a full-length edge of exactly that kind, landing at a screen
position that depends on the zoom. **29 of the 151 levels** between 0.25 and 1.00 (step
0.005) carried one and the rest did not — Two Continents, 1920×1080, `ss=2`. It was the
map edge and not a tile seam, confirmed to the pixel: the line sat at screen
y = `cy + (vpT − eyeY − cy)·z`, which for that rect (`cy` = 540) and eye y = 460 is
`540 − 968·z` — exactly where world y = 0 projects — measured at z = 0.26, 0.265, 0.30
and 0.33. Ten levels at each of five camera positions, plus 151 levels of zoom
1.00→2.50, put **every** leak on the map boundary — never once between tiles, as §7.1
says it cannot be.

**The verification that missed it is the lesson.** "Zero key pixels on screen" counted
pixels *equal* to the key, and a blend never is one: on the seam row at z = 0.300, 1779
pixels carried a cyan cast and **none** of them equalled index 254's colour. So measure
the key's **tint** — `min(g,b) - r` for key 254 — not its exact value, and move the key
to magenta first (§7.3) so that ocean and dithered water cannot answer the same test.

The fix is one clause in `CFS`: inside the fill, `frag = vec4(c.rgb, 1.0)`. Opaque
`c.rgb` **is** the composite against black, so it subsumes the empty case that used to
be special-cased, and the cursor box needed the same treatment for the same reason. Same
sweep afterwards: full-length lines **29 → 0**; the detector's total over 151 frames
69 659 → 8 869 px, all of which resolve to genuine purple terrain art (`(51,19,63)`,
`(99,59,127)`, `(119,75,143)`) with **no exact key pixel** among them.

One review finding was **wrong** and is recorded so it is not "fixed" later: making
`fogMode` bit0 mean "the grid is live" does **not** double-darken when the engine's own
overlay is still running. The overlay remaps pixels in the **8bpp offscreen**; our
fragments are in the GL FBO and are composited over that surface afterwards, so it can
never touch them. Measured with the engine drawing its own terrain and overlay while we
draw features: mean grey-band luminance **62.21 (engine) vs 61.77 (ours)** — a second
remap would roughly halve it.

#### The fifth mode, and the one the interior report was about

[LIVE-VERIFIED 2026-09-03] **A fragment centre landing exactly on a quad's far edge
samples one texel outside the atlas cell.** This is the reported interior crack, and it
is not a key leak at all — which is why 350+ frames of sweeping never found it (below).

**The mechanism.** Quads are emitted on integer game-pixel boundaries, so at some zooms
a quad's far edge falls exactly on a fragment centre. The rasteriser must give that
fragment to exactly one of the two quads sharing the edge, and — measured on this stack,
not assumed — it gives it to the **upper/left** one. Its interpolated `u`/`v` is then
exactly `u1`/`v1`, and `GL_NEAREST` reads `floor(u1·W)`: the first texel of the next
atlas cell. On terrain the next cell down the atlas is tile index **+64**, an unrelated
tile, so the sample is an arbitrary other piece of the map — blue over forest, because
Two Continents' tile set is mostly water. On a GAF sprite it is the shelf packer's
**gutter**, which `glTexImage2D(…, NULL)` had never written: index 0, a real palette
entry (black) rather than the frame's colour key, so it survived the key test and drew
**a black hairline down the right of every tree**.

**When it fires** is pure arithmetic, and it is why the bug came and went. At zoom `z`
with supersample `ss` a tile's screen pitch is `32·z·ss` FBO px and its edges sit at
`z·y0·ss + (1−z)·cy·ss`; the pathology needs that to be a half-integer. At `z = 0.25`,
`ss = 2` it reduces to the **parity of `y0`**, so moving the camera **one world pixel**
turns the whole artefact on or off — measured, 8 phases, at 1024×768 and at 1920×1080:
a period-8 row anomaly of **1.92×** and **2.05×** the baseline at odd `eyeY`, **1.04×**
and **1.12×** at even. At `ss = 1` the same condition is mod 4 and the peak is **2.05×**.
At `z = 0.5` and `z = 1` the edges land on integers and it cannot fire at all — which is
why every earlier check, all of them at or near 1×, was clean.

**Two fixes, because there are two halves.** *Padding* stops the sample leaving the cell:
every terrain cell now sits on a **34-texel pitch with its outermost row and column
repeated on all four sides** (`CELL_PITCH`, atlas 2176×2720 for 5062 tiles), and every
GAF frame gets the same 1-texel border (the packer advances `w+2`/`h+2` and uploads the
bordered block, instead of reserving one gutter and never writing it). That is also what
a **filtered** sampler will need the day this stops being `GL_NEAREST`, which is the
reason the border is on all four sides rather than the two that would close today's bug.

Padding alone is **not** enough under `GL_NEAREST`, and the measurement says so: with the
guard texel in place the fragment reads a *copy of the cell's last row*, which is out of
phase with the row cadence the rest of the 4×-minified tile is sampled on, and TA's tile
art is dithered — so an out-of-phase row still reads as a coloured line. It moved the
seam metric from **1.92× to 1.86×**, i.e. not at all. So the geometry moves too:
`TAGPU_EDGE_NUDGE` (`tagpu_glsl.h`) subtracts **1/32 of a game-screen pixel** in the
terrain vertex shader, *after* the zoom scale so it is the same sub-pixel distance at
every zoom. That puts a coincident fragment centre inside the **following** quad, where
it samples that cell's first texel — the thing it is standing on — and the cadence stays
uniform. 1/32 px is ~300× the float noise in a coordinate that size and 1/8 of a texel at
the 0.25 zoom floor, so it can neither be lost nor change which texel any other fragment
reads. Sprites need no nudge: repeating a silhouette's edge column is correct, because a
sprite's neighbour is not another piece of the same picture.

**Verified.** Two Continents, `feat-forest`. Period-8 row anomaly **1.03–1.13×** at every
camera phase, at `ss=1` and `ss=2`, 1024×768 and 1920×1080 — against 1.92×/2.05× before,
and against **1.71×** in the frame the user captured from the running game. No periodic
anomaly at any of ten zoom levels 0.25→8.0 (the residual outliers are single map-edge and
coastline rows). The black hairline beside every tree is gone; the feature atlas still
packs the map's frames (`atlas=27`) with the wider border. **And the 1× path is
untouched, bit-exactly**: the GL frame at zoom 1.0 is **0 differing pixels** against a
build of the same tree without these three files, and the `over` A/B residual against the
engine's own blit is identical on both (39.7583 %, which is the fog remap, not us).

**Why the sweeps missed it.** The detector was `min(r,b) − g` — the *key's* tint — because
every hypothesis at the time was a key leak. This defect never touches the key: it draws
a neighbouring **tile**, whose colour is ordinary map art. The 350+ archived frames were
very likely carrying it all along. A seam that is periodic in screen space wants a
periodic detector, not a colour one: score each row against its own two neighbours and
group by `y mod (32·z)`; a clean frame is flat across the phases and a leaking one has
one phase standing 2× above the rest.

**What this does not close.** The nudge is applied to the terrain pass only, so features,
effects, particles and markers still rasterise on the un-nudged grid — they are correct
today because their atlases now have borders and a repeated silhouette edge is harmless,
but a future filtered sampler will want the same treatment and the same reasoning. The
three leads that were never tried are still untried and now unmotivated: a tile set that
overflows the atlas (`idx >= s_atlasN` cells are skipped, `junk=0` on Two Continents),
fog actually active during a sweep, and transients during the wheel ease.

### 7.7 What is left inside the viewport

**Since G13d: the mouse cursor, and nothing else** — measured at 112 px of 630 784,
99.98 % key, with the world-space markers (health bars, order markers, group digits,
build cursor, band box, and the engine's own copy of the selection rect) all taken by
`tagpu_markown.c` / `tagpu_mark.c` ([UI markers](ui-markers.html) §6). Chat, dialogs,
the side panel, the minimap and the top bar are screen-space and stay the engine's
forever: they are correct at 1:1 at any zoom, which is exactly what the key exists to
let through.

**G13e turned that measurement into a mechanism, and G13m retired it.** Because the
cursor was the *only* engine pixel left inside the viewport, the composite could MOVE
it: at zoom `z` the engine drew its cursor where it thought the mouse was, which was the
unzoomed position `u` the input path fed it, and the composite painted the texels of a
128×128 box around `u` at the box around the real pointer `s` instead, letting our world
cover the box at `u`.

**That is gone.** The move could never be exact — the surface texture the composite
samples is only replaced when the game flipped, the engine draws its cursor several times
per flip, and any residual mismatch is multiplied by `1/z`, so at 0.25× the sprite was
left behind at `u` on 10–11 % of motion frames. **G13m fixed the input instead**
(`gpu-status.md` §2.3d): `fake_GetCursorPos` answers the TRUE pointer, so the engine
blits its own cursor under it, and the unzoomed `u` is put back at the one place the
world point is computed. The composite's cursor branch, its two uniforms and `CURSOR_PAD`
are deleted; the cursor is still the only engine pixel in the viewport, and it is now in
the right place by construction.

The capture route was ruled out first, and that part still holds: **the cursor is the one
thing in the frame that does not go through `DrawGameScreen`'s OFFSCREEN.** `0x4C2870`
(the engine's show-cursor, called at `0x46A3C7` after everything else in the frame) blits
it with a **NULL context** — `push 0` at `0x4C297C` — and a NULL context makes
`0x4B7F90`/`0x4C6B70` build their own default offscreen over the primary surface, so
swapping a pixel base cannot reach it. That was measured, not assumed: a capture window
bracketing the widget-tree draw at `0x46A303` opens fine and catches **zero** non-key
texels. *[TWO CORRECTIONS, 2026-09-04: this named `0x4C2380` as the second blitter — that
function has no call site and no address literal anywhere in the image, and the live
draw-from-record path is `0x4C67C0` ([exe RE](exe-reverse-engineering.html)). And
`0x4C67C0` does **not** use a NULL context: it passes its own second argument to both
`0x4C6B70` and `0x4B7F90`. The NULL-context argument holds for `0x4C2870` alone, which is
enough for the conclusion but is not the blanket claim this made.]*

One known deviation from suppressing `0x4848E0`: the engine used to shade-remap its
*own* overlays under the grey band, and we no longer do — visible only if a health bar
were ever drawn on out-of-LOS ground, which the engine does not do.

---

## Appendix — address & offset tables

### Functions

| VA | Role | Convention |
|---|---|---|
| `0x483FA0` | **terrain tile blit** (body to `0x4843B3`) | stdcall(OFFSCREEN*), ret 4 |
| `0x418310` | map **debug overlay** (mapDebugMode/`0x511DD0` gated) | stdcall(OFFSCREEN*), ret 4 @`0x418BA8` |
| `0x4C6E70` → `0x4CBEF1` | full 32×32 tile copy (colour-only). `0x4CBEF1` is `(ctx,x,y,gfx)` **cdecl**, dst = `ctx[+0x0C] + ctx[+0x08]·y + x`, 32 rows of 8 dwords — this is where the OFFSCREEN pitch/base offsets are read from [BINARY-VERIFIED G13b] | `0x4C6E70` stdcall ×4, ret 0x10 |
| `0x4C5E70` | fill a default OFFSCREEN (called by both blit wrappers when `ctx == NULL`) | stdcall(ctx*), ret 4 |
| `0x4C6B10` | **set the OFFSCREEN clip rect** — `ecx = ctx`, writes 4 dwords at `ctx+0x1C` [BINARY-VERIFIED G13b] | thiscall ×4, ret 0x10 |
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

### `OFFSCREEN` — the software draw context [BINARY-VERIFIED G13b]

12 dwords on the caller's stack (`DrawGameScreen` builds it at `[esp+0x34]`, so
`0x483FA0` sees it at `[esp+0x5C]` after its own prologue). Only four fields matter
to us, and the terrain key-fill (§7.2) needs all four:

| Off | Field | Read from |
|---|---|---|
| `+0x00` | **width** | `SurfaceCreateNamed 0x4C69F0` writes it, and the default clip from it |
| `+0x04` | **height** | ditto |
| `+0x08` | **pitch** (bytes per row) | `0x4CBEF1`: `dst = base + pitch·y + x` |
| `+0x0C` | **pixel base** | ditto |
| `+0x1C/0x20/0x24/0x28` | **clip rect L/T/R/B, INCLUSIVE** | `0x4C6B10` writes exactly these 4 dwords |

**Corrected G13f:** this note used to say there was no width or height field. There is —
`SurfaceCreateNamed` allocates `w·h + 0x30`, stores `w` at `+0x00`, `h` at `+0x04`,
the pitch at `+0x08`, the pixel base at `+0x0C`, and initialises the clip to
`(0, 0, w-1, h-1)`. `tagpu_vpwide.c` reads `+0x00`/`+0x04` to clamp the clip rect back
inside the surface, which is the whole point of that redirect. What remains true is that
`tagpu_terrown.c` validates the **clip rect** before filling: the context it is handed is
a stack COPY of the descriptor, so the clip is the bound that actually governs the blit,
and refusing an implausible one is cheaper than trusting the viewport fields alone.

### Prologue bytes of the functions we detour [BINARY-VERIFIED G13b]

| VA | Bytes | Steal | Resume |
|---|---|---|---|
| `0x483FA0` terrain | `83 EC 48 · 8B 0D E8 1D 51 00` | **9** (`sub esp,0x48` is only 3, so the first boundary at or past 5 is 9) | `0x483FA9` |
| `0x4848E0` fog overlay | `83 EC 2C · 53 · 55` | **5** exactly | `0x4848E5` |
| `0x46A610` feature leaf | `8B 4C 24 08 · 53` | 5 | `0x46A615` |

### The fog overlay's lazy grid rebuild — what a skip path must replicate

```
4848e6  mov  esi,[0x511DE8]
4848ec  mov  ebx,8
4848f2  test byte [esi+0x14281],bl    ; LosType bit3 = "grid is current"
4848f8  jne  0x484911                 ; already current -> straight to the cells
4848fa  call 0x4843c0                 ; rebuild the screen fog grid
4848ff  mov  eax,[0x511DE8]
484904  or   word [eax+0x14281],bx    ; LosType |= 8
```

Five lines, and they are **the only engine state the overlay writes**. Every native
pass samples the grid this rebuilds, so suppressing `0x4848E0` without replicating
them freezes the grid and silently takes G13c's fog parity with it (§7.2).

### GUI colours — `main+0xDCB` [BINARY-VERIFIED G13b, live read]

The 16-entry table `DrawBar` and the rest of the GUI index by colour number. Read
live on Two Continents:

```
00 04 02 06 D5 05 CB 55 5A 09 E9 20 D3 FD C2 FF
```

Entry 0 is palette index 0 — the fog's own solid black, which is why **index 0 is not
a free key**. The table uses `0xFD` and `0xFF` and **leaves `0xFE` (254) unused**,
which is where G13b's composite key lives (§7.3). In `palettes/palette.pal` that index
is `(0,255,255)` — the cyan of the Windows system tail at `249..255` — so a key leak is
always blue-green, and always identifiable by moving the key (§7.3).

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
