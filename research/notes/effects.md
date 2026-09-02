# Effects — weapon fire, explosions, debris (the effects pass)

*2026-09-02. Reverse-engineered from the pristine exe (Ghidra decompiles of `0x49BE60`,
`0x420B00` and their leaves; `tools/ghidra-scripts/DecompileTAFuncs.java`), verified live
in a skirmish through `tagpu_fx.c`'s gather log, then owned end-to-end (`tagpu_fxown.c`).
Struct names follow TADR's `tamem.h`; every offset below was read back from a running
game.*

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
3. **An explosion record carries up to three things**: a debris 3DO (`+0x00`) rotated by
   `+0x4C`, an opaque sprite animation (anim state `+0x04`) and a *light flash* (anim
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
   piece is `*(sys+0x2C)`: `{Model3DONode* @0, short turn[3] @0x12, i32 16.16 x/alt/y
   @0x16/0x1A/0x1E}`.
2. **Flash loop** over `ExplosionStruct[]` (count `main+0x1491B`, inline at
   `main+0x1491F`, stride `0x54`, 300 slots): anchor `+0x1C/+0x20/+0x24` (16.16 x/alt/y)
   inside the viewport rect `main+0x37E27`, and `+0x18 != 0` → frame of anim state
   `+0x10` → `0x4B8EC0`: **LHT blit**, `dst = LHT[(src − 0x4F)·256 + dst]`, gated by
   `TAProgram+0xF0` bit 7. The flash sequence is a short-lived object: its memory is
   freed when the explosion ends (a later read of a dead record's `+0x18` finds
   unreadable frames — read it only while `i < count`).
3. **Body loop**: per record, `+0x00` debris node → `0x46BAE0` rotated by `+0x4C`; then
   anim state `+0x04` (`seq @+0x0C`) → `CopyGafToContext` (opaque, colour-keyed).
   Explosion sequences seen live: `Explode2/3/5`, `Explosion` (19–23 frames, frame 0 as
   small as 4×3, RLE).

## 3. The blit leaves [BINARY-VERIFIED]

| Function | Rule |
|---|---|
| `CopyGafToContext 0x4B7F90` | colour-keyed copy (`ColorKey @+8`); sub-frame lists recurse, a sub-frame with `+0x0B != 0` goes through the alpha blit instead. `ret 0x10` |
| `AlphaCompsteBuf2OFFScreen 0x4B8500` | `dst = ALP[src·256 + dst]` (50 % blend, `TAProgram+0xC0`), gated by `+0xF0` bit 5 |
| `0x4B8EC0` | LHT lighten as above, gated by bit 7. `ret 0x10` |
| `0x46BAE0(ctx, pos16.16[3], node, turn[3])` | rotate `node+0x24` verts by the triple, project `sx = hi(vx+px) + 0x80`, `sy = hi(py − vz) − hi(vy+palt)/2 + 0x20`; faces `node+0x28` (stride 0x20): skip face 0 when `node+0x0C` (selection primitive) `!= −1`; flag bit 0 → flat fill `0x4C0330` in the face colour; else **only 4-vertex faces** → `GAF_DrawTransformed` (texel copy, **no shade table**; bit 1 → current anim frame of `face+0x10`). `ret 0x10` |
| `0x4211D0` | the same face rules for a debris piece (team-colour frame via bits 1+2). `ret 0xC` |
| `0x4B6CC0(in, out, turn)` | `Rz(t0)` on (x,y), then `Rx(t2)` on (y,z), then `Ry(t1)` on (x,z); each `0x4B7173`: `a' = a·cos − b·sin, b' = a·sin + b·cos`, angle = `t·2π/65536` |
| `DrawLine 0x4BE950` | clip to the context rect (`CorrecLinetPosition`), Bresenham in the palette index |

GAF frame header (0x18): `u16 w@0, h@2; i16 hotX@4, hotY@6; u8 ck@8, compressed@9,
subframes@0xA, subAlpha@0xB; u8* pixels@0x10`. RLE rows: `u16 length`, then codes
`b&1 → skip b>>1`, `b&2 → repeat next byte (b>>2)+1`, else `(b>>2)+1` literals.

## 4. The native pass — `tagpu_fx.c` + `tagpu_native.c`

Armed by `tagpu_fx.on` (tokens `log`, `nolines`, `nomodels`, `nosprites`, `noexpl`,
`nodebris`). It rides the native unit frame: `tagpu_native_frame` builds the view
(eye, viewport, fog textures, palette), calls `tagpu_fx_gather`, and:

- **Models** (rockets, missiles, shells, debris, flying pieces) are emitted through the
  unit geometry path — `emit_node`, the face loop shared with units, fed raw node
  vertices rotated by the engine triple exactly as `0x4B6CC0` does. Unshaded (neutral
  SHD row), selection-primitive face skipped, textured faces quads-only: the
  `0x46BAE0` rules. Depth band `FX_ENC_MODEL = 400 ± 1.8`.
- **Lines and sprites** are the module's own program: a private 2048² R8 atlas (raw,
  RLE and sub-frame frames decoded on first use), palette lookup, four modes — flat
  colour (lines, `GL_LINES` at supersample width), opaque colour-keyed, 50 % alpha
  (the ALP blend in RGB), and **flash** = additive with a per-level colour derived from
  the live LHT table (mean palette delta of each of the 32 rows; an RGB approximation of
  a palette-space remap). Depth 403, depth writes off, drawn after the unit bodies so
  aircraft (band 1201) cover them and nothing else does; the same fog rule as units
  (unexplored discarded, explored-out-of-LOS darkened). Three fixed buckets drawn in
  order — lines, flashes (additive), sprites — so explosion sprites sit over their flash
  as in the engine and nothing is dropped however often kinds alternate.
- Gates mirrored: `+0x60 == 0`, the per-projectile LOS/MAPPED test, the viewport-rect
  test for explosions and pieces, `TAProgram+0xF0` bits 5 and 7.
- **Depth bands are per frame**, not constants: `fxKey = 3 + (rows + 8)·4 + 4` sits above
  the last row key a gathered unit can carry (`rows` = the sweep's row count, +8 for the
  ±256 px gather slack), the air band is `fxKey + 12`, sprites at `fxKey + 3`, and the
  vertex shaders divide by `airKey + 8` instead of a fixed 512 — so a 1440p or 2160p
  viewport keeps the order ground → effects → aircraft.
- **The native FBO became premultiplied** for the additive flash (blend `ONE,
  ONE_MINUS_SRC_ALPHA` in the FBO and at the composite; the unit FS outputs `rgb·a`). Side
  effect, deliberate: the unit pass's half-alpha draws (shadows, cloak) now land at the
  engine's 50 % instead of the 25 % the straight-alpha path had silently produced
  (`a·a` at the composite) — shadows and cloaked units are a little darker than in the
  G12c panels, and match the ALP-table blend.

Log every 60 frames: `fx: proj=N (laser= model= sprite= flare= light= ball= hidden=
fogged=) expl=K flash= debris= -> lines= sprites= flashq= models= atlas=`; with `log`,
the first records of each kind every 60 frames (`fx: p0 "EMG" rt=4 …`, `fx: e0
seq="Explode3" …`).

## 5. Owning the draw — `tagpu_fxown.c` [LIVE-VERIFIED]

Installed once at DllMain when `tagpu_fxown.on` exists (tacli auto-creates it at launch
when `tagpu_fx.on` is set); all six sites are byte-matched first, all-or-nothing:

| Site | Patch | Effect while the skip byte is set |
|---|---|---|
| `0x469B22` `call 0x49BE60` | call-site redirect | the projectile pass returns at once (`ret 4`) |
| `0x469B2C` `call 0x420B00` | call-site redirect | the pass runs (particle updates keep emitting smoke/fire) bracketed by an *inside* byte |
| `0x46BAE0` | 5-byte prologue detour | `ret 0x10` — no projectile/debris models |
| `0x4B8EC0` | 6-byte prologue detour | `ret 0x10` — no flashes |
| `0x4211D0` | 5-byte prologue detour | `ret 0xC` — no flying pieces |
| `0x4B7F90` `CopyGafToContext` | 6-byte prologue detour, checks the *inside* byte | no explosion sprites; every other caller (features, UI) unaffected |

The skip byte is set by each frame the native effects pass actually gathers (never by
the file alone), cleared by `passive`, by the file's absence, and by a 90-frame heartbeat
timeout in `tagpu_fxown_flush` when the pass stops running (overlay off, GL failure) —
so the engine's draw comes back instead of effects vanishing. The explosion-pass stub
copies the skip byte into the *inside* byte, so the shared `CopyGafToContext` skips only
while the skip is on. Log: `fxown: engine effects draw SKIPPED / restored`, `FXOWN skip=
in=` every 300 frames; an engine-vs-ours A/B is a file flip on one instance, no relaunch.
Verify install with `fxown: ARMED site.proj=1 site.expl=1 model@0x46BAE0=1
flash@0x4B8EC0=1 piece@0x4211D0=1 copy@0x4B7F90=1`. tacli drops a stale `fxown.on` at
launch when `fx.on` is absent.

Caveats: while only some unit types are native (`native.on=armcom` or no unit pass at
all), engine-drawn aircraft sit in the 8bpp frame *under* our composited effects — the
reverse of the engine's order; with `native.on=all` the depth buffer restores it. The
rendertype-2 refraction ball is dropped while owning.

Follow-ups noted by review, not done: the GAF RLE decoder and shelf atlas duplicate
`tagpu_scaffold.c` / `tagpu_render3do.c` (a shared `tagpu_gaf.c` would serve all three);
the byte-match/stub/detour machinery is now the fifth private copy across owndraw,
tracer, suppress, scenario and fxown; effects models share the unit pass's 49 152-vertex
stream and are emitted last (a `VERTEX-BUDGET-HIT` tag appears in the `native:` log line
when it caps).

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
- Gaps: rendertype 2 (refraction ball) is dropped while owning; smoke/fire/wake/nano
  particles (hook-vector sfx) stay engine-drawn; the flash is an additive RGB
  approximation of the LHT remap; the session was locked, so no 60 fps video — burst GL
  shots at sim speed −6 correlated with the `fx: proj=` log line stood in.

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
| `0x4E4870` | `rand2` = CRT `rand()` (per-thread `_getptd` seed), lightning jitter only |
| `main+0x141F3/0x141F7` | projectile count / array (stride 0x6B) |
| `main+0x1491B/0x1491F` | explosion count / inline array (stride 0x54, 300) |
| `main+0x38A47` | game tick |
| `main+0xDCB` | weapon colour → palette index byte table |
| `main+0x1480F` / `0x147BB..0x147CB` / `0x147F3` / `0x1AB9B` | shadow-blob seq / five sprite-weapon seqs / flare seq / refraction frame |
| `main+0x37E27` | viewport rect `{l,t,r,b}` |
| `0x511DF0..0x511F80` | flying-debris particle slots |
| `*(0x51FBD0)` | `TAProgram`: `+0xC0` ALP, `+0xC8` LHT, `+0xF0` capability bits |
