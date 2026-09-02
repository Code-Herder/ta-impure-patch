# Frame Composition — the per-unit draw path (Gate G3)

*Static map of how `TotalA.exe` composes one frame, anchored on the verified
Lock/Unlock heartbeat and walked down to the per-unit sprite blit. Produced for
**Gate G3** of the GPU-renderer plan. All addresses are VAs for our pristine
build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`); disassembly
via `objdump -D -b binary -m i386 -M intel --adjust-vma=0x400C00` on the raw exe,
decompiler output from the `tools/ghidra-projects/TA.gpr` project (613 corpus
symbols applied).*

Evidence tags: **[VERIFIED-static]** = I read the instructions/decompiler output
for this build. **[CORPUS]** = a name/signature from TADR or the Ghidra-import
corpus, cross-checked against our bytes. **[INFERRED]** = my reading, not yet
runtime-confirmed.

## Summary — the one thing to know

**The `ddraw` Lock/Unlock heartbeat is the *present*, not the composition.** Our
ddraw hook sees `IDirectDrawSurface::Lock` return to `0x4C650C` and `Unlock`
called from `0x4C659B` ~30×/s. Both sit inside **`FlipOffscreenToPrimary`
`0x4C63A0`** [VERIFIED-static], which is called *last*, from the tail of the frame
compositor at `0x46A3DB`. It Locks the primary DirectDraw surface, byte-copies the
already-finished 8bpp game offscreen onto it (`CopyScreenContext 0x4CBBE0`), and
Unlocks. **Every unit, feature, terrain tile and shadow was already stamped into
an offscreen buffer *before* the Lock fires.**

So the whole unit-render pipeline runs earlier, inside **`DrawGameScreen`
`0x468CF0`** [CORPUS: TADR `DrawGameScreen`], which draws into the game's working
offscreen context (a software `OFFSCREEN` struct, not a DDraw surface). To own the
unit render we hook *inside* `DrawGameScreen`, never at the Lock. The Lock stays
useful only as a "frame is done" fence.

The pipeline, top to bottom (each link verified in this build):

```
main message loop  (FUN_0049e830, per MAIN_LOOP_ANALYSIS.md)
  └─ IdleTick 0x499890                         [CORPUS] runs when no Win32 msgs pending
       └─ (**(TA+0x391F5))()                   installed frame callback; = 0x496790 in-game
            └─ GameFrame_InGame 0x496790       [VERIFIED-static] the in-game per-frame handler
                 ├─ sim/logic ticks (0x49523x/0x495490/0x428Cxx…)
                 └─ DrawGameScreen(1,1) 0x468CF0   [CORPUS] ← composes the whole frame
                      ├─ terrain/map      0x418310
                      ├─ build hot-unit list 0x48BAE0 → per-tile-row buckets
                      ├─ features + units  0x46A610 (feature) / DrawUnit 0x45AC20 (unit)
                      │      └─ DrawUnit → 0x458810 → 0x459200 → composite blit + z-merge
                      │             ├─ (cache miss) build sprite: 0x4586A0 → 0x459830
                      │             └─ (per-piece)  Render_DrawSpriteGroupUnit 0x4584D0
                      ├─ fog of war        0x420B00 / 0x49BE60
                      ├─ selection/health/UI 0x46A430, 0x46A530, 0x4AB170, 0x48CC30
                      ├─ HUD text + profiler bars  0x46B900 ×9, 0x45FFB0
                      └─ FlipOffscreenToPrimary 0x4C63A0   ← **Lock 0x4C650C / Unlock 0x4C659B**
```

`GameFrame_InGame 0x496790` is one of several frame callbacks the engine swaps
into `TA+0x391F5` depending on game state (menu, loading, in-game, replay). The
writer table is at `0x490B4x`/`0x496Axx` [VERIFIED-static]; the in-game value is
`0x496790`. This matches `UI_PIPELINE.md`'s "high-level frame callback stored at
`DAT_00511de8 + 0x391f5`" and `IdleTick`'s call of it at `0x4998xx`.

---

## 1. The frame compositor — `DrawGameScreen 0x468CF0`

Prologue `81 EC 14 02 00 00` (`sub esp,0x214`); epilogue `ret 0x8` at `0x46A3FD`.
Delphi signature [CORPUS]: `DrawGameScreen(DrawUnits: Integer; BlitScreen:
Integer) stdcall` — so `[esp+4]=drawUnits`, `[esp+8]=blitScreen`. Called as
`DrawGameScreen(1,1)` from `0x496790` [VERIFIED-static]; `blitScreen` gates the
final `0x4C63A0` present (`0x46A3CC..0x46A3DB`).

It builds a working `OFFSCREEN` context on the stack at `[esp+0x34]` (12 dwords,
copied from `TA+0x37E27` by `0x4C6B10`) and passes `&[esp+0x34]` as the draw
target to every phase. The phases fire in strict painter order; Cavedog's own
debug profiler labels them (strings decoded from `.data`) — those labels *are* the
frame's structure:

| Phase | Calls | What | Profiler label (`.data`) |
|---|---|---|---|
| view setup | `0x4C69A0`,`0x4C2470`,`0x4C6B10` | seed draw context, scroll origin from `TA+0x37E1B/1F/23` | — |
| **terrain/map** | `0x483FA0`, then **`0x418310`** | map tile blit; `0x418310` reads `eyeX/eyeY` (`TA+0x1431F/14323`) and walks the tile grid | (part of "Render Static") |
| scroll-edge fills | `0x4BE950` ×2 | letterbox borders when scrolled past map edge | — |
| **hot-unit binning** | `0x48BAE0` | rebuilds `HotUnits`/`NumHotUnits` (`TA+0x1435F/14367`) and bins each into per-tile-row buckets `[edi+…]` | — |
| **features + units** | row sweep `0x4697CF`..`0x469AF0` calling `0x4658E0`, **`0x46A610`** (feature), **`0x45AC20`** (unit) | the interleaved painter's-algorithm pass — see §2 | "Render Stuff" |
| **fog of war** | `0x49BE60` (see-projectiles), **`0x420B00`** | LOS/fog overlay over the drawn scene | "Render Fog" (`0x5077B8`) |
| minimap/radar | `0x48CC30`, `0x46A430` (health bars) | | — |
| selection & UI-world | `0x46A530` (select boxes), name/rank text `0x4C14F0` | drawn per hot-unit | — |
| screen HUD/GUI | `0x46A308` post-GUI hook pt [CORPUS], `0x45FFB0` (options), `0x464060` (chat), `0x46B900` ×9 | | "SFX/Weapon/Misc/Logic/Units/Network" bars |
| **present** | **`0x4C63A0`** at `0x46A3DB` | blit offscreen → primary, **Lock/Unlock heartbeat** | — |

`0x418310` [VERIFIED-static] is the terrain/features background: it clips the tile
range from `eyeX/eyeY` (`>>4` to tiles), walks `FeatureMap` (`TA+0x14287`) and
`MAPPED_MEMORY` (`TA+0x1426F`), and paints tiles into the offscreen. It runs
*before* the unit sweep, so terrain is the backdrop the unit z-buffer composites
against.

---

## 2. The per-unit draw path (the G5 detour target)

The row sweep in `DrawGameScreen` (`0x4699C0` and `0x469B6A`) iterates the
per-tile-row unit buckets and, for each alive unit whose state word
`UnitStruct+0x110 & 3 == 1` and `+0x9A != 0`, calls:

```
0045a9fe   push esi                 ; esi = UnitStruct*
0045a9ff   push eax                 ; eax = &offscreen ([esp+0x34])
0045aa00   call 0x45ac20            ; DrawUnit(OFFSCREEN* ctx, UnitStruct* unit)
```

### `DrawUnit 0x45AC20` — one unit, entry point [CORPUS: TADR `DrawUnit`]

Signature [CORPUS, byte-confirmed]: `DrawUnit(void* p_Offscreen /*[esp+4]*/,
UnitStruct* p_Unit /*[esp+8]*/) stdcall` (`ret 8`). **This is the cleanest single
per-unit hook site** — TADR's own Delphi (`plugins/NanoFrameUnits.pas`) calls it
directly with a synthesised `UnitStruct` to draw ghost units, which is strong
prior evidence it is cleanly re-entrant and detourable.

What it does [VERIFIED-static, decompiled]:
1. Skips if the unit is being transported (`UnitStruct+0x86 != 0`) — cargo is
   drawn by its carrier.
2. **Re-poses if moved:** compares the `Object3doStruct` (`unit+0x9E`) cached
   position words (`Object3do+0x18/0x1A/0x1C`) against the unit's live pos
   (`unit+0x64/0x66/0x68`); if any axis moved ≥8, marks the model dirty
   (`Object3do+0x08 = 1`, clears cached composite header `+0x26`).
3. Runs the **COB piece-transform bake** for the root and every attached unit:
   `0x45B0A0(Object3do, Object3do->BaseObject /*+0x1E*/, 0)` [VERIFIED-static],
   which recurses the `PrimitiveStruct` tree (`0x45B150`) applying each piece's
   `XPos/ZPos/YPos` + `XTurn/ZTurn/YTurn`. This is the COB→geometry bridge that
   turns animation state into posed vertices.
4. Calls **`0x458810(this=*(TA+0x1437B), Object3do, OFFSCREEN)`** — the draw.

Note the `this` = `*(TAdynmemStruct+0x1437B)` [VERIFIED-static] — a global
"current model layer / composite-buffer owner" object whose `+0x10` slot holds the
scratch composite `GAFFrame`. Every unit reuses it, one at a time; there is no
per-unit persistent sprite surface here (see §3 caveat).

### `0x458810` — dirty-check, then cache-blit-or-rasterise [VERIFIED-static]

```c
void FUN_00458810(this, Object3do* obj, OFFSCREEN* ctx) {
    dirty = (obj->drawCount==0) | (buildstate & ...);   // nanoframe/first-draw/etc
    if (dirty) { obj->cacheFrame /*+0x10*/ = 0; FUN_004586a0(this,obj,0,1); }  // rebuild
    if (obj->cacheFrame == 0) {                          // still no composite (e.g. under construction)
        for (piece in obj) if (piece.visible) FUN_004584d0();   // per-piece direct draw
    } else {
        FUN_00459200(this, ctx, obj, eyeX<<16, ?, eyeY<<16);    // composite path
    }
    obj->drawCount++;
}
```

Two live sinks fan out from here:
- **`0x459200`** — the normal path: blit the *cached composite sprite* + overlays
  onto the offscreen (see below). This is what draws a finished, moving unit.
- **`0x4584D0`** — `Render_DrawSpriteGroupUnit` [CORPUS], the per-primitive
  sprite-group draw used when there is no composite cache (build/nanoframe). Per
  vertex it computes `sx = x>>16`, `sy = (-z - y/2)>>16` [VERIFIED-static, the
  sprite-space oblique iso], looks up each face's GAF frame
  (`GAF_SequenceIndex2Frame 0x4B7F30` / `GAFGetCurrentFramePtr`), and stamps it
  with `GAF_DrawTransformed 0x4C7580`.

### `0x459200` — composite sprite → screen, with z-merge [VERIFIED-static]

Arguments: `(this, OFFSCREEN* ctx, Object3do* obj, eyeX<<16, _, eyeY<<16)`.
Screen projection (matches the field-notes world→screen formula, at instruction
level):

```
worldX = obj->unit->pos.x  (unit+0x6A)      screenX = (worldX - eyeX)>>16 + 0x80   (= +128)
worldZ = obj->unit->pos.z  (unit+0x72)      screenY = (worldZ - eyeY)>>16
worldY = obj->unit->pos.y  (unit+0x6E)                 - (GetPosHeight(unit)>>1) + 0x20  (= -alt/2 +32)
```

Then, depending on cloak/shadow/water flags (`TA+0x37F06`, `UnitType+0x241`
bitfield, `unit+0x110`/`+0x113`), it blits the cached composite frame
(`obj+0x10`) via one of:
- **`CopyGafToContext 0x4B7F90`** [CORPUS] — the plain color-keyed, clipped 8bpp
  sprite blit (the normal "stamp this unit's sprite onto the frame"). *This is the
  lowest-level per-unit pixel writer for opaque units.*
- **`AlphaCompsteBuf2OFFScreen 0x4B8500`** [CORPUS] — the shaded/alpha variant for
  cloaked units, shadows, and underwater tint (uses the `SHADE/ALPHA` tables).

After the body, it z-merges **attached/transported units** into the same
composite: it walks `unit+0x8A` (attached-unit chain), rebuilds each child's
sprite (`0x4586A0`), and merges it with **`0x4B90A0`** [VERIFIED-static] — a
**per-pixel 8-bit-depth-tested blit**: for each pixel it writes color+depth only
when `child_depth <= existing_depth + offset`. That confirms Mavor's account:
**there is a per-unit 8-bit z-buffer** (the composite's second plane), and draw
order within a unit's cargo stack is resolved by depth, not paint order. The
inter-unit ordering is the painter's algorithm (tile-row bucket sweep, back to
front); the intra-unit ordering is the 8-bit z-buffer.

**Implication for G5 suppression:** skipping `DrawUnit 0x45AC20` for one unit type
removes it from the offscreen with no effect on the tile buckets, the sim, or
other units (each unit's composite is independent; the shared scratch buffer at
`this+0x10` is rebuilt per unit). A unit made invisible this way stays in
`HotUnits`, stays selectable (selection boxes are a *separate* pass, `0x46A530`,
also driven off the bucket list), and stays simulated. **Per-unit blits look
cleanly isolatable** — see the confidence table.

---

## 3. The 3DO→sprite-cache rasteriser (the G6 substitution target)

When a unit's model is dirty, `0x458810` calls **`0x4586A0`** [VERIFIED-static]
— the sprite-cache *builder*:

1. Computes the model's screen-space AABB across all posed pieces via
   **`0x4581E0`** [VERIFIED-static] (walks every visible `PrimitiveStruct`,
   projecting each vertex with the same `sx=x, sy=-z-y/2` oblique iso, taking
   min/max). Output = composite width/height + hotspot.
2. Allocates the composite `GAFFrame` buffer:
   - **`0x437B50`** — colour-only buffer, `w*h + 0x18` bytes, pixels pre-filled
     with `0x01` (the background/transparent index). [VERIFIED-static]
   - **`0x437BE0`** — colour **+ depth** buffer, `w*h*2 + 0x18` bytes: a colour
     plane (filled `0x01`) followed by a depth plane (filled `0x00` = far). This
     is the per-unit z-buffer allocation. [VERIFIED-static]
   The `GAFFrame` header is 0x18 bytes: `Width/Height/HotX/HotY` (words),
   `ColorKey/Compressed/SubFrames/AlphaBlend` (bytes), then pixel ptr at `+0x10`
   and depth ptr at `+0x14` [CORPUS: TADR `buildghost.h` `GhostGAFFrame`, matches
   our bytes]. **TADR patches this allocation's fixed 600×600 dimension at
   `0x458195`** [CORPUS] — confirming the stock composite buffer is 600×600.
3. Rasterises the posed pieces into that buffer via **`0x459830`** (opaque) or
   **`0x459C70`** (build/nanoframe with shadow). [VERIFIED-static] `0x459830`
   walks each `PrimitiveStruct`, projects vertices (`kNanoframeStart 0x458DF1`
   region [CORPUS]), and for each face looks up its GAF frame and stamps it with
   **`GAF_DrawTransformed 0x4C7580`** [CORPUS] into the composite pixel+depth
   planes — writing depth per pixel so the intra-model z-buffer is populated here.

So the **cache is per-`Object3doStruct`, single-frame, rebuilt on demand** (on
move/animation/pose change) — it is *not* the "pre-rendered one sprite per
orientation" cache that folklore describes. Stock TA re-rasterises the whole 3DO
into a fresh composite whenever the unit moves ≥8 units or animates. The cache
frame lives at `Object3do+0x10` and is consumed the same frame by `0x459200`.
[VERIFIED-static — this refines the Mavor "cached sprites per orientation" claim:
the *composite* is cached, but it is regenerated continuously, not baked per
heading.]

Structures involved [CORPUS: TADR `tamem.h`, Ghidra-verified sizes]:
- `UnitStruct` (0x118): `+0x6A/0x6E/0x72` pos (int), `+0x9E` → `Object3doStruct*`,
  `+0x8A` attached-unit chain, `+0x92` → `UnitDefStruct`, `+0x110` state mask.
- `Object3doStruct` (0x22): `+0x00 NumParts`, `+0x0C ThisUnit*`, `+0x10` composite
  `GAFFrame*` (the cache slot — but note the live path uses the shared scratch at
  `*(TA+0x1437B)+0x10`, see caveat), `+0x1E BaseObject` (`PrimitiveStruct*` root).
- `PrimitiveStruct` (0x36): `+0x00 PrimitiveInfo*`, `+0x04 XPos`, `+0x08 ZPos`,
  `+0x0C YPos`, `+0x10 XTurn`, `+0x12 ZTurn`, `+0x14 YTurn`, `+0x22..` sibling/
  child/parent tree links.
- `Model3DONode` (0x40) / `Model3DOFace` (0x20): the static 3DO tree reached via
  `TA->MODEL_PTRS` (`TA+0x14377`), indexed by unit type; 16.16 fixed-point verts.

**Caveat to confirm at G4:** two composite slots exist — `Object3do+0x10` and the
shared `*(TA+0x1437B)+0x10`. `0x458810` passes `this=*(TA+0x1437B)`, and `0x459200`
reads the composite from `this+0x10`, i.e. the *shared* scratch — but `0x4586A0`'s
rebuild also touches `Object3do+0x10` (`obj->cacheFrame`). The exact split (which
buffer is persistent per unit vs. per-frame scratch) needs a live read of both
pointers across two frames. This matters for G6: if the cache is genuinely
per-unit-persistent we can substitute lazily; if it is per-frame scratch we must
substitute every frame.

---

## 4. Z / order handling — what breaks if a unit is skipped

- **Inter-object order = painter's algorithm** over per-tile-row buckets. The
  bucket build (`0x48BAE0` + the binning loop at `0x4697CF`) sorts hot units and
  features into rows keyed by screen-Y tile; the sweep draws back rows first. A
  skipped unit leaves a hole (terrain shows through) but does not disturb any
  other unit's ordering — the buckets are rebuilt from `HotUnits` each frame.
  [VERIFIED-static]
- **Intra-object order = 8-bit z-buffer.** Within a unit and its cargo, `0x4B90A0`
  depth-tests each pixel against the composite depth plane. This is populated
  during rasterisation (`0x459830`/`GAF_DrawTransformed`). [VERIFIED-static]
- **Nothing reads back the frame into sim state.** The offscreen is write-only
  from the sim's perspective; `0x4C63A0` only *copies it out* to the primary. No
  gameplay code samples drawn pixels. So suppressing/replacing a unit's blit is
  observationally pure w.r.t. the simulation — the MP-safety requirement is met by
  construction, provided the hook does not perturb the bucket build or the pose
  bake (both run *before* the blit, inside `DrawUnit`). [INFERRED — verify at G4
  with a replay byte-diff.]

---

## 5. The present / flip — `FlipOffscreenToPrimary 0x4C63A0`

Prologue `81 EC F4 00 00 00` at `0x4C63A0`; the Lock/Unlock body:

```
004c6503   mov ecx,[eax]            ; eax = primary surface (esi+0x88), ecx = its vtable
004c6508   push eax
004c6509   call [ecx+0x64]          ; IDirectDrawSurface::Lock   ← returns to 0x4C650C  ★HEARTBEAT★
004c650c   test eax,eax
004c650e   jne  0x4c65a0            ; if Lock failed (DDERR_SURFACELOST), restore path
...
004c6553   call 0x4cbbe0            ; CopyScreenContext: byte-copy game offscreen → locked primary
...
004c658a   mov  esi,[esi+0x88]      ; primary surface
004c6593   mov  edx,[esi]
004c6595   call [edx+0x80]          ; IDirectDrawSurface::Unlock ← call site 0x4C659B  ★HEARTBEAT★
```

`0x4CBBE0` [VERIFIED-static] is the raw 8bpp clipped rect copy (source
`[iVar6+0x98]` = the game's rendered offscreen, dest = the locked primary). So the
`Lock`→`CopyScreenContext`→`Unlock` sequence is purely "present the finished
frame". This is exactly what our cnc-ddraw hook observes, and it explains why the
Lock fires once per frame after all drawing: **it is the upload, not the render.**

`0x4C63A0` is also called from ~40 other sites (menus, dialogs, loading screens) —
it is the generic "flip current offscreen to screen" routine, not unique to the
game view. Confirming "this Lock is the in-game frame" at runtime means checking
the caller return address is `0x46A3E0` (inside `DrawGameScreen`), which our
ddraw-side hook already can (it records the return address).

---

## Candidate hook sites (for G4 freeze / G5 detour)

| Address | Role | Args at entry | Confidence | Notes |
|---|---|---|---|---|
| **`0x45AC20`** `DrawUnit` | **Primary per-unit hook** — draw one unit | `[esp+4]`=OFFSCREEN\*, `[esp+8]`=UnitStruct\* (stdcall, ret 8) | **HIGH** [VERIFIED-static + CORPUS] | Skipping it hides exactly one unit; sim/selection untouched. TADR calls it directly ⇒ re-entrant. **Best G5 detour + G4 tracer site.** |
| `0x469A00` / `0x469BA3` | The two `call DrawUnit` sites inside the compositor's unit sweep | `esi`=UnitStruct\*, `eax`=&OFFSCREEN | **HIGH** [VERIFIED-static] | Hook here instead of the entry if you want the *loop context* (bucket row, filter to specific units cheaply). |
| `0x458810` | Per-unit cache-blit-or-rasterise dispatch | `ecx(this)`=`*(TA+0x1437B)`, `[esp+4]`=Object3do\*, `[esp+8]`=OFFSCREEN\* | **HIGH** [VERIFIED-static] | One level below DrawUnit, after the pose bake. Hook to intercept *drawing* but keep TA's re-pose. |
| `0x459200` | Composite sprite → screen + z-merge | `ecx(this)`, `[esp+4]`=OFFSCREEN\*, `[esp+8]`=Object3do\*, `[esp+0xC]`=eyeX<<16, `[esp+0x14]`=eyeY<<16 | **HIGH** [VERIFIED-static] | The actual screen-space blit; args carry the projected origin. Good G4 confirmation point ("unit N at (sx,sy)"). |
| **`0x4B7F90`** `CopyGafToContext` | Low-level color-keyed 8bpp sprite blit | `[esp+4]`=OFFSCREEN\*, `[esp+8]`=GAFFrame\*, `[esp+0xC]`=x, `[esp+0x10]`=y (stdcall) | **HIGH** [CORPUS + VERIFIED-static] | Shared by units, features, HUD — too broad to *suppress* per-unit, but the definitive "a sprite lands here" fence for G4 tracing. |
| `0x4B8500` `AlphaCompsteBuf2OFFScreen` | Shaded/alpha sprite blit (cloak/shadow/water) | same shape as `0x4B7F90` | **MED** [CORPUS] | The other pixel sink; hook alongside `0x4B7F90` for full blit coverage. |
| **`0x4586A0`** | Sprite-cache **builder** (rasterise 3DO→composite) | `ecx(this)`, `[esp+4]`=Object3do\*, `[esp+8]`=flag, `[esp+0xC]`=mode | **HIGH** [VERIFIED-static] | **G6 substitution target.** Detour to write our GPU-rendered sprite into the composite instead of `0x459830`. |
| `0x459830` / `0x459C70` | Rasterise posed pieces into composite (opaque / shadowed) | stack frame (large; see decomp) | **MED** [VERIFIED-static] | The inner rasteriser `0x4586A0` calls; alternative G6 hook if we keep TA's buffer alloc. |
| `0x4B90A0` | Per-pixel 8-bit z-tested composite merge | `[esp+4]`=srcFrame, `[esp+8]`=dstFrame, x,y,depthBias | **MED** [VERIFIED-static] | Where the intra-unit z-buffer is enforced; instrument to dump depth for G4. |
| `0x468CF0` `DrawGameScreen` | Whole-frame compositor entry | `[esp+4]`=drawUnits, `[esp+8]`=blitScreen | **HIGH** [CORPUS] | Frame fence; wrap to bracket "all unit draws for frame N" and to gate our overlay pass. |
| `0x4C650C` / `0x4C659B` | ddraw Lock/Unlock (present) | — (return-address landmarks) | **HIGH** [VERIFIED live, this build] | The heartbeat. Use as "frame done"; **not** a unit hook. |
| `0x4C7580` `GAF_DrawTransformed` | Textured-face stamp used during rasterise | (per TADR) | **MED** [CORPUS] | Deepest sprite primitive; G6 could replace this to inject per-face GPU output. |

Recommended G4/G5 pair: **trace at `0x459200`** (gives unit ptr + projected sx/sy
+ sprite ptr in one place) and **detour at `0x45AC20`** (clean per-unit on/off).
Recommended G6 target: **`0x4586A0`** (own the sprite-cache build).

---

## Biggest open questions for runtime confirmation (G4)

1. **Which composite buffer is persistent?** `Object3do+0x10` vs. shared
   `*(TA+0x1437B)+0x10`. Read both pointers for one unit across two frames; decide
   whether the sprite cache is per-unit (lazy G6 substitution possible) or
   per-frame scratch (must substitute every frame). *This is the single most
   decisive fact for the G5-vs-G6 pivot.*
2. **`DrawUnit` call cadence.** Confirm `0x45AC20` fires exactly once per visible
   unit per frame (not per LOS-player, not twice for shadow+body). The compositor
   has *two* `DrawUnit` sites (`0x469A00`, `0x469BA3`) — verify which one fires for
   a normal in-view unit and whether both can fire for the same unit.
3. **Detour purity.** Suppress `0x45AC20` for one unit type, then diff a replay /
   savegame byte-for-byte to prove the pose bake (`0x45B0A0`) and bucket build are
   untouched by skipping the *blit*. If the pose bake must keep running, hook
   `0x458810` (after the bake) rather than `0x45AC20`.
4. **Shadow pass ownership.** Is the ground shadow drawn inside `0x459200`/
   `0x459C70` (per-unit, so it disappears with the unit) or in a separate map pass?
   Determines whether a suppressed unit leaves a floating shadow.
5. **Heartbeat attribution.** Confirm the in-game Lock at `0x4C650C` always has
   caller return `0x46A3E0` (inside `DrawGameScreen`) vs. menu/dialog flips, so our
   ddraw hook can tell "game frame" from "UI frame".
6. **Depth-plane semantics.** Confirm `0x4B90A0`'s depth convention (0=far vs.
   255=far) and the bias term, so our GPU depth maps into the same 8-bit space for
   G6 write-back.

## Pivot assessment (per the G4 rule)

**Per-unit blits look isolatable.** `DrawUnit 0x45AC20` is a per-unit,
per-frame, self-contained call with the offscreen and unit pointer right at entry,
already proven re-entrant by TADR's own use of it, and downstream of both the pose
bake and the bucket sort — so suppressing or replacing one unit's screen output
does not disturb ordering, selection, or the sim. **G5 (suppress one unit type) is
very likely achievable as a direct `0x45AC20` detour.**

The sprite-cache substitution path (G6) is *also* cleanly targetable at `0x4586A0`
and does not depend on G5 failing. Given that stock TA **re-rasterises the whole
3DO into a fresh composite whenever the unit moves/animates** (not a static
per-orientation bake), G6 is attractive regardless: it means our GPU renderer
would be invoked on the same "model dirty" trigger TA already uses, and we write an
8bpp+depth composite into the buffer TA then blits and z-merges for free. The one
gating unknown is open question #1 (buffer persistence). Recommendation: pursue G5
via `0x45AC20` first (fast, low-risk proof), and stand up G6 at `0x4586A0` in
parallel since it is the permanent Phase-B mechanism.

## Runtime confirmation (G4) — DONE (2026-08-31)

An in-process tracer (`tagpu/ddraw/src/tagpu_tracer.c`, armed by a `tagpu_tracer.on`
trigger file) detoured the two candidate sites with 5-byte `E9` jumps over verified,
relocatable prologues (byte-match guarded, exactly like `tagpu_patches.c`), captured
events into a lock-free ring, and flushed from the overlay's per-frame callback. Run:
live skirmish, ARM vs CORE, Canal Crossing. Evidence: `evidence/g4-tracer.txt`.

**Both hypothesised hooks fire, and the G3 chain is confirmed on live memory:**

| Site | Confirmed behaviour | Evidence |
|---|---|---|
| **`DrawUnit 0x45AC20`** | Called per on-screen unit per frame. All calls arrive via **call site A `0x469A00`** (return `0x469A05`); site B `0x469BA3` **never fired** → no separate shadow/second pass in this config. Args at entry `[esp+4]`=offscreen, `[esp+8]`=UnitStruct as predicted. | `TR_EV … site=DU ret=00469A05 unit=06440138`; `DU=3250(A@469A05=3250 B@469BA8=0 other=0)` |
| **blit `0x459200`** | Fires **1:1 with DrawUnit** (`DU==BL` every window), reached through `0x458810` (return **`0x45894D`**), carrying the unit's **Object3do** pointer and a **projected screen position** (e.g. `sx=256 sy=197`). Confirms `DrawUnit → 0x458810 → 0x459200`. | `TR_EV … site=BL ret=0045894D unit=06440138 arg0=0534DEB8 sx=256 sy=197` |

**Frustum/LOS gating (new finding).** DrawUnit fires **only for units the engine
actually rasterises** — on-screen *and* in line-of-sight. With the ARM Commander in
view: `DU/BL ≈ 3250 per window, distinct=1, umin=umax=06440138` (the one on-screen
unit). Scroll it off, or point the camera at the fogged CORE base, and `DU=BL=0`
even though our overlay still marks those units (it reads sim memory, so it draws
through fog; the engine does not). Consequence for G5/G6: a render detour only ever
sees units the player can see — fogged units are already suppressed by the engine.

**Open question #1 resolved — composite buffer is per-unit-persistent.** The tracer
sampled both candidates each frame. `compA = Object3do+0x10` held a stable heap
pointer (`0x05890028`) tied to the commander's obj3do (`0x0534DEB8`, = the blit's
`arg0`), changing only when the unit animated and otherwise reported `same` — i.e.
a **persistent per-unit composite**, not a per-frame scratch. (`compB`, the shared
`*(main+0x1437B)+0x10`, was also stable.) This is the buffer TA blits and z-merges,
so it is the **G6 write-back target**, and G6 can be lazy (write on the engine's own
"model dirty" trigger) rather than every frame.

**Frozen hook list.**
- **G5 suppression / replacement:** detour **`DrawUnit 0x45AC20`** (single site, `0x469A00`).
- **G4 pixel-fence / projected-position source:** **`0x459200`** (via `0x458810`, ret `0x45894D`).
- **G6 sprite-cache write-back:** **`Object3do+0x10`** composite buffer; builder **`0x4586A0`**.

## Suppression (G5) — DONE observationally (2026-08-31)

A detour on `DrawUnit 0x45AC20` (`tagpu/ddraw/src/tagpu_suppress.c`, armed by a
`tagpu_suppress.on` file naming the type, default `armcom`) makes one **unit type**
render-invisible while leaving the simulation untouched. Evidence:
`evidence/g5-suppress.txt`.

**Calling convention (needed to unwind cleanly).** `DrawUnit` is **stdcall / callee-
cleans-8**: its epilogue is `ret 0x8` at `0x45AE7D`, and neither live caller does
`add esp,8` after the `call` (`0x469A00`: next insn `mov ecx,[edi+8]`; `0x469BA3`:
`mov ecx,[esp+0x20]`). Entry stack: `[esp]=ret, [esp+4]=offscreen, [esp+8]=UnitStruct`.
The suppressed path is `pushad → classify(unit) → popad → ret 8` — byte-identical
unwind to DrawUnit's own return, callee-saved registers preserved, **nothing written**
to the offscreen buffer or any unit field. The non-suppressed path is a transparent
passthrough (stolen 7 bytes then `jmp 0x45AC27`), so other units draw normally.

**Type match.** `UnitStruct+0x92` → `UnitDefStruct*`; the configured token is matched
case-insensitively against the def's `Name@0x00` / `UnitName@0x20` / `ObjectName@0x80`
inline strings, so `armcom` selects exactly the ARM Commander regardless of which
field community tooling calls "the name". Classification is read-only.

**Live result.** With the ARM Commander centred and suppressed: `SUPP hits` climbed to
**>277k** across the run (`passed=0` — the only in-LOS on-screen unit was the
suppressed commander). Its **sprite is gone** (bare terrain where G4 showed the full
commander at the same spot), while the map, foliage and features render normally and
our memory-read overlay still marks it — proving the suppression is *selective* and
*render-only*. The `probe_unit_model` dump confirms the suppressed commander is still a
**complete live model** (15 pieces, correct names, visibility flags) with authoritative
`pos/turn` sim state intact. (Side effect worth noting for G6: because the engine
reposes lazily *inside* the draw it now skips, the suppressed unit's derived
`vbuf`/`org` stop refreshing — the authoritative `pos`/`turn` do not. A GPU renderer
replacing the draw must drive its own pose from `pos`/`turn`, or trigger the repose.)

**Remaining formal sign-off.** Replay/savegame byte-diff (with vs without the detour)
is the gold-standard sim-integrity check; it needs interactive input the locked headless
session can't supply (mouse clicks don't land), same constraint noted at G2. The
observable proof above (live model + intact sim state + no crash + everything-else-
renders) stands in until an unlocked session can run the byte-diff.

**Coexistence.** When `tagpu_suppress.on` is present the suppressor owns `0x45AC20`
and the tracer skips its own DrawUnit hook (both read the same trigger file); the
tracer's `0x459200` blit hook still coexists.

## Corrections (Phase C terrain-depth RE, 2026-08-31)

The deep terrain/feature RE ([terrain & depth](terrain-depth.html)) corrected three labels in
this note's frame map — the chain and hook table above remain valid:

- **`0x418310` is the map DEBUG overlay, not the terrain draw.** Real terrain =
  **`0x483FA0`** (stdcall(ctx), flat 32×32 tile blit, colour-only, no height/LOS).
- **`0x420B00`/`0x49BE60` are the explosion/particle and projectile passes, not fog.** Real
  fog-of-war = **`0x4848E0`** (call site `0x469D8E`, after health bars; per-cell decision,
  per-pixel LUT/checkerboard application).
- **DrawUnit call site B `0x469BA3` is the AIRBORNE-unit sweep** (second pass after
  projectiles, `(state&3)!=1`), not a shadow pass — it never fired in G4 because no aircraft
  were on screen. Ground sweep ordering: per 16-px map row, units first, then tall features
  (def Height≥10); flat features draw under everything in a pre-pass.
- **There is no destination depth plane.** `0x4B90A0` merges composite↔composite only (cargo);
  every sprite→screen path is colour-only — scene ordering is purely the painter's row sweep.

## What is left of this frame map (2026-09-02, after G13b)

Everything in the world half of the chain above is now drawn by our GL passes, and the
engine's own calls for it are detoured away: units and wrecks (`owndraw`), weapon fire,
explosions and particles (`fxown`), features (`featown`), and — as of G13b — **the terrain
blit `0x483FA0` and the fog overlay `0x4848E0`** (`terrown`). `DrawGameScreen` still runs
in full; what it now produces inside the viewport rect is a **flat fill of one palette
index** (the composite key), with nothing on it but the overlays we have yet to take:
health bars at `~0x469BD7`, nanoframe wireframes, the build-cursor rect, chat and dialogs.

That inverts the compositing rule this note's Lock/Unlock story implies. The engine's
finished offscreen is no longer the picture our overlay decorates — it is a **mask**
telling the composite which pixels the engine still owns. Detail: [terrain &
depth](terrain-depth.html) §7. The Lock/Unlock heartbeat is unchanged and still the
"frame is done" fence.
