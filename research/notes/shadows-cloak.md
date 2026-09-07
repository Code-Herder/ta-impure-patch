# Unit shadows & cloaked/translucent drawing — the shade-table pipeline

*Static RE of how TA draws unit shadows, cloaked units, underwater tint and the
shade tables behind them, answering the G-questions handed off from the live G4
trace ("site B never fired", "no separate shadow pass seen"). All VAs are for the
pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`).
Disassembly via `objdump -d -M intel` (PE-direct, `.text` VA→file = `VA −
0x400C00`), decompiles via the shared Ghidra project (headless, on a scratchpad
copy).*

Evidence tags: **[BINARY-VERIFIED]** = instruction/bytes read for this build this
session (VA quoted). **[CORPUS]** = name/layout from the symbol corpus / TADR,
cross-checked. **[INFERRED]** = my reading, not yet runtime-confirmed.

---

## Summary — the five answers

1. **`AlphaCompsteBuf2OFFScreen 0x4B8500`** is a stdcall 4-arg colour-keyed blit
   (`ctx, GAFFrame*, x, y`, `ret 0x10`) that writes every non-key pixel as
   **`dst = ALP[dst + src·0x100]`** where **ALP = a 64 KB 256×256 50 %-blend
   table at `*(TAProgramStruct+0xC0)`**, generated from the live palette at
   startup (`UIPipelinesInit → 0x42E140 → 0x4BA750`) and disk-cached as
   **`palettes\PALETTE.ALP`**.
2. **`0x4CBF2C`** is the inner uncompressed loop of that blit (cdecl, 6 args,
   last arg = the table). Only callers: `0x4B8682` (inside `0x4B8500`) and
   `0x4B8499` (a 5-arg screen-locking sibling).
3. **DrawUnit call site B `0x469BA3` is the AIRBORNE-unit sweep**, not a shadow
   pass: DrawGameScreen runs ground units+features per row (site A), then
   weapons `0x49BE60` + explosions `0x420B00`, then site B over **all** rows for
   units with `(unit+0x110 & 3) != 1` (in-flight = 2). It never fired live
   because no aircraft was on screen.
4. **Units DO get shadows** — drawn *inside* `0x459200` (the per-unit blit),
   right before the body: the unit's own composite silhouette **blackened to
   palette index 0** (`0x45A470` + `0x4B96A0`) and alpha-blitted through the ALP
   table at **(sx+0x85, groundY)** — 5 px right of the body, at the ground
   height under the unit. Gated by option word `TA+0x37F06` bit2 ("Shadow") and,
   for completed units, bit3 ("TShadow"), minus `noshadow`/`canhover`/`floater`
   unit types. Structures — state bit `0x20000000`, nanoframe or completed —
   instead get a **true slant-projected, RLE-compressed shadow cached at
   `Object3do+0x14`** (`0x45A790`). *Since G13k that branch is redirected under
   `owndraw all` and the native pass draws the slant projection itself — see
   "Structure shadows, owned" below.*
5. **Cloak changes only the final blit**: `unit+0x10E & 4` (actively cloaked) →
   the body goes through `0x4B8500` (50 % blend with the background) instead of
   the plain `CopyGafToContext`; enemies never see it at all
   (`UnitInPlayerLOS 0x465AC0` returns false on that bit). The rasteriser's
   `[esp+0xC]` arg is **not** a cloak flag — it is `unit+0xFF` (owner id) used
   to pick **team-colour GAF frame variants**. Cloak never touches
   rasterisation or the composite.

---

## 1. `AlphaCompsteBuf2OFFScreen 0x4B8500` — convention, gate, table

### Calling convention [BINARY-VERIFIED]
```
4b8500  sub esp,0x94 ... 4b86ce  ret 0x10
args: [esp+4]=OFFSCREEN* ctx (NULL ⇒ lock live screen via GetContext 0x4C5E70,
      unlock 0x4C5FA0), [esp+8]=GAFFrame*, [esp+0xC]=x, [esp+0x10]=y
```
Identical shape to `CopyGafToContext 0x4B7F90` (stdcall, 4 args, `ret 0x10`).
Sprite-local behaviour: hotspot subtraction (`x−HotX, y−HotY`), clip against the
ctx clip rect (`0x4C6AE0` + `0x4B7E60`), `SubFrames (+0x0A) != 0` ⇒ recurse over
the subframe pointer array at `+0x10`.

### The global gate [BINARY-VERIFIED `0x4B850A..0x4B851C`]
```
4b850a  call 0x4b6220            ; GetTAProgramStruct = *(ds:0x51FBD0)
4b850f  mov  cl,[eax+0xf0]
4b8519  test cl,0x20             ; capability bit5 = "ALP table exists"
4b851c  je   0x4b86c4            ; clear ⇒ DRAW NOTHING (silent no-op)
```
`GetTAProgramStruct 0x4B6220` returns `*(0x51FBD0)` (TADR `gSchedulerBlock`) — a
heap block created by `0x4B5980` (pointer stored at `0x4B5990`). `+0xF0` is a
16-bit capability word; bit5 also gates the table's generation, so if the table
was built, the blit works. The same block hosts the whole **table set**:

| Field | Table | Size | File cache | Generator | Consumer |
|---|---|---|---|---|---|
| `+0xC0` | **ALP** 50 % blend | 0x10000 (256 src × 256 dst) | `palettes\PALETTE.ALP` | `0x4BA750` | `0x4B8500`/`0x4CBF2C`/`0x4CC057`, `DrawAlpha 0x4BED70`, `0x4CC8DF` |
| `+0xC4` | **SHD** shade | 0x2000 (32 levels × 256) | `palettes\PALETTE.SHD` | `0x4BADF0` | face stampers inside `GAF_DrawTransformed` (`0x4C81B7`, `0x4C824D`, …: `out = SHD[(level<<8)+texel]`) |
| `+0xC8` | **LHT** lighten | 0x2000 (32 × 256) | `palettes\PALETTE.LHT` | `0x4BABD0` | `DrawLight 0x4BEC70` |
| `+0xCC` | grey remap | 0x100 | — (rebuilt) | `0x4BAD30` (avg RGB/3) | grey blit sibling `0x4B86E0` (caller `0x484A98`) [INFERRED: disabled/paralysed rendering] |
| `+0xD0` | water tint | 0x100 | — (rebuilt) | `0x4BAF30` (**r/2, g/2, b/2+0x32** clamped) | `0x4B96E0` (tint submerged unit pixels) |

Gates in `+0xF0`: bit5 ALP, bit6 SHD, bit7 LHT, bit8 grey, bit9 water.
[BINARY-VERIFIED in each generator/installer]

### Table generation & format [BINARY-VERIFIED `0x4BA750`]
Called at game init: `UIPipelinesInit 0x491200` →
`0x42E140/0x42E1D0/0x42E260/0x42E2F0/0x42E300(TA+0x143A7 /*live palette*/)`.
`0x42E140` first tries the HPI file `palettes\PALETTE` + ext `ALP`
(strings `0x5031BC "palettes"`, `0x5033A4 "PALETTE"`, `0x5033A0 "ALP"`); a hit is
memcpy'd into `prog+0xC0` (`0x4BAAD0`, 0x4000 dwords); a miss generates and
**writes the cache file** (`0x4BC290(name, table, 0x10000)`).

Generator formula (`0x4BA750`):
```c
for (src = 0..255) for (dst = 0..255)
  ALP[src*256 + dst] = (src == dst) ? src
      : nearest_palette_index( (pal[src].rgb + pal[dst].rgb) / 2 );   // 50/50 mix
```
`nearest_palette_index` = `0x4BA9D0` over a luminance-sorted index built by
`0x4BA920`. **Row = source pixel (the sprite), column = current dest pixel.**

### The inner loops
`0x4CBF2C` — uncompressed path, cdecl 6 args
`(dstCtx, srcDesc{W,H,stride,pixels}, srcRect, dstRect, colorKey, table)`
[BINARY-VERIFIED]:
```c
if (*src != colorKey) *dst = table[(uint)*dst + (uint)*src * 0x100];
```
(The old composite-buffer note's "`+ bias`" was wrong — the 6th arg IS the table
base; there is no extra bias.)
`0x4CC057` — same thing for RLE-compressed frames (`+0x09 Compressed != 0`),
used by the nanoframe shadow (§3).

### What routes a unit through `0x4B8500` instead of `0x4B7F90`
All in `FUN_00459200` (per-unit composite→offscreen blit; thiscall, 6 args,
`ret 0x18`). The **body** blit selector [BINARY-VERIFIED `0x45937D` (path A) /
`0x459779` (path B)]:
```
45937d  test BYTE [ecx+0x10e],0x4      ; unit+0x10E bit2 = actively cloaked
459384  jne  →AlphaCompsteBuf2OFFScreen(ctx, comp, sx+0x80, bodyY)
45938b  mov  cl,[eax+0x14280]          ; TA+0x14280 = mapDebugMode (0..4)
459391  test cl,cl ; jne →Alpha...     ; any debug view forces alpha for ALL units
        ; else CopyGafToContext(ctx, comp, sx+0x80, bodyY)
```
The **shadow** blits (§3) always use `0x4B8500`. Other callers of `0x4B8500`:
feature draw `0x46A610` (`0x46A7AF/0x46A807/0x46A840` — feature shadow/reclaim
subframes, gated by `TA+0x37F06` bit4 "FShadow" [BINARY-VERIFIED `0x46A6ED`]),
weapons `0x49C0F5..` (smoke/alpha sprites), GUI (`0x4736B1..`), and
`CopyGafToContext` itself — its subframe recursion sends any subframe with
**`+0x0B AlphaBlend != 0`** to the alpha blit [BINARY-VERIFIED] (this is how
GAF-baked feature shadows work: shadow subframe carries the AlphaBlend byte).

---

## 2. The options word `TA+0x37F06` and the unit-type mask bits

### `TA+0x37F06` (word, TADR "GameOptionMask") — console/option toggle map
Resolved from the command table at `.data 0x501D44` (records
`{char* name, void* toggleFunc, 1}`) [BINARY-VERIFIED — cross-checked: the
"FShadow" bit (4) is exactly the bit the feature draw tests]:

| Bit | Mask | Name | Toggle fn | Used at |
|---|---|---|---|---|
| 1 | 0x0002 | **AntiAlias** | `0x416510` | `0x459830` 2× supersampled nanoframe render |
| 2 | 0x0004 | **Shadow** | `0x416550` | `0x459295` — master gate of the whole unit-shadow section |
| 3 | 0x0008 | **TShadow** | `0x416630` | `0x459324` — required for the completed-unit (translucent) shadow |
| 4 | 0x0010 | **FShadow** | `0x416660` | `0x46A6ED` — feature shadows |
| 5 | 0x0020 | **Shading** | `0x416420` | `0x45874A` — builder picks the shaded/nanoframe rasteriser `0x459C70` |
| 6 | 0x0040 | Dither | `0x416590` | terrain |
| 8 | 0x0100 | SwitchAlt | `0x4165C0` | — |

### `UnitDefStruct+0x241` (`UnitTypeMask_0`) — FBI boolean bit map
From the FBI parser run at `0x42C6xx–0x42C8xx` (`GetBool(name)` then
`shl eax,N ; or`) [BINARY-VERIFIED]:

| Bit | Mask | FBI field | Draw-path role |
|---|---|---|---|
| 11 | 0x00000800 | `canfly` | (sim: airborne checks with `state&3==2`) |
| 12 | 0x00001000 | `canhover` | **excluded from the standard shadow** (`mask & 0x81000`) |
| 14 | 0x00004000 | `hidedamage` | — |
| 15 | 0x00008000 | `shootme` | — |
| 17 | 0x00020000 | `armoredstate`? [INFERRED name] | — |
| 18 | 0x00040000 | `activatewhenbuilt` | — |
| 19 | 0x00080000 | `floater` | **excluded from the standard shadow** |
| 20 | 0x00100000 | `upright` | — |
| 21 | 0x00200000 | `amphibious` | — |
| 24 | 0x01000000 | `isfeature` | — |
| 25 | 0x02000000 | **`noshadow`** | **no shadow, ever** (`0x4592AC`) |
| 26 | 0x04000000 | `immunetoparalyzer` | — |
| 27 | 0x08000000 | `hoverattack` | — |
| 29 | 0x20000000 | `antiweapons` | — |
| 30 | 0x40000000 | **`digger`** | depth bias +0x4B and below-ground clip (§3) |

---

## 3. The shadow pipeline inside `0x459200` — where shadow pixels come from

`0x459200(this, OFFSCREEN* ctx, Object3do* obj, eyeX<<16, _, eyeY<<16)` computes
[BINARY-VERIFIED `0x459228..0x45927A`]:
```
sx      = (unit.X − eyeX)>>16                  ; body drawn at sx+0x80
bodyY   = ((unit.Z − eyeY)>>16) − (altitude>>1)        + 0x20
shadowY = ((unit.Z − eyeY)>>16) − (GetPosHeight(pos)>>1) + 0x20   ; GROUND height (0x485070)
```
→ for a grounded unit `shadowY == bodyY`; for an aircraft the shadow stays on
the ground. Shadow x = **`sx+0x85`** (`0x45930B`, `0x459349`) — 5 px right of the
body's `sx+0x80`.

Two paths split on the composite's depth-plane pointer (`frame+0x14`,
`0x45927E`): **path A** (colour-only composite — stationary buildings) at
`0x459288`, **path B** (colour+depth) at `0x45949D`.

### Shadow decision tree (both paths, drawn BEFORE the body) [BINARY-VERIFIED]
```
if (TA+0x37F06 & 4)                        ; option "Shadow"
 └ if !(unitdef.mask & 0x2000000)          ; not FBI noshadow
    ├ COMPLETED (not nanoframe, or digger):
    │   if (TA+0x37F06 & 8)                ; option "TShadow"
    │    └ if !(mask & 0x81000)            ; not canhover / floater
    │        0x45A470(this, composite)     ; scratch := composite, colours→0x00
    │        [path B only] waterline/ground clip on scratch:
    │           digger:      0x4BA1B0(scratch, 0x7D)          ; clip below ground
    │           else if seaLevel>alt: 0x4BA1B0(scratch, (seaLevel−alt)+0x32)
    │        0x4B8500(ctx, scratch,  sx+0x85, shadowY)        ; 50% black blend
    └ STRUCTURE (state & 0x20000000 — buildings, nanoframe or complete; not digger):
        skip if (unit+0xA6 model index == 0 && altitude<seaLevel)
        if (obj+0x14 == 0)  0x45A790(this, obj, composite)    ; build cached shadow
        0x4B8500(ctx, obj+0x14frame, sx+0x85, shadowY)        ; RLE path 0x4CC057
        (blitted as built on BOTH paths: no waterline erase, no digger erase)
```
(`seaLevel` = byte `TA+0x1427F`; `state` = `unit+0x110`; the structure test is
`byte[unit+0x113] & 0x20` in path A at `0x4592BF` and `test dword [ecx+0x110],
0x20000000` in path B at `0x459522`. Site addresses of every branch:
`exe-reverse-engineering.md` §"The unit blit's shadow branches". The word this
tree used to call `UnitID` is `unit+0xA6`, the u16 model index the native pass
reads as `U_MODELID` — corrected 2026-09-03 off the disassembly at `0x4592D5`.)
The `je` that enters the STRUCTURE branch — `0x4592C6` (path A) and `0x45952C`
(path B) — is what `owndraw all` flips to a `jmp` (G13k), so every structure then
takes the COMPLETED branch and its blank composite blits nothing. Two path
asymmetries the tree above flattens: path A tests the structure bit *before*
`digger` and sends a digger structure to COMPLETED (with the TShadow and
`canhover`/`floater` tests); path B tests `digger` *first* (`0x4594D0`) and
takes an inline branch (`0x4594D8..0x45951D`: silhouette, ground clip
`0x7D`, straight to the shared blit) that applies neither test.

### `0x45A470` + `0x4B96A0` — the completed-unit shadow builder
`0x45A470` copies the unit's composite (header W/H/Hot/ColorKey + colour plane +
depth plane if present) into the **shared scratch composite** (`this+0x10`), then
`0x4B96A0` [BINARY-VERIFIED `0x4B96C1..0x4B96CD`]:
```
4b96c1  mov cl,[eax]        ; colour pixel
4b96c6  cmp cl,bl ; je skip ; == ColorKey → keep
4b96ca  mov BYTE [eax],0x0  ; else → palette index 0 (black)
```
So the shadow sprite = **the body silhouette filled with index 0**; blitting it
through the ALP table darkens the background 50 % toward black in exactly the
unit's shape. It is NOT a re-projection — a completed unit's shadow is a cheap
offset copy of its own sprite.

### `0x45A790` — the structure shadow (nanoframe or complete), cached at `Object3do+0x14`
A **true slant-projected** shadow [BINARY-VERIFIED]:
1. `0x45A510` — ground-projection AABB over the posed prims:
   `gx = x + y/4`, `gy = −z − y/4` (height leans the shadow up-right).
2. Fill scratch colour = ColorKey, depth = 0.
3. `0x45A610` — rasterise the silhouette with the same slant
   (per-vertex `(x + y/4, −z − y/4)`, faces flat-filled via `0x4C1000`; only
   prims whose flag byte (`PrimitiveStruct+0x28`) has **both bit0 and bit1**
   set — `test cl,1 ; je` at `0x45A64C`, `test cl,2 ; je` at `0x45A655`. The
   AABB pass `0x45A510` tests bit0 alone (`0x45A55B`). What bit1 *means* is
   not settled: it is not simply "build-completed" — a completed CORE wind
   generator's mast and rotor pieces lack it and cast no shadow in the engine
   (measured 2026-09-03, `shadow-struct` fixture, engine terrain), while its
   base pieces carry it. [INFERRED: a per-piece "casts shadow" bit set by the
   model or the script, not by construction.])
4. `0x4B9D70(bodyComposite, scratch, 5, 0)` — punch the body's own pixels back
   out of the shadow (writes ColorKey where the body overlaps, offset +5 px)
   so the ground shadow never darkens under the sprite itself. [INFERRED intent]
5. `0x4B9E60` — RLE-compress the silhouette (per row: `u16 len` + RLE via
   `0x4BA000`), `0x437B50` allocates `obj+0x14`, header copied,
   **`+0x09 Compressed = 1`** — so the blit takes the `0x4CC057` RLE-alpha path.

**What the raster takes, settled 2026-09-07 (G14j):** every face of every piece with bits 0
and 1, whatever its material — the body rasteriser's colour-fill / quad-only dispatch is not
consulted, so the footprint quad it skips is in the shadow and a texture key punches no hole
— face 0 skipped when the node's selection primitive is not −1, each vertex snapped to whole
units by the 16.16 high word of x, of y and of −z before the quarter is taken by an arithmetic
shift. **Bit1 is the piece's `cached` flag**, cleared by COB `dont-cache` (`0x480DB0`): a
wind generator's `cradle` and `fan`, an ARM extractor's `arms`. The cached sprite is rebuilt
with every composite rebake (`0x458905` nulls `+0x14` before the builder runs) and blitted as
built on both paths — **no waterline or digger erase for a structure**. The punch-out (step 4)
is aligned with the body on screen. Addresses and the reading:
[exe-reverse-engineering](exe-reverse-engineering.html) §"The slant builders".


### Depth bias & waterline clipping (path B bodies AND shadows)
The rasteriser writes per-pixel depth = **`vertexY + 0x32 (+0x4B if digger)`**
[BINARY-VERIFIED, `0x459830` vertex loop — this corrects composite-buffer.md's
"+0x4B if not cloaked"]. Consumers on the scratch copy (`0x4589C0` copies
composite+cargo into the scratch first, so the cache is never damaged):
- `0x4BA1B0(frame, t)` — **erase**: colour := ColorKey where `depth <= t`.
- `0x4B96E0(frame, t)` — **tint**: colour := waterTable[colour] (`prog+0xD0`:
  r/2, g/2, b/2+0x32) where `depth <= t` and colour != key.

Body rules [BINARY-VERIFIED decompile `0x459200` path B tail]:
```
sub = seaLevel − altitude
if (sub > 0):
    t = sub + 0x32 (+0x4B if digger)
    enemy-owned (unit+0xFF != TA+0x2A43) and no sonar flag (!(state & 0x200)) → erase(t)
    else (own unit, or sonar-detected)                                        → tint(t)
if (digger): erase(0x7D)          ; clip model parts below ground level
```
Shadows: digger → `erase(0x7D)`; else if submerged → `erase(sub+0x32)` (above).

**GAF shading during rasterisation** (unrelated to shadows but same table
family): the textured face stampers map every texel through
`SHD[(faceLightLevel<<8) + texel]` (`0x4C81B7` etc.) — per-face lighting.

---

## 4. DrawUnit call site B `0x469BA3` — the airborne sweep

`DrawGameScreen 0x468CF0(drawWorld, _)`. Both sites call
`DrawUnit(offscreen 0x45AC20)` with identical args `(offscreenCtx, unit)`;
both require `unit+0x9A != 0` (the model-attached pointer, set at `0x485D9F`)
and draw the selection rect first if `state & 0x10`. [BINARY-VERIFIED]

### Site A `0x469A00` — ground layer, per row
```
4699c3  mov eax,[esi+0x110]
4699cb  and ecx,3 ; cmp cl,1 ; jne skip      ; ONLY (state&3)==1  (on-ground)
4699d3  mov ecx,[esp+0x228] ; test ; je skip ; DrawGameScreen arg1 "draw units"
```
Interleaved with the feature sweep per bucket row (back-to-front painter's
order over visible rows only).

### Between the sweeps (only when arg1 != 0)
`0x49BE60` = weapons draw (laser DrawLines, projectile GAFs);
`0x420B00` = explosions/effects draw. [BINARY-VERIFIED decompile skim]

### Site B `0x469BA3` — everything not in the ground layer, over ALL rows
```
469b6d  mov eax,[esi+0x110]
469b75  and edx,3 ; cmp dl,1 ; je skip       ; ONLY (state&3) != 1
469b93  mov eax,[esi+0x9a] ; test ; je skip
469ba3  call 0x45ac20                        ; same DrawUnit, same args
```
Row loop runs `0 .. TA+0x1424F` (the whole sort grid), i.e. no depth sorting —
these draw **on top of ground units, features, projectiles and explosions**.
`(state&3) == 2` is the in-flight state: `0x401C48` checks
`(state&3)==2 && (mask & 0x800 /*canfly*/)` [BINARY-VERIFIED];
so site B = **aircraft (plus any state-0/3 stragglers)**. It never fired in the
live trace because only the grounded commander was on screen. **There is no
shadow DrawUnit pass** — shadows live inside `0x459200` within the one call.
(N.B. an aircraft's ground shadow is therefore drawn during this late sweep,
i.e. composited above projectiles — engine quirk, reproduce or knowingly fix.)

---

## 4b. Aircraft, measured

[MEASURED 2026-09-04, this project. Fixture `scenarios/shadow-air.json` on Two Continents,
engine pixels only — no passes armed — with `glshot`. Offsets by cross-correlating each unit's
body mask against its shadow mask inside one frame, which is immune to the roster lagging the
capture; altitude from `roster --json`, ground from the feature-map height byte read with
`tacli peek`.]

Everything §3 predicts for an aircraft holds, and the numbers are worth having because §2.2 of
[renderers](renderers.html) had to choose a Classic++ behaviour against them.

- **It is the silhouette, not a projection.** The shadow is the plane's own sprite shape.
- **Offset `+5 px` in x and `(altitude − ground) / 2` px straight DOWN the screen.** The
  confident matches (mask overlap > 300 px): ARMBRAWL alt 192 / ground 84 → measured
  `(+5, 54)` against 54.0 predicted; ARMPEEP 312 / 84 → `(+5, 114)` against 114.0; ARMFIG
  240 / 84 → `(+6, 78)` against 78.0; ARMATLAS 181 / 84 → `(−1, 47)` against 48.5. So the
  shadow sits on the ground under the plane and **separates downward as altitude grows** — it
  does not lean up-right the way a 40° parallel light would put it. Register-level derivation
  of the two Y values: [exe-reverse-engineering](exe-reverse-engineering.html)
  §"The unit blit's shadow branches", `0x459228..0x45927A`.
- **One 50 % blend.** Luminance ratio against the bare ground: median **0.487**, and the
  histogram is a single sharp mode at 0.44–0.52 with a shoulder to 0.60 — the shoulder is the
  ALP table snapping to the nearest palette entry, not a second blend. Per channel the medians
  are R 0.55, G 0.48, B 0.21: the halving happens in **palette space** and then snaps, so it is
  not a clean per-channel halving in RGB.
- **It darkens units and features, not just terrain.** Watching the four Peewees' own pixels
  across a 40-frame burst, one dropped to **0.532** of its baseline on the two frames a
  fighter's shadow crossed it, and the tree behind it darkened with it. Panel:
  the shadow lies across both.
- **Over water it lands on the SEABED.** The height byte there is below sea level (51–68 on the
  fixture's northern lanes against sea level 75), so the shadow is drawn *further* below the
  plane than on land — a lower receiver is a lower screen row — as an unclipped plane-shaped
  patch on the sea. The completed-unit waterline clip never fires for an aircraft because it is
  gated on `seaLevel > altitude`.
- **Aircraft LAND unless they are given an order**, which is a fixture fact that cost an hour:
  a plane spawned with `height` set simply sinks to the ground and sits there at
  `(state&3) != 2`. Every lane in the fixture is a `patrol` for that reason.
- **CruiseAlt is the still-frame model.** The FBI's own value — ARMBRAWL 60, ARMATLAS 90,
  ARMFIG 110, ARMPEEP 180, ARMTHUND 200 — plus the ground under the unit lands within the
  band the live altitudes bob through (a Freedom Fighter reads 198–240 over ground 67–84).

**Not closed.** The map edge was not tested. And no still was ever caught with a shadow lying
across a fireball: the layering in §4 rests on the call order in `DrawGameScreen`, re-verified
by disassembly 2026-09-04 (features `0x46A610`, site A `0x469A00`, weapons `0x469B22`,
explosions `0x469B2C`, then site B `0x469BA3`) and on `0x4B8500` having no depth test — not on
a photograph. Four staged attempts to line the two up (bombers on a high-HP building, a 20v20
firefight, gunships hovering over it, damaged buildings that turned out not to smoke) all
failed to coincide.

## 5. Cloak — end-to-end

- **`unit+0x10E`** (tamem `cIsCloaked`, ushort): bit0 = cloak enabled/ordered
  (sim: energy drain, many readers); **bit2 = actively cloaked**; bit4 = sim
  state tested at `0x408B08`/`0x40B97B` [not draw-related].
- **Visibility**: `UnitInPlayerLOS 0x465AC0(player, unit)` returns **false for
  any unit with bit2 set unless `player` owns it** [BINARY-VERIFIED] — enemy
  cloaked units simply never enter another player's draw/LOS set. The same
  function also hides underwater units (`altitude < seaLevel`) lacking the
  sonar flag `state & 0x200`.
- **Rendering (the owner's / an ally-with-vision's screen)**: rasterisation and
  the composite are 100 % normal; only the final blit changes —
  `0x459200` routes the body through `AlphaCompsteBuf2OFFScreen` (50 % ALP
  blend with whatever is already on the offscreen). Binary, not graded.
- **`TA+0x14280` (mapDebugMode, 0–4)**: any non-zero value forces the alpha
  path for every unit (debug view). Zeroed at `UIPipelinesInit`.
- **Correction to the handoff**: the rasterisers' `[esp+0xC]` arg
  (`unit+0xFF`, pushed by the builder `0x45875F/0x458785` and blit `0x459633`)
  is the **owner player id**, consumed as
  `GAF_SequenceIndex2Frame(faceSeq, playerColour)` with
  `playerColour = *(byte*)(*(TA + owner*0x14B + 0x1B8A) + 0x96)` for faces with
  the team-colour flag (bit2 of face flags) [BINARY-VERIFIED `0x459830`] — i.e.
  **per-player-colour texture variants**, nothing to do with cloak.

---

## What our GL renderer must do

1. **Shadows exist — reproduce them.** For every drawn unit whose def lacks
   `noshadow` (mask bit25), when the Shadow option is on:
   - *Completed units* (+ TShadow option, not `canhover`/`floater`): draw the
     unit's silhouette (its own sprite alpha-mask) in **black at 50 % opacity**,
     at `(sx+5, groundY)` where `groundY` uses the terrain height under the
     unit (`bodyY` uses the unit's altitude) — aircraft automatically get a
     ground shadow, groundlings a 5-px-right drop shadow.
   - *Under construction* (nanoframe, not digger): TA builds a slant-projected
     silhouette (`x += y/4`, `screenY -= y/4` in model space, completed pieces
     only) minus the body overlap. A GL renderer can either replicate the slant
     projection or keep the cheap silhouette — the engine itself uses the cheap
     one for all finished units.
   - *Clipping*: erase shadow/body pixels below the waterline for units above
     water (`seaLevel − altitude` in elevation units, bias `0x32`); `digger`
     units clip everything below ground (depth ≤ `0x7D`).
   **ONE BLEND PER SILHOUETTE PIXEL, not one per surface the view ray crosses**
   (implemented 2026-09-04, G13n). The engine blackens a *copy of the composite* and blits it
   ONCE, so a pixel the model covers twice is still darkened once. A GL pass that re-uses the
   body's 3-D geometry with depth writes off does not get that for free: every surface blends
   again. Aircraft are where it became unmissable — from above a plane is a two-sided shell
   over its whole area, so ours came out at **0.25** of the ground where the engine is
   **0.49**, and the histogram was bimodal at 0.25/0.50 (two layers / one) with a 0.125 tail
   for three. Per airframe before the fix: ARMFIG 0.252, ARMPEEP 0.254, ARMATLAS 0.253,
   ARMBRAWL 0.253. Ground units had it too, milder. Both renderers now mark the silhouette
   into a **stencil** with colour writes off and then blend where the mark is with the op that
   ZEROES it, so the second fragment on a pixel fails `EQUAL 1`; both draws see the same depth
   buffer, so they cover the same fragments and leave no stale mark, and the mark is cleared
   per unit so two DIFFERENT units' shadows still stack the way the engine's two separate blits
   do. After: the peak moved to 0.44–0.52 (77 % of shadow pixels, against the engine's 55 %).
   The four Peewees on the fixture are **bit-identical** before and after — their silhouette
   was already one layer — and the Commander changed in 175 px.

   **Ownership split, measured 2026-09-02 (native pass armed, composite wiped):**
   the engine's *completed-unit* shadow is built from the composite, so for a
   natively-owned mobile unit it comes out empty — that shadow is ours to draw.
   The `0x20000000`-path shadow (structures: the cached slant projection at
   `Object3do+0x14`, built from the posed prims) and the `FShadow` feature
   shadow of a 3D wreck survive the wipe and keep drawing — the native pass
   drew **no** shadow for those, and honours `noshadow`/`canhover`/`floater`
   (`UnitDefStruct+0x241`) exactly as the engine does (an ARM Skimmer had been
   getting a shadow from us). Rule in `tagpu_native.c` at the time: `shadow =
   !(state & 0x20000000) && !(mask & 0x02000000) && !(mask & 0x81000)`, never
   for wrecks. Panel: `assets/shots/g12c-shadow-ownership.png`.
   **Structure shadows, owned 2026-09-03 (G13k).** That split stopped holding
   the moment the terrain became ours: the cached shadow is drawn by the ALP
   blend blit `0x4B8500`, and inside the key-filled viewport its destination
   is palette 254's cyan, so every building's shadow came out as an **opaque
   teal silhouette** (`(0,128,128)`, cyan halved) over our terrain — and, being
   in the engine's frame, at the **1× position whatever the zoom**, so zoomed
   out it sat detached beside the body (reported from play on Two Continents:
   *"metal extractors casting strange shadows"*). Now: `owndraw all` flips the
   two `je`s that enter the STRUCTURE branch (`0x4592C6`, `0x45952C`) to `jmp`,
   so a building takes the COMPLETED branch, whose composite is blank under
   `all` and blits nothing; the native pass emits the slant projection itself
   from the live posed prims — `(x + y/4, −z − y/4)`, pieces with flags
   bit0|bit1 only, 5 px right, 50 % black, gated by the Shadow option bit alone
   (the engine's cached branch never tests TShadow), `noshadow`, and the
   model-0-under-water skip; `canhover`/`floater` are NOT tested on that branch
   and are not tested by us for it either. A `digger` structure is not slanted
   — the engine never gives one the cached shadow — and takes the silhouette
   rule. Rule now: structures `shadow = structshadow_ours && !noshadow`, the
   rest as before. Measured over the
   engine's own terrain (`terr.on=passive`) against the pre-fix engine shadow,
   same frame position, four buildings: the only differences are the rotating
   pieces (drill arms, rotor) caught at other animation phases and a **strip
   up to 5 px wide along each body's right edge**, which is the engine's
   punch-out (`0x4B9D70(body, scratch, 5, 0)`) that we do not replicate.
   (Re-read 2026-09-07: the punch-out's shift arithmetic puts the body's pixels
   on the shadow's own screen pixels, so it cannot erase a strip beside the
   body; the strip is not in that day's toggle masks either, and its cause is
   open — [exe-reverse-engineering](exe-reverse-engineering.html) §"The slant
   builders".) Not closed: a replacement (glTF) structure casts every piece (no per-piece
   bit1 on that side). **A nanoframe casting nothing is now the correct
   behaviour for most of a build** — measured 2026-09-03 against the stock
   renderer on one solar at one spot with only the build state varying: over the
   pixels the completed unit darkens by half, the lobe reads 1.00 of bare terrain
   at 25 % built, 0.87 at 89 %, 0.70 at 95 % and 0.48 complete. So the engine
   draws no shadow for a unit under construction until the last few per cent,
   where something partial appears — and the fixture back-dates a completed unit
   rather than lathing one, so that tail is not trustworthy
   ([build-state](build-state.html) §7). The native pass suppresses it on `Nanoframe != 0`
   (G13l), which it has to: the scaffold's erased parts would otherwise show
   our slant projection through as a black silhouette. Why the engine's own
   structure branch — which has no nanoframe test and does blit
   `Object3do+0x14` — ends up drawing nothing is NOT settled; see
   [exe-reverse-engineering](exe-reverse-engineering.html) §"The unit blit's
   shadow branches". Fixture: `scenarios/shadow-struct.json`.
   **Factory-built check (2026-09-02):** a Peewee rolled out of an ARMLAB reads
   state `0x90242321` with nano = 0 — bit `0x20000000` CLEAR — while a
   commander-built ARMSOLAR reads `0x30282321` after completion — bit SET. So
   the bit marks structures (and nanoframes), not only "under construction",
   and the ownership rule holds in a real game (panel
   `assets/shots/g12c-factory-built-shadow.png`).
   **Structure shadows, parity (G14j, 2026-09-07).** The owned slant was drawn
   by the body emitter with the projection swapped in, so it inherited the
   body's rules — the material lookup (a face with neither texture nor colour
   skipped, a texture's key discarded per fragment) and, in the draw, the
   silhouette's waterline and digger erase. The engine's raster has none of
   them (§3), and the erase is what emptied the Kbot lab's shadow on the shore:
   the applier-created lab sits at altitude 63 under a sea level of 75 with a
   depth plane on its composite (path B), so every shadow fragment at model
   height ≤ 12 was erased and only the nano arms' tops (14.7) survived — 515 px
   in the engine's own Shadows-toggle mask against the stock engine's 959.
   `emit_slant` in `tagpu_native.c` now follows `0x45A610` rule for rule (every
   face of every visible+cached piece, flat, integer-snapped, face 0 under the
   selection rule) and the draw passes −1e9 for both erases on a slant unit
   (`tagpu_hires_draw.c` does the same for a replacement structure's). Measured
   with the engine's toggle on `scenarios/shadow-lab.json`, same eye, 200×140 px around each
   building, stock engine / ours: Kbot lab **997 / 1094**, solar 1228 / 1246,
   ARM extractor 2406 / 2442, COR extractor 618 / 859 and COR wind 2273 / 3169
   (the drill arms and the rotor caught at other phases), commander 1060 /
   1098 — panel `assets/shots/g14j-slant-toggle.png`. The lab's Classic lane
   draws the same slant since the same day ([tascene design](tascene-design.html)):
   its own toggle gives the Kbot lab the same three strips, 692 px against the
   game's 1094 and the engine's 997 (the lab's rest-pose body is 2 px narrower
   on the right and shorter at the top than the engine's live one). One seam the
   engine never has, in the game and the lab alike: the body is drawn from float
   vertices while the engine snaps composite and shadow to whole units alike,
   so along a body edge on a fractional row the snapped shadow shows as a 1-px
   line beside it — about a hundred of the Kbot lab's 1094. Snapping the body
   the engine's way would close it and moves every body edge; not this landing's.

   **Waterline / digger clipping, implemented 2026-09-02** in the native
   pass, mirroring the rules above: per-vertex model height in the vertex
   stream; per unit, only when the composite has a depth plane (path B),
   `sub = seaLevel(TA+0x1427F) − altitude`; if `sub > 0`, fragments with
   `vy <= sub` are erased for enemies without state bit `0x200` (the compare
   byte is `TA+0x2A43`) and tinted `r/2, g/2, b/2+0x32` for own / sonar-seen
   units; the shadow pass always erases them; `digger` (mask bit30) erases
   `vy <= 0`. Measured on Anteer Strait (sea 75): commanders at altitude 62
   and 55 — the stock engine tints shins/feet navy and cuts the shadow at the
   same height ours now does (panel `assets/shots/g12c-waterline-ab.png`).
   Not exercised: the enemy-erase branch (the AI walks its units out of the
   water; a kbot cannot be created in it) and 3D wrecks (left unclipped).
2. **Cloak = 50 % blend, owner-only.** If `unit+0x10E & 4`: blend the unit's
   colour output with the destination at 50 % (the engine's ALP table is
   exactly `nearest(pal[src]/2 + pal[dst]/2)`); in GL just use
   `glBlendFunc(GL_SRC_ALPHA, ...)` with α = 0.5. Enemy-view invisibility is
   already handled upstream (the unit never reaches the draw list).
3. **Underwater**: submerged parts of *enemy* units without the sonar flag are
   cut; *own/sonar-detected* submerged parts are tinted (r,g halved, b/2+50) —
   depth-plane thresholding in TA; a y-cut in world space for GL.
4. **Team colours** are chosen at rasterise time by owner id (per-player GAF
   frame variants) — the GL texture path must key its team-colour remap off
   `unit+0xFF`, not off any cloak state.
5. **Layer order** per frame: terrain → per-row (ground units `(state&3)==1`
   interleaved with features, back to front) → weapons → explosions → airborne
   units `(state&3)!=1` (all rows, unsorted) → UI. Shadows are emitted inside
   each unit's own draw, immediately before its body.
6. **If we suppress TA's blit and own the pixels**, the shadow and cloak also
   vanish (they are blits of the composite/scratch) — the GL renderer must
   re-create both, which this note fully specifies.

---

## Appendix — addresses & conventions

| VA | Role | Convention |
|---|---|---|
| `0x4B8500` | `AlphaCompsteBuf2OFFScreen` — ALP-table blit | stdcall(ctx,frame,x,y), ret 0x10 |
| `0x4B7F90` | `CopyGafToContext` — plain colour-keyed blit | stdcall, ret 0x10 |
| `0x4CBF2C` | inner shaded copy `dst = ALP[dst + src·256]` | cdecl(ctx,desc,srcR,dstR,key,table) |
| `0x4CC057` | inner shaded copy, RLE frames | cdecl |
| `0x4CBE70` / `0x4CC51D` | plain inner copies (raw / RLE) | cdecl |
| `0x4B86E0` | grey-table blit sibling (gate `+0xF1`&1) | stdcall |
| `0x4B6220` | `GetTAProgramStruct` → `*(0x51FBD0)` | — |
| `prog+0xC0/C4/C8/CC/D0` | ALP / SHD / LHT / grey / water tables | see §1 |
| `prog+0xF0` | table capability bits 5..9 | — |
| `0x4BA750/0x4BADF0/0x4BABD0/0x4BAD30/0x4BAF30` | table generators (from palette `TA+0x143A7`) | — |
| `0x42E140/1D0/260/2F0/300` | load-or-generate wrappers (`PALETTE.ALP/.SHD/.LHT`), called by `UIPipelinesInit 0x491200` | — |
| `0x459200` | per-unit blit; shadow + cloak routing lives here | thiscall, 6 args, ret 0x18 |
| `0x459295` / `0x459324` | Shadow (bit2) / TShadow (bit3) gates | — |
| `0x4592AC` / `0x45932B` / `0x4592C8` | `noshadow` / `canhover|floater` / `digger` mask tests | — |
| `0x45930B`,`0x459349` | shadow x = sx+0x85 (body +0x80) | — |
| `0x45A470` | scratch := composite; `0x4B96A0` blackens (→ index 0) | thiscall(frame) |
| `0x45A790` | nanoframe slant shadow → `Object3do+0x14`, Compressed=1 | thiscall(obj,frame) |
| `0x45A510` / `0x45A610` | slant AABB / slant silhouette raster (`x+y/4`, `−z−y/4`) | — |
| `0x4B9D70` / `0x4B9E60` / `0x4BA000` | overlap punch-out / RLE compressor | — |
| `0x4BA1B0(frame,t)` | erase colour where depth ≤ t (waterline/ground clip) | cdecl-ish, 2 args |
| `0x4B96E0(frame,t)` | water-tint colour where depth ≤ t | — |
| `0x4589C0` | composite+cargo → scratch (protects the cache) | thiscall |
| `0x459830` | opaque rasteriser; depth = y+0x32(+0x4B digger); owner→team colour | thiscall, 4 args, ret 0x10 |
| `0x468CF0` | `DrawGameScreen(drawWorld,_)` | — |
| `0x469A00` / `0x469BA3` | DrawUnit sites: ground `(state&3)==1` / airborne `!=1` | — |
| `0x49BE60` / `0x420B00` | weapons draw / explosions draw (between the sweeps) | — |
| `0x465AC0` | `UnitInPlayerLOS` — cloak bit2 ⇒ invisible to non-owners | — |
| `TA+0x37F06` | options word (bit1 AA, 2 Shadow, 3 TShadow, 4 FShadow, 5 Shading) | — |
| `TA+0x1427F` / `TA+0x14280` / `TA+0x2A43` | sea level byte / mapDebugMode / local player id | — |
| `unit+0x10E` | cIsCloaked: bit0 order, **bit2 cloaked**, bit4 sim | — |
| `unit+0x110 & 3` | 1 = ground layer, 2 = airborne (draw-sweep selector) | — |
| `unit+0xFF` | owner id → team-colour GAF variant (the mislabelled "cloak" arg) | — |
| `unitdef+0x241` | FBI bool mask (11 canfly, 12 canhover, 19 floater, 25 noshadow, 30 digger) | — |
| `Object3do+0x10` / `+0x14` | body composite / cached compressed nanoframe shadow | — |
