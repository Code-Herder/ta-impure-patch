# Effects — weapon fire, explosions, debris (the effects pass)

*2026-09-02. Reverse-engineered from the pristine exe (Ghidra decompiles of `0x49BE60`,
`0x420B00` and their leaves; `tools/ghidra-scripts/DecompileTAFuncs.java`), verified live
in a skirmish through `tagpu_fx.c`'s gather log, then owned end-to-end (`tagpu_fxown.c`).
Struct names follow TADR's `tamem.h`; every offset below was read back from a running
game. §7 (same day, second session) adds the particle layers — smoke, fire, wakes,
nanolathe — which are not drawn by the two effects passes at all.*

## Summary — the five things to know

1. **Effects are two colour-only passes between the unit sweeps.** `DrawGameScreen`
   draws terrain → per-row ground units + tall features → **projectiles `0x49BE60`**
   (call site `0x469B22`) → **explosions/sfx `0x420B00`** (`0x469B2C`) → the airborne
   unit sweep → UI. Nothing occludes an effect except an aircraft; an effect never
   occludes a ground unit. Both passes are `stdcall(ctx)`, `ret 4`.
2. **A projectile is drawn by its weapon's `RenderType`** (`WeaponStruct+0x10C`, table
   below): a line, a rotated 3DO, a sprite from one of five fixed sequences, a fading
   flare, a lightning bolt — or a background-refraction "ball" nobody has reproduced.
   Each is gated by the local player's LOS at its anchor tile, and the model kinds get
   a translucent ground-shadow blob first.
3. **An explosion record carries up to three things**: a 3DO body (`+0x00`, only on the
   explosions a dying unit's piece breaks into) rotated by `+0x4C`, an opaque sprite animation (anim state `+0x04`) and a *light flash* (anim
   state `+0x10`) blitted through the **LHT** lighten table — the big white disc around
   a big explosion. Flying debris pieces are separate objects in the particle slots
   `0x511DF0..0x511F80`, drawn by `0x4211D0`.
4. **GAF frames come in three flavours** the blits all understand: raw, TA-RLE
   (`Compressed=1`), and a *sub-frame list* (`+0x0A` count, `+0x10` → pointer array,
   each sub-frame's `+0x0B` picking plain or alpha). Sequence → frame is
   `*(seq + 0x28 + idx*8)`; an anim state is `{u16 frame @0, seq* @8}`.
5. **Everything is read-only over the sim.** The projectile pass's only side effect is
   the CRT `rand()` jitter of lightning (not the sim RNG, which would already desync
   multiplayer if the render rate touched it); the explosion pass *updates* the debris
   particle systems (smoke/fire emission), so owning the draw means skipping its leaves,
   not the pass.
6. **Smoke, fire, wakes and the nanolathe spray are a third mechanism** (§7): pooled
   particle objects filed into ten *layer* vectors, each layer drawn by `0x471F90(ctx, n)`
   at a fixed point of `DrawGameScreen` — the layer number is the draw depth — and
   updated by the sim tick. One detour on that walker owns all of them.

## 1. The projectile pass `0x49BE60(ctx)` [BINARY-VERIFIED, LIVE-VERIFIED]

Array: count `main+0x141F3`, base `*(main+0x141F7)`, stride `0x6B` (`ProjectileStruct`).

| Offset | Field | Used for |
|---|---|---|
| `+0x00` | `WeaponStruct*` | render type, colours, model, lifetime, type mask |
| `+0x04/+0x08/+0x0C` | i32 16.16 x / altitude / map-depth (head) | anchor: `sx = hi(x) − eyeX + vpL`, `sy = hi(y) − (hi(alt)>>1) − eyeY + vpT` |
| `+0x10/+0x14/+0x18` | i32 16.16 start (tail) position | lasers and lightning run tail→head |
| `+0x34..+0x39` | short[3] rotation triple | model orientation (see §4) |
| `+0x42` | int spawn tick | sprite frame `(tick − spawn) % n` |
| `+0x46` | int death tick | flare fade, missile thrust-flame lifetime |
| `+0x52` | attacker `UnitStruct*` | team colour of model textures (`+0xFF` owner) |
| `+0x5E` | u16 terrain height under the projectile | ground-shadow blob y |
| `+0x60` | short | **draw only when 0** |
| `+0x64` | short spin angle | spinning missile flame (mask bit 21) |

Tick = `*(main+0x38A47)`. Weapon: name `+0x00`, 3DO root `+0x74`, lifetime u16 `+0xE6`,
`RenderType` i8 `+0x10C`, `color` `+0x10D`, `color2` `+0x10E`, `WeaponTypeMask` `+0x111`.
Line colours go through the byte table `main+0xDCB[color]` → palette index.

**LOS gate** (per projectile, local player `main+0x2A43`): with true LOS
(`LosType & 2`) the LOS counter byte at tile `(hi(x)>>5, (hi(y) − hi(alt)/2)>>5)` must
be non-zero; otherwise `PositionInPlayerMapped 0x408090` = the MAPPED bit. Explosions
are **not** gated (only anchor-in-viewport-rect), the later fog overlay darkens them.
(**Since G13b there is no later fog overlay** — `terrown` suppresses `0x4848E0` and our
own passes apply the rule per fragment. Effects *hide* in grey rather than darken, as each
is gated by LOS at its own point; units are greyed like terrain: features.md §9.)

| `RenderType` | Stock example | Engine draw |
|---|---|---|
| 0 | `CORE_LASER`, `ARMCOMLASER` | `DrawLine` tail→head in `coltab[color]`; when `color2 != 0` a second line in `coltab[color2]` is drawn *first*, offset one pixel (y−1 for shallow lines; x−1 at the top end / x+1 at the bottom end for steep ones) |
| 1 | `KBOT_ROCKET`, `ARMKBOT_MISSILE` | shadow blob; root 3DO rotated by `(t0, t1−0x8000, t2−0x8000)`; the root's **child** node (thrust flame) while `tick < death`, spinning by `+0x64` as `t0` when `WeaponTypeMask` bit 21 |
| 2 | (rare) | `0x4B9360`: grabs the background under `*(main+0x1AB9B)` and re-emits it through a per-pixel offset table — a refraction "ball". **Aborts the whole pass** if the anchor is outside the viewport (engine bug). Not reproduced natively (counted as `ball=`) |
| 3 | — | shadow blob; root 3DO with an *uninitialised* stack triple (effectively unrotated) |
| 4 | `EMG` (color 2) | shadow blob (unless `color == 0xFF`); sequence `*(main+0x147BB + 4·color)` for `color 0..4`; frame `(tick − spawn) % n`; `CopyGafToContext` (opaque) |
| 5 | flares | `*(main+0x147F3)`, frame `n − ((death − tick)·n)/lifetime`, alpha blit |
| 6 | — | shadow blob; root 3DO rotated by the raw triple |
| 7 | lightning | two jagged polylines tail→head in `n = floor(len/5)` steps of `len/n`, each vertex jittered ±5 world units per axis by CRT `rand()` |

Shadow blob = `AlphaCompsteBuf2OFFScreen` of frame 0 of `*(main+0x1480F)` at
`(hi(x) − eyeX + vpL, hi(y) − eyeY − (groundH>>1) + vpT)`.

## 2. The explosion pass `0x420B00(ctx)` [BINARY-VERIFIED, LIVE-VERIFIED]

1. **Particle slots** `0x511DF0..0x511F80` (100 dwords): each live slot → `0x421550(ctx,
   sys)` which emits the piece's smoke/fire (`EmitSfx_GraySmoke` / `EmitSfx_Unk5` by
   `sys+0x28` bits 1/0 — these become objects in the plugin-layer hook vectors
   `*(main+0x38D77)`, drawn by `0x471F90(ctx, n)` at the hook sites, **not by this
   pass**), rotates the piece's verts (`0x4B6CC0`) and draws it via `0x4211D0`. The
   piece is `*(sys+0x2C)`: `{Model3DONode* @0, the turn words @0x10/0x12/0x14 — handed to
   0x4B6CC0 reversed, as {w14, w12, w10} — i32 16.16 x/alt/y @0x16/0x1A/0x1E, the rotated
   vertices @0x22}`.
2. **Flash loop** over `ExplosionStruct[]` (count `main+0x1491B`, inline at
   `main+0x1491F`, stride `0x54`, 300 slots): anchor `+0x1C/+0x20/+0x24` (16.16 x/alt/y)
   inside the viewport rect `main+0x37E27`, and `+0x18 != 0` → frame of anim state
   `+0x10` → `0x4B8EC0`: **LHT blit**, `dst = LHT[(src − 0x4F)·256 + dst]`, gated by
   `TAProgram+0xF0` bit 7. The flash sequence is a short-lived object: its memory is
   freed when the explosion ends (a later read of a dead record's `+0x18` finds
   unreadable frames — read it only while `i < count`).
3. **Body loop**: per record inside the viewport rect, `+0x00` → `0x46BAE0` rotated by
   `+0x4C` when it is non-zero. The ordinary add site `0x420A30` writes it 0; the explosions
   `0x421700` makes from each face of a dying unit's piece carry a **piece-explosion
   record** — node-shaped, 8 vertices and 6 faces whose index pointers are the executable's
   own table `0x502BF8`, out of a pool (stock's 300 at `main+0x1AB9F`, laid out by
   `0x420620` at the level load), rewritten per explosion and freed by the tick writing
   `0xFF` over its first byte. It is not a template, so only the thread that draws it can say
   what it holds ([engine map](exe-reverse-engineering.html), *The effects models*); then
   anim state `+0x04` (`seq @+0x0C`) → `CopyGafToContext` (opaque, colour-keyed).
   Explosion sequences seen live: `Explode2/3/5`, `Explosion` (19–23 frames, frame 0 as
   small as 4×3, RLE).

## 3. The blit leaves [BINARY-VERIFIED]

| Function | Rule |
|---|---|
| `CopyGafToContext 0x4B7F90` | colour-keyed copy (`ColorKey @+8`); sub-frame lists recurse, a sub-frame with `+0x0B != 0` goes through the alpha blit instead. `ret 0x10` |
| `AlphaCompsteBuf2OFFScreen 0x4B8500` | `dst = ALP[src·256 + dst]` (`TAProgram+0xC0`, `palettes\PALETTE.ALP`: on retail the palette entry nearest the two colours' mean, a 50 % blend quantised to the palette, §7.6), on the raw and the RLE arm alike; gated by `+0xF0` bit 5 (clear: nothing drawn) |
| `0x4B8EC0` | LHT lighten as above, gated by bit 7. `ret 0x10` |
| `0x46BAE0(ctx, pos16.16[3], node, turn[3])` | `pos` eye-relative (the callers take `eye << 16` off x and y). Rotate `node+0x24` verts by the triple, project `sx = (int16)hi(vx+px) + 0x80`, `sy = (int16)hi(py − vz) − ((int16)hi(vy+palt) >> 1) + 0x20`; faces `node+0x28` (stride 0x20): skip face 0 when `node+0x0C` (selection primitive) `!= −1`; indices unbounded; flag bit 0 → flat fill `0x4C0310` in the colour word's low byte, any vertex count; else **only 4-vertex faces** → `GAF_DrawTransformed 0x4C7580` with `uv = NULL` (texel copy, **no shade table**; bit 1 → current anim frame of `face+0x10` through `0x4B7EE0`, unbounded). `ret 0x10` |
| `0x4211D0` | the same face rules for a debris piece, drawn only when its anchor is in the viewport rect, from the vertices `0x421550` rotated into `piece+0x22`; with bits 1 **and** 2 the frame is `0x4B7F30(face+0x18, logo)`, the logo colour of the unit the piece flew off (`sys+0 → +0x96 → +0x27 → +0x96`). `ret 0xC` |
| `0x4B6CC0(in, out, turn)` | `Rz(t0)` on (x,y), then `Rx(t2)` on (y,z), then `Ry(t1)` on (x,z); each `0x4B7173`: `a' = a·cos − b·sin, b' = a·sin + b·cos`, angle = `t·2π/65536` |
| `DrawLine 0x4BE950` | clip to the context rect (`CorrecLinetPosition`), Bresenham in the palette index |
| `0x4C7580` / `0x4C7310` / `0x4C0330` | the textured quad, its span and the flat fill: two edge chains walked in 16.16 from a vertex biased by `0xFFFF`, steps by truncating division, rows appended per side and read back by position, one texel per pixel by the frame width's own addressing, the frame's key colour copied like any other texel; clipped to the context's `+0x1C` rect, which `DrawGameScreen` sets to the viewport rect `main+0x37E27`. The whole arithmetic is in the [engine map](exe-reverse-engineering.html), *The effects models* |

GAF frame header (0x18): `u16 w@0, h@2; i16 hotX@4, hotY@6; u8 ck@8, compressed@9,
subframes@0xA, subAlpha@0xB; u8* pixels@0x10`. RLE rows: `u16 length`, then codes
`b&1 → skip b>>1`, `b&2 → repeat next byte (b>>2)+1`, else `(b>>2)+1` literals.

## 4. The native pass — `tagpu_fx.c` + `tagpu_native.c`

Armed by `tagpu_fx.on` (tokens `log`, `nolines`, `nomodels`, `nosprites`, `noexpl`,
`nodebris`). It rides the native unit frame: `tagpu_native_frame` builds the view
(eye, viewport, fog textures, palette), calls `tagpu_fx_gather`, and:

- **Models** (a weapon's body and its thrust flame, an explosion's body, a debris piece)
  are the ENGINE'S OWN PIXELS, drawn by the unit pass ([gpu-status](gpu-status.html) §2.89):

    - the frame packet's publisher poses each one on the game thread with the engine's own
      rotation `0x4B6CC0`, offsets it by the effect's position and copies it with its faces
      and every frame already resolved into five tables (`tagpu_packet.h`, *The effects
      models*), appended last of the fill. It poses only a model whose margin (from its
      node's rest vertices) meets the frame's reach, tested before its faces are read, and
      carries a model whole or not at all: a face the engine draws nothing for (a NULL frame
      or sequence) is left out, one it draws and the packet cannot carry refuses the model,
      as does a node with no vertex and a face to paint, and the tables stop where the
      acquire's bound and the slot's reserve do;
    - `tagpu_fx.c` hands each model to `tagpu_fxmodel.c` as it emits the record, which
      projects it with this pass's eye and runs the engine's rasterisers over it integer for
      integer — the `0x46BAE0`/`0x4211D0` face rules, `0x4C7580` + `0x4C7310` for a textured
      quad, `0x4C0330` for a flat face — and keeps the result as **runs**: a row's pixels that
      take one texel, or one palette index where the face is flat or the texel is the frame's
      key. A frame's runs stop at a budget of 131 072, which the posed pass and the unit
      pass hold to as well (2 + nimg copies of the runs, 20 MB at three swapchain images);
    - the unit pass draws the runs through a vertex stage of its own (`FXVS`) and the unit
      program's fragment stage, each model with a window of the two blocks that draw binds
      (448 B, after every unit's 1 536), so a model takes the unit atlas's texels, the neutral face
      shade (neither draw shades a face), the fog at its effect's anchor, Classic++'s restored
      art and lighting, and the Gamma once at the world composite, exactly as a unit does.
      No shadow and no waterline: the engine draws neither for an effect.
    - **Why runs and not geometry**: the engine's pixel is decided by its 16.16 walk and its
      truncating divisions, and a rocket at 1× is a few dozen pixels. The node posed on the GPU
      and drawn as two triangles a face differed from the engine on 3 of 20 model pixels, 10 of
      62 and 309 of 831 (paused, 1×). The runs match it (MEASURED below).
- **A record is drawn whole or not at all.** A projectile with its ground shadow, its body
  and its flame, an explosion with its flash, its body and its sprite, a debris piece, is
  emitted inside one bracket (`effect_begin`/`effect_end`); any part of it lost for want of
  room — a sprite whose atlas paint the allowance defers, one the full atlas refuses or that
  would not decode, a quad or a line past a bucket's end, a model whose texel the unit
  atlas's allowance has not painted or that the band has no key for (`s_partsLost`) —
  takes the whole record back out for that frame: every bucket, the model list, the run
  arena and the sequence key. So a flash never shows without its explosion, a shadow
  without its shell or a flame without its rocket. The two halves are drawn by two passes,
  and they agree per frame through the hand-over's `nmodels`: the unit pass draws the
  frame's models all or none, and the effects pass draws only a frame whose models the unit
  pass is drawing, so neither shows a frame's effects without the other. The unit pass never
  refuses or stands down for an effect — models it cannot carry (past the run budget, or
  buffers it cannot allocate) are dropped and its units drawn, and that frame shows no
  effects — so it then owes that share, and until every slot holds it the frames after take
  back only their model records. It is asked before the gather whether this frame's models
  can be drawn (`modelsOn`, `tagpu_vk_unit_fx_ready`), and on a no only the records that
  carry a model are taken back and every other effect draws: a no when it has refused or has
  no effects pipeline, when it did not draw the last hand-over it took, while it owes a
  models' share, and when the frame's forecast stands it down — cast shadows without a
  linear depth filter, draws the hand-over will not carry, a fog grid it could not carry,
  the scaffold armed, a texel mirror not there yet. So those stand-downs cost no frame of
  effects (the frame after uncarried draws stop takes its models back once more), and any
  other costs the frame it begins on ([gpu-status](gpu-status.html) §2.89 has what a view
  of models with no unit costs).
- **The paint order is a sequence of keys.** The engine paints with no depth: every
  projectile record in turn (shadow, lines, model or sprite), the debris, every explosion's
  flash, then each explosion's body and sprite. Model `k` of the frame takes `encFx +
  (2k + 1)·step` and a sprite or a line after `k` models `encFx + 2k·step`, so a sprite
  lands over every model emitted before it and under every one after it; every flash takes
  the key the explosion walk starts at. A record taken back rewinds the key with it.
- **Lines and sprites** are the module's own program: a private 2048² atlas (raw, RLE
  and sub-frame frames decoded on first use, `tagpu_gaf.c`), four modes — flat colour
  (lines, Bresenham on the Vulkan lane, [gpu-status](gpu-status.html) §2.31), opaque
  colour-keyed, **ALP** (the engine's table against the pixel underneath, §7.5 *The ALP
  blend*; a 50 % mix in RGB on a device that cannot read it in order), and **flash** = additive with a
  per-level colour derived from the live LHT table (mean palette delta of each of the 32
  rows; an RGB approximation of a palette-space remap). Depth tested and never written, at
  the keys above, drawn after the unit pass; the same fog rule as units (unexplored
  discarded, explored-out-of-LOS darkened). Buckets drawn in order — the lower particle
  layers, lines, flashes (additive), sprites — so explosion sprites sit over their flash
  as in the engine and nothing is dropped however often kinds alternate. **The flashes
  draw before every sprite of the frame**, a projectile's included, where the engine
  draws them after the projectiles: a flash over a projectile's sprite is under it here.
- Gates mirrored: `+0x60 == 0`, the per-projectile LOS/MAPPED test, the viewport-rect
  test for explosions and pieces, `TAProgram+0xF0` bits 5 and 7.
- **Depth bands are per frame**, not constants (`tagpu_native.c`): `fxKey = 3 + (rows +
  8)·4 + 4` sits above the last row key a gathered unit can carry (`rows` = the sweep's row
  count, +8 for the ±256 px gather slack). The effects band starts at `encFx = fxKey − 1.5`
  and holds `2N + 2` keys for `N` = the packet's own bound on the frame's models
  (`tagpu_fx_model_bound`), six wide by default; its step is never below `16·2⁻²³` of the
  depth scale, and the band grows rather than the step shrinking, so adjacent keys stay
  apart in the depth buffer (D24 or float, [GPU status](gpu-status.html) §2.91) and no frame refuses a model for want of keys. The air band is
  `encFx + band + 7.5` (`fxKey + 12` at six keys) and the vertex stages divide by `airKey +
  8`, so any viewport keeps the order ground → effects → aircraft.
- **The world target is premultiplied** for the additive flash (blend `ONE,
  ONE_MINUS_SRC_ALPHA`; the unit program outputs `rgb·a`), so the unit pass's half-alpha
  draws (shadows, cloak) land at 50 %, an RGB stand-in for the ALP table that this pass's
  sprites no longer use where the device allows (§7.5).

Log every 60 frames: `fx: proj=N (laser= model= sprite= flare= light= ball= fogged=)
expl=K flash= debris= -> lines= sprites= flashq= models= runs= (lost= why= of cap )
atlas=` — `models=` drawn this frame, `runs=` their rasterised runs, `lost=` models taken
back with their records, `why=` the rasteriser's last refusal (1 a model the packet did not
carry, 2 a texel the unit atlas has not painted, 3 no room), `cap` the frame's bound on
models; with `log`, the first eight projectile records every 60 frames (`fx: p0 rt=1 …
model=0/1 frame=…`, `model=` the indices of its body and its flame in the packet's model
table).

**MEASURED 2026-09-24** (1024×768; the oracle the engine's own draw with `fxown` off, the
game paused):

| check | result |
| --- | --- |
| model pixels at 1×, Classic | `fx-rockets` 28 of 28, 18 of 18, 2 of 2, 15 of 15, 19 of 19, 20 of 20, 13 of 13, 28 of 28 exact; `big-battle` 1292 of 1352, 487 of 492, 587 of 588, 968 of 968, 1381 of 1459, the others under a health bar or at a sprite's edge the engine draws later |
| the band order, in the presented frame | where only a model has ink the frame shows it, 857 of 857; where a model and a sprite overlap, the frame shows whichever the engine painted last, 247 of 257 — the 10 others sit under a flash and take its additive RGB approximation of the engine's LHT remap |
| Classic++ | the same pixels covered; the models take the restored unit art, 812 of 1192 pixels a new colour |
| zoom 0.49× and 2.35× | every model where the world's own zoom transform puts it, within 0.15 px at 0.49× and under 1 px at 2.35× |
| a forced texel loss (a test build whose unit atlas answers "not painted yet" for every other GAF frame, then "refused" for all of them, 22 s each on `big-battle`) | 38 106 and 50 534 records taken back whole, 60 628 and 127 767 on the review build, 48 282 and 59 001 on the two-block one; records left half-drawn **0** |
| forced overflows and a forced refusal (the same test build, `big-battle` in play) | the model tables held small, the slot cut at them, the run budget held to 256: records taken back whole, half-drawn **0**, no packet refused; the unit pass's own budget and sizing forced to fail: its units drawn and those frames' effects not; the unit pass refused: every effect without a model still drawn |
| a forced models' growth failure and a held-back mirror (the final build, `big-battle`) | the slot's growth for the models failing, retries included: 1 frame without effects, then the sprites drawn with the model records taken back, and the models back once it grew; the unit atlas's mirror held back on a view of models alone: no frame without effects after the onset, where the forecast blind to it lost every other one |
| forced stand-downs (the two-block build, `big-battle` in play, every frame whose models were carried and not drawn counted) | the scaffold armed: **0** such frames, the sprites drawn and the models taken back; a draw the hand-over does not carry, counted before the gather: **0**; the same counted after it: **1**, the frame it began on, then the models held back |
| the publisher in play (the posing, once a tick) | `big-battle` at the battle p50 128–182 µs, p99 183–257 µs; `limits-flood`'s first volley, about 1100 models a tick, p50 1.22 ms, p99 1.92 ms ([gpu-status](gpu-status.html) §2.89 has the whole fill and the comparison without models) |
| the GPU frame, `big-battle` paused, `--maxfps 0`, two launches each | p50 2.40–2.47 ms against 1.83–2.40 ms without the models |

## 5. The engine draws no effect on a Vulkan launch — `tagpu_fxown.c` [LIVE-VERIFIED]

Installed once at DllMain on every launch that is not the `tagpu_gdi.on` oracle, with no
lever; all seven sites are byte-matched first, all-or-nothing:

| Site | Patch | Effect |
|---|---|---|
| `0x469B22` `call 0x49BE60` | call-site redirect | the projectile pass returns at once (`ret 4`) |
| `0x469B2C` `call 0x420B00` | call-site redirect | the pass runs (particle updates keep emitting smoke/fire) bracketed by an *inside* byte |
| `0x46BAE0` | 5-byte prologue detour | `ret 0x10` — no projectile/debris models |
| `0x4B8EC0` | 6-byte prologue detour | `ret 0x10` — no flashes |
| `0x4211D0` | 5-byte prologue detour | `ret 0xC` — no flying pieces |
| `0x4B7F90` `CopyGafToContext` | 6-byte prologue detour, checks the *inside* byte | no explosion sprites; every other caller (features, UI) unaffected |
| `0x471F90` | 5-byte prologue detour | `ret 8` — no particle layer (§ the particle layers below) |

The stubs test a byte that is set once and never written, so each leaf returns at once;
the explosion-pass stub copies it into the *inside* byte around the real pass, so the
shared `CopyGafToContext` returns at once only inside it. Install line: `fxown: the
engine's effects draw is removed (Vulkan) site.proj=1 site.expl=1 model@0x46BAE0=1
flash@0x4B8EC0=1 piece@0x4211D0=1 copy@0x4B7F90=1 sfx@0x471F90=1`; on bytes it does not
know, `… is KEPT`.

Why nothing is lost: the engine's frame reaches no pixel of the screen ([gpu-status](gpu-status.html)
§2.81), so what the removal buys is the engine's CPU, and the golden source `tacli shot`
reads holds no effect. The picture with the engine's effects in it is GDI's, the oracle.
The projectile pass draws from the CRT `rand` `0x4E4870`, a stream the meteor tick reads,
already drawn from a varying number of times a tick by the draw path ([sim](engine/sim.html),
*The meteor shower and the CRT rand*): removing the pass changes that count and adds no
write. The rendertype-2 refraction ball is
dropped while owning. The particle layers have their own
skip byte and detour (§7.5), armed by `tagpu_sfx.on` independently of `tagpu_fx.on`.

Follow-ups noted by review, not done: `tagpu_scaffold.c` keeps a GAF RLE decoder of its
own beside `tagpu_gaf.c`; the byte-match/stub/detour machinery is the fifth private copy
across owndraw, tracer, suppress, scenario and fxown.

## 6. Verification (2026-09-02, `scenarios/fx-mix.json`, `scenarios/fx-lasers.json`)

- Gather: weapon names, render types and positions read clean (`EMG` rt 4, `CORE_LASER`
  / `ARMCOMLASER` rt 0, `KBOT_ROCKET` / `ARMKBOT_MISSILE` rt 1); explosion sequences
  named; screen anchors inside the view; debris pieces appear when a structure dies.
- Double-draw stage: our explosion sprites sit exactly over the engine's (same colours,
  RLE decode confirmed on 4×3 first frames); rocket models render.
- Ownership: with `fx.on` armed the engine's 8bpp surface shows **no** effects while the
  GL frame shows ours; flipping `fx.on` off restores the engine's draw within 30 frames.
- Same-fight A/B (`fx.on=log passive` → engine draws, then `fx.on=log` → ours, seconds
  apart on one instance): laser lines in the same three palette colours
  (147,23,0 / 255,71,0 / 167,27,0), EMG bolt sprites, rockets with thrust flames and
  impact explosions, explosion sequences with the LHT flash, flying debris — panels
  `assets/shots/fx-lasers-ab.png`, `fx-laser-zoom.png`, `fx-ownership.png`,
  `fx-explosions.png`.
- Stress (`200v200`, sim +3, native `all` + effects): **60 fps** (30 `fx:` lines in 30 s)
  with up to 49 projectiles, 122 explosions / 81 flashes, 149 sprites and 189 native units
  on screen; no crash. At that density the shared vertex stream hits its cap (49 152
  verts): effects models are the last emitted, so they are the first dropped — raise
  `MAXNV` or emit effects first when armies that size matter.
- Gaps: rendertype 2 (refraction ball) is dropped while owning; the flash is an additive
  RGB approximation of the LHT remap; the session was locked, so no 60 fps video — burst
  GL shots at sim speed −6 correlated with the `fx: proj=` log line stood in. (The
  particle layers, engine-drawn at the time of this section, are native since §7.)


## 7. The particle layers — smoke, fire, wakes, nanolathe [BINARY-VERIFIED, LIVE-VERIFIED]

*Decompiles of `0x471F90`, the seven particle vtables (every slot), the emitters and the pool,
2026-09-02; the layer map was read live from the engine's objects then. On Vulkan the particles are
emitter events drawn on the GPU (§7.8); the engine's draw is skipped through one more detour in
`tagpu_fxown.c`. Fixtures `scenarios/sfx-strait.json`, `sfx-smoke.json`.*

### 7.1 What they are

The effects passes never draw a puff of smoke. Every smoke puff, flame, wake dot and
nanolathe particle is a **pooled object filed into one of ten layer vectors**, and each
layer is drawn by `0x471F90(ctx, n)` — `stdcall`, `ret 8` — at a fixed point of
`DrawGameScreen`. These sites are the particle sfx, and **the layer number is the draw depth**.
What each layer holds is the layer argument every caller of an emitter pushes (§7.4;
DISASSEMBLED 2026-11-22, every call site of the twelve live emitters):

| Layer | Site | Where in the frame | What the callers file there |
|---|---|---|---|
| 0, 1 | `0x469849/55` | before the flat-feature pre-pass (under every unit and feature) | nothing: no caller passes 0 or 1 |
| 2 | `0x469861` | the same | **wake foam**: COB `EMIT_SFX` types 2–5 (`0x472430`, the ship's trail) |
| 3, 4 | `0x469964/70` | after the flat features, before the row sweep | 4 = **feature smoke** (`SmokeInfinite 0x472C50` from `SpawnFeatureOnMap 0x423FE3`, the vents on Two Continents); 3 nothing |
| 5 | `0x469AFD` | after the row sweep, before the projectile pass | **a burning feature's smoke** (`0x472810` from the feature tick, `0x4243CF`, `push 5` at `0x42439E`) and **the teleport flare** (`0x471FD0` from `0x406B9F`, `push 5` at `0x406B6B`) |
| 6 | `0x469B18` | the same | **nanolathe spray** (the seventeen order-handler calls) |
| 7 | `0x469B38` | after the explosion pass | **COB fire** (`EMIT_SFX` types 0 and 1, `0x472330`) and **submarine bubbles** (`EMIT_SFX 0x103`, `0x472530`) |
| 8 | `0x469BD7` | after the airborne sweep | nothing: no caller passes 8 |
| 9 | `0x469D2C` | just before the fog overlay | **every weapon's smoke** — the impact (`0x49A007`), a line-of-sight shot expiring (`0x49BC80`), the projectile trail (`0x49BDC1`), the shot's trail `0x4729D0` (`0x49CC07`, `0x49CDC3`, `0x49CFE6`) — the explosion's and the corpse's smoke `0x472630`, COB smoke and black smoke (`0x101`, `0x102`), and the flying pieces' smoke and **fire** |

Layer table: `*(main+0x38D77)` = `MEM_Alloc(0xA0)` by `0x471D90`, ten entries of `0x10`:
`{u8 flag @0, void** begin @4, void** end @8, void** cap @0xC}`; `0x471DE0` destroys every
object (vtable slot 0 with flag 1) and frees the vectors at game end. An emitter appends
through `0x4732E0(layer, end, 1, &obj)` (a vector insert with grow-by-copy) after capping
the layer (the oldest is destroyed and the rest shifted down, so a layer holds the cap plus one):
**400** in stock, 20 480 under the raised limits (`tagpu_limits.h`).

### 7.2 The object and its pool

Objects are 76 bytes from the pool at `0x51E610` (`ParticleBase`), whose capacity is fixed
once a process by the C runtime's static initializer (1000 in stock, 204 800 under the raised
limits; the engine map, *The raised effect pools*): `0x470EB0` pops a
pointer from the stack at `+0x14` (`+0x20` = handed out, `+0x1C` = capacity), `0x470ED0`
pushes it back; `+8/+0xC` hold the vector of the pool's allocated blocks (one: `0x470C10` makes a
single block, and the destructor `0x470B80` frees each). A capacity of 0 is safe: the
constructor then allocates nothing and every emitter makes no particle (the engine map,
*The raised effect pools*). The byte `0x51E608` disables every emitter when set; its one
writer is the console's `SFX` toggle at `0x419550`. Layout (`ParticleSystemStruct`):

| Offset | Field |
|---|---|
| `+0x00` | vtable (class, table below) |
| `+0x04` | end tick — `0x471D70(this, life)` = tick + life |
| `+0x08` | tick of the next spawn: each spawn sets it (tick + 1, the flare tick + 10, smoke tick + interval) and spawn-again (slot 5) compares it; the smoke constructors `0x4750B0` (Smoke1) and `0x474CD0` (Smoke2) first set it to GameTime |
| `+0x0C` | u8, read by nothing found: every emitter but the two smoke classes stores its layer argument's low byte here; the smoke constructors store `[esp+7]`, the high byte of the spilled object pointer, so the "2" seen live on smoke is the heap address's top byte [INFERRED] — **not** the layer |
| `+0x10 / +0x14 / +0x18` | sub-particle vector: begin, end, cap (stride per class) |
| `+0x1C…` | class fields: emission point(s), spread, frame caps, sequence choice |

vtable slots: **0** destroy(flag: 1 = return to pool), **1** update (walk the sub-vector:
move by the per-particle step, advance the frame countdown, compact the dead; then if
slot 5 says so, slot 4), **2** draw(ctx) — the only slot `0x471F90` calls, **3** empty?
(sub-vector empty → the sim walker destroys the object; Smoke2's also needs its end tick
passed, Smoke1's always answers 0), **4** spawn one burst, **5** spawn-again? (`+8 <= +4 &&
+8 <= tick`; Smoke1's is `tick >= +8` with no end test), **6** init(pos…, life). So a
**Smoke1 object re-puffs every 5 ticks for the rest of the level**: its spawn-again
`0x4750F0` has no end test and its empty `0x475330` is `xor eax,eax; ret`, so only the
layer cap's trim or the level's teardown removes it. The update runs from the sim tick: the
walker `0x471EB0`, called only at `0x4955BF` (Impure's `particle_walk` replaces that call),
calls slot 3 then slot 1 per object, so **the draw is pure** — rendering from the vectors at
present rate reads exactly what the engine would blit. Every init reads GameTime
`main+0x38A47` through `0x471D70(this, life)` (`+4` = GameTime + life).

### 7.3 The classes (the draw side)

Every sub-particle draw projects `sx = hi(x) − eyeX + vpL`, `sy = hi(y) − hi(alt)/2 − eyeY
+ vpT` from **16.16** positions, and (except Smoke1) gates on the local player's LOS at
tile `(hi(x)>>5, (hi(y) − hi(alt)/2)>>5)` — LOS counter with true LOS, else the MAPPED
bit — the same rule as the projectiles.

| vtable | Class (TADR's name) | Sub stride | Per-particle draw | Fields |
|---|---|---|---|---|
| `0x4FD638` | Smoke1 (`SmokeInfinite`'s) | `0x20` | `AlphaCompsteBuf2OFFScreen` of `seq[frame]`, **no LOS gate** (`0x475700`) | `seq* @0, x/alt/y @4/8/C, nframes @0x10, frame @0x14, countdown reset @0x18, countdown @0x1C` (TADR's `SmokeGraphics`); the sequence is always `*(main+0x147CF)` ("smoke 1") |
| `0x4FD618` | Smoke2 (every other smoke) | `0x20` | same, LOS-gated (`0x475470`) | same layout; sequence `*(main+0x147CF)` ("smoke 1") or `*(main+0x147D3)` ("smoke 2") by the init's last argument, which only `EmitSfx_BlackSmoke 0x4728F0` sets |
| `0x4FD5D8` | fire | `0x3C` | alpha blit of `seq[frame]`, LOS-gated (`0x474170`) | `seq* @0` = the flare sequence `*(main+0x147F3)`, pos @4/8/C, velocity @0x10.., frame @0x2C, end tick @0x38 |
| `0x4FD588` | flare sprite (the teleport's) | `0x34` | alpha blit of `seq[frame]`, LOS-gated (`0x473590`) | pos @4/8/C, frame @0x2C |
| `0x4FD5F8` | wake / bubbles | `0x44` | `DrawBar(ctx, {x, y, x+1, y+1}, colour)` = a **2×2 dot**, LOS-gated (`0x4745E0`) | pos @4/8/C, colour byte @0x30 cycling `0x61..0x67` (foam), dies when its ground height rises above sea level |
| `0x4FD5B8` | nanolathe spray | `0x30` | `DrawBar(ctx, {sx, sy, sx+1, sy+1}, colour)` = a **2×2 dot** (its fill `0x4CCDEA` is inclusive), LOS-gated (`0x473A00`, which adds the constants `0x80` and `0x20`) | **pos @0/4/8**, target @0xC.., step @0x18.., colour byte @0x28 = `0xA1 + n%7` cycling its low nibble 1..7, end tick @0x2C |
| `0x4FD5A8` | base (destroyed object) | — | nothing | the destructors reset the vtable to this |

**Each class's update** (slot 1; DISASSEMBLED 2026-11-22). Every update walks the
sub-vector — move, death test, the dead compacted by shifting the rest down (`rep movs`) —
then `if slot5() slot4()`. Positions are 16.16; T is GameTime after this tick's increment.

| Class | Update | Per sub-particle per tick | Death | CRT `rand` |
|---|---|---|---|---|
| Smoke1 | `0x475600` | `x += wind_x(main+0x37ECC) << 3`, `alt += main+0x14263 << 4`, `y += wind_y(main+0x37ED4) << 3`; `--countdown`, at 0 `frame++` and `countdown = h + rand·h/0x8000`, h = reset/2 toward zero | `frame >= nframes`, tested every tick after the move | one a frame advance (`0x475679`) |
| Smoke2 | `0x475340` | the same with `alt += main+0x14263 << 2` | the same | one a frame advance (`0x4753B9`) |
| fire | `0x473010` → move `0x474130` | `pos += step`; `counter = (counter+1) % period`, at 0 `frame = (frame+1) % (nseq−1)` | `0x4742A0`: T > end | none |
| flare | `0x472D50` → move `0x473560` | `pos += step`; `frame = (frame+1) % (nseq−1)` | `0x4736C0`: T > end | none |
| wake | `0x473170` → move `0x474580` | `pos += step`; `counter = (counter+1) % period`, at 0 `colour += delta`, past max → min, under min → max | `0x474720`: T > end, **or** the ground under the dot `0x485070(&pos)` at or above the sea-level byte `main+0x1427F` | none |
| nano | `0x472EB0` → move `0x4739B0` | `pos += step`; colour's low nibble + 1, 7 → 1 | `0x473B30`: T > end | none |

The wind is piecewise constant (its updater changes it 150–300 ticks apart: the engine map's
*The wind*), the rise `main+0x14263` has no store by displacement, and the ground `0x485070` is
the terrain's height. So fire, flare and nano positions and frames are closed in (initial
state, ticks updated), the wake's too but for its death over shallow ground, and smoke's
position is closed given the wind's history, its frame times taking one `rand` a frame advance.

**Each class's spawn and spawn-again** (slots 4 and 5; DISASSEMBLED 2026-11-22):

| Class | Init (slot 6) | Spawn (slot 4) | Again (slot 5) | Empty (slot 3) |
|---|---|---|---|---|
| flare | `0x4736E0(from, to, life)`: `+0x20` from, `+0x2C` to, d = to − from; steps = `_ftol(fsqrt(dx²+dy²+dz²))` / 5 world units (the int16 high word of `(len << 16)/0x50000`, `0x47376A..0x473788`), step = d/steps by `idiv` — **no zero guard**: a teleport shorter than 5 world units divides by zero at `0x47378E` | `0x4737C0`: one sub `{flare seq, pos = from, step, frame = rand·(nseq−1)/0x8000 (0x47395F), end = T + steps}`; `+8 = T + 10` | `0x472E00`: `+8 <= +4 && +8 <= T`, so subs at T, T+10, T+20, T+30 (life 30) | `0x472E70`: sub-vector empty — a flare of fewer than 10 steps dies with its first sub |
| nano | `0x473B50` (below) | `0x473D50`: five subs, six draws each | `0x472F60`: once, at T+1 | `0x472FD0`: sub-vector empty |
| fire | `0x4742C0(from, to, period, life)`: end `+4 = T + life`, `+0x1C` period, step = `(d · ⌊2^32/(life << 16)⌋) >> 16` per axis (`0x47432C..0x47438B`) | `0x4743A0`: one sub `{flare seq, pos = from, step, frame 0, counter 0, period, end = +4}`, no `rand`; `+8 = T+1` | `0x4730C0`: every tick until `+8 > +4`, so life + 1 subs | `0x473130`: sub-vector empty |
| wake | `0x474760(a, b, period, life, dir)`: end `+4 = T + life` (the emitters pass 1), step = d · (2^32 / (len·2)) >> 16, a half world unit a tick toward b (`0x4747CC..0x474868`) — **len 0 divides by zero** (`_alldiv` at `0x474811`), as bubbles emitted exactly at sea level are; `+0x44` = dir | `0x474880`: one sub; three draws (`0x4749D9`, `0x474A00`, `0x474A27`) add `rand·7/0x8000 − 3` to the x, alt and y high words; colours `0x61..0x67`, starting `0x61` rising when dir else `0x67` falling; end = T + 6·period; `+8 = T+1` | `0x473220`: at T+1, so two subs an emission | `0x473290`: sub-vector empty |
| Smoke2 | `0x474D50(pos, fcap, interval, countdown, life, seqsel)` `ret 0x18`: `+4 = T + life`, `+0x1C` interval, `+0x28` seqsel, `+0x24` = nseq−1 capped by fcap when fcap is nonzero and smaller, `+0x20` = countdown or 7 | `0x474DF0`: one puff `{seq, pos, nframes = rand·(+0x24−2)/0x8000 + 2 (0x474F1C), frame 0, countdown = reset = +0x20}`; `+8 = T + interval` | `0x475440`: `+8 <= +4` (signed) `&& +8 <= T` | `0x474F80`: sub-vector empty **and** `+4 < T` |
| Smoke1 | `0x475150(pos, interval, countdown, life)` `ret 0x10`, the same fields, the point at `+0x28` | `0x4751C0`: as Smoke2's (one draw, `0x4752D4`) | `0x4750F0`: `T >= +8`, no end test | `0x475330`: always 0 |

The nano spray's init (`0x473B50`) cuts each of its two 6-int boxes, on every axis, to its middle: from
the 4/11 to the 7/11 point of that axis's span, in int32. Its spawn (`0x473D50`) makes five
sub-particles a call, each from a random point of the first box to a random point of the
second (six CRT `rand` draws), in a straight line at about 4 world units a tick
(steps = `(int16)(len >> 18)` of the 16.16 distance's integer length, `0x473EB6..0x473F04`; a
particle of 0 steps not made). One emission is two such
bursts: the first in the builder's turn, moved by the walker the same tick, the second made
by the walker on the next tick after it moves the live ones, and only when the first made a
particle. The engine map's *The nano particle* has the addresses.

### 7.4 Emitters — who makes them, and the layer argument

DISASSEMBLED 2026-11-22 (every call site of each, and the pushes before it). All are
`stdcall`; every one reads the layer table `main+0x38D77` (no NULL test), skips everything
while `0x51E608` is set, takes an object from the pool (`0x470EB0(0x51E610, size)` — the
size is ignored, `ret 4`, every slot is `0x4C`), zeroes it, sets the class, calls init (slot
6), then trims the layer to its cap and appends (§7.1). A NULL from the pool skips init and
append. The layer is the low word, sign-extended and **not bounded**.

| Emitter | Class | Object | Init call | `rand` in the emitter | Callers and the layer they pass |
|---|---|---|---|---|---|
| `EmitSfx_Teleport 0x471FD0(from, to, life, layer)` `ret 0x10` | flare | `0x44` | `0x4736E0(from, to, life)` | none | the Teleport handler `0x406AA0` at `0x406B9F`: life 30, layer 5; Impure's `tagpu_rec_teleport_flare` the same |
| `EmitSfx_NanoParticles 0x4720D0(point3, box6, layer)` `ret 0xC` | nano | `0x4C` | `0x473B50(box1, box2, 1)` | none | ten order handlers, layer 6 (*The nano spray* in the engine map) |
| `…Reverse 0x472200(box6, point3, layer)` `ret 0xC` | nano | `0x4C` | the same, box to point | none | seven order handlers, layer 6 |
| `0x472330(from, to, period, life, layer)` `ret 0x14` | fire | `0x44` | `0x4742C0(from, to, period, life)` | none | COB `EMIT_SFX 0x480EB0`: type 0 (VTOL) at `0x481010` (period 1, life 6, layer 7), type 1 (thrust) at `0x48102F` (1, 7, layer 7) |
| `0x472430(a, b, period, layer)` `ret 0x10` | wake | `0x48` | `0x474760(a, b, period, 1, 1)` | none | `EMIT_SFX` types 2–5: `0x48104C` (period 16), `0x481069` (8), `0x481086` (16, points swapped), `0x4810A3` (8, swapped); layer 2 |
| `EmitSfx_Bubbles 0x472530(a, b, period, layer)` `ret 0x10` | wake | `0x48` | `0x474760(a, b, period, 1, 0)` | none | `EMIT_SFX 0x103` at `0x481102`: the point and the same point at sea level, period 8, layer 7 |
| `0x472630(pos, interval, life, layer)` `ret 0x10` | **Smoke2** (constructor `0x474CD0`), "smoke 1" | `0x38` | `0x474D50(pos, 0, interval, 0, life, 0)` | none | the explosion add `0x420A30` at `0x420AE1` (interval 7, life 15, layer 9; only when its 4th argument is 0 and the point is above sea level: the engine map's *sinks*); the corpse helper `0x486360` at `0x48644B` (interval 15, life 900, layer 9) |
| `EmitSfx_GraySmoke 0x472810(pos, layer)` `ret 8` | Smoke2, "smoke 1", one puff | `0x38` | `0x474D50(pos, 0, 1, 0, 0, 0)` | none | a flying piece `0x421583` (9: once a draw in stock; under Impure once per frame that ran ticks, none while paused — [engine patches](modules/engine-patches.html)); a burning feature every third tick `0x4243CF` (**5**); `EMIT_SFX 0x101` `0x4810B9` (9, damaged-unit smoke); the impact `0x49A007`, weapon `+0x111` bit 10 (9); a line-of-sight shot expiring without burnblow `0x49BC80` (9); the projectile trail `0x49BDC1`, weapon `+0x111` bit 18, one puff every `weapon+0xFA` ticks while the projectile lives (9) |
| `EmitSfx_BlackSmoke 0x4728F0(pos, layer)` `ret 8` | Smoke2, **"smoke 2"**, one puff | `0x38` | `0x474D50(pos, 0, 1, 0, 0, 1)` | none | `EMIT_SFX 0x102` at `0x481118` (9) |
| `0x4729D0(pos, layer)` `ret 8` | Smoke2, "smoke 1", frame cap 3, countdown 30 | `0x38` | `0x474D50(pos, 3, 1, 30, 0, 0)` | none | the three shot paths, weapon `+0x111` bit 9, once a shot: `0x49CC07`, `0x49CDC3`, `0x49CFE6` (9) |
| `EmitSfx_Unk5 0x472AB0(pos, layer)` `ret 8` = **fire** | fire | `0x44` | `0x4742C0(&L, &L, 1, life)`, from = to = L | **four, before the `0x51E608` test**: x, alt, y of a copy L each `+= rand·3/0x8000 − 1` on the high word (`0x472ACF`, `0x472AF4`, `0x472B17`), life `rand·3/0x8000 + 1` (`0x472B3A`) | a flying piece on fire (`sys+0x28` bit 0) at `0x4215AA` only (9; emitted as the piece's smoke is) |
| `EmitSfx_SmokeInfinite 0x472C50(pos, layer)` `ret 8` | **Smoke1** (constructor `0x4750B0`) | `0x34` | `0x475150(pos, 5, 0, 150)` | none | `SpawnFeatureOnMap 0x423C50` at `0x423FE3` (layer 4): at map load, on the loader thread, and in play (the feature tick, a reclaim's swap, a corpse, the console) |

The layer is **an argument of the emitter**, and each caller passes its own: weapon smoke
of every kind is filed in layer 9, over everything, while a burning feature's smoke and the
teleport flare sit in layer 5, under the projectiles. TADR's "1 smoke, 2 wake, 6 nano,
7 fire" type tags were read from the `+0xC` byte (§7.2).

**What a call makes** (from §7.3's spawns): a nano emission two bursts of five; a wake or
bubbles call two dots (T and T+1); a COB fire call life + 1 flames, one a tick; a fire from
`0x472AB0` 2 to 4 flames; the teleport up to four flares, ten ticks apart; one-puff smoke
(`0x472810`, `0x4728F0`, `0x4729D0`) one puff; `0x472630` a puff every `interval` ticks until
`life` (3 from the explosion add, 61 from a corpse); `SmokeInfinite` a puff every 5 ticks for
the rest of the level.

**When inside a tick** (the sim step `0x495490`'s call order): GameTime is incremented
(`0x4954C0`), then units (`0x4954ED`: the nano spray, and COB `EMIT_SFX` [INFERRED]),
projectiles (`0x495513`: trails, impacts, shots), the explosion records (`0x495539`),
players, features (`0x495585`: feature smoke, spawns), the wind (`0x49558F`), meteors
(`0x495594`) and last the particle walker (`0x4955BF`). An object emitted inside the step
is updated the same tick; the flying pieces' smoke and fire, emitted in the explosion
draw, are updated on the next one.

**Which random stream.** In stock the emitters, spawns and smoke updates draw the C runtime's `rand`
`0x4E4870` at eighteen sites (the tables above), and the feature tick two more for a burning
feature's smoke point, never the simulation's generator `0x4B6C30`. That `rand` keeps one seed per
thread, and the game thread's stream is also the meteor shower's, the loader thread's also the
skirmish start positions': the engine map's [*The two random streams*](engine/sim.html), which
classifies all 61 call sites and measures the loader's order. Impure answers all twenty from the
particles' own rule instead (§7.7).

### 7.5 Native — `tagpu_sfx.c`, and owning the draw

On a Vulkan launch the engine holds no particle object (§7.8), so there are no layers to gather:
`tagpu_sfx.c` (armed by `tagpu_sfx.on`: tokens `log`, `passive`, `nosmoke`, `nofire`, `nowake`,
`nonano`) hands the frame packet's particle events to the effects pass once a frame
(`tagpu_fx_set_events`), and the pass draws every particle from its event (§7.8): layers 0..6 before
the lines and flashes (the engine draws those layers before its projectile pass, so a laser or a
flash sits over trail smoke), 7..9 after the sprites, so the draw order reproduces the engine's. Per-frame
keys from the native pass (`tagpu_native.c`, nothing absolute): layers 0..2 at `0.30` and 3..4 at `0.60`
(the engine draws 0..2 before its flat-feature pre-pass and 3..4 after it, so the flat band 0.40..0.50
sits between them; all under every row key), 5 and 6 at `fxKey − 2` (above the last row key
`fxKey − 2.2`, below the fx models at `fxKey ± 1.8`), 7 at `encFx + fxBand + 0.5` (above the fx
sprites), 8 at `airKey + 3`, 9 at `airKey + 5`. The tokens choose the
classes drawn: `nonano` no nano event, `nofire` no fire or flare, `nowake` no wake or bubbles,
`nosmoke` no smoke, `passive` none. Log every 60 frames with `log`: `sfx: events=N (W wind rows)`,
`EVENTS-TRUNCATED` when the packet cut the table, `(passive: nothing drawn)`.

The engine's own particle draw is one more prologue detour in `tagpu_fxown.c`: `0x471F90`
starts with `mov eax,[0x511DE8]` (five position-independent bytes), so the stub is `cmp byte
[removed],0; jz stolen; ret 8`. The walker is the only draw path (ten call sites, all in
`DrawGameScreen`; the update lives elsewhere), so skipping it removes exactly the pixels
we draw and nothing else. It is removed with the effects, on every Vulkan launch
(§5); without the oracle it walks ten empty layers when it runs, and under `tagpu_particlecheck.on`,
which makes the engine's objects in full, it is what keeps them off the screen.

**The ALP blend** [MEASURED 2026-11-22]. Every sprite the engine draws through `0x4B8500` —
fire, the teleport flare, Smoke1, Smoke2 (all four leaves, §3), and a projectile's ALP sprites,
its ground shadow and an alpha sub-frame (mode 2 of the sprite program) — is drawn by the
engine's rule: the destination's palette index `i` is the entry the engine's own search
`0x4BA9D0` picks for the pixel underneath (R+G+B within 40, walked in `0x4BA920`'s sorted
order, the first strict minimum of the squared distance), the table answers `T[src][i]`, and
the pixel becomes `dst + pal[T[src][i]] − pal[i]`, clamped. Where the destination is a palette
colour that is the engine's pixel exactly; under Classic++ the ground's restored detail stays
and moves by what the table does to its nearest entry. The table is the engine's own,
`[0x51FBD0]+0xC0`, copied once a pointer by the packet's publisher (`alp_snapshot`) — what the
install's PALETTE.ALP or the generator put there, so a mod's table is a mod's.
- **How the pixel underneath is read**: in the ordered pixel interlock
  (`VK_EXT_fragment_shader_interlock`, `pixel_interlock_ordered`), which runs the critical
  sections of every fragment on one pixel in primitive order, so a stack of overlapping
  sprites in one draw applies the table one sprite after another, as the engine's blits do.
  The world target is bound as a storage image (in GENERAL, read and written without a
  format, since the swapchain's format is BGRA and no GLSL qualifier names it), and each
  fragment of the effects pass loads the pixel, blends — the table for an ALP texel, the fixed
  function's premultiplied rule or a flash's addition for every other — and stores it inside
  the interlock; the depth test runs early, so a hidden fragment never stores. The effects
  pass suspends the world render pass, draws in an ordered pass with the depth attachment
  alone (the colour written two ways in one subpass would be undefined), and resumes the world
  after it (`tagpu_vk_world.c` `build_ordered`); the passes are separated by explicit pipeline
  barriers as well as by their dependencies, which one driver dropped between the restorer's
  passes in `GENERAL` ([vulkan-backend](modules/vulkan-backend.html) §2.95): every colour layout
  change of the three passes is in a barrier (the world pass ends in COLOR_ATTACHMENT_OPTIMAL
  where it may be suspended, the ordered pass's way in and out move it to GENERAL and back, and
  the world's end moves it to what the composite samples), so no render pass makes one between
  the world pass's end and that last barrier. The world pass's own entry from UNDEFINED is still
  ordered by its first dependency, which keeps the previous frame's composite read before the clear.
- **Why the interlock and not rasterization-order attachment access**
  (`VK_EXT_`/`VK_ARM_rasterization_order_attachment_access`), which would read the colour as an
  input attachment of the same subpass and leave the other modes' blend to the fixed function:
  the reference setup's GPU (an RTX 4070) offers the interlock and not that extension, so the
  access would have left the reference setup on the fallback. llvmpipe offers both, and draws by
  the table (its log: `vk: the effects' ALP blend: the engine's table against the pixel
  underneath, in the ordered pixel interlock`).
- **What it reads**: the sprite's palette index from an index atlas beside the base atlas (the
  same texels, unexpanded), the table and the sorted palette from one 256 × 257 image
  (`tagpu_fx.c` `s_alpT`). The index atlas's tiles ride in the same frame's staging as the base
  atlas's, and the pass's staging is sized for both (`tagpu_vk_fx.c` `FX_STAGE_CAP`: the
  allowance's 2 MiB of base tiles, a quarter of that for their indices, 4 B a rect and the
  table, 2 888 724 B a slot), so a partial upload the allowance admits always fits. Where the
  device maps less than that, or a copy is past what one frame sends, the page goes whole
  through the bands, which cost that frame a wait and need one row of staging (8 KB at 2048
  texels); a device that will not map one row is the only staging shortfall that skips the
  effects, and it skips them on every frame it lasts.
- **Where it falls back** to the 50 % mix in RGB: a device without the interlock, an instance
  without `VK_KHR_get_physical_device_properties2`, a swapchain colour format without storage or
  (where the device reports it per format) without storage read and write without a format, a frame before the table has reached the device, and
  a world drawn straight into the swapchain image. No setting chooses it. The log names the path
  at device creation (`vk: the effects' ALP blend: …`) and counts the frames of each every 600
  (`fx: the ALP blend: N frame(s) by the engine's table in the pixel interlock, M by the 50 %
  mix`).

Caveat, the same as the effects pass's: with `sfx.on` but no `native.on=all`, the layers
the engine draws under its units (0..4, wake foam above all) composite over the
engine-drawn hulls in the 8bpp frame, because nothing wrote depth for those hulls.

### 7.6 Verification

Every particle class is held to the engine's own objects by the oracle (`tagpu_particlecheck.on`,
§7.8): the same emissions made in full by the engine and as events, every sub-particle compared after
every walk, in four replay test groups (`particle-check`, `particle-check-sea`,
`particle-check-smoke`, and the battle's and teleport's groups beside them). The pictures are compared
against `tagpu_gdi.on`, the engine's own draw, on the same scenarios at zoom 1 (§7.8, *MEASURED*).

Gaps: layer 8 has no caller at all (§7.1); the teleport flare `0x4FD588`, unseen in 2026-09, is
held to its closed form under the oracle since 2026-11-22 (§7.8); `DrawBar`'s
rect `{x, y, x+1, y+1}` is taken as 2×2 from the panels, not from its fill routine
(`0x4CCDEA`); sub-pixel motion for puffs is not used (engine parity: integer anchors);
the sprites (fire, flare, smoke) take the engine's table only on a device with the pixel
interlock (§7.5, *The ALP blend*), and the 50 % mix in RGB elsewhere differs visibly on the flame.
The flare sequence `*(main+0x147F3)` is `flamestream` (pushed at `0x429A05`, found by `0x4B8D40` at
`0x429A11`, stored at `0x429A22`), whose twenty frames hold two indices only, 208 (255, 255, 0) and
209 (255, 191, 0); retail's table answers 209 for either over the other (`ALP[208][209] =
ALP[209][208] = 209`: the mean (255, 223, 0) is equidistant from 208, 209 and 251, and `0x4BA9D0`'s
walk over the list sorted by R+G+B meets 209 first), so a stack of flames converges to orange,
where an RGB mix stays between the two. Where the destination is not a palette colour (Classic++'s
ground, 5.3 % of the eight Atlases' world pixels are palette colours against 100 % on GDI) the
table is applied to its nearest entry and the difference carried over, so the result is the
engine's colour moved by the ground's own detail, not the engine's pixel.

MEASURED 2026-11-22, the warm pixels' mean (R ≥ 200, G ≥ 100, B ≤ 60, pure yellow excluded), zoom 1,
`--defaults`, against `tagpu_gdi.on` on the same scenario:

| sample | 50 % mix | the engine's table | GDI |
|---|---|---|---|
| eight Atlases taking off (`b2-stack-atlas.json`), ten frames | (231.3, 212.8, 4.9) | (246.3, 169.8, 16.4) | (247.8, 175.9, 8.7) |
| the teleport flare (`replay-teleport`, four riders), eight and sixteen frames | (204.9, 178.5, 12.3) | (235.4, 153.8, 26.4) | (236.6, 153.8, 23.4) |

The mix's row is the same on the build with the interlock forced off: (231.4, 212.6, 4.9).
Smoke shows no such contrast in the captures: on `replay-smoke`'s battle the grey pixels' mean
(channel spread under 25, mean 100..220) is (142.2, 144.7, 138.7) with the table, (146.9, 150.1, 146.8) with the mix and
(148.6, 148.6, 148.7) on GDI — a battle that is not the same frame in the three, so the figures
bound the difference rather than measure it.

### 7.7 The random rule — the same particles on both renderers [DISASSEMBLED, LIVE-VERIFIED 2026-11-22]

Impure redirects every particle draw of the CRT's `rand` to a keyed rule (`tagpu_patches.c`, *The
particles' own random rule*; the function itself in `inc/tagpu_prand.h`), on GDI and on Vulkan, so no
CRT stream sees a particle and a closed form that knows an emission can compute every value the
engine's particle code computes:

    mix(x)            = lowbias32: x ^= x>>16; x *= 0x7FEB352D; x ^= x>>15; x *= 0x846CA68B; x ^= x>>16
    seed(level, T, n) = mix(mix(level ^ T) ^ n)
    value(seed, p, d) = mix(mix(seed ^ p) ^ d) >> 17          (0..0x7FFF, rand's range)

32-bit unsigned arithmetic, wrapping: a GLSL `uint` port is bit-exact.

- **level** is the level's particle seed, made at the layer table's allocation (`0x4918B1`) on the
  loader thread, before the map load spawns a feature: from the session's GUID and the map in a
  network game (every peer alike), from the harness's fixed seed and the map under
  `tagpu_seeds.on`, otherwise from the performance counter. The log names it: `enginefix:
  particles: level N draws from seed 0x…`.
- **T** is GameTime and **n** the emission's ordinal among the GameTime's emissions, counted at
  the pool's alloc `0x470EB0`, which every live emitter calls once a call after its SFX test,
  whatever the pool answers. An object keeps its emission's seed, by pool slot.
- **p**, the particle: the object's own count of the particles it began.
  - nano (`0x473D50`): the first burst's particles 0..4, the second's 5..9; draws 0..2 the start
    x, alt, y, 3..5 the end's, made whatever the steps (a particle of 0 steps is skipped, its
    ordinal spent);
  - wake and bubbles (`0x474880`): the dot at T is 0, at T+1 is 1; draws 0..2 the x, alt, y jitter;
  - flare (`0x4737C0`): the sprite at T + 10k is k; draw 0 its start frame;
  - smoke (`0x474DF0`, `0x4751C0`): a puff is its spawn's ordinal k (the object's spawn count when
    it spawned: 0, 1, 2 ... for the object's life); draw 0 its frame count, and each frame
    advance's countdown (`0x475340`, `0x475600`) draw f, f the frame it advances to (1 or more).
    The puff carries k's low bits in its position's low bits (x bits 0..2, y bits 0..2, alt bits
    0..1 for Smoke2, 0..3 for Smoke1: 8 or 10 bits), which the updates only ever add multiples of 8
    and 4 (16) to, so every high word, the only part the leaves and the packet read, is stock's. An
    update recovers k as `last − ((last − low) mod span)`, last = the object's spawn count − 1 at the
    draw, span 256 (Smoke2) or 1024 (Smoke1): k itself for a puff fewer than span spawns old. A
    Smoke2 object spawns at most 61 puffs; a Smoke1 puff 1024 spawns old would have lived 5 120
    ticks. A closed form computes the same recovered value from k and the spawn count at the tick;
  - draws made before the object exists take the seed the next alloc gives and p = `0xFFFFFFFF`:
    the fire emitter `0x472AB0`'s four (d 0..2 the x, alt, y jitter, 3 the life) and the feature
    tick's two (d 0, 1, the smoke point's jitter), then the Smoke2 object it emits.
- **What differs from stock**: only the values drawn, as any change of draw order would make them.
  The emissions, their order, the classes' moves and deaths are the engine's. A replay host draws
  its own particles from its own level seed and its own emissions: a puppet re-fires the emitters
  as its replayed simulation reaches them.
- **The two inits that divided by zero** — the flare's steps (`0x473788`) and the wake's length
  (`0x47480A`) — end at their epilogues on a zero divisor, so such an emission makes no particle
  (the walk destroys the empty object at its next pass).

Its counters (draws answered, emissions, objects outside the pool) are in the install line's
`Counters:` and logged for each level at the next level's load. MEASURED 2026-11-22: Two Continents'
load makes six emissions (the vents' `SmokeInfinite`), and 1 274 values were drawn by the rule by
the first `peek` in play, none outside the pool.

### 7.8 The particles from emitter events — the GPU computes them [DISASSEMBLED, LIVE-VERIFIED 2026-11-22]

On a Vulkan launch every class is drawn from **emitter events**: the nano spray (`0x4720D0`,
`0x472200`), fire (`0x472330` COB `EMIT_SFX` 0 and 1, `0x472AB0` the flying pieces), the teleport
flare (`0x471FD0`), the wake with the submarines' bubbles (`0x472430` COB `EMIT_SFX` 2..5,
`0x472530` `EMIT_SFX 0x103`), Smoke2 (`0x472630`, `0x472810`, `0x4728F0`, `0x4729D0`) and Smoke1
(`0x472C50`). Each emission records one event on the game thread (Smoke1's at a map's load on the
loader thread) with its seed, the frame packet carries the live ones, and the GPU computes every
particle from its event and the packet's GameTime. GDI is untouched: the engine's objects are made,
walked and drawn as before. **On Vulkan the engine makes no particle object**: the pool's capacity
operand `0x471C83` is written 0 at attach (both builds; the constructor `0x470A90` then allocates
neither the 15.6 MB block nor the 0.8 MB stack, [weapons](engine/weapons.md), *The pool*), every
alloc answers NULL, and the walk at `0x4955BF` walks no layer.

**Every emission is recorded at the alloc.** The pool alloc `0x470EB0`'s stub (`fix_particle_rand`)
asks `pevt_pre` first. It knows the twelve emitters by their return address (`0x47212B`,
`0x47225B` nano; `0x47245A` the wake, `0x47255A` the bubbles; `0x471FFA` the flare; `0x47235A` COB
fire, `0x472B7F` the pieces' fire; `0x472657`, `0x472837`, `0x472917`, `0x4729F7` Smoke2; `0x472C77`
Smoke1) and copies out of the emitter's frame what the init would take and the layer word the
emitter reads after it: the nano boxes; the wake's and bubbles' points and period (the init's life is
the constant 1, its direction 1 or 0 by emitter); the flare's two points and life; COB fire's two
points, period and life; the pieces' fire's point L (on the emitter's stack, from = to = &L), period 1
and life (in `ebp`); Smoke2's point and, for `0x472630`, interval and life, the three one-puff
emitters' constants (`(0,1,0,0,0)`, `(0,1,0,0,1)`, `(3,1,30,0,0)`), Smoke1's `(5,0,150)`. Then it
answers "no object", and the emitter skips its init, its layer trim and its append exactly as for a
full pool. The emission is still counted where every other one is (`prand_alloc`), so n and every later
seed are what they would be with the object made, and the event is recorded there with its seed
(`tagpu_patches.c`, *The particles as events*). What `pevt_pre` reads is laid out by the emitters'
own code, so their bytes are rows of the limits table, held to stock all or nothing with the alloc's
stub: the nano emitters' from the entry to the layer's read (`0x4720D7..0x472187`,
`0x472207..0x4722B7`), the wake's (`0x472435..0x4724B6`), and every other emitter's from the alloc's
return to the layer's `movsx` after its init (`0x47255A..0x4725B5`, `0x471FFA..0x472052`,
`0x47235A..0x4723B7`, `0x472B7F..0x472BD3`, `0x472657..0x4726A1`, `0x472837..0x47287B`,
`0x472917..0x47295B`, `0x4729F7..0x472A3B`, `0x472C77..0x472CBA`). The first seven bytes of each nano
emitter and the wake's first five are the recorder's, compared before it installs; on a Vulkan launch
other bytes there, or a walk hook `0x4955BF` not installed, refuse the table by name as a differing
row does, since the events are the only particle draw there. A table not in ends the process before
the game starts ([engine patches](modules/engine-patches.md) §2.6c).

**The layer's cap and the pool's count are applied to the events.** Every object the engine would
have made is an event, so a layer's events are its objects: an emission into a layer that holds more
than `TAGPU_LIM_SFX` events first loses that layer's oldest, whatever its class, as the emitter's
trim destroys the front object (§7.1), and `TAGPU_LIM_SFXPOOL` live events refuse an emission
(counted as `not made for a full pool` in the level's line), as the alloc refuses one past its
capacity — so a stock-limits game, whose pool of 1000 is smaller than six layers at cap, refuses what
stock refuses. The oracle checks both by construction: any difference is a *missing* or *extra*
object. An emission an event cannot hold draws nothing and is counted in the level's `refused`: a layer
outside 0..9, more than ten particles, a fire life or period under 1 (stock divides by zero on them),
no flare sequence, a wake or bubbles period outside 1..1024, a smoke interval under 1 or reset under 2,
a smoke frame cap past the 64-row frame table, a puff life past the wind ring, a Smoke2 object of more
than 1 024 puffs, or smoke whose puffs could be drawn with another puff's ordinal. No stock caller
makes one: every caller pushes its arguments as constants (`EMIT_SFX` life 1 and 6 or 7, periods 1, 8
and 16; the flare life 30; the smoke emitters intervals 1, 5, 7 and 15, resets 7 and 30; the pieces'
fire life 1..3).

**The event** (`inc/tagpu_pevt.h`, 80 B): the seed, T, the layer, two flags and the class.
- *Nano*: the init's two boxes as it cuts them (per axis base `min + 4d/11`, extent `7d/11 − 4d/11`,
  `0x473B80..0x473D34`) and each of the ten particles' step count: the int16 high word of `(len <<
  16) / 0x40000`, `len` the x87 length of the particle's path through `_ftol 0x4E43A0`
  (`0x473E75..0x473ED9`), which the game thread computes at emission with the spawn's own
  instruction sequence, so the GPU never takes a square root.
- *Fire and the flare*: the start point, the step, fire's period, the particle count, the flare
  sequence's frame count − 1 and the life (fire) or step count (flare). Fire's step is the init's
  `(d · q) >> 16`, q the low dword of `2^32 / (life << 16)`; the flare's step count is the int16
  high word of `(len << 16) / 0x50000` by the same x87 sequence, its step `d / count` by `idiv`.
- *The wake*: the init's first point a, the step (the init's `(d · q) >> 16`, q the low dword of
  `2^32 / (len · 2)` with len by the same x87 sequence: half a world unit a tick), the period, the
  dot count (0 when `len · 2` is 0 in its low dword: the divide's fix makes no dot), the colour's
  direction, and **each dot's death walk**.
- *Smoke*: the init's point, the spawn interval, the countdown's reset, the frame cap, the sequence,
  Smoke2's end `T + life`, and three timing words: puff 0's first walk, puff 1's walk and the puff
  count (Smoke1 endless); the longest puff life its reset and cap allow.

**The closed forms.** *Nano* (`tagpu_pevt_nano_at`): burst 1 (particles 0..4) is born at T and
first moved by the walk of T; burst 2 (5..9) exists only when the object is updated at T+1, born
then after that walk's moves and first moved at T+2 (weapons engine map, *Two bursts an emission*).
After the walk of GameTime P a particle of `s ≠ 0` steps that has had `u` updates is at `start +
u·step` per axis (`step = (end − start) / s`, `idiv`), colour `0xA0 | ((place + u) mod 7 + 1)`, and
is removed by the first walk past `born + s` (`0x473B30`, signed). Its start and end are
tagpu_prand's draws 0..2 and 3..5 against the two boxes (`base + r·extent / 0x8000`, the 64-bit
product toward zero). *Fire and the flare* (`tagpu_pevt_sprite_at`; the engine map's *Fire and the
teleport flare*): fire's particle k, k = 0 … life, is born by the update of T + k, the flare's by
that of T + 10k while 10k ≤ life and the step count is 9 or more (with fewer, the walk of T + 10
finds the object empty first); each is at `from + u·step`
after `u` moves, moves from the update after its birth (particle 0 from the update of T), and is
removed by the first update past its end — T + life for fire, its birth + the step count for the
flare. Fire's frame is `(u / period) mod (count − 1)`, the flare's `(draw 0 of tagpu_prand(seed, k,
0) · (count − 1) / 0x8000 + u) mod (count − 1)`. An event emitted after its GameTime's walk — a
replay's frame visit re-firing an emitter between two steps, or the pieces' fire, which the
explosion draw emits — is `LATE`: particle 0's first update is T+1 (and nano's burst 2 always
exists). *The wake* (`tagpu_pevt_wake_at`; the engine map's *The wake and the bubbles*): dot 0 is
born at T, dot 1 by the update of T + 1, which exists when the walk of T left dot 0 or the emission
was late; a dot starts at a with draws 0..2 of `tagpu_prand(seed, k, d)` adding `r · 7 / 0x8000 − 3` to
the x, altitude and y high words, is at `start + u·step` after `u` moves, its colour `0x61 + (u /
period) mod 7` rising (the wake) or `0x67 − (u / period) mod 7` falling (the bubbles), and is in the
object from its birth to its death walk. D, the walk that destroys the object (slot 3 finds the
sub-vector empty), is `tagpu_pevt_nano_finish` / `tagpu_pevt_sprite_finish` /
`tagpu_pevt_wake_finish` / `tagpu_pevt_smoke_finish`: for fire T + life + 2, for the flare the last
particle's birth + its step count + 2, for the wake its last dot's death walk + 1, the first walk
when nothing was made; for Smoke2 the later of the walk after its last puff's removal and T + life
+ 1 (its empty also wants `+4` past); Smoke1 has none.

*Smoke* (`tagpu_pevt_puff_*`; the engine map's *Smoke2 and Smoke1*): puff j is the object's spawn
ordinal — puff 0 at the emission, puff j ≥ 1 by the update of `again + (j − 1) · interval`, after
that update's moves, while Smoke2's `+8 ≤ T + life` holds — and is first moved at the walk after its
spawn (puff 0 at `first`). It starts at the point with j's low bits in its position's low bits
(§7.7), its frame count `2 + draw 0 · (cap − 2) / 0x8000`; after u updates it has moved by the sums of
the wind (`<< 3`) and the rise (`<< 2` Smoke2, `<< 4` Smoke1) over the walks of those updates, and its
frame is found by walking its countdowns: frame 0 lasts `reset` updates, frame f `h + draw f · h /
0x8000` (h = reset / 2), and the update that reaches `nframes` removes it — at most the frame cap's
draws, so the walk is bounded. Its countdown is what is left of the frame's. All of it is the
engine's integer arithmetic; only the wind's history is not in the event.

**The smoke's timing and the wind's sums.** A puff moves by the wind and the rise its walk reads,
and puff 0's first walk is whichever walk runs first after the emission: the same tick's in the
step, the next tick's for an emission after the walk (the explosion draw's pieces, a replay's frame
visit), and the level's first walk for a vent's `SmokeInfinite` at the map's load — an emission that
sees GameTime 0 (the loader stores it at `0x4971BB` before the map load) while its first walk comes
only after the load and the catch-up ticks. So the start of every particle walk
(`pevt_walk_begin`, before the engine's slots run) sets the timing of the smoke events emitted since
the last walk — `first` that walk, `again` the first walk from it at or past `T + interval` (the again
tests' unsigned compare), Smoke2's puff count and D — and notes the wind's x and y and the rise into a
ring of prefix sums, one row a walk (`TAGPU_PEVT_WIND`, 1 024 rows). A puff first moved at walk F is,
after the walk of P, at its start plus row P less row F − 1. A walk with no held row for the tick
before it (the level's first; after a seek, when the ring holds none for the target's tick) starts the
sums again, and no row from before it is read: a puff that needed one is not drawn (none can be: a
seek clears the events). A row kept from before a seek is a consistent base, since the ticks before
the seek's target are the same on both sides of it. The frame
packet carries the rows its smoke events reach back to, and an event whose puff could outlive the
ring is not made.

**The wake's death, found at emission.** The engine removes a wake dot at the first update past its
birth + 6 · period, or at the first whose move leaves it where the ground's height `0x485070(&pos)` is
at or above the sea level `main+0x1427F`. A dot moves on a straight line, so the game thread finds
that walk once, when the event is made: it asks `0x485070` itself for every walk the engine would
until one answers death (`pevt_wake_gone`, at most 6 · period + 1 lookups a dot, as many as the
engine makes for it over its life), and the event carries it, so no renderer needs the terrain. That
rests on the terrain not changing in play: the heights are written only by the map's load, and the
sea level by the load and by the console's `SeaLevel` cheat; after each walk the byte is compared
with the one the live dots were marched at, and a change finds every live dot's death walk again
from that walk on, and for an event emitted that tick whether its dot 1 exists (dot 0 surviving the
walk of its own tick decides it), before the walk's purge (`pevt_wake_resea`, marked `SIM_UNTESTED`:
no test group can change the sea level, since the recorder leaves the byte) ([map](engine/map.md), *The two lookups
bound themselves*).

**The list** is presentation state on the game thread (`SIM_PRIVATE`): emitted at the alloc,
purged after each particle walk (`particle_walk` → `pevt_walked`) of every event past its D and by the
layer cap's trim, cleared at a seek (`tagpu_effects_clear`) and at each level's start
(`prand_level_begin`). A Smoke1 event has no D: it ends only by its layer's trim or the level's end,
as the engine's object, which never answers empty, does. Its layer table pointer `main+0x38D77` is
NULLed by the teardown `0x471DE0`, and the packet copies no event while it is NULL, so a level's
vents are not drawn over the next level's loading screen. The list holds `6·(TAGPU_LIM_SFX + 1)`
rows of 88 B, each with its 10-B box beside it in an array of its own (about 12.0 MB in the raised
build): the six layers the stock callers use, each at its cap plus the
one the trim leaves; only events in a seventh layer could fill it, which no caller makes, and then
`pevt_room` drops the oldest sixteenth of the list (`PEVT_CAP / 16` rows) at once, counts them in the
level's `dropped` and logs once. **The layer cap's trim and the full list's squeeze run only on the
stock-limits build** (`pevt_trim`, `pevt_room`, each asked before every add): no game of the raised
build reaches its cap of 20 480 objects in one layer, nor the about 61 000 live events that fill its
list with the walk's squeeze (the 13 500-unit battle peaks at 16 118 in one layer and 35 903 in all,
MEASURED 2026-11-22), so the replay test groups hold them on the stock-limits build alone, from the
same source. **A removed row stays in place until the list is squeezed**
(`pevt_squeeze`: the live rows moved down in order, with their boxes): events end anywhere in the
list, and no walk ends a Smoke1 event near its front (only the layer cap's trim or the level's end
does), so the head alone does not shorten it. After
each walk's purge the list is squeezed when it holds more removed rows than live ones, which copies
each live row once for at least as many removals, so every walk over it (the purge, the packet's
copy) is at most twice the live rows plus what was emitted since. In the 13 500-unit battle the
list had spanned about 70 000 rows for 18 000–30 000 live ones (MEASURED 2026-11-22).

**Publishing**: the frame packet's fill copies the live events whose particles can reach the view
(each event's box of world x and `y − alt/2`, widened by the reach slack and, for a sprite, 128
world pixels for its frame) into the slot, in the list's order, newest kept when the slot runs out
(`TAGPU_PK_TRUNC_PEVT`, `EVENTS-TRUNCATED` in the `sfx:` line). The boxes are made at the event's
emission and kept apart from the rows (`PevtBox`, 10 B: the box and its kind, plain, smoke or
removed), so the cull walks the boxes and reads a row only when its box meets the view; a smoke box
is tested against the view moved back by the drift's reach, which is the same test. One walk writes
the records it meets and counts them; only when they are more than the slot holds does a second walk
skip the oldest (`tagpu_pevt_copy`). The fill also carries the frame handles of the three
sprite sequences (`pevt_frame`: the flare sequence, "smoke 1", "smoke 2", the first 64 of each),
and the wind's rows, the newest kept when the slot cuts them (the same bit; the rows cut are counted
in the fill's need, so the slot grows and the refill fits them). A smoke event is copied once for every ten of its puffs that can be alive at the
packet's GameTime, each copy naming its first puff (`pbase`); its reach is its point widened by the
least and most the wind and the rise have carried a puff over the level's longest puff life. The
acquire validates the table, every layer and class, a
sprite event's particle count, frame modulus and period, a wake event's dot count and period, and a
smoke event's interval, reset, frame cap, sequence and puff count. The
packet's GameTime is the P the GPU
computes at. The `packet:` heartbeat's `pevt=last/most/truncated`. **The thread split**: the game
thread alone reads and writes the list; the render thread's gather copies the packet's table into
`tagpu_fx.c`'s own storage (`tagpu_fx_set_events`, sized to `TAGPU_PK_MAX_PEVT`) and puts each
sequence frame through the effects atlas there (size, hotspot, atlas rect: the lookup `emit_sprite`
makes) while it holds the packet, before `tagpu_packet_frame_end`; the Vulkan pass, which prepares
after that, reads only the copy — the ordering every other part of the effects hand-over (`s_verts`,
the LHT) rests on. A frame not drawable that frame (not painted yet under the atlas's allowance, a
compound frame, the alpha blit not built: `TAProgram+0xF0` bit 5) has width 0, and its particles
are not drawn, as the native pass drops such a sprite.

**The draw** (`tagpu_fx.c` NVS/NFS, `tagpu_vk_fx.c` `s_pipeEvt`): one draw of `60·n` vertices with
no vertex input before the lines (the events of layers 0..6, sorted first) and one after the sprites
for the events of layers 7..9; vertex v is corner `v mod 6` of particle `(v / 6) mod 10` of event `v / 60`,
read from a storage buffer at binding 1 that holds the frame table (three sequences of 64 rows of 8
words), the wind's rows (a header and 1 024 rows of 4 words) and then the events. The vertex stage is `tagpu_pevt_nano_at`, `tagpu_pevt_sprite_at`, `tagpu_pevt_wake_at`
and the puff's closed form in GLSL, statement
for statement (`umulExtended` for the 64-bit product, a sign-split division toward zero); a particle
not in the engine's object is a degenerate quad. A nano dot is `DrawBar`'s 2×2 at the engine's
projection; a fire, flare or smoke particle is its frame's quad at that point less
the hotspot, the quad `emit_sprite` makes, at 50 % alpha through FS's alpha path (the base atlas's
key, the restored twin where it is painted); all at their layer's key (§7.5: 0..2 `0.30`, 3..4
`0.60`, 5 and 6 `fxKey − 2`, 7 `encFx + fxBand + 0.5`, 8 `airKey + 3`, 9 `airKey + 5`). The fragment stage
applies **the engine's sight test exactly** (`0x473A00`, `0x474170`, `0x473590`, Smoke2's
`0x475470`; Smoke1's draw `0x475700` has none, and nor has its event): the tile `(hi(x) >> 5, (hi(y) −
(hi(alt) >> 1)) >> 5)` read from the slot's fog source (`inc/tagpu_fogsrc.h`: bit 1 out of LOS in
true-LOS mode, else bit 0 never explored), off the map hidden — not the smoothed fog sample of the
other effects. Smooth motion (`tagpu_lerp.on`) blends only unit piece poses, so particles are drawn
at the packet's tick. The `sfx.on` tokens apply: `nonano` the nano
events, `nofire` fire and the flare, `nowake` the wake, `nosmoke` both smokes, `passive` all.

**The oracle, `tagpu_particlecheck.on`** (read at install): the pool keeps its capacity, is asked as
in stock and the init runs, so every object is made in full and walked by the engine's own code, and
an event is recorded only for an object the pool gave: both sides hold the same emissions. After each walk every live object of the
three classes is paired with its layer's next event of its class, by seed, and every sub-particle's
record is held to the closed form: nano's position (both words), end, step, colour and end tick;
fire's and the flare's position (both words), step, frame modulus, frame, end tick and fire's
counter and period; a smoke puff's sequence, position (both words), frame count, frame, reset and
countdown; and the count. An event whose object is gone while a later one is there is
*missing*, an object with no event *extra*; the events' own trim and pool count make every object an
event, so either is a difference. A line a tick: `pcheck: T= objects N (missing extra)
particles M differing D [seed count pos end step colour tick frame] nano a/b fire c/d flare e/f
wake g/h smoke2 i/j smoke1 k/l level: …, wake dots dead over ground w`, the first eight differences of a level in detail, and the level's totals by class at the
next level's start. At a level's first check **the oracle tests itself**: a planted word unlike its
form goes through the comparison every record goes through (`pevt_self_test`), which must count it
and detail it (as object 0, which is no object's), and logs `pcheck: the oracle's own test: a planted
difference is found` (or `IS NOT FOUND`); it spends one of the level's eight detail lines. Under the oracle the events are drawn and the engine's objects are not (the
`0x471F90` detour, §7.5). The replay test group `particle-check` plays the battle under the lever and ends
with `tools/replay/particlecheck_check.py`, which fails the test on a differing particle, a missing
or extra object, an oracle that did not arm, an own test that is missing or did not find its planted
difference, or no fire particle compared; `stock-particle-check` plays the same game on the
stock-limits build, where the layer cap (400) trims and the oracle holds the trims too. The test group
`particle-check-sea` plays ships and submarines patrolling beside Anteer Strait's shore
(`scenarios/replay-wake-shore.json`) under the same lever and fails, by the same rules, when no wake
particle was compared (`--classes wake`). The wake's oracle compares each dot's both words of its
position, step, colour bounds, colour, delta, counter, period and end tick. The test group
`particle-check-smoke` plays a battle on Two Continents beside one of its six vents
(`scenarios/replay-smoke.json`: rockets, hammers, burning trees, units left at health 12 on hold)
and fails when no Smoke2 or no Smoke1 particle was compared (`--classes smoke2,smoke1`).

**MEASURED 2026-11-22.** *Nano*, the 13 500-unit battle (`tools/battle13k.py`'s game, 1920×1080, the
oracle armed): GameTime 1 to 3 622, 159 512 073 sub-particles compared, 0 differing, 0 objects
missing, 0 extra, up to 8 362 live nano objects and 73 455 sub-particles in a tick; the packet
carried at most 753 events in view, never truncated. Across a live rewind (`nano-factory`, the
plant building and the commander guarding it; rewind opened at 1 184, sought to 884, forked): 1 783
ticks checked, 0 differing, 0 missing or extra; at the seek the events and the engine's objects are
both gone (the paused frame draws neither) and both come back once play resumes. *Fire and the
flare* (`7c4eb930`): the same battle, GameTime 1 to 2 885: 1 066 702 fire particles (the pieces'
fire, layer 9) and 118 192 278 nano particles compared, 0 differing, 0 missing, extra or trimmed;
`nano-factory` with eight Atlases built and flying (their `ProcessFlames`, COB `EMIT_SFX` 0, layer
7), a live rewind opened at 1 786, sought to 1 486 and forked, to GameTime 2 553: 3 139 ticks,
15 522 fire and 466 122 nano particles, 0 differing; `replay-teleport` with eighteen teleports of
four riders 480 to 640 world units: 3 684 ticks, 35 008 flare particles, 0 differing, 0 missing or
extra. `tareplay.py run battle teleport --cover`: both test groups pass. Against `tagpu_gdi.on`, same
scenarios at zoom 1 (Atlases flying on Two Continents, a teleport on Zombie Kingdom): fire and the
flare stand where GDI puts them, frame for frame of the sequence; their colour is the restored
twin's, as the gathered sprites' was, where GDI shows the palette's dithered frame. In the 13 500-unit battle
(battle13k, 1920×1080, two runs each, the base fd7d5a86 against `7c4eb930`): the game thread's
`particles` 2.27/1.84 → 1.99/2.10 ms a tick, `publish` 2.55/2.35 → 2.36/2.41; the render thread's
gather median 4.39/3.78/3.80/4.36 → 4.76/4.74/4.75/4.73 ms in alternated runs, but
`tagpu_fx_set_events` itself costs 3 to 4 µs a call in that battle and a lone run of this build
gathered at 3.55 ms, so the gap follows the run order, not the code.

*The wake and the bubbles* (`replay-wake-shore`: four patrol boats and a destroyer beside the shore,
two submarines under water, the oracle armed): a live rewind opened at 1 544, sought to 1 243 and
forked, to GameTime 2 698: 3 046 ticks, 780 974 wake particles compared, 0 differing, 0 missing,
extra or trimmed, the bubbles' proxies (layer 7) 3 to 15 at a time. With the console's `SeaLevel`
typed twice (79 at GameTime 1 011, 70 at 1 868), to 2 645: 2 657 ticks, 623 026 wake particles, 0
differing; the two changes found 238 live dots' walks again (none moved) and 358 (26 moved), and 30
dots died over ground. `tareplay.py run battle teleport particle-check particle-check-sea --cover`:
290 tests pass, 0 fail (`particle-check-sea`: 1 150 ticks, 233 771 wake particles, 0 differing).
Against `tagpu_gdi.on` (the same scenario at zoom 1, GameTime about 490): the trails have the same
one-pixel dots, the same rising colour and the same spacing behind each boat
(`.data/gpu-particles/l4/wake-pair.png`, uncommitted). In the 13 500-unit battle (1920×1080, the base
9b07aca0 and this build alternated, two runs each): the game thread's `particles` 2.06/1.89 →
0.84/0.86 ms a frame, `publish` 2.50/2.54 → 1.73/1.77, the render thread's gather median 4.74/4.74
→ 3.41/2.85 ms, 18.2/18.4 → 19.6/19.7 frames a second.

*Smoke* (`scenarios/sfx-smoke.json` on Two Continents, its six vents and damaged units, the oracle
armed): a live rewind opened at 1 677, sought to 1 377 and forked at 1 200, to GameTime 2 838: 3 323
ticks, 54 172 Smoke1 and 23 314 Smoke2 particles compared, 0 differing, 0 missing, extra or trimmed;
the vents ran 1 677 ticks before the rewind, so each made about 335 puffs (past the 256 of a bare
8-bit ordinal) across at least three changes of the wind (sampled 551 to 1 677: 747, 960, 1 366). `replay-smoke` (the battle beside a vent), a live
rewind opened at 1 750, sought to 1 450, forked at 1 200, to GameTime 2 900: 3 458 ticks, 373 928
Smoke2 and 56 713 Smoke1 particles, 0 differing, 0 missing, extra or trimmed. A seek clears the
layers, so the vents' smoke, made at the map's load, is gone after a rewind on both sides of the
oracle. `tareplay.py run battle teleport particle-check particle-check-sea particle-check-smoke
--cover`: 343 tests pass, 0 fail (`particle-check-smoke`: 1 400 ticks, 197 173 Smoke2 and 45 176
Smoke1 particles, 0 differing; `particle-check`'s battle: 554 856 Smoke2 and 31 517 Smoke1 beside its
fire). Against `tagpu_gdi.on` (the same scenario at zoom 1, GameTime about 830): the vent's column
stands where GDI's does, with the same puffs and frames; the battle's smoke has GDI's sprites and
blend (`.data/gpu-particles/l5/smoke-pair-vent.png`, `smoke-pair-battle.png`, uncommitted; the two
battles are not the same frame). In the 13 500-unit battle (1920×1080, the base 9266b98d and this
build alternated, two runs each): the game thread's `particles` 0.86/0.87 → 1.20/1.21 ms a frame (the
smoke inits now record events and each walk notes the wind), `publish` 1.73/1.80 → 1.45/1.46, the
render thread's gather median 3.27/3.39 → 1.94/1.96 ms, 19.6/19.2 → 20.0/19.8 frames a second; no
packet truncated.

*The engine's particles off* (the pool at 0, every class from the alloc). Under the oracle, the replay
groups on the coverage build: `particle-check` 1 080 ticks, 106 607 fire, 610 978 Smoke2 and 40 274
Smoke1 particles; `particle-check-sea` 1 380 ticks, 254 593 wake particles, 10 dots dead over ground;
`particle-check-smoke` 1 620 ticks, 203 783 Smoke2 and 52 212 Smoke1; 0 differing, 0 missing or extra
in each, now that no object is excused as trimmed. Under the oracle on the other classes:
`nano-factory` with eight Atlases flying (COB fire, layer 7), a live rewind opened at 1 793, sought to
1 492, forked at 1 200, to 2 545: 3 143 ticks, 450 854 nano, 15 186 fire and 57 969 Smoke1
particles; `replay-teleport`, twelve teleports: 2 550 ticks, 23 600 flare particles; `replay-smoke`
on the stock-limits build (its pool of 1000, its layers of 400), a live rewind opened at 1 378, sought
to 1 077, forked at 900, to 2 238: 2 725 ticks, 321 413 Smoke2 and 44 542 Smoke1 particles; 0
differing, 0 missing or extra in each. `tareplay.py run particle-check particle-check-sea
particle-check-smoke battle teleport stock-limits --cover`: 493 pass, 0 fail, every Vulkan lane's
install line naming the empty pool. Without the oracle on Vulkan: a save and a load on `sfx-strait`
(GameTime 803, loaded at 1 440) and a live rewind on `replay-smoke` (opened at 1 220, sought to 919,
forked at 900, played to 1 735): the events keep coming after each (7 to 12 in view after the load,
52 to 168 after the fork), no `ErrorLog.txt`. **The address space**, the same scene at the same
point (`replay-smoke`, 30 s after the load, GameTime 950 to 960, `/proc/<pid>/status` of the game's
Wine process): `VmData` 730.6/731.8 MB on the base 5a820806 against 704.9/704.8 on this build, 26.4
MB less — the pool's 15.6 MB block and 0.8 MB stack never allocated, and the effects pass's under
bucket, the sprite bucket's particle share and the packet's particle scratch gone (11.0 MB of static
arrays: 2 × 6 × 24 576 vertices of 36 B, and 24 576 × 16 B; measured on da3415e4, before the sprite
bucket took back 5.3 MB as its allowance for composite frames, so about 21 MB at this build); `VmSize`
22.6 MB less; the resident set the same (607 MB), since a pool the game never
fills is mostly untouched pages. The largest free range below 4 GiB moved between runs of one build
by more than between builds (332 to 348 MiB), so it is not a measure of this change. In the 13 500-unit
battle (1920×1080, the base 5a820806 and this build alternated, two runs each): the game thread's
`particles` 1.14/1.15 → 0.37/0.38 ms a frame, `publish` 1.40/1.40 → 1.31/1.31, the render thread's
gather median 1.95/1.97 → 2.31/2.29 ms (open: [the plan](gpu-particles-plan.md)), 20.3/19.9 →
20.3/20.4 frames a second; against main c3ddc579, the feature's whole: `particles` 3.19 → 0.36,
`publish` 2.91 → 1.33, gather 5.27 → 2.31, 17.8 → 19.8 frames a second.

**The blend order is the engine's.** The engine draws layer by layer, each in its vector's order,
and every emission appends to its layer's vector; the upload puts the events in layer order and in
emission order inside a layer (`tagpu_vk_fx.c`), and no effect writes depth, so the particles blend
in the engine's painter order. Each layer's depth key still puts it over or under everything else as
before.

**Not covered here**: an event's particles are culled by their event's box, not each by its own; a
frame whose packet carries no fog source draws the particles without the tile test. A wake
dot dies on the terrain its death walk found at emission: a sea level changed later (the `SeaLevel`
console command) marches every live dot's walk again at the next tick, which no replay test group
can reach, since the recorder leaves the sea byte. Smoke drawn from events stands at the sight
test's tile of its own puff, and a puff whose wind rows a seek has cleared is not drawn, but a seek
clears the events too, so no live puff meets that.

## Appendix — addresses

| VA | What |
|---|---|
| `0x49BE60` / `0x420B00` | projectile / explosion pass, `stdcall(ctx)` `ret 4`; call sites `0x469B22` / `0x469B2C` |
| `0x46BAE0` | generic 3DO draw (models, debris), `ret 0x10`, callers = the two passes only |
| `0x4211D0` / `0x421550` | debris piece draw (`ret 0xC`) / particle update, caller chain `0x420B00 → 0x421550 → 0x4211D0` |
| `0x4B8EC0` | LHT flash blit, `ret 0x10`, caller = explosion pass (+ itself) |
| `0x4B9360` | rendertype-2 refraction sprite, `ret 0x10` |
| `0x4B7F90` / `0x4B8500` | `CopyGafToContext` / `AlphaCompsteBuf2OFFScreen`, `ret 0x10` |
| `0x4B7F30` / `0x4B7EE0` | `GAF_SequenceIndex2Frame` / `GAFGetCurrentFramePtrAddr` |
| `0x4B6CC0` / `0x4B7173` | vertex rotate by triple / 2D rotate |
| `0x4BE950` / `0x4BEA20` / `0x4CC7AB` | `DrawLine` / clip / Bresenham |
| `0x4E4870` | `rand2` = CRT `rand()` (per-thread `_getptd` seed): the lightning jitter here, and in stock twenty particle sites (§7.3, §7.4, the feature tick's two), which Impure answers from its keyed rule instead (§7.7); 61 sites in all ([sim](engine/sim.html), *The two random streams*) |
| `main+0x141F3/0x141F7` | projectile count / array (stride 0x6B) |
| `main+0x1491B/0x1491F` | explosion count / inline array (stride 0x54, 300) |
| `main+0x38A47` | game tick |
| `main+0xDCB` | weapon colour → palette index byte table |
| `main+0x1480F` / `0x147BB..0x147CB` / `0x147F3` / `0x1AB9B` | shadow-blob seq / five sprite-weapon seqs / flare seq / refraction frame |
| `main+0x37E27` | viewport rect `{l,t,r,b}` |
| `0x511DF0..0x511F80` | flying-debris particle slots |
| `*(0x51FBD0)` | `TAProgram`: `+0xC0` ALP, `+0xC8` LHT, `+0xF0` capability bits |
| `0x471F90` | particle-layer draw walker `(ctx, n)`, `stdcall` `ret 8`; ten sites in `DrawGameScreen` (§7.1) |
| `*(main+0x38D77)` | ten layer vectors `{flag, begin, end, cap}` × `0x10`; `0x471D90` alloc, `0x471DE0` destroy-all, `0x4732E0` insert |
| `0x51E610` / `0x51E608` | particle pool (`0x470EB0` pop / `0x470ED0` push) / global disable byte |
| `0x4FD638 / 0x4FD618 / 0x4FD5D8 / 0x4FD588 / 0x4FD5F8 / 0x4FD5B8 / 0x4FD5A8` | vtables: Smoke1 / Smoke2 / fire / flare sprite / wake / nanolathe / base (slots 0 destroy, 1 update, 2 draw, 3 empty?, 4 spawn, 5 again?, 6 init) |
| `0x475700 / 0x475470 / 0x474170 / 0x473590 / 0x4745E0 / 0x473A00` | per-particle draw leaves in the same order |
| `0x472810 / 0x4728F0 / 0x472C50 / 0x4729D0 / 0x472AB0 / 0x472330 / 0x472530 / 0x472430 / 0x4720D0 / 0x472200 / 0x471FD0 / 0x472630` | emitters (§7.4) |
| `0x4BF6F0` | `DrawBar(ctx, rect, colour)` — the 2×2 dot |
| `main+0x147CF / 0x147D3 / 0x147F3` | smoke sequences "smoke 1" / "smoke 2" / the flare sequence fire uses |
| `main+0x37ECC / 0x37ED4 / 0x14263` | wind x / wind y / smoke rise per tick |
