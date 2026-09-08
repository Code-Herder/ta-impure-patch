# GPU renderer — status, hook map, and known limits

*The one page to read before touching the renderer. Where the port actually stands, every
engine address the stack patches and why, what is still wrong, and the handful of rules the
work has converged on. The [roadmap](roadmap.html) is the chronological log and the gate
definitions; this is the current state, and it is written from the SOURCE (`tagpu/ddraw/src/`)
rather than from the log, so it cannot drift into describing an intention.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5
`8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

---

## 1. Status — what is ours, what is still the engine's

**The engine's software frame is UI only.** Inside the world viewport it is a flat fill of one
palette index — the *key* — and the composite is inverted against it: we drop OUR fragment
wherever the engine's frame is **not** the key, so anything it still draws in there shows
through. Measured 99.98 % key with nothing on it but the mouse cursor — which is the engine's
own sprite, drawn under the pointer at every zoom and left alone by the composite (§2.3d).

| What the engine used to draw | Ours since | Owned how |
|---|---|---|
| Units, wrecks, shadows, cloak, waterline | G12a–c, G13n | `owndraw` skips the software rasterisers; `tagpu_native.c` draws them. **Under Classic++ (G14i) neither Classic shadow is drawn: `tagpu_shadow.c`'s depth map replaces the silhouette and the slant, and an aircraft under `airshadow=drop` alone keeps its silhouette.** **The silhouette shadow blends once per silhouette PIXEL through a stencil** (G13n) — the engine blits one blackened copy of the composite, so re-using the body's 3-D geometry with depth writes off darkened once per surface the ray crossed: aircraft came out at 0.25 of the ground against the engine's 0.49. Both FBOs are `DEPTH24_STENCIL8` for it — [shadows & cloak](shadows-cloak.html) §"What our GL renderer must do" |
| **Units under construction** — the nanoframe scaffold, its fill and its wireframe | G13l | the same pass: ownership no longer stops at `Nanoframe > 0`, the recolour is three per-unit uniforms in the unit shader and the wireframe a line range per unit; a third `owndraw` detour (`0x458DD0`) stops the engine stamping its own copy at the 1× position. A unit under construction casts no shadow, as the engine's does not, and a factory's cargo takes the FACTORY's depth key — the engine z-merges it into the factory's sprite (`0x4B90A0`) rather than sorting it, and on its own tile row it disappeared under the lab. The carry relationship itself — attach/detach `0x48AB70`, and why a *released* unit appears to walk under the plant (stock, measured) — is on [factories](factory-build.html) |
| Structure shadows (the cached slant projection) | G13k | `owndraw all` flips the blit's two structure-shadow `je`s; the native pass emits the slant projection from the posed prims — see §2.1 and [shadows & cloak](shadows-cloak.html) §"Structure shadows, owned" |
| Weapon fire, explosions, debris | G12e | `fxown`: 2 call-site redirects + 4 leaf detours |
| Smoke, fire, wakes, nanolathe | G12f | `fxown`: one detour on the layer walker |
| Features (trees, rocks, splats, wreckage) | G13a | `featown`: one detour on the feature leaf |
| Terrain tiles + the fog overlay | G13b | `terrown`: two detours; the terrain skip path key-fills the viewport |
| Fog of war *as drawn* | G13c | one shared rule (`tagpu_glsl.h`) in all four native passes |
| Health bars, order markers, group digits, ShowRanges labels, build cursor, band box, selection rect | G13d, cursor G13n, order block G13o, text G13p | `markown`: 14 call-site redirects + 1 detour, and **nothing world-anchored is captured any more**. Health bars, the selection rect, the build cursor and drag band box are re-drawn from engine state; **G13o ported the order-marker block** (`tagpu_order.c`) as a game-thread snapshot of the order lists at `0x469BFC`; **G13p ported the text** (`tagpu_text.c`) — the group digit at `0x469CF9` and the `ShowRanges` labels — by calling TA's own glyph blitter `0x4CCF60` with a buffer of ours. Window A is retired and the identity blend LUT with it; the post-fog capture survives only for `mark.on=nocursor` (§2.2) |
| Mouse cursor position, clicks, minimap view rect, scroll rate | G13e, cured G13m | `tagpu_zoom.c`; the cursor is the engine's own again — the composite no longer touches it (§2.3d) |
| Which cursor sprite the engine picks on hover (move / reclaim / …) | G13j | one byte patch in `tagpu_patches.c`; the engine still draws it — see §2.6 |
| The engine's *addressable* viewport at zoom < 1 — clicks, orders and unit picking in the outer ring | G13f | `vpwide`: 3 call-site redirects + a 3-site byte patch behind `vpwide.on`, plus the `0x499221` redirect that also carries the zoom's mouse-point repair and is armed by `zoom.on` too (§2.3d) |
| Terrain in **restored true colour** (Classic++, `tagpu_classicpp.on`) | G14a (spike, 2026-09-04); GPU G14b (2026-09-04); **GLSL G14c (2026-09-05)** | `tagpu_restoreglsl.c` runs the unditherer's full model as **fragment passes in the game's own GL context** — the shaders of `tagpu_restore_glsl.h`, the weights of `<model>.w32.bin` — sliced from `tagpu_terr.c`'s gather at 12 ms of GPU time per frame under a `GL_TIME_ELAPSED` budget, visible tiles first, painting straight into the terrain pass's RGBA atlas: Two Continents' 5062 tiles in 2.1 s at 59.7 fps, the biggest stock map's 11,561 in 4.2 s, no worker thread, no runtime, no disk. The ONNX Runtime path (`tagpu_restore.c`, G14a/b) was deleted the same day (G14d). **G14e (2026-09-05)**: the cells show as they land, centre-out (the restored atlas's alpha is the flag), and the **feature and effects atlases restore lazily** — `tagpu_gaf.c` queues every atlas miss to the same restorer, each atlas carries an RGBA8 twin the sprite shaders sample where its alpha is 1, keyed texels inpainted by a nearest-ring stand-in in the FILL pass. **G14f (2026-09-05): lit** — the terrain from the engine's height grid (`main+0x14287`, one R8 texture per map, the lab's grid normal per fragment), the units from the posed face normal carried in the vertex stream, the feature sprites from the ground's lambert at their anchor; one rule, `tagpu_glsl.h` `TAGPU_GLSL_LIGHT_FN`, level ground exactly 1.0; the knobs in `tagpu_classicpp.cfg` (`tagpu_classicpp.c`). **G14g (2026-09-05): the unit textures** — `tagpu_render3do.c`'s atlas is a `TAGPU_GAFATLAS` now, every frame in a 4-texel-padded, 4-aligned cell, its RGBA8 twin restored lazily like the sprites' (priority 3) and **mipmapped to level 2**, trilinear and 4× anisotropic, the mips regenerated after each painted batch; the unit shader samples it where its alpha says so. Classic's R8 atlas has the same cells and does not move a pixel (`tascene ab`, and the engine shot byte-identical to G14f's outside the chat). Reads only — see [Classic and Classic++ renderers](renderers.html) §4c and §5 |
| **Chat, dialogs, side panel, minimap, top bar, the shell** | **Phase E, in progress** ([GL UI renderer](gui-renderer.html), decided 2026-09-06). **G15a done 2026-09-07**: every UI pixel-writing leaf is observed and the census explains 100 % of what changes on the presented surface across the screen inventory. **G15b done 2026-09-07**: the observed ops are replayed into GL twins of the engine's surfaces and the presented surface's twin is drawn over the world composite — the in-game panel, build pages, bars, option screens, chat and the F4 popup, and the whole shell, at 1:1 Classic; **0 differing pixels and 0 holes under `strict` on every in-game stop of the inventory at 1024×768 and at 1920×1080** (§2.3e). **G15c done 2026-09-07**: the rest of the in-game frame reached and measured — ARM and CORE, both resolutions, the HUD strings, the popups and the option screens over the world at 0.5× and 2×, the minimap under motion: 32 of 32 stops at 0/0/0 in all four runs, no DLL change. **G15d done 2026-09-07**: the shell across the 640×480 context switch, three game→shell→game cycles in one process, clean — the publisher drops batches while the render thread is dead or crawling (the switch stops it and it crawls out of a game), skips the stale queue after the new GL context, retires the main offscreen's dead entry, and resolves the twin through the palette the frame is *presented* with, not `main+0x143A7` (the engine gamma-scales the presented one) | the engine's surface is still drawn and still the fallback beneath the twin (nothing is suppressed in phase 1); the cursor stays the engine's; Classic++ art (G15e) is the next gate; the world passes reading `main+0x143A7` are wrong at Gamma ≠ 12 (a native-pass fix, not this gate's); on by default since 2026-09-08 (§2.8; `tagpu_gui.off` turns it off, `tagpu_gui.on` still carries the tokens) |

**Phases.** Phase 0 (foothold) is complete and Phase B (blit-level GPU units) is verified
complete. Phase D's scene takeover — G13a through G13e — has landed, which is what the table
above records; **G13e is the newest and is still in "awaiting review" on the roadmap.** Two
things remain from the original plan:

- **G11 — the replacement pipeline** (glTF convention + loader + one exemplar unit driven by
  live COB state). This is the project's stated purpose and it is the next real gate. The
  groundwork is real and both halves are on `main` as of `f508124`, but "the slot is proven
  and the exporter exists" still reads as further along than it is — the proven exemplar did
  not come out of the exporter:

  | Half | Where it is | The gap |
  |---|---|---|
  | Replacement-mesh slot in the DLL | `tagpu_hires.c` (glTF 2.0 loader) + `tagpu_hires_draw.c` (its own GL program) — `gamedir/hires/<defname>.glb` or `.gltf` renders instead of the 3DO, hot-reloaded on mtime. One static VBO per model, triangles sorted by material, one draw per material; per-pixel lighting against the engine's own light direction, normal maps through a derived TBN, metallic/roughness, mipmapped true colour. **Posed per piece from the engine's live COB state**, bound by glTF node name, so it walks, aims, recoils and honours `HIDE`. Shares the native pass's FBO, depth keys, scaffold, fog, waterline and silhouette shadow. **Proven end-to-end** (a 1577-tri 6-material Peewee, 20 of them in a fight vs engine-drawn AKs; the pose reconstructs the engine's own posed vertex buffer to 2e-5 model units) — [model import](model-import.html) | No skins, morph targets, glTF animation, sparse accessors or texture wrap modes (UVs clamp); PNG images only; 48 posable pieces. Team colour arrives as a `baseColorFactor` authored into the model by hand — the exemplar's `pw_team` and `pw_team_chest` materials carry ARM blue as a linear factor — and nothing drives it from the owning player. The order two simultaneous piece-turn axes compose in is a documented guess, and COB `MOVE` read zero in every sample |
  | glTF exporter | `tools/ta3do` — 3DO + GAF → glTF, standard views, `--undither` | on `main` since `f508124`. The **landed exemplar did not come out of it**: `units/pee-wee/armpw-detailed.glb` carries Blender's own `Khronos glTF Blender I/O` generator string and a `baseColorFactor` this tool never writes (it emits `pbrMetallicRoughness` with `metallicFactor`/`roughnessFactor` only). So the hand-authoring step between 3DO and shippable model is the unautomated half |

  So G11 is three concrete pieces of work, not a wiring job: **(a)** land the exporter and
  agree one format between it and the loader; **(b)** per-piece pose — **done**: the engine
  hands us *fully posed* vertex buffers for its own 3DOs (`PrimitiveStruct+0x22`), which is why
  the native pass needs no rotation maths today, and replacement geometry, not being in the
  engine's piece tree, is placed instead from the node's rest offset (`Model3DONode+0x10`), the
  `MOVE` delta (`PrimitiveStruct+0x04`) and the `TURN` triple (`+0x10`) accumulated down the
  tree. The `tagpu_posedump.on` probe it was written for has now been run, and it settled both
  the rotation convention and the axis the exported model has to be mirrored on — the residual
  against the engine's own posed vertices is 2e-5 model units per piece. It also caught the
  replacement pass applying the body yaw *inverted*, which had been invisible because the test
  scenario was parked at facing 90, one of the four facings where the two agree. Derivation and
  authoring guide: [model import](model-import.html); **(c)** true-colour and translucent
  materials, which is the part of the stated purpose the palette-index path does not reach at
  all.
- **G9 — the MP-safety replay byte-diff.** Mostly formalisation now: 200v200 measures 59.7 fps
  and every hook is read-only over the sim, but this is the gate that *proves* the native stack
  is sim-neutral, and the byte-diff needs an unlocked session. It has been deferred several
  times.

**Numbers that are load-bearing.** 200v200 at 59.7 fps. Terrain 0-px parity against the
engine's own blit. Fog overlay 99.06–99.39 % lit-vs-grey agreement. Viewport black fraction
0.0019 at 1×, 0.0003 at 0.5×, 0.0002 at 0.35×. Engine surface 99.98 % key (112 px of 630 784).

---

## 2. The hook map — every engine address we touch

Three mechanisms, and the difference matters:

- **Call-site redirect** — rewrite one `E8 rel32` so it calls our `__stdcall` stub, which
  usually calls the original through. **Composes**: our stub *calls* the target, so a prologue
  detour someone else installed on that target still runs. This is why `markown` can redirect
  sites that call `0x471F90` and `0x4BF8C0` without colliding with `fxown` and `terrown`.
- **Prologue detour (`tagpu_detour_leaf`)** — steal N prologue bytes, jump to a stub that, while
  a flag byte is set, returns immediately with the callee's own `ret n`; otherwise runs the
  stolen bytes and jumps back. **Skips** a function wholesale.
- **`tagpu_detour_leaf_call`** — the same, but the skipped path first calls a function of ours
  with the original's first stack argument. Used where taking the draw over still leaves us
  something to do in its place (the terrain key-fill, the fog grid's lazy rebuild).

**BUILD THE HEADERS INTO THE DEPENDENCIES.** Until 2026-09-03 neither Makefile
tracked header dependencies, so `make` rebuilt only the `.c` files that changed and
objects that disagreed about a struct's layout linked without a murmur. G13d
(`0e6b6b7`) inserted four ints — `evpL/evpT/evw/evh` — into the **middle** of
`TAGPU_FXVIEW`; `tagpu_sfx.o` had been built hours earlier and was never rebuilt, so
`tagpu_sfx_gather` went on reading `fogGrid` 16 bytes early, where `evpT` now lives.
At zoom < 1 that is a small negative int, the `!v->fogGrid` test passes because it is
non-zero, and the render thread dereferences it — a hard crash that froze the game
with the process still up. It hid for a day because the mis-read gate (`fogMode` <-
`evpL`) is EVEN at 1x and only odd at *some* zoom levels, so it presented as an
intermittent, zoom-and-scroll-dependent crash rather than a build fault. `-MMD -MP`
plus `-include` now covers both DLLs; the arithmetic is in the `tagpu_fog_at` guard's
comment. **A crash whose faulting base is a small negative integer is this shape.**

Every install is **byte-matched first and all-or-nothing**: nothing is written unless every
site in the module still holds the bytes we recorded, so a patched or different exe arms
nothing rather than half of it. Every module installs **once at `DllMain`** and only if its
arm file exists *then* — patching live engine bytes off the frame loop is racy, so there is no
per-frame re-arm. The behaviour flags on top of the patches *are* re-read live.

### 2.1 Draw ownership

| VA | What it is | Module (arm file) | Mechanism |
|---|---|---|---|
| `0x459830` | opaque 3DO rasteriser | `owndraw` (`owndraw.on`) | prologue detour, 5 stolen |
| `0x459C70` | the Gouraud-lit 3DO rasteriser — **selected for STRUCTURES**, not for nanoframes (`0x45873C` tests `unit+0x110 & 0x20000000`, measured to be the structure bit; [build-state](build-state.html) §1) | `owndraw` | prologue detour, 5 stolen |
| `0x458DD0` | the blit-time **build-state effect** (`thiscall(this, frame, obj)`, `ret 8`): the height-threshold recolour plus the nanoframe wireframe, applied to a scratch copy of the composite every frame. Wiping the composite does not stop it — with the rasterise skipped it recoloured nothing and stamped its wireframe alone, at the 1× position | `owndraw` | prologue detour, **6 stolen** (`53 55 8B 6C 24 0C`; a 5-byte steal splits the `mov`), skip path `xor eax,eax; ret 8` = the callee's own early-out. Taken only for units `tagpu_native_owns_obj` claims |
| `0x4592C6` | `je 0x459324` in the blit `0x459200`, path A — the branch into the cached structure shadow (`Object3do+0x14`, blitted through the ALP blend, which turns the fill key teal) | `owndraw`, target `all` only | `74`→`EB`, one byte, verified `74 5C` first; installed with the next as a pair or not at all |
| `0x45952C` | the same `je` in path B (`je 0x459578`) | `owndraw`, target `all` only | `74`→`EB`, verified `74 4A` |
| `0x469B22` | `call 0x49BE60` — projectile pass | `fxown` (`fxown.on`) | call-site redirect |
| `0x469B2C` | `call 0x420B00` — explosions/particles pass | `fxown` | call-site redirect |
| `0x46BAE0` | model-projectile leaf (`ret 0x10`) | `fxown` | prologue detour, 5 stolen |
| `0x4B8EC0` | muzzle-flash leaf (`ret 0x10`) | `fxown` | prologue detour, 6 stolen |
| `0x4211D0` | debris-piece leaf (`ret 0x0C`) | `fxown` | prologue detour, 5 stolen |
| `0x4B7F90` | GAF-frame blit (`ret 0x10`) — **shared, see below** | `fxown` | prologue detour, 6 stolen, on a SCOPED flag |
| `0x471F90` | particle layer-draw walker (`ret 8`) | `fxown` | prologue detour, 5 stolen |
| `0x46A610` | feature draw leaf (`ret 0x10`) | `featown` (`featown.on`) | prologue detour, 5 stolen |
| `0x483FA0` | terrain tile blit (`ret 4`) | `terrown` (`terrown.on`) | `leaf_call` — the skip path key-fills the viewport |
| `0x4848E0` | fog overlay (`ret 4`) | `terrown` | `leaf_call` — the skip path replicates only the lazy grid rebuild |
| `0x4843C0` | screen fog-grid rebuild | `terrown` | *called by us*, not patched |

> **`0x4B7F90` is not an effects function** — it is the engine's generic GAF-frame blit, and the
> mouse cursor goes through it too (`0x4C2870` → `0x4C297E`). It is detoured on **`g_fxown_in`**,
> which the explosion pass's own call-site stub sets on entry and clears on exit, *not* on the
> global `g_fxown_skip`. Detouring a shared leaf on a global flag would have silently eaten the
> cursor whenever `fx.on` was armed. **When a leaf turns out to be shared, scope the flag to the
> pass rather than the session.**

### 2.2 World-space UI markers (`markown`, `markown.on`)

| VA | What it is | Mechanism |
|---|---|---|
| `0x4699EB` | `call 0x46A530` — selection rect, ground sweep | call-site redirect, **per-unit** test |
| `0x469B8A` | `call 0x46A530` — selection rect, air sweep | call-site redirect, per-unit test |
| `0x469BD7` | `call 0x471F90(ctx,8)` — hook 8, the marker block's start | call-site redirect; since G13p it opens no capture — it brackets the order arena's block and **latches `[globals+0x204]`/`+0x208`**, the font and text colour the block's own text would use |
| `0x469D2C` | `call 0x471F90(ctx,9)` — hook 9, the block's end | call-site redirect; closes the order arena's block |
| `0x469CF9` | `call 0x4C14F0` — the group digit | call-site redirect; the engine's call is **skipped** and the digit re-drawn out of our text atlas (G13p) |
| `0x469EC5` | `call 0x4BF8C0` — build-cursor rect | call-site redirect; the engine's call is **skipped** and the rect re-drawn (G13n) |
| `0x469F1E` | `call 0x4BF8C0` — drag band box | call-site redirect, same — one rect, one gate, both re-drawn |
| `0x439516` | `call 0x439740` — target sprite, from the route-dot drawer | call-site redirect, **trace only** since G13p (the identity blend LUT went with window A) |
| `0x439C7D` | `call 0x439740` — target sprite, from the walker's bit-3 dispatch | call-site redirect, same |
| `0x46A430` | `DrawHealthBars` (`ret 0x10`) | prologue detour, 5 stolen — bars are **re-drawn**, not captured |
| `0x4C1B80` | `KeyboardHotkeySampler(id)` (`ret 4`) | *called by us* — we sample the engine's own SHIFT gate (`0xF9`) rather than reading the key |
| `0x469BFC` | `call 0x48CC30(ctx, main+0x142F3)` — the order-marker driver, SHIFT-gated | call-site redirect; our stub **snapshots the order lists on the game thread** and the engine's driver is then **skipped** (G13o). `passive`/`trace` make the snapshot decline, so the engine draws its own |
| `0x439BAC` | `call 0x438C00` — build-site rect, from the walker's bit-0 dispatch | call-site redirect, **trace only**: logs the engine's node list under `order.on=trace`, otherwise a compare and a call through |
| `0x439BF2` | `call 0x4394E0` — route dots, bit 1 | call-site redirect, trace only |
| `0x439C37` | `call 0x4399F0` — target circle, bit 2 | call-site redirect, trace only |
| `0x439CBE` | `call 0x4390A0` — range circles, bit 4 | call-site redirect, trace only |
| `0x465AC0` | `UnitInPlayerLOS(player, unit)` (`ret 8`) | *called by us*, on the game thread, to reproduce the target sprite's LOS rule and its last-seen cache write |
| `0x485070` | `GetPosHeight(POS16_16*)` (`ret 4`) | *called by us*, on the render thread — a pure read of the height grid, which is what makes a range circle follow the terrain |
| `0x4CCF60` | the glyph blitter (**cdecl**, 9 args, base and pitch taken directly) | *called by us*, on the present thread, once per distinct string — it reads the font object and writes our atlas and touches no engine state at all (`tagpu_text.c`) |

**The selection rect is drawn at 1x, after the downsample, and matches the engine's pixels.**
Four things had to be right and none of them was ([UI markers](ui-markers.html) §1, all measured
2026-09-08 against the engine's own rect — `mark.on=noselbox` hands it back while everything
else stays ours, so both boxes land in one `glshot`):

1. **The rotation.** The four corners take `0x4B6CC0`'s triple — `Rz(u+0x64)` on `(x,y)`,
   `Rx(u+0x68)` on `(y,z)`, `Ry(u+0x66)` on `(x,z)` — which is what `0x467A50` does to these
   same points. The loop used the *transposed* yaw until 2026-09-08: a rotation by −heading, so
   a unit turning on the spot had its box turning the other way (2× the heading off — invisible
   at every multiple of 45°, obvious between them). The tilt words are not decoration either: a
   tank on a hillside carries up to 17° of bank and 31° of pitch.
2. **The bounds.** `0x4CB650(model,&min,&max,**0**)` seeds min and max with the ORIGIN, skips
   any node with fewer than three vertices, and with that last argument 0 never leaves the root
   node — so the rect is the root piece's own vertices, not the whole tree. Ours was the whole
   tree and stood ~11 px taller with the bottoms aligned. `aabb_walk` is unchanged (it is the
   shadow pass's model height); the rect has its own `selbox_aabb`.
3. **The arithmetic.** The engine truncates each term of the projection separately and halves
   the height with `sar 1` *after* truncating it. One float expression instead is a pixel out on
   some edges: 46 of ~110 box pixels differed until that was reproduced, 7 after.
4. **The line.** The engine's is Bresenham (`0x4BE950`): one fully coloured pixel per major-axis
   step. Ours was a GL line in the 2x supersampled FBO, and **the driver clamps aliased line
   width to 1** — `glLineWidth(ss*3)` was measured to draw pixel-identically to
   `glLineWidth(ss)` — so it was one SUPERSAMPLE wide and resolved to about half the engine's
   colour, (66,136,56) against a flat (83,223,79). It is now drawn into the **1x FBO right after
   the box-downsample**, with the supersampled depth blitted down (`GL_NEAREST`, the only filter
   a depth blit takes) so it still sits under its own unit. 100 % of our box pixels are then
   exactly the engine's colour, and its own box differs from ours on 5–18 pixels of ~110 — a
   Bresenham step landing on the other neighbour, or a pixel where ours is correctly occluded
   and the A/B's engine box (which composites over our whole world) is not.

Two consequences worth knowing. The rect now draws **after** the marker layer, so where a box
edge crosses a health bar our line wins where the engine's bar would; the bars sit inside the box
on every stock unit measured. And the same supersampling that dimmed the rect dims **every line
and glyph the marker layer draws** — bars, order lines, range circles, the text atlas — which is
the same fix one layer up, and is not done.

**The order block is PORTED, not captured, since G13o.** `tagpu_order.c` re-derives §3 of
[UI markers](ui-markers.html) — the driver's three selection rules, the walker's
capability-mask dispatch and its `pos` chaining, and all five leaf drawers — and draws them
at native resolution in `tagpu_mark.c`'s pass. A capture could never reach past the
OFFSCREEN's own width and height, and the offscreen is screen-sized while `vpwide` lets the
projection run far outside it, so at zoom < 1 the engine's own clipper threw the outer ring's
markers away before our buffer saw them.

**And the text went the same way in G13p.** The bound on a string is not even a clip:
`DrawTextCustomFont 0x4C14F0` measures the whole box, tests it for CONTAINMENT in the
context's clip rect (`0x4C6AE0` + `0x4B6750`) and draws nothing at all if it does not fit. So
a group digit or a `ShowRanges` label in the outer ring at zoom < 1 vanished rather than
stopping short. `tagpu_text.c` calls the blitter underneath, `0x4CCF60`, which takes a base
and a pitch directly and has no bound of any kind, with `(fg,bg,transparent) = (255,0,0)` —
that turns it into a 1-bit coverage mask, so one raster per string serves every colour it is
drawn in. Each string lands once in a shelf-packed 512×256 atlas; the quads are constant
SCREEN size and snapped to the device pixel grid by pre-image, because a bitmap glyph at a
fractional anchor resolves each 1-px stroke across two device pixels.

**Window A is retired, and the identity blend LUT with it.** `0x439740` is the only
alpha-composited marker: it reads the destination pixel through `tab[(src<<8)|dst]`, and while
we captured the block into a buffer of ours that destination was the fill key, so the waypoint
star blended with palette 254's bright cyan and rendered teal. The two call sites were
therefore bracketed with an identity LUT — a 64 KB table and a pointer swap scoped to the call
because `[globals+0xC0]` owns a heap buffer the engine allocates, frees and refills
(`exe-reverse-engineering.md` §"The blend LUT and the marker composites"). With nothing of the
engine's landing in a buffer of ours, the star composites against the engine's own frame again,
exactly as stock does, and all of that machinery is gone. Under `passive` and `trace`, where
the engine draws its markers into its own key-filled surface and we composite that surface,
the star is teal — a debug mode's business, not the shipped frame's.

### 2.3 Zoom (`tagpu_zoom.c`, `zoom.on`)

| VA | What it is | Mechanism |
|---|---|---|
| `0x41C426` | `call 0x466B70` — minimap view rect, eye clamped at top | call-site redirect; the engine fills the rect, we rescale it by 1/z |
| `0x41C442` | `call 0x466B70` — ...and at bottom | call-site redirect |
| `0x430FAE` | `call 0x4B6A50` — the one site that persists `ScrollSpeed` | call-site redirect; substitutes the player's own value so our scaling can never reach the registry |
| `0x41C3C0` | the eye clamp — `eye = clamp(eye, 0, map − W)`, plus the minimap rect as its last act | **`leaf_call` detour, 5 stolen**, on a flag raised only while zoom > 1; our replacement widens the range and calls the same minimap wrapper |
| `0x498EF9` | `call 0x484B50` — the `GetTPosition` inside the mouse → world conversion | call-site redirect; clamps the world point to the map, a no-op for any eye the engine's own bounds can produce |

**The camera's range follows the zoom (§2.3c).** **The level comes from two levers, and
neither is an engine patch.** `tagpu_zoom.txt` is the
scripted one and **wins whenever it exists**; the **mouse wheel** is the player's, and takes
over the moment the file is gone. The wheel needs nothing from the engine because the engine
never wanted it: TA's window procedure dispatches only `0x200..0x206` through its jump table
at `0x4B5E3B`, so `WM_MOUSEWHEEL` (`0x20A`) fails the `CMP EAX,6` at `0x4B5E41` and falls to
a bare `DefWindowProcA` tail call at `0x4B5FA8`. Nothing had to be taken away from anyone.

`tagpu_zoom_wheel()` is offered the message at the two of the three engine doors a wheel can
arrive at (`wndproc.c`'s tail for hardware, the shield's `to_game` for injected; the third,
`wndproc.c`'s `WM_NCHITTEST` arm, carries no wheel) and consumes it only over a
live world viewport, so the menus cannot be wheeled and a future scrollable list keeps its
wheel. It does one thing on the message thread — `InterlockedExchangeAdd` the raw delta — and
the level itself still moves in exactly one place, `tagpu_zoom_read_lever()` on the render
thread, which folds the notches in (×1.1 per notch, geometric, clamped to 0.25–8.0), eases a
quarter of the remaining log-distance per frame, and **snaps a cancelled round trip to exactly
`1.0f`** so 1× stays the byte-identical identity the transform, the minimap rect and the
scroll rate all test for by equality. While the file is in force the wheel is *pinned* to it,
which is what makes deleting the file a handover rather than a jump.

Centre-anchored: no eye motion at all, so `vpwide`, the minimap rect and `ScrollSpeed` follow
with no further plumbing. Pointer-anchored zoom is the open follow-up and is a camera move —
`tagpu_input.c`'s eye hold, not a transient bias (§3.1).

### 2.3b The addressable viewport at zoom < 1 (`tagpu_vpwide.c`, `vpwide.on`)

**On by default since 2026-09-08 through the play defaults (§2.8); opt-in before that.** Nothing
here writes a byte unless the pass was on at DLL attach — `tagpu_vpwide.on`, or the default with
the zoom on and no `tagpu_vpwide.off` — the true rect verified, *and* a zoomed-out view is live.

| VA | What it is | Mechanism |
|---|---|---|
| `0x468D85` / `0x46964F` / `0x469F95` | `call 0x4C6B10` — the three sites in `DrawGameScreen` that copy the viewport rect into the offscreen surface's clip rect (`+0x1C..+0x28`) | call-site redirect; **clamped to the surface**, because `0x4C6B10` is a bare four-dword store with no clamping and a wide rect would license an engine drawer to write outside its allocation |
| `0x499221` | `call 0x498DA0` — the mouse → world / map cell / hovered feature conversion, and the ONE reader that uses L and T as the screen→world **origin** (`world = eye + clamp(pos, L, R) − L`) | call-site redirect; redone with the TRUE origin and the WIDE clamp, a straight pass-through whenever the rect is not ours |
| `0x4B5E5F` / `0x4B5EC0` / `0x4B5F0C` | the three arms of TA's own window procedure's `0x200..0x206` jump table, each unpacking the mouse `lParam` with `AND 0xffff` + `SHR 0x10` | **byte patch** → `MOVSX ECX,CX` + `SAR EAX,0x10`. `LOWORD`/`HIWORD` is zero-extending, so a client x of −20 arrived as 65516 and the event was lost; this is Microsoft's own `GET_X_LPARAM`, and for any position a real mouse can report it is bit-for-bit identical |

The rect it writes is `main+0x37E27..0x37E33` (L, T, R, B) only — **never W/H at `+0x37E37`/`+0x37E3B`**,
because the eye clamp `0x41C3C0` derives `maxEye = map − W` from them and a negative `maxEye`
makes it alternate between 0 and a negative eye. The true rect is derived from the **screen
dimensions** at `+0x37E1F`/`+0x37E23` — fields nothing here writes — because `0x497F40` computes
`W = R − L + 1` by *re-reading* L, so a store of ours landing in that window would corrupt W;
W/H are checked against the derivation every frame and put back when they disagree.

**The cursor is not a reader of this rect and no longer needs to be** — §2.3d is why. Until
G13m it was: the engine drew its sprite wherever `GetCursorPos` reported, so widening what it
could NAME also moved where it DREW, and in the ring a widened `u` lands on the side panel or off
the surface (measured at 0.5× with the pointer at screen (320,400): no cursor at the pointer and
a ghost one on the build panel). That is why the draw poll had its own transform. The cure was to
stop transforming that poll at all.

**The stub takes `g_ddraw.cursor` rather than trusting the engine coordinate it is handed,** and
the widened rect is the reason: at 0.5× the range `[0,128)` is reached both by a ring pointer
(transformed) and by a pointer on the panel (passed through 1:1), and without the true pointer to
settle it a click in the world lands in the minimap's click rect. Since G13m the same sample also
*produces* the engine coordinate — see §2.3d.

### 2.3c The camera's range at zoom > 1 (`tagpu_zoom.c`, `zoom.on`)

The mirror of §2.3b, at the other end of the lever, and it needed no new arm file. `0x41C3C0`
clamps the eye to `[0, map − W]`, W being the 1× viewport size: the range that puts the
**viewport's** own edges exactly on the map's. At zoom `z` the view is still centred on
`eye + W/2` but is only `W/z` wide, so those bounds stop the visible window
`W/2 − W/(2z)` short of the map on **every** side — at 1024×768 (W=896, H=704) that is
224 px at 2×, and 392 px at 8×. Zoomed in, the edges and corners of the map could not be
reached at all, and the camera read as though it were being pushed back off them.

The range the zoom needs is the engine's own, widened by exactly that shortfall:

```
d   = (W/2)(1 − 1/z)                    0 at 1×, W/2 in the limit
eye ∈ [−d, (map − W) + d]
```

which is the same arithmetic the transform uses about the same centre, so the two cannot
disagree at the edges: the world at the viewport's left edge is `eye + d`, which is 0 exactly
when `eye = −d`. **Everything that reads the EYE follows for free, because the eye is the
engine's camera** — the minimap's view box, the minimap click jump, the mouse and edge scroll,
the HotUnits cull and our own passes needed nothing new.

**What is not covered, and the scroll target is where the line falls.** `main+0x14327`/`+0x1432B`
is where the camera is *heading*, and the per-frame stepper `0x41CA30` eases the eye toward it.
Paths that set the eye and copy it into the target afterwards (`0x41C574`, `0x41CDB0`, the
scroll `0x41D037`) reach the widened range through the detour. Three sites instead compute the
target and clamp it **inline** against `[0, map − W]`, never calling `0x41C3C0` for it —
`0x41C4C0` (smooth `SetCamera`), `0x41C7F7` (smooth centre-on) and `0x41CAF7` (the per-frame
camera **follow**, which recomputes the target from the tracked unit every frame). The stepper
walks the eye to that target and our wider clamp leaves it there, so **those paths still stop
`d` short of a map edge**. Nothing fights and nothing churns — the eye arrives at a target
inside our range and both stop — it is simply the old behaviour where the detour does not sit.
Closing it means widening three inline clamps in the middle of the camera module.

**The right-edge mouse scroll could not fire at zoom > 1 — only the right one. CLOSED in G13m
(§2.3d).** Found while documenting this, not by the change: TA's scroll poll (`0x41CE90`, mapped
in [exe RE](exe-reverse-engineering.html)) fires on *hotkey* or *pointer on an exact screen
edge*, and the mouse half is an **equality on the outermost pixel** — `x == 0`, `y == 0`,
`x == screenW − 1`, `y == screenH − 1` — taken from `[obj+0x196]`, the record the `GetCursorPos`
polls keep, which `fake_GetCursorPos` used to fill with the **unzoomed** `u`. Three of those four
screen edges lie *outside* the viewport rect (`L=128`, `T=32`, `B=screenH−33`), so the transform
passed them through as identity and they still scrolled. The screen's right column, though, *is*
the viewport's right column, so it was contracted toward the centre: measured at 2× on 1024×768 a
pointer at `x=1023` reached the engine as **800**, and `x == 1023` became unsatisfiable.

§2.3d fixed it by construction rather than by a special case — the poll now answers the true
pointer, so every screen-edge equality holds again. Measured at 1920×1080, all four edges, eye
before → after a 2 s hold: at **2×** left 3000→1864, **right 2856→3928**, up →1880, down →3912;
the same four fire at 1× and 0.25×.

*[CORRECTION: this section named the poll `0x41CF10`, which is not an instruction boundary. The
function is `0x41CE90`, one caller `0x496976`; and the `GetCursorPos` at `0x41CEE7` inside it is
not the edge test at all but the off-screen warp-back.]*

**The flag is what keeps 1× byte-identical.** The detour is a `leaf_call` on a flag raised
only while a zoomed-**in** world is live; with it clear the engine's own function runs
verbatim, *including the two minimap-rect redirects inside it*. `tagpu_zoomedge.off` in the
gamedir clears it live and walks the eye back onto the 1× range.

**Two things an off-map eye needed.** The engine clamps the eye only when *it* moves the
camera, so an eye parked at −d at 4× would sit there until the next scroll — a zoom-out at a
map edge would show the void past it. `apply_eye_range()` re-applies the same clamp from the
render thread once a frame and writes only when the eye is actually outside the range in
force (same standing as the `ScrollSpeed` write: local camera state no other machine sees).
**It must clamp the scroll target with it**: that correction has no caller to copy the eye
into the target afterwards, and the stepper acts on any disagreement — `0x41CB5F` sets the
camera-moved bit and `0x41CB6B` **clears `main+0x14281` bit 3, the fog grid's is-current
flag**, then halves the distance and hands the result to the (no longer widened) engine clamp,
which puts it straight back. Left alone that is a permanent per-frame fog-grid rebuild after
any zoom-out from a map edge, on exactly the path `97e518f` had to guard against a crash.
Clamping the target rather than assigning the eye to it is what preserves a camera move that
is genuinely in flight: such a target is inside `[0, map − W]` already, so inside ours too.
And `0x498DA0` hands a pointer **outside** the viewport the world point
`eye + clamp(pos, L, R) − L`, which on the side panel is `eye` itself and under the bottom bar
is `eye + H − 1`: on the map for every eye the engine can produce, off it for ours, and the
chain from there is the `GetGridPosPLOT` → NULL → `GetGridPosFeature` crash vpwide's own stub
carries a clamp for. So the `GetTPosition` call that starts it is redirected and the world
point clamped. A pointer **inside** the viewport needs none of this: at `z > 1` the transform
maps the whole viewport into `[L+d, R−d]`, so the world it names is `[0, map−1]` at either
extreme of the range and inside it everywhere else.

### 2.3d The cursor, and where `u` is allowed to reach the engine (`zoom.on`, G13m)

**The engine is told the truth about where the pointer is.** `fake_GetCursorPos` answers `s`, at
every zoom. It used to answer the unzoomed `u`, because the engine's screen→world arithmetic is
1:1 and needs that number — but the engine also DRAWS its cursor from that poll, so the sprite
landed at `u` and the composite had to carry it back under the pointer. That job cannot be done
exactly: the surface texture the composite samples is only replaced when the game flipped, the
engine draws its cursor several times per flip, and any residual mismatch is multiplied by 1/z.
This gate's own first commit removed the largest term (a second, later sample of the pointer,
taken on the render thread) and halved the artefact; the rest was structural.

So `u` now reaches the engine in exactly two places, and neither is a poll:

| Path | Where | Why there |
|---|---|---|
| a **button** message's `lParam` | `tagpu_zoom_mouse_lparam()`, at the three `CallWindowProcA` doors | a button is queued on the engine's own event ring (`0x4B5EB2`/`0x4B5EFE` → `0x4C2E30`) and dispatched whenever the game loop reaches it. Its position is where the press was MADE, and no later pointer sample can reconstruct that |
| the **dispatched record**, at the mouse→world conversion | `vpw_mouse_world()`, our redirect of `0x498DA0` at `0x499221` | this is the one place the 1:1 arithmetic happens. It recomputes `u` from a single `g_ddraw.cursor` sample and writes it into `main+0x2C76` as well as the stack copy it is handed, because `GetUnitAtMouse 0x48CD80` (called at `0x499278`) and the routing test at `0x469DE1` read the field |

**A MOVE message must NOT be rewritten, and that is the half that took a measurement to find.**
`0x4B5E51` does not queue: it copies its record into `[obj+0x196]` through `0x4C2360`, and
`[obj+0x196]` is *also* a position the cursor is drawn at — `0x4C67C0`, called from the surface
present machinery, blits the sprite from that record without polling ([exe
RE](exe-reverse-engineering.html)). With the poll answering `s` and the move still carrying `u`,
whichever ran last decided where the sprite appeared: measured at 1920×1080 / 0.25×, the sprite
tracked `u` across the frame at 4× the pointer's speed and off the left edge — the very artefact
this set out to remove. `carries_point()` is now the button messages only.

**Records that came off the ring are left alone, and it takes TWO tests to know which those
are.** The message id at `+0x10` is the obvious one, and on its own it is not safe: the input
reset `0x4B5A88` zeroes only the first three dwords of the record it writes to `[obj+0x196]`, so
time, msg and the double-click flag are left as stack garbage while the drawing polls put a real
pointer back in x and y. A garbage msg landing in `0x201..0x206` would make a poll record look
like a press and skip the repair — `main+0x2C76` left holding a SCREEN position while zoomed. So
the record must also **differ from `[obj+0x196]`**: the fallback is a `rep movsd` of those same
six dwords, garbage included, and can never differ from them, while a real ring entry differs in
at least its timestamp. *(Found by the landing review, not by the change.)*

**Armed by `tagpu_zoom.on` as well as `tagpu_vpwide.on` (each by file or by the play default,
§2.8), and WITHOUT EITHER THERE IS NO ZOOM.**
The repair is not optional once the engine is told the truth — and the zoom's own levers,
`tagpu_zoom.txt` and the wheel, are gated by no arm file at all (`zoom.on` installs the
minimap/`ScrollSpeed`/camera-range patches and nothing more; the lever is read by
`tagpu_zoom_read_lever()` and the view published by the native pass, neither of which consults an
arm file). A build with `native.on` and `terr.on` but no `zoom.on` could therefore be wheeled to
0.5× into exactly the state this section exists to prevent: clicks landing correctly, because the
message carries `u`, while hover, the cursor-shape choice, build placement, `GetUnitAtMouse` and
the routing test all name the world under the SCREEN position. **So `tagpu_zoom_read_lever()`
asks `tagpu_vpwide_mouse_world_live()` and pins the level at 1.0 when the redirect is not in,
logging `zoom: PINNED AT 1.0` once.** The invariant is structural rather than documented, which
is what it needed to be — *found by the landing review, not by the change.*

With `zoom.on` and no `vpwide.on`, the redirect goes in and **nothing else does**: no rect is
ever widened, no other byte written. Verified live at 0.25× with `vpwide.on` absent: the viewport rect stays `(128 … 1919)`, the log reads
`vpwide: mouse->world repair only (0x498DA0)`, hover in the band is correct, and the ring stays
display-only exactly as §2.3b describes it without `vpwide`. At zoom 1
`tagpu_zoom_to_engine()` reports "nothing to do" and the stub writes nothing.

**What it closed.**

- **The widened hover, which was being clobbered every frame.** `main+0x2C76` followed the poll,
  so the wide `u` the window message correctly delivered was thrown away, and hover, the
  cursor-shape choice and build placement all named the 1× world point in the ring. Measured now,
  1920×1080 at 0.25×, `vpwide` armed, eye 2000: a commander whose 1× screen position is
  (956, 480) sits at screen (699, 525); `main+0x2C76` reads **(−276, 480)** and `main+0x2CBA`
  reads **unit 2**. The ring picks units up on hover, not only on click.
- **The right-edge scroll at zoom > 1** — §2.3c, measured there.
- **The composite's cursor machinery**, deleted: `uCur`/`uCurOff` and their branch in the
  composite shader, `CURSOR_PAD`, the pair recorder behind its seqlock, the poll filter, the
  latch at the surface upload, and `tagpu_zoom_to_engine_draw()`. The composite no longer knows
  the cursor exists.

**Measured, the same rig and scorer as the mitigation** (1920×1080, 0.25×, a real pointer crossing the band
at ~37 px per composited frame, three runs each), share of motion frames with the cursor left
behind at `u`:

| build | three runs |
|---|---|
| before the gate | 11 %, 10 %, 11 % |
| the composite mitigation (`13bc91c`) | 6 %, 7 %, 0 % |
| **this** | **0 %, 0 %, 0 %** (120–128 motion frames per run) |

Statically the sprite sits exactly under the pointer at (1100,570), (1600,600), (700,600) and
(300,500). Screen space is byte-identical at 1×, 0.25× and 2× — same `main+0x2C76`, same map
cell, same `main+0x2CC6` — over the minimap, the side panel and below the bottom bar. Box select
and click-select both pick the commander in the band at 0.25×, in the ring at 0.25×, and at 2×.

**Two gaps the landing review found and this did NOT close**, both pre-existing and neither
touched by the change:

- **The drag-scroll anchor drifts at zoom != 1.** `0x41CD50` subtracts a *screen*-space centre
  (`main+0x2CE3`/`+0x2CE7`) from a record that now reliably holds `u`, so the anchor is off by
  `64·(1 − 1/z)` px in x. It was equally wrong before, from a record that held whichever of `s`
  or `u` had landed last; the repair makes it *consistently* wrong rather than intermittently.
- **The whole repair is skipped when `[[main+0xC]+0xF1] & 8` is set**, because that bit gates
  `0x499A1C`, the call to the input-mode handler. What the bit means was not established.

**What it did not close.** In the ring the engine's own build-placement footprint is not drawn at
all: the engine projects it to a `u` off the surface and the clip discards it, so `markown` has
nothing to capture. Before, it was drawn — in the wrong place, from a wrong cell. The cell is now
right and the preview is absent; closing that means drawing the footprint ourselves, the same
answer `ui-markers.md` §6.1 gives for everything else outside the 1× viewport. **Dialogs drawn
over the viewport** (`ARMOPT`, `EXITMENU`) are unchanged and still take the transform as though
they were world, which was already true before this and is not verified either way here.

### 2.3e The GL UI layer — observers, publisher, twins (`tagpu_gui_hook.c`, `tagpu_gui_surf.c`, `gui.on`, Phase E G15a + G15b + G15d)

Nothing here changes what the engine draws. Every site is an **observer detour**
(`tagpu_detour_observe`): the original runs unchanged, we read its arguments on the way in and,
for the allocator, its result on the way out; every register and EFLAGS are saved around both
calls (`pushfd/pushad … popad/popfd`, since the 2026-09-07 review). Installed once at DllMain when `tagpu_gui.on`
exists, byte-matched, all-or-nothing — 17 sites. **One site was already owned**: `fxown` holds
`CopyGafToContext 0x4B7F90`, so the observer **chains** onto fxown's stub (the shared detour code
now records every landed stub and hooks the earlier stub's copy of the stolen bytes; `own the
draw` §"chaining") rather than overwriting its jmp — fxown's skip still wins, and the observer
sees only the blits that really draw. Full argument lists, boxes and evidence: the engine map's
"The UI surfaces and their writers".

| VA | What it is | Stolen | Observer records |
|---|---|---|---|
| `0x4C63A0` | `FlipOffscreenToPrimary` — the engine's "this frame is complete"; the census runs here | 6 | the frame marker; diffs `*(globals+0xBC)` and every surface an op named |
| `0x4B7F90` | `CopyGafToContext(ctx, frame, x, y)` — **chained onto fxown's stub** | 6 | a sprite box at `(x−HotX, y−HotY)`, clipped |
| `0x4B8500`, `0x4B8310` | the shaded blit and DrawText's alternate blit, same shape | 6 | same |
| `0x4C6D20` | descriptor blit `(ctx, desc, src, dst)` — listbox, textfield | 7 | `*dst` |
| `0x4C7580` | the textured-triangle stamp `(ctx, src, xy[6], uv[6])` — the option screens' wide backdrop | 5 | the vertices' bounding box |
| `0x4CCF60` | the glyph blitter, cdecl 9 args | 6 | the string's box from the font's width table |
| `0x4BE950`, `0x4BF6F0`, `0x4BF8C0`, `0x4BF7B0`, `0x4BF4D0` | line, bar, hollow rect, focus rect, framed box | 8/7/6/7/7 | the rect, clipped |
| `0x4C6890` | `SurfaceFill(surface, colour)` | 7 | the whole surface |
| `0x4C6B70` | surface → surface `(dst, src, x, y)` — the GUI panel reaching the frame | 8 | the source's box at `(x−originX, y−originY)`, clipped |
| `0x4C69F0` | `SurfaceCreateNamed(tag, w, h)` — return hijacked | 6 | registers the surface, seeds its copy so its build is diffed |
| `0x4C6AC0` | `SurfaceFree(surface)` | 6 | forgets it |
| `0x4A81E0` | `GUI_StageUpdateDraw(gi, flags)` | 10 | a build/redraw event for the log |

Tokens in `tagpu_gui.on`: `strict` (the fallback off, a miss painted magenta, the cursor rect
exempt — the harness's mode), `off` (the detours stay installed for the next launch, nothing is
published or drawn — the live A/B lever), `census` (the G15a diff), `log` (a census line per 50
censuses and on any residual), `pgm` (`tagpu_gui_census.trigger` → `tagpu_gui_census.pgm`, the
accumulated unexplained mask), `trace` (the ops intersecting a residual, the first blits after a
build, every allocation with its tag), `probe=x,y` (with `trace`: every published op touching
that pixel of the presented surface), `key=N`. **MEASURED** 0 unexplained of 3 710 035 changed
pixels across the inventory (engine map, "What the census measured").

**The layer (G15b).** Nothing is patched beyond the observers above; the drawing is a second
half on the render thread, and the two halves meet only in a lock-free SPSC queue
(`tagpu_gui_int.h`: 65 536 ops and a 16 MB byte arena).

- *Game thread, inside the flip observer, at the census cadence (≤ 1 per 5 ms):* the ring of
  ops since the last publish becomes queue ops. A **seed** (the surface's bytes, whole) for
  every surface first seen since the last reset; a **sprite** for a plain keyed `0x4B7F90`
  blit of a frame ≤ 512 px with no sub-frames — the frame identity `(header, pixel pointer)`
  plus, on first sight, its pixels decoded here by `tagpu_gaf_decode` (the shell frees a
  popped screen's art under the render thread, so the pointer must not be read there); a
  twin-to-twin **copy** for `0x4C6B70` when the source is twinned; a **clear** of the true
  viewport rect at every flip after which `terrown`'s fill sequence advanced (the terrain skip's
  key fill is the engine's per-frame erase, mirrored); and for everything else — text, lines,
  rects, fills, the descriptor blit, the textured triangles, the shaded and sub-frame GAF
  variants, a copy from an untwinned source — **pixels**: the box's bytes as they stand at
  publish time, so a pixel op is the final state of its box and the twin converges on the
  engine's surface whatever order the writers ran in. **Identical ops within one batch collapse
  to their last occurrence** (the shell redraws every gadget on every flip — ~41 ops at ~12 000
  flips/s on `MAINMENU` — and without this the queue overflowed into a reseed storm).
  Excluded, on the return address: the unit composite blit, the cursor code, the flip's own
  blits (engine map, "What the twin layer excludes, tests and reads"). Three rules from the
  landing review: a sprite's identity is the frame's addresses **plus a hash of its plane's
  first bytes** (a popped screen's art is freed and the heap reuses the addresses); the batch
  dedup **never moves a write past a copy that read it** — an earlier duplicate is dropped only
  when no `0x4C6B70` reading its surface lies between the two, or one follows the survivor
  (a per-surface epoch bumped by every copy was tried first and re-created the shell's reseed
  storm, since the shell copies its panel to the frame on every flip); and `SurfaceFree` **zeroes the
  ring's ops on the freed base**, so a surface re-allocated over the same bytes before the next
  census is never diffed or replayed against an old box.
- *Render thread, inside `tagpu_overlay_draw` after `tagpu_native_frame`:* poll the trigger
  (500 ms), drain the queue (20 000 ops per present at most), then draw. Every seeded surface
  has a **twin**: an `RG8` texture its size (R = the palette index, G = coverage) behind an FBO,
  1:1 and `NEAREST`; sprites are quads from a `TAGPU_GAFATLAS` of the UI frames (2048², `pad 0
  align 0 mip 0`, colour key discarded in the fragment), copies are quads sampling the source
  twin, pixels and seeds are `glTexSubImage2D`, clears are scissored `glClear`s to coverage 0.
  **The seam is one draw**: the presented surface's twin over the whole frame, into the
  overlay's target FBO with blending and depth off, `discard` where coverage is 0 — so the
  native composite's key rule beneath is unchanged and the engine's pixels remain the
  fallback wherever a twin has nothing. The index is resolved through the live palette
  (`main+0x143A7`, re-uploaded when it moves). The cursor's rect (`*(0x51FBD0)+0x1B2/+0x1B6/
  +0x1BA`) is left to the engine's frame. `strict` paints a miss magenta instead of falling
  back, outside the viewport or on a non-key pixel inside it.
- *Fresh starts:* the trigger reappearing, a GL context change (`tagpu_gui_glreset` from the
  overlay's reset), a queue or arena overflow, a sprite whose bytes never arrived, a copy
  from a source with no twin, and the consumer coming back from a stall (below) all raise
  `reseed`; the next publish sends a **reset** and seeds every surface again from the
  engine's bytes. Nothing is reconstructed from history. **Since G15d every reset is logged
  with its reason** (`gui: reset #n: arm | gl-context | queue-full | arena-full |
  box-outside-surface | lost-sprite | atlas-full | untwinned-copy | stall-over`, with the
  queue and arena occupancy), so the heartbeat's `resets=` is never a bare count.
- *The consumer can die, or crawl (G15d):* cnc-ddraw stops its render thread inside every
  `SetDisplayMode` and starts a new one on a new GL context, and on the way out of a game the
  old thread presents only every few hundred ms while the game thread is in the exit path —
  while the shell already flips ~5 000 times a second and the game frame publishes ~150 KB of
  box bytes per 5 ms cadence, so the 16 MB arena is half a second of backlog. The publisher
  therefore **drops its batch** when the tail has not moved for 250 ms with work queued, or
  when the backlog is past half the arena or a quarter of the ring (`stalls=` counts the
  episodes), and publishes again — one reset, every surface re-seeded — once the consumer has
  caught up. On the render thread, after a context change the drain **skips every op up to the
  producer's next reset** (`skipped=`): they were published against twins and an atlas that
  died with the context, and applying them only counted their sprites as lost. MEASURED
  2026-09-07: before, every game → shell switch cost 38 `arena-full` overflows, 39 resets and
  705 lost sprites; after, one reset (`stall-over`), no overflow, none lost, and the game's own
  OFFSCREEN — freed to the heap by `MEM_Free` at `0x491AB8`, not through `SurfaceFree`, and
  re-created 640×480 on the same base — no longer leaves a 1024-wide box in the ring for the
  next publish to trip on (`surf_get` forgets a base's ops on a same-base size change).
- *The palette (G15d):* the twin resolves through **the palette the engine's frame is presented
  with — cnc-ddraw's `g_ddraw.primary->palette->data_rgb`, what the engine's `SetEntries`
  stored — not `main+0x143A7`**. The engine scales every palette it sets by the Gamma option on
  its way to DirectDraw (`0x4BA200`, `SetGamma 0x4BA590`; engine map, "The palette the screen is
  presented with") and never scales its own table, so at Gamma ≠ 12, or after `+gamma N`, the
  two differ and every `+0x143A7` reader — the world passes — is off by the factor. The read is
  taken under the fork's `g_ddraw.cs` (the game thread NULLs the primary inside it); the
  engine's table is the fallback until a primary exists. The heartbeat carries `palchg=` (uploads
  — a fade is a run of them) and `paldiff=n@i` (entries where the presented palette and
  `+0x143A7` disagree, and the first): 0 in game, 1 (index 9) in the shell, 255 at `+gamma 15`.

**MEASURED 2026-09-07** (`tools/uiwalk.py --layer`, `strict`, every world pass armed): **0
differing pixels outside the viewport and 0 holes on every in-game stop** — `ARMMAIN2`,
`ARMCOM1`, its second page, `ARMOPT`, `PREFS`, `VISUALRT`, back out, chat, the F4 popup — at
**1024×768 and at 1920×1080**; every shell stop 0 except `MAINMENU`, whose ~185 differing
pixels are its sparkle animation between the two shots (the diff is single scattered pixels
in the sky, none on a gadget). Resets 3 per run (the arm, the shell→game context switch, the
game's mode switch), overflows 0, the atlas at 123 of 4 096 entries after the whole
inventory. Frame rates and the parity md5 with the trigger absent: [GL UI renderer](gui-renderer.html) §10.

**MEASURED 2026-09-07, G15c** (the same walk, `--side` and nineteen more in-game stops, the
compare extended to every non-key engine pixel *inside* the viewport, each GL shot bracketed by
two surface shots): **ARM and CORE, at 1024×768 and 1920×1080, 32 of 32 in-game stops at 0
differing pixels outside the viewport, 0 inside it and 0 holes** — the `+clock` and `+bps`
strings, the hold-SPACE box, `PREFS`, `VISUALRT`, the F4 box and the chat over the world at 1×,
0.5× and 2×, the minimap's dot and view box after a move and a scroll. No hook, no field and no
GL object changed for it; the census on CORE explains 1 717 044 of 1 717 044 changed pixels.
[GL UI renderer](gui-renderer.html) §11.

**Fields we write: none.** The module reads the engine's surfaces, the palette and the mouse
object — and, since G15d, the fork's own palette object under the fork's lock — and writes GL
objects of its own; the engine's behaviour is byte-identical with it armed, on or off.

### 2.4 Tooling (not part of the render path)

| VA | What it is | Module (arm file) |
|---|---|---|
| `0x45AC20` | `DrawUnit` entry (7 stolen) | `suppress` (`suppress.on`), `tracer` (`tracer.on`) |
| `0x459200` | composite sprite → screen blit | `tracer` |
| `0x469A05` / `0x469BA8` | the two `DrawUnit` return sites | `tracer` |
| `0x4969D2` | the sim tick site (5 stolen) | `scenario` — applies a situation on the game thread |
| `0x4B08C0` `0x4B0DA0` `0x4B19D0` `0x4B1A99` + the `E8` at `0x4B15E0` | the COB engine's thread allocator (5 stolen, wrapped: the original runs through the stolen prologue, then the start is latched), the thread runner's entry (5), the `RETURN` handler mid-function (5), the `signal` kill mid-function (6), and the `rand` handler's call into the sim RNG `0x4B6C30` (call-site redirect that calls it itself) | `cobtrace` (`tagpu_cobtrace.on`, its contents a unit-type filter) — the COB script-call oracle for tacob: S/R/X/K/D lines to `tagpu_cobtrace.log`, stamped with the sim tick `main+0x38A47`; reads only, game thread only; all five or none. `tagpu_posedump.on`'s header line now carries `tick=` and `idx=` so the two logs join — [exe-reverse-engineering](exe-reverse-engineering.html) §"The COB engine", [tacob-design](tacob-design.html) §"The trace contract" |
| — | `tagpu_shadowdump.on`: the Classic++ shadow map written once as a 16-bit PGM, `tagpu_shadow.pgm` (near = small), with its matrix on the `shadow: dumped` log line — the lab's `debug=shadow` for the game; how a caster's silhouette in light space is checked instead of guessed (G14i) | `tagpu_shadow.c`, no engine address |
| `0x485F50` `0x4864B0` `0x422DD0` `0x4224B0` `0x481550` `0x423C50` `0x43F0E0` `0x43AFC0` | `CreateUnit`, `KillUnit`, `FeatureName2ID`, `LoadFeature`, `GetGridPosPLOT`, `SpawnFeatureOnMap`, `ScriptAction_Type2Index`, `NewMainOrder2Unit` | `scenario` — *called by us*, never patched. **`NewMainOrder2Unit` takes 16.16 in `{x, altitude, depth}`**, the same convention as `CreateUnit`; we passed whole units in `{x, depth, altitude}` until 2026-09-04 and every ordered unit walked to the map origin — [exe-reverse-engineering](exe-reverse-engineering.html) §"The order module" |

**The window title** (`tagpu_title.c`) patches no engine address at all: it is a
`SetWindowTextA` from inside `dd_SetCooperativeLevel`, composing `"<stock title> - <label>"`
from `tagpu_title.txt` (tacli writes `wt:<branch> | tacli:<instance>` there by default; how
the label is *composed* is tacli's business, the DLL only appends it). Listed here only
so the module is accounted for — it reads no engine state and writes none. Placed after the
`GetWindowText` into `g_ddraw.title`, so cnc-ddraw's own per-game `strcmp`s and
`screenshot.c`'s filenames still see the unsuffixed name.

### 2.5 State we read, and the fields we write

| Where | What |
|---|---|
| `0x511DE8` | `TAdynmemStruct**` — the root of everything below |
| `main+0x14357` / `+0x1435B` | unit array begin/end, stride `0x118` |
| `main+0x1435F` / `+0x14367` | HotUnits ids / count (culled to whatever the viewport rect says — the unzoomed one, or the widened one under `vpwide`) |
| `main+0x1431F` / `+0x14323` | eyeX / eyeY. **WRITTEN**, and only ever *clamped*: our replacement of the engine's own clamp widens its range to what the zoom shows (§2.3c), and `apply_eye_range()` re-applies the same bounds once a frame so a zoom-out cannot leave the eye past them. Sim-neutral for the same reason `ScrollSpeed` is |
| `main+0x14327` / `+0x1432B` | `MapXScrollingTo` — where the camera is heading; the stepper `0x41CA30` eases the eye toward it. **WRITTEN by `apply_eye_range()` only**, clamped to the same range as the eye and for the same frame, because a disagreement between the two costs the fog grid its is-current flag every frame (§2.3c). The replacement clamp deliberately does **not** touch it — three of its callers are inside the stepper, and writing the target there would stop the camera ever arriving |
| `main+0x142CB` | the minimap's view RECT. Engine-drawn and engine-filled — `0x41C3C0` is the only place it is computed — so `apply_eye_range()` recomputes it through the same wrapper on the frames it corrects the eye. The one **render-thread** write of it; a game thread drawing the minimap in that instant sees a one-frame torn box, the same standing as the published view |
| `main+0x37E27..0x37E3B` | viewport rect: L, T, R, B, then W, H. **L/T/R/B are WRITTEN while `vpwide` is live** (§2.3b); every pass that means the true 1× rect must call `tagpu_vpwide_true_rect()` rather than read the field |
| `main+0x2C76` / `+0x2C7A` | mouse position, two dwords (`+0x2C78` is the high half of x, not the y) — the x and y of the 6-dword record the dispatch fills. **WRITTEN by `vpw_mouse_world()` while the zoom transform is live** (§2.3d): the engine is polled with the true pointer now, so the unzoomed `u` is put back here, where `GetUnitAtMouse 0x48CD80` and the routing test at `0x469DE1` read it. Untouched at zoom 1, on the screen-space UI, and for a record that came off the event ring. **Local, but NOT inert:** all three fillers (`0x4999C4`, `0x4999E7`, `0x4999F9`) and our write sit inside one game-thread tick, before the first reader, so nothing races — but the readers include the order dispatchers `0x419BE0`/`0x41A490`, so a wrong value here becomes a wrong **replicated order**, not just a wrong highlight. That is why the ring test above has to be exact |
| `main+0x0DCB` | GUI colour byte array (`gui[i]` is an INDEX INTO this, not a palette index). `DrawGameScreen` caches it in a local at `0x468D49`/`0x468D51`, which is the `[esp+0x74]` the build-cursor block indexes |
| `main+0x2CC3` / `+0x2CC6` | cursor/order mode byte and the mouse-region flags. Read only. The build-cursor draw keys off `0x2CC3 == 0x0E` (placement) or `0x2CC6 & 8` (band drag), and picks green vs blocked from `0x2CC6 & 0x40` |
| `main+0x2C92`, `+0x2C96`, `+0x2C9A`, `+0x2C9E`, `+0x2CA2`, `+0x2CA6` | the build-cursor / band-box rect: two corners as (world x, altitude, world z) DWORDs. Read only, once a frame on the render thread, and projected with the engine's own baked `+0x80`/`+0x20` origin — **not** `main+0x37E27`'s L/T, which `vpwide` widens |
| `main+0x1B63` (+ watched × `0x14B`) | PlayerStruct; `+0x67`/`+0x6B` its first/last unit pointer — the range the order-marker driver walks. Read only, game thread |
| `main+0x37E9C` / `main+0x2CBA` | tracked / hovered unit id. Read only; with `viewStruct+0` (`CameraToUnit`) these are the three units the order-marker driver's selection rules key off |
| `main+0x1487F` / `main+0x148D3` | `cursor_ary[0x15]` and the `pathicon` GAF sequence. Read only, once each per session — the order pass decodes frame 0 of each for the ink its procedural dot and crosshair inherit |
| `0x512344` / `0x512348` | the order-descriptor array and its end: `+0xC` is the type's marker-capability mask and `+0x10` its `cursor_ary` index. Read only; the end pointer is what lets us bound an index the engine does not |
| `unit+0xAC` | the squad tag. Read only — and read as a **DWORD** and then used as a byte, because that is what `0x469C55`/`0x469CD1` do |
| `main+0x2A43` | the player id the health-bar and group-digit loop compares unit owners against (`0x46967D` → `[esp+0x70]`, read at `0x469CA6`/`0x469CC9`). Read only. **Not `main+0x2A42`**, which is what the order-marker driver `0x48CC30` uses for its player range — two bytes, two loops, one block, written independently at `0x416B25`/`0x416B38`. `tagpu_mark.c` was on `0x2A42` from G13d until G13p corrected it |
| `[0x51FBD0]+0x204` / `+0x208` | the current font object and text foreground colour. Read only, on the GAME THREAD at hook 8: the engine re-points both many times a frame, so a present-thread read would get whatever the side panel last drew with (`tagpu_text.c`) |
| **order node `+0x32`, `+0x34`, `+0x42`** | **the target sprite's last-seen cache. WRITTEN, on the GAME THREAD, at the instant the engine's own drawer would have written it.** It is the only sim-side field this stack writes for a marker, and it is not optional: the cache is what stops a waypoint marker following a target that has left LOS, so a port that drops it leaks the target's live position (`tagpu_order.c`, `resolve_sprite`) |
| `main+0x37F06` bit0 | `damagebars` registry option |
| `main+0x37F06` bit2 / bit3 | the graphics options `Shadow` / `TShadow` (the blit tests `al,4` at `0x45928E`, [shadows & cloak](shadows-cloak.html) §2). Read only, per frame. The Classic silhouette needs both, the slant only bit2 — and **since G14i bit2 also gates the Classic++ shadow map** (with `shadows=1` in the cfg), so the player's in-game Shadows toggle keeps its meaning under the switch; bit3 is ignored there |
| `main+0x37F2F` bit2 | `SelBoxes` |
| `main+0x142E7..0x142ED` | minimap rect on screen |
| `main+0x1423B` / `+0x1423F` | view size in map cells (the minimap rect's size comes from here) |
| `main+0x14283` / `+0x1428B` | `TILE_SET` `{count, pixels}` and the `u16` `TILE_MAP` — the terrain pass reads both per frame. The GLSL restorer reads them once more when its job starts, on the render thread: each tile's edge texels for the tileability test and the whole tile map once, to rank tiles by distance from the centre of the viewport (`tagpu_terr.c` `restore_order`, G14e: from the centre, so the reveal radiates); the pixels themselves are sampled from the R8 atlas already on the GPU |
| `main+0x143A7` | the live palette, 256 × (R, G, B, pad) — uploaded per frame by the native passes; snapshotted once per job by the restorer into its own 256×1 texture (the terrain's, and since G14e the feature and effects atlases' — `tagpu_feat.c`/`tagpu_fx.c` hand it to `tagpu_gaf_atlas_restore` each frame, and `tagpu_gaf_atlas_get` reads it for the tileability test of every frame it uploads; since G14g the unit atlas's too — `tagpu_native.c` hands it to `tagpu_r3d_atlas_frame` once per frame before its first UV lookup) |
| `main+0x14287` | the `FeatureStruct` grid, one 13-byte record per 16-px cell, `mapW16 × mapH16` (`main+0x14233`/`+0x14237`). Read only. `tagpu_feat.c` reads the height byte (`+0x04`) of the anchor's four corners per anchor per frame for the engine's own projection, and since G14f the four central-difference neighbours too, for the ground's lambert (Classic++ only); `tagpu_terr.c` (G14f) copies the height byte of **every** cell once per map, when it builds the atlas, into an R8 texture the terrain shader samples — keyed on the grid pointer, the dims and the tile set, re-checked every frame; the grid is `IsBadReadPtr`-checked whole before the copy (7 MB on Two Continents), and an unreadable grid leaves Classic++ terrain **unlit** (the restored colour and the grey rule stay, the lambert is skipped), logged and retried every 60 frames |
| **`main+0x1434D`** | **`ScrollSpeed`** — sim-neutral (a local camera preference no other machine ever sees), driven at base/z, and its save path is guarded (§2.3) |
| **`UnitOrders->Pos`, `unit+0x5C` → `+0x22`/`+0x26`/`+0x2A`** | **WRITTEN, and it is SIM state** — not by us directly but by `ORDERS_NewMainOrder2Unit 0x43AFC0`, which the scenario applier calls on the game thread from the tick site. Three 16.16 dwords, `{x, altitude, depth}`, copied verbatim by the constructor `0x43A0C0`. An order is a sim command and replicates in multiplayer, so a wrong value here is a wrong game, not a wrong picture; the applier is a fixture tool and is never armed in a played session |
| **`*(0x51FBD0) + 0xC0`** | **the blend LUT pointer. WRITTEN, transiently, and this is the one field we write that is NOT in `main`.** Swapped to an identity table across the target sprite's draw and restored on return, so the star composites as a copy (§2.2). Game thread only, bracketed around one call that always returns, restored only if ours is still installed, with a belt-and-braces restore at hook 8. It must never be left installed across a frame: `0x4BA5C0` allocates that buffer, `0x4BA5F0` frees it and `0x4BAAD0` refills 64 KB through the pointer, so a stale one of ours would be clobbered or cross-heap-freed |

### 2.6 Engine byte patches — no hook, no state (`tagpu_patches.c`)

Not detours and not redirects: bytes rewritten once in `DllMain` through `VirtualProtect`, each
written only if the site still holds the value we recorded. They own no state and run no code of
ours, so they are listed here rather than in §2.1–2.5. The table of them with the before/after
bytes is `field-notes.md` §"Our engine patches".

| VA | What it is | Mechanism |
|---|---|---|
| `0x4266A7` | the `jne` that reaches TA's startup DirectX-version warning | `75` → `EB`, so the warning is always skipped |
| `0x43E50C` | `je 0x43EB02` — the `Interface Type == 1` arm of `0x43E490`'s order-1 (contextual) case, which suppresses `cursormove`, `cursorreclamate` and the rest | `0F 84 F0 05 00 00` → `90` ×6, so the contextual cursor always takes the classic branch. `0x43E490` has exactly one caller (`CorretCursor_InGame 0x48D220`) and no address literal in the image, so it governs which sprite is chosen — but the index it returns is *also* the left button's state, which is what the row below is for. `tagpu_curs.off` opts out, read once at attach |
| `0x499041` | the left click's own dispatch inside `0x498F70`: `cmp dl,0x11 / jl` on `main+0x2CBE`, the installed cursor index, deciding "issue the order" against "deselect everything" | 27 bytes for 27, decided on `main+0x37EFA` and the order byte instead: `cmp [eax+0x37EFA],1 / jne classic / cmp cl,1 / jne classic / jmp deselect / classic: cmp dl,0x11 / jl act / jmp done`. Armed only when the `0x43E50C` patch above took, and off with the same `tagpu_curs.off` |

Neither writes engine state, so neither appears in §2.5. The engine still draws the cursor
itself — the composite only moves it (§1); what the patch changes is which sequence out of
`cursor_ary` (`main+0x1487F + idx*4`) the engine hands to `SetUICursor 0x4AB400`.

**Why the second row exists.** `0x43E50C` alone is not cursor-only, and shipped as a bug from
G13j until 2026-09-07: `0x4992AD` stores the chosen index in `main+0x2CBE`, and `0x499027` — the
left click's action — dispatches on that byte, treating anything below `0x11` as "an action
cursor, issue the order". At `Interface Type 1` the engine's own arm returns only 15/17/18/19,
so `< 0x11` never happened and the compare *was* the type-1 rule; feed it the classic 14
`cursormove` and the left button starts issuing move orders alongside the right one. Measured
live, commander selected on Two Continents at type 1: a left click on ground walked the unit to
the clicked point, and the same click with `tagpu_curs.off` deselected it and moved nothing. The
full path is `exe-reverse-engineering.md` §"The in-game mouse buttons".

---

### 2.7 Deferred reclamation of the engine's model objects (`tagpu_reclaim.c`, on by default, `tagpu_reclaim.off`)

The one module that patches nothing the engine *draws* with: it changes **when** a freed block
goes back to the heap, and nothing else. The render thread gathers a unit's or wreck's
`Object3do` and dereferences it later in the same frame; the game thread frees it on death —
`200v200` faulted about 95 s in, two runs in three (`0x486D9E`: the object is freed one
instruction *before* its pointer is nulled and well before the alive bit is cleared, so the
gather's alive gate cannot help). Two sites, byte-matched, all-or-nothing, installed at
`DllMain`, disjoint from every detour above:

| site | mechanism | what |
|---|---|---|
| `FreeObjectState 0x45AAA0` | prologue detour, `tagpu_detour_leaf_call` shape built by hand so the stolen tail is also a callable trampoline | while armed the call **enqueues** the object and returns (`ret 4`); the engine's own null of `unit+0x9E` / `rec+4` and its alive-bit clear run unchanged. Every `Object3do` free — unit death, wreck destroy, the bulk teardown loop — goes through this one entry |
| level teardown `0x491B60` | **wrap**: `pushad; call pre; popad; call <stolen tail>; pushad; call post; popad; ret` (no stack args; two exits — `ret` at `0x491C59` and a tail-jump to `0x450DD0`, which also takes nothing and returns with `ret` — so its body can be called and returns to us either way) | pre: raise a flag (fenced), wait ≤ 1 s for the render thread to leave its pass; if it does, **flush** the queue through the real destructor while the composite registry it walks is still alive and let the cascade (every unit, through `0x485980 → 0x4864B0 → 0x4866D0`, plus the wreck loop) free synchronously; if it does not, **nothing is freed** — the queue is kept and deferral stays on through the cascade (`held=`), draining at the next game's first deaths; post: deferral back on, flag down |

**The reclamation rule is quiescence, never a count.** `render_ogl.c` brackets
`tagpu_overlay_draw` — the render thread's only reader of these objects — with
`tagpu_reclaim_pass_begin` / `pass_end`, one unconditional pair so every early return inside the
overlay closes it. The reader publishes *pass-started* (an `InterlockedIncrement` before its first
engine read) and *pass-completed* (after its last). The real free runs on the **game thread**, from
the next `FreeObjectState` call: every entry already queued has had its record nulled since
(program order), so after a full fence the drain stamps them with pass-started and frees each
once pass-completed has reached its stamp. An idle reader passes at once; a reader mid-pass makes
the entry wait for that pass; a stuck reader freezes reclamation and the 4096-entry ring leaks
on overflow — never a synchronous free, never a spin. The drain is pumped only by frees, so the
most recent death's object (and its composite-registry slot) is held until the next death or the
level ends — one object for the length of a lull, harmless: the engine draws from the unit array.
While a teardown is in progress the driver skips only its engine-reading half (input injection,
the GL-context-change detection and the flushes still run). Measured on `200v200`: high-water 6,
overflow 0, every free drained within a frame or two. The engine sees no different value: the
sim never reads the object or its posed geometry back, and two stock peers already stay in
lockstep with different heap layouts. `tagpu_native.c` keeps a belt-and-braces re-read of the
record pointer before each emit (wrecks included: the record is kept in the gather now) and skips
a unit whose object moved (`reread=N` on the `native:` line).

Read it in `tagpu.log`: `reclaim: ARMED FreeObjectState@0x45AAA0 -> deferred …` at launch, then
every 300 frames `reclaim: def=… drn=… queued=… hw=… ovf=… foreign=… flushed=… held=…
teardowns=… pass=…` (`def` deferred, `drn` drained, `ovf` must stay 0, `foreign` a call from a
thread other than the game thread — freed synchronously and counted, never seen; `held` the
teardowns where the reader did not leave its pass in time), and at a level change either
`reclaim: level teardown: flushed N queued object(s), reader idle; the cascade frees synchronously`
or `… reader still in its pass after 1000 ms — N queued object(s) KEPT, the cascade's frees deferred`.
`tagpu_reclaim.off` in the gamedir disables the whole module at launch (the A/B lever, and the
way back to the racing build). Design, proof and the object catalogue:
[Thread-safe destruction](thread-safe-destruction.html).

### 2.8 The play defaults (`tagpu_opt.c`, `tagpu_defaults.off`) — since 2026-09-08

Every pass is armed by a file beside `TotalA.exe`, `tagpu_<x>.on`, whose contents are its tokens;
[renderers](renderers.html) §2.10 makes those files the store the in-game menu will drive when it
exists. Until it does, one table stands in for the menu: **with no file at all, the play set is
on.** `tagpu_opt.c` answers two questions for the seventeen files below — *is the pass on*
(`tagpu_opt_on`) and *what are its tokens* (`tagpu_opt_read`) — and every reader of those files,
at attach and on its per-frame poll, asks it instead of the file system. Nothing else changed:
the parsers, the polls, the `*own` install-at-attach rule.

| file | default tokens | only with |
|---|---|---|
| `tagpu_native.on` | `all wrecks` | |
| `tagpu_owndraw.on` | `all` | `native` |
| `tagpu_terr.on`, `tagpu_terrown.on` | | `terrown` with `terr` |
| `tagpu_feat.on`, `tagpu_featown.on` | | `featown` with `feat` |
| `tagpu_fx.on`, `tagpu_sfx.on`, `tagpu_fxown.on` | | `fxown` with `fx` or `sfx` |
| `tagpu_mark.on`, `tagpu_markown.on`, `tagpu_order.on` | | `markown` with `mark` |
| `tagpu_zoom.on`, `tagpu_vpwide.on` | | `vpwide` with `zoom` |
| `tagpu_gui.on`, `tagpu_classicpp.on`, `tagpu_weapons.on` | | |

The rules, in precedence: a `tagpu_<x>.on` that exists wins, tokens and all, as ever; a
`tagpu_<x>.off` (and no `.on`) turns a default-on pass off; `tagpu_defaults.off` turns the table
off — every pass opt-in again. The *only with* column is the pairing `tacli launch` makes when it
auto-arms the `*own` half: a default `owndraw` with no native pass would skip the engine's
rasterise and draw nothing (the "stale owndraw.on" footgun of the ta-drive skill), so `native.off`
takes `owndraw` with it, and `zoom.off` takes `vpwide`, because the repair `vpwide` carries is what
the zoom cannot go live without (§2.3b). Everything off the table — the instrumentation triggers,
the knob files `tagpu_hires.on` / `tagpu_restoreglsl.on` / `tagpu_classicpp.cfg`, the `.off`
levers of §2.7 and the render passes — reads its file exactly as before. The module is stateless
(the readers poll on their own cadence) and reads nothing until asked; one line at attach,
`opt: play defaults ON (no tagpu_defaults.off): native=all wrecks owndraw=all terr …`, or
`opt: play defaults OFF (tagpu_defaults.off present)`, says which applied.

**tacli opts its instances out.** An instance is a lab bench: a bare launch has to stay the stock
control and a measurement has to arm exactly the passes it names, so `launch` and
`scenario load` write `tagpu_defaults.off` into the gamedir unless given `--defaults` (sticky per
instance, `--no-defaults` back). Every arm-set recipe in the skill therefore still holds; under
`--defaults` a pass is turned off with `tacli arm <i> <pass>.off`.

**Measured 2026-09-08** (Two Continents, 1024×768, `scenario load … one-unit --defaults`, no arm
file in the gamedir): the `opt:` line listed all seventeen; `owndraw`, `fxown`, `featown`,
`terrown`, `markown`, `gui`, `zoom`, `vpwide` and `weapons` each logged `ARMED` at attach, and
`fx`, `sfx`, `feat`, `terr`, `mark`, `order` and `native` (`1 unit(s) … 555 verts`) in the game;
Classic++ lit the frame with the lab's defaults and restored the 5062 tiles in 2494 ms at 58.5 fps.
Writing `tagpu_classicpp.off` while it ran changed 424 380 of the 786 432 GL pixels within a
second (the world back to Classic, the UI unmoved); deleting it restored them. The same instance
relaunched with `--no-defaults` logged `opt: play defaults OFF` and armed only `curs`, `reclaim`
and `shield`, the three that were never on the table. **Not measured**: a player's Windows, which
is what the `_local` test VM is for; the defaults on a map change (the `*own` halves are attach
time, the rest re-read every 30 frames, so nothing new is expected).

## 3. Known limits — what is still wrong, and what closing it needs

### 3.0 Closed since the last pass: the interior cracks at zoom-out

**Reproduced, root-caused and fixed** ([terrain & depth](terrain-depth.html) §7.6, the *fifth*
mode). Two artefacts, one cause: at zoom 0.25 a quad's far edge can land exactly on a fragment
centre, and that fragment's `u`/`v` interpolates to exactly `u1`/`v1`, which `GL_NEAREST` reads as
the first texel of the **next atlas cell** — an unrelated tile for terrain (the blue hairlines
along tile edges) and the packer's unwritten gutter for a GAF sprite (the black hairline down the
right of every tree). It is **not** a key leak, which is why the key-tint detector used for 350+
frames of sweeping was blind to it by construction.

Fixed by a 1-texel replicated border on all four sides of every atlas cell (terrain now on a
34-texel pitch, 2176×2720; GAF frames advance `w+2`/`h+2`) **plus** `TAGPU_EDGE_NUDGE`, a 1/32
game-screen-pixel offset in the terrain vertex shader — the border alone leaves the fragment
reading a repeated row that is out of phase with the minified tile's sampling cadence, which on
dithered tile art is still a visible line (measured: 1.92× → 1.86×, i.e. no help). Verified flat
(1.03–1.13× against a 1.92–2.05× baseline) at every camera phase, `ss=1` and `ss=2`, 1024×768 and
1920×1080, across ten zoom levels; **1× output is bit-identical** to a build without the change.

The border is also what a filtered sampler will need when the atlases stop being `GL_NEAREST`,
which is why it is on all four sides rather than only the two that close today's bug.


Ranked by how much they cost a player.

### 3.1 The ring at zoom < 1 — closed for play, bounded for markers

At zoom < 1 the view shows more world than the engine's 1× viewport can **name**, and until
G13f that made the outer ring display-only: a click there was dropped whole rather than landed
on the wrong world point, and the captured marker layers clipped at the same edge. **G13f closes
the input half** — `vpwide` (§2.3b) widens the rect the engine addresses to exactly the range the
zoom transform produces, so a unit in the ring can be selected and ordered like any other. It is
**on by default** since 2026-09-08 (§2.8); in a tacli instance, which opts out of the defaults,
`tacli arm <i> vpwide.on`, at launch like every other code-patching pass.

**What it took, and why the shape is not obvious.** The rect's readers want four different
things — bounds, origin, clip, and W/H — and §2.3b is that table. Two of them are traps:

- **`0x498DA0` uses L and T as the screen→world ORIGIN**, not as a bound. Measured: moving
  L 128→0 and T 32→0 shifts the map cell under the cursor by exactly `(+8, +2)` cells. It is
  redirected and redone with the true origin and the wide clamp.
- **TA's own window procedure zero-extends the mouse `lParam`** (`AND 0xffff` / `SHR 0x10`, all
  three arms of its `0x200..0x206` table), so a negative client x arrived as 65516 and the whole
  event vanished. Hover worked and clicks did not, for exactly `x < 0` or `y < 0` — which is half
  the ring. The byte patch is `GET_X_LPARAM`, identical for any position a real mouse can report.
- **The engine can name more than it can draw on.** Widening the addressable rect also moved
  where the engine drew its cursor, and in the ring that is the side panel or off the surface —
  measured as no cursor at the pointer and a ghost one on the build panel. The cursor poll got
  its own transform, which keeps the ring identity.

And one crash the survey did not predict and running it did: the widened clamp reaches world
points the 1× viewport never could, `GetGridPosPLOT` returns NULL outside the plot grid, and
`GetGridPosFeature` dereferences whatever it is handed — an access violation at `0x421E64`
reading `[NULL+8]` during an edge scroll at 0.5×. The world point is now clamped to the map, the
cell to the plot grid, and the plot null-checked even then. **The engine is not defensive about
inputs its own eye clamp made impossible; a widened viewport is exactly what makes them possible.**

**What is still bounded.** The captured marker layers improve but do not close, and the bound is
not the rect any more — it is the **engine's offscreen, which is screen-sized**. Markers are drawn
by the engine into a buffer of ours through its own clip; that clip now reaches the surface edge
instead of the 1× viewport, so at 0.5× on a 1024×768 frame they cover screen `[288,863]×[192,575]`
where they used to stop at `[352,799]×[208,559]` — confirmed with a waypoint at `s=(318,542)` that
used to be clipped. Beyond that the engine would have to draw at a negative position into a
screen-sized buffer, which it cannot. Closing that half means giving the capture window its own
wider buffer and offsetting the base so negative engine coordinates land inside it: `markown`
already swaps `ctx[CTX_BASE]`, so it would also have to own `CTX_PITCH` and the clip fields.
Health bars are unaffected either way — they are re-drawn from unit state, not captured.

**MP-safety** is the `ScrollSpeed` argument unchanged: the viewport rect is camera state that no
other machine ever sees. G9's replay byte-diff would settle it for good.

**If it ever needs undoing:** the two routes that were not taken are shifting the eye for the
duration of a click (exact, but it assumes the engine consumes the click during dispatch rather
than queueing it, and our overlay reads the eye on cnc-ddraw's render thread so a transient bias
races it), and resolving the click ourselves against `ORDERS_NewMainOrder2Unit 0x43AFC0` — which
means reimplementing selection, box-select, build placement and every cursor mode.

### 3.2 Smaller, known, and cheap to close

- **The Kbot lab's Classic slant shadow — found mostly missing on 2026-09-06, closed 2026-09-07
  (G14j).** Not the piece flags: the slant was drawn through the silhouette's waterline erase,
  and the fixture's lab on the shore (altitude 63, sea level 75, a path-B composite) lost every
  shadow fragment below model height 12. `emit_slant` now follows the engine's raster and the
  draw exempts a slant unit from the erase; the engine's own Shadows-toggle mask reads **1094 px
  against the stock engine's 997** (was 515 / 959), the other buildings as before —
  [shadows & cloak](shadows-cloak.html) §"Structure shadows, parity". The lab's Classic lane
  draws the slant for structures since the same day ([renderers](renderers.html) §1).
- **A replacement mesh casts a Classic++ shadow and does not receive one** (G14i,
  [renderers](renderers.html) §2.4): `tagpu_hires_draw.c` lights with its own GGX rule and has no
  shadow read-back, so a glb body standing in a cast shadow is lit as though it were not. The
  depth-pass half is done; the read-back waits on the hires lighting decision.
- **The Classic++ shadow's soft edge and the ridge haze are lattice noise** (G14i): the
  PCF's bilinear compares round differently wherever texel centres fall, and the game's
  map-anchored lattice is not the lab's view-anchored one — 156 game-only pixels within 2 %
  on the parity fixture's hills stage (11 within 5 %), 60 lab-only of the same kind. Invisible;
  a larger constant bias would trade it for peter-panning at the shadow's root.

- **CLOSED (G14h, 2026-09-06): the native pass faulted on a unit or wreck freed mid-frame.**
  Found 2026-09-05 measuring G14g, present on the G14f DLL too: `200v200` about 95 s in, twice
  in three runs, an access violation at the first instruction of `emit_geom` reading a model
  object the game thread had freed between the gather and the emit. Instrumentation put the
  race at 43 deaths inside the gather-to-emit window over two 210 s fights (12 units, 26 wrecks
  in the second), the fault needing the extra step of the freed page becoming unreadable — the
  CRT small-block heap decommits pages inside a free, and the one-piece wreck objects live there
  (the crash's faulting index was a wreck). The engine frees the object *before* it nulls its
  pointer and clears the alive bit (`0x486D9E → 0x486DA3 → 0x486DCE`), so no read-side gate could
  close it; `tagpu_reclaim.c` (§2.7) defers the free itself behind the render thread's published
  quiescence. `tacli crash` prints the *first* report in `ErrorLog.txt` (TA appends), so read the
  file's tail after a second crash.

| Limit | Where | What it needs |
|---|---|---|
| **Palette-space blend tables not reproduced.** Feature shadows are a true 50 % RGB blend where the engine remaps through `ALP[src·256 + dst]`, which quantises to palette entries and therefore *dithers* — the diff map shows a speckled crescent on each tree's shadow side, clean bodies. The effects pass's LHT flash is the same class of deviation | [Features](features.html) §8, [Effects](effects.html) | sample the real ALP/LHT tables in the shader instead of blending in RGB |
| The engine used to shade-remap its **own** overlays under the fog's grey band; we no longer do | [Terrain & depth](terrain-depth.html) §7.7 | visible only if a health bar were drawn on out-of-LOS ground, which the engine does not do — probably nothing to do |
| The options screen shows the **scaled** `ScrollSpeed` while zoomed | §2.3 | cosmetic; it is the rate scrolling is actually running at |
| Body 2 of `0x46A610` (animated GAF wreckage) is written but was never reached live | [Features](features.html) §8 | a fixture that produces animated wreckage |
| The engine's own feature `+0xFF` bit3 LOS gate never fired on the maps tested (`los-skip` is always 0) | [Features](features.html) | a map that exercises it, or accept it as dead on stock content |
| Layer 8 and the `0x4FD588` particle class were never observed live (the latter is emitted by teleport and `0x472630`) | [Effects](effects.html) | a fixture that emits them |
| The sim-side particle update walker's entry is not pinned | [Effects](effects.html) | nothing — only the DRAW is owned, by design |
| Cloak polish (true alpha) | [Shadows & cloak](shadows-cloak.html) | a cloakable unit in a fixture |
| **The hires stencil shadow is correct by construction but unproven on content.** `tagpu_hires_draw` got the same per-unit mark/blend as the 3DO path (G13n), because it blended `alpha 0.5` per fragment with depth writes off and a replacement mesh is denser than a 3DO. The only replacement mesh that exists is a Peewee, which is single-layer from above — the A/B (npass forced to 1 vs 2, `hires-peewee`, 10 frames each) shows the peak staying at 0.44–0.52 and no 0.25 mode either way, exactly as the stock Peewees were bit-identical across the 3DO fix. So the change is verified not to regress, and its benefit is unverified for want of a two-layer glb | `tagpu_hires_draw.c` | a replacement mesh with overlapping groups from above |
| **A structure's slant shadow is not punched out by its own body at +5 px.** The engine erases the shadow where the body sprite sits 5 px right of itself (`0x4B9D70`), then draws the body at +0; we draw the shadow and cover it with the body at +0, so a strip up to 5 px wide along each building's right edge is shadowed where the engine shows ground | [Shadows & cloak](shadows-cloak.html) §"Structure shadows, owned" | a stencil pass — **the FBO carries a stencil since G13n**, so this is now cheap where it was not — or accept it |
| A **replacement-mesh structure** casts every piece; the engine's raster takes only pieces with prim flag bit1 | `tagpu_hires_draw.c` `uSlant` | zeroing the shadow pose of the pieces whose engine prim lacks bit1 — no replacement structure exists yet to test it on |
| A **nanoframe** casts nothing until complete; the engine drew the cached shadow of its finished pieces (teal under `terrown`) | `tagpu_native.c` (nano > 0 is not listed) | listing nanoframes shadow-only, pieces with bit1 |
| **The structure-shadow flip is installed at attach and never undone**, like every code patch. Remove `tagpu_native.on` live under `owndraw all` and buildings lose their shadows along with their bodies (that state already draws no unit at all — *ta-drive*, "a stale owndraw.on is worse than none"); re-creating the file brings both back within 30 frames. With `writeback` armed beside `owndraw all` the engine's COMPLETED branch blits a real silhouette for a structure — teal under `terrown`, a silhouette instead of the slant without it | `tagpu_owndraw.c` | nothing for play; an A/B that needs the engine's cached shadow launches without `owndraw all` |
| Extreme zoom-out clamps the terrain span to `MAXCELL` and leaves an honest black margin | `tagpu_terr_clamp_span()` | nothing — a **clamp** was chosen over a bail on purpose; a bail hands the draw back and flashes, which is the one behaviour that looks like a bug |

### 3.3 Open questions, not limits

- **The MAPPED anomaly** ([terrain & depth](terrain-depth.html) §5.2) — two LOS stores that will
  not reconcile. Unresolved, and moot for rendering.
- **A scenario `move` order across the Two Continents forest** walks the unit to the map's west
  edge instead of the target. Undiagnosed; a direct right-click order works, so no fixture
  depends on it.

---

## 4. What the work taught us

These are the transferable parts — the reasons things are shaped the way they are.

**Own the draw four ways, and pick the cheapest that is honest.** (These are the four *ways*.
[Own the draw](own-the-draw.html) numbers its rules differently — its first is the **arming
rule**, that a code-patching pass installs once at `DllMain` and only if its trigger exists
*then*, because patching live engine bytes off the frame loop is racy. Ways 2–4 below are its
second, third and fourth rules.)

1. **Suppress and redraw.** Skip the engine's function, write the pixels ourselves. The default,
   and the only option when *our* visible set must be bigger than the engine's — health bars are
   re-drawn rather than captured precisely because the engine's loop walks HotUnits, culled to
   the unzoomed viewport, so a captured layer would leave a zoomed-out view's outer ring bare.
2. **Suppress and substitute.** Skip it, and put something of ours in its place at the same
   point in the frame (`tagpu_detour_leaf_call`, the terrain key-fill).
3. **Capture and replay** — *the third rule.* If the thing is expensive to re-derive and cheap
   to redirect, take its **output** instead of its logic: point the engine's draw context at a
   buffer of ours, let it draw, replay the buffer on your own terms. The `OFFSCREEN` handed down
   `DrawGameScreen` is a stack local, so writing its pixel base (`+0x0C`) for the length of a
   block redirects every clipped blit inside it. Parity is exact by construction — including
   text, which we have no font path for at all.
4. **Move it in a pass you already own** — *the fourth rule, and the one that was retired.* A
   pass that already arbitrates between your pixels and theirs can move theirs. The mouse cursor
   failed every capture test (it is blitted with a **NULL draw context**, which makes the callee
   build its own offscreen over the primary surface), so the composite read its texels from where
   the engine put them and painted them where the pointer is. It worked, and it could never be
   exact — the composite cannot know which of several draws per flip produced the texture it
   holds. **G13m deleted it** by fixing the input instead: tell the engine the truth about the
   pointer and it draws the sprite in the right place itself (§2.3d). The rule still stands for
   pixels you genuinely cannot reach any other way; the lesson is to check first whether what you
   are compensating for is something you told the engine.

**Never invent a gate.** Every condition we honour is the engine's own: health bars follow the
`damagebars` registry option, order markers follow SHIFT sampled through the engine's *own*
`KeyboardHotkeySampler(0xF9)` so a rebind cannot make us disagree, selection boxes follow
`SelBoxes`. The one time we polled a key ourselves, we first checked all nine of the image's
`GetAsyncKeyState` call sites and found every one does `and al,0xfe` — nothing in the engine
ever reads the "pressed since last call" bit, so an extra poll consumes nothing.

**A bail is the one behaviour that looks like a bug.** Handing the draw back for a frame
flashes. Clamp, and accept an honest black margin.

**Redirect the site, not the target, when you want to compose.** Our stubs *call* `0x471F90`
and `0x4BF8C0`, so whatever `fxown` and `terrown` detoured on those still runs.

**Measure the invariant you are about to rely on.** "Inside the viewport the engine's surface is
99.98 % key with nothing on it but the cursor" was what let the composite move the cursor at all.
It was measured (112 px of 630 784). The composite no longer needs it (§2.3d), but the invariant
is still the one to re-measure if anything engine-drawn ever appears in the viewport again.

**Prove the negative before designing around it.** The cursor cost a session partly because
`0x491CA1`/`0x491D58`/`0x4992B9` *look* like cursor draws and are actually mode selectors. What
settled it was instrumenting a capture window on the real candidate and counting **zero**
non-key texels — not more reading.

**Establish the noise floor before trusting any A/B number.** Capture the same arm state twice
and diff those first; it must come out zero. A walking unit or a still-scrolling camera shows up
as thousands of "mismatched" pixels that look exactly like a rendering bug. This cost a wrong
diagnosis in the G13c fog gate — 19,768 px of apparent error, chased into the shader, when the
real reading was 97 once the commander had parked.

**Do not trust a click to have changed anything.** Read the state, not the screenshot: `native:
… N sel` in the log, or the gadget page `tacli ui` reports. One selection box is exactly 8
vertices, so a `3599` vs `3591` vertex count is a cleaner proof than any image.

**Undefined is not the same as broken.** `glReadPixels` on the default framebuffer is only
defined for pixels that pass the pixel-ownership test, so every `glshot` of a window that was
not fully on the visible desktop came back mangled — with no error anywhere. Capture into an
FBO you own and the window stops being part of the equation.

---

## Where to go next

- [GPU renderer roadmap](roadmap.html) — the gates, the chronological log, the decisions locked.
- [Own the draw](own-the-draw.html) — the arming rule and the three take-over rules, in full,
  with the byte-level detail and the stub shapes.
- [Frame composition](frame-composition.html) — `DrawGameScreen 0x468CF0` and the per-unit path.
- [Terrain, features & depth](terrain-depth.html) — the OFFSCREEN, the fog, what is left in the viewport.
- [UI markers](ui-markers.html) — every engine-drawn per-unit marker and how G13d took them.
- [Field notes & gotchas](field-notes.html) — the things that cost time.
