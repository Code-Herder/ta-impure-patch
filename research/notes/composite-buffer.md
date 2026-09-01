# The per-unit composite buffer & the live palette (Gate G6)

*The two static-RE prerequisites that unblock **G6 write-back**: (1) the byte-accurate
format of the per-unit composite `GAFFrame` that `Object3do+0x10` points at — header,
colour plane, depth plane, how the blit reads it and maps it to screen — and (2) where
TA's live 256-colour palette lives and its entry format, so our palettiser can nearest-
match GPU pixels. Plus a concrete G6 write-back proof plan.*

All VAs are for the pristine build (ImageBase `0x400000`, md5
`8e74a1dffa1f5988624c52048f5b20cd`). Disassembly via
`objdump -D -b binary -m i386 -M intel --adjust-vma=0x400C00`; decompiler output from
`tools/ghidra-projects/TA.gpr` (613 corpus symbols + `tamem.h`). `.text` VA→file =
`VA−0x400C00`; `.data` VA→file = `VA−0x401A00`.

Evidence tags: **[BINARY-VERIFIED]** = I read the instructions/decompiler output for
this build (VA cited). **[CORPUS]** = a name/layout from TADR (`buildghost.h`,
`tamem.h`) or `totala-re`, cross-checked against our bytes. **[INFERRED]** = my reading,
not yet runtime-confirmed.

---

## Summary — the one thing to know

The buffer at **`Object3do+0x10`** is a single-frame **`GAFFrame`**: a **0x18-byte
header** followed by **two byte-planes** — a `w·h` 8bpp **colour** plane (palette
indices), then a `w·h` 8bpp **depth** plane. Total `w·h·2 + 0x18`. It is **planar, not
interleaved**; colour first, depth second. Both planes are **top-down, row-major, stride
= Width**. The header carries the two intra-buffer plane pointers at `+0x10` (colour) and
`+0x14` (depth) and a **`HotspotX/Y`** that positions the model origin inside the sprite.
[BINARY-VERIFIED `0x437BE0`]

TA blits it with `CopyGafToContext 0x4B7F90` — a colour-keyed 8bpp copy that reads
**only the colour plane**, skipping pixels equal to `ColorKey` (index **1**), and lands
the sprite at screen `(sx − HotspotX, sy − HotspotY)` where `(sx,sy)` is the projected
unit origin. The **depth plane is not consumed by the ground blit**; it is read only by
the intra-model rasteriser and by the cargo z-merge `0x4B90A0`. [BINARY-VERIFIED]

For G6 we allocate/own exactly this structure, fill the header dims + hotspot, write our
palettised pixels into the colour plane (background = index 1), write a sane depth plane,
and TA blits it for free.

---

## 1. The `GAFFrame` composite struct — `Object3do+0x10`

Byte-accurate layout, read straight out of the allocator `0x437BE0` (colour+depth) and
its colour-only twin `0x437B50`. Matches TADR's `GhostGAFFrame` [CORPUS,
`buildghost.h`] field-for-field.

| Off | Size | Field | Set by allocator | Meaning |
|---|---|---|---|---|
| `+0x00` | u16 | **Width** | `= w` (AABB) | sprite width in px = colour/depth plane stride |
| `+0x02` | u16 | **Height** | `= h` (AABB) | sprite height in px |
| `+0x04` | s16 | **HotspotX** | `0`, then overwritten by builder | model-origin X inside the sprite (`2 − minX`) |
| `+0x06` | s16 | **HotspotY** | `0`, then overwritten by builder | model-origin Y inside the sprite (`2 − minY`) |
| `+0x08` | u8 | **ColorKey** | `= 1` | transparent palette index (background fill) |
| `+0x09` | u8 | **Compressed** | `= 0` | 0 ⇒ raw plane; 1 ⇒ RLE (`0x4CC51D` path) |
| `+0x0A` | u8 | **SubFrames** | `= 0` | 0 ⇒ single frame; >0 ⇒ `+0x10` is a `GAFFrame*[]` |
| `+0x0B` | u8 | **AlphaBlend** | `= 0` | picks alpha vs. plain blit inside `CopyGafToContext` |
| `+0x0C` | u32 | Unknown_0C | `= 0` | unused by the paths we hit |
| `+0x10` | ptr | **PtrColour** | `= buf + 0x18` | colour plane (w·h bytes) |
| `+0x14` | ptr | **PtrDepth** | `= buf + 0x18 + w·h` | depth plane (w·h bytes); **0** for colour-only frames |

Then, contiguous in the same allocation:

```
buf+0x18                    : colour plane, w·h bytes, pre-filled 0x01 (ColorKey)
buf+0x18 + w·h              : depth  plane, w·h bytes, pre-filled 0x00 (far)
```

**Pixel format** [BINARY-VERIFIED]: **8bpp palettised** colour (one index/byte) + **8-bit
depth** (one byte/pixel). Confirmed — the allocation is exactly `w·h·2 + 0x18` and the two
fill loops write `0x01` to the colour plane and `0x00` to the depth plane.

**Orientation / stride** [BINARY-VERIFIED, from the copy loop `0x4CBE70`]: **top-down,
row-major, stride = Width** (no negative pitch). Pixel `(x,y)` = `PtrColour[y·Width + x]`
and `PtrDepth[y·Width + x]`.

> **Evidence — allocator `0x437BE0` size + planes** [BINARY-VERIFIED]
> ```
> 437bf2  imul  esi,ebx              ; esi = width * height
> 437bf5  lea   eax,[esi+esi*1+0x18] ; alloc size = w·h·2 + 0x18
> 437bfb  call  0x437a30             ; = the heap allocator (returns buf at *param_1)
> ```
> Decompiled tail (`FUN_00437be0`):
> ```c
> uVar4 = param_3 * param_2;                     // w·h  (param_2=W, param_3=H)
> FUN_00437a30(this, param_1, uVar4*2 + 0x18);   // alloc
> *puVar1        = (short)param_2;               // +0x00 Width
> puVar1[1]      = (short)param_3;               // +0x02 Height
> *(u8*)(puVar1+4)= 1;                            // +0x08 ColorKey = 1
> *(void**)(puVar1+8)  = puVar1 + 0xc;           // +0x10 PtrColour = buf+0x18
> *(void**)(puVar1+10) = (buf+0x18) + uVar4;     // +0x14 PtrDepth  = buf+0x18+w·h
> // fill depth plane 0x00 (far), fill colour plane 0x01 (ColorKey)
> ```
> Colour-only twin `0x437B50` is identical but allocates `w·h + 0x18`, writes `+0x14 = 0`
> (no depth plane) and fills only the colour plane.

TADR corroboration [CORPUS, `buildghost.h`]: *"GAFFrame layout (0x18 bytes total; matches
engine GAFFrame struct). `Sprite_RemapColorsByDepthRange (0x458d30)` reads exactly this
layout: pixel ptr at +0x10 and depth ptr at +0x14."* Verified: `0x458D30` walks
`w·h = *param_1 * param_1[1]` pixels, reads colour from `*(param_1+8)` and depth from
`*(param_1+10)`, skips `colour == param_1[4]` (ColorKey). [BINARY-VERIFIED]

---

## 2. The AABB & builder — `0x4581E0`, `0x4586A0` (what fills the header)

`0x4586A0` (the sprite-cache builder, G6 substitution target) orchestrates:

1. **`0x4581E0`** computes the screen-space AABB over every **visible** posed piece
   (`prim+0x28 & 1`), projecting each posed vertex (`prim->vbuf`, `+0x22`) with the
   oblique iso `sx = x>>16`, `sy = (−z − y/2)>>16`, taking min/max with a 2px margin.
   [BINARY-VERIFIED] Outputs:
   - `Width  = (maxX − minX) + 4`
   - `Height = (maxY − minY) + 4`
   - `HotspotX = 2 − minX`, `HotspotY = 2 − minY`  ← model origin's pixel inside the sprite
2. Allocates the buffer at **`Object3do+0x10`** (`piVar3+4`) via `0x437BE0`
   (colour+depth) — or `0x437B50` (colour-only) for the special non-z case
   `param_2==0 && !(unit+0x114 & 1) && unit.speed==0`. [BINARY-VERIFIED]
3. Writes `HotspotX→buf+0x04`, `HotspotY→buf+0x06` (the allocator had zeroed them).
4. Rasterises the posed pieces into both planes via **`0x459830`** (opaque) or
   **`0x459C70`** (build/nanoframe), each face stamped by `GAF_DrawTransformed 0x4C7580`,
   which interpolates a per-pixel **depth = vertex.y + 0x32 (+0x4B if not cloaked)** and
   writes it into the depth plane. [BINARY-VERIFIED — depth carries elevation]

> `FUN_004586a0`: `FUN_004581e0(&W,&H,&hotX,&hotY,obj3do,0)` →
> `FUN_00437be0(this, obj3do+0x10, W, H)` → `buf[+4]=hotX; buf[+6]=hotY` →
> `FUN_00459830()`. [BINARY-VERIFIED]

**TADR patches the AABB cap at `0x458195`** [CORPUS] — the stock composite is bounded
600×600, so a full-screen unit sprite is at most `600·600·2 + 0x18 ≈ 703 KB`.

---

## 3. The blit — `0x459200` → `CopyGafToContext 0x4B7F90` → `0x4CBE70`

`0x459200(this, OFFSCREEN* ctx, Object3do* obj, eyeX<<16, _, eyeY<<16)` reads
`obj+0x10` (the composite), projects the unit origin, and stamps the colour plane onto
the game offscreen.

**Screen mapping** [BINARY-VERIFIED, matches the field-notes world→screen]:
```
worldX = unit+0x6A   sx = (worldX − eyeX)>>16 + 0x80        (+128)
worldZ = unit+0x72   sy = (worldZ − eyeY)>>16 − (worldY>>1) + 0x20   (−alt/2 +32)
worldY = unit+0x6E   (altitude; GetPosHeight variant for shadow y)
```
The normal opaque path is:
```c
CopyGafToContext(ctx, obj->composite /*+0x10*/, sx + 0x80, sy);
```
Inside `CopyGafToContext 0x4B7F90` (for `SubFrames==0, Compressed==0`) [BINARY-VERIFIED]:
```c
dstLeft = (sx) − HotspotX;   dstTop = (sy) − HotspotY;   // model origin lands at (sx,sy)
srcDescriptor = { Width, Height, stride=Width, pixels=PtrColour, key=ColorKey };
FUN_004cbe70(ctx, &srcDescriptor, &srcRect, &dstRect, ColorKey);   // clipped copy
```
`0x4CBE70` is a straight **top-down, colour-keyed** copy [BINARY-VERIFIED]:
```c
srcStride = *(desc+8);   src = *(desc+0xc) + top*srcStride + left;   // composite colour plane
dstStride = *(ctx+8);    dst = *(ctx+0xc) + top*dstStride + left;    // OFFSCREEN pixels
for each row: for each col: if (*src != ColorKey) *dst = *src;       // depth NOT read here
              src += srcStride − rowW;  dst += dstStride − rowW;
```
So the **ground blit reads the colour plane only** and honours the colour-key.
`OFFSCREEN` is itself 8bpp (`+0x08` = pitch, `+0x0C` = pixel base) [BINARY-VERIFIED].
The alpha/shaded variant `AlphaCompsteBuf2OFFScreen 0x4B8500 → 0x4CBF2C` is identical in
shape but maps each pixel through a shade table (`*(dst) = table[dst + src·0x100 + bias]`)
— used for cloak/shadow/water. [BINARY-VERIFIED]

---

## 4. Depth-plane semantics — `0x4B90A0` (z-merge)

The depth plane is consumed when TA merges **attached/cargo units** into the parent
composite. `0x4B90A0(src, dst, dx, dy, bias)` walks the child's colour+depth planes into
the parent's:

> **Evidence — inner test** [BINARY-VERIFIED `0x4B9134`]
> ```
> 4b9134  mov cl,[edx]      ; src colour
> 4b9136  mov dl,[ebp+8]    ; src.ColorKey  (frame+0x08)
> 4b9139  cmp cl,dl ; je    ; skip if transparent
> 4b9143  mov dl,[edi]      ; src depth
> 4b9145  add edx,ebx       ; src_depth + bias
> 4b9149  mov bl,[eax]      ; dst depth (existing)
> 4b914b  cmp ebx,edx
> 4b914d  jg  skip          ; skip if  dst_depth > src_depth+bias
> 4b9153  mov [esi],cl      ; write colour
> 4b9157  add cl,dl ; mov [eax],cl   ; write depth = src_depth + bias
> ```

So a pixel is written iff **`dst_depth ≤ src_depth + bias`** → **larger depth value =
nearer/in-front**; the plane is initialised to **`0x00` = far**, so the first write always
takes. The `bias` passed from `0x459200` is the **altitude difference** `(child.y −
parent.y)>>16`, i.e. depth encodes **world-Y (elevation)**: higher pieces/cargo draw in
front. [BINARY-VERIFIED]

> **Correction to `frame-composition.md`:** that note stated the merge condition as
> "child_depth ≤ existing_depth + offset". The bytes show the operands the other way:
> **existing(dst) ≤ new(src)+bias**, i.e. the *new* pixel wins when it is nearer-or-equal.
> Convention: **0 = far, 255 = near** (matches `buildghost.h`).

This answers G4 open-question #6 (depth convention): **0 far / 255 near, higher wins,
bias = elevation delta**. For a *lone* unit with no cargo the depth plane is written during
rasterisation but never read by the ground blit — it only matters intra-model (the
rasteriser's own per-pixel z, via `GAF_DrawTransformed`) and for cargo merges.

---

## 5. Fields we must fill for the engine to blit our content

To hand TA a valid composite (own the buffer, or overwrite after it builds one):

| Field | Value to write | Why |
|---|---|---|
| `+0x00 Width` / `+0x02 Height` | our sprite W/H (≤600 stock) | plane stride & blit extent — **must match the plane sizes** |
| `+0x04 HotspotX` / `+0x06 HotspotY` | model-origin px inside sprite | positions the sprite; wrong ⇒ unit renders offset from its feet |
| `+0x08 ColorKey` | **1** (keep engine default) | pixels == 1 are transparent; fill background with 1 |
| `+0x09 Compressed` | **0** | ensures the raw-plane path (`0x4CBE70`), not RLE |
| `+0x0A SubFrames` | **0** | ensures single-frame path (else `+0x10` is treated as a `GAFFrame*[]`) |
| `+0x0B AlphaBlend` | 0 (opaque) | leave 0 unless we want the shade-table path |
| `+0x10 PtrColour` | → our `w·h` colour plane | 8bpp palette indices, top-down, stride=W |
| `+0x14 PtrDepth` | → our `w·h` depth plane | 8-bit elevation depth; **must stay valid** — the cargo z-merge reads it |

If we only overwrite the **contents** of a buffer TA already allocated (recommended, §8),
we leave the header dims/hotspot as TA set them and just repaint the two planes within
`Width·Height` — nothing to reallocate, nothing else to touch.

---

## 6. TA's live 256-colour palette (the palettiser target)

**The active 256-colour RGB palette lives inline at `MainStruct + 0x143A7`** (i.e.
`*(void**)0x00511DE8 + 0x143A7`), **256 entries × 4 bytes = 1024 bytes**. [BINARY-VERIFIED]

It is loaded once by **`FUN_0042A400`** from the HPI resource **`palettes\PALETTE`** and
copied into `+0x143A7`:

> `FUN_0042a400`: `p = FUN_00429330("PALETTE"); for(256 dwords) *(main+0x143a7+i)=p[i];`
> [BINARY-VERIFIED] — string at `.data 0x5033A4` = `"PALETTE"`, dir prefix `0x5031BC` =
> `"palettes"`. `UIPipelinesInit 0x491200` calls `0x42A400` at init, then feeds
> `main+0x143A7` to the shade/alpha-table builders `0x42E140/0x42E1D0/0x42E260/0x42E2F0/
> 0x42E300` and to the GUI loader `0x4AC7D0` — i.e. `+0x143A7` is *the* palette passed
> everywhere. [BINARY-VERIFIED]

**Entry format = DirectDraw `PALETTEENTRY` order: `{ byte0 R, byte1 G, byte2 B, byte3 0 }`**
— **RGB, not BGR.** Proven by the PCX/PAL loader `FUN_004CAF30`, which copies each source
RGB triple as R→[0], G→[1], B→[2], 0→[3]:

> `FUN_004caf30` [BINARY-VERIFIED]:
> ```c
> *param_2   = puVar6[-2];  // R
> param_2[1] = puVar6[-1];  // G
> param_2[2] = *puVar6;     // B
> param_2[3] = 0;           // flags/pad
> param_2 += 4; puVar6 += 3;
> ```
> Consistent with TA handing this array straight to `IDirectDraw::CreatePalette(caps,
> LPPALETTEENTRY, …)` (whose entry type is exactly R,G,B,flags).

**Runtime shortcut for our overlay (best source).** Our GPU code lives inside the
cnc-ddraw fork, and TA sets the surface palette through our own DLL. cnc-ddraw's
`ddp_SetEntries` (`vendor/cnc-ddraw/src/ddpalette.c`) already stores the live 256 entries
as **`This->data_rgb[256]` (`RGBQUAD`: rgbRed,rgbGreen,rgbBlue,0)** and a packed
`This->data_bgr[256]`. Read `g_ddraw.primary->palette->data_rgb` — it is byte-identical to
`main+0x143A7` and needs no engine-memory read. [CORPUS + BINARY-VERIFIED path]
**Caveat:** with `!DDPCAPS_ALLOW256`, cnc-ddraw forces index **0 → black** and **255 →
white**; our palettiser should treat 0/255 as reserved and never emit index **1** for
opaque pixels (it is the composite ColorKey).

### Player-colour remap
Two distinct "palette" tables exist — keep them separate:
- `MainStruct + 0x143A7` — the **256×RGB** table above (what we match against).
- `MainStruct + 0x0DCB` — TADR's `ColorsPalette`: a **256-byte LUT of logical→physical
  indices** (`FIRST_GUIPAL_COLOR`), used for HUD/GUI colours (selection box, health/reload
  bars). [CORPUS, `ReloadBars.cpp`, `Colors.pas`] **Not** RGB, and **not** the unit
  team-colour ramp. `GetGuiPaletteColor(ta,i) = *(u8*)(ta+0xDCB+i)`.

Player/team **unit** colour in stock TA is applied as a small **contiguous index remap**
inside the unit sprites (a per-player band of the 256 indices), not by rewriting the RGB
table — so `+0x143A7` stays the ground truth for RGB, and the player's units already carry
their remapped indices in the composite colour plane we read. [INFERRED — exact band not
needed for the palettiser; our GPU pixels are matched to `+0x143A7` RGB regardless.]

---

## 7. G6 write-back — evidence-backed plan for the simplest proof

**Goal:** stamp a recognisable test pattern (solid colour / vertical gradient) into a
unit's `Object3do+0x10` composite in the correct 8bpp+depth format, and let TA's own
`0x459200`/`CopyGafToContext` blit it onto the frame — isolating the write-back mechanism
from 3DO rendering.

**Where to inject.** The composite is **per-unit-persistent** (G4-confirmed: `compA =
Object3do+0x10` stable heap ptr `0x05890028`, tied to the commander's obj3do). Reach it
read-side exactly as G4 did — from the tracer's `0x459200` hook we already have
`arg0 = Object3do*`; `obj3do+0x10` is the buffer. The overlay/tracer runs in-process, so
we can memcpy into it.

**Steps (per targeted unit, once per frame it is drawn):**
1. `obj3do = *(void**)(unit + 0x9E)`; `frame = *(GAFFrame**)(obj3do + 0x10)`.
2. **Guard:** bail unless `frame != NULL` **and** `frame->Compressed == 0` **and**
   `frame->SubFrames == 0`. Read `W=frame->Width`, `H=frame->Height`,
   `col=frame->PtrColour (+0x10)`, `dep=frame->PtrDepth (+0x14)`.
3. Fill the colour plane in place: for `y,x` in `W·H`, write our test index to
   `col[y·W + x]` — **never index 1** for visible pixels (that is the ColorKey and would
   show as transparent); leave a 1-px border = 1 to prove the key still works.
   A vertical gradient (index = `min(254, 8 + y·200/H)`, skipping 0/1/255) reads as an
   obvious "our content" banner.
4. Write a **monotonic depth** so intra-model/cargo z stays sane: `dep[y·W+x] = clamp(y or
   a constant, 1..254)`. Do **not** leave it uninitialised and do **not** overrun `W·H` —
   the cargo z-merge `0x4B90A0` reads this plane.
5. Do nothing else — TA's `0x459200` (already running that frame) blits `col` via
   `CopyGafToContext` at the projected `(sx − HotspotX, sy − HotspotY)`. Our pattern
   appears exactly where the unit's sprite would.

**Guards / correctness:**
- Only write when the ptr is valid and the unit is actually being drawn this frame — hook
  or gate off `0x459200` (fires 1:1 with `DrawUnit`, only for on-screen, in-LOS units), so
  we never touch a stale buffer.
- Respect `Width/Height`; index strictly `[0 .. W·H)` for both planes. Keep `ColorKey=1`,
  `Compressed=0`, `SubFrames=0` untouched.
- Don't corrupt the depth plane (write it, sized to `W·H`) — the z-merge depends on it.

**Suppress the engine draw, or overwrite after it? — Overwrite after.**
The G5 suppression detours `DrawUnit 0x45AC20`, which **skips the engine's lazy repose and
sprite-cache build** — so for a suppressed unit `Object3do+0x10` may be **stale or never
allocated** (its `vbuf`/composite stop refreshing; noted in G5). Therefore the clean G6
proof is **not** to suppress: let the engine run `DrawUnit → 0x458810 → 0x4586A0`
(building a correctly-sized, correctly-hotspotted buffer), then **overwrite the two planes
in place after the build but before/at the blit**. Concretely, hook **`0x458810`** *after*
its `0x4586A0` rebuild (or the entry of `0x459200`), repaint `col`/`dep`, and fall through
to TA's blit. This guarantees a valid buffer, valid dims, valid hotspot, and a live
projected position — the write-back mechanism is exercised with zero dependence on our own
3DO rasteriser. Suppressing the engine draw is the *next* step (own the whole buffer),
once write-back-after-build is proven; at that point we must also drive our own repose or
re-trigger `0x4586A0`, per the G5 caveat.

---

## Appendix — addresses & file offsets

| VA | File off | Role |
|---|---|---|
| `0x4586A0` | `0x57AA0` | composite **builder** (AABB→alloc→rasterise); G6 target |
| `0x4581E0` | `0x575E0` | screen-space **AABB** → W/H/HotX/HotY |
| `0x437BE0` | `0x36FE0` | **allocator** colour+depth (`w·h·2 + 0x18`) |
| `0x437B50` | `0x36F50` | allocator colour-only (`w·h + 0x18`, `+0x14 = 0`) |
| `0x459200` | `0x58600` | composite → offscreen **blit** + cargo z-merge; projection |
| `0x4B7F90` | `0xB7390` | `CopyGafToContext` — colour-keyed 8bpp copy (reads colour plane) |
| `0x4CBE70` | `0xCB270` | inner raw copy loop (top-down, stride=W, key-skip) |
| `0x4B8500` | `0xB7900` | `AlphaCompsteBuf2OFFScreen` — shaded/alpha variant |
| `0x4CBF2C` | `0xCB32C` | inner shaded copy (shade-table indexed) |
| `0x4B90A0` | `0xB84A0` | per-pixel **8-bit z-merge** (dst ≤ src+bias ⇒ write; 0=far) |
| `0x458D30` | `0x58130` | `Sprite_RemapColorsByDepthRange` — confirms +0x10/+0x14 planes |
| `0x459830` | `0x58C30` | opaque rasteriser (writes both planes; depth = y+const) |
| `0x4C7580` | `0xC6980` | `GAF_DrawTransformed` — per-face stamp, per-pixel depth |
| `0x42A400` | `0x29800` | loads `palettes\PALETTE` → `main+0x143A7` |
| `0x429330` | `0x28730` | resource loader for `"PALETTE"` (0x400 bytes) |
| `0x4CAF30` | `0xCA330` | PCX/PAL loader — proves entry order **R,G,B,0** |
| `0x491200` | `0x90600` | `UIPipelinesInit` — feeds `main+0x143A7` everywhere |
| `main+0x143A7` | — | **live 256×4 RGB0 palette** (palettiser target) |
| `main+0x0DCB` | — | GUI-colour LUT (`ColorsPalette`, index→index; not RGB) |
| `main+0x3907F..0x3908B` | — | `Palette`/`currentPalette`/`desiredPalette`/`FadeTable` (menu fades) |
| `Object3do+0x10` | — | **the per-unit composite `GAFFrame*`** (G6 write target) |

## Write-back proven live (G6 mechanism) — 2026-08-31

The write-back path is proven end-to-end: **our content, written into `Object3do+0x10`,
is blitted onto the frame by TA's own compositor.** Implementation:
`writeback_paint()` in `tagpu/ddraw/src/tagpu_overlay.c`, armed by a `tagpu_writeback.on`
file (first token = target unit type, default `armcom`). Evidence:
`evidence/g6-writeback.txt`; shots `g6-writeback-scene.png` / `g6-writeback-zoom.png`.

**Approach (per Deliverable 3 — overwrite, do NOT suppress).** The overlay's per-frame
callback runs *after* the frame's blit, so each frame we walk the unit array, match the
target type, and repaint the composite's colour plane **in place**; the *next* frame TA
re-blits our modified buffer (one-frame latency, invisible). This needs the engine to
have built the buffer, so the unit must be on-screen & in-LOS (off-screen ⇒
`Object3do+0x10` is null — confirmed by the diagnostic below). No detour, so it never
fights the G5 suppressor.

**Live confirmation.** The one-shot diagnostic printed the real identity + format:
`name@00="Commander" unitname@20="ARMCOM" objname@80="ARMCOM"` (so type-matching must
check all three fields — @0x00 is the display name), and `comp=00000000` while the
commander was off-screen, becoming a valid pointer once scrolled on-screen. We then
filled the colour plane with a scrolling diagonal index gradient (`2 + (x+y+t)%252`,
avoiding reserved indices 0/1/255), row-major, stride = Width. Result: the ARM
Commander's sprite is replaced by our animated gradient rectangle — the exact bytes we
wrote, mapped through TA's live palette, at the correct hotspot-anchored screen position.
This validates every field of the format above (Width/Height/PtrColour, top-down stride,
ColorKey semantics) and the palette address, and closes the integration question for the
whole renderer: **we now have a proven channel to put arbitrary pixels into TA's
software compositor.**

**What remains for full G6.** Replace the synthetic gradient with a real render: rasterise
the unit's posed 3DO (posed vertices at `PrimitiveStruct+0x22`, faces from the
`Model3DONode` tree — see [Unit → 3DO bridge](unit-3do-bridge.html); quads/N-gons need
triangulation) into a GL FBO with an ortho camera matching TA's dimetric view, read it
back, nearest-match each texel to the `main+0x143A7` palette (emitting ColorKey=1 for
transparent texels and a sensible depth plane), and write both planes. The hard part —
getting our output into the engine's pipeline — is done; this is the geometry/texturing
work that carries into Phase B.
