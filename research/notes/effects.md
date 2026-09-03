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
own passes apply the rule per fragment. Effects *hide* in grey rather than darken, which
is the engine's behaviour for anything that is not terrain furniture: features.md §9.)

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
rendertype-2 refraction ball is dropped while owning. The particle layers have their own
skip byte and detour (§7.5), armed by `tagpu_sfx.on` independently of `tagpu_fx.on`.

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
- Gaps: rendertype 2 (refraction ball) is dropped while owning; the flash is an additive
  RGB approximation of the LHT remap; the session was locked, so no 60 fps video — burst
  GL shots at sim speed −6 correlated with the `fx: proj=` log line stood in. (The
  particle layers, engine-drawn at the time of this section, are native since §7.)


## 7. The particle layers — smoke, fire, wakes, nanolathe [BINARY-VERIFIED, LIVE-VERIFIED]

*Second session of 2026-09-02. Decompiles of `0x471F90`, the seven particle vtables
(every slot), the emitters and the pool; live layout and layer map from `tagpu_sfx.c`'s
gather log; owned through one more detour in `tagpu_fxown.c`. Fixtures
`scenarios/sfx-strait.json`, `sfx-smoke.json`.*

### 7.1 What they are

The effects passes never draw a puff of smoke. Every smoke puff, flame, wake dot and
nanolathe particle is a **pooled object filed into one of ten layer vectors**, and each
layer is drawn by `0x471F90(ctx, n)` — `stdcall`, `ret 8` — at a fixed point of
`DrawGameScreen`. terrain-depth.md §3 used to call these sites "plugin-layer hooks, empty
in stock play"; they are the particle sfx, and **the layer number is the draw depth**:

| Layer | Site | Where in the frame | Seen live |
|---|---|---|---|
| 0, 1, 2 | `0x469849/55/61` | before the flat-feature pre-pass (under every unit and feature) | 2 = **wake foam** (`EmitSfx_Bubbles` class, the ship's trail) |
| 3, 4 | `0x469964/70` | after the flat features, before the row sweep | 4 = **feature smoke** (`SmokeInfinite`, the vents on Two Continents) |
| 5, 6 | `0x469AFD`, `0x469B18` | after the row sweep, before the projectile pass | 5 = **rocket/missile trail puffs**, 6 = **nanolathe spray** |
| 7 | `0x469B38` | after the explosion pass | 7 = **submarine bubbles** |
| 8 | `0x469BD7` | after the airborne sweep | — (nothing observed) |
| 9 | `0x469D2C` | just before the fog overlay | 9 = **impact and damage smoke**, **fire** (burning debris) |

Layer table: `*(main+0x38D77)` = `MEM_Alloc(0xA0)` by `0x471D90`, ten entries of `0x10`:
`{u8 flag @0, void** begin @4, void** end @8, void** cap @0xC}`; `0x471DE0` destroys every
object (vtable slot 0 with flag 1) and frees the vectors at game end. An emitter appends
through `0x4732E0(layer, end, 1, &obj)` (a vector insert with grow-by-copy) after capping
the layer at **400** objects (the oldest is destroyed and the rest shifted down).

### 7.2 The object and its pool

Objects are 76 bytes from the pool at `0x51E610` (`ParticleBase`): `0x470EB0` pops a
pointer from the stack at `+0x14` (`+0x20` = handed out, `+0x1C` = capacity), `0x470ED0`
pushes it back; `+8/+0xC` hold the vector of all objects. The byte `0x51E608` disables
every emitter when set. Layout (`ParticleSystemStruct`):

| Offset | Field |
|---|---|
| `+0x00` | vtable (class, table below) |
| `+0x04` | end tick — `0x471D70(this, life)` = tick + life |
| `+0x08` | tick of the last spawn (`+0x14`/`+0x10` slots re-spawn from it) |
| `+0x0C` | u8 *type tag* (2 for smoke; the fire emitter stores its layer argument here) — **not** the layer |
| `+0x10 / +0x14 / +0x18` | sub-particle vector: begin, end, cap (stride per class) |
| `+0x1C…` | class fields: emission point(s), spread, frame caps, sequence choice |

vtable slots: **0** destroy(flag: 1 = return to pool), **1** update (walk the sub-vector:
move by the per-particle step, advance the frame countdown, compact the dead; then if
slot 5 says so, slot 4), **2** draw(ctx) — the only slot `0x471F90` calls, **3** empty?
(sub-vector empty → the sim walker destroys the object), **4** spawn one burst, **5**
spawn-again? (`+8 <= +4 && +8 <= tick`; Smoke1 never), **6** init(pos…, life). The update
runs from the sim tick (the walker next to `0x471F90`, `0x471EC0..0x471F8F`, calls slot 3
then slot 1 per object), so **the draw is pure** — rendering from the vectors at present
rate reads exactly what the engine would blit.

### 7.3 The classes (the draw side)

Every sub-particle draw projects `sx = hi(x) − eyeX + vpL`, `sy = hi(y) − hi(alt)/2 − eyeY
+ vpT` from **16.16** positions, and (except Smoke1) gates on the local player's LOS at
tile `(hi(x)>>5, (hi(y) − hi(alt)/2)>>5)` — LOS counter with true LOS, else the MAPPED
bit — the same rule as the projectiles.

| vtable | Class (TADR's name) | Sub stride | Per-particle draw | Fields |
|---|---|---|---|---|
| `0x4FD638` | Smoke1 — grey | `0x20` | `AlphaCompsteBuf2OFFScreen` of `seq[frame]`, **no LOS gate** (`0x475700`) | `seq* @0, x/alt/y @4/8/C, nframes @0x10, frame @0x14, countdown reset @0x18, countdown @0x1C` (TADR's `SmokeGraphics`) |
| `0x4FD618` | Smoke2 — dark | `0x20` | same, LOS-gated (`0x475470`) | same layout; sequence `*(main+0x147CF)` ("smoke 1") or `*(main+0x147D3)` ("smoke 2") by the emitter's last argument |
| `0x4FD5D8` | fire | `0x3C` | alpha blit of `seq[frame]`, LOS-gated (`0x474170`) | `seq* @0` = the flare sequence `*(main+0x147F3)`, pos @4/8/C, velocity @0x10.., frame @0x2C, end tick @0x38 |
| `0x4FD588` | flare sprite (teleport / `0x472630` callers) | `0x34` | alpha blit of `seq[frame]`, LOS-gated (`0x473590`) | pos @4/8/C, frame @0x2C |
| `0x4FD5F8` | wake / bubbles | `0x44` | `DrawBar(ctx, {x, y, x+1, y+1}, colour)` = a **2×2 dot**, LOS-gated (`0x4745E0`) | pos @4/8/C, colour byte @0x30 cycling `0x61..0x67` (foam), dies when its ground height rises above sea level |
| `0x4FD5B8` | nanolathe spray | `0x30` | 2×2 dot, LOS-gated (`0x473A00`) | **pos @0/4/8**, target @0xC.., step @0x18.., colour byte @0x28 = `0xA1 + n%7` cycling its low nibble 1..7, end tick @0x2C |
| `0x4FD5A8` | base (destroyed object) | — | nothing | the destructors reset the vtable to this |

Smoke update (`0x475340`/`0x475600`): `x += wind_x(main+0x37ECC)·8`, `alt += main+0x14263·4`
(Smoke1: `·16`), `y += wind_y(main+0x37ED4)·8`; the countdown at `+0x1C` reaches 0 → `frame++`,
reset to `rand()·(reset/2)/0x8000 + reset/2`; a puff dies at `frame >= nframes`. The nano
spray (`0x473D50`) spawns five sub-particles per tick along a randomised segment between
two 6-int boxes set up by its init (`0x473B50`: the 4/11 and 7/11 points of the
builder→site line).

### 7.4 Emitters — who makes them, and the layer argument

| Emitter | Class | Callers (xrefs) |
|---|---|---|
| `EmitSfx_GraySmoke 0x472810(pos, layer)` | Smoke2, grey | `WEAPONS_ProjectileDamage 0x49A007` (impact puffs), `0x421550` (debris pieces), `0x4243CF`, `0x49BC80/0x49BDC1`, `0x4810B9` (damaged-unit smoke) |
| `EmitSfx_BlackSmoke 0x4728F0(pos, layer)` | Smoke2, dark | `0x481118` |
| `EmitSfx_SmokeInfinite 0x472C50(pos)` | Smoke1 | `SpawnFeatureOnMap 0x423FE3` (smoking map features) |
| `0x4729D0(pos)` | Smoke2 | `UNITS_FireProjectile 0x49CC07/0x49CDC3/0x49CFE6` (rocket and missile trails) |
| `EmitSfx_Unk5 0x472AB0(pos, layer)` = **fire** | fire | `0x421550` only (debris on fire: `sys+0x28` bit 0) |
| `0x472330(…, layer)` | fire | `0x481010/0x48102F` |
| `EmitSfx_Bubbles 0x472530(…, layer)`, `0x472430(…, layer)` | wake / bubbles | `0x481102`; `0x48104C/69/86/A3` (the ship movement code) |
| `EmitSfx_NanoParticles 0x4720D0(pos6, to, layer)` / `…Reverse 0x472200` | nanolathe | nine sites in `0x402ABD..0x4151EC` (build) / seven in `0x404676..0x414C43` (reclaim) |
| `EmitSfx_Teleport 0x471FD0`, `0x472630` | flare sprite | `0x406B9F`; `0x48644B`, `ShowExplodeGaf 0x420AE1` |

The layer is **an argument of the emitter** (GraySmoke reads it back from `[esp+0x14]`
after the init call; the fire emitter stores the same argument in the object's `+0xC`),
and each caller passes its own — which is why smoke from a rocket trail (layer 5) sits
under the missile while an impact puff (layer 9) sits over everything. The `+0xC` byte is
otherwise a type tag (TADR's "1 smoke, 2 wake, 6 nano, 7 fire" was read from it).

### 7.5 Native — `tagpu_sfx.c`, and owning the walker

`tagpu_sfx.c` (armed by `tagpu_sfx.on`: tokens `log`, `passive`, `nosmoke`, `nofire`,
`nowake`, `nonano`) walks the ten layers each present frame, classifies every object by
vtable, walks its sub-vector with the class stride and emits **through `tagpu_fx.c`'s
buckets and program**: sequence frames as 50 % alpha sprites (the same atlas, RLE decode
and palette lookup as the explosions), dots as flat-colour 2×2 quads — each with its own
depth key. Layers 0..6 go to an **under** bucket drawn before the lines and flashes (the
engine draws those layers before its projectile pass, so a laser or a flash sits over
trail smoke) and 7..9 are emitted after the explosions into the sprite bucket, so the
draw order reproduces the engine's. Per-frame keys from the native pass (nothing
absolute): layers 0..4 at `0.5` (under every row key — and only the under bucket runs
the scene-scaffold test, the unit shader's rule shared through `tagpu_glsl.h`, so a tall
feature still hides a wake dot behind it), 5 and 6 at `fxKey − 2` (above the last row
key `fxKey − 2.2`, below the fx models at `fxKey ± 1.8`), 7 at `fxKey + 5` (above the fx
sprites), 8 at `airKey + 3`, 9 at `airKey + 5`. LOS gate and the `TAProgram+0xF0` bit-5
alpha gate mirrored; the fog shader darkens them like everything else. A full sprite
bucket or atlas drops the rest of the frame and says so (`fx: DROPPED this frame …`);
a full atlas resets at the *next* gather, never mid-frame.

Owning the draw is one more prologue detour in `tagpu_fxown.c`: `0x471F90` starts with
`mov eax,[0x511DE8]` (five position-independent bytes), so the stub is `cmp byte
[skipSfx],0; jz stolen; ret 8`. The walker is the only draw path (ten call sites, all in
`DrawGameScreen`; the update lives elsewhere), so skipping it removes exactly the pixels
we now draw and nothing else. The skip byte follows `tagpu_sfx.on` live (set by a
successful gather, cleared by `passive`, by the file's absence, and by the 90-frame
heartbeat), independent of the effects skip; tacli auto-arms `tagpu_fxown.on` at launch
when either `fx.on` or `sfx.on` exists. Install line: `fxown: ARMED … sfx@0x471F90=1`.

Log every 60 frames: `sfx: layers L2=44(wake:44) L6=26(nano:26) L9=9(smoke2:9) sub=…
fogged= bad= -> sprites= dots=` (the two last counts are quads actually emitted, so
`passive` reports zero); with `log`, the first objects per sample (`sfx: L5 obj=
vt=004fd618 smoke2 type=2 end= tick= n= p0 pos=(x,alt,y) frame= seq="smoke 1"`) and the
first three of the pass's own sprite emissions (`fx: emit b= mode= at= …`).

Caveat, the same as the effects pass's: with `sfx.on` but no `native.on=all`, the layers
the engine draws under its units (0..4, wake foam above all) composite over the
engine-drawn hulls in the 8bpp frame, because nothing wrote depth for those hulls.

### 7.6 Verification (2026-09-02, `scenarios/sfx-strait.json` on Anteer Strait)

Same-fight A/B on one instance (`sfx.on="log passive"` → engine draws while we count;
`sfx.on=log` → ours, the engine skipped), panels `assets/shots/sfx-*-ab.png`:

- **Wakes**: a Skeeter sailing along the strait leaves the same trail of 2×2 foam dots in
  the engine's frame and in ours; the 8bpp surface has no dots while we own the draw.
- **Nanolathe**: the commander finishing a nanoframe (repair through the orders page —
  a scripted repair order does not spray, and the spray stops the moment metal runs out:
  the fixture gives storage units and 100 000 metal) — the green spray from the nano
  piece to the site, identical dot colours and spread; the engine surface shows only the
  frame.
- **Smoke**: damage smoke over three badly damaged structures, grey puffs of the same
  sequence frames at the same anchors; rocket trails (layer 5) and impact puffs (layer 9)
  read clean on `fx-rockets`.
- **Fire**: a self-destructed metal extractor (its explosion emits 200 fire objects — a
  metal storage's does not) burns with the same flare-sequence flames in both.
- **Stress**: `200v200` at sim +3 with `native.on=all wrecks`, `fx.on` and `sfx.on`:
  **~60 fps** (28 `sfx:` lines — one per 60 present frames — in 30 s, 387 units alive,
  217 native units and up to ~230 particle sprites on screen, 620 at a big death, plus
  117 effect sprites and 37 flashes); the unit vertex budget caps as before.
- The gather never saw a `bad` object (pointer or vector out of range) across every
  fixture; the layer cap of 400 was hit by the extractor's death (201 smoke + 200 fire).

Review follow-ups (2026-09-02, `/code-review medium`, applied before shipping): the fx
`nosprites` token had gated the shared emitter and so the particle sprites, and its
disarm path left the tokens stale — the gate moved to the fx call sites and the tokens
reset on disarm; the layer log line's `_snprintf` accumulation could run backwards on
truncation (MSVCRT returns −1) — a bounded append replaced it; trail smoke was drawn
over lasers and flashes — the under bucket; the emission trace leaked into the fx pass —
it is zeroed after the particle emits; the scaffold GLSL was a second copy of the unit
shader's — one snippet now serves both, and only the under bucket pays the fetch. Left
as noted: a `GL_POINTS` bucket for dots (a 2×2 dot costs six vertices; the under bucket
already keeps a nano flood from starving explosions), and the small helper duplication
between `tagpu_sfx.c` and `tagpu_fx.c`.

Gaps: layer 8 and the `0x4FD588` class were never observed live (the latter is emitted
by teleport and `0x472630`, which the flash loop's callers reach); the sim-side update
walker's exact entry is not pinned (not needed — only the draw is owned); `DrawBar`'s
rect `{x, y, x+1, y+1}` is taken as 2×2 from the panels, not from its fill routine
(`0x4CCDEA`); sub-pixel motion for puffs is not used (engine parity: integer anchors);
particles are drawn at 50 % alpha like the engine's ALP blend, so they look identical to
the engine's rather than better — a translucency slider is a G13 freedom.

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
| `0x471F90` | particle-layer draw walker `(ctx, n)`, `stdcall` `ret 8`; ten sites in `DrawGameScreen` (§7.1) |
| `*(main+0x38D77)` | ten layer vectors `{flag, begin, end, cap}` × `0x10`; `0x471D90` alloc, `0x471DE0` destroy-all, `0x4732E0` insert |
| `0x51E610` / `0x51E608` | particle pool (`0x470EB0` pop / `0x470ED0` push) / global disable byte |
| `0x4FD638 / 0x4FD618 / 0x4FD5D8 / 0x4FD588 / 0x4FD5F8 / 0x4FD5B8 / 0x4FD5A8` | vtables: Smoke1 / Smoke2 / fire / flare sprite / wake / nanolathe / base (slots 0 destroy, 1 update, 2 draw, 3 empty?, 4 spawn, 5 again?, 6 init) |
| `0x475700 / 0x475470 / 0x474170 / 0x473590 / 0x4745E0 / 0x473A00` | per-particle draw leaves in the same order |
| `0x472810 / 0x4728F0 / 0x472C50 / 0x4729D0 / 0x472AB0 / 0x472330 / 0x472530 / 0x472430 / 0x4720D0 / 0x472200 / 0x471FD0 / 0x472630` | emitters (§7.4) |
| `0x4BF6F0` | `DrawBar(ctx, rect, colour)` — the 2×2 dot |
| `main+0x147CF / 0x147D3 / 0x147F3` | smoke sequences "smoke 1" / "smoke 2" / the flare sequence fire uses |
| `main+0x37ECC / 0x37ED4 / 0x14263` | wind x / wind y / smoke rise per tick |
