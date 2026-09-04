# Build-state (nanoframe) drawing — the scaffold, the fill cutoff, and `mode`

*Instruction-level map of how TA draws units under construction, produced so the GL
renderer can reproduce the look: the animated blue scaffold, the bottom-up fill, the
wireframe, and the `mode` argument of the two rasterisers. All VAs are for the
pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`,
verified this session). Disassembly via `objdump -d -M intel` on the PE
(`.text` VA→file = `VA − 0x400C00`, `.rdata` = `VA − 0x4FC000 + 0xFAE00`,
`.data` = `VA − 0x501000 + 0xFF600`); decompiles via the shared Ghidra project
(headless, on a scratch copy).*

Evidence tags as in `own-the-draw.md`: **[BINARY-VERIFIED]** = bytes read this
session; **[CORPUS]** = name/layout from TADR / the other notes; **[INFERRED]**.

---

## Summary — the one thing to know

**The nanoframe look is NOT produced by the "nanoframe rasteriser" `0x459C70`.**
Both rasterisers (`0x459830` opaque, `0x459C70` lit) bake the *fully textured* model
into the composite — colour plane + a **depth plane whose byte per pixel is the
pixel's model-space HEIGHT** (`worldY + 0x32`, or `+0x7D` for airborne units). The
under-construction appearance is applied **at blit time, every frame**, by
**`0x458DD0`** (thiscall `(this, GAFFrame* frame, Object3doStruct*)`, ret 8), which:

1. reads **`UnitStruct+0x104` (`float Nanoframe`, 1.0 → 0.0 = fraction of build
   REMAINING)**, scales it to `p = (int)(Nanoframe * 255.0f)`;
2. runs a **5-stage height-threshold recolour** (`0x458D30`) over the composite
   using the depth plane: pixels become *erased*, *solid blue*, *band blue*, or
   *keep texture* depending on `depth` vs a threshold that sweeps with `p`;
3. draws a **wireframe** (`0x458FA0`): every face of every visible piece as a
   closed polygon outline, 2 edge pixels per scanline, depth-tested against the
   depth plane, in the second animated blue.

The two animated colours are **palette indices `0xA0..0xAF`**, animated as
triangle waves of the 30 Hz game tick, salted by the unit's slot index. This page
used to call that ramp *blue*; it is **green** — read live off the engine's own
palette at `main+0x143A7` (2026-09-03): `0xA0` = `D7 FF A7`, then `AB E7 7F`,
`83 D3 5B`, `67 BF 3F` … down to a near-black green at `0xAF`. The names `b1`/`b2`
survive below as the two oscillator phases, not as a colour claim. The fill
cutoff is a **height threshold in depth-plane units** (whole-model space, not
per-piece). `mode` (`[esp+0x10]` of both rasterisers) selects **which piece-cache
class to draw** — `1` = cached pieces (the bake), `0` = `dont-cache` pieces
(per-frame redraw, animated textures), `-1` = all pieces — and is bypassed
entirely while `Nanoframe != 0` (nanoframes always bake all visible pieces).

---

## 1. Where build-state hooks into the draw chain

Established chain (own-the-draw): `DrawUnit 0x45AC20 → dispatch 0x458810 →
builder 0x4586A0 → rasterise (0x459830 | 0x459C70)` then `blit 0x459200`.

### Builder `0x4586A0` — allocator + rasteriser selection [BINARY-VERIFIED]

```c
// thiscall(this, Object3doStruct* obj, int flag, int mode), ret 0xC
unit = obj->ThisUnit;                       // obj+0x0C
<AABB 0x4581E0 → W/H/HotX/HotY>
if (flag == 0 && !(unit+0x114 & 1) && unit->Nanoframe == 0.0f)
    0x437B50(...);                          // colour-only composite
else
    0x437BE0(...);                          // colour + DEPTH plane (w*h*2+0x18)
if (composite) {
    <hotspot writes>
    if ((unit+0x110 & 0x20000000)           // 0x45873C: the STRUCTURE bit
        && (TAdynmem+0x37F06 & 0x20))       // 0x45874A: GameOptionMask bit5
        0x459C70(composite, obj, unit->cOwnerID, mode);   // "lit" rasteriser
    else
        0x459830(composite, obj, unit->cOwnerID, mode);   // plain rasteriser
}
```

- **A composite gets a depth plane iff the unit is a nanoframe (`Nanoframe != 0`),
  or `unit+0x114 & 1`, or the builder was called with `flag != 0`** (the cargo
  path). The depth plane is what makes the blit take the build-state path.
- **`0x459C70` is selected only for `0x20000000`-flagged units when GameOptionMask
  (`TAdynmem+0x37F06`) bit `0x20` is set** — it is the *same* rasteriser as
  `0x459830` plus per-vertex Gouraud lighting (§4). With bit `0x20` clear those
  units bake through plain `0x459830`. The scaffold appears either way.
- **`0x20000000` IS THE STRUCTURE BIT, NOT "UNDER CONSTRUCTION"** — measured live
  2026-09-03 by reading `unit+0x110` for three units in one game: a **complete
  mobile** ARMCOM `0x91600371` (clear), a **complete building** ARMSOLAR
  `0x30282321` (set), and an **ARMLAB under construction** `0x30E42321` (set).
  So `0x459C70` is the *structure* rasteriser — a nanoframe reaches it only when
  the thing being built is a building — and every "nanoframe bit" reading of
  `0x20000000` on this page and in the code that quoted it was wrong. The state
  that means under construction is `Nanoframe != 0` at `+0x104`, nothing else.
  ([shadows & cloak](shadows-cloak.html) already read this bit as the structure
  bit in the blit's shadow tree; the two pages disagreed and this settles it.)

Builder call sites (all four in the binary) [BINARY-VERIFIED]:

| Call VA | Caller | Args `(obj, flag, mode)` |
|---|---|---|
| `0x45890C` | dispatch `0x458810` (rebuild) | `(obj, 0, 1)` |
| `0x45936E` | blit `0x459200` (no-depth path, composite null) | `(obj, 0, 1)` |
| `0x4595FC` | blit (depth path, composite null) | `(obj, 0, 1)` |
| `0x459670` | blit cargo loop | `(cargo_obj, 1, -1)` |

### Dispatch `0x458810` — extra rebuild triggers for nanoframes [BINARY-VERIFIED]

On top of the `Object3do+0x04 == 0` rule (own-the-draw §3), the rebuild flag is
also set when `unit+0x110 & 0x20000000` and either the composite is null, **or**
(`Nanoframe != 0.0` && `unit+0x110 & 0x2000` && composite has **no depth plane**
(`frame+0x14 == 0`)) — i.e. the first frame after construction starts, the old
colour-only composite is thrown away and rebuilt with a depth plane. Bit `0x2000`
is set by the build-tick (§3) on every nanolathe pulse and by the completion
routine.

### Blit `0x459200` — the per-frame build-state work [BINARY-VERIFIED]

The blit branches on `*(composite+0x14)` (depth-plane pointer):

**Depth plane present** (nanoframes, factories with cargo, `0x114&1` units):
```
0x459608  call 0x4589C0            ; size shared scratch (this+0x10) to unit+cargo AABB,
                                   ;   COPY composite colour+depth planes into scratch,
                                   ;   then tail-call 0x458DD0(scratch, obj)  <-- SCAFFOLD
0x459610  test [unit+0x110],0x20000000
0x45961A  je   rerasterise         ; not a nanoframe -> re-rasterise
0x45961C  fld  [unit+0x104] ; fcomp 0.0 ; je 0x459646   ; active nanoframe -> skip
rerasterise:
0x459641  call 0x459830(scratch, obj, ownerID, mode=0)  ; redraw dont-cache pieces
                                   ;   with CURRENT anim texture frames, z-tested
; cargo loop (unit+0x8A chain, next at +0x8E, skip if 0x110&0x20000):
0x459670  call 0x4586A0(cargo_obj, 1, -1)   ; rebuild cargo composite EVERY frame
0x459686  call 0x458DD0(cargo_composite, cargo_obj)     ; scaffold on the cargo
          call 0x4B90A0(cargo_comp, scratch, dx, dy, bias)  ; z-merge (depth <=)
; then: underwater erase 0x4BA1B0 / tint 0x4B96E0, aircraft ground clip
;   0x4BA1B0(scratch,0x7D) if UnitDef+0x241 bit30, cloak alpha vs CopyGafToContext
```

So for a **building constructed in the open**: its own composite is baked once
(all pieces, textured), and each frame the blit copies it to scratch and runs
`0x458DD0` on the copy — the composite itself is never scribbled on. For a
**factory building a unit**: the *cargo* unit's composite is rebuilt + scaffolded
+ z-merged every frame; the factory itself gets the mode-0 re-rasterise for its
moving (dont-cache) pieces.

**No depth plane** (normal units): composite blitted straight
(`CopyGafToContext`, or `AlphaCompsteBuf2OFFScreen` when cloaked `unit+0x10E&4`),
then visible `dont-cache` pieces (`prim+0x28` bit0 set, bit1 clear) are drawn
individually via `0x4584D0` on top — no depth games.

---

## 2. `UnitStruct+0x104 float Nanoframe` — the build-progress field

TADR names it (`tamem_ghidra.h` line 1106): `float Nanoframe; //0x104`. Confirmed
from disassembly [BINARY-VERIFIED]:

- **Creation** `0x485B27` (in the unit-spawn path near `UNITS_CreateModelScripts`):
  spawned-under-construction → `Nanoframe = 1.0f`, `Health(+0x108) = 0`;
  spawned complete → `Nanoframe = 0`, `Health = UnitDef->maxdamage(+0x1FA)`.
- **Build tick** `0x41BA60` (callers pass work amount as float arg):
  `Nanoframe -= work / (float)UnitDef->buildtime(+0x1EA)`, clamped to `[0,1]`;
  health incremented proportionally; sets `unit+0x110 |= 0x2000` each pulse
  (stores at `0x41BC16` / `0x41BC96`).
- **Completion** `0x41B92A..0x41B93D`: `Nanoframe = 0.0`,
  `Object3do->composite(+0x10) = 0` (forces a colour-only rebake),
  `unit+0x110 |= 0x2000`.
- **Consumption**: everywhere in the draw path it is compared against
  `0.0f` (`ds:0x4FD4C0`) — `!= 0` means "under construction". `0x458DD0` scales it:
  `p = ftol(Nanoframe * 255.0f)` (`fmul ds:0x4FD4C4` = 255.0, `0x458E92..0x458E9E`).

**Type/range: IEEE float, 1.0 (just placed) → 0.0 (complete) = fraction of build
REMAINING.** Fraction built = `1 − Nanoframe`.

State bits involved (`unit+0x110`): `0x20000000` = **structure** (see the
correction in §1; tested by builder/dispatch/blit/rasterisers, and true of a
building whether it is finished or a nanoframe); `0x2000` = "lathed/progress
changed" pulse. (The tamem comment claiming "0x20: nanoframe" is off; so was this
page's own earlier claim that `0x485AFE..0x485B03` sets `0x20000000` at spawn —
disassembled 2026-09-03, that site computes `(UnitDef+0x241 & 0x200) << 0x15` and
stores it, which is bit **`0x40000000`**, and it is the only thing it writes to
`+0x110`.)

---

## 3. `0x458DD0` — the scaffold driver (colour staging) [BINARY-VERIFIED]

`thiscall (this, GAFFrame* frame, Object3doStruct* obj)`, ret 8. Returns 0 and does
nothing if `frame->PtrDepth(+0x14) == 0` or `unit->Nanoframe == 0.0`.

### The two animated blues

```
tick   = TAdynmem+0x38A47                  (30 Hz game tick counter)
idx    = unit->UnitInGameIndex (+0xA8)     (per-unit salt)
phase1 = (idx ^ 5) + tick*0x21/0x1E        ; +1.1 steps/tick
phase2 = (idx ^ 9) + tick*0x39/0x1E        ; +1.9 steps/tick
tri(ph) = (ph & 0x10) ? 0xAF - (ph & 0xF) : 0xA0 + (ph & 0xF)
blue1  = tri(phase1)   ; blue2 = tri(phase2)      ; palette indices 0xA0..0xAF
```

Two independent triangle waves bouncing across the palette's 16-entry blue ramp
`0xA0..0xAF` — this is the flicker/shimmer. (Sample the live palette at
`*(0x511DE8)+0x143A7` for actual RGB; the indices are the contract.)

### The 5 stages (`p = ftol(Nanoframe*255)`, so p runs 255 → 0 during the build)

Each stage calls `0x458D30(frame, t, cAbove, cBelow, cBand)` with a threshold `t`
(byte) and three "colours" where `-1` = keep original pixel, `-2` = erase (write
the ColorKey byte `frame+8`), else = palette index to write:

| Stage (p) | built % | t formula (byte) | sweep during build | cAbove (`d≥t`) | cBand (`t-4≤d<t`) | cBelow (`d<t-4`) |
|---|---|---|---|---|---|---|
| A `236..255` | 0–7.5% | `(p-235)*255/20` | 255→13, top→bottom | erase | **blue1** | erase |
| B `201..235` | 8–21% | `(p-200)*255/35` | 255→7, top→bottom | erase | **blue1** | erase |
| C `116..200` | 22–55% | `byte((115-p)*255/85 − 1)` | 0→252, bottom→top | erase | **blue2** | **blue1** (solid) |
| D `31..115` | 55–88% | `byte((30-p)*255/85 − 1)` | 0→252, bottom→top | **blue1** (solid) | **blue2** | keep (texture!) |
| E `0..30` | 88–100% | `p*255/30` | 255→0, top→bottom | keep | **blue1** | keep |

Read as a story: two thin blue scanlines sweep down over nothing (A, B); a solid
blue silhouette rises from the ground with a brighter leading band (C); the real
textured pixels are revealed bottom-up beneath the rising band while blue remains
above (D); a final scanline sweeps down over the finished texture (E).

### `0x458D30` — the recolour kernel [BINARY-VERIFIED]

```c
// (GAFFrame* f, byte t, int cAbove, int cBelow, int cBand), thiscall shell, ret 0x10
lo = (t < 4) ? 0 : t - 4;
for (i = 0; i < W*H; i++) {
    if (colour[i] == (byte)f->ColorKey) continue;      // transparent stays
    d = depth[i];                                      // = height byte, see §5
    c = (d < lo) ? cBelow : (d < t) ? cBand : cAbove;
    if      (c == -2) colour[i] = (byte)f->ColorKey;   // erase
    else if (c != -1) colour[i] = (byte)c;             // else keep
}
```

**Depth plane untouched** — so later passes (wireframe, underwater/aircraft clips,
cargo z-merge) still see the full model's heights even where colour was erased.
The cutoff is a plain per-pixel height threshold over the *whole model* — there is
no per-piece cutoff.

### `0x458FA0` — the wireframe pass [BINARY-VERIFIED]

Called by `0x458DD0` right after the recolour, as
`0x458FA0(frame, obj, colour=blue2)` — the wireframe uses the **second** blue.
It re-projects every visible piece (same projection as the rasterisers, §5, using
`frame`'s HotX/HotY, no 2× mode), then for **every face** (selection primitive
skipped, any vertex count, no cache/shade filter) builds the screen-space polygon
*closed* (first vertex appended) and calls `0x4C0820(frame, verts, count+1,
colourByte)`:

- `0x4C0820` edge-walks the polygon (screen-space left/right x + interpolated
  depth per scanline) and calls `0x4C0A90` per scanline.
- `0x4C0A90` draws **only the two edge pixels** of the scanline: for each, if the
  depth plane exists, `if (plane[px] <= edgeDepth) { colour[px] = wireColour;
  plane[px] = edgeDepth; }` — depth-tested *and depth-writing* (ties pass, so
  edges over their own faces draw; edges behind nearer geometry are hidden-line
  removed — even where the fill was erased, because the depth plane keeps the
  full model).

Two pixels per scanline means near-horizontal edges render sparse — that's the
authentic TA wireframe texture. The wireframe is drawn at **every** stage (the
whole build, A through E), over whatever the recolour left.

---

## 4. The `mode` argument and the piece-cache system

Both rasterisers: thiscall, 4 stack args `[esp+4]=composite GAFFrame*`,
`[esp+8]=Object3doStruct*`, `[esp+0xC]=unit+0xFF cOwnerID byte`,
`[esp+0x10]=mode`, ret 0x10 (own-the-draw §4). Complete call-site census
[BINARY-VERIFIED via full-disasm grep]:

| Rasteriser | Call VA | From | mode |
|---|---|---|---|
| `0x459C70` | `0x458765` | builder (nanoframe+option branch) | pass-through: `1` or `-1` |
| `0x459830` | `0x45878B` | builder (default branch) | pass-through: `1` or `-1` |
| `0x459830` | `0x459641` | blit depth path (non-nanoframe) | `0` |

### Per-piece draw test (identical in both rasterisers) [BINARY-VERIFIED]

```c
// piece = obj + 0x22 + i*0x36 (resident PrimitiveStruct), flags byte at +0x28
if (!(flags & 1)) skip;                          // bit0 = visible
if (mode != -1 && (mode != ((flags >> 1) & 1))   // bit1 = "cached in composite"
    && unit->Nanoframe == 0.0f) skip;            // nanoframe overrides the filter
```

So: **mode = which cache class to draw.** `1` = cached pieces (the composite
bake), `0` = `dont-cache` pieces (redrawn each frame), `-1` = everything (cargo
rebuild). While `Nanoframe != 0` the filter is bypassed → the nanoframe bake
always contains **all visible pieces**.

`mode` also selects the texture frame (§5): `mode==0` → current animated GAF frame
(`GAFGetCurrentFramePtrAddr`), `mode!=0` → sequence frame 0 — i.e. the cached bake
is animation-frame-0, the per-frame pass uses live animation.

### Piece flag byte `prim+0x28` [BINARY-VERIFIED]

| Bit | Meaning | Set/cleared by |
|---|---|---|
| 0x01 | visible | init `0x45AF21` (set iff face's piece has ≥3 verts); COB show/hide (invalidates `Object3do+0x04` if piece cached — `0x480D9A`) |
| 0x02 | **cached** (part of the baked composite) | default ON at init `0x45AED4`; COB `cache`/`dont-cache` handler **`0x480DB0`** (also nulls `Object3do+0x10` → forced rebake) |
| 0x04 | **shaded** (Gouraud in `0x459C70`) | default ON `0x45AF31`; COB `shade`/`dont-shade` handler **`0x480DF0`** (also forces rebake) |

DrawUnit's pose-sync (`0x45ADBA`) invalidates the composite (`Object3do+0x04 = 0`)
when a piece whose **cache bit is set** has turned ≥8 angle units — moving cached
pieces force a rebake; `dont-cache` pieces (spinning radars etc.) don't, they're
just redrawn via mode-0 / `0x4584D0`.

### The nanoframe-vs-opaque selection (the exact test)

There is **no per-unit "draw nanoframe" flag consumed by the rasterisers** for the
scaffold. The chain is:

1. `unit+0x110 & 0x20000000` && `GameOptionMask(0x37F06) & 0x20` → builder picks
   `0x459C70` (adds Gouraud), else `0x459830`. Purely cosmetic option.
2. `unit->Nanoframe != 0.0f` (+0x104) → depth-plane composite, all pieces baked.
3. Blit: depth plane present → `0x4589C0` copy → **`0x458DD0`** applies the
   scaffold iff `Nanoframe != 0.0f`. This is the test that makes a unit *look*
   under construction.

---

## 5. What the bake writes (both rasterisers) [BINARY-VERIFIED]

### Projection & the depth plane's meaning

Per posed vertex (16.16 fixed, from `prim+0x22` posed buffer):

```
sx = (vx >> 16) + HotX
sy = ((-vz) >> 16) - ((vy >> 16) >> 1) + HotY      ; TA's y − y/2 oblique view
d  = (vy >> 16) + 0x32                              ; +0x4B more if UnitDef+0x241 bit30
```

`d` is interpolated across faces and stored in the depth plane on every pixel win
(test: `plane <= d`, larger = higher/nearer wins). **The depth plane is a height
map**: byte = model-space height + 50 (ground units) or +125 (bit-30/airborne
units). Cleared to 0 at alloc. This is what the scaffold threshold, the underwater
erase/tint (`0x4BA1B0`/`0x4B96E0`, level = `sealevel(0x1427F) − unitY + bias`),
the aircraft ground clip (`0x4BA1B0(frame, 0x7D)`), and the cargo z-merge
(`0x4B90A0`, `dstDepth <= srcDepth + bias`) all consume.

In 2× supersample mode (below), `sx`,`sy` double but `d` does **not**.

### Face records (resident, stride 0x20; base `PrimitiveInfo+0x28`, face[0] skipped
when `PrimitiveInfo+0x0C` (selection primitive) `!= -1`)

| Offset | Meaning |
|---|---|
| +0x00 | palette colour (used when bit0 of flags set) |
| +0x04 | vertex count |
| +0x0C | vertex-index array ptr |
| +0x10 | resident GAF frame ptr (current) |
| +0x18 | GAF sequence ptr (for by-name/frame-0 lookups) |
| +0x1C | flags: bit0 = colour-fill n-gon (no texture); bit1 = textured; bit2 = team-colour texture |

Face dispatch (identical shape in both rasterisers, `0x45A30B..0x45A3BF` in
`0x459C70`):

- `flags & 1` → **colour-fill n-gon**: `0x4C0C70(composite, verts, count,
  colour=face+0)` (lit variant; `0x4C1000` in the opaque rasteriser).
- else quad only (`count == 4`, others skipped): texture frame =
  `bit1 clear` → `face+0x10` as-is; `bit2 set` → `GAF_SequenceIndex2Frame(face+0x18,
  PlayerStruct[owner].colour)` (player struct at `TAdynmem+0x1B8A`, stride
  `0x14B`, colour byte +0x96); else `mode==0` → `GAFGetCurrentFramePtrAddr(face+0x10)`,
  `mode!=0` → frame 0. Then `0x4C8BB0(composite, texFrame, verts, uvs=0)` (lit;
  `0x4C8760` opaque). `uvs=0` → full-frame UVs `(0,0)-(W-1,H-1)`.

### `0x459C70`'s only difference: Gouraud lighting [BINARY-VERIFIED]

The transformed-vertex record grows from 3 to 4 ints `(sx, sy, d, intensity)`.
Per piece it computes face normals (`0x4B6F00` edge-sub ×2, `0x4B6F70` cross,
`0x4B6FF0` normalize; degenerate faces get `(0,1,0)`), averages them into vertex
normals, then per face-vertex:

```
intensity = (piece flags bit2)                       ; the SHADE bit
          ? ftol(dot(N, L) * 5.0) & 0x1F             ; L = ds:0x5065F8 = (-0.8, 1.0, 0.25)
          : 0xF                                       ; default mid-shade
```

(`0x45A2B6..0x45A2EC`; K=5.0 at `0x4FD4CC`; light vector settable via
`0x4597F0(x,y,z)` ints ×0.01.) The lit scanline core `0x4C8020` writes
`shadeTable[intensity*256 + texel]` with `shadeTable = *(TAProgramStruct+0xC4)`
(32 rows × 256), depth-tested `plane <= d`, depth written. The opaque core
`0x4C7A20` writes raw texels. So `0x459C70` = same geometry/textures, shaded.

### The 2× supersample option [BINARY-VERIFIED]

Both rasterisers, on entry: if `GameOptionMask & 0x02` && `unit+0x110 & 0x20000000`
&& `mode != 0`, they render into the **shared scratch** (`this+0x10`) at 2× W/H
(vertices doubled, depth values NOT doubled), then downsample back into the real
composite: colour via `0x4B95A0` — a 2×2 box filter through the palette-blend LUT
`*(TAProgramStruct+0xC0)` (256×256 "average of two palette indices") — and depth
by point-sampling every 2nd pixel of every 2nd row (`0x45A427..0x45A460`).
Anti-aliased nanoframe bakes, option-gated.

---

## 6. Struct-offset table (build-state relevant)

| Field | Offset | Meaning |
|---|---|---|
| `UnitStruct.Nanoframe` | +0x104 | float, build fraction REMAINING 1.0→0.0; 0 = complete |
| `UnitStruct.Health` | +0x108 | s16, grows with build ticks |
| `UnitStruct.UnitSelected` (state mask) | +0x110 | bit `0x20000000` = **structure** (measured, §1 — not "under construction"); bit `0x2000` = lathe pulse/progress-changed; bit `0x200` = sonar-LOS (underwater tint choice); bit `0x40000000` = `UnitDef+0x241 & 0x200`, written at spawn (`0x485AFE`) |
| `UnitStruct` unknown | +0x114 | bit0 forces depth-plane composite (non-nanoframe depth users) |
| `UnitStruct.UnitInGameIndex` | +0xA8 | salt for the blue-phase oscillators |
| `UnitStruct.cOwnerID` | +0xFF | flagByte arg → team-colour texture row |
| `UnitStruct` cargo chain | +0x8A / +0x8E | first cargo unit / next-in-chain (z-merged, scaffolded) |
| `UnitDefStruct.buildtime` | +0x1EA | u32 divisor of the per-tick decrement |
| `UnitDefStruct.maxdamage` | +0x1FA | health target |
| `UnitDefStruct` flags | +0x241 | bit30 = airborne: depth bias +0x4B and ground clip at 0x7D |
| `Object3doStruct` piece array | +0x22, stride 0x36 | resident `PrimitiveStruct` |
| `PrimitiveStruct` posed verts | +0x22 | 16.16 ×3 per vertex |
| `PrimitiveStruct` flags | +0x28 | bit0 visible, bit1 cache, bit2 shade |
| composite `GAFFrame` | `Object3do+0x10` | W@0,H@2,HotX@4,HotY@6,ColorKey@8,Compressed@9, PtrColour@0x10, **PtrDepth@0x14 (height map!)** |
| `TAdynmemStruct.GameOptionMask` | +0x37F06 | bit1 `0x02` = 2× supersample bakes; bit2 `0x04` = shadows; bit5 `0x20` = lit (Gouraud) nanoframe bake |
| `TAdynmemStruct` tick | +0x38A47 | 30 Hz counter driving the blue oscillators |
| `TAdynmemStruct` sea level | +0x1427F | byte, underwater erase/tint threshold |
| `TAProgramStruct` LUTs | +0xC0 blend(256×256), +0xC4 shade(32×256), +0xD0 water tint(256) | |

---

## 7. Address / convention appendix

| VA | Role | Convention |
|---|---|---|
| `0x4586A0` | builder: alloc + rasteriser select (`0x110&0x20000000 && opt&0x20` → lit) | thiscall, 3 args, ret 0xC |
| `0x459830` | rasteriser, plain (bakes colour+depth) | thiscall, 4 args, ret 0x10 |
| `0x459C70` | rasteriser, + Gouraud (`shadeTbl[i*256+texel]`) | thiscall, 4 args, ret 0x10 |
| `0x4589C0` | blit helper: scratch sizing + plane copy, tail-calls 0x458DD0 | thiscall(this, frame, obj) |
| **`0x458DD0`** | **scaffold driver: blues, stages, wireframe** | thiscall(this, frame, obj), ret 8 |
| `0x458D30` | height-threshold recolour kernel | (frame, t, cAbove, cBelow, cBand) |
| `0x458FA0` | wireframe: all faces as closed outlines | (frame, obj, colourByte) |
| `0x4C0820` → `0x4C0A90` | polygon edge walk → 2 edge px/scanline, depth test+write | — |
| `0x4C8BB0` → `0x4C8020` | lit quad walker → scanline (shade LUT, depth test+write) | — |
| `0x4C8760` → `0x4C7A20` | opaque quad walker → scanline | — |
| `0x4C0C70` / `0x4C1000` | colour-fill n-gon, lit / opaque | — |
| `0x4B90A0` | z-merge blit (cargo → scratch, `dst<=src+bias`) | (src, dst, dx, dy, bias) |
| `0x4BA1B0` | erase where `depth <= level` (underwater hide, aircraft clip 0x7D) | (frame, level) |
| `0x4B96E0` | tint via `*(TAProgram+0xD0)` where `depth <= level` | (frame, level) |
| `0x4B95A0` | 2×→1× colour downsample via blend LUT `+0xC0` | (scratch2x, dst) |
| `0x41BA60` | build tick: `Nanoframe -= work/buildtime`, sets `0x110|=0x2000` | stores `0x41BC16/0x41BC96` |
| `0x41B92A` | completion: `Nanoframe=0`, composite invalidated | — |
| `0x485B27` | spawn under construction: `Nanoframe=1.0`, Health=0 | — |
| `0x480DB0` / `0x480DF0` | COB cache / shade piece-flag setters (bit1 / bit2, force rebake) | (pieceIdx, bool) |
| `0x4597F0` | set light vector `0x5065F8` (init `(-0.8, 1.0, 0.25)`) | (x,y,z ints ×0.01) |
| `0x4FD4C0` / `0x4FD4C4` / `0x4FD4CC` | consts 0.0f / 255.0f / 5.0f | .rdata |

---

## 8. What our GL renderer must do

The scaffold runs at **blit time on a copy** — it never dirties the composite — so
under the own-the-draw model (we fill the composite planes, TA blits) there are two
workable designs:

**Option A — let TA keep the scaffold (recommended first step).** Keep `0x458DD0`
unsuppressed and make the GL writeback fill **both planes** of a nanoframe unit's
composite: colour = full textured render (all visible pieces, texture frame 0),
**depth = per-pixel height byte `clamp(modelY + 0x32, 1, 255)`** (`+0x7D` base for
UnitDef+0x241-bit30 units). TA will then copy, stage-recolour, wireframe,
water-clip and z-merge exactly as retail. If the depth plane is left zero the
scaffold degrades badly (every pixel classifies as "below threshold": stages C/D
render solid-blue/plain-texture with no rise, and the water/aircraft clips
misfire) — filling depth is not optional. Detect the case with
`unit+0x110 & 0x20000000 && *(float*)(unit+0x104) != 0`.

**Option B — own the scaffold in GL** (needed if we also own the blit). Reproduce:

1. `p = (int)(Nanoframe * 255)`; pick stage A–E and threshold `t` from §3's table.
2. Fragment classification by model-space height `h` (use `d = h + 50` to match):
   `d ≥ t` → cAbove, `t-4 ≤ d < t` → cBand, else cBelow; colours `-2` = discard,
   `-1` = textured render, else palette index.
3. Blues per frame: `tri(ph) = (ph&0x10) ? 0xAF-(ph&0xF) : 0xA0+(ph&0xF)`;
   `blue1 = tri((idx^5) + tick*33/30)`, `blue2 = tri((idx^9) + tick*57/30)`;
   sample the live palette for RGB.
4. Wireframe: every face of every visible piece (selection primitive excluded) as
   a closed line loop in `blue2`, depth-tested (`GL_LEQUAL`) against the model's
   own depth — a normal depth-tested line pass over the full (uncut) model depth
   buffer gives retail hidden-line behaviour. Drawn at all stages.
5. Draw all visible pieces regardless of cache class, texture animation frame 0;
   Gouraud (`dot(N, normalize(-0.8, 1.0, 0.25)) * 5 / 32` as a shade factor) only
   if imitating GameOptionMask bit `0x20`.

Either way, nothing about the scaffold needs sim writes; it is pure presentation
derived from `unit+0x104`, `unit+0x110`, the tick counter, and the unit slot.

---

## 7. What we built — Option B, live (2026-09-03)

**Option B is the one that shipped**, because by then a nanoframe was the last
world thing still coming from the engine's own frame, and everything the engine
draws inside the viewport lands at the **unzoomed** projection: the composite
scales OUR fragments and passes its frame through 1:1. A building or a factory's
cargo therefore slid across the map the moment the zoom left 1, away from the
commander or factory building it. Reported from play, reproduced on a scripted
`nanoframe` fixture and on a live commander build.

- **Ownership.** `tagpu_native_owns_unit()` no longer excludes `Nanoframe > 0`
  (`tagpu_native.c`); the native pass draws the unit like any other, so the
  scaffold goes through the zoom transform with the world.
- **The formulas live once.** `tagpu_r3d_nano_state(unit, &t, c, &wire)` in
  `tagpu_render3do.c` is §3's stage table plus the two oscillators; the composite
  path (`tagpu_render3do`) and the native pass both call it. `tagpu_nano.off`
  still disables the staging: the native pass reads it once per arm poll, the
  composite path only after `nano_state` has said the unit is a nanoframe at
  all. With the lever set, a nanoframe goes back to the engine entirely —
  `tagpu_native_owns_unit()` declines it — so the lever stays a real A/B
  instead of showing a finished-looking unit with the engine's copy suppressed.
- **The recolour** is three per-unit uniforms in the native fragment shader
  (`uNanoOn`, `uNanoT`, `uNanoC`), classifying by `vVY + 50` exactly as §1.2.
  An **erased fragment discards**, and that is a deliberate divergence — see
  the gap below.
- **The wireframe** is a second line range per unit (`emit_wire`), every face of
  every visible piece, in the second oscillator's colour, biased 0.15 depth-key
  units nearer than the skin it traces (1.8 + 0.15 = 1.95 against the 2.0
  half-gap between row keys — inside it, with 0.05 to spare). It is not
  decoration: at the top of a build the recolour erases the whole model and the
  skeleton is the only thing on screen.
- **Where the rest of this lives.** A factory's unit-in-progress is *carried*, and being carried
  is a separate axis from being a nanoframe: the attach/detach function, the guards it enforces,
  why a released unit appears to walk under the plant, and the A/B proving that is stock, are all
  on [factories](factory-build.html).
- **GAP — the wireframe's back edges show through the unbuilt part, and the
  engine's do not.** The engine hides them by testing each outline pixel
  against the composite's own height plane, which keeps the whole model's
  heights even where the colour was erased (§1). That plane is **per sprite**;
  ours is the one shared GL depth buffer. An erased fragment that wrote depth
  would hide the back edges correctly *and* become an invisible occluder for
  everything drawn after it — and since `nano_stage` erases all but a thin band
  at `p >= 201`, that occluder is nearly the whole model for the first fifth of
  every build. It took a factory's own far wall against the unit on its pad
  (the cargo is given the parent's `encBase`, so the two sort by `md` alone),
  and the nanolathe spray, later-indexed units and hires bodies with it. So the
  erased fragment discards and the extra edges are accepted. Closing this
  properly needs per-sprite isolation — a stencil pass around each nanoframe —
  which this landing did not attempt. **Not yet confirmed by eye in a running
  game.**
- **The engine's own copy had to be stopped.** Wiping the composite does not do
  it — `0x458DD0` runs on a scratch COPY every frame, so with the rasterise
  skipped it recoloured nothing and stamped its **wireframe alone**, at the 1×
  position. `tagpu_owndraw.c` now carries a third detour on `0x458DD0` (6 stolen
  bytes, `xor eax,eax; ret 8` on the skip path — the engine's own "did nothing"
  return), taken for exactly the units the native pass owns.
- **A unit in a factory is part of the FACTORY's sprite, not a sprite beside
  it.** The blit's cargo loop (`0x459646..0x4596DD`) rebuilds the cargo
  composite, scaffolds it and **z-merges** it into the factory's own scratch
  through `0x4B90A0`, per pixel, by the two height planes offset by the position
  delta (`cargo+0x6A/0x6E/0x72 − parent's`). Sorted as an independent sprite it
  lands on its OWN tile row instead: measured on an ARM lab at world y 1072
  building a Hammer at 1068 — one 16-unit row apart, four whole depth keys, and
  the lab covered it at every pixel it filled ("the unit is being built UNDER
  the lab", reported from play against the first cut of this gate). The gather
  now walks `unit+0x8A` / `+0x8E` and gives every chain member the parent's row
  and band, mirroring the engine's own skip on `state & 0x20000`, so the two
  models sort against each other by `md`, our intra-model view depth. **That is
  an approximation of the merge, not a port of it.** `0x4B90A0` compares
  `dstDepth` against `srcDepth + HIWORD(dy)`: its depth plane is a *height*,
  biased by the world height delta between the two origins, and it samples at
  the projected offset. `md = (2y − z)/256` is model-local and carries neither.
  They agree while parent and cargo are at the same height — every factory pad —
  and diverge for a cargo whose origin sits above or below its parent, which
  this landing did not cover.
- **A unit under construction casts no shadow — nearly to the end.** Ours had to
  drop it or the erased body showed our slant projection through as a black
  silhouette. Measured against the stock renderer on ONE solar at one spot, only
  the build state varying, taking the pixels the completed unit darkens by half
  as the lobe and its own 25 % frame as the bare-terrain reference:

  | built | `p` | lobe / terrain | reads as |
  |---|---|---|---|
  | 25 % | 191 | 1.00 | no shadow |
  | 75 % | 63 | 0.82 | no shadow (the body itself covers part of the lobe) |
  | 85 % | 38 | 0.84 | no shadow |
  | 89 % | 28 | 0.87 | no shadow |
  | 95 % | 12 | 0.70 | *something* |
  | 99 % | 2 | 0.71 | *something* |
  | 100 % | 0 | 0.48 | the full shadow |

  **We draw none of it while `Nanoframe != 0`**, which matches every row up to
  89 % and is conservative for the last two — a missing shadow rather than a
  wrong black one.

  **Two things this does NOT establish.** The mechanism: the blit's shadow tree
  has no nanoframe test and does blit `Object3do+0x14` for a structure
  ([shadows & cloak](shadows-cloak.html)), so what that branch has to blit is
  evidently empty for most of a build, and why is unknown. And the fixture is
  not a real build: `scenario`'s `nanoframe` creates the unit COMPLETE and then
  writes `+0x104`, so its composite and its cached shadow have a history a
  lathed unit's does not — which is the likeliest reason the 95/99 % rows differ
  from 89 % at all, since the recolour classifies this model identically at
  `p` 28 and 12. A real build watched through those last percentages is the
  measurement that would settle it.

**Verified** at 1024×768 against an unarmed control instance on the same
scenario: the 5/25/50/75/95/100 % ladder reproduces the engine's own progression
— wireframe, solid fill, texture — and its pulse; a commander-built solar tracks
the zoom at 0.6/1.0/1.8 where before it stood still.

**Not closed.** A replacement (glTF) mesh under construction draws unstaged: the
hires pass has no build-state uniforms, though the wireframe still comes off the
3DO tree. The `mode`-selected `dont-cache` re-rasterise (§1) has no equivalent in
our pass. And the engine's reason for dropping the shadow is unknown.
